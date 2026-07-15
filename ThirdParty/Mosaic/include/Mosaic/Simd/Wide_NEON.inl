// 4-wide ARM NEON (AArch64) backend. Included by Simd.hpp under __ARM_NEON__.
// Never compiled standalone. UNVALIDATED -- there is no ARM CI/hardware in this
// repo yet, so this mirrors the AVX2/scalar contract by construction; it will be
// validated against the shared [simd] tests when the ARM port lands. Until then
// the scalar backend is the guaranteed ARM fallback (build without NEON).
// Uses AArch64-only intrinsics (vsqrtq_f32, vdivq_f32, vmaxvq_u32) -- targets
// ARMv8/Apple-Silicon/modern mobile, not 32-bit ARMv7.
#include <arm_neon.h>
#include <cstdint>

namespace Mosaic { namespace Simd { namespace Neon {

inline constexpr const char* kBackendName = "NEON";

struct f32w { float32x4_t v; static constexpr int width = 4; };
struct b32w { uint32x4_t  v; static constexpr int width = 4; };
struct i32w { int32x4_t   v; static constexpr int width = 4; };

MOSAIC_FORCEINLINE f32w splat(float x)    noexcept { return f32w{ vdupq_n_f32(x) }; }
MOSAIC_FORCEINLINE f32w setzero()         noexcept { return f32w{ vdupq_n_f32(0.0f) }; }
MOSAIC_FORCEINLINE i32w isplat(int32_t x) noexcept { return i32w{ vdupq_n_s32(x) }; }
MOSAIC_FORCEINLINE i32w iota()            noexcept { const int32_t k[4] = {0,1,2,3}; return i32w{ vld1q_s32(k) }; }
// Load `width` int32 lanes from an int32_t array (NEON vld1q tolerates unaligned,
// but the solver's index arrays are alignas(32) regardless). Exact load ->
// bit-identical. Mirrors the AVX2/scalar iload contract.
MOSAIC_FORCEINLINE i32w iload(const int32_t* p) noexcept { return i32w{ vld1q_s32(p) }; }
MOSAIC_FORCEINLINE void istore(int32_t* p, i32w v) noexcept { vst1q_s32(p, v.v); }

MOSAIC_FORCEINLINE f32w load(const float* p)      noexcept { return f32w{ vld1q_f32(p) }; }
MOSAIC_FORCEINLINE void store(float* p, f32w a)   noexcept { vst1q_f32(p, a.v); }
MOSAIC_FORCEINLINE f32w loadu(const float* p)     noexcept { return f32w{ vld1q_f32(p) }; }   // NEON loads tolerate unaligned
MOSAIC_FORCEINLINE void storeu(float* p, f32w a)  noexcept { vst1q_f32(p, a.v); }

MOSAIC_FORCEINLINE f32w operator+(f32w a, f32w b) noexcept { return f32w{ vaddq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator-(f32w a, f32w b) noexcept { return f32w{ vsubq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator*(f32w a, f32w b) noexcept { return f32w{ vmulq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator/(f32w a, f32w b) noexcept { return f32w{ vdivq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w operator-(f32w a)         noexcept { return f32w{ vnegq_f32(a.v) }; }
MOSAIC_FORCEINLINE f32w& operator+=(f32w& a, f32w b) noexcept { a.v = vaddq_f32(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator-=(f32w& a, f32w b) noexcept { a.v = vsubq_f32(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator*=(f32w& a, f32w b) noexcept { a.v = vmulq_f32(a.v, b.v); return a; }
MOSAIC_FORCEINLINE f32w& operator/=(f32w& a, f32w b) noexcept { a.v = vdivq_f32(a.v, b.v); return a; }
// vfmaq_f32(acc, x, y) = acc + x*y  ->  mul_add(a,b,c) = c + a*b ; mul_sub(a,b,c) = a*b - c.
MOSAIC_FORCEINLINE f32w mul_add(f32w a, f32w b, f32w c) noexcept { return f32w{ vfmaq_f32(c.v, a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w mul_sub(f32w a, f32w b, f32w c) noexcept { return f32w{ vfmaq_f32(vnegq_f32(c.v), a.v, b.v) }; }

MOSAIC_FORCEINLINE f32w min (f32w a, f32w b) noexcept { return f32w{ vminq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w max (f32w a, f32w b) noexcept { return f32w{ vmaxq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w abs (f32w a)         noexcept { return f32w{ vabsq_f32(a.v) }; }
MOSAIC_FORCEINLINE f32w sqrt(f32w a)         noexcept { return f32w{ vsqrtq_f32(a.v) }; }
MOSAIC_FORCEINLINE f32w rsqrt(f32w a)        noexcept { return f32w{ vrsqrteq_f32(a.v) }; }
MOSAIC_FORCEINLINE f32w recip(f32w a)        noexcept { return f32w{ vrecpeq_f32(a.v) }; }

MOSAIC_FORCEINLINE b32w cmp_gt(f32w a, f32w b) noexcept { return b32w{ vcgtq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE b32w cmp_ge(f32w a, f32w b) noexcept { return b32w{ vcgeq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE b32w cmp_lt(f32w a, f32w b) noexcept { return b32w{ vcltq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE b32w cmp_le(f32w a, f32w b) noexcept { return b32w{ vcleq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE b32w cmp_eq(f32w a, f32w b) noexcept { return b32w{ vceqq_f32(a.v, b.v) }; }
MOSAIC_FORCEINLINE f32w select(b32w m, f32w t, f32w f) noexcept { return f32w{ vbslq_f32(m.v, t.v, f.v) }; }
MOSAIC_FORCEINLINE bool any (b32w m) noexcept { return vmaxvq_u32(m.v) != 0u; }
MOSAIC_FORCEINLINE bool all (b32w m) noexcept { return vminvq_u32(m.v) != 0u; }
MOSAIC_FORCEINLINE bool none(b32w m) noexcept { return vmaxvq_u32(m.v) == 0u; }

// NEON has no gather/scatter -> serialize.
MOSAIC_FORCEINLINE f32w gather(const float* base, i32w idx) noexcept
{
    int32_t ix[4]; vst1q_s32(ix, idx.v);
    float v[4] = { base[ix[0]], base[ix[1]], base[ix[2]], base[ix[3]] };
    return f32w{ vld1q_f32(v) };
}
MOSAIC_FORCEINLINE void scatter(float* base, i32w idx, f32w a) noexcept
{
    int32_t ix[4]; vst1q_s32(ix, idx.v);
    float v[4];    vst1q_f32(v, a.v);
    for (int i = 0; i < 4; ++i) base[ix[i]] = v[i];
}

} } } // namespace Mosaic::Simd::Neon
