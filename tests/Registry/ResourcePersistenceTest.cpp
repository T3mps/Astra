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

// AstraTransientResource (Arcane input-seam spec 2026-10-02 s4 amendment): a
// resource that opts out is never written by Save, so a Load -- even into a
// ComponentRegistry that never saw the type -- comes back without it, while an
// ordinary resource beside it survives.
namespace
{
    struct TransientClock
    {
        static constexpr bool AstraTransientResource = true;
        std::uint64_t step = 0;
        const int*    view = nullptr;   // the motivating case: a pointer into a host object
    };
    struct NotTransient
    {
        static constexpr bool AstraTransientResource = false;
        int v = 0;
    };
}

TEST(ResourcePersistence, TransientResourceIsAbsentAfterSaveLoadAndANormalOneSurvives)
{
    static_assert(Astra::IsTransientResourceV<TransientClock>);
    static_assert(!Astra::IsTransientResourceV<NotTransient>);
    static_assert(!Astra::IsTransientResourceV<GameCfg>);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    const int host = 5;
    reg.SetResource(GameCfg{99, 0.5f});
    reg.SetResource(TransientClock{42, &host});
    reg.SetResource(NotTransient{3});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());
    Astra::Registry& out = **loaded.GetValue();

    EXPECT_FALSE(out.HasResource<TransientClock>());
    EXPECT_EQ(out.GetResource<TransientClock>(), nullptr);
    ASSERT_NE(out.GetResource<GameCfg>(), nullptr);
    EXPECT_EQ(out.GetResource<GameCfg>()->seed, 99);
    ASSERT_NE(out.GetResource<NotTransient>(), nullptr);
    EXPECT_EQ(out.GetResource<NotTransient>()->v, 3);

    // The source registry still holds it: Save only skipped it.
    ASSERT_NE(reg.GetResource<TransientClock>(), nullptr);
    EXPECT_EQ(reg.GetResource<TransientClock>()->step, 42u);
}

TEST(ResourcePersistence, TransientResourceBytesNeverReachTheArchive)
{
    // A registry holding ONLY a transient resource saves an empty resource block,
    // so a Load against a ComponentRegistry that never registered the type
    // succeeds (an unknown hash would fail it -- UnknownResourceHashFailsLoad).
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    reg.SetResource(TransientClock{7, nullptr});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto freshReg = std::make_shared<Astra::ComponentRegistry>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), freshReg);
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_FALSE((*loaded.GetValue())->HasResource<TransientClock>());
}

TEST(ResourcePersistence, ATransientResourceInAnOlderResourceBlockIsDecodedAndDiscarded)
{
    // A resource block written before the type opted out still carries it.
    // Deserialize consumes its bytes (so the entries after it still decode) and
    // never creates it.
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<TransientClock>();
    creg->RegisterComponent<GameCfg>();
    const Astra::ComponentDescriptor* clockDesc = creg->GetComponentDescriptor(Astra::TypeID<TransientClock>::Value());
    const Astra::ComponentDescriptor* cfgDesc   = creg->GetComponentDescriptor(Astra::TypeID<GameCfg>::Value());
    ASSERT_NE(clockDesc, nullptr);
    ASSERT_NE(cfgDesc, nullptr);
    EXPECT_TRUE(clockDesc->isTransientResource);
    EXPECT_FALSE(cfgDesc->isTransientResource);

    std::vector<std::byte> block;
    {
        Astra::BinaryWriter writer(block);
        TransientClock clock{11, nullptr};
        GameCfg cfg{321, 4.0f};
        writer(std::uint32_t{2});
        writer(clockDesc->hash);
        clockDesc->serializeVersioned(writer, &clock);
        writer(cfgDesc->hash);
        cfgDesc->serializeVersioned(writer, &cfg);
        ASSERT_FALSE(writer.HasError());
    }

    Astra::ResourceStorage storage(creg);
    Astra::BinaryReader reader(block);
    ASSERT_TRUE(storage.Deserialize(reader));
    EXPECT_FALSE(storage.Has<TransientClock>());
    ASSERT_NE(storage.Get<GameCfg>(), nullptr);
    EXPECT_EQ(storage.Get<GameCfg>()->seed, 321);
    EXPECT_EQ(storage.Get<GameCfg>()->speed, 4.0f);
}
