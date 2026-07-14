#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Archetype/Archetype.hpp"  // For MakeComponentMask
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/Delegate.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Registry/Registry.hpp"
#include "System.hpp"
#include "SystemExecutor.hpp"
#include "SystemMetadata.hpp"

namespace Astra
{
    enum class SystemError
    {
        AlreadyRegistered,  // a system of this type is already registered
        AllocationFailed,   // nothrow allocation of the system instance failed
        SchedulerExecuting  // registration attempted while Execute() is running
    };

    class SystemScheduler
    {
    public:
        // RAII depth counter for execution. NOT a lock: it does not provide
        // mutual exclusion against external threads. Registration (Add/Remove/
        // Clear) follows the same single-writer contract as the Registry — it
        // must not race Execute. The counter exists to (a) make the one
        // practical mistake safe (a system, on a worker, calling Remove/Add
        // mid-frame no-ops) and (b) stay truthful under reentrant Execute.
        // Depth returning to zero is the designated B2 command-buffer sync point.
        class ExecutionGuard
        {
        public:
            explicit ExecutionGuard(std::atomic<int>& depth) : m_depth(depth)
            {
                m_depth.fetch_add(1, std::memory_order_acq_rel);
            }
            ~ExecutionGuard()
            {
                m_depth.fetch_sub(1, std::memory_order_acq_rel);
            }
            ExecutionGuard(const ExecutionGuard&) = delete;
            ExecutionGuard& operator=(const ExecutionGuard&) = delete;
        private:
            std::atomic<int>& m_depth;
        };

        // Check if scheduler is currently executing (cannot be modified during execution)
        ASTRA_NODISCARD bool IsExecuting() const noexcept
        {
            return m_executionDepth.load(std::memory_order_acquire) > 0;
        }

        template<System T, typename... Args>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Args&&... args)
        {
            // Uniform-graceful misuse policy (decision 2026-07-13): NO
            // ASTRA_ASSERT here — the Result channel below IS the contract, so
            // asserting-and-aborting on the same condition would make the error
            // unreachable/untestable in Debug. Return the typed error instead.
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            // Systems are keyed by TypeID::Hash() (64-bit). A hash collision
            // would make a DISTINCT type look already-registered and be dropped;
            // astronomically unlikely, but it is a hash, not a dense unique id.
            const uint64_t typeId = TypeID<T>::Hash();
            if (m_systemIndices.Contains(typeId))
            {
                // No ASTRA_ASSERT — duplicate registration is a handleable
                // runtime error (uniform-graceful policy, decision 2026-07-13).
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);
            }

            T* instance = new (std::nothrow) T(std::forward<Args>(args)...);
            if (!instance)
                return Result<void, SystemError>::Err(SystemError::AllocationFailed);

            const size_t index = m_systems.size();
            m_systemIndices[typeId] = index;

            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = static_cast<size_t>(typeId),
                .insertionOrder = index,
                .requiresExclusive = false
            };
            if constexpr (HasSystemTraits_v<T>)
                ExtractSystemTraits<T>(metadata);
            if constexpr (requires { T::RequiresExclusive; })
                metadata.requiresExclusive = T::RequiresExclusive;

            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<T*>(ptr); }),
                .execute = [instance](Registry& reg) { (*instance)(reg); },
                .metadata = metadata
            });

            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }

        template<typename Lambda>
        requires LambdaLike<Lambda>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Lambda&& lambda)
        {
            return AddLambdaSystemImpl(std::forward<Lambda>(lambda), &std::decay_t<Lambda>::operator());
        }

        template<System T>
        void RemoveSystem()
        {
            // Prevent modification during execution to avoid use-after-free.
            // No assert here (unlike AddSystem): a system calling RemoveSystem
            // on itself mid-Execute is the one practical mistake the guard
            // makes safe, so this must no-op gracefully rather than abort.
            if (IsExecuting()) return;

            uint64_t typeId = TypeID<T>::Hash();
            auto it = m_systemIndices.Find(typeId);
            if (it == m_systemIndices.end())
                return;
            
            size_t index = it->second;
            m_systems.erase(m_systems.begin() + index);
            m_systemIndices.Erase(it);
            
            // Update indices for systems after the removed one
            for (auto& [tid, idx] : m_systemIndices)
            {
                if (idx > index)
                {
                    --idx;
                }
            }

            // Keep insertionOrder consistent with vector position after erase
            // (it is exposed via SystemExecutionContext.metadata).
            for (size_t idx = 0; idx < m_systems.size(); ++idx)
                m_systems[idx].metadata.insertionOrder = idx;

            m_needsRebuild = true;
        }
        
        template<System T>
        ASTRA_NODISCARD bool HasSystem() const
        {
            return m_systemIndices.Contains(TypeID<T>::Hash());
        }

        void Execute(Registry& registry)
        {
            static SequentialExecutor defaultExecutor;
            Execute(registry, &defaultExecutor);
        }
        
        void Execute(Registry& registry, ISystemExecutor* executor)
        {
            if (!ASTRA_ENSURE(executor != nullptr, "Executor cannot be null"))
                return;

            if (m_systems.empty())
                return;

            // Acquire execution lock - prevents modification during parallel execution
            // This prevents use-after-free when systems are removed while executing
            ASTRA_ASSERT(m_executionDepth.load(std::memory_order_acquire) == 0,
                "Reentrant SystemScheduler::Execute is unsupported; if a system must "
                "re-run systems, do it from an Astra::Exclusive system. (The depth "
                "counter keeps this safe, but nesting is almost always a design error.)");
            ExecutionGuard guard(m_executionDepth);

            if (m_needsRebuild)
            {
                BuildExecutionPlan();
            }

            // Build execution context
            SystemExecutionContext context;
            context.registry = &registry;
            context.parallelGroups = m_executionPlan;
            context.systems.reserve(m_systems.size());
            context.metadata.reserve(m_systems.size());

            for (const auto& entry : m_systems)
            {
                context.systems.push_back(entry.execute);
                context.metadata.push_back(entry.metadata);
            }

            // Execute via the provided executor
            executor->Execute(context);
        }
        
        void Clear()
        {
            // Prevent modification during execution to avoid use-after-free.
            // No assert here (unlike AddSystem): see RemoveSystem's note above.
            if (IsExecuting()) return;

            m_systems.clear();
            m_systemIndices.Clear();
            m_executionPlan.clear();
            m_needsRebuild = true;
        }
        
        ASTRA_NODISCARD size_t Size() const noexcept
        {
            return m_systems.size();
        }
        
        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_systems.empty();
        }
        
        ASTRA_NODISCARD const std::vector<std::vector<size_t>>& GetExecutionPlan() const
        {
            if (m_needsRebuild)
            {
                const_cast<SystemScheduler*>(this)->BuildExecutionPlan();
            }
            return m_executionPlan;
        }
        
    private:
        struct SystemEntry
        {
            std::unique_ptr<void, void(*)(void*)> instance;  // Type-erased system instance
            Delegate<void(Registry&)> execute;               // Execution delegate (more efficient than std::function)
            SystemMetadata metadata;                         // System metadata
        };

        template<typename T>
        void ExtractSystemTraits(SystemMetadata& metadata)
        {
            if constexpr (HasSystemTraits_v<T>)
            {
                ExtractComponentMask<typename T::ReadsComponents>(metadata.reads);
                ExtractComponentMask<typename T::WritesComponents>(metadata.writes);
            }
        }

        template<typename Tuple>
        void ExtractComponentMask(ComponentMask& mask)
        {
            ExtractComponentMaskImpl<Tuple>(mask, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }
        
        template<typename Tuple, size_t... Is>
        void ExtractComponentMaskImpl(ComponentMask& mask, std::index_sequence<Is...>)
        {
            ((mask |= MakeComponentMask<std::tuple_element_t<Is, Tuple>>()), ...);
        }
        
        // Partition systems into sequential groups of concurrently-runnable
        // systems. The plan is a set of CONTIGUOUS insertion-order runs: a run
        // grows from its opener until the first system that conflicts (mask
        // overlap), is Exclusive, or declares no traits. This keeps Sequential
        // and Parallel executors in identical observable order and never lets a
        // later system's effects appear before an earlier system's (I3). O(n).
        void BuildExecutionPlan()
        {
            m_executionPlan.clear();
            if (m_systems.empty())
            {
                m_needsRebuild = false;
                return;
            }

            size_t i = 0;
            while (i < m_systems.size())
            {
                const auto& sysI = m_systems[i].metadata;

                std::vector<size_t> group;
                group.push_back(i);
                ComponentMask groupReads = sysI.reads;
                ComponentMask groupWrites = sysI.writes;

                // A solo opener (Exclusive, or no declared hints) accepts nobody.
                const bool acceptsMore = !sysI.requiresExclusive
                                      && !(sysI.reads.None() && sysI.writes.None());

                size_t j = i + 1;
                for (; acceptsMore && j < m_systems.size(); ++j)
                {
                    const auto& sysJ = m_systems[j].metadata;

                    // Exclusive / no-trait systems never join an existing group,
                    // and any conflict ends the contiguous run (order preserved).
                    if (sysJ.requiresExclusive || (sysJ.reads.None() && sysJ.writes.None()))
                        break;
                    if ((sysJ.writes & groupWrites).Any() ||
                        (sysJ.writes & groupReads ).Any() ||
                        (sysJ.reads  & groupWrites).Any())
                        break;

                    group.push_back(j);
                    groupReads  |= sysJ.reads;
                    groupWrites |= sysJ.writes;
                }

                m_executionPlan.push_back(std::move(group));
                i = j;  // next group starts right after this contiguous run
            }

            m_needsRebuild = false;
        }

        // Helper to extract signature from const lambda
        template<typename Lambda, typename Ret, typename Class, typename... Args>
        ASTRA_NODISCARD Result<void, SystemError> AddLambdaSystemImpl(Lambda&& lambda, Ret(Class::*)(Args...) const)
        {
            using Wrapper = LambdaSystemWrapper<std::decay_t<Lambda>, Args...>;
            return AddSystemInternal<Wrapper>(Wrapper{std::forward<Lambda>(lambda)});
        }

        // Helper to extract signature from non-const lambda
        template<typename Lambda, typename Ret, typename Class, typename... Args>
        ASTRA_NODISCARD Result<void, SystemError> AddLambdaSystemImpl(Lambda&& lambda, Ret(Class::*)(Args...))
        {
            using Wrapper = LambdaSystemWrapper<std::decay_t<Lambda>, Args...>;
            return AddSystemInternal<Wrapper>(Wrapper{std::forward<Lambda>(lambda)});
        }

        template<typename SystemType>
        ASTRA_NODISCARD Result<void, SystemError> AddSystemInternal(SystemType system)
        {
            // Uniform-graceful misuse policy (decision 2026-07-13): NO
            // ASTRA_ASSERT here — the Result channel below IS the contract, so
            // asserting-and-aborting on the same condition would make the error
            // unreachable/untestable in Debug. Return the typed error instead.
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            // Systems are keyed by TypeID::Hash() (64-bit). A hash collision
            // would make a DISTINCT type look already-registered and be dropped;
            // astronomically unlikely, but it is a hash, not a dense unique id.
            const uint64_t typeId = TypeID<SystemType>::Hash();
            if (m_systemIndices.Contains(typeId))
            {
                // No ASTRA_ASSERT — duplicate registration is a handleable
                // runtime error (uniform-graceful policy, decision 2026-07-13).
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);
            }

            SystemType* instance = new (std::nothrow) SystemType(std::move(system));
            if (!instance)
                return Result<void, SystemError>::Err(SystemError::AllocationFailed);

            const size_t index = m_systems.size();
            m_systemIndices[typeId] = index;

            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = static_cast<size_t>(typeId),
                .insertionOrder = index,
                .requiresExclusive = false
            };
            if constexpr (HasSystemTraits_v<SystemType>)
                ExtractSystemTraits<SystemType>(metadata);
            if constexpr (requires { SystemType::RequiresExclusive; })
                metadata.requiresExclusive = SystemType::RequiresExclusive;

            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<SystemType*>(ptr); }),
                .execute = [instance](Registry& reg) { (*instance)(reg); },
                .metadata = metadata
            });

            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }
        
        std::vector<SystemEntry> m_systems;                             // All registered systems
        FlatMap<uint64_t, size_t> m_systemIndices;                      // key: TypeID<T>::Hash() — systems must not consume dense ComponentIDs
        mutable std::vector<std::vector<size_t>> m_executionPlan;       // Cached parallel groups
        mutable bool m_needsRebuild = true;                             // Whether execution plan needs rebuild
        mutable std::atomic<int> m_executionDepth{0};                   // reentrancy-safe; ==0 is the B2 sync point
    };
} // namespace Astra
