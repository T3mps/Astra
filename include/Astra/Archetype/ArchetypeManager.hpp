#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Container/Bitmap.hpp"
#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "Archetype.hpp"
#include "ArchetypeChunkPool.hpp"
#include "ArchetypeGraph.hpp"

namespace Astra
{
    // Forward declaration for friend class
    template<typename... QueryArgs>
    class View;
    
    class ArchetypeManager
    {
        // Allow View to access versioning methods
        template<typename... QueryArgs>
        friend class View;
        
    public:
        struct EntityRecord
        {
            Archetype* archetype = nullptr;
            EntityLocation location;
        };
        
        // Metrics for tracking archetype usage
        struct ArchetypeMetrics
        {
            size_t currentEntityCount = 0;
            size_t peakEntityCount = 0;
            size_t emptyDuration = 0;  // Number of cleanup calls while empty
            
            void UpdateForCleanup(size_t entityCount)
            {
                currentEntityCount = entityCount;
                if (entityCount > peakEntityCount)
                {
                    peakEntityCount = entityCount;
                }
                
                if (entityCount == 0)
                {
                    emptyDuration++;
                }
                else
                {
                    emptyDuration = 0;
                }
            }
            
            void UpdatePeak(size_t entityCount)
            {
                currentEntityCount = entityCount;
                if (entityCount > peakEntityCount)
                {
                    peakEntityCount = entityCount;
                }
            }
        };
        
        // Default constructor creates new component registry
    // Original: explicit ArchetypeManager(const ArchetypeChunkPool::Config& poolConfig = {})
    explicit ArchetypeManager(ArchetypeChunkPool::Config poolConfig = {}) 
            : m_chunkPool(poolConfig)
            , m_componentRegistry(std::make_shared<ComponentRegistry>())
        {
            InitializeRootArchetype();
        }
        
        // Constructor for sharing component registry
    // Original: ArchetypeManager(std::shared_ptr<ComponentRegistry> registry, const ArchetypeChunkPool::Config& poolConfig = {})
    ArchetypeManager(std::shared_ptr<ComponentRegistry> registry, ArchetypeChunkPool::Config poolConfig = {})
            : m_chunkPool(poolConfig)
            , m_componentRegistry(std::move(registry))
        {
            InitializeRootArchetype();
        }
        
        // Get the component registry for sharing with other storage instances
        std::shared_ptr<ComponentRegistry> GetComponentRegistry() const
        {
            return m_componentRegistry;
        }
        
        /**
         * Create or get archetype for a specific component combination
         */
        template<Component... Components>
        ASTRA_NODISCARD Archetype* GetOrCreateArchetype()
        {
            // Register components before creating archetype
            (m_componentRegistry->RegisterComponent<Components>(), ...);
            ComponentMask mask = MakeComponentMask<Components...>();
            return GetOrCreateArchetype(mask);
        }
        
        /**
         * Find an existing archetype with the exact set of components
         * @tparam Components Component types to search for
         * @return Pointer to archetype if found, nullptr otherwise
         */
        template<Component... Components>
        ASTRA_NODISCARD Archetype* FindArchetype() const
        {
            ComponentMask mask = MakeComponentMask<Components...>();
            return FindArchetype(mask);
        }
        
        /**
         * Find an existing archetype with the given component mask
         * @param mask Component mask to search for
         * @return Pointer to archetype if found, nullptr otherwise
         */
        ASTRA_NODISCARD Archetype* FindArchetype(const ComponentMask& mask) const
        {
            auto it = m_archetypeMap.Find(mask);
            if (it != m_archetypeMap.end())
            {
                return it->second;
            }
            return nullptr;
        }
        
        /**
         * Add entity to storage (in root archetype)
         * Entity should be created by EntityManager
         */
        void AddEntity(Entity entity)
        {
            EntityLocation location = m_rootArchetype->AddEntity(entity);
            if (!location.IsValid()) ASTRA_UNLIKELY
            {
                // Allocation failed - entity won't be tracked
                return;
            }
            m_entityMap[entity] = EntityRecord{m_rootArchetype, location};
            UpdateArchetypeMetrics(m_rootArchetype);
        }
        
        template<Component... Components, typename Generator>
        void AddEntities(std::span<const Entity> entities, Generator&& generator)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            // Get or create archetype once
            Archetype* archetype = GetOrCreateArchetype<Components...>();

            // Use optimized batch addition
            std::vector<EntityLocation> locations = archetype->AddEntities(entities);
            
            // Apply components to all successfully added entities
            for (size_t i = 0; i < locations.size(); ++i)
            {
                // Apply components
                std::apply([&](auto&&... components)
                {
                    ((archetype->SetComponent(locations[i], std::forward<decltype(components)>(components))), ...);
                }, generator(i));
            }

            // Batch update entity map - reserve space first for efficiency
            m_entityMap.reserve(m_entityMap.size() + locations.size());
            for (size_t i = 0; i < locations.size(); ++i)
            {
                m_entityMap[entities[i]] = EntityRecord{archetype, locations[i]};
            }
            
            // Update metrics after batch add
            UpdateArchetypeMetrics(archetype);
        }

        /**
         * Set entity location directly (for batch operations)
         * Used when creating entities with known archetype
         */
        void SetEntityLocation(Entity entity, Archetype* archetype, EntityLocation location)
        {
            m_entityMap[entity] = EntityRecord{archetype, location};
            UpdateArchetypeMetrics(archetype);
        }
        
        /**
         * Remove entity from storage
         * Entity destruction should be handled by EntityManager
         */
        void RemoveEntity(Entity entity)
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return;
            
            EntityRecord& loc = it->second;
            
            // Remove from archetype, check if another entity was moved
            if (auto movedEntity = loc.archetype->RemoveEntity(loc.location)) ASTRA_LIKELY
            {
                // Update location of moved entity
                auto movedIt = m_entityMap.find(*movedEntity);
                ASTRA_ASSERT(movedIt != m_entityMap.end(), "Moved entity not found in map");
                movedIt->second.location = loc.location;
            }
            
            m_entityMap.erase(it);
            UpdateArchetypeMetrics(loc.archetype);
        }
        
        /**
         * Add component to entity (moves to new archetype)
         */
        template<Component T, typename... Args>
        T* AddComponent(Entity entity, Args&&... args)
        {
            // Ensure component is registered
            m_componentRegistry->RegisterComponent<T>();
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return nullptr;
            
            EntityRecord& oldLoc = it->second;
            ComponentID componentId = TypeID<T>::Value();
            
            // Check if entity already has component
            if (oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return nullptr;
                
            // Find or create edge to new archetype
            Archetype* newArchetype = GetArchetypeWithAdded(oldLoc.archetype, componentId);
            
            // Optimized move with in-place construction
            EntityLocation newEntityLocation = MoveEntityWithComponent<T>(entity, oldLoc, newArchetype, 
                                         std::forward<Args>(args)...);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                // Allocation failed
                return nullptr;
            }
            
            return newArchetype->GetComponent<T>(newEntityLocation);
        }
        
        /**
         * Remove component from entity
         */
        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            // Ensure component is registered
            m_componentRegistry->RegisterComponent<T>();
            
            ComponentID componentId = TypeID<T>::Value();
            
            // Component validity is ensured at compile time through the type system
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return false;
            
            EntityRecord& oldLoc = it->second;
            
            // Check if entity has component
            if (!oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;
                
            // Find or create edge to new archetype
            Archetype* newArchetype = GetArchetypeWithRemoved(
                oldLoc.archetype, componentId);
            
            // Move entity to new archetype
            // Note: The component will be destructed automatically by RemoveEntity
            EntityLocation newEntityLocation = MoveEntity(entity, oldLoc, newArchetype);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                // Allocation failed - component was destroyed but entity couldn't be moved
                // This is a critical error state
                return false;
            }
            
            return true;
        }
        
        /**
         * Get component for entity
         */
        template<Component T>
        ASTRA_NODISCARD T* GetComponent(Entity entity)
        {
            // Don't auto-register - component should already exist if retrieving
            ComponentID componentId = TypeID<T>::Value();
            
            // Component validity is ensured at compile time
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return nullptr;
            
            EntityRecord& loc = it->second;
            return loc.archetype->GetComponent<T>(loc.location);
        }
        
        /**
         * Check if entity has a component
         */
        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return false;
            
            const EntityRecord& loc = it->second;
            return loc.archetype->HasComponent<T>();
        }

        ASTRA_NODISCARD std::pair<Archetype*, EntityLocation> GetEntityLocation(Entity entity) const
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY 
                return {nullptr, EntityLocation{}};
            
            const EntityRecord& loc = it->second;
            return {loc.archetype, loc.location};
        }
        
        /**
         * Query archetypes that match a component mask
         */
        ASTRA_NODISCARD auto QueryArchetypes(const ComponentMask& mask)
        {
            return m_archetypes |
                std::views::transform([](auto& entry) { return entry.archetype.get(); }) |
                std::views::filter([mask](Archetype* arch) { return arch->GetMask().HasAll(mask); });
        }
        
        /**
         * Get all archetypes for custom query logic
         */
        ASTRA_NODISCARD auto GetAllArchetypes()
        {
            return m_archetypes | std::views::transform([](auto& entry) { return entry.archetype.get(); });
        }
        
        /**
         * Get chunk pool statistics
         */
        ASTRA_NODISCARD ArchetypeChunkPool::Stats GetPoolStats() const
        {
            return m_chunkPool.GetStats();
        }
        
        /**
         * Add component to multiple entities in batch (optimized)
         * Uses the same optimized pattern as AddEntities for maximum performance
         */
        template<Component T, typename... Args>
        void AddComponents(std::span<Entity> entities, Args&&... args)
        {
            if (entities.empty()) return;
            
            m_componentRegistry->RegisterComponent<T>();
            ComponentID componentId = TypeID<T>::Value();
            
            // Group entities by current archetype for batch processing
            auto batches = GroupEntitiesByArchetype(entities, 
                [componentId](Archetype* arch) { return !arch->GetMask().Test(componentId); });
            
            // Process each archetype group
            for (auto& [srcArchetype, entityBatch] : batches)
            {
                if (entityBatch.empty()) continue;
                
                Archetype* dstArchetype = GetArchetypeWithAdded(srcArchetype, componentId);
                
                // Execute optimized batch move with component addition
                BatchMoveEntitiesWithComponent<T>(srcArchetype, dstArchetype, entityBatch, 
                                                  std::forward<Args>(args)...);
            }
        }
        
        /**
         * Remove component from multiple entities in batch (optimized)
         * Returns number of entities that had the component removed
         */
        template<Component T>
        size_t RemoveComponents(std::span<Entity> entities)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return 0;
            
            m_componentRegistry->RegisterComponent<T>();
            ComponentID componentId = TypeID<T>::Value();
            
            // Group entities by current archetype for batch processing
            auto batches = GroupEntitiesByArchetype(entities, 
                [componentId](Archetype* arch) { return arch->GetMask().Test(componentId); });
            
            size_t removedCount = 0;
            
            // Process each archetype group
            for (auto& [srcArchetype, entityBatch] : batches)
            {
                if (entityBatch.empty()) continue;
                
                Archetype* dstArchetype = GetArchetypeWithRemoved(srcArchetype, componentId);
                
                // Execute optimized batch move without component
                BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
                removedCount += entityBatch.size();
            }
            
            return removedCount;
        }
        
        /**
         * Remove multiple entities in batch (optimized)
         */
        void RemoveEntities(std::span<Entity> entities)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return;
                
            // Group entities by archetype for batch processing
            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;
            
            for (Entity entity : entities)
            {
                auto it = m_entityMap.find(entity);
                if (it == m_entityMap.end()) ASTRA_UNLIKELY continue;
                
                EntityRecord& loc = it->second;
                batches[loc.archetype].emplace_back(entity, loc.location);
            }
            
            // Process each batch
            for (auto& [archetype, entityBatch] : batches)
            {
                SmallVector<EntityLocation, 8> locations;
                locations.reserve(entityBatch.size());
                for (const auto& [entity, location] : entityBatch)
                {
                    locations.push_back(location);
                }
                
                // Batch remove from archetype
                auto movedEntities = archetype->RemoveEntities(locations);
                
                // Update entity locations for moved entities
                for (const auto& [movedEntity, newEntityLocation] : movedEntities)
                {
                    auto movedIt = m_entityMap.find(movedEntity);
                    if (movedIt != m_entityMap.end()) ASTRA_LIKELY
                    {
                        movedIt->second.location = newEntityLocation;
                    }
                }
                
                // Remove entities from map
                for (const auto& [entity, _] : entityBatch)
                {
                    m_entityMap.erase(entity);
                }
                
                // Update metrics after batch removal
                UpdateArchetypeMetrics(archetype);
            }
        }
        
        // Cleanup options for archetype removal
        struct CleanupOptions
        {
            size_t minEmptyDuration = 1;              // Remove if empty for at least this many calls
            size_t minArchetypesToKeep = 8;           // Keep at least this many archetypes
            size_t maxArchetypesToRemove = SIZE_MAX;  // Max to remove in one call
            size_t maxPeakEntityCount = SIZE_MAX;     // Only remove if peak count below this
            
            // GCC fix: explicit default constructor for brace initialization
            CleanupOptions() = default;
        };
        
        // Information about an archetype
        struct ArchetypeInfo
        {
            Archetype* archetype;
            size_t currentEntityCount;
            size_t peakEntityCount;
            size_t emptyDuration;
            ComponentMask mask;
            size_t approximateMemoryUsage;
        };
        
        // Update metrics for all archetypes (call before cleanup)
        void UpdateArchetypeMetrics()
        {
            for (auto& entry : m_archetypes)
            {
                entry.metrics.UpdateForCleanup(entry.archetype->GetEntityCount());
            }
        }
        
        // Get archetype statistics
        ASTRA_NODISCARD std::vector<ArchetypeInfo> GetArchetypeStats() const
        {
            std::vector<ArchetypeInfo> stats;
            stats.reserve(m_archetypes.size());
            
            for (const auto& entry : m_archetypes)
            {
                ArchetypeInfo info;
                info.archetype = entry.archetype.get();
                info.currentEntityCount = entry.metrics.currentEntityCount;
                info.peakEntityCount = entry.metrics.peakEntityCount;
                info.emptyDuration = entry.metrics.emptyDuration;
                info.mask = entry.archetype->GetMask();
                
                // Calculate approximate memory usage
                size_t chunkCount = entry.archetype->GetChunks().size();
                size_t chunkSize = m_chunkPool.GetChunkSize();
                info.approximateMemoryUsage = chunkCount * chunkSize + 
                                             sizeof(Archetype) + 
                                             sizeof(size_t) * MAX_COMPONENTS * 2;
                
                stats.push_back(info);
            }
            
            return stats;
        }
        
        // Get total archetype count
        ASTRA_NODISCARD size_t GetArchetypeCount() const
        {
            return m_archetypes.size();
        }
        
        // Get approximate memory usage
        ASTRA_NODISCARD size_t GetArchetypeMemoryUsage() const
        {
            size_t total = 0;
            size_t chunkSize = m_chunkPool.GetChunkSize();
            for (const auto& entry : m_archetypes)
            {
                size_t chunkCount = entry.archetype->GetChunks().size();
                total += chunkCount * chunkSize;
                total += sizeof(Archetype) + sizeof(size_t) * MAX_COMPONENTS * 2;
            }
            return total;
        }
        
        // GCC fix: Provide overloaded methods to avoid default member initializer issues
        size_t CleanupEmptyArchetypes() { return CleanupEmptyArchetypes(CleanupOptions()); }
        
        size_t CleanupEmptyArchetypes(CleanupOptions options)
        {
            // Never remove root archetype
            if (m_archetypes.size() <= options.minArchetypesToKeep)
            {
                return 0;
            }
            
            // Identify candidates for removal
            SmallVector<size_t, 8> candidates;
            
            for (size_t i = 0; i < m_archetypes.size(); ++i)
            {
                const auto& entry = m_archetypes[i];
                
                // Skip root archetype
                if (entry.archetype.get() == m_rootArchetype)
                {
                    continue;
                }
                
                // Check if candidate for removal
                if (entry.metrics.currentEntityCount == 0 &&
                    entry.metrics.emptyDuration >= options.minEmptyDuration &&
                    entry.metrics.peakEntityCount <= options.maxPeakEntityCount)
                {
                    candidates.push_back(i);
                }
            }
            
            // Ensure we keep minimum archetypes
            size_t maxCanRemove = m_archetypes.size() - options.minArchetypesToKeep;
            if (candidates.size() > maxCanRemove)
            {
                candidates.resize(maxCanRemove);
            }
            
            // Limit removals per call
            if (candidates.size() > options.maxArchetypesToRemove)
            {
                // Sort by empty duration (longest empty first)
                std::partial_sort(
                    candidates.begin(),
                    candidates.begin() + options.maxArchetypesToRemove,
                    candidates.end(),
                    [this](size_t a, size_t b) {
                        return m_archetypes[a].metrics.emptyDuration > 
                               m_archetypes[b].metrics.emptyDuration;
                    }
                );
                candidates.resize(options.maxArchetypesToRemove);
            }
            
            // Remove archetypes in reverse order to maintain indices
            std::sort(candidates.rbegin(), candidates.rend());
            
            size_t removed = 0;
            for (size_t idx : candidates)
            {
                RemoveArchetypeAt(idx);
                removed++;
            }
            
            return removed;
        }
        
        /**
         * Serialize ArchetypeManager to binary format
         * Includes all archetypes and entity mappings
         */
        void Serialize(BinaryWriter& writer) const
        {
            // Write storage metadata
            writer(static_cast<uint32_t>(m_archetypes.size()));
            writer(static_cast<uint32_t>(m_entityMap.size()));
            
            // Write each archetype (skip root archetype at index 0)
            for (size_t i = 1; i < m_archetypes.size(); ++i)
            {
                const auto& entry = m_archetypes[i];
                
                // Write archetype index for reference
                writer(static_cast<uint32_t>(i));
                
                // Serialize the archetype
                entry.archetype->Serialize(writer);
                
                // Write metrics
                writer(entry.metrics.currentEntityCount);
                writer(entry.metrics.peakEntityCount);
                writer(entry.metrics.emptyDuration);
            }
            
            // Write entity-to-archetype mappings
            for (const auto& [entity, location] : m_entityMap)
            {
                writer(entity);
                
                // Find archetype index
                uint32_t archetypeIndex = 0;
                for (size_t i = 0; i < m_archetypes.size(); ++i)
                {
                    if (m_archetypes[i].archetype.get() == location.archetype)
                    {
                        archetypeIndex = static_cast<uint32_t>(i);
                        break;
                    }
                }
                
                writer(archetypeIndex);
                writer(location.location.chunkIndex);
                writer(location.location.entityIndex);
            }
        }
        
        /**
         * Deserialize ArchetypeManager from binary format
         * Returns false if deserialization fails
         */
        bool Deserialize(BinaryReader& reader)
        {
            // Clear existing archetypes (except root)
            while (m_archetypes.size() > 1)
            {
                m_archetypes.pop_back();
            }
            m_archetypeMap.Clear();
            m_entityMap.clear();
            
            // Read storage metadata
            uint32_t archetypeCount, entityCount;
            reader(archetypeCount)(entityCount);
            
            if (reader.HasError())
                return false;
            
            // Reserve space
            m_archetypes.reserve(archetypeCount);
            m_entityMap.reserve(entityCount);
            
            // Get all registered component descriptors
            std::vector<ComponentDescriptor> registryDescriptors;
            m_componentRegistry->GetAllDescriptors(registryDescriptors);
            
            // Read each archetype
            std::vector<uint32_t> archetypeIndices;
            for (uint32_t i = 1; i < archetypeCount; ++i)
            {
                uint32_t index;
                reader(index);
                archetypeIndices.push_back(index);
                
                // Deserialize the archetype
                auto archetype = Archetype::Deserialize(reader, registryDescriptors, &m_chunkPool);
                if (!archetype || reader.HasError())
                {
                    return false;
                }
                
                // Read metrics
                ArchetypeMetrics metrics;
                reader(metrics.currentEntityCount);
                reader(metrics.peakEntityCount);
                reader(metrics.emptyDuration);
                
                // Add to storage
                ArchetypeEntry entry;
                entry.archetype = std::move(archetype);
                entry.metrics = metrics;
                
                m_archetypeMap[entry.archetype->GetMask()] = entry.archetype.get();
                m_archetypes.push_back(std::move(entry));
            }
            
            // Read entity-to-archetype mappings
            for (uint32_t i = 0; i < entityCount; ++i)
            {
                Entity entity;
                uint32_t archetypeIndex;
                uint32_t chunkIndex;
                uint32_t entityIndex;
                
                reader(entity)(archetypeIndex)(chunkIndex)(entityIndex);
                
                if (reader.HasError())
                    return false;
                
                if (archetypeIndex < m_archetypes.size())
                {
                    EntityRecord location;
                    location.archetype = m_archetypes[archetypeIndex].archetype.get();
                    location.location = EntityLocation(chunkIndex, entityIndex);
                    m_entityMap[entity] = location;
                }
            }
            
            return !reader.HasError();
        }
        
        
    private:        
        ASTRA_NODISCARD Archetype* GetOrCreateArchetype(const ComponentMask& mask)
        {
            // Check if archetype already exists
            auto it = m_archetypeMap.Find(mask);
            if (it != m_archetypeMap.end()) ASTRA_LIKELY
            {
                return it->second;
            }

            // Create new archetype
            auto archetype = std::make_unique<Archetype>(mask);
            Archetype* ptr = archetype.get();
            
            // Set the pool for the new archetype
            ptr->m_chunkPool = &m_chunkPool;

            // Initialize based on component mask
            std::vector<ComponentDescriptor> componentDescriptors;


            for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
            {
                if (mask.Test(id)) ASTRA_UNLIKELY
                {
                    if (const auto* desc = m_componentRegistry->GetComponentDescriptor(id)) ASTRA_LIKELY
                    {
                        // Just copy the entire descriptor - all fields are needed
                        componentDescriptors.push_back(*desc);
                    }
                }
            }

            // Always initialize archetype, even with empty component list
            ptr->Initialize(componentDescriptors);

            // Store archetype
            m_archetypeMap[mask] = ptr;
            
            ArchetypeEntry entry;
            entry.archetype = std::move(archetype);
            entry.creationGeneration = ++m_generation;  // Assign generation
            // Don't call Update(0) here - let it track naturally
            m_archetypes.push_back(std::move(entry));
            
            // Increment structural change counter for fast path checking
            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);

            return ptr;
        }

        Archetype* GetArchetypeWithAdded(Archetype* from, ComponentID componentId)
        {
            // Check edge cache in the edge graph
            if (Archetype* target = m_edgeGraph.GetAddEdge(from, componentId)) ASTRA_LIKELY
            {
                return target;
            }
            
            // Create new mask with component added
            ComponentMask newMask = from->GetMask();
            newMask.Set(componentId);
            
            // Get or create archetype
            Archetype* to = GetOrCreateArchetype(newMask);
            
            // Cache edge in the edge graph
            m_edgeGraph.SetAddEdge(from, componentId, to);
            
            return to;
        }
        
        Archetype* GetArchetypeWithRemoved(Archetype* from, ComponentID componentId)
        {
            // Check edge cache in the edge graph
            if (Archetype* target = m_edgeGraph.GetRemoveEdge(from, componentId)) ASTRA_LIKELY
            {
                return target;
            }
            
            // Create new mask with component removed
            ComponentMask newMask = from->GetMask();
            newMask.Reset(componentId);
            
            // Get or create archetype
            Archetype* to = GetOrCreateArchetype(newMask);
            
            // Cache edge in the edge graph
            m_edgeGraph.SetRemoveEdge(from, componentId, to);
            
            return to;
        }
        
        /**
         * Get the current structural change counter for fast path checking
         * This is incremented whenever archetypes are created or removed
         */
        ASTRA_NODISCARD uint32_t GetStructuralChangeCounter() const noexcept
        {
            return m_structuralChangeCounter.load(std::memory_order_acquire);
        }
        
        /**
         * Get the current generation number
         */
        ASTRA_NODISCARD uint32_t GetCurrentGeneration() const noexcept
        {
            return m_generation;
        }
        
        /**
         * Get archetypes created after a specific generation
         * Used for incremental view updates
         */
        ASTRA_NODISCARD std::vector<Archetype*> GetArchetypesSince(uint32_t sinceGeneration) const
        {
            std::vector<Archetype*> result;
            for (const auto& entry : m_archetypes)
            {
                if (entry.creationGeneration > sinceGeneration)
                {
                    result.push_back(entry.archetype.get());
                }
            }
            return result;
        }
        
    private:
        /**
         * Move entity to a new archetype (used after component removal)
         * Does NOT construct any new components, just transfers existing ones
         */
        EntityLocation MoveEntity(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype)
        {
            // Reserve space in new archetype without constructing
            EntityLocation newEntityLocation = newArchetype->AddEntityNoConstruct(entity);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                // Allocation failed - return invalid location
                return newEntityLocation;
            }
            
            // Move existing components efficiently
            if (oldLoc.archetype->IsInitialized() && newArchetype->IsInitialized()) ASTRA_LIKELY
            {
                // Move entity data from old archetype to new archetype
                newArchetype->MoveEntityFrom(newEntityLocation, *oldLoc.archetype, oldLoc.location);
            }
            
            // Remove from old archetype and handle moved entity
            if (auto movedEntity = oldLoc.archetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
            {
                m_entityMap[*movedEntity].location = oldLoc.location;
            }
            
            // Update entity location
            Archetype* oldArchetype = oldLoc.archetype;
            oldLoc.archetype = newArchetype;
            oldLoc.location = newEntityLocation;
            
            // Update metrics for both archetypes
            UpdateArchetypeMetrics(oldArchetype);
            UpdateArchetypeMetrics(newArchetype);
            
            return newEntityLocation;
        }
        
        /**
         * Move entity to a new archetype and construct component T in-place
         * Used when adding a component to an existing entity
         */
        template<Component T, typename... Args>
        EntityLocation MoveEntityWithComponent(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype, Args&&... args)
        {
            // Reserve space in new archetype without constructing
            EntityLocation newEntityLocation = newArchetype->AddEntityNoConstruct(entity);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                // Allocation failed - return invalid location
                return newEntityLocation;
            }
            
            // Move existing components and construct new one efficiently
            if (oldLoc.archetype->IsInitialized() && newArchetype->IsInitialized()) ASTRA_LIKELY
            {
                ExecuteComponentMoveAndAdd<T>(
                    newEntityLocation, newArchetype, 
                    oldLoc.location, oldLoc.archetype,
                    std::forward<Args>(args)...
                );
            }
            
            // Remove from old archetype and handle moved entity
            if (auto movedEntity = oldLoc.archetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
            {
                m_entityMap[*movedEntity].location = oldLoc.location;
            }
            
            // Update entity location
            Archetype* oldArchetype = oldLoc.archetype;
            oldLoc.archetype = newArchetype;
            oldLoc.location = newEntityLocation;
            
            // Update metrics for both archetypes
            UpdateArchetypeMetrics(oldArchetype);
            UpdateArchetypeMetrics(newArchetype);
            
            return newEntityLocation;
        }
        
        /**
         * Execute component move and add new component T
         * Directly transfers existing components and constructs new component in-place
         */
        template<Component T, typename... Args>
        void ExecuteComponentMoveAndAdd(EntityLocation dstEntityLocation, Archetype* dstArchetype, 
                                       EntityLocation srcEntityLocation, Archetype* srcArchetype, 
                                       Args&&... args)
        {
            // Get component info for both archetypes
            auto& dstComponents = dstArchetype->GetComponents();
            auto& srcComponents = srcArchetype->GetComponents();
            
            // Get chunks
            auto [dstChunk, dstEntityIdx] = dstArchetype->GetChunkAndIndex(dstEntityLocation);
            auto [srcChunk, srcEntityIdx] = srcArchetype->GetChunkAndIndex(srcEntityLocation);
            
            // Create index map for source components - use array for O(1) access
            std::array<size_t, MAX_COMPONENTS> srcIndexMap;
            srcIndexMap.fill(std::numeric_limits<size_t>::max()); // Use max as "not found" marker
            for (size_t i = 0; i < srcComponents.size(); ++i)
            {
                srcIndexMap[srcComponents[i].id] = i;
            }
            
            ComponentID newComponentId = TypeID<T>::Value();
            
            // Process each destination component
            for (size_t dstIdx = 0; dstIdx < dstComponents.size(); ++dstIdx)
            {
                auto& dstComp = dstComponents[dstIdx];
                void* dstPtr = dstChunk->GetComponentPointerCached(dstIdx, dstEntityIdx);
                
                if (dstComp.id == newComponentId) ASTRA_UNLIKELY
                {
                    // This is our new component - construct in-place
                    new (dstPtr) T(std::forward<Args>(args)...);
                }
                else
                {
                    // Check if component exists in source
                    size_t srcIdx = srcIndexMap[dstComp.id];
                    if (srcIdx != std::numeric_limits<size_t>::max()) ASTRA_LIKELY
                    {
                        // Move existing component from source
                        void* srcPtr = srcChunk->GetComponentPointerCached(srcIdx, srcEntityIdx);
                        dstComp.MoveConstruct(dstPtr, srcPtr);
                    }
                    // Note: Skip case shouldn't happen in AddComponent operations
                }
            }
        }
        
        template<Component T, typename... Args>
        void MoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entities, Args&&... args)
        {
            // Entities should already be sorted by caller for cache efficiency
            if (entities.empty()) return;
            
            // Move entities and components using the same pattern as regular AddComponent
            // Let AddEntityNoConstruct handle chunk allocation internally
            size_t processedCount = 0;
            for (size_t i = 0; i < entities.size(); ++i)
            {
                auto [entity, srcLocation] = entities[i];
                
                // Use AddEntityNoConstruct like the regular AddComponent does
                EntityLocation dstLocation = dstArchetype->AddEntityNoConstruct(entity);
                if (!dstLocation.IsValid()) ASTRA_UNLIKELY
                {
                    // This shouldn't happen since we pre-allocated chunks, but be safe
                    break;
                }
                
                // Move components and add the new one
                ExecuteComponentMoveAndAdd<T>(dstLocation, dstArchetype, srcLocation, srcArchetype, args...);
                
                // Update entity map
                m_entityMap[entity] = {dstArchetype, dstLocation};
                ++processedCount;
            }
            
            // Update first non-full chunk index for destination
            for (size_t idx = dstArchetype->m_firstNonFullChunkIdx; idx < dstArchetype->m_chunks.size(); ++idx)
            {
                if (!dstArchetype->m_chunks[idx]->IsFull())
                {
                    dstArchetype->m_firstNonFullChunkIdx = idx;
                    break;
                }
            }
            
            // Remove processed entities from source archetype in reverse order
            // Only remove the ones we actually moved
            for (size_t i = processedCount; i > 0; --i)
            {
                auto& [entity, location] = entities[i - 1];
                if (auto movedEntity = srcArchetype->RemoveEntity(location)) ASTRA_LIKELY
                {
                    m_entityMap[*movedEntity].location = location;
                }
            }
            
            UpdateArchetypeMetrics(srcArchetype);
            UpdateArchetypeMetrics(dstArchetype);
        }
        
    private:
        /**
         * Group entities by their current archetype for batch processing
         * @param entities Span of entities to group
         * @param filter Predicate to filter entities (e.g., check if component exists)
         * @return Map of archetypes to their entities and locations
         */
        template<typename Predicate>
        FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> 
        GroupEntitiesByArchetype(std::span<Entity> entities, Predicate&& filter)
        {
            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;
            
            for (Entity entity : entities)
            {
                auto it = m_entityMap.find(entity);
                if (it == m_entityMap.end()) ASTRA_UNLIKELY continue;
                
                EntityRecord& loc = it->second;
                if (filter(loc.archetype))
                {
                    batches[loc.archetype].emplace_back(entity, loc.location);
                }
            }
            
            return batches;
        }
        
        /**
         * Optimized batch move of entities with component addition
         * Uses new batch infrastructure for maximum performance
         */
        template<Component T, typename... Args>
        void BatchMoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype,
                                           SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch,
                                           Args&&... args)
        {
            // Check if already sorted (common case for batch-created entities)
            bool needsSort = false;
            for (size_t i = 1; i < entityBatch.size(); ++i)
            {
                if (entityBatch[i].second < entityBatch[i-1].second)
                {
                    needsSort = true;
                    break;
                }
            }
            
            // Only sort if necessary
            if (needsSort)
            {
                std::sort(entityBatch.begin(), entityBatch.end(), 
                    [](const auto& a, const auto& b) { return a.second < b.second; });
            }
            
            // Extract entities and source locations
            SmallVector<Entity, 256> entitiesToAdd;
            SmallVector<EntityLocation, 256> srcLocations;
            entitiesToAdd.reserve(entityBatch.size());
            srcLocations.reserve(entityBatch.size());
            
            for (const auto& [entity, location] : entityBatch)
            {
                entitiesToAdd.push_back(entity);
                srcLocations.push_back(location);
            }
            
            // Use new batch move infrastructure
            std::vector<EntityLocation> newLocations = dstArchetype->BatchMoveEntitiesFrom(
                entitiesToAdd, *srcArchetype, srcLocations);
            
            // Batch add the new component T
            dstArchetype->BatchSetComponent<T>(newLocations, T{args...});
            
            // Batch update entity map
            for (size_t i = 0; i < newLocations.size(); ++i)
            {
                m_entityMap[entityBatch[i].first] = {dstArchetype, newLocations[i]};
            }
            
            // Batch remove from source (defer chunk cleanup to avoid invalidating locations)
            auto movedEntities = srcArchetype->RemoveEntities(srcLocations, true);
            
            // Update locations of entities moved during removal
            for (const auto& [movedEntity, newLocation] : movedEntities)
            {
                auto it = m_entityMap.find(movedEntity);
                if (it != m_entityMap.end()) ASTRA_LIKELY
                {
                    it->second.location = newLocation;
                }
            }
            
            UpdateArchetypeMetrics(srcArchetype);
            UpdateArchetypeMetrics(dstArchetype);
        }
        
        /**
         * Optimized batch move of entities with component removal
         * Uses new batch infrastructure for maximum performance
         */
        void BatchMoveEntitiesWithoutComponent(Archetype* srcArchetype, Archetype* dstArchetype,
                                              SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch)
        {
            // Sort for cache-efficient access
            std::sort(entityBatch.begin(), entityBatch.end(), 
                [](const auto& a, const auto& b) { return a.second < b.second; });
            
            // Extract entities and source locations
            SmallVector<Entity, 256> entitiesToAdd;
            SmallVector<EntityLocation, 256> srcLocations;
            entitiesToAdd.reserve(entityBatch.size());
            srcLocations.reserve(entityBatch.size());
            
            for (const auto& [entity, location] : entityBatch)
            {
                entitiesToAdd.push_back(entity);
                srcLocations.push_back(location);
            }
            
            // Use new batch move infrastructure
            std::vector<EntityLocation> newLocations = dstArchetype->BatchMoveEntitiesFrom(
                entitiesToAdd, *srcArchetype, srcLocations);
            
            // Note: No need to set any component since we're removing one
            
            // Batch update entity map
            for (size_t i = 0; i < newLocations.size(); ++i)
            {
                m_entityMap[entityBatch[i].first] = {dstArchetype, newLocations[i]};
            }
            
            // Batch remove from source (defer chunk cleanup to avoid invalidating locations)
            auto movedEntities = srcArchetype->RemoveEntities(srcLocations, true);
            
            // Update locations of entities moved during removal
            for (const auto& [movedEntity, newLocation] : movedEntities)
            {
                auto it = m_entityMap.find(movedEntity);
                if (it != m_entityMap.end()) ASTRA_LIKELY
                {
                    it->second.location = newLocation;
                }
            }
            
            UpdateArchetypeMetrics(srcArchetype);
            UpdateArchetypeMetrics(dstArchetype);
        }
        
        void InitializeRootArchetype()
        {
            // Create root archetype (no components)
            auto rootArchetype = std::make_unique<Archetype>(ComponentMask{});
            m_rootArchetype = rootArchetype.get();
            
            // Set the pool for the root archetype
            m_rootArchetype->m_chunkPool = &m_chunkPool;
            
            // Initialize root archetype with empty component list
            m_rootArchetype->Initialize({});
            
            ArchetypeEntry entry;
            entry.archetype = std::move(rootArchetype);
            entry.creationGeneration = 0;  // Root archetype is generation 0
            // Metrics will be tracked when entities are added
            m_archetypes.push_back(std::move(entry));
        }
        
    private:
        // Archetype entry with metrics
        struct ArchetypeEntry
        {
            std::unique_ptr<Archetype> archetype;
            ArchetypeMetrics metrics;
            uint32_t creationGeneration;  // Generation when this archetype was created
        };
        
        ArchetypeChunkPool m_chunkPool;  // Pool owned by storage
        std::shared_ptr<ComponentRegistry> m_componentRegistry;  // Shared component registry
        ArchetypeGraph m_edgeGraph;  // Manages archetype transition graph
        std::vector<ArchetypeEntry> m_archetypes;
        FlatMap<ComponentMask, Archetype*, BitmapHash<MAX_COMPONENTS>> m_archetypeMap;
        std::unordered_map<Entity, EntityRecord> m_entityMap;
        
        Archetype* m_rootArchetype = nullptr;
        
        // Versioning for incremental updates
        std::atomic<uint32_t> m_structuralChangeCounter{0};  // Fast path check
        uint32_t m_generation = 1;  // Generation counter for new archetypes
        
        // Helper to update metrics when entity count changes
        void UpdateArchetypeMetrics(Archetype* archetype)
        {
            for (auto& entry : m_archetypes)
            {
                if (entry.archetype.get() == archetype)
                {
                    entry.metrics.UpdatePeak(archetype->GetEntityCount());
                    break;
                }
            }
        }
        
        // Helper method to remove archetype at index
        void RemoveArchetypeAt(size_t index)
        {
            ASTRA_ASSERT(index < m_archetypes.size(), "Invalid archetype index");
            
            Archetype* archetype = m_archetypes[index].archetype.get();
            
            // Remove from archetype map
            m_archetypeMap.Erase(archetype->GetMask());
            
            // Remove all edges involving this archetype
            RemoveArchetypeEdges(archetype);
            
            // Remove the archetype entry (this will destroy the archetype)
            m_archetypes.erase(m_archetypes.begin() + index);
        }
        
        // Remove all edges to/from an archetype
        void RemoveArchetypeEdges(Archetype* archetype)
        {
            // Remove edges pointing TO this archetype
            m_edgeGraph.RemoveEdgesTo(archetype);
            
            // Remove edges FROM this archetype
            m_edgeGraph.RemoveEdgesFrom(archetype);
        }
    };
}