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

// Feature Detection Macros
#ifdef __has_builtin
    #define ASTRA_HAS_BUILTIN(x) __has_builtin(x)
#else
    #define ASTRA_HAS_BUILTIN(x) 0
#endif

#define ASTRA_NODISCARD [[nodiscard]]
#define ASTRA_MAYBE_UNUSED [[maybe_unused]]
#define ASTRA_FALLTHROUGH [[fallthrough]]
#define ASTRA_LIKELY [[likely]]
#define ASTRA_UNLIKELY [[unlikely]]

// Compiler-specific attributes
#if defined(ASTRA_COMPILER_MSVC)
    #define ASTRA_FORCEINLINE __forceinline
    #define ASTRA_NOINLINE __declspec(noinline)
    #define ASTRA_ASSUME(x) __assume(x)
#elif defined(ASTRA_COMPILER_GCC) || defined(ASTRA_COMPILER_CLANG)
    #define ASTRA_FORCEINLINE inline __attribute__((always_inline))
    #define ASTRA_NOINLINE __attribute__((noinline))
    
    #if ASTRA_HAS_BUILTIN(__builtin_assume)
        #define ASTRA_ASSUME(x) __builtin_assume(x)
    #else
        #define ASTRA_ASSUME(x) do { if (!(x)) __builtin_unreachable(); } while(0)
    #endif
#endif

// Runtime assertion macro
// Note: Define ASTRA_BUILD_DEBUG in your build system (premake5) for debug builds
#ifdef ASTRA_BUILD_DEBUG
    #include <cassert>
    #define ASTRA_ASSERT(condition, message) assert((condition) && (message))
#else
    #define ASTRA_ASSERT(condition, message) ((void)0)
#endif
