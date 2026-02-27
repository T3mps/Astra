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
    
    /**
     * @brief Metadata describing a system's component access patterns and scheduling hints
     */
    struct SystemMetadata
    {
        // Components this system reads (immutable access)
        ComponentMask reads;
        
        // Components this system writes (mutable access)
        ComponentMask writes;
        
        // Runtime type identifier for the system (type-erased)
        size_t typeId;
        
        // Insertion order (for stable sorting and debugging)
        size_t insertionOrder;
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
         * Metadata for each system (optional, for debugging/profiling)
         * Indexed by system index (matches parallelGroups indices)
         */
        std::vector<SystemMetadata> metadata;
        
        /**
         * The registry to execute systems on
         */
        Registry* registry = nullptr;
    };
} // namespace Astra
