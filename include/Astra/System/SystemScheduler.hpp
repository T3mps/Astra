#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Archetype/Archetype.hpp"  // For MakeComponentMask
#include "../Commands/CommandBuffer.hpp"  // ParallelCommandBuffer (Task 2: owned command sink)
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

        // Class-typed registration for a struct/class context system: a type
        // with operator()(SystemContext&). Mirrors the System<T> overload
        // above (same Result/uniqueness/allocation handling, same optional
        // SystemTraits scan for scheduling), differing only in which
        // execution delegate it populates on the resulting SystemEntry.
        // T is always given explicitly (AddSystem<MySystem>(args...)), so
        // this never competes with the System<T> overload above for the
        // same call: T can satisfy at most one of the two concepts in
        // practice (they require invocability with disjoint argument types).
        template<ContextSystem T, typename... Args>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Args&&... args)
        {
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            const uint64_t typeId = TypeID<T>::Hash();
            if (m_systemIndices.Contains(typeId))
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);

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
                .metadata = metadata,
                .executeContext = [instance](SystemContext& ctx) { (*instance)(ctx); },
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

        // Lambda registration for a void(SystemContext&) context system --
        // e.g. [](Astra::SystemContext& ctx) { ... }. Unlike the LambdaLike
        // overload above, no view/component extraction wrapper is needed:
        // the lambda IS the thunk, stored directly as the executeContext
        // delegate. LambdaLike is amended (System.hpp) to exclude
        // ContextSystem, so a context lambda can never match both overloads.
        template<typename Lambda>
        requires ContextSystem<Lambda>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Lambda&& lambda)
        {
            using SystemType = std::decay_t<Lambda>;
            return AddContextSystemInternal<SystemType>(SystemType(std::forward<Lambda>(lambda)));
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
            // Task 4: this call's deferred-command errors start empty --
            // GetLastDeferredErrors() must never return a PRIOR call's
            // errors, including on the early-return paths below.
            m_lastDeferredErrors.clear();

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

            if (m_needsRebuild)
            {
                BuildExecutionPlan();
            }

            // Lazily (re)bind the owned ParallelCommandBuffer to this call's
            // Registry. CommandBuffer captures Registry* at construction and
            // has no rebind API, so a change of registry between Execute()
            // calls requires a fresh ParallelCommandBuffer; ordinary usage
            // (and every test) calls Execute() repeatedly with the SAME
            // registry, so in practice this allocates once. Since Task 3 (below)
            // flushes and clears m_commandBuffer before every Execute() call
            // returns, a caller switching registries between calls never finds
            // leftover unflushed commands here -- the old buffer is already
            // empty by the time that would matter.
            if (!m_commandBuffer || m_commandBufferRegistry != &registry)
            {
                m_commandBuffer = std::make_unique<ParallelCommandBuffer>(&registry);
                m_commandBufferRegistry = &registry;
            }

            {
                // Scope the ExecutionGuard so m_executionDepth returns to 0
                // BEFORE the flush below runs, making the class's own
                // documented contract literal (see ExecutionGuard's doc
                // comment: "Depth returning to zero is the designated B2
                // command-buffer sync point") rather than relying on the
                // guard being about to destruct at function exit anyway.
                ExecutionGuard guard(m_executionDepth);

                // Build execution context
                SystemExecutionContext context;
                context.registry = &registry;
                context.commandBuffer = m_commandBuffer.get();
                context.parallelGroups = m_executionPlan;
                context.systems.reserve(m_systems.size());
                context.contextSystems.reserve(m_systems.size());
                context.metadata.reserve(m_systems.size());

                for (const auto& entry : m_systems)
                {
                    context.systems.push_back(entry.execute);
                    context.contextSystems.push_back(entry.executeContext);
                    context.metadata.push_back(entry.metadata);
                }

                // Execute via the provided executor
                executor->Execute(context);
            }

            // Task 3: the depth==0 sync point. Every context system has now
            // returned (the ExecutionGuard above just destructed), so no
            // worker is still recording into m_commandBuffer's per-thread
            // buffers -- flush every recorded deferred command, across every
            // worker buffer, in deterministic SortKey order. Determinism
            // holds because SystemContext::Commands() stamps every command
            // with {this system's unique insertionOrder, 0, a per-system
            // monotonic recordSequence} -- see ParallelCommandBuffer::
            // ExecuteSorted()'s documented precondition.
            auto flushResult = m_commandBuffer->ExecuteSorted();
            // Task 4: surface this flush's deferred-command errors (commands
            // skipped because their target entity/component state no longer
            // permitted the op, plus anything reported via SystemContext::
            // ReportError()) through the scheduler's own accessor. ExecuteSorted()
            // itself always returns Ok() now -- a skipped command is reported,
            // not treated as a flush failure -- so flushResult carries no
            // additional information; kept only so a future genuine flush-level
            // failure mode has somewhere to be checked.
            (void)flushResult;
            m_lastDeferredErrors = m_commandBuffer->GetDeferredErrors();

            // Start the next frame's recording from empty regardless of
            // outcome. ExecuteSorted() already clears every worker buffer on
            // both success and failure (see its rollback comment), so this is
            // a defensive no-op today -- kept so "Execute() always leaves the
            // buffer empty" doesn't silently depend on ExecuteSorted's
            // internals never changing.
            m_commandBuffer->Clear();
        }

        /**
         * Total number of deferred commands recorded across every context
         * system's per-worker CommandBuffer that have not yet been flushed.
         * Since Task 3, Execute() flushes and clears m_commandBuffer before
         * it returns, so this is 0 between Execute() calls; it is only
         * meaningfully non-zero while inspected from inside a system mid-
         * Execute(). Exposed so callers/tests can observe that a
         * void(SystemContext&) system actually recorded something without
         * reaching into the owned ParallelCommandBuffer directly.
         */
        ASTRA_NODISCARD size_t PendingCommandCount() const
        {
            return m_commandBuffer ? m_commandBuffer->GetCommandCount() : 0;
        }

        /**
         * Deferred-command errors from the most recent Execute() call's
         * flush (Task 4): commands skipped because their target entity/
         * component state no longer permitted the op (e.g. an earlier
         * system's deferred command destroyed the entity first), plus
         * anything a system explicitly reported via SystemContext::
         * ReportError(). Cleared at the start of every Execute() call --
         * empty before the first call and after any Execute() that flushed
         * cleanly.
         *
         * Ordering note: only the skip-path (flush) errors are deterministically
         * ordered (gathered in sort-key order); explicit ReportError() entries
         * are gathered in worker-buffer-slot order, which is NOT deterministic.
         */
        ASTRA_NODISCARD const std::vector<DeferredCommandError>& GetLastDeferredErrors() const noexcept
        {
            return m_lastDeferredErrors;
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
            // Stale errors from a since-cleared set of systems must not
            // outlive the scheduler state that produced them.
            m_lastDeferredErrors.clear();

            // M2 (Task 2 review fix): Clear() used to leave any pending-but-
            // unflushed deferred commands sitting in m_commandBuffer, so a
            // later Execute() with a completely different set of systems
            // could still flush stale commands recorded by systems Clear()
            // just removed. Drop them here too, consistent with the Task 3
            // flush lifecycle (every Execute() call starts and ends with an
            // empty buffer; Clear() must not be the one path that leaves it
            // dirty). m_commandBuffer may still be null if Execute() was
            // never called on this scheduler.
            if (m_commandBuffer)
            {
                m_commandBuffer->Clear();
            }
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

            // Execution delegate for void(SystemContext&) systems (Task 2).
            // Empty (default-constructed -> falsy) for ordinary void(Registry&)
            // systems and view-lambda systems, which instead populate `execute`
            // above; exactly one of the two delegates is non-empty per entry.
            Delegate<void(SystemContext&)> executeContext{};
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

        // Registers a lambda-typed void(SystemContext&) system directly (no
        // view/component-extraction wrapper -- unlike AddSystemInternal
        // above, SystemType IS the thunk here). Mirrors AddSystemInternal's
        // Result/uniqueness/allocation handling; SystemType is a raw lambda
        // closure type, so it has no SystemTraits to scan (see the
        // ContextSystem-lambda AddSystem overload for why that's fine: no
        // traits => BuildExecutionPlan gives it a safe solo group).
        template<typename SystemType>
        ASTRA_NODISCARD Result<void, SystemError> AddContextSystemInternal(SystemType system)
        {
            // Uniform-graceful misuse policy (decision 2026-07-13): see
            // AddSystemInternal above.
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            // See AddSystemInternal above re: TypeID::Hash() collisions.
            const uint64_t typeId = TypeID<SystemType>::Hash();
            if (m_systemIndices.Contains(typeId))
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);

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

            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<SystemType*>(ptr); }),
                .metadata = metadata,
                .executeContext = [instance](SystemContext& ctx) { (*instance)(ctx); },
            });

            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }

        std::vector<SystemEntry> m_systems;                             // All registered systems
        FlatMap<uint64_t, size_t> m_systemIndices;                      // key: TypeID<T>::Hash() — systems must not consume dense ComponentIDs
        mutable std::vector<std::vector<size_t>> m_executionPlan;       // Cached parallel groups
        mutable bool m_needsRebuild = true;                             // Whether execution plan needs rebuild
        mutable std::atomic<int> m_executionDepth{0};                   // reentrancy-safe; ==0 is the B2 sync point

        // Task 2: owned per-worker deferred-command sink, lazily (re)bound to
        // whichever Registry Execute() is called with (see Execute() above).
        std::unique_ptr<ParallelCommandBuffer> m_commandBuffer;
        Registry* m_commandBufferRegistry = nullptr;                    // registry m_commandBuffer is currently bound to

        // Task 4: this scheduler's per-system error channel -- see
        // GetLastDeferredErrors().
        std::vector<DeferredCommandError> m_lastDeferredErrors;
    };
} // namespace Astra
