#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <set>

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

// Relations::ForEachChild must iterate a stable snapshot of the parent's
// children so that a callback which destroys the very entities being
// iterated (mutating RelationshipGraph::m_children via swap-and-pop, and
// potentially freeing the SmallVector's heap buffer once it has spilled
// past its inline capacity) is safe: no skipped children, no UAF.
TEST(IterationSafety, DestroyEachChildDuringForEachIsSafe)
{
    Astra::Registry reg;
    Astra::Entity parent = reg.CreateEntity();

    // ChildrenContainer = SmallVector<Entity, 4>; use 8 children (> inline
    // capacity) to force heap promotion of m_children[parent] before we
    // mutate it mid-iteration.
    constexpr size_t kChildCount = 8;
    for (size_t i = 0; i < kChildCount; ++i)
    {
        Astra::Entity c = reg.CreateEntity();
        reg.SetParent(c, parent);
    }

    size_t destroyed = 0;
    reg.GetRelations(parent).ForEachChild([&](Astra::Entity child)
    {
        reg.DestroyEntity(child);  // mutates m_children mid-iteration
        ++destroyed;
    });

    EXPECT_EQ(destroyed, kChildCount);  // all visited, no UAF, none skipped
}

// Stronger, deterministic regression guard for the ForEachChild snapshot.
//
// The DestroyEachChild test above passes even WITHOUT the snapshot fix, because
// destroying-the-just-visited-entity is coincidentally mirror-symmetric under
// RelationshipGraph's swap-and-pop removal and never dereferences freed memory.
// This test instead forces a genuine buffer reallocation: with the container
// already heap-promoted (8 > SmallVector<Entity,4> inline capacity), reparenting
// a brand-new 9th child *inside the first callback* runs SmallVector::Grow()
// (capacity 8 -> 16), which allocates a new buffer and FREES the old one. Without
// the snapshot, the iteration holds a live reference whose cached begin/end now
// point into that freed buffer, so it reads garbage / freed entity slots for the
// remaining originals. With the snapshot, the copy is unaffected.
//
// We assert only on OBSERVABLE CORRECT behavior (never on garbage values):
//   (a) every original child is visited exactly once  -> visitedOriginals == N
//   (b) exactly N callback invocations                -> totalVisits == N
//   (c) no non-original (garbage/freed) entity visited -> unexpectedVisits == 0
// Post-fix all three hold; pre-fix (a) and (c) fail deterministically (in Debug
// the freed buffer is CRT-fill garbage, so the remaining originals are skipped
// and non-original values are observed instead).
TEST(IterationSafety, ForEachChildSurvivesContainerReallocationDuringCallback)
{
    Astra::Registry reg;
    Astra::Entity parent = reg.CreateEntity();

    constexpr size_t N = 8;  // > SmallVector<Entity,4> inline capacity -> heap buffer
    std::set<Astra::Entity> originals;
    for (size_t i = 0; i < N; ++i)
    {
        Astra::Entity c = reg.CreateEntity();
        reg.SetParent(c, parent);
        originals.insert(c);
    }

    std::set<Astra::Entity> visitedOriginals;
    size_t totalVisits = 0;
    size_t unexpectedVisits = 0;
    bool grewOnce = false;

    reg.GetRelations(parent).ForEachChild([&](Astra::Entity child)
    {
        ++totalVisits;
        if (originals.count(child) != 0)
            visitedOriginals.insert(child);
        else
            ++unexpectedVisits;

        if (!grewOnce)
        {
            grewOnce = true;
            // Reparent a fresh 9th child under `parent`: this push_back forces
            // m_children[parent] to grow (8 -> 16), reallocating and freeing the
            // buffer that a live-reference iteration is walking.
            Astra::Entity extra = reg.CreateEntity();
            reg.SetParent(extra, parent);
        }
    });

    EXPECT_EQ(visitedOriginals.size(), N);  // (a) all originals visited, none skipped
    EXPECT_EQ(totalVisits, N);              // (b) no duplicates / extra iterations
    EXPECT_EQ(unexpectedVisits, 0u);        // (c) never read a freed/garbage entity
}
