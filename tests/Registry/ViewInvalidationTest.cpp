#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct VPos { float x, y, z; }; struct VVel { float dx, dy, dz; }; }

TEST(ViewInvalidation, SeesEntityAddedToPreexistingEmptyArchetype)
{
    Astra::Registry reg;
    auto e1 = reg.CreateEntity<VPos>();   // creates archetype [VPos]
    reg.DestroyEntity(e1);                // archetype now empty but alive

    auto view = reg.CreateView<VPos>();   // collected while archetype is empty

    reg.CreateEntity<VPos>();             // reuses existing archetype: no new-archetype event

    size_t seen = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++seen; });
    EXPECT_EQ(seen, 1u);                  // was 0 before the fix

    // Size()/Empty() must agree with iteration
    EXPECT_EQ(view.Size(), 1u);
    EXPECT_FALSE(view.Empty());
}

TEST(ViewInvalidation, SurvivesArchetypeRemoval)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<VPos>();
    auto b = reg.CreateEntity<VPos, VVel>();   // second archetype so removal has candidates
    auto view = reg.CreateView<VPos>();
    size_t first = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++first; });
    EXPECT_EQ(first, 2u);

    reg.DestroyEntity(a);
    reg.DestroyEntity(b);

    Astra::Registry::DefragmentationOptions opts;
    opts.minArchetypesToKeep = 1;              // allow removing the emptied archetypes
    reg.Defragment(opts);

    size_t after = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++after; });  // must not touch freed archetypes
    EXPECT_EQ(after, 0u);
}

TEST(ViewInvalidation, SizeIsSafeAndCorrectAfterDefragment)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<VPos>();
    auto b = reg.CreateEntity<VPos, VVel>();
    auto view = reg.CreateView<VPos>();
    EXPECT_EQ(view.Size(), 2u);

    reg.DestroyEntity(a);
    reg.DestroyEntity(b);
    Astra::Registry::DefragmentationOptions opts;
    opts.minArchetypesToKeep = 1;
    reg.Defragment(opts);

    // Before the fix: Size() dereferences a freed Archetype* (UAF).
    EXPECT_EQ(view.Size(), 0u);
    EXPECT_TRUE(view.Empty());
}
