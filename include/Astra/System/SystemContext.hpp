#pragma once

#include <concepts>
#include <cstdint>

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
     * with {insertionOrder, 0, recordSequence++} via CommandBuffer's sticky
     * SetNextSortKey (Task 1) before the caller records anything, so the
     * Nth command this system records gets recordSequence N-1 -- preserving
     * this system's exact record order once Task 3's ExecuteSorted() flush
     * is wired in.
     */
    class SystemContext
    {
    public:
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder) noexcept
            : m_registry(reg), m_commands(cmds), m_insertionOrder(insertionOrder) {}

        [[nodiscard]] Registry& GetRegistry() const noexcept { return m_registry; }

        /**
         * The per-worker deferred-command recorder for this system. Every
         * call re-stamps the buffer's sticky SortKey with this system's
         * insertionOrder and the next recordSequence, so ANY command the
         * caller records immediately after (DestroyEntity, AddComponent,
         * ...) carries this call's key -- see class docs above.
         */
        CommandBuffer& Commands() noexcept
        {
            m_commands.SetNextSortKey(SortKey{m_insertionOrder, 0u, m_recordSequence++});
            return m_commands;
        }

        // ReportError(SystemError) is Task 4's error sink -- deliberately not
        // added here (YAGNI: Task 2 only needs recording, not reporting).

    private:
        Registry& m_registry;
        CommandBuffer& m_commands;
        uint32_t m_insertionOrder;
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
