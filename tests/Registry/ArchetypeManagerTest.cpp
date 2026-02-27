#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Archetype/ArchetypeManager.hpp"

class ArchetypeManagerTest : public ::testing::Test
{
protected:
    std::shared_ptr<Astra::ComponentRegistry> componentRegistry;
    std::unique_ptr<Astra::ArchetypeManager> manager;
    std::vector<Astra::Entity> testEntities;
    
    void SetUp() override 
    {
        // Create component registry first
        componentRegistry = std::make_shared<Astra::ComponentRegistry>();
        
        // Register test components
        using namespace Astra::Test;
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();
        
        // Create manager with the component registry
        manager = std::make_unique<Astra::ArchetypeManager>(componentRegistry);
        
        // Create some test entities
        for (int i = 0; i < 100; ++i)
        {
            testEntities.emplace_back(i, 1);
        }
    }
    
    void TearDown() override 
    {
        manager.reset();
        testEntities.clear();
    }
};

// Test basic entity addition and removal
TEST_F(ArchetypeManagerTest, BasicEntityOperations)
{
    Astra::Entity entity(1, 1);
    
    // Add entity to manager
    manager->AddEntity(entity);
    
    // Entity should exist in root archetype (no components)
    EXPECT_EQ(manager->GetArchetypeCount(), 1u); // Root archetype
    
    // Remove entity
    manager->RemoveEntity(entity);
    
    // Manager should still have root archetype
    EXPECT_EQ(manager->GetArchetypeCount(), 1u);
}

// Test adding components to entities
TEST_F(ArchetypeManagerTest, AddComponentToEntity)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Add Position component
    Position* pos = manager->AddComponent<Position>(entity, 10.0f, 20.0f, 30.0f);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 10.0f);
    EXPECT_EQ(pos->y, 20.0f);
    EXPECT_EQ(pos->z, 30.0f);
    
    // Should have created new archetype
    EXPECT_EQ(manager->GetArchetypeCount(), 2u); // Root + Position archetype
    
    // Get component back
    Position* retrieved = manager->GetComponent<Position>(entity);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->x, 10.0f);
    
    // Modify component
    retrieved->x = 100.0f;
    
    // Get again and verify modification
    Position* modified = manager->GetComponent<Position>(entity);
    EXPECT_EQ(modified->x, 100.0f);
}

// Test removing components from entities
TEST_F(ArchetypeManagerTest, RemoveComponentFromEntity)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Add components
    manager->AddComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    manager->AddComponent<Velocity>(entity, 10.0f, 0.0f, 0.0f);
    
    EXPECT_EQ(manager->GetArchetypeCount(), 3u); // Root, Position, Position+Velocity
    
    // Verify both components exist
    EXPECT_NE(manager->GetComponent<Position>(entity), nullptr);
    EXPECT_NE(manager->GetComponent<Velocity>(entity), nullptr);
    
    // Remove Velocity component
    bool removed = manager->RemoveComponent<Velocity>(entity);
    EXPECT_TRUE(removed);
    
    // Position should still exist, Velocity should not
    EXPECT_NE(manager->GetComponent<Position>(entity), nullptr);
    EXPECT_EQ(manager->GetComponent<Velocity>(entity), nullptr);
    
    // Try removing non-existent component
    bool removedAgain = manager->RemoveComponent<Velocity>(entity);
    EXPECT_FALSE(removedAgain);
}

// Test archetype transitions
TEST_F(ArchetypeManagerTest, ArchetypeTransitions)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Track archetype count as we add components
    EXPECT_EQ(manager->GetArchetypeCount(), 1u); // Root
    
    // Add Position - creates Position archetype
    manager->AddComponent<Position>(entity);
    EXPECT_EQ(manager->GetArchetypeCount(), 2u);
    
    // Add Velocity - creates Position+Velocity archetype
    manager->AddComponent<Velocity>(entity);
    EXPECT_EQ(manager->GetArchetypeCount(), 3u);
    
    // Add Health - creates Position+Velocity+Health archetype
    manager->AddComponent<Health>(entity);
    EXPECT_EQ(manager->GetArchetypeCount(), 4u);
    
    // Remove Velocity - reuses Position+Health archetype or creates it
    manager->RemoveComponent<Velocity>(entity);
    EXPECT_EQ(manager->GetArchetypeCount(), 5u); // Added Position+Health
    
    // Add Velocity back - reuses Position+Velocity+Health archetype
    manager->AddComponent<Velocity>(entity);
    EXPECT_EQ(manager->GetArchetypeCount(), 5u); // No new archetype
}

// Test batch entity addition
TEST_F(ArchetypeManagerTest, BatchEntityAddition)
{
    using namespace Astra::Test;
    
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 50; ++i)
    {
        entities.emplace_back(i, 1);
    }
    
    // Add entities with components in batch
    manager->AddEntitiesWith<Position, Velocity>(
        entities,
        [](size_t i) {
            return std::make_tuple(
                Position{float(i), float(i * 2), float(i * 3)},
                Velocity{float(i * 10), 0.0f, 0.0f}
            );
        }
    );
    
    // Verify all entities have the components
    for (size_t i = 0; i < entities.size(); ++i)
    {
        Position* pos = manager->GetComponent<Position>(entities[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->x, float(i));
        EXPECT_EQ(pos->y, float(i * 2));
        EXPECT_EQ(pos->z, float(i * 3));
        
        Velocity* vel = manager->GetComponent<Velocity>(entities[i]);
        ASSERT_NE(vel, nullptr);
        EXPECT_EQ(vel->dx, float(i * 10));
    }
}

// Test batch entity removal
TEST_F(ArchetypeManagerTest, BatchEntityRemoval)
{
    using namespace Astra::Test;
    
    // Add entities
    for (const auto& entity : testEntities)
    {
        manager->AddEntity(entity);
        manager->AddComponent<Position>(entity);
    }
    
    // Remove first 50 entities in batch
    std::vector<Astra::Entity> toRemove(testEntities.begin(), testEntities.begin() + 50);
    manager->RemoveEntities(toRemove);
    
    // Verify removed entities don't have components
    for (const auto& entity : toRemove)
    {
        EXPECT_EQ(manager->GetComponent<Position>(entity), nullptr);
    }
    
    // Verify remaining entities still have components
    for (size_t i = 50; i < testEntities.size(); ++i)
    {
        EXPECT_NE(manager->GetComponent<Position>(testEntities[i]), nullptr);
    }
}

// Test batch component addition
TEST_F(ArchetypeManagerTest, BatchComponentAddition)
{
    using namespace Astra::Test;
    
    // Add entities with Position
    for (const auto& entity : testEntities)
    {
        manager->AddEntity(entity);
        manager->AddComponent<Position>(entity);
    }
    
    // Add Velocity to all entities in batch
    manager->AddComponents<Velocity>(testEntities, 1.0f, 2.0f, 3.0f);
    
    // Verify all entities have both components
    for (const auto& entity : testEntities)
    {
        EXPECT_NE(manager->GetComponent<Position>(entity), nullptr);
        Velocity* vel = manager->GetComponent<Velocity>(entity);
        ASSERT_NE(vel, nullptr);
        EXPECT_EQ(vel->dx, 1.0f);
        EXPECT_EQ(vel->dy, 2.0f);
        EXPECT_EQ(vel->dz, 3.0f);
    }
}

// Test archetype cleanup
TEST_F(ArchetypeManagerTest, ArchetypeCleanup)
{
    using namespace Astra::Test;
    
    // Create and remove entities to create empty archetypes
    for (int i = 0; i < 10; ++i)
    {
        Astra::Entity entity(i, 1);
        manager->AddEntity(entity);
        manager->AddComponent<Position>(entity);
        if (i % 2 == 0)
        {
            manager->AddComponent<Velocity>(entity);
        }
        if (i % 3 == 0)
        {
            manager->AddComponent<Health>(entity);
        }
    }
    
    size_t archetypeCount = manager->GetArchetypeCount();
    EXPECT_GT(archetypeCount, 1u);
    
    // Remove all entities
    for (int i = 0; i < 10; ++i)
    {
        manager->RemoveEntity(Astra::Entity(i, 1));
    }
    
    // Cleanup empty archetypes
    Astra::ArchetypeManager::DefragmentOptions options;
    options.minArchetypesToKeep = 1; // Keep root
    
    auto result = manager->Defragment(options);
    size_t removed = result.emptyArchetypesRemoved;
    EXPECT_GT(removed, 0u);
    
    // Should have fewer archetypes now
    EXPECT_LT(manager->GetArchetypeCount(), archetypeCount);
}

// Test component registry sharing
TEST_F(ArchetypeManagerTest, ComponentRegistrySharing)
{
    using namespace Astra::Test;
    
    // Create second manager with shared component registry
    Astra::ArchetypeManager manager2(componentRegistry);
    
    // Components registered in first manager should work in second
    Astra::Entity entity(100, 1);
    manager2.AddEntity(entity);
    
    // Should be able to add component without registering again
    Position* pos = manager2.AddComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
}

// Test edge caching for archetype transitions
TEST_F(ArchetypeManagerTest, EdgeCaching)
{
    using namespace Astra::Test;
    
    // Create multiple entities that will follow same transition path
    for (int i = 0; i < 10; ++i)
    {
        Astra::Entity entity(i, 1);
        manager->AddEntity(entity);
        
        // Same sequence of component additions
        manager->AddComponent<Position>(entity);
        manager->AddComponent<Velocity>(entity);
        manager->AddComponent<Health>(entity);
        
        // Same sequence of component removals
        manager->RemoveComponent<Velocity>(entity);
        manager->AddComponent<Velocity>(entity);
    }
    
    // Should reuse archetypes due to edge caching
    // We expect: Root, Position, Position+Velocity, Position+Velocity+Health, Position+Health
    EXPECT_EQ(manager->GetArchetypeCount(), 5u);
}

// Test move-only component handling
TEST_F(ArchetypeManagerTest, MoveOnlyComponents)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Add move-only component
    auto* resource = manager->AddComponent<Resource>(entity, 42);
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(*resource->data, 42);
    
    // Should be able to retrieve it
    auto* retrieved = manager->GetComponent<Resource>(entity);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(*retrieved->data, 42);
    
    // Add another component to trigger archetype transition
    manager->AddComponent<Position>(entity);
    
    // Resource should still be accessible after move
    auto* afterMove = manager->GetComponent<Resource>(entity);
    ASSERT_NE(afterMove, nullptr);
    EXPECT_EQ(*afterMove->data, 42);
}

// Test entity location tracking
TEST_F(ArchetypeManagerTest, EntityLocationTracking)
{
    using namespace Astra::Test;
    
    // Create entity with components to ensure archetype exists
    Astra::Entity entity(1, 1);
    manager->AddEntityWith(entity, Position{0.0f, 0.0f, 0.0f}, Velocity{0.0f, 0.0f, 0.0f});
    
    // Find the archetype
    auto* archetype = manager->FindArchetype<Position, Velocity>();
    ASSERT_NE(archetype, nullptr);
    
    // Get entity's location from the manager
    auto* record = manager->GetEntityRecord(entity);
    ASSERT_NE(record, nullptr);
    auto location = record->location;
    
    // Should be able to get components
    archetype->SetComponent(location, Position{1.0f, 2.0f, 3.0f});
    
    Position* pos = manager->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
}

// Test stress with many archetypes
TEST_F(ArchetypeManagerTest, StressManyArchetypes)
{
    using namespace Astra::Test;
    
    // Create entities with many different component combinations
    // This will create 2^3 = 8 different archetypes (including root)
    for (int i = 0; i < 100; ++i)
    {
        Astra::Entity entity(i, 1);
        manager->AddEntity(entity);
        
        if (i & 1) manager->AddComponent<Position>(entity);
        if (i & 2) manager->AddComponent<Velocity>(entity);
        if (i & 4) manager->AddComponent<Health>(entity);
    }
    
    // Should have created multiple archetypes
    EXPECT_GE(manager->GetArchetypeCount(), 8u);
    
    // All entities should still be accessible
    for (int i = 0; i < 100; ++i)
    {
        Astra::Entity entity(i, 1);
        
        if (i & 1)
        {
            EXPECT_NE(manager->GetComponent<Position>(entity), nullptr);
        }
        else
        {
            EXPECT_EQ(manager->GetComponent<Position>(entity), nullptr);
        }
        
        if (i & 2)
        {
            EXPECT_NE(manager->GetComponent<Velocity>(entity), nullptr);
        }
        else
        {
            EXPECT_EQ(manager->GetComponent<Velocity>(entity), nullptr);
        }
    }
}

// Test component data preservation during transitions
TEST_F(ArchetypeManagerTest, ComponentDataPreservation)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Add Position with specific values
    manager->AddComponent<Position>(entity, 10.0f, 20.0f, 30.0f);
    
    // Add Health with specific values
    manager->AddComponent<Health>(entity, 75, 100);
    
    // Add Velocity (triggers archetype transition)
    manager->AddComponent<Velocity>(entity, 5.0f, 10.0f, 15.0f);
    
    // Verify all components retained their values after transitions
    Position* pos = manager->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 10.0f);
    EXPECT_EQ(pos->y, 20.0f);
    EXPECT_EQ(pos->z, 30.0f);
    
    Health* health = manager->GetComponent<Health>(entity);
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->current, 75);
    EXPECT_EQ(health->max, 100);
    
    Velocity* vel = manager->GetComponent<Velocity>(entity);
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->dx, 5.0f);
    EXPECT_EQ(vel->dy, 10.0f);
    EXPECT_EQ(vel->dz, 15.0f);
    
    // Remove Velocity and verify other components still intact
    manager->RemoveComponent<Velocity>(entity);
    
    pos = manager->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 10.0f);
    
    health = manager->GetComponent<Health>(entity);
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->current, 75);
}

// Test invalid entity operations
TEST_F(ArchetypeManagerTest, InvalidEntityOperations)
{
    using namespace Astra::Test;
    
    Astra::Entity invalidEntity(9999, 1);
    
    // Try to get component from non-existent entity
    EXPECT_EQ(manager->GetComponent<Position>(invalidEntity), nullptr);
    
    // Try to add component to non-existent entity
    EXPECT_EQ(manager->AddComponent<Position>(invalidEntity), nullptr);
    
    // Try to remove component from non-existent entity
    EXPECT_FALSE(manager->RemoveComponent<Position>(invalidEntity));
    
    // Try to remove non-existent entity
    manager->RemoveEntity(invalidEntity); // Should not crash
}

// Test duplicate component addition
TEST_F(ArchetypeManagerTest, DuplicateComponentAddition)
{
    using namespace Astra::Test;
    
    Astra::Entity entity(1, 1);
    manager->AddEntity(entity);
    
    // Add Position component
    Position* pos1 = manager->AddComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    ASSERT_NE(pos1, nullptr);
    
    // Try to add Position again - should return nullptr
    Position* pos2 = manager->AddComponent<Position>(entity, 4.0f, 5.0f, 6.0f);
    EXPECT_EQ(pos2, nullptr);
    
    // Original component should be unchanged
    Position* original = manager->GetComponent<Position>(entity);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->x, 1.0f);
    EXPECT_EQ(original->y, 2.0f);
    EXPECT_EQ(original->z, 3.0f);
}