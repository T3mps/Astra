#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include "Astra/Archetype/Archetype.hpp"
#include "Astra/Archetype/ArchetypeManager.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Entity/EntityManager.hpp"
#include "Astra/Entity/EntityTable.hpp"
#include "Astra/Registry/Registry.hpp"
#include "Astra/Registry/Relations.hpp"
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

// CountExceedsRemaining rejects a count larger than the remaining buffer could hold.
TEST(LoadRobustness, CountExceedsRemainingRejectsOversizedCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 1'000'000'000ull);   // claims a billion elements...
    // ...but nothing follows, so Remaining() after the count is 0.
    // Braced init (not parens) avoids C++'s most-vexing-parse: with parens,
    // `BinaryReader reader(std::span<const std::byte>(buf))` parses as a
    // function declaration for `reader`, not an object definition.
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    uint64_t count;
    reader(count);
    ASSERT_FALSE(reader.HasError());
    EXPECT_TRUE(reader.CountExceedsRemaining(count, 8));
}

// CountExceedsRemaining accepts a count the remaining buffer can justify.
TEST(LoadRobustness, CountExceedsRemainingAcceptsFeasibleCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 2);                   // 2 elements...
    AppendBytes(buf, 16);                // ...16 bytes follow (8 each)
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    uint64_t count;
    reader(count);
    ASSERT_FALSE(reader.HasError());
    EXPECT_FALSE(reader.CountExceedsRemaining(count, 8));
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

// BinaryReader::ReadCompressedBlock's COMPRESSED branch allocates
// `std::vector<uint8_t> compressedData(compressedSize)` BEFORE calling
// ReadBytes to actually read that many bytes -- unlike the sibling
// UNCOMPRESSED branch a few lines above it, which already checks
// `originalSize > Remaining()` before allocating. compressedSize is a raw
// uint32_t read straight off the wire, so a corrupted save can claim up to
// ~4GB: the allocation (and its zero-init) happens regardless of how few
// bytes actually remain in the buffer, before ReadBytes' own bounds check
// ever gets a chance to reject it. That is an uncontrolled multi-GB
// allocation attempt (and, since Astra is built with exceptions off, a
// std::bad_alloc from it is not a catchable Result -- it is a process abort)
// reachable from a hand-corrupted save, not just a theoretical concern.
//
// Rationale for how this is built: ReadCompressedBlock is a small,
// self-contained BinaryReader method with its own well-defined two-field
// wire prefix (originalSize, then compressedSize, both uint32_t -- see the
// method just above BinaryReader's Serialize-side counterpart,
// WriteCompressedBlock, in BinaryWriter.hpp), so this test hand-builds just
// that prefix and calls ReadCompressedBlock directly rather than routing
// through a full Registry::Save()/Load() (which would need to be coaxed into
// actually emitting a compressed block in the first place). Nothing follows
// the corrupted compressedSize, so Remaining() is 0 once it's read.
TEST(LoadRobustness, ReadCompressedBlockRejectsOversizedCompressedSize)
{
    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);
        writer(static_cast<uint32_t>(64));           // originalSize -- irrelevant once compressedSize is rejected
        writer(static_cast<uint32_t>(0xFFFFFFF0u));  // compressedSize, corrupted: ~4GB, nothing follows it
        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};

    const auto start = std::chrono::steady_clock::now();
    auto result = reader.ReadCompressedBlock();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.IsErr());     // must fail cleanly -- no bad_alloc/abort
    EXPECT_TRUE(reader.HasError());
    EXPECT_EQ(reader.GetError(), Astra::SerializationError::CorruptedData);

    // The functional Err/Ok result alone cannot distinguish "rejected before
    // allocating" from "allocated ~4GB, zero-initialized it, THEN rejected
    // when ReadBytes' own bounds check catches the truncation" -- both paths
    // return Err (confirmed: without the fix below, this test's Err/HasError
    // assertions above already pass, taking ~1.2s locally to allocate and
    // zero-init the ~4GB vector first). The compressedSize > Remaining()
    // guard must reject BEFORE any allocation, so the whole call is
    // submillisecond; this bound is two orders of magnitude below the
    // unguarded cost, so it isn't sensitive to normal scheduling jitter.
    EXPECT_LT(elapsed, std::chrono::milliseconds(250));
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

    // The manager needs a (non-null) shared record table; this crafted buffer is
    // rejected at chunkIndex validation before any record is written, so no
    // version seeding is required here.
    Astra::EntityTable table;
    Astra::ArchetypeManager manager(cr, Astra::ArchetypeChunkPool::Config{}, &table);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const bool ok = manager.Deserialize(reader);
    EXPECT_FALSE(ok);   // must fail cleanly -- no OOB store, no crash
}

// ArchetypeManager::Deserialize's entity-to-archetype-map loop (2026-07-27
// review P0) used to pass the raw wire entity id straight to
// EntityTable::GetOrCreateRecord -- a CREATING accessor -- even though the
// loop's own comment states segments already exist by this point (EntityManager
// restored versions first). A crafted mapping id far outside anything
// EntityManager actually restored drives GetOrCreateSegment's
// `m_segmentIndex.resize(segIdx + 1, ...)` with an attacker-controlled segIdx:
// bounded-but-real memory amplification (and a silently-successful load on
// corrupt data) on the default 32-bit-id build, and an unbounded resize ->
// uncaught std::bad_alloc (process termination, since Astra builds with
// exceptions off) on 64-bit-id builds. The fix (spec §2) swaps in the
// existing non-creating, allocation-free EntityTable::GetRecord and rejects
// the load when it returns null.
//
// Rationale for how this is built: same self-contained-wire-format reasoning
// as EntityMapChunkIndexOutOfRangeIsRejected directly above -- a minimal
// single-archetype (root/empty mask), single-chunk, single-entity buffer,
// hand-built with BinaryWriter mirroring ArchetypeManager::Serialize's field
// order. archetypeIndex/chunkIndex/entityIndex are all left VALID here (unlike
// the sibling test) so control reaches the entity-id guard under test; only
// the mapping row's entity id is corrupted, to Entity::ID_MASK -- the largest
// id GetID() can ever yield (id is masked to ID_MASK bits by the Entity
// constructor, so this is "huge" in the sense of "maximum representable", not
// merely large) -- against a freshly-constructed EntityTable that has never
// created any segment. segIdx for that id is far beyond the empty table's
// zero-length segment index, so GetRecord's bounds-checked GetSegment must
// return null and reject the load; GetOrCreateRecord would instead resize and
// allocate a segment for it and let the load "succeed" on corrupt input.
TEST(LoadRobustness, EntityMapHugeEntityIdIsRejected)
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

        // Entity-to-archetype mapping: valid archetypeIndex/chunkIndex/entityIndex
        // (0,0,0) -- unlike the sibling test above, those all pass their guards
        // here so control actually reaches the entity-id guard under test --
        // but the mapping entity's id is corrupted to Entity::ID_MASK, the
        // largest id value GetID() can ever produce.
        writer(Astra::Entity(Astra::Entity::ID_MASK, 1));   // entity -- corrupted id
        writer(static_cast<uint32_t>(0));                    // archetypeIndex - valid
        writer(static_cast<uint32_t>(0));                    // chunkIndex - valid
        writer(static_cast<uint32_t>(0));                    // entityIndex - valid

        ASSERT_FALSE(writer.HasError());
    }

    // Fresh table: no segment has ever been created, so ANY id's segIdx is
    // out of range of the (empty) segment index -- GetRecord must return null.
    Astra::EntityTable table;
    Astra::ArchetypeManager manager(cr, Astra::ArchetypeChunkPool::Config{}, &table);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const bool ok = manager.Deserialize(reader);
    EXPECT_FALSE(ok);   // must fail cleanly -- no segment-index resize, no silent success
}

// Same guard as EntityMapHugeEntityIdIsRejected, but for a mapping id whose
// segment DOES exist (so GetRecord's segment lookup alone would not catch
// it) yet whose slot was never restored as alive -- i.e. version == 0
// (EntityTable::NULL_VERSION, the dead/empty marker). This is the "mapping
// references an entity EntityManager never restored" half of the guard the
// spec calls out (§2's "aliveness cross-check"): a null segment isn't the
// only way a mapping id can be bogus, a dead slot within an otherwise-real
// segment is another, and the version-based half of the predicate is what
// catches it. On current code, GetOrCreateRecord happily returns that dead
// slot's pointer (segment already exists) and SetRecordLocation writes an
// archetype/location into it -- WITHOUT ever touching rec->version (see the
// "NEVER assign rec->version" convention at every other GetOrCreateRecord call
// site in this file) -- so the entity ends up "located" while still formally
// dead: the load silently succeeds on corrupt data instead of being rejected.
//
// Rationale for how this is built: same minimal hand-built
// ArchetypeManager::Deserialize buffer as EntityMapHugeEntityIdIsRejected.
// The EntityTable is pre-populated with EntityTable::SetVersion for entity id
// 0 first, which forces GetOrCreateSegment to create the segment covering ids
// [0, entitiesPerSegment) (65536 by default) -- id 5 falls in that same
// segment but its slot's version was never touched, so it is still 0 (dead).
// The mapping row then targets id 5 with a non-zero wire version (1), so
// there is no ambiguity between "id 5's version legitimately restored as 0"
// (which can't happen -- 0 is never a live version) and "id 5's slot is dead".
TEST(LoadRobustness, EntityMapDeadEntityIdIsRejected)
{
    auto cr = std::make_shared<Astra::ComponentRegistry>();

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        writer(static_cast<uint32_t>(1));   // archetypeCount = 1 (root only)
        writer(static_cast<uint32_t>(1));   // entityCount = 1

        writer(static_cast<uint32_t>(0));   // archetype index

        Astra::ComponentMask mask;
        for (size_t i = 0; i < Astra::ComponentMask::WORD_COUNT; ++i)
        {
            writer(mask.Data()[i]);
        }

        writer(static_cast<uint64_t>(1));   // archetype entityCount
        writer(static_cast<uint64_t>(4));   // entitiesPerChunk (real chunk capacity)
        writer(static_cast<uint32_t>(1));   // chunkCount = 1

        writer(static_cast<uint32_t>(0));   // descriptorCount = 0 (no components)

        writer(static_cast<uint32_t>(1));   // chunkEntityCount
        writer(Astra::Entity(1, 1));        // entities[0]

        writer(static_cast<uint64_t>(1));   // trailing per-archetype entity count

        // Entity-to-archetype mapping: valid archetypeIndex/chunkIndex/entityIndex,
        // but the mapping's entity id (5) is a slot within a real, already-
        // created segment (see the table setup below) whose version was never
        // restored -- it is still EntityTable::NULL_VERSION (dead).
        writer(Astra::Entity(5, 1));        // entity -- id 5, wire version 1
        writer(static_cast<uint32_t>(0));   // archetypeIndex - valid
        writer(static_cast<uint32_t>(0));   // chunkIndex - valid
        writer(static_cast<uint32_t>(0));   // entityIndex - valid

        ASSERT_FALSE(writer.HasError());
    }

    // Force segment creation for the range containing id 5 by giving a
    // DIFFERENT id (0) in the same segment a real version first. Id 5's own
    // slot is left untouched -- still version 0 (dead) -- even though its
    // segment now exists.
    Astra::EntityTable table;
    table.SetVersion(static_cast<Astra::EntityTable::IDType>(0), static_cast<Astra::EntityTable::VersionType>(1));
    ASSERT_EQ(table.GetVersion(static_cast<Astra::EntityTable::IDType>(5)), 0u);   // id 5's slot is dead, not out of range

    Astra::ArchetypeManager manager(cr, Astra::ArchetypeChunkPool::Config{}, &table);
    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    const bool ok = manager.Deserialize(reader);
    EXPECT_FALSE(ok);   // must fail cleanly -- dead slot must not be given a location
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

// RelationshipGraph::Deserialize writes m_parents directly with no cycle
// check (unlike runtime SetParent, which calls IsAncestorOf to reject a
// cycle BEFORE writing). A corrupted/crafted save can therefore install a
// cyclic parent map; IsAncestorOf must not hang when a later runtime call
// (e.g. the next SetParent) walks such a map.
//
// Rationale for how this is built: same self-contained-wire-format reasoning
// as the other RelationshipGraph tests above. This test hand-builds a buffer
// mirroring RelationshipGraph::Serialize's format for two entities, A and B,
// with m_parents = { A -> B, B -> A } (a 2-cycle: child A's parent is B,
// child B's parent is A), followed by zero children-map entries and zero
// link-map entries (IsAncestorOf only ever walks m_parents, so nothing else
// is needed). It calls RelationshipGraph::Deserialize directly -- the exact
// load path a corrupt save would take -- confirms the cycle was actually
// installed (GetParent(A)==B and GetParent(B)==A, i.e. not a vacuous test),
// then calls IsAncestorOf with an ancestor C that is NOT part of the cycle,
// so the walk can never short-circuit on the first or second step: without a
// cycle guard, current alternates B,A,B,A,... forever and never equals C or
// becomes invalid. With the guard, the call must still RETURN a defined bool.
TEST(LoadRobustness, CyclicParentMapDoesNotHangIsAncestorOf)
{
    const Astra::Entity a(1, 1);
    const Astra::Entity b(2, 1);
    const Astra::Entity c(3, 1);   // not part of the cycle -- can never match

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        writer(static_cast<uint32_t>(2));   // parentCount = 2
        writer(a.GetValue());                // child = A
        writer(b.GetValue());                // parent = B   (A's parent is B)
        writer(b.GetValue());                // child = B
        writer(a.GetValue());                // parent = A   (B's parent is A -- cycle!)

        writer(static_cast<uint32_t>(0));   // parentWithChildrenCount = 0
        writer(static_cast<uint32_t>(0));   // linkedEntityCount = 0

        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::RelationshipGraph::Deserialize(reader);
    ASSERT_TRUE(result.IsOk());

    auto& graph = *result.GetValue();

    // Confirm the cyclic parent map was actually installed by the load path
    // (proves the crafted buffer is real, not vacuous).
    ASSERT_EQ(graph.GetParent(a), b);
    ASSERT_EQ(graph.GetParent(b), a);

    // Must RETURN (not hang), even though the parent chain from A never
    // reaches C and never naturally terminates.
    const bool ancestorResult = graph.IsAncestorOf(c, a);
    SUCCEED();   // reaching here proves the traversal did not infinite-loop
    EXPECT_FALSE(ancestorResult);   // C is never actually an ancestor of A

    // The same hazard is reachable through the public runtime API: the next
    // SetParent on an entity in the cycle calls IsAncestorOf internally.
    const Astra::Entity d(4, 1);
    graph.SetParent(d, a);   // must also RETURN, not hang
    EXPECT_EQ(graph.GetParent(d), a);
}

// BuildAncestorCache() has its own, independent cycle-detection branch (it walks
// m_parents directly rather than calling IsAncestorOf), so a cyclic parent map
// must be shown not to abort *that* traversal too. Same crafted buffer as
// CyclicParentMapDoesNotHangIsAncestorOf (A's parent is B, B's parent is A), but
// this test drives the cache-building path through the public entry point:
// Relations<>::ForEachAncestor(), which calls GetAncestorsCached() ->
// BuildAncestorCache() before it ever touches the ArchetypeManager, so a null
// ArchetypeManager still exercises the code under test. BuildAncestorCache()'s
// cycle-detection branch used to be `ASTRA_ASSERT(false, ...)`, which is fatal
// (aborts the process via std::abort) in Debug and any checked-release with no
// custom handler installed -- unlike IsAncestorOf's step-capped walk, it would
// NOT fall through to the break. This test's only assertion is that execution
// reaches SUCCEED(): before the fix, the process aborts partway through
// ForEachAncestor() and the test never gets there.
TEST(LoadRobustness, CyclicParentMapDoesNotAbortForEachAncestor)
{
    const Astra::Entity a(1, 1);
    const Astra::Entity b(2, 1);

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        writer(static_cast<uint32_t>(2));   // parentCount = 2
        writer(a.GetValue());                // child = A
        writer(b.GetValue());                // parent = B   (A's parent is B)
        writer(b.GetValue());                // child = B
        writer(a.GetValue());                // parent = A   (B's parent is A -- cycle!)

        writer(static_cast<uint32_t>(0));   // parentWithChildrenCount = 0
        writer(static_cast<uint32_t>(0));   // linkedEntityCount = 0

        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::RelationshipGraph::Deserialize(reader);
    ASSERT_TRUE(result.IsOk());

    // Relations<> needs a std::shared_ptr<const RelationshipGraph>; move the
    // deserialized graph out of the Result into one (mirrors Registry::Load's
    // own std::make_shared<RelationshipGraph>(std::move(*graphResult.GetValue()))).
    auto graph = std::make_shared<Astra::RelationshipGraph>(std::move(*result.GetValue()));

    // Confirm the cyclic parent map was actually installed by the load path
    // (proves the crafted buffer is real, not vacuous).
    ASSERT_EQ(graph->GetParent(a), b);
    ASSERT_EQ(graph->GetParent(b), a);

    // No ArchetypeManager needed -- ForEachAncestor() unconditionally builds the
    // ancestor cache (the code under test) before it would ever dereference one.
    Astra::Relations<> relations(nullptr, a, graph);

    // Must RETURN (not abort), even though BuildAncestorCache's walk up the
    // parent chain from A never terminates naturally. The callback takes
    // (Entity, depth) -- ForEachAncestor's unfiltered instantiation always
    // calls func(entity, depth), even though it's unreachable here since
    // m_archetypeManager is null.
    relations.ForEachAncestor([](Astra::Entity, size_t) {});
    SUCCEED();   // reaching here proves the traversal did not abort the process
}

// Task 6: the offset-agnostic integration net over the whole robustness floor
// (Tasks 1-5b). Rather than pinning to one field's byte offset the way the
// tests above do, this builds one real, rich Registry::Save() -- several
// entities, multiple component types (trivial and non-trivial/Name), a
// parent/child relationship, and a recycled entity slot (destroy then create
// again so the free list is exercised) -- and truncates it at every possible
// prefix length. Every prefix, from empty to full, must make Registry::Load
// return Ok or Err; it must never crash, read/write out of bounds, or hang.
// Reaching the next loop iteration (and eventually SUCCEED()) is the
// assertion -- there is no meaningful expectation on the Result itself,
// since most prefixes are legitimately corrupt and are expected to Err.
TEST(LoadRobustness, TruncationSweepNeverCrashes)
{
    using namespace Astra::Test;

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity, Health, Name>();
    auto a = reg.CreateEntityWith(Position{1, 2, 3}, Velocity{4, 5, 6});
    auto b = reg.CreateEntityWith(Health{100, 100}, Name{"child"});
    reg.SetParent(b, a);
    reg.DestroyEntity(reg.CreateEntity());   // create a recycled slot

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity, Health, Name>();

    // Every truncation prefix must return Ok or Err -- never crash/OOB/hang.
    for (size_t len = 0; len <= full.size(); ++len)
    {
        std::vector<std::byte> t(full.begin(), full.begin() + len);
        auto r = Astra::Registry::Load(t, cr);
        (void)r;   // reaching the next iteration is the assertion
    }
    SUCCEED();
}

// Task 6, part two: same reasoning as TruncationSweepNeverCrashes above, but
// instead of truncating, this corrupts a real Registry::Save() in place --
// every aligned 4-byte window is overwritten with 0xFFFFFFFF (the pattern
// most likely to turn a count/index field into a huge or out-of-range value)
// and separately with 0x00000000 -- and asserts Load still only ever returns
// Ok or Err for each corrupted copy. Together with the truncation sweep this
// exercises every buffer-derived bound and OOB guard added across Tasks 1-5b
// without depending on any single field's byte offset.
TEST(LoadRobustness, ByteCorruptionSweepNeverCrashes)
{
    using namespace Astra::Test;

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity, Health, Name>();
    reg.CreateEntityWith(Position{1, 2, 3}, Velocity{4, 5, 6});
    reg.CreateEntityWith(Health{9, 100}, Name{"x"});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity, Health, Name>();

    // Overwrite each aligned 4-byte window with 0xFFFFFFFF and with 0x00000000.
    for (size_t off = 0; off + 4 <= full.size(); off += 4)
    {
        for (std::byte fill : { std::byte{0xFF}, std::byte{0x00} })
        {
            std::vector<std::byte> c = full;
            for (int i = 0; i < 4; ++i) c[off + i] = fill;
            auto r = Astra::Registry::Load(c, cr);
            (void)r;
        }
    }
    SUCCEED();
}

// EntityManager::Deserialize must validate that entitiesPerSegment,
// entitiesPerSegmentShift, and entitiesPerSegmentMask form a mutually
// consistent power-of-two triple BEFORE EntityTable ever sizes or indexes a
// segment from them (GetOrCreateSegment / Segment::ToLocal). A valid save
// always writes entitiesPerSegment as a power of two, shift ==
// log2(entitiesPerSegment), and mask == entitiesPerSegment - 1; corrupting
// any one of the three in isolation breaks that relationship and can
// otherwise drive a multi-GB m_segmentIndex resize, a heap OOB write into
// Segment::versions[], or the ASTRA_ASSERT fail-fast inside Segment::ToLocal
// (compiled out in Release/Dist, but an uncatchable process abort in Debug).
//
// Rationale for how this is built: unlike the self-contained-wire-format
// tests above, this test corrupts a REAL Registry::Save() buffer in place,
// because the byte offset of EntityManager's config block is only stable
// relative to the WHOLE-Registry format: a fixed 32-byte BinaryHeader
// (static_assert'd to exactly 32 bytes -- see BinaryArchive.hpp), followed
// immediately by EntityManager::Serialize's field order, which starts with
// entitiesPerSegment, then entitiesPerSegmentShift, then
// entitiesPerSegmentMask (each IDType-sized, back to back with no padding --
// BinaryWriter's POD path writes exactly sizeof(T) bytes per field).
TEST(LoadRobustness, EntityManagerSegmentConfigInconsistencyIsRejected)
{
    using namespace Astra::Test;
    using IDType = Astra::EntityManager::IDType;

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity>();
    reg.CreateEntityWith(Position{1, 2, 3}, Velocity{4, 5, 6});
    reg.CreateEntityWith(Position{7, 8, 9}, Velocity{1, 1, 1});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity>();

    constexpr size_t kHeaderSize = 32;   // BinaryHeader is static_assert'd to exactly 32 bytes.
    constexpr size_t kEntitiesPerSegmentOffset = kHeaderSize;
    constexpr size_t kEntitiesPerSegmentShiftOffset = kHeaderSize + sizeof(IDType);
    constexpr size_t kEntitiesPerSegmentMaskOffset = kHeaderSize + 2 * sizeof(IDType);

    auto corruptedCopy = [&](size_t offset, IDType value)
    {
        std::vector<std::byte> c = full;
        std::memcpy(c.data() + offset, &value, sizeof(value));
        return c;
    };

    // entitiesPerSegment corrupted to 0 -- fails the nonzero-power-of-two check.
    {
        auto c = corruptedCopy(kEntitiesPerSegmentOffset, IDType{0});
        auto r = Astra::Registry::Load(c, cr);
        EXPECT_TRUE(r.IsErr());   // must fail cleanly -- no crash, no OOB
    }

    // entitiesPerSegment corrupted to an all-ones value -- nonzero, but not a
    // power of two (multiple bits set).
    {
        auto c = corruptedCopy(kEntitiesPerSegmentOffset, static_cast<IDType>(~IDType{0}));
        auto r = Astra::Registry::Load(c, cr);
        EXPECT_TRUE(r.IsErr());
    }

    // entitiesPerSegmentShift corrupted -- entitiesPerSegment is untouched
    // (still a valid power of two), but 1 << shift no longer reproduces it.
    {
        auto c = corruptedCopy(kEntitiesPerSegmentShiftOffset, IDType{31});
        auto r = Astra::Registry::Load(c, cr);
        EXPECT_TRUE(r.IsErr());
    }

    // entitiesPerSegmentMask corrupted -- entitiesPerSegment/shift are
    // untouched, but mask no longer equals entitiesPerSegment - 1.
    {
        auto c = corruptedCopy(kEntitiesPerSegmentMaskOffset, static_cast<IDType>(~IDType{0}));
        auto r = Astra::Registry::Load(c, cr);
        EXPECT_TRUE(r.IsErr());
    }
}

// Archetype::Deserialize's per-chunk sanity guard
// (`entitiesPerChunk * perEntitySize + alignmentOverhead > poolChunkSize`) is
// vacuous for the root (zero-component) archetype every registry always has:
// perEntitySize is always 0 for it, so the product is always 0 and the guard
// never rejects any entitiesPerChunk value. A corrupted entitiesPerChunk then
// reaches ArchetypeChunkPool::Chunk's constructor, which unconditionally does
// `m_entities.reserve(entitiesPerChunk)` with no bound against the pool's
// actual chunk size -- an uncaught std::bad_alloc that violates the
// never-throw / Result contract Registry::Load is built on.
//
// Rationale for how this is built: same reasoning as
// EntityManagerSegmentConfigInconsistencyIsRejected above -- this corrupts a
// REAL Registry::Save() buffer in place, because the byte offset of the root
// archetype's entitiesPerChunk field is only stable relative to the WHOLE
// on-disk format (BinaryHeader, then EntityManager::Serialize, then
// ArchetypeManager::Serialize, then Archetype::Serialize for the root
// archetype). An empty Registry (zero entities) is used so EntityManager's
// variable-length alive/recycled sections contribute a fixed, known size,
// making the offset arithmetic below exact rather than approximate.
TEST(LoadRobustness, RootArchetypeEntitiesPerChunkUnboundedIsRejected)
{
    using IDType = Astra::EntityManager::IDType;

    Astra::Registry reg;   // No entities created -- ArchetypeManager's
                            // constructor still always creates the root
                            // (zero-component) archetype, and it always
                            // round-trips through Save/Load even when empty.

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();

    // Byte layout for an empty Registry::Save(), field-by-field from
    // Registry::Save / EntityManager::Serialize / ArchetypeManager::Serialize
    // / Archetype::Serialize (all POD fields, written back-to-back with no
    // padding -- BinaryWriter::operator()(const T&) does
    // WriteBytes(&value, sizeof(T))):
    //   [0, 32)    BinaryHeader (static_assert'd to exactly 32 bytes)
    //   EntityManager::Serialize (0 alive entities, 0 recycled entries):
    //     entitiesPerSegment(IDType) + entitiesPerSegmentShift(IDType) +
    //     entitiesPerSegmentMask(IDType) + releaseThreshold(float) +
    //     autoRelease(bool) + maxEmptySegments(uint64_t) + nextFreshID(IDType)
    //     + recycledCount(uint32_t)=0 + aliveCount(uint32_t)=0
    //   ArchetypeManager::Serialize:
    //     archetypeCount(uint32_t)=1 (root only) + entityMapCount(uint32_t)=0
    //   Archetype record 0 (the root archetype):
    //     archetype index(uint32_t)=0, then Archetype::Serialize:
    //       ComponentMask (Astra::ComponentMask::WORD_COUNT * uint64_t words,
    //       all zero for the root archetype) + m_entityCount(uint64_t)=0 +
    //       maxChunkEntityCount(uint64_t)  <-- target field
    constexpr size_t kHeaderSize = 32;
    constexpr size_t kEntityManagerBlockSize =
        3 * sizeof(IDType) + sizeof(float) + sizeof(bool) + sizeof(uint64_t) +
        sizeof(IDType) + sizeof(uint32_t) + sizeof(uint32_t);
    constexpr size_t kArchetypeManagerHeaderSize = sizeof(uint32_t) + sizeof(uint32_t);
    constexpr size_t kArchetypeIndexSize = sizeof(uint32_t);
    constexpr size_t kMaskSize = Astra::ComponentMask::WORD_COUNT * sizeof(uint64_t);
    constexpr size_t kEntitiesPerChunkOffset =
        kHeaderSize + kEntityManagerBlockSize + kArchetypeManagerHeaderSize +
        kArchetypeIndexSize + kMaskSize + sizeof(uint64_t) /* m_entityCount */;

    ASSERT_GE(full.size(), kEntitiesPerChunkOffset + sizeof(uint64_t));

    // Sanity-check the offset actually lands on the real field before
    // corrupting it -- this confirms the test isn't silently corrupting the
    // wrong bytes if the wire format ever shifts. The field's on-disk POSITION
    // and WIDTH are unchanged; Phase 2 (variable-capacity chunks, Task 4)
    // changed what it carries: Archetype::Serialize now writes the MAXIMUM
    // per-chunk entity count (floored at 1) instead of a uniform per-chunk
    // capacity, because chunks of one archetype no longer share a capacity.
    // This registry is empty, so the root archetype's single chunk holds 0
    // entities and the floor makes the legitimate value 1 (it was 256 under
    // the retired uniform-capacity write). What the reader does with the
    // field -- bound every chunkEntityCount against it -- is unchanged, and so
    // is this test's point: an unbounded value must be rejected, not reserved.
    uint64_t existing;
    std::memcpy(&existing, full.data() + kEntitiesPerChunkOffset, sizeof(existing));
    ASSERT_EQ(existing, 1u);

    // Corrupt to a huge value -- same order of magnitude as the sweep's
    // original repro (~1.1 TB in entity-count terms). perEntitySize is 0 for
    // the root archetype, so the OLD guard's product term is always 0 and
    // never rejects this, no matter how large entitiesPerChunk is.
    const uint64_t corrupted = uint64_t{1} << 40;
    std::vector<std::byte> c = full;
    std::memcpy(c.data() + kEntitiesPerChunkOffset, &corrupted, sizeof(corrupted));

    auto r = Astra::Registry::Load(c, cr);
    EXPECT_TRUE(r.IsErr());   // must fail cleanly -- no std::bad_alloc thrown
}

// EntityManager::Deserialize's alive-entity restore loop reads each entity's
// raw id and passes it straight to EntityTable::SetVersion, which resolves
// (and may create) a segment via `segIdx = id >> shift` and then indexes into
// it via Segment::ToLocal -- guarded only by an ASTRA_ASSERT, which compiles
// out in Release/Dist. A corrupted id near IDType max makes
// Segment::Contains's bounds check overflow-wrap, firing that assert as an
// uncatchable process abort in Debug builds.
//
// Rationale for how this is built: same reasoning as
// EntityManagerSegmentConfigInconsistencyIsRejected -- corrupts a REAL
// Registry::Save() buffer in place, since the byte offset of an alive
// entity's id field is only stable relative to the whole EntityManager
// block, which itself is only stable relative to the fixed 32-byte
// BinaryHeader. Two entities with no destroys keeps recycledCount at 0, so
// the alive-entity records immediately follow aliveCount with no variable-
// length recycled section in between.
TEST(LoadRobustness, AliveEntityIdOutOfRangeIsRejected)
{
    using namespace Astra::Test;
    using IDType = Astra::EntityManager::IDType;
    using VersionType = Astra::EntityManager::VersionType;

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity>();
    reg.CreateEntityWith(Position{1, 2, 3}, Velocity{4, 5, 6});
    reg.CreateEntityWith(Position{7, 8, 9}, Velocity{1, 1, 1});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity>();

    constexpr size_t kHeaderSize = 32;   // BinaryHeader is static_assert'd to exactly 32 bytes.
    // Three IDType entitiesPerSegment{,Shift,Mask} fields precede releaseThreshold.
    constexpr size_t kReleaseThresholdOffset = kHeaderSize + 3 * sizeof(IDType);
    constexpr size_t kAutoReleaseOffset = kReleaseThresholdOffset + sizeof(float);
    constexpr size_t kMaxEmptySegmentsOffset = kAutoReleaseOffset + sizeof(bool);
    constexpr size_t kNextFreshIDOffset = kMaxEmptySegmentsOffset + sizeof(uint64_t);
    constexpr size_t kRecycledCountOffset = kNextFreshIDOffset + sizeof(IDType);
    constexpr size_t kAliveCountOffset = kRecycledCountOffset + sizeof(uint32_t);
    constexpr size_t kAliveEntitiesOffset = kAliveCountOffset + sizeof(uint32_t);
    constexpr size_t kAliveEntityRecordSize = sizeof(IDType) + sizeof(VersionType);
    // The second alive entity's id field -- matches where the byte-corruption
    // sweep originally found this hole (offset ~76 for a similarly-shaped
    // save; see task-7-report.md).
    constexpr size_t kSecondAliveEntityIdOffset = kAliveEntitiesOffset + kAliveEntityRecordSize;

    ASSERT_GE(full.size(), kSecondAliveEntityIdOffset + sizeof(IDType));

    // Sanity-check the offset actually lands on a real, legitimate id before
    // corrupting it (must be well under Entity::ID_MASK for a 2-entity save).
    IDType existingId;
    std::memcpy(&existingId, full.data() + kSecondAliveEntityIdOffset, sizeof(existingId));
    ASSERT_LT(existingId, Astra::Entity::ID_MASK);

    // Corrupt to IDType's max value -- mirrors the sweep's original repro
    // (flipping an id's high 16 bits landed it near IDType max), which is
    // what makes Segment::Contains's `id < baseID + capacity` overflow-wrap
    // and fire the ASTRA_ASSERT fail-fast pre-fix. Also well past
    // Entity::ID_MASK, the "IDs exhausted" sentinel Allocate()/
    // AllocateBatch() never legitimately hand out, so the new bound must
    // reject it regardless.
    const IDType corrupted = std::numeric_limits<IDType>::max();
    std::vector<std::byte> c = full;
    std::memcpy(c.data() + kSecondAliveEntityIdOffset, &corrupted, sizeof(corrupted));

    auto r = Astra::Registry::Load(c, cr);
    EXPECT_TRUE(r.IsErr());   // must fail cleanly -- no abort, no OOB
}

// EntityManager::Deserialize's segment-config validation (added in 36ee6a6)
// checks that entitiesPerSegment/Shift/Mask form a mutually consistent
// power-of-two triple, and caps entitiesPerSegment from above -- but has no
// LOWER bound. {entitiesPerSegment=1, shift=0, mask=0} is internally
// consistent (1 is a power of two, 1<<0==1, mask==1-1==0) and passes that
// check untouched. With shift==0, EntityTable::GetOrCreateSegment's
// `segIdx = id >> shift` becomes `segIdx = id`, so restoring even one
// legitimate (well under Entity::ID_MASK, so it also passes the existing
// id >= ID_MASK check) alive entity with a several-million id drives
// `m_segmentIndex.resize(segIdx + 1)` to a several-million-entry
// std::vector<size_t> -- tens of MB for one entity, unbounded as the id
// grows. This must be rejected before EntityTable::SetVersion ever resolves
// a segment from it.
//
// Rationale for how this is built: same self-contained-wire-format
// reasoning as EntityManagerRecycledCountOverBufferIsRejected -- this
// hand-builds EntityManager::Serialize's exact field order/types directly
// with BinaryWriter rather than corrupting a real Registry::Save(), because
// what matters here is the (config, id) pairing, not any particular save's
// byte offsets.
TEST(LoadRobustness, EntityManagerAliveIdSegmentIndexExplosionIsRejected)
{
    using IDType = Astra::EntityManager::IDType;
    using VersionType = Astra::EntityManager::VersionType;

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        // Table config fields, in EntityManager::Serialize's exact order/types.
        writer(static_cast<IDType>(1));               // entitiesPerSegment -- corrupted: below the Config clamp floor (1024)
        writer(static_cast<IDType>(0));               // entitiesPerSegmentShift -- consistent with entitiesPerSegment=1 (1<<0==1)
        writer(static_cast<IDType>(0));               // entitiesPerSegmentMask -- consistent (1-1==0)
        writer(0.1f);                                  // releaseThreshold
        writer(true);                                   // autoRelease
        writer(static_cast<uint64_t>(2));                // maxEmptySegments

        writer(static_cast<IDType>(15'000'001));          // ID stack nextFreshID

        writer(static_cast<uint32_t>(0));                  // recycledCount = 0

        writer(static_cast<uint32_t>(1));                   // aliveCount = 1

        // The one alive entity: id is legitimate (well under Entity::ID_MASK,
        // ~16.7M for the default 32-bit-ID/8-bit-version build), so it passes
        // the existing id >= Entity::ID_MASK check untouched -- the only thing
        // wrong here is what the corrupted config turns it into (segIdx == id).
        writer(static_cast<IDType>(15'000'000));            // id
        writer(static_cast<VersionType>(1));                 // version

        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::EntityManager::Deserialize(reader);
    EXPECT_TRUE(result.IsErr());   // must fail cleanly -- no multi-MB m_segmentIndex resize
}

// Same hazard as EntityManagerAliveIdSegmentIndexExplosionIsRejected above,
// but through the recycled-entry restore loop instead of the alive-entity
// one. RestoreRecycledEntries() itself never touches EntityTable/segments
// during Load -- the danger is deferred: the next legitimate Create() call
// after Load hands this id back out via EntityIDStack::Allocate(), which
// flows straight into EntityTable::SetVersion(id, ...) and the same
// `segIdx = id >> shift` explosion, just outside of Load's own Result-based
// error contract and far harder to trace back to the corrupted save. The
// recycled-entry loop must reject this at Load time, before the id ever
// re-enters circulation.
TEST(LoadRobustness, EntityManagerRecycledIdSegmentIndexExplosionIsRejected)
{
    using IDType = Astra::EntityManager::IDType;
    using VersionType = Astra::EntityManager::VersionType;

    std::vector<std::byte> buf;
    {
        Astra::BinaryWriter writer(buf);

        writer(static_cast<IDType>(1));                // entitiesPerSegment -- corrupted: below the Config clamp floor (1024)
        writer(static_cast<IDType>(0));                // entitiesPerSegmentShift
        writer(static_cast<IDType>(0));                // entitiesPerSegmentMask
        writer(0.1f);                                   // releaseThreshold
        writer(true);                                    // autoRelease
        writer(static_cast<uint64_t>(2));                 // maxEmptySegments

        writer(static_cast<IDType>(15'000'001));           // ID stack nextFreshID

        writer(static_cast<uint32_t>(1));                   // recycledCount = 1
        writer(static_cast<IDType>(15'000'000));             // recycled id -- legitimate (< Entity::ID_MASK)
        writer(static_cast<VersionType>(2));                  // recycled entry's nextVersion

        writer(static_cast<uint32_t>(0));                      // aliveCount = 0

        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    auto result = Astra::EntityManager::Deserialize(reader);
    EXPECT_TRUE(result.IsErr());   // must fail cleanly -- no id re-enters circulation to explode later
}

// 2026-09-11 grading finding S1: Archetype::Deserialize rebuilt every chunk from
// its own validated chunkEntityCount but then assigned m_entityCount from the
// HEADER's wire value, and ArchetypeManager::Deserialize read the trailing
// per-archetype count "for validation" and discarded it. A crafted archive could
// therefore leave m_entityCount disagreeing with what the chunks hold, and
// Registry::Defragment trusts that field (it frees an archetype whose count reads
// 0 and sizes compaction from it). Both counts must agree with the chunk sum or
// the load is refused.
//
// Layout: same empty-registry buffer as RootArchetypeEntitiesPerChunkUnboundedIsRejected.
// The root archetype record is: index(u32), mask, m_entityCount(u64)  <-- header count,
// maxChunkEntityCount(u64), chunkCount(u32)=1, descriptorCount(u32)=0,
// chunk[0].entityCount(u32)=0, then the manager's trailing entityCount(u64).
namespace
{
    struct EmptyRootArchetypeOffsets
    {
        size_t headerCount;     // Archetype::Serialize's m_entityCount
        size_t trailingCount;   // ArchetypeManager::Serialize's "for validation" count
    };

    EmptyRootArchetypeOffsets LocateEmptyRootArchetypeCounts(const std::vector<std::byte>& full)
    {
        using IDType = Astra::EntityManager::IDType;
        constexpr size_t kHeaderSize = 32;
        constexpr size_t kEntityManagerBlockSize =
            3 * sizeof(IDType) + sizeof(float) + sizeof(bool) + sizeof(uint64_t) +
            sizeof(IDType) + sizeof(uint32_t) + sizeof(uint32_t);
        constexpr size_t kArchetypeManagerHeaderSize = sizeof(uint32_t) + sizeof(uint32_t);
        constexpr size_t kArchetypeIndexSize = sizeof(uint32_t);
        constexpr size_t kMaskSize = Astra::ComponentMask::WORD_COUNT * sizeof(uint64_t);
        const size_t headerCount = kHeaderSize + kEntityManagerBlockSize + kArchetypeManagerHeaderSize +
                                   kArchetypeIndexSize + kMaskSize;
        const size_t maxChunk    = headerCount + sizeof(uint64_t);
        const size_t chunkCount  = maxChunk + sizeof(uint64_t);
        const size_t descCount   = chunkCount + sizeof(uint32_t);
        const size_t chunk0Count = descCount + sizeof(uint32_t);
        const size_t trailing    = chunk0Count + sizeof(uint32_t);
        EXPECT_GE(full.size(), trailing + sizeof(uint64_t));

        // Sanity-check every field on the path so a wire-format shift fails loudly
        // instead of corrupting the wrong bytes.
        uint64_t v64 = 0; uint32_t v32 = 0;
        std::memcpy(&v64, full.data() + headerCount, sizeof(v64)); EXPECT_EQ(v64, 0u);   // empty root: 0 entities
        std::memcpy(&v64, full.data() + maxChunk,    sizeof(v64)); EXPECT_EQ(v64, 1u);   // floored max chunk count
        std::memcpy(&v32, full.data() + chunkCount,  sizeof(v32)); EXPECT_EQ(v32, 1u);   // one (empty) chunk
        std::memcpy(&v32, full.data() + descCount,   sizeof(v32)); EXPECT_EQ(v32, 0u);   // no descriptors
        std::memcpy(&v32, full.data() + chunk0Count, sizeof(v32)); EXPECT_EQ(v32, 0u);   // chunk holds 0
        std::memcpy(&v64, full.data() + trailing,    sizeof(v64)); EXPECT_EQ(v64, 0u);   // trailing count 0
        return {headerCount, trailing};
    }
}

TEST(LoadRobustness, ArchetypeHeaderEntityCountDisagreeingWithChunksIsRejected)
{
    Astra::Registry reg;
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());
    const auto off = LocateEmptyRootArchetypeCounts(full);
    if (::testing::Test::HasFailure()) return;

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    {
        auto clean = Astra::Registry::Load(full, cr);
        ASSERT_TRUE(clean.IsOk()) << "precondition: the uncorrupted buffer must load (error "
                                  << static_cast<int>(*clean.GetError()) << ")";
    }

    // The chunks hold 0 entities; the header claims 7.
    const uint64_t corrupted = 7;
    std::vector<std::byte> c = full;
    std::memcpy(c.data() + off.headerCount, &corrupted, sizeof(corrupted));

    // The archive checksum is verified only at the END of Registry::Load, after
    // every archetype has already been rebuilt -- so "Load fails" alone proves
    // nothing (it failed on the checksum even before the fix, with m_entityCount
    // already poisoned). The count check must refuse FIRST, with CorruptedData.
    auto r = Astra::Registry::Load(c, cr);
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(*r.GetError(), Astra::SerializationError::CorruptedData)
        << "must be refused by the archetype count check, not caught later by the checksum";
}

TEST(LoadRobustness, ArchetypeTrailingEntityCountDisagreeingWithChunksIsRejected)
{
    Astra::Registry reg;
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());
    const auto off = LocateEmptyRootArchetypeCounts(full);
    if (::testing::Test::HasFailure()) return;

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    {
        auto clean = Astra::Registry::Load(full, cr);
        ASSERT_TRUE(clean.IsOk()) << "precondition: the uncorrupted buffer must load (error "
                                  << static_cast<int>(*clean.GetError()) << ")";
    }

    // Header and chunks agree (0); the manager's trailing count claims 7.
    const uint64_t corrupted = 7;
    std::vector<std::byte> c = full;
    std::memcpy(c.data() + off.trailingCount, &corrupted, sizeof(corrupted));

    auto r = Astra::Registry::Load(c, cr);
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(*r.GetError(), Astra::SerializationError::CorruptedData)
        << "must be refused by the manager's trailing-count check, not caught later by the checksum";
}
