#pragma once

// Mosaic::BitSet -- a minimal grow-only fixed-capacity bit set (the b2BitSet
// equivalent). Move origin: Manifold2D Core/BitSet.hpp, where it backs the
// narrowphase MT serial tail: each worker flags changed contact ids into its OWN
// BitSet, then the tail OR-reduces (InPlaceUnion) and walks set bits in ascending
// order (ForEachSetBit, via std::countr_zero == Box2D's b2CTZ64). That shape --
// per-worker scratch, lock-free, reduced once -- is generic enough to belong in
// the shared core rather than in one engine.
//
// Presentation-free, C++23-clean: std only.

#include <Mosaic/Assert.hpp>   // MOSAIC_ASSERT (bounds guard)

#include <bit>        // std::countr_zero
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Mosaic
{
    class BitSet
    {
    public:
        // Size to hold [0, bitCount) bits. Grows the block backing; never shrinks
        // capacity (grow-only -> zero steady-state alloc when reused per step).
        void Resize(std::size_t bitCount)
        {
            const std::size_t blocks = (bitCount + 63u) / 64u;
            if (blocks > m_blocks.size())
            {
                m_blocks.resize(blocks, 0ull);
            }
            m_blockCount = blocks;
        }

        void ClearAll() noexcept
        {
            for (std::size_t i = 0; i < m_blockCount; ++i) { m_blocks[i] = 0ull; }
        }

        void Set(std::size_t i) noexcept
        {
            // Bounds guard: i must be < capacity (m_blockCount * 64); an out-of-range
            // index is an OOB write into m_blocks (UB). Debug-only -- the release path
            // stays branch-free (MOSAIC_ASSERT compiles out under NDEBUG).
            // Resize(bitCount) must precede any Set.
            MOSAIC_ASSERT(i < m_blockCount * 64u, "BitSet::Set: index out of range (call Resize first)");
            m_blocks[i >> 6] |= (1ull << (i & 63u));
        }

        // OR `other` into this. Both must have been Resize()d to the same bitCount.
        void InPlaceUnion(const BitSet& other) noexcept
        {
            const std::size_t n = m_blockCount < other.m_blockCount
                                      ? m_blockCount : other.m_blockCount;
            for (std::size_t i = 0; i < n; ++i) { m_blocks[i] |= other.m_blocks[i]; }
        }

        // Call fn(id) for every set bit, ascending id order (block scan + CTZ).
        template <class Fn>
        void ForEachSetBit(Fn&& fn) const
        {
            for (std::size_t k = 0; k < m_blockCount; ++k)
            {
                std::uint64_t bits = m_blocks[k];
                while (bits != 0ull)
                {
                    const std::uint32_t ctz = static_cast<std::uint32_t>(std::countr_zero(bits));
                    fn(static_cast<std::uint32_t>(64u * k) + ctz);
                    bits &= (bits - 1ull); // clear lowest set bit
                }
            }
        }

    private:
        std::vector<std::uint64_t> m_blocks;
        std::size_t                m_blockCount = 0;
    };
} // namespace Mosaic
