#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
    using Astra::Test::Physics;
}

// ---- Task 1: the Res/ResMut handle types wrap a pointer and deref it. -------

TEST(SystemParam, ResAndResMutWrapAndDereferencePointer)
{
    Health h{7, 42};
    Astra::Res<Health>    r{&h};
    Astra::ResMut<Health> m{&h};

    EXPECT_EQ(r->current, 7);
    EXPECT_EQ((*r).max,   42);

    m->current = 9;            // ResMut is mutable
    EXPECT_EQ(h.current, 9);
    EXPECT_EQ(r.Get().current, 9);
}

// ---- Task 2: per-parameter access classification (component reads/writes -----
// ---- from View const-ness; Res->resReads, ResMut->resWrites). ----------------

TEST(SystemParam, ParamAccessClassifiesComponentsAndResources)
{
    using VRef = Astra::View<Position, const Velocity>&;
    using PA_V = Astra::Detail::ParamAccess<VRef>;
    EXPECT_TRUE((std::is_same_v<PA_V::Writes, std::tuple<Position>>));   // Position (non-const) = write
    EXPECT_TRUE((std::is_same_v<PA_V::Reads,  std::tuple<Velocity>>));   // const Velocity = read
    EXPECT_TRUE((std::is_same_v<PA_V::ResReads,  std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_V::ResWrites, std::tuple<>>));

    using PA_R = Astra::Detail::ParamAccess<Astra::Res<Health>>;
    EXPECT_TRUE((std::is_same_v<PA_R::ResReads,  std::tuple<Health>>));
    EXPECT_TRUE((std::is_same_v<PA_R::ResWrites, std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_R::Reads,     std::tuple<>>));

    using PA_M = Astra::Detail::ParamAccess<Astra::ResMut<Physics>>;
    EXPECT_TRUE((std::is_same_v<PA_M::ResWrites, std::tuple<Physics>>));
    EXPECT_TRUE((std::is_same_v<PA_M::ResReads,  std::tuple<>>));

    using PA_C = Astra::Detail::ParamAccess<Astra::Commands>;
    EXPECT_TRUE((std::is_same_v<PA_C::Reads,     std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_C::ResWrites,  std::tuple<>>));

    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<VRef>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::Res<Health>>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::ResMut<Physics>>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::Commands>));
    EXPECT_FALSE((Astra::Detail::IsSystemParam_v<Position>));            // bare component: NOT a param
    EXPECT_FALSE((Astra::Detail::IsSystemParam_v<Astra::View<Position>>)); // by-value View: NOT a param (must be &)
}

// ---- Task 3: SystemParamBinder unions each parameter's access into the -------
// ---- typedefs ExtractSystemTraits harvests. ----------------------------------

TEST(SystemParam, BinderHarvestsUnionedAccess)
{
    using Binder = Astra::SystemParamBinder<
        Astra::View<Position, const Velocity>&,   // write Position, read Velocity
        Astra::Res<Health>,                        // read resource Health
        Astra::ResMut<Physics>,                    // write resource Physics
        Astra::Commands>;                          // nothing

    EXPECT_TRUE((std::is_same_v<Binder::WritesComponents,    std::tuple<Position>>));
    EXPECT_TRUE((std::is_same_v<Binder::ReadsComponents,     std::tuple<Velocity>>));
    EXPECT_TRUE((std::is_same_v<Binder::ReadsResourceTypes,  std::tuple<Health>>));
    EXPECT_TRUE((std::is_same_v<Binder::WritesResourceTypes, std::tuple<Physics>>));
    EXPECT_TRUE(Binder::HasTraits);
    EXPECT_FALSE(Binder::RequiresExclusive);

    // The binder must satisfy the trait-detection gate the scheduler uses.
    EXPECT_TRUE((Astra::HasSystemTraits_v<Binder>));
}

// ---- Task 4: a wrapper builds each parameter from a SystemContext, iterates ---
// ---- the cached view, records Commands, and skips-and-logs a missing resource.

namespace
{
    // A free function param-system used by several tests (Task 4 & Task 6).
    inline void AddOneToPositions(Astra::View<Position>& v)
    {
        v.ForEach([](Position& p) { p.x += 1.0f; });
    }
}

TEST(SystemParam, WrapperRunsViewParamAndCachesView)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 10.0f;

    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    Astra::FunctionSystemWrapper<decltype(AddOneToPositions)*, Astra::View<Position>&>
        wrapper{&AddOneToPositions};

    wrapper(ctx);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 11.0f);
    wrapper(ctx);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 12.0f);   // second run reuses cached view
}

TEST(SystemParam, WrapperRunsResourceAndCommandsParams)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.SetResource(Health{5, 5});

    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    int seen = -1;
    auto body = [&](Astra::Res<Health> h, Astra::Commands c)
    {
        seen = h->current;              // reads the resource
        c->DestroyEntity(e);            // records a deferred command
    };
    Astra::FunctionSystemWrapper<decltype(body), Astra::Res<Health>, Astra::Commands>
        wrapper{body};

    wrapper(ctx);
    EXPECT_EQ(seen, 5);
    EXPECT_EQ(cmds.GetCommandCount(), 1u);    // Commands recorded into the buffer
}

TEST(SystemParam, WrapperSkipsWhenResourceAbsent)
{
    Astra::Registry reg;                 // no Health resource set
    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    bool ran = false;
    auto body = [&](Astra::ResMut<Health> h) { (void)h; ran = true; };
    Astra::FunctionSystemWrapper<decltype(body), Astra::ResMut<Health>> wrapper{body};

    wrapper(ctx);
    EXPECT_FALSE(ran);                   // body never entered (skip-and-log)

    reg.SetResource(Health{1, 1});       // now present
    wrapper(ctx);
    EXPECT_TRUE(ran);                    // resumes once the resource exists
}

// ---- Task 5: register param-systems on the scheduler; masks flow through ------
// ---- ExtractSystemTraits; both spellings run under Execute(). -----------------

TEST(SystemParam, LambdaParamSystemRegistersRunsAndHarvestsMasks)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;
    reg.SetResource(Health{3, 3});

    Astra::SystemScheduler s;
    auto added = s.AddSystem([](Astra::View<Position>& v, Astra::Res<Health> h)
    {
        v.ForEach([&](Position& p) { p.x += static_cast<float>(h->current); });
    });
    ASSERT_TRUE(added.IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 3.0f);   // body ran with the resource
}

TEST(SystemParam, FreeFunctionParamSystemRegistersViaNTTPAndRuns)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    auto added = s.AddSystem<AddOneToPositions>();       // free fn as template arg
    ASSERT_TRUE(added.IsOk());
    EXPECT_TRUE(s.HasSystem<AddOneToPositions>());       // symmetric Has<>

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 1.0f);
}

// ---- Task 6: derived access drives grouping; disambiguation intact; free-fn ---
// ---- NTTP collision-free; multi-view / no-view / skip-under-scheduler. --------

namespace
{
    inline void MovePlayers(Astra::View<Position>& v) { v.ForEach([](Position& p){ p.x += 1.0f; }); }
    inline void MoveEnemies(Astra::View<Position>& v) { v.ForEach([](Position& p){ p.x += 1.0f; }); }  // same signature as MovePlayers
    inline void ReadsHealthRes(Astra::Res<Health>)        {}   // resource reader
    inline void WritesHealthRes(Astra::ResMut<Health>)    {}   // resource writer (conflicts with reader)
}

TEST(SystemParam, DisjointParamSystemsShareAGroupConflictingOnesDoNot)
{
    // Two systems touching different resources -> same parallel group.
    Astra::SystemScheduler s1;
    ASSERT_TRUE(s1.AddSystem<ReadsHealthRes>().IsOk());
    ASSERT_TRUE(s1.AddSystem([](Astra::Res<Physics>){}).IsOk());   // different resource
    const auto& plan1 = s1.GetExecutionPlan();
    ASSERT_EQ(plan1.size(), 1u);
    EXPECT_EQ(plan1[0].size(), 2u);                                 // grouped together

    // A resource reader + writer of the SAME resource -> serialized (2 groups).
    Astra::SystemScheduler s2;
    ASSERT_TRUE(s2.AddSystem<ReadsHealthRes>().IsOk());
    ASSERT_TRUE(s2.AddSystem<WritesHealthRes>().IsOk());
    const auto& plan2 = s2.GetExecutionPlan();
    EXPECT_EQ(plan2.size(), 2u);                                    // separate groups
}

TEST(SystemParam, TwoSameSignatureFreeFunctionsBothRegisterAndRun)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<MovePlayers>().IsOk());
    ASSERT_TRUE(s.AddSystem<MoveEnemies>().IsOk());   // MUST NOT be AlreadyRegistered
    EXPECT_EQ(s.Size(), 2u);

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 2.0f);   // both ran

    s.RemoveSystem<MovePlayers>();
    EXPECT_FALSE(s.HasSystem<MovePlayers>());
    EXPECT_TRUE(s.HasSystem<MoveEnemies>());
}

TEST(SystemParam, SiblingSameSignatureLambdasAreDistinctSystems)
{
    // Every lambda expression is its own closure type, so two sibling lambdas
    // with the same signature are two systems on every compiler. GCC's
    // pretty-name prints every closure in a function as `fn()::<lambda(Args)>`
    // (no per-lambda numbering, unlike MSVC's <lambda_N> and Clang's
    // `(lambda at file:line:col)`), so a NAME hash cannot tell them apart --
    // lambda systems are keyed by a per-type anchor instead. All three lambda
    // entry points (context / view / param) are covered.
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;
    int ctxRuns = 0;

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem([&](Astra::SystemContext&){ ctxRuns += 1; }).IsOk());                                     // context lambda
    ASSERT_TRUE(s.AddSystem([&](Astra::SystemContext&){ ctxRuns += 10; }).IsOk());                                    //   ...same signature
    ASSERT_TRUE(s.AddSystem([](Astra::Entity, Position& p){ p.x += 1.0f; }).IsOk());                                 // view lambda
    ASSERT_TRUE(s.AddSystem([](Astra::Entity, Position& p){ p.x += 10.0f; }).IsOk());                                //   ...same signature
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& v){ v.ForEach([](Position& p){ p.x += 100.0f; }); }).IsOk());  // param lambda
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& v){ v.ForEach([](Position& p){ p.x += 1000.0f; }); }).IsOk()); //   ...same signature
    EXPECT_EQ(s.Size(), 6u);

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(ctxRuns, 11);                                          // both context lambdas ran once
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 1111.0f);     // all four view/param lambdas ran once

    // The SAME closure type is still one system: re-registering it is refused.
    auto same = [](Astra::SystemContext&) {};
    ASSERT_TRUE(s.AddSystem(same).IsOk());
    auto dup = s.AddSystem(same);
    ASSERT_TRUE(dup.IsErr());
    EXPECT_EQ(*dup.GetError(), Astra::SystemError::AlreadyRegistered);
    EXPECT_EQ(s.Size(), 7u);
}

TEST(SystemParam, DisambiguationViewLambdaAndContextLambdaStillRoute)
{
    // A view-lambda, a context lambda, and a param-system coexist and each runs.
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    int viewRuns = 0, ctxRuns = 0;
    ASSERT_TRUE(s.AddSystem([&](Astra::Entity, Position& p){ p.x += 1.0f; ++viewRuns; }).IsOk());   // view-lambda
    ASSERT_TRUE(s.AddSystem([&](Astra::SystemContext&){ ++ctxRuns; }).IsOk());                       // context lambda
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& v){ v.ForEach([](Position& p){ p.x += 10.0f; }); }).IsOk()); // param-system

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(viewRuns, 1);
    EXPECT_EQ(ctxRuns, 1);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 11.0f);   // view-lambda +1, param-system +10
}

TEST(SystemParam, MultiViewAndNoViewSystemsCompileAndRun)
{
    Astra::Registry reg;
    Astra::Entity a = reg.CreateEntity<Position>();
    Astra::Entity b = reg.CreateEntity<Velocity>();
    reg.SetResource(Health{0, 0});

    Astra::SystemScheduler s;
    // Two views in one system.
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& vp, Astra::View<Velocity>& vv)
    {
        vp.ForEach([](Position& p){ p.x += 1.0f; });
        vv.ForEach([](Velocity& v){ v.dx += 1.0f; });
    }).IsOk());
    // No view at all: pure resource + commands.
    ASSERT_TRUE(s.AddSystem([](Astra::ResMut<Health> h, Astra::Commands){ h->current += 5; }).IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(a)->x, 1.0f);
    EXPECT_FLOAT_EQ(reg.GetComponent<Velocity>(b)->dx, 1.0f);
    EXPECT_EQ(reg.GetResource<Health>()->current, 5);
}
