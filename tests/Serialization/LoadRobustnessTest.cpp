#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include "Astra/Serialization/BinaryReader.hpp"

namespace
{
    // Little-endian append helpers for hand-built reader buffers.
    void AppendU64(std::vector<std::byte>& b, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    }
    void AppendBytes(std::vector<std::byte>& b, size_t n, std::byte fill = std::byte{0})
    {
        b.insert(b.end(), n, fill);
    }
}

// ReadBoundedCount rejects a count larger than the remaining buffer could hold.
TEST(LoadRobustness, ReadBoundedCountRejectsOversizedCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 1'000'000'000ull);   // claims a billion elements...
    // ...but nothing follows, so Remaining() after the count is 0.
    // Braced init (not parens) avoids C++'s most-vexing-parse: with parens,
    // `BinaryReader reader(std::span<const std::byte>(buf))` parses as a
    // function declaration for `reader`, not an object definition.
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const uint64_t n = reader.ReadBoundedCount(8);
    EXPECT_TRUE(reader.HasError());
    EXPECT_EQ(reader.GetError(), Astra::SerializationError::CorruptedData);
    EXPECT_EQ(n, 0u);
}

// ReadBoundedCount accepts a count the remaining buffer can justify.
TEST(LoadRobustness, ReadBoundedCountAcceptsFeasibleCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 2);                   // 2 elements...
    AppendBytes(buf, 16);                // ...16 bytes follow (8 each)
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const uint64_t n = reader.ReadBoundedCount(8);
    EXPECT_FALSE(reader.HasError());
    EXPECT_EQ(n, 2u);
}

// The POD-vector read must not integer-overflow its bounds check (size * sizeof(T)).
TEST(LoadRobustness, VectorReadRejectsOverflowingSize)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 0x2000000000000000ull); // 2^61; *8 wraps to 0 in the buggy check
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    std::vector<uint64_t> v;
    reader(v);                              // must set error, NOT resize(2^61)
    EXPECT_TRUE(reader.HasError());
}
