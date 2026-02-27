#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "../Container/FlatMap.hpp"
#include "../Container/FlatSet.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Entity/Entity.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"

namespace Astra
{
    template<typename... QueryArgs>
    class Relations;

    class RelationshipGraph
    {
    public:
        using ChildrenContainer = SmallVector<Entity, 4>;
        using LinksContainer = SmallVector<Entity, 8>;
        
        // Combined entry for better cache locality
        struct TraversalEntry
        {
            Entity entity;
            uint16_t depth;
            uint16_t padding;  // Align to 8 bytes
            
            TraversalEntry() = default;
            TraversalEntry(Entity e, uint16_t d) : entity(e), depth(d), padding(0) {}
        };
        
        // Traversal cache for improved performance
        struct alignas(SIMD_ALIGNMENT) TraversalCache
        {
            std::vector<TraversalEntry> entries;  // Combined entity+depth for better locality
            uint32_t version = 0;                 // Version when cache was built
            
            bool IsValid(uint32_t currentVersion) const noexcept
            {
                return version == currentVersion && version != 0;
            }
            
            void Clear()
            {
                entries.clear();
                version = 0;
            }
            
            void Reserve(size_t capacity)
            {
                entries.reserve(capacity);
            }
            
            size_t Size() const noexcept
            {
                return entries.size();
            }
        };
        
        // Default constructor
        RelationshipGraph() = default;
        
        // Copy constructor
        RelationshipGraph(const RelationshipGraph& other) :
            m_parents(other.m_parents),
            m_children(other.m_children),
            m_links(other.m_links),
            m_structureVersion(other.m_structureVersion.load())
        {
            // Don't copy caches - they'll be rebuilt on demand
        }
        
        // Move constructor
        RelationshipGraph(RelationshipGraph&& other) noexcept :
            m_parents(std::move(other.m_parents)),
            m_children(std::move(other.m_children)),
            m_links(std::move(other.m_links)),
            m_descendantCaches(std::move(other.m_descendantCaches)),
            m_ancestorCaches(std::move(other.m_ancestorCaches)),
            m_cacheMutex(),  // shared_mutex is not movable, create new one
            m_structureVersion(other.m_structureVersion.load())
        {
            other.m_structureVersion = 1;
        }
        
        // Copy assignment operator
        RelationshipGraph& operator=(const RelationshipGraph& other)
        {
            if (this != &other)
            {
                m_parents = other.m_parents;
                m_children = other.m_children;
                m_links = other.m_links;
                m_descendantCaches.Clear();  // Clear caches - they'll be rebuilt
                m_ancestorCaches.Clear();
                m_structureVersion = other.m_structureVersion.load();
            }
            return *this;
        }
        
        // Move assignment operator
        RelationshipGraph& operator=(RelationshipGraph&& other) noexcept
        {
            if (this != &other)
            {
                m_parents = std::move(other.m_parents);
                m_children = std::move(other.m_children);
                m_links = std::move(other.m_links);
                m_descendantCaches = std::move(other.m_descendantCaches);
                m_ancestorCaches = std::move(other.m_ancestorCaches);
                m_structureVersion = other.m_structureVersion.load();
                other.m_structureVersion = 1;
            }
            return *this;
        }

        Entity GetParent(Entity child) const
        {
            auto it = m_parents.Find(child);
            return (it != m_parents.end()) ? it->second : Entity::Invalid();
        }

        /**
         * Check if 'ancestor' is an ancestor of 'entity' in the parent hierarchy.
         * Used to detect cycles before setting a parent relationship.
         */
        bool IsAncestorOf(Entity ancestor, Entity entity) const
        {
            Entity current = GetParent(entity);
            while (current.IsValid())
            {
                if (current == ancestor)
                    return true;
                current = GetParent(current);
            }
            return false;
        }

        void SetParent(Entity child, Entity parent)
        {
            ASTRA_ASSERT(child != parent, "Entity cannot be its own parent");
            ASTRA_ASSERT(child.IsValid() && parent.IsValid(), "Invalid entity in relationship");

            // Silently ignore invalid operations in release builds
            if (!child.IsValid() || !parent.IsValid() || child == parent)
                return;

            // Check for circular hierarchy: if child is an ancestor of parent,
            // setting parent as child's parent would create a cycle
            ASTRA_ASSERT(!IsAncestorOf(child, parent), "Circular hierarchy detected: child is an ancestor of parent");
            if (IsAncestorOf(child, parent))
                return;

            // Remove from old parent if exists
            RemoveParent(child);

            // Set new parent
            m_parents[child] = parent;
            m_children[parent].push_back(child);

            // Invalidate caches
            IncrementVersion();
        }

        void RemoveParent(Entity child)
        {
            auto it = m_parents.Find(child);
            if (it != m_parents.end())
            {
                Entity parent = it->second;
                m_parents.Erase(it);
                
                // Remove from parent's children list (swap-and-pop for O(1) removal)
                auto& children = m_children[parent];
                auto it = std::find(children.begin(), children.end(), child);
                if (it != children.end())
                {
                    // Swap with last element and pop (changes order but faster)
                    if (it != children.end() - 1)
                    {
                        *it = std::move(children.back());
                    }
                    children.pop_back();
                }
                
                // Clean up empty children container
                if (children.empty())
                {
                    m_children.Erase(parent);
                }
                
                // Invalidate caches
                IncrementVersion();
            }
        }

        bool HasParent(Entity child) const
        {
            return m_parents.Contains(child);
        }

        const ChildrenContainer& GetChildren(Entity parent) const
        {
            auto it = m_children.Find(parent);
            return (it != m_children.end()) ? it->second : s_emptyChildren;
        }

        bool HasChildren(Entity parent) const
        {
            auto it = m_children.Find(parent);
            return it != m_children.end() && !it->second.empty();
        }
        
        size_t GetChildCount(Entity parent) const
        {
            auto it = m_children.Find(parent);
            return (it != m_children.end()) ? it->second.size() : 0;
        }
        
        void AddLink(Entity a, Entity b)
        {
            ASTRA_ASSERT(a != b, "Entity cannot link to itself");
            ASTRA_ASSERT(a.IsValid() && b.IsValid(), "Invalid entity in link");
            
            // Silently ignore invalid operations in release builds
            if (!a.IsValid() || !b.IsValid() || a == b)
                return;
            
            // Add bidirectional link
            auto& linksA = m_links[a];
            if (std::find(linksA.begin(), linksA.end(), b) == linksA.end())
            {
                linksA.push_back(b);
            }
            
            auto& linksB = m_links[b];
            if (std::find(linksB.begin(), linksB.end(), a) == linksB.end())
            {
                linksB.push_back(a);
            }
            
            // Note: Links don't affect hierarchy traversal caches
            // Only increment version if we decide to cache link traversals
        }
        
        void RemoveLink(Entity a, Entity b)
        {
            auto removeFromLinks = [this](Entity from, Entity to)
            {
                auto it = m_links.Find(from);
                if (it != m_links.end())
                {
                    auto& links = it->second;
                    links.erase(std::remove(links.begin(), links.end(), to), links.end());
                    
                    if (links.empty())
                    {
                        m_links.Erase(it);
                    }
                }
            };
            
            removeFromLinks(a, b);
            removeFromLinks(b, a);
            
            // Note: Links don't affect hierarchy traversal caches
        }

        const LinksContainer& GetLinks(Entity entity) const
        {
            auto it = m_links.Find(entity);
            return (it != m_links.end()) ? it->second : s_emptyLinks;
        }

        bool AreLinked(Entity a, Entity b) const
        {
            auto it = m_links.Find(a);
            if (it != m_links.end())
            {
                const auto& links = it->second;
                return std::find(links.begin(), links.end(), b) != links.end();
            }
            return false;
        }

        bool HasLinks(Entity entity) const
        {
            auto it = m_links.Find(entity);
            return it != m_links.end() && !it->second.empty();
        }
        
        void OnEntityDestroyed(Entity entity)
        {
            bool hadParent = m_parents.Contains(entity);
            
            // Remove as child from parent (this calls IncrementVersion)
            if (hadParent)
            {
                RemoveParent(entity);
            }
            
            // Remove all children (they become orphaned)
            auto childrenIt = m_children.Find(entity);
            if (childrenIt != m_children.end())
            {
                // Clear parent references for all children
                for (Entity child : childrenIt->second)
                {
                    m_parents.Erase(child);
                }
                m_children.Erase(childrenIt);
                
                // Only increment version if we didn't already via RemoveParent
                if (!hadParent)
                {
                    IncrementVersion();
                }
            }
            
            // Remove all links
            auto linksIt = m_links.Find(entity);
            if (linksIt != m_links.end())
            {
                // Remove this entity from all linked entities
                for (Entity linked : linksIt->second)
                {
                    auto otherIt = m_links.Find(linked);
                    if (otherIt != m_links.end())
                    {
                        auto& otherLinks = otherIt->second;
                        otherLinks.erase(std::remove(otherLinks.begin(), otherLinks.end(), entity), otherLinks.end());
                        
                        if (otherLinks.empty())
                        {
                            m_links.Erase(otherIt);
                        }
                    }
                }
                m_links.Erase(linksIt);
            }
            
            // Remove entity's caches
            m_descendantCaches.Erase(entity);
            m_ancestorCaches.Erase(entity);
        }
        
        size_t GetParentChildCount() const { return m_parents.Size(); }
        size_t GetParentCount() const { return m_children.Size(); }
        size_t GetLinkedEntityCount() const { return m_links.Size(); }
        
        void Clear()
        {
            m_parents.Clear();
            m_children.Clear();
            m_links.Clear();
            m_descendantCaches.Clear();
            m_ancestorCaches.Clear();
            IncrementVersion();
        }
        
        // Cache statistics for monitoring
        struct CacheStats
        {
            size_t descendantCacheCount = 0;
            size_t ancestorCacheCount = 0;
            size_t totalCachedEntities = 0;
            size_t approximateMemoryBytes = 0;
        };
        
        CacheStats GetCacheStats() const
        {
            CacheStats stats;
            stats.descendantCacheCount = m_descendantCaches.Size();
            stats.ancestorCacheCount = m_ancestorCaches.Size();
            
            for (const auto& [entity, cache] : m_descendantCaches)
            {
                stats.totalCachedEntities += cache.entries.size();
                stats.approximateMemoryBytes += cache.entries.capacity() * sizeof(TraversalEntry);
            }
            
            for (const auto& [entity, cache] : m_ancestorCaches)
            {
                stats.totalCachedEntities += cache.entries.size();
                stats.approximateMemoryBytes += cache.entries.capacity() * sizeof(TraversalEntry);
            }
            
            return stats;
        }
        
        // Clear all caches (useful for memory management)
        void ClearCaches()
        {
            m_descendantCaches.Clear();
            m_ancestorCaches.Clear();
        }
        
        void Serialize(BinaryWriter& writer) const
        {
            // Write parent-child relationships
            // Write parent count
            uint32_t parentCount = static_cast<uint32_t>(m_parents.Size());
            writer(parentCount);
            
            // Write each parent-child pair
            for (const auto& [child, parent] : m_parents)
            {
                writer(child.GetValue());
                writer(parent.GetValue());
            }
            
            // Write children mappings
            // Note: We can reconstruct this from parents, but storing it is faster
            uint32_t parentWithChildrenCount = static_cast<uint32_t>(m_children.Size());
            writer(parentWithChildrenCount);
            
            for (const auto& [parent, children] : m_children)
            {
                writer(parent.GetValue());
                uint32_t childCount = static_cast<uint32_t>(children.size());
                writer(childCount);
                
                for (Entity child : children)
                {
                    writer(child.GetValue());
                }
            }
            
            // Write link relationships
            uint32_t linkedEntityCount = static_cast<uint32_t>(m_links.Size());
            writer(linkedEntityCount);
            
            for (const auto& [entity, links] : m_links)
            {
                writer(entity.GetValue());
                uint32_t linkCount = static_cast<uint32_t>(links.size());
                writer(linkCount);
                
                for (Entity linked : links)
                {
                    writer(linked.GetValue());
                }
            }
        }
        
        static Result<RelationshipGraph, SerializationError> Deserialize(BinaryReader& reader)
        {
            RelationshipGraph graph;
            
            // Read parent-child relationships
            uint32_t parentCount;
            reader(parentCount);
            
            if (reader.HasError())
            {
                return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
            }
            
            graph.m_parents.Reserve(parentCount);
            
            for (uint32_t i = 0; i < parentCount; ++i)
            {
                Entity::StorageType childValue, parentValue;
                reader(childValue);
                reader(parentValue);
                
                if (reader.HasError())
                {
                    return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
                }
                
                Entity child(childValue);
                Entity parent(parentValue);
                graph.m_parents[child] = parent;
            }
            
            // Read children mappings
            uint32_t parentWithChildrenCount;
            reader(parentWithChildrenCount);
            
            if (reader.HasError())
            {
                return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
            }
            
            graph.m_children.Reserve(parentWithChildrenCount);
            
            for (uint32_t i = 0; i < parentWithChildrenCount; ++i)
            {
                Entity::StorageType parentValue;
                reader(parentValue);
                
                uint32_t childCount;
                reader(childCount);
                
                if (reader.HasError())
                {
                    return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
                }
                
                Entity parent(parentValue);
                auto& children = graph.m_children[parent];
                children.reserve(childCount);
                
                for (uint32_t j = 0; j < childCount; ++j)
                {
                    Entity::StorageType childValue;
                    reader(childValue);
                    
                    if (reader.HasError())
                    {
                        return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
                    }
                    
                    children.push_back(Entity(childValue));
                }
            }
            
            // Read link relationships
            uint32_t linkedEntityCount;
            reader(linkedEntityCount);
            
            if (reader.HasError())
            {
                return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
            }
            
            graph.m_links.Reserve(linkedEntityCount);
            
            for (uint32_t i = 0; i < linkedEntityCount; ++i)
            {
                Entity::StorageType entityValue;
                reader(entityValue);
                
                uint32_t linkCount;
                reader(linkCount);
                
                if (reader.HasError())
                {
                    return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
                }
                
                Entity entity(entityValue);
                auto& links = graph.m_links[entity];
                links.reserve(linkCount);
                
                for (uint32_t j = 0; j < linkCount; ++j)
                {
                    Entity::StorageType linkedValue;
                    reader(linkedValue);
                    
                    if (reader.HasError())
                    {
                        return Result<RelationshipGraph, SerializationError>::Err(reader.GetError());
                    }
                    
                    links.push_back(Entity(linkedValue));
                }
            }
            
            return Result<RelationshipGraph, SerializationError>::Ok(std::move(graph));
        }

    private:
        // Build descendant cache for an entity
        void BuildDescendantCache(Entity root, TraversalCache& cache) const
        {
            cache.Clear();
            
            // Early exit if root has no children
            auto childrenIt = m_children.Find(root);
            if (childrenIt == m_children.end() || childrenIt->second.empty())
            {
                cache.version = m_structureVersion.load(std::memory_order_acquire);
                return;
            }
            
            // Estimate based on typical tree fanout (4 children per node)
            size_t estimatedSize = std::min(childrenIt->second.size() * 8, size_t(1024));
            cache.Reserve(estimatedSize);
            
            // Use SmallVector as queue to avoid heap allocations
            SmallVector<TraversalEntry, 64> queue;
            size_t readIdx = 0;
            
            FlatSet<Entity> visited;
            visited.Reserve(estimatedSize);
            visited.Insert(root);
            
            // Add immediate children
            for (Entity child : childrenIt->second)
            {
                queue.emplace_back(child, 1);
                visited.Insert(child);
            }
            
            // BFS traversal using ring buffer approach
            while (readIdx < queue.size())
            {
                TraversalEntry current = queue[readIdx++];
                
                cache.entries.push_back(current);
                
                // Add this entity's children
                auto entityChildrenIt = m_children.Find(current.entity);
                if (entityChildrenIt != m_children.end())
                {
                    for (Entity child : entityChildrenIt->second)
                    {
                        if (visited.Insert(child).second)
                        {
                            // Limit depth to prevent overflow (uint16_t max)
                            if (current.depth < std::numeric_limits<uint16_t>::max())
                            {
                                queue.emplace_back(child, static_cast<uint16_t>(current.depth + 1));
                            }
                        }
                    }
                }
            }
            
            cache.version = m_structureVersion.load(std::memory_order_acquire);
        }
        
        // Build ancestor cache for an entity
        void BuildAncestorCache(Entity entity, TraversalCache& cache) const
        {
            cache.Clear();
            
            Entity current = entity;
            uint16_t depth = 0;
            FlatSet<Entity> visited;
            visited.Reserve(16);  // Most hierarchies are shallow
            visited.Insert(entity);
            
            // Walk up the parent chain
            auto it = m_parents.Find(current);
            while (it != m_parents.end())
            {
                current = it->second;
                
                // Cycle detection
                if (!visited.Insert(current).second)
                {
                    ASTRA_ASSERT(false, "Cycle detected in parent-child relationships");
                    break;
                }
                
                depth++;
                cache.entries.emplace_back(current, depth);
                
                // Limit depth to prevent overflow
                if (depth >= std::numeric_limits<uint16_t>::max())
                {
                    break;
                }
                
                it = m_parents.Find(current);
            }
            
            cache.version = m_structureVersion.load(std::memory_order_acquire);
        }

        // Get cached descendants for a root entity
        const TraversalCache& GetDescendantsCached(Entity root) const
        {
            uint32_t currentVersion = m_structureVersion.load(std::memory_order_acquire);

            // First check with shared lock (allows concurrent reads)
            {
                std::shared_lock<std::shared_mutex> readLock(m_cacheMutex);
                auto it = m_descendantCaches.Find(root);
                if (it != m_descendantCaches.end() && it->second.IsValid(currentVersion))
                {
                    return it->second;
                }
            }

            // Need to rebuild - acquire exclusive lock
            {
                std::unique_lock<std::shared_mutex> writeLock(m_cacheMutex);
                // Double-check after acquiring write lock (another thread may have built it)
                auto& cache = m_descendantCaches[root];
                if (!cache.IsValid(currentVersion))
                {
                    BuildDescendantCache(root, cache);
                }
                return cache;
            }
        }

        // Get cached ancestors for an entity
        const TraversalCache& GetAncestorsCached(Entity entity) const
        {
            uint32_t currentVersion = m_structureVersion.load(std::memory_order_acquire);

            // First check with shared lock (allows concurrent reads)
            {
                std::shared_lock<std::shared_mutex> readLock(m_cacheMutex);
                auto it = m_ancestorCaches.Find(entity);
                if (it != m_ancestorCaches.end() && it->second.IsValid(currentVersion))
                {
                    return it->second;
                }
            }

            // Need to rebuild - acquire exclusive lock
            {
                std::unique_lock<std::shared_mutex> writeLock(m_cacheMutex);
                // Double-check after acquiring write lock (another thread may have built it)
                auto& cache = m_ancestorCaches[entity];
                if (!cache.IsValid(currentVersion))
                {
                    BuildAncestorCache(entity, cache);
                }
                return cache;
            }
        }
        
        // Increment version to invalidate all caches
        void IncrementVersion()
        {
            m_structureVersion.fetch_add(1, std::memory_order_release);
        }
    
        // Relationship management
        FlatMap<Entity, Entity> m_parents;              // child -> parent
        FlatMap<Entity, ChildrenContainer> m_children;  // parent -> children
        FlatMap<Entity, LinksContainer> m_links;        // entity -> linked entities
        
        // Traversal caches
        mutable FlatMap<Entity, TraversalCache> m_descendantCaches;
        mutable FlatMap<Entity, TraversalCache> m_ancestorCaches;

        // Mutex for thread-safe cache access (shared_mutex allows concurrent reads)
        mutable std::shared_mutex m_cacheMutex;

        // Version tracking for cache invalidation
        std::atomic<uint32_t> m_structureVersion{1};
        
        // Empty containers for const references
        static inline const ChildrenContainer s_emptyChildren{};
        static inline const LinksContainer s_emptyLinks{};

        template<typename... QueryArgs>
        friend class Relations;
    };
}