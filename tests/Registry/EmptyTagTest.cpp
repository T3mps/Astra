#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include <span>
#include <vector>

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

TEST(EmptyTag, BatchAddTagComponentIsSafe)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> entities(64);
    reg.CreateEntities<TPos>(64, entities);   // batch of entities carrying a non-empty component

    // Batch-add an empty tag to all of them: exercises
    // SetComponents<Tag> -> BatchConstructComponent<Tag>, which pre-fix wrote to nullptr.
    reg.AddComponents<Tag>(std::span<Astra::Entity>(entities.data(), entities.size()), Tag{});

    for (auto e : entities)
    {
        EXPECT_TRUE(reg.HasComponent<Tag>(e));
        EXPECT_TRUE(reg.HasComponent<TPos>(e));
    }
}
