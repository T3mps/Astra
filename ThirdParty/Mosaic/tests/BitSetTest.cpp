// Mosaic::BitSet (b2BitSet equivalent) unit tests. Moved with the type from
// Manifold2D, where it was the narrowphase MT tail's per-worker scratch.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Mosaic/BitSet.hpp>
using Mosaic::BitSet;

TEST_CASE("BitSet: set + ForEachSetBit walks ascending", "[mosaic][bitset]")
{
    BitSet b; b.Resize(200);
    b.Set(5); b.Set(63); b.Set(64); b.Set(199);
    std::vector<std::uint32_t> got;
    b.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{5u, 63u, 64u, 199u});
}

TEST_CASE("BitSet: ClearAll empties", "[mosaic][bitset]")
{
    BitSet b; b.Resize(100); b.Set(10); b.Set(70);
    b.ClearAll();
    int n = 0; b.ForEachSetBit([&](std::uint32_t){ ++n; });
    REQUIRE(n == 0);
}

TEST_CASE("BitSet: Set at the highest valid index stays in range", "[mosaic][bitset]")
{
    // Valid bit range after Resize(64) is [0, 64). The highest valid index (63)
    // must not trip the bounds guard and must be walked back.
    BitSet b; b.Resize(64);
    b.Set(63);
    std::vector<std::uint32_t> got;
    b.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{63u});
    // Set(64) here (index == capacity) would trip the debug-only OOB-write assert;
    // it is not run as a death test.
}

TEST_CASE("BitSet: InPlaceUnion ORs, walks ascending across blocks", "[mosaic][bitset]")
{
    BitSet a; a.Resize(130); BitSet b; b.Resize(130);
    a.Set(1); a.Set(129); b.Set(1); b.Set(64);
    a.InPlaceUnion(b);
    std::vector<std::uint32_t> got;
    a.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{1u, 64u, 129u}); // 1 deduped
}
