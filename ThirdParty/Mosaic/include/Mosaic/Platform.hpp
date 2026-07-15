#pragma once

// Mosaic/Platform.hpp -- the compile-time substrate every Starworks library
// keys off: platform / compiler / architecture / endianness / C++-standard /
// SIMD-capability detection, plus the portability attribute macros
// (MOSAIC_FORCEINLINE, MOSAIC_NODISCARD, ...). Header-only, zero dependencies.
//
// Unifies what Astra (Core/Platform.hpp + Core/Base.hpp attribute block) and
// Manifold2D (a 3-macro subset hand-rolled inside Simd.hpp) each detected
// separately -- one MOSAIC_ layer instead of three. Manifold2D's SIMD ladder
// and its Linux port key off MOSAIC_HAS_AVX2 / MOSAIC_HAS_NEON /
// MOSAIC_FORCEINLINE; Astra's byte-match toolbox off MOSAIC_HAS_SSE2/AVX2/NEON.

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
    #define MOSAIC_PLATFORM_WINDOWS 1
    #if defined(_WIN64)
        #define MOSAIC_PLATFORM_WIN64 1
    #else
        #define MOSAIC_PLATFORM_WIN32 1
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#elif defined(__APPLE__) && defined(__MACH__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IPHONE == 1
        #define MOSAIC_PLATFORM_IOS 1
    #else
        #define MOSAIC_PLATFORM_MACOS 1
    #endif
    #define MOSAIC_PLATFORM_APPLE 1
#elif defined(__linux__)
    #define MOSAIC_PLATFORM_LINUX 1
    #if defined(__ANDROID__)
        #define MOSAIC_PLATFORM_ANDROID 1
    #endif
#elif defined(__unix__)
    #define MOSAIC_PLATFORM_UNIX 1
#else
    #error "Mosaic: unknown platform"
#endif

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(__clang__)
    #define MOSAIC_COMPILER_MSVC 1
    #define MOSAIC_COMPILER_VERSION _MSC_VER
#elif defined(__clang__)
    #define MOSAIC_COMPILER_CLANG 1
    #define MOSAIC_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
    #if defined(__apple_build_version__)
        #define MOSAIC_COMPILER_APPLE_CLANG 1
    #endif
#elif defined(__GNUC__) || defined(__GNUG__)
    #define MOSAIC_COMPILER_GCC 1
    #define MOSAIC_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#elif defined(__INTEL_COMPILER)
    #define MOSAIC_COMPILER_INTEL 1
    #define MOSAIC_COMPILER_VERSION __INTEL_COMPILER
#else
    #error "Mosaic: unknown compiler"
#endif

// ---------------------------------------------------------------------------
// Architecture
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    #define MOSAIC_ARCH_X64 1
    #define MOSAIC_ARCH_NAME "x86_64"
    #define MOSAIC_POINTER_SIZE 8
#elif defined(__i386__) || defined(_M_IX86)
    #define MOSAIC_ARCH_X86 1
    #define MOSAIC_ARCH_NAME "x86"
    #define MOSAIC_POINTER_SIZE 4
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define MOSAIC_ARCH_ARM64 1
    #define MOSAIC_ARCH_NAME "arm64"
    #define MOSAIC_POINTER_SIZE 8
#elif defined(__arm__) || defined(_M_ARM)
    #define MOSAIC_ARCH_ARM32 1
    #define MOSAIC_ARCH_NAME "arm32"
    #define MOSAIC_POINTER_SIZE 4
#elif defined(__wasm__)
    #define MOSAIC_ARCH_WASM 1
    #define MOSAIC_ARCH_NAME "wasm"
    #if defined(__wasm64__)
        #define MOSAIC_ARCH_WASM64 1
        #define MOSAIC_POINTER_SIZE 8
    #else
        #define MOSAIC_ARCH_WASM32 1
        #define MOSAIC_POINTER_SIZE 4
    #endif
#else
    #error "Mosaic: unknown architecture"
#endif

// ---------------------------------------------------------------------------
// C++ standard (robust on MSVC regardless of /Zc:__cplusplus, via _MSVC_LANG)
// ---------------------------------------------------------------------------
#if defined(MOSAIC_COMPILER_MSVC)
    #define MOSAIC_CPLUSPLUS _MSVC_LANG
#else
    #define MOSAIC_CPLUSPLUS __cplusplus
#endif
#if MOSAIC_CPLUSPLUS >= 202302L
    #define MOSAIC_CPP23 1
    #define MOSAIC_CPP_VERSION 23
#elif MOSAIC_CPLUSPLUS >= 202002L
    #define MOSAIC_CPP20 1
    #define MOSAIC_CPP_VERSION 20
#else
    #error "Mosaic requires C++20 or later"
#endif

// ---------------------------------------------------------------------------
// Portability attributes
// ---------------------------------------------------------------------------
#ifdef __has_builtin
    #define MOSAIC_HAS_BUILTIN(x) __has_builtin(x)
#else
    #define MOSAIC_HAS_BUILTIN(x) 0
#endif

#define MOSAIC_NODISCARD    [[nodiscard]]
#define MOSAIC_MAYBE_UNUSED [[maybe_unused]]
#define MOSAIC_FALLTHROUGH  [[fallthrough]]
#define MOSAIC_LIKELY       [[likely]]
#define MOSAIC_UNLIKELY     [[unlikely]]

#if defined(MOSAIC_COMPILER_MSVC)
    #define MOSAIC_FORCEINLINE __forceinline
    #define MOSAIC_NOINLINE    __declspec(noinline)
    #define MOSAIC_ASSUME(x)   __assume(x)
#elif defined(MOSAIC_COMPILER_GCC) || defined(MOSAIC_COMPILER_CLANG)
    #define MOSAIC_FORCEINLINE inline __attribute__((always_inline))
    #define MOSAIC_NOINLINE    __attribute__((noinline))
    #if MOSAIC_HAS_BUILTIN(__builtin_assume)
        #define MOSAIC_ASSUME(x) __builtin_assume(x)
    #else
        #define MOSAIC_ASSUME(x) do { if (!(x)) __builtin_unreachable(); } while (0)
    #endif
#else
    #define MOSAIC_FORCEINLINE inline
    #define MOSAIC_NOINLINE
    #define MOSAIC_ASSUME(x) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Endianness
// ---------------------------------------------------------------------------
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define MOSAIC_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define MOSAIC_BIG_ENDIAN 1
    #endif
#elif defined(MOSAIC_COMPILER_MSVC) || defined(MOSAIC_ARCH_X64) || defined(MOSAIC_ARCH_X86) || defined(MOSAIC_ARCH_ARM64)
    #define MOSAIC_LITTLE_ENDIAN 1
#endif

// ---------------------------------------------------------------------------
// SIMD capabilities
// ---------------------------------------------------------------------------
#if defined(MOSAIC_ARCH_X64) || defined(MOSAIC_ARCH_X86)
    // x64 guarantees an SSE2 baseline; MSVC does not predefine __SSE2__.
    #if defined(MOSAIC_ARCH_X64) || defined(__SSE2__)
        #define MOSAIC_HAS_SSE2 1
    #endif
    #if defined(__SSE4_2__)
        #define MOSAIC_HAS_SSE42 1
    #endif
    #if defined(__AVX__)
        #define MOSAIC_HAS_AVX 1
    #endif
    #if defined(__AVX2__)
        #define MOSAIC_HAS_AVX2 1
    #endif
    #if defined(__AVX512F__)
        #define MOSAIC_HAS_AVX512F 1
    #endif
    #if defined(__AVX512BW__)
        #define MOSAIC_HAS_AVX512BW 1
    #endif
    #if defined(__AVX512VL__)
        #define MOSAIC_HAS_AVX512VL 1
    #endif
#endif

#if defined(MOSAIC_ARCH_ARM64) || defined(MOSAIC_ARCH_ARM32)
    #if defined(__ARM_NEON) || defined(__ARM_NEON__)
        #define MOSAIC_HAS_NEON 1
    #endif
    #if defined(__ARM_FEATURE_CRC32)
        #define MOSAIC_HAS_ARM_CRC32 1
    #endif
#endif
