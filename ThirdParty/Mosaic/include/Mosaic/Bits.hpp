#pragma once

// Mosaic::Bits -- scalar bit-twiddling primitives over plain integer masks:
// population count, trailing/leading bit scans. ISA-accelerated where the target
// offers an instruction (MSVC intrinsics / GCC-Clang builtins), with a portable
// fallback everywhere else. Zero dependencies beyond <Mosaic/Platform.hpp>.
//
// These are the natural companions of the Simd byte-match toolbox (a match
// returns a bitmask, and you then scan it), so <Mosaic/Simd/Bits.hpp> re-exports
// them into Mosaic::Simd::Ops. They are useful on their own, hence a separate
// header with no SIMD/intrinsics baggage.
//
// Speed-first, NOT determinism-critical: unlike Mosaic::Simd (Wide), these are
// exact integer operations whose results are identical on every backend by
// construction. Move origin: Astra Core/Simd.hpp.

#include <Mosaic/Platform.hpp>

#include <cstdint>

#if defined(MOSAIC_COMPILER_MSVC) && (defined(MOSAIC_ARCH_X64) || defined(MOSAIC_ARCH_X86))
    #include <intrin.h>   // __popcnt / _BitScanForward / _BitScanReverse
#endif

namespace Mosaic
{
    namespace Bits
    {
        // Count trailing zeros. Returns the mask's bit-width when mask == 0.
        template<typename MaskType>
        MOSAIC_FORCEINLINE int CountTrailingZeros(MaskType mask) noexcept
        {
            if (!mask) return sizeof(MaskType) * 8;

#if defined(MOSAIC_COMPILER_MSVC)
            unsigned long idx;
            if constexpr (sizeof(MaskType) <= 4)
            {
                _BitScanForward(&idx, static_cast<unsigned long>(mask));
            }
            else
            {
    #if defined(MOSAIC_PLATFORM_WIN64)
                _BitScanForward64(&idx, static_cast<unsigned long long>(mask));
    #else
                if (static_cast<uint32_t>(mask))
                {
                    _BitScanForward(&idx, static_cast<uint32_t>(mask));
                }
                else
                {
                    _BitScanForward(&idx, static_cast<uint32_t>(mask >> 32));
                    idx += 32;
                }
    #endif
            }
            return static_cast<int>(idx);
#elif MOSAIC_HAS_BUILTIN(__builtin_ctz) || MOSAIC_HAS_BUILTIN(__builtin_ctzll)
            if constexpr (sizeof(MaskType) <= 4)
            {
                return __builtin_ctz(static_cast<unsigned>(mask));
            }
            else
            {
                return __builtin_ctzll(static_cast<unsigned long long>(mask));
            }
#else
            int count = 0;
            while ((mask & 1) == 0)
            {
                mask >>= 1;
                ++count;
            }
            return count;
#endif
        }

        // Population count (number of set bits).
        template<typename MaskType>
        MOSAIC_FORCEINLINE int PopCount(MaskType mask) noexcept
        {
#if defined(MOSAIC_COMPILER_MSVC)
            if constexpr (sizeof(MaskType) <= 4)
            {
                return static_cast<int>(__popcnt(static_cast<unsigned>(mask)));
            }
            else
            {
    #if defined(MOSAIC_PLATFORM_WIN64)
                return static_cast<int>(__popcnt64(static_cast<unsigned long long>(mask)));
    #else
                return static_cast<int>(__popcnt(static_cast<unsigned>(mask))) +
                       static_cast<int>(__popcnt(static_cast<unsigned>(mask >> 32)));
    #endif
            }
#elif MOSAIC_HAS_BUILTIN(__builtin_popcount) || MOSAIC_HAS_BUILTIN(__builtin_popcountll)
            if constexpr (sizeof(MaskType) <= 4)
            {
                return __builtin_popcount(static_cast<unsigned>(mask));
            }
            else
            {
                return __builtin_popcountll(static_cast<unsigned long long>(mask));
            }
#else
            // Brian Kernighan's algorithm
            int count = 0;
            while (mask)
            {
                mask &= mask - 1;
                count++;
            }
            return count;
#endif
        }

        // Find first set bit (1-indexed; 0 when no bits are set).
        template<typename MaskType>
        MOSAIC_FORCEINLINE int FindFirstSet(MaskType mask) noexcept
        {
            if (!mask) return 0;
            return CountTrailingZeros(mask) + 1;
        }

        // Find last set bit (1-indexed; 0 when no bits are set).
        template<typename MaskType>
        MOSAIC_FORCEINLINE int FindLastSet(MaskType mask) noexcept
        {
            if (!mask) return 0;

#if defined(MOSAIC_COMPILER_MSVC)
            unsigned long idx;
            if constexpr (sizeof(MaskType) <= 4)
            {
                _BitScanReverse(&idx, static_cast<unsigned long>(mask));
                return static_cast<int>(idx) + 1;
            }
            else
            {
    #if defined(MOSAIC_PLATFORM_WIN64)
                _BitScanReverse64(&idx, static_cast<unsigned long long>(mask));
                return static_cast<int>(idx) + 1;
    #else
                if (mask >> 32)
                {
                    _BitScanReverse(&idx, static_cast<unsigned>(mask >> 32));
                    return static_cast<int>(idx) + 33;
                }
                else
                {
                    _BitScanReverse(&idx, static_cast<unsigned>(mask));
                    return static_cast<int>(idx) + 1;
                }
    #endif
            }
#elif MOSAIC_HAS_BUILTIN(__builtin_clz) || MOSAIC_HAS_BUILTIN(__builtin_clzll)
            if constexpr (sizeof(MaskType) <= 4)
            {
                return 32 - __builtin_clz(static_cast<unsigned>(mask));
            }
            else
            {
                return 64 - __builtin_clzll(static_cast<unsigned long long>(mask));
            }
#else
            int pos = 0;
            while (mask)
            {
                pos++;
                mask >>= 1;
            }
            return pos;
#endif
        }
    }
}
