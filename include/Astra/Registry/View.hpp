#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/WorkScheduler.hpp"
#include "../Entity/Entity.hpp"
#include "Query.hpp"
#include "ViewIterator.hpp"

namespace Astra
{
    template<typename... QueryArgs>
    class View
    {
        static_assert(ValidQuery<QueryArgs...>, "View template arguments must be valid components or query modifiers");

        // Query type extraction - must be declared early for Iterator type
        using RequiredTypes = typename Detail::QueryClassifier<QueryArgs...>::RequiredComponents;
        using OptionalTypes = typename Detail::QueryClassifier<QueryArgs...>::OptionalComponents;
        using QueryBuilder = Astra::QueryBuilder<QueryArgs...>;  // qualified to avoid -Wchanges-meaning

        // Parallel execution thresholds - based on empirical testing
        static constexpr size_t AVG_ENTITIES_PER_CHUNK = 256;                           // Typical for 16KB chunks with ~50 byte entities
        static constexpr size_t MIN_CHUNKS_PER_THREAD = 4;                              // Each thread should process at least 4 chunks (64KB)
        static constexpr size_t MIN_CHUNKS_FOR_PARALLEL = MIN_CHUNKS_PER_THREAD * 2;    // Need enough for at least 2 threads

        // Derive entity thresholds from chunk-based values
        static constexpr size_t MIN_ENTITIES_QUICK_CHECK = AVG_ENTITIES_PER_CHUNK / 2;  // Less than half a chunk = definitely sequential
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = MIN_CHUNKS_FOR_PARALLEL * AVG_ENTITIES_PER_CHUNK / 2;  // ~4 chunks worth

    public:
        explicit View(std::shared_ptr<ArchetypeManager> manager,
                      std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
            m_archetypeManager(manager),
            m_scheduler(std::move(scheduler)),   // null => sequential fallback
            m_lastRefreshCounter(0),
            m_lastGeneration(0)
        {
            CollectArchetypes();
            m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            m_lastGeneration = m_archetypeManager->m_generation;
            m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
        }
        
        /**
         * Check if the View is still valid (Registry not destroyed)
         * @return true if the underlying ArchetypeManager is still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr;
        }

        /**
         * Invoke func(Entity, Components&...) for every entity matching this View.
         *
         * CONTRACT: structural mutation is NOT supported during this call. Do not
         * create or destroy entities, or add/remove components, from within func --
         * the Archetype/chunk pointers this loop walks are captured up front and are
         * not re-validated mid-iteration, so a direct structural change here is
         * undefined behavior. In-place edits to already-present component VALUES
         * (e.g. `pos.x += 1`) are fine.
         *
         * To change entity structure while iterating, record the changes into a
         * `CommandBuffer` and call `Execute()` AFTER this loop returns. Recording
         * into a CommandBuffer does not itself touch the ArchetypeManager, so a
         * properly deferred CommandBuffer never trips the Debug guard below.
         *
         * In Debug builds this is additionally enforced by an ASTRA_ASSERT that
         * compares the ArchetypeManager's structural-change counter before and
         * after the loop; the check (and the counter read) is compiled out
         * entirely in Release/Dist builds, so this contract carries zero Release
         * cost.
         */
        template<typename Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            EnsureArchetypes();

#ifdef ASTRA_BUILD_DEBUG
            // Captured AFTER EnsureArchetypes() so its own (legitimate) refresh
            // of the counter is never mistaken for an in-loop structural change.
            const uint32_t debugStartStructuralChangeCounter =
                m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
#endif

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;

            for (Archetype* archetype : m_archetypes)
            {
                ForEachImpl(archetype, std::forward<Func>(func), RequiredTypes{}, OptionalTypes{});
            }

#ifdef ASTRA_BUILD_DEBUG
            ASTRA_ASSERT(m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire) == debugStartStructuralChangeCounter,
                "Structural mutation (create/destroy entity, add/remove component) detected during View::ForEach. "
                "Defer structural changes into a CommandBuffer and call Execute() after the loop.");
#endif
        }
        
        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            EnsureArchetypes();
            
            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;
            
            // Quick check: if we have very few matching entities, don't even try parallel
            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            if (quickCount < MIN_ENTITIES_QUICK_CHECK)
            {
                return ForEach(std::forward<Func>(func));
            }

            // No scheduler injected: Astra spawns no threads — run sequentially inline.
            if (!m_scheduler)
                return ForEach(std::forward<Func>(func));

            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            // Better estimation based on typical entities per 16KB chunk
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;
            
            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }
            
            // Fall back to sequential for tiny workloads
            if (chunkWork.empty() || totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL || chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                return ForEach(std::forward<Func>(func));
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        auto [archetype, chunkIndex] = chunkWork[w];
                        ParallelForEachChunkImpl(archetype, chunkIndex, func, RequiredTypes{}, OptionalTypes{});
                    }
                });
        }

        /**
         * Like ParallelForEach, but threads a per-chunk sub-context to the body
         * (Theme B2 Phase B, Task 3 -- the machinery behind
         * SystemContext::ParallelForEach). For each unit of chunk work at FLAT
         * chunkWork index `w`, builds `auto sub = factory(w);` and invokes
         * `body(entity, components..., sub)` for every entity in that chunk. The
         * factory runs ON the worker executing the chunk, so a sub-context whose
         * recorder is a per-thread CommandBuffer records into THAT worker's own
         * buffer.
         *
         * DETERMINISM (critical): the factory argument is the FLAT chunkWork
         * index `w`, NOT the per-archetype chunk index (chunkWork[w].second). In
         * a multi-archetype view chunk 0 of archetype A and chunk 0 of archetype
         * B share the per-archetype index 0, so stamping that would give two
         * chunks the same {insertionOrder, 0, ...} key -- and since each
         * sub-context restarts its recordSequence at 0, their commands would
         * collide and the flush's stable-sort tiebreak would be non-
         * deterministic. The flat `w` is globally unique across the whole view
         * AND deterministic (chunkWork is built by iterating the
         * deterministically-sorted archetypes x their chunks in order). The
         * per-archetype chunk index is still what selects the actual chunk.
         *
         * Additive sibling of ParallelForEach: REUSES ParallelForEachChunkImpl
         * for the chunk walk (both the no-optional ForEachChunk path and the
         * optional InvokeEntityCallback path invoke the callback as
         * `(entity, component-refs..., [optional ptrs...])`, which the wrapper
         * adapts by appending `sub`). Does NOT modify ParallelForEach itself.
         *
         * Unlike ParallelForEach, the null-scheduler / below-threshold case does
         * NOT delegate to ForEach (which has neither a per-chunk sub-context nor
         * a chunk index): it builds chunkWork unconditionally and walks it INLINE
         * IN FLAT ORDER, which is deterministic. The per-chunk work is factored
         * into one `runChunk(w)` local used by both the scheduler-dispatch and
         * the inline path so the two can't drift.
         *
         * RETURNS the number of chunk-work items processed (chunkWork.size()) --
         * i.e. the count of distinct flat indices `w` in [0, chunkWork.size())
         * the factory could have been called with, IDENTICAL on the scheduler-
         * dispatch and inline paths (both process the whole chunkWork). Every
         * early-return path returns 0. SystemContext::ParallelForEach uses this
         * to advance its per-scope iterationIndex band by exactly the number of
         * distinct iterationIndex values this call could have stamped, keeping
         * deferred-command SortKeys globally unique across sequential calls.
         * Callers that ignore the return value are unaffected (additive).
         */
        template<typename Factory, typename Body>
        ASTRA_FORCEINLINE size_t ParallelForEachWithContext(Factory&& factory, Body&& body)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();

            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return 0;

            size_t quickCount = 0;
            for (Archetype* archetype : m_archetypes)
            {
                quickCount += archetype->GetEntityCount();
            }

            // Build the chunk work list unconditionally (see method doc): both
            // the parallel-dispatch and the inline fallback path need it, and
            // the per-archetype chunk index it carries.
            std::vector<std::pair<Archetype*, size_t>> chunkWork;
            size_t estimatedChunks = (quickCount / AVG_ENTITIES_PER_CHUNK) + m_archetypes.size();
            chunkWork.reserve(estimatedChunks);
            size_t totalMatchingEntities = 0;

            for (Archetype* archetype : m_archetypes)
            {
                size_t chunkCount = archetype->GetChunkCount();
                for (size_t i = 0; i < chunkCount; ++i)
                {
                    size_t chunkEntityCount = archetype->GetChunkEntityCount(i);
                    if (chunkEntityCount > 0)
                    {
                        chunkWork.emplace_back(archetype, i);
                        totalMatchingEntities += chunkEntityCount;
                    }
                }
            }

            if (chunkWork.empty()) ASTRA_UNLIKELY
                return 0;

            // Per-chunk work, shared by the scheduler-dispatch and inline paths
            // so they can't drift. `w` is the FLAT chunkWork index -- the
            // iterationIndex stamped into the sub-context; chunkWork[w].second is
            // the per-archetype chunk index selecting the actual chunk.
            auto runChunk = [&](size_t w)
            {
                auto [archetype, chunkIndex] = chunkWork[w];
                auto sub = factory(static_cast<uint32_t>(w));
                auto wrapped = [&body, &sub](Astra::Entity e, auto&&... comps)
                {
                    body(e, std::forward<decltype(comps)>(comps)..., sub);
                };
                ParallelForEachChunkImpl(archetype, chunkIndex, wrapped, RequiredTypes{}, OptionalTypes{});
            };

            // No scheduler, or workload below the parallel thresholds: walk every
            // chunk inline, in flat order -- deterministic, no thread fan-out.
            if (!m_scheduler ||
                quickCount < MIN_ENTITIES_QUICK_CHECK ||
                totalMatchingEntities < MIN_ENTITIES_FOR_PARALLEL ||
                chunkWork.size() < MIN_CHUNKS_FOR_PARALLEL)
            {
                for (size_t w = 0; w < chunkWork.size(); ++w)
                {
                    runChunk(w);
                }
                return chunkWork.size();
            }

            m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
                [&](size_t begin, size_t end, uint32_t /*worker*/)
                {
                    for (size_t w = begin; w < end; ++w)
                    {
                        runChunk(w);
                    }
                });
            return chunkWork.size();
        }

        ASTRA_NODISCARD size_t Size() noexcept
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return 0;  // Registry destroyed

            EnsureArchetypes();

            size_t total = 0;
            for (Archetype* archetype : m_archetypes)
            {
                total += archetype->GetEntityCount();
            }
            return total;
        }

        ASTRA_NODISCARD bool Empty() noexcept
        {
            return Size() == 0;
        }

        // ============= Range-based for loop support =============

        /**
         * Iterator type for range-based for loops.
         * Uses the required components from the query.
         */
        using Iterator = ViewIteratorFromTuple_t<RequiredTypes>;

        /**
         * Begin iterator for range-based for loop.
         * Ensures archetypes are up-to-date before returning iterator.
         */
        ASTRA_FORCEINLINE Iterator begin()
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return Iterator(nullptr, 0);

            EnsureArchetypes();
            return Iterator(m_archetypes.data(), m_archetypes.size());
        }

        /**
         * End sentinel for range-based for loop.
         */
        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            return ViewSentinel{};
        }

    private:
        struct ArchetypeEntityCountComparator
        {
            bool operator()(Archetype* a, Archetype* b) const
            {
                return a->GetEntityCount() > b->GetEntityCount();
            }
        };

        void EnsureArchetypes()
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            uint32_t currentCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            if (m_lastRefreshCounter == currentCounter)
            {
                return;
            }

            uint32_t removalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            if (m_lastGeneration == 0 || removalCounter != m_lastRemovalCounter)
            {
                // Archetypes were removed (or first refresh): cached pointers
                // may be stale — rebuild the whole list.
                CollectArchetypes();
                m_lastRemovalCounter = removalCounter;
            }
            else
            {
                auto newArchetypes = m_archetypeManager->GetArchetypesSince(m_lastGeneration);
                for (Archetype* arch : newArchetypes)
                {
                    if (QueryBuilder::Matches(arch->GetMask()))
                    {
                        m_archetypes.push_back(arch);
                    }
                }
                std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
            }

            m_lastRefreshCounter = currentCounter;
            m_lastGeneration = m_archetypeManager->m_generation;
        }

        void CollectArchetypes()
        {
            m_archetypes.clear();
            if (!m_archetypeManager) ASTRA_UNLIKELY
            {
                return;  // Registry destroyed
            }

            auto archetypes = m_archetypeManager->GetArchetypes();
            const size_t queryComponentCount = QueryBuilder::GetRequiredMask().Count();

            m_archetypes.reserve(archetypes.size());

            for (Archetype* archetype : archetypes)
            {
                // NOTE: empty archetypes are deliberately KEPT — they may gain
                // entities later without any archetype-creation event, and
                // iterating an empty archetype is free (zero-count chunks).
                if (archetype->GetComponentCount() < queryComponentCount) ASTRA_UNLIKELY
                {
                    continue;
                }
                if (QueryBuilder::Matches(archetype->GetMask()))
                {
                    m_archetypes.push_back(archetype);
                }
            }

            std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
        }

        template<typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ForEachImpl(Archetype* archetype, Func&& func, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (sizeof...(Optional) == 0)
            {
                archetype->ForEach<Required...>(std::forward<Func>(func));
            }
            else
            {
                ForEachWithOptional<Required..., Optional...>(archetype, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
            }
        }
        
        template<typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ForEachWithOptional(Archetype* archetype, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };

            const auto& chunks = archetype->GetChunks();

            for (auto& chunk : chunks)
            {
                size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                {
                    continue;
                }

                std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
                {
                    chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
                };
                std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
                {
                    (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
                };

                const auto& entities = chunk->GetEntities();

                InvokeEntityCallback(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{});
            }
        }

        template<typename Func, typename... Required, typename... Optional>
        ASTRA_FORCEINLINE void ParallelForEachChunkImpl(Archetype* archetype, size_t chunkIndex, Func&& func, std::tuple<Required...>, std::tuple<Optional...>)
        {
            if constexpr (sizeof...(Optional) == 0)
            {
                archetype->ForEachChunk<Required...>(chunkIndex, std::forward<Func>(func));
            }
            else
            {
                ParallelForEachChunkWithOptional<Required..., Optional...>(archetype, chunkIndex, std::forward<Func>(func), std::make_index_sequence<sizeof...(Required)>{}, std::make_index_sequence<sizeof...(Optional)>{});
            }
        }
        
        template<typename... Components, typename Func, size_t... RequiredTs, size_t... OptionalTs>
        ASTRA_FORCEINLINE void ParallelForEachChunkWithOptional(Archetype* archetype, size_t chunkIndex, Func&& func, std::index_sequence<RequiredTs...>, std::index_sequence<OptionalTs...>)
        {
            constexpr size_t OptionalCount = sizeof...(OptionalTs);
            std::array<bool, OptionalCount> hasOptional =
            {
                archetype->HasComponent<std::tuple_element_t<OptionalTs, OptionalTypes>>()...
            };
            
            const auto& chunks = archetype->GetChunks();
            if (chunkIndex >= chunks.size()) ASTRA_UNLIKELY
                return;
                
            auto& chunk = chunks[chunkIndex];
            size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;
                
            std::tuple<std::tuple_element_t<RequiredTs, RequiredTypes>*...> requiredPtrs =
            {
                chunk->GetComponentArray<std::tuple_element_t<RequiredTs, RequiredTypes>>()...
            };
            std::tuple<std::tuple_element_t<OptionalTs, OptionalTypes>*...> optionalPtrs =
            {
                (hasOptional[OptionalTs] ? chunk->GetComponentArray<std::tuple_element_t<OptionalTs, OptionalTypes>>() : nullptr)...
            };
            
            const auto& entities = chunk->GetEntities();
            
            InvokeEntityCallback(entities, requiredPtrs, optionalPtrs, count, std::forward<Func>(func), std::make_index_sequence<sizeof...(RequiredTs)>{}, std::make_index_sequence<sizeof...(OptionalTs)>{});
        }

        template<typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallback(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs, size_t count, Func&& func, std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>)
        {
            for (size_t i = 0; i < count; ++i)
            {
                func(entities[i], std::get<ReqIs>(reqPtrs)[i]..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...);
            }
        }

        std::vector<Archetype*> m_archetypes;
        std::shared_ptr<ArchetypeManager> m_archetypeManager;
        std::shared_ptr<IWorkScheduler> m_scheduler;  // null = sequential inline fallback

        uint32_t m_lastRefreshCounter = 0;
        uint32_t m_lastGeneration = 0;
        uint32_t m_lastRemovalCounter = 0;
    };
} // namespace Astra