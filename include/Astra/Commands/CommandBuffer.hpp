#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Registry/Registry.hpp"
#include "Command.hpp"

namespace Astra
{
    /**
     * Error types for command buffer execution.
     */
    enum class CommandError
    {
        None,
        InvalidRegistry,
        ExecutionFailed,
        AllocationFailed
    };

    /**
     * Result of command buffer execution.
     */
    struct ExecutionResult
    {
        CommandError error = CommandError::None;
        size_t executedCount = 0;
        size_t totalCount = 0;

        [[nodiscard]] bool IsOk() const noexcept { return error == CommandError::None; }
        [[nodiscard]] bool IsErr() const noexcept { return error != CommandError::None; }
    };

    /**
     * Internal byte buffer for storing commands contiguously.
     * Commands are stored as [Header][Payload] pairs.
     */
    class CommandByteBuffer
    {
    public:
        static constexpr size_t DEFAULT_INITIAL_CAPACITY = 4096;
        // Every command start is aligned to 16. std::vector<std::byte>'s
        // allocation is aligned to __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 on all
        // supported targets), so for payload alignment A <= 16 the reader's
        // absolute-address alignment and the writer's command-relative offset
        // computation are guaranteed to agree.
        static constexpr size_t ALIGNMENT = 16;

        explicit CommandByteBuffer(size_t initialCapacity = DEFAULT_INITIAL_CAPACITY)
        {
            m_data.reserve(initialCapacity);
        }

        /**
         * Allocate space in the buffer for a command.
         * @param size Total size needed (header + payload + data)
         * @return Pointer to the allocated space, or nullptr if allocation failed
         */
        std::byte* Allocate(size_t size, size_t* outAlignedSize = nullptr)
        {
            // Align the size to 8 bytes
            size_t alignedSize = AlignUp(size, ALIGNMENT);

            size_t currentSize = m_data.size();
            m_data.resize(currentSize + alignedSize);

            if (outAlignedSize)
                *outAlignedSize = alignedSize;

            return m_data.data() + currentSize;
        }

        /**
         * Get pointer to the beginning of the buffer.
         */
        [[nodiscard]] std::byte* Data() noexcept { return m_data.data(); }
        [[nodiscard]] const std::byte* Data() const noexcept { return m_data.data(); }

        /**
         * Get the current size of the buffer in bytes.
         */
        [[nodiscard]] size_t Size() const noexcept { return m_data.size(); }

        /**
         * Check if the buffer is empty.
         */
        [[nodiscard]] bool IsEmpty() const noexcept { return m_data.empty(); }

        /**
         * Clear the buffer, but keep the allocated capacity.
         */
        void Clear() noexcept { m_data.clear(); }

        /**
         * Reserve capacity in the buffer.
         */
        void Reserve(size_t capacity) { m_data.reserve(capacity); }

    private:
        std::vector<std::byte> m_data;
    };

    /**
     * CommandBuffer stores deferred operations to be executed on a Registry.
     *
     * This implementation uses a type-erased byte buffer approach where commands and
     * component data are stored inline in a contiguous buffer. This avoids the UB issues
     * with lambda captures and provides better cache locality.
     *
     * IMPORTANT: Execution is NOT fully transactional. If a command fails during Execute():
     * - Commands that already executed successfully are NOT rolled back
     * - Only pre-allocated entities that haven't been processed yet are destroyed
     * - Use smaller command buffers if you need atomic all-or-nothing semantics
     * - Consider validating preconditions before adding commands
     *
     * Usage:
     *   CommandBuffer cmd(&registry);
     *   Entity e = cmd.CreateEntity();
     *   cmd.AddComponent(e, Position{1, 2, 3});
     *   auto result = cmd.Execute();
     *   if (result.IsErr()) { // handle error }
     */
    class CommandBuffer
    {
    public:
        using ExecutionError = CommandError;

        // Placeholder-resolution map (Theme B2 Task 5): maps a deferred-mode
        // placeholder Entity's raw storage value -> the real Entity it resolved
        // to. Populated as CreateEntity/CreateEntities commands are applied at
        // flush; consumed to translate every later command's Entity fields.
        // Keyed by value (not by Entity) because a placeholder is deliberately
        // NOT a live entity. ParallelCommandBuffer::ExecuteSorted() keeps ONE
        // of these per worker buffer (placeholders are per-buffer), so a
        // placeholder value reused across two buffers never collides.
        using PlaceholderMap = std::unordered_map<Entity::StorageType, Entity>;

        // deferredCreation: when true, CreateEntity/CreateEntities return a
        // PLACEHOLDER Entity and record the create command WITHOUT touching the
        // shared EntityManager (worker-safe); the real id is allocated
        // single-threaded at flush and back-patched (see ResolvePlaceholders).
        // Standalone CommandBuffers default to eager (false) -- unchanged
        // behavior. ParallelCommandBuffer constructs its per-worker buffers
        // with deferredCreation == true (see InitializeThreadBuffer).
        explicit CommandBuffer(Registry* registry, bool deferredCreation = false) :
            m_registry(registry),
            m_deferredCreation(deferredCreation)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
        }

        ~CommandBuffer()
        {
            // Clean up any pending commands that have destructors
            CleanupPendingCommands();
        }

        // ============= Sort Key Control =============

        /**
         * Sets the SortKey to stamp on the next-recorded command AND every
         * command recorded after it, until this is called again (STICKY, not
         * one-shot). This lets a single call site tag every command belonging
         * to one logical recording unit -- e.g. all commands a deferred system
         * records into a buffer shared across systems -- with that unit's key,
         * so a multi-command system doesn't have its 2nd+ command silently
         * fall back to a default key.
         *
         * Commands recorded before the first call to this method (or after
         * Clear()/a successful Execute()/ExecuteSorted()) use the default key
         * {0, 0, seq}, where seq auto-increments per recorded command in this
         * buffer, preserving simple arrival order for callers that never opt
         * into explicit keys.
         *
         * See ParallelCommandBuffer::ExecuteSorted() for where the key is
         * consumed: it stable-sorts every recorded command (across every
         * worker buffer) by SortKey before applying them, so this is only
         * relevant to sorted flushes -- Execute() always applies in physical
         * (arrival) order regardless of any key set here.
         */
        void SetNextSortKey(SortKey key) noexcept
        {
            m_currentSortKey = key;
            m_hasCustomSortKey = true;
        }

        // ============= Deferred-Command Error Reporting (Task 4) =============

        /**
         * Push a Task-4 deferred-command error into THIS buffer's own error
         * list. Called from two places: ParallelCommandBuffer::ExecuteSorted()
         * when a command this buffer recorded fails to apply (see
         * DeferredCommandError's doc comment for why every such failure is a
         * logical, not fatal, one), and SystemContext::ReportError() when a
         * system explicitly reports a domain-specific failure mid-execution.
         *
         * A worker only ever calls this on ITS OWN per-worker buffer (same
         * contract as Commands()' recording), so no synchronization is
         * needed even when many workers call this concurrently on their own
         * buffers.
         */
        void ReportError(DeferredCommandError error)
        {
            m_reportedErrors.push_back(error);
        }

        /**
         * Errors reported into this buffer via ReportError(), in report
         * order. Gathered single-threaded by ParallelCommandBuffer::
         * ExecuteSorted() (via GetDeferredErrors()) after every worker has
         * joined -- see that function's comment.
         */
        [[nodiscard]] const std::vector<DeferredCommandError>& GetReportedErrors() const noexcept
        {
            return m_reportedErrors;
        }

        // ============= Entity Commands =============

        /**
         * Create a new entity.
         *
         * EAGER mode (standalone CommandBuffer, the default): the entity ID is
         * allocated from the Registry's EntityManager IMMEDIATELY (at record
         * time); only the archetype insertion is deferred to Execute().
         * THREADING: because allocation mutates shared Registry state at record
         * time, in this mode CreateEntity/CreateEntities must only be recorded
         * from the thread that owns the Registry.
         *
         * DEFERRED mode (ParallelCommandBuffer per-worker buffers, Task 5):
         * returns a PLACEHOLDER Entity and records the create command WITHOUT
         * allocating from the EntityManager -- fully worker-safe. The
         * placeholder may be used in subsequent deferred ops recorded into the
         * SAME buffer (AddComponent, SetParent, ...). At flush the placeholder
         * resolves to a real id (single-threaded, in sort-key order ->
         * deterministic) and every referencing command is back-patched. See
         * ResolvePlaceholders / ParallelCommandBuffer::ExecuteSorted.
         */
        Entity CreateEntity()
        {
            if (!m_registry)
                return Entity::Invalid();

            if (m_deferredCreation)
            {
                // Worker-safe: no EntityManager touch at record time.
                Entity placeholder = MakePlaceholder();

                size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntityPayload);
                size_t alignedSize = 0;
                std::byte* ptr = m_buffer.Allocate(totalSize, &alignedSize);
                StampCommand(ptr);

                new (ptr) CommandHeader{CommandType::CreateEntity, 0, static_cast<uint32_t>(alignedSize)};
                new (ptr + sizeof(CommandHeader)) CreateEntityPayload{placeholder};

                m_commandCount++;
                return placeholder;
            }

            auto& manager = m_registry->GetEntityManager();
            Entity entity = manager.Create();
            if (entity == Entity::Invalid())
                return Entity::Invalid();

            m_allocatedEntities.push_back(entity);

            // Write command to buffer
            size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntityPayload);
            size_t alignedSize = 0;
            std::byte* ptr = m_buffer.Allocate(totalSize, &alignedSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::CreateEntity, 0, static_cast<uint32_t>(alignedSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntityPayload{entity};
            (void)header;
            (void)payload;

            m_commandCount++;
            return entity;
        }

        /**
         * Destroy an entity. The entity is destroyed when Execute() is called.
         */
        void DestroyEntity(Entity entity)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntityPayload);
            size_t alignedSize = 0;
            std::byte* ptr = m_buffer.Allocate(totalSize, &alignedSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::DestroyEntity, 0, static_cast<uint32_t>(alignedSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) DestroyEntityPayload{entity};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Create multiple entities at once.
         */
        void CreateEntities(size_t count, Entity* outEntities)
        {
            if (!m_registry || count == 0)
                return;

            if (m_deferredCreation)
            {
                // Worker-safe: fill placeholders, record them, allocate at flush.
                for (size_t i = 0; i < count; ++i)
                    outEntities[i] = MakePlaceholder();

                size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntitiesPayload) + count * sizeof(Entity);
                std::byte* ptr = m_buffer.Allocate(totalSize);
                StampCommand(ptr);

                new (ptr) CommandHeader{CommandType::CreateEntities, 0, static_cast<uint32_t>(totalSize)};
                auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntitiesPayload{static_cast<uint32_t>(count)};

                Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
                std::memcpy(entityDst, outEntities, count * sizeof(Entity));

                m_commandCount++;
                return;
            }

            auto& manager = m_registry->GetEntityManager();
            size_t created = manager.CreateBatch(count, outEntities);
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0)
                return;

            // Track for potential rollback
            for (size_t i = 0; i < created; ++i)
            {
                m_allocatedEntities.push_back(outEntities[i]);
            }

            // Calculate total size
            size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntitiesPayload) + created * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::CreateEntities, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntitiesPayload{static_cast<uint32_t>(created)};
            (void)header;

            // Copy entities after payload
            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, outEntities, created * sizeof(Entity));

            m_commandCount++;
        }

        /**
         * Destroy multiple entities at once.
         */
        void DestroyEntities(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t count = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntitiesPayload) + count * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::DestroyEntities, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) DestroyEntitiesPayload{static_cast<uint32_t>(count)};
            (void)header;

            // Copy entities after payload
            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, entities.data(), count * sizeof(Entity));

            m_commandCount++;
        }

        // ============= Component Commands =============

        /**
         * Add a component to an entity. The component data is stored inline in the buffer.
         *
         * Thread-safety note: this registers T with the Registry's
         * ComponentRegistry at record time, which is NOT thread-safe. T
         * must already be registered (main-thread AddComponent/CreateView/
         * RegisterComponent, or an existing entity carrying it) before two
         * worker threads may concurrently first-add DIFFERENT component
         * types via their own CommandBuffers -- otherwise the concurrent
         * first-registrations race.
         */
        template<Component T>
        void AddComponent(Entity entity, T&& component)
        {
            using DecayedT = std::decay_t<T>;
            static_assert(alignof(DecayedT) <= CommandByteBuffer::ALIGNMENT,
                "CommandBuffer supports component alignment up to 16 bytes; "
                "add over-aligned components directly via Registry::AddComponent");

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            // Calculate total size with alignment padding
            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(AddComponentPayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::AddComponent, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) AddComponentPayload{
                entity,
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                &DestructComponent<DecayedT>
            };
            (void)payload;

            // Write component data inline (properly aligned)
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(std::forward<T>(component));

            m_commandCount++;
        }

        /**
         * Emplace a component on an entity with constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceComponent(Entity entity, Args&&... args)
        {
            // Create the component and add it
            AddComponent<T>(entity, T(std::forward<Args>(args)...));
        }

        /**
         * Remove a component from an entity.
         */
        template<Component T>
        void RemoveComponent(Entity entity)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveComponentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveComponent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveComponentPayload{entity, TypeID<T>::Value()};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a component to multiple entities with the same value.
         */
        template<Component T>
        void AddComponents(std::span<const Entity> entities, const T& component)
        {
            if (entities.empty())
                return;

            using DecayedT = std::decay_t<T>;
            static_assert(alignof(DecayedT) <= CommandByteBuffer::ALIGNMENT,
                "CommandBuffer supports component alignment up to 16 bytes; "
                "add over-aligned components directly via Registry::AddComponent");

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            size_t entityCount = entities.size();
            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(AddComponentBatchPayload);
            size_t entitiesSize = entityCount * sizeof(Entity);
            size_t dataOffset = AlignUp(headerSize + payloadSize + entitiesSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::AddComponentBatch, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) AddComponentBatchPayload{
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                static_cast<uint32_t>(entityCount),
                &DestructComponent<DecayedT>
            };

            // Copy entities
            Entity* entityDst = payload->GetEntitiesPtr();
            std::memcpy(entityDst, entities.data(), entitiesSize);

            // Write component data inline
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(component);

            m_commandCount++;
        }

        /**
         * Emplace a component on multiple entities with the same constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceComponents(std::span<const Entity> entities, Args&&... args)
        {
            AddComponents<T>(entities, T(std::forward<Args>(args)...));
        }

        /**
         * Remove a component from multiple entities.
         */
        template<Component T>
        void RemoveComponents(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t entityCount = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveComponentBatchPayload) + entityCount * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveComponentBatch, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveComponentBatchPayload{
                TypeID<T>::Value(),
                0,  // padding
                static_cast<uint32_t>(entityCount)
            };
            (void)header;

            // Copy entities
            Entity* entityDst = payload->GetEntitiesPtr();
            std::memcpy(entityDst, entities.data(), entityCount * sizeof(Entity));

            m_commandCount++;
        }

        // ============= Relationship Commands =============

        /**
         * Set the parent of an entity.
         */
        void SetParent(Entity child, Entity parent)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(SetParentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::SetParent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) SetParentPayload{child, parent};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a child to a parent (same as SetParent with reversed parameters).
         */
        void AddChild(Entity parent, Entity child)
        {
            SetParent(child, parent);
        }

        /**
         * Remove the parent from an entity.
         */
        void RemoveParent(Entity child)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveParentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveParent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveParentPayload{child};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove a specific child from a parent.
         */
        void RemoveChild(Entity parent, Entity child)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveChildPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveChild, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveChildPayload{parent, child};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove all children from a parent.
         */
        void RemoveAllChildren(Entity parent)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveAllChildrenPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveAllChildren, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveAllChildrenPayload{parent};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a bidirectional link between two entities.
         */
        void AddLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(AddLinkPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::AddLink, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) AddLinkPayload{a, b};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove a bidirectional link between two entities.
         */
        void RemoveLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveLinkPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveLink, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveLinkPayload{a, b};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        // ============= Resource Commands =============

        /**
         * Set a global resource.
         */
        template<Component T>
        void SetResource(T&& resource)
        {
            using DecayedT = std::decay_t<T>;
            static_assert(alignof(DecayedT) <= CommandByteBuffer::ALIGNMENT,
                "CommandBuffer supports component alignment up to 16 bytes; "
                "add over-aligned resources directly via Registry::SetResource");

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(SetResourcePayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::SetResource, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) SetResourcePayload{
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                0,  // padding
                &DestructComponent<DecayedT>
            };
            (void)payload;

            // Write resource data inline
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(std::forward<T>(resource));

            m_commandCount++;
        }

        /**
         * Emplace a global resource with constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceResource(Args&&... args)
        {
            SetResource<T>(T(std::forward<Args>(args)...));
        }

        /**
         * Remove a global resource.
         */
        template<Component T>
        void RemoveResource()
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveResourcePayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveResource, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveResourcePayload{TypeID<T>::Value()};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Clear all global resources.
         */
        void ClearResources()
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(ClearResourcesPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);
            StampCommand(ptr);

            auto* header = new (ptr) CommandHeader{CommandType::ClearResources, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) ClearResourcesPayload{};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        // ============= Execution and Management =============

        /**
         * Execute all recorded commands.
         * On success, the buffer is cleared (if clearAfterExecution is true).
         *
         * On failure:
         * - Commands that already executed remain in effect (NOT rolled back)
         * - Pre-allocated entities that weren't processed yet are destroyed
         * - Component data in the buffer is cleaned up
         * - Returns error with m_lastExecutedCount set for debugging
         *
         * @param clearAfterExecution If true, clears the buffer after successful execution
         * @return Result indicating success or the type of failure
         */
        Result<void, ExecutionError> Execute(bool clearAfterExecution = true)
        {
            if (!m_registry)
            {
                RollbackAllocatedEntities();
                return Result<void, ExecutionError>::Err(ExecutionError::InvalidRegistry);
            }

            std::byte* ptr = m_buffer.Data();
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(ptr) % CommandByteBuffer::ALIGNMENT) == 0,
                         "Command buffer base must be 16-aligned");
            std::byte* end = ptr + m_buffer.Size();
            m_lastExecutedCount = 0;

            // Deferred-mode buffers (ParallelCommandBuffer's per-worker buffers)
            // carry placeholder entities from CreateEntity/CreateEntities; a
            // per-Execute() map resolves them to real ids in physical apply
            // order (each buffer owns its own placeholders, so a local map is
            // sufficient here -- the cross-buffer key is only needed by the
            // sorted flush). Eager buffers never enter this branch, paying
            // nothing.
            PlaceholderMap placeholders;

            while (ptr < end)
            {
                auto* header = reinterpret_cast<CommandHeader*>(ptr);
                std::byte* payloadPtr = ptr + sizeof(CommandHeader);

                if (m_deferredCreation)
                    ResolvePlaceholders(header->type, payloadPtr, placeholders);

                bool success = ExecuteCommand(header->type, payloadPtr);

                if (!success)
                {
                    // Partial execution occurred - clean up what we can
                    // Note: Already-executed commands are NOT rolled back
                    RollbackAllocatedEntities();
                    CleanupPendingCommands();
                    m_buffer.Clear();
                    m_commandCount = 0;
                    m_commandKeys.clear();
                    m_hasCustomSortKey = false;
                    m_autoSeq = 0;
                    m_nextPlaceholder = 0;
                    return Result<void, ExecutionError>::Err(ExecutionError::ExecutionFailed);
                }

                // Advance by aligned size (buffer allocates with 8-byte alignment)
                ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
                m_lastExecutedCount++;
            }

            // Success - clear allocated entities tracking
            m_allocatedEntities.clear();
            m_committedCount = 0;

            if (clearAfterExecution)
            {
                Clear();
            }

            return Result<void, ExecutionError>::Ok();
        }

        /**
         * Get the number of commands that were successfully executed in the last Execute() call.
         * Useful for debugging partial execution failures.
         */
        [[nodiscard]] size_t GetLastExecutedCount() const noexcept { return m_lastExecutedCount; }

        /**
         * Get the {SortKey, byte offset} descriptor recorded for every command
         * currently in this buffer, in the same order they were recorded
         * (i.e. in the same order their offsets appear walking the byte
         * buffer). Consumed by ParallelCommandBuffer::ExecuteSorted() to build
         * a cross-buffer, globally-sorted apply order; never used by the
         * physical-order Execute() path.
         */
        [[nodiscard]] const std::vector<std::pair<SortKey, size_t>>& CommandKeys() const noexcept
        {
            return m_commandKeys;
        }

        /**
         * Apply the single command whose header starts at the given byte
         * offset into this buffer, via the same per-command dispatch Execute()
         * uses. This is the public entry point ParallelCommandBuffer::
         * ExecuteSorted() uses to apply commands out of physical order without
         * reaching into CommandBuffer's private execution internals.
         *
         * @param offset Byte offset of a CommandHeader previously returned via
         *               CommandKeys(); must belong to THIS buffer.
         * @return true if the command applied successfully (same semantics as
         *         each per-command Execute*() helper).
         */
        bool ApplyCommandAt(size_t offset)
        {
            std::byte* ptr = m_buffer.Data() + offset;
            auto* header = reinterpret_cast<CommandHeader*>(ptr);
            std::byte* payloadPtr = ptr + sizeof(CommandHeader);
            return ExecuteCommand(header->type, payloadPtr);
        }

        /**
         * Placeholder-resolving variant of ApplyCommandAt, used by the sorted
         * flush (ParallelCommandBuffer::ExecuteSorted). Before applying, it
         * translates every placeholder Entity field in the command through
         * `map`, and -- for a CreateEntity/CreateEntities command -- allocates
         * the real id THEN (single-threaded at the flush -> worker-safe and,
         * because ExecuteSorted visits commands in sort-key order,
         * deterministic) and records placeholder->real in `map` so later
         * commands referencing it resolve correctly.
         *
         * `map` MUST be the per-buffer map for THIS buffer: placeholders are
         * per-buffer counters, so two buffers can mint the same placeholder
         * value; ExecuteSorted therefore keys one map per CommandBuffer*.
         *
         * @param offset Byte offset of a CommandHeader from CommandKeys().
         * @param map    This buffer's placeholder->real resolution map.
         * @return true iff the (translated) command applied successfully.
         */
        bool ResolveAndApplyCommandAt(size_t offset, PlaceholderMap& map)
        {
            std::byte* ptr = m_buffer.Data() + offset;
            auto* header = reinterpret_cast<CommandHeader*>(ptr);
            std::byte* payloadPtr = ptr + sizeof(CommandHeader);
            ResolvePlaceholders(header->type, payloadPtr, map);
            return ExecuteCommand(header->type, payloadPtr);
        }

        /**
         * Clear all pending commands without executing them.
         * Also cleans up any component data destructors.
         */
        void Clear()
        {
            CleanupPendingCommands();
            m_buffer.Clear();
            m_allocatedEntities.clear();
            m_committedCount = 0;
            m_commandCount = 0;
            m_commandKeys.clear();
            m_hasCustomSortKey = false;
            m_autoSeq = 0;
            m_nextPlaceholder = 0;
            m_reportedErrors.clear();
        }

        /**
         * Reserve space in the command buffer for the expected number of bytes.
         */
        void Reserve(size_t bytes)
        {
            m_buffer.Reserve(bytes);
        }

        /**
         * Merge commands from another buffer into this one.
         * The other buffer is left empty after the merge.
         *
         * NOTE: this does not carry the other buffer's SortKey descriptors
         * over into this buffer's CommandKeys() -- MergeFrom predates
         * ExecuteSorted() and nothing routes a merged-into buffer through it
         * today (ExecuteSorted reads each worker buffer directly, never via
         * MergeInto). other's descriptors are dropped below purely so
         * CommandKeys() on the now-empty `other` doesn't return stale offsets.
         */
        void MergeFrom(CommandBuffer&& other)
        {
            // Copy buffer data
            size_t otherSize = other.m_buffer.Size();
            if (otherSize > 0)
            {
                std::byte* dst = m_buffer.Allocate(otherSize);
                std::memcpy(dst, other.m_buffer.Data(), otherSize);
            }

            // Merge allocated entities
            m_allocatedEntities.insert(
                m_allocatedEntities.end(),
                other.m_allocatedEntities.begin(),
                other.m_allocatedEntities.end()
            );

            m_commandCount += other.m_commandCount;

            // Clear other buffer (don't call CleanupPendingCommands since we copied the data)
            other.m_buffer.Clear();
            other.m_allocatedEntities.clear();
            other.m_committedCount = 0;
            other.m_commandCount = 0;
            other.m_commandKeys.clear();
            other.m_hasCustomSortKey = false;
            other.m_autoSeq = 0;
        }

        /**
         * Get the number of commands in the buffer.
         */
        [[nodiscard]] size_t GetCommandCount() const noexcept
        {
            return m_commandCount;
        }

        /**
         * Check if the buffer is empty.
         */
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_commandCount == 0;
        }

        /**
         * Get the memory usage of the command buffer in bytes.
         */
        [[nodiscard]] size_t GetMemoryUsage() const noexcept
        {
            return m_buffer.Size() + m_allocatedEntities.capacity() * sizeof(Entity);
        }

        /**
         * Rollback all entities that were allocated but not yet added to archetypes.
         *
         * m_allocatedEntities is populated in the same order CreateEntity/CreateEntities
         * commands appear in the command buffer, and m_committedCount advances over that
         * same prefix as each such command executes successfully during Execute(). So the
         * committed entities are always exactly the prefix [0, m_committedCount) and only
         * the uncommitted suffix needs to be destroyed here; committed entities already
         * have live archetype rows and must survive an aborted Execute().
         */
        void RollbackAllocatedEntities()
        {
            if (m_registry)
            {
                auto& manager = m_registry->GetEntityManager();
                for (size_t i = m_committedCount; i < m_allocatedEntities.size(); ++i)
                {
                    manager.Destroy(m_allocatedEntities[i]);
                }
            }
            m_allocatedEntities.clear();
            m_committedCount = 0;
        }

    private:
        /**
         * Record the {SortKey, offset} descriptor for the command whose header
         * was just allocated at commandPtr. Must be called immediately after
         * m_buffer.Allocate() returns, before any further allocation on this
         * buffer, so the offset is computed from the current (possibly just-
         * reallocated) base pointer -- storing an offset rather than the raw
         * pointer keeps the descriptor valid across future buffer growth.
         */
        void StampCommand(const std::byte* commandPtr)
        {
            size_t offset = static_cast<size_t>(commandPtr - m_buffer.Data());
            SortKey key = m_hasCustomSortKey ? m_currentSortKey : SortKey{0, 0, m_autoSeq};
            ++m_autoSeq;
            m_commandKeys.emplace_back(key, offset);
        }

        // ============= Placeholder Entities (deferred creation, Task 5) =======

        /**
         * Mint the next per-buffer placeholder Entity for deferred creation.
         *
         * ENCODING (collision-free by construction): the version field is set
         * to 0 and the id field holds a per-buffer monotonic counter. A real
         * entity handed out by EntityManager ALWAYS carries a nonzero version
         * (INITIAL_VERSION == 1; recycling wraps 255->1, never to NULL_VERSION
         * == 0; IsValid() rejects version 0), so a version-0 handle can never
         * equal any real entity -- see IsPlaceholderEntity. It is also never the
         * all-ones INVALID sentinel (whose version field is all ones, != 0). The
         * id field is ID_BITS wide (2^24 values in the default build), so a
         * single buffer would have to defer 16M creations in one flush window to
         * exhaust the space; the counter resets to 0 every Clear()/flush.
         */
        Entity MakePlaceholder() noexcept
        {
            // Capacity invariant: at m_nextPlaceholder == ID_MASK the id&ID_MASK
            // wrap would alias this placeholder with id 0, aliasing two
            // resolution-map keys. Can't-happen in practice (16M deferred
            // creates in one flush window); Debug-only tripwire, not a
            // shipping check.
            ASTRA_ASSERT(m_nextPlaceholder < Entity::ID_MASK, "CommandBuffer: per-flush placeholder capacity exhausted (16M deferred creates)");
            Entity placeholder(static_cast<Entity::StorageType>(m_nextPlaceholder),
                               static_cast<Entity::VersionType>(0));
            ++m_nextPlaceholder;
            return placeholder;
        }

        // True iff `e` is a deferred-creation placeholder (version field == 0).
        // Real entities never have version 0; the INVALID sentinel has an
        // all-ones version, so this also excludes it.
        static constexpr bool IsPlaceholderEntity(Entity e) noexcept
        {
            return e.GetVersion() == 0;
        }

        /**
         * Translate an entity FIELD of an already-recorded command from a
         * placeholder to its resolved real id, in place. A non-placeholder
         * (real) entity passes through untouched. A placeholder with no entry
         * in `map` -- an unresolved reference (created in a DIFFERENT buffer, or
         * never created by any CreateEntity in this flush) -- is left as-is: it
         * is a version-0 handle that IsValid() rejects, so the Registry op will
         * fail and be reported through the Task 4 error channel
         * (InvalidTargetEntity). No Registry placeholder-awareness needed.
         */
        static void TranslateEntity(Entity& e, const PlaceholderMap& map) noexcept
        {
            if (!IsPlaceholderEntity(e))
                return;
            auto it = map.find(e.GetValue());
            if (it != map.end())
                e = it->second;
        }

        /**
         * Resolve a CreateEntity/CreateEntities payload's OWN entity slot: if it
         * is a placeholder, allocate a real id now (single-threaded at flush)
         * and record placeholder->real in `map`, back-patching the slot so the
         * create executor adds the real id. A non-placeholder slot (eager-mode
         * create, already allocated) passes through unchanged.
         */
        void ResolveCreatedEntity(Entity& e, PlaceholderMap& map)
        {
            if (!IsPlaceholderEntity(e))
                return;
            const Entity::StorageType placeholderValue = e.GetValue();
            Entity real = m_registry->GetEntityManager().Create();
            map.emplace(placeholderValue, real);  // real may be Invalid on id-space exhaustion; recorded so refs also see Invalid
            e = real;
        }

        /**
         * Back-patch every placeholder Entity in the command at `payload` (of
         * the given `type`) through `map`, allocating real ids for
         * CreateEntity/CreateEntities. Mirrors ExecuteCommand's type switch;
         * resource commands carry no entities. Called by Execute() (deferred
         * mode) and ResolveAndApplyCommandAt() (sorted flush) immediately
         * before the command is applied.
         */
        void ResolvePlaceholders(CommandType type, std::byte* payload, PlaceholderMap& map)
        {
            switch (type)
            {
                case CommandType::CreateEntity:
                {
                    auto* cmd = reinterpret_cast<CreateEntityPayload*>(payload);
                    ResolveCreatedEntity(cmd->entity, map);
                    break;
                }
                case CommandType::CreateEntities:
                {
                    auto* cmd = reinterpret_cast<CreateEntitiesPayload*>(payload);
                    Entity* entities = reinterpret_cast<Entity*>(cmd + 1);
                    for (uint32_t i = 0; i < cmd->entityCount; ++i)
                        ResolveCreatedEntity(entities[i], map);
                    break;
                }
                case CommandType::DestroyEntity:
                {
                    TranslateEntity(reinterpret_cast<DestroyEntityPayload*>(payload)->entity, map);
                    break;
                }
                case CommandType::DestroyEntities:
                {
                    auto* cmd = reinterpret_cast<DestroyEntitiesPayload*>(payload);
                    Entity* entities = reinterpret_cast<Entity*>(cmd + 1);
                    for (uint32_t i = 0; i < cmd->entityCount; ++i)
                        TranslateEntity(entities[i], map);
                    break;
                }
                case CommandType::AddComponent:
                {
                    TranslateEntity(reinterpret_cast<AddComponentPayload*>(payload)->entity, map);
                    break;
                }
                case CommandType::RemoveComponent:
                {
                    TranslateEntity(reinterpret_cast<RemoveComponentPayload*>(payload)->entity, map);
                    break;
                }
                case CommandType::AddComponentBatch:
                {
                    auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payload);
                    Entity* entities = cmd->GetEntitiesPtr();
                    for (uint32_t i = 0; i < cmd->entityCount; ++i)
                        TranslateEntity(entities[i], map);
                    break;
                }
                case CommandType::RemoveComponentBatch:
                {
                    auto* cmd = reinterpret_cast<RemoveComponentBatchPayload*>(payload);
                    Entity* entities = cmd->GetEntitiesPtr();
                    for (uint32_t i = 0; i < cmd->entityCount; ++i)
                        TranslateEntity(entities[i], map);
                    break;
                }
                case CommandType::SetParent:
                case CommandType::AddChild:  // dispatches to ExecuteSetParent; two Entity fields regardless of order
                {
                    auto* cmd = reinterpret_cast<SetParentPayload*>(payload);
                    TranslateEntity(cmd->child, map);
                    TranslateEntity(cmd->parent, map);
                    break;
                }
                case CommandType::RemoveParent:
                {
                    TranslateEntity(reinterpret_cast<RemoveParentPayload*>(payload)->child, map);
                    break;
                }
                case CommandType::RemoveChild:
                {
                    auto* cmd = reinterpret_cast<RemoveChildPayload*>(payload);
                    TranslateEntity(cmd->parent, map);
                    TranslateEntity(cmd->child, map);
                    break;
                }
                case CommandType::RemoveAllChildren:
                {
                    TranslateEntity(reinterpret_cast<RemoveAllChildrenPayload*>(payload)->parent, map);
                    break;
                }
                case CommandType::AddLink:
                {
                    auto* cmd = reinterpret_cast<AddLinkPayload*>(payload);
                    TranslateEntity(cmd->a, map);
                    TranslateEntity(cmd->b, map);
                    break;
                }
                case CommandType::RemoveLink:
                {
                    auto* cmd = reinterpret_cast<RemoveLinkPayload*>(payload);
                    TranslateEntity(cmd->a, map);
                    TranslateEntity(cmd->b, map);
                    break;
                }
                case CommandType::SetResource:
                case CommandType::RemoveResource:
                case CommandType::ClearResources:
                default:
                    break;  // resource commands carry no entity fields
            }
        }

        /**
         * Destructor function for component cleanup.
         */
        template<typename T>
        static void DestructComponent(void* ptr)
        {
            static_cast<T*>(ptr)->~T();
        }

        /**
         * Execute a single command from the buffer.
         */
        bool ExecuteCommand(CommandType type, std::byte* payload)
        {
            switch (type)
            {
                case CommandType::CreateEntity:
                    return ExecuteCreateEntity(payload);
                case CommandType::DestroyEntity:
                    return ExecuteDestroyEntity(payload);
                case CommandType::CreateEntities:
                    return ExecuteCreateEntities(payload);
                case CommandType::DestroyEntities:
                    return ExecuteDestroyEntities(payload);
                case CommandType::AddComponent:
                    return ExecuteAddComponent(payload);
                case CommandType::RemoveComponent:
                    return ExecuteRemoveComponent(payload);
                case CommandType::AddComponentBatch:
                    return ExecuteAddComponentBatch(payload);
                case CommandType::RemoveComponentBatch:
                    return ExecuteRemoveComponentBatch(payload);
                case CommandType::SetParent:
                    return ExecuteSetParent(payload);
                case CommandType::RemoveParent:
                    return ExecuteRemoveParent(payload);
                case CommandType::AddChild:
                    return ExecuteSetParent(payload);  // Same implementation
                case CommandType::RemoveChild:
                    return ExecuteRemoveChild(payload);
                case CommandType::RemoveAllChildren:
                    return ExecuteRemoveAllChildren(payload);
                case CommandType::AddLink:
                    return ExecuteAddLink(payload);
                case CommandType::RemoveLink:
                    return ExecuteRemoveLink(payload);
                case CommandType::SetResource:
                    return ExecuteSetResource(payload);
                case CommandType::RemoveResource:
                    return ExecuteRemoveResource(payload);
                case CommandType::ClearResources:
                    return ExecuteClearResources(payload);
                default:
                    return false;
            }
        }

        // ============= Command Executors =============

        bool ExecuteCreateEntity(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<CreateEntityPayload*>(payload);
            // In eager mode cmd->entity is a real, already-allocated id (never
            // Invalid -- CreateEntity early-returns before recording on
            // exhaustion). In deferred mode ResolvePlaceholders has back-patched
            // this to the real id it allocated at flush; if allocation failed
            // (id space exhausted) it left Invalid here, which we must not
            // AddEntity(). Report+skip via the same false-return path.
            if (cmd->entity == Entity::Invalid())
                return false;
            m_registry->GetArchetypeManager()->AddEntity(cmd->entity);
            m_registry->GetSignalManager()->Emit<Events::EntityCreated>(cmd->entity);
            // Commit point: cmd->entity now has a live archetype row and must
            // be excluded from any later RollbackAllocatedEntities() in this
            // Execute() call. m_allocatedEntities is populated in the same
            // order CreateEntity commands appear in the buffer, so advancing
            // by one here keeps the committed set an exact prefix.
            ++m_committedCount;
            return true;
        }

        bool ExecuteDestroyEntity(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<DestroyEntityPayload*>(payload);
            if (cmd->entity == Entity::Invalid())
                return false;
            m_registry->DestroyEntity(cmd->entity);
            return true;
        }

        bool ExecuteCreateEntities(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<CreateEntitiesPayload*>(payload);
            Entity* entities = reinterpret_cast<Entity*>(cmd + 1);

            auto* archetypeManager = m_registry->GetArchetypeManager();
            auto* signalManager = m_registry->GetSignalManager();

            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                // A deferred-mode placeholder whose flush-time allocation failed
                // (id space exhausted) was left Invalid by ResolvePlaceholders;
                // skip it rather than AddEntity(Invalid). Eager-mode batches
                // only ever record successfully-allocated ids, so this never
                // fires for them.
                if (entities[i] == Entity::Invalid())
                    continue;
                archetypeManager->AddEntity(entities[i]);
                signalManager->Emit<Events::EntityCreated>(entities[i]);
            }
            // Commit point for the whole batch: see ExecuteCreateEntity for
            // why this keeps m_allocatedEntities' committed set a prefix.
            m_committedCount += cmd->entityCount;
            return true;
        }

        bool ExecuteDestroyEntities(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<DestroyEntitiesPayload*>(payload);
            Entity* entities = reinterpret_cast<Entity*>(cmd + 1);

            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(cmd->entityCount);

            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    validEntities.push_back(entities[i]);
                }
            }

            if (!validEntities.empty())
            {
                m_registry->DestroyEntities(validEntities);
            }
            return true;
        }

        bool ExecuteAddComponent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentPayload*>(payload);

            if (cmd->entity == Entity::Invalid())
                return false;

            const void* data = cmd->GetDataPtr();
            return m_registry->AddComponentByID(cmd->entity, cmd->componentId, data, cmd->dataSize);
        }

        bool ExecuteRemoveComponent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentPayload*>(payload);
            if (cmd->entity == Entity::Invalid())
                return false;

            return m_registry->RemoveComponentByID(cmd->entity, cmd->componentId);
        }

        bool ExecuteAddComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();
            const void* data = cmd->GetDataPtr();

            // Use direct single-entity calls to avoid any span conversion issues
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    m_registry->AddComponentByID(entities[i], cmd->componentId, data, cmd->dataSize);
                }
            }
            return true;
        }

        bool ExecuteRemoveComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();

            // Use direct single-entity calls to avoid any span conversion issues
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    m_registry->RemoveComponentByID(entities[i], cmd->componentId);
                }
            }
            return true;
        }

        bool ExecuteSetParent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<SetParentPayload*>(payload);
            if (cmd->child == Entity::Invalid() || cmd->parent == Entity::Invalid())
                return false;
            m_registry->SetParent(cmd->child, cmd->parent);
            return true;
        }

        bool ExecuteRemoveParent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveParentPayload*>(payload);
            if (cmd->child == Entity::Invalid())
                return false;
            m_registry->RemoveParent(cmd->child);
            return true;
        }

        bool ExecuteRemoveChild(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveChildPayload*>(payload);
            if (cmd->parent == Entity::Invalid() || cmd->child == Entity::Invalid())
                return false;
            m_registry->RemoveChild(cmd->parent, cmd->child);
            return true;
        }

        bool ExecuteRemoveAllChildren(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveAllChildrenPayload*>(payload);
            if (cmd->parent == Entity::Invalid())
                return false;
            m_registry->RemoveAllChildren(cmd->parent);
            return true;
        }

        bool ExecuteAddLink(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddLinkPayload*>(payload);
            if (cmd->a == Entity::Invalid() || cmd->b == Entity::Invalid())
                return false;
            m_registry->AddLink(cmd->a, cmd->b);
            return true;
        }

        bool ExecuteRemoveLink(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveLinkPayload*>(payload);
            if (cmd->a == Entity::Invalid() || cmd->b == Entity::Invalid())
                return false;
            m_registry->RemoveLink(cmd->a, cmd->b);
            return true;
        }

        bool ExecuteSetResource(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<SetResourcePayload*>(payload);
            const void* data = cmd->GetDataPtr();
            return m_registry->SetResourceByID(cmd->componentId, data, cmd->dataSize);
        }

        bool ExecuteRemoveResource(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveResourcePayload*>(payload);
            return m_registry->RemoveResourceByID(cmd->componentId);
        }

        bool ExecuteClearResources([[maybe_unused]] std::byte* payload)
        {
            m_registry->ClearResources();
            return true;
        }

        /**
         * Clean up any component data stored in pending commands.
         * Called during Clear() and destructor.
         */
        void CleanupPendingCommands()
        {
            std::byte* ptr = m_buffer.Data();
            std::byte* end = ptr + m_buffer.Size();

            while (ptr < end)
            {
                auto* header = reinterpret_cast<CommandHeader*>(ptr);
                std::byte* payloadPtr = ptr + sizeof(CommandHeader);

                // Only need to cleanup commands with inline component data
                switch (header->type)
                {
                    case CommandType::AddComponent:
                    {
                        auto* cmd = reinterpret_cast<AddComponentPayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    case CommandType::AddComponentBatch:
                    {
                        auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    case CommandType::SetResource:
                    {
                        auto* cmd = reinterpret_cast<SetResourcePayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    default:
                        break;
                }

                // Advance by aligned size (buffer allocates with 8-byte alignment)
                ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
            }
        }

        Registry* m_registry;
        CommandByteBuffer m_buffer;
        std::vector<Entity> m_allocatedEntities;
        // Entities in m_allocatedEntities[0, m_committedCount) have already been
        // placed into an archetype (by ExecuteCreateEntity/ExecuteCreateEntities)
        // earlier in the current Execute() call and are live rows; only the
        // suffix [m_committedCount, size()) is rolled back on partial failure.
        // Reset to 0 everywhere m_allocatedEntities is cleared.
        size_t m_committedCount = 0;
        size_t m_commandCount = 0;
        size_t m_lastExecutedCount = 0;  // For debugging partial execution failures

        // Task 5: deferred-creation mode (ParallelCommandBuffer per-worker
        // buffers). When true, CreateEntity/CreateEntities mint placeholders
        // instead of touching the shared EntityManager at record time, and the
        // flush resolves them (see MakePlaceholder/ResolvePlaceholders). Fixed
        // at construction; standalone buffers are eager (false).
        const bool m_deferredCreation = false;
        // Per-buffer placeholder counter (id field of the next placeholder);
        // reset to 0 on every Clear()/flush, so it stays small frame to frame.
        Entity::StorageType m_nextPlaceholder = 0;

        // Sort-key bookkeeping, parallel to m_buffer (see StampCommand/SetNextSortKey).
        std::vector<std::pair<SortKey, size_t>> m_commandKeys;
        SortKey m_currentSortKey{};
        bool m_hasCustomSortKey = false;
        uint32_t m_autoSeq = 0;  // stamped as recordSequence when no explicit key was set

        // Task 4: this buffer's own deferred-command errors (see ReportError()).
        std::vector<DeferredCommandError> m_reportedErrors;
    };

    /**
     * Thread-safe command buffer that provides per-thread buffers.
     * Commands from all threads are executed sequentially when Execute() is called.
     */
    class ParallelCommandBuffer
    {
    public:
        explicit ParallelCommandBuffer(Registry* registry) :
            m_registry(registry),
            // Every instance gets a process-lifetime-unique id (never reused,
            // 64-bit monotonic counter) -- see t_cache/ThreadCache below for
            // why this exists: it is NOT the same thing as m_registry and
            // must not be conflated with it.
            m_instanceId(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
            // Pre-reserve space for typical thread counts
            const size_t expectedThreads = std::thread::hardware_concurrency();
            m_buffers.reserve(expectedThreads);
        }

        /**
         * Get the command buffer for the current thread.
         * Creates a new buffer if one doesn't exist for this thread.
         *
         * Relies on stable OS-thread identity (thread_local cache): callers
         * running on fiber-based job systems must pin the fiber to its thread
         * while recording commands, or the buffer of a different thread may
         * be written concurrently.
         *
         * These per-worker buffers are DEFERRED-mode (Task 5), so
         * CreateEntity/CreateEntities ARE safe to record from any worker
         * thread: they mint a placeholder and record it without touching the
         * shared EntityManager, and the flush resolves the placeholder to a
         * real id single-threaded and deterministically.
         */
        CommandBuffer& GetThreadBuffer() const
        {
            // Fast path: check thread-local cache. Guarded by BOTH the raw
            // pointer AND m_instanceId (see ThreadCache below) -- pointer
            // equality alone is not sufficient: if a ParallelCommandBuffer is
            // destroyed and a later instance happens to be allocated at the
            // same address, `t_cache.context == this` can spuriously match a
            // STALE cache entry left behind by the destroyed instance,
            // returning a dangling CommandBuffer& (use-after-free). The
            // per-instance id can never repeat for the process lifetime, so
            // this cannot false-positive.
            if (t_cache.context == this && t_cache.contextInstanceId == m_instanceId && t_cache.buffer != nullptr)
            {
                return *t_cache.buffer;
            }

            // Slow path: create new buffer for this thread
            return InitializeThreadBuffer();
        }

        /**
         * Execute all commands from all thread buffers.
         */
        Result<void, CommandBuffer::ExecutionError> Execute()
        {
            for (size_t i = 0; i < m_buffers.size(); ++i)
            {
                if (m_buffers[i] && !m_buffers[i]->IsEmpty())
                {
                    auto result = m_buffers[i]->Execute();
                    if (result.IsErr())
                    {
                        // Rollback remaining buffers' allocated entities
                        for (size_t j = i + 1; j < m_buffers.size(); ++j)
                        {
                            if (m_buffers[j])
                            {
                                m_buffers[j]->RollbackAllocatedEntities();
                            }
                        }
                        return result;
                    }
                }
            }
            return Result<void, CommandBuffer::ExecutionError>::Ok();
        }

        /**
         * Execute every recorded command from every thread buffer in
         * deterministic SortKey order rather than physical arrival order.
         *
         * Gathers a {SortKey, CommandBuffer*, offset} descriptor for every
         * command across every worker buffer (via CommandBuffer::CommandKeys()),
         * stable-sorts by key (so commands with equal keys keep their original
         * gather order -- which is arrival order within a buffer, and
         * worker-registration order across buffers), then applies each command
         * single-threaded, in that order, via CommandBuffer::ApplyCommandAt().
         * The result is identical regardless of how many threads recorded
         * commands or in what order they happened to run.
         *
         * FAILURE HANDLING (Task 4): a command that fails to apply (target
         * entity/component state doesn't permit the op -- see
         * DeferredCommandError's doc comment for why this is always a
         * LOGICAL failure, never a "fatal" one the bool could express) is
         * SKIPPED and recorded into GetDeferredErrors(), and the flush
         * CONTINUES -- it does NOT abort or roll back. This is a deliberate
         * change from the whole-buffer-abandonment policy CommandBuffer::
         * Execute() still uses: a system's deferred op can legitimately
         * target an entity an EARLIER system's deferred op destroyed in the
         * same flush (they don't see each other's effects until the sync
         * point), and that must not nuke every other system's unrelated
         * work. The skip is attributed to the failed command's own
         * SortKey::insertionOrder (== the recording system's insertionOrder)
         * and surfaced to the caller via GetDeferredErrors().
         *
         * DETERMINISM PRECONDITION: the sort below is a std::stable_sort, so
         * commands with EQUAL keys fall back to their gather order, which is
         * arrival order within a buffer and worker-registration (buffer
         * slot) order across buffers -- and buffer slot assignment is
         * scheduling-dependent, not deterministic. A fully deterministic
         * cross-buffer apply order therefore requires every recorded
         * SortKey to be globally unique. In real use this holds: the system
         * scheduler stamps each system with a unique insertionOrder and a
         * monotonic recordSequence, so no two systems' commands ever share a
         * key. It does NOT hold for the all-default key path ({0, 0,
         * perBufferSeq}, set when SetNextSortKey() is never called): default
         * keys are only unique within a single buffer, so the relative order
         * of equal-keyed commands recorded on different worker buffers is
         * unspecified.
         */
        Result<void, CommandBuffer::ExecutionError> ExecuteSorted()
        {
            // Task 4: this flush's error list starts empty every call --
            // errors from a PRIOR flush must not leak into this one's result.
            m_deferredErrors.clear();

            struct Item
            {
                SortKey key;
                CommandBuffer* buf;
                size_t offset;
            };

            std::vector<Item> items;
            for (auto& b : m_buffers)
            {
                if (b)
                {
                    for (const auto& [key, offset] : b->CommandKeys())
                    {
                        items.push_back({key, b.get(), offset});
                    }
                }
            }

            std::stable_sort(items.begin(), items.end(),
                [](const Item& a, const Item& b) { return a.key < b.key; });

            // Task 5: placeholder entities (from deferred CreateEntity) resolve
            // to real ids HERE, as each CreateEntity command applies -- so real
            // ids are allocated in sort-key order (deterministic) on this single
            // thread. Placeholders are per-buffer counters, so keep one
            // resolution map PER buffer: buffer A's placeholder value and buffer
            // B's identical placeholder value must not alias. A command carrying
            // a placeholder created in a DIFFERENT buffer finds no entry in its
            // own map, passes through unresolved, and fails the op -> reported
            // below as InvalidTargetEntity (the documented cross-buffer rule).
            std::unordered_map<CommandBuffer*, CommandBuffer::PlaceholderMap> perBufferMaps;

            for (const auto& it : items)
            {
                if (!it.buf->ResolveAndApplyCommandAt(it.offset, perBufferMaps[it.buf]))
                {
                    // Task 4: logical failure -- skip and record, do NOT abort
                    // or roll back the flush. See this function's class-level
                    // FAILURE HANDLING comment and DeferredCommandError's doc
                    // comment for why every ApplyCommandAt() false is a
                    // logical (never fatal) failure in this exception-free
                    // build.
                    m_deferredErrors.push_back(
                        DeferredCommandError{it.key.insertionOrder, DeferredCommandError::Reason::InvalidTargetEntity});
                    continue;
                }
            }

            // Gather every worker buffer's explicitly-reported errors
            // (SystemContext::ReportError()) BEFORE clearing the buffers
            // below (Clear() empties each buffer's own reported-errors list).
            // Safe single-threaded here: this runs at the Task 3 depth==0
            // sync point, so every worker has already joined and nothing can
            // be concurrently writing to any buffer.
            for (auto& b : m_buffers)
            {
                if (b)
                {
                    const auto& reported = b->GetReportedErrors();
                    m_deferredErrors.insert(m_deferredErrors.end(), reported.begin(), reported.end());
                }
            }

            // Every touched buffer must still be cleared, regardless of
            // whether any command was skipped above: ApplyCommandAt() bypasses
            // CommandBuffer::Execute(), so nothing else clears the byte buffer
            // or destructs inline component data (e.g. AddComponent/
            // SetResource payloads) that Execute() would normally clean up via
            // Clear() at the end of a run. This also matches Task 3's
            // established contract that Execute() always leaves the buffer
            // empty afterward.
            for (auto& b : m_buffers)
            {
                if (b)
                {
                    b->Clear();
                }
            }

            // Task 4: a flush that skipped some commands is still an overall
            // success -- the skips are surfaced as errors via
            // GetDeferredErrors(), not as a Result failure. There is no
            // remaining path that returns Err() from this function.
            return Result<void, CommandBuffer::ExecutionError>::Ok();
        }

        /**
         * Every deferred-command error from the most recent ExecuteSorted()
         * call: commands skipped because their target entity/component state
         * didn't permit the op, PLUS anything systems explicitly reported via
         * SystemContext::ReportError(). Cleared at the start of every
         * ExecuteSorted() call (see above); empty before the first call.
         */
        [[nodiscard]] const std::vector<DeferredCommandError>& GetDeferredErrors() const noexcept
        {
            return m_deferredErrors;
        }

        /**
         * Merge all thread buffers into a single target buffer.
         */
        void MergeInto(CommandBuffer& target)
        {
            for (auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    target.MergeFrom(std::move(*buffer));
                }
            }
        }

        /**
         * Clear all thread buffers.
         */
        void Clear()
        {
            for (auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    buffer->Clear();
                }
            }
        }

        /**
         * Get the total number of commands across all thread buffers.
         */
        [[nodiscard]] size_t GetCommandCount() const
        {
            size_t total = 0;
            for (const auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    total += buffer->GetCommandCount();
                }
            }
            return total;
        }

        /**
         * Check if all thread buffers are empty.
         */
        [[nodiscard]] bool IsEmpty() const
        {
            for (const auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * Get the number of thread buffers that have been created.
         */
        [[nodiscard]] size_t GetThreadCount() const
        {
            return m_buffers.size();
        }

    private:
        CommandBuffer& InitializeThreadBuffer() const
        {
            // Allocate a new index for this thread
            const size_t index = m_nextIndex.fetch_add(1, std::memory_order_relaxed);

            // Lock only for vector modification
            std::unique_lock lock(m_mutex);

            // Ensure vector is large enough
            if (index >= m_buffers.size())
            {
                m_buffers.resize(index + 1);
            }

            // Create the buffer if it doesn't exist. Per-worker buffers are
            // DEFERRED-mode (Task 5): CreateEntity/CreateEntities mint
            // placeholders instead of allocating from the shared EntityManager
            // at record time, so creation is safe from any worker thread; the
            // deterministic flush (Execute/ExecuteSorted) resolves them.
            if (!m_buffers[index])
            {
                m_buffers[index] = std::make_unique<CommandBuffer>(m_registry, /*deferredCreation=*/true);
            }

            CommandBuffer* buffer = m_buffers[index].get();

            // Unlock before updating thread-local cache
            lock.unlock();

            // Update thread-local cache
            t_cache.context = const_cast<ParallelCommandBuffer*>(this);
            t_cache.contextInstanceId = m_instanceId;
            t_cache.buffer = buffer;
            t_cache.index = index;

            return *buffer;
        }

        Registry* m_registry;
        const uint64_t m_instanceId;  // see GetThreadBuffer()/ThreadCache: never reused, guards against address-reuse after destruction
        mutable std::mutex m_mutex;
        mutable std::vector<std::unique_ptr<CommandBuffer>> m_buffers;
        mutable std::atomic<size_t> m_nextIndex{0};

        // Task 4: gathered errors from the most recent ExecuteSorted() flush
        // (see ExecuteSorted()/GetDeferredErrors()). Single-threaded: only
        // ever touched from ExecuteSorted() itself, after every worker has
        // joined at the Task 3 depth==0 sync point.
        std::vector<DeferredCommandError> m_deferredErrors;

        inline static std::atomic<uint64_t> s_nextInstanceId{1};  // 0 is never assigned; ThreadCache's default contextInstanceId is 0 so a never-populated cache can't accidentally match

        // Thread-local cache to avoid repeated lookups.
        //
        // SAFETY: keyed by BOTH `context` (raw ParallelCommandBuffer*) AND
        // `contextInstanceId` (ParallelCommandBuffer::m_instanceId). Pointer
        // identity alone is not a safe cache key here: a ParallelCommandBuffer
        // is frequently short-lived (e.g. SystemScheduler owns one and
        // recreates it as needed), and the allocator can place a NEW instance
        // at the exact address of a previously-destroyed one. A long-lived
        // thread (e.g. a test runner's main thread, which persists across many
        // short-lived ParallelCommandBuffers) that cached `context == thatAddress`
        // for the OLD instance would then spuriously hit for the NEW instance
        // too, returning a dangling `CommandBuffer&` into freed memory --
        // observed in practice as heap corruption / access violations under
        // repeated create-Execute-destroy cycles on one thread. The instance
        // id is a 64-bit monotonic counter that is never reused for the
        // process lifetime, so it can't false-positive the way the pointer
        // alone can.
        struct ThreadCache
        {
            ParallelCommandBuffer* context = nullptr;
            uint64_t contextInstanceId = 0;
            CommandBuffer* buffer = nullptr;
            size_t index = std::numeric_limits<size_t>::max();
        };

        static thread_local ThreadCache t_cache;
    };

    // Thread-local storage definition
    inline thread_local ParallelCommandBuffer::ThreadCache ParallelCommandBuffer::t_cache;

} // namespace Astra
