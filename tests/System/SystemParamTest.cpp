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
