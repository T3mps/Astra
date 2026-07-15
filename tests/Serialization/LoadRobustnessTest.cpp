#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include "Astra/Archetype/Archetype.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Serialization/BinaryReader.hpp"
#include "../TestComponents.hpp"

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

// A chunk claiming more entities than its capacity must be rejected before
// Archetype::Deserialize writes anything into the chunk's fixed-size heap arena.
//
// Rationale for how this is built: the obvious approach -- Registry::Save() a
// real registry, then flip the on-disk chunkEntityCount byte -- requires a
// stable byte offset into the FULL Registry format (header + EntityManager +
// ArchetypeManager + Archetype), which depends on EntityManager's and
// ArchetypeManager's serialization layout as much as Archetype's own, and is
// brittle to pin deterministically. Archetype::Deserialize is a self-contained
// static method with its own well-defined wire format (visible directly above
// it, in Archetype::Serialize), so this test instead builds a minimal
// single-archetype, single-chunk buffer by hand with BinaryWriter -- mirroring
// Archetype::Serialize's field order -- and calls Archetype::Deserialize
// directly. That keeps the test anchored to the format that matters for this
// guard, not the whole-registry format.
//
// No entity or component payload follows the corrupted count: Deserialize's
// entity-reading loop (`for i in 0..chunkEntityCount: chunk->AddEntity(entity)`)
// runs unconditionally chunkEntityCount times regardless of what the reader has
// left, and AddEntity's only capacity guard is an ASTRA_ASSERT -- which compiles
// out in Release/Dist -- so even a payload-less corrupted count is enough to
// drive `chunkEntityCount` out-of-bounds placement-news into the chunk's
// fixed-size component arena. That is exactly the bug this test pins: with the
// production guard removed, this reliably crashes (heap corruption / access
// violation) well before chunkEntityCount iterations complete; with the guard
// in place, Deserialize returns Err immediately after reading chunkEntityCount
// and never reaches that loop.
TEST(LoadRobustness, ChunkEntityCountOverCapacityIsRejected)
{
    using namespace Astra::Test;

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position>();
    const Astra::ComponentID posId = Astra::TypeID<Position>::Value();
    const Astra::ComponentDescriptor* posDesc = cr->GetComponentDescriptor(posId);
    ASSERT_NE(posDesc, nullptr);

    std::vector<Astra::ComponentDescriptor> registryDescriptors;
    cr->GetAllDescriptors(registryDescriptors);

    std::vector<std::byte> buf;
    {
        // Parens (not braces) here are fine: `buf` is a named lvalue, not a
        // temporary, so there is no most-vexing-parse hazard.
        Astra::BinaryWriter writer(buf);

        Astra::ComponentMask mask;
        mask.Set(posId);
        for (size_t i = 0; i < Astra::ComponentMask::WORD_COUNT; ++i)
        {
            writer(mask.Data()[i]);
        }

        const uint64_t entitiesPerChunk = 4;          // real chunk capacity
        const uint32_t chunkEntityCount = 1'000'000;   // corrupted: far over capacity

        writer(static_cast<uint64_t>(1));              // archetype entityCount (informational)
        writer(entitiesPerChunk);
        writer(static_cast<uint32_t>(1));               // chunkCount = 1

        writer(static_cast<uint32_t>(1));               // descriptorCount = 1
        writer(posDesc->hash);
        writer(static_cast<uint64_t>(posDesc->size));
        writer(static_cast<uint64_t>(posDesc->alignment));
        writer(posDesc->version);

        // Chunk 0: just the corrupted per-chunk count. Nothing else needs to
        // follow -- see the comment above the test for why.
        writer(chunkEntityCount);

        ASSERT_FALSE(writer.HasError());
    }

    Astra::ArchetypeChunkPool pool;
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::Archetype::Deserialize(reader, registryDescriptors, &pool);
    EXPECT_TRUE(result.IsErr());   // must fail cleanly -- no crash, no OOB
}
