#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;

// ---- Task 1: Tick, IsNewer, Registry counter ------------------------------

TEST(ChangeDetectionTick, IsNewerIsSignedDifferenceAndTreatsZeroAsNever)
{
    static_assert(std::is_same_v<Tick, uint32_t>);
    static_assert(Astra::IsNewer(2u, 1u));
    static_assert(!Astra::IsNewer(1u, 1u));
    static_assert(!Astra::IsNewer(1u, 2u));
    static_assert(Astra::IsNewer(1u, 0u));            // anything stamped is newer than "never"
    static_assert(!Astra::IsNewer(0u, 0u));           // never vs never: not newer
    // Wraparound: a stamp that wrapped past 2^32 still orders after a big tick.
    constexpr Tick big = 0xFFFFFFF0u;
    static_assert(Astra::IsNewer(Tick(big + 16u), big));         // 0x00000000 after wrap
    static_assert(Astra::IsNewer(Tick(1u), Tick(0xFFFFFFFFu)));
    static_assert(!Astra::IsNewer(Tick(0x80000000u), Tick(0u)));  // exactly 2^31 ahead reads as older (documented bound)
    SUCCEED();
}

TEST(ChangeDetectionTick, RegistryCounterStartsAtOneAndIsMonotonic)
{
    Astra::Registry reg;
    EXPECT_EQ(reg.CurrentTick(), 1u);
    EXPECT_EQ(reg.AdvanceTick(), 2u);
    EXPECT_EQ(reg.CurrentTick(), 2u);
    Tick prev = reg.CurrentTick();
    for (int i = 0; i < 1000; ++i)
    {
        Tick t = reg.AdvanceTick();
        EXPECT_TRUE(Astra::IsNewer(t, prev));
        prev = t;
    }
    // The ArchetypeManager owns the counter; Registry forwards.
    EXPECT_EQ(reg.GetArchetypeManager()->CurrentTick(), reg.CurrentTick());
}

TEST(ChangeDetectionTick, EntityTicksIsTwoTicksTightlyPacked)
{
    static_assert(sizeof(Astra::EntityTicks) == 8);
    static_assert(alignof(Astra::EntityTicks) == 4);
    Astra::EntityTicks t{};
    EXPECT_EQ(t.added, 0u);
    EXPECT_EQ(t.changed, 0u);
}

#include <Astra/Commands/CommandBuffer.hpp>

using Astra::Test::Velocity;
using Astra::Test::Health;

namespace
{
    // Version of T's column in the chunk that currently holds `e`.
    template<typename T>
    Tick VersionOf(Astra::Registry& reg, Astra::Entity e)
    {
        const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) { ADD_FAILURE() << "entity not located"; return 0; }
        const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<T>::Value()];
        if (col < 0) { ADD_FAILURE() << "T has no storage column"; return 0; }
        return rec->chunk->GetColumnVersion(col);
    }

    void AdvanceTo(Astra::Registry& reg, Tick t)
    {
        while (reg.CurrentTick() < t) reg.AdvanceTick();
    }
}

// ---- Task 2: chunk column versions + structural-write stamping ------------

TEST(ChangeDetectionChunkVersion, EveryCreatePathStampsAllColumnsWithCurrentTick)
{
    Astra::Registry reg;
    AdvanceTo(reg, 5);

    auto a = reg.CreateEntity<Position, Velocity>();
    EXPECT_EQ(VersionOf<Position>(reg, a), 5u);
    EXPECT_EQ(VersionOf<Velocity>(reg, a), 5u);

    AdvanceTo(reg, 6);
    auto b = reg.CreateEntityWith(Position{1, 2, 3}, Velocity{});
    EXPECT_EQ(VersionOf<Position>(reg, b), 6u);
    EXPECT_EQ(VersionOf<Velocity>(reg, b), 6u);

    AdvanceTo(reg, 7);
    std::vector<Astra::Entity> batch(64);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(64, std::span{batch})), 64u);
    EXPECT_EQ(VersionOf<Position>(reg, batch.back()), 7u);

    AdvanceTo(reg, 8);
    std::vector<Astra::Entity> gen(64);
    ASSERT_EQ((reg.CreateEntitiesWith<Position, Velocity>(64, std::span{gen},
        [](size_t i) { return std::tuple{Position{float(i), 0, 0}, Velocity{}}; })), 64u);
    EXPECT_EQ(VersionOf<Velocity>(reg, gen.front()), 8u);
    EXPECT_EQ(VersionOf<Velocity>(reg, gen.back()), 8u);
}

TEST(ChangeDetectionChunkVersion, AddAndRemoveComponentStampTheDestinationChunk)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position>();
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.AddComponent<Velocity>(e, Velocity{1, 1, 1}));
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 3u);   // the new column
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);   // the carried column: destination chunk stamped

    AdvanceTo(reg, 4);
    ASSERT_TRUE(reg.EmplaceComponent<Health>(e, 10, 10));
    EXPECT_EQ(VersionOf<Health>(reg, e), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);

    AdvanceTo(reg, 5);
    ASSERT_TRUE(reg.RemoveComponent<Health>(e));
    EXPECT_EQ(VersionOf<Position>(reg, e), 5u);   // remove is a move too: destination stamped
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 5u);
}

TEST(ChangeDetectionChunkVersion, BatchAddAndDeferredAddStampTheDestinationChunk)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(32);
    ASSERT_EQ(reg.CreateEntities<Position>(32, std::span{ents}), 32u);

    AdvanceTo(reg, 3);
    reg.AddComponents<Velocity>(std::span{ents}, Velocity{});   // batch move path
    EXPECT_EQ(VersionOf<Velocity>(reg, ents[0]), 3u);
    EXPECT_EQ(VersionOf<Position>(reg, ents[31]), 3u);

    AdvanceTo(reg, 4);
    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(ents[0], Health{5, 5});                    // AddComponentByID at flush
    cmd.Execute();
    EXPECT_EQ(VersionOf<Health>(reg, ents[0]), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, ents[0]), 4u);
}

TEST(ChangeDetectionChunkVersion, CompactionCarriesTheNewerVersionNotZeroNotNow)
{
    Astra::Registry reg;
    AdvanceTo(reg, 9);
    std::vector<Astra::Entity> ents(3000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(3000, std::span{ents})), 3000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u) << "need >1 chunk for compaction to run";

    // Destroy 70% so the archetype crosses the 0.5 fragmentation threshold.
    AdvanceTo(reg, 10);
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < ents.size(); ++i) if (i % 10 < 7) doomed.push_back(ents[i]);
    reg.DestroyEntities(std::span{doomed});

    AdvanceTo(reg, 11);
    auto res = reg.Defragment();
    ASSERT_GT(res.entitiesMoved, 0u) << "compaction did not run; the test setup must fragment harder";

    for (size_t i = 0; i < ents.size(); ++i)
    {
        if (i % 10 < 7) continue;
        EXPECT_EQ(VersionOf<Position>(reg, ents[i]), 9u);   // folded from the source chunk, not 0, not 11
    }
}

TEST(ChangeDetectionChunkVersion, DeserializeStampsEveryRestoredChunkWithTheLoadersTick)
{
    std::vector<std::byte> buffer;
    {
        Astra::Registry reg;
        AdvanceTo(reg, 40);
        std::vector<Astra::Entity> ents(500);
        ASSERT_EQ((reg.CreateEntities<Position, Velocity>(500, std::span{ents})), 500u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        buffer = std::move(*saved.GetValue());
    }
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity>();
    auto loaded = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg = **loaded.GetValue();
    EXPECT_EQ(reg.CurrentTick(), 1u);   // a fresh registry: nothing tick-related is serialized

    size_t seen = 0;
    reg.CreateView<const Position>().ForEach([&](Astra::Entity e, const Position&)
    {
        ++seen;
        EXPECT_EQ(VersionOf<Position>(reg, e), 1u);   // everything reads as changed-since-never once
        EXPECT_EQ(VersionOf<Velocity>(reg, e), 1u);
    });
    EXPECT_EQ(seen, 500u);
}

TEST(ChangeDetectionChunkVersion, EnableableArchetypeStillFitsItsCarve)
{
    // Net for the layout math: the precise fit loop runs for enableable archetypes
    // and must now account for the version region too. In Debug, InitializeColumns'
    // `offset <= m_chunkSize` assert is the tripwire; in Release, the population
    // simply must survive chunk growth.
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(20000);
    ASSERT_EQ((reg.CreateEntities<Astra::Test::Timer, Position>(20000, std::span{ents})), 20000u);
    size_t n = 0;
    reg.CreateView<const Astra::Test::Timer, const Position>().ForEach([&](const Astra::Test::Timer&, const Position&) { ++n; });
    EXPECT_EQ(n, 20000u);
}

// ---- Task 3: view-entry stamping + Registry accessor write side ------------

TEST(ChangeDetectionStamp, NonConstViewStampsEveryVisitedChunkConstViewStampsNone)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(2000, std::span{ents})), 2000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u);

    AdvanceTo(reg, 3);
    reg.CreateView<const Position, const Velocity>().ForEach([](const Position&, const Velocity&) {});
    for (auto& chunk : arch->GetChunks())
    {
        const auto& cm = arch->GetColumnMeta();
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Position>::Value()]), 2u);   // untouched
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Velocity>::Value()]), 2u);
    }

    AdvanceTo(reg, 4);
    reg.CreateView<Position, const Velocity>().ForEach([](Position&, const Velocity&) { /* writes nothing */ });
    for (auto& chunk : arch->GetChunks())
    {
        const auto& cm = arch->GetColumnMeta();
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Position>::Value()]), 4u);   // stamped by access, not by writing
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Velocity>::Value()]), 2u);   // const: untouched
    }

    AdvanceTo(reg, 5);
    reg.CreateView<const Position, Velocity>().ParallelForEach([](const Position&, Velocity&) {});
    for (auto& chunk : arch->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(arch->GetColumnMeta().idToColumn[Astra::TypeID<Velocity>::Value()]), 5u);

    AdvanceTo(reg, 6);
    for (auto [e, p] : reg.CreateView<Position>()) { (void)e; (void)p; }   // range-for stamps too
    for (auto& chunk : arch->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(arch->GetColumnMeta().idToColumn[Astra::TypeID<Position>::Value()]), 6u);
}

TEST(ChangeDetectionStamp, OptionalAndEnabledFilteredPathsStampMutableColumns)
{
    using EnA = Astra::Test::Hierarchy;   // enableable. NOTE: With<EnA> is match-only (not a bare
                                          // required enableable arg), so the first view below takes
                                          // the ForEachWithOptional path; the second (<Position, EnA>)
                                          // is what exercises the enabled-filtered VisitChunkFiltered path.
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position, Velocity, EnA>();
    auto b = reg.CreateEntity<Position, EnA>();
    (void)b;

    AdvanceTo(reg, 3);
    reg.CreateView<const Position, Astra::Optional<Velocity>, Astra::With<EnA>>().ForEach(
        [](const Position&, Velocity*) {});
    EXPECT_EQ(VersionOf<Velocity>(reg, a), 3u);   // present optional, non-const: stamped
    EXPECT_EQ(VersionOf<Position>(reg, a), 2u);   // const required: not

    AdvanceTo(reg, 4);
    reg.CreateView<Position, EnA>().ForEach([](Position&, EnA&) {});   // enabled-filtered path
    EXPECT_EQ(VersionOf<Position>(reg, a), 4u);
    EXPECT_EQ(VersionOf<EnA>(reg, a), 4u);
}

TEST(ChangeDetectionStamp, GetAndSingleStampOnlyNonConstRequests)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position, Velocity>();

    AdvanceTo(reg, 3);
    auto vRead = reg.CreateView<const Position, const Velocity>();
    ASSERT_TRUE(vRead.Get(e).IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    auto vWrite = reg.CreateView<Position, const Velocity>();
    ASSERT_TRUE(vWrite.Get(e).IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 2u);

    AdvanceTo(reg, 4);
    ASSERT_TRUE(vWrite.Single().IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);
}

TEST(ChangeDetectionStamp, RegistryGetComponentNonConstStampsConstDoesNot)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position>();

    AdvanceTo(reg, 3);
    const Astra::Registry& creg = reg;
    ASSERT_NE(creg.GetComponent<Position>(e), nullptr);
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    ASSERT_NE(reg.GetComponent<Position>(e), nullptr);
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
}

TEST(ChangeDetectionStamp, ModifiedStampsAndSetIfNeqStampsOnlyOnInequality)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(Position{1, 2, 3});

    AdvanceTo(reg, 3);
    EXPECT_TRUE(reg.Modified<Position>(e));
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_FALSE(reg.Modified<Velocity>(e));               // absent component: false, nothing stamped
    EXPECT_FALSE(reg.Modified<Position>(Astra::Entity{}));   // invalid handle: false

    AdvanceTo(reg, 4);
    EXPECT_FALSE(reg.SetIfNeq<Health>(e, Health{1, 1}));     // absent: false
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{1, 2, 3}));   // equal: no store, no stamp
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_TRUE(reg.SetIfNeq<Position>(e, Position{9, 2, 3}));    // different: stored + stamped
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 9.0f);
}

// Ruling E (fix round 1): Single() stamps ONLY the returned entity's chunk and
// NOTHING on the Empty / MultipleMatched paths -- its counting pass is not a write.
TEST(ChangeDetectionStamp, SingleStampsOnlyTheReturnedChunkAndNothingOnFailurePaths)
{
    using EnA = Astra::Test::Timer;       // enableable
    using EnB = Astra::Test::Hierarchy;   // enableable
    Astra::Registry reg;
    AdvanceTo(reg, 2);

    // (1) MultipleMatched across >= 2 chunks: every chunk's Position version stays put.
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(2000, std::span{ents})), 2000u);
    auto* pv = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(pv->GetChunks().size(), 1u);
    const int pvPosCol = pv->GetColumnMeta().idToColumn[Astra::TypeID<Position>::Value()];

    AdvanceTo(reg, 3);
    auto multi = reg.CreateView<Position, const Velocity>().Single();
    ASSERT_TRUE(multi.IsErr());
    EXPECT_EQ(*multi.GetError(), Astra::QueryError::MultipleMatched);
    for (auto& chunk : pv->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(pvPosCol), 2u);   // no chunk marked by the counting pass

    // (2) Empty through a VISITED chunk: two required enableable columns disabled in
    // complementary patterns -> the chunk passes the whole-chunk reject (no column is
    // fully disabled) but yields nobody. Single() must still stamp nothing.
    auto c1 = reg.CreateEntity<Position, Velocity, EnA, EnB>();
    auto c2 = reg.CreateEntity<Position, Velocity, EnA, EnB>();
    ASSERT_TRUE(reg.SetEnabled<EnA>(c1, false));
    ASSERT_TRUE(reg.SetEnabled<EnB>(c2, false));
    ASSERT_EQ(VersionOf<Position>(reg, c1), 3u);   // created at tick 3

    AdvanceTo(reg, 4);
    auto empty = reg.CreateView<Position, EnA, EnB>().Single();
    ASSERT_TRUE(empty.IsErr());
    EXPECT_EQ(*empty.GetError(), Astra::QueryError::Empty);
    EXPECT_EQ(VersionOf<Position>(reg, c1), 3u);
    EXPECT_EQ(VersionOf<EnA>(reg, c1), 3u);
    EXPECT_EQ(VersionOf<EnB>(reg, c1), 3u);

    // (3) Exactly one match (archetype <Position, EnA, EnB>) while the view also
    // visits the non-yielding <Position, Velocity, EnA, EnB> chunk above and the
    // <Position, Velocity> archetype holds other non-empty chunks: only the returned
    // entity's chunk is stamped.
    auto one = reg.CreateEntity<Position, EnA, EnB>();
    ASSERT_EQ(VersionOf<Position>(reg, one), 4u);

    AdvanceTo(reg, 5);
    auto single = reg.CreateView<Position, EnA, EnB>().Single();
    ASSERT_TRUE(single.IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, one), 5u);   // the returned entity's chunk: stamped by Get
    EXPECT_EQ(VersionOf<EnA>(reg, one), 5u);
    EXPECT_EQ(VersionOf<Position>(reg, c1), 3u);    // visited-but-empty chunk: untouched
    EXPECT_EQ(VersionOf<EnA>(reg, c1), 3u);
    for (auto& chunk : pv->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(pvPosCol), 2u);   // unrelated archetype: untouched

    // Same with the unfiltered shape the ruling names: one <Position, Health> entity
    // next to the 2000 <Position, Velocity> ones.
    auto ph = reg.CreateEntity<Position, Health>();
    AdvanceTo(reg, 6);
    ASSERT_TRUE((reg.CreateView<Position, const Health>().Single().IsOk()));
    EXPECT_EQ(VersionOf<Position>(reg, ph), 6u);
    for (auto& chunk : pv->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(pvPosCol), 2u);
}
