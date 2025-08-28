#pragma once

#include <future>
#include <vector>

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
        void Execute(const SystemExecutionContext& context) override
        {
            for (const auto& group : context.parallelGroups)
            {
                if (group.size() == 1)
                {
                    // Single system in group - run directly to avoid overhead
                    context.systems[group[0]](*context.registry);
                }
                else
                {
                    // Multiple systems can run in parallel
                    std::vector<std::future<void>> futures;
                    futures.reserve(group.size());
                    
                    for (size_t systemIdx : group)
                    {
                        futures.push_back(std::async(std::launch::async,
                            [&context, systemIdx]()
                            {
                                context.systems[systemIdx](*context.registry);
                            }));
                    }
                    
                    for (auto& future : futures)
                    {
                        future.wait();
                    }
                }
            }
        }
    };
} // namespace Astra
