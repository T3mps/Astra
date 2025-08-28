#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Simd.hpp"

namespace Astra
{
    template<size_t Bits>
    class alignas(CACHE_LINE_SIZE) Bitmap
    {
    public:
        static constexpr size_t BITS_PER_WORD = 64;
        static constexpr size_t WORD_COUNT = (Bits + BITS_PER_WORD - 1) / BITS_PER_WORD;
        static constexpr size_t SIMD_WORDS = 2;
        
        using Word = std::uint64_t;
        
        Bitmap() noexcept
        {
            std::memset(m_words.data(), 0, sizeof(m_words));
        }
        
        void Set(size_t index) noexcept
        {
            if (index < Bits) ASTRA_LIKELY
            {
                const size_t word = index / BITS_PER_WORD;
                const size_t bit = index % BITS_PER_WORD;
                m_words[word] |= (Word(1) << bit);
            }
        }
        
        void Reset(size_t index) noexcept
        {
            if (index < Bits) ASTRA_LIKELY
            {
                const size_t word = index / BITS_PER_WORD;
                const size_t bit = index % BITS_PER_WORD;
                m_words[word] &= ~(Word(1) << bit);
            }
        }
        
        ASTRA_NODISCARD bool Test(size_t index) const noexcept
        {
            if (index >= Bits) ASTRA_UNLIKELY
                return false;
            const size_t word = index / BITS_PER_WORD;
            const size_t bit = index % BITS_PER_WORD;
            return (m_words[word] & (Word(1) << bit)) != 0;
        }
        
        ASTRA_NODISCARD bool HasAll(const Bitmap& mask) const noexcept
        {
            if constexpr (WORD_COUNT >= SIMD_WORDS)
            {
                auto this128 = Simd::Ops::Load128(m_words.data());
                auto mask128 = Simd::Ops::Load128(mask.m_words.data());
                auto result = Simd::Ops::And128(this128, mask128);
                
                if (!Simd::Ops::TestEqual128(result, mask128)) ASTRA_UNLIKELY
                {
                    return false;
                }
                    
                for (size_t i = SIMD_WORDS; i < WORD_COUNT; ++i)
                {
                    if ((m_words[i] & mask.m_words[i]) != mask.m_words[i]) ASTRA_UNLIKELY
                    {
                        return false;
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < WORD_COUNT; ++i)
                {
                    if ((m_words[i] & mask.m_words[i]) != mask.m_words[i]) ASTRA_UNLIKELY
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        
        ASTRA_NODISCARD bool operator==(const Bitmap& other) const noexcept
        {
            if constexpr (WORD_COUNT >= SIMD_WORDS)
            {
                auto this128 = Simd::Ops::Load128(m_words.data());
                auto other128 = Simd::Ops::Load128(other.m_words.data());
                
                if (!Simd::Ops::TestEqual128(this128, other128)) ASTRA_UNLIKELY
                {
                    return false;
                }
                    
                for (size_t i = SIMD_WORDS; i < WORD_COUNT; ++i)
                {
                    if (m_words[i] != other.m_words[i]) ASTRA_UNLIKELY
                    {
                        return false;
                    }
                }
            }
            else
            {
                return std::memcmp(m_words.data(), other.m_words.data(), sizeof(Word) * WORD_COUNT) == 0;
            }
            return true;
        }
        
        // Bitwise AND
        ASTRA_NODISCARD Bitmap operator&(const Bitmap& other) const noexcept
        {
            Bitmap result;
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                result.m_words[i] = m_words[i] & other.m_words[i];
            }
            return result;
        }
        
        // Bitwise OR (union)
        ASTRA_NODISCARD Bitmap operator|(const Bitmap& other) const noexcept
        {
            Bitmap result;
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                result.m_words[i] = m_words[i] | other.m_words[i];
            }
            return result;
        }
        
        // Bitwise OR assignment (union in-place)
        Bitmap& operator|=(const Bitmap& other) noexcept
        {
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                m_words[i] |= other.m_words[i];
            }
            return *this;
        }
        
        // Count set bits
        ASTRA_NODISCARD size_t Count() const noexcept
        {
            size_t count = 0;
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                count += std::popcount(m_words[i]);
            }
            return count;
        }
        
        // Check if any bits are set
        ASTRA_NODISCARD bool Any() const noexcept
        {
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                if (m_words[i] != 0) ASTRA_UNLIKELY return true;
            }
            return false;
        }
        
        // Check if no bits are set
        ASTRA_NODISCARD bool None() const noexcept
        {
            return !Any();
        }
        
        ASTRA_NODISCARD size_t GetHash() const noexcept
        {
            // Use CRC32 when available for better performance and distribution
            size_t hash = 0;
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                hash = Simd::Ops::HashCombine(hash, m_words[i]);
            }
            return hash;
        }
        
        // Get raw data for SIMD operations
        ASTRA_NODISCARD const Word* Data() const noexcept { return m_words.data(); }
        ASTRA_NODISCARD Word* Data() noexcept { return m_words.data(); }
        
        // SIMD batch check: Check if multiple bitmaps have all bits from mask
        // Returns a bitmask where bit i is set if bitmaps[i].HasAll(mask)
        ASTRA_NODISCARD static uint32_t BatchHasAll(const Bitmap* bitmaps, size_t count, const Bitmap& mask) noexcept
        {
            if (count == 0) ASTRA_UNLIKELY
                return 0;
            
            uint32_t results = 0;
            
            // Process up to 32 bitmaps (limited by result bitmask size)
            const size_t processCount = std::min(count, size_t(32));
            
            // For each bitmap
            for (size_t i = 0; i < processCount; ++i)
            {
                if (bitmaps[i].HasAll(mask)) ASTRA_UNLIKELY
                {
                    results |= (uint32_t(1) << i);
                }
            }
            
            return results;
        }
        
    private:
        std::array<Word, WORD_COUNT> m_words;
    };
    
    template<size_t Bits>
    struct BitmapHash
    {
        size_t operator()(const Bitmap<Bits>& bitmap) const noexcept
        {
            return bitmap.GetHash();
        }
    };
}
