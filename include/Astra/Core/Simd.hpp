#pragma once

// Re-export shim. The SIMD bit toolbox this header used to define -- byte-match
// masks, 128/256-bit bitmap logic, hash-combine, prefetch, batch helpers, and the
// scalar bit scans -- MOVED TO MOSAIC, the shared Starworks core, where it is the
// single canonical copy (Manifold2D and Arcane were each carrying near-duplicates
// of the same intrinsics ladder). Astra keeps its `Astra::Simd::...` vocabulary
// via the namespace alias below, so every call site (FlatMap / FlatSet / Bitmap /
// Archetype / Entity / Relations / BinaryArchive) is unchanged.
//
//   Mosaic/Simd/Bits.hpp -- Width128/256, MatchByteMask, MatchEitherByteMask,
//                           Int128/Int256 ops, HashCombine, prefetch, BatchOps
//   Mosaic/Bits.hpp      -- PopCount / CountTrailingZeros / FindFirstSet /
//                           FindLastSet (re-exported into Mosaic::Simd::Ops)
//
// Backend selection is UNCHANGED: MOSAIC_HAS_SSE2 / SSE42 / AVX / AVX2 / NEON key
// off the same predefined macros ASTRA_HAS_* did (premake defines __SSE2__ and
// __SSE4_2__ on MSVC, which does not predefine them under /arch:AVX).
//
// Alignment preconditions inside Mosaic currently assert via <cassert> (Debug-only,
// same as ASTRA_ASSERT). Mosaic's own assert seam lands next, at which point Astra
// injects its handler and the two converge.

#include <Mosaic/Simd/Bits.hpp>

#include "Base.hpp"

namespace Astra
{
    namespace Simd = ::Mosaic::Simd;
}
