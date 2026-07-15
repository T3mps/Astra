#pragma once

// Re-export shim. The platform / compiler / architecture / endianness /
// C++-standard / SIMD-capability DETECTION moved to Mosaic, the shared Starworks
// core -- Astra, Manifold2D and Arcane were each maintaining their own copy of the
// same preprocessor ladder. Astra keeps its ASTRA_* vocabulary by re-exporting
// Mosaic's, so every call site (Memory / TypeID / Assert / Base / Simd) is
// unchanged and nothing outside this file needs to learn a new macro name.
//
// Astra-specific macros that Mosaic has no opinion on -- the build-type string and
// the MSVC-generation flags -- are still defined here, at the bottom.
//
// One deliberate difference inherited from Mosaic: clang-cl is detected as CLANG
// (Mosaic checks `_MSC_VER && !__clang__` for MSVC), where this header used to call
// it MSVC. Neither Astra nor its CI builds with clang-cl; plain clang, GCC and MSVC
// all resolve identically.

#include <Mosaic/Platform.hpp>

// --- Platform ----------------------------------------------------------------
#if defined(MOSAIC_PLATFORM_WINDOWS)
    #define ASTRA_PLATFORM_WINDOWS 1
#endif
#if defined(MOSAIC_PLATFORM_WIN64)
    #define ASTRA_PLATFORM_WIN64 1
#endif
#if defined(MOSAIC_PLATFORM_WIN32)
    #define ASTRA_PLATFORM_WIN32 1
#endif
#if defined(MOSAIC_PLATFORM_APPLE)
    #define ASTRA_PLATFORM_APPLE 1
#endif
#if defined(MOSAIC_PLATFORM_MACOS)
    #define ASTRA_PLATFORM_MACOS 1
#endif
#if defined(MOSAIC_PLATFORM_IOS)
    #define ASTRA_PLATFORM_IOS 1
#endif
#if defined(MOSAIC_PLATFORM_LINUX)
    #define ASTRA_PLATFORM_LINUX 1
#endif
#if defined(MOSAIC_PLATFORM_ANDROID)
    #define ASTRA_PLATFORM_ANDROID 1
#endif
#if defined(MOSAIC_PLATFORM_UNIX)
    #define ASTRA_PLATFORM_UNIX 1
#endif

// --- Compiler ----------------------------------------------------------------
#define ASTRA_COMPILER_VERSION MOSAIC_COMPILER_VERSION

#if defined(MOSAIC_COMPILER_MSVC)
    #define ASTRA_COMPILER_MSVC 1
    #if _MSC_VER >= 1930
        #define ASTRA_COMPILER_MSVC_2022 1
    #elif _MSC_VER >= 1920
        #define ASTRA_COMPILER_MSVC_2019 1
    #elif _MSC_VER >= 1910
        #define ASTRA_COMPILER_MSVC_2017 1
    #endif
#endif
#if defined(MOSAIC_COMPILER_CLANG)
    #define ASTRA_COMPILER_CLANG 1
#endif
#if defined(MOSAIC_COMPILER_APPLE_CLANG)
    #define ASTRA_COMPILER_APPLE_CLANG 1
#endif
#if defined(MOSAIC_COMPILER_GCC)
    #define ASTRA_COMPILER_GCC 1
#endif
#if defined(MOSAIC_COMPILER_INTEL)
    #define ASTRA_COMPILER_INTEL 1
#endif

// --- Architecture ------------------------------------------------------------
#define ASTRA_ARCH_NAME    MOSAIC_ARCH_NAME
#define ASTRA_POINTER_SIZE MOSAIC_POINTER_SIZE

#if defined(MOSAIC_ARCH_X64)
    #define ASTRA_ARCH_X64 1
#endif
#if defined(MOSAIC_ARCH_X86)
    #define ASTRA_ARCH_X86 1
#endif
#if defined(MOSAIC_ARCH_ARM64)
    #define ASTRA_ARCH_ARM64 1
#endif
#if defined(MOSAIC_ARCH_ARM32)
    #define ASTRA_ARCH_ARM32 1
#endif
#if defined(MOSAIC_ARCH_WASM)
    #define ASTRA_ARCH_WASM 1
#endif
#if defined(MOSAIC_ARCH_WASM64)
    #define ASTRA_ARCH_WASM64 1
#endif
#if defined(MOSAIC_ARCH_WASM32)
    #define ASTRA_ARCH_WASM32 1
#endif

// --- Endianness --------------------------------------------------------------
#if defined(MOSAIC_LITTLE_ENDIAN)
    #define ASTRA_LITTLE_ENDIAN 1
#endif
#if defined(MOSAIC_BIG_ENDIAN)
    #define ASTRA_BIG_ENDIAN 1
#endif

// --- C++ standard ------------------------------------------------------------
#define ASTRA_CPP_VERSION MOSAIC_CPP_VERSION
#if defined(MOSAIC_CPP23)
    #define ASTRA_CPP23 1
#endif
#if defined(MOSAIC_CPP20)
    #define ASTRA_CPP20 1
#endif

// --- SIMD capabilities -------------------------------------------------------
// Astra's own code no longer reads these (the byte-match toolbox moved to Mosaic
// and keys off MOSAIC_HAS_*); they stay as part of Astra's published surface.
#if defined(MOSAIC_HAS_SSE2)
    #define ASTRA_HAS_SSE2 1
#endif
#if defined(MOSAIC_HAS_SSE42)
    #define ASTRA_HAS_SSE42 1
#endif
#if defined(MOSAIC_HAS_AVX)
    #define ASTRA_HAS_AVX 1
#endif
#if defined(MOSAIC_HAS_AVX2)
    #define ASTRA_HAS_AVX2 1
#endif
#if defined(MOSAIC_HAS_AVX512F)
    #define ASTRA_HAS_AVX512F 1
#endif
#if defined(MOSAIC_HAS_AVX512BW)
    #define ASTRA_HAS_AVX512BW 1
#endif
#if defined(MOSAIC_HAS_AVX512VL)
    #define ASTRA_HAS_AVX512VL 1
#endif
#if defined(MOSAIC_HAS_NEON)
    #define ASTRA_HAS_NEON 1
#endif
#if defined(MOSAIC_HAS_ARM_CRC32)
    #define ASTRA_HAS_ARM_CRC32 1
#endif

// --- Astra-only: build configuration (set by the build system) ---------------
#if defined(ASTRA_BUILD_DEBUG)
    #define ASTRA_BUILD_TYPE "Debug"
#elif defined(ASTRA_BUILD_RELEASE)
    #define ASTRA_BUILD_TYPE "Release"
#elif defined(ASTRA_BUILD_DIST)
    #define ASTRA_BUILD_TYPE "Dist"
#else
    #define ASTRA_BUILD_TYPE "Unknown"
#endif
