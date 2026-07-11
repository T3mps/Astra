#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct TPos { float x, y, z; }; struct Tag {}; }

TEST(EmptyTag, RangeForBindsTagAtValidAddress)
{
    Astra::Registry reg;
    reg.CreateEntity<TPos, Tag>();

    auto view = reg.CreateView<TPos, Tag>();
    size_t n = 0;
    const Tag* tagAddr = nullptr;
    for (auto [e, pos, tag] : view)
    {
        ++n;
        tagAddr = &tag;
    }
    EXPECT_EQ(n, 1u);
    EXPECT_NE(tagAddr, nullptr);   // was nullptr before the fix
}

TEST(EmptyTag, MigrationAndRemovalPathsAreSafe)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<TPos, Tag>();
    auto b = reg.CreateEntity<TPos, Tag>();
    reg.DestroyEntity(a);                       // swap-and-pop with a Tag column
    EXPECT_TRUE(reg.RemoveComponent<Tag>(b));   // migration dropping the tag
    EXPECT_TRUE(reg.HasComponent<TPos>(b));
    EXPECT_FALSE(reg.HasComponent<Tag>(b));
    auto* p = reg.GetComponent<TPos>(b);
    ASSERT_NE(p, nullptr);
}
