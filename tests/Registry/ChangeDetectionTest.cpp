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
