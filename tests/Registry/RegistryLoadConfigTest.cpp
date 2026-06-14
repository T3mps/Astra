// Astra 3.3: Registry::Load must thread a Config (hence workScheduler) into the
// restored registry, so a hot-reload restore keeps parallel iteration. The two-arg
// Load stays sequential (default Config) -- proving the existing path is unchanged.

#include <gtest/gtest.h>

#include <Astra/Registry/Registry.hpp>
#include "../Support/TestWorkerPool.hpp"

#include <atomic>
#include <memory>

namespace
{
    struct Tally { int value = 0; };
}

TEST(RegistryLoadConfig, RestoredRegistryKeepsWorkScheduler)
{
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>(4);

    Astra::Registry::Config cfg;
    cfg.workScheduler = pool;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Tally>();

    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i)
        reg.CreateEntityWith(Tally{i});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Tally>();

    // NEW overload: pass the Config so the restored registry runs parallel.
    auto restored = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, cfg);
    ASSERT_TRUE(restored.IsOk());
    Astra::Registry& r = **restored.GetValue();

    std::atomic<int> visited{0};
    std::atomic<long long> sum{0};
    r.CreateView<Tally>().ParallelForEach([&](Astra::Entity, Tally& t) {
        visited.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(t.value, std::memory_order_relaxed);
    });
    EXPECT_EQ(visited.load(), kN);          // every entity visited exactly once
    EXPECT_EQ(sum.load(), static_cast<long long>(kN) * (kN - 1) / 2);   // 0+1+...+(kN-1): data survived
}

TEST(RegistryLoadConfig, TwoArgLoadWorksWithDefaultConfig)
{
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>(4);
    Astra::Registry::Config cfg;
    cfg.workScheduler = pool;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Tally>();
    reg.CreateEntityWith(Tally{1});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Tally>();

    // Existing two-arg Load: default Config -> null scheduler -> still works, sequential.
    auto restored = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(restored.IsOk());
    std::atomic<int> visited{0};
    (**restored.GetValue()).CreateView<Tally>().ParallelForEach(
        [&](Astra::Entity, Tally&) { visited.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(visited.load(), 1);
}
