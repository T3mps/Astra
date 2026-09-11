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
