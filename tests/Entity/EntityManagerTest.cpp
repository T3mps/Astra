#include <gtest/gtest.h>
#include "Astra/Entity/EntityManager.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <thread>
#include <chrono>

class EntityManagerTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test default construction
TEST_F(EntityManagerTest, DefaultConstruction)
{
    Astra::EntityManager pool;
    
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(pool.Capacity(), 0u);
    EXPECT_TRUE(pool.Empty());
    EXPECT_EQ(pool.RecycledCount(), 0u);
}

// Test construction with capacity
TEST_F(EntityManagerTest, ConstructionWithCapacity)
{
    Astra::EntityManager pool(1000);
    
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_TRUE(pool.Empty());
    // Capacity is allocated lazily, so it stays 0 until entities are created
    EXPECT_EQ(pool.Capacity(), 0u);
}

// Test construction with custom memory config
TEST_F(EntityManagerTest, ConstructionWithMemoryConfig)
{
    Astra::EntityManager::Config config(1024);  // Smaller segments for testing
    config.tableConfig.autoRelease = true;
    config.tableConfig.maxEmptySegments = 1;
    
    Astra::EntityManager pool(config);
    
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_TRUE(pool.Empty());
}

// Test single entity creation
TEST_F(EntityManagerTest, SingleEntityCreation)
{
    Astra::EntityManager pool;
    
    Astra::Entity entity = pool.Create();
    
    EXPECT_TRUE(entity.IsValid());
    EXPECT_EQ(entity.GetVersion(), 1u);  // Initial version
    EXPECT_EQ(pool.Size(), 1u);
    EXPECT_FALSE(pool.Empty());
    EXPECT_EQ(pool.Capacity(), 1u);
}

// Test multiple entity creation
TEST_F(EntityManagerTest, MultipleEntityCreation)
{
    Astra::EntityManager pool;
    std::vector<Astra::Entity> entities;
    
    const size_t count = 100;
    for (size_t i = 0; i < count; ++i)
    {
        entities.push_back(pool.Create());
    }
    
    EXPECT_EQ(pool.Size(), count);
    EXPECT_EQ(pool.Capacity(), count);
    
    // All entities should be valid and unique
    for (const auto& entity : entities)
    {
        EXPECT_TRUE(entity.IsValid());
        EXPECT_TRUE(pool.IsValid(entity));
    }
    
    // Check uniqueness
    std::unordered_set<Astra::Entity, Astra::EntityHash> uniqueSet(entities.begin(), entities.end());
    EXPECT_EQ(uniqueSet.size(), count);
}

// Test batch entity creation
TEST_F(EntityManagerTest, BatchEntityCreation)
{
    Astra::EntityManager pool;
    
    const size_t batchSize = 1000;
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(batchSize, std::back_inserter(entities));
    
    EXPECT_EQ(entities.size(), batchSize);
    EXPECT_EQ(pool.Size(), batchSize);
    
    // All should be valid and unique
    for (const auto& entity : entities)
    {
        EXPECT_TRUE(entity.IsValid());
        EXPECT_TRUE(pool.IsValid(entity));
    }
    
    // Check uniqueness
    std::unordered_set<Astra::Entity, Astra::EntityHash> uniqueSet(entities.begin(), entities.end());
    EXPECT_EQ(uniqueSet.size(), batchSize);
    
    // IDs should be sequential for fresh batch
    for (size_t i = 1; i < entities.size(); ++i)
    {
        EXPECT_EQ(entities[i].GetID(), entities[i-1].GetID() + 1);
    }
}

// Test entity destruction
TEST_F(EntityManagerTest, EntityDestruction)
{
    Astra::EntityManager pool;
    
    Astra::Entity entity = pool.Create();
    EXPECT_TRUE(pool.IsValid(entity));
    EXPECT_EQ(pool.Size(), 1u);
    
    bool destroyed = pool.Destroy(entity);
    EXPECT_TRUE(destroyed);
    EXPECT_FALSE(pool.IsValid(entity));
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_TRUE(pool.Empty());
    EXPECT_EQ(pool.RecycledCount(), 1u);
}

// Test destroying invalid entity
TEST_F(EntityManagerTest, DestroyInvalidEntity)
{
    Astra::EntityManager pool;
    
    Astra::Entity invalid;
    bool destroyed = pool.Destroy(invalid);
    EXPECT_FALSE(destroyed);
    
    // Destroy entity with wrong version
    Astra::Entity entity = pool.Create();
    Astra::Entity wrongVersion(entity.GetID(), 99);
    destroyed = pool.Destroy(wrongVersion);
    EXPECT_FALSE(destroyed);
    EXPECT_TRUE(pool.IsValid(entity));  // Original still valid
}

// Test entity recycling
TEST_F(EntityManagerTest, EntityRecycling)
{
    Astra::EntityManager pool;
    
    // Create and destroy an entity
    Astra::Entity first = pool.Create();
    auto firstId = first.GetID();
    auto firstVersion = first.GetVersion();
    
    pool.Destroy(first);
    EXPECT_FALSE(pool.IsValid(first));
    
    // Create new entity - should reuse the ID with incremented version
    Astra::Entity second = pool.Create();
    EXPECT_EQ(second.GetID(), firstId);
    EXPECT_EQ(second.GetVersion(), firstVersion + 1);
    EXPECT_TRUE(pool.IsValid(second));
    EXPECT_FALSE(pool.IsValid(first));  // Old version still invalid
}

// Test version wraparound
TEST_F(EntityManagerTest, VersionWraparound)
{
    Astra::EntityManager pool;
    
    // Create entity with max version (simulate many recycles)
    Astra::Entity entity = pool.Create();
    auto id = entity.GetID();
    
    // Destroy and recreate many times to test version increment
    for (int i = 0; i < 10; ++i)
    {
        pool.Destroy(entity);
        entity = pool.Create();
        EXPECT_EQ(entity.GetID(), id);
        EXPECT_GT(entity.GetVersion(), 0u);  // Version should never be 0 (NULL_VERSION)
    }
}

// Test batch destruction
TEST_F(EntityManagerTest, BatchDestruction)
{
    Astra::EntityManager pool;
    
    // Create entities
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(100, std::back_inserter(entities));
    
    // Destroy half of them
    std::vector<Astra::Entity> toDestroy(entities.begin(), entities.begin() + 50);
    size_t destroyed = pool.DestroyBatch(toDestroy.begin(), toDestroy.end());
    
    EXPECT_EQ(destroyed, 50u);
    EXPECT_EQ(pool.Size(), 50u);
    
    // First half should be invalid
    for (size_t i = 0; i < 50; ++i)
    {
        EXPECT_FALSE(pool.IsValid(entities[i]));
    }
    
    // Second half should still be valid
    for (size_t i = 50; i < 100; ++i)
    {
        EXPECT_TRUE(pool.IsValid(entities[i]));
    }
}

// Test IsValid method
TEST_F(EntityManagerTest, IsValidMethod)
{
    Astra::EntityManager pool;
    
    // Invalid entity
    Astra::Entity invalid;
    EXPECT_FALSE(pool.IsValid(invalid));
    
    // Valid entity
    Astra::Entity valid = pool.Create();
    EXPECT_TRUE(pool.IsValid(valid));
    
    // After destruction
    pool.Destroy(valid);
    EXPECT_FALSE(pool.IsValid(valid));
    
    // Wrong version
    Astra::Entity wrongVersion(valid.GetID(), 99);
    EXPECT_FALSE(pool.IsValid(wrongVersion));
}

// Test GetVersion method
TEST_F(EntityManagerTest, GetVersionMethod)
{
    Astra::EntityManager pool;
    
    Astra::Entity entity = pool.Create();
    auto id = entity.GetID();
    
    EXPECT_EQ(pool.GetVersion(id), entity.GetVersion());
    
    pool.Destroy(entity);
    EXPECT_EQ(pool.GetVersion(id), 0u);  // NULL_VERSION after destruction
    
    // Invalid ID
    EXPECT_EQ(pool.GetVersion(99999), 0u);
}

// Test Clear method
TEST_F(EntityManagerTest, ClearMethod)
{
    Astra::EntityManager pool;
    
    // Create some entities
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(100, std::back_inserter(entities));
    
    EXPECT_EQ(pool.Size(), 100u);
    EXPECT_FALSE(pool.Empty());
    
    pool.Clear();
    
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(pool.Capacity(), 0u);
    EXPECT_TRUE(pool.Empty());
    
    // All entities should be invalid
    for (const auto& entity : entities)
    {
        EXPECT_FALSE(pool.IsValid(entity));
    }
}

// Test Reserve method
TEST_F(EntityManagerTest, ReserveMethod)
{
    Astra::EntityManager pool;
    
    // Reserve doesn't pre-allocate entities, just prepares internal structures
    pool.Reserve(10000);
    
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_TRUE(pool.Empty());
    
    // Should be able to create many entities efficiently
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(10000, std::back_inserter(entities));
    
    EXPECT_EQ(pool.Size(), 10000u);
}

// Test iterator functionality - REMOVED: EntityManager iterators have been removed
// TEST_F(EntityManagerTest, IteratorFunctionality)
// {
//     Astra::EntityManager pool;
//     
//     // Empty pool
//     EXPECT_EQ(pool.begin(), pool.end());
//     
//     // Create entities
//     std::vector<Astra::Entity> entities;
//     pool.CreateBatch(10, std::back_inserter(entities));
//     
//     // Iterate and count
//     size_t count = 0;
//     for (auto it = pool.begin(); it != pool.end(); ++it)
//     {
//         EXPECT_TRUE(pool.IsValid(*it));
//         ++count;
//     }
//     EXPECT_EQ(count, 10u);
//     
//     // Range-based for loop
//     count = 0;
//     for (Astra::Entity entity : pool)
//     {
//         EXPECT_TRUE(pool.IsValid(entity));
//         ++count;
//     }
//     EXPECT_EQ(count, 10u);
//     
//     // Destroy some entities
//     pool.Destroy(entities[2]);
//     pool.Destroy(entities[5]);
//     pool.Destroy(entities[8]);
//     
//     // Iterator should skip destroyed entities
//     count = 0;
//     for (Astra::Entity entity : pool)
//     {
//         EXPECT_TRUE(pool.IsValid(entity));
//         ++count;
//     }
//     EXPECT_EQ(count, 7u);
// }

// Test segmented memory allocation
TEST_F(EntityManagerTest, SegmentedMemoryAllocation)
{
    Astra::EntityManager::Config config(1024);  // Small segments
    Astra::EntityManager pool(config);
    
    // Create entities across multiple segments
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(3000, std::back_inserter(entities));  // Should span 3 segments
    
    EXPECT_EQ(pool.Size(), 3000u);
    
    // All should be valid
    for (const auto& entity : entities)
    {
        EXPECT_TRUE(pool.IsValid(entity));
    }
    
    // Destroy entities from different segments
    pool.Destroy(entities[10]);     // First segment
    pool.Destroy(entities[1500]);   // Second segment
    pool.Destroy(entities[2500]);   // Third segment
    
    EXPECT_EQ(pool.Size(), 2997u);
}

// Test memory release functionality
TEST_F(EntityManagerTest, MemoryRelease)
{
    Astra::EntityManager::Config config(1024);
    config.tableConfig.autoRelease = true;
    config.tableConfig.maxEmptySegments = 0;  // Release immediately
    
    Astra::EntityManager pool(config);
    
    // Create entities in multiple segments
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(2048, std::back_inserter(entities));  // 2 segments
    
    // Destroy all entities in first segment
    for (size_t i = 0; i < 1024; ++i)
    {
        pool.Destroy(entities[i]);
    }
    
    // First segment should be released
    // MaybeReleaseSegments is now internal to EntityTable
    // It's called automatically when entities are destroyed
    
    // Second segment entities should still be valid
    for (size_t i = 1024; i < 2048; ++i)
    {
        EXPECT_TRUE(pool.IsValid(entities[i]));
    }
}

// Test ShrinkToFit method
TEST_F(EntityManagerTest, ShrinkToFit)
{
    Astra::EntityManager pool;
    
    // Create and destroy many entities
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(1000, std::back_inserter(entities));
    
    // Destroy most
    for (size_t i = 0; i < 900; ++i)
    {
        pool.Destroy(entities[i]);
    }
    
    EXPECT_EQ(pool.Size(), 100u);
    EXPECT_EQ(pool.RecycledCount(), 900u);
    
    pool.ShrinkToFit();
    
    // Remaining entities should still be valid
    for (size_t i = 900; i < 1000; ++i)
    {
        EXPECT_TRUE(pool.IsValid(entities[i]));
    }
}

// Test large scale operations
TEST_F(EntityManagerTest, LargeScaleOperations)
{
    Astra::EntityManager pool;
    
    const size_t largeCount = 100000;
    std::vector<Astra::Entity> entities;
    entities.reserve(largeCount);
    
    // Batch create
    pool.CreateBatch(largeCount, std::back_inserter(entities));
    
    EXPECT_EQ(pool.Size(), largeCount);
    EXPECT_EQ(entities.size(), largeCount);
    
    // All should be valid
    for (size_t i = 0; i < std::min(size_t(1000), largeCount); ++i)
    {
        EXPECT_TRUE(pool.IsValid(entities[i]));
    }
    
    // Batch destroy half
    size_t destroyed = pool.DestroyBatch(entities.begin(), entities.begin() + largeCount/2);
    
    EXPECT_EQ(destroyed, largeCount/2);
    EXPECT_EQ(pool.Size(), largeCount/2);
}

// Test recycling pattern
TEST_F(EntityManagerTest, RecyclingPattern)
{
    Astra::EntityManager pool;
    
    // Simulate game loop: create, destroy, recycle
    for (int frame = 0; frame < 10; ++frame)
    {
        std::vector<Astra::Entity> frameEntities;
        
        // Create some entities
        pool.CreateBatch(100, std::back_inserter(frameEntities));
        
        // Destroy half
        for (size_t i = 0; i < 50; ++i)
        {
            pool.Destroy(frameEntities[i]);
        }
        
        // Create more (should recycle)
        pool.CreateBatch(25, std::back_inserter(frameEntities));
    }
    
    // Should have efficient recycling
    EXPECT_GT(pool.RecycledCount(), 0u);
}

// Test entity ID limits
TEST_F(EntityManagerTest, EntityIDLimits)
{
    Astra::EntityManager::Config config(1024);
    Astra::EntityManager pool(config);
    
    // Create many entities to test ID allocation
    std::vector<Astra::Entity> entities;
    const size_t count = 10000;
    pool.CreateBatch(count, std::back_inserter(entities));
    
    // All IDs should be within valid range
    for (const auto& entity : entities)
    {
        EXPECT_LE(entity.GetID(), Astra::Entity::ID_MASK);
        EXPECT_GT(entity.GetVersion(), 0u);
    }
}
// Test empty batch operations
TEST_F(EntityManagerTest, EmptyBatchOperations)
{
    Astra::EntityManager pool;
    
    // Empty batch creation
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(0, std::back_inserter(entities));
    EXPECT_EQ(entities.size(), 0u);
    EXPECT_EQ(pool.Size(), 0u);
    
    // Empty batch destruction
    size_t destroyed = pool.DestroyBatch(entities.begin(), entities.end());
    EXPECT_EQ(destroyed, 0u);
}

// Test Validate method (debug builds)
TEST_F(EntityManagerTest, ValidateMethod)
{
    Astra::EntityManager pool;
    
    // Should not crash on empty pool
    pool.Validate();
    
    // Create entities
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(100, std::back_inserter(entities));
    pool.Validate();
    
    // Destroy some
    pool.DestroyBatch(entities.begin(), entities.begin() + 50);
    pool.Validate();
    
    // Clear
    pool.Clear();
    pool.Validate();
}

// Test recycled count tracking
TEST_F(EntityManagerTest, RecycledCountTracking)
{
    Astra::EntityManager pool;
    
    EXPECT_EQ(pool.RecycledCount(), 0u);
    
    // Create and destroy entities
    std::vector<Astra::Entity> entities;
    pool.CreateBatch(100, std::back_inserter(entities));
    
    pool.DestroyBatch(entities.begin(), entities.begin() + 50);
    EXPECT_EQ(pool.RecycledCount(), 50u);
    
    // Reuse some
    pool.CreateBatch(25, std::back_inserter(entities));
    EXPECT_EQ(pool.RecycledCount(), 25u);
    
    // Clear resets recycled count
    pool.Clear();
    EXPECT_EQ(pool.RecycledCount(), 0u);
}

// Test custom segment size in MemoryConfig
TEST_F(EntityManagerTest, CustomSegmentSize)
{
    // Test with various segment sizes (minimum enforced is 1024)
    std::vector<size_t> segmentSizes = {512, 1024, 2048, 4096, 8192};
    
    for (size_t segSize : segmentSizes)
    {
        Astra::EntityManager::Config config(static_cast<Astra::EntityManager::IDType>(segSize));
        
        // Verify it was rounded to power of 2 and enforced minimum of 1024
        size_t expected = std::max(size_t(1024), std::bit_floor(segSize));
        EXPECT_EQ(config.tableConfig.entitiesPerSegment, static_cast<Astra::EntityManager::IDType>(expected));
        
        // Verify shift and mask are correct
        EXPECT_EQ(config.tableConfig.entitiesPerSegmentMask, config.tableConfig.entitiesPerSegment - 1);
        EXPECT_EQ(1u << config.tableConfig.entitiesPerSegmentShift, config.tableConfig.entitiesPerSegment);
        
        Astra::EntityManager pool(config);
        
        // Create entities spanning multiple segments
        std::vector<Astra::Entity> entities;
        pool.CreateBatch(config.tableConfig.entitiesPerSegment * 2 + 100, std::back_inserter(entities));
        
        // All should be valid
        for (const auto& entity : entities)
        {
            EXPECT_TRUE(pool.IsValid(entity));
        }
    }
}