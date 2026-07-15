#pragma once

// Mosaic::Simd -- portable wide-float SIMD abstraction (the "Wide" numeric lane
// vectors, as opposed to the Bits byte-match/bitmap toolbox). The canonical
// Starworks copy: Manifold2D's determinism-critical solver + broadphase key off
// these types (move origin: Manifold2D Core/Simd; design:
// docs/superpowers/specs/2026-06-24-arcane-simd-wide-float-design.md).
//
// One value type per concept (f32w / b32w / i32w) with operator overloads + free
// functions, mapped per target to AVX2 (8-wide) / NEON (4-wide) / scalar (1-wide)
// at COMPILE time. Presentation-free, header-only; builds /MD and static-CRT.
//
// Determinism: within a build the lane order + op sequence is fixed (run-twice
// identical). Elementwise ops bit-match a scalar reference (mul_add uses fused
// multiply-add on every backend, incl. std::fma in scalar); rsqrt/recip are
// hardware-estimate approximations (tolerance-checked, not bit-matched). The
// determinism contract is preserved VERBATIM from the move origin -- this layer
// must not be "optimized" toward the Bits layer's speed-first ethos.
//
// Op surface (each backend provides): splat/setzero/isplat/iota/iload, load/
// store/loadu/storeu, + - * / unary- (and the compound-assign forms), mul_add/
// mul_sub (the only fused ops under /fp:strict), min/max/abs/sqrt/rsqrt/recip,
// cmp_gt/ge/lt/le/eq -> b32w, select, any/all/none, gather/scatter. `iload`
// builds an i32w from a plain int32_t[width] array (the missing counterpart to
// load's f32w; the contact solver needs per-lane body indices as an i32w for
// gather/scatter). It is an EXACT load (bit-identical, no rounding).
//
// Define MOSAIC_SIMD_SCALAR before including to force the scalar backend on any
// target (used by the forced-scalar test TU).
//
// Backend isolation strategy (ODR safety):
//   Each backend lives in its own sub-namespace (Mosaic::Simd::Scalar,
//   Mosaic::Simd::Avx2, Mosaic::Simd::Neon).  This header then imports the ACTIVE
//   backend into Mosaic::Simd via "using namespace" -- exactly one backend is
//   compiled per TU, keeping the types distinct across TUs and eliminating ODR
//   violations when multiple TUs are linked into one binary.
//   To add a new backend: define its ops in Mosaic::Simd::<Name> inside a new
//   Wide_<Name>.inl, then add a ladder arm below that includes it, re-exports it
//   with "using namespace <Name>", and sets MOSAIC_SIMD_NS.

#include <Mosaic/Platform.hpp>   // MOSAIC_FORCEINLINE, MOSAIC_HAS_AVX2 / MOSAIC_HAS_NEON

#include <cstdint>

// Exactly one backend is included per TU, selected by the ladder below.
// MOSAIC_FORCEINLINE and MOSAIC_SIMD_NS are intentionally not #undef'd here
// because the .inl bodies need the inline macro at parse time, and MOSAIC_SIMD_NS
// is consumed by the test harness (SimdWideTests.inl) after this header is parsed.
// PRODUCTION code should use Mosaic::Simd:: directly -- MOSAIC_SIMD_NS is a
// test-harness seam that names a specific backend sub-namespace; never use it in
// non-test code.
#if defined(MOSAIC_SIMD_SCALAR)
    #include <Mosaic/Simd/Wide_Scalar.inl>
    namespace Mosaic { namespace Simd { using namespace Scalar; } }
    #define MOSAIC_SIMD_NS ::Mosaic::Simd::Scalar
#elif defined(MOSAIC_HAS_AVX2)
    #include <Mosaic/Simd/Wide_AVX2.inl>
    namespace Mosaic { namespace Simd { using namespace Avx2; } }
    #define MOSAIC_SIMD_NS ::Mosaic::Simd::Avx2
#elif defined(MOSAIC_HAS_NEON)
    #include <Mosaic/Simd/Wide_NEON.inl>
    namespace Mosaic { namespace Simd { using namespace Neon; } }
    #define MOSAIC_SIMD_NS ::Mosaic::Simd::Neon
#else
    // Fallback: no explicit MOSAIC_SIMD_SCALAR, no hardware intrinsics detected.
    #include <Mosaic/Simd/Wide_Scalar.inl>
    namespace Mosaic { namespace Simd { using namespace Scalar; } }
    #define MOSAIC_SIMD_NS ::Mosaic::Simd::Scalar
#endif

static_assert(Mosaic::Simd::f32w::width == 1 || Mosaic::Simd::f32w::width == 4 ||
              Mosaic::Simd::f32w::width == 8,
              "Mosaic::Simd::f32w::width must be 1, 4, or 8");
