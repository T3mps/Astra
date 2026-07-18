#pragma once

#include <concepts>
#include <cstdint>
#include <utility>

#include "../Registry/Registry.hpp"
#include "../Commands/CommandBuffer.hpp"

namespace Astra
{
    /**
     * @brief Lightweight per-system handle for systems registered with the
     * additive void(SystemContext&) signature (Theme B2 Phase A, Task 2).
     *
     * Bundles read/query access to the Registry with the recording system's
     * per-worker deferred-command recorder (a CommandBuffer -- typically a
     * ParallelCommandBuffer's per-thread buffer, see SystemScheduler /
     * SystemExecutor). Structural changes recorded via Commands() are NOT
     * applied immediately; they are applied later by a deterministic flush
     * (Theme B2 Task 3), sorted by SortKey.
     *
     * SortKey stamping: every command recorded through Commands() is stamped
     * with {insertionOrder, iterationIndex, recordSequence++} via
     * CommandBuffer's sticky SetNextSortKey (Task 1) before the caller
     * records anything, so the Nth command this system records gets
     * recordSequence N-1 -- preserving this system's exact record order once
     * Task 3's ExecuteSorted() flush is wired in. iterationIndex is 0 for an
     * ordinary system context (the 3-arg ctor below); a chunk sub-context
     * built by ParallelForEach (Theme B2 Phase B, Task 3) stamps its own
     * chunk index instead, via the full 5-arg ctor.
     */
    class SystemContext
    {
    public:
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder) noexcept
            : SystemContext(reg, cmds, insertionOrder, 0u, nullptr) {}

        /**
         * Full ctor (Theme B2 Phase B, Task 2): builds a sub-context for one
         * chunk of a ParallelForEach dispatch (Task 3), stamping every
         * command it records with the given iterationIndex (the chunk index)
         * instead of the Phase A default of 0. `parallelBuffer` is a nullable,
         * additive handle to the owning ParallelCommandBuffer -- unused by
         * Commands()/ReportError() here, read only by ParallelForEach (Task 3)
         * via GetParallelBuffer().
         */
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder,
                      uint32_t iterationIndex, ParallelCommandBuffer* parallelBuffer) noexcept
            : m_registry(reg), m_commands(cmds), m_insertionOrder(insertionOrder),
              m_iterationIndex(iterationIndex), m_parallelBuffer(parallelBuffer) {}

        [[nodiscard]] Registry& GetRegistry() const noexcept { return m_registry; }

        /**
         * The per-worker deferred-command recorder for this system. Every
         * call re-stamps the buffer's sticky SortKey with this system's
         * insertionOrder and the next recordSequence, so ANY command the
         * caller records immediately after (DestroyEntity, AddComponent,
         * ...) carries this call's key -- see class docs above.
         *
         * Thread-safety note: CommandBuffer::AddComponent<T> registers T
         * with the Registry's ComponentRegistry at record time, which is
         * NOT thread-safe. Ensure every component type is registered (via a
         * main-thread AddComponent/CreateView/RegisterComponent, or by an
         * existing entity carrying it) before running systems that may
         * first-add DIFFERENT component types concurrently from Commands().
         */
        CommandBuffer& Commands() noexcept
        {
            m_commands.SetNextSortKey(SortKey{m_insertionOrder, m_iterationIndex, m_recordSequence++});
            return m_commands;
        }

        /**
         * Report a deferred-command error attributed to THIS system, via its
         * insertionOrder (the same attribution ExecuteSorted() uses when it
         * skips a command whose target entity/component state no longer
         * permits the op -- see DeferredCommandError's doc comment).
         *
         * Pushes into this system's OWN per-worker CommandBuffer (same-
         * thread write, exactly like Commands()' recording), so no
         * synchronization is needed even when many systems on different
         * worker threads call this concurrently on their own buffers.
         * Gathered after the flush via SystemScheduler::GetLastDeferredErrors().
         */
        void ReportError(DeferredCommandError::Reason reason)
        {
            m_commands.ReportError(DeferredCommandError{m_insertionOrder, reason});
        }

        /**
         * The chunk sub-context's owning ParallelCommandBuffer, or nullptr
         * for a Phase A (non-chunked) context. Additive handle for Task 3's
         * ParallelForEach; unused by Commands()/ReportError() above.
         */
        [[nodiscard]] ParallelCommandBuffer* GetParallelBuffer() const noexcept { return m_parallelBuffer; }

        /**
         * @brief Fan `view` out across worker threads by chunk (Theme B2 Phase
         * B, Task 3), handing each chunk a per-chunk sub-context so its body can
         * defer structural changes that land deterministically at the flush.
         *
         * Invocable as `func(Entity, Components&..., SystemContext& sub)`: for
         * every entity in a chunk, `func` receives that chunk's own sub-context,
         * whose Commands() stamp the chunk's iterationIndex (the flat chunkWork
         * index -- globally unique across the view, so keys never collide across
         * chunks even though each sub-context restarts its recordSequence at 0).
         *
         * The factory runs ON the worker executing the chunk, so
         * GetThreadBuffer() is called there: each worker records into its OWN
         * per-thread CommandBuffer (Phase A's worker-thread rule). When this
         * context has no parallel buffer (m_parallelBuffer == nullptr -- e.g. a
         * standalone context), every sub-context falls back to this context's
         * own immediate CommandBuffer; determinism still holds because the
         * stamped iterationIndex is still the flat chunk index.
         *
         * See View::ParallelForEachWithContext for the chunk-split mechanics and
         * the flat-index determinism argument in full.
         */
        template<typename ViewT, typename Func>
        void ParallelForEach(ViewT& view, Func&& func)
        {
            Registry& reg = m_registry;
            const uint32_t insertionOrder = m_insertionOrder;
            ParallelCommandBuffer* pcb = m_parallelBuffer;
            CommandBuffer& immediate = m_commands;  // fallback when pcb == nullptr
            view.ParallelForEachWithContext(
                [&reg, insertionOrder, pcb, &immediate](uint32_t iterationIndex)
                {
                    // Called ON the chunk-worker thread: GetThreadBuffer() picks
                    // that worker's own per-thread buffer.
                    CommandBuffer& buf = pcb ? pcb->GetThreadBuffer() : immediate;
                    return SystemContext(reg, buf, insertionOrder, iterationIndex, pcb);
                },
                std::forward<Func>(func));
        }

    private:
        Registry& m_registry;
        CommandBuffer& m_commands;
        uint32_t m_insertionOrder;
        uint32_t m_iterationIndex = 0;
        ParallelCommandBuffer* m_parallelBuffer = nullptr;
        uint32_t m_recordSequence = 0;
    };

    /**
     * @brief A "context system": a callable invocable as void(SystemContext&).
     *
     * Distinct from System<T> (invocable as void(Registry&)) and from
     * LambdaLike<T> (a view-lambda: void(Entity, Components&...) run over a
     * View). LambdaLike is amended in System.hpp to explicitly exclude
     * ContextSystem, because a void(SystemContext&) callable has an
     * operator() and is NOT invocable with Registry& -- which is exactly
     * LambdaLike's pre-Task-2 test -- so without the exclusion it would be
     * misrouted into the view-lambda ExtractAndExecute path and fail to
     * compile (it expects (Entity, Components...), not (SystemContext&)).
     * See System.hpp for the full explanation.
     */
    template<typename T>
    concept ContextSystem = requires(T system, SystemContext& ctx)
    {
        { system(ctx) } -> std::same_as<void>;
    };
} // namespace Astra
