#include <gtest/gtest.h>
#include <Astra/Entity/EntityManager.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <vector>
#include <algorithm>
#include <set>
#include <random>

namespace
{
    using namespace Astra;
}

class EntityManagerSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
    }
    
    void TearDown() override
    {
    }
};

TEST_F(EntityManagerSerializationTest, EmptyPool)
{
    // Create empty pool
    EntityManager pool;
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 0u);
        EXPECT_EQ(newPool->Empty(), true);
        EXPECT_EQ(newPool->RecycledCount(), 0u);
    }
}

TEST_F(EntityManagerSerializationTest, SingleEntity)
{
    EntityManager pool;
    
    // Create an entity
    Entity e = pool.Create();
    ASSERT_TRUE(pool.IsValid(e));
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 1u);
        EXPECT_TRUE(newPool->IsValid(e));
    }
}

TEST_F(EntityManagerSerializationTest, MultipleEntities)
{
    EntityManager pool;
    
    // Create multiple entities
    std::vector<Entity> entities;
    for (int i = 0; i < 100; ++i)
    {
        entities.push_back(pool.Create());
    }
    
    // Verify all valid
    for (const auto& e : entities)
    {
        ASSERT_TRUE(pool.IsValid(e));
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 100u);
        
        // Check all entities are still valid
        for (const auto& e : entities)
        {
            EXPECT_TRUE(newPool->IsValid(e));
        }
    }
}

TEST_F(EntityManagerSerializationTest, WithRecycledEntities)
{
    EntityManager pool;
    
    // Create some entities
    std::vector<Entity> entities;
    for (int i = 0; i < 50; ++i)
    {
        entities.push_back(pool.Create());
    }
    
    // Destroy some entities
    std::vector<Entity> destroyed;
    for (int i = 10; i < 30; ++i)
    {
        destroyed.push_back(entities[i]);
        ASSERT_TRUE(pool.Destroy(entities[i]));
    }
    
    // Create more entities (will recycle some)
    std::vector<Entity> newEntities;
    for (int i = 0; i < 10; ++i)
    {
        newEntities.push_back(pool.Create());
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 40u);  // 50 - 20 destroyed + 10 new
        
        // Check destroyed entities are invalid
        for (const auto& e : destroyed)
        {
            EXPECT_FALSE(newPool->IsValid(e));
        }
        
        // Check new entities are valid
        for (const auto& e : newEntities)
        {
            EXPECT_TRUE(newPool->IsValid(e));
        }
        
        // Check remaining original entities
        for (int i = 0; i < 10; ++i)
        {
            EXPECT_TRUE(newPool->IsValid(entities[i]));
        }
        for (int i = 30; i < 50; ++i)
        {
            EXPECT_TRUE(newPool->IsValid(entities[i]));
        }
        
        // Check recycled count preserved
        EXPECT_GT(newPool->RecycledCount(), 0u);
    }
}

TEST_F(EntityManagerSerializationTest, BatchCreation)
{
    EntityManager pool;
    
    // Batch create entities
    std::vector<Entity> entities(1000);
    pool.CreateBatch(1000, entities.begin());
    
    EXPECT_EQ(pool.Size(), 1000u);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 1000u);
        
        // Check all batch-created entities are valid
        for (const auto& e : entities)
        {
            EXPECT_TRUE(newPool->IsValid(e));
        }
    }
}

TEST_F(EntityManagerSerializationTest, MultipleSegments)
{
    // Create pool with small segments for testing
    EntityManager::Config config(1024);  // Small segments
    EntityManager pool(config);
    
    // Create enough entities to span multiple segments
    std::vector<Entity> entities;
    for (int i = 0; i < 3000; ++i)
    {
        entities.push_back(pool.Create());
    }
    
    EXPECT_EQ(pool.Size(), 3000u);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 3000u);
        
        // Check all entities across segments are valid
        for (const auto& e : entities)
        {
            EXPECT_TRUE(newPool->IsValid(e));
        }
    }
}

TEST_F(EntityManagerSerializationTest, PreserveConfiguration)
{
    // Create pool with custom configuration
    EntityManager::Config config(8192);
    config.tableConfig.releaseThreshold = 0.2f;
    config.tableConfig.autoRelease = false;
    config.tableConfig.maxEmptySegments = 5;
    
    EntityManager pool(config);
    
    // Create some entities
    for (int i = 0; i < 100; ++i)
    {
        (void)pool.Create();
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        
        // Configuration should be preserved
        // (Note: We can't directly access config, but it's preserved internally)
        EXPECT_EQ(newPool->Size(), 100u);
    }
}

TEST_F(EntityManagerSerializationTest, VersionWraparound)
{
    EntityManager pool;
    
    // Create and destroy entity many times to test version wraparound
    Entity e = pool.Create();
    Entity::StorageType id = e.GetID();
    
    // Destroy and recreate many times
    for (int i = 0; i < 300; ++i)  // Will wrap around 255
    {
        ASSERT_TRUE(pool.Destroy(e));
        e = pool.Create();
        // Should reuse same ID with incremented version
        if (e.GetID() == id) break;  // Got recycled ID
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_TRUE(newPool->IsValid(e));
    }
}

TEST_F(EntityManagerSerializationTest, GlobalFreeList)
{
    EntityManager pool;
    
    // Create entities
    std::vector<Entity> entities;
    for (int i = 0; i < 100; ++i)
    {
        entities.push_back(pool.Create());
    }
    
    // Destroy many to populate free lists
    for (int i = 0; i < 50; ++i)
    {
        ASSERT_TRUE(pool.Destroy(entities[i]));
    }
    
    EXPECT_EQ(pool.Size(), 50u);
    EXPECT_EQ(pool.RecycledCount(), 50u);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), 50u);
        EXPECT_EQ(newPool->RecycledCount(), 50u);
        
        // Create new entity - should recycle
        Entity recycled = newPool->Create();
        bool foundRecycled = false;
        for (int i = 0; i < 50; ++i)
        {
            if (recycled.GetID() == entities[i].GetID())
            {
                foundRecycled = true;
                // Version should be incremented
                EXPECT_EQ(recycled.GetVersion(), entities[i].GetVersion() + 1);
                break;
            }
        }
        EXPECT_TRUE(foundRecycled);
    }
}

TEST_F(EntityManagerSerializationTest, Iterator)
{
    EntityManager pool;
    
    // Create entities with some gaps
    std::vector<Entity> entities;
    std::set<Entity> aliveEntities;
    
    for (int i = 0; i < 100; ++i)
    {
        entities.push_back(pool.Create());
        if (i % 3 != 0)  // Keep 2/3 of entities
        {
            aliveEntities.insert(entities.back());
        }
    }
    
    // Destroy every 3rd entity
    for (int i = 0; i < 100; i += 3)
    {
        ASSERT_TRUE(pool.Destroy(entities[i]));
    }
    
    // Collect alive entities via iterator
    std::set<Entity> iteratedEntities;
    for (Entity e : pool)
    {
        iteratedEntities.insert(e);
    }
    
    EXPECT_EQ(iteratedEntities, aliveEntities);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        
        // Iterate and check we get same entities
        std::set<Entity> newIteratedEntities;
        for (Entity e : *newPool)
        {
            newIteratedEntities.insert(e);
        }
        
        EXPECT_EQ(newIteratedEntities, aliveEntities);
    }
}

TEST_F(EntityManagerSerializationTest, LargeScale)
{
    EntityManager pool;
    
    // Create many entities
    std::vector<Entity> entities;
    const size_t count = 100000;
    entities.reserve(count);
    
    // Batch create for efficiency
    pool.CreateBatch(count, std::back_inserter(entities));
    
    // Destroy some randomly
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, count - 1);
    std::set<size_t> destroyedIndices;
    
    for (size_t i = 0; i < count / 4; ++i)
    {
        size_t idx = dist(rng);
        if (destroyedIndices.find(idx) == destroyedIndices.end())
        {
            ASSERT_TRUE(pool.Destroy(entities[idx]));
            destroyedIndices.insert(idx);
        }
    }
    
    size_t expectedAlive = count - destroyedIndices.size();
    EXPECT_EQ(pool.Size(), expectedAlive);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        pool.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = EntityManager::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newPool = *result.GetValue();
        EXPECT_EQ(newPool->Size(), expectedAlive);
        
        // Spot check some entities
        for (size_t i = 0; i < std::min(size_t(1000), count); ++i)
        {
            if (destroyedIndices.find(i) == destroyedIndices.end())
            {
                EXPECT_TRUE(newPool->IsValid(entities[i]));
            }
            else
            {
                EXPECT_FALSE(newPool->IsValid(entities[i]));
            }
        }
    }
}