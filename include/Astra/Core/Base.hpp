#pragma once

#include "Platform.hpp"

// Cross-platform struct packing macros
#ifdef ASTRA_COMPILER_MSVC
    #define ASTRA_PACK_BEGIN __pragma(pack(push, 1))
    #define ASTRA_PACK_END __pragma(pack(pop))
#elif defined(ASTRA_COMPILER_GCC) || defined(ASTRA_COMPILER_CLANG)
    #define ASTRA_PACK_BEGIN _Pragma("pack(push, 1)")
    #define ASTRA_PACK_END _Pragma("pack(pop)")
#else
    #error "Unsupported compiler for struct packing"
#endif

// Portability attributes. Mosaic (via Platform.hpp above) owns the per-compiler
// definitions -- these are Astra's names for them, one line each, so call sites
// keep reading ASTRA_FORCEINLINE / ASTRA_NODISCARD / ...
#define ASTRA_HAS_BUILTIN(x) MOSAIC_HAS_BUILTIN(x)

#define ASTRA_NODISCARD     MOSAIC_NODISCARD
#define ASTRA_MAYBE_UNUSED  MOSAIC_MAYBE_UNUSED
#define ASTRA_FALLTHROUGH   MOSAIC_FALLTHROUGH
#define ASTRA_LIKELY        MOSAIC_LIKELY
#define ASTRA_UNLIKELY      MOSAIC_UNLIKELY

#define ASTRA_FORCEINLINE   MOSAIC_FORCEINLINE
#define ASTRA_NOINLINE      MOSAIC_NOINLINE
#define ASTRA_ASSUME(x)     MOSAIC_ASSUME(x)

// Diagnostics seam — included last so Log/Assert see the macros defined above.
#include "Log.hpp"
#include "Assert.hpp"
