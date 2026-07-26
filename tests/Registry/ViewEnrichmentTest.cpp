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

// ---- Task 2: entity-optional iteration ------------------------------------

TEST(ViewEntityOptional, ForEachWithAndWithoutEntityVisitSameSet)
{
    Astra::Registry reg;
    reg.CreateEntity<Position, Velocity>();
    reg.CreateEntity<Position, Velocity>();
    reg.CreateEntity<Position>();   // no Velocity: excluded from the view below

    auto v = reg.CreateView<Position, const Velocity>();

    size_t withEntity = 0, withoutEntity = 0;
    v.ForEach([&](Astra::Entity, Position&, const Velocity&) { ++withEntity; });
    v.ForEach([&](Position&, const Velocity&)               { ++withoutEntity; });

    EXPECT_EQ(withEntity, 2u);
    EXPECT_EQ(withoutEntity, withEntity);
}

TEST(ViewEntityOptional, ParallelForEachAcceptsEntityless)
{
    Astra::Registry reg;
    for (int i = 0; i < 10; ++i) reg.CreateEntity<Position, Velocity>();

    auto v = reg.CreateView<Position, const Velocity>();
    std::atomic<size_t> n{0};
    v.ParallelForEach([&](Position&, const Velocity&) { n.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(n.load(), 10u);   // sequential fallback (no scheduler injected) still runs the body
}
