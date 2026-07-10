#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <iostream>
#include "../TestComponents.hpp"
#include "Astra/Registry/Registry.hpp"

// Split from RegistryTest.cpp (2026-07-10 test-suite audit): the registry's
// core-operations tests and its Save/Load serialization tests are two
// separable themes. Fixture duplicated verbatim (< 20 lines) per the split
// policy; TEST bodies below are byte-identical to the ones removed from
// RegistryTest.cpp.
class RegistryTest : public ::testing::Test
{
protected:
    std::unique_ptr<Astra::Registry> registry;

    void SetUp() override
    {
        registry = std::make_unique<Astra::Registry>();

        // Register test components
        using namespace Astra::Test;
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();
    }

    void TearDown() override
    {
        registry.reset();
    }
};

// ============================== Serialization Tests ==============================

TEST_F(RegistryTest, EmptyRegistrySerialization)
{
    using namespace Astra::Test;

    // Save empty registry to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify loaded registry is empty
    EXPECT_EQ(loadedRegistry->Size(), 0u);
    EXPECT_TRUE(loadedRegistry->IsEmpty());
}

TEST_F(RegistryTest, SingleEntitySerialization)
{
    using namespace Astra::Test;

    // Create entity with components
    Astra::Entity entity = registry->CreateEntityWith(
        Position{1.0f, 2.0f, 3.0f},
        Velocity{4.0f, 5.0f, 6.0f}
    );
    (void)entity;

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify loaded registry has one entity
    EXPECT_EQ(loadedRegistry->Size(), 1u);

    // Get entities from loaded registry
    auto view = loadedRegistry->CreateView<Position, Velocity>();
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos, Velocity& vel)
    {
        count++;
        EXPECT_FLOAT_EQ(pos.x, 1.0f);
        EXPECT_FLOAT_EQ(pos.y, 2.0f);
        EXPECT_FLOAT_EQ(pos.z, 3.0f);
        EXPECT_FLOAT_EQ(vel.dx, 4.0f);
        EXPECT_FLOAT_EQ(vel.dy, 5.0f);
        EXPECT_FLOAT_EQ(vel.dz, 6.0f);
    });
    EXPECT_EQ(count, 1u);
}

TEST_F(RegistryTest, MultipleEntitiesSerialization)
{
    using namespace Astra::Test;

    // Create multiple entities with different component combinations
    std::vector<Astra::Entity> entities;

    // 10 entities with Position only
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), float(i * 2), float(i * 3)}
        ));
    }

    // 10 entities with Position and Velocity
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i + 10), float(i * 2 + 10), float(i * 3 + 10)},
            Velocity{float(i), 0.0f, 0.0f}
        ));
    }

    // 10 entities with Position, Velocity, and Health
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i + 20), float(i * 2 + 20), float(i * 3 + 20)},
            Velocity{float(i + 10), 0.0f, 0.0f},
            Health{100 - i, 100}
        ));
    }

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify total entity count
    EXPECT_EQ(loadedRegistry->Size(), 30u);

    // Verify entities with Position only
    auto posOnlyView = loadedRegistry->CreateView<Position, Astra::Not<Velocity>>();
    size_t posOnlyCount = 0;
    posOnlyView.ForEach([&posOnlyCount](Astra::Entity, Position& pos)
    {
        posOnlyCount++;
        int i = int(pos.x);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(pos.y, float(i * 2));
        EXPECT_FLOAT_EQ(pos.z, float(i * 3));
    });
    EXPECT_EQ(posOnlyCount, 10u);

    // Verify entities with Position and Velocity but not Health
    auto posVelView = loadedRegistry->CreateView<Position, Velocity, Astra::Not<Health>>();
    size_t posVelCount = 0;
    posVelView.ForEach([&posVelCount](Astra::Entity, Position& pos, Velocity& vel)
    {
        posVelCount++;
        int i = int(pos.x) - 10;
        EXPECT_GE(i, 0);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(vel.dx, float(i));
    });
    EXPECT_EQ(posVelCount, 10u);

    // Verify entities with all three components
    auto allView = loadedRegistry->CreateView<Position, Velocity, Health>();
    size_t allCount = 0;
    allView.ForEach([&allCount](Astra::Entity, Position& pos, Velocity& vel, Health& health)
    {
        allCount++;
        int i = int(pos.x) - 20;
        EXPECT_GE(i, 0);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(vel.dx, float(i + 10));
        EXPECT_EQ(health.current, 100 - i);
        EXPECT_EQ(health.max, 100);
    });
    EXPECT_EQ(allCount, 10u);
}

TEST_F(RegistryTest, RelationshipSerialization)
{
    using namespace Astra::Test;

    // Create parent and children
    Astra::Entity parent = registry->CreateEntityWith(Position{0, 0, 0});
    Astra::Entity child1 = registry->CreateEntityWith(Position{1, 0, 0});
    Astra::Entity child2 = registry->CreateEntityWith(Position{2, 0, 0});
    Astra::Entity linked1 = registry->CreateEntityWith(Position{3, 0, 0});
    Astra::Entity linked2 = registry->CreateEntityWith(Position{4, 0, 0});

    // Set up relationships
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->AddLink(linked1, linked2);

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify entity count
    EXPECT_EQ(loadedRegistry->Size(), 5u);

    // We can't directly verify relationships without entity IDs being preserved
    // But we can verify that the relationship graph was deserialized
    // and contains the right number of relationships
    const auto& graph = loadedRegistry->GetRelationshipGraph();
    (void)graph;

    // Check that we have parent-child relationships
    // Note: We'd need to iterate through entities to find the actual relationships
    // since entity IDs are not preserved across serialization
}

TEST_F(RegistryTest, SerializationErrorHandling)
{
    using namespace Astra::Test;

    // Test loading with missing components
    {
        // Create entity with Position and Velocity
        registry->CreateEntityWith(
            Position{1.0f, 2.0f, 3.0f},
            Velocity{4.0f, 5.0f, 6.0f}
        );

        // Save to memory
        auto saveResult = registry->Save();
        ASSERT_TRUE(saveResult.IsOk());
        auto buffer = std::move(*saveResult.GetValue());

        // Create component registry WITHOUT Velocity registered
        auto incompleteRegistry = std::make_shared<Astra::ComponentRegistry>();
        incompleteRegistry->RegisterComponent<Position>();  // Missing Velocity!

        // Load should fail
        auto loadResult = Astra::Registry::Load(buffer, incompleteRegistry);
        EXPECT_TRUE(loadResult.IsErr());
    }

    // Test loading with corrupted data
    {
        std::vector<std::byte> corruptedData(100, std::byte{0xFF});

        auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
        componentRegistry->RegisterComponents<Position, Velocity>();

        auto loadResult = Astra::Registry::Load(std::span(corruptedData), componentRegistry);
        EXPECT_TRUE(loadResult.IsErr());
    }
}
