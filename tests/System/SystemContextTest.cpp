#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
    using Astra::Test::Transform;
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

// ---- Task 5: worker-safe deferred entity creation via placeholder entities, --
// ---- resolved to DETERMINISTIC real ids at the sync-point flush. ------------

namespace
{
    // Two struct-typed context systems with DISJOINT declared Writes<> masks so
    // BuildExecutionPlan groups them into ONE multi-member parallel group --
    // ParallelExecutor then dispatches them genuinely concurrently under the
    // real TestWorkerPool (same pattern as the Task 3 determinism test).
    //
    // Each system defers CREATION of a fresh entity plus an AddComponent on
    // that just-created entity, referencing it by the PLACEHOLDER handle that
    // ctx.Commands().CreateEntity() returns. No EntityManager allocation
    // happens at record time (that would race across the two workers); the
    // placeholder is resolved to a real id single-threaded at the flush, in
    // sort-key (insertionOrder) order -- so system A (insertionOrder 0) always
    // resolves before system B (insertionOrder 1), giving deterministic ids.
    struct SpawnPositionEntity : Astra::SystemTraits<Astra::Writes<Position>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            Astra::Entity e = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(e, Position{1.0f, 2.0f, 3.0f});
        }
    };

    struct SpawnVelocityEntity : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            Astra::Entity e = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Velocity>(e, Velocity{4.0f, 5.0f, 6.0f});
        }
    };
}

TEST(SystemContext, DeferredCreateResolvesToRealEntitiesWithDeterministicIdsAcross20Runs)
{
    // One real multi-threaded pool reused across every run: what's under test
    // is that placeholder RESOLUTION is deterministic despite genuinely
    // concurrent recording of CreateEntity from two workers, not that thread
    // startup/teardown is deterministic.
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();

    // Captured from run 0; every later run must reproduce these EXACT resolved
    // real-entity values -- the B2 determinism contract for deferred creation.
    Astra::Entity::StorageType posValueRun0 = 0;
    Astra::Entity::StorageType velValueRun0 = 0;

    for (int run = 0; run < 20; ++run)
    {
        Astra::Registry reg;

        // Pre-register Position/Velocity on the main thread. CommandBuffer::
        // AddComponent registers the component type at RECORD time; the two
        // systems record concurrently, so a first-ever registration of two
        // different component types would race the shared ComponentRegistry.
        // That race is orthogonal to Task 5 (deferred entity CREATION) -- the
        // Task 3 determinism test avoids it the same way by pre-seeding its
        // component. Pre-registering leaves each system's record-time
        // AddComponent a pure lookup, isolating this test to what it tests:
        // worker-safe placeholder creation + deterministic id resolution.
        reg.GetComponentRegistry()->RegisterComponent<Position>();
        reg.GetComponentRegistry()->RegisterComponent<Velocity>();

        Astra::SystemScheduler s;
        ASSERT_TRUE(s.AddSystem<SpawnPositionEntity>().IsOk());  // insertionOrder 0
        ASSERT_TRUE(s.AddSystem<SpawnVelocityEntity>().IsOk());  // insertionOrder 1

        // Guard the premise: disjoint Writes<> masks must yield one group of 2
        // (concurrent dispatch), not two solo groups.
        const auto& plan = s.GetExecutionPlan();
        ASSERT_EQ(plan.size(), 1u);
        ASSERT_EQ(plan[0].size(), 2u);

        Astra::ParallelExecutor exec(pool);
        s.Execute(reg, &exec);

        // Both placeholders resolved to real entities carrying their component.
        // The placeholder handles CreateEntity() returned are NOT the real ids,
        // so query the world by component (view) to find them.
        EXPECT_EQ(reg.Size(), 2u) << "run " << run;

        auto posView = reg.CreateView<Position>();
        auto velView = reg.CreateView<Velocity>();
        ASSERT_EQ(posView.Size(), 1u) << "run " << run;
        ASSERT_EQ(velView.Size(), 1u) << "run " << run;

        Astra::Entity posEntity = Astra::Entity::Invalid();
        posView.ForEach([&](Astra::Entity e, Position&) { posEntity = e; });
        Astra::Entity velEntity = Astra::Entity::Invalid();
        velView.ForEach([&](Astra::Entity e, Velocity&) { velEntity = e; });

        ASSERT_TRUE(reg.IsValid(posEntity)) << "run " << run;
        ASSERT_TRUE(reg.IsValid(velEntity)) << "run " << run;

        if (run == 0)
        {
            posValueRun0 = posEntity.GetValue();
            velValueRun0 = velEntity.GetValue();
        }
        else
        {
            // Deterministic resolution: identical resolved real ids every run.
            EXPECT_EQ(posEntity.GetValue(), posValueRun0) << "run " << run;
            EXPECT_EQ(velEntity.GetValue(), velValueRun0) << "run " << run;
        }

        // The flush drained every recorded command -- nothing left pending.
        EXPECT_EQ(s.PendingCommandCount(), 0u) << "run " << run;
    }
}

// ---- Task 6: the acceptance gate -- spec Section 14's determinism ---------
// ---- contract, end to end: same initial world + same systems => -----------
// ---- IDENTICAL applied order of every deferred structural change (and -----
// ---- the real ids assigned to placeholder entities), independent of ------
// ---- thread count / scheduling. --------------------------------------------

namespace
{
    // Four struct-typed context systems with pairwise DISJOINT declared
    // Writes<> masks (Position / Velocity / Health / Transform) so
    // BuildExecutionPlan places all four in ONE parallel group (hard-
    // asserted in the test body below) -- forcing ParallelExecutor to
    // dispatch all four concurrently on real TestWorkerPool threads via
    // IWorkScheduler::ParallelFor, not the group.size()==1 sequential
    // shortcut. As with DeferWinnerA/B and DestroyViaContextA/B above, the
    // declared mask is purely a SCHEDULING tag -- none of the four systems'
    // deferred ops are restricted to their own mask. What makes concurrent
    // RECORDING safe is that every mutation below goes through
    // ctx.Commands() (deferred), never applied to the Registry synchronously
    // inside operator().
    //
    // SortKey (Task 1) totally orders the flush by {insertionOrder,
    // recordSequence}: EVERY command SysA (insertionOrder 0) recorded
    // applies before ANY command SysB (1) recorded, which applies before ALL
    // of SysC's (2), then SysD's (3) -- regardless of which worker thread
    // ran which system or how their recording interleaved in real time
    // (Task 3). That total order is what turns the elaborate overlap below
    // into a deterministic OUTCOME rather than a race:
    //
    //   * health[0] and health[1]: SysA, then SysB, then SysD each
    //     Remove+Add Health with a different value -- the highest-
    //     insertionOrder writer (SysD) always wins (last-write-wins,
    //     mirroring the Task 3 WinnerTag test above).
    //   * pos[4]: SysA destroys it (insertionOrder 0); SysC's later
    //     AddComponent<Health> targeting it (insertionOrder 2) therefore
    //     always finds an invalid entity and is deterministically skipped +
    //     reported (Task 4), attributed to SysC.
    //   * vel[4]: SysB destroys it (insertionOrder 1); SysD's later
    //     AddComponent<Health> targeting it (insertionOrder 3) is likewise
    //     always skipped + reported, attributed to SysD.
    //   * Every system also creates 2 entities via a placeholder handle
    //     (Task 5), immediately adding a component to it -- resolved to a
    //     real id single-threaded, in sort-key order, at the flush.
    struct DetSysA : Astra::SystemTraits<Astra::Writes<Position>>
    {
        const std::vector<Astra::Entity>* pos;
        const std::vector<Astra::Entity>* health;
        DetSysA(const std::vector<Astra::Entity>* p, const std::vector<Astra::Entity>* h) : pos(p), health(h) {}

        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity((*pos)[0]);
            ctx.Commands().DestroyEntity((*pos)[1]);
            ctx.Commands().DestroyEntity((*pos)[4]);  // SysC targets this later -> deterministic skip

            ctx.Commands().AddComponent<Velocity>((*pos)[2], Velocity{702.0f, 0.0f, 0.0f});
            ctx.Commands().AddComponent<Velocity>((*pos)[3], Velocity{703.0f, 0.0f, 0.0f});

            // Opens the health[0]/health[1] last-write-wins chain.
            ctx.Commands().RemoveComponent<Health>((*health)[0]);
            ctx.Commands().AddComponent<Health>((*health)[0], Health{100, 999});
            ctx.Commands().RemoveComponent<Health>((*health)[1]);
            ctx.Commands().AddComponent<Health>((*health)[1], Health{101, 999});

            Astra::Entity n0 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(n0, Position{9000.0f, 0.0f, 0.0f});
            Astra::Entity n1 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(n1, Position{9001.0f, 0.0f, 0.0f});
        }
    };

    struct DetSysB : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        const std::vector<Astra::Entity>* vel;
        const std::vector<Astra::Entity>* health;
        DetSysB(const std::vector<Astra::Entity>* v, const std::vector<Astra::Entity>* h) : vel(v), health(h) {}

        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity((*vel)[0]);
            ctx.Commands().DestroyEntity((*vel)[1]);
            ctx.Commands().DestroyEntity((*vel)[4]);  // SysD targets this later -> deterministic skip

            ctx.Commands().AddComponent<Health>((*vel)[2], Health{802, 999});
            ctx.Commands().AddComponent<Health>((*vel)[3], Health{803, 999});

            // Middle of the health[0]/health[1] chain: always applies after
            // SysA's (insertionOrder 0 < 1) and before SysD's (1 < 3).
            ctx.Commands().RemoveComponent<Health>((*health)[0]);
            ctx.Commands().AddComponent<Health>((*health)[0], Health{200, 999});
            ctx.Commands().RemoveComponent<Health>((*health)[1]);
            ctx.Commands().AddComponent<Health>((*health)[1], Health{201, 999});

            Astra::Entity n0 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Velocity>(n0, Velocity{8000.0f, 0.0f, 0.0f});
            Astra::Entity n1 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Velocity>(n1, Velocity{8001.0f, 0.0f, 0.0f});
        }
    };

    struct DetSysC : Astra::SystemTraits<Astra::Writes<Health>>
    {
        const std::vector<Astra::Entity>* health;
        const std::vector<Astra::Entity>* pos;
        DetSysC(const std::vector<Astra::Entity>* h, const std::vector<Astra::Entity>* p) : health(h), pos(p) {}

        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity((*health)[2]);
            ctx.Commands().DestroyEntity((*health)[3]);

            ctx.Commands().AddComponent<Position>((*health)[4], Position{404.0f, 0.0f, 0.0f});
            ctx.Commands().AddComponent<Position>((*health)[5], Position{405.0f, 0.0f, 0.0f});

            // pos[4] was destroyed by SysA (insertionOrder 0 < 2): this
            // AddComponent deterministically fails and is reported against
            // THIS system's insertionOrder (2) -- Task 4's skip+report path.
            ctx.Commands().AddComponent<Health>((*pos)[4], Health{999, 999});

            // Ordinary (non-overlapping-with-another-system) overwrite.
            ctx.Commands().RemoveComponent<Position>((*pos)[2]);
            ctx.Commands().AddComponent<Position>((*pos)[2], Position{502.0f, 0.0f, 0.0f});
            ctx.Commands().RemoveComponent<Position>((*pos)[3]);
            ctx.Commands().AddComponent<Position>((*pos)[3], Position{503.0f, 0.0f, 0.0f});

            Astra::Entity n0 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Health>(n0, Health{6000, 999});
            Astra::Entity n1 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Health>(n1, Health{6001, 999});
        }
    };

    struct DetSysD : Astra::SystemTraits<Astra::Writes<Transform>>
    {
        const std::vector<Astra::Entity>* pos;
        const std::vector<Astra::Entity>* vel;
        const std::vector<Astra::Entity>* health;
        DetSysD(const std::vector<Astra::Entity>* p, const std::vector<Astra::Entity>* v, const std::vector<Astra::Entity>* h)
            : pos(p), vel(v), health(h) {}

        void operator()(Astra::SystemContext& ctx)
        {
            ctx.Commands().DestroyEntity((*pos)[5]);
            ctx.Commands().DestroyEntity((*vel)[6]);

            ctx.Commands().AddComponent<Position>((*health)[6], Position{606.0f, 0.0f, 0.0f});

            // Final write of the health[0]/health[1] chain: SysD has the
            // highest insertionOrder (3), so this value always survives.
            ctx.Commands().RemoveComponent<Health>((*health)[0]);
            ctx.Commands().AddComponent<Health>((*health)[0], Health{300, 999});
            ctx.Commands().RemoveComponent<Health>((*health)[1]);
            ctx.Commands().AddComponent<Health>((*health)[1], Health{301, 999});

            // vel[4] was destroyed by SysB (insertionOrder 1 < 3): this
            // AddComponent deterministically fails and is reported against
            // THIS system's insertionOrder (3).
            ctx.Commands().AddComponent<Health>((*vel)[4], Health{777, 999});

            Astra::Entity n0 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(n0, Position{7000.0f, 0.0f, 0.0f});
            Astra::Entity n1 = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(n1, Position{7001.0f, 0.0f, 0.0f});
        }
    };
}

TEST(SystemContext, RichMixOfDeferredStructuralChangesFlushesIdenticallyAcross50RunsUnderRealWorkers)
{
    // One real multi-threaded pool, reused across every run: what's under
    // test is that the FLUSH (sort-key ordering, placeholder resolution,
    // and skip+report attribution) is deterministic despite genuinely
    // concurrent recording, not that thread startup/teardown is
    // deterministic.
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();

    std::string snapshotRun0;

    for (int run = 0; run < 50; ++run)
    {
        Astra::Registry reg;

        // CRITICAL prerequisite (Task 5 known limitation): CommandBuffer::
        // AddComponent<T> calls RegisterComponent<T>() at RECORD time, so
        // two workers first-registering DIFFERENT component types
        // concurrently would race the shared ComponentRegistry. Pre-register
        // every component type any system below will AddComponent<T>, on the
        // main thread, BEFORE Execute() -- the same pattern the Task 5
        // determinism test uses. (The entity creation below already
        // registers these as a side effect -- RegisterComponent<T>() is
        // idempotent -- but the calls are kept explicit so this test's
        // correctness doesn't silently depend on that.)
        reg.GetComponentRegistry()->RegisterComponent<Position>();
        reg.GetComponentRegistry()->RegisterComponent<Velocity>();
        reg.GetComponentRegistry()->RegisterComponent<Health>();

        // Seed a rich initial world: 30 pre-existing entities across 3
        // component types, single-threaded, identical every run.
        std::vector<Astra::Entity> pos, vel, health;
        pos.reserve(10);
        vel.reserve(10);
        health.reserve(10);
        for (int i = 0; i < 10; ++i)
            pos.push_back(reg.CreateEntityWith(Position{float(i), 0.0f, 0.0f}));
        for (int i = 0; i < 10; ++i)
            vel.push_back(reg.CreateEntityWith(Velocity{float(100 + i), 0.0f, 0.0f}));
        for (int i = 0; i < 10; ++i)
            health.push_back(reg.CreateEntityWith(Health{200 + i, 999}));
        ASSERT_EQ(reg.Size(), 30u) << "run " << run;

        Astra::SystemScheduler s;
        ASSERT_TRUE(s.AddSystem<DetSysA>(&pos, &health).IsOk());        // insertionOrder 0
        ASSERT_TRUE(s.AddSystem<DetSysB>(&vel, &health).IsOk());        // insertionOrder 1
        ASSERT_TRUE(s.AddSystem<DetSysC>(&health, &pos).IsOk());        // insertionOrder 2
        ASSERT_TRUE(s.AddSystem<DetSysD>(&pos, &vel, &health).IsOk());  // insertionOrder 3

        // Hard-assert the premise: 4 pairwise-disjoint Writes<> masks must
        // yield ONE group of 4 (genuine concurrent dispatch), never solo
        // groups -- otherwise this gate would silently stop exercising the
        // parallel dispatch path it exists to cover.
        const auto& plan = s.GetExecutionPlan();
        ASSERT_EQ(plan.size(), 1u) << "run " << run;
        ASSERT_EQ(plan[0].size(), 4u) << "run " << run;

        Astra::ParallelExecutor exec(pool);
        s.Execute(reg, &exec);

        EXPECT_EQ(s.PendingCommandCount(), 0u) << "run " << run;

        // 10 destroyed (3 by A, 3 by B, 2 by C, 2 by D) + 8 created (2 per
        // system) out of 30 initial = 28 live entities every run.
        EXPECT_EQ(reg.Size(), 28u) << "run " << run;

        // Exactly 2 deterministic skip+report failures every run (pos[4] via
        // SysC, vel[4] via SysD -- see the systems' comments above).
        const auto& errors = s.GetLastDeferredErrors();
        EXPECT_EQ(errors.size(), 2u) << "run " << run;

        // ---- Canonical world snapshot ----
        // Sorted (by real entity id) map of every live entity -> a string
        // describing exactly which of {Position, Velocity, Health} it
        // carries and their field values. A std::map key-iterates in sorted
        // StorageType order for free, so this is deterministic regardless of
        // archetype/chunk iteration order. Every live entity in this world
        // carries at least one of these 3 types (nothing else is ever
        // added), so unioning the 3 single-type views covers every entity
        // exactly once (describe() re-derives the FULL per-entity string
        // regardless of which view found it first).
        auto describe = [](Astra::Registry& r, Astra::Entity e) -> std::string
        {
            std::string out;
            if (auto* p = r.GetComponent<Position>(e))
                out += "P(" + std::to_string(p->x) + ")";
            if (auto* v = r.GetComponent<Velocity>(e))
                out += "V(" + std::to_string(v->dx) + ")";
            if (auto* h = r.GetComponent<Health>(e))
                out += "H(" + std::to_string(h->current) + ")";
            return out;
        };

        std::map<Astra::Entity::StorageType, std::string> byId;
        reg.CreateView<Position>().ForEach([&](Astra::Entity e, Position&) { byId[e.GetValue()] = describe(reg, e); });
        reg.CreateView<Velocity>().ForEach([&](Astra::Entity e, Velocity&) { byId[e.GetValue()] = describe(reg, e); });
        reg.CreateView<Health>().ForEach([&](Astra::Entity e, Health&) { byId[e.GetValue()] = describe(reg, e); });
        ASSERT_EQ(byId.size(), reg.Size()) << "run " << run
            << ": every live entity must carry Position, Velocity, and/or Health";

        std::string snapshot;
        for (const auto& [id, desc] : byId)
        {
            snapshot += std::to_string(id) + ":" + desc + ";";
        }
        // The deferred-command error list is itself gathered in globally
        // SortKey-sorted order (ParallelCommandBuffer::ExecuteSorted -- every
        // key here is globally unique, so there are no ties to break non-
        // deterministically), so appending it in-order is safe.
        for (const auto& err : errors)
        {
            snapshot += "ERR(" + std::to_string(err.systemInsertionOrder) + "," +
                        std::to_string(static_cast<int>(err.reason)) + ");";
        }

        if (run == 0)
        {
            snapshotRun0 = snapshot;
            // Guard against a degenerate always-equal comparison: run 0's
            // snapshot must actually contain the entities/values this test
            // was designed to produce.
            ASSERT_FALSE(snapshotRun0.empty());
        }
        else
        {
            EXPECT_EQ(snapshot, snapshotRun0)
                << "run " << run << ": deferred-command flush produced a "
                   "DIFFERENT world state than run 0 -- a determinism bug "
                   "(sort-key ordering, placeholder resolution, or dispatch) "
                   "survived Tasks 1-5. This test must NOT be weakened; "
                   "escalate instead.";
        }
    }
}

// ---- Theme B2 Phase B, Task 2: SystemContext chunk sub-context plumbing ---
// ---- (parameterized iterationIndex + nullable ParallelCommandBuffer*). ----

TEST(SystemContext, SubContextStampsIterationIndexAndNormalContextStampsZero)
{
    Astra::Registry reg;
    Astra::Entity e1 = reg.CreateEntity<Position>();
    Astra::Entity e2 = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.IsValid(e1));
    ASSERT_TRUE(reg.IsValid(e2));

    // Phase A path: the existing 3-arg ctor must still stamp iterationIndex 0.
    Astra::CommandBuffer normalBuf(&reg);
    Astra::SystemContext normalCtx(reg, normalBuf, /*insertionOrder=*/5u);
    normalCtx.Commands().DestroyEntity(e1);  // recordSequence 0
    normalCtx.Commands().DestroyEntity(e1);  // recordSequence 1 (same context, monotonic)

    const auto& normalKeys = normalBuf.CommandKeys();
    ASSERT_EQ(normalKeys.size(), 2u);
    EXPECT_EQ(normalKeys[0].first.insertionOrder, 5u);
    EXPECT_EQ(normalKeys[0].first.iterationIndex, 0u);
    EXPECT_EQ(normalKeys[0].first.recordSequence, 0u);
    EXPECT_EQ(normalKeys[1].first.insertionOrder, 5u);
    EXPECT_EQ(normalKeys[1].first.iterationIndex, 0u);
    EXPECT_EQ(normalKeys[1].first.recordSequence, 1u);

    // Task 2 path: the new 5-arg ctor stamps the given iterationIndex K, and
    // exposes the (here null) ParallelCommandBuffer* via GetParallelBuffer().
    constexpr uint32_t K = 7u;
    Astra::CommandBuffer subBuf(&reg);
    Astra::SystemContext subCtx(reg, subBuf, /*insertionOrder=*/5u, K, /*parallelBuffer=*/nullptr);
    subCtx.Commands().DestroyEntity(e2);  // recordSequence 0
    subCtx.Commands().DestroyEntity(e2);  // recordSequence 1 (same context, monotonic)

    const auto& subKeys = subBuf.CommandKeys();
    ASSERT_EQ(subKeys.size(), 2u);
    EXPECT_EQ(subKeys[0].first.insertionOrder, 5u);
    EXPECT_EQ(subKeys[0].first.iterationIndex, K);
    EXPECT_EQ(subKeys[0].first.recordSequence, 0u);
    EXPECT_EQ(subKeys[1].first.insertionOrder, 5u);
    EXPECT_EQ(subKeys[1].first.iterationIndex, K);
    EXPECT_EQ(subKeys[1].first.recordSequence, 1u);

    EXPECT_EQ(subCtx.GetParallelBuffer(), nullptr);
}

// ---- Theme B2 Phase B, Task 3: ctx.ParallelForEach -- per-chunk sub- -------
// ---- context deferral. Each chunk of a view runs on a worker thread with ---
// ---- its own sub-context stamping a per-chunk (flat chunkWork) iteration ---
// ---- index, so the deferred structural changes each chunk records land -----
// ---- deterministically at the depth==0 flush. ------------------------------

namespace
{
    // A deferred-added marker so the test can count, after the flush, exactly
    // which entities the chunk-parallel bodies tagged. A distinct type means
    // reg.CreateView<Tag>() finds precisely the tagged entities.
    struct Tag
    {
        int v = 0;
    };
    static_assert(Astra::Component<Tag>, "Tag must satisfy Component concept");

    // A void(SystemContext&) system that fans its Position view out across
    // worker threads by chunk (ctx.ParallelForEach) and defers an
    // AddComponent<Tag> per entity into each chunk-worker's OWN sub-context.
    // Struct-typed only for readability; a context lambda would behave
    // identically (both get a solo execution group -> the outer system runs on
    // the submitting thread, so the inner view fans out to real workers).
    struct TagEveryEntityViaParallelForEach : Astra::SystemTraits<Astra::Writes<Position>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            auto view = ctx.GetRegistry().CreateView<Position>();
            ctx.ParallelForEach(view,
                [](Astra::Entity e, const Position&, Astra::SystemContext& sub)
                {
                    sub.Commands().AddComponent<Tag>(e, Tag{1});
                });
        }
    };
}

TEST(SystemContext, ParallelForEachRecordsDeferredChangesPerChunkThatApplyAtFlush)
{
    // The REGISTRY must carry the scheduler so the view's ParallelForEach
    // actually fans out across worker threads (View reads registry.m_workScheduler).
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();
    Astra::Registry::Config cfg;
    cfg.workScheduler = pool;
    Astra::Registry reg(cfg);

    // Pre-register Tag on the main thread: CommandBuffer::AddComponent<Tag>
    // registers Tag at RECORD time, and here many chunk-workers first-add Tag
    // concurrently. (Module 1 makes registration thread-safe; keep the
    // main-thread pre-registration pattern regardless.)
    reg.GetComponentRegistry()->RegisterComponent<Tag>();

    // Seed enough Position entities to cross every parallel threshold and span
    // many chunks (AVG_ENTITIES_PER_CHUNK=256, MIN_CHUNKS_FOR_PARALLEL=8,
    // MIN_ENTITIES_FOR_PARALLEL=1024): 10k entities is well above the 8-chunk
    // floor at the Position archetype's chunk capacity.
    constexpr size_t kCount = 10'000;
    std::vector<Astra::Entity> entities(kCount);
    reg.CreateEntities<Position>(kCount, entities);
    ASSERT_EQ(reg.CreateView<Position>().Size(), kCount);

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<TagEveryEntityViaParallelForEach>().IsOk());

    Astra::ParallelExecutor exec(pool);
    s.Execute(reg, &exec);

    // Every seeded entity now carries Tag: each chunk's per-entity deferred
    // AddComponent<Tag> was recorded into its chunk-worker's own buffer and
    // applied at the deterministic depth==0 flush. No crash, nothing left
    // pending.
    EXPECT_EQ(s.PendingCommandCount(), 0u);
    EXPECT_EQ(reg.CreateView<Tag>().Size(), kCount);
}

// ---- Theme B2 Phase B, Task 4: the chunk-parallel acceptance gate ---------
// ---- (spec Section 14's determinism contract extended to -----------------
// ---- ctx.ParallelForEach): a SINGLE system fans a MULTI-ARCHETYPE view ----
// ---- out across worker threads by chunk and defers a MIX of structural ---
// ---- changes per entity. The flush (Tasks 1-3's flat chunkWork -----------
// ---- iterationIndex + ExecuteSorted) must reproduce the IDENTICAL world --
// ---- across 50 runs under a real multi-threaded pool. ---------------------

namespace
{
    // Global entity counts for the multi-archetype seed below. All three
    // archetypes carry Position, so a single CreateView<Position>() spans
    // all of them -- this is what lets the gate catch a regression back to
    // stamping the PER-ARCHETYPE chunk index instead of the flat chunkWork
    // index `w` (Task 3): chunk 0 of {Position} and chunk 0 of {Position,
    // Velocity} would otherwise collide on an identical {insertionOrder,
    // iterationIndex} key, and the flush's stable-sort tiebreak (recordSequence,
    // which each sub-context restarts at 0) would become non-deterministic.
    // 3000 entities/archetype comfortably clears MIN_ENTITIES_FOR_PARALLEL
    // (1024) and MIN_CHUNKS_FOR_PARALLEL (8) with margin (~15 total chunks
    // at Position's ~1024- and Position+Velocity/Health's ~512-entity chunk
    // capacities -- View.hpp's chunk-size math), well above a single chunk.
    constexpr size_t kChunkGateNA = 3000;  // {Position} only
    constexpr size_t kChunkGateNB = 3000;  // {Position, Velocity}
    constexpr size_t kChunkGateNC = 3000;  // {Position, Health}
    constexpr size_t kChunkGateTotal = kChunkGateNA + kChunkGateNB + kChunkGateNC;
    static_assert(kChunkGateTotal >= 1024, "must clear View::MIN_ENTITIES_FOR_PARALLEL");

    // Every multiple-of-300 global index spawns one placeholder child (Mix
    // item 2 below); this count is used to predict the exact post-flush
    // entity total.
    constexpr size_t kChunkGateExpectedCreated = kChunkGateTotal / 300;
    // The three explicit overlapping pairs (Mix item 3) each destroy exactly
    // one entity.
    constexpr size_t kChunkGateExpectedDestroyed = 3;
}

TEST(SystemContext, ChunkParallelDeferredChangesFlushIdenticallyAcross50Runs)
{
    // One real multi-threaded pool, reused across every run: what's under
    // test is that the chunk-parallel FLUSH is deterministic despite
    // genuinely concurrent per-chunk recording, not that thread startup/
    // teardown is deterministic (same rationale as the Phase A gate above).
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();

    std::string snapshotRun0;

    for (int run = 0; run < 50; ++run)
    {
        // The REGISTRY must carry the scheduler so the inner view's
        // ParallelForEach actually fans out across worker threads (View
        // reads registry.m_workScheduler) -- unlike the Phase A gate above
        // (which tests SYSTEM-level parallelism via ParallelExecutor), this
        // gate's parallelism happens INSIDE one system, over a View.
        Astra::Registry::Config cfg;
        cfg.workScheduler = pool;
        Astra::Registry reg(cfg);

        // Pre-register every component type the system body will
        // AddComponent<T> on the main thread before Execute() -- many
        // chunk-workers will concurrently first-touch these types, and
        // CommandBuffer::AddComponent<T> registers T at record time (same
        // prerequisite as the Phase A gate above).
        reg.GetComponentRegistry()->RegisterComponent<Position>();
        reg.GetComponentRegistry()->RegisterComponent<Velocity>();
        reg.GetComponentRegistry()->RegisterComponent<Health>();
        reg.GetComponentRegistry()->RegisterComponent<Tag>();

        // Seed the rich, MULTI-ARCHETYPE world (see the constants' comment
        // above for why this shape matters). Position.x is set to a stable
        // globally-unique index (0..kChunkGateTotal-1, in creation order) so
        // the per-entity system body below can derive a deterministic
        // predicate from the component value alone.
        std::vector<Astra::Entity> archA(kChunkGateNA), archB(kChunkGateNB), archC(kChunkGateNC);
        reg.CreateEntitiesWith<Position>(kChunkGateNA, archA, [](size_t i)
        {
            return std::make_tuple(Position{float(i), 0.0f, 0.0f});
        });
        reg.CreateEntitiesWith<Position, Velocity>(kChunkGateNB, archB, [](size_t i)
        {
            return std::make_tuple(Position{float(kChunkGateNA + i), 0.0f, 0.0f}, Velocity{});
        });
        reg.CreateEntitiesWith<Position, Health>(kChunkGateNC, archC, [](size_t i)
        {
            return std::make_tuple(Position{float(kChunkGateNA + kChunkGateNB + i), 0.0f, 0.0f}, Health{100, 999});
        });
        ASSERT_EQ(reg.Size(), kChunkGateTotal) << "run " << run;

        // Fan-out precondition: hard-assert the premise BEFORE running the
        // system, mirroring the Phase A gate's "hard-assert the premise"
        // (line 657-663) -- a degenerate below-threshold seed would let this
        // gate pass by silently inlining, unable to catch the class of bug
        // it exists to catch. Sum GetChunkCount() over every Position-
        // bearing archetype (there are no others in this fresh registry).
        // View.hpp's MIN_CHUNKS_FOR_PARALLEL/MIN_ENTITIES_FOR_PARALLEL are
        // private, so the threshold (8 chunks) is reproduced here as a
        // literal, same as the Task 3 test's comment above.
        size_t totalChunks = 0;
        for (Astra::Archetype* a : reg.GetArchetypeManager()->GetArchetypes())
        {
            if (a->HasComponent<Position>())
                totalChunks += a->GetChunkCount();
        }
        ASSERT_GE(totalChunks, 8u) << "run " << run
            << ": fan-out precondition -- must clear MIN_CHUNKS_FOR_PARALLEL "
               "or this gate silently stops exercising the parallel path";

        // Special pairs: the FIRST two entities created in each archetype
        // are guaranteed to land in that archetype's chunk 0, at storage
        // positions 0 and 1 -- InvokeEntityCallback (View.hpp) walks a
        // chunk's entities in strict forward storage order, so the shared
        // per-chunk sub-context processes special0X strictly before
        // special1X, giving both entities' commands the SAME iterationIndex
        // and consecutive (monotonically increasing) recordSequence values.
        Astra::Entity special0A = archA[0], special1A = archA[1];
        Astra::Entity special0B = archB[0], special1B = archB[1];
        Astra::Entity special0C = archC[0], special1C = archC[1];

        Astra::SystemScheduler s;
        auto added = s.AddSystem(
            [special0A, special1A, special0B, special1B, special0C, special1C]
            (Astra::SystemContext& ctx)
            {
                auto view = ctx.GetRegistry().CreateView<Position>();
                ctx.ParallelForEach(view,
                    [special0A, special1A, special0B, special1B, special0C, special1C]
                    (Astra::Entity e, const Position& p, Astra::SystemContext& sub)
                    {
                        const int idx = static_cast<int>(p.x);

                        // Mix item 1: every entity tags itself
                        // (deterministic, unconditional).
                        sub.Commands().AddComponent<Tag>(e, Tag{idx});

                        // Mix item 2: a sparse deterministic subset spawns a
                        // placeholder related entity via CreateEntity() +
                        // AddComponent (exercises deterministic placeholder
                        // resolution under chunk-parallel recording).
                        if (idx % 300 == 0)
                        {
                            Astra::Entity child = sub.Commands().CreateEntity();
                            sub.Commands().AddComponent<Position>(child, Position{float(500000 + idx), 0.0f, 0.0f});
                        }

                        // Mix item 3: three explicit overlapping pairs (one
                        // per archetype) -- special0X destroys special1X.
                        // special1X's OWN unconditional self-tag (item 1
                        // above, recorded when special1X itself is later
                        // processed in the SAME chunk) therefore always
                        // targets an already-destroyed entity: a
                        // deterministic skip+report, mirroring DetSysC/D's
                        // pos[4]/vel[4] pattern above -- apply order (this
                        // destroy vs. that add) and the skip+report error
                        // channel are both directly observable.
                        if (e == special0A) sub.Commands().DestroyEntity(special1A);
                        if (e == special0B) sub.Commands().DestroyEntity(special1B);
                        if (e == special0C) sub.Commands().DestroyEntity(special1C);
                    });
            });
        ASSERT_TRUE(added.IsOk()) << "run " << run;

        Astra::ParallelExecutor exec(pool);
        s.Execute(reg, &exec);

        EXPECT_EQ(s.PendingCommandCount(), 0u) << "run " << run;

        // kChunkGateTotal initial - 3 destroyed (special1A/B/C) + 1 created
        // placeholder per multiple-of-300 global index = final live count.
        EXPECT_EQ(reg.Size(), kChunkGateTotal - kChunkGateExpectedDestroyed + kChunkGateExpectedCreated) << "run " << run;

        // Every surviving ORIGINAL entity (kChunkGateTotal - 3) was tagged;
        // the 3 destroyed originals' self-tag deterministically failed
        // (item 3), and the kChunkGateExpectedCreated placeholder children
        // are never tagged (only the outer view's original entities are).
        EXPECT_EQ(reg.CreateView<Tag>().Size(), kChunkGateTotal - kChunkGateExpectedDestroyed) << "run " << run;

        // Exactly 3 deterministic skip+report failures every run (special1A
        // via special0A, special1B via special0B, special1C via special0C).
        const auto& errors = s.GetLastDeferredErrors();
        EXPECT_EQ(errors.size(), kChunkGateExpectedDestroyed) << "run " << run;
        for (const auto& err : errors)
        {
            EXPECT_EQ(err.systemInsertionOrder, 0u) << "run " << run;
            EXPECT_EQ(err.reason, Astra::DeferredCommandError::Reason::InvalidTargetEntity) << "run " << run;
        }

        // ---- Canonical world snapshot ----
        // Every live entity (original or newly-created placeholder) carries
        // Position -- it is never removed from anyone in this gate -- so a
        // single CreateView<Position>() covers every live entity exactly
        // once, sorted by real id (independent of archetype/chunk iteration
        // order), same technique as the Phase A gate above.
        auto describe = [](Astra::Registry& r, Astra::Entity e) -> std::string
        {
            std::string out;
            if (auto* p = r.GetComponent<Position>(e))
                out += "P(" + std::to_string(p->x) + ")";
            if (auto* v = r.GetComponent<Velocity>(e))
                out += "V(" + std::to_string(v->dx) + ")";
            if (auto* h = r.GetComponent<Health>(e))
                out += "H(" + std::to_string(h->current) + ")";
            if (auto* t = r.GetComponent<Tag>(e))
                out += "T(" + std::to_string(t->v) + ")";
            return out;
        };

        std::map<Astra::Entity::StorageType, std::string> byId;
        reg.CreateView<Position>().ForEach([&](Astra::Entity e, Position&) { byId[e.GetValue()] = describe(reg, e); });
        ASSERT_EQ(byId.size(), reg.Size()) << "run " << run
            << ": every live entity must carry Position";

        std::string snapshot;
        for (const auto& [id, desc] : byId)
        {
            snapshot += std::to_string(id) + ":" + desc + ";";
        }
        // The deferred-command error list is itself gathered in globally
        // SortKey-sorted order (every key here is globally unique -- see
        // the special-pair comment above -- so there are no ties to break
        // non-deterministically), so appending it in-order is safe.
        for (const auto& err : errors)
        {
            snapshot += "ERR(" + std::to_string(err.systemInsertionOrder) + "," +
                        std::to_string(static_cast<int>(err.reason)) + ");";
        }

        if (run == 0)
        {
            snapshotRun0 = snapshot;
            // Guard against a degenerate always-equal comparison: run 0's
            // snapshot must actually contain the entities/values this test
            // was designed to produce.
            ASSERT_FALSE(snapshotRun0.empty());
        }
        else
        {
            EXPECT_EQ(snapshot, snapshotRun0)
                << "run " << run << ": chunk-parallel deferred-command flush "
                   "produced a DIFFERENT world state than run 0 -- a "
                   "determinism bug (chunkIndex stamping, sort-key ordering, "
                   "or placeholder resolution) survived Tasks 1-3. This test "
                   "must NOT be weakened; escalate instead.";
        }
    }
}

// ---- Theme B2 Phase B, Task 5 fix: iterationIndex BANDING ------------------
// ---- (whole-branch review, Important Issue 1). A deferred-command SortKey ---
// ---- is {insertionOrder, iterationIndex, recordSequence} and the depth==0 --
// ---- flush requires keys to be GLOBALLY UNIQUE (equal keys fall back to -----
// ---- scheduling-dependent buffer-slot order). Under ONE system (one --------
// ---- insertionOrder), the OUTER context's own Commands() uses iterationIndex-
// ---- 0, and BEFORE the fix each ParallelForEach stamped its chunks with the -
// ---- flat index w STARTING AT 0 -- so the outer Commands() and chunk w==0, --
// ---- AND any two ParallelForEach calls (the 2nd call's w also restarts at 0),
// ---- recorded COLLIDING keys. The fix bands iterationIndex: 0 is reserved ---
// ---- for the outer Commands(); each ParallelForEach call takes the next -----
// ---- disjoint band [base, base+chunkCount). This gate pins the fix. ---------

TEST(SystemContext, OuterCommandsPlusSequentialParallelForEachRecordDisjointBandsAcross30Runs)
{
    // One real multi-threaded pool, reused across every run (same rationale as
    // the two gates above): what's under test is that the FLUSH is
    // deterministic despite genuinely concurrent per-chunk recording, not
    // thread startup/teardown.
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>();

    std::string snapshotRun0;

    // Multi-archetype seed shape (mirrors the Task 4 gate): all three
    // archetypes carry Position, so ONE CreateView<Position>() spans them and
    // fans out to >= MIN_CHUNKS_FOR_PARALLEL (8) chunks. Position.x carries a
    // stable globally-unique index 0..N-1 (creation order) so each chunk body
    // derives its per-entity value from the component alone.
    constexpr size_t NA = 3000, NB = 3000, NC = 3000;
    constexpr size_t NTotal = NA + NB + NC;
    static_assert(NTotal >= 1024, "must clear View::MIN_ENTITIES_FOR_PARALLEL");

    for (int run = 0; run < 30; ++run)
    {
        // The REGISTRY must carry the scheduler so the inner views'
        // ParallelForEach actually fan out across worker threads.
        Astra::Registry::Config cfg;
        cfg.workScheduler = pool;
        Astra::Registry reg(cfg);

        // Pre-register every component the system body AddComponent<T>s, on the
        // main thread before Execute() (many chunk-workers first-touch these
        // concurrently, and AddComponent<T> registers T at record time -- same
        // prerequisite as the two gates above).
        reg.GetComponentRegistry()->RegisterComponent<Position>();
        reg.GetComponentRegistry()->RegisterComponent<Velocity>();
        reg.GetComponentRegistry()->RegisterComponent<Health>();

        std::vector<Astra::Entity> archA(NA), archB(NB), archC(NC);
        reg.CreateEntitiesWith<Position>(NA, archA, [](size_t i)
        {
            return std::make_tuple(Position{float(i), 0.0f, 0.0f});
        });
        reg.CreateEntitiesWith<Position, Velocity>(NB, archB, [](size_t i)
        {
            return std::make_tuple(Position{float(NA + i), 0.0f, 0.0f}, Velocity{});
        });
        reg.CreateEntitiesWith<Position, Health>(NC, archC, [](size_t i)
        {
            return std::make_tuple(Position{float(NA + NB + i), 0.0f, 0.0f}, Health{100, 999});
        });
        ASSERT_EQ(reg.Size(), NTotal) << "run " << run;

        // Fan-out precondition (mirrors the Task 4 gate): a below-threshold seed
        // would let this gate pass by silently inlining, unable to catch the
        // collision it exists to catch. MIN_CHUNKS_FOR_PARALLEL (8) reproduced
        // as a literal (View.hpp's threshold is private).
        size_t totalChunks = 0;
        for (Astra::Archetype* a : reg.GetArchetypeManager()->GetArchetypes())
        {
            if (a->HasComponent<Position>())
                totalChunks += a->GetChunkCount();
        }
        ASSERT_GE(totalChunks, 8u) << "run " << run
            << ": fan-out precondition -- must clear MIN_CHUNKS_FOR_PARALLEL";

        Astra::SystemScheduler s;
        auto added = s.AddSystem([](Astra::SystemContext& ctx)
        {
            // Scope 1 (iterationIndex band 0): the OUTER context creates a
            // placeholder carrying a unique, snapshot-visible Position value via
            // ctx.Commands(). Before the fix this recorded key (io, 0, 0);
            // chunk w==0 of the first ParallelForEach also recorded (io, 0, 0).
            Astra::Entity outerChild = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(outerChild, Position{900001.0f, 0.0f, 0.0f});

            // Scopes 2 and 3: two SEQUENTIAL ParallelForEach calls over the SAME
            // Position view (deferred creates don't mutate the view, so both see
            // the identical chunk layout). Each chunk body creates a placeholder
            // for a sparse deterministic subset, with a value unique to (call,
            // entity). Sparse (idx % 300) keeps the created-entity count modest
            // while still landing multiple creates in many chunks.
            //
            // WHY THIS IS THE REGRESSION DRIVER (and why it goes RED without the
            // fix while a bare outer-vs-chunk0 collision would NOT): the outer
            // context always registers buffer slot 0 (DispatchSystem calls
            // GetThreadBuffer() on the submitting thread before any worker), so
            // an outer-vs-chunk0 equal-key pair is gathered outer-first in EVERY
            // run -- stable, not flaky. The observable non-determinism is
            // worker-vs-worker: WITHOUT banding, call-2 chunk-w restarts its
            // iterationIndex at w and collides with call-1 chunk-w; both run on
            // scheduling-dependent worker buffers, so the colliding creates'
            // resolved real ids (hence the value->id map) vary run-to-run and
            // the by-real-id snapshot flakes. WITH banding: outer = band 0,
            // call1 = [1, 1+n), call2 = [1+n, 1+2n) -- all disjoint, so keys
            // stay globally unique and the flush is deterministic.
            auto view1 = ctx.GetRegistry().CreateView<Position>();
            ctx.ParallelForEach(view1,
                [](Astra::Entity, const Position& p, Astra::SystemContext& sub)
                {
                    const int idx = static_cast<int>(p.x);
                    if (idx % 300 == 0)
                    {
                        Astra::Entity child = sub.Commands().CreateEntity();
                        sub.Commands().AddComponent<Position>(child, Position{float(700000 + idx), 0.0f, 0.0f});
                    }
                });
            auto view2 = ctx.GetRegistry().CreateView<Position>();
            ctx.ParallelForEach(view2,
                [](Astra::Entity, const Position& p, Astra::SystemContext& sub)
                {
                    const int idx = static_cast<int>(p.x);
                    if (idx % 300 == 0)
                    {
                        Astra::Entity child = sub.Commands().CreateEntity();
                        sub.Commands().AddComponent<Position>(child, Position{float(800000 + idx), 0.0f, 0.0f});
                    }
                });
        });
        ASSERT_TRUE(added.IsOk()) << "run " << run;

        Astra::ParallelExecutor exec(pool);
        s.Execute(reg, &exec);

        EXPECT_EQ(s.PendingCommandCount(), 0u) << "run " << run;

        // Canonical world snapshot: every live entity (original or created
        // placeholder) carries Position, so a single CreateView<Position>()
        // covers each exactly once, sorted by real id (independent of
        // archetype/chunk iteration order). The created placeholders' real ids
        // are assigned in sorted-flush order, so if the colliding creates above
        // resolved in a scheduling-dependent order, the value->id map -- and
        // thus this snapshot string -- differs from run 0.
        std::map<Astra::Entity::StorageType, std::string> byId;
        reg.CreateView<Position>().ForEach([&](Astra::Entity e, Position& p)
        {
            byId[e.GetValue()] = "P(" + std::to_string(p.x) + ")";
        });
        ASSERT_EQ(byId.size(), reg.Size()) << "run " << run
            << ": every live entity must carry Position";

        std::string snapshot;
        for (const auto& [id, desc] : byId)
        {
            snapshot += std::to_string(id) + ":" + desc + ";";
        }

        if (run == 0)
        {
            snapshotRun0 = snapshot;
            // Guard against a degenerate always-equal comparison: run 0 must
            // actually contain the created placeholders this test produces.
            ASSERT_FALSE(snapshotRun0.empty());
        }
        else
        {
            EXPECT_EQ(snapshot, snapshotRun0)
                << "run " << run << ": deferred-command flush produced a "
                   "DIFFERENT world state than run 0 -- an iterationIndex band "
                   "collision (outer Commands() vs a ParallelForEach chunk, or "
                   "two ParallelForEach calls' overlapping bands) made colliding "
                   "SortKeys resolve non-deterministically. This test must NOT "
                   "be weakened; escalate instead.";
        }
    }
}
