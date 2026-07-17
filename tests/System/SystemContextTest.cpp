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
// ---- Task 3 wires the flush: by the time Execute() returns, every ---------
// ---- recorded command has been applied and the buffer is empty again. -----

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

    // Task 3: Execute() flushes the recorded command (in insertion order for
    // the sequential/null-scheduler path) and clears the buffer before
    // returning -- the deferred DestroyEntity has now actually taken effect.
    EXPECT_EQ(s.PendingCommandCount(), 0u);
    EXPECT_FALSE(reg.IsValid(e));
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
    // Task 3: Execute() flushes every per-worker CommandBuffer (regardless of
    // which worker thread happened to record which command) and clears them
    // before returning -- both deferred DestroyEntity commands have now
    // actually taken effect, and nothing is left pending.
    EXPECT_EQ(s.PendingCommandCount(), 0u);
    EXPECT_FALSE(reg.IsValid(e1));
    EXPECT_FALSE(reg.IsValid(e2));
}

// ---- Task 3: the deterministic flush at the depth==0 sync point -----------

namespace
{
    // A tiny non-empty component whose value records WHICH system last wrote
    // it -- lets the test tell the two systems' deferred writes apart after
    // the flush without needing to inspect the CommandBuffer internals.
    struct WinnerTag
    {
        int writer = -1;
    };
    static_assert(Astra::Component<WinnerTag>, "WinnerTag must satisfy Component concept");

    // Two struct-typed context systems with DISJOINT declared Writes<> masks
    // (same pattern as DestroyViaContextA/B above) so BuildExecutionPlan
    // groups them into ONE multi-member parallel group -- exercising
    // ParallelExecutor's IWorkScheduler::ParallelFor branch, not the
    // group.size()==1 sequential shortcut. A trait-less context LAMBDA always
    // gets a solo group and would never exercise this.
    //
    // Each system defers an unconditional "overwrite" of WinnerTag on the
    // SAME target entity: RemoveComponent (always succeeds because the tag
    // is pre-seeded on the entity before Execute()) immediately followed by
    // AddComponent with this system's own writer id. WinnerTag itself is NOT
    // in either system's declared mask -- it's the deferred change, not a
    // declared read/write, which is exactly why deferring it lets both
    // systems run concurrently without racing each other's structural
    // mutation of the shared target entity.
    //
    // Determinism: SortKey compares insertionOrder FIRST, so every command
    // DeferWinnerA (insertionOrder 0) records sorts before every command
    // DeferWinnerB (insertionOrder 1) records, regardless of which worker
    // thread recorded them or how the two systems happened to interleave.
    // The flush therefore always applies A's Remove+Add, then B's Remove+Add
    // -- B (the higher insertionOrder) always wins.
    struct DeferWinnerA : Astra::SystemTraits<Astra::Writes<Position>>
    {
        Astra::Entity target;
        explicit DeferWinnerA(Astra::Entity e) : target(e) {}
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().RemoveComponent<WinnerTag>(target);
            ctx.Commands().AddComponent<WinnerTag>(target, WinnerTag{0});
        }
    };

    struct DeferWinnerB : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        Astra::Entity target;
        explicit DeferWinnerB(Astra::Entity e) : target(e) {}
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().RemoveComponent<WinnerTag>(target);
            ctx.Commands().AddComponent<WinnerTag>(target, WinnerTag{1});
        }
    };
}

TEST(SystemContext, DeferredCommandsFlushDeterministicallyByInsertionOrderAcross20Runs)
{
    // One real multi-threaded pool, reused across every run: what's under
    // test is that the FLUSH is deterministic despite genuine concurrent
    // recording, not that thread startup/teardown is deterministic.
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();

    for (int run = 0; run < 20; ++run)
    {
        Astra::Registry reg;
        Astra::Entity target = reg.CreateEntity<Position, Velocity>();
        // Pre-seed directly via the immediate Registry API (not deferred) so
        // both systems' deferred RemoveComponent<WinnerTag> has something to
        // remove -- RemoveComponentByID fails if the entity doesn't already
        // have the component, and a failed command would abort the flush
        // (ExecuteSorted treats any failed ApplyCommandAt as an error), which
        // is not what this test is exercising.
        reg.AddComponent<WinnerTag>(target, WinnerTag{-1});

        Astra::SystemScheduler s;
        ASSERT_TRUE(s.AddSystem<DeferWinnerA>(target).IsOk());  // insertionOrder 0
        ASSERT_TRUE(s.AddSystem<DeferWinnerB>(target).IsOk());  // insertionOrder 1

        // Guard the premise: disjoint Writes<> masks must still yield one
        // group of 2 (not two solo groups), or this test would silently stop
        // exercising the parallel dispatch path it's designed to cover.
        const auto& plan = s.GetExecutionPlan();
        ASSERT_EQ(plan.size(), 1u);
        ASSERT_EQ(plan[0].size(), 2u);

        Astra::ParallelExecutor exec(pool);
        s.Execute(reg, &exec);

        ASSERT_TRUE(reg.HasComponent<WinnerTag>(target));
        EXPECT_EQ(reg.GetComponent<WinnerTag>(target)->writer, 1)
            << "run " << run << ": the higher-insertionOrder system (B) must "
               "deterministically win the last-write-wins flush";
        // The flush must have cleared the buffer -- nothing left pending for
        // a subsequent frame to (re)apply.
        EXPECT_EQ(s.PendingCommandCount(), 0u) << "run " << run;
    }
}

// ---- Task 4: a flush-time logical failure is skipped + reported, not ------
// ---- aborted -- the world stays consistent and the error is surfaced. -----

TEST(SystemContext, DeferredCommandTargetingEntityDestroyedEarlierInSameFlushIsSkippedAndReported)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.IsValid(e));

    Astra::SystemScheduler s;

    // System A (insertionOrder 0): defers DestroyEntity(e).
    auto addedA = s.AddSystem([e](Astra::SystemContext& ctx)
    {
        ctx.Commands().DestroyEntity(e);
    });
    ASSERT_TRUE(addedA.IsOk());

    // System B (insertionOrder 1, added second): defers AddComponent<Velocity>
    // on the SAME entity e. SortKey compares insertionOrder first, so the
    // sorted flush always applies A's destroy BEFORE B's add, regardless of
    // execution order -- B's add then targets an already-destroyed entity,
    // so ApplyCommandAt() returns false for it. That must be skipped and
    // reported, NOT abort the whole flush (which would leave A's destroy's
    // fate -- and the rest of the world -- in question).
    auto addedB = s.AddSystem([e](Astra::SystemContext& ctx)
    {
        ctx.Commands().AddComponent<Velocity>(e, Velocity{1.0f, 2.0f, 3.0f});
    });
    ASSERT_TRUE(addedB.IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    // The world is consistent: A's destroy took effect, no crash, and B's
    // skipped add left no trace (impossible -- the entity is gone).
    EXPECT_FALSE(reg.IsValid(e));
    EXPECT_EQ(s.PendingCommandCount(), 0u);  // flush still drains regardless of skips

    // B's failed op is surfaced, attributed to B's insertionOrder (1) -- NOT
    // A's, and NOT silently dropped.
    const auto& errors = s.GetLastDeferredErrors();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].systemInsertionOrder, 1u);
    EXPECT_EQ(errors[0].reason, Astra::DeferredCommandError::Reason::InvalidTargetEntity);
}
