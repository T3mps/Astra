#pragma once

#include <memory>
#include <vector>

#include "../Core/WorkScheduler.hpp"
#include "SystemMetadata.hpp"

#ifdef ASTRA_BUILD_DEBUG
    #include "../Registry/Registry.hpp"          // GetArchetypeManager() (Debug tripwire only)
#endif

namespace Astra
{
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
                    context.systems[systemIdx](*context.registry);
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
                        context.systems[systemIdx](*context.registry);
                }
                else
                {
#ifdef ASTRA_BUILD_DEBUG
                    const uint32_t structuralBefore =
                        context.registry->GetArchetypeManager()->GetStructuralChangeCounter();
#endif
                    // Dispatch each system in the group as its own unit of work.
                    m_scheduler->ParallelFor(group.size(), 1, [&](size_t begin, size_t end, uint32_t /*worker*/)
                    {
                        for (size_t i = begin; i < end; ++i)
                            context.systems[group[i]](*context.registry);
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
