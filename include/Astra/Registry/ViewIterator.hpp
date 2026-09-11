#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include "../Archetype/Archetype.hpp"
#include "../Component/Component.hpp"
#include "../Core/Base.hpp"
#include "../Core/Tick.hpp"
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

            // `now` is the registry's current tick, applied as the coarse
            // change-detection stamp to every non-const component column of every
            // chunk this iterator enters (View::begin() supplies it). Deliberately
            // NO default: the stamp is an unconditional store, so a defaulted `1`
            // would REWIND a column already stamped at a later tick -- a silent
            // change-detection false negative (final review). The empty iterator
            // (no archetypes, enters no chunk) is the default constructor.
            Iterator(Archetype* const* archetypes, size_t archetypeCount, Tick now) noexcept
                : m_archetypes(archetypes)
                , m_archetypeCount(archetypeCount)
                , m_now(now)
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

                // Coarse change-detection stamp (spec §3.3 row 1): range-for yields
                // references like ForEach, so every non-const component column is
                // stamped on chunk entry. All-const iteration compiles to nothing.
                if constexpr ((Detail::IsMutableYield<Components> || ...))
                {
                    const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
                    ((Detail::IsMutableYield<Components>
                        ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<Components>>::Value()], m_now)
                        : void()), ...);
                }

                m_chunkEntityCount = chunk->GetCount();
                m_entityIndex = 0;
                m_entities = chunk->GetEntities().data();
                m_componentArrays = std::tuple{chunk->GetComponentArray<std::remove_const_t<Components>>()...};
            }

            // Empty components have no storage (array pointer is nullptr);
            // hand out a shared static instance instead — same contract as
            // Archetype::ForEach.
            template<typename T>
            ASTRA_FORCEINLINE T& DerefComponent(std::remove_const_t<T>* array) const noexcept
            {
                if constexpr (std::is_empty_v<std::remove_const_t<T>>)
                {
                    static std::remove_const_t<T> s_emptyInstance{};
                    return s_emptyInstance;
                }
                else
                {
                    return array[m_entityIndex];
                }
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
                    DerefComponent<Components>(std::get<Is>(m_componentArrays))...
                };
            }

            // View-level state
            Archetype* const* m_archetypes = nullptr;
            size_t m_archetypeCount = 0;
            Tick m_now = 0;   // change-detection tick stamped on chunk entry; 0 ("never") only in the empty iterator, which enters no chunk

            // Navigation state
            size_t m_archetypeIndex = 0;
            size_t m_chunkIndex = 0;
            size_t m_entityIndex = 0;

            // Cached chunk state
            size_t m_chunkEntityCount = 0;
            const Entity* m_entities = nullptr;
            std::tuple<std::remove_const_t<Components>*...> m_componentArrays{};
        };

        // `now`: the registry's current tick for the iterator's chunk-entry stamp
        // (see Iterator's constructor); no default, for the same reason.
        ViewIterable(Archetype* const* archetypes, size_t count, Tick now) noexcept
            : m_archetypes(archetypes)
            , m_archetypeCount(count)
            , m_now(now)
        {
        }

        ASTRA_FORCEINLINE Iterator begin() const noexcept
        {
            return Iterator(m_archetypes, m_archetypeCount, m_now);
        }

        ASTRA_FORCEINLINE ViewSentinel end() const noexcept
        {
            return ViewSentinel{};
        }

    private:
        Archetype* const* m_archetypes;
        size_t m_archetypeCount;
        Tick m_now;
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
