#pragma once

#include <memory>
#include <vector>

#include "../Core/WorkScheduler.hpp"
#include "SystemMetadata.hpp"

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
                    // Dispatch each system in the group as its own unit of work.
                    // minBatch=1 so the scheduler may assign one system per worker.
                    m_scheduler->ParallelFor(group.size(), 1, [&](size_t begin, size_t end)
                    {
                        for (size_t i = begin; i < end; ++i)
                            context.systems[group[i]](*context.registry);
                    });
                }
            }
        }

    private:
        std::shared_ptr<IWorkScheduler> m_scheduler;
    };
} // namespace Astra
