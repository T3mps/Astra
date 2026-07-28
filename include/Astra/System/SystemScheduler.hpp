#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Archetype/Archetype.hpp"  // For MakeComponentMask
#include "../Commands/CommandBuffer.hpp"  // ParallelCommandBuffer (Task 2: owned command sink)
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/Delegate.hpp"
#include "../Core/Log.hpp"
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
        SchedulerExecuting, // registration attempted while Execute() is running
        OrderingCycle       // a Before/After cycle was detected at plan build
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
        // A deferred-command flush happens at EVERY SyncPoint segment boundary
        // (Phase E); depth returning to zero is only the LAST such flush point.
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
                .requiresExclusive = false,
                .segmentIndex = m_currentSegment
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
                .requiresExclusive = false,
                .segmentIndex = m_currentSegment
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

        // Param-function system: a lambda/functor whose params are
        // View<...>&/Res<T>/ResMut<T>/Commands. Access is derived from the
        // params (design §5). Deduces the param pack off operator().
        template<typename Fn>
        requires ParamFunctor<Fn>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Fn&& fn)
        {
            return AddParamSystemImpl(std::forward<Fn>(fn), &std::decay_t<Fn>::operator());
        }

        // Param-function system registered as a free function (non-type template
        // argument), so two same-signature free functions get distinct wrapper
        // types. Deduces the param pack off decltype(FnPtr).
        template<auto FnPtr>
        requires Detail::IsParamFreeFunction_v<decltype(FnPtr)>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem()
        {
            return AddFreeFnParamSystemImpl<FnPtr>(FnPtr);
        }

        // Insert a sync-point fence at the current end of the registration
        // sequence (Phase E). Systems registered after it are in a later segment
        // and never reorder/group across it; all deferred structural commands
        // recorded before the fence are applied before the next segment runs.
        ASTRA_NODISCARD Result<void, SystemError> AddSyncPoint()
        {
            return AddSyncPoint(std::string_view{});
        }

        ASTRA_NODISCARD Result<void, SystemError> AddSyncPoint(std::string_view label)
        {
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);
            ++m_currentSegment;
            m_fenceLabels.emplace_back(label);   // owns a copy; empty for the no-arg form
            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }

        // Number of segments = number of SyncPoint fences + 1.
        ASTRA_NODISCARD size_t GetSegmentCount() const noexcept { return m_currentSegment + 1; }

        // IM-25: constrained by System<T> || ContextSystem<T> so a pure
        // context system (invocable only with SystemContext&, which fails the
        // System<T> concept) can be removed symmetrically with how it was
        // added. The body keys purely on TypeID<T>::Hash(), so either concept
        // is sufficient; both AddSystem overloads store under the same key.
        template<typename T>
        requires (System<T> || ContextSystem<T>)
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

        // Symmetric with AddSystem<FnPtr>() / HasSystem<FnPtr>() (free-function
        // param-system registration, task 5): FnPtr is a non-type template
        // argument (a function, not a type), so it cannot go through the
        // template<typename T> overload above. Reconstruct the identical
        // FreeFunctionSystemWrapper via RemoveFreeFnParamSystemImpl and erase it
        // by that key, using the exact same erase/reindex logic as the
        // type-based RemoveSystem<T>() above (including the same "no assert,
        // graceful no-op mid-Execute" policy).
        template<auto FnPtr>
        requires Detail::IsParamFreeFunction_v<decltype(FnPtr)>
        void RemoveSystem()
        {
            if (IsExecuting()) return;
            RemoveFreeFnParamSystemImpl<FnPtr>(FnPtr);
        }

        // IM-25: see RemoveSystem above -- relaxed to accept a context system
        // (invocable only with SystemContext&) so add/remove/has are symmetric.
        template<typename T>
        requires (System<T> || ContextSystem<T>)
        ASTRA_NODISCARD bool HasSystem() const
        {
            return m_systemIndices.Contains(TypeID<T>::Hash());
        }

        // Symmetric with AddSystem<FnPtr>() (free-function param-system
        // registration, task 5): FnPtr is a non-type template argument (a
        // function, not a type), so it cannot go through the template<typename T>
        // overload above. Reconstruct the identical FreeFunctionSystemWrapper via
        // HasFreeFnParamSystemImpl and key-check it the same way.
        template<auto FnPtr>
        requires Detail::IsParamFreeFunction_v<decltype(FnPtr)>
        ASTRA_NODISCARD bool HasSystem() const
        {
            return HasFreeFnParamSystemImpl<FnPtr>(FnPtr);
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
            // IM-22 made m_context shared member state, so a reentrant Execute() would
            // clobber the outer call's context mid-iteration in Release (where the assert
            // above is compiled out). Reentrancy is unsupported regardless -- refuse it
            // gracefully in all configs (a no-op, per the project's graceful-misuse policy)
            // rather than corrupt the in-flight context.
            if (m_executionDepth.load(std::memory_order_acquire) != 0) ASTRA_UNLIKELY
                return;

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
                // The ExecutionGuard now wraps the WHOLE segment loop below,
                // not just a single executor->Execute() call: with Phase E
                // sync points, a "frame" is potentially several run+flush
                // pairs, and the reentrancy guard must stay armed for all of
                // them (a system on segment 1 must not be able to kick off a
                // nested Execute() any more than one on segment 0 could).
                // Each per-segment flush below therefore runs at depth==1,
                // not depth==0 -- that is still safe, because a flush only
                // ever happens immediately after THAT segment's
                // executor->Execute(context) has RETURNED, i.e. every system
                // in that segment has completed and no worker is still
                // recording into m_commandBuffer's per-thread buffers. See
                // Task 3's original comment (now folded into the loop below)
                // for why that makes the flush safe.
                ExecutionGuard guard(m_executionDepth);

                // IM-22: the frame-invariant parts of the context
                // (systems/contextSystems/metadata) are cached in m_context and
                // rebuilt only when the plan is rebuilt (RebuildContextCache,
                // called from BuildExecutionPlan just above). Here we bind only
                // this call's frame-varying state; the per-segment group slice is
                // selected in the loop below. This avoids copying n Delegates + n
                // SystemMetadata (each carrying std::vectors) on every Execute()
                // -- none of which depends on frame-varying state.
                m_context.registry = &registry;
                m_context.commandBuffer = m_commandBuffer.get();

                // Run the plan segment-by-segment. Groups are emitted in
                // segment-major order (Task 1), so groups sharing a segment
                // are contiguous in m_executionPlan; m_planGroupSegment[g] is
                // group g's segment. Every segment boundary is a sync point:
                // flush all deferred structural commands recorded by that
                // segment's systems, in deterministic SortKey order, before
                // the next segment runs -- so a segment-N+1 system sees
                // entities/components a segment-N system deferred-created
                // the SAME frame (spawn-then-process). With no SyncPoint
                // there is exactly one segment, so this loop runs once and
                // flushes once at the end, byte-identical to the pre-Phase-E
                // behavior; the final segment's flush is still the last
                // thing that happens before Execute() returns.
                size_t g = 0;
                while (g < m_executionPlan.size())
                {
                    const size_t seg = m_planGroupSegment[g];
                    size_t gEnd = g;
                    while (gEnd < m_executionPlan.size() && m_planGroupSegment[gEnd] == seg)
                        ++gEnd;

                    m_context.parallelGroups.assign(m_executionPlan.begin() + g,
                                                    m_executionPlan.begin() + gEnd);

                    // Execute via the provided executor
                    executor->Execute(m_context);

                    // Task 3/Task 4, per segment: executor->Execute() above
                    // has just returned, so every system in this segment has
                    // completed and no worker is still recording into
                    // m_commandBuffer's per-thread buffers -- flush every
                    // recorded deferred command, across every worker buffer,
                    // in deterministic SortKey order. Determinism holds
                    // because SystemContext::Commands() stamps every command
                    // with {this system's unique scheduleOrder (==
                    // insertionOrder absent Before/After edges), 0, a
                    // per-system monotonic recordSequence} -- see
                    // ParallelCommandBuffer::ExecuteSorted()'s documented
                    // precondition.
                    auto flushResult = m_commandBuffer->ExecuteSorted();
                    // CR-2: ExecuteSorted() now returns Err(AllocationFailed) when a worker
                    // buffer was truncated by a record-time allocation failure (a single
                    // command exceeding the command arena's ~32MB ceiling, or OOM). The
                    // buffer is skipped wholesale so state stays consistent, but the dropped
                    // structural changes must not vanish silently -- surface it in ALL build
                    // configs (the Debug-only Allocate() assert compiles out in Release). A
                    // programmatic failure channel is a follow-up (the uniform
                    // failure-reporting convention noted for IM-24).
                    if (flushResult.IsErr()) ASTRA_UNLIKELY
                    {
                        ASTRA_LOG_ERROR("SystemScheduler::Execute: a deferred command buffer was "
                            "dropped due to a record-time allocation failure; those structural "
                            "changes were NOT applied this frame.");
                    }

                    // Surface this segment's deferred-command errors (commands
                    // skipped because their target entity/component state no
                    // longer permitted the op, plus anything reported via
                    // SystemContext::ReportError()) through the scheduler's
                    // own accessor. ExecuteSorted() clears its error list at
                    // the START of every call and GetDeferredErrors() returns
                    // only the most recent call's errors, so this ACCUMULATES
                    // each segment's errors onto m_lastDeferredErrors without
                    // double-counting a prior segment's.
                    const auto& segErrors = m_commandBuffer->GetDeferredErrors();
                    m_lastDeferredErrors.insert(m_lastDeferredErrors.end(),
                                                segErrors.begin(), segErrors.end());

                    // Start the next segment's recording from empty
                    // regardless of outcome. ExecuteSorted() already clears
                    // every worker buffer on both success and failure (see
                    // its rollback comment), so this is a defensive no-op
                    // today -- kept so "each segment leaves the buffer empty
                    // before the next one runs" doesn't silently depend on
                    // ExecuteSorted's internals never changing.
                    m_commandBuffer->Clear();

                    // Task 3: if the fence that just flushed this segment was
                    // labeled, surface that label in a Debug-level log line.
                    // Fence `seg` sits between segment `seg` and `seg + 1`, so
                    // it's the fence THIS flush corresponds to; m_fenceLabels.
                    // size() == m_currentSegment, so `seg < m_fenceLabels.
                    // size()` also means "this was not the last segment" (the
                    // last segment has no following fence). ASTRA_LOG_DEBUG is
                    // off at the default Info level -> zero production cost.
                    //
                    // Final-review M2: the whole block (including the message
                    // construction) is behind this compile-time floor check, not
                    // just the ASTRA_LOG_DEBUG call, so a labeled fence does not
                    // heap-allocate `msg` on every flush in Release/Dist -- there
                    // the macro compiles to nothing, so building `msg` first and
                    // then handing it to a no-op call was pure waste.
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_DEBUG
                    if (seg < m_fenceLabels.size() && !m_fenceLabels[seg].empty())
                    {
                        std::string msg = "SystemScheduler: sync point '" + m_fenceLabels[seg]
                                        + "' flushed segment " + std::to_string(seg);
                        ASTRA_LOG_DEBUG(msg);
                    }
#endif

                    g = gEnd;
                }

                // If the plan is empty (no systems, or only empty segments),
                // there is nothing to run or flush -- matches the old
                // early-out behavior for an empty schedule.
            }
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
            m_currentSegment = 0;
            m_fenceLabels.clear();
            m_planGroupSegment.clear();
            // Stale errors from a since-cleared set of systems must not
            // outlive the scheduler state that produced them.
            m_lastDeferredErrors.clear();

            // IM-22: drop the cached context. Its systems/contextSystems
            // Delegates captured raw `instance` pointers that the m_systems
            // clear above just deleted -- they must not linger (a later
            // Execute() early-returns on the now-empty m_systems, but keep the
            // cache from holding dangling delegate copies regardless). A
            // subsequent AddSystem sets m_needsRebuild, so RebuildContextCache
            // repopulates before the next Execute() uses it.
            m_context.systems.clear();
            m_context.contextSystems.clear();
            m_context.metadata.clear();
            m_context.parallelGroups.clear();
            m_context.registry = nullptr;
            m_context.commandBuffer = nullptr;

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

        // Forces a plan build if needed, then reports whether the last build hit
        // a Before/After cycle. Ok() means the declared ordering is acyclic.
        // (A cycle is still handled gracefully -- Execute() runs with a
        // deterministic fallback order -- this is the explicit programmatic check.)
        ASTRA_NODISCARD Result<void, SystemError> ValidateSchedule()
        {
            if (m_needsRebuild)
                BuildExecutionPlan();
            if (m_scheduleHadCycle)
                return Result<void, SystemError>::Err(SystemError::OrderingCycle);
            return Result<void, SystemError>::Ok();
        }

        // Opt-in (default off): after each plan build, report every pair of
        // systems that mutably conflict (share a component or resource with a
        // write on either side) whose relative order is fixed only by an
        // insertion-order accident -- no Before/After edge (direct or transitive)
        // orders them and neither declares AmbiguousWith the other. Reported via
        // ASTRA_LOG_WARN; a development aid, never an error.
        void SetAmbiguityReporting(bool enabled) noexcept { m_reportAmbiguities = enabled; }

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

                if constexpr (requires { typename T::ReadsResourceTypes; typename T::WritesResourceTypes; })
                {
                    ExtractResourceReadMask<typename T::ReadsResourceTypes>(metadata.resourceReads, metadata.resourceWrites);
                    // Resource writes always route to resourceWrites regardless of ConcurrentReadSafe (only reads are folded).
                    ExtractComponentMask<typename T::WritesResourceTypes>(metadata.resourceWrites);
                }

                if constexpr (requires { typename T::BeforeTypes; typename T::AfterTypes; typename T::AmbiguousWithTypes; })
                {
                    ExtractSystemIdList<typename T::BeforeTypes>(metadata.beforeIds);
                    ExtractSystemIdList<typename T::AfterTypes>(metadata.afterIds);
                    ExtractSystemIdList<typename T::AmbiguousWithTypes>(metadata.ambiguousWithIds);
                }
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

        // Push TypeID<Each>::Hash() for each system type in the tuple into `out`
        // (the same 64-bit key m_systemIndices uses to look systems up).
        template<typename Tuple>
        void ExtractSystemIdList(std::vector<uint64_t>& out)
        {
            ExtractSystemIdListImpl<Tuple>(out, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        template<typename Tuple, size_t... Is>
        void ExtractSystemIdListImpl(std::vector<uint64_t>& out, std::index_sequence<Is...>)
        {
            ((out.push_back(TypeID<std::tuple_element_t<Is, Tuple>>::Hash())), ...);
        }

        // For each read resource R: a ConcurrentReadSafe resource sets its bit in
        // `reads`; a non-safe one folds into `writes` so the existing write-involved
        // conflict predicate serializes even two readers.
        template<typename Tuple>
        void ExtractResourceReadMask(ComponentMask& reads, ComponentMask& writes)
        {
            ExtractResourceReadMaskImpl<Tuple>(reads, writes, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        template<typename Tuple, size_t... Is>
        void ExtractResourceReadMaskImpl(ComponentMask& reads, ComponentMask& writes, std::index_sequence<Is...>)
        {
            ([&]
            {
                using R = std::tuple_element_t<Is, Tuple>;
                if constexpr (ResourceTraits<R>::ConcurrentReadSafe)
                    reads |= MakeComponentMask<R>();
                else
                    writes |= MakeComponentMask<R>();
            }(), ...);
        }

        // Stable topological order over the Before/After edges. Honors every
        // resolved edge; among unconstrained systems preserves insertion order
        // (m_systems is stored in registration order, so index == insertionOrder,
        // and picking the lowest ready index is the insertion-order tiebreak).
        // A cycle cannot be ordered -- it is broken deterministically by forcing
        // the lowest-index unplaced system, so this ALWAYS terminates with a
        // total order (m_scheduleHadCycle records that a break happened).
        std::vector<size_t> ComputeScheduleOrder()
        {
            m_scheduleHadCycle = false;
            m_cycleMembers.clear();
            const size_t n = m_systems.size();
            const size_t UNKNOWN = n;

            auto resolve = [&](uint64_t hash) -> size_t
            {
                auto it = m_systemIndices.Find(hash);
                return it == m_systemIndices.end() ? UNKNOWN : it->second;
            };

            // predecessor -> successor adjacency (predecessor runs first) + in-degrees.
            std::vector<std::vector<size_t>> succ(n);
            std::vector<size_t> indeg(n, 0);
            for (size_t s = 0; s < n; ++s)
            {
                const auto& md = m_systems[s].metadata;
                for (uint64_t h : md.afterIds)   // After<T> on s: T runs before s => T -> s
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s
                        && m_systems[t].metadata.segmentIndex == md.segmentIndex)
                    { succ[t].push_back(s); ++indeg[s]; }
                }
                for (uint64_t h : md.beforeIds)  // Before<T> on s: s runs before T => s -> T
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s
                        && m_systems[t].metadata.segmentIndex == md.segmentIndex)
                    { succ[s].push_back(t); ++indeg[t]; }
                }
            }

            std::vector<size_t> order;
            order.reserve(n);
            std::vector<bool> placed(n, false);
            while (order.size() < n)
            {
                size_t pick = UNKNOWN;
                for (size_t k = 0; k < n; ++k)
                    if (!placed[k] && indeg[k] == 0) { pick = k; break; }  // lowest ready index
                if (pick == UNKNOWN)
                {
                    // Cycle: no ready node but systems remain. Force the lowest
                    // unplaced index (deterministic), recording it for Task 3.
                    m_scheduleHadCycle = true;
                    for (size_t k = 0; k < n; ++k)
                        if (!placed[k]) { pick = k; break; }
                    m_cycleMembers.push_back(m_systems[pick].metadata.insertionOrder);
                }
                placed[pick] = true;
                order.push_back(pick);
                for (size_t nx : succ[pick])
                    if (indeg[nx] > 0) --indeg[nx];
            }
            return order;
        }

        // True if there is a DIRECT ordering edge predIdx -> succIdx, i.e. succ
        // declares After<pred> or pred declares Before<succ>. (Systems are keyed
        // by TypeID::Hash(), stored in metadata.typeId.) Direct edges suffice for
        // the grouping barrier: because the plan is grouped over the topological
        // order in contiguous runs, any transitive predecessor sits in an earlier,
        // already-closed group, so it can never be a current-group member.
        ASTRA_NODISCARD bool IsOrderingPredecessor(size_t predIdx, size_t succIdx) const
        {
            const auto& pred = m_systems[predIdx].metadata;
            const auto& succ = m_systems[succIdx].metadata;
            const uint64_t predHash = static_cast<uint64_t>(pred.typeId);
            const uint64_t succHash = static_cast<uint64_t>(succ.typeId);
            for (uint64_t h : succ.afterIds)  if (h == predHash) return true;
            for (uint64_t h : pred.beforeIds) if (h == succHash) return true;
            return false;
        }

        // Pairwise mutable conflict: share a component OR resource with a write on
        // either side. (Same semantics as the grouping conflict test, in pairwise
        // form.) Returns the first conflicting component bit in `outComp` (or
        // MAX_COMPONENTS if the conflict is resource-only), and likewise the first
        // resource bit in `outRes`, for the report.
        ASTRA_NODISCARD static bool Conflicts(const SystemMetadata& a, const SystemMetadata& b,
                                              size_t& outComp, size_t& outRes)
        {
            const ComponentMask comp = (a.writes & b.writes) | (a.writes & b.reads) | (a.reads & b.writes);
            const ComponentMask res  = (a.resourceWrites & b.resourceWrites)
                                     | (a.resourceWrites & b.resourceReads)
                                     | (a.resourceReads  & b.resourceWrites);
            outComp = MAX_COMPONENTS;
            outRes  = MAX_COMPONENTS;
            for (size_t bit = 0; bit < MAX_COMPONENTS; ++bit)
            {
                if (outComp == MAX_COMPONENTS && comp.Test(bit)) outComp = bit;
                if (outRes  == MAX_COMPONENTS && res.Test(bit))  outRes  = bit;
            }
            return comp.Any() || res.Any();
        }

        // Reachability over the resolved ordering DAG: can `from` reach `to` by
        // following Before/After edges (transitively)? Used to decide whether a
        // conflicting pair is already ordered. n is small (tens); a per-query DFS
        // is fine.
        ASTRA_NODISCARD bool OrderingReaches(size_t from, size_t to,
                                             const std::vector<std::vector<size_t>>& succ) const
        {
            std::vector<bool> seen(succ.size(), false);
            std::vector<size_t> stack{from};
            while (!stack.empty())
            {
                size_t cur = stack.back(); stack.pop_back();
                if (cur == to) return true;
                if (seen[cur]) continue;
                seen[cur] = true;
                for (size_t nx : succ[cur]) stack.push_back(nx);
            }
            return false;
        }

        // True if a declares AmbiguousWith b, or b declares AmbiguousWith a.
        ASTRA_NODISCARD bool SuppressedAsAmbiguous(size_t aIdx, size_t bIdx) const
        {
            const auto& a = m_systems[aIdx].metadata;
            const auto& b = m_systems[bIdx].metadata;
            const uint64_t aHash = static_cast<uint64_t>(a.typeId);
            const uint64_t bHash = static_cast<uint64_t>(b.typeId);
            for (uint64_t h : a.ambiguousWithIds) if (h == bHash) return true;
            for (uint64_t h : b.ambiguousWithIds) if (h == aHash) return true;
            return false;
        }

        void ReportAmbiguities()
        {
            const size_t n = m_systems.size();
            const size_t UNKNOWN = n;
            auto resolve = [&](uint64_t hash) -> size_t
            {
                auto it = m_systemIndices.Find(hash);
                return it == m_systemIndices.end() ? UNKNOWN : it->second;
            };
            // Rebuild the successor adjacency (same convention as ComputeScheduleOrder,
            // INCLUDING its same-segment filter (Phase E/final-review I1): a Before/After
            // edge whose endpoints sit in different segments is never followed by the
            // scheduler either, so an edge like that must not manufacture a transitive
            // ordering path here -- otherwise this function could believe a same-segment
            // conflicting pair is "ordered" via a path that only exists by bouncing
            // through another segment (false negative).
            std::vector<std::vector<size_t>> succ(n);
            for (size_t s = 0; s < n; ++s)
            {
                const auto& md = m_systems[s].metadata;
                for (uint64_t h : md.afterIds)  { size_t t = resolve(h); if (t != UNKNOWN && t != s && m_systems[t].metadata.segmentIndex == md.segmentIndex) succ[t].push_back(s); }
                for (uint64_t h : md.beforeIds) { size_t t = resolve(h); if (t != UNKNOWN && t != s && m_systems[t].metadata.segmentIndex == md.segmentIndex) succ[s].push_back(t); }
            }
            for (size_t a = 0; a < n; ++a)
                for (size_t b = a + 1; b < n; ++b)
                {
                    // Systems in different segments are deterministically ordered by the
                    // SyncPoint fence between them -- never ambiguous.
                    if (m_systems[a].metadata.segmentIndex != m_systems[b].metadata.segmentIndex) continue;
                    size_t comp = 0, res = 0;
                    if (!Conflicts(m_systems[a].metadata, m_systems[b].metadata, comp, res)) continue;
                    if (OrderingReaches(a, b, succ) || OrderingReaches(b, a, succ)) continue;   // ordered
                    if (SuppressedAsAmbiguous(a, b)) continue;                                  // opted out
                    std::string msg = "SystemScheduler: ambiguous system order -- systems (insertionOrder) "
                        + std::to_string(m_systems[a].metadata.insertionOrder) + " and "
                        + std::to_string(m_systems[b].metadata.insertionOrder)
                        + " mutably conflict but declare no relative order.";
                    if (comp != MAX_COMPONENTS) msg += " component id " + std::to_string(comp) + '.';
                    if (res  != MAX_COMPONENTS) msg += " resource id "  + std::to_string(res)  + '.';
                    msg += " Add Before/After, or AmbiguousWith to silence.";
                    ASTRA_LOG_WARN(msg);
                }
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
            m_planGroupSegment.clear();
            if (m_systems.empty())
            {
                RebuildContextCache();   // IM-22: clears the cached context (no systems)
                m_needsRebuild = false;
                return;
            }

            // A system participates in grouping only if it declares SOME access.
            // A system with no declared component OR resource access runs solo
            // (unchanged for component-only systems; extended to cover resources
            // so a resource-only system is groupable rather than forced solo).
            const auto declaresAccess = [](const SystemMetadata& m)
            {
                return !(m.reads.None() && m.writes.None()
                      && m.resourceReads.None() && m.resourceWrites.None());
            };

            const std::vector<size_t> order = ComputeScheduleOrder();
            for (size_t k = 0; k < order.size(); ++k)
                m_systems[order[k]].metadata.scheduleOrder = k;

            size_t p = 0;
            while (p < order.size())
            {
                const size_t iIdx = order[p];
                const auto& sysI = m_systems[iIdx].metadata;

                std::vector<size_t> group;
                group.push_back(iIdx);
                ComponentMask groupReads = sysI.reads;
                ComponentMask groupWrites = sysI.writes;
                ComponentMask groupResourceReads = sysI.resourceReads;
                ComponentMask groupResourceWrites = sysI.resourceWrites;

                // A solo opener (Exclusive, or no declared hints) accepts nobody.
                const bool acceptsMore = !sysI.requiresExclusive && declaresAccess(sysI);

                size_t q = p + 1;
                for (; acceptsMore && q < order.size(); ++q)
                {
                    const size_t jIdx = order[q];
                    const auto& sysJ = m_systems[jIdx].metadata;

                    if (sysJ.segmentIndex != sysI.segmentIndex)  // never group across a fence
                        break;
                    // Exclusive / no-trait systems never join an existing group,
                    // and any conflict ends the contiguous run (order preserved).
                    if (sysJ.requiresExclusive || !declaresAccess(sysJ))
                        break;
                    if ((sysJ.writes & groupWrites).Any() ||
                        (sysJ.writes & groupReads ).Any() ||
                        (sysJ.reads  & groupWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceReads ).Any() ||
                        (sysJ.resourceReads  & groupResourceWrites).Any())
                        break;
                    // Edge-as-barrier: an explicit ordering edge into sysJ from any
                    // current-group member forces sysJ into a later group, even with
                    // disjoint masks (the edge demands serialization).
                    {
                        bool blockedByEdge = false;
                        for (size_t member : group)
                            if (IsOrderingPredecessor(member, jIdx)) { blockedByEdge = true; break; }
                        if (blockedByEdge)
                            break;
                    }

                    group.push_back(jIdx);
                    groupReads  |= sysJ.reads;
                    groupWrites |= sysJ.writes;
                    groupResourceReads  |= sysJ.resourceReads;
                    groupResourceWrites |= sysJ.resourceWrites;
                }

                m_planGroupSegment.push_back(sysI.segmentIndex);
                m_executionPlan.push_back(std::move(group));
                p = q;
            }

            if (m_scheduleHadCycle)
            {
                std::string msg = "SystemScheduler: Before/After ordering cycle detected and broken "
                                  "deterministically (insertion-order fallback). Systems forced during "
                                  "the break (by insertionOrder):";
                for (size_t io : m_cycleMembers)
                    msg += ' ' + std::to_string(io);
                ASTRA_LOG_ERROR(msg);
            }

            // IM-22: refresh the frame-invariant context cache AFTER
            // ComputeScheduleOrder has stamped metadata.scheduleOrder above, so
            // the cached metadata carries the current schedule order.
            RebuildContextCache();

            m_needsRebuild = false;

            if (m_reportAmbiguities)
                ReportAmbiguities();
        }

        // IM-22: rebuild the frame-invariant part of the shared execution
        // context (systems/contextSystems/metadata) from m_systems. Called from
        // BuildExecutionPlan on every plan rebuild, so Execute() only sets the
        // frame-varying bits (registry/commandBuffer/parallelGroups). MUST run
        // after ComputeScheduleOrder has stamped metadata.scheduleOrder. Reuses
        // the vectors' capacity across rebuilds (clear keeps capacity). When
        // m_systems is empty this simply clears the cache.
        void RebuildContextCache()
        {
            m_context.systems.clear();
            m_context.contextSystems.clear();
            m_context.metadata.clear();
            m_context.systems.reserve(m_systems.size());
            m_context.contextSystems.reserve(m_systems.size());
            m_context.metadata.reserve(m_systems.size());
            for (const auto& entry : m_systems)
            {
                m_context.systems.push_back(entry.execute);
                m_context.contextSystems.push_back(entry.executeContext);
                m_context.metadata.push_back(entry.metadata);
            }
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

        // Deduce Params from a const operator() (the common lambda case).
        // Param wrappers are context systems (they need the per-worker
        // CommandBuffer for a Commands param), so they register through the
        // refactored AddContextSystemInternal below -- which now harvests
        // SystemTraits via the shared RegisterSystemImpl (a wrapper HAS traits;
        // a raw context lambda does not, so it stays a no-op there). No separate
        // param internal (PF1).
        template<typename Fn, typename Ret, typename Class, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddParamSystemImpl(Fn&& fn, Ret(Class::*)(Params...) const)
        {
            using Wrapper = FunctionSystemWrapper<std::decay_t<Fn>, Params...>;
            return AddContextSystemInternal<Wrapper>(Wrapper{std::forward<Fn>(fn)});
        }
        // Deduce Params from a non-const (mutable) operator().
        template<typename Fn, typename Ret, typename Class, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddParamSystemImpl(Fn&& fn, Ret(Class::*)(Params...))
        {
            using Wrapper = FunctionSystemWrapper<std::decay_t<Fn>, Params...>;
            return AddContextSystemInternal<Wrapper>(Wrapper{std::forward<Fn>(fn)});
        }
        // Deduce Params from the free-function pointer type; build the NTTP wrapper.
        template<auto FnPtr, typename Ret, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddFreeFnParamSystemImpl(Ret(*)(Params...))
        {
            using Wrapper = FreeFunctionSystemWrapper<FnPtr, Params...>;
            return AddContextSystemInternal<Wrapper>(Wrapper{});
        }
        // Symmetric with AddFreeFnParamSystemImpl above: reconstruct the same
        // FreeFunctionSystemWrapper<FnPtr, Params...> type from decltype(FnPtr)
        // to compute the identical TypeID::Hash() key for HasSystem<FnPtr>().
        template<auto FnPtr, typename Ret, typename... Params>
        ASTRA_NODISCARD bool HasFreeFnParamSystemImpl(Ret(*)(Params...)) const
        {
            using Wrapper = FreeFunctionSystemWrapper<FnPtr, Params...>;
            return m_systemIndices.Contains(TypeID<Wrapper>::Hash());
        }
        // Symmetric with HasFreeFnParamSystemImpl above: reconstruct the same
        // FreeFunctionSystemWrapper<FnPtr, Params...> type from decltype(FnPtr)
        // to compute the identical TypeID::Hash() key, then erase it using the
        // exact same erase/reindex logic as the type-based RemoveSystem<T>()
        // (public section above): drop the entry, drop the index, shift every
        // later index down by one, and re-stamp insertionOrder so it stays
        // consistent with vector position (metadata is exposed via
        // SystemExecutionContext). Caller (RemoveSystem<FnPtr>()) already
        // guarded IsExecuting().
        template<auto FnPtr, typename Ret, typename... Params>
        void RemoveFreeFnParamSystemImpl(Ret(*)(Params...))
        {
            using Wrapper = FreeFunctionSystemWrapper<FnPtr, Params...>;
            uint64_t typeId = TypeID<Wrapper>::Hash();
            auto it = m_systemIndices.Find(typeId);
            if (it == m_systemIndices.end())
                return;

            size_t index = it->second;
            m_systems.erase(m_systems.begin() + index);
            m_systemIndices.Erase(it);

            for (auto& [tid, idx] : m_systemIndices)
            {
                if (idx > index)
                {
                    --idx;
                }
            }

            for (size_t idx = 0; idx < m_systems.size(); ++idx)
                m_systems[idx].metadata.insertionOrder = idx;

            m_needsRebuild = true;
        }

        // Shared registration core (PF1): duplicate-key/allocation/metadata/
        // trait-harvest/exclusive handling in ONE place. `makeEntry(instance,
        // metadata)` is the only per-caller variation -- it builds the SystemEntry
        // with the right execution delegate (execute vs executeContext). The
        // `if constexpr HasSystemTraits_v` + RequiresExclusive scans are correct for
        // ALL callers: a plain view-lambda wrapper / param wrapper HAS traits; a raw
        // void(SystemContext&) closure has neither member, so both `if constexpr`
        // arms are skipped -- byte-identical to the pre-refactor behavior.
        template<typename SystemType, typename MakeEntry>
        ASTRA_NODISCARD Result<void, SystemError> RegisterSystemImpl(SystemType system, MakeEntry makeEntry)
        {
            // Uniform-graceful misuse policy (decision 2026-07-13): NO ASTRA_ASSERT
            // here -- the Result channel below IS the contract.
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            // Systems are keyed by TypeID::Hash() (64-bit). A collision would make a
            // DISTINCT type look already-registered; astronomically unlikely, but it
            // is a hash, not a dense unique id.
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
                .requiresExclusive = false,
                .segmentIndex = m_currentSegment
            };
            if constexpr (HasSystemTraits_v<SystemType>)
                ExtractSystemTraits<SystemType>(metadata);
            if constexpr (requires { SystemType::RequiresExclusive; })
                metadata.requiresExclusive = SystemType::RequiresExclusive;

            m_systems.emplace_back(makeEntry(instance, std::move(metadata)));
            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }

        // void(Registry&) systems (traditional + view-lambda wrappers): execute delegate.
        template<typename SystemType>
        ASTRA_NODISCARD Result<void, SystemError> AddSystemInternal(SystemType system)
        {
            return RegisterSystemImpl<SystemType>(std::move(system),
                [](SystemType* instance, SystemMetadata metadata)
                {
                    return SystemEntry
                    {
                        .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                            [](void* ptr) { delete static_cast<SystemType*>(ptr); }),
                        .execute = [instance](Registry& reg) { (*instance)(reg); },
                        .metadata = std::move(metadata)
                    };
                });
        }

        // void(SystemContext&) systems (raw context lambdas AND param wrappers):
        // executeContext delegate. Trait harvest happens in RegisterSystemImpl --
        // inert for a trait-less closure, active for a param wrapper.
        template<typename SystemType>
        ASTRA_NODISCARD Result<void, SystemError> AddContextSystemInternal(SystemType system)
        {
            return RegisterSystemImpl<SystemType>(std::move(system),
                [](SystemType* instance, SystemMetadata metadata)
                {
                    return SystemEntry
                    {
                        .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                            [](void* ptr) { delete static_cast<SystemType*>(ptr); }),
                        .metadata = std::move(metadata),
                        .executeContext = [instance](SystemContext& ctx) { (*instance)(ctx); }
                    };
                });
        }

        std::vector<SystemEntry> m_systems;                             // All registered systems
        FlatMap<uint64_t, size_t> m_systemIndices;                      // key: TypeID<T>::Hash() — systems must not consume dense ComponentIDs
        bool m_scheduleHadCycle = false;            // set by ComputeScheduleOrder; surfaced in Task 3
        std::vector<size_t> m_cycleMembers;         // insertionOrders forced during a cycle break (Task 3 log)
        bool m_reportAmbiguities = false;  // opt-in ambiguity reporting (Phase D §12)
        mutable std::vector<std::vector<size_t>> m_executionPlan;       // Cached parallel groups
        mutable bool m_needsRebuild = true;                             // Whether execution plan needs rebuild
        mutable std::atomic<int> m_executionDepth{0};                   // reentrancy-safe; each segment boundary is a sync point, ==0 is the last

        size_t m_currentSegment = 0;                 // segment stamped onto systems registered now (Phase E)
        std::vector<std::string> m_fenceLabels;      // label of each fence i (between segment i and i+1); "" if unlabeled
        mutable std::vector<size_t> m_planGroupSegment;  // segment index of each group in m_executionPlan (parallel)

        // Task 2: owned per-worker deferred-command sink, lazily (re)bound to
        // whichever Registry Execute() is called with (see Execute() above).
        std::unique_ptr<ParallelCommandBuffer> m_commandBuffer;
        Registry* m_commandBufferRegistry = nullptr;                    // registry m_commandBuffer is currently bound to

        // Task 4: this scheduler's per-system error channel -- see
        // GetLastDeferredErrors().
        std::vector<DeferredCommandError> m_lastDeferredErrors;

        // IM-22: frame-invariant execution context reused across Execute()
        // calls. Its stable parts (systems/contextSystems/metadata) are rebuilt
        // only when the plan is rebuilt (RebuildContextCache, from
        // BuildExecutionPlan); Execute() sets registry/commandBuffer and assigns
        // the per-segment group slice into parallelGroups. mutable to match
        // m_executionPlan: BuildExecutionPlan is reachable from const
        // GetExecutionPlan()/ValidateSchedule() via const_cast.
        mutable SystemExecutionContext m_context;
    };
} // namespace Astra
