#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Platform.hpp>

#include <cstddef>

namespace
{
    MOSAIC_FORCEINLINE int Doubled(int x) noexcept { return x * 2; }
}

TEST_CASE("Mosaic Platform: attributes + detection are usable", "[mosaic][platform]")
{
    // MOSAIC_FORCEINLINE compiles and the function works.
    CHECK(Doubled(21) == 42);

    // Pointer-size macro agrees with the real pointer size.
    CHECK(static_cast<std::size_t>(MOSAIC_POINTER_SIZE) == sizeof(void*));

    // The C++-standard gate resolved to >= 20 (else Platform.hpp #errors).
    CHECK(MOSAIC_CPP_VERSION >= 20);

    // x64 dev/CI target: pointer is 8 bytes and the SSE2/AVX2 baselines are set
    // (built with /arch:AVX2 -> the numeric SIMD in step 2 keys off AVX2).
#if defined(MOSAIC_ARCH_X64)
    CHECK(MOSAIC_POINTER_SIZE == 8);
    #if !defined(MOSAIC_HAS_SSE2)
        FAIL("x64 must define MOSAIC_HAS_SSE2");
    #endif
#endif
}
