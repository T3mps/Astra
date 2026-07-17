#include <atomic>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
}

// ---- Task 2: void(SystemContext&) systems run, read the registry, and -----
// ---- record deferred commands into their per-worker CommandBuffer. --------
//
// Scope boundary (per the Task 2 brief): the SystemScheduler OWNS the
// ParallelCommandBuffer and the executors invoke context systems into it, but
// nothing flushes/applies the recorded commands yet -- that is Task 3. So
// these tests assert INVOCATION and RECORDING only; they deliberately do NOT
// assert that the deferred DestroyEntity actually took effect.

TEST(SystemContext, ContextLambdaSystemRunsReadsRegistryAndRecordsCommand)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.IsValid(e));

    Astra::SystemScheduler s;
    bool ran = false;
    bool sawValidEntity = false;
    auto added = s.AddSystem([&ran, &sawValidEntity, e](Astra::SystemContext& ctx)
    {
        ran = true;
        sawValidEntity = ctx.GetRegistry().IsValid(e);  // reads the registry
        ctx.Commands().DestroyEntity(e);                // records a deferred command
    });
    ASSERT_TRUE(added.IsOk());
    EXPECT_EQ(s.PendingCommandCount(), 0u);  // nothing recorded before Execute()

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    EXPECT_TRUE(ran);                        // the context system was invoked
    EXPECT_TRUE(sawValidEntity);              // and could read the registry
    EXPECT_EQ(s.PendingCommandCount(), 1u);   // and recorded exactly one command

    // Task 3 (flush) is not wired in yet: the deferred DestroyEntity has not
    // been applied. This is the deliberate Task 2/3 scope boundary.
    EXPECT_TRUE(reg.IsValid(e));
}

TEST(SystemContext, ViewLambdaSystemStillRoutesToViewForEachNotMisroutedAsContext)
{
    // Guards the LambdaLike-vs-ContextSystem overload-resolution boundary:
    // a view-lambda ((Entity, Components&...)) is NOT invocable with a
    // single SystemContext&, so it must still satisfy LambdaLike and run
    // over a View, not be (mis)treated as a context system.
    Astra::Registry reg;
    (void)reg.CreateEntity<Position>();
    (void)reg.CreateEntity<Position>();

    Astra::SystemScheduler s;
    int touched = 0;
    auto added = s.AddSystem([&touched](Astra::Entity, Position& p)
    {
        p.x += 1.0f;
        ++touched;
    });
    ASSERT_TRUE(added.IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    EXPECT_EQ(touched, 2);
    EXPECT_EQ(s.PendingCommandCount(), 0u);  // view-lambda never touches the command buffer
}

TEST(SystemContext, RegistrySystemStillRoutesToRegistrySignature)
{
    // A plain void(Registry&) class-typed system must still match System<T>
    // only (unaffected by the new ContextSystem overloads).
    struct TouchesPosition : Astra::SystemTraits<Astra::Writes<Position>>
    {
        int* count;
        // Explicit ctor: a SystemTraits<...> base + extra data members can't
        // be safely constructed via T(args...) parenthesized aggregate init
        // (C++20 P0960) -- the empty base subobject consumes the first
        // argument slot, so args end up applied to the wrong "element".
        explicit TouchesPosition(int* c) : count(c) {}
        void operator()(Astra::Registry& r)
        {
            auto v = r.CreateView<Position>();
            v.ForEach([&](Astra::Entity, Position&) { ++(*count); });
        }
    };

    Astra::Registry reg;
    (void)reg.CreateEntity<Position>();

    Astra::SystemScheduler s;
    int count = 0;
    auto added = s.AddSystem<TouchesPosition>(&count);
    ASSERT_TRUE(added.IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    EXPECT_EQ(count, 1);
    EXPECT_EQ(s.PendingCommandCount(), 0u);
}

// ---- Struct-typed context systems + the parallel dispatch path ------------

namespace
{
    // Two struct-typed context systems declaring disjoint Writes<> masks so
    // BuildExecutionPlan groups them together for real concurrent dispatch
    // (a raw context LAMBDA never carries SystemTraits, so it can never join
    // a multi-member group -- these struct-typed systems are what exercises
    // ParallelExecutor's multi-member-group branch for context systems).
    struct DestroyViaContextA : Astra::SystemTraits<Astra::Writes<Position>>
    {
        std::atomic<int>* recorded;
        Astra::Entity target;
        DestroyViaContextA(std::atomic<int>* r, Astra::Entity e) : recorded(r), target(e) {}
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity(target);
            recorded->fetch_add(1);
        }
    };

    struct DestroyViaContextB : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        std::atomic<int>* recorded;
        Astra::Entity target;
        DestroyViaContextB(std::atomic<int>* r, Astra::Entity e) : recorded(r), target(e) {}
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity(target);
            recorded->fetch_add(1);
        }
    };
}

TEST(SystemContext, StructTypedContextSystemsRunConcurrentlyEachRecordingIntoOwnThreadBuffer)
{
    Astra::Registry reg;
    Astra::Entity e1 = reg.CreateEntity<Position>();
    Astra::Entity e2 = reg.CreateEntity<Velocity>();

    Astra::SystemScheduler s;
    std::atomic<int> recorded{0};
    ASSERT_TRUE(s.AddSystem<DestroyViaContextA>(&recorded, e1).IsOk());
    ASSERT_TRUE(s.AddSystem<DestroyViaContextB>(&recorded, e2).IsOk());

    // Disjoint Writes<> masks => the two context systems share one parallel
    // group, so ParallelExecutor dispatches them via IWorkScheduler::ParallelFor
    // (the multi-member-group branch), not the size==1 sequential shortcut.
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    ASSERT_EQ(plan[0].size(), 2u);

    Astra::ParallelExecutor exec(std::make_shared<Astra::Testing::TestWorkerPool>());
    s.Execute(reg, &exec);

    EXPECT_EQ(recorded.load(), 2);
    // Total across every per-worker CommandBuffer, regardless of which
    // worker thread happened to record which command.
    EXPECT_EQ(s.PendingCommandCount(), 2u);
}
