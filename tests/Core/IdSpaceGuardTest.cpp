#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct IdCompA { int v; }; struct IdCompB { int v; }; }

TEST(IdSpaceGuard, SystemsDoNotConsumeComponentIds)
{
    // Force IdCompA's ID to be assigned now.
    Astra::ComponentID before = Astra::TypeID<IdCompA>::Value();

    Astra::SystemScheduler scheduler;
    (void)scheduler.AddSystem([](Astra::Entity, IdCompA& a) { a.v++; });   // lambda wrapper type
    // A second scheduler-side type for good measure:
    (void)scheduler.AddSystem([](Astra::Entity, const IdCompA& a) { (void)a; });

    // If systems consumed ComponentIDs, IdCompB would now be before+3 or more.
    Astra::ComponentID after = Astra::TypeID<IdCompB>::Value();
    EXPECT_EQ(after, static_cast<Astra::ComponentID>(before + 1));
}
