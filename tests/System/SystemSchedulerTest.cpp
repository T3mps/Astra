#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
}

// ---- Task 1: structural-change counter accessor -----------------------------

TEST(SystemScheduler, StructuralChangeCounterIncrementsOnCreate)
{
    Astra::Registry reg;
    auto* am = reg.GetArchetypeManager();
    const uint32_t before = am->GetStructuralChangeCounter();
    (void)reg.CreateEntity<Position>();  // creates the {Position} archetype
    EXPECT_GT(am->GetStructuralChangeCounter(), before);
}

// ---- Task 2: SystemTraits pack-scan + Exclusive tag -------------------------

namespace
{
    using RW   = Astra::SystemTraits<Astra::Reads<Velocity>, Astra::Writes<Position>>;
    using WOnly = Astra::SystemTraits<Astra::Writes<Position>>;
    using WEx   = Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>;
    using ExOnly= Astra::SystemTraits<Astra::Exclusive>;

    static_assert(RW::HasTraits && !RW::RequiresExclusive);
    static_assert(std::tuple_size_v<RW::ReadsComponents>  == 1);
    static_assert(std::tuple_size_v<RW::WritesComponents> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::ReadsComponents>,  Velocity>);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::WritesComponents>, Position>);

    static_assert(!WOnly::RequiresExclusive);
    static_assert(std::tuple_size_v<WOnly::ReadsComponents> == 0);

    static_assert(WEx::RequiresExclusive);
    static_assert(std::tuple_size_v<WEx::WritesComponents> == 1);
    static_assert(std::tuple_size_v<WEx::ReadsComponents>  == 0);

    static_assert(ExOnly::RequiresExclusive && ExOnly::HasTraits);
    static_assert(std::tuple_size_v<ExOnly::ReadsComponents>  == 0);
    static_assert(std::tuple_size_v<ExOnly::WritesComponents> == 0);
}

TEST(SystemScheduler, SystemTraitsPackScanCompiles) { SUCCEED(); }

// ---- Task 3: plan construction ---------------------------------------------

namespace
{
    // A=Position, B=Velocity, C=Health. Distinct types => distinct registrations.
    struct WA  : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct WA2 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct WB  : Astra::SystemTraits<Astra::Writes<Velocity>> { void operator()(Astra::Registry&) {} };
    struct WC  : Astra::SystemTraits<Astra::Writes<Health>>   { void operator()(Astra::Registry&) {} };
    struct RA  : Astra::SystemTraits<Astra::Reads<Position>>  { void operator()(Astra::Registry&) {} };
    struct ExA : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive> { void operator()(Astra::Registry&) {} };
    struct NoTraits { void operator()(Astra::Registry&) {} };
}

TEST(SystemScheduler, NonConflictingSystemsShareAGroup)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();  // A
    s.AddSystem<WB>();  // B (disjoint)
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

TEST(SystemScheduler, ConflictingSystemsSplitIntoSeparateGroups)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();
    s.AddSystem<WA2>();  // both write A => conflict
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 0u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, PlanIsInsertionOrderStableNoLeapfrog)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();   // 0: writes A
    s.AddSystem<WA2>();  // 1: writes A (conflicts with 0)
    s.AddSystem<WB>();   // 2: writes B (independent)
    // Stable plan: [[0],[1,2]] — 2 never leapfrogs ahead of 1 into group 0.
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    ASSERT_EQ(plan[0].size(), 1u);
    EXPECT_EQ(plan[0][0], 0u);
    ASSERT_EQ(plan[1].size(), 2u);
    EXPECT_EQ(plan[1][0], 1u);
    EXPECT_EQ(plan[1][1], 2u);
}

TEST(SystemScheduler, ExclusiveSystemGetsSoloGroup)
{
    Astra::SystemScheduler s;
    s.AddSystem<WB>();   // 0: writes B
    s.AddSystem<ExA>();  // 1: exclusive (even though A is disjoint from B)
    s.AddSystem<WC>();   // 2: writes C
    const auto& plan = s.GetExecutionPlan();
    // 1 must be alone; nothing shares its group.
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, NoTraitSystemForcesSerialization)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();       // 0
    s.AddSystem<NoTraits>(); // 1: no hints => solo
    s.AddSystem<WB>();       // 2
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);  // the no-trait system is alone
}
