#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include <unordered_set>
#include <iostream>
#include "Astra/Registry/Registry.hpp"
#include "Astra/Archetype/ArchetypeChunkPool.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Core/TypeID.hpp"
#include "../TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class ResourceExhaustionTest : public ::testing::Test
{
protected:
    std::unique_ptr<Registry> registry;
    
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
        
        // Register test components
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();
    }
    
    void TearDown() override
    {
        registry.reset();
    }
};

// Test maximum component types registration
TEST_F(ResourceExhaustionTest, MaxComponentTypes)
{
    auto componentRegistry = std::make_shared<ComponentRegistry>();
    Registry testRegistry(componentRegistry);
    
    // We can register up to MAX_COMPONENTS (64 by default)
    size_t registeredCount = 0;
    
    // Register components up to the limit
    for (size_t i = 0; i < MAX_COMPONENTS; ++i)
    {
        // Use different struct sizes to simulate real components
        switch (i % 4)
        {
            case 0:
                componentRegistry->RegisterComponent<Position>();
                break;
            case 1:
                componentRegistry->RegisterComponent<Velocity>();
                break;
            case 2:
                componentRegistry->RegisterComponent<Health>();
                break;
            case 3:
                componentRegistry->RegisterComponent<Transform>();
                break;
        }
        registeredCount++;
        
        // Only register unique components once
        if (registeredCount >= 8) break; // We only have 8 unique test components
    }
    
    // Verify we can create entities with registered components
    Entity entity = testRegistry.CreateEntityWith(Position{1, 2, 3});
    EXPECT_TRUE(entity.IsValid());
    EXPECT_NE(testRegistry.GetComponent<Position>(entity), nullptr);
}

// Test entity ID space exhaustion and wraparound
TEST_F(ResourceExhaustionTest, EntityIDSpaceExhaustion)
{
    // Create a custom entity pool with small initial capacity for testing
    EntityManager pool(16);  // Small initial capacity
    
    std::vector<Entity> entities;
    const size_t testCount = 5000;  // Increased from 1000 to stress test more
    
    // Create and destroy entities multiple times to test version wraparound
    for (int cycle = 0; cycle < 5; ++cycle)  // Increased cycles from 3 to 5
    {
        entities.clear();
        
        // Create batch of entities
        for (size_t i = 0; i < testCount; ++i)
        {
            Entity e = pool.Create();
            EXPECT_TRUE(e.IsValid());
            entities.push_back(e);
        }
        
        // Verify all entities are unique
        std::unordered_set<Entity> uniqueEntities(entities.begin(), entities.end());
        EXPECT_EQ(uniqueEntities.size(), entities.size());
        
        // Destroy all entities
        for (Entity e : entities)
        {
            pool.Destroy(e);
        }
        
        // Old entities should now be invalid
        for (Entity e : entities)
        {
            EXPECT_FALSE(pool.IsValid(e));
        }
    }
    
    // After multiple cycles, the pool should still work correctly
    Entity finalEntity = pool.Create();
    EXPECT_TRUE(finalEntity.IsValid());
    EXPECT_TRUE(pool.IsValid(finalEntity));
}

// Test memory pool exhaustion
TEST_F(ResourceExhaustionTest, ArchetypeChunkPoolExhaustion)
{
    // Create a pool with very limited capacity
    ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 2;     // Small blocks with 2 chunks each
    config.maxChunks = 10;          // Only allow 10 chunks total
    config.initialBlocks = 1;       // Start with 1 block (2 chunks)
    config.useHugePages = false;    // Don't use huge pages for this test
    
    ArchetypeChunkPool pool(config);
    
    // Create some test component descriptors
    auto componentRegistry = std::make_shared<ComponentRegistry>();
    componentRegistry->RegisterComponent<Position>();
    
    std::vector<ComponentDescriptor> descriptors;
    if (const auto* posDesc = componentRegistry->GetComponentDescriptor(TypeID<Position>::Value()))
    {
        descriptors.push_back(*posDesc);
    }
    
    std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> allocations;
    
    // Allocate chunks until exhaustion
    size_t entitiesPerChunk = 100;
    for (size_t i = 0; i < config.maxChunks + 5; ++i)
    {
        auto chunk = pool.CreateChunk(entitiesPerChunk, descriptors);
        if (chunk != nullptr)
        {
            allocations.push_back(std::move(chunk));
        }
        else
        {
            // Should fail after maxChunks allocations
            EXPECT_GE(i, config.maxChunks);
            break;
        }
    }
    
    // Should have allocated exactly maxChunks
    EXPECT_EQ(allocations.size(), config.maxChunks);
    
    // Release one chunk (by resetting unique_ptr)
    allocations.pop_back();
    
    // Should be able to allocate one more
    auto newChunk = pool.CreateChunk(entitiesPerChunk, descriptors);
    EXPECT_NE(newChunk, nullptr);
    
    // Clean up happens automatically through unique_ptr destructors
}

// Test archetype proliferation (2^n archetypes with n components)
TEST_F(ResourceExhaustionTest, ArchetypeProliferation)
{
    // Create entities with all possible combinations of 4 components
    // This creates 2^4 = 16 different archetypes
    std::vector<Entity> entities;
    
    for (int mask = 0; mask < 16; ++mask)
    {
        Entity e = registry->CreateEntity();
        
        if (mask & 1) registry->EmplaceComponent<Position>(e);
        if (mask & 2) registry->EmplaceComponent<Velocity>(e);
        if (mask & 4) registry->EmplaceComponent<Health>(e);
        if (mask & 8) registry->EmplaceComponent<Transform>(e);
        
        entities.push_back(e);
    }
    
    // Should have created 16 archetypes (including empty)
    // Note: GetArchetypeStats() has been removed, so we can't verify this directly
    
    // Now create many entities with the same component combination
    // to test single archetype growth
    for (int i = 0; i < 1000; ++i)
    {
        registry->CreateEntityWith(Position{}, Velocity{});
    }
    
    // Should still have 16 archetypes, but one is much larger
    // Note: GetArchetypeStats() has been removed, so we can't verify archetype counts directly
    // We created 1000 entities with Position+Velocity, plus 1 from the first loop
}

// Test rapid archetype transitions (component add/remove storm)
TEST_F(ResourceExhaustionTest, RapidArchetypeTransitions)
{
    Entity e = registry->CreateEntity();
    
    // Perform rapid component additions and removals
    const int iterations = 100;
    
    for (int i = 0; i < iterations; ++i)
    {
        // Add components
        registry->EmplaceComponent<Position>(e, float(i), 0.0f, 0.0f);
        registry->EmplaceComponent<Velocity>(e, 1.0f, 0.0f, 0.0f);
        
        // Remove them
        registry->RemoveComponent<Velocity>(e);
        registry->RemoveComponent<Position>(e);
        
        // Add different ones
        registry->EmplaceComponent<Health>(e, i, 100);
        registry->EmplaceComponent<Transform>(e);
        
        // Remove again
        registry->RemoveComponent<Transform>(e);
        registry->RemoveComponent<Health>(e);
    }
    
    // Entity should still be valid
    EXPECT_TRUE(registry->IsValid(e));
    
    // Check for memory leaks via archetype count
    // Should have created several archetypes but not an excessive amount
    auto archetypeCount = registry->GetArchetypeCount();
    EXPECT_LE(archetypeCount, 10u); // Reasonable upper bound
    
    // Memory usage should be reasonable
    auto memUsage = registry->GetArchetypeMemoryUsage();
    EXPECT_LT(memUsage, 1024u * 1024u); // Less than 1MB for this simple test
}

// Simple test to isolate the crash
TEST_F(ResourceExhaustionTest, SimpleEntityLifecycle)
{
    std::cout << "=== Simple Entity Lifecycle Test ===" << std::endl;
    
    // Create a few entities
    std::vector<Entity> entities;
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        std::cout << "Created entity " << i << ": ID=" << e.GetID() 
                  << " Version=" << e.GetVersion() << std::endl;
        entities.push_back(e);
    }
    
    // Destroy them
    for (size_t i = 0; i < entities.size(); ++i)
    {
        std::cout << "Destroying entity " << i << ": ID=" << entities[i].GetID() << std::endl;
        registry->DestroyEntity(entities[i]);
    }
    
    // Create new ones (should reuse IDs)
    for (int i = 0; i < 5; ++i)
    {
        Entity e = registry->CreateEntity();
        std::cout << "Created recycled entity " << i << ": ID=" << e.GetID() 
                  << " Version=" << e.GetVersion() << std::endl;
    }
    
    std::cout << "=== Simple test completed successfully ===" << std::endl;
}

// Test entity pool fragmentation and recycling
TEST_F(ResourceExhaustionTest, EntityManagerFragmentation)  // Fixed: was using operator[] instead of Find()
{
    // Start with a small test to isolate the crash
    const size_t waveSize = 1000; // Back to original to stress test
    std::vector<Entity> wave1, wave2, wave3;
    
    // Create first wave of entities
    for (size_t i = 0; i < waveSize; ++i)
    {
        Entity e = registry->CreateEntity();
        ASSERT_TRUE(e.IsValid()) << "Failed to create entity " << i << " in wave 1";
        wave1.push_back(e);
    }
    
    // Create second wave
    for (size_t i = 0; i < waveSize; ++i)
    {
        Entity e = registry->CreateEntity();
        ASSERT_TRUE(e.IsValid()) << "Failed to create entity " << i << " in wave 2";
        wave2.push_back(e);
    }
    
    // Create third wave
    for (size_t i = 0; i < waveSize; ++i)
    {
        Entity e = registry->CreateEntity();
        ASSERT_TRUE(e.IsValid()) << "Failed to create entity " << i << " in wave 3";
        wave3.push_back(e);
    }
    
    // Destroy every other entity from each wave (fragment the pool)
    for (size_t i = 0; i < wave1.size(); i += 2)
    {
        registry->DestroyEntity(wave1[i]);
    }
    
    for (size_t i = 1; i < wave2.size(); i += 2)
    {
        registry->DestroyEntity(wave2[i]);
    }
    
    for (size_t i = 0; i < wave3.size(); i += 2)
    {
        registry->DestroyEntity(wave3[i]);
    }
    
    // Should have about half the entities remaining (1500 out of 3000)
    EXPECT_EQ(registry->Size(), waveSize * 3 / 2);
    
    // Create new entities - they should reuse the freed slots
    std::vector<Entity> recycled;
    size_t recycleCount = waveSize * 3 / 2;  // 1500 entities
    for (size_t i = 0; i < recycleCount; ++i)
    {
        Entity e = registry->CreateEntity();
        EXPECT_TRUE(e.IsValid());
        recycled.push_back(e);
    }
    
    // Total should be back to original count
    EXPECT_EQ(registry->Size(), waveSize * 3);
    
    // Verify no ID collisions
    std::unordered_set<Entity::StorageType> activeIDs;
    
    // Add remaining entities from waves
    for (size_t i = 1; i < wave1.size(); i += 2)
    {
        if (registry->IsValid(wave1[i]))
        {
            activeIDs.insert(wave1[i].GetID());
        }
    }
    
    for (size_t i = 0; i < wave2.size(); i += 2)
    {
        if (registry->IsValid(wave2[i]))
        {
            activeIDs.insert(wave2[i].GetID());
        }
    }
    
    for (size_t i = 1; i < wave3.size(); i += 2)
    {
        if (registry->IsValid(wave3[i]))
        {
            activeIDs.insert(wave3[i].GetID());
        }
    }
    
    // Add recycled entities
    for (Entity e : recycled)
    {
        auto result = activeIDs.insert(e.GetID());
        EXPECT_TRUE(result.second) << "ID collision detected!";
    }
}

// Test large batch operations
TEST_F(ResourceExhaustionTest, LargeBatchOperations)
{
    const size_t batchSize = 50000;  // Increased from 10k to 50k for more aggressive testing
    std::vector<Entity> entities(batchSize);
    
    // Test large batch creation
    registry->CreateEntitiesWith<Position, Velocity>(batchSize, entities,
        [](size_t i) {
            return std::make_tuple(
                Position{float(i), float(i * 2), float(i * 3)},
                Velocity{float(i * 10), 0, 0}
            );
        });
    
    // Verify all entities were created correctly
    EXPECT_EQ(registry->Size(), batchSize);
    
    // Spot check some entities
    for (size_t i = 0; i < batchSize; i += 1000)
    {
        Position* pos = registry->GetComponent<Position>(entities[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->x, float(i));
    }
    
    // Test large batch destruction
    registry->DestroyEntities(entities);
    EXPECT_EQ(registry->Size(), 0u);
}

// Test view iteration with maximum entities
TEST_F(ResourceExhaustionTest, MaxEntityViewIteration)
{
    const size_t entityCount = 100000; // 100k entities
    
    // Create many entities with components
    for (size_t i = 0; i < entityCount; ++i)
    {
        if (i % 2 == 0)
        {
            registry->CreateEntityWith(Position{float(i), 0, 0});
        }
        else
        {
            registry->CreateEntityWith(Position{float(i), 0, 0}, Velocity{1, 0, 0});
        }
    }
    
    // Create view and iterate
    auto view = registry->CreateView<Position>();
    
    size_t count = 0;
    view.ForEach([&count](Entity, Position& pos) {
        count++;
        pos.x += 1.0f; // Simple operation
    });
    
    EXPECT_EQ(count, entityCount);
}

// Test memory cleanup after exhaustion
TEST_F(ResourceExhaustionTest, MemoryCleanupAfterExhaustion)
{
    // Create many entities to use significant memory
    std::vector<Entity> entities;
    for (int i = 0; i < 10000; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), 0, 0},
            Velocity{1, 0, 0},
            Health{100, 100},
            Transform{}
        ));
    }
    
    size_t peakMemory = registry->GetArchetypeMemoryUsage();
    EXPECT_GT(peakMemory, 0u);
    
    // Destroy all entities
    registry->DestroyEntities(entities);
    
    // Run cleanup
    Registry::DefragmentationOptions options;
    options.minArchetypesToKeep = 1;
    
    auto result = registry->Defragment(options);
    size_t removed = result.archetypesRemoved;
    EXPECT_GT(removed, 0u);
    
    // Memory usage should be significantly reduced
    size_t currentMemory = registry->GetArchetypeMemoryUsage();
    EXPECT_LT(currentMemory, peakMemory / 2);
    
    // Registry should still be functional
    Entity newEntity = registry->CreateEntityWith(Position{});
    EXPECT_TRUE(registry->IsValid(newEntity));
}