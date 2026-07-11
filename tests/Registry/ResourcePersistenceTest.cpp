#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct GameCfg { int seed = 0; float speed = 1.0f; }; }

TEST(ResourcePersistence, RoundTripsThroughSaveLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    reg.SetResource(GameCfg{1234, 2.5f});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());

    auto* cfg = (*loaded.GetValue())->GetResource<GameCfg>();
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->seed, 1234);
    EXPECT_EQ(cfg->speed, 2.5f);
}

TEST(ResourcePersistence, UnknownResourceHashFailsLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    reg.SetResource(GameCfg{7, 1.0f});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    // Fresh registry that has never seen GameCfg:
    auto emptyReg = std::make_shared<Astra::ComponentRegistry>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), emptyReg);
    EXPECT_TRUE(loaded.IsErr());
}

namespace { struct BigCfg { double vals[16]; }; }  // 128 bytes > SBO_SIZE(64) → heap storage

TEST(ResourcePersistence, HeapResourceRoundTrips)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    BigCfg in{};
    for (int i = 0; i < 16; ++i) in.vals[i] = double(i) * 1.5;
    reg.SetResource(std::move(in));

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());

    auto* out = (*loaded.GetValue())->GetResource<BigCfg>();
    ASSERT_NE(out, nullptr);
    for (int i = 0; i < 16; ++i) EXPECT_EQ(out->vals[i], double(i) * 1.5);
}
