#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct ConfigPos { float x, y, z; }; }

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
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<ConfigPos>();
    for (int i = 0; i < 10000; ++i)
        reg.CreateEntityWith(ConfigPos{float(i), 0, 0});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<ConfigPos>();

    // Same config: must load and preserve pool size.
    auto ok = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, cfg);
    ASSERT_TRUE(ok.IsOk());
    EXPECT_EQ((*ok.GetValue())->GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);
    EXPECT_EQ((*ok.GetValue())->Size(), 10000u);

    // Default (16KB) config: saved 64KB chunk layout cannot fit — must error,
    // not overflow.
    auto bad = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    EXPECT_TRUE(bad.IsErr());
}
