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

TEST(SignalContract, RemoveDisabledNeverEmitsAndReturnsMatchTheTable)
{
    Astra::Registry reg;   // signals disabled by default
    int calls = 0;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++calls; });

    auto e = reg.CreateEntity<Tracked>();
    EXPECT_TRUE(reg.RemoveComponent<Tracked>(e));    // present -> removed
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(e));   // now absent -> false

    auto dead = reg.CreateEntity<Tracked>();
    reg.DestroyEntity(dead);
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(dead)); // stale handle -> false

    EXPECT_EQ(calls, 0);   // disabled: handler registered but nothing may fire
}

TEST(SignalContract, RemoveEnabledAbsentAndStaleEmitNothing)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    int calls = 0;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++calls; });

    auto noComp = reg.CreateEntity();                    // no Tracked on it
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(noComp));  // absent -> false, no emit

    auto dead = reg.CreateEntity<Tracked>();
    reg.DestroyEntity(dead);
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(dead));    // stale -> false, no emit

    EXPECT_EQ(calls, 0);
}

TEST(SignalContract, AddEmitsOnlyWhenEnabledWithTheNewPointer)
{
    {
        Astra::Registry reg;
        reg.EnableSignals(Astra::Signal::ComponentAdded);
        const void* seen = nullptr;
        int seenValue = 0;
        reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
            [&](const Astra::Events::ComponentAdded& ev)
            {
                seen = ev.component;
                seenValue = static_cast<const Tracked*>(ev.component)->value;
            });

        auto e = reg.CreateEntity();
        reg.AddComponent<Tracked>(e, Tracked{7});
        ASSERT_NE(seen, nullptr);
        EXPECT_EQ(seenValue, 7);                              // payload live at emit time
        EXPECT_EQ(seen, reg.GetComponent<Tracked>(e));        // emitted ptr == the new component
    }
    {
        Astra::Registry reg;   // disabled
        int calls = 0;
        reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
            [&](const Astra::Events::ComponentAdded&) { ++calls; });
        auto e = reg.CreateEntity();
        reg.AddComponent<Tracked>(e, Tracked{7});
        EXPECT_NE(reg.GetComponent<Tracked>(e), nullptr);     // added regardless
        EXPECT_EQ(calls, 0);                                  // but no emission
    }
}
