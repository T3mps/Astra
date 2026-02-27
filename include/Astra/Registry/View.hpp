#pragma once

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#include "../Archetype/Archetype.hpp"
#include "../Archetype/ArchetypeManager.hpp"
#include "../Component/Component.hpp"
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
        using QueryBuilder = QueryBuilder<QueryArgs...>;

        // Parallel execution thresholds - based on empirical testing
        static constexpr size_t AVG_ENTITIES_PER_CHUNK = 256;                           // Typical for 16KB chunks with ~50 byte entities
        static constexpr size_t MIN_CHUNKS_PER_THREAD = 4;                              // Each thread should process at least 4 chunks (64KB)
        static constexpr size_t MIN_CHUNKS_FOR_PARALLEL = MIN_CHUNKS_PER_THREAD * 2;    // Need enough for at least 2 threads

        // Derive entity thresholds from chunk-based values
        static constexpr size_t MIN_ENTITIES_QUICK_CHECK = AVG_ENTITIES_PER_CHUNK / 2;  // Less than half a chunk = definitely sequential
        static constexpr size_t MIN_ENTITIES_FOR_PARALLEL = MIN_CHUNKS_FOR_PARALLEL * AVG_ENTITIES_PER_CHUNK / 2;  // ~4 chunks worth

    public:
        explicit View(std::shared_ptr<ArchetypeManager> manager) :
            m_archetypeManager(manager),
            m_lastRefreshCounter(0),
            m_lastGeneration(0)
        {
            CollectArchetypes();
            m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            m_lastGeneration = m_archetypeManager->m_generation;
        }
        
        /**
         * Check if the View is still valid (Registry not destroyed)
         * @return true if the underlying ArchetypeManager is still alive
         */
        ASTRA_NODISCARD bool IsValid() const noexcept
        {
            return m_archetypeManager != nullptr;
        }

        template<typename Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed
            
            EnsureArchetypes();
            
            if (m_archetypes.empty()) ASTRA_UNLIKELY
                return;
            
            for (Archetype* archetype : m_archetypes)
            {
                ForEachImpl(archetype, std::forward<Func>(func), RequiredTypes{}, OptionalTypes{});
            }
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
            
            // Determine optimal thread count ensuring each thread gets meaningful work
            const size_t hardwareConcurrency = std::thread::hardware_concurrency();
            const size_t maxThreadsByWork = chunkWork.size() / MIN_CHUNKS_PER_THREAD;
            const size_t numWorkers = std::min(hardwareConcurrency, std::max(size_t(1), maxThreadsByWork));
            
            std::atomic<size_t> nextChunkIndex{0};
            std::vector<std::future<void>> futures;
            futures.reserve(numWorkers);
            
            for (size_t t = 0; t < numWorkers; ++t)
            {
                futures.push_back(std::async(std::launch::async,
                    [this, &func, &chunkWork, &nextChunkIndex]()
                    {
                        size_t chunkIdx;
                        while ((chunkIdx = nextChunkIndex.fetch_add(1, std::memory_order_relaxed)) < chunkWork.size())
                        {
                            auto [archetype, chunkIndex] = chunkWork[chunkIdx];
                            ParallelForEachChunkImpl(archetype, chunkIndex, func, RequiredTypes{}, OptionalTypes{});
                        }
                    }));
            }
            
            for (auto& future : futures)
            {
                future.wait();
            }
        }
        
        ASTRA_NODISCARD size_t Size() const noexcept
        {
            size_t total = 0;
            for (const auto* archetype : m_archetypes)
            {
                total += archetype->GetEntityCount();
            }
            return total;
        }

        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_archetypes.empty();
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
            
            if (m_lastGeneration == 0)
            {
                CollectArchetypes();
            }
            else
            {
                auto newArchetypes = m_archetypeManager->GetArchetypesSince(m_lastGeneration);
                for (Archetype* arch : newArchetypes)
                {
                    if (arch->GetEntityCount() == 0) ASTRA_UNLIKELY
                    {
                        continue;
                    }
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
            if (!m_archetypeManager) ASTRA_UNLIKELY
            {
                m_archetypes.clear();
                return;  // Registry destroyed
            }
            
            auto archetypes = m_archetypeManager->GetArchetypes();
            const size_t queryComponentCount = QueryBuilder::GetRequiredMask().Count();
            
            m_archetypes.reserve(archetypes.size());
            
            for (Archetype* archetype : archetypes)
            {
                if (archetype->GetEntityCount() == 0 || archetype->GetComponentCount() < queryComponentCount) ASTRA_UNLIKELY
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
        
        uint32_t m_lastRefreshCounter = 0;
        uint32_t m_lastGeneration = 0;
    };
} // namespace Astra