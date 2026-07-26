#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include "../TestComponents.hpp"

using Astra::Test::Position;
using Astra::Test::Velocity;
using Astra::Test::Health;
using Astra::Test::Transform;
using Astra::Test::Name;

// ---- Task 1: With<T> ------------------------------------------------------

TEST(ViewWith, MatchesRequirePresenceButDoNotYield)
{
    Astra::Registry reg;
    auto withT   = reg.CreateEntity<Position, Transform>();   // matches With<Transform>
    auto without = reg.CreateEntity<Position>();              // no Transform -> excluded
    (void)withT; (void)without;

    auto v = reg.CreateView<Position, Astra::With<Transform>>();

    // Callback receives ONLY Position (Transform is not yielded).
    size_t n = 0;
    v.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 1u);   // only the Position+Transform entity
}

TEST(ViewWith, CombinesWithNotAndRequired)
{
    Astra::Registry reg;
    auto ok      = reg.CreateEntity<Position, Transform>();
    auto frozen  = reg.CreateEntity<Position, Transform, Name>();  // excluded by Not<Name>
    auto noWith  = reg.CreateEntity<Position>();                   // excluded by With<Transform>
    (void)ok; (void)frozen; (void)noWith;

    auto v = reg.CreateView<Position, Astra::With<Transform>, Astra::Not<Name>>();
    size_t n = 0;
    v.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 1u);   // only `ok`
}
