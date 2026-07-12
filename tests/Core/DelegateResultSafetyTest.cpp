#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <memory>

TEST(DelegateSafety, CopyOfMoveOnlyFunctorIsEmptyNotUninitialized)
{
    // A small functor capturing a move-only type (unique_ptr) — not copy-constructible.
    auto up = std::make_unique<int>(5);
    Astra::Delegate<void()> d([p = std::move(up)]() { /* uses p */ });
    ASSERT_TRUE(static_cast<bool>(d));

    Astra::Delegate<void()> copy(d);      // copying a move-only functor
    // Contract: rather than "valid but uninitialized storage", the copy is empty.
    EXPECT_FALSE(static_cast<bool>(copy));
    // And it must be safe to destroy/leave-scope with no UB (no invoke of garbage).

    // The original delegate must remain unaffected and still invocable.
    EXPECT_TRUE(static_cast<bool>(d));
    d();

    // Copy-assignment of a move-only functor must also yield empty, not UB.
    Astra::Delegate<void()> assigned;
    assigned = d;
    EXPECT_FALSE(static_cast<bool>(assigned));
}
