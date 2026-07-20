#pragma once

#include <cstdint>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/Bitmap.hpp"
#include "../Core/Base.hpp"
#include "../Core/Delegate.hpp"
#include "../Core/TypeID.hpp"

namespace Astra
{
    class Registry;
    class SystemContext;         // Task 2: void(SystemContext&) systems (pointer/ref-only use below)
    class ParallelCommandBuffer; // Task 2: owned by SystemScheduler, threaded through to executors

    /**
     * @brief Metadata describing a system's component access patterns and scheduling hints
     */
    struct SystemMetadata
    {
        // Components this system reads (immutable access)
        ComponentMask reads;
        
        // Components this system writes (mutable access)
        ComponentMask writes;

        // Resources this system reads / writes (singleton state; keyed by the
        // resource type's ComponentID, in masks distinct from the component
        // reads/writes so component-vs-resource can never false-conflict).
        ComponentMask resourceReads;
        ComponentMask resourceWrites;

        // Runtime type identifier for the system (type-erased)
        size_t typeId;
        
        // Insertion order (for stable sorting and debugging)
        size_t insertionOrder;

        // True if the system declared Astra::Exclusive (runs in its own solo group).
        bool requiresExclusive = false;

        // Explicit ordering edges (Phase D), resolved to the target systems'
        // TypeID::Hash() -- the same 64-bit key m_systemIndices uses. Filled by
        // ExtractSystemTraits; resolved to indices in BuildExecutionPlan.
        std::vector<uint64_t> beforeIds;
        std::vector<uint64_t> afterIds;
        std::vector<uint64_t> ambiguousWithIds;

        // Position of this system in the topological execution order (filled by
        // BuildExecutionPlan). Equals insertionOrder when no ordering edges
        // exist. Primary key of the deferred-command SortKey (see Task 4).
        size_t scheduleOrder = 0;

        // Sync-point segment this system belongs to (Phase E): the number of
        // AddSyncPoint() fences registered before it. Systems never reorder or
        // group across a segment boundary; deferred commands flush at each fence.
        // 0 for every system when no SyncPoint is used (byte-identical to Phase D).
        size_t segmentIndex = 0;
    };
    
    /**
     * @brief Execution context passed to system executors
     * 
     * Contains pre-analyzed information about which systems can run in parallel
     * and provides the functions to execute them.
     * 
     * This is the main data structure passed to custom job system integrations.
     */
    struct SystemExecutionContext
    {
        /**
         * Groups of system indices that can run in parallel.
         * - Outer vector: Sequential groups (must run in order)
         * - Inner vector: System indices that can run in parallel within each group
         * 
         * Example: [[0], [1, 2], [3]] means:
         * - System 0 runs first (alone)
         * - Systems 1 and 2 run in parallel (after 0 completes)
         * - System 3 runs last (after 1 and 2 complete)
         */
        std::vector<std::vector<size_t>> parallelGroups;
        
        /**
         * The actual system execution functions.
         * Using Delegate for better performance than std::function.
         * Indexed by system index (matches parallelGroups indices)
         */
        std::vector<Delegate<void(Registry&)>> systems;

        /**
         * Execution delegates for void(SystemContext&) systems (Task 2),
         * parallel to `systems` (same indexing). An entry is non-empty
         * (truthy via Delegate::operator bool) exactly when the system at
         * that index was registered with the SystemContext& signature; that
         * truthiness is how SystemExecutor tells the two dispatch paths
         * apart, so it doubles as the "is this a context system" flag rather
         * than needing a separate std::vector<bool>.
         */
        std::vector<Delegate<void(SystemContext&)>> contextSystems;

        /**
         * Metadata for each system (optional, for debugging/profiling)
         * Indexed by system index (matches parallelGroups indices)
         */
        std::vector<SystemMetadata> metadata;

        /**
         * The registry to execute systems on
         */
        Registry* registry = nullptr;

        /**
         * SystemScheduler's owned per-worker deferred-command sink (Task 2).
         * Non-null whenever the scheduler has been Execute()'d at least once;
         * SystemExecutor uses ->GetThreadBuffer() to hand each context
         * system its recording CommandBuffer. Never dereferenced unless a
         * context system is actually dispatched.
         */
        ParallelCommandBuffer* commandBuffer = nullptr;
    };
} // namespace Astra
