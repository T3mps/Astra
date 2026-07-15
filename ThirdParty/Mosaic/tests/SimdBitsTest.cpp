#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Simd/Bits.hpp>

#include <cstdint>
#include <cstring>

using namespace Mosaic::Simd;

namespace
{
    // A deterministic byte pattern with plenty of repeats, so every group has
    // matches, near-misses, and gaps.
    void FillPattern(uint8_t* p, size_t n, uint32_t seed) noexcept
    {
        uint32_t s = seed;
        for (size_t i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;      // LCG -- reproducible, not random
            p[i] = static_cast<uint8_t>((s >> 16) & 0x07u);   // values 0..7 -> dense repeats
        }
    }

    uint32_t RefMatchMask(const uint8_t* p, size_t bytes, uint8_t value) noexcept
    {
        uint32_t mask = 0;
        for (size_t i = 0; i < bytes; ++i)
            if (p[i] == value) mask |= (1u << i);
        return mask;
    }

    uint32_t RefMatchEitherMask(const uint8_t* p, size_t bytes, uint8_t a, uint8_t b) noexcept
    {
        uint32_t mask = 0;
        for (size_t i = 0; i < bytes; ++i)
            if (p[i] == a || p[i] == b) mask |= (1u << i);
        return mask;
    }
}

TEST_CASE("Mosaic Simd Bits: the active backend cross-validates against scalar", "[mosaic][simd][bits]")
{
    // The whole point of the byte-match layer: whichever backend the target picks
    // (SSE / NEON / AVX2 / scalar), the MASK IS THE SAME. That is what lets a
    // container key its layout off the mask without caring about the ISA.
    alignas(32) uint8_t buf[32];

    for (uint32_t seed = 1; seed <= 64; ++seed)
    {
        FillPattern(buf, sizeof(buf), seed);
        for (uint8_t v = 0; v < 8; ++v)
        {
            REQUIRE(Ops::MatchByteMask<Width128>(buf, v) == static_cast<uint16_t>(RefMatchMask(buf, 16, v)));
            REQUIRE(Ops::MatchByteMask<Width256>(buf, v) == RefMatchMask(buf, 32, v));

            const uint8_t w = static_cast<uint8_t>((v + 3u) & 7u);
            REQUIRE(Ops::MatchEitherByteMask<Width128>(buf, v, w) ==
                    static_cast<uint16_t>(RefMatchEitherMask(buf, 16, v, w)));
            REQUIRE(Ops::MatchEitherByteMask<Width256>(buf, v, w) ==
                    RefMatchEitherMask(buf, 32, v, w));
        }
    }
}

TEST_CASE("Mosaic Simd Bits: match edge cases -- all / none", "[mosaic][simd][bits]")
{
    alignas(32) uint8_t buf[32];

    std::memset(buf, 0xAB, sizeof(buf));
    CHECK(Ops::MatchByteMask<Width128>(buf, 0xAB) == uint16_t{0xFFFF});
    CHECK(Ops::MatchByteMask<Width256>(buf, 0xAB) == uint32_t{0xFFFFFFFFu});
    CHECK(Ops::MatchByteMask<Width128>(buf, 0x00) == uint16_t{0});
    CHECK(Ops::MatchByteMask<Width256>(buf, 0x00) == uint32_t{0});

    // A single byte set in the last lane of each width.
    std::memset(buf, 0x00, sizeof(buf));
    buf[15] = 0x7F;
    buf[31] = 0x7F;
    CHECK(Ops::MatchByteMask<Width128>(buf, 0x7F) == uint16_t{0x8000});
    CHECK(Ops::MatchByteMask<Width256>(buf, 0x7F) == uint32_t{0x80008000u});
    CHECK(Ops::PopCount(Ops::MatchByteMask<Width256>(buf, 0x7F)) == 2);
    CHECK(Ops::CountTrailingZeros(Ops::MatchByteMask<Width128>(buf, 0x7F)) == 15);
}

TEST_CASE("Mosaic Simd Bits: Int128 / Int256 bitmap ops", "[mosaic][simd][bits]")
{
    alignas(32) uint64_t a[4] = { 0x00FF00FF00FF00FFull, 0xF0F0F0F0F0F0F0F0ull,
                                  0x1111111111111111ull, 0x00000000FFFFFFFFull };
    alignas(32) uint64_t b[4] = { 0x0F0F0F0F0F0F0F0Full, 0xFFFF0000FFFF0000ull,
                                  0x3333333333333333ull, 0xFFFFFFFFFFFFFFFFull };
    alignas(32) uint64_t out[4] = {};

    const auto a128 = Ops::Load128(a);
    const auto b128 = Ops::Load128(b);

    Ops::Store128(out, Ops::And128(a128, b128));
    CHECK(out[0] == (a[0] & b[0]));
    CHECK(out[1] == (a[1] & b[1]));

    Ops::Store128(out, Ops::Or128(a128, b128));
    CHECK(out[0] == (a[0] | b[0]));
    CHECK(out[1] == (a[1] | b[1]));

    CHECK(Ops::TestEqual128(a128, a128));
    CHECK_FALSE(Ops::TestEqual128(a128, b128));

    // a & b == a  <=>  a is a subset of b. Build one that genuinely is.
    alignas(32) uint64_t sub[2] = { a[0] & b[0], a[1] & b[1] };
    CHECK(Ops::TestSubset128(Ops::Load128(sub), a128));
    CHECK(Ops::TestSubset128(Ops::Load128(sub), b128));
    CHECK_FALSE(Ops::TestSubset128(b128, a128));

    const auto a256 = Ops::Load256(a);
    const auto b256 = Ops::Load256(b);

    Ops::Store256(out, Ops::And256(a256, b256));
    for (int i = 0; i < 4; ++i) CHECK(out[i] == (a[i] & b[i]));

    Ops::Store256(out, Ops::Or256(a256, b256));
    for (int i = 0; i < 4; ++i) CHECK(out[i] == (a[i] | b[i]));

    CHECK(Ops::TestEqual256(a256, a256));
    CHECK_FALSE(Ops::TestEqual256(a256, b256));

    alignas(32) uint64_t sub256[4] = { a[0] & b[0], a[1] & b[1], a[2] & b[2], a[3] & b[3] };
    CHECK(Ops::TestSubset256(Ops::Load256(sub256), a256));
    CHECK_FALSE(Ops::TestSubset256(b256, a256));
}

TEST_CASE("Mosaic Simd Bits: hash-combine seams", "[mosaic][simd][bits]")
{
    // HashCombine is ISA-dependent (CRC32C where available, MurmurHash3 finalizer
    // otherwise), so the only cross-backend guarantees are determinism and that
    // distinct inputs mix to distinct outputs. Note it is NOT symmetry-proof: the
    // fallback opens with `seed ^ value`, so HashCombine(a,b) == HashCombine(b,a)
    // there while the CRC32C path is order-sensitive. Never rely on either.
    CHECK(Ops::HashCombine(1, 2) == Ops::HashCombine(1, 2));
    CHECK(Ops::HashCombine(0, 0) != Ops::HashCombine(0, 1));
    CHECK(Ops::HashCombine(0, 1) != Ops::HashCombine(1, 1));
    CHECK(Ops::HashCombine(0x9E3779B97F4A7C15ull, 7) != Ops::HashCombine(0x9E3779B97F4A7C15ull, 8));

    // PortableHashCombine is the ISA-INDEPENDENT one (persisted checksums use it).
    // Checked against the MurmurHash3 fmix64 spec written out independently here --
    // a transcription slip in the move would show up as a mismatch.
    auto fmix64 = [](uint64_t seed, uint64_t value) noexcept
    {
        uint64_t h = seed ^ value;
        h = (h ^ (h >> 33)) * 0xff51afd7ed558ccdull;
        h = (h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53ull;
        return h ^ (h >> 33);
    };

    CHECK(Ops::PortableHashCombine(0, 0) == 0ull);   // fmix64(0) is a fixed point
    for (uint64_t s = 0; s < 32; ++s)
        for (uint64_t v = 0; v < 32; ++v)
            REQUIRE(Ops::PortableHashCombine(s, v) == fmix64(s, v));

    CHECK(Ops::PortableHashCombine(0x9E3779B97F4A7C15ull, 0xDEADBEEFull) ==
          fmix64(0x9E3779B97F4A7C15ull, 0xDEADBEEFull));
}

TEST_CASE("Mosaic Simd Bits: BatchOps agree with the single-group path", "[mosaic][simd][bits]")
{
    constexpr size_t kGroups = 9;   // deliberately not a multiple of BatchSize (4)
    alignas(32) uint8_t buf[kGroups * 16];
    FillPattern(buf, sizeof(buf), 7u);

    uint16_t batched[kGroups] = {};
    Ops::BatchOps<Width128>::MatchByteMaskBatch(buf, 5, batched, kGroups);
    for (size_t g = 0; g < kGroups; ++g)
        REQUIRE(batched[g] == Ops::MatchByteMask<Width128>(buf + g * 16, 5));

    Ops::BatchOps<Width128>::MatchEitherByteMaskBatch(buf, 5, 6, batched, kGroups);
    for (size_t g = 0; g < kGroups; ++g)
        REQUIRE(batched[g] == Ops::MatchEitherByteMask<Width128>(buf + g * 16, 5, 6));

    // FindFirstMatchInBatch returns a BYTE offset across the groups, or -1.
    std::memset(buf, 0, sizeof(buf));
    CHECK(Ops::BatchOps<Width128>::FindFirstMatchInBatch(buf, 0x42, kGroups) == -1);
    buf[3 * 16 + 7] = 0x42;
    CHECK(Ops::BatchOps<Width128>::FindFirstMatchInBatch(buf, 0x42, kGroups) == 3 * 16 + 7);
}

TEST_CASE("Mosaic Simd Bits: width traits + alignment helpers", "[mosaic][simd][bits]")
{
    STATIC_REQUIRE(WidthTraits<Width128>::bytes == 16);
    STATIC_REQUIRE(WidthTraits<Width256>::bytes == 32);
    STATIC_REQUIRE(sizeof(WidthTraits<Width128>::MaskType) == 2);
    STATIC_REQUIRE(sizeof(WidthTraits<Width256>::MaskType) == 4);
    STATIC_REQUIRE(AlignmentV<Width128> == 16);
    STATIC_REQUIRE(AlignmentV<Width256> == 32);

    alignas(32) uint8_t buf[64] = {};
    CHECK(IsAligned<Width128>(buf));
    CHECK(IsAligned<Width256>(buf));
    CHECK(IsAligned<Width128>(buf + 16));
    CHECK_FALSE(IsAligned<Width256>(buf + 16));
    CHECK_FALSE(IsAligned<Width128>(buf + 1));
}
