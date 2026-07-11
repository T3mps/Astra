#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace
{
    // Destructor writes a sentinel so a ComponentRemoved handler that runs
    // AFTER the component is destroyed would observably read -999 instead of
    // the live value. This makes the test discriminate emit-before-removal
    // (correct) from emit-after-removal (the bug Task 8 fixed).
    struct Tracked
    {
        int value = 0;
        ~Tracked() { value = -999; }

        // Serialization support (needed because of user-defined destructor,
        // which makes this type non-trivially-copyable).
        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(value);
        }
    };
}

TEST(SignalLifetime, ComponentRemovedSeesLiveValue)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);

    auto e = reg.CreateEntity<Tracked>();
    reg.GetComponent<Tracked>(e)->value = 42;

    int observed = 0;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev)
        {
            observed = static_cast<const Tracked*>(ev.component)->value;
        });

    ASSERT_TRUE(reg.RemoveComponent<Tracked>(e));
    EXPECT_EQ(observed, 42);   // live value at emit time; would be -999 if emitted after destruction
}
