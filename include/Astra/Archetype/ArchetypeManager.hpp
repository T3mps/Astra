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
    template<typename... QueryArgs>
    class View;
    
    class ArchetypeManager
    {
    public:
        struct EntityRecord
        {
            Archetype* archetype = nullptr;
            EntityLocation location;
        };
        
        explicit ArchetypeManager(std::shared_ptr<ComponentRegistry> registry, const ArchetypeChunkPool::Config& poolConfig = {}) :
            m_chunkPool(poolConfig),
            m_componentRegistry(registry)
        {
            ASTRA_ASSERT(registry, "ComponentRegistry must not be null");

            auto rootArchetype = std::make_unique<Archetype>(ComponentMask{});
            m_rootArchetype = rootArchetype.get();
            m_rootArchetype->m_chunkPool = &m_chunkPool;
            m_rootArchetype->Initialize({});

            ArchetypeEntry entry;
            entry.archetype = std::move(rootArchetype);
            entry.creationGeneration = 0;  // Root archetype is generation 0
            m_archetypes.push_back(std::move(entry));
        }

        template<Component... Components>
        void AddEntity(Entity entity)
        {
            Archetype* archetype;
            if constexpr (sizeof...(Components) == 0)
            {
                archetype = m_rootArchetype;
            }
            else
            {
                archetype = GetOrCreateArchetype<Components...>();
            }
            
            EntityLocation location = archetype->AddEntity(entity);
            
            if (!location.IsValid()) ASTRA_UNLIKELY
            {
                return;
            }
            
            m_entityMap[entity] = EntityRecord{archetype, location};
        }
        
        template<Component... Components>
        void AddEntityWith(Entity entity, Components&&... components)
        {
            static_assert(sizeof...(Components) > 0, "AddEntityWith requires at least one component");
            
            Archetype* archetype = GetOrCreateArchetype<std::decay_t<Components>...>();
            EntityLocation location = archetype->AddEntityWith(entity, std::forward<Components>(components)...);
            
            if (!location.IsValid()) ASTRA_UNLIKELY
            {
                return;
            }
            
            m_entityMap[entity] = EntityRecord{archetype, location};
        }
        
        template<Component... Components>
        void AddEntities(std::span<const Entity> entities)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            Archetype* archetype;
            if constexpr (sizeof...(Components) == 0)
            {
                archetype = m_rootArchetype;
            }
            else
            {
                archetype = GetOrCreateArchetype<Components...>();
            }

            std::vector<EntityLocation> locations;
            if constexpr (sizeof...(Components) == 0)
            {
                locations = archetype->AddEntities(entities);
            }
            else
            {
                auto generator = [](size_t) { return std::make_tuple(Components{}...); };
                locations = archetype->AddEntitiesWith(entities, generator);
            }

            m_entityMap.reserve(m_entityMap.size() + locations.size());
            for (size_t i = 0; i < locations.size(); ++i)
            {
                m_entityMap[entities[i]] = EntityRecord{archetype, locations[i]};
            }
        }
        
        template<Component... Components, std::invocable<size_t> Generator>
        void AddEntitiesWith(std::span<const Entity> entities, Generator&& generator)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            Archetype* archetype = GetOrCreateArchetype<Components...>();
            std::vector<EntityLocation> locations = archetype->AddEntitiesWith(entities, std::forward<Generator>(generator));

            m_entityMap.reserve(m_entityMap.size() + locations.size());
            for (size_t i = 0; i < locations.size(); ++i)
            {
                m_entityMap[entities[i]] = EntityRecord{archetype, locations[i]};
            }
        }

        void RemoveEntity(Entity entity)
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return;

            EntityRecord& loc = it->second;

            if (auto movedEntity = loc.archetype->RemoveEntity(loc.location)) ASTRA_LIKELY
            {
                auto movedIt = m_entityMap.find(*movedEntity);
                ASTRA_ASSERT(movedIt != m_entityMap.end(), "Moved entity not found in map");
                movedIt->second.location = loc.location;
            }
            
            m_entityMap.erase(it);
        }

        void RemoveEntities(std::span<Entity> entities)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return;

            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;

            for (Entity entity : entities)
            {
                auto it = m_entityMap.find(entity);
                if (it == m_entityMap.end()) ASTRA_UNLIKELY continue;

                EntityRecord& loc = it->second;
                batches[loc.archetype].emplace_back(entity, loc.location);
            }

            for (auto& [archetype, entityBatch] : batches)
            {
                SmallVector<EntityLocation, 8> locations;
                locations.reserve(entityBatch.size());
                for (const auto& [entity, location] : entityBatch)
                {
                    locations.push_back(location);
                }

                auto movedEntities = archetype->RemoveEntities(locations);

                for (const auto& [movedEntity, newEntityLocation] : movedEntities)
                {
                    auto movedIt = m_entityMap.find(movedEntity);
                    if (movedIt != m_entityMap.end()) ASTRA_LIKELY
                    {
                        movedIt->second.location = newEntityLocation;
                    }
                }

                for (const auto& [entity, _] : entityBatch)
                {
                    m_entityMap.erase(entity);
                }
            }
        }

        ASTRA_NODISCARD const EntityRecord* GetEntityRecord(Entity entity) const
        {
            auto it = m_entityMap.find(entity);
            return it != m_entityMap.end() ? &it->second : nullptr;
        }
        
        void SetEntityLocation(Entity entity, Archetype* archetype, EntityLocation location)
        {
            m_entityMap[entity] = EntityRecord{archetype, location};
        }

        template<Component T, typename... Args>
        T* AddComponent(Entity entity, Args&&... args)
        {
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;
            registry->RegisterComponent<T>();
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return nullptr;
            
            EntityRecord& oldLoc = it->second;
            ComponentID componentId = TypeID<T>::Value();
            
            if (oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return nullptr;
                
            Archetype* newArchetype = GetArchetypeWithAdded(oldLoc.archetype, componentId);
            EntityLocation newEntityLocation = MoveEntityWithComponent<T>(entity, oldLoc, newArchetype, std::forward<Args>(args)...);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                return nullptr;
            }
            
            return newArchetype->GetComponent<T>(newEntityLocation);
        }

        template<Component T, typename... Args>
        void AddComponents(std::span<Entity> entities, Args&&... args)
        {
#ifdef ASTRA_BUILD_DEBUG
            printf("ArchetypeManager::AddComponents called with %zu entities\n", entities.size());
#endif
            if (entities.empty())
                return;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return;
            registry->RegisterComponent<T>();
            ComponentID componentID = TypeID<T>::Value();

            // First, ensure all entities exist in the archetype system
            // Entities not in the map need to be added to the root archetype
            SmallVector<Entity, 256> newEntities;
            for (Entity entity : entities)
            {
                auto it = m_entityMap.find(entity);
                if (it == m_entityMap.end())
                {
                    newEntities.push_back(entity);
#ifdef ASTRA_BUILD_DEBUG
                    printf("  Entity %u not in map, will add to root\n", entity.GetID());
#endif
                }
                else
                {
#ifdef ASTRA_BUILD_DEBUG
                    printf("  Entity %u already in archetype %p\n", entity.GetID(), it->second.archetype);
#endif
                }
            }
            
            // Add new entities to root archetype
            if (!newEntities.empty())
            {
#ifdef ASTRA_BUILD_DEBUG
                printf("  Adding %zu new entities to root archetype\n", newEntities.size());
#endif
                auto locations = m_rootArchetype->AddEntities(newEntities);
                for (size_t i = 0; i < locations.size(); ++i)
                {
                    m_entityMap[newEntities[i]] = EntityRecord{m_rootArchetype, locations[i]};
                }
            }

            // Group entities by archetype, including newly added ones
            auto batches = GroupEntitiesByArchetype(entities, 
                [componentID](Archetype* arch)
                {
                    return !arch->GetMask().Test(componentID);
                });

#ifdef ASTRA_BUILD_DEBUG
            printf("  Grouped into %zu archetype batches\n", batches.Size());
#endif

            // Process each archetype group
            for (auto& [srcArchetype, entityBatch] : batches)
            {
                if (entityBatch.empty())
                {
                    continue;
                }

                Archetype* dstArchetype = GetArchetypeWithAdded(srcArchetype, componentID);

                // Execute optimized batch move with component addition
                BatchMoveEntitiesWithComponent<T>(srcArchetype, dstArchetype, entityBatch, std::forward<Args>(args)...);
            }
        }

        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            ComponentID componentId = TypeID<T>::Value();
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY
                return false;
            
            EntityRecord& oldLoc = it->second;
            
            if (!oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;
                
            Archetype* newArchetype = GetArchetypeWithRemoved(oldLoc.archetype, componentId);
            EntityLocation newEntityLocation = MoveEntity(entity, oldLoc, newArchetype);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                return false;  // Critical: component destroyed but entity couldn't be moved
            }
            
            return true;
        }

        template<Component T>
        size_t RemoveComponents(std::span<Entity> entities)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return 0;

            ComponentID componentId = TypeID<T>::Value();

            auto batches = GroupEntitiesByArchetype(entities,
                [componentId](Archetype* arch)
                {
                    return arch->GetMask().Test(componentId);
                });

            size_t removedCount = 0;

            for (auto& [srcArchetype, entityBatch] : batches)
            {
                if (entityBatch.empty()) continue;

                Archetype* dstArchetype = GetArchetypeWithRemoved(srcArchetype, componentId);
                BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
                removedCount += entityBatch.size();
            }

            return removedCount;
        }

        /**
         * Type-erased component addition for use by CommandBuffer.
         * Adds a component to an entity using the component ID and raw data pointer.
         * The component must already be registered in the ComponentRegistry.
         *
         * @param entity The entity to add the component to
         * @param componentId The ComponentID of the component to add
         * @param data Pointer to the source component data (will be copy-constructed)
         * @param dataSize Size of the component data (for validation)
         * @return true if component was added successfully, false otherwise
         */
        bool AddComponentByID(Entity entity, ComponentID componentId, const void* data, size_t dataSize)
        {
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return false;

            const ComponentDescriptor* desc = registry->GetComponentDescriptor(componentId);
            if (!desc) ASTRA_UNLIKELY
                return false;

            // Validate data size matches component size
            if (dataSize != desc->size && desc->size > 0) ASTRA_UNLIKELY
                return false;

            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY
                return false;

            EntityRecord& oldLoc = it->second;

            // Check if entity already has this component
            if (oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;

            // Get or create the target archetype
            Archetype* newArchetype = GetArchetypeWithAdded(oldLoc.archetype, componentId);
            if (!newArchetype) ASTRA_UNLIKELY
                return false;

            // Move entity to new archetype with the new component
            EntityLocation newEntityLocation = MoveEntityWithComponentByID(entity, oldLoc, newArchetype, componentId, data, *desc);

            return newEntityLocation.IsValid();
        }

        /**
         * Type-erased batch component addition for use by CommandBuffer.
         * Adds a component to multiple entities using the component ID and raw data pointer.
         *
         * @param entities Span of entities to add the component to
         * @param componentId The ComponentID of the component to add
         * @param data Pointer to the source component data (will be copy-constructed to each entity)
         * @param dataSize Size of the component data (for validation)
         * @return Number of entities that successfully had the component added
         */
        size_t AddComponentsByID(std::span<Entity> entities, ComponentID componentId, const void* data, size_t dataSize)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return 0;

            // Use the simpler single-entity path to avoid complexity with batch moves
            // This is less efficient but more reliable
            size_t addedCount = 0;
            for (Entity entity : entities)
            {
                if (AddComponentByID(entity, componentId, data, dataSize))
                {
                    ++addedCount;
                }
            }

            return addedCount;
        }

        /**
         * Type-erased component removal for use by CommandBuffer.
         *
         * @param entity The entity to remove the component from
         * @param componentId The ComponentID of the component to remove
         * @return true if component was removed successfully, false otherwise
         */
        bool RemoveComponentByID(Entity entity, ComponentID componentId)
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY
                return false;

            EntityRecord& oldLoc = it->second;

            // Check if entity has this component
            if (!oldLoc.archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;

            Archetype* newArchetype = GetArchetypeWithRemoved(oldLoc.archetype, componentId);
            EntityLocation newEntityLocation = MoveEntity(entity, oldLoc, newArchetype);

            return newEntityLocation.IsValid();
        }

        /**
         * Type-erased batch component removal for use by CommandBuffer.
         *
         * @param entities Span of entities to remove the component from
         * @param componentId The ComponentID of the component to remove
         * @return Number of entities that successfully had the component removed
         */
        size_t RemoveComponentsByID(std::span<Entity> entities, ComponentID componentId)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return 0;

            // Use the simpler single-entity path to avoid complexity with batch moves
            size_t removedCount = 0;
            for (Entity entity : entities)
            {
                if (RemoveComponentByID(entity, componentId))
                {
                    ++removedCount;
                }
            }

            return removedCount;
        }

        template<Component T>
        ASTRA_NODISCARD T* GetComponent(Entity entity)
        {
            ComponentID componentId = TypeID<T>::Value();
            
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY return nullptr;
            
            EntityRecord& loc = it->second;
            return loc.archetype->GetComponent<T>(loc.location);
        }

        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            auto it = m_entityMap.find(entity);
            if (it == m_entityMap.end()) ASTRA_UNLIKELY
                return false;
            const EntityRecord& loc = it->second;
            return loc.archetype->HasComponent<T>();
        }

        template<Component... Components>
        ASTRA_NODISCARD Archetype* FindArchetype() const
        {
            ComponentMask mask = MakeComponentMask<Components...>();
            return FindArchetype(mask);
        }

        ASTRA_NODISCARD Archetype* FindArchetype(const ComponentMask& mask) const
        {
            auto it = m_archetypeMap.Find(mask);
            if (it != m_archetypeMap.end())
            {
                return it->second;
            }
            return nullptr;
        }

        ASTRA_NODISCARD auto GetArchetypes()
        {
            return m_archetypes | std::views::transform([](auto& entry) { return entry.archetype.get(); });
        }

        ASTRA_NODISCARD size_t GetArchetypeCount() const
        {
            return m_archetypes.size();
        }

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

        ASTRA_NODISCARD ArchetypeChunkPool::Stats GetPoolStats() const
        {
            return m_chunkPool.GetStats();
        }
        
        ArchetypeChunkPool& GetChunkPool() { return m_chunkPool; }
        
        // Options for archetype defragmentation
        struct DefragmentOptions
        {
            size_t minArchetypesToKeep = 8;           // Keep at least this many archetypes
            size_t maxArchetypesToRemove = std::numeric_limits<size_t>::max();  // Max to remove in one call
            // Future: could add archetype merging threshold, reordering strategy, etc.
        };
        
        // Result of archetype defragmentation
        struct DefragmentResult
        {
            size_t emptyArchetypesRemoved = 0;
            size_t totalArchetypes = 0;
            // Future: could add merged archetypes count, reordered count, etc.
        };
        
        // Public defragmentation API - orchestrates archetype-level defragmentation
        DefragmentResult Defragment(const DefragmentOptions& options = {})
        {
            DefragmentResult result;
            result.totalArchetypes = m_archetypes.size();
            
            // Currently only removes empty archetypes, but could be extended
            result.emptyArchetypesRemoved = CleanupEmptyArchetypes(options);
            
            return result;
        }
        
    private:
        // Remove empty archetypes based on options
        size_t CleanupEmptyArchetypes(const DefragmentOptions& options)
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
                if (entry.archetype->GetEntityCount() == 0)
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
                // Sort by creation generation (oldest first)
                std::partial_sort(
                    candidates.begin(),
                    candidates.begin() + options.maxArchetypesToRemove,
                    candidates.end(),
                    [this](size_t a, size_t b)
                    {
                        return m_archetypes[a].creationGeneration < m_archetypes[b].creationGeneration;
                    }
                );
                candidates.resize(options.maxArchetypesToRemove);
            }
            
            // Process removals: clean up references first, then batch remove
            size_t removed = candidates.size();
            
            // First pass: clean up all references
            for (size_t index : candidates)
            {
                ASTRA_ASSERT(index < m_archetypes.size(), "Invalid archetype index");
                Archetype* archetype = m_archetypes[index].archetype.get();

                // Remove from archetype map
                m_archetypeMap.Erase(archetype->GetMask());

                // Remove graph edges
                m_edgeGraph.RemoveEdgesTo(archetype);
                m_edgeGraph.RemoveEdgesFrom(archetype);
            }
            
            // Second pass: mark for removal by moving unique_ptr to release ownership
            // This ensures archetypes are destroyed before removal
            for (size_t index : candidates)
            {
                m_archetypes[index].archetype.reset();
            }
            
            // Third pass: batch remove all null entries in one go
            m_archetypes.erase(
                std::remove_if(m_archetypes.begin(), m_archetypes.end(),
                    [](const ArchetypeEntry& entry) { return !entry.archetype; }),
                m_archetypes.end()
            );
            
            return removed;
        }
        
    public:
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
                // Write entity count for validation
                writer(entry.archetype->GetEntityCount());
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
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return false;  // Registry destroyed
            registry->GetAllDescriptors(registryDescriptors);
            
            // Read each archetype
            std::vector<uint32_t> archetypeIndices;
            for (uint32_t i = 1; i < archetypeCount; ++i)
            {
                uint32_t index;
                reader(index);
                archetypeIndices.push_back(index);
                
                // Deserialize the archetype
                auto archetypeResult = Archetype::Deserialize(reader, registryDescriptors, &m_chunkPool);
                if (archetypeResult.IsErr() || reader.HasError())
                {
                    return false;
                }
                auto archetype = std::move(*archetypeResult.GetValue());
                
                // Read metrics
                // Read entity count for validation
                size_t entityCount;
                reader(entityCount);
                
                // Add to storage
                ArchetypeEntry entry;
                entry.archetype = std::move(archetype);
                
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
        struct ArchetypeEntry
        {
            std::unique_ptr<Archetype> archetype;
            uint32_t creationGeneration;  // Generation when this archetype was created
        };

        template<Component... Components>
        ASTRA_NODISCARD Archetype* GetOrCreateArchetype()
        {
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;
            (registry->RegisterComponent<Components>(), ...);
            ComponentMask mask = MakeComponentMask<Components...>();
            
            auto it = m_archetypeMap.Find(mask);
            if (it != m_archetypeMap.end()) ASTRA_LIKELY
            {
                return it->second;
            }

            auto archetype = std::make_unique<Archetype>(mask);
            Archetype* ptr = archetype.get();
            ptr->m_chunkPool = &m_chunkPool;

            std::vector<ComponentDescriptor> componentDescriptors;

            for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
            {
                if (mask.Test(id)) ASTRA_UNLIKELY
                {
                    if (const auto* desc = registry->GetComponentDescriptor(id)) ASTRA_LIKELY
                    {
                        componentDescriptors.push_back(*desc);
                    }
                }
            }

            ptr->Initialize(componentDescriptors);
            m_archetypeMap[mask] = ptr;

            ArchetypeEntry entry;
            entry.archetype = std::move(archetype);
            entry.creationGeneration = ++m_generation;
            m_archetypes.push_back(std::move(entry));

            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);

            return ptr;
        }

        template<typename GetEdgeFunc, typename SetEdgeFunc, typename MaskOp>
        Archetype* GetArchetypeWithModified(Archetype* from, ComponentID componentId, GetEdgeFunc&& getEdge, SetEdgeFunc&& setEdge, MaskOp&& maskOp)
        {
            if (Archetype* target = getEdge(m_edgeGraph, from, componentId)) ASTRA_LIKELY
            {
                return target;
            }
            
            ComponentMask newMask = from->GetMask();
            maskOp(newMask, componentId);
            
            auto it = m_archetypeMap.Find(newMask);
            if (it != m_archetypeMap.end()) ASTRA_LIKELY
            {
                Archetype* to = it->second;
                setEdge(m_edgeGraph, from, componentId, to);
                return to;
            }
            
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
            {
                return nullptr;
            }
            
            auto archetype = std::make_unique<Archetype>(newMask);
            Archetype* ptr = archetype.get();
            ptr->m_chunkPool = &m_chunkPool;

            std::vector<ComponentDescriptor> componentDescriptors;

            for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
            {
                if (newMask.Test(id)) ASTRA_UNLIKELY
                {
                    if (const auto* desc = registry->GetComponentDescriptor(id)) ASTRA_LIKELY
                    {
                        componentDescriptors.push_back(*desc);
                    }
                }
            }

            // Always initialize archetype, even with empty component list
            ptr->Initialize(componentDescriptors);

            // Store archetype
            m_archetypeMap[newMask] = ptr;

            ArchetypeEntry entry;
            entry.archetype = std::move(archetype);
            entry.creationGeneration = ++m_generation;  // Assign generation

            m_archetypes.push_back(std::move(entry));

            // Increment structural change counter for fast path checking
            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
            
            Archetype* to = ptr;
            
            // Cache edge in the edge graph
            setEdge(m_edgeGraph, from, componentId, to);
            
            return to;
        }
        
        Archetype* GetArchetypeWithAdded(Archetype* from, ComponentID componentId)
        {
            return GetArchetypeWithModified(from, componentId,
                [](auto& graph, auto* arch, auto id) { return graph.GetAddEdge(arch, id); },
                [](auto& graph, auto* from, auto id, auto* to) { graph.SetAddEdge(from, id, to); },
                [](auto& mask, auto id) { mask.Set(id); });
        }
        
        Archetype* GetArchetypeWithRemoved(Archetype* from, ComponentID componentId)
        {
            return GetArchetypeWithModified(from, componentId,
                [](auto& graph, auto* arch, auto id) { return graph.GetRemoveEdge(arch, id); },
                [](auto& graph, auto* from, auto id, auto* to) { graph.SetRemoveEdge(from, id, to); },
                [](auto& mask, auto id) { mask.Reset(id); });
        }

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

        template<typename MoveFunc>
        EntityLocation MoveEntityInternal(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype, MoveFunc&& moveFunc)
        {
            EntityLocation newEntityLocation = newArchetype->AllocateEntitySlot(entity);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                return newEntityLocation;
            }
            
            if (oldLoc.archetype->IsInitialized() && newArchetype->IsInitialized()) ASTRA_LIKELY
            {
                moveFunc(newEntityLocation, newArchetype);
            }
            
            if (auto movedEntity = oldLoc.archetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
            {
                m_entityMap[*movedEntity].location = oldLoc.location;
            }
            
            oldLoc.archetype = newArchetype;
            oldLoc.location = newEntityLocation;
            
            return newEntityLocation;
        }

        EntityLocation MoveEntity(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype)
        {
            return MoveEntityInternal(entity, oldLoc, newArchetype, [&](EntityLocation newLoc, Archetype* newArch)
            {
                newArch->MoveEntityFrom(newLoc, *oldLoc.archetype, oldLoc.location);
            });
        }

        template<Component T, typename... Args>
        EntityLocation MoveEntityWithComponent(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype, Args&&... args)
        {
            return MoveEntityInternal(entity, oldLoc, newArchetype, [&](EntityLocation newLoc, Archetype* newArch)
            {
                MoveAndAdd<T>(newLoc, newArch, oldLoc.location, oldLoc.archetype, std::forward<Args>(args)...);
            });
        }

        template<Component T, typename... Args>
        void MoveAndAdd(EntityLocation dstEntityLocation, Archetype* dstArchetype, EntityLocation srcEntityLocation, Archetype* srcArchetype, Args&&... args)
        {
            auto& dstComponents = dstArchetype->GetComponentDescriptors();
            auto& srcComponents = srcArchetype->GetComponentDescriptors();
            
            // Get chunks
            auto [dstChunk, dstEntityIdx] = dstArchetype->ResolveLocation(dstEntityLocation);
            auto [srcChunk, srcEntityIdx] = srcArchetype->ResolveLocation(srcEntityLocation);
            
            // Create index map for source components - use array for O(1) access
            std::array<size_t, MAX_COMPONENTS> srcIndexMap;
            srcIndexMap.fill(std::numeric_limits<size_t>::max());
            for (size_t i = 0; i < srcComponents.size(); ++i)
            {
                srcIndexMap[srcComponents[i].id] = i;
            }
            
            ComponentID newComponentId = TypeID<T>::Value();
            
            for (size_t dstIdx = 0; dstIdx < dstComponents.size(); ++dstIdx)
            {
                auto& dstComp = dstComponents[dstIdx];
                const auto& dstArrayInfo = dstChunk->m_componentArrays[dstComp.id];
                void* dstPtr = static_cast<std::byte*>(dstArrayInfo.base) + dstEntityIdx * dstArrayInfo.stride;
                
                if (dstComp.id == newComponentId) ASTRA_UNLIKELY
                {
                    new (dstPtr) T(std::forward<Args>(args)...);
                }
                else
                {
                    size_t srcIdx = srcIndexMap[dstComp.id];
                    if (srcIdx != std::numeric_limits<size_t>::max()) ASTRA_LIKELY
                    {
                        const auto& srcArrayInfo = srcChunk->m_componentArrays[dstComp.id];
                        void* srcPtr = static_cast<std::byte*>(srcArrayInfo.base) + srcEntityIdx * srcArrayInfo.stride;
                        dstComp.MoveConstruct(dstPtr, srcPtr);
                    }
                }
            }
        }
        
        template<Component T, typename... Args>
        void MoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entities, Args&&... args)
        {
            if (entities.empty())
                return;
            
            size_t processedCount = 0;
            for (size_t i = 0; i < entities.size(); ++i)
            {
                auto [entity, srcLocation] = entities[i];
                
                EntityLocation dstLocation = dstArchetype->AllocateEntitySlot(entity);
                if (!dstLocation.IsValid()) ASTRA_UNLIKELY
                {
                    break;
                }
                
                MoveAndAdd<T>(dstLocation, dstArchetype, srcLocation, srcArchetype, args...);
                m_entityMap[entity] = {dstArchetype, dstLocation};
                ++processedCount;
            }
            
            for (size_t idx = dstArchetype->m_firstNonFullChunkIndex; idx < dstArchetype->m_chunks.size(); ++idx)
            {
                if (!dstArchetype->m_chunks[idx]->IsFull())
                {
                    dstArchetype->m_firstNonFullChunkIndex = idx;
                    break;
                }
            }
            
            // Sort processed entities by location in DESCENDING order before removal
            // This ensures we remove from highest location to lowest, preventing
            // swap-and-pop from invalidating locations we haven't processed yet
            std::sort(entities.begin(), entities.begin() + processedCount,
                [](const auto& a, const auto& b) { return a.second > b.second; });

            // Remove processed entities from source archetype (now in descending location order)
            for (size_t i = 0; i < processedCount; ++i)
            {
                auto& [entity, location] = entities[i];
                if (auto movedEntity = srcArchetype->RemoveEntity(location)) ASTRA_LIKELY
                {
                    m_entityMap[*movedEntity].location = location;
                }
            }
            
        }
        
        template<typename Predicate>
        FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> GroupEntitiesByArchetype(std::span<Entity> entities, Predicate&& filter)
        {
            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;
            
            for (Entity entity : entities)
            {
                auto it = m_entityMap.find(entity);
                if (it == m_entityMap.end()) ASTRA_UNLIKELY
                {
                    continue;
                }
                
                EntityRecord& loc = it->second;
                if (filter(loc.archetype))
                {
                    batches[loc.archetype].emplace_back(entity, loc.location);
                }
            }
            
            return batches;
        }
        
        template<typename PostMoveOp>
        void BatchMoveEntitiesInternal(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, PostMoveOp&& postMoveOp)
        {
#ifdef ASTRA_BUILD_DEBUG
            printf("BatchMoveEntitiesInternal: Moving %zu entities from archetype %p to %p\n", 
                   entityBatch.size(), srcArchetype, dstArchetype);
#endif
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
                std::sort(entityBatch.begin(), entityBatch.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
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
            
#ifdef ASTRA_BUILD_DEBUG
            printf("  BatchMoveEntitiesFrom returned %zu locations (expected %zu)\n", 
                   newLocations.size(), entityBatch.size());
#endif
            
            // Check if the operation succeeded (non-empty result means success)
            if (newLocations.empty() && !entityBatch.empty())
            {
                // Failed to allocate chunks - cannot proceed with batch operation
                // This is a critical failure as we cannot guarantee entity integrity
#ifdef ASTRA_BUILD_DEBUG
                printf("  ERROR: Failed to allocate chunks for batch move!\n");
#endif
                ASTRA_ASSERT(false, "Failed to allocate chunks for batch move operation");
                return;
            }
            
            // Execute post-move operation (e.g., setting component)
            postMoveOp(dstArchetype, newLocations);
            
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
        }
        
        template<Component T, typename... Args>
        void BatchMoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, Args&&... args)
        {
            // Create component value upfront and capture by value to avoid dangling reference
            // The lambda may be invoked after args go out of scope in optimized builds
            T component{std::forward<Args>(args)...};
            BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [component](Archetype* dst, const std::vector<EntityLocation>& locs) { dst->SetComponents<T>(locs, component); });
        }

        void BatchMoveEntitiesWithoutComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch)
        {
            BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [](Archetype*, const std::vector<EntityLocation>&) { /* No component operation needed for removal */ });
        }

        /**
         * Type-erased version of MoveEntityWithComponent for CommandBuffer use.
         */
        EntityLocation MoveEntityWithComponentByID(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype,
                                                   ComponentID componentId, const void* data, const ComponentDescriptor& desc)
        {
            // Allocate slot in new archetype
            EntityLocation newEntityLocation = newArchetype->AllocateEntitySlot(entity);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
                return newEntityLocation;

            // Copy component data to new location
            if (oldLoc.archetype->IsInitialized() && newArchetype->IsInitialized()) ASTRA_LIKELY
                MoveAndAddByID(newEntityLocation, newArchetype, oldLoc.location, oldLoc.archetype, componentId, data, desc);

            // Remove from old archetype
            if (auto movedEntity = oldLoc.archetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
                m_entityMap[*movedEntity].location = oldLoc.location;

            // Update entity record
            oldLoc.archetype = newArchetype;
            oldLoc.location = newEntityLocation;

            return newEntityLocation;
        }

        /**
         * Type-erased version of MoveAndAdd for CommandBuffer use.
         */
        void MoveAndAddByID(EntityLocation dstEntityLocation, Archetype* dstArchetype,
                           EntityLocation srcEntityLocation, Archetype* srcArchetype,
                           ComponentID newComponentId, const void* componentData, const ComponentDescriptor& newDesc)
        {
            auto& dstComponents = dstArchetype->GetComponentDescriptors();
            auto& srcComponents = srcArchetype->GetComponentDescriptors();

            // Get chunks
            auto [dstChunk, dstEntityIdx] = dstArchetype->ResolveLocation(dstEntityLocation);
            auto [srcChunk, srcEntityIdx] = srcArchetype->ResolveLocation(srcEntityLocation);

            // Create index map for source components - use array for O(1) access
            std::array<size_t, MAX_COMPONENTS> srcIndexMap;
            srcIndexMap.fill(std::numeric_limits<size_t>::max());
            for (size_t i = 0; i < srcComponents.size(); ++i)
            {
                srcIndexMap[srcComponents[i].id] = i;
            }

            for (size_t dstIdx = 0; dstIdx < dstComponents.size(); ++dstIdx)
            {
                auto& dstComp = dstComponents[dstIdx];
                const auto& dstArrayInfo = dstChunk->m_componentArrays[dstComp.id];
                void* dstPtr = static_cast<std::byte*>(dstArrayInfo.base) + dstEntityIdx * dstArrayInfo.stride;

                if (dstComp.id == newComponentId) ASTRA_UNLIKELY
                {
                    // Copy new component data - use memcpy for trivially copyable types
                    if (newDesc.is_trivially_copyable)
                    {
                        std::memcpy(dstPtr, componentData, newDesc.size);
                    }
                    else if (newDesc.constructWith)
                    {
                        newDesc.constructWith(dstPtr, componentData);
                    }
                    else if (newDesc.copyConstruct)
                    {
                        newDesc.copyConstruct(dstPtr, componentData);
                    }
                }
                else
                {
                    size_t srcIdx = srcIndexMap[dstComp.id];
                    if (srcIdx != std::numeric_limits<size_t>::max()) ASTRA_LIKELY
                    {
                        const auto& srcArrayInfo = srcChunk->m_componentArrays[dstComp.id];
                        void* srcPtr = static_cast<std::byte*>(srcArrayInfo.base) + srcEntityIdx * srcArrayInfo.stride;
                        dstComp.MoveConstruct(dstPtr, srcPtr);
                    }
                }
            }
        }

        /**
         * Type-erased version of BatchMoveEntitiesWithComponent for CommandBuffer use.
         */
        void BatchMoveEntitiesWithComponentByID(Archetype* srcArchetype, Archetype* dstArchetype,
                                                SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch,
                                                ComponentID componentId, const void* data, const ComponentDescriptor& desc)
        {
            BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch,
                [componentId, data, &desc](Archetype* dst, const std::vector<EntityLocation>& locs)
                {
                    // Set the new component for all entities at their new locations
                    for (const auto& location : locs)
                    {
                        auto [chunk, entityIdx] = dst->ResolveLocation(location);
                        const auto& arrayInfo = chunk->m_componentArrays[componentId];
                        void* dstPtr = static_cast<std::byte*>(arrayInfo.base) + entityIdx * arrayInfo.stride;
                        desc.ConstructWith(dstPtr, data);
                    }
                });
        }

        ArchetypeChunkPool m_chunkPool;
        std::weak_ptr<ComponentRegistry> m_componentRegistry;
        ArchetypeGraph m_edgeGraph;
        std::vector<ArchetypeEntry> m_archetypes;
        FlatMap<ComponentMask, Archetype*, BitmapHash<MAX_COMPONENTS>> m_archetypeMap;
        std::unordered_map<Entity, EntityRecord> m_entityMap;
        
        Archetype* m_rootArchetype = nullptr;
        
        std::atomic<uint32_t> m_structuralChangeCounter{0};  // Fast path check
        uint32_t m_generation = 1;  // Generation counter for new archetypes

        template<typename... QueryArgs>
        friend class View;
    };
}
