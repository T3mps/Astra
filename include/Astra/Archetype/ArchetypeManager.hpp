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
#include "../Entity/EntityRecord.hpp"
#include "../Entity/EntityTable.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "Archetype.hpp"
#include "ArchetypeChunkPool.hpp"

namespace Astra
{
    template<typename... QueryArgs>
    class View;
    
    class ArchetypeManager
    {
    public:
        explicit ArchetypeManager(std::shared_ptr<ComponentRegistry> registry,
                                  const ArchetypeChunkPool::Config& poolConfig = {},
                                  EntityTable* records = nullptr) :
            m_chunkPool(poolConfig),
            m_componentRegistry(registry),
            m_records(records)
        {
            ASTRA_ASSERT(registry, "ComponentRegistry must not be null");
            ASTRA_ASSERT(records, "EntityRecordTable must not be null");

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
            if (!entity.IsValid()) ASTRA_UNLIKELY
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
            
            EntityLocation location = archetype->AddEntity(entity);
            
            if (!location.IsValid()) ASTRA_UNLIKELY
            {
                return;
            }

            EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
            SetRecordLocation(rec, archetype, location);   // NEVER assign rec->version
        }

        template<Component... Components>
        void AddEntityWith(Entity entity, Components&&... components)
        {
            static_assert(sizeof...(Components) > 0, "AddEntityWith requires at least one component");

            if (!entity.IsValid()) ASTRA_UNLIKELY
                return;
            
            Archetype* archetype = GetOrCreateArchetype<std::decay_t<Components>...>();
            EntityLocation location = archetype->AddEntityWith(entity, std::forward<Components>(components)...);

            if (!location.IsValid()) ASTRA_UNLIKELY
            {
                return;
            }

            EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
            SetRecordLocation(rec, archetype, location);   // NEVER assign rec->version
        }

        template<Component... Components>
        void AddEntities(std::span<const Entity> entities)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return;

            SmallVector<Entity, 256> validStorage;
            bool anyInvalid = false;
            for (Entity e : entities)
            {
                if (!e.IsValid())
                {
                    anyInvalid = true;
                    break;
                }
            }
            if (anyInvalid) ASTRA_UNLIKELY
            {
                validStorage.reserve(entities.size());
                for (Entity e : entities)
                {
                    if (e.IsValid())
                    {
                        validStorage.push_back(e);
                    }
                }
                if (validStorage.empty())
                    return;
                entities = std::span<const Entity>(validStorage.data(), validStorage.size());
                count = entities.size();
            }

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

            // No table sizing here: the shared record table is owned and pre-sized
            // by EntityManager (these entities were already Created), and segments
            // are created on demand by GetOrCreateRecord.
            for (size_t i = 0; i < locations.size(); ++i)
            {
                EntityRecord* rec = m_records->GetOrCreateRecord(entities[i].GetID());
                SetRecordLocation(rec, archetype, locations[i]);   // NEVER assign rec->version
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

            // See AddEntities: the shared record table is owned/sized by EntityManager.
            // Locations arrive in chunk-run order (Archetype fills one chunk fully before
            // the next), so the chunk pointer only changes at run boundaries: derive it
            // once per chunk and feed the 4-arg funnel, avoiding a per-entity
            // GetChunks()[...] re-derivation.
            const auto& chunks = archetype->GetChunks();
            size_t lastChunkIndex = SIZE_MAX;
            ArchetypeChunk* chunk = nullptr;
            for (size_t i = 0; i < locations.size(); ++i)
            {
                const size_t ci = locations[i].GetChunkIndex();
                if (ci != lastChunkIndex) ASTRA_UNLIKELY
                {
                    chunk = chunks[ci].get();
                    lastChunkIndex = ci;
                }
                EntityRecord* rec = m_records->GetOrCreateRecord(entities[i].GetID());
                SetRecordLocation(rec, archetype, chunk, locations[i]);   // NEVER assign rec->version
            }
        }

        void RemoveEntity(Entity entity)
        {
            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return;

            RemoveEntity(entity, rec);
        }

        // Record-taking variant: caller already validated the record. Same body as
        // RemoveEntity(Entity) minus the fetch/guard.
        void RemoveEntity(Entity entity, EntityRecord* rec)
        {
            ASTRA_ASSERT(rec && rec->version == entity.GetVersion() && rec->archetype,
                         "RemoveEntity(rec): caller must pass a validated record");
            Archetype* archetype = rec->archetype;
            EntityLocation oldLocation = rec->location;

            if (auto movedEntity = archetype->RemoveEntity(oldLocation)) ASTRA_LIKELY
            {
                // The swapped-in entity is guaranteed live and located.
                EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                SetRecordLocation(movedRec, archetype, oldLocation);
            }

            // Erase clears LOCATION ONLY -- EntityManager::Destroy owns the version.
            ClearRecordLocation(rec);
        }

        void RemoveEntities(std::span<Entity> entities)
        {
            if (entities.empty()) ASTRA_UNLIKELY
                return;

            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;

            for (Entity entity : entities)
            {
                EntityRecord* rec = m_records->GetRecord(entity.GetID());
                if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY continue;

                batches[rec->archetype].emplace_back(entity, rec->location);
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
                    if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
                    {
                        SetRecordLocation(rec, archetype, newEntityLocation);
                    }
                }

                for (const auto& [entity, _] : entityBatch)
                {
                    // Erase clears LOCATION ONLY -- versions belong to EntityManager.
                    if (EntityRecord* rec = m_records->GetRecord(entity.GetID())) ASTRA_LIKELY
                    {
                        ClearRecordLocation(rec);
                    }
                }
            }
        }

        ASTRA_NODISCARD const EntityRecord* GetEntityRecord(Entity entity) const
        {
            const EntityRecord* rec = m_records->GetRecord(entity.GetID());
            return (rec && rec->version == entity.GetVersion() && rec->archetype) ? rec : nullptr;
        }

        void SetEntityLocation(Entity entity, Archetype* archetype, EntityLocation location)
        {
            // Any caller may pass a null archetype / invalid location (a clear);
            // only resolve the cached chunk when the location is fully valid.
            ArchetypeChunk* chunk =
                (archetype && location.IsValid() &&
                 location.GetChunkIndex() < archetype->GetChunks().size())
                    ? archetype->GetChunks()[location.GetChunkIndex()].get()
                    : nullptr;
            m_records->SetRecord(entity.GetID(), archetype, chunk, location);   // archetype/chunk/location only
        }

        template<Component T, typename... Args>
        T* AddComponent(Entity entity, Args&&... args)
        {
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;
            registry->RegisterComponent<T>();

            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return nullptr;

            EntityRecord& oldLoc = *rec;
            ComponentID componentId = TypeID<T>::Value();
            if (componentId >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;   // registration refused (ID-space exhausted): typed-path parity with the ByID guard

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
            if (entities.empty())
                return;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return;
            registry->RegisterComponent<T>();
            ComponentID componentID = TypeID<T>::Value();
            if (componentID >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return;   // registration refused (ID-space exhausted): typed-path parity with the ByID guard

            auto batches = GroupEntitiesByArchetype(entities,
                [componentID](Archetype* arch)
                {
                    return !arch->GetMask().Test(componentID);
                });

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

            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return false;

            EntityRecord& oldLoc = *rec;

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
                removedCount += BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
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

            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return false;

            EntityRecord& oldLoc = *rec;

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
            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return false;

            EntityRecord& oldLoc = *rec;

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
            EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return nullptr;

            if constexpr (std::is_empty_v<T>)
            {
                // Tags have no storage column (idToColumn == -1 whether present or not),
                // so presence MUST come from the archetype mask.
                return rec->archetype->GetComponent<T>(rec->location);
            }
            else
            {
                // Load-bearing guard, not decoration: the old path's safety for
                // over-ceiling/collision-refused ids (INVALID_COMPONENT, Theme E) came
                // from Bitmap::Test's internal range check, which this fast path skips.
                // Without it, idToColumn[id] is an OOB read in Release. Register-only
                // compare -- zero memory traffic. Spec sec 4.4/4.5.
                const ComponentID id = TypeID<T>::Value();
                if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                    return nullptr;

                // Defense-in-depth (Lever 1 final-review Minor #1): a record can hold
                // archetype != nullptr with chunk == nullptr only via SetEntityLocation
                // fed a degenerate location -- unreachable today, but the ByID sites
                // all guard, so the typed path matches them rather than null-deref.
                if (!rec->chunk) ASTRA_UNLIKELY
                    return nullptr;

                ASTRA_ASSERT(rec->chunk ==
                             rec->archetype->GetChunks()[rec->location.GetChunkIndex()].get(),
                             "EntityRecord chunk/location desync");
                return rec->chunk->GetComponent<T>(rec->location.GetEntityIndex());
            }
        }

        template<Component T>
        ASTRA_NODISCARD bool HasComponent(Entity entity) const
        {
            const EntityRecord* rec = m_records->GetRecord(entity.GetID());
            if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                return false;
            return rec->archetype->HasComponent<T>();
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
            for (const auto& entry : m_archetypes)
            {
                // Chunks of one archetype no longer share a byte size, so sum
                // each chunk's own footprint instead of chunkCount * chunkSize.
                for (const auto& chunk : entry.archetype->GetChunks())
                {
                    if (chunk) ASTRA_LIKELY
                        total += chunk->GetChunkBytes();
                }
                total += sizeof(Archetype) + sizeof(size_t) * MAX_COMPONENTS * 2;
            }
            return total;
        }

        ASTRA_NODISCARD ArchetypeChunkPool::Stats GetPoolStats() const
        {
            return m_chunkPool.GetStats();
        }

        ArchetypeChunkPool& GetChunkPool() { return m_chunkPool; }

        // Monotonic count of archetype-SET changes: a new archetype being
        // created, or an empty archetype removed during defragmentation. This
        // does NOT count entity add/remove/destroy or component transitions that
        // stay within already-existing archetypes. Used as a best-effort Debug
        // tripwire by the built-in ParallelExecutor and for View cache
        // invalidation. Read-only; single-writer.
        ASTRA_NODISCARD uint32_t GetStructuralChangeCounter() const noexcept
        {
            return m_structuralChangeCounter.load(std::memory_order_acquire);
        }

        /**
         * Resets the manager to a freshly-constructed state IN PLACE: every
         * archetype (including the old root) is destroyed, the entity map,
         * archetype map, and edge-graph caches are emptied, and a brand-new
         * empty-mask root archetype is created and registered exactly as the
         * constructor does. The ComponentRegistry (weak_ptr) and the chunk
         * pool (and its configured chunk size) are left untouched so Clear()
         * keeps honoring the pool config the manager was constructed with.
         *
         * This is done in place - the ArchetypeManager object identity never
         * changes - so any View/Relations already holding a shared_ptr to
         * this manager keep pointing at a live, valid object instead of a
         * discarded one. Bumping both counters below signals those cached
         * views that everything changed AND that cached Archetype* pointers
         * are stale, so they fully re-collect (see View::EnsureArchetypes).
         */
        void Clear()
        {
            // The shared EntityRecord table is owned by EntityManager::Clear();
            // clearing it here would wipe the versions EntityManager owns. This
            // manager only resets its own archetypes/chunks. Cached transition
            // edges live inside each Archetype, so they are freed with them below.
            m_archetypeMap.Clear();
            m_archetypes.clear();  // destroys every archetype, incl. the old root

            auto rootArchetype = std::make_unique<Archetype>(ComponentMask{});
            m_rootArchetype = rootArchetype.get();
            m_rootArchetype->m_chunkPool = &m_chunkPool;
            m_rootArchetype->Initialize({});

            ArchetypeEntry entry;
            entry.archetype = std::move(rootArchetype);
            entry.creationGeneration = 0;  // Root archetype is generation 0
            m_archetypes.push_back(std::move(entry));

            m_generation = 1;  // matches the NSDMI a freshly constructed manager starts with

            m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
            m_archetypeRemovalCounter.fetch_add(1, std::memory_order_release);
        }

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
        // Overload instead of a default argument: gcc/clang reject a default argument
        // that needs DefragmentOptions' NSDMIs before the enclosing class is complete.
        DefragmentResult Defragment() { return Defragment(DefragmentOptions{}); }

        DefragmentResult Defragment(const DefragmentOptions& options)
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

                // Null any cached edge (in any surviving archetype) that points to
                // `archetype` before it is freed in the second pass below -- a
                // lingering edge would dangle (use-after-free). The doomed
                // archetype's OWN outgoing edges are freed with it when its
                // unique_ptr is reset, so only incoming edges need explicit nulling.
                for (auto& entry : m_archetypes)
                {
                    if (entry.archetype && entry.archetype.get() != archetype) ASTRA_LIKELY
                    {
                        entry.archetype->ClearEdgesTo(archetype);
                    }
                }
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

            if (removed > 0)
            {
                // Views cache raw Archetype*: signal both "something changed"
                // and "pointers may be stale" so they rebuild from scratch.
                m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
                m_archetypeRemovalCounter.fetch_add(1, std::memory_order_release);
            }

            return removed;
        }
        
    public:
        void Serialize(BinaryWriter& writer) const
        {
            // Collect located entities (archetype != nullptr) up front so the count
            // written at the fixed metadata slot below matches exactly what the
            // entity-record loop emits. A record's stored version reconstructs the
            // full Entity handle that used to be the entity-map key.
            SmallVector<std::pair<Entity, EntityRecord>, 256> located;
            m_records->ForEachRecord([&](EntityTable::IDType id, const EntityRecord& rec)
            {
                if (rec.archetype)
                    located.push_back({ Entity(id, rec.version), rec });
            });

            // Write storage metadata
            writer(static_cast<uint32_t>(m_archetypes.size()));
            writer(static_cast<uint32_t>(located.size()));

            // Write each archetype, including the root (zero-component) archetype
            // at index 0 - its entities must round-trip through Save/Load just
            // like any other archetype's, or they become dangling entity-map
            // entries after Load (invisible to iteration, UB on later removal).
            for (size_t i = 0; i < m_archetypes.size(); ++i)
            {
                const auto& entry = m_archetypes[i];
                
                // Write archetype index for reference
                writer(static_cast<uint32_t>(i));
                
                // Serialize the archetype
                entry.archetype->Serialize(writer);
                
                // Write metrics
                // Write entity count for validation
                writer(static_cast<uint64_t>(entry.archetype->GetEntityCount()));
            }
            
            // Write entity-to-archetype mappings
            for (const auto& [entity, rec] : located)
            {
                writer(entity);

                // Find archetype index
                uint32_t archetypeIndex = 0;
                for (size_t i = 0; i < m_archetypes.size(); ++i)
                {
                    if (m_archetypes[i].archetype.get() == rec.archetype)
                    {
                        archetypeIndex = static_cast<uint32_t>(i);
                        break;
                    }
                }

                writer(archetypeIndex);
                writer(rec.location.chunkIndex);
                writer(rec.location.entityIndex);
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

            // Every non-root archetype was just freed, so the surviving root's cached
            // transition edges are all dangling -- null them before anything can follow one.
            // (v3 replaces index 0 below, making this a no-op there; v2 and earlier carry
            // the root over unchanged, where this is the actual fix for a latent UAF: a
            // post-load root AddComponent/RemoveComponent would otherwise follow a stale
            // edge into freed memory.) Only index 0 (the root) survives as a reused object;
            // every other archetype is re-created fresh below with null edges.
            if (!m_archetypes.empty() && m_archetypes[0].archetype)
                m_archetypes[0].archetype->ClearAllEdges();
            // Do NOT clear the shared record table: EntityManager::Deserialize has
            // already restored versions (and created the segments) into it; this
            // pass only writes archetype/location back into those same slots.

            // Read storage metadata
            uint32_t archetypeCount, entityCount;
            reader(archetypeCount)(entityCount);

            if (reader.HasError())
                return false;

            // Bound archetypeCount against the remaining buffer via the reader's
            // width-agnostic count-bound helper (archetypeCount is a uint32_t on
            // disk; Serialize above writes it via writer(static_cast<uint32_t>(
            // m_archetypes.size()))). Each archetype record's guaranteed fixed
            // prefix: a uint32_t index, the ComponentMask words,
            // Archetype::Serialize's own unconditional fixed fields (entityCount,
            // entitiesPerChunk, chunkCount, descriptorCount), and the trailing
            // per-archetype entityCount written after it; the chunks/descriptors
            // themselves are variable-length.
            constexpr uint64_t kMinBytesPerArchetype =
                sizeof(uint32_t) +                                        // archetype index
                ComponentMask::WORD_COUNT * sizeof(ComponentMask::Word) + // mask
                sizeof(uint64_t) + sizeof(uint64_t) +                     // entityCount, entitiesPerChunk
                sizeof(uint32_t) + sizeof(uint32_t) +                     // chunkCount, descriptorCount
                sizeof(uint64_t);                                         // trailing per-archetype entity count
            if (reader.CountExceedsRemaining(archetypeCount, kMinBytesPerArchetype))
                return false;

            // Same bound for entityCount, sized to an entity-map record's fixed
            // on-disk fields: entity + archetypeIndex(4) + chunkIndex(4) +
            // entityIndex(4), all written unconditionally per entity below.
            constexpr uint64_t kMinBytesPerEntityRecord = sizeof(Entity) + sizeof(uint32_t) * 3;
            if (reader.CountExceedsRemaining(entityCount, kMinBytesPerEntityRecord))
                return false;

            // Reserve space (archetypes only; the shared record table's sizing is
            // owned by EntityManager, which already restored/sized it above).
            m_archetypes.reserve(archetypeCount);

            // Get all registered component descriptors
            std::vector<ComponentDescriptor> registryDescriptors;
            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return false;  // Registry destroyed
            registry->GetAllDescriptors(registryDescriptors);
            
            // Read each archetype, including the root (zero-component) archetype
            // written at index 0 (format v3+). The constructor already created a
            // fresh, empty root archetype (m_archetypes[0], pointed to by
            // m_rootArchetype) before Deserialize ran, so index 0 must repopulate
            // that EXISTING entry in place rather than push a second root -
            // m_rootArchetype and m_archetypeMap must keep referring to exactly
            // one root archetype.
            //
            // Version gate: archives written before v3 (format v2 and earlier)
            // do not contain a root-archetype record at all - their archetypeCount
            // covers only the non-root archetypes starting at index 1. Starting
            // the read loop at 1 for those archives reproduces the pre-v3 reader
            // exactly (root stays empty, same as when it was written), instead of
            // misreading the first non-root record's bytes as a root record.
            const uint32_t firstArchetypeIndex = (reader.GetVersion() >= 3) ? 0u : 1u;
            for (uint32_t i = firstArchetypeIndex; i < archetypeCount; ++i)
            {
                // The on-disk archetype index isn't otherwise used by this loop
                // (i drives it, and the root-vs-non-root branch below keys off
                // i == 0) -- read it only to stay in sync with the wire format
                // Archetype::Serialize's caller writes it against.
                uint32_t index;
                reader(index);

                // Deserialize the archetype
                auto archetypeResult = Archetype::Deserialize(reader, registryDescriptors, &m_chunkPool);
                if (archetypeResult.IsErr() || reader.HasError())
                {
                    return false;
                }
                auto archetype = std::move(*archetypeResult.GetValue());

                // Read metrics
                // Read entity count for validation
                uint64_t entityCount;
                reader(entityCount);

                if (i == 0)
                {
                    // Repopulate the pre-existing root archetype entry instead of
                    // appending a new one.
                    m_archetypes[0].archetype = std::move(archetype);
                    m_rootArchetype = m_archetypes[0].archetype.get();
                    m_archetypeMap[m_rootArchetype->GetMask()] = m_rootArchetype;
                }
                else
                {
                    // Add to storage
                    ArchetypeEntry entry;
                    entry.archetype = std::move(archetype);

                    m_archetypeMap[entry.archetype->GetMask()] = entry.archetype.get();
                    m_archetypes.push_back(std::move(entry));
                }
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

                // By this point every archetype (and its chunks) from the loop
                // above already exists, so chunkIndex/entityIndex can be validated
                // against the real chunk layout instead of stored raw - a corrupt
                // index here would otherwise make a later GetComponent/iteration
                // index a chunk or slot out of bounds (OOB read/write). A bad
                // archetypeIndex used to be silently skipped; now it fails the
                // whole load like any other corrupted record.
                if (archetypeIndex >= m_archetypes.size())
                    return false;

                Archetype* arch = m_archetypes[archetypeIndex].archetype.get();
                if (chunkIndex >= arch->GetChunkCount() ||
                    entityIndex >= arch->GetChunkEntityCount(chunkIndex))
                    return false;

                // Wire entity ids are untrusted. EntityManager::Deserialize restored every
                // live entity's segment+version BEFORE this runs (see the invariant note
                // above), so a legitimate mapping's record must already exist AND carry the
                // same version. GetRecord is non-creating and allocation-free for ANY id --
                // GetSegment bounds-checks segIdx against the existing segment index
                // (EntityTable.hpp:577) -- so a crafted huge id (the 64-bit unbounded-resize
                // DoS) and a mapping to a dead/never-restored entity both fail the load
                // instead of allocating or silently corrupting (2026-07-27 review P0).
                EntityRecord* rec = m_records->GetRecord(entity.GetID());
                if (!rec || rec->version == 0 || rec->version != entity.GetVersion())
                    return false;
                SetRecordLocation(rec, arch, EntityLocation::Create(chunkIndex, entityIndex));
            }
            
            return !reader.HasError();
        }
        
    private:
        struct ArchetypeEntry
        {
            std::unique_ptr<Archetype> archetype;
            uint32_t creationGeneration = 0;  // Generation when this archetype was created
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
            if (Archetype* target = getEdge(from, componentId)) ASTRA_LIKELY
            {
                return target;
            }

            ComponentMask newMask = from->GetMask();
            maskOp(newMask, componentId);

            auto it = m_archetypeMap.Find(newMask);
            if (it != m_archetypeMap.end()) ASTRA_LIKELY
            {
                Archetype* to = it->second;
                setEdge(from, componentId, to);
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

            // Cache edge on the source archetype (per-archetype array-indexed edge).
            setEdge(from, componentId, to);

            return to;
        }

        Archetype* GetArchetypeWithAdded(Archetype* from, ComponentID componentId)
        {
            return GetArchetypeWithModified(from, componentId,
                [](Archetype* f, ComponentID id) { return f->GetAddEdge(id); },
                [](Archetype* f, ComponentID id, Archetype* to) { f->SetAddEdge(id, to); },
                [](ComponentMask& mask, ComponentID id) { mask.Set(id); });
        }

        Archetype* GetArchetypeWithRemoved(Archetype* from, ComponentID componentId)
        {
            return GetArchetypeWithModified(from, componentId,
                [](Archetype* f, ComponentID id) { return f->GetRemoveEdge(id); },
                [](Archetype* f, ComponentID id, Archetype* to) { f->SetRemoveEdge(id, to); },
                [](ComponentMask& mask, ComponentID id) { mask.Reset(id); });
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
            
            // Capture the OLD archetype before oldLoc is reassigned: the swap-moved
            // entity stays in it, so its chunk must resolve against srcArchetype.
            Archetype* srcArchetype = oldLoc.archetype;
            if (auto movedEntity = srcArchetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
            {
                EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                SetRecordLocation(movedRec, srcArchetype, oldLoc.location);
            }

            // oldLoc aliases the shared record for `entity`; writing it here IS the
            // record update (archetype/chunk/location only -- never version). Runs
            // AFTER the movedRec fixup, which resolves against the OLD archetype.
            SetRecordLocation(&oldLoc, newArchetype, newEntityLocation);

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
            // Get chunks
            auto [dstChunk, dstEntityIdx] = dstArchetype->ResolveLocation(dstEntityLocation);
            auto [srcChunk, srcEntityIdx] = srcArchetype->ResolveLocation(srcEntityLocation);

            const ArchetypeColumnMeta& dm = dstArchetype->GetColumnMeta();
            const ArchetypeColumnMeta& sm = srcArchetype->GetColumnMeta();
            const ComponentID newComponentId = TypeID<T>::Value();

            // Iterate the destination's storage columns; construct the new component in
            // place, move the shared ones from the source. (Tags carry no column.)
            for (uint16_t c = 0; c < dm.columnCount; ++c)
            {
                const ComponentID id = dm.columns[c].id;
                // dstEntityIdx is < the dst chunk's count: AllocateEntitySlot (invoked by
                // MoveEntityInternal before this runs) already bumped the destination slot's
                // count, so the count-asserting GetComponentPointer is safe on the destination.
                void* dstPtr = dstChunk->GetComponentPointer(id, dstEntityIdx);

                if (id == newComponentId) ASTRA_UNLIKELY
                {
                    new (dstPtr) T(std::forward<Args>(args)...);
                }
                else
                {
                    const int sc = sm.idToColumn[id];
                    if (sc >= 0) ASTRA_LIKELY
                    {
                        // Carried-over (shared) column: move src -> dst. The per-column
                        // is_trivially_copyable check is the CORRECTNESS GATE, not a mere
                        // optimization -- memcpy of a move-only / lifetime-counting component
                        // would skip its move ctor and corrupt it. Must NOT be a blanket memcpy.
                        void* srcPtr = srcChunk->GetComponentPointer(id, srcEntityIdx);
                        const ComponentDescriptor& desc = *dm.columns[c].descriptor;
                        if (desc.is_trivially_copyable) std::memcpy(dstPtr, srcPtr, dm.columns[c].stride);
                        else                            desc.MoveConstruct(dstPtr, srcPtr);
                        // Disabled-bit carry (Task 2): shared enableable column keeps its
                        // state across the add transition. dst slot is freshly allocated
                        // (born enabled); the src bit is cleared by the caller's source
                        // swap-remove. The newly ADDED component (id == newComponentId)
                        // takes the born-enabled branch above -- no bit write.
                        if (desc.isEnableable) ASTRA_UNLIKELY
                            dstChunk->SetDisabled(c, dstEntityIdx, srcChunk->IsDisabled(sc, srcEntityIdx));
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
                EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
                SetRecordLocation(rec, dstArchetype, dstLocation);   // NEVER assign rec->version
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
                    EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                    ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                    SetRecordLocation(movedRec, srcArchetype, location);
                }
            }

        }
        
        template<typename Predicate>
        FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> GroupEntitiesByArchetype(std::span<Entity> entities, Predicate&& filter)
        {
            FlatMap<Archetype*, SmallVector<std::pair<Entity, EntityLocation>, 8>> batches;
            
            for (Entity entity : entities)
            {
                EntityRecord* rec = m_records->GetRecord(entity.GetID());
                if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
                {
                    continue;
                }

                if (filter(rec->archetype))
                {
                    batches[rec->archetype].emplace_back(entity, rec->location);
                }
            }

            return batches;
        }
        
        template<typename PostMoveOp>
        size_t BatchMoveEntitiesInternal(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, PostMoveOp&& postMoveOp)
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

            // Check if the operation succeeded (non-empty result means success)
            if (newLocations.empty() && !entityBatch.empty())
            {
                // Chunk-pool exhaustion is a recoverable allocation failure, not an
                // invariant violation - it must not abort in any config (mirrors the
                // "if (!chunk) return ..." bails elsewhere in Archetype.hpp). At this
                // point BatchMoveEntitiesFrom has failed before mutating dstArchetype
                // (no entities placed, m_entityCount/m_chunks untouched) and
                // srcArchetype/the shared record table haven't been touched yet
                // either (the post-move op, record update, and RemoveEntities all
                // happen below) - so bailing here leaves entityBatch exactly as it
                // was before the call, and the batch simply remains in srcArchetype.
                return 0;
            }
            
            // Execute post-move operation (e.g., setting component)
            postMoveOp(dstArchetype, newLocations);
            
            // Batch update entity records (archetype/location only, never version)
            for (size_t i = 0; i < newLocations.size(); ++i)
            {
                EntityRecord* rec = m_records->GetOrCreateRecord(entityBatch[i].first.GetID());
                SetRecordLocation(rec, dstArchetype, newLocations[i]);
            }
            
            // Normally every entity is placed (dst chunks are pre-allocated to fit
            // the whole batch). If BatchMoveEntitiesFrom ever returns a short result,
            // remove ONLY the entities actually moved to dst -- removing all of
            // srcLocations would drop the un-moved entities from src without ever
            // placing them in dst (silent entity loss).
            if (!ASTRA_ENSURE(newLocations.size() == entityBatch.size(),
                              "BatchMoveEntitiesFrom placed fewer entities than requested")) ASTRA_UNLIKELY
            {
                // Diagnostic only; the prefix-limited removal below keeps the
                // un-moved entities valid in src.
            }

            // Batch remove from source (defer chunk cleanup to avoid invalidating locations)
            std::span<const EntityLocation> movedSrcLocations(srcLocations.data(), newLocations.size());
            auto movedEntities = srcArchetype->RemoveEntities(movedSrcLocations, true);

            // Update locations of entities moved during removal
            for (const auto& [movedEntity, newLocation] : movedEntities)
            {
                if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
                {
                    SetRecordLocation(rec, srcArchetype, newLocation);
                }
            }

            return newLocations.size();
        }

        template<Component T, typename... Args>
        size_t BatchMoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, Args&&... args)
        {
            // Create component value upfront and capture by value to avoid dangling reference
            // The lambda may be invoked after args go out of scope in optimized builds
            T component{std::forward<Args>(args)...};
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [component](Archetype* dst, const std::vector<EntityLocation>& locs) { dst->SetComponents<T>(locs, component); });
        }

        size_t BatchMoveEntitiesWithoutComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch)
        {
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [](Archetype*, const std::vector<EntityLocation>&) { /* No component operation needed for removal */ });
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

            // Remove from old archetype. Capture the OLD archetype before oldLoc is
            // reassigned: the swap-moved entity stays in it.
            Archetype* srcArchetype = oldLoc.archetype;
            if (auto movedEntity = srcArchetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
            {
                EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                SetRecordLocation(movedRec, srcArchetype, oldLoc.location);
            }

            // Update entity record (oldLoc aliases the shared record for `entity`;
            // archetype/chunk/location only -- never version). Runs AFTER the movedRec
            // fixup, which resolves against the OLD archetype.
            SetRecordLocation(&oldLoc, newArchetype, newEntityLocation);

            return newEntityLocation;
        }

        /**
         * Type-erased version of MoveAndAdd for CommandBuffer use.
         */
        void MoveAndAddByID(EntityLocation dstEntityLocation, Archetype* dstArchetype,
                           EntityLocation srcEntityLocation, Archetype* srcArchetype,
                           ComponentID newComponentId, const void* componentData, const ComponentDescriptor& newDesc)
        {
            // Get chunks
            auto [dstChunk, dstEntityIdx] = dstArchetype->ResolveLocation(dstEntityLocation);
            auto [srcChunk, srcEntityIdx] = srcArchetype->ResolveLocation(srcEntityLocation);

            const ArchetypeColumnMeta& dm = dstArchetype->GetColumnMeta();
            const ArchetypeColumnMeta& sm = srcArchetype->GetColumnMeta();

            // Iterate the destination's storage columns; write the new component from
            // the type-erased data, move the shared ones from the source. (Tags carry
            // no column, so a tag being added has no storage to write -- matching the
            // old base==nullptr skip.)
            for (uint16_t c = 0; c < dm.columnCount; ++c)
            {
                const ComponentID id = dm.columns[c].id;
                // dstEntityIdx is < the dst chunk's count: AllocateEntitySlot (invoked by
                // MoveEntityWithComponentByID before this runs) already bumped the destination
                // slot's count, so the count-asserting GetComponentPointer is safe on the dst.
                void* dstPtr = dstChunk->GetComponentPointer(id, dstEntityIdx);

                if (id == newComponentId) ASTRA_UNLIKELY
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
                    else if (newDesc.moveConstruct)
                    {
                        // Move-only component: the CommandBuffer move-constructed the
                        // value into its own storage, so move it out (the buffer's copy
                        // is destructed after flush). Without this the slot was left
                        // uninitialized.
                        newDesc.moveConstruct(dstPtr, const_cast<void*>(componentData));
                    }
                    else
                    {
                        // Never-UB floor (mirrors ComponentDescriptor::ConstructWith).
                        newDesc.DefaultConstruct(dstPtr);
                    }
                }
                else
                {
                    const int sc = sm.idToColumn[id];
                    if (sc >= 0) ASTRA_LIKELY
                    {
                        // Carried-over (shared) column: move src -> dst. The per-column
                        // is_trivially_copyable check is the CORRECTNESS GATE, not a mere
                        // optimization -- memcpy of a move-only / lifetime-counting component
                        // would skip its move ctor and corrupt it. Must NOT be a blanket memcpy.
                        void* srcPtr = srcChunk->GetComponentPointer(id, srcEntityIdx);
                        const ComponentDescriptor& desc = *dm.columns[c].descriptor;
                        if (desc.is_trivially_copyable) std::memcpy(dstPtr, srcPtr, dm.columns[c].stride);
                        else                            desc.MoveConstruct(dstPtr, srcPtr);
                        // Disabled-bit carry (Task 2): mirror MoveAndAdd -- a shared
                        // enableable column keeps its state; the added component (handled
                        // in the id == newComponentId branch above) is born enabled.
                        if (desc.isEnableable) ASTRA_UNLIKELY
                            dstChunk->SetDisabled(c, dstEntityIdx, srcChunk->IsDisabled(sc, srcEntityIdx));
                    }
                }
            }
        }

        /**
         * Type-erased version of BatchMoveEntitiesWithComponent for CommandBuffer use.
         */
        size_t BatchMoveEntitiesWithComponentByID(Archetype* srcArchetype, Archetype* dstArchetype,
                                                SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch,
                                                ComponentID componentId, const void* data, const ComponentDescriptor& desc)
        {
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch,
                [componentId, data, &desc](Archetype* dst, const std::vector<EntityLocation>& locs)
                {
                    // Set the new component for all entities at their new locations
                    for (const auto& location : locs)
                    {
                        auto [chunk, entityIdx] = dst->ResolveLocation(location);
                        void* dstPtr = chunk->GetComponentPointer(componentId, entityIdx);
                        desc.ConstructWith(dstPtr, data);
                    }
                });
        }

        // The ONLY sanctioned writers of a record's storage fields (archetype/chunk/
        // location). NEVER touch rec->version (EntityManager owns it). Spec 4.3.
        static void SetRecordLocation(EntityRecord* rec, Archetype* arch,
                                      ArchetypeChunk* chunk, EntityLocation loc) noexcept
        {
            ASTRA_ASSERT(arch && chunk && loc.IsValid(), "SetRecordLocation: incomplete location");
            rec->archetype = arch;
            rec->chunk     = chunk;
            rec->location  = loc;
        }

        // Resolving overload: the chunk is L1-hot at every call site (the move that
        // produced `loc` just wrote it), so this lookup is effectively free.
        static void SetRecordLocation(EntityRecord* rec, Archetype* arch, EntityLocation loc)
        {
            ASTRA_ASSERT(loc.IsValid() && loc.GetChunkIndex() < arch->GetChunks().size(),
                         "SetRecordLocation: location out of range");
            SetRecordLocation(rec, arch, arch->GetChunks()[loc.GetChunkIndex()].get(), loc);
        }

        static void ClearRecordLocation(EntityRecord* rec) noexcept
        {
            rec->archetype = nullptr;
            rec->chunk     = nullptr;
            rec->location  = EntityLocation{};
        }

        ArchetypeChunkPool m_chunkPool;
        std::weak_ptr<ComponentRegistry> m_componentRegistry;
        std::vector<ArchetypeEntry> m_archetypes;
        FlatMap<ComponentMask, Archetype*, BitmapHash<MAX_COMPONENTS>> m_archetypeMap;
        // Shared paged EntityRecord table, owned by EntityManager and injected at
        // construction. This manager only ever writes archetype/location into a
        // record (via GetOrCreateRecord/GetRecord); versions belong to EntityManager.
        // Non-owning: the shared record table is owned by the Registry's EntityManager and
        // outlives this manager. Never retain an ArchetypeManager (or a View) past its Registry.
        EntityTable* m_records = nullptr;

        Archetype* m_rootArchetype = nullptr;
        
        std::atomic<uint32_t> m_structuralChangeCounter{0};  // Fast path check
        std::atomic<uint32_t> m_archetypeRemovalCounter{0};  // Bumped when archetypes are deleted; views must fully re-collect
        uint32_t m_generation = 1;  // Generation counter for new archetypes

        template<typename... QueryArgs>
        friend class View;
    };
}
