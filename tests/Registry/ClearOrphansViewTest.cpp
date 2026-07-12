#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct CPos { float x, y, z; }; }

TEST(ClearOrphansView, CachedViewRefreshesToEmptyAfterClear)
{
    Astra::Registry reg;
    reg.CreateEntity<CPos>();
    reg.CreateEntity<CPos>();
    auto view = reg.CreateView<CPos>();
    EXPECT_EQ(view.Size(), 2u);

    reg.Clear();

    // Before the fix: the view points at the discarded manager and still reports 2.
    size_t seen = 0;
    view.ForEach([&](Astra::Entity, CPos&) { ++seen; });
    EXPECT_EQ(seen, 0u);
    EXPECT_EQ(view.Size(), 0u);
}
