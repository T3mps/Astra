// 8-wide AVX2 backend (FMA). Included by Simd.hpp under __AVX2__. Never compiled
// standalone.
#include <immintrin.h>
#include <cassert>
#include <cstdint>

namespace Mosaic { namespace Simd { namespace Avx2 {

inline constexpr const char* kBackendName = "AVX2";

struct f32w { __m256  v; static constexpr int width = 8; };
struct b32w { __m256  v; static constexpr int width = 8; }; // lane mask = all-1s/all-0s float bits
struct i32w { __m256i v; static constexpr int width = 8; };

MOSAIC_FORCEINLINE f32w splat(float x)    noexcept { return f32w{ _mm256_set1_ps(x) }; }
MOSAIC_FORCEINLINE f32w setzero()         noexcept { return f32w{ _mm256_setzero_ps() }; }
MOSAIC_FORCEINLINE i32w isplat(int32_t x) noexcept { return i32w{ _mm256_set1_epi32(x) }; }
MOSAIC_FORCEINLINE i32w iota()            noexcept { return i32w{ _mm256_setr_epi32(0,1,2,3,4,5,6,7) }; }
// Aligned load of `width` int32 lanes from a 32-byte-aligned array (the SoA
// body-index arrays the solver gathers/scatters by are alignas(32)). Exact load
// -> bit-identical (no rounding). The contact-solver builds its gather/scatter
// index vector from a plain int32_t[width] array via this op.
MOSAIC_FORCEINLINE i32w iload(const int32_t* p) noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); return i32w{ _mm256_load_si256(reinterpret_cast<const __m256i*>(p)) }; }
MOSAIC_FORCEINLINE void istore(int32_t* p, i32w v) noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); _mm256_store_si256(reinterpret_cast<__m256i*>(p), v.v); }

MOSAIC_FORCEINLINE f32w load(const float* p)    noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); return f32w{ _mm256_load_ps(p) }; }
MOSAIC_FORCEINLINE void store(float* p, f32w a)  noexcept { assert((reinterpret_cast<std::uintptr_t>(p) & 31u) == 0); _mm256_store_ps(p, a.v); }
MOSAIC_FORCEINLINE f32w loadu(const float* p)    noexcept { return f32w{ _mm256_loadu_ps(p) }; }
MOSAIC_FORCEINLINE void storeu(float* p, f32w a)  noexcept { _mm256_storeu_ps(p, a.v); }

MOSAIC_FORCEINLINE f32w operator+(f32w a, f32w b) noexcept { return f32w{ _mm256_add_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator-(f32w a, f32w b) noexcept { return f32w{ _mm256_sub_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator*(f32w a, f32w b) noexcept { return f32w{ _mm256_mul_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator/(f32w a, f32w b) noexcept { return f32w{ _mm256_div_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator-(f32w a)         noexcept { return f32w{ _mm256_sub_ps(_mm256_setzero_ps(), a.v) }; }
MOSAIC_FORCEINLINE f32w& operator+=(f32w& a, f32w b) noexcept { a.v = _mm256_add_ps(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator-=(f32w& a, f32w b) noexcept { a.v = _mm256_sub_ps(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator*=(f32w& a, f32w b) noexcept { a.v = _mm256_mul_ps(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator/=(f32w& a, f32w b) noexcept { a.v = _mm256_div_ps(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w mul_add(f32w a, f32w b, f32w c) noexcept { return f32w{ _mm256_fmadd_ps(a.v, b.v, c.v) }; }
MOSAIC_FORCEINLINE f32w mul_sub(f32w a, f32w b, f32w c) noexcept { return f32w{ _mm256_fmsub_ps(a.v, b.v, c.v) }; }

MOSAIC_FORCEINLINE f32w min (f32w a, f32w b) noexcept { return f32w{ _mm256_min_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w max (f32w a, f32w b) noexcept { return f32w{ _mm256_max_ps(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w abs (f32w a)         noexcept { return f32w{ _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v) }; }
MOSAIC_FORCEINLINE f32w sqrt(f32w a)         noexcept { return f32w{ _mm256_sqrt_ps(a.v) }; }
MOSAIC_FORCEINLINE f32w rsqrt(f32w a)        noexcept { return f32w{ _mm256_rsqrt_ps(a.v) }; }
MOSAIC_FORCEINLINE f32w recip(f32w a)        noexcept { return f32w{ _mm256_rcp_ps(a.v) }; }

MOSAIC_FORCEINLINE b32w cmp_gt(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ) }; }
MOSAIC_FORCEINLINE b32w cmp_ge(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ) }; }
MOSAIC_FORCEINLINE b32w cmp_lt(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ) }; }
MOSAIC_FORCEINLINE b32w cmp_le(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ) }; }
MOSAIC_FORCEINLINE b32w cmp_eq(f32w a, f32w b) noexcept { return b32w{ _mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ) }; }
// blendv selects b where mask high bit set -> select(mask, t, f) = blendv(f, t, mask).
MOSAIC_FORCEINLINE f32w select(b32w m, f32w t, f32w f) noexcept { return f32w{ _mm256_blendv_ps(f.v, t.v, m.v) }; }
MOSAIC_FORCEINLINE bool any (b32w m) noexcept { return _mm256_movemask_ps(m.v) != 0; }
MOSAIC_FORCEINLINE bool all (b32w m) noexcept { return _mm256_movemask_ps(m.v) == 0xFF; }
MOSAIC_FORCEINLINE bool none(b32w m) noexcept { return _mm256_movemask_ps(m.v) == 0; }

MOSAIC_FORCEINLINE f32w gather(const float* base, i32w idx) noexcept { return f32w{ _mm256_i32gather_ps(base, idx.v, 4) }; }
// AVX2 has no scatter -> serialize (store lanes, scalar write-back).
MOSAIC_FORCEINLINE void scatter(float* base, i32w idx, f32w a) noexcept
{
    alignas(32) float   vals[8];
    alignas(32) int32_t ix[8];
    _mm256_store_ps(vals, a.v);
    _mm256_store_si256(reinterpret_cast<__m256i*>(ix), idx.v);
    for (int i = 0; i < 8; ++i) base[ix[i]] = vals[i];
}

} } } // namespace Mosaic::Simd::Avx2
