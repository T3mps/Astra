#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

// MulticastDelegate::Invoke must dispatch over a stable snapshot of its
// handlers so that a handler which registers/unregisters during dispatch
// (e.g. a Signal listener removing itself) cannot invalidate the iteration
// (UAF / null call). Contract:
//   - Handlers newly registered during a dispatch are NOT invoked in that
//     same dispatch.
//   - Handlers unregistered mid-dispatch that were already snapshotted at
//     the start of the dispatch STILL run for that dispatch.

TEST(IterationSafety, HandlerUnregisteringItselfDuringDispatchIsSafe)
{
    Astra::MulticastDelegate<void()> mc;
    int calls = 0;

    // Handler A removes itself (by id) from mc while mc is dispatching.
    // With a naive range-for over the live m_handlers, this would mutate
    // the vector being iterated (UAF / skipped-or-duplicated calls).
    Astra::MulticastDelegate<void()>::HandlerID idA = 0;
    idA = mc.Register([&]
    {
        ++calls;
        mc.Unregister(idA);
    });
    mc.Register([&] { ++calls; });

    mc.Invoke();   // must not crash / read freed handlers

    EXPECT_EQ(calls, 2);       // both handlers snapshotted at dispatch start ran
    EXPECT_EQ(mc.Size(), 1u);  // but A is gone afterward - Unregister still took effect
}

TEST(IterationSafety, HandlerClearingAllDuringDispatchIsSafe)
{
    Astra::MulticastDelegate<void()> mc;
    int calls = 0;

    // Handler A clears the ENTIRE multicast mid-dispatch. Handler B was
    // already snapshotted before the clear and must still run safely.
    mc.Register([&]
    {
        ++calls;
        mc.Clear();
    });
    mc.Register([&] { ++calls; });

    mc.Invoke();   // must not crash / read freed handlers

    EXPECT_EQ(calls, 2);        // both snapshotted handlers ran this dispatch
    EXPECT_TRUE(mc.IsEmpty());  // Clear() took effect for subsequent dispatches
}

TEST(IterationSafety, HandlerRegisteredDuringDispatchIsNotInvokedUntilNextDispatch)
{
    Astra::MulticastDelegate<void()> mc;
    int calls = 0;
    int lateCalls = 0;

    // Handler A registers a brand-new handler B mid-dispatch. B must NOT be
    // invoked as part of THIS dispatch (it wasn't in the snapshot), only on
    // subsequent ones.
    mc.Register([&]
    {
        ++calls;
        mc.Register([&] { ++lateCalls; });
    });

    mc.Invoke();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(lateCalls, 0);   // newly-registered handler skipped this dispatch
    EXPECT_EQ(mc.Size(), 2u); // registration itself still succeeded

    mc.Invoke();
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(lateCalls, 1);  // now runs on the next dispatch
}
