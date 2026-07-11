#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct SPos { float x, y, z; }; }

TEST(SignalLifetime, ComponentRemovedSeesLiveValue)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);

    auto e = reg.CreateEntity<SPos>();
    reg.GetComponent<SPos>(e)->x = 42.0f;

    float observed = -1.0f;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev)
        {
            observed = static_cast<const SPos*>(ev.component)->x;
        });

    ASSERT_TRUE(reg.RemoveComponent<SPos>(e));
    EXPECT_EQ(observed, 42.0f);   // pointer must be alive at emit time
}
