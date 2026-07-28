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
