#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "../Core/Base.hpp"

#if defined(ARCH_X64) || defined(ARCH_X86)
    #if defined(ASTRA_COMPILER_MSVC)
        #include <intrin.h>
        #include <nmmintrin.h>  // SSE4.2
        #if defined(HAS_AVX) || defined(HAS_AVX2)
            #include <immintrin.h>  // AVX/AVX2
        #endif
    #else
        #include <x86intrin.h>
    #endif
#elif defined(ASTRA_ARCH_ARM64) || defined(ASTRA_ARCH_ARM32)
    #include <arm_neon.h>
    #if defined(__ARM_FEATURE_CRC32)
        #define ASTRA_HAS_ARM_CRC32 1
    #endif
#include <cstdarg>
#include <intrin0.inl.h>
#include "FlatMap.hpp"
#include "Platform.hpp"
#endif

namespace Astra
{

    namespace Simd
    {
        // Width tags for explicit control
        struct Width128 {};  // 16 bytes
        struct Width256 {};  // 32 bytes

        // Concept for valid SIMD widths
        template<typename T>
        concept SimdWidth = std::is_same_v<T, Width128> || std::is_same_v<T, Width256>;

        // Width traits
        template<typename Width>
        struct WidthTraits;

        template<>
        struct WidthTraits<Width128>
        {
            static constexpr size_t bytes = 16;
            using MaskType = uint16_t;
        };

        template<>
        struct WidthTraits<Width256>
        {
            static constexpr size_t bytes = 32;
            using MaskType = uint32_t;
        };

        // Alignment utilities
        template<SimdWidth Width>
        inline constexpr size_t AlignmentV = WidthTraits<Width>::bytes;

        template<SimdWidth Width>
        ASTRA_NODISCARD ASTRA_FORCEINLINE bool IsAligned(const void* ptr) noexcept
        {
            return (reinterpret_cast<uintptr_t>(ptr) & (AlignmentV<Width> - 1)) == 0;
        }

        // Capability detection
        namespace Capabilities
        {
            // Check if a specific width is supported with hardware acceleration
            template<SimdWidth Width>
            ASTRA_NODISCARD inline constexpr bool HasWidth() noexcept
            {
                if constexpr (std::is_same_v<Width, Width128>)
                {
#if defined(HAS_SSE2) || defined(HAS_NEON)
                    return true;
#else
                    return false;
#endif
                }
                else if constexpr (std::is_same_v<Width, Width256>)
                {
#if defined(HAS_AVX2)
                    return true;
#else
                    return false;
#endif
                }
                return false;
            }

            // Check if a specific width has optimized fallback
            template<SimdWidth Width>
            ASTRA_NODISCARD inline constexpr bool HasFallback() noexcept
            {
                if constexpr (std::is_same_v<Width, Width256>)
                {
                    // 256-bit can fall back to 2x128 if 128-bit is available
                    return HasWidth<Width128>();
                }
                return true; // Scalar fallback always available
            }

            // Get the best width for a given data size
            // This is for informational purposes only - users must still specify width explicitly
            ASTRA_NODISCARD inline constexpr size_t SuggestedWidthForSize(size_t data_size) noexcept
            {
                // For small data, 128-bit might be more efficient due to lower latency
                if (data_size <= 64)
                    return 16;

                // For larger data, use wider vectors if available
#if defined(HAS_AVX2)
                return 32;
#else
                return 16;
#endif
            }
        }

        namespace Ops
        {
            // ============== 128-bit implementations ==============
            namespace Detail128
            {
                ASTRA_FORCEINLINE uint16_t MatchByteMask_SSE(const void* data, uint8_t value) noexcept
                {
#if defined(HAS_SSE2)
                    const __m128i group = _mm_load_si128(static_cast<const __m128i*>(data));
                    const __m128i match = _mm_set1_epi8(static_cast<char>(value));
                    const __m128i eq = _mm_cmpeq_epi8(group, match);
                    return static_cast<uint16_t>(_mm_movemask_epi8(eq));
#else
                    return 0; // Should not reach here
#endif
                }

                ASTRA_FORCEINLINE uint16_t MatchByteMask_NEON(const void* data, uint8_t value) noexcept
                {
#if defined(HAS_NEON)
                    const uint8x16_t group = vld1q_u8(static_cast<const uint8_t*>(data));
                    const uint8x16_t match = vdupq_n_u8(value);
                    const uint8x16_t eq = vceqq_u8(group, match);

                    const uint8x16_t bit_mask = {
                        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
                    };
                    const uint8x16_t masked = vandq_u8(eq, bit_mask);
                    const uint8x8_t low = vget_low_u8(masked);
                    const uint8x8_t high = vget_high_u8(masked);

                    uint8_t low_mask = vaddv_u8(low);
                    uint8_t high_mask = vaddv_u8(high);
                    return (static_cast<uint16_t>(high_mask) << 8) | low_mask;
#else
                    return 0; // Should not reach here
#endif
                }

                ASTRA_FORCEINLINE uint16_t MatchByteMask_Scalar(const void* data, uint8_t value) noexcept
                {
                    uint16_t mask = 0;
                    const uint8_t* bytes = static_cast<const uint8_t*>(data);
                    for (int i = 0; i < 16; ++i)
                    {
                        if (bytes[i] == value)
                        {
                            mask |= (1u << i);
                        }
                    }
                    return mask;
                }

                ASTRA_FORCEINLINE uint16_t MatchEitherByteMask_SSE(const void* data, uint8_t val1, uint8_t val2) noexcept
                {
#if defined(HAS_SSE2)
                    const __m128i group = _mm_load_si128(static_cast<const __m128i*>(data));
                    const __m128i match1 = _mm_set1_epi8(static_cast<char>(val1));
                    const __m128i match2 = _mm_set1_epi8(static_cast<char>(val2));
                    const __m128i eq1 = _mm_cmpeq_epi8(group, match1);
                    const __m128i eq2 = _mm_cmpeq_epi8(group, match2);
                    const __m128i combined = _mm_or_si128(eq1, eq2);
                    return static_cast<uint16_t>(_mm_movemask_epi8(combined));
#else
                    return 0;
#endif
                }

                ASTRA_FORCEINLINE uint16_t MatchEitherByteMask_NEON(const void* data, uint8_t val1, uint8_t val2) noexcept
                {
#if defined(HAS_NEON)
                    const uint8x16_t group = vld1q_u8(static_cast<const uint8_t*>(data));
                    const uint8x16_t match1 = vdupq_n_u8(val1);
                    const uint8x16_t match2 = vdupq_n_u8(val2);
                    const uint8x16_t eq1 = vceqq_u8(group, match1);
                    const uint8x16_t eq2 = vceqq_u8(group, match2);
                    const uint8x16_t combined = vorrq_u8(eq1, eq2);

                    const uint8x16_t bit_mask = {
                        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
                    };
                    const uint8x16_t masked = vandq_u8(combined, bit_mask);
                    const uint8x8_t low = vget_low_u8(masked);
                    const uint8x8_t high = vget_high_u8(masked);

                    uint8_t low_mask = vaddv_u8(low);
                    uint8_t high_mask = vaddv_u8(high);
                    return (static_cast<uint16_t>(high_mask) << 8) | low_mask;
#else
                    return 0;
#endif
                }

                ASTRA_FORCEINLINE uint16_t MatchEitherByteMask_Scalar(const void* data, uint8_t val1, uint8_t val2) noexcept
                {
                    uint16_t mask = 0;
                    const uint8_t* bytes = static_cast<const uint8_t*>(data);
                    for (int i = 0; i < 16; ++i)
                    {
                        if (bytes[i] == val1 || bytes[i] == val2)
                        {
                            mask |= (1u << i);
                        }
                    }
                    return mask;
                }
            }

            // ============== 256-bit implementations ==============
            // Note: Fallback implementations are optimized for instruction scheduling:
            // - Both 128-bit loads are issued before any comparisons
            // - Match values are broadcast once and reused
            // - Operations are interleaved to hide latency
            namespace Detail256
            {
                ASTRA_FORCEINLINE uint32_t MatchByteMask_AVX(const void* data, uint8_t value) noexcept
                {
#if defined(HAS_AVX2)
                    const __m256i group = _mm256_load_si256(static_cast<const __m256i*>(data));
                    const __m256i match = _mm256_set1_epi8(static_cast<char>(value));
                    const __m256i eq = _mm256_cmpeq_epi8(group, match);
                    return static_cast<uint32_t>(_mm256_movemask_epi8(eq));
#else
                    return 0;
#endif
                }

                ASTRA_FORCEINLINE uint32_t MatchByteMask_Fallback(const void* data, uint8_t value) noexcept
                {
#if defined(HAS_SSE2)
                    // Optimized 2x128 with better scheduling
                    const uint8_t* ptr = static_cast<const uint8_t*>(data);
                    const __m128i* ptr_low = reinterpret_cast<const __m128i*>(ptr);
                    const __m128i* ptr_high = reinterpret_cast<const __m128i*>(ptr + 16);

                    // Load both halves first to hide latency
                    const __m128i group_low = _mm_load_si128(ptr_low);
                    const __m128i group_high = _mm_load_si128(ptr_high);

                    // Broadcast match value once
                    const __m128i match = _mm_set1_epi8(static_cast<char>(value));

                    // Compare both halves
                    const __m128i eq_low = _mm_cmpeq_epi8(group_low, match);
                    const __m128i eq_high = _mm_cmpeq_epi8(group_high, match);

                    // Extract masks
                    const uint16_t mask_low = static_cast<uint16_t>(_mm_movemask_epi8(eq_low));
                    const uint16_t mask_high = static_cast<uint16_t>(_mm_movemask_epi8(eq_high));

                    return (static_cast<uint32_t>(mask_high) << 16) | mask_low;
#else
                    // Fallback to function calls
                    auto mask_low = Detail128::MatchByteMask_Scalar(data, value);
                    auto mask_high = Detail128::MatchByteMask_Scalar(static_cast<const uint8_t*>(data) + 16, value);
                    return (static_cast<uint32_t>(mask_high) << 16) | mask_low;
#endif
                }

                ASTRA_FORCEINLINE uint32_t MatchEitherByteMask_AVX(const void* data, uint8_t val1, uint8_t val2) noexcept
                {
#if defined(HAS_AVX2)
                    const __m256i group = _mm256_load_si256(static_cast<const __m256i*>(data));
                    const __m256i match1 = _mm256_set1_epi8(static_cast<char>(val1));
                    const __m256i match2 = _mm256_set1_epi8(static_cast<char>(val2));
                    const __m256i eq1 = _mm256_cmpeq_epi8(group, match1);
                    const __m256i eq2 = _mm256_cmpeq_epi8(group, match2);
                    const __m256i combined = _mm256_or_si256(eq1, eq2);
                    return static_cast<uint32_t>(_mm256_movemask_epi8(combined));
#else
                    return 0;
#endif
                }

                ASTRA_FORCEINLINE uint32_t MatchEitherByteMask_Fallback(const void* data, uint8_t val1, uint8_t val2) noexcept
                {
#if defined(HAS_SSE2)
                    // Optimized 2x128 with better scheduling
                    const uint8_t* ptr = static_cast<const uint8_t*>(data);
                    const __m128i* ptr_low = reinterpret_cast<const __m128i*>(ptr);
                    const __m128i* ptr_high = reinterpret_cast<const __m128i*>(ptr + 16);

                    // Load both halves first
                    const __m128i group_low = _mm_load_si128(ptr_low);
                    const __m128i group_high = _mm_load_si128(ptr_high);

                    // Broadcast match values once
                    const __m128i match1 = _mm_set1_epi8(static_cast<char>(val1));
                    const __m128i match2 = _mm_set1_epi8(static_cast<char>(val2));

                    // Compare both values on both halves
                    const __m128i eq1_low = _mm_cmpeq_epi8(group_low, match1);
                    const __m128i eq2_low = _mm_cmpeq_epi8(group_low, match2);
                    const __m128i eq1_high = _mm_cmpeq_epi8(group_high, match1);
                    const __m128i eq2_high = _mm_cmpeq_epi8(group_high, match2);

                    // Combine results
                    const __m128i combined_low = _mm_or_si128(eq1_low, eq2_low);
                    const __m128i combined_high = _mm_or_si128(eq1_high, eq2_high);

                    // Extract masks
                    const uint16_t mask_low = static_cast<uint16_t>(_mm_movemask_epi8(combined_low));
                    const uint16_t mask_high = static_cast<uint16_t>(_mm_movemask_epi8(combined_high));

                    return (static_cast<uint32_t>(mask_high) << 16) | mask_low;
#else
                    auto mask_low = Detail128::MatchEitherByteMask_Scalar(data, val1, val2);
                    auto mask_high = Detail128::MatchEitherByteMask_Scalar(static_cast<const uint8_t*>(data) + 16, val1, val2);
                    return (static_cast<uint32_t>(mask_high) << 16) | mask_low;
#endif
                }
            }

            // ============== Public API ==============
            // IMPORTANT: All operations require explicit width specification
            // This ensures type safety and predictable behavior across platforms

            // Explicit width control
            template<SimdWidth Width>
            ASTRA_FORCEINLINE auto MatchByteMask(const void* data, uint8_t value) noexcept -> typename WidthTraits<Width>::MaskType
            {
                ASTRA_ASSERT(IsAligned<Width>(data), "Data must be aligned for SIMD operations");

                if constexpr (std::is_same_v<Width, Width128>)
                {
#if defined(HAS_SSE2)
                    return Detail128::MatchByteMask_SSE(data, value);
#elif defined(HAS_NEON)
                    return Detail128::MatchByteMask_NEON(data, value);
#else
                    return Detail128::MatchByteMask_Scalar(data, value);
#endif
                }
                else if constexpr (std::is_same_v<Width, Width256>)
                {
#if defined(HAS_AVX2)
                    return Detail256::MatchByteMask_AVX(data, value);
#elif defined(HAS_SSE2)
                    return Detail256::MatchByteMask_Fallback(data, value);
#else
                    // Scalar fallback for 256-bit
                    uint32_t mask = 0;
                    const uint8_t* bytes = static_cast<const uint8_t*>(data);
                    for (int i = 0; i < 32; ++i)
                    {
                        if (bytes[i] == value)
                        {
                            mask |= (1u << i);
                        }
                    }
                    return mask;
#endif
                }
            }

            template<SimdWidth Width>
            ASTRA_FORCEINLINE auto MatchEitherByteMask(const void* data, uint8_t val1, uint8_t val2) noexcept -> typename WidthTraits<Width>::MaskType
            {
                ASTRA_ASSERT(IsAligned<Width>(data), "Data must be aligned for SIMD operations");

                if constexpr (std::is_same_v<Width, Width128>)
                {
#if defined(HAS_SSE2)
                    return Detail128::MatchEitherByteMask_SSE(data, val1, val2);
#elif defined(HAS_NEON)
                    return Detail128::MatchEitherByteMask_NEON(data, val1, val2);
#else
                    return Detail128::MatchEitherByteMask_Scalar(data, val1, val2);
#endif
                }
                else if constexpr (std::is_same_v<Width, Width256>)
                {
#if defined(HAS_AVX2)
                    return Detail256::MatchEitherByteMask_AVX(data, val1, val2);
#elif defined(HAS_SSE2)
                    return Detail256::MatchEitherByteMask_Fallback(data, val1, val2);
#else
                    // Scalar fallback for 256-bit
                    uint32_t mask = 0;
                    const uint8_t* bytes = static_cast<const uint8_t*>(data);
                    for (int i = 0; i < 32; ++i)
                    {
                        if (bytes[i] == val1 || bytes[i] == val2)
                        {
                            mask |= (1u << i);
                        }
                    }
                    return mask;
#endif
                }
            }

            // Population count (number of set bits)
            template<typename MaskType>
            ASTRA_FORCEINLINE int PopCount(MaskType mask) noexcept
            {
#if defined(ASTRA_COMPILER_MSVC)
                if constexpr (sizeof(MaskType) <= 4)
                {
                    return static_cast<int>(__popcnt(static_cast<unsigned>(mask)));
                }
                else
                {
#ifdef ASTRA_PLATFORM_WIN64
                    return static_cast<int>(__popcnt64(static_cast<unsigned long long>(mask)));
#else
                    return __popcnt(static_cast<unsigned>(mask)) + 
                        __popcnt(static_cast<unsigned>(mask >> 32));
#endif
                }
// Original: #elif ASTRA_HAS_BUILTIN(__builtin_popcount) || ASTRA_HAS_BUILTIN(__builtin_popcountll)
#elif (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_popcount)) || (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_popcountll))
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

            // Find first set bit (1-indexed, returns 0 if no bits set)
            template<typename MaskType>
            ASTRA_FORCEINLINE int FindFirstSet(MaskType mask) noexcept
            {
                if (!mask) return 0;
                return CountTrailingZeros(mask) + 1;
            }

            // Find last set bit (1-indexed, returns 0 if no bits set)
            template<typename MaskType>
            ASTRA_FORCEINLINE int FindLastSet(MaskType mask) noexcept
            {
                if (!mask) return 0;

#if defined(ASTRA_COMPILER_MSVC)
                unsigned long idx;
                if constexpr (sizeof(MaskType) <= 4)
                {
                    _BitScanReverse(&idx, static_cast<unsigned long>(mask));
                    return static_cast<int>(idx) + 1;
                }
                else
                {
#ifdef _WIN64
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
// Original: #elif ASTRA_HAS_BUILTIN(__builtin_clz) || HAS_BUILTIN(__builtin_clzll)
#elif (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_clz)) || (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_clzll))
                if constexpr (sizeof(MaskType) <= 4)
                {
                    return 32 - __builtin_clz(static_cast<unsigned>(mask));
                }
                else
                {
                    return 64 - __builtin_clzll(static_cast<unsigned long long>(mask));
                }
#else
                // Fallback
                int pos = 0;
                while (mask)
                {
                    pos++;
                    mask >>= 1;
                }
                return pos;
#endif
            }

            // Count trailing zeros - works with any mask type
            template<typename MaskType>
            ASTRA_FORCEINLINE int CountTrailingZeros(MaskType mask) noexcept
            {
                if (!mask) return sizeof(MaskType) * 8;

#if defined(ASTRA_COMPILER_MSVC)
                unsigned long idx;
                if constexpr (sizeof(MaskType) <= 4)
                {
                    _BitScanForward(&idx, static_cast<unsigned long>(mask));
                }
                else
                {
#ifdef _WIN64
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
// Original: #elif ASTRA_HAS_BUILTIN(__builtin_ctz) || HAS_BUILTIN(__builtin_ctzll)
#elif (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_ctz)) || (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_ctzll))
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

            // 128-bit SIMD operations for Bitmap optimization
#if defined(HAS_SSE2)
            using Int128 = __m128i;
#elif defined(HAS_NEON)
            using Int128 = uint8x16_t;
#else
            struct Int128 {
                uint64_t low;
                uint64_t high;
            };
#endif

            // Load 128 bits from memory
            ASTRA_FORCEINLINE Int128 Load128(const void* ptr) noexcept
            {
#if defined(HAS_SSE2)
                return _mm_load_si128(static_cast<const __m128i*>(ptr));
#elif defined(HAS_NEON)
                return vld1q_u8(static_cast<const uint8_t*>(ptr));
#else
                const uint64_t* p = static_cast<const uint64_t*>(ptr);
                return {p[0], p[1]};
#endif
            }

            // Bitwise AND of 128-bit values
            ASTRA_FORCEINLINE Int128 And128(Int128 a, Int128 b) noexcept
            {
#if defined(HAS_SSE2)
                return _mm_and_si128(a, b);
#elif defined(HAS_NEON)
                return vandq_u8(a, b);
#else
                return {a.low & b.low, a.high & b.high};
#endif
            }

            // Bitwise OR of 128-bit values
            ASTRA_FORCEINLINE Int128 Or128(Int128 a, Int128 b) noexcept
            {
#if defined(HAS_SSE2)
                return _mm_or_si128(a, b);
#elif defined(HAS_NEON)
                return vorrq_u8(a, b);
#else
                return {a.low | b.low, a.high | b.high};
#endif
            }

            // Store 128 bits to memory
            ASTRA_FORCEINLINE void Store128(void* ptr, Int128 value) noexcept
            {
#if defined(HAS_SSE2)
                _mm_store_si128(static_cast<__m128i*>(ptr), value);
#elif defined(HAS_NEON)
                vst1q_u8(static_cast<uint8_t*>(ptr), value);
#else
                uint64_t* p = static_cast<uint64_t*>(ptr);
                p[0] = value.low;
                p[1] = value.high;
#endif
            }

            // Compare 128-bit values for equality (returns mask)
            ASTRA_FORCEINLINE Int128 CompareEqual128(Int128 a, Int128 b) noexcept
            {
#if defined(HAS_SSE2)
                return _mm_cmpeq_epi64(a, b);
#elif defined(HAS_NEON)
                return vreinterpretq_u8_u64(vceqq_u64(vreinterpretq_u64_u8(a), vreinterpretq_u64_u8(b)));
#else
                Int128 result;
                result.low = (a.low == b.low) ? ~0ULL : 0;
                result.high = (a.high == b.high) ? ~0ULL : 0;
                return result;
#endif
            }

            // Test if two 128-bit values are equal
            ASTRA_FORCEINLINE bool TestEqual128(Int128 a, Int128 b) noexcept
            {
#if defined(HAS_SSE2)
                __m128i cmp = _mm_cmpeq_epi8(a, b);
                return _mm_movemask_epi8(cmp) == 0xFFFF;
#elif defined(HAS_NEON)
                uint8x16_t cmp = vceqq_u8(a, b);
                // Check if all bytes are equal
                uint8x8_t low = vget_low_u8(cmp);
                uint8x8_t high = vget_high_u8(cmp);
                uint64_t low64 = vget_lane_u64(vreinterpret_u64_u8(low), 0);
                uint64_t high64 = vget_lane_u64(vreinterpret_u64_u8(high), 0);
                return low64 == 0xFFFFFFFFFFFFFFFFULL && high64 == 0xFFFFFFFFFFFFFFFFULL;
#else
                return a.low == b.low && a.high == b.high;
#endif
            }

            // Test if (a & b) == a (i.e., a is subset of b)
            ASTRA_FORCEINLINE bool TestSubset128(Int128 a, Int128 b) noexcept
            {
                Int128 result = And128(a, b);
                return TestEqual128(result, a);
            }

            // 256-bit SIMD operations (for extended bloom filters)
#if defined(HAS_AVX2)
            using Int256 = __m256i;
#else
            struct Int256 {
                Int128 low;
                Int128 high;
            };
#endif

            // Load 256 bits from memory
            ASTRA_FORCEINLINE Int256 Load256(const void* ptr) noexcept
            {
#if defined(HAS_AVX2)
                return _mm256_load_si256(static_cast<const __m256i*>(ptr));
#else
                const uint8_t* p = static_cast<const uint8_t*>(ptr);
                return {Load128(p), Load128(p + 16)};
#endif
            }

            // Store 256 bits to memory
            ASTRA_FORCEINLINE void Store256(void* ptr, Int256 value) noexcept
            {
#if defined(HAS_AVX2)
                _mm256_store_si256(static_cast<__m256i*>(ptr), value);
#else
                uint8_t* p = static_cast<uint8_t*>(ptr);
                Store128(p, value.low);
                Store128(p + 16, value.high);
#endif
            }

            // Bitwise AND of 256-bit values
            ASTRA_FORCEINLINE Int256 And256(Int256 a, Int256 b) noexcept
            {
#if defined(HAS_AVX2)
                return _mm256_and_si256(a, b);
#else
                return {And128(a.low, b.low), And128(a.high, b.high)};
#endif
            }

            // Bitwise OR of 256-bit values
            ASTRA_FORCEINLINE Int256 Or256(Int256 a, Int256 b) noexcept
            {
#if defined(HAS_AVX2)
                return _mm256_or_si256(a, b);
#else
                return {Or128(a.low, b.low), Or128(a.high, b.high)};
#endif
            }

            // Compare 256-bit values for equality
            ASTRA_FORCEINLINE Int256 CompareEqual256(Int256 a, Int256 b) noexcept
            {
#if defined(HAS_AVX2)
                return _mm256_cmpeq_epi64(a, b);
#else
                return {CompareEqual128(a.low, b.low), CompareEqual128(a.high, b.high)};
#endif
            }

            // Test if two 256-bit values are equal
            ASTRA_FORCEINLINE bool TestEqual256(Int256 a, Int256 b) noexcept
            {
#if defined(HAS_AVX2)
                __m256i cmp = _mm256_cmpeq_epi64(a, b);
                return _mm256_movemask_epi8(cmp) == 0xFFFFFFFF;
#else
                return TestEqual128(a.low, b.low) && TestEqual128(a.high, b.high);
#endif
            }

            // Test if (a & b) == a (i.e., a is subset of b)
            ASTRA_FORCEINLINE bool TestSubset256(Int256 a, Int256 b) noexcept
            {
                Int256 result = And256(a, b);
                return TestEqual256(result, a);
            }

            // Hardware CRC32 or high-quality fallback
            ASTRA_FORCEINLINE uint64_t HashCombine(uint64_t seed, uint64_t value) noexcept
            {
#if defined(ASTRA_HAS_SSE42)
                return _mm_crc32_u64(seed, value);
#elif defined(ASTRA_HAS_ARM_CRC32)
                return __crc32cd(static_cast<uint32_t>(seed), value);
#else
                // MurmurHash3 finalizer
                uint64_t h = seed ^ value;
                h ^= h >> 33;
                h *= 0xff51afd7ed558ccdULL;
                h ^= h >> 33;
                h *= 0xc4ceb9fe1a85ec53ULL;
                h ^= h >> 33;
                return h;
#endif
            }

            // Prefetch operations
            // 
            // Optimal prefetch distance depends on:
            // - Memory bandwidth vs computation ratio
            // - Cache sizes (L1: ~32KB, L2: ~256KB, L3: ~8MB typical)
            // - Access pattern (sequential vs random)
            //
            // Guidelines:
            // - Light computation: prefetch 2-4 cache lines ahead (~128-256 bytes)
            // - Heavy computation: prefetch 8-16 cache lines ahead (~512-1024 bytes)
            // - Use T0 for data needed in next few iterations
            // - Use T1 for data needed in ~10-50 iterations
            // - Use T2 for data needed in ~100+ iterations
            // - Use NTA for streaming data that won't be reused
            //
            // Example distances for different scenarios:
            // - Simple byte matching: prefetch 256-512 bytes ahead
            // - Complex pattern matching: prefetch 1-2KB ahead
            // - Video/image processing: prefetch next row/tile
            //
            enum class PrefetchHint
            {
                T0 = 0,  // Prefetch to all cache levels (use for immediate data)
                T1 = 1,  // Prefetch to L2 and higher (use for near-future data)
                T2 = 2,  // Prefetch to L3 and higher (use for far-future data)
                NTA = 3  // Non-temporal (use for streaming, write-once data)
            };

            ASTRA_FORCEINLINE void PrefetchRead(const void* ptr, PrefetchHint hint = PrefetchHint::T0) noexcept
            {
#if defined(ARCH_X64) || defined(ARCH_X86)
#if defined(ASTRA_COMPILER_MSVC)
                switch (hint)
                {
                case PrefetchHint::T0:  _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0); break;
                case PrefetchHint::T1:  _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T1); break;
                case PrefetchHint::T2:  _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T2); break;
                case PrefetchHint::NTA: _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_NTA); break;
                }
#else
                switch (hint)
                {
                case PrefetchHint::T0:  __builtin_prefetch(ptr, 0, 3); break;
                case PrefetchHint::T1:  __builtin_prefetch(ptr, 0, 2); break;
                case PrefetchHint::T2:  __builtin_prefetch(ptr, 0, 1); break;
                case PrefetchHint::NTA: __builtin_prefetch(ptr, 0, 0); break;
                }
#endif
// Original: #elif ASTRA_HAS_BUILTIN(__builtin_prefetch)
#elif (defined(ASTRA_HAS_BUILTIN) && ASTRA_HAS_BUILTIN(__builtin_prefetch))
                // GCC fix: __builtin_prefetch requires constant third argument
                switch (hint)
                {
                case PrefetchHint::T0:  __builtin_prefetch(ptr, 0, 3); break;
                case PrefetchHint::T1:  __builtin_prefetch(ptr, 0, 2); break;
                case PrefetchHint::T2:  __builtin_prefetch(ptr, 0, 1); break;
                case PrefetchHint::NTA: __builtin_prefetch(ptr, 0, 0); break;
                }
#elif defined(HAS_NEON) && defined(__ARM_FEATURE_UNALIGNED)
                __pld(ptr);
#else
                (void)ptr;
                (void)hint;
#endif
            }

            ASTRA_FORCEINLINE void PrefetchT0(const void* ptr) noexcept { PrefetchRead(ptr, PrefetchHint::T0); }
            ASTRA_FORCEINLINE void PrefetchT1(const void* ptr) noexcept { PrefetchRead(ptr, PrefetchHint::T1); }
            ASTRA_FORCEINLINE void PrefetchT2(const void* ptr) noexcept { PrefetchRead(ptr, PrefetchHint::T2); }
            ASTRA_FORCEINLINE void PrefetchNTA(const void* ptr) noexcept { PrefetchRead(ptr, PrefetchHint::NTA); }

            // ============== Batch Operations ==============

            // Process multiple vectors in a batch for better performance
            template<SimdWidth Width, size_t BatchSize = 4>
            struct BatchOps
            {
                using MaskType = typename WidthTraits<Width>::MaskType;
                static constexpr size_t stride = WidthTraits<Width>::bytes;

                // Batch match with prefetching
                static ASTRA_FORCEINLINE void MatchByteMaskBatch(
                    const void* data,
                    uint8_t value,
                    MaskType* results,
                    size_t count) noexcept
                {
                    const uint8_t* ptr = static_cast<const uint8_t*>(data);

                    // Process in batches with prefetching
                    size_t i = 0;
                    for (; i + BatchSize <= count; i += BatchSize)
                    {
                        // Prefetch next batch
                        if (i + BatchSize < count)
                        {
                            PrefetchT0(ptr + (i + BatchSize) * stride);
                            if constexpr (BatchSize >= 2)
                                PrefetchT0(ptr + (i + BatchSize + 1) * stride);
                        }

                        // Process current batch - unrolled
                        results[i] = MatchByteMask<Width>(ptr + i * stride, value);
                        if constexpr (BatchSize >= 2)
                            results[i + 1] = MatchByteMask<Width>(ptr + (i + 1) * stride, value);
                        if constexpr (BatchSize >= 4)
                        {
                            results[i + 2] = MatchByteMask<Width>(ptr + (i + 2) * stride, value);
                            results[i + 3] = MatchByteMask<Width>(ptr + (i + 3) * stride, value);
                        }
                    }

                    // Process remaining
                    for (; i < count; ++i)
                    {
                        results[i] = MatchByteMask<Width>(ptr + i * stride, value);
                    }
                }

                // Batch match either with prefetching
                static ASTRA_FORCEINLINE void MatchEitherByteMaskBatch(
                    const void* data,
                    uint8_t val1,
                    uint8_t val2,
                    MaskType* results,
                    size_t count) noexcept
                {
                    const uint8_t* ptr = static_cast<const uint8_t*>(data);

                    size_t i = 0;
                    for (; i + BatchSize <= count; i += BatchSize)
                    {
                        // Prefetch next batch
                        if (i + BatchSize < count)
                        {
                            PrefetchT0(ptr + (i + BatchSize) * stride);
                            if constexpr (BatchSize >= 2)
                                PrefetchT0(ptr + (i + BatchSize + 1) * stride);
                        }

                        // Process current batch
                        results[i] = MatchEitherByteMask<Width>(ptr + i * stride, val1, val2);
                        if constexpr (BatchSize >= 2)
                            results[i + 1] = MatchEitherByteMask<Width>(ptr + (i + 1) * stride, val1, val2);
                        if constexpr (BatchSize >= 4)
                        {
                            results[i + 2] = MatchEitherByteMask<Width>(ptr + (i + 2) * stride, val1, val2);
                            results[i + 3] = MatchEitherByteMask<Width>(ptr + (i + 3) * stride, val1, val2);
                        }
                    }

                    // Process remaining
                    for (; i < count; ++i)
                    {
                        results[i] = MatchEitherByteMask<Width>(ptr + i * stride, val1, val2);
                    }
                }

                // Find first match in batch
                static ASTRA_FORCEINLINE int FindFirstMatchInBatch(
                    const void* data,
                    uint8_t value,
                    size_t count) noexcept
                {
                    const uint8_t* ptr = static_cast<const uint8_t*>(data);

                    for (size_t i = 0; i < count; ++i)
                    {
                        auto mask = MatchByteMask<Width>(ptr + i * stride, value);
                        if (mask)
                        {
                            int bit_pos = CountTrailingZeros(mask);
                            return static_cast<int>(i * stride + bit_pos);
                        }

                        // Prefetch ahead
                        if (i + 2 < count)
                            PrefetchT1(ptr + (i + 2) * stride);
                    }

                    return -1; // Not found
                }
            };
        }
    }
}
