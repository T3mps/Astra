#include <gtest/gtest.h>

#include <Astra/Entity/EntityTable.hpp>
#include <Astra/Entity/EntityRecord.hpp>

using namespace Astra;
using IDType = EntityTable::IDType;

// A located record occupies exactly half a cache line and must never straddle one.
static_assert(sizeof(Astra::EntityRecord) == 32, "record must stay 2-per-cache-line");
static_assert(alignof(Astra::EntityRecord) == 32, "record must never straddle a cache line");

// EntityTable never dereferences Archetype*/ArchetypeChunk*, so opaque non-null dummies are fine.
static Archetype* Fake(uintptr_t v) { return reinterpret_cast<Archetype*>(v); }
static ArchetypeChunk* FakeChunk(uintptr_t v) { return reinterpret_cast<ArchetypeChunk*>(v); }

TEST(EntityRecordTable, VersionFaceUnchanged)
{
    EntityTable t;
    t.SetVersion(5, 1);
    EXPECT_TRUE(t.IsAlive(5, 1));
    EXPECT_FALSE(t.IsAlive(5, 2));
    EXPECT_EQ(t.GetVersion(5), 1u);
    t.Destroy(5);
    EXPECT_FALSE(t.IsAlive(5, 1));
    EXPECT_EQ(t.GetVersion(5), EntityTable::NULL_VERSION);
}

TEST(EntityRecordTable, LocationRoundTripLeavesVersionUntouched)
{
    EntityTable t;
    t.SetVersion(7, 1);                        // make the slot live
    t.SetRecord(7, Fake(0x1234), FakeChunk(0x99), EntityLocation::Create(2, 3));

    EntityRecord* r = t.GetRecord(7);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->archetype, Fake(0x1234));
    EXPECT_EQ(r->chunk, FakeChunk(0x99));
    EXPECT_EQ(r->location.chunkIndex, 2u);
    EXPECT_EQ(r->location.entityIndex, 3u);
    EXPECT_EQ(r->version, 1u);                 // SetRecord must NOT touch version
}

TEST(EntityRecordTable, GetRecordAbsentSegmentReturnsNull)
{
    EntityTable t;
    EXPECT_EQ(t.GetRecord(999999), nullptr);
}

TEST(EntityRecordTable, MultiSegmentPagingWithRecordPayload)
{
    EntityTable::Config cfg(1024);             // small segments → force many segments
    EntityTable t(cfg);
    for (IDType id = 0; id < 5000; id += 777)
    {
        t.SetVersion(id, 1);
        t.SetRecord(id, Fake(0x10 + id), FakeChunk(0x20 + id), EntityLocation::Create(id, id));
    }
    for (IDType id = 0; id < 5000; id += 777)
    {
        EntityRecord* r = t.GetRecord(id);
        ASSERT_NE(r, nullptr) << "id=" << id;
        EXPECT_EQ(r->location.chunkIndex, id);
        EXPECT_EQ(r->archetype, Fake(0x10 + id));
    }
}

TEST(EntityRecordTable, ForEachRecordVisitsOnlyLiveSlots)
{
    EntityTable t;
    t.SetVersion(1, 1); t.SetRecord(1, Fake(0xA), FakeChunk(0xA0), EntityLocation::Create(0, 0));
    t.SetVersion(2, 1); t.SetRecord(2, Fake(0xB), FakeChunk(0xB0), EntityLocation::Create(0, 1));
    t.SetVersion(3, 1);                        // live but never located (archetype == null)

    int located = 0, live = 0;
    t.ForEachRecord([&](IDType, const EntityRecord& rec) {
        ++live;
        if (rec.archetype) ++located;
    });
    EXPECT_EQ(live, 3);
    EXPECT_EQ(located, 2);
}
