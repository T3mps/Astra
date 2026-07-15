#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Bits.hpp>

#include <cstdint>

using namespace Mosaic::Bits;

namespace
{
    // Independent reference implementations -- deliberately naive, so a broken
    // intrinsic path cannot agree with them by sharing the same mistake.
    template<typename T>
    int RefPopCount(T mask) noexcept
    {
        int n = 0;
        for (unsigned i = 0; i < sizeof(T) * 8; ++i)
            if ((static_cast<uint64_t>(mask) >> i) & 1u) ++n;
        return n;
    }

    template<typename T>
    int RefCountTrailingZeros(T mask) noexcept
    {
        if (!mask) return static_cast<int>(sizeof(T) * 8);
        int n = 0;
        while (((static_cast<uint64_t>(mask) >> n) & 1u) == 0u) ++n;
        return n;
    }
}

TEST_CASE("Mosaic Bits: PopCount matches a naive reference", "[mosaic][bits]")
{
    CHECK(PopCount(uint16_t{0}) == 0);
    CHECK(PopCount(uint16_t{0xFFFF}) == 16);
    CHECK(PopCount(uint32_t{0xFFFFFFFFu}) == 32);
    CHECK(PopCount(uint64_t{0xFFFFFFFFFFFFFFFFull}) == 64);

    for (uint32_t m = 0; m < 0x10000u; ++m)
        REQUIRE(PopCount(static_cast<uint16_t>(m)) == RefPopCount(static_cast<uint16_t>(m)));

    CHECK(PopCount(uint64_t{0x8000000000000001ull}) == 2);
    CHECK(PopCount(uint64_t{0x0F0F0F0F0F0F0F0Full}) == 32);
}

TEST_CASE("Mosaic Bits: CountTrailingZeros matches a naive reference", "[mosaic][bits]")
{
    // Zero yields the mask's bit-width (the documented sentinel).
    CHECK(CountTrailingZeros(uint16_t{0}) == 16);
    CHECK(CountTrailingZeros(uint32_t{0}) == 32);
    CHECK(CountTrailingZeros(uint64_t{0}) == 64);

    for (uint32_t m = 0; m < 0x10000u; ++m)
        REQUIRE(CountTrailingZeros(static_cast<uint16_t>(m)) == RefCountTrailingZeros(static_cast<uint16_t>(m)));

    // Every single-bit 64-bit mask.
    for (int bit = 0; bit < 64; ++bit)
        REQUIRE(CountTrailingZeros(uint64_t{1} << bit) == bit);

    // The high half is only reachable on the 64-bit path.
    CHECK(CountTrailingZeros(uint64_t{0x8000000000000000ull}) == 63);
}

TEST_CASE("Mosaic Bits: FindFirstSet / FindLastSet are 1-indexed, 0 when empty", "[mosaic][bits]")
{
    CHECK(FindFirstSet(uint16_t{0}) == 0);
    CHECK(FindLastSet(uint16_t{0}) == 0);
    CHECK(FindFirstSet(uint64_t{0}) == 0);
    CHECK(FindLastSet(uint64_t{0}) == 0);

    CHECK(FindFirstSet(uint16_t{0b0000'0000'0010'0100}) == 3);   // lowest set bit = index 2
    CHECK(FindLastSet(uint16_t{0b0000'0000'0010'0100}) == 6);    // highest set bit = index 5

    for (int bit = 0; bit < 64; ++bit)
    {
        const uint64_t m = uint64_t{1} << bit;
        REQUIRE(FindFirstSet(m) == bit + 1);
        REQUIRE(FindLastSet(m) == bit + 1);
    }

    CHECK(FindFirstSet(uint64_t{0x8000000000000001ull}) == 1);
    CHECK(FindLastSet(uint64_t{0x8000000000000001ull}) == 64);
}
