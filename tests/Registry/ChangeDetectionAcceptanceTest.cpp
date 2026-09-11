// Arcane-shaped acceptance (spec 2026-09-10 §4.10): a "transform propagation" pass
// over N entities where 5% change per frame. The Changed<T> path must visit only the
// chunks that hold changed entities and produce exactly the brute-force result.
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <algorithm>
#include <random>
#include <unordered_set>
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;      // stands in for LocalTransform (untracked: coarse tier)
using Astra::Test::TrackedPos;    // stands in for WorldTransform (tracked: exact tier)

namespace
{
#ifdef ASTRA_BUILD_DEBUG
    constexpr size_t kN = 100'000;   // Debug: keep the suite fast
#else
    constexpr size_t kN = 1'000'000;
#endif
}

TEST(ChangeDetectionAcceptance, FivePercentPerFrameVisitsOnlyChangedChunksAndMatchesBruteForce)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(kN);
    ASSERT_EQ((reg.CreateEntitiesWith<Position, TrackedPos>(kN, std::span{ents},
        [](size_t i) { return std::tuple{Position{float(i), 0, 0}, TrackedPos{float(i), 0, 0}}; })), kN);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    const size_t chunkCount = arch->GetChunks().size();
    ASSERT_GT(chunkCount, 20u);

    // Fix round 1 (Controller Ruling L): computed ONCE, before the frame loop, over ALL of
    // arch->GetChunks() -- independent of whatever set of chunks a given frame's ForEach
    // actually visits. Chunks grow geometrically as the archetype populates (grow-as-populate
    // dynamic chunk sizing), so chunk[0] (allocated before the archetype ramped up) is the
    // SMALLEST chunk, not a representative one -- later chunks are ~100x bigger, capped at
    // 512KB. Using the archetype-wide maximum keeps the bound a real upper limit on what ANY
    // touched-chunk set could hold, so a filter that wrongly visited every chunk would still
    // be caught (unlike a bound built from the touched set itself, which is true by
    // construction and cannot discriminate a bug).
    size_t maxCapacity = 0;
    for (auto& chunk : arch->GetChunks()) maxCapacity = std::max(maxCapacity, chunk->GetCapacity());

    std::mt19937 rng(0xC0FFEE);
    auto propagate = reg.CreateView<const Position, TrackedPos, Astra::Changed<Position>>();

    for (int frame = 0; frame < 5; ++frame)
    {
        const Tick since = reg.CurrentTick();
        reg.AdvanceTick();

        // "Physics" writes 5% of LocalTransforms, CLUSTERED (as a real scene is: a moving
        // region of entities), via the explicit raw-pointer contract.
        std::unordered_set<uint32_t> changed;
        const size_t start = std::uniform_int_distribution<size_t>(0, kN - kN / 20)(rng);
        for (size_t i = start; i < start + kN / 20; ++i)
        {
            reg.GetComponent<Position>(ents[i])->x += 1.0f;   // non-const Get stamps the chunk
            changed.insert(ents[i].GetID());
        }

        // Brute force: every entity whose Position moved this frame.
        // Changed<Position> path: visit only stamped chunks, copy into WorldTransform.
        size_t visited = 0;
        std::vector<size_t> visitedChunks(chunkCount, 0);
        reg.AdvanceTick();
        propagate.Since(since).ForEach([&](Astra::Entity e, const Position& p, Astra::Mut<TrackedPos> w)
        {
            ++visited;
            visitedChunks[reg.GetArchetypeManager()->GetEntityRecord(e)->location.GetChunkIndex()] = 1;
            w.Write().x = p.x;
        });

        // Chunk granularity: every changed entity is visited, plus at most the rest of
        // the chunks it touched (~5% of chunks + 1 boundary chunk).
        size_t touchedChunks = 0;
        for (size_t v : visitedChunks) touchedChunks += v;
        EXPECT_LE(touchedChunks, chunkCount / 20 + 2);
        EXPECT_GE(visited, changed.size());
        // Independent of the touched set (see maxCapacity comment above): a filter bug that
        // visited every chunk would still exceed this bound, since maxCapacity is a per-chunk
        // ceiling, not a sum over whatever happened to be touched.
        EXPECT_LE(visited, (chunkCount / 20 + 2) * maxCapacity);

        // Result parity with brute force: every changed entity's WorldTransform equals its LocalTransform.
        for (size_t i = start; i < start + kN / 20; ++i)
        {
            const auto& creg = reg;
            EXPECT_FLOAT_EQ(creg.GetComponent<TrackedPos>(ents[i])->x, creg.GetComponent<Position>(ents[i])->x);
        }

        // And downstream (render) sees exactly the propagated set through the EXACT tier.
        size_t rendered = 0;
        reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>().Since(since).ForEach([&](const TrackedPos&) { ++rendered; });
        EXPECT_EQ(rendered, visited);   // Mut::Write marked exactly the visited entities
    }
}
