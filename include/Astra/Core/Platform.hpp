#pragma once

// Platform Detection
#if defined(_WIN32) || defined(_WIN64)
    #define ASTRA_PLATFORM_WINDOWS 1
    #if defined(_WIN64)
        #define ASTRA_PLATFORM_WIN64 1
    #else
        #define ASTRA_PLATFORM_WIN32 1
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#elif defined(__APPLE__) && defined(__MACH__)
    #include <TargetConditionals.h>
    #if TARGET_OS_MAC == 1
        #define ASTRA_PLATFORM_MACOS 1
    #elif TARGET_OS_IPHONE == 1
        #define ASTRA_PLATFORM_IOS 1
    #endif
    #define ASTRA_PLATFORM_APPLE 1
#elif defined(__linux__)
    #define ASTRA_PLATFORM_LINUX 1
    #if defined(__ANDROID__)
        #define ASTRA_PLATFORM_ANDROID 1
    #endif
#elif defined(__unix__)
    #define ASTRA_PLATFORM_UNIX 1
#else
    #error "Unknown platform"
#endif

// Compiler Detection
#if defined(_MSC_VER)
    #define ASTRA_COMPILER_MSVC 1
    #define ASTRA_COMPILER_VERSION _MSC_VER
    #if _MSC_VER >= 1930
        #define ASTRA_COMPILER_MSVC_2022 1
    #elif _MSC_VER >= 1920
        #define ASTRA_COMPILER_MSVC_2019 1
    #elif _MSC_VER >= 1910
        #define ASTRA_COMPILER_MSVC_2017 1
    #endif
#elif defined(__clang__)
    #define ASTRA_COMPILER_CLANG 1
    #define ASTRA_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
    #if defined(__apple_build_version__)
        #define ASTRA_COMPILER_APPLE_CLANG 1
    #endif
#elif defined(__GNUC__) || defined(__GNUG__)
    #define ASTRA_COMPILER_GCC 1
    #define ASTRA_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#elif defined(__INTEL_COMPILER)
    #define ASTRA_COMPILER_INTEL 1
    #define ASTRA_COMPILER_VERSION __INTEL_COMPILER
#else
    #error "Unknown compiler"
#endif

// Architecture Detection
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    #define ASTRA_ARCH_X64 1
    #define ASTRA_ARCH_NAME "x86_64"
    #define ASTRA_POINTER_SIZE 8
#elif defined(__i386__) || defined(_M_IX86)
    #define ASTRA_ARCH_X86 1
    #define ASTRA_ARCH_NAME "x86"
    #define ASTRA_POINTER_SIZE 4
#elif defined(__arm__) || defined(_M_ARM)
    #define ASTRA_ARCH_ARM32 1
    #define ASTRA_ARCH_NAME "arm32"
    #define ASTRA_POINTER_SIZE 4
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ASTRA_ARCH_ARM64 1
    #define ASTRA_ARCH_NAME "arm64"
    #define ASTRA_POINTER_SIZE 8
#elif defined(__wasm__)
    #define ASTRA_ARCH_WASM 1
    #define ASTRA_ARCH_NAME "wasm"
    #if defined(__wasm64__)
        #define ASTRA_ARCH_WASM64 1
        #define ASTRA_POINTER_SIZE 8
    #else
        #define ASTRA_ARCH_WASM32 1
        #define ASTRA_POINTER_SIZE 4
    #endif
#else
    #error "Unknown architecture"
#endif

// Endianness Detection
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define ASTRA_LITTLE_ENDIAN 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define ASTRA_BIG_ENDIAN 1
    #endif
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
    #define ASTRA_LITTLE_ENDIAN_DEFINED 1
#else
    #error "Unknown endianness"
#endif

// Build Configuration Detection (set by build system)
#if defined(ASTRA_BUILD_DEBUG)
    #define ASTRA_BUILD_TYPE "Debug"
#elif defined(ASTRA_BUILD_RELEASE)
    #define ASTRA_BUILD_TYPE "Release"
#else
    #define ASTRA_BUILD_TYPE "Unknown"
#endif

// C++ Standard Detection
#if __cplusplus >= 202302L
    #define ASTRA_CPP23 1
    #define ASTRA_CPP_VERSION 23
#elif __cplusplus >= 202002L
    #define ASTRA_CPP20 1
    #define ASTRA_CPP_VERSION 20
#else
    #error "Requires C++20 or later"
#endif

// SIMD Capabilities Detection
#if defined(ARCH_X64) || defined(ARCH_X86)
    #ifdef __SSE2__
        #define ASTRA_HAS_SSE2 1
    #endif
    #ifdef __SSE4_2__
        #define ASTRA_HAS_SSE42 1
    #endif
    #ifdef __AVX__
        #define ASTRA_HAS_AVX 1
    #endif
    #ifdef __AVX2__
        #define ASTRA_HAS_AVX2 1
    #endif
    #ifdef __AVX512F__
        #define ASTRA_HAS_AVX512F 1  // Foundation
    #endif
    #ifdef __AVX512BW__
        #define ASTRA_HAS_AVX512BW 1  // Byte and Word operations
    #endif
    #ifdef __AVX512VL__
        #define ASTRA_HAS_AVX512VL 1  // Vector Length extensions
    #endif
#endif
