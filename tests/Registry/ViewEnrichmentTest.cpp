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

// ---- Task 3: ViewAccess harvesting ----------------------------------------

TEST(ViewAccess, HarvestsReadsWritesAndIgnoresFilters)
{
    // Force ID assignment so both expected and actual masks reference the same ids.
    (void)Astra::MakeComponentMask<Position, Velocity, Health, Transform, Name>();

    using V = Astra::View<Position, const Velocity, Astra::With<Transform>,
                          Astra::Not<Name>, Astra::Optional<Health>>;

    const auto reads  = Astra::ViewAccess<V>::ReadMask();
    const auto writes = Astra::ViewAccess<V>::WriteMask();

    // Reads = Velocity (const data) + Health (Optional non-const is a WRITE though) ...
    // Health here is Optional<Health> (non-const) -> write. So reads = {Velocity}.
    EXPECT_EQ(reads,  (Astra::MakeComponentMask<Velocity>()));
    EXPECT_EQ(writes, (Astra::MakeComponentMask<Position, Health>()));

    // With<Transform> and Not<Name> contribute to NEITHER set.
    EXPECT_FALSE(reads.Test(Astra::TypeID<Transform>::Value()));
    EXPECT_FALSE(writes.Test(Astra::TypeID<Transform>::Value()));
    EXPECT_FALSE(reads.Test(Astra::TypeID<Name>::Value()));
    EXPECT_FALSE(writes.Test(Astra::TypeID<Name>::Value()));
}

TEST(ViewAccess, ConstOptionalIsRead)
{
    (void)Astra::MakeComponentMask<Position, Health>();
    using V = Astra::View<Position, Astra::Optional<const Health>>;
    EXPECT_TRUE(Astra::ViewAccess<V>::ReadMask().Test(Astra::TypeID<Health>::Value()));
    EXPECT_FALSE(Astra::ViewAccess<V>::WriteMask().Test(Astra::TypeID<Health>::Value()));
}

// ---- Task 4: Contains + QueryError ----------------------------------------

TEST(ViewContains, FilterAware)
{
    Astra::Registry reg;
    auto match   = reg.CreateEntity<Position, Velocity>();
    auto noVel   = reg.CreateEntity<Position>();
    auto excluded= reg.CreateEntity<Position, Velocity, Name>();

    auto v = reg.CreateView<Position, const Velocity, Astra::Not<Name>>();
    EXPECT_TRUE(v.Contains(match));
    EXPECT_FALSE(v.Contains(noVel));      // missing required Velocity
    EXPECT_FALSE(v.Contains(excluded));   // has excluded Name

    reg.DestroyEntity(match);
    EXPECT_FALSE(v.Contains(match));      // stale handle
}
