#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include "Astra/Archetype/Archetype.hpp"
#include "Astra/Archetype/ArchetypeManager.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Entity/EntityManager.hpp"
#include "Astra/Registry/RelationshipGraph.hpp"
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

// ArchetypeManager::Deserialize's entity-to-archetype-map loop must validate a
// record's chunkIndex/entityIndex against the archetype's real chunk layout
// before storing the entity location. By the time this loop runs, every
// archetype (and its chunks) from the archetype loop above it already exists,
// so a corrupted chunkIndex would otherwise be stored raw -- later making a
// GetComponent/iteration index a chunk out of bounds (OOB read/write). A bad
// archetypeIndex used to be silently skipped rather than rejected; that is
// also covered here indirectly (this test corrupts chunkIndex, not
// archetypeIndex, but both paths share the same "return false" convention).
//
// Rationale for how this is built: pinning a stable byte offset into a real
// Registry::Save() (header + EntityManager + ArchetypeManager + Archetype +
// RelationshipGraph) to flip one chunkIndex byte is brittle, for the same
// reason noted above ChunkEntityCountOverCapacityIsRejected. ArchetypeManager
// has its own well-defined wire format (visible directly above Deserialize,
// in ArchetypeManager::Serialize, which itself calls Archetype::Serialize per
// archetype), so this test builds a minimal single-archetype (root/empty
// mask), single-chunk, single-entity buffer by hand with BinaryWriter --
// mirroring that field order -- and calls ArchetypeManager::Deserialize
// directly. Using the empty-mask root archetype means descriptorCount == 0,
// so no component payload needs to be hand-encoded (no compression, no
// per-component serializer to mimic) -- keeping the buffer anchored purely to
// ArchetypeManager's + Archetype's fixed-field wire format.
TEST(LoadRobustness, EntityMapChunkIndexOutOfRangeIsRejected)
{
    auto cr = std::make_shared<Astra::ComponentRegistry>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        writer(static_cast<uint32_t>(1));   // archetypeCount = 1 (root only)
        writer(static_cast<uint32_t>(1));   // entityCount = 1

        // Archetype record 0: the root archetype (empty mask), one chunk,
        // one entity, no components.
        writer(static_cast<uint32_t>(0));   // archetype index

        Astra::ComponentMask mask;          // default-constructed -> all-zero (empty) mask
        for (size_t i = 0; i < Astra::ComponentMask::WORD_COUNT; ++i)
        {
            writer(mask.Data()[i]);
        }

        writer(static_cast<uint64_t>(1));   // archetype entityCount
        writer(static_cast<uint64_t>(4));   // entitiesPerChunk (real chunk capacity)
        writer(static_cast<uint32_t>(1));   // chunkCount = 1

        writer(static_cast<uint32_t>(0));   // descriptorCount = 0 (no components)

        // Chunk 0: one entity, no component arrays to follow (descriptorCount == 0).
        writer(static_cast<uint32_t>(1));   // chunkEntityCount
        writer(Astra::Entity(1, 1));        // entities[0]

        // Trailing per-archetype entity count (ArchetypeManager::Serialize
        // writes this immediately after Archetype::Serialize returns).
        writer(static_cast<uint64_t>(1));

        // Entity-to-archetype mapping: valid archetypeIndex (0), but the
        // archetype above has only one chunk (index 0) -- chunkIndex here is
        // corrupted to reference a chunk that does not exist.
        writer(Astra::Entity(1, 1));                  // entity
        writer(static_cast<uint32_t>(0));              // archetypeIndex - valid
        writer(static_cast<uint32_t>(0xFFFFFFFFu));     // chunkIndex - out of range
        writer(static_cast<uint32_t>(0));              // entityIndex

        ASSERT_FALSE(writer.HasError());
    }

    Astra::ArchetypeManager manager(cr);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const bool ok = manager.Deserialize(reader);
    EXPECT_FALSE(ok);   // must fail cleanly -- no OOB store, no crash
}

// EntityManager::Deserialize must not let a corrupted recycledCount drive a
// multi-GB std::vector::reserve.
//
// Rationale for how this is built: same as the tests above -- pinning a byte
// offset into a full Registry::Save() is brittle because it depends on the
// header + EntityManager's own layout together. EntityManager::Deserialize is
// a self-contained static method with its own well-defined wire format
// (visible directly above it, in EntityManager::Serialize), so this test
// hand-builds just that method's prefix by hand with BinaryWriter --
// mirroring Serialize's field order and types exactly through recycledCount
// -- and calls EntityManager::Deserialize directly. Nothing follows the
// corrupted count, so Remaining() is 0 and no count could justify it.
TEST(LoadRobustness, EntityManagerRecycledCountOverBufferIsRejected)
{
    using IDType = Astra::EntityManager::IDType;

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        // Table config fields, in EntityManager::Serialize's exact order/types.
        writer(static_cast<IDType>(1024));           // entitiesPerSegment
        writer(static_cast<IDType>(10));              // entitiesPerSegmentShift
        writer(static_cast<IDType>(1023));             // entitiesPerSegmentMask
        writer(0.1f);                                   // releaseThreshold
        writer(true);                                    // autoRelease
        writer(static_cast<uint64_t>(2));                 // maxEmptySegments

        writer(static_cast<IDType>(5));                    // ID stack nextFreshID

        // Corrupted: claims ~4 billion recycled entries; nothing follows.
        writer(static_cast<uint32_t>(0xFFFFFFFFu));          // recycledCount

        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::EntityManager::Deserialize(reader);
    EXPECT_TRUE(result.IsErr());   // must fail cleanly -- no multi-GB reserve
}

// RelationshipGraph::Deserialize must not let a corrupted parentCount drive a
// multi-GB FlatMap::Reserve.
//
// Rationale for how this is built: same self-contained-wire-format reasoning
// as EntityManagerRecycledCountOverBufferIsRejected above.
// RelationshipGraph::Serialize's wire format opens with parentCount as a
// uint32_t, followed by that many (child, parent) Entity::StorageType pairs;
// this test hand-builds just the corrupted count with nothing following it.
TEST(LoadRobustness, RelationshipGraphParentCountOverBufferIsRejected)
{
    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);
        writer(static_cast<uint32_t>(0xFFFFFFFFu));   // parentCount, corrupted
        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::RelationshipGraph::Deserialize(reader);
    EXPECT_TRUE(result.IsErr());   // must fail cleanly -- no multi-GB reserve
}
