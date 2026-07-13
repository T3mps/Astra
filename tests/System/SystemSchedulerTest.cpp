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
    (void)s.AddSystem<WA>();  // A
    (void)s.AddSystem<WB>();  // B (disjoint)
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

TEST(SystemScheduler, ConflictingSystemsSplitIntoSeparateGroups)
{
    Astra::SystemScheduler s;
    (void)s.AddSystem<WA>();
    (void)s.AddSystem<WA2>();  // both write A => conflict
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 0u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, PlanIsInsertionOrderStableNoLeapfrog)
{
    Astra::SystemScheduler s;
    (void)s.AddSystem<WA>();   // 0: writes A
    (void)s.AddSystem<WA2>();  // 1: writes A (conflicts with 0)
    (void)s.AddSystem<WB>();   // 2: writes B (independent)
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
    (void)s.AddSystem<WB>();   // 0: writes B
    (void)s.AddSystem<ExA>();  // 1: exclusive (even though A is disjoint from B)
    (void)s.AddSystem<WC>();   // 2: writes C
    const auto& plan = s.GetExecutionPlan();
    // 1 must be alone; nothing shares its group.
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, NoTraitSystemForcesSerialization)
{
    Astra::SystemScheduler s;
    (void)s.AddSystem<WA>();       // 0
    (void)s.AddSystem<NoTraits>(); // 1: no hints => solo
    (void)s.AddSystem<WB>();       // 2
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);  // the no-trait system is alone
}

// ---- Task 4: execution guard -----------------------------------------------

namespace
{
    // A system that mutates the scheduler mid-Execute (the practical misuse).
    struct SelfRemovingSystem
    {
        Astra::SystemScheduler* sched = nullptr;
        bool* sawExecuting = nullptr;
        void operator()(Astra::Registry&)
        {
            *sawExecuting = sched->IsExecuting();     // must be true inside Execute
            sched->RemoveSystem<SelfRemovingSystem>(); // must no-op (guarded)
        }
    };
}

TEST(SystemScheduler, IsExecutingTrueInsideExecuteAndMutationNoOps)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    bool sawExecuting = false;
    (void)s.AddSystem<SelfRemovingSystem>(&s, &sawExecuting);
    EXPECT_FALSE(s.IsExecuting());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    EXPECT_TRUE(sawExecuting);          // flag was set during Execute
    EXPECT_FALSE(s.IsExecuting());      // cleared after Execute
    EXPECT_EQ(s.Size(), 1u);           // RemoveSystem no-oped during execution
}

// ---- Task 5: failable registration -----------------------------------------

TEST(SystemScheduler, AddSystemReportsDuplicateAndSuccess)
{
    Astra::SystemScheduler s;
    auto first = s.AddSystem<WA>();
    EXPECT_TRUE(first.IsOk());
    auto dup = s.AddSystem<WA>();
    ASSERT_TRUE(dup.IsErr());
    EXPECT_EQ(*dup.GetError(), Astra::SystemError::AlreadyRegistered);
    EXPECT_EQ(s.Size(), 1u);
}

TEST(SystemScheduler, AddSystemDuringExecuteReturnsExecutingError)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    struct Probe {
        Astra::SystemScheduler* sched = nullptr;
        Astra::SystemError* out = nullptr;
        void operator()(Astra::Registry&)
        {
            auto r = sched->AddSystem<WB>();
            if (r.IsErr()) *out = *r.GetError();
        }
    };
    Astra::SystemError captured = Astra::SystemError::AlreadyRegistered;  // sentinel
    (void)s.AddSystem<Probe>(&s, &captured);
    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(captured, Astra::SystemError::SchedulerExecuting);
    EXPECT_EQ(s.Size(), 1u);
}

TEST(SystemScheduler, RemoveSystemKeepsSurvivorsValid)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    std::atomic<int> ran{0};
    struct Counter0 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(1); } };
    struct Counter1 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(10); } };
    struct Counter2 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(100); } };
    (void)s.AddSystem<Counter0>(&ran);
    (void)s.AddSystem<Counter1>(&ran);
    (void)s.AddSystem<Counter2>(&ran);
    s.RemoveSystem<Counter1>();          // remove the middle one
    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(ran.load(), 101);          // 0 and 2 ran, 1 did not; delegates valid
}

// ---- Task 6: C1 safety — Exclusive spawner is solo and safe under threads ---

namespace
{
    struct SpawnSystem : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>
    {
        void operator()(Astra::Registry& r)
        {
            for (int k = 0; k < 10; ++k) (void)r.CreateEntity<Position>();  // structural
        }
    };
    struct TouchVelocity : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        void operator()(Astra::Registry& r)
        {
            auto v = r.CreateView<Velocity>();
            v.ForEach([](Astra::Entity, Velocity& vel) { vel.dx += 1.0f; });
        }
    };
    struct TouchHealth : Astra::SystemTraits<Astra::Writes<Health>>
    {
        void operator()(Astra::Registry& r)
        {
            auto v = r.CreateView<Health>();
            v.ForEach([](Astra::Entity, Health& h) { h.current += 1; });
        }
    };

    size_t CountPositions(Astra::Registry& r)
    {
        auto v = r.CreateView<Position>();
        size_t n = 0;
        v.ForEach([&](Astra::Entity, Position&) { ++n; });
        return n;
    }
}

TEST(SystemScheduler, ExclusiveSpawnerRunsSoloWhilePureGroupRunsOnThreads)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    (void)s.AddSystem<SpawnSystem>();     // 0: exclusive => solo group
    (void)s.AddSystem<TouchVelocity>();   // 1: pure (writes B)
    (void)s.AddSystem<TouchHealth>();     // 2: pure (writes C, disjoint from B)
    // Plan: [[0]], [[1,2]] — 1 and 2 form a real multi-member group dispatched
    // concurrently to the pool; 0 (structural) is solo, so it never races them.
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);
    ASSERT_EQ(s.GetExecutionPlan()[1].size(), 2u);

    Astra::ParallelExecutor exec(std::make_shared<Astra::Testing::TestWorkerPool>());

    constexpr int kFrames = 50;
    for (int f = 0; f < kFrames; ++f)
        s.Execute(reg, &exec);

    // 10 new Position entities per frame, no corruption/loss. The Debug tripwire
    // sees no structural change across the pure [1,2] group, so it never fires.
    EXPECT_EQ(CountPositions(reg), static_cast<size_t>(10 * kFrames));
}
