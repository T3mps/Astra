#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Component/ResourceStorage.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/Signal.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "../Entity/EntityManager.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"
#include "Query.hpp"
#include "Relations.hpp"
#include "RelationshipGraph.hpp"
#include "View.hpp"

namespace Astra
{
    class Registry
    {
    public:
        struct Config
        {
            EntityManager::Config entityManagerConfig;
            ArchetypeChunkPool::Config chunkPoolConfig;
            ResourceStorage::Config resourceStorageConfig;
        };
        
        explicit Registry(const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig)
        {}
        
        Registry(const EntityManager::Config& entityConfig, const ArchetypeChunkPool::Config& chunkConfig) :
            m_entityManager(entityConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, chunkConfig)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry)
        {}
        
        Registry(std::shared_ptr<ComponentRegistry> componentRegistry, const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::move(componentRegistry)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig)),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_relationshipGraph(std::make_shared<RelationshipGraph>())
        {}
        
        explicit Registry(const Registry& other, const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(other.m_componentRegistry),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig)),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_relationshipGraph(std::make_shared<RelationshipGraph>())
        {}

        ~Registry() = default;
        
        template<Component... Components>
        Entity CreateEntity()
        {
            Entity entity = m_entityManager.Create();
            
            // Use AddEntity for default-constructed components
            m_archetypeManager->AddEntity<Components...>(entity);
            
            m_signalManager.Emit<Events::EntityCreated>(entity);
            
            if constexpr (sizeof...(Components) > 0)
            {
                if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                {
                    // Get the entity location to retrieve components for signals
                    auto* record = m_archetypeManager->GetEntityRecord(entity);
                    if (record)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<Components>::Value(), record->archetype->GetComponent<Components>(record->location))), ...);
                    }
                }
            }
            
            return entity;
        }
        
        template<Component... Components>
        Entity CreateEntityWith(Components&&... components)
        {
            Entity entity = m_entityManager.Create();
            
            // Use AddEntityWith for components with values
            m_archetypeManager->AddEntityWith(entity, std::forward<Components>(components)...);
            
            m_signalManager.Emit<Events::EntityCreated>(entity);
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                // Get the entity location to retrieve components for signals
                auto* record = m_archetypeManager->GetEntityRecord(entity);
                if (record)
                {
                    ((m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<std::decay_t<Components>>::Value(), record->archetype->GetComponent<std::decay_t<Components>>(record->location))), ...);
                }
            }
            
            return entity;
        }

        template<Component... Components>
        void CreateEntities(size_t count, std::span<Entity> outEntities)
        {
            if (count == 0 || outEntities.size() < count)
                return;
            
            m_entityManager.CreateBatch(count, outEntities.begin());
            
            // Use unified batch AddEntities - handles archetype selection internally
            m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, count));
            
            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }
            
            if constexpr (sizeof...(Components) > 0)
            {
                if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
                    }
                }
            }
        }

        template<Component... Components, std::invocable<size_t> Generator>
        void CreateEntitiesWith(size_t count, std::span<Entity> outEntities, Generator&& generator)
        {
            if (count == 0 || outEntities.size() < count)
                return;
            
            m_entityManager.CreateBatch(count, outEntities.begin());
            m_archetypeManager->AddEntitiesWith<Components...>(outEntities.subspan(0, count), std::forward<Generator>(generator));
            
            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (size_t i = 0; i < count; ++i)
                {
                    ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
                }
            }
        }

        void DestroyEntity(Entity entity)
        {
            if (!m_entityManager.IsValid(entity))
                return;
            
            m_signalManager.Emit<Events::EntityDestroyed>(entity);
            
            m_archetypeManager->RemoveEntity(entity);
            m_relationshipGraph->OnEntityDestroyed(entity);
            m_entityManager.Destroy(entity);
        }
        
        void DestroyEntities(std::span<Entity> entities)
        {
            if (entities.empty())
                return;
            
            std::vector<Entity> validEntities;
            
            size_t reserveSize = std::min(entities.size(), size_t(10000));
            validEntities.reserve(reserveSize);
            
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            
            if (validEntities.empty())
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed))
            {
                for (Entity entity : validEntities)
                {
                    m_signalManager.Emit<Events::EntityDestroyed>(entity);
                }
            }
                
            m_archetypeManager->RemoveEntities(validEntities);
            
            for (Entity entity : validEntities)
            {
                m_relationshipGraph->OnEntityDestroyed(entity);
            }
            
            for (Entity entity : validEntities)
            {
                m_entityManager.Destroy(entity);
            }
        }

        ASTRA_NODISCARD bool IsValid(Entity entity) const noexcept
        {
            return m_entityManager.IsValid(entity);
        }

        template<Component T>
        void AddComponent(Entity entity, const T& component)
        {
            if (!m_entityManager.IsValid(entity))
                return;
                
            T* newComponent = m_archetypeManager->AddComponent<T>(entity, component);
            
            if (newComponent)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), newComponent);
            }
        }
        
        template<Component T, typename... Args>
        void EmplaceComponent(Entity entity, Args&&... args)
        {
            if (!m_entityManager.IsValid(entity))
                return;
                
            T* component = m_archetypeManager->AddComponent<T>(entity, std::forward<Args>(args)...);
            
            if (component)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
            }
        }

        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            if (!m_entityManager.IsValid(entity))
                return false;
            
            T* component = m_archetypeManager->GetComponent<T>(entity);
            bool removed = m_archetypeManager->RemoveComponent<T>(entity);
            
            if (removed && component)
            {
                m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);
            }
            
            return removed;
        }
        
        template<Component T>
        void AddComponents(std::span<Entity> entities, const T& component)
        {
            m_archetypeManager->AddComponents<T>(entities, component);
            
            // Emit signals for all entities if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                ComponentID componentId = TypeID<T>::Value();
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        T* comp = m_archetypeManager->GetComponent<T>(entity);
                        if (comp)
                        {
                            m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, comp);
                        }
                    }
                }
            }
        }
        
        template<Component T, typename... Args>
        void EmplaceComponents(std::span<Entity> entities, Args&&... args)
        {
            if (entities.empty())
                return;
            
            // Filter out invalid entities
            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());
            
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            
            if (validEntities.empty())
                return;
            
            // Batch add components
            m_archetypeManager->AddComponents<T>(validEntities, std::forward<Args>(args)...);
            
            // Emit signals if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (Entity entity : validEntities)
                {
                    T* component = m_archetypeManager->GetComponent<T>(entity);
                    if (component)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
                    }
                }
            }
        }
        
        template<Component T>
        size_t RemoveComponents(std::span<Entity> entities)
        {
            if (entities.empty())
                return 0;
            
            // Filter out invalid entities and collect components for signals
            SmallVector<Entity, 256> validEntities;
            SmallVector<T*, 256> componentsToRemove;
            validEntities.reserve(entities.size());
            
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                componentsToRemove.reserve(entities.size());
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        T* component = m_archetypeManager->GetComponent<T>(entity);
                        if (component)
                        {
                            validEntities.push_back(entity);
                            componentsToRemove.push_back(component);
                        }
                    }
                }
            }
            else
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        validEntities.push_back(entity);
                    }
                }
            }
            
            if (validEntities.empty())
                return 0;
            
            // Batch remove components
            size_t removedCount = m_archetypeManager->RemoveComponents<T>(validEntities);
            
            // Emit signals if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                for (size_t i = 0; i < removedCount && i < componentsToRemove.size(); ++i)
                {
                    m_signalManager.Emit<Events::ComponentRemoved>(validEntities[i], TypeID<T>::Value(), componentsToRemove[i]);
                }
            }
            
            return removedCount;
        }

        template<Component T>
        ASTRA_NODISCARD T* GetComponent(Entity entity)
        {
            if (!m_entityManager.IsValid(entity))
                return nullptr;
            return m_archetypeManager->GetComponent<T>(entity);
        }
        
        template<Component T>
        ASTRA_NODISCARD const T* GetComponent(Entity entity) const
        {
            if (!m_entityManager.IsValid(entity))
                return nullptr;
            return m_archetypeManager->GetComponent<T>(entity);
        }
        
        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            if (!m_entityManager.IsValid(entity))
                return false;
            return m_archetypeManager->HasComponent<T>(entity);
        }

        // Resource (singleton component) management
        
        template<Component T>
        ASTRA_NODISCARD T* GetResource() noexcept
        {
            return m_resourceStorage.Get<T>();
        }

        template<Component T>
        ASTRA_NODISCARD const T* GetResource() const noexcept
        {
            return m_resourceStorage.Get<T>();
        }

        template<Component T>
        T* SetResource(T&& resource)
        {
            ComponentID id = TypeID<T>::Value();
            bool isNew = !m_resourceStorage.Has<T>();
            
            T* result = m_resourceStorage.Set(std::forward<T>(resource));
            
            if (isNew)
            {
                m_signalManager.Emit<Events::ResourceAdded>(id, result);
            }
            else
            {
                m_signalManager.Emit<Events::ResourceUpdated>(id, result);
            }
            
            return result;
        }
        
        template<Component T, typename... Args>
        T* EmplaceResource(Args&&... args)
        {
            ComponentID id = TypeID<T>::Value();
            bool isNew = !m_resourceStorage.Has<T>();
            
            T* result = m_resourceStorage.Emplace<T>(std::forward<Args>(args)...);
            
            if (isNew)
            {
                m_signalManager.Emit<Events::ResourceAdded>(id, result);
            }
            else
            {
                m_signalManager.Emit<Events::ResourceUpdated>(id, result);
            }
            
            return result;
        }

        template<Component T>
        ASTRA_NODISCARD bool HasResource() const noexcept
        {
            return m_resourceStorage.Has<T>();
        }

        template<Component T>
        void RemoveResource()
        {
            ComponentID id = TypeID<T>::Value();
            T* resource = m_resourceStorage.Get<T>();
            
            if (resource)
            {
                m_signalManager.Emit<Events::ResourceRemoved>(id, resource);
                m_resourceStorage.Remove<T>();
            }
        }
        
        void ClearResources()
        {
            // Note: We don't emit individual ResourceRemoved events here for performance
            // If needed, users can listen to a bulk clear event or iterate resources first
            m_resourceStorage.Clear();
        }
        
        // View creation
        
        template<ValidQueryArg... QueryArgs>
        ASTRA_NODISCARD auto CreateView()
        {
            return View<QueryArgs...>(m_archetypeManager);
        }
        
        void Clear()
        {
            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed))
            {
                // TODO: Consider if we want to emit signals during Clear()
            }
            
            m_archetypeManager = std::make_shared<ArchetypeManager>(m_componentRegistry);

            m_relationshipGraph->Clear();
            
            m_entityManager.Clear();
            
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept
        {
            return m_entityManager.Size();
        }
        

        ASTRA_NODISCARD bool IsEmpty() const noexcept
        {
            return Size() == 0;
        }

        ASTRA_NODISCARD EntityManager& GetEntityManager() noexcept { return m_entityManager; }
        ASTRA_NODISCARD const EntityManager& GetEntityManager() const noexcept { return m_entityManager; }
        ASTRA_NODISCARD ComponentRegistry* GetComponentRegistry() noexcept { return m_componentRegistry.get(); }
        ASTRA_NODISCARD const ComponentRegistry* GetComponentRegistry() const noexcept { return m_componentRegistry.get(); }
        ASTRA_NODISCARD ArchetypeManager* GetArchetypeManager() noexcept { return m_archetypeManager.get(); }
        ASTRA_NODISCARD const ArchetypeManager* GetArchetypeManager() const noexcept { return m_archetypeManager.get(); }
        
        ASTRA_NODISCARD float GetFragmentationLevel() const
        {
            auto archetypes = m_archetypeManager->GetArchetypes();
            if (archetypes.empty())
                return 0.0f;

            size_t totalEntities = 0;
            size_t totalChunks = 0;
            size_t optimalChunks = 0;

            for (const auto* arch : archetypes)
            {
                size_t entityCount = arch->GetEntityCount();
                size_t chunkCount = arch->GetChunks().size();

                totalEntities += entityCount;
                totalChunks += chunkCount;

                // Calculate optimal chunks for this archetype
                if (entityCount > 0)
                {
                    size_t entitiesPerChunk = arch->GetEntitiesPerChunk();
                    optimalChunks += (entityCount + entitiesPerChunk - 1) / entitiesPerChunk;
                }
            }

            if (totalChunks == 0) return 0.0f;

            // Fragmentation = excess chunks / total chunks
            size_t excessChunks = totalChunks > optimalChunks ? totalChunks - optimalChunks : 0;
            return static_cast<float>(excessChunks) / static_cast<float>(totalChunks);
        }
        
        struct DefragmentationOptions
        {
            size_t minArchetypesToKeep = 8;           // Never go below this many archetypes
            size_t maxArchetypesToRemove = 10;        // Limit per call (for incremental)
            
            bool defragmentChunks = true;             // Enable chunk coalescing
            float chunkUtilizationThreshold = 0.5f;   // Defragment archetypes when chunks fall below this utilization (0.5 = 50% full)
            size_t maxChunksToProcess = 100;          // Limit chunks processed per call
            
            bool defragmentPool = true;               // Enable memory pool defragmentation
            
            size_t maxEntitiesToMove = 10000;         // Total entity move budget
            bool incremental = false;                 // If true, strictly respect all limits
        };
        
        struct DefragmentationResult
        {
            size_t archetypesRemoved = 0;
            size_t chunksRemoved = 0;
            size_t entitiesMoved = 0;
            size_t archetypesProcessed = 0;
            size_t poolBlocksReleased = 0;
            size_t poolMemoryFreed = 0;
            float fragmentationBefore = 0.0f;
            float fragmentationAfter = 0.0f;
            
            ASTRA_NODISCARD bool DidWork() const noexcept 
            { 
                return archetypesRemoved > 0 || chunksRemoved > 0 || entitiesMoved > 0 || poolBlocksReleased > 0; 
            }
        };
        
        DefragmentationResult Defragment(const DefragmentationOptions& options = {})
        {
            DefragmentationResult result;
            
            // Calculate initial fragmentation
            result.fragmentationBefore = GetFragmentationLevel();
            
            size_t totalEntitiesMoved = 0;
            auto archetypes = m_archetypeManager->GetArchetypes();
            
            // Step 1: Defragment chunks within archetypes
            if (options.defragmentChunks)
            {
                size_t chunksProcessed = 0;
                
                for (auto* arch : archetypes)
                {
                    // Check limits
                    if (options.incremental)
                    {
                        if (chunksProcessed >= options.maxChunksToProcess) break;
                        if (totalEntitiesMoved >= options.maxEntitiesToMove) break;
                    }
                    
                    // Skip if archetype has few chunks or is already well-packed
                    if (arch->GetChunks().size() <= 1) continue;
                    
                    // Only process archetypes with significant fragmentation
                    // Fragmentation level: 0.0 = perfectly packed, 1.0 = all wasted space
                    // Skip if fragmentation is below threshold (e.g., < 50% wasted space)
                    float archFragmentation = arch->GetFragmentationLevel();
                    float fragmentationThreshold = 1.0f - options.chunkUtilizationThreshold;
                    if (archFragmentation < fragmentationThreshold) continue;
                    
                    // Perform chunk coalescing with the configured threshold
                    auto [chunksFreed, movedEntities] = arch->CoalesceChunks(options.chunkUtilizationThreshold);
                    
                    // Update entity locations in ArchetypeManager
                    for (const auto& [entity, newLocation] : movedEntities)
                    {
                        m_archetypeManager->SetEntityLocation(entity, arch, newLocation);
                    }
                    
                    result.chunksRemoved += chunksFreed;
                    totalEntitiesMoved += movedEntities.size();
                    chunksProcessed += arch->GetChunks().size();
                    result.archetypesProcessed++;
                    
                    // Break if we've hit our entity move budget
                    if (options.incremental && totalEntitiesMoved >= options.maxEntitiesToMove)
                    {
                        break;
                    }
                }
                
                result.entitiesMoved = totalEntitiesMoved;
            }
            
            // Step 2: Remove empty archetypes
            // Only do this if we haven't exhausted our limits
            if (!options.incremental || totalEntitiesMoved < options.maxEntitiesToMove)
            {
                // Build defragmentation options for archetype manager
                ArchetypeManager::DefragmentOptions archOpts;
                archOpts.minArchetypesToKeep = options.minArchetypesToKeep;
                archOpts.maxArchetypesToRemove = options.maxArchetypesToRemove;
                
                // If we're in incremental mode and close to entity limit, reduce archetype removals
                if (options.incremental && totalEntitiesMoved > options.maxEntitiesToMove * 0.8f)
                {
                    archOpts.maxArchetypesToRemove = std::min(size_t(2), options.maxArchetypesToRemove);
                }
                
                // Defragment archetypes (currently removes empty ones, could be extended)
                auto archResult = m_archetypeManager->Defragment(archOpts);
                result.archetypesRemoved = archResult.emptyArchetypesRemoved;
            }
            
            // Step 3: Defragment memory pool to release unused blocks
            if (options.defragmentPool)
            {
                auto& pool = m_archetypeManager->GetChunkPool();
                auto poolResult = pool.Defragment();
                result.poolBlocksReleased = poolResult.blocksReleased;
                result.poolMemoryFreed = poolResult.bytesFreed;
            }
            
            // Calculate final fragmentation
            result.fragmentationAfter = GetFragmentationLevel();
            
            return result;
        }
        
        ASTRA_NODISCARD size_t GetArchetypeCount() const
        {
            return m_archetypeManager->GetArchetypeCount();
        }
        
        ASTRA_NODISCARD size_t GetArchetypeMemoryUsage() const
        {
            return m_archetypeManager->GetArchetypeMemoryUsage();
        }

        template<Component... Components>
        ASTRA_NODISCARD Archetype* FindArchetype() const
        {
            return m_archetypeManager->FindArchetype<Components...>();
        }
        
        ASTRA_NODISCARD Archetype* FindArchetype(const ComponentMask& mask) const
        {
            return m_archetypeManager->FindArchetype(mask);
        }
        
        ASTRA_NODISCARD auto GetAllArchetypes()
        {
            return m_archetypeManager->GetArchetypes();
        }

        template<typename... QueryArgs>
        ASTRA_NODISCARD Relations<QueryArgs...> GetRelations(Entity entity) const
        {
            return Relations<QueryArgs...>(m_archetypeManager, entity, m_relationshipGraph);
        }
        
        void SetParent(Entity child, Entity parent)
        {
            if (m_entityManager.IsValid(child) && m_entityManager.IsValid(parent))
            {
                m_relationshipGraph->SetParent(child, parent);
                
                // Emit parent changed signal
                m_signalManager.Emit<Events::ParentChanged>(child, parent);
            }
        }

        void RemoveParent(Entity child)
        {
            if (m_entityManager.IsValid(child))
            {
                // Get current parent before removal for signal
                Entity parent = m_relationshipGraph->GetParent(child);
                m_relationshipGraph->RemoveParent(child);
                
                // Emit parent changed signal (parent is now invalid)
                if (parent.IsValid())
                {
                    m_signalManager.Emit<Events::ParentChanged>(child, Entity::Invalid());
                }
            }
        }

        void AddChild(Entity parent, Entity child)
        {
            SetParent(child, parent);
        }

        bool RemoveChild(Entity parent, Entity child)
        {
            if (!m_entityManager.IsValid(parent) || !m_entityManager.IsValid(child))
                return false;
                
            Entity currentParent = m_relationshipGraph->GetParent(child);
            if (currentParent == parent)
            {
                RemoveParent(child);
                return true;
            }
            return false;
        }
        
        size_t RemoveAllChildren(Entity parent)
        {
            if (!m_entityManager.IsValid(parent))
                return 0;
                
            auto children = GetChildren(parent);
            for (Entity child : children)
            {
                RemoveParent(child);
            }
            return children.size();
        }

        ASTRA_NODISCARD Entity GetParent(Entity child) const
        {
            if (!m_entityManager.IsValid(child))
                return Entity::Invalid();
            return m_relationshipGraph->GetParent(child);
        }

        ASTRA_NODISCARD std::vector<Entity> GetChildren(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return {};
                
            // Direct access to avoid copy overhead
            const auto& children = m_relationshipGraph->GetChildren(parent);
            return std::vector<Entity>(children.begin(), children.end());
        }
        
        template<typename Func>
        void ForEachChild(Entity parent, Func&& func) const
        {
            if (!m_entityManager.IsValid(parent))
                return;
                
            const auto& children = m_relationshipGraph->GetChildren(parent);
            for (Entity child : children)
            {
                func(child);
            }
        }

        ASTRA_NODISCARD size_t GetChildCount(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return 0;
            return m_relationshipGraph->GetChildCount(parent);
        }
        
        ASTRA_NODISCARD bool HasChildren(Entity parent) const
        {
            if (!m_entityManager.IsValid(parent))
                return false;
            return m_relationshipGraph->HasChildren(parent);
        }
        
        ASTRA_NODISCARD bool HasParent(Entity child) const
        {
            return GetParent(child).IsValid();
        }
        
        ASTRA_NODISCARD bool IsParentOf(Entity parent, Entity child) const
        {
            return GetParent(child) == parent;
        }
        
        ASTRA_NODISCARD bool IsChildOf(Entity child, Entity parent) const
        {
            return GetParent(child) == parent;
        }

        void AddLink(Entity a, Entity b)
        {
            if (m_entityManager.IsValid(a) && m_entityManager.IsValid(b))
            {
                m_relationshipGraph->AddLink(a, b);
                
                // Emit link added signal
                m_signalManager.Emit<Events::LinkAdded>(a, b);
            }
        }
        
        void RemoveLink(Entity a, Entity b)
        {
            if (m_entityManager.IsValid(a) && m_entityManager.IsValid(b))
            {
                m_relationshipGraph->RemoveLink(a, b);
                
                // Emit link removed signal
                m_signalManager.Emit<Events::LinkRemoved>(a, b);
            }
        }

        ASTRA_NODISCARD RelationshipGraph& GetRelationshipGraph() { return *m_relationshipGraph; }
        ASTRA_NODISCARD const RelationshipGraph& GetRelationshipGraph() const { return *m_relationshipGraph; }
        
        void EnableSignals(Signal signals)
        {
            m_signalManager.EnableSignals(signals);
        }
        
        void DisableSignals(Signal signals)
        {
            m_signalManager.DisableSignals(signals);
        }
        
        void SetEnabledSignals(Signal signals)
        {
            m_signalManager.SetEnabledSignals(signals);
        }

        ASTRA_NODISCARD Signal GetEnabledSignals() const
        {
            return m_signalManager.GetEnabledSignals();
        }
        
        ASTRA_NODISCARD SignalManager* GetSignalManager() noexcept
        {
            return &m_signalManager;
        }
        
        ASTRA_NODISCARD const SignalManager* GetSignalManager() const noexcept
        {
            return &m_signalManager;
        }
        
        // ====================== Serialization API ======================
        
        struct SaveConfig
        {
            CompressionMode compressionMode = CompressionMode::LZ4;
            Compression::CompressionLevel compressionLevel = Compression::CompressionLevel::Fast;
            size_t compressionThreshold = 1024; // Only compress blocks larger than this
        };
        
        Result<void, SerializationError> Save(const std::filesystem::path& path, const SaveConfig& config = SaveConfig{}) const
        {
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(path, writerConfig);
            if (writer.HasError())
            {
                return Result<void, SerializationError>::Err(SerializationError::IOError);
            }
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager.Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager.Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_relationshipGraph->Serialize(writer);
            
            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<void, SerializationError>::Err(writer.GetError()) : 
                Result<void, SerializationError>::Ok();
        }
        
        Result<std::vector<std::byte>, SerializationError> Save(const SaveConfig& config = SaveConfig{}) const
        {
            std::vector<std::byte> buffer;
            
            BinaryWriter::Config writerConfig;
            writerConfig.compressionMode = config.compressionMode;
            writerConfig.compressionLevel = config.compressionLevel;
            writerConfig.compressionThreshold = config.compressionThreshold;
            
            BinaryWriter writer(buffer, writerConfig);
            
            // Write header
            BinaryHeader header;
            header.entityCount = static_cast<uint32_t>(m_entityManager.Size());
            header.archetypeCount = static_cast<uint32_t>(m_archetypeManager->GetArchetypeCount());
            
            writer.WriteHeader(header);
            
            // Serialize components
            m_entityManager.Serialize(writer);
            m_archetypeManager->Serialize(writer);
            m_relationshipGraph->Serialize(writer);
            
            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<std::vector<std::byte>, SerializationError>::Err(writer.GetError()) : 
                Result<std::vector<std::byte>, SerializationError>::Ok(std::move(buffer));
        }

        static Result<std::unique_ptr<Registry>, SerializationError> Load(const std::filesystem::path& path, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(path);
            if (reader.HasError())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            }
            
            return LoadInternal(reader, std::move(componentRegistry));
        }

        static Result<std::unique_ptr<Registry>, SerializationError> Load(std::span<const std::byte> data, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry));
        }
        
    private:
        /**
         * Internal helper to load registry from reader
         */
        static Result<std::unique_ptr<Registry>, SerializationError> LoadInternal(BinaryReader& reader, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            // Read and validate header
            auto headerResult = reader.ReadHeader();
            if (headerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*headerResult.GetError());
            }
            
            // Deserialize EntityManager
            auto managerResult = EntityManager::Deserialize(reader);
            if (managerResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*managerResult.GetError());
            }
            
            // Create new registry instance with the provided component registry
            auto registry = std::make_unique<Registry>(componentRegistry);
            
            // Move the entity manager from unique_ptr  
            registry->m_entityManager = std::move(*(*managerResult.GetValue()));
            
            // Create new ArchetypeManager with the component registry and deserialize into it
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry);
            if (!registry->m_archetypeManager->Deserialize(reader))
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::CorruptedData);
            }
            
            // Deserialize RelationshipGraph
            auto graphResult = RelationshipGraph::Deserialize(reader);
            if (graphResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*graphResult.GetError());
            }
            registry->m_relationshipGraph = std::make_shared<RelationshipGraph>(std::move(*graphResult.GetValue()));
            
            // Verify checksum
            auto checksumResult = reader.VerifyChecksum();
            if (checksumResult.IsErr())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(*checksumResult.GetError());
            }
            
            return Result<std::unique_ptr<Registry>, SerializationError>::Ok(std::move(registry));
        }
        
        EntityManager m_entityManager;
        std::shared_ptr<ComponentRegistry> m_componentRegistry;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        std::shared_ptr<RelationshipGraph> m_relationshipGraph;
        SignalManager m_signalManager;
        ResourceStorage m_resourceStorage;
    };
}
