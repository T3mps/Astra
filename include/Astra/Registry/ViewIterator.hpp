#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include "../Archetype/Archetype.hpp"
#include "../Core/Base.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    /**
     * Lightweight sentinel type for View iteration.
     */
    struct ViewSentinel {};

    /**
     * Iterable wrapper for View iteration.
     * Provides begin()/end() for range-based for loop support.
     *
     * @tparam Components The required component types to iterate over
     */
    template<typename... Components>
    class ViewIterable
    {
    public:
        class Iterator
        {
        public:
            // STL iterator traits
            using iterator_category = std::forward_iterator_tag;
            using value_type = std::tuple<Entity, Components&...>;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            Iterator() noexcept = default;

            Iterator(Archetype* const* archetypes, size_t archetypeCount) noexcept
                : m_archetypes(archetypes)
                , m_archetypeCount(archetypeCount)
            {
                if (m_archetypeCount > 0)
                {
                    FindFirstValidPosition();
                }
            }

            /**
             * Sentinel comparison - simple integer comparison.
             * When done, m_entityIndex >= m_chunkEntityCount (both 0).
             */
            ASTRA_FORCEINLINE bool operator!=(ViewSentinel) const noexcept
            {
                return m_entityIndex < m_chunkEntityCount;
            }

            ASTRA_FORCEINLINE bool operator==(ViewSentinel) const noexcept
            {
                return m_entityIndex >= m_chunkEntityCount;
            }

            /**
             * Pre-increment operator - advances to next entity.
             * Single index increment on hot path, matching ForEach's pattern.
             */
            ASTRA_FORCEINLINE Iterator& operator++() noexcept
            {
                ++m_entityIndex;
                if (m_entityIndex >= m_chunkEntityCount) [[unlikely]]
                {
                    AdvanceChunk();
                }
                return *this;
            }

            ASTRA_FORCEINLINE Iterator operator++(int) noexcept
            {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            /**
             * Dereference operator - returns tuple of entity and component references.
             * Uses index-based access matching ForEach's optimized pattern.
             */
            ASTRA_FORCEINLINE value_type operator*() const noexcept
            {
                return MakeEntityTuple(std::index_sequence_for<Components...>{});
            }

        private:
            void FindFirstValidPosition() noexcept
            {
                while (m_archetypeIndex < m_archetypeCount)
                {
                    Archetype* archetype = m_archetypes[m_archetypeIndex];
                    const auto& chunks = archetype->GetChunks();

                    while (m_chunkIndex < chunks.size())
                    {
                        size_t count = chunks[m_chunkIndex]->GetCount();
                        if (count > 0)
                        {
                            CacheChunkState(archetype, m_chunkIndex);
                            return;
                        }
                        ++m_chunkIndex;
                    }

                    ++m_archetypeIndex;
                    m_chunkIndex = 0;
                }
            }

            void AdvanceChunk() noexcept
            {
                ++m_chunkIndex;
                m_entityIndex = 0;

                while (m_archetypeIndex < m_archetypeCount)
                {
                    Archetype* archetype = m_archetypes[m_archetypeIndex];
                    const auto& chunks = archetype->GetChunks();

                    while (m_chunkIndex < chunks.size())
                    {
                        size_t count = chunks[m_chunkIndex]->GetCount();
                        if (count > 0)
                        {
                            CacheChunkState(archetype, m_chunkIndex);
                            return;
                        }
                        ++m_chunkIndex;
                    }

                    ++m_archetypeIndex;
                    m_chunkIndex = 0;
                }

                // End state
                m_chunkEntityCount = 0;
            }

            ASTRA_FORCEINLINE void CacheChunkState(Archetype* archetype, size_t chunkIndex) noexcept
            {
                const auto& chunks = archetype->GetChunks();
                auto* chunk = chunks[chunkIndex].get();

                m_chunkEntityCount = chunk->GetCount();
                m_entityIndex = 0;
                m_entities = chunk->GetEntities().data();
                m_componentArrays = std::tuple{chunk->GetComponentArray<std::remove_const_t<Components>>()...};
            }

            /**
             * Index-based access matching ForEach's optimized pattern:
             * entities[i], array0[i], array1[i], ...
             */
            template<size_t... Is>
            ASTRA_FORCEINLINE value_type MakeEntityTuple(std::index_sequence<Is...>) const noexcept
            {
                return value_type{
                    m_entities[m_entityIndex],
                    std::get<Is>(m_componentArrays)[m_entityIndex]...
                };
            }

            // View-level state
            Archetype* const* m_archetypes = nullptr;
            size_t m_archetypeCount = 0;

            // Navigation state
            size_t m_archetypeIndex = 0;
            size_t m_chunkIndex = 0;
            size_t m_entityIndex = 0;

            // Cached chunk state
            size_t m_chunkEntityCount = 0;
            const Entity* m_entities = nullptr;
            std::tuple<std::remove_const_t<Components>*...> m_componentArrays{};
        };

        ViewIterable(Archetype* const* archetypes, size_t count) noexcept
            : m_archetypes(archetypes)
            , m_archetypeCount(count)
        {
        }

        ASTRA_FORCEINLINE Iterator begin() const noexcept
        {
            return Iterator(m_archetypes, m_archetypeCount);
        }

        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            return ViewSentinel{};
        }

    private:
        Archetype* const* m_archetypes;
        size_t m_archetypeCount;
    };

    template<typename Tuple>
    struct ViewIterableFromTuple;

    template<typename... Components>
    struct ViewIterableFromTuple<std::tuple<Components...>>
    {
        using type = ViewIterable<Components...>;
        using Iterator = typename type::Iterator;
    };

    template<typename Tuple>
    using ViewIterableFromTuple_t = typename ViewIterableFromTuple<Tuple>::type;

    template<typename Tuple>
    using ViewIteratorFromTuple_t = typename ViewIterableFromTuple<Tuple>::Iterator;

} // namespace Astra
