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
#include "../Core/TypeID.hpp"
#include "../Registry/Registry.hpp"
#include "System.hpp"
#include "SystemExecutor.hpp"
#include "SystemMetadata.hpp"

namespace Astra
{
    class SystemScheduler
    {
    public:
        // RAII guard for execution lock (prevents use-after-free during parallel execution)
        class ExecutionGuard
        {
        public:
            explicit ExecutionGuard(std::atomic<bool>& flag) : m_flag(flag)
            {
                m_flag.store(true, std::memory_order_release);
            }
            ~ExecutionGuard()
            {
                m_flag.store(false, std::memory_order_release);
            }
            ExecutionGuard(const ExecutionGuard&) = delete;
            ExecutionGuard& operator=(const ExecutionGuard&) = delete;
        private:
            std::atomic<bool>& m_flag;
        };

        // Check if scheduler is currently executing (cannot be modified during execution)
        ASTRA_NODISCARD bool IsExecuting() const noexcept
        {
            return m_isExecuting.load(std::memory_order_acquire);
        }

        template<System T, typename... Args>
        void AddSystem(Args&&... args)
        {
            // Prevent modification during execution to avoid use-after-free
            ASTRA_ASSERT(!IsExecuting(), "Cannot add system while scheduler is executing");
            if (IsExecuting()) return;

            size_t typeId = TypeID<T>::Value();

            // Check if system type is already registered
            if (m_systemIndices.Contains(typeId))
            {
                ASTRA_ASSERT(false, "System type already registered");
                return;
            }
            
            size_t index = m_systems.size();
            m_systemIndices[typeId] = index;
            
            // Create system instance with perfect forwarding
            auto* instance = new T(std::forward<Args>(args)...);
            
            // Create initial metadata
            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = typeId,
                .insertionOrder = index
            };
            
            // Auto-detect component dependencies if the system has traits
            if constexpr (HasSystemTraits_v<T>)
            {
                ExtractSystemTraits<T>(metadata);
            }
            else
            {
                // No traits = conservative approach: assume system touches everything
                // This forces sequential execution for safety
                // Leave reads and writes empty - this triggers conservative scheduling
            }
            
            // Create entry with type erasure
            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<T*>(ptr); }
                ),
                .execute = [instance](Registry& reg) { (*instance)(reg); },
                .metadata = metadata
            });
            
            m_needsRebuild = true;
        }

        template<typename Lambda>
        requires LambdaLike<Lambda>
        void AddSystem(Lambda&& lambda)
        {
            AddLambdaSystemImpl(std::forward<Lambda>(lambda), &std::decay_t<Lambda>::operator());
        }

        template<System T>
        void RemoveSystem()
        {
            // Prevent modification during execution to avoid use-after-free
            ASTRA_ASSERT(!IsExecuting(), "Cannot remove system while scheduler is executing");
            if (IsExecuting()) return;

            size_t typeId = TypeID<T>::Value();
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
            
            m_needsRebuild = true;
        }
        
        template<System T>
        ASTRA_NODISCARD bool HasSystem() const
        {
            return m_systemIndices.Contains(TypeID<T>::Value());
        }

        void Execute(Registry& registry)
        {
            static SequentialExecutor defaultExecutor;
            Execute(registry, &defaultExecutor);
        }
        
        void Execute(Registry& registry, ISystemExecutor* executor)
        {
            ASTRA_ASSERT(executor != nullptr, "Executor cannot be null");

            if (m_systems.empty())
                return;

            // Acquire execution lock - prevents modification during parallel execution
            // This prevents use-after-free when systems are removed while executing
            ExecutionGuard guard(m_isExecuting);

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
            // Prevent modification during execution to avoid use-after-free
            ASTRA_ASSERT(!IsExecuting(), "Cannot clear scheduler while executing");
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
        
        void BuildExecutionPlan()
        {
            m_executionPlan.clear();
            
            if (m_systems.empty())
            {
                m_needsRebuild = false;
                return;
            }
            
            std::vector<bool> scheduled(m_systems.size(), false);
            
            // Process systems in insertion order
            for (size_t i = 0; i < m_systems.size(); ++i)
            {
                if (scheduled[i])
                {
                    continue;
                }
                
                std::vector<size_t> group;
                group.reserve(m_systems.size() - i);  // Reserve space for potential members
                group.push_back(i);
                scheduled[i] = true;
                
                // Track component usage for the entire group
                // This allows us to check conflicts with the group as a whole
                // rather than checking against each system in the group
                const auto& sysI = m_systems[i].metadata;
                ComponentMask groupReads = sysI.reads;
                ComponentMask groupWrites = sysI.writes;
                
                // If the first system has no hints, no other system can join this group
                // This ensures conservative safety
                const bool groupAcceptsMore = !(sysI.reads.None() && sysI.writes.None());
                
                // Look ahead for systems that can run in parallel
                for (size_t j = i + 1; j < m_systems.size() && groupAcceptsMore; ++j)
                {
                    if (scheduled[j])
                    {
                        continue;
                    }
                    
                    const auto& sysJ = m_systems[j].metadata;
                    
                    // Fast conflict check against group's aggregate component usage
                    // System j conflicts with the group if:
                    // - It writes to something the group reads or writes
                    // - It reads something the group writes
                    bool conflictsWithGroup = false;
                    
                    // Check if system has no hints (conservative approach)
                    if (sysJ.reads.None() && sysJ.writes.None())
                    {
                        conflictsWithGroup = true;
                    }
                    else
                    {
                        // Check actual component conflicts using bitmasks
                        conflictsWithGroup = 
                            (sysJ.writes & groupWrites).Any() || // Write-write conflict
                            (sysJ.writes & groupReads).Any()  || // Write-read conflict  
                            (sysJ.reads  & groupWrites).Any();   // Read-write conflict
                    }
                    
                    if (conflictsWithGroup)
                    {
                        continue;
                    }
                    
                    // Check if j depends on any unscheduled system before it
                    // This preserves relative ordering
                    bool dependsOnEarlier = false;
                    for (size_t k = i + 1; k < j; ++k)
                    {
                        if (!scheduled[k] && HasConflict(k, j))
                        {
                            dependsOnEarlier = true;
                            break;
                        }
                    }
                    
                    if (!dependsOnEarlier)
                    {
                        // Add system to group and update groups component usage
                        group.push_back(j);
                        scheduled[j] = true;
                        groupReads |= sysJ.reads;
                        groupWrites |= sysJ.writes;
                    }
                }
                
                m_executionPlan.push_back(std::move(group));
            }
            
            m_needsRebuild = false;
        }

        ASTRA_NODISCARD bool HasConflict(size_t a, size_t b) const
        {
            const auto& sysA = m_systems[a].metadata;
            const auto& sysB = m_systems[b].metadata;
            
            // Conservative: if either system has no hints, assume conflict
            // This ensures safety when users don't provide Read/Write information
            if ((sysA.reads.None() && sysA.writes.None()) || (sysB.reads.None() && sysB.writes.None()))
                return true;
            
            // Check for write-write conflicts
            if ((sysA.writes & sysB.writes).Any())
                return true;
            
            // Check for read-write conflicts
            if (((sysA.reads & sysB.writes).Any()) || (sysA.writes & sysB.reads).Any())
                return true;

            return false;
        }

        // Helper to extract signature from const lambda
        template<typename Lambda, typename Ret, typename Class, typename... Args>
        void AddLambdaSystemImpl(Lambda&& lambda, Ret(Class::*)(Args...) const)
        {
            using Wrapper = LambdaSystemWrapper<std::decay_t<Lambda>, Args...>;
            AddSystemInternal<Wrapper>(Wrapper{std::forward<Lambda>(lambda)});
        }

        // Helper to extract signature from non-const lambda
        template<typename Lambda, typename Ret, typename Class, typename... Args>
        void AddLambdaSystemImpl(Lambda&& lambda, Ret(Class::*)(Args...))
        {
            using Wrapper = LambdaSystemWrapper<std::decay_t<Lambda>, Args...>;
            AddSystemInternal<Wrapper>(Wrapper{std::forward<Lambda>(lambda)});
        }

        template<typename SystemType>
        void AddSystemInternal(SystemType system)
        {
            // Prevent modification during execution to avoid use-after-free
            ASTRA_ASSERT(!IsExecuting(), "Cannot add system while scheduler is executing");
            if (IsExecuting()) return;

            size_t typeId = TypeID<SystemType>::Value();

            // Check if system type is already registered
            if (m_systemIndices.Contains(typeId))
            {
                ASTRA_ASSERT(false, "System type already registered");
                return;
            }

            size_t index = m_systems.size();
            m_systemIndices[typeId] = index;

            // Create system instance
            auto* instance = new SystemType(std::move(system));

            // Create initial metadata
            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = typeId,
                .insertionOrder = index
            };

            // Auto-detect component dependencies
            if constexpr (HasSystemTraits_v<SystemType>)
            {
                ExtractSystemTraits<SystemType>(metadata);
            }

            // Create entry with type erasure
            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(
                    instance,
                    [](void* ptr) { delete static_cast<SystemType*>(ptr); }
                ),
                    .execute = [instance](Registry& reg) { (*instance)(reg); },
                    .metadata = metadata
                });

            m_needsRebuild = true;
        }
        
        std::vector<SystemEntry> m_systems;                             // All registered systems
        FlatMap<size_t, size_t> m_systemIndices;                        // TypeID value to index mapping
        mutable std::vector<std::vector<size_t>> m_executionPlan;       // Cached parallel groups
        mutable bool m_needsRebuild = true;                             // Whether execution plan needs rebuild
        mutable std::atomic<bool> m_isExecuting{false};                 // Execution lock to prevent modification during parallel execution
    };
} // namespace Astra
