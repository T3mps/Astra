#pragma once

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Commands/CommandBlockArena.hpp"
#include "../Component/Component.hpp"
#include "../Component/ComponentRegistry.hpp"
#include "../Component/ResourceStorage.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/Signal.hpp"
#include "../Core/TypeID.hpp"
#include "../Core/WorkScheduler.hpp"
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
            // Scheduler used by parallel iteration/execution. Astra creates no threads:
            // null (the default) means every Parallel* API runs sequentially inline.
            // Hosts inject one shared instance (e.g. an enkiTS adapter) -- and in
            // multi-module (DLL) setups, the SAME instance into every module.
            std::shared_ptr<IWorkScheduler> workScheduler;
        };
        
        explicit Registry(const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig, &m_entityManager.GetRecordTable())),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_workScheduler(config.workScheduler),
            m_config(config)
        {}

        Registry(const EntityManager::Config& entityConfig, const ArchetypeChunkPool::Config& chunkConfig) :
            m_entityManager(entityConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, chunkConfig, &m_entityManager.GetRecordTable())),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry),
            m_workScheduler(nullptr),
            m_config{entityConfig, chunkConfig, {}, nullptr}
        {}

        Registry(std::shared_ptr<ComponentRegistry> componentRegistry, const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(std::move(componentRegistry)),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig, &m_entityManager.GetRecordTable())),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_workScheduler(config.workScheduler),
            m_config(config)
        {}

        // A Registry is a heavy, stateful container: copying is not supported (the old
        // (const Registry&, Config) ctor looked like a copy but silently produced an empty
        // registry). To create a second world that shares this one's component-type
        // registrations, use ShareComponentRegistry() with the shared_ptr ctor.
        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;

        ~Registry() = default;
        
        template<Component... Components>
        Entity CreateEntity()
        {
            Entity entity = m_entityManager.Create();
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();
            
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
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();
            
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
        size_t CreateEntities(size_t count, std::span<Entity> outEntities)
        {
            if (count == 0 || outEntities.size() < count)
                return 0;

            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0) ASTRA_UNLIKELY
                return 0;

            // Use unified batch AddEntities - handles archetype selection internally
            m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, created));

            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < created; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }

            if constexpr (sizeof...(Components) > 0)
            {
                if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
                {
                    for (size_t i = 0; i < created; ++i)
                    {
                        auto* record = m_archetypeManager->GetEntityRecord(outEntities[i]);
                        if (record)
                        {
                            ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), record->archetype->GetComponent<Components>(record->location))), ...);
                        }
                    }
                }
            }

            return created;
        }

        template<Component... Components, std::invocable<size_t> Generator>
        size_t CreateEntitiesWith(size_t count, std::span<Entity> outEntities, Generator&& generator)
        {
            if (count == 0 || outEntities.size() < count)
                return 0;

            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0) ASTRA_UNLIKELY
                return 0;

            m_archetypeManager->AddEntitiesWith<Components...>(outEntities.subspan(0, created), std::forward<Generator>(generator));

            if (m_signalManager.IsSignalEnabled(Signal::EntityCreated))
            {
                for (size_t i = 0; i < created; ++i)
                {
                    m_signalManager.Emit<Events::EntityCreated>(outEntities[i]);
                }
            }

            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                for (size_t i = 0; i < created; ++i)
                {
                    auto* record = m_archetypeManager->GetEntityRecord(outEntities[i]);
                    if (record)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), record->archetype->GetComponent<Components>(record->location))), ...);
                    }
                }
            }

            return created;
        }

        void DestroyEntity(Entity entity)
        {
            // Single validated fetch: version + archetype checks subsume the old
            // IsValid pre-check (same unified record slot -- W1). GetEntityRecord
            // hands back const for its read-only call sites; the record is
            // genuinely mutable memory here, so bridge constness once at this
            // consumer (same const_cast idiom used elsewhere in this file).
            auto* rec = const_cast<EntityRecord*>(m_archetypeManager->GetEntityRecord(entity));
            if (!rec) ASTRA_UNLIKELY
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed)) ASTRA_UNLIKELY
            {
                m_signalManager.Emit<Events::EntityDestroyed>(entity);
            }

            m_archetypeManager->RemoveEntity(entity, rec);

            // Empty-graph fast path: a handful of size loads replace several hash probes.
            // A relation-holding entity (including one with only a cached traversal
            // entry) makes the graph non-empty by construction, so cleanup can never
            // be skipped for an entity that needs it.
            if (!m_relationshipGraph->Empty()) ASTRA_UNLIKELY
            {
                m_relationshipGraph->OnEntityDestroyed(entity);
            }

            m_entityManager.Destroy(entity, rec);
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

            // IM-1: a duplicated live entity in the span would be removed twice by the
            // archetype swap-with-last path. The first removal swaps the former tail into
            // the freed slot; the second removal of the same (still < count) slot index
            // evicts that swapped-in bystander -- an entity never in the destroy list --
            // and double-frees the intended one. De-duplicate before dispatching to the
            // archetype/graph/entity layers (also collapses duplicate signals below).
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

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

        // IM-24: a stale/destroyed handle makes ArchetypeManager return nullptr, which a
        // void return would swallow silently -- an undetectable write to a dead handle
        // (data loss). Report it via a bool (matching RemoveComponent/SetEnabled) rather
        // than a silent no-op OR a Debug abort: the project's misuse policy is a graceful,
        // testable result, never assert-and-abort. Returns true when the component is
        // present on the live entity afterward (added, or already present, or a tag);
        // false only for a stale/invalid handle. The bool return is not [[nodiscard]], so
        // existing statement-style callers are unaffected.
        template<Component T>
        bool AddComponent(Entity entity, const T& component)
        {
            if (!m_entityManager.IsValid(entity))
                return false;

            T* newComponent = m_archetypeManager->AddComponent<T>(entity, component);

            if (newComponent)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), newComponent);
            }
            return true;
        }

        template<Component T, typename... Args>
        bool EmplaceComponent(Entity entity, Args&&... args)
        {
            // See AddComponent (IM-24): stale/invalid handle -> graceful false, no abort.
            if (!m_entityManager.IsValid(entity))
                return false;

            T* component = m_archetypeManager->AddComponent<T>(entity, std::forward<Args>(args)...);

            if (component)
            {
                m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
            }
            return true;
        }

        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
            {
                // Cold arm: the signal needs the PRE-removal pointer. The validated
                // GetComponent doubles as the liveness + presence guard (nullptr for
                // stale handles and absent components alike).
                T* component = m_archetypeManager->GetComponent<T>(entity);
                if (!component)
                    return false;

                // Emit BEFORE removal: the pointer is only valid until the entity
                // migrates. Handlers must not retain it past their invocation.
                m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);

                return m_archetypeManager->RemoveComponent<T>(entity);
            }

            // Hot arm: ArchetypeManager validates once (version + archetype + mask).
            return m_archetypeManager->RemoveComponent<T>(entity);
        }
        
        template<Component T>
        void AddComponents(std::span<Entity> entities, const T& component)
        {
            if (entities.empty())
                return;

            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            // IM-1: de-duplicate before the archetype layer -- a repeated entity would be
            // migrated twice (second move operates on a stale source slot, corrupting an
            // unrelated entity). See DestroyEntities for the full mechanism.
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());
            if (validEntities.empty())
                return;

            m_archetypeManager->AddComponents<T>(validEntities, component);
            
            // Emit signals for all entities if enabled
            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                ComponentID componentId = TypeID<T>::Value();
                for (Entity entity : validEntities)
                {
                    T* comp = m_archetypeManager->GetComponent<T>(entity);
                    if (comp)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, comp);
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

            // IM-1: de-duplicate before the archetype layer (see DestroyEntities/AddComponents).
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

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
            
            // Filter out invalid entities
            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());

            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        T* component = m_archetypeManager->GetComponent<T>(entity);
                        if (component)
                        {
                            // Emit before removal — see RemoveComponent.
                            m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);
                            validEntities.push_back(entity);
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

            // IM-1: de-duplicate before the archetype layer -- a repeated entity would be
            // migrated twice, the second move corrupting the bystander swapped into its
            // freed slot. See DestroyEntities for the full mechanism. (Note: in the
            // signal-enabled branch above, a duplicated entity still emits ComponentRemoved
            // once per occurrence -- a cosmetic double-signal, not the corruption IM-1
            // targets; see the follow-up flag in the report.)
            std::sort(validEntities.begin(), validEntities.end());
            validEntities.erase(std::unique(validEntities.begin(), validEntities.end()), validEntities.end());

            if (validEntities.empty())
                return 0;

            // Batch remove components
            return m_archetypeManager->RemoveComponents<T>(validEntities);
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

        // ===================== Enableable components (Task 2) =====================
        // O(1) enable/disable of a component for one entity. SET bit == DISABLED;
        // components are born enabled. Has/GetComponent IGNORE these bits (existence
        // never lies) -- only IsEnabled / queries / Size observe them.
        //
        // Scheduling contract (spec §9): SetEnabled counts as WRITE access on T
        // (owner-thread immediate mutation, identical threading rules to other
        // immediate mutations such as AddComponent); IsEnabled and query-time
        // filtering count as READ access on T.

        // Sets whether component T is enabled on `entity`. Returns true iff `entity`
        // currently holds the enableable component T (regardless of whether the state
        // changed); false for a stale handle or a missing component. Fires the gated
        // ComponentEnabled/ComponentDisabled signal ONLY on a genuine state change --
        // an idempotent SetEnabled is silent.
        template<Component T>
        bool SetEnabled(Entity entity, bool enable)
        {
            static_assert(IsEnableableV<T>,
                "SetEnabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`)");
            return SetEnabledByID(entity, TypeID<T>::Value(), enable);
        }

        // Returns true iff enableable component T is present on `entity` AND currently
        // enabled; false for stale / missing / disabled. Mirrors SetEnabled's table.
        template<Component T>
        ASTRA_NODISCARD bool IsEnabled(Entity entity) const
        {
            static_assert(IsEnableableV<T>,
                "IsEnabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`)");
            return IsEnabledByID(entity, TypeID<T>::Value());
        }

        // Type-erased toggle -- the shared body the typed SetEnabled delegates to.
        bool SetEnabledByID(Entity entity, ComponentID componentId, bool enable)
        {
            if (componentId >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;
            // One validated record fetch (version + archetype); stale handle => null.
            auto* rec = const_cast<EntityRecord*>(m_archetypeManager->GetEntityRecord(entity));
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            if (!rec->archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;   // component absent
            const int col = rec->archetype->GetColumnMeta().idToColumn[componentId];
            if (col < 0) ASTRA_UNLIKELY
                return false;   // present tag: no storage column to carry a bit
            ArchetypeChunk* chunk = rec->chunk;
            if (!chunk->GetDisabledWords(col)) ASTRA_UNLIKELY
                return false;   // present but not enableable: nothing to toggle
            const size_t idx = rec->location.GetEntityIndex();
            const bool changed = chunk->SetDisabled(col, idx, !enable);   // SET == DISABLED
            if (changed)
            {
                // Fire only on genuine change, via the IsSignalEnabled-then-Emit idiom
                // (copy of DestroyEntity at Registry.hpp:233). The component pointer is
                // computed only when the relevant signal is actually live.
                const Signal flag = enable ? Signal::ComponentEnabled : Signal::ComponentDisabled;
                if (m_signalManager.IsSignalEnabled(flag)) ASTRA_UNLIKELY
                {
                    void* comp = chunk->GetComponentPointer(componentId, idx);
                    if (enable) m_signalManager.Emit<Events::ComponentEnabled>(entity, componentId, comp);
                    else        m_signalManager.Emit<Events::ComponentDisabled>(entity, componentId, comp);
                }
            }
            return true;
        }

        // Type-erased query -- the shared body the typed IsEnabled delegates to.
        ASTRA_NODISCARD bool IsEnabledByID(Entity entity, ComponentID componentId) const
        {
            if (componentId >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;
            const auto* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) ASTRA_UNLIKELY
                return false;
            if (!rec->archetype->GetMask().Test(componentId)) ASTRA_UNLIKELY
                return false;   // absent => not enabled (behavior table)
            const int col = rec->archetype->GetColumnMeta().idToColumn[componentId];
            if (col < 0) ASTRA_UNLIKELY
                return true;    // present tag (no storage): always enabled
            if (!rec->chunk->GetDisabledWords(col)) ASTRA_UNLIKELY
                return true;    // present non-enableable column: always enabled
            return !rec->chunk->IsDisabled(col, rec->location.GetEntityIndex());
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
            bool result = m_archetypeManager->AddComponentByID(entity, componentId, data, dataSize);

            if (result && m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                // Get the newly added component pointer for the signal
                auto* record = m_archetypeManager->GetEntityRecord(entity);
                if (record)
                {
                    // We need to get the component descriptor to find the component
                    const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
                    if (desc)
                    {
                        if (record->chunk)
                        {
                            void* actualPtr = nullptr;
                            if (desc->size == 0)
                            {
                                actualPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compPtr = record->chunk->GetComponentArrayByID(componentId);
                                actualPtr = compPtr ? static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size : nullptr;
                            }
                            if (actualPtr)
                                m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
                        }
                    }
                }
            }

            return result;
        }

        /**
         * Type-erased batch component addition for use by CommandBuffer.
         * Note: This function does NOT emit ComponentAdded signals for performance.
         * Use individual AddComponentByID calls if signals are required.
         *
         * @param entities Span of entities to add the component to
         * @param componentId The ComponentID of the component to add
         * @param data Pointer to the source component data (will be copy-constructed to each entity)
         * @param dataSize Size of the component data (for validation)
         * @return Number of entities that successfully had the component added
         */
        size_t AddComponentsByID(std::span<Entity> entities, ComponentID componentId, const void* data, size_t dataSize)
        {
            return m_archetypeManager->AddComponentsByID(entities, componentId, data, dataSize);
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
            // Get component pointer before removal for signal emission
            void* componentPtr = nullptr;
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                auto* record = m_archetypeManager->GetEntityRecord(entity);
                if (record)
                {
                    const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
                    if (desc)
                    {
                        if (record->chunk)
                        {
                            if (desc->size == 0)
                            {
                                componentPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compArray = record->chunk->GetComponentArrayByID(componentId);
                                if (compArray)
                                    componentPtr = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
                            }
                        }
                    }
                }
            }

            if (componentPtr)
            {
                // Emit BEFORE removal so the pointer is still valid.
                m_signalManager.Emit<Events::ComponentRemoved>(entity, componentId, componentPtr);
            }

            return m_archetypeManager->RemoveComponentByID(entity, componentId);
        }

        /**
         * Type-erased batch component removal for use by CommandBuffer.
         * Note: This function does NOT emit ComponentRemoved signals for performance.
         * Use individual RemoveComponentByID calls if signals are required.
         *
         * @param entities Span of entities to remove the component from
         * @param componentId The ComponentID of the component to remove
         * @return Number of entities that successfully had the component removed
         */
        size_t RemoveComponentsByID(std::span<Entity> entities, ComponentID componentId)
        {
            return m_archetypeManager->RemoveComponentsByID(entities, componentId);
        }

        // ====================== Reflection Integration ======================

        /**
         * Gets a component by type hash (for reflection/runtime access).
         * The type hash should come from TypeID<T>::Hash() for a registered component.
         *
         * @param entity The entity to get the component from
         * @param typeHash XXHash64 of the component type name
         * @return Pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetComponentByHash(Entity entity, uint64_t typeHash)
        {
            if (!m_entityManager.IsValid(entity))
                return nullptr;

            // Look up component ID from type hash
            auto result = m_componentRegistry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return nullptr;

            ComponentID componentId = *result.GetValue();
            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return nullptr;

            const auto* desc = m_componentRegistry->GetComponentDescriptor(componentId);
            if (!desc)
                return nullptr;

            // Presence is the archetype mask, not the chunk array (which is null for a
            // size==0 tag). This makes Get(...) != nullptr agree with Has(...).
            if (!record->archetype->GetMask().Test(componentId))
                return nullptr;                     // entity does not have the component

            if (desc->size == 0)
                return EmptyComponentSentinel();     // present tag: no data

            if (!record->chunk)
                return nullptr;

            void* compArray = record->chunk->GetComponentArrayByID(componentId);
            if (!compArray)
                return nullptr;                      // defensive (should not happen for size > 0)

            return static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
        }

        /**
         * Gets a component by type hash (const version).
         *
         * @param entity The entity to get the component from
         * @param typeHash XXHash64 of the component type name
         * @return Const pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetComponentByHash(Entity entity, uint64_t typeHash) const
        {
            return const_cast<Registry*>(this)->GetComponentByHash(entity, typeHash);
        }

        /**
         * Checks if an entity has a component by type hash.
         *
         * @param entity The entity to check
         * @param typeHash XXHash64 of the component type name
         * @return true if the entity has the component
         */
        ASTRA_NODISCARD bool HasComponentByHash(Entity entity, uint64_t typeHash) const
        {
            if (!m_entityManager.IsValid(entity))
                return false;

            auto result = m_componentRegistry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return false;

            ComponentID componentId = *result.GetValue();
            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return false;

            return record->archetype->GetMask().Test(componentId);
        }

        /**
         * Gets all component descriptors for an entity.
         * Useful for editor/inspector UI that needs to enumerate all components.
         *
         * @param entity The entity to query
         * @return Vector of ComponentDescriptor pointers for all components on the entity
         */
        ASTRA_NODISCARD std::vector<const ComponentDescriptor*> GetEntityComponents(Entity entity) const
        {
            std::vector<const ComponentDescriptor*> result;

            if (!m_entityManager.IsValid(entity))
                return result;

            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return result;

            const ComponentMask& mask = record->archetype->GetMask();

            // Iterate through all registered components and check if entity has them
            m_componentRegistry->ForEachComponent([&](ComponentID id, const ComponentDescriptor& desc)
            {
                if (mask.Test(id))
                    result.push_back(&desc);
            });

            return result;
        }

        /**
         * Information about a component on an entity for inspection.
         */
        struct ComponentInfo
        {
            const ComponentDescriptor* descriptor;  // Component type info and lifecycle ops
            const TypeMeta* meta;                   // Reflection metadata (may be nullptr if not reflected)
            void* data;                             // Pointer to the component data
        };

        /**
         * Inspects an entity and returns detailed information about all its components.
         * This is the primary method for editor/inspector UIs to enumerate and modify components.
         *
         * @param entity The entity to inspect
         * @return Vector of ComponentInfo for all components on the entity
         */
        ASTRA_NODISCARD std::vector<ComponentInfo> InspectEntity(Entity entity)
        {
            std::vector<ComponentInfo> result;

            if (!m_entityManager.IsValid(entity))
                return result;

            auto* record = m_archetypeManager->GetEntityRecord(entity);
            if (!record)
                return result;

            const ComponentMask& mask = record->archetype->GetMask();

            if (!record->chunk)
                return result;

            // Iterate through all registered components and collect info
            m_componentRegistry->ForEachComponent([&](ComponentID id, const ComponentDescriptor& desc)
            {
                if (!mask.Test(id))
                    return;

                ComponentInfo info;
                info.descriptor = &desc;
                info.meta = desc.meta;  // May be nullptr if type is not reflected

                // Get component data pointer
                void* compArray = record->chunk->GetComponentArrayByID(id);
                if (compArray && desc.size > 0)
                {
                    info.data = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc.size;
                }
                else
                {
                    info.data = nullptr;  // Empty component
                }

                result.push_back(info);
            });

            return result;
        }

        /**
         * Inspects an entity (const version - returns const data pointers).
         * @param entity The entity to inspect
         * @return Vector of ComponentInfo for all components (data pointers are const)
         */
        ASTRA_NODISCARD std::vector<ComponentInfo> InspectEntity(Entity entity) const
        {
            return const_cast<Registry*>(this)->InspectEntity(entity);
        }

        /**
         * Information about a resource for inspection.
         */
        struct ResourceInfo
        {
            const ComponentDescriptor* descriptor;  // Resource type info and lifecycle ops
            const TypeMeta* meta;                   // Reflection metadata (may be nullptr if not reflected)
            void* data;                             // Pointer to the resource data
        };

        /**
         * Inspects all resources and returns detailed information.
         * This is the primary method for editor/inspector UIs to enumerate and modify resources.
         *
         * @return Vector of ResourceInfo for all active resources
         */
        ASTRA_NODISCARD std::vector<ResourceInfo> InspectResources()
        {
            std::vector<ResourceInfo> result;

            auto descriptors = m_resourceStorage.GetAllResources();
            result.reserve(descriptors.size());

            for (const auto* desc : descriptors)
            {
                ResourceInfo info;
                info.descriptor = desc;
                info.meta = desc->meta;  // May be nullptr if type is not reflected
                info.data = m_resourceStorage.GetByID(desc->id);
                result.push_back(info);
            }

            return result;
        }

        /**
         * Inspects all resources (const version - returns const data pointers).
         * @return Vector of ResourceInfo for all active resources
         */
        ASTRA_NODISCARD std::vector<ResourceInfo> InspectResources() const
        {
            return const_cast<Registry*>(this)->InspectResources();
        }

        /**
         * Gets a component by name (for debugging/scripting).
         * This is slower than GetComponentByHash as it requires a name lookup.
         *
         * @param entity The entity to get the component from
         * @param name Component type name (as returned by TypeID<T>::Name())
         * @return Pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetComponentByName(Entity entity, std::string_view name)
        {
            // Compute hash from name
            uint64_t hash = Detail::XXHash::XXHash64(name);
            return GetComponentByHash(entity, hash);
        }

        /**
         * Gets a component by name (const version).
         *
         * @param entity The entity to get the component from
         * @param name Component type name
         * @return Const pointer to the component data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetComponentByName(Entity entity, std::string_view name) const
        {
            return const_cast<Registry*>(this)->GetComponentByName(entity, name);
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

        // ====================== Resource Reflection ======================

        /**
         * Gets a resource by type hash (for reflection/runtime access).
         * @param typeHash XXHash64 of the resource type name
         * @return Pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetResourceByHash(uint64_t typeHash)
        {
            return m_resourceStorage.GetResourceByHash(typeHash);
        }

        /**
         * Gets a resource by type hash (const version).
         * @param typeHash XXHash64 of the resource type name
         * @return Const pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetResourceByHash(uint64_t typeHash) const
        {
            return m_resourceStorage.GetResourceByHash(typeHash);
        }

        /**
         * Checks if a resource exists by type hash.
         * @param typeHash XXHash64 of the resource type name
         * @return true if the resource exists
         */
        ASTRA_NODISCARD bool HasResourceByHash(uint64_t typeHash) const
        {
            return m_resourceStorage.HasResourceByHash(typeHash);
        }

        /**
         * Gets all resource descriptors.
         * Useful for editor/inspector UI that needs to enumerate all resources.
         * @return Vector of ComponentDescriptor pointers for all active resources
         */
        ASTRA_NODISCARD std::vector<const ComponentDescriptor*> GetAllResources() const
        {
            return m_resourceStorage.GetAllResources();
        }

        /**
         * Gets a resource by ComponentID.
         * @param componentId The ComponentID of the resource
         * @return Pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetResourceByID(ComponentID componentId)
        {
            return m_resourceStorage.GetByID(componentId);
        }

        /**
         * Gets a resource by ComponentID (const version).
         * @param componentId The ComponentID of the resource
         * @return Const pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetResourceByID(ComponentID componentId) const
        {
            return m_resourceStorage.GetByID(componentId);
        }

        /**
         * Type-erased resource setting for use by CommandBuffer.
         * Sets a resource using the component ID and raw data pointer.
         * The component must already be registered in the ComponentRegistry.
         *
         * @param componentId The ComponentID of the resource
         * @param data Pointer to the source resource data (will be copy-constructed)
         * @param dataSize Size of the resource data (for validation)
         * @return true if resource was set successfully, false otherwise
         */
        bool SetResourceByID(ComponentID componentId, const void* data, size_t dataSize)
        {
            return m_resourceStorage.SetByID(componentId, data, dataSize);
        }

        /**
         * Type-erased resource removal for use by CommandBuffer.
         *
         * @param componentId The ComponentID of the resource to remove
         * @return true if resource was removed, false if it didn't exist
         */
        bool RemoveResourceByID(ComponentID componentId)
        {
            return m_resourceStorage.RemoveByID(componentId);
        }

        // View creation
        
        template<ValidQueryArg... QueryArgs>
        ASTRA_NODISCARD auto CreateView()
        {
            return View<QueryArgs...>(m_archetypeManager, m_workScheduler);
        }
        
        void Clear()
        {
            m_archetypeManager->Clear();   // in place -- keeps cached Views/Relations valid
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
        ASTRA_NODISCARD std::shared_ptr<ComponentRegistry> ShareComponentRegistry() const noexcept { return m_componentRegistry; }
        ASTRA_NODISCARD ArchetypeManager* GetArchetypeManager() noexcept { return m_archetypeManager.get(); }
        ASTRA_NODISCARD const ArchetypeManager* GetArchetypeManager() const noexcept { return m_archetypeManager.get(); }

        /**
         * Block source for CommandBuffer storage (Commands C1 fix): stable
         * blocks that never move until released. Command buffers bound to
         * this Registry must not outlive it. This is a real destruction-order
         * requirement, not just a don't-use-after-free caveat: because
         * m_commandBlockArena is a Registry member, any CommandBuffer/
         * ParallelCommandBuffer/SystemScheduler declared BEFORE the Registry
         * in the same scope is destroyed AFTER it (reverse declaration order)
         * and will trip CommandBlockArena's outstanding-block assert on exit
         * -- declare Registry first.
         */
        ASTRA_NODISCARD CommandBlockArena& GetCommandBlockArena() noexcept { return m_commandBlockArena; }
        
        ASTRA_NODISCARD float GetFragmentationLevel() const
        {
            auto archetypes = m_archetypeManager->GetArchetypes();
            if (archetypes.empty())
                return 0.0f;

            // Fill-based, matching Archetype::GetFragmentationLevel: chunks no
            // longer share a capacity, so there is no single "optimal chunk
            // count" to compare against. Unused slots are what defragmentation
            // actually reclaims, so measure them directly.
            size_t totalEntities = 0;
            size_t totalCapacity = 0;

            for (const auto* arch : archetypes)
            {
                totalEntities += arch->GetEntityCount();
                totalCapacity += arch->GetTotalCapacity();
            }

            if (totalCapacity == 0) return 0.0f;

            return 1.0f - static_cast<float>(totalEntities) / static_cast<float>(totalCapacity);
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
        
        // Overload instead of a default argument: gcc/clang reject a default argument
        // that needs DefragmentationOptions' NSDMIs before the enclosing class is complete.
        DefragmentationResult Defragment() { return Defragment(DefragmentationOptions{}); }

        DefragmentationResult Defragment(const DefragmentationOptions& options)
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
                    
                    // Rebuild-style compaction: repacks every live entity into
                    // fresh right-sized chunks and reports every one's new
                    // location (movedEntities covers the whole archetype). The
                    // SetEntityLocation loop below already handles that.
                    auto [chunksFreed, movedEntities] = arch->CompactChunks();
                    
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
            return Relations<QueryArgs...>(m_archetypeManager, entity, m_relationshipGraph, m_workScheduler);
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
        
        // Overload instead of a default argument: gcc/clang reject a default argument
        // that needs SaveConfig's NSDMIs before the enclosing class is complete.
        Result<void, SerializationError> Save(const std::filesystem::path& path) const { return Save(path, SaveConfig{}); }

        Result<void, SerializationError> Save(const std::filesystem::path& path, const SaveConfig& config) const
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
            m_resourceStorage.Serialize(writer);   // format v2 resource block

            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<void, SerializationError>::Err(writer.GetError()) : 
                Result<void, SerializationError>::Ok();
        }
        
        Result<std::vector<std::byte>, SerializationError> Save() const { return Save(SaveConfig{}); }

        Result<std::vector<std::byte>, SerializationError> Save(const SaveConfig& config) const
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
            m_resourceStorage.Serialize(writer);   // format v2 resource block

            // Finalize with checksum
            writer.FinalizeHeader();
            
            return writer.HasError() ? 
                Result<std::vector<std::byte>, SerializationError>::Err(writer.GetError()) : 
                Result<std::vector<std::byte>, SerializationError>::Ok(std::move(buffer));
        }

        // Load validates structural integrity enough to fail cleanly -- returning
        // Err(SerializationError) -- on truncated, corrupt, or version-skewed input, without
        // out-of-bounds access, unbounded allocation, or hangs. It is NOT a security boundary:
        // it does not guarantee rejection of every maliciously-crafted archive, and callers must
        // not load saves from untrusted sources without their own validation.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(const std::filesystem::path& path, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(path);
            if (reader.HasError())
            {
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            }

            return LoadInternal(reader, std::move(componentRegistry), Config{});
        }

        static Result<std::unique_ptr<Registry>, SerializationError> Load(std::span<const std::byte> data, std::shared_ptr<ComponentRegistry> componentRegistry)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry), Config{});
        }

        // 3.3: thread a Config (hence workScheduler) into the restored registry, so a
        // hot-reload restore keeps parallel iteration. Additive; the two-arg forms above
        // delegate with a default Config and are byte-for-byte unchanged.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            const std::filesystem::path& path,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(path);
            if (reader.HasError())
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }

        // In-memory data form of the 3.3 Config overload above.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            std::span<const std::byte> data,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }

    private:
        /**
         * Internal helper to load registry from reader
         */
        static Result<std::unique_ptr<Registry>, SerializationError> LoadInternal(BinaryReader& reader, std::shared_ptr<ComponentRegistry> componentRegistry, const Config& config)
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

            // Construct with the caller's Config so its runtime policy (workScheduler,
            // resource storage) is honoured. Note: entityManagerConfig / chunkPoolConfig
            // are superseded below by the deserialized entity manager + archetypes (the
            // saved shape wins); only workScheduler-class fields survive Load.
            auto registry = std::make_unique<Registry>(componentRegistry, config);

            // Move the entity manager from unique_ptr. This restores versions into
            // registry->m_entityManager's record table BEFORE the ArchetypeManager
            // below is created against that same table -- the deserialize ordering
            // the shared-table design requires (versions first, then locations).
            registry->m_entityManager = std::move(*(*managerResult.GetValue()));

            // INVARIANT: EntityManager restores versions into the shared record table BEFORE
            // ArchetypeManager writes locations into the same slots (Task 5 / W1 unified record).
            // Create new ArchetypeManager pointing at the (now version-restored)
            // shared record table, then deserialize archetype/location into it.
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry, config.chunkPoolConfig, &registry->m_entityManager.GetRecordTable());
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

            // Resource block exists from format v2 onward.
            if (reader.GetVersion() >= 2)
            {
                if (!registry->m_resourceStorage.Deserialize(reader))
                {
                    return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::UnknownComponent);
                }
            }

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
        // Stable block source for CommandBuffer storage (Commands C1 fix). Holds
        // a std::mutex, so Registry is intentionally non-movable (documented).
        CommandBlockArena m_commandBlockArena;
        std::shared_ptr<IWorkScheduler> m_workScheduler;  // null = sequential inline fallback
        Config m_config;   // retained so Clear()/Load() preserve pool + storage policy
    };
}
