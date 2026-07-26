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

// ---- Task 5: Get ----------------------------------------------------------

TEST(ViewGet, ReturnsRefsAndPointers)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity>();
    reg.GetComponent<Position>(e)->x = 5.0f;
    reg.GetComponent<Velocity>(e)->dx = 2.0f;

    auto v = reg.CreateView<Position, const Velocity, Astra::Optional<Health>>();

    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto [pos, vel, health] = *r.GetValue();
    EXPECT_FLOAT_EQ(pos->x, 5.0f);
    EXPECT_FLOAT_EQ(vel->dx, 2.0f);
    EXPECT_EQ(health, nullptr);        // Optional<Health> absent -> null

    pos->x = 9.0f;                     // write through the returned pointer
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 9.0f);

    // pointer identity with GetComponent
    EXPECT_EQ(pos, reg.GetComponent<Position>(e));
}

TEST(ViewGet, NotMatchedCases)
{
    Astra::Registry reg;
    auto noVel = reg.CreateEntity<Position>();
    auto v = reg.CreateView<Position, const Velocity>();

    auto r = v.Get(noVel);
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(*r.GetError(), Astra::QueryError::NotMatched);

    reg.DestroyEntity(noVel);
    EXPECT_TRUE(v.Get(noVel).IsErr());   // stale handle
}

TEST(ViewGet, OptionalPresentIsNonNull)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity, Health>();
    auto v = reg.CreateView<Position, const Velocity, Astra::Optional<Health>>();
    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto [pos, vel, health] = *r.GetValue();
    (void)pos; (void)vel;
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health, reg.GetComponent<Health>(e));
}

// ---- Task 6: Single -------------------------------------------------------

TEST(ViewSingle, EmptyOneMany)
{
    Astra::Registry reg;
    auto v = reg.CreateView<Position, const Velocity>();

    EXPECT_TRUE(v.Single().IsErr());
    EXPECT_EQ(*v.Single().GetError(), Astra::QueryError::Empty);

    auto only = reg.CreateEntity<Position, Velocity>();
    reg.GetComponent<Position>(only)->x = 7.0f;
    {
        auto r = v.Single();
        ASSERT_TRUE(r.IsOk());
        auto [pos, vel] = *r.GetValue();   // Position*, const Velocity*  (POINTERS, not refs)
        (void)vel;
        EXPECT_FLOAT_EQ(pos->x, 7.0f);     // deref with ->
    }

    reg.CreateEntity<Position, Velocity>();   // now two
    auto r2 = v.Single();
    ASSERT_TRUE(r2.IsErr());
    EXPECT_EQ(*r2.GetError(), Astra::QueryError::MultipleMatched);
}

// ---- Enableable visibility: Contains/Get/Single agree with ForEach ----------

// EnA is the enableable-components suite's alias for Astra::Test::Hierarchy
// (ViewTest.cpp). Reuse it here rather than introducing a new type.
using EnA = Astra::Test::Hierarchy;

TEST(ViewEnableableVisibility, DisabledEntityInvisibleToRandomAccess)
{
    Astra::Registry reg;
    auto enabled  = reg.CreateEntity<Position, EnA>();
    auto disabled = reg.CreateEntity<Position, EnA>();
    reg.SetEnabled<EnA>(disabled, false);

    auto v = reg.CreateView<Position, EnA>();  // required enableable => HasRequiredFilter

    // ForEach visits only the enabled entity (the baseline all four must agree with)
    size_t seen = 0; Astra::Entity seenE{};
    v.ForEach([&](Astra::Entity e, Position&, EnA&) { ++seen; seenE = e; });
    EXPECT_EQ(seen, 1u);
    EXPECT_EQ(seenE, enabled);

    // Contains agrees
    EXPECT_TRUE(v.Contains(enabled));
    EXPECT_FALSE(v.Contains(disabled));

    // Get agrees (disabled -> NotMatched)
    EXPECT_TRUE(v.Get(enabled).IsOk());
    auto rd = v.Get(disabled);
    ASSERT_TRUE(rd.IsErr());
    EXPECT_EQ(*rd.GetError(), Astra::QueryError::NotMatched);

    // Single: exactly one ENABLED match (the disabled one must NOT count toward MultipleMatched)
    EXPECT_TRUE(v.Single().IsOk());
}
