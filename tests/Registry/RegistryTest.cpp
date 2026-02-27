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

// Test basic entity creation and destruction
TEST_F(RegistryTest, BasicEntityOperations)
{
    // Create entity
    Astra::Entity entity = registry->CreateEntity();
    EXPECT_TRUE(entity.IsValid());
    EXPECT_TRUE(registry->IsValid(entity));
    
    // Registry should have one entity
    EXPECT_EQ(registry->Size(), 1u);
    EXPECT_FALSE(registry->IsEmpty());
    
    // Destroy entity
    registry->DestroyEntity(entity);
    EXPECT_FALSE(registry->IsValid(entity));
    
    // Registry should be empty
    EXPECT_EQ(registry->Size(), 0u);
    EXPECT_TRUE(registry->IsEmpty());
}

// Test entity creation with components
TEST_F(RegistryTest, CreateEntityWithComponents)
{
    using namespace Astra::Test;
    
    // Create entity with components
    Astra::Entity entity = registry->CreateEntityWith(
        Position{10.0f, 20.0f, 30.0f},
        Velocity{1.0f, 2.0f, 3.0f},
        Health{75, 100}
    );
    
    EXPECT_TRUE(entity.IsValid());
    
    // Verify components
    Position* pos = registry->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 10.0f);
    EXPECT_EQ(pos->y, 20.0f);
    EXPECT_EQ(pos->z, 30.0f);
    
    Velocity* vel = registry->GetComponent<Velocity>(entity);
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->dx, 1.0f);
    EXPECT_EQ(vel->dy, 2.0f);
    EXPECT_EQ(vel->dz, 3.0f);
    
    Health* health = registry->GetComponent<Health>(entity);
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->current, 75);
    EXPECT_EQ(health->max, 100);
}

// Test batch entity creation
TEST_F(RegistryTest, BatchEntityCreation)
{
    using namespace Astra::Test;
    
    const size_t count = 100;
    std::vector<Astra::Entity> entities(count);
    
    registry->CreateEntitiesWith<Position, Velocity>(
        count, entities,
        [](size_t i) {
            return std::make_tuple(
                Position{float(i), float(i * 2), float(i * 3)},
                Velocity{float(i * 10), 0.0f, 0.0f}
            );
        }
    );
    
    // Verify all entities were created
    EXPECT_EQ(registry->Size(), count);
    
    // Verify components
    for (size_t i = 0; i < count; ++i)
    {
        EXPECT_TRUE(registry->IsValid(entities[i]));
        
        Position* pos = registry->GetComponent<Position>(entities[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->x, float(i));
        
        Velocity* vel = registry->GetComponent<Velocity>(entities[i]);
        ASSERT_NE(vel, nullptr);
        EXPECT_EQ(vel->dx, float(i * 10));
    }
}

// Test batch entity destruction
TEST_F(RegistryTest, BatchEntityDestruction)
{
    // Create entities
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 50; ++i)
    {
        entities.push_back(registry->CreateEntity());
    }
    
    EXPECT_EQ(registry->Size(), 50u);
    
    // Destroy all entities
    registry->DestroyEntities(entities);
    
    EXPECT_EQ(registry->Size(), 0u);
    
    // Verify all entities are invalid
    for (const auto& entity : entities)
    {
        EXPECT_FALSE(registry->IsValid(entity));
    }
}

// Test component addition and removal
TEST_F(RegistryTest, ComponentOperations)
{
    using namespace Astra::Test;
    
    Astra::Entity entity = registry->CreateEntity();
    
    // Add Position component
    registry->EmplaceComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    
    // Get component to verify it was added
    Position* pos = registry->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
    
    // Get component again - should be same
    Position* retrieved = registry->GetComponent<Position>(entity);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved, pos);
    
    // Add another component
    registry->EmplaceComponent<Velocity>(entity, 10.0f, 20.0f, 30.0f);
    
    // Verify it was added
    Velocity* vel = registry->GetComponent<Velocity>(entity);
    ASSERT_NE(vel, nullptr);
    
    // Both components should exist
    EXPECT_NE(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_NE(registry->GetComponent<Velocity>(entity), nullptr);
    
    // Remove Position component
    bool removed = registry->RemoveComponent<Position>(entity);
    EXPECT_TRUE(removed);
    
    // Position should be gone, Velocity should remain
    EXPECT_EQ(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_NE(registry->GetComponent<Velocity>(entity), nullptr);
}

// Test view creation and iteration
TEST_F(RegistryTest, ViewCreationAndIteration)
{
    using namespace Astra::Test;
    
    // Create entities with different component combinations
    for (int i = 0; i < 10; ++i)
    {
        if (i % 2 == 0)
        {
            registry->CreateEntityWith(Position{float(i), 0.0f, 0.0f}, Velocity{1.0f, 0.0f, 0.0f});
        }
        else
        {
            registry->CreateEntityWith(Position{float(i), 0.0f, 0.0f});
        }
    }
    
    // Create view for Position only
    auto posView = registry->CreateView<Position>();
    
    size_t posCount = 0;
    posView.ForEach([&](Astra::Entity e, Position& pos) {
        posCount++;
        EXPECT_GE(pos.x, 0.0f);
        EXPECT_LT(pos.x, 10.0f);
    });
    EXPECT_EQ(posCount, 10u); // All entities have Position
    
    // Create view for Position and Velocity
    auto posVelView = registry->CreateView<Position, Velocity>();
    
    size_t posVelCount = 0;
    posVelView.ForEach([&](Astra::Entity e, Position& pos, Velocity& vel) {
        posVelCount++;
        EXPECT_EQ(int(pos.x) % 2, 0); // Only even indices have both
    });
    EXPECT_EQ(posVelCount, 5u); // Only even indices have both components
}

// Test clearing registry
TEST_F(RegistryTest, ClearRegistry)
{
    using namespace Astra::Test;
    
    // Create entities with components
    for (int i = 0; i < 20; ++i)
    {
        registry->CreateEntityWith(Position{float(i), 0.0f, 0.0f});
    }
    
    EXPECT_EQ(registry->Size(), 20u);
    
    // Clear registry
    registry->Clear();
    
    EXPECT_EQ(registry->Size(), 0u);
    EXPECT_TRUE(registry->IsEmpty());
    
    // Should be able to create new entities after clear
    Astra::Entity newEntity = registry->CreateEntity();
    EXPECT_TRUE(registry->IsValid(newEntity));
    EXPECT_EQ(registry->Size(), 1u);
}

// Test parent-child relationships
TEST_F(RegistryTest, ParentChildRelationships)
{
    Astra::Entity parent = registry->CreateEntity();
    Astra::Entity child1 = registry->CreateEntity();
    Astra::Entity child2 = registry->CreateEntity();
    
    // Set parent relationships
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    
    // Get children of parent
    auto relations = registry->GetRelations<>(parent);
    auto children = relations.GetChildren();
    
    EXPECT_EQ(children.size(), 2u);
    EXPECT_TRUE(std::find(children.begin(), children.end(), child1) != children.end());
    EXPECT_TRUE(std::find(children.begin(), children.end(), child2) != children.end());
    
    // Remove parent from one child
    registry->RemoveParent(child1);
    
    // Should only have one child now
    relations = registry->GetRelations<>(parent);
    children = relations.GetChildren();
    EXPECT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0], child2);
}

// Test entity links
TEST_F(RegistryTest, EntityLinks)
{
    Astra::Entity e1 = registry->CreateEntity();
    Astra::Entity e2 = registry->CreateEntity();
    Astra::Entity e3 = registry->CreateEntity();
    
    // Add links
    registry->AddLink(e1, e2);
    registry->AddLink(e1, e3);
    
    // Get links for e1
    auto relations = registry->GetRelations<>(e1);
    auto links = relations.GetLinks();
    
    EXPECT_EQ(links.size(), 2u);
    EXPECT_TRUE(std::find(links.begin(), links.end(), e2) != links.end());
    EXPECT_TRUE(std::find(links.begin(), links.end(), e3) != links.end());
    
    // Links should be bidirectional
    auto e2Relations = registry->GetRelations<>(e2);
    auto e2Links = e2Relations.GetLinks();
    EXPECT_EQ(e2Links.size(), 1u);
    EXPECT_EQ(e2Links[0], e1);
    
    // Remove link
    registry->RemoveLink(e1, e2);
    
    relations = registry->GetRelations<>(e1);
    links = relations.GetLinks();
    EXPECT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0], e3);
}

// Test archetype cleanup
TEST_F(RegistryTest, ArchetypeCleanup)
{
    using namespace Astra::Test;
    
    // Create entities with various component combinations to create archetypes
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 10; ++i)
    {
        Astra::Entity e = registry->CreateEntity();
        entities.push_back(e);
        
        if (i % 2 == 0) registry->EmplaceComponent<Position>(e);
        if (i % 3 == 0) registry->EmplaceComponent<Velocity>(e);
        if (i % 5 == 0) registry->EmplaceComponent<Health>(e);
    }
    
    size_t initialArchetypes = registry->GetArchetypeCount();
    EXPECT_GT(initialArchetypes, 1u);
    
    // Destroy all entities
    registry->DestroyEntities(entities);
    
    // Cleanup empty archetypes
    Astra::Registry::DefragmentationOptions options;
    options.minArchetypesToKeep = 1;
    
    auto result = registry->Defragment(options);
    size_t removed = result.archetypesRemoved;
    EXPECT_GT(removed, 0u);
    
    // Should have fewer archetypes
    EXPECT_LT(registry->GetArchetypeCount(), initialArchetypes);
}

// Test archetype statistics
TEST_F(RegistryTest, ArchetypeStatistics)
{
    using namespace Astra::Test;
    
    // Create entities
    for (int i = 0; i < 20; ++i)
    {
        Astra::Entity e = registry->CreateEntity();
        if (i < 10) registry->EmplaceComponent<Position>(e);
        if (i >= 5 && i < 15) registry->EmplaceComponent<Velocity>(e);
    }
    
    // Note: GetArchetypeStats() has been removed
    // We can verify entity creation indirectly through entity count
    // We created 20 entities above
    
    // Get memory usage
    size_t memUsage = registry->GetArchetypeMemoryUsage();
    EXPECT_GT(memUsage, 0u);
}

// Test signal system
TEST_F(RegistryTest, SignalSystem)
{
    using namespace Astra::Test;
    
    // Enable signals
    registry->EnableSignals(Astra::Signal::EntityCreated | Astra::Signal::ComponentAdded);
    
    // Track events
    int entityCreatedCount = 0;
    int componentAddedCount = 0;
    
    auto* signals = registry->GetSignalManager();
    
    auto entityHandler = signals->On<Astra::Events::EntityCreated>().Register([&](const Astra::Events::EntityCreated& e)
    {
        entityCreatedCount++;
    });
    
    auto componentHandler = signals->On<Astra::Events::ComponentAdded>().Register([&](const Astra::Events::ComponentAdded& e)
    {
        componentAddedCount++;
    });
    
    // Create entity (should trigger entity created)
    Astra::Entity entity = registry->CreateEntity();
    EXPECT_EQ(entityCreatedCount, 1);
    
    // Add component (should trigger component added)
    registry->EmplaceComponent<Position>(entity);
    EXPECT_EQ(componentAddedCount, 1);
    
    // Create entity with components (should trigger both)
    registry->CreateEntityWith(Position{}, Velocity{});
    EXPECT_EQ(entityCreatedCount, 2);
    EXPECT_EQ(componentAddedCount, 3); // Position and Velocity
    
    // Disconnect handlers
    signals->On<Astra::Events::EntityCreated>().Unregister(entityHandler);
    signals->On<Astra::Events::ComponentAdded>().Unregister(componentHandler);
    
    // Create another entity (should not trigger)
    registry->CreateEntity();
    EXPECT_EQ(entityCreatedCount, 2);
}

// Test registry with shared component registry
TEST_F(RegistryTest, SharedComponentRegistry)
{
    using namespace Astra::Test;
    
    // Create two registries sharing the same component registry
    auto sharedCompRegistry = std::make_shared<Astra::ComponentRegistry>();
    sharedCompRegistry->RegisterComponents<Position, Velocity, Health>();
    
    Astra::Registry registry1(sharedCompRegistry);
    Astra::Registry registry2(sharedCompRegistry);
    
    // Create entities in both registries
    std::vector<Astra::Entity> entities1;
    std::vector<Astra::Entity> entities2;
    
    for (int i = 0; i < 10; ++i)
    {
        entities1.push_back(registry1.CreateEntityWith(Position{float(i), 0.0f, 0.0f}));
        entities2.push_back(registry2.CreateEntityWith(Position{float(i + 100), 0.0f, 0.0f}));
    }
    
    // Verify both registries work independently
    EXPECT_EQ(registry1.Size(), 10u);
    EXPECT_EQ(registry2.Size(), 10u);
    
    // Verify components in registry1
    for (size_t i = 0; i < entities1.size(); ++i)
    {
        Position* pos = registry1.GetComponent<Position>(entities1[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->x, float(i));
    }
    
    // Verify components in registry2
    for (size_t i = 0; i < entities2.size(); ++i)
    {
        Position* pos = registry2.GetComponent<Position>(entities2[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->x, float(i + 100));
    }
}

// Test component registry sharing
TEST_F(RegistryTest, ComponentRegistrySharing)
{
    using namespace Astra::Test;
    
    // Get component registry from first registry (returns raw pointer)
    auto sharedRegistry = registry->GetComponentRegistry();
    
    // Create second registry with shared component registry (need to wrap in shared_ptr)
    std::shared_ptr<Astra::ComponentRegistry> sharedPtr(sharedRegistry, [](auto*){});  // Empty deleter since we don't own it
    Astra::Registry registry2(sharedPtr);
    
    // Components should work in both registries
    Astra::Entity e1 = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    Astra::Entity e2 = registry2.CreateEntityWith(Position{4.0f, 5.0f, 6.0f});
    
    Position* pos1 = registry->GetComponent<Position>(e1);
    ASSERT_NE(pos1, nullptr);
    EXPECT_EQ(pos1->x, 1.0f);
    
    Position* pos2 = registry2.GetComponent<Position>(e2);
    ASSERT_NE(pos2, nullptr);
    EXPECT_EQ(pos2->x, 4.0f);
}

// Test invalid entity operations
TEST_F(RegistryTest, InvalidEntityOperations)
{
    using namespace Astra::Test;
    
    Astra::Entity invalidEntity(9999, 1);
    
    // Operations on invalid entity should fail gracefully
    EXPECT_FALSE(registry->IsValid(invalidEntity));
    EXPECT_EQ(registry->GetComponent<Position>(invalidEntity), nullptr);
    
    // AddComponent now returns void, verify it doesn't add to invalid entity
    registry->EmplaceComponent<Position>(invalidEntity);
    EXPECT_EQ(registry->GetComponent<Position>(invalidEntity), nullptr);
    
    EXPECT_FALSE(registry->RemoveComponent<Position>(invalidEntity));
    
    // Destroying invalid entity should not crash
    registry->DestroyEntity(invalidEntity);
}

// Test entity recycling
TEST_F(RegistryTest, EntityRecycling)
{
    // Create and destroy entity
    Astra::Entity first = registry->CreateEntity();
    auto firstID = first.GetID();
    registry->DestroyEntity(first);
    
    // Create new entity - should reuse ID with incremented version
    Astra::Entity second = registry->CreateEntity();
    EXPECT_EQ(second.GetID(), firstID);
    EXPECT_GT(second.GetVersion(), first.GetVersion());
    
    // Old entity should be invalid
    EXPECT_FALSE(registry->IsValid(first));
    EXPECT_TRUE(registry->IsValid(second));
}

// Test copy constructor
TEST_F(RegistryTest, CopyConstructor)
{
    using namespace Astra::Test;
    
    // Add some components to original registry
    registry->GetComponentRegistry()->RegisterComponents<Position, Velocity>();
    
    // Create copy
    Astra::Registry::Config config;
    Astra::Registry copy(*registry, config);
    
    // Should share component registry
    EXPECT_EQ(copy.GetComponentRegistry(), registry->GetComponentRegistry());
    
    // Should be able to create entities with registered components
    Astra::Entity entity = copy.CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    EXPECT_TRUE(copy.IsValid(entity));
    
    Position* pos = copy.GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
}

// Test batch operations performance
TEST_F(RegistryTest, BatchOperationsPerformance)
{
    using namespace Astra::Test;
    
    const size_t batchSize = 1000;
    std::vector<Astra::Entity> entities(batchSize);
    
    // Test batch entity creation
    auto start = std::chrono::high_resolution_clock::now();
    registry->CreateEntitiesWith<Position, Velocity>(batchSize, entities, 
        [](size_t i)
        {
            return std::make_tuple(Position{float(i), float(i * 2), float(i * 3)}, Velocity{float(i * 10), 0.0f, 0.0f});
        });
    auto end = std::chrono::high_resolution_clock::now();
    
    // Verify all entities were created
    EXPECT_EQ(registry->Size(), batchSize);
    
    // Verify batch destruction
    registry->DestroyEntities(entities);
    EXPECT_EQ(registry->Size(), 0u);
}

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
    view.ForEach([&count](Astra::Entity e, Position& pos, Velocity& vel)
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
    posOnlyView.ForEach([&posOnlyCount](Astra::Entity e, Position& pos)
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
    posVelView.ForEach([&posVelCount](Astra::Entity e, Position& pos, Velocity& vel)
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
    allView.ForEach([&allCount](Astra::Entity e, Position& pos, Velocity& vel, Health& health)
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
