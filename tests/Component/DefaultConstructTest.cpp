#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <vector>

namespace
{
    struct NsdmiHealth { int current = 100; int max = 100; };  // trivially copyable, NOT trivially default-constructible
    struct PodPoint { float x, y, z; };                        // trivially default-constructible
}

TEST(DefaultConstruct, NsdmiAppliedOnCreateEntity)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<NsdmiHealth>();
    auto* h = reg.GetComponent<NsdmiHealth>(e);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->current, 100);
    EXPECT_EQ(h->max, 100);
}

TEST(DefaultConstruct, NsdmiAppliedOnBatchCreate)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> out(64);
    reg.CreateEntities<NsdmiHealth>(64, out);
    for (auto e : out)
    {
        auto* h = reg.GetComponent<NsdmiHealth>(e);
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->current, 100);
    }
}

TEST(DefaultConstruct, PodZeroedAfterSlotReuse)
{
    Astra::Registry reg;
    auto e1 = reg.CreateEntity<PodPoint>();
    auto* p1 = reg.GetComponent<PodPoint>(e1);
    p1->x = 123.0f; p1->y = 456.0f; p1->z = 789.0f;
    reg.DestroyEntity(e1);

    auto e2 = reg.CreateEntity<PodPoint>();   // reuses the vacated chunk slot
    auto* p2 = reg.GetComponent<PodPoint>(e2);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->x, 0.0f);
    EXPECT_EQ(p2->y, 0.0f);
    EXPECT_EQ(p2->z, 0.0f);
}
