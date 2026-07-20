#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <vector>
#include <algorithm>

using namespace Astra;

namespace
{
    struct VITPosition
    {
        float x, y, z;
    };

    struct VITVelocity
    {
        float vx, vy, vz;
    };

    struct VITHealth
    {
        int value;
    };
}

class ViewIteratorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
    }

    void TearDown() override
    {
        registry.reset();
    }

    std::unique_ptr<Registry> registry;
};

TEST_F(ViewIteratorTest, BasicIteration)
{
    // Create entities with VITPosition and VITVelocity
    std::vector<Entity> entities;
    for (int i = 0; i < 10; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{static_cast<float>(i), 0.0f, 0.0f});
        registry->AddComponent(e, VITVelocity{1.0f, 0.0f, 0.0f});
        entities.push_back(e);
    }

    // Iterate using range-based for loop
    auto view = registry->CreateView<VITPosition, VITVelocity>();

    int count = 0;
    for (auto [entity, pos, vel] : view)
    {
        EXPECT_TRUE(std::find(entities.begin(), entities.end(), entity) != entities.end());
        pos.x += vel.vx;
        ++count;
    }

    EXPECT_EQ(count, 10);

    // Verify the modifications took effect
    for (Entity e : entities)
    {
        auto* pos = registry->GetComponent<VITPosition>(e);
        EXPECT_NE(pos, nullptr);
        // Original x was the index, now it should be index + 1
    }
}

TEST_F(ViewIteratorTest, EmptyView)
{
    // Create entities but with different components
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITHealth{100});
    }

    // View for VITPosition/VITVelocity should be empty
    auto view = registry->CreateView<VITPosition, VITVelocity>();

    int count = 0;
    for (auto [entity, pos, vel] : view)
    {
        (void)entity;
        (void)pos;
        (void)vel;
        ++count;
    }

    EXPECT_EQ(count, 0);
}

TEST_F(ViewIteratorTest, SingleComponent)
{
    // Create entities with just VITPosition
    for (int i = 0; i < 100; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{static_cast<float>(i), 0.0f, 0.0f});
    }

    auto view = registry->CreateView<VITPosition>();

    int count = 0;
    float sum = 0.0f;
    for (auto [entity, pos] : view)
    {
        (void)entity;
        sum += pos.x;
        ++count;
    }

    EXPECT_EQ(count, 100);
    // Sum of 0..99 = 99*100/2 = 4950
    EXPECT_FLOAT_EQ(sum, 4950.0f);
}

TEST_F(ViewIteratorTest, MultipleArchetypes)
{
    // Create entities in different archetypes
    // Archetype 1: VITPosition only
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{1.0f, 0.0f, 0.0f});
    }

    // Archetype 2: VITPosition + VITVelocity
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{2.0f, 0.0f, 0.0f});
        registry->AddComponent(e, VITVelocity{0.0f, 0.0f, 0.0f});
    }

    // Archetype 3: VITPosition + VITHealth
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{3.0f, 0.0f, 0.0f});
        registry->AddComponent(e, VITHealth{100});
    }

    // View for VITPosition should see all 15 entities
    auto view = registry->CreateView<VITPosition>();

    int count = 0;
    float sum = 0.0f;
    for (auto [entity, pos] : view)
    {
        (void)entity;
        sum += pos.x;
        ++count;
    }

    EXPECT_EQ(count, 15);
    // 5*1 + 5*2 + 5*3 = 5 + 10 + 15 = 30
    EXPECT_FLOAT_EQ(sum, 30.0f);
}

TEST_F(ViewIteratorTest, CompareWithForEach)
{
    // Create many entities
    constexpr int NUM_ENTITIES = 1000;
    for (int i = 0; i < NUM_ENTITIES; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{static_cast<float>(i), 0.0f, 0.0f});
        registry->AddComponent(e, VITVelocity{1.0f, 0.0f, 0.0f});
    }

    auto view = registry->CreateView<VITPosition, VITVelocity>();

    // Collect entities via iterator
    std::vector<Entity> iteratorEntities;
    for (auto [entity, pos, vel] : view)
    {
        (void)pos;
        (void)vel;
        iteratorEntities.push_back(entity);
    }

    // Collect entities via ForEach
    std::vector<Entity> forEachEntities;
    view.ForEach([&](Entity e, VITPosition&, VITVelocity&) {
        forEachEntities.push_back(e);
    });

    // Both should have the same entities (though possibly in different order)
    EXPECT_EQ(iteratorEntities.size(), forEachEntities.size());
    EXPECT_EQ(iteratorEntities.size(), static_cast<size_t>(NUM_ENTITIES));

    // Sort and compare
    std::sort(iteratorEntities.begin(), iteratorEntities.end());
    std::sort(forEachEntities.begin(), forEachEntities.end());
    EXPECT_EQ(iteratorEntities, forEachEntities);
}

TEST_F(ViewIteratorTest, ModifyDuringIteration)
{
    // Create entities
    for (int i = 0; i < 10; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{static_cast<float>(i), 0.0f, 0.0f});
    }

    auto view = registry->CreateView<VITPosition>();

    // Modify component values during iteration
    for (auto [entity, pos] : view)
    {
        (void)entity;
        pos.x *= 2.0f;
        pos.y = 100.0f;
    }

    // Verify modifications
    float sumX = 0.0f;
    float sumY = 0.0f;
    view.ForEach([&](Entity, VITPosition& pos) {
        sumX += pos.x;
        sumY += pos.y;
    });

    // Sum of 2*(0..9) = 2*45 = 90
    EXPECT_FLOAT_EQ(sumX, 90.0f);
    // 10 entities * 100 = 1000
    EXPECT_FLOAT_EQ(sumY, 1000.0f);
}

TEST_F(ViewIteratorTest, LargeEntityCount)
{
    // Test with many entities to ensure chunk transitions work
    constexpr int NUM_ENTITIES = 10000;

    for (int i = 0; i < NUM_ENTITIES; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{1.0f, 1.0f, 1.0f});
    }

    auto view = registry->CreateView<VITPosition>();

    int count = 0;
    for (auto [entity, pos] : view)
    {
        (void)entity;
        (void)pos;
        ++count;
    }

    EXPECT_EQ(count, NUM_ENTITIES);
}

TEST_F(ViewIteratorTest, ThreeComponents)
{
    // Test with three components
    for (int i = 0; i < 50; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->AddComponent(e, VITPosition{static_cast<float>(i), 0.0f, 0.0f});
        registry->AddComponent(e, VITVelocity{1.0f, 1.0f, 1.0f});
        registry->AddComponent(e, VITHealth{100 + i});
    }

    auto view = registry->CreateView<VITPosition, VITVelocity, VITHealth>();

    int count = 0;
    int healthSum = 0;
    for (auto [entity, pos, vel, health] : view)
    {
        (void)entity;
        (void)pos;
        (void)vel;
        healthSum += health.value;
        ++count;
    }

    EXPECT_EQ(count, 50);
    // Sum of (100+0) + (100+1) + ... + (100+49) = 50*100 + (0+1+...+49) = 5000 + 1225 = 6225
    EXPECT_EQ(healthSum, 6225);
}
