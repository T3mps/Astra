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
        /**
         * NOTE: the `insertionOrder` parameter name is historical -- callers
         * now conventionally pass the caller's schedule-order sort rank (see
         * Commands() below); renaming the parameter is a tracked follow-up.
         */
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
         * schedule-order sort rank (its scheduleOrder, which equals
         * insertionOrder absent Before/After edges) and the next
         * recordSequence, so ANY command the caller records immediately
         * after (DestroyEntity, AddComponent, ...) carries this call's key
         * -- see class docs above.
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
         * whose Commands() stamp the chunk's iterationIndex (a per-scope band
         * base plus the flat chunkWork index -- globally unique across the view,
         * so keys never collide across chunks even though each sub-context
         * restarts its recordSequence at 0).
         *
         * ITERATIONINDEX BANDING (why keys stay globally unique within ONE
         * system): a deferred-command SortKey is {insertionOrder, iterationIndex,
         * recordSequence}, and the depth==0 flush relies on these keys being
         * globally unique for a deterministic apply order (equal keys fall back
         * to scheduling-dependent buffer-slot order). This system's own
         * Commands() (the outer context, m_iterationIndex == 0) and each chunk of
         * each ParallelForEach share this system's single insertionOrder, so the
         * iterationIndex must separate them. iterationIndex 0 is RESERVED for the
         * outer context's own Commands(); the FIRST ParallelForEach takes the
         * disjoint band [1, 1+chunkCount), the next call the band immediately
         * after that, and so on (m_nextIterationBase advances by the chunk count
         * each call). Thus the outer Commands() plus any number of SEQUENTIAL
         * ParallelForEach calls on this context all record globally-unique keys
         * in every build config -- not just the single-ParallelForEach-and-no-
         * outer-Commands() shape the Task 4 gate happens to exercise.
         *
         * LIMITATION (documented, NOT fixed here): a NESTED ParallelForEach
         * called FROM WITHIN a chunk body (on the `sub` sub-context) is NOT made
         * band-disjoint. A chunk sub-context is a fresh SystemContext with its
         * own m_nextIterationBase == 1 but the SAME insertionOrder as this
         * system, so the inner call's bands would collide with THIS call's band.
         * A chunk body must record via `sub.Commands()`, not call
         * `sub.ParallelForEach`.
         *
         * The factory runs ON the worker executing the chunk, so
         * GetThreadBuffer() is called there: each worker records into its OWN
         * per-thread CommandBuffer (Phase A's worker-thread rule). When this
         * context has no parallel buffer (m_parallelBuffer == nullptr -- e.g. a
         * standalone context), every sub-context falls back to this context's
         * own immediate CommandBuffer; determinism still holds because the
         * stamped iterationIndex (band base + flat chunk index) is still unique.
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
            // Capture this call's band base BEFORE dispatch; the factory stamps
            // base + w so this call's chunk keys can't collide with the outer
            // context's Commands() (band 0) or with an earlier ParallelForEach's
            // band. See the ITERATIONINDEX BANDING note above.
            const uint32_t base = m_nextIterationBase;
            const size_t chunkCount = view.ParallelForEachWithContext(
                [&reg, insertionOrder, pcb, &immediate, base](uint32_t w)
                {
                    // Called ON the chunk-worker thread: GetThreadBuffer() picks
                    // that worker's own per-thread buffer.
                    CommandBuffer& buf = pcb ? pcb->GetThreadBuffer() : immediate;
                    return SystemContext(reg, buf, insertionOrder, base + w, pcb);
                },
                std::forward<Func>(func));
            // Reserve [base, base + chunkCount) for THIS call; the next
            // ParallelForEach on this context starts after it.
            m_nextIterationBase = base + static_cast<uint32_t>(chunkCount);
        }

    private:
        Registry& m_registry;
        CommandBuffer& m_commands;
        uint32_t m_insertionOrder;
        uint32_t m_iterationIndex = 0;
        ParallelCommandBuffer* m_parallelBuffer = nullptr;
        uint32_t m_recordSequence = 0;
        // iterationIndex 0 is reserved for THIS context's own Commands(); each
        // ParallelForEach call reserves the next disjoint band [base, base +
        // chunkCount) so deferred-command SortKeys stay globally unique across
        // the outer Commands() and any number of sequential ParallelForEach
        // calls under this system. See ParallelForEach's BANDING note.
        uint32_t m_nextIterationBase = 1;
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
