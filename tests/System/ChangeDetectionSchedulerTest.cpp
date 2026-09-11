#include <atomic>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;
using Astra::Test::Velocity;

// Stage 3 task 5: SystemContext::LastRun()/ThisRun(), the per-GROUP tick advance
// (BeginSystemGroup, plan deviation 1), the once-per-segment advance before the
// deferred flush (controller ruling G), and the scheduler's preference for an
// operator()(SystemContext&) overload. Position/Velocity are NOT change-tracked
// here, so every Changed/Added term below is the coarse chunk-version tier.

namespace
{
    // Writer: non-const view over Position (stamps every chunk it visits).
    struct WritePos : Astra::SystemTraits<Astra::Writes<Position>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.GetRegistry().CreateView<Position>().ForEach([](Position& p) { p.x += 1.0f; });
        }
    };

    // Reader: counts entities whose Position changed since this system's last run.
    struct ReadChanged : Astra::SystemTraits<Astra::Reads<Position>>
    {
        static inline size_t s_seen = 0;
        static inline std::vector<std::pair<Tick, Tick>> s_ticks;   // (LastRun, ThisRun) per run
        void operator()(Astra::SystemContext& ctx)
        {
            s_ticks.emplace_back(ctx.LastRun(), ctx.ThisRun());
            auto v = ctx.GetRegistry().CreateView<const Position, Astra::Changed<Position>>();
            s_seen = 0;
            v.ForEach(ctx, [](const Position&) { ++s_seen; });
        }
    };

    // A type offering BOTH signatures: the scheduler must prefer SystemContext&.
    struct Both
    {
        static inline int s_registryCalls = 0;
        static inline int s_contextCalls = 0;
        void operator()(Astra::Registry&) { ++s_registryCalls; }
        void operator()(Astra::SystemContext& ctx) { ++s_contextCalls; EXPECT_GT(ctx.ThisRun(), 0u); }
    };

    // Three ThisRun() probes for the parallel-group test: WA and WB have disjoint
    // writes (one group); WA2 conflicts with WA (next group). File-scope because a
    // local class cannot have static data members.
    struct WA : Astra::SystemTraits<Astra::Writes<Position>>
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };
    struct WB : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };
    struct WA2 : Astra::SystemTraits<Astra::Writes<Position>>   // conflicts with WA -> next group
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };

    // Change-filtered view fanned out through SystemContext::ParallelForEach:
    // the context must hand the view its LastRun() as the "since" tick.
    struct Reader : Astra::SystemTraits<Astra::Reads<Position>>
    {
        static inline std::atomic<size_t> s_seen{0};
        void operator()(Astra::SystemContext& ctx)
        {
            auto v = ctx.GetRegistry().CreateView<const Position, Astra::Changed<Position>>();
            s_seen.store(0);
            ctx.ParallelForEach(v, [](Astra::Entity, const Position&, Astra::SystemContext&) { s_seen.fetch_add(1); });
        }
    };

    // Ruling G: a system in the LAST group defers a structural change (frame 1
    // only); a reader sharing that group must see it on frame 2.
    struct DeferAddVelocity : Astra::SystemTraits<Astra::Reads<Position>>
    {
        static inline bool s_deferred = false;
        static inline Astra::Entity s_target{};
        void operator()(Astra::SystemContext& ctx)
        {
            if (s_deferred) return;
            s_deferred = true;
            ctx.Commands().AddComponent(s_target, Velocity{});
        }
    };
    struct ReadAddedVelocity : Astra::SystemTraits<Astra::Reads<Velocity>>
    {
        static inline size_t s_seen = 0;
        void operator()(Astra::SystemContext& ctx)
        {
            auto v = ctx.GetRegistry().CreateView<const Velocity, Astra::Added<Velocity>>();
            s_seen = 0;
            v.ForEach(ctx, [](const Velocity&) { ++s_seen; });
        }
    };
}

TEST(ChangeDetectionScheduler, StandaloneContextHasLastRunZeroAndThisRunCurrentTick)
{
    Astra::Registry reg;
    reg.AdvanceTick(); reg.AdvanceTick();   // 3
    Astra::CommandBuffer cmds(&reg);
    Astra::SystemContext ctx(reg, cmds, 0u);
    EXPECT_EQ(ctx.LastRun(), 0u);
    EXPECT_EQ(ctx.ThisRun(), reg.CurrentTick());
}

TEST(ChangeDetectionScheduler, WriterThenReaderInOneFrameSeesWritesAndNotItsOwnNextFrame)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(3000);
    ASSERT_EQ((reg.CreateEntities<Position, Velocity>(3000, std::span{ents})), 3000u);

    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WritePos>().IsOk());      // writes Position -> group 0
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());   // reads Position  -> group 1 (conflict)
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);

    const Tick before = reg.CurrentTick();
    s.Execute(reg);                                   // frame 1
    EXPECT_EQ(ReadChanged::s_seen, 3000u);            // first run: LastRun 0 sees everything
    ASSERT_EQ(ReadChanged::s_ticks.size(), 1u);
    EXPECT_EQ(ReadChanged::s_ticks[0].first, 0u);
    EXPECT_GT(ReadChanged::s_ticks[0].second, before);
    EXPECT_EQ(reg.CurrentTick(), before + 3);   // one advance per GROUP (deviation 1) + one after the segment, before the deferred flush (Ruling G)

    s.Execute(reg);                                   // frame 2: writer ran again in group 0 -> reader sees all again
    EXPECT_EQ(ReadChanged::s_seen, 3000u);
    EXPECT_EQ(ReadChanged::s_ticks[1].first, ReadChanged::s_ticks[0].second);   // LastRun == previous ThisRun

    // Remove the writer: the reader must now see NOTHING (its own previous run wrote nothing).
    s.RemoveSystem<WritePos>();                       // RemoveSystem returns void
    ASSERT_FALSE(s.HasSystem<WritePos>());
    s.Execute(reg);                                   // plan rebuilt -> lastRun reset to 0 (documented): sees everything once
    EXPECT_EQ(ReadChanged::s_seen, 3000u);
    s.Execute(reg);                                   // and then nothing
    EXPECT_EQ(ReadChanged::s_seen, 0u);
}

TEST(ChangeDetectionScheduler, ReaderBeforeWriterInFrameSeesPreviousFramesWrites)
{
    // Ordering: reader in group 0, writer in group 1 (Before edge). Frame N's writes
    // (stamped with group 1's tick) must be visible to frame N+1's reader, whose
    // LastRun is frame N's group-0 tick -- older than group 1's stamp.
    struct WriteAfter : Astra::SystemTraits<Astra::Writes<Position>, Astra::After<ReadChanged>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.GetRegistry().CreateView<Position>().ForEach([](Position& p) { p.x += 1.0f; });
        }
    };
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ(reg.CreateEntities<Position>(500, std::span{ents}), 500u);
    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());
    ASSERT_TRUE(s.AddSystem<WriteAfter>().IsOk());
    s.Execute(reg);                       // reader (sees all: first run), then writer
    s.Execute(reg);                       // reader must see the previous frame's writes
    EXPECT_EQ(ReadChanged::s_seen, 500u);
}

TEST(ChangeDetectionScheduler, ParallelGroupSharesOneTickAndGroupsAdvance)
{
    WA::s_tick.store(0); WB::s_tick.store(0); WA2::s_tick.store(0);
    Astra::Registry reg;
    (void)reg.CreateEntity<Position, Velocity>();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WA>().IsOk());
    ASSERT_TRUE(s.AddSystem<WB>().IsOk());
    ASSERT_TRUE(s.AddSystem<WA2>().IsOk());
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);

    Astra::ParallelExecutor exec(std::make_shared<Astra::Testing::TestWorkerPool>());
    s.Execute(reg, &exec);
    EXPECT_EQ(WA::s_tick.load(), WB::s_tick.load());                 // same group, same tick
    EXPECT_TRUE(Astra::IsNewer(WA2::s_tick.load(), WA::s_tick.load()));   // next group is newer
}

TEST(ChangeDetectionScheduler, SystemContextOverloadPreferredWhenBothExist)
{
    Both::s_contextCalls = 0; Both::s_registryCalls = 0;
    Astra::Registry reg;
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<Both>().IsOk());
    s.Execute(reg);
    EXPECT_EQ(Both::s_contextCalls, 1);
    EXPECT_EQ(Both::s_registryCalls, 0);
}

TEST(ChangeDetectionScheduler, RegistrySwitchResetsLastRun)
{
    Astra::Registry a, b;
    (void)a.CreateEntity<Position>();
    (void)b.CreateEntity<Position>();
    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());
    s.Execute(a);
    s.Execute(a);
    EXPECT_NE(ReadChanged::s_ticks.back().first, 0u);
    s.Execute(b);                                        // ticks of `a` mean nothing in `b`
    EXPECT_EQ(ReadChanged::s_ticks.back().first, 0u);    // reset: sees everything once
    EXPECT_EQ(ReadChanged::s_seen, 1u);
}

TEST(ChangeDetectionScheduler, ContextParallelForEachPassesLastRunToChangeFilteredViews)
{
    Reader::s_seen.store(0);
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<Position>(2000, std::span{ents}), 2000u);
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<Reader>().IsOk());
    s.Execute(reg);
    EXPECT_EQ(Reader::s_seen.load(), 2000u);   // first run sees everything
    s.Execute(reg);
    EXPECT_EQ(Reader::s_seen.load(), 0u);      // nothing changed since
}

// ---- Ruling G: the scheduler advances once more per segment, AFTER its groups
// ---- ran and BEFORE the deferred flush, so flushed commands and any main-thread
// ---- write made after Execute() returns are newer than every system's lastRun.

TEST(ChangeDetectionScheduler, DeferredCommandsFlushedAtSegmentEndAreSeenByLastGroupReaderNextFrame)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(4);
    ASSERT_EQ(reg.CreateEntities<Position>(4, std::span{ents}), 4u);   // Position only: nobody has Velocity yet

    DeferAddVelocity::s_deferred = false;
    DeferAddVelocity::s_target = ents[1];
    ReadAddedVelocity::s_seen = 0;

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<DeferAddVelocity>().IsOk());   // Reads<Position>
    ASSERT_TRUE(s.AddSystem<ReadAddedVelocity>().IsOk());  // Reads<Velocity>: no conflict -> SAME group, same tick
    ASSERT_EQ(s.GetExecutionPlan().size(), 1u);            // both in the (last) group -- that is the point
    ASSERT_EQ(s.GetExecutionPlan()[0].size(), 2u);

    s.Execute(reg);                                  // frame 1: the add is deferred, flushed after the group ran
    EXPECT_EQ(ReadAddedVelocity::s_seen, 0u);        // nothing had Velocity when the reader ran
    ASSERT_TRUE(reg.HasComponent<Velocity>(ents[1]));   // the flush applied it

    s.Execute(reg);                                  // frame 2: the flushed add must be newer than the reader's lastRun
    EXPECT_EQ(ReadAddedVelocity::s_seen, 1u);

    s.Execute(reg);                                  // frame 3: nothing new
    EXPECT_EQ(ReadAddedVelocity::s_seen, 0u);
}

TEST(ChangeDetectionScheduler, MainThreadWriteAfterExecuteIsSeenByLastGroupReaderNextFrame)
{
    // Position is coarse-tracked (chunk granularity), so the two entities live in
    // two archetypes: Modified<Position> on one stamps only that chunk, making
    // "exactly 1" meaningful while "sees all" is 2.
    Astra::Registry reg;
    const Astra::Entity e0 = reg.CreateEntity<Position>();
    const Astra::Entity e1 = reg.CreateEntity<Position, Velocity>();
    ASSERT_TRUE(reg.IsValid(e0));
    ASSERT_TRUE(reg.IsValid(e1));

    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());   // the only system: last group by construction

    s.Execute(reg);                                   // frame 1: first run sees all
    EXPECT_EQ(ReadChanged::s_seen, 2u);

    ASSERT_TRUE(reg.Modified<Position>(e0));          // main-thread write AFTER Execute() returned

    s.Execute(reg);                                   // frame 2: that write must be newer than the reader's lastRun
    EXPECT_EQ(ReadChanged::s_seen, 1u);

    s.Execute(reg);                                   // frame 3: nothing new
    EXPECT_EQ(ReadChanged::s_seen, 0u);
}
