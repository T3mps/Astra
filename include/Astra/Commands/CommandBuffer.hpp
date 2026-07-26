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
     * Internal segmented byte storage for commands. Commands are stored as
     * [Header][Payload] pairs inside a chain of STABLE blocks: once written,
     * a command's bytes never move until Clear()/destruction (Commands C1
     * fix -- growth acquires a fresh block instead of relocating). Commands
     * never straddle blocks; walks use CommandHeader::totalSize within each
     * block's [base, base+used) extent.
     */
    class CommandByteBuffer
    {
    public:
        static constexpr size_t DEFAULT_INITIAL_CAPACITY = 4096;
        static constexpr size_t MAX_BLOCK_BYTES = 64 * 1024;
        // Every command start is aligned to 16 within its block. Blocks come
        // from CommandBlockArena (TLSF payloads: 64B-aligned by its size-
        // congruence law), so block bases satisfy this with headroom.
        static constexpr size_t ALIGNMENT = 16;
        static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= ALIGNMENT,
            "CommandByteBuffer requires operator-new alignment >= 16 (32-bit targets are unsupported)");

        struct Block
        {
            std::byte* base = nullptr;
            size_t capacity = 0;
            size_t used = 0;
        };

        explicit CommandByteBuffer(CommandBlockArena* arena,
                                   size_t initialCapacity = DEFAULT_INITIAL_CAPACITY) :
            m_arena(arena),
            m_nextBlockBytes(initialCapacity)
        {}

        CommandByteBuffer(const CommandByteBuffer&) = delete;
        CommandByteBuffer& operator=(const CommandByteBuffer&) = delete;

        ~CommandByteBuffer()
        {
            if (m_arena)
            {
                for (Block& b : m_blocks)
                    m_arena->Release(b.base);
            }
        }

        /**
         * Allocate STABLE space for one command (never moves until
         * Clear()/destruction). Returns nullptr only on arena exhaustion.
         *
         * @param size           Total size needed (header + payload + data).
         * @param outAlignedSize If non-null, receives the 16-aligned byte
         *                       stride the command actually occupies (some
         *                       record sites stamp this into the header).
         */
        std::byte* Allocate(size_t size, size_t* outAlignedSize = nullptr)
        {
            size_t alignedSize = AlignUp(size, ALIGNMENT);
            if (outAlignedSize)
                *outAlignedSize = alignedSize;

            // Advance-only: a command that doesn't fit the active block moves
            // to the NEXT block (never back), so walking blocks in order
            // always replays record order. Skipped remainders stay dead until
            // Clear() resets the cursors.
            while (m_activeBlock < m_blocks.size() &&
                   m_blocks[m_activeBlock].capacity - m_blocks[m_activeBlock].used < alignedSize)
            {
                ++m_activeBlock;
            }
            if (m_activeBlock == m_blocks.size())
            {
                if (!AcquireBlock(alignedSize)) ASTRA_UNLIKELY
                {
                    ASTRA_ASSERT(false,
                        "command arena exhausted or single command exceeds the TLSF request "
                        "ceiling (batch commands encode all entities inline -- split batches "
                        "over ~4M entities into multiple calls)");
                    return nullptr;
                }
            }

            Block& b = m_blocks[m_activeBlock];
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(b.base) % ALIGNMENT) == 0,
                         "command block base must satisfy command alignment");
            std::byte* ptr = b.base + b.used;
            b.used += alignedSize;
            m_totalUsed += alignedSize;
            return ptr;
        }

        [[nodiscard]] const std::vector<Block>& Blocks() const noexcept { return m_blocks; }
        [[nodiscard]] size_t BlockCount() const noexcept { return m_blocks.size(); }
        [[nodiscard]] size_t Size() const noexcept { return m_totalUsed; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_totalUsed == 0; }

        /**
         * Reset every block's write cursor, KEEPING the blocks (retention:
         * steady-state record->flush->clear cycles make zero arena calls).
         */
        void Clear() noexcept
        {
            for (Block& b : m_blocks)
                b.used = 0;
            m_activeBlock = 0;
            m_totalUsed = 0;
        }

        /**
         * Ensure total capacity >= capacity by acquiring at most one block.
         */
        void Reserve(size_t capacity)
        {
            size_t total = 0;
            for (const Block& b : m_blocks)
                total += b.capacity;
            if (total < capacity)
                AcquireBlock(capacity - total);
        }

    private:
        bool AcquireBlock(size_t minBytes)
        {
            if (!m_arena) ASTRA_UNLIKELY
                return false;
            // Geometric ramp capped at MAX_BLOCK_BYTES; an oversized command
            // gets a block sized to fit it exactly.
            size_t request = std::max(m_nextBlockBytes, minBytes);
            CommandBlockArena::BlockAlloc alloc = m_arena->Acquire(request);
            if (!alloc.ptr) ASTRA_UNLIKELY
                return false;
            m_blocks.push_back(Block{alloc.ptr, alloc.bytes, 0});
            m_activeBlock = m_blocks.size() - 1;
            m_nextBlockBytes = std::min(request * 2, MAX_BLOCK_BYTES);
            return true;
        }

        CommandBlockArena* m_arena = nullptr;
        std::vector<Block> m_blocks;
        size_t m_activeBlock = 0;
        size_t m_nextBlockBytes = DEFAULT_INITIAL_CAPACITY;
        size_t m_totalUsed = 0;
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
     * LIFETIME: a CommandBuffer holds blocks from its Registry's internal command
     * arena and MUST be destroyed BEFORE that Registry -- this includes plain
     * declaration/destruction order in the same scope (e.g. a SystemScheduler or
     * a CommandBuffer declared before its Registry is a bug); see
     * Registry::GetCommandBlockArena() for the enforced invariant.
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
            m_buffer(registry ? &registry->GetCommandBlockArena() : nullptr),
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
                std::byte* ptr = AllocateCommand(CommandType::CreateEntity, totalSize);
                if (!ptr)
                    return placeholder;  // command not recorded; Execute() reports AllocationFailed

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
            std::byte* ptr = AllocateCommand(CommandType::CreateEntity, totalSize);
            if (!ptr)
                return entity;  // entity is tracked in m_allocatedEntities -> rolled back at Execute()

            new (ptr + sizeof(CommandHeader)) CreateEntityPayload{entity};

            m_commandCount++;
            return entity;
        }

        /**
         * Destroy an entity. The entity is destroyed when Execute() is called.
         */
        void DestroyEntity(Entity entity)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntityPayload);
            std::byte* ptr = AllocateCommand(CommandType::DestroyEntity, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) DestroyEntityPayload{entity};

            m_commandCount++;
        }

        /**
         * Create multiple entities at once.
         *
         * This records as ONE command whose encoded size grows with count (the
         * entity array is inline); a single call is capped by the arena's TLSF
         * request ceiling (32MB, ~4M entities) -- split larger batches into
         * multiple calls.
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
                std::byte* ptr = AllocateCommand(CommandType::CreateEntities, totalSize);
                if (!ptr)
                    return;  // command not recorded; Execute() reports AllocationFailed

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
            std::byte* ptr = AllocateCommand(CommandType::CreateEntities, totalSize);
            if (!ptr)
                return;  // created entities are tracked in m_allocatedEntities -> rolled back at Execute()

            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntitiesPayload{static_cast<uint32_t>(created)};

            // Copy entities after payload
            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, outEntities, created * sizeof(Entity));

            m_commandCount++;
        }

        /**
         * Destroy multiple entities at once.
         *
         * This records as ONE command whose encoded size grows with
         * entities.size() (the entity array is inline); a single call is
         * capped by the arena's TLSF request ceiling (32MB, ~4M entities) --
         * split larger batches into multiple calls.
         */
        void DestroyEntities(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t count = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntitiesPayload) + count * sizeof(Entity);
            std::byte* ptr = AllocateCommand(CommandType::DestroyEntities, totalSize);
            if (!ptr)
                return;

            auto* payload = new (ptr + sizeof(CommandHeader)) DestroyEntitiesPayload{static_cast<uint32_t>(count)};

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
            static_assert(sizeof(DecayedT) <= 0xFFFF,
                "CommandBuffer encodes payload size as uint16_t; components/resources larger "
                "than 65535 bytes must go through Registry directly");

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            // Calculate total size with alignment padding
            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(AddComponentPayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = AllocateCommand(CommandType::AddComponent, totalSize);
            if (!ptr)
                return;

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
            std::byte* ptr = AllocateCommand(CommandType::RemoveComponent, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveComponentPayload{entity, TypeID<T>::Value()};

            m_commandCount++;
        }

        /**
         * Defer an enable/disable toggle of enableable component T on an entity
         * (spec 2026-07-25 §7). Applied at flush via Registry::SetEnabledByID;
         * mirrors RemoveComponent's header + POD-payload shape (no inline data).
         */
        template<Component T>
        void SetEnabled(Entity entity, bool enable)
        {
            using DecayedT = std::decay_t<T>;
            static_assert(IsEnableableV<DecayedT>,
                "SetEnabled<T> requires an enableable component (opt in with `static constexpr bool AstraEnableable = true;`)");

            size_t totalSize = sizeof(CommandHeader) + sizeof(SetEnabledPayload);
            std::byte* ptr = AllocateCommand(CommandType::SetEnabled, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) SetEnabledPayload{entity, TypeID<DecayedT>::Value(), static_cast<uint8_t>(enable ? 1 : 0)};

            m_commandCount++;
        }

        /**
         * Add a component to multiple entities with the same value.
         *
         * This records as ONE command whose encoded size grows with
         * entities.size() (the entity array is inline); a single call is
         * capped by the arena's TLSF request ceiling (32MB, ~4M entities) --
         * split larger batches into multiple calls.
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
            static_assert(sizeof(DecayedT) <= 0xFFFF,
                "CommandBuffer encodes payload size as uint16_t; components/resources larger "
                "than 65535 bytes must go through Registry directly");

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

            std::byte* ptr = AllocateCommand(CommandType::AddComponentBatch, totalSize);
            if (!ptr)
                return;

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
         *
         * This records as ONE command whose encoded size grows with
         * entities.size() (the entity array is inline); a single call is
         * capped by the arena's TLSF request ceiling (32MB, ~4M entities) --
         * split larger batches into multiple calls.
         */
        template<Component T>
        void RemoveComponents(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t entityCount = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveComponentBatchPayload) + entityCount * sizeof(Entity);
            std::byte* ptr = AllocateCommand(CommandType::RemoveComponentBatch, totalSize);
            if (!ptr)
                return;

            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveComponentBatchPayload{
                TypeID<T>::Value(),
                0,  // padding
                static_cast<uint32_t>(entityCount)
            };

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
            std::byte* ptr = AllocateCommand(CommandType::SetParent, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) SetParentPayload{child, parent};

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
            std::byte* ptr = AllocateCommand(CommandType::RemoveParent, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveParentPayload{child};

            m_commandCount++;
        }

        /**
         * Remove a specific child from a parent.
         */
        void RemoveChild(Entity parent, Entity child)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveChildPayload);
            std::byte* ptr = AllocateCommand(CommandType::RemoveChild, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveChildPayload{parent, child};

            m_commandCount++;
        }

        /**
         * Remove all children from a parent.
         */
        void RemoveAllChildren(Entity parent)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveAllChildrenPayload);
            std::byte* ptr = AllocateCommand(CommandType::RemoveAllChildren, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveAllChildrenPayload{parent};

            m_commandCount++;
        }

        /**
         * Add a bidirectional link between two entities.
         */
        void AddLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(AddLinkPayload);
            std::byte* ptr = AllocateCommand(CommandType::AddLink, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) AddLinkPayload{a, b};

            m_commandCount++;
        }

        /**
         * Remove a bidirectional link between two entities.
         */
        void RemoveLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveLinkPayload);
            std::byte* ptr = AllocateCommand(CommandType::RemoveLink, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveLinkPayload{a, b};

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
            static_assert(sizeof(DecayedT) <= 0xFFFF,
                "CommandBuffer encodes payload size as uint16_t; components/resources larger "
                "than 65535 bytes must go through Registry directly");

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(SetResourcePayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = AllocateCommand(CommandType::SetResource, totalSize);
            if (!ptr)
                return;

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
            std::byte* ptr = AllocateCommand(CommandType::RemoveResource, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) RemoveResourcePayload{TypeID<T>::Value()};

            m_commandCount++;
        }

        /**
         * Clear all global resources.
         */
        void ClearResources()
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(ClearResourcesPayload);
            std::byte* ptr = AllocateCommand(CommandType::ClearResources, totalSize);
            if (!ptr)
                return;

            new (ptr + sizeof(CommandHeader)) ClearResourcesPayload{};

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

            m_lastExecutedCount = 0;

            // CR-2: if any record method hit arena exhaustion, this buffer is
            // INCOMPLETE (>=1 command silently dropped). Applying its partial
            // contents would silently execute a truncated operation set, so
            // refuse the whole flush, clean up as the partial-failure path
            // does, and surface AllocationFailed. Checked before the walk so a
            // partially-recorded buffer is never applied.
            if (m_recordFailed) ASTRA_UNLIKELY
            {
                RollbackAllocatedEntities();
                CleanupPendingCommands();
                m_buffer.Clear();
                m_commandCount = 0;
                m_commandKeys.clear();
                m_hasCustomSortKey = false;
                m_autoSeq = 0;
                m_nextPlaceholder = 0;
                m_recordFailed = false;
                return Result<void, ExecutionError>::Err(ExecutionError::AllocationFailed);
            }

            // Deferred-mode buffers (ParallelCommandBuffer's per-worker buffers)
            // carry placeholder entities from CreateEntity/CreateEntities; a
            // per-Execute() map resolves them to real ids in physical apply
            // order (each buffer owns its own placeholders, so a local map is
            // sufficient here -- the cross-buffer key is only needed by the
            // sorted flush). Eager buffers never enter this branch, paying
            // nothing.
            PlaceholderMap placeholders;

            // Walk each STABLE block in acquisition order (== record order):
            // every command lives wholly within one block's [base, base+used)
            // extent (C1 fix -- commands never straddle blocks).
            for (const CommandByteBuffer::Block& block : m_buffer.Blocks())
            {
                std::byte* ptr = block.base;
                std::byte* end = block.base + block.used;
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

                    // Advance by aligned size (commands are stored at ALIGNMENT-byte stride)
                    ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
                    m_lastExecutedCount++;
                }
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
         * Per-entity failure count of the most recently applied batch
         * command (see ExecuteSorted's per-entity error reporting).
         */
        [[nodiscard]] size_t GetLastBatchFailureCount() const noexcept { return m_lastBatchFailureCount; }

        /**
         * Get the {SortKey, stable command pointer} descriptor recorded for
         * every command currently in this buffer, in the same order they were
         * recorded (i.e. the same order they appear walking the block chain).
         * The pointers are STABLE (C1 fix): each addresses its command's header
         * in place, valid until Clear()/destruction. Consumed by
         * ParallelCommandBuffer::ExecuteSorted() to build a cross-buffer,
         * globally-sorted apply order; never used by the physical-order
         * Execute() path.
         */
        [[nodiscard]] const std::vector<std::pair<SortKey, std::byte*>>& CommandKeys() const noexcept
        {
            return m_commandKeys;
        }

        /**
         * Apply the single command whose header is at the given stable command
         * pointer, via the same per-command dispatch Execute() uses. This is
         * the public entry point ParallelCommandBuffer::ExecuteSorted() uses to
         * apply commands out of physical order without reaching into
         * CommandBuffer's private execution internals.
         *
         * @param command Stable command pointer from CommandKeys(); must belong
         *                to THIS buffer.
         * @return true if the command applied successfully (same semantics as
         *         each per-command Execute*() helper).
         */
        bool ApplyCommandAt(std::byte* command)
        {
            auto* header = reinterpret_cast<CommandHeader*>(command);
            std::byte* payloadPtr = command + sizeof(CommandHeader);
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
         * @param command Stable command pointer from CommandKeys(); must belong
         *                to THIS buffer.
         * @param map     This buffer's placeholder->real resolution map.
         * @return true iff the (translated) command applied successfully.
         */
        bool ResolveAndApplyCommandAt(std::byte* command, PlaceholderMap& map)
        {
            auto* header = reinterpret_cast<CommandHeader*>(command);
            std::byte* payloadPtr = command + sizeof(CommandHeader);
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
            m_recordFailed = false;
        }

        /**
         * Reserve space in the command buffer for the expected number of bytes.
         */
        void Reserve(size_t bytes)
        {
            m_buffer.Reserve(bytes);
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
         * Number of storage blocks currently held (diagnostics/tests: the
         * retention contract -- Clear() keeps blocks -- is observable here).
         */
        [[nodiscard]] size_t GetStorageBlockCount() const noexcept { return m_buffer.BlockCount(); }

        /**
         * True iff a record method hit arena exhaustion since the last
         * Clear()/successful Execute() and could NOT write its command (CR-2).
         * A buffer in this state is missing >=1 command: Execute() refuses it
         * with AllocationFailed, and ParallelCommandBuffer::ExecuteSorted()
         * skips applying it wholesale (see those functions). Exposed so the
         * sorted flush -- which only touches CommandBuffer through its public
         * API -- can detect a partially-recorded worker buffer.
         */
        [[nodiscard]] bool HasRecordFailure() const noexcept { return m_recordFailed; }

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
         * Record the {SortKey, command pointer} descriptor for the command
         * whose header was just allocated at commandPtr. Block storage is
         * STABLE (C1 fix), so the raw pointer stays valid until
         * Clear()/destruction -- no offset indirection needed.
         */
        void StampCommand(std::byte* commandPtr)
        {
            SortKey key = m_hasCustomSortKey ? m_currentSortKey : SortKey{0, 0, m_autoSeq};
            ++m_autoSeq;
            m_commandKeys.emplace_back(key, commandPtr);
        }

        /**
         * The SINGLE funnel every record method uses to reach the byte buffer
         * (CR-2 fix). Allocates STABLE space for one command, records its
         * {SortKey, pointer} descriptor (StampCommand), and constructs the
         * command's CommandHeader in place -- returning a pointer to that
         * header.
         *
         * On arena exhaustion (CommandByteBuffer::Allocate returns null -- a
         * batch that exceeds the TLSF request ceiling, or genuine OOM) it sets
         * the sticky m_recordFailed flag and returns nullptr WITHOUT stamping a
         * sort key or constructing anything, so no record site ever
         * placement-news or memcpys into the null page. Every caller MUST check
         * the result and return immediately on nullptr, before touching the
         * (non-existent) payload region. Execute()/ExecuteSorted() then refuse
         * to apply the partially-recorded buffer and surface AllocationFailed.
         *
         * The header's totalSize is stamped with the 16-aligned stride the
         * command actually occupies. That is AlignUp-equivalent to `size`, and
         * the only readers -- the Execute()/CleanupPendingCommands() block
         * walks -- AlignUp it again, so this matches every prior per-site value
         * exactly (some sites stamped `totalSize`, the create/destroy-single
         * sites stamped `alignedSize`; both collapse to the same aligned
         * stride).
         *
         * @param type The command-type tag for the header.
         * @param size Total command size (header + payload [+ inline data]).
         * @return Pointer to the constructed CommandHeader, or nullptr on
         *         allocation failure (m_recordFailed set).
         */
        [[nodiscard]] std::byte* AllocateCommand(CommandType type, size_t size)
        {
            size_t alignedSize = 0;
            std::byte* ptr = m_buffer.Allocate(size, &alignedSize);
            if (!ptr) ASTRA_UNLIKELY
            {
                m_recordFailed = true;
                return nullptr;
            }
            StampCommand(ptr);
            new (ptr) CommandHeader{type, 0, static_cast<uint32_t>(alignedSize)};
            return ptr;
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
                case CommandType::SetEnabled:
                {
                    TranslateEntity(reinterpret_cast<SetEnabledPayload*>(payload)->entity, map);
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
            m_lastBatchFailureCount = 0;

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
                case CommandType::SetEnabled:
                    return ExecuteSetEnabled(payload);
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

        bool ExecuteSetEnabled(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<SetEnabledPayload*>(payload);
            if (cmd->entity == Entity::Invalid())
                return false;

            return m_registry->SetEnabledByID(cmd->entity, cmd->componentId, cmd->enable != 0);
        }

        bool ExecuteAddComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();
            const void* data = cmd->GetDataPtr();

            // Attempt-all, count failures: matches the single-entity
            // executor's failure contract, scaled to N (spec §4). An Invalid
            // slot counts as a failure exactly like the single-entity path.
            size_t failed = 0;
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] == Entity::Invalid() ||
                    !m_registry->AddComponentByID(entities[i], cmd->componentId, data, cmd->dataSize))
                {
                    ++failed;
                }
            }
            m_lastBatchFailureCount = failed;
            return failed == 0;
        }

        bool ExecuteRemoveComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();

            size_t failed = 0;
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] == Entity::Invalid() ||
                    !m_registry->RemoveComponentByID(entities[i], cmd->componentId))
                {
                    ++failed;
                }
            }
            m_lastBatchFailureCount = failed;
            return failed == 0;
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
            // Per-block walk (C1 fix): each command lives wholly within one
            // STABLE block. Destructor thunks run exactly once per recorded
            // command -- once over the whole chain here.
            for (const CommandByteBuffer::Block& block : m_buffer.Blocks())
            {
                std::byte* ptr = block.base;
                std::byte* end = block.base + block.used;
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

                    // Advance by aligned size (commands are stored at ALIGNMENT-byte stride)
                    ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
                }
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

        // CR-2: sticky flag set by AllocateCommand() when the byte buffer's
        // arena is exhausted and a command could NOT be recorded (single
        // command over the ~32MB TLSF request ceiling, or genuine OOM). A set
        // flag means the buffer is INCOMPLETE -- >=1 command silently dropped.
        // Execute() refuses such a buffer with AllocationFailed and never
        // applies its partial contents; ExecuteSorted() skips the whole
        // buffer. Reset by Clear() (and the Execute() abort path).
        bool m_recordFailed = false;

        // Per-entity failure count from the most recently dispatched BATCH
        // command executor (0 for non-batch commands -- reset by
        // ExecuteCommand before every dispatch). Lets ExecuteSorted() report
        // one DeferredCommandError per failed entity instead of one per
        // failed batch.
        size_t m_lastBatchFailureCount = 0;

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
        // The second element is a STABLE command pointer (C1 fix): block storage
        // never moves, so the raw pointer stays valid until Clear()/destruction.
        std::vector<std::pair<SortKey, std::byte*>> m_commandKeys;
        SortKey m_currentSortKey{};
        bool m_hasCustomSortKey = false;
        uint32_t m_autoSeq = 0;  // stamped as recordSequence when no explicit key was set

        // Task 4: this buffer's own deferred-command errors (see ReportError()).
        std::vector<DeferredCommandError> m_reportedErrors;
    };

    /**
     * Thread-safe command buffer that provides per-thread buffers.
     * Commands from all threads are flushed deterministically via ExecuteSorted().
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
         * Execute every recorded command from every thread buffer in
         * deterministic SortKey order rather than physical arrival order.
         *
         * Gathers a {SortKey, CommandBuffer*, command pointer} descriptor for
         * every command across every worker buffer (via CommandBuffer::CommandKeys()),
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
         * ALLOCATION FAILURE (CR-2): distinct from the above logical skips, a
         * worker buffer that could not RECORD a command (arena exhausted) is
         * truncated. Its whole command stream is skipped and the flush returns
         * Err(AllocationFailed) -- see the gather loop below. Every buffer is
         * still cleared regardless.
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
                std::byte* cmd;   // stable command pointer (C1 fix)
            };

            // CR-2: a worker buffer that hit arena exhaustion during recording
            // has an INCOMPLETE command stream (>=1 command silently dropped).
            // Unlike a logical apply failure (skip one command, continue), a
            // truncated buffer must not be applied at all -- its remaining
            // commands could reference entities its dropped commands were
            // meant to create, producing inconsistent state. Skip such a
            // buffer WHOLESALE (it is still Clear()ed below) and surface the
            // condition as AllocationFailed after the flush completes.
            bool anyRecordFailure = false;
            std::vector<Item> items;
            for (auto& b : m_buffers)
            {
                if (b)
                {
                    if (b->HasRecordFailure()) ASTRA_UNLIKELY
                    {
                        anyRecordFailure = true;
                        continue;
                    }
                    for (const auto& [key, cmd] : b->CommandKeys())
                    {
                        items.push_back({key, b.get(), cmd});
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
                if (!it.buf->ResolveAndApplyCommandAt(it.cmd, perBufferMaps[it.buf]))
                {
                    // Task 4: logical failure -- skip and record, do NOT abort
                    // or roll back the flush. See this function's class-level
                    // FAILURE HANDLING comment and DeferredCommandError's doc
                    // comment for why every ApplyCommandAt() false is a
                    // logical (never fatal) failure in this exception-free
                    // build.
                    //
                    // Task 4 channel, scaled: a failed batch reports one error
                    // PER failed entity (same Reason the single-entity op
                    // yields); non-batch failures report exactly one.
                    const size_t failures = std::max<size_t>(size_t(1), it.buf->GetLastBatchFailureCount());
                    for (size_t f = 0; f < failures; ++f)
                    {
                        m_deferredErrors.push_back(
                            DeferredCommandError{it.key.insertionOrder, DeferredCommandError::Reason::InvalidTargetEntity});
                    }
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

            // CR-2: if any worker buffer was truncated by arena exhaustion, the
            // whole flush is reported as AllocationFailed. This is surfaced
            // AFTER draining reported errors and clearing every buffer, so the
            // contract that ExecuteSorted() always leaves buffers empty holds
            // on this path too. Logical per-command skips (Task 4) remain a
            // success surfaced via GetDeferredErrors(); a record-time
            // allocation failure is a distinct, harder failure.
            if (anyRecordFailure)
                return Result<void, CommandBuffer::ExecutionError>::Err(CommandBuffer::ExecutionError::AllocationFailed);

            // Task 4: a flush that skipped some commands is still an overall
            // success -- the skips are surfaced as errors via
            // GetDeferredErrors(), not as a Result failure.
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
