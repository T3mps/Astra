#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct ConfigPos { float x, y, z; }; }

// Large-payload variant used only by LoadHonorsChunkPoolConfig below: Phase 2
// Task 5 (grow-as-populate chunk sizing) moved the per-chunk BYTE-SIZE bound
// off ArchetypeChunkPool::Config::chunkSize and onto ::maxChunkBytes (which
// defaults identically for "cfg" and the plain default Config below), so a
// 12-byte component would need ~44K entities packed into one chunk to make a
// save actually exceed the default 512KB cap. A big-payload component keeps
// the required entity count (and thus this test's runtime) small while still
// exercising the same "oversized-chunk save can't load into a smaller-cap
// pool" guard.
namespace { struct alignas(4) ConfigBigPos { float x, y, z; char pad[4084]; }; }
static_assert(sizeof(ConfigBigPos) == 4096, "guard math below assumes exactly 4096 bytes/entity");

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

    // Enough entities to ramp past the default 512KB cap (512KB / 4096B =
    // 128 entities/chunk) and into steady-state chunks clamped at cfg's 1MB
    // cap (1MB / 4096B = 256 entities/chunk) -- comfortably clears 128.
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
