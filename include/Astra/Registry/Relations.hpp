#pragma once

#include <atomic>
#include <memory>
#include <queue>
#include <type_traits>

#include "../Archetype/ArchetypeManager.hpp"
#include "../Container/FlatSet.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Simd.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "Query.hpp"
#include "RelationshipGraph.hpp"

namespace Astra
{
    class Registry;

    template<typename... QueryArgs>
    class Relations
    {
        static_assert(ValidQuery<QueryArgs...>, "Relations template arguments must be valid components or query modifiers");
        
        static constexpr bool HAS_FILTERING = sizeof...(QueryArgs) > 0;
        
        // Parallel execution threshold: below this, sequential inline is used.
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = 1024;
        
        // Extract query information only if filtering
        using Classifier = std::conditional_t<HAS_FILTERING,
            Detail::QueryClassifier<QueryArgs...>,
                Detail::QueryClassifier<>>;
        using RequiredTuple = typename Classifier::RequiredComponents;
        using ExcludedTuple = typename Classifier::ExcludedComponents;
        using AnyGroups = typename Classifier::AnyGroups;
        using OneOfGroups = typename Classifier::OneOfGroups;
        
    public:
        Relations(std::shared_ptr<ArchetypeManager> manager, Entity entity,
                  std::shared_ptr<const RelationshipGraph> graph,
                  std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
            m_archetypeManager(manager),
            m_rootEntity(entity),
            m_relationsGraph(graph),
            m_scheduler(std::move(scheduler))
        {}
        
        /**
         * Check if the Relations object is still valid (Registry not destroyed)
         * @return true if both ArchetypeManager and RelationshipGraph are still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr && m_relationsGraph != nullptr;
        }
        
        /**
         * @brief Get the parent of the entity (filtered by components if applicable)
         * @return Parent entity if it exists and passes filter, Entity::Null() otherwise
         */
        ASTRA_FORCEINLINE Entity GetParent() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return Entity::Invalid();  // Registry destroyed
            
            Entity parent = m_relationsGraph->GetParent(m_rootEntity);
            if constexpr (!HAS_FILTERING)
            {
                return parent;
            }
            else
            {
                return (parent.IsValid() && PassesFilter(parent)) ? parent : Entity::Invalid();
            }
        }
        
        ASTRA_FORCEINLINE auto GetChildren() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return RelationshipGraph::ChildrenContainer{};
            
            if constexpr (!HAS_FILTERING)
            {
                // No filtering - return direct reference
                return m_relationsGraph->GetChildren(m_rootEntity);
            }
            else
            {
                // With filtering - build filtered list
                RelationshipGraph::ChildrenContainer filtered;
                const auto& children = m_relationsGraph->GetChildren(m_rootEntity);
                for (Entity child : children)
                {
                    if (PassesFilter(child))
                    {
                        filtered.push_back(child);
                    }
                }
                return filtered;
            }
        }
        
        ASTRA_FORCEINLINE auto GetLinks() const
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return RelationshipGraph::LinksContainer{};
            
            if constexpr (!HAS_FILTERING)
            {
                // No filtering - return direct reference
                return m_relationsGraph->GetLinks(m_rootEntity);
            }
            else
            {
                // With filtering - build filtered list
                RelationshipGraph::LinksContainer filtered;
                const auto& links = m_relationsGraph->GetLinks(m_rootEntity);
                for (Entity linked : links)
                {
                    if (PassesFilter(linked))
                    {
                        filtered.push_back(linked);
                    }
                }
                return filtered;
            }
        }
        
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachChild(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            const auto& children = m_relationsGraph->GetChildren(m_rootEntity);
            
            // Early exit for empty
            if (children.empty()) ASTRA_UNLIKELY
                return;
            
            // Process children with proper component expansion
            ForEachChildImpl(children, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ForEachDescendant(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            // Use cached BFS traversal for optimal performance
            const auto& cache = m_relationsGraph->GetDescendantsCached(m_rootEntity);
            
            const size_t count = cache.entries.size();
            if (count == 0) ASTRA_UNLIKELY
                return;
            
            // Process with proper component expansion
            ForEachDescendantImpl(cache, count, std::forward<Func>(func), RequiredTuple{});
        }
        
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachAncestor(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            // Use cached ancestor traversal
            const auto& cache = m_relationsGraph->GetAncestorsCached(m_rootEntity);
            
            const size_t count = cache.entries.size();
            if (count == 0) ASTRA_UNLIKELY
                return;
            
            // Process with proper component expansion
            ForEachAncestorImpl(cache, count, std::forward<Func>(func), RequiredTuple{});
        }
        
        template<typename Func>
        ASTRA_FORCEINLINE void ForEachLink(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            const auto& links = m_relationsGraph->GetLinks(m_rootEntity);
            
            if (links.empty()) ASTRA_UNLIKELY
                return;
            
            // Process with proper component expansion
            ForEachLinkImpl(links, std::forward<Func>(func), RequiredTuple{});
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEachDescendant(Func&& func)
        {
            if (!m_relationsGraph) ASTRA_UNLIKELY
                return;  // Registry destroyed

            const auto& cache = m_relationsGraph->GetDescendantsCached(m_rootEntity);
            const size_t count = cache.entries.size();

            // Astra creates no threads — parallelism requires an injected scheduler.
            if (!m_scheduler)
            {
                return ForEachDescendant(std::forward<Func>(func));
            }

            if (count < MIN_ENTITIES_FOR_PARALLEL) ASTRA_LIKELY
            {
                return ForEachDescendant(std::forward<Func>(func));
            }

            constexpr size_t kBatchSize = 64;
            m_scheduler->ParallelFor(count, kBatchSize, [&](size_t begin, size_t end)
            {
                for (size_t i = begin; i < end; ++i)
                {
                    const auto& entry = cache.entries[i];
                    Entity entity = entry.entity;
                    size_t depth = entry.depth;

                    if (PassesFilter(entity))
                    {
                        // Empty RequiredTuple covers the unfiltered case too
                        // (no QueryArgs => no required components).
                        if constexpr (std::tuple_size_v<RequiredTuple> == 0)
                        {
                            func(entity, depth);
                        }
                        else
                        {
                            InvokeParallelWithDepth(entity, depth, func, RequiredTuple{});
                        }
                    }
                }
            });
        }
        
    private:
        template<typename Container, typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachChildImpl(const Container& children, Func&& func, std::tuple<Components...>)
        {
            ForEachEntityList(children, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachDescendantImpl(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::tuple<Components...>)
        {
            ForEachCachedWithDepth(cache, count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachAncestorImpl(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::tuple<Components...>)
        {
            ForEachCachedWithDepth(cache, count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Container, typename Func, typename... Components>
        ASTRA_FORCEINLINE void ForEachLinkImpl(const Container& links, Func&& func, std::tuple<Components...>)
        {
            ForEachEntityList(links, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
        
        template<typename Container, typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachEntityList(const Container& entities, Func&& func, std::index_sequence<Is...>)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            for (Entity entity : entities)
            {
                if (PassesFilter(entity))
                {
                    if constexpr (!HAS_FILTERING)
                    {
                        func(entity);
                    }
                    else if constexpr (sizeof...(Is) == 0)
                    {
                        func(entity);
                    }
                    else
                    {
                        // Expand components using index_sequence
                        func(entity, *m_archetypeManager->GetComponent<std::tuple_element_t<Is, RequiredTuple>>(entity)...);
                    }
                }
            }
        }
        
        template<typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachCachedWithDepth(const RelationshipGraph::TraversalCache& cache, size_t count, Func&& func, std::index_sequence<Is...>)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            for (size_t i = 0; i < count; ++i)
            {
                const auto& entry = cache.entries[i];
                Entity entity = entry.entity;
                size_t depth = entry.depth;
                
                if (PassesFilter(entity))
                {
                    if constexpr (!HAS_FILTERING)
                    {
                        func(entity, depth);
                    }
                    else if constexpr (sizeof...(Is) == 0)
                    {
                        func(entity, depth);
                    }
                    else
                    {
                        // Expand components using index_sequence
                        func(entity, depth, *m_archetypeManager->GetComponent<std::tuple_element_t<Is, RequiredTuple>>(entity)...);
                    }
                }
            }
        }
        
        template<typename Func, typename... Components>
        ASTRA_FORCEINLINE void InvokeParallelWithDepth(Entity entity, size_t depth, Func&& func, std::tuple<Components...>)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            if constexpr (sizeof...(Components) == 0)
            {
                func(entity, depth);
            }
            else
            {
                func(entity, depth, *m_archetypeManager->GetComponent<Components>(entity)...);
            }
        }
        
        ASTRA_FORCEINLINE bool PassesFilter(Entity entity) const
        {
            if constexpr (!HAS_FILTERING)
            {
                return true;  // No filtering, always pass
            }
            else
            {
                // Check required components
                if constexpr (std::tuple_size_v<RequiredTuple> > 0)
                {
                    if (!HasAllRequired(entity, RequiredTuple{}))
                    {
                        return false;
                    }
                }
                
                // Check excluded components
                if constexpr (std::tuple_size_v<ExcludedTuple> > 0)
                {
                    if (HasAnyExcluded(entity, ExcludedTuple{}))
                    {
                        return false;
                    }
                }
                
                // Check Any groups
                if constexpr (std::tuple_size_v<AnyGroups> > 0)
                {
                    if (!CheckAnyGroups(entity))
                    {
                        return false;
                    }
                }
                
                // Check OneOf groups
                if constexpr (std::tuple_size_v<OneOfGroups> > 0)
                {
                    if (!CheckOneOfGroups(entity))
                    {
                        return false;
                    }
                }
                
                return true;
            }
        }
        
        // Simplified component checking helpers (only compiled if HAS_FILTERING)
        template<typename... Ts>
        bool HasAllRequired(Entity entity, std::tuple<Ts...>) const
        {
            if constexpr (sizeof...(Ts) == 0)
            {
                return true;
            }
            else
            {
                if (!m_archetypeManager) ASTRA_UNLIKELY
                    return false;  // Registry destroyed
                return (m_archetypeManager->HasComponent<Ts>(entity) && ...);
            }
        }
        
        template<typename... Ts>
        bool HasAnyExcluded(Entity entity, std::tuple<Ts...>) const
        {
            if constexpr (sizeof...(Ts) == 0)
            {
                return false;
            }
            else
            {
                if (!m_archetypeManager) ASTRA_UNLIKELY
                    return false;  // Registry destroyed
                return (m_archetypeManager->HasComponent<Ts>(entity) || ...);
            }
        }
        
        bool CheckAnyGroups(Entity entity) const
        {
            return CheckAnyGroupsImpl(entity, std::make_index_sequence<std::tuple_size_v<AnyGroups>>{});
        }
        
        template<size_t... Is>
        bool CheckAnyGroupsImpl(Entity entity, std::index_sequence<Is...>) const
        {
            return (CheckSingleAnyGroup<Is>(entity) && ...);
        }
        
        template<size_t I>
        bool CheckSingleAnyGroup(Entity entity) const
        {
            using Group = std::tuple_element_t<I, AnyGroups>;
            return HasAnyInGroup(entity, Group{});
        }
        
        template<typename... Ts>
        bool HasAnyInGroup(Entity entity, std::tuple<Ts...>) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return false;  // Registry destroyed
            return (m_archetypeManager->HasComponent<Ts>(entity) || ...);
        }
        
        bool CheckOneOfGroups(Entity entity) const
        {
            return CheckOneOfGroupsImpl(entity, std::make_index_sequence<std::tuple_size_v<OneOfGroups>>{});
        }
        
        template<size_t... Is>
        bool CheckOneOfGroupsImpl(Entity entity, std::index_sequence<Is...>) const
        {
            return (CheckSingleOneOfGroup<Is>(entity) && ...);
        }
        
        template<size_t I>
        bool CheckSingleOneOfGroup(Entity entity) const
        {
            using Group = std::tuple_element_t<I, OneOfGroups>;
            return CountInGroup(entity, Group{}) == 1;
        }
        
        template<typename... Ts>
        size_t CountInGroup(Entity entity, std::tuple<Ts...>) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed
            return ((m_archetypeManager->HasComponent<Ts>(entity) ? 1 : 0) + ...);
        }
        
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        Entity m_rootEntity;
        std::shared_ptr<const RelationshipGraph> m_relationsGraph;
        std::shared_ptr<IWorkScheduler> m_scheduler;
    };
}
