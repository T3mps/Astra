#include <gtest/gtest.h>
#include <vector>
#include <Astra/Astra.hpp>

// Large-payload variant used by LoadHonorsChunkPoolConfig and
// DefaultConfigStoresComponentAboveMinChunk below: Phase 2 Task 5 (grow-as-
// populate chunk sizing) moved the per-chunk BYTE-SIZE bound off
// ArchetypeChunkPool::Config::chunkSize and onto ::maxChunkBytes (which
// defaults identically for "cfg" and the plain default Config below), so a
// 12-byte component would need ~44K entities packed into one chunk to make a
// save actually exceed the default 512KB cap. A big-payload component keeps
// the required entity count (and thus this test's runtime) small while still
// exercising the same "oversized-chunk save can't load into a smaller-cap
// pool" guard. Sized to 6000 bytes (strictly inside the (4KB, 16KB] band) so
// it also covers the Task 5 "fit-one-entity floor" regression: NextChunkBytes'
// first chunk used to always be minChunkBytes (4KB), so any component bigger
// than that but still well under maxChunkBytes made Initialize's refusal path
// trip silently.
namespace { struct alignas(4) ConfigBigPos { float x, y, z; char pad[5988]; }; }
static_assert(sizeof(ConfigBigPos) == 6000, "guard math below assumes exactly 6000 bytes/entity");

TEST(ConfigPreservation, ClearKeepsChunkPoolConfig)
{
    Astra::Registry::Config cfg;
    cfg.chunkPoolConfig.chunkSize = 65536;
    Astra::Registry reg(cfg);
    ASSERT_EQ(reg.GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);

    reg.Clear();
    EXPECT_EQ(reg.GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);  // was 16384
}

TEST(ConfigPreservation, LoadHonorsChunkPoolConfig)
{
    Astra::Registry::Config cfg;
    cfg.chunkPoolConfig.chunkSize = 65536;
    // The knob Archetype::Deserialize's guard actually checks post-Task-5 is
    // maxChunkBytes (grow-as-populate's ceiling), not chunkSize -- raise it
    // above the default Config's 512KB so a save made under `cfg` can ramp
    // chunks bigger than what a plain default Config's pool will accept.
    cfg.chunkPoolConfig.maxChunkBytes = Astra::ArchetypeChunkPool::MAX_CHUNK_SIZE;  // 1MB
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<ConfigBigPos>();

    // Enough entities to ramp past the default 512KB cap (524288 / 6000B
    // floors to 87 entities/chunk) and into steady-state chunks clamped at
    // cfg's 1MB cap (1048576 / 6000B floors to 174 entities/chunk) --
    // comfortably clears 87. The ramp (first-chunk floor now the "fit-one-
    // entity" 6000B, doubling roughly every append thereafter) reaches the
    // 174-capacity steady state well before entity ~475 and stays there, so
    // 2000 entities guarantee at least one fully-packed 174-capacity chunk.
    constexpr int kCount = 2000;
    for (int i = 0; i < kCount; ++i)
        reg.CreateEntityWith(ConfigBigPos{float(i), 0, 0});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<ConfigBigPos>();

    // Same config: must load and preserve pool size.
    auto ok = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, cfg);
    ASSERT_TRUE(ok.IsOk());
    EXPECT_EQ((*ok.GetValue())->GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);
    EXPECT_EQ((*ok.GetValue())->Size(), static_cast<size_t>(kCount));

    // Default (512KB maxChunkBytes) config: saved chunks ramped up to 1MB
    // under `cfg` cannot fit -- must error, not overflow.
    auto bad = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    EXPECT_TRUE(bad.IsErr());
}

// Regression coverage for the Task 5 "fit-one-entity floor" fix. Before the
// fix, NextChunkBytes()'s first chunk was always clamped to minChunkBytes
// (4KB) regardless of the component's actual footprint, so any component in
// (4KB, 16KB] made Initialize's refusal path trip: the entity id was still
// allocated, but the component was silently never attached (no crash, no
// error -- just a missing component). ConfigBigPos (6000 bytes) sits inside
// that band. With the fix, NextChunkBytes floors its ramp target at one
// entity's footprint, so the first chunk is sized to fit exactly one
// ConfigBigPos and this must round-trip under a plain DEFAULT registry.
TEST(ConfigPreservation, DefaultConfigStoresComponentAboveMinChunk)
{
    Astra::Registry reg;

    Astra::Entity e = reg.CreateEntityWith(ConfigBigPos{111.0f, 222.0f, 333.0f});
    ASSERT_TRUE(e.IsValid());

    auto* stored = reg.GetComponent<ConfigBigPos>(e);
    ASSERT_NE(stored, nullptr);
    EXPECT_FLOAT_EQ(stored->x, 111.0f);
    EXPECT_FLOAT_EQ(stored->y, 222.0f);
    EXPECT_FLOAT_EQ(stored->z, 333.0f);
}

// Regression coverage for CompactChunks' fit-one-entity floor (final-review
// minor fix). ConfigBigPos (6000 bytes, alignmentOverhead == 0 since it is the
// archetype's only component) ramps its chunk capacities SLOWLY under the
// default pool (divisor 2, 4KB min, 512KB max): capacity is 1 for the first
// several chunks (raw = totalCapacity*6000/2 stays below 6000 until
// totalCapacity > 4), so 64 entities added in sequence naturally spread across
// ~12 small, mostly-single-occupant chunks before the last one starts holding
// more than one entity.
//
// Destroying every entity but the first (in ascending order) empties every
// chunk except chunk 0 (the survivor's) and the true trailing chunk (whose
// single occupant is destroyed last, auto-popping it via RemoveEntity's
// trailing-empty-chunk check) -- but every OTHER now-empty chunk is a middle
// chunk, so it is retained rather than dropped. That leaves a fragmented
// archetype: 1 live entity spread across a double-digit chunk count, i.e.
// GetChunks().size() > 1 and GetFragmentationLevel() far above the default
// 0.5 Defragment() threshold, so the Registry hands this straight to
// Archetype::CompactChunks().
//
// Pre-fix, CompactChunks' targetBytes for this state is
// clamp(1*6000/2, 4096, 524288) = clamp(3000, 4096, 524288) = 4096 -- which is
// LESS than the 6000-byte entity, so ComputeCapacityForBytes(4096) == 0 and
// compaction silently aborts every time (entitiesMoved == 0): this archetype
// could never be compacted. Post-fix, flooring at oneEntityBytes (6000) before
// the clamp yields targetBytes == 6000, capacity == 1, and compaction
// succeeds (entitiesMoved == 1), preserving the survivor's data.
TEST(ConfigPreservation, CompactChunksFitsOneEntityAcrossFragmentedBigComponentChunks)
{
    Astra::Registry reg;

    constexpr int kCount = 64;
    std::vector<Astra::Entity> entities;
    entities.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        entities.push_back(reg.CreateEntityWith(ConfigBigPos{float(i), float(i) * 2.0f, float(i) * 3.0f}));
        ASSERT_TRUE(entities.back().IsValid());
    }

    // Destroy every entity except the first, ascending order: this leaves the
    // survivor alone in chunk 0 and empties every other chunk (the true
    // trailing chunk auto-pops when its lone occupant is destroyed last;
    // every earlier chunk was a middle chunk when emptied and is retained).
    constexpr int survivorIndex = 0;
    for (int i = 0; i < kCount; ++i)
    {
        if (i != survivorIndex)
        {
            reg.DestroyEntity(entities[static_cast<size_t>(i)]);
        }
    }
    ASSERT_EQ(reg.Size(), 1u);

    auto result = reg.Defragment();
    EXPECT_EQ(result.entitiesMoved, 1u) << "CompactChunks aborted -- fit-one-entity floor regressed";

    auto* pos = reg.GetComponent<ConfigBigPos>(entities[static_cast<size_t>(survivorIndex)]);
    ASSERT_NE(pos, nullptr);
    EXPECT_FLOAT_EQ(pos->x, float(survivorIndex));
    EXPECT_FLOAT_EQ(pos->y, float(survivorIndex) * 2.0f);
    EXPECT_FLOAT_EQ(pos->z, float(survivorIndex) * 3.0f);
}
