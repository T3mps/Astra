#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Archetype/Archetype.hpp"
#include "Astra/Serialization/BinaryWriter.hpp"
#include "Astra/Serialization/BinaryReader.hpp"

class ArchetypeTest : public ::testing::Test
{
protected:
    Astra::ComponentRegistry registry;
    Astra::ArchetypeChunkPool componentPool;
    
    void SetUp() override 
    {
        // Register test components
        using namespace Astra::Test;
        registry.RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();
    }
    
    void TearDown() override {}
    
    std::vector<Astra::ComponentDescriptor> GetDescriptors(const Astra::ComponentMask& mask)
    {
        std::vector<Astra::ComponentDescriptor> descriptors;
        for (Astra::ComponentID id = 0; id < Astra::MAX_COMPONENTS; ++id)
        {
            if (mask.Test(id))
            {
                const auto* desc = registry.GetComponentDescriptor(id);
                if (desc)
                {
                    descriptors.push_back(*desc);
                }
            }
        }
        return descriptors;
    }
};

// Test basic archetype creation and initialization
TEST_F(ArchetypeTest, BasicCreationAndInitialization)
{
    using namespace Astra::Test;
    
    // Create archetype with Position and Velocity components
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    // Initially not initialized
    EXPECT_FALSE(archetype.IsInitialized());
    EXPECT_EQ(archetype.GetEntityCount(), 0u);
    
    // Initialize with component descriptors
    auto descriptors = GetDescriptors(mask);
    EXPECT_EQ(descriptors.size(), 2u);
    
    archetype.Initialize(descriptors);
    
    // Should now be initialized
    EXPECT_TRUE(archetype.IsInitialized());
    EXPECT_EQ(archetype.GetEntityCount(), 0u);
    EXPECT_TRUE(archetype.HasComponent<Position>());
    EXPECT_TRUE(archetype.HasComponent<Velocity>());
    EXPECT_FALSE(archetype.HasComponent<Health>());
    
    // Check entities per chunk is reasonable
    EXPECT_GT(archetype.GetEntitiesPerChunk(), 0u);
    EXPECT_LE(archetype.GetEntitiesPerChunk(), 16384u); // Max reasonable for 16KB chunks
    
    // Should be power of 2 for fast indexing
    auto epc = archetype.GetEntitiesPerChunk();
    EXPECT_EQ(epc & (epc - 1), 0u) << "Entities per chunk should be power of 2";
}

// Test adding single entity
TEST_F(ArchetypeTest, AddSingleEntity)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add an entity
    Astra::Entity entity(1, 1);
    auto location = archetype.AddEntity(entity);
    
    // Should be valid location
    EXPECT_TRUE(location.IsValid());
    EXPECT_EQ(archetype.GetEntityCount(), 1u);
    
    // Should be able to get the entity back
    EXPECT_EQ(archetype.GetEntity(location), entity);
    
    // Should be in first chunk at index 0
    EXPECT_EQ(location.GetChunkIndex(), 0u);
    EXPECT_EQ(location.GetEntityIndex(), 0u);
}

// Test adding multiple entities
TEST_F(ArchetypeTest, AddMultipleEntities)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    std::vector<Astra::Entity> entities;
    std::vector<Astra::EntityLocation> locations;
    
    // Add 100 entities
    for (int i = 0; i < 100; ++i)
    {
        Astra::Entity entity(i, 1);
        entities.push_back(entity);
        locations.push_back(archetype.AddEntity(entity));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), 100u);
    
    // Verify all locations are valid and unique
    for (size_t i = 0; i < locations.size(); ++i)
    {
        EXPECT_TRUE(locations[i].IsValid());
        EXPECT_EQ(archetype.GetEntity(locations[i]), entities[i]);
        
        // Check uniqueness
        for (size_t j = i + 1; j < locations.size(); ++j)
        {
            EXPECT_NE(locations[i], locations[j]);
        }
    }
}

// Test batch adding entities
TEST_F(ArchetypeTest, BatchAddEntities)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Health>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Create batch of entities
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 50; ++i)
    {
        entities.emplace_back(i, 1);
    }
    
    // Batch add
    auto locations = archetype.AddEntities(entities);
    
    EXPECT_EQ(locations.size(), entities.size());
    EXPECT_EQ(archetype.GetEntityCount(), 50u);
    
    // Verify all entities
    for (size_t i = 0; i < entities.size(); ++i)
    {
        EXPECT_TRUE(locations[i].IsValid());
        EXPECT_EQ(archetype.GetEntity(locations[i]), entities[i]);
    }
}

// Test removing single entity
TEST_F(ArchetypeTest, RemoveSingleEntity)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add some entities
    std::vector<Astra::Entity> entities;
    std::vector<Astra::EntityLocation> locations;
    for (int i = 0; i < 5; ++i)
    {
        Astra::Entity entity(i, 1);
        entities.push_back(entity);
        locations.push_back(archetype.AddEntity(entity));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), 5u);
    
    // Remove middle entity (swap-and-pop)
    auto movedEntity = archetype.RemoveEntity(locations[2]);
    
    EXPECT_EQ(archetype.GetEntityCount(), 4u);
    
    // The last entity should have been moved to fill the gap
    if (movedEntity.has_value())
    {
        EXPECT_EQ(*movedEntity, entities[4]);
        // Entity at location[2] should now be the last entity
        EXPECT_EQ(archetype.GetEntity(locations[2]), entities[4]);
    }
}

// Test getting and setting components
TEST_F(ArchetypeTest, GetAndSetComponents)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add entity
    Astra::Entity entity(1, 1);
    auto location = archetype.AddEntity(entity);
    
    // Get component pointer
    Position* pos = archetype.GetComponent<Position>(location);
    ASSERT_NE(pos, nullptr);
    
    // Components should be default initialized
    EXPECT_EQ(pos->x, 0.0f);
    EXPECT_EQ(pos->y, 0.0f);
    EXPECT_EQ(pos->z, 0.0f);
    
    // Modify component
    pos->x = 10.0f;
    pos->y = 20.0f;
    pos->z = 30.0f;
    
    // Get again and verify
    Position* pos2 = archetype.GetComponent<Position>(location);
    EXPECT_EQ(pos2->x, 10.0f);
    EXPECT_EQ(pos2->y, 20.0f);
    EXPECT_EQ(pos2->z, 30.0f);
    
    // Set component using SetComponent
    archetype.SetComponent(location, Position{100.0f, 200.0f, 300.0f});
    Position* pos3 = archetype.GetComponent<Position>(location);
    EXPECT_EQ(pos3->x, 100.0f);
    EXPECT_EQ(pos3->y, 200.0f);
    EXPECT_EQ(pos3->z, 300.0f);
}

// Test ForEach iteration
TEST_F(ArchetypeTest, ForEachIteration)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add entities with specific values
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 10; ++i)
    {
        Astra::Entity entity(i, 1);
        auto location = archetype.AddEntity(entity);
        entities.push_back(entity);
        
        // Set components
        archetype.SetComponent(location, Position{float(i), float(i * 2), float(i * 3)});
        archetype.SetComponent(location, Velocity{float(i * 10), 0.0f, 0.0f});
    }
    
    // Iterate and verify
    int count = 0;
    archetype.ForEach<Position, Velocity>([&](Astra::Entity e, Position& pos, Velocity& vel) {
        // Find which entity this is
        auto it = std::find(entities.begin(), entities.end(), e);
        ASSERT_NE(it, entities.end());
        
        size_t idx = std::distance(entities.begin(), it);
        EXPECT_EQ(pos.x, float(idx));
        EXPECT_EQ(pos.y, float(idx * 2));
        EXPECT_EQ(pos.z, float(idx * 3));
        EXPECT_EQ(vel.dx, float(idx * 10));
        
        count++;
    });
    
    EXPECT_EQ(count, 10);
}

// Test chunk allocation and capacity
TEST_F(ArchetypeTest, ChunkAllocationAndCapacity)
{
    using namespace Astra::Test;
    
    // Use small components to get more entities per chunk
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    EXPECT_GT(entitiesPerChunk, 0u);
    
    // Add enough entities to require multiple chunks
    size_t totalEntities = entitiesPerChunk * 2 + 10;
    for (Astra::Entity::StorageType i = 0; i < totalEntities; ++i)
    {
        archetype.AddEntity(Astra::Entity(i, 1));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), totalEntities);
    
    // Should have at least 3 chunks
    const auto& chunks = archetype.GetChunks();
    EXPECT_GE(chunks.size(), 3u);
}

// Test empty archetype (no components)
TEST_F(ArchetypeTest, EmptyArchetype)
{
    // Create archetype with no components
    Astra::ComponentMask mask;
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    std::vector<Astra::ComponentDescriptor> descriptors; // Empty
    archetype.Initialize(descriptors);
    
    EXPECT_TRUE(archetype.IsInitialized());
    
    // Should still be able to add entities
    Astra::Entity entity(1, 1);
    auto location = archetype.AddEntity(entity);
    
    EXPECT_TRUE(location.IsValid());
    EXPECT_EQ(archetype.GetEntityCount(), 1u);
    EXPECT_EQ(archetype.GetEntity(location), entity);
}

TEST_F(ArchetypeTest, EntityLocationOperations)
{
    // Test creating and accessing entity location
    size_t chunkIdx = 5;
    size_t entityIdx = 123;
    
    auto location = Astra::EntityLocation::Create(chunkIdx, entityIdx);
    
    EXPECT_EQ(location.GetChunkIndex(), chunkIdx);
    EXPECT_EQ(location.GetEntityIndex(), entityIdx);
    EXPECT_TRUE(location.IsValid());
    
    // Test invalid location
    Astra::EntityLocation invalid;
    EXPECT_FALSE(invalid.IsValid());
}

// Test component mask operations
TEST_F(ArchetypeTest, ComponentMaskOperations)
{
    using namespace Astra::Test;
    
    auto mask1 = Astra::MakeComponentMask<Position>();
    auto mask2 = Astra::MakeComponentMask<Position, Velocity>();
    auto mask3 = Astra::MakeComponentMask<Position, Velocity, Health>();
    
    // mask1 should have Position
    EXPECT_TRUE(mask1.Test(Astra::TypeID<Position>::Value()));
    EXPECT_FALSE(mask1.Test(Astra::TypeID<Velocity>::Value()));
    
    // mask2 should have Position and Velocity
    EXPECT_TRUE(mask2.Test(Astra::TypeID<Position>::Value()));
    EXPECT_TRUE(mask2.Test(Astra::TypeID<Velocity>::Value()));
    EXPECT_FALSE(mask2.Test(Astra::TypeID<Health>::Value()));
    
    // mask3 should have all three
    EXPECT_TRUE(mask3.Test(Astra::TypeID<Position>::Value()));
    EXPECT_TRUE(mask3.Test(Astra::TypeID<Velocity>::Value()));
    EXPECT_TRUE(mask3.Test(Astra::TypeID<Health>::Value()));
    
    // mask2 should have all of mask1's components
    EXPECT_TRUE(mask2.HasAll(mask1));
    
    // mask1 should not have all of mask2's components
    EXPECT_FALSE(mask1.HasAll(mask2));
}

// Test batch removal of entities
TEST_F(ArchetypeTest, BatchRemoveEntities)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add entities
    std::vector<Astra::Entity> entities;
    std::vector<Astra::EntityLocation> locations;
    
    for (int i = 0; i < 20; ++i)
    {
        Astra::Entity entity(i, 1);
        entities.push_back(entity);
        auto loc = archetype.AddEntity(entity);
        locations.push_back(loc);
        
        // Set unique component values
        archetype.SetComponent(loc, Position{float(i), float(i), float(i)});
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), 20u);
    
    // Remove every other entity
    std::vector<Astra::EntityLocation> toRemove;
    for (size_t i = 0; i < locations.size(); i += 2)
    {
        toRemove.push_back(locations[i]);
    }
    
    auto movedEntities = archetype.RemoveEntities(toRemove);
    
    EXPECT_EQ(archetype.GetEntityCount(), 10u);
    
    // Verify remaining entities
    int remainingCount = 0;
    archetype.ForEach<Position>([&](Astra::Entity e, Position& pos) {
        remainingCount++;
        // Position values should match entity ID for remaining entities
        EXPECT_EQ(pos.x, float(e.GetID()));
    });
    
    EXPECT_EQ(remainingCount, 10);
}

// Test MoveEntityFrom for archetype transitions
TEST_F(ArchetypeTest, MoveEntityBetweenArchetypes)
{
    using namespace Astra::Test;
    
    // Create source archetype with Position only
    auto srcMask = Astra::MakeComponentMask<Position>();
    Astra::Archetype srcArchetype(srcMask);
    srcArchetype.SetComponentPool(&componentPool);
    srcArchetype.Initialize(GetDescriptors(srcMask));
    
    // Create destination archetype with Position and Velocity
    auto dstMask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype dstArchetype(dstMask);
    dstArchetype.SetComponentPool(&componentPool);
    dstArchetype.Initialize(GetDescriptors(dstMask));
    
    // Add entity to source
    Astra::Entity entity(42, 1);
    auto srcLoc = srcArchetype.AddEntity(entity);
    srcArchetype.SetComponent(srcLoc, Position{1.0f, 2.0f, 3.0f});
    
    // Add entity to destination (allocate space)
    auto dstLoc = dstArchetype.AddEntity(entity);
    
    // Move entity data from source to destination
    dstArchetype.MoveEntityFrom(dstLoc, srcArchetype, srcLoc);
    
    // Verify position was moved
    Position* pos = dstArchetype.GetComponent<Position>(dstLoc);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
    EXPECT_EQ(pos->y, 2.0f);
    EXPECT_EQ(pos->z, 3.0f);
    
    // Velocity should be default constructed
    Velocity* vel = dstArchetype.GetComponent<Velocity>(dstLoc);
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->dx, 0.0f);
    EXPECT_EQ(vel->dy, 0.0f);
    EXPECT_EQ(vel->dz, 0.0f);
}

// Test EnsureCapacity pre-allocation
TEST_F(ArchetypeTest, EnsureCapacity)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Pre-allocate for many entities
    size_t targetCount = 1000;
    archetype.EnsureCapacity(targetCount);
    
    // Add entities - should not need to allocate new chunks during loop
    for (size_t i = 0; i < targetCount; ++i)
    {
        archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(i), 1));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), targetCount);
}

// Test CalculateRemainingCapacity
TEST_F(ArchetypeTest, CalculateRemainingCapacity)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    
    // Initially should have capacity of first chunk
    size_t initialCapacity = archetype.GetRemainingCapacity();
    EXPECT_EQ(initialCapacity, entitiesPerChunk);
    
    // Add some entities
    size_t toAdd = entitiesPerChunk / 2;
    for (size_t i = 0; i < toAdd; ++i)
    {
        archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(i), 1));
    }
    
    // Remaining capacity should decrease
    size_t remainingCapacity = archetype.GetRemainingCapacity();
    EXPECT_EQ(remainingCapacity, entitiesPerChunk - toAdd);
}

// Test chunk metrics and coalescing
TEST_F(ArchetypeTest, ChunkCoalescing)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    
    // Fill multiple chunks
    size_t totalEntities = entitiesPerChunk * 3;
    std::vector<Astra::EntityLocation> locations;
    
    for (size_t i = 0; i < totalEntities; ++i)
    {
        locations.push_back(archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(i), 1)));
    }
    
    // Remove many entities to create sparse chunks
    std::vector<Astra::EntityLocation> toRemove;
    // Remove 80% of entities from chunks 1 and 2
    for (size_t i = entitiesPerChunk; i < totalEntities; ++i)
    {
        if (i % 5 != 0) // Keep every 5th entity
        {
            toRemove.push_back(locations[i]);
        }
    }
    
    archetype.RemoveEntities(toRemove);
    
    // Check if coalescing is needed
    bool needsCoalescing = archetype.ShouldCoalesce();
    // This depends on thresholds but with 80% removed, it should need coalescing
    
    if (needsCoalescing)
    {
        // Perform coalescing
        auto [chunksFreed, movedEntities] = archetype.CoalesceChunks();
        
        // Should have freed at least one chunk
        EXPECT_GT(chunksFreed, 0u);
    }
    
    // Verify all remaining entities are still accessible
    size_t remainingCount = 0;
    archetype.ForEach<Position>([&](Astra::Entity e, Position& pos) {
        remainingCount++;
    });
    
    EXPECT_GT(remainingCount, 0u);
}

// Test with different component sizes
TEST_F(ArchetypeTest, DifferentComponentSizes)
{
    using namespace Astra::Test;
    
    // Small components
    auto smallMask = Astra::MakeComponentMask<Player, Enemy>(); // Empty components
    Astra::Archetype smallArchetype(smallMask);
    smallArchetype.SetComponentPool(&componentPool);
    smallArchetype.Initialize(GetDescriptors(smallMask));
    
    // Large components  
    auto largeMask = Astra::MakeComponentMask<Transform, Name>(); // Transform has 16 floats, Name has string
    Astra::Archetype largeArchetype(largeMask);
    largeArchetype.SetComponentPool(&componentPool);
    largeArchetype.Initialize(GetDescriptors(largeMask));
    
    // Small components should fit more entities per chunk
    EXPECT_GT(smallArchetype.GetEntitiesPerChunk(), largeArchetype.GetEntitiesPerChunk());
    
    // Add entities to both
    for (int i = 0; i < 100; ++i)
    {
        smallArchetype.AddEntity(Astra::Entity(i, 1));
        largeArchetype.AddEntity(Astra::Entity(i, 1));
    }
    
    EXPECT_EQ(smallArchetype.GetEntityCount(), 100u);
    EXPECT_EQ(largeArchetype.GetEntityCount(), 100u);
    
    // Large archetype should need more chunks
    EXPECT_GE(largeArchetype.GetChunks().size(), smallArchetype.GetChunks().size());
}

// Test stress with many entities
TEST_F(ArchetypeTest, StressTestManyEntities)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity, Health>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    const size_t entityCount = 10000;
    
    // Batch add entities
    std::vector<Astra::Entity> entities;
    entities.reserve(entityCount);
    for (size_t i = 0; i < entityCount; ++i)
    {
        entities.emplace_back(static_cast<Astra::Entity::StorageType>(i), 1);
    }
    
    auto locations = archetype.AddEntities(entities);
    
    EXPECT_EQ(locations.size(), entityCount);
    EXPECT_EQ(archetype.GetEntityCount(), entityCount);
    
    // Set component values
    for (size_t i = 0; i < locations.size(); ++i)
    {
        archetype.SetComponent(locations[i], Position{float(i), 0, 0});
        archetype.SetComponent(locations[i], Health{int(i), 100});
    }
    
    // Iterate and verify
    size_t count = 0;
    archetype.ForEach<Position, Health>([&](Astra::Entity e, Position& pos, Health& health) {
        EXPECT_EQ(pos.x, float(e.GetID()));
        EXPECT_EQ(health.current, int(e.GetID()));
        count++;
    });
    
    EXPECT_EQ(count, entityCount);
}

// Test edge case: Adding entity when chunk is exactly full
TEST_F(ArchetypeTest, ChunkBoundaryConditions)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    
    // Fill exactly one chunk
    for (size_t i = 0; i < entitiesPerChunk; ++i)
    {
        archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(i), 1));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), entitiesPerChunk);
    EXPECT_EQ(archetype.GetChunks().size(), 1u);
    
    // Add one more - should create new chunk
    archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(entitiesPerChunk), 1));
    
    EXPECT_EQ(archetype.GetEntityCount(), entitiesPerChunk + 1);
    EXPECT_EQ(archetype.GetChunks().size(), 2u);
}

// Test component access patterns
TEST_F(ArchetypeTest, ComponentAccessPatterns)
{
    using namespace Astra::Test;
    
    auto mask = Astra::MakeComponentMask<Position, Velocity, Health>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    archetype.Initialize(GetDescriptors(mask));
    
    // Add entities
    const size_t count = 100;
    std::vector<Astra::EntityLocation> locations;
    
    for (size_t i = 0; i < count; ++i)
    {
        locations.push_back(archetype.AddEntity(Astra::Entity(static_cast<Astra::Entity::StorageType>(i), 1)));
    }
    
    // Test different access patterns
    
    // 1. Sequential write
    for (size_t i = 0; i < locations.size(); ++i)
    {
        archetype.SetComponent(locations[i], Position{float(i), float(i), float(i)});
    }
    
    // 2. Random access read
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), size_t(0));
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(indices.begin(), indices.end(), gen);
    
    for (size_t idx : indices)
    {
        Position* pos = archetype.GetComponent<Position>(locations[idx]);
        EXPECT_EQ(pos->x, float(idx));
    }
    
    // 3. Partial component iteration (only Position and Velocity, not Health)
    size_t iterCount = 0;
    archetype.ForEach<Position, Velocity>([&](Astra::Entity e, Position& pos, Velocity& vel) {
        vel.dx = pos.x * 2.0f;
        iterCount++;
    });
    
    EXPECT_EQ(iterCount, count);
}

// Test with maximum number of components
TEST_F(ArchetypeTest, MaximumComponents)
{
    using namespace Astra::Test;
    
    // Create mask with many components
    auto mask = Astra::MakeComponentMask<Position, Velocity, Health, Transform, Name, Physics>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    auto descriptors = GetDescriptors(mask);
    EXPECT_EQ(descriptors.size(), 6u);
    
    archetype.Initialize(descriptors);
    EXPECT_TRUE(archetype.IsInitialized());
    
    // Entities per chunk should be reduced due to many components
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    EXPECT_GT(entitiesPerChunk, 0u);
    
    // Add some entities
    for (int i = 0; i < 50; ++i)
    {
        archetype.AddEntity(Astra::Entity(i, 1));
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), 50u);
}

// Test serialization of empty archetype
TEST_F(ArchetypeTest, SerializeEmptyArchetype)
{
    using namespace Astra::Test;
    
    // Create archetype with Position and Velocity
    auto mask = Astra::MakeComponentMask<Position, Velocity>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    auto descriptors = GetDescriptors(mask);
    archetype.Initialize(descriptors);
    
    // Serialize the empty archetype
    std::vector<std::byte> buffer;
    {
        Astra::BinaryWriter writer(buffer);
        archetype.Serialize(writer);
    }
    
    // Deserialize into a new archetype
    {
        Astra::BinaryReader reader(buffer);
        
        // Get all component descriptors from registry for deserialization
        std::vector<Astra::ComponentDescriptor> registryDescriptors;
        registry.GetAllDescriptors(registryDescriptors);
        
        auto deserializedResult = Astra::Archetype::Deserialize(reader, registryDescriptors, &componentPool);

        ASSERT_TRUE(deserializedResult.IsOk()) << "Deserialization failed";
        auto deserializedArchetype = std::move(*deserializedResult.GetValue());

        // Verify the deserialized archetype matches
        EXPECT_EQ(deserializedArchetype->GetMask(), mask);
        EXPECT_EQ(deserializedArchetype->GetEntityCount(), 0u);
        EXPECT_EQ(deserializedArchetype->GetEntitiesPerChunk(), archetype.GetEntitiesPerChunk());
        EXPECT_TRUE(deserializedArchetype->IsInitialized());
    }
}

// Test serialization with entities and component data
TEST_F(ArchetypeTest, SerializeWithEntities)
{
    using namespace Astra::Test;
    
    // Create and populate archetype
    auto mask = Astra::MakeComponentMask<Position, Velocity, Health>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    auto descriptors = GetDescriptors(mask);
    archetype.Initialize(descriptors);
    
    // Add entities with component data
    const size_t entityCount = 100;
    std::vector<Astra::Entity> entities;
    std::vector<Astra::EntityLocation> locations;
    
    for (size_t i = 0; i < entityCount; ++i)
    {
        Astra::Entity entity(static_cast<Astra::Entity::StorageType>(i), 1);
        entities.push_back(entity);
        
        Astra::EntityLocation loc = archetype.AddEntity(entity);
        ASSERT_TRUE(loc.IsValid());
        locations.push_back(loc);
        
        // Set component data
        archetype.SetComponent(loc, Position{float(i), float(i * 2), float(i * 3)});
        archetype.SetComponent(loc, Velocity{float(i * 0.1f), float(i * 0.2f), float(i * 0.3f)});
        archetype.SetComponent(loc, Health{int(100 - i), 100});
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        Astra::BinaryWriter writer(buffer);
        archetype.Serialize(writer);
    }
    
    // Deserialize
    {
        Astra::BinaryReader reader(buffer);
        
        // Get all component descriptors from registry for deserialization
        std::vector<Astra::ComponentDescriptor> registryDescriptors;
        registry.GetAllDescriptors(registryDescriptors);
        
        auto deserializedResult = Astra::Archetype::Deserialize(reader, registryDescriptors, &componentPool);

        ASSERT_TRUE(deserializedResult.IsOk()) << "Deserialization failed";
        auto deserializedArchetype = std::move(*deserializedResult.GetValue());

        // Verify basic properties
        EXPECT_EQ(deserializedArchetype->GetMask(), mask);
        EXPECT_EQ(deserializedArchetype->GetEntityCount(), entityCount);
        
        // Verify component data is preserved by checking entities in order
        // Archetypes store entities in chunks, we need to iterate through them
        size_t verifiedCount = 0;
        const auto& chunks = deserializedArchetype->GetChunks();
        
        for (const auto& chunk : chunks)
        {
            size_t chunkEntityCount = chunk->GetCount();
            const auto& chunkEntities = chunk->GetEntities();
            
            for (size_t idx = 0; idx < chunkEntityCount; ++idx)
            {
                Astra::Entity entity = chunkEntities[idx];
                
                // Find original index of this entity
                auto origIt = std::find(entities.begin(), entities.end(), entity);
                ASSERT_NE(origIt, entities.end());
                size_t i = std::distance(entities.begin(), origIt);
                
                size_t chunkIdx = &chunk - &chunks[0];
                Astra::EntityLocation loc = Astra::EntityLocation::Create(
                    chunkIdx, idx);
                
                // Check Position
                Position* pos = deserializedArchetype->GetComponent<Position>(loc);
                ASSERT_NE(pos, nullptr);
                EXPECT_EQ(pos->x, float(i));
                EXPECT_EQ(pos->y, float(i * 2));
                EXPECT_EQ(pos->z, float(i * 3));
                
                // Check Velocity
                Velocity* vel = deserializedArchetype->GetComponent<Velocity>(loc);
                ASSERT_NE(vel, nullptr);
                EXPECT_FLOAT_EQ(vel->dx, float(i * 0.1f));
                EXPECT_FLOAT_EQ(vel->dy, float(i * 0.2f));
                EXPECT_FLOAT_EQ(vel->dz, float(i * 0.3f));
                
                // Check Health
                Health* health = deserializedArchetype->GetComponent<Health>(loc);
                ASSERT_NE(health, nullptr);
                EXPECT_EQ(health->current, int(100 - i));
                EXPECT_EQ(health->max, 100);
                
                verifiedCount++;
            }
        }
        
        EXPECT_EQ(verifiedCount, entityCount);
    }
}

// Test serialization with multiple chunks
TEST_F(ArchetypeTest, SerializeMultipleChunks)
{
    using namespace Astra::Test;
    
    // Create archetype with small components to fit many per chunk
    auto mask = Astra::MakeComponentMask<Position, Player>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    auto descriptors = GetDescriptors(mask);
    archetype.Initialize(descriptors);
    
    // Add enough entities to span multiple chunks
    size_t entitiesPerChunk = archetype.GetEntitiesPerChunk();
    size_t entityCount = entitiesPerChunk * 3 + entitiesPerChunk / 2; // 3.5 chunks
    
    std::vector<Astra::Entity> entities;
    for (size_t i = 0; i < entityCount; ++i)
    {
        Astra::Entity entity(static_cast<Astra::Entity::StorageType>(i), 1);
        entities.push_back(entity);
        
        Astra::EntityLocation loc = archetype.AddEntity(entity);
        ASSERT_TRUE(loc.IsValid());
        
        archetype.SetComponent(loc, Position{float(i), 0.0f, 0.0f});
    }
    
    EXPECT_EQ(archetype.GetEntityCount(), entityCount);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        Astra::BinaryWriter writer(buffer);
        archetype.Serialize(writer);
    }
    
    // Deserialize
    {
        Astra::BinaryReader reader(buffer);
        
        // Get all component descriptors from registry for deserialization
        std::vector<Astra::ComponentDescriptor> registryDescriptors;
        registry.GetAllDescriptors(registryDescriptors);
        
        auto deserializedResult = Astra::Archetype::Deserialize(reader, registryDescriptors, &componentPool);

        ASSERT_TRUE(deserializedResult.IsOk()) << "Deserialization failed";
        auto deserializedArchetype = std::move(*deserializedResult.GetValue());

        EXPECT_EQ(deserializedArchetype->GetEntityCount(), entityCount);

        // Verify all entities and their data by iterating through chunks
        size_t verifiedCount = 0;
        const auto& chunks = deserializedArchetype->GetChunks();
        
        for (const auto& chunk : chunks)
        {
            size_t chunkEntityCount = chunk->GetCount();
            const auto& chunkEntities = chunk->GetEntities();
            
            for (size_t idx = 0; idx < chunkEntityCount; ++idx)
            {
                Astra::Entity entity = chunkEntities[idx];
                
                // Find original index
                auto origIt = std::find(entities.begin(), entities.end(), entity);
                ASSERT_NE(origIt, entities.end());
                size_t i = std::distance(entities.begin(), origIt);
                
                size_t chunkIdx = &chunk - &chunks[0];
                Astra::EntityLocation loc = Astra::EntityLocation::Create(chunkIdx, idx);
                
                Position* pos = deserializedArchetype->GetComponent<Position>(loc);
                ASSERT_NE(pos, nullptr);
                EXPECT_EQ(pos->x, float(i));
                
                verifiedCount++;
            }
        }
        
        EXPECT_EQ(verifiedCount, entityCount);
    }
}

// Test serialization with non-trivial components
TEST_F(ArchetypeTest, SerializeNonTrivialComponents)
{
    using namespace Astra::Test;
    
    // Create archetype with non-trivial Name component (contains std::string)
    auto mask = Astra::MakeComponentMask<Position, Name>();
    Astra::Archetype archetype(mask);
    archetype.SetComponentPool(&componentPool);
    
    auto descriptors = GetDescriptors(mask);
    archetype.Initialize(descriptors);
    
    // Add entities with string data
    const size_t entityCount = 10;
    std::vector<Astra::Entity> entities;
    
    for (size_t i = 0; i < entityCount; ++i)
    {
        Astra::Entity entity(static_cast<Astra::Entity::StorageType>(i), 1);
        entities.push_back(entity);
        
        Astra::EntityLocation loc = archetype.AddEntity(entity);
        ASSERT_TRUE(loc.IsValid());
        
        archetype.SetComponent(loc, Position{float(i), 0.0f, 0.0f});
        archetype.SetComponent(loc, Name{"Entity_" + std::to_string(i)});
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        Astra::BinaryWriter writer(buffer);
        archetype.Serialize(writer);
    }
    
    // Deserialize
    {
        Astra::BinaryReader reader(buffer);
        
        // Get all component descriptors from registry for deserialization
        std::vector<Astra::ComponentDescriptor> registryDescriptors;
        registry.GetAllDescriptors(registryDescriptors);
        
        auto deserializedResult = Astra::Archetype::Deserialize(reader, registryDescriptors, &componentPool);

        ASSERT_TRUE(deserializedResult.IsOk()) << "Deserialization failed";
        auto deserializedArchetype = std::move(*deserializedResult.GetValue());

        // Verify string data is preserved by checking entities in chunks
        size_t verifiedCount = 0;
        const auto& chunks = deserializedArchetype->GetChunks();
        
        for (const auto& chunk : chunks)
        {
            size_t chunkEntityCount = chunk->GetCount();
            const auto& chunkEntities = chunk->GetEntities();
            
            for (size_t idx = 0; idx < chunkEntityCount; ++idx)
            {
                Astra::Entity entity = chunkEntities[idx];
                
                // Find original index
                auto origIt = std::find(entities.begin(), entities.end(), entity);
                ASSERT_NE(origIt, entities.end());
                size_t i = std::distance(entities.begin(), origIt);
                
                size_t chunkIdx = &chunk - &chunks[0];
                Astra::EntityLocation loc = Astra::EntityLocation::Create(chunkIdx, idx);
                
                Name* name = deserializedArchetype->GetComponent<Name>(loc);
                ASSERT_NE(name, nullptr);
                EXPECT_EQ(name->value, "Entity_" + std::to_string(i));
                
                verifiedCount++;
            }
        }
        
        EXPECT_EQ(verifiedCount, entityCount);
    }
}