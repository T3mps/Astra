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

// ---- Task 4: Changed<T>/Added<T> at chunk granularity ----------------------

namespace
{
    struct FakeCtx { Tick last; Tick LastRun() const noexcept { return last; } };
    static_assert(Astra::TickContext<FakeCtx>);

    template<typename V>
    size_t CountSince(V& view, Tick since)
    {
        size_t n = 0;
        view.Since(since).ForEach([&](auto&&...) { ++n; });
        return n;
    }
}

TEST(ChangeDetectionFilter, ChangedYieldsStampedChunksAndSkipsUnstamped)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(2000, std::span{ents})), 2000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u);

    auto changed = reg.CreateView<const Position, Astra::Changed<Position>>();

    // First run: since 0 sees everything (create stamped every chunk at tick 2).
    EXPECT_EQ(CountSince(changed, 0), 2000u);
    // Since the creation tick itself: nothing is newer than 2.
    EXPECT_EQ(CountSince(changed, 2), 0u);

    // Write exactly one entity at tick 3: its whole CHUNK reads as changed (chunk granularity).
    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.Modified<Position>(ents[0]));
    const size_t chunk0Count = arch->GetChunks()[0]->GetCount();
    EXPECT_EQ(CountSince(changed, 2), chunk0Count);
    EXPECT_EQ(CountSince(changed, 3), 0u);

    // A non-const view over Position at tick 4 stamps every chunk -> everything changed since 3.
    AdvanceTo(reg, 4);
    reg.CreateView<Position>().ForEach([](Position&) {});
    EXPECT_EQ(CountSince(changed, 3), 2000u);
    EXPECT_EQ(CountSince(changed, 4), 0u);
}

TEST(ChangeDetectionFilter, ForEachWithContextUsesLastRunAndParallelMatchesSerial)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(2000, std::span{ents})), 2000u);
    AdvanceTo(reg, 3);
    reg.CreateView<Velocity>().ForEach([](Velocity&) {});   // stamps Velocity everywhere at 3

    auto v = reg.CreateView<const Position, const Velocity, Astra::Changed<Velocity>>();
    size_t serial = 0, parallel = 0;
    v.ForEach(FakeCtx{2}, [&](const Position&, const Velocity&) { ++serial; });
    v.ParallelForEach(FakeCtx{2}, [&](const Position&, const Velocity&) { ++parallel; });
    EXPECT_EQ(serial, 2000u);
    EXPECT_EQ(parallel, 2000u);

    serial = 0;
    v.ForEach(FakeCtx{3}, [&](const Position&, const Velocity&) { ++serial; });
    EXPECT_EQ(serial, 0u);
}

TEST(ChangeDetectionFilter, AddedIsChunkGranularForUntrackedTypesAndCombinesWithNotWith)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position>();
    auto b = reg.CreateEntity<Position, Health>();
    (void)a;

    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.AddComponent<Velocity>(b, Velocity{}));   // b moves into {Position, Health, Velocity} at 3

    // Added<Velocity>: b's new chunk was stamped at 3.
    auto added = reg.CreateView<const Velocity, Astra::Added<Velocity>>();
    EXPECT_EQ(CountSince(added, 2), 1u);
    EXPECT_EQ(CountSince(added, 3), 0u);

    // Filters compose with Not/With and the enabled filter's chunk skip.
    auto composed = reg.CreateView<const Position, Astra::Changed<Position>, Astra::With<Velocity>, Astra::Not<Astra::Test::Name>>();
    EXPECT_EQ(CountSince(composed, 2), 1u);   // only b (a has no Velocity)
}

TEST(ChangeDetectionFilter, ChangedTermCountsTowardArchetypeMatchingAndAccess)
{
    // Changed<T> requires T for MATCHING (like With) but adds nothing to ViewAccess.
    using V = Astra::View<const Position, Astra::Changed<Position>, Astra::Added<Position>>;
    static_assert(V::HasChangeFilter);
    static_assert(std::tuple_size_v<Astra::ViewAccess<V>::Writes> == 0);
    static_assert(std::tuple_size_v<Astra::ViewAccess<V>::Reads> == 1);   // from `const Position` only
    using Plain = Astra::View<const Position>;
    static_assert(!Plain::HasChangeFilter);
    SUCCEED();
}

// ---- Task 6: opt-in trait + per-entity tick columns ----------------------

using Astra::Test::TrackedPos;
using Astra::Test::TrackedVel;

namespace
{
    template<typename T>
    Astra::EntityTicks TicksOf(Astra::Registry& reg, Astra::Entity e)
    {
        const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) { ADD_FAILURE() << "entity not located"; return {}; }
        const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<T>::Value()];
        auto* ticks = rec->chunk->GetTicks(col);
        if (!ticks) { ADD_FAILURE() << "T is not tracked"; return {}; }
        return ticks[rec->location.GetEntityIndex()];
    }

    namespace TrackedTraitDetail
    {
        struct Spelled { static constexpr bool AstraChangeTracked = true; int v; };
        struct Plain   { int v; };
        struct Spec    { int v; };
    }
}
template<> struct Astra::ChangeTrackedTraits<TrackedTraitDetail::Spec> { static constexpr bool value = true; };

TEST(ChangeDetectionTracked, TraitDetectsBothSpellingsAndDescriptorSnapshotsIt)
{
    static_assert(Astra::IsChangeTrackedV<TrackedTraitDetail::Spelled>);
    static_assert(Astra::IsChangeTrackedV<TrackedTraitDetail::Spec>);
    static_assert(!Astra::IsChangeTrackedV<TrackedTraitDetail::Plain>);
    static_assert(!Astra::IsChangeTrackedV<int>);
    static_assert(Astra::IsChangeTrackedV<const TrackedPos>);
    static_assert(!Astra::IsChangeTrackedV<Position>);

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<TrackedPos, Position>();
    EXPECT_TRUE(reg.GetComponentRegistry()->GetComponentDescriptor(Astra::TypeID<TrackedPos>::Value())->isChangeTracked);
    EXPECT_FALSE(reg.GetComponentRegistry()->GetComponentDescriptor(Astra::TypeID<Position>::Value())->isChangeTracked);
}

TEST(ChangeDetectionTracked, UntrackedColumnsCarveNoTicksTrackedOnesDo)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, TrackedPos>();
    const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    const auto& cm = rec->archetype->GetColumnMeta();
    EXPECT_EQ(cm.trackedColumnCount, 1u);
    EXPECT_FALSE(rec->chunk->IsTracked(cm.idToColumn[Astra::TypeID<Position>::Value()]));
    EXPECT_EQ(rec->chunk->GetTicks(cm.idToColumn[Astra::TypeID<Position>::Value()]), nullptr);
    EXPECT_TRUE(rec->chunk->IsTracked(cm.idToColumn[Astra::TypeID<TrackedPos>::Value()]));
    EXPECT_EQ(rec->chunk->GetTicksOffset(static_cast<uint16_t>(cm.idToColumn[Astra::TypeID<TrackedPos>::Value()])) % 8, 0u);

    // A plain archetype has zero tracked columns: the zero-cost early-out.
    auto p = reg.CreateEntity<Position, Velocity>();
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(p)->archetype->GetColumnMeta().trackedColumnCount, 0u);
}

TEST(ChangeDetectionTracked, CreateAndAddInitialiseAddedAndChangedToNow)
{
    Astra::Registry reg;
    AdvanceTo(reg, 5);
    auto a = reg.CreateEntity<TrackedPos>();
    EXPECT_EQ(TicksOf<TrackedPos>(reg, a).added, 5u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, a).changed, 5u);

    auto b = reg.CreateEntityWith(TrackedPos{1, 2, 3});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, b).added, 5u);

    std::vector<Astra::Entity> batch(300);
    ASSERT_EQ((reg.CreateEntitiesWith<TrackedPos, TrackedVel>(300, std::span{batch},
        [](size_t) { return std::tuple{TrackedPos{}, TrackedVel{}}; })), 300u);
    EXPECT_EQ(TicksOf<TrackedVel>(reg, batch.back()).added, 5u);

    AdvanceTo(reg, 6);
    auto c = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.AddComponent<TrackedPos>(c, TrackedPos{}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).added, 6u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).changed, 6u);

    AdvanceTo(reg, 7);
    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(c, TrackedVel{});
    cmd.Execute();
    EXPECT_EQ(TicksOf<TrackedVel>(reg, c).added, 7u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).added, 6u);   // carried component keeps its ticks
}

TEST(ChangeDetectionTracked, TicksTravelAcrossArchetypeMovesSwapRemoveAndCompaction)
{
    Astra::Registry reg;
    AdvanceTo(reg, 3);
    std::vector<Astra::Entity> ents(1000);
    ASSERT_EQ((reg.CreateEntities<TrackedPos, Position>(1000, std::span{ents})), 1000u);

    // Give ents[500] distinct ticks by adding it later... instead: re-create it later.
    AdvanceTo(reg, 4);
    auto late = reg.CreateEntity<TrackedPos, Position>();
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Cross-archetype add: ticks copied (added stays 4, not 5).
    AdvanceTo(reg, 5);
    ASSERT_TRUE(reg.AddComponent<Velocity>(late, Velocity{}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).changed, 4u);

    // Cross-archetype remove: same.
    AdvanceTo(reg, 6);
    ASSERT_TRUE(reg.RemoveComponent<Velocity>(late));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Batch add (BatchMoveComponentsFrom): ticks copied for the shared tracked column.
    AdvanceTo(reg, 7);
    std::vector<Astra::Entity> some(ents.begin(), ents.begin() + 100);
    reg.AddComponents<Velocity>(std::span{some}, Velocity{});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, some[0]).added, 3u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, some[99]).added, 3u);

    // Swap-remove within a chunk: destroy the entity in front of `late` in its chunk;
    // `late` (or whoever fills the hole) must keep its own ticks.
    AdvanceTo(reg, 8);
    const auto* recLate = reg.GetArchetypeManager()->GetEntityRecord(late);
    const auto* chunk = recLate->chunk;
    const size_t lateIdx = recLate->location.GetEntityIndex();
    ASSERT_GT(lateIdx, 0u);
    Astra::Entity victim = chunk->GetEntity(0);
    ASSERT_NE(victim, late);
    // Make `late` the tail so it is the one swapped down: destroy everything after it.
    std::vector<Astra::Entity> tail;
    for (size_t i = lateIdx + 1; i < chunk->GetCount(); ++i) tail.push_back(chunk->GetEntity(i));
    reg.DestroyEntities(std::span{tail});
    reg.DestroyEntity(victim);                          // late swaps into slot 0
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(late)->location.GetEntityIndex(), 0u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Compaction: fragment the {TrackedPos, Position} archetype hard, defragment, ticks survive.
    AdvanceTo(reg, 9);
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < ents.size(); ++i) if (i % 10 < 8) doomed.push_back(ents[i]);
    reg.DestroyEntities(std::span{doomed});
    auto res = reg.Defragment();
    ASSERT_GT(res.entitiesMoved, 0u);
    for (size_t i = 0; i < ents.size(); ++i)
        if (i % 10 >= 8 && i >= 100 && reg.IsValid(ents[i]))
            EXPECT_EQ(TicksOf<TrackedPos>(reg, ents[i]).added, 3u);
}

TEST(ChangeDetectionTracked, DeserializeSetsEveryTrackedEntityToTheLoadersTick)
{
    std::vector<std::byte> buffer;
    {
        Astra::Registry reg;
        AdvanceTo(reg, 30);
        std::vector<Astra::Entity> ents(400);
        ASSERT_EQ((reg.CreateEntities<TrackedPos, Position>(400, std::span{ents})), 400u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        buffer = std::move(*saved.GetValue());
    }
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<TrackedPos, Position>();
    auto loaded = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg = **loaded.GetValue();
    size_t n = 0;
    reg.CreateView<const TrackedPos>().ForEach([&](Astra::Entity e, const TrackedPos&)
    {
        ++n;
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).added, 1u);
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 1u);
    });
    EXPECT_EQ(n, 400u);
}

// ---- Task 7: Mut<T> + marking accessors -----------------------------------

TEST(ChangeDetectionMut, ConversionAndWriteMarkReadDoesNotSetIfNeqMarksOnlyOnChange)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(TrackedPos{1, 2, 3});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);

    AdvanceTo(reg, 3);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { (void)p.Read(); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // Read() never marks
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 3u);         // ...but the chunk was still stamped (coarse tier)

    AdvanceTo(reg, 4);
    reg.CreateView<TrackedPos>().ForEach([](TrackedPos& p) { p.x += 1.0f; });   // legacy lambda: implicit conversion marks
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);

    AdvanceTo(reg, 5);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { p->y = 9.0f; });   // operator-> marks
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);

    AdvanceTo(reg, 6);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { EXPECT_FALSE(p.SetIfNeq(TrackedPos{2, 9, 3})); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);   // equal: no mark
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { EXPECT_TRUE(p.SetIfNeq(TrackedPos{7, 9, 3})); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 6u);

    AdvanceTo(reg, 7);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p)
    {
        EXPECT_TRUE(p.IsChanged(5));  EXPECT_FALSE(p.IsChanged(6));
        EXPECT_TRUE(p.IsAdded(1));    EXPECT_FALSE(p.IsAdded(2));
        p.Write().z = 0.0f;
    });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 7u);

    // const yield stays a plain const T& (no Mut, no mark, no stamp).
    AdvanceTo(reg, 8);
    reg.CreateView<const TrackedPos>().ForEach([](const TrackedPos&) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 7u);
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 7u);
}

TEST(ChangeDetectionMut, GenericAndEntityLeadingLambdasStillCompileAndMark)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<TrackedPos, Position>();
    AdvanceTo(reg, 3);
    size_t n = 0;
    reg.CreateView<TrackedPos, const Position>().ForEach([&](Astra::Entity, auto& p, const Position&) { ++n; p.Write().x = 1.0f; });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);
    AdvanceTo(reg, 4);
    reg.CreateView<TrackedPos, const Position>().ParallelForEach([](TrackedPos& p, const Position&) { p.x = 2.0f; });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);
}

TEST(ChangeDetectionMut, GetTupleYieldsMutForTrackedAndMarksOnUse)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<TrackedPos, Position>();
    auto v = reg.CreateView<TrackedPos, const Position>();
    static_assert(std::is_same_v<std::tuple_element_t<0, decltype(v)::AccessTuple>, Astra::Mut<TrackedPos>>);
    static_assert(std::is_same_v<std::tuple_element_t<1, decltype(v)::AccessTuple>, const Position*>);

    AdvanceTo(reg, 3);
    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto& [mp, pp] = *r.GetValue();
    (void)pp;
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // handing out Mut does not mark
    mp.Write().x = 5.0f;
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 3u);         // Get stamped the chunk on the non-const request
}

TEST(ChangeDetectionMut, OptionalNonConstTrackedMarksWhenPresentConstDoesNot)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto with    = reg.CreateEntity<Position, TrackedPos>();
    auto without = reg.CreateEntity<Position>();
    (void)without;
    AdvanceTo(reg, 3);
    reg.CreateView<const Position, Astra::Optional<TrackedPos>>().ForEach([](const Position&, TrackedPos*) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, with).changed, 3u);   // deviation 5: present + non-const => marked
    AdvanceTo(reg, 4);
    reg.CreateView<const Position, Astra::Optional<const TrackedPos>>().ForEach([](const Position&, const TrackedPos*) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, with).changed, 3u);
}

TEST(ChangeDetectionMut, RegistryAccessorsMarkTrackedEntities)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(TrackedPos{1, 1, 1});

    AdvanceTo(reg, 3);
    ASSERT_NE(std::as_const(reg).GetComponent<TrackedPos>(e), nullptr);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // const: no mark
    ASSERT_NE(reg.GetComponent<TrackedPos>(e), nullptr);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);   // non-const: marked

    AdvanceTo(reg, 4);
    EXPECT_TRUE(reg.Modified<TrackedPos>(e));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);

    AdvanceTo(reg, 5);
    EXPECT_FALSE(reg.SetIfNeq<TrackedPos>(e, TrackedPos{1, 1, 1}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);
    EXPECT_TRUE(reg.SetIfNeq<TrackedPos>(e, TrackedPos{2, 1, 1}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);
}

// Ruling I: range-for over a NON-const tracked yield is compile-time refused (the
// iterator yields T& and cannot mark); the const walk still iterates and touches
// neither the entity ticks nor the chunk version.
TEST(ChangeDetectionMut, ConstRangeForOverTrackedTypeStillIteratesAndMarksNothing)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(5);
    ASSERT_EQ((reg.CreateEntities<TrackedPos>(5, std::span{ents})), 5u);
    for (auto e : ents) EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);

    AdvanceTo(reg, 3);
    size_t n = 0;
    for (auto [e, p] : reg.CreateView<const TrackedPos>()) { (void)e; (void)p; ++n; }
    EXPECT_EQ(n, 5u);
    for (auto e : ents)
    {
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // const: no mark
        EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 2u);         // const: no stamp
    }
}

// Fix round 1 (Ruling E extended): Single()'s Stamp=false count/locate pass is not
// a write at EITHER tier -- no coarse stamp AND no per-entity mark, even for a
// present non-const tracked optional (deviation 5 marks only on a real walk).
TEST(ChangeDetectionMut, SingleCountPassNeverMarksTrackedOptionals)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position, TrackedPos>();
    auto b = reg.CreateEntity<Position, TrackedPos>();

    AdvanceTo(reg, 3);
    auto r = reg.CreateView<const Position, Astra::Optional<TrackedPos>>().Single();   // unfiltered ForEachWithOptional<false>
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(*r.GetError(), Astra::QueryError::MultipleMatched);
    for (auto e : {a, b})
    {
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // counting is not a write: no mark
        EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 2u);         // ...and no stamp
    }
}

TEST(ChangeDetectionMut, SingleCountPassNeverMarksTrackedOptionalsOnEnabledFilteredPath)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position, TrackedVel>();
    auto b = reg.CreateEntity<Position, TrackedVel>();
    auto c = reg.CreateEntity<Position, TrackedVel>();

    // Optional<TrackedVel> (enableable) makes the view enabled-filtered: Single()'s
    // count pass runs VisitChunkFiltered<false>. All enabled => Tier 1 body.
    AdvanceTo(reg, 3);
    auto view = reg.CreateView<const Position, Astra::Optional<TrackedVel>>();
    auto r1 = view.Single();
    ASSERT_TRUE(r1.IsErr());
    EXPECT_EQ(*r1.GetError(), Astra::QueryError::MultipleMatched);
    for (auto e : {a, b, c})
    {
        EXPECT_EQ(TicksOf<TrackedVel>(reg, e).changed, 2u);
        EXPECT_EQ(VersionOf<TrackedVel>(reg, e), 2u);
    }

    // One disabled => mixed chunk => Tier 3 (InvokeEntityCallbackFiltered). SetEnabled stamps nothing.
    ASSERT_TRUE(reg.SetEnabled<TrackedVel>(b, false));
    auto r2 = view.Single();
    ASSERT_TRUE(r2.IsErr());
    EXPECT_EQ(*r2.GetError(), Astra::QueryError::MultipleMatched);
    for (auto e : {a, b, c})
    {
        EXPECT_EQ(TicksOf<TrackedVel>(reg, e).changed, 2u);
        EXPECT_EQ(VersionOf<TrackedVel>(reg, e), 2u);
    }
}

// ---- Task 8: per-entity tier (tracked Changed/Added, union with enabled runs) ----

TEST(ChangeDetectionTrackedFilter, ChangedIsPerEntityInsideAStampedChunk)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ((reg.CreateEntities<TrackedPos, Position>(500, std::span{ents})), 500u);

    // Mark exactly 7 scattered entities at tick 3 (the chunk gets stamped by GetComponent).
    AdvanceTo(reg, 3);
    std::vector<size_t> marked = {0, 3, 64, 65, 127, 300, 499};
    for (size_t i : marked) reg.GetComponent<TrackedPos>(ents[i])->x = 1.0f;

    auto v = reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>();
    std::vector<Astra::Entity> seen;
    v.Since(2).ForEach([&](Astra::Entity e, const TrackedPos&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), marked.size());               // per-entity precision, not the whole chunk
    for (size_t k = 0; k < marked.size(); ++k) EXPECT_EQ(seen[k], ents[marked[k]]);   // ascending index order

    EXPECT_EQ(CountSince(v, 3), 0u);
    EXPECT_EQ(CountSince(v, 0), 500u);                   // first run still sees everything
}

TEST(ChangeDetectionTrackedFilter, AddedIsPerEntityAndIgnoresLaterChanges)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> old(200);
    ASSERT_EQ((reg.CreateEntities<TrackedPos, Position>(200, std::span{old})), 200u);
    AdvanceTo(reg, 3);
    auto fresh = reg.CreateEntity<TrackedPos, Position>();    // lands in the same archetype/chunk, added == 3
    for (auto e : old) reg.GetComponent<TrackedPos>(e)->x = 2.0f;   // changed == 3 for the old ones too

    auto added = reg.CreateView<const TrackedPos, Astra::Added<TrackedPos>>();
    std::vector<Astra::Entity> seen;
    added.Since(2).ForEach([&](Astra::Entity e, const TrackedPos&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], fresh);

    auto changed = reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>();
    EXPECT_EQ(CountSince(changed, 2), 201u);
}

TEST(ChangeDetectionTrackedFilter, UnionWithEnabledRunsSkipsDisabledAndUnchanged)
{
    // TrackedVel is BOTH change-tracked and enableable: the run scan must be the
    // intersection of enabled AND changed, visited in ascending order.
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(300);
    ASSERT_EQ((reg.CreateEntities<TrackedVel, Position>(300, std::span{ents})), 300u);
    AdvanceTo(reg, 3);
    for (size_t i = 0; i < 300; i += 2) reg.GetComponent<TrackedVel>(ents[i])->dx = 1.0f;   // even: changed
    for (size_t i = 0; i < 300; i += 3) ASSERT_TRUE(reg.SetEnabled<TrackedVel>(ents[i], false));   // multiples of 3: disabled

    auto v = reg.CreateView<const TrackedVel, Astra::Changed<TrackedVel>>();
    std::vector<Astra::Entity> seen;
    v.Since(2).ForEach([&](Astra::Entity e, const TrackedVel&) { seen.push_back(e); });
    size_t expected = 0;
    for (size_t i = 0; i < 300; ++i) if (i % 2 == 0 && i % 3 != 0) ++expected;
    EXPECT_EQ(seen.size(), expected);
    for (size_t k = 1; k < seen.size(); ++k) EXPECT_LT(seen[k - 1].GetID(), seen[k].GetID());   // ascending

    // IncludeDisabled lifts the enabled filter: every even entity, disabled or not.
    auto all = reg.CreateView<Astra::IncludeDisabled<TrackedVel>, Astra::Changed<TrackedVel>>();
    EXPECT_EQ(CountSince(all, 2), 150u);
}

TEST(ChangeDetectionTrackedFilter, MixedTrackedAndUntrackedTermsAndParallelParity)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ((reg.CreateEntities<TrackedPos, Position>(2000, std::span{ents})), 2000u);
    AdvanceTo(reg, 3);
    for (size_t i = 0; i < 2000; i += 10) reg.GetComponent<TrackedPos>(ents[i])->x = 1.0f;   // 200 tracked marks

    // Untracked Position term: chunk-granular (every chunk was stamped at creation, tick 2 -> not newer than 2).
    auto both = reg.CreateView<const TrackedPos, const Position, Astra::Changed<TrackedPos>, Astra::Changed<Position>>();
    EXPECT_EQ(CountSince(both, 2), 0u);   // Position's chunk versions are 2 -> reject
    AdvanceTo(reg, 4);
    reg.CreateView<Position>().ForEach([](Position&) {});   // stamp Position everywhere at 4
    EXPECT_EQ(CountSince(both, 3), 0u);   // TrackedPos marks were at 3, not newer than 3
    EXPECT_EQ(CountSince(both, 2), 200u); // chunk passes on both; per-entity on TrackedPos

    std::atomic<size_t> par{0};
    both.Since(2).ParallelForEach([&](const TrackedPos&, const Position&) { par.fetch_add(1); });
    EXPECT_EQ(par.load(), 200u);
}

// ---- Task 9: zero-cost when unused -----------------------------------------

TEST(ChangeDetectionZeroCost, PlainViewsInstantiateNoFilterOrTrackedYieldPath)
{
    using Plain   = Astra::View<Position, const Velocity>;
    using Filtered = Astra::View<const Position, Astra::Changed<Position>>;
    static_assert(!Plain::HasChangeFilter);
    static_assert(Filtered::HasChangeFilter);
    // An all-const view stamps nothing: IsMutableYield is false for every arg.
    static_assert(!Astra::Detail::IsMutableYield<const Position>);
    static_assert(Astra::Detail::IsMutableYield<Position>);
    static_assert(!Astra::Detail::IsMutableYield<Astra::Test::Player>);   // tag: no column, never stamped
    // Untracked types are yielded as plain references, never Mut.
    static_assert(std::is_same_v<std::tuple_element_t<0, Plain::AccessTuple>, Position*>);
    SUCCEED();
}

TEST(ChangeDetectionZeroCost, UntrackedArchetypeCarvesNoTickColumnsAndVersionRegionIsSmall)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity>();
    const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    const auto& cm = rec->archetype->GetColumnMeta();
    EXPECT_EQ(cm.trackedColumnCount, 0u);
    for (uint16_t c = 0; c < cm.columnCount; ++c)
    {
        EXPECT_FALSE(rec->chunk->IsTracked(c));
        EXPECT_EQ(rec->chunk->GetTicksOffset(c), std::numeric_limits<size_t>::max());
    }
    // Version region: exactly columnCount Ticks, 8-byte aligned, inside the arena.
    EXPECT_EQ(rec->chunk->GetColumnVersionOffset() % 8, 0u);
    EXPECT_LE(rec->chunk->GetColumnVersionOffset() + cm.columnCount * sizeof(Tick), rec->chunk->GetChunkBytes());
}

// ---- Final review wave: Ruling M (Relations traversals are write paths) ---

TEST(ChangeDetectionStamp, RelationTraversalStampsNonConstYieldsOnly)
{
    // Root lives in a DIFFERENT archetype from its descendants so its chunk's
    // versions can be told apart from theirs (VersionOf is per chunk).
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto root = reg.CreateEntity<Position, Velocity>();
    std::vector<Astra::Entity> kids(4);
    ASSERT_EQ((reg.CreateEntities<Position, TrackedPos>(4, std::span{kids})), 4u);
    for (auto k : kids) reg.SetParent(k, root);
    auto grand = reg.CreateEntity<Position, TrackedPos>();
    reg.SetParent(grand, kids[0]);
    for (auto e : kids) ASSERT_EQ(VersionOf<Position>(reg, e), 2u);

    // const yield: a read -- nothing stamped, nothing marked.
    AdvanceTo(reg, 3);
    size_t seen = 0;
    reg.GetRelations<const Position>(root).ForEachDescendant([&](Astra::Entity, size_t, const Position&) { ++seen; });
    EXPECT_EQ(seen, 5u);
    for (auto e : kids) EXPECT_EQ(VersionOf<Position>(reg, e), 2u);
    EXPECT_EQ(VersionOf<Position>(reg, grand), 2u);
    EXPECT_EQ(VersionOf<Position>(reg, root), 2u);

    // non-const UNTRACKED yield: every visited descendant's Position column is
    // stamped (coarse tier); the root is not a descendant and its chunk is untouched;
    // the un-yielded TrackedPos column is untouched.
    AdvanceTo(reg, 4);
    reg.GetRelations<Position>(root).ForEachDescendant([](Astra::Entity, size_t, Position& p) { p.x = 1.0f; });
    for (auto e : kids) EXPECT_EQ(VersionOf<Position>(reg, e), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, grand), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, root), 2u);
    EXPECT_EQ(VersionOf<TrackedPos>(reg, kids[0]), 2u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, kids[0]).changed, 2u);

    // non-const TRACKED yield: stamps the column AND marks each visited entity (exact tier).
    AdvanceTo(reg, 5);
    reg.GetRelations<TrackedPos>(root).ForEachDescendant([](Astra::Entity, size_t, TrackedPos& p) { p.x = 1.0f; });
    for (auto e : kids)
    {
        EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 5u);
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).added, 2u);   // a mark is not an add
    }
    EXPECT_EQ(TicksOf<TrackedPos>(reg, grand).changed, 5u);

    // const TRACKED yield: nothing moves.
    AdvanceTo(reg, 6);
    reg.GetRelations<const TrackedPos>(root).ForEachDescendant([](Astra::Entity, size_t, const TrackedPos&) {});
    for (auto e : kids)
    {
        EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 5u);
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);
    }

    // The other traversal shapes go through the same yield: ForEachChild (no depth),
    // ForEachAncestor (walks up into the root's own chunk), ForEachLink.
    AdvanceTo(reg, 7);
    reg.GetRelations<Position>(root).ForEachChild([](Astra::Entity, Position& p) { p.y = 1.0f; });
    for (auto e : kids) EXPECT_EQ(VersionOf<Position>(reg, e), 7u);
    EXPECT_EQ(VersionOf<Position>(reg, root), 2u);

    AdvanceTo(reg, 8);
    reg.GetRelations<Position>(grand).ForEachAncestor([](Astra::Entity, size_t, Position& p) { p.z = 1.0f; });
    EXPECT_EQ(VersionOf<Position>(reg, root), 8u);      // root IS an ancestor: its chunk is stamped now
    EXPECT_EQ(VersionOf<Position>(reg, kids[0]), 8u);

    AdvanceTo(reg, 9);
    reg.AddLink(root, grand);
    reg.GetRelations<const Position>(root).ForEachLink([](Astra::Entity, const Position&) {});
    EXPECT_EQ(VersionOf<Position>(reg, grand), 8u);     // const link yield: untouched
    reg.GetRelations<TrackedPos>(root).ForEachLink([](Astra::Entity, TrackedPos& p) { p.y = 2.0f; });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, grand).changed, 9u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, kids[1]).changed, 5u);   // not linked: not visited, not marked

    // End to end: a Changed<TrackedPos> reader sees exactly what the traversals marked.
    auto v = reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>();
    EXPECT_EQ(CountSince(v, 8), 1u);   // grand, via the link walk at 9
    EXPECT_EQ(CountSince(v, 4), 5u);   // the four kids + grand, via the descendant walk at 5
}
