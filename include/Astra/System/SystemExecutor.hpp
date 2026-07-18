#pragma once

#include <memory>
#include <vector>

#include "../Commands/CommandBuffer.hpp"  // ParallelCommandBuffer::GetThreadBuffer()
#include "../Core/WorkScheduler.hpp"
#include "SystemContext.hpp"
#include "SystemMetadata.hpp"

#ifdef ASTRA_BUILD_DEBUG
    #include "../Registry/Registry.hpp"          // GetArchetypeManager() (Debug tripwire only)
#endif

namespace Astra
{
    /**
     * Dispatches system `systemIdx` from `context`: for a void(SystemContext&)
     * system (Task 2 -- contextSystems[systemIdx] is non-empty), builds a
     * SystemContext wrapping *context.registry, THIS call's per-worker
     * CommandBuffer, the system's insertionOrder, iterationIndex 0, and the
     * owning ParallelCommandBuffer* (Phase B, Task 2 -- read only by
     * ParallelForEach, Task 3; unused here), then invokes it; otherwise
     * invokes the ordinary void(Registry&) delegate as before.
     *
     * Shared by Sequential/ParallelExecutor so both dispatch identically.
     *
     * CALLER CONTRACT: call this from the thread that should own the
     * resulting per-worker CommandBuffer -- GetThreadBuffer() is called
     * INSIDE this function, so calling it from a worker thread (e.g. from
     * inside an IWorkScheduler::ParallelFor job lambda) makes that worker
     * record into its own buffer; calling it from the submitting thread
     * makes the submitting thread own the recording.
     */
    inline void DispatchSystem(const SystemExecutionContext& context, size_t systemIdx)
    {
        if (context.contextSystems[systemIdx])
        {
            SystemContext sysCtx(*context.registry,
                context.commandBuffer->GetThreadBuffer(),
                static_cast<uint32_t>(context.metadata[systemIdx].insertionOrder),
                0u, context.commandBuffer);
            context.contextSystems[systemIdx](sysCtx);
        }
        else
        {
            context.systems[systemIdx](*context.registry);
        }
    }

    class ISystemExecutor
    {
    public:
        virtual ~ISystemExecutor() = default;
        virtual void Execute(const SystemExecutionContext& context) = 0;
    };

    struct SequentialExecutor : public ISystemExecutor
    {
        void Execute(const SystemExecutionContext& context) override
        {
            for (const auto& group : context.parallelGroups)
            {
                for (size_t systemIdx : group)
                {
                    DispatchSystem(context, systemIdx);
                }
            }
        }
    };

    struct ParallelExecutor : public ISystemExecutor
    {
        ParallelExecutor() = default;  // no scheduler => sequential execution
        explicit ParallelExecutor(std::shared_ptr<IWorkScheduler> scheduler) :
            m_scheduler(std::move(scheduler))
        {}

        void Execute(const SystemExecutionContext& context) override
        {
            for (const auto& group : context.parallelGroups)
            {
                if (group.size() == 1 || !m_scheduler)
                {
                    // Single system or no scheduler: run sequentially to avoid overhead
                    for (size_t systemIdx : group)
                        DispatchSystem(context, systemIdx);
                }
                else
                {
#ifdef ASTRA_BUILD_DEBUG
                    const uint32_t structuralBefore =
                        context.registry->GetArchetypeManager()->GetStructuralChangeCounter();
#endif
                    // Dispatch each system in the group as its own unit of work.
                    // DispatchSystem is called INSIDE this worker lambda (not
                    // hoisted out) so that for a context system, GetThreadBuffer()
                    // runs on the worker thread actually executing it -- each
                    // concurrent worker records into its OWN per-thread
                    // CommandBuffer (Task 2 scope: recording only; flush/Clear
                    // of the resulting ParallelCommandBuffer is Task 3).
                    m_scheduler->ParallelFor(group.size(), 1, [&](size_t begin, size_t end, uint32_t /*worker*/)
                    {
                        for (size_t i = begin; i < end; ++i)
                            DispatchSystem(context, group[i]);
                    });
#ifdef ASTRA_BUILD_DEBUG
                    const uint32_t structuralAfter =
                        context.registry->GetArchetypeManager()->GetStructuralChangeCounter();
                    ASTRA_ASSERT(structuralBefore == structuralAfter,
                        "A system in a multi-member parallel group changed the archetype "
                        "set (created a new archetype, or triggered defragmentation) "
                        "without declaring Astra::Exclusive. Mark it Exclusive, or defer "
                        "the change via a CommandBuffer. (Structural mutation races the "
                        "archetype storage against the other systems in the group. This "
                        "tripwire is best-effort: it catches archetype-set changes, not "
                        "entity add/remove within an existing archetype.)");
#endif
                }
            }
        }

    private:
        std::shared_ptr<IWorkScheduler> m_scheduler;
    };
} // namespace Astra
