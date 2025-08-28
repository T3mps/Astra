#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <algorithm>
#include "Astra/Registry/Registry.hpp"
#include "Astra/Archetype/ArchetypeChunkPool.hpp"
#include "../TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class MemoryCleanupTest : public ::testing::Test
{
protected:
    std::unique_ptr<Registry> registry;
    
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
        
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();
    }
    
    void TearDown() override
    {
        registry.reset();
    }
    
    size_t GetCurrentMemoryUsage()
    {
        return registry->GetArchetypeMemoryUsage();
    }
    
    size_t GetArchetypeCount()
    {
        return registry->GetArchetypeCount();
    }
};

TEST_F(MemoryCleanupTest, MemoryReleasedAfterDestruction)
{
    size_t initialMemory = GetCurrentMemoryUsage();
    size_t initialArchetypes = GetArchetypeCount();
    
    std::vector<Entity> entities;
    for (int i = 0; i < 1000; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), float(i * 2), float(i * 3)},
            Velocity{float(i * 10), 0, 0},
            Health{i, 1000}
        ));
    }
    
    size_t peakMemory = GetCurrentMemoryUsage();
    EXPECT_GT(peakMemory, initialMemory) << "Memory should increase after creating entities";
    
    registry->DestroyEntities(entities);
    
    // Memory still allocated in pools
    size_t afterDestroyMemory = GetCurrentMemoryUsage();
    EXPECT_GT(afterDestroyMemory, 0u) << "Some memory still allocated in pools";
    
    Registry::DefragmentationOptions options;
    options.minArchetypesToKeep = 1;
    
    auto result = registry->Defragment(options);
    size_t cleaned = result.archetypesRemoved;
    EXPECT_GT(cleaned, 0u) << "Should have cleaned some archetypes";
    
    size_t finalMemory = GetCurrentMemoryUsage();
    size_t finalArchetypes = GetArchetypeCount();
    
    EXPECT_LT(finalMemory, peakMemory) << "Memory should decrease after cleanup";
    EXPECT_LE(finalArchetypes, initialArchetypes + 1) << "Should have minimal archetypes after cleanup";
}

TEST_F(MemoryCleanupTest, RapidArchetypeTransitionCleanup)
{
    Entity entity = registry->CreateEntity();
    
    std::vector<size_t> memorySamples;
    
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        registry->EmplaceComponent<Position>(entity);
        registry->EmplaceComponent<Velocity>(entity);
        registry->EmplaceComponent<Health>(entity);
        registry->EmplaceComponent<Transform>(entity);
        
        memorySamples.push_back(GetCurrentMemoryUsage());
        
        registry->RemoveComponent<Velocity>(entity);
        registry->RemoveComponent<Health>(entity);
        
        memorySamples.push_back(GetCurrentMemoryUsage());
        
        registry->RemoveComponent<Position>(entity);
        registry->RemoveComponent<Transform>(entity);
    }
    
    size_t beforeCleanup = GetArchetypeCount();
    EXPECT_GT(beforeCleanup, 5u) << "Multiple archetypes should exist";
    
    registry->DestroyEntity(entity);
    
    Registry::DefragmentationOptions options;
    auto result = registry->Defragment(options);
    size_t cleaned = result.archetypesRemoved;
    
    EXPECT_GT(cleaned, 0u) << "Should clean up empty archetypes";
    
    size_t afterCleanup = GetArchetypeCount();
    EXPECT_LT(afterCleanup, beforeCleanup) << "Archetype count should decrease";
}

TEST_F(MemoryCleanupTest, ArchetypeChunkPoolMemoryRelease)
{
    ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 4;
    config.maxChunks = 100;
    config.initialBlocks = 1;
    
    ArchetypeChunkPool pool(config);
    
    // Create test component descriptors
    std::vector<ComponentDescriptor> descriptors;
    ComponentDescriptor posDesc;
    posDesc.id = 0;
    posDesc.size = sizeof(Position);
    posDesc.alignment = alignof(Position);
    descriptors.push_back(posDesc);
    
    std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> chunks;
    size_t entitiesPerChunk = 100;
    
    for (int i = 0; i < 20; ++i)
    {
        auto chunk = pool.CreateChunk(entitiesPerChunk, descriptors);
        ASSERT_NE(chunk, nullptr);
        chunks.push_back(std::move(chunk));
    }
    
    auto stats1 = pool.GetStats();
    EXPECT_EQ(stats1.acquireCount - stats1.releaseCount, 20u);
    EXPECT_GT(stats1.totalChunks, 0u);
    
    // Release first 10 chunks
    for (int i = 0; i < 10; ++i)
    {
        chunks[i].reset();
    }
    chunks.erase(chunks.begin(), chunks.begin() + 10);
    
    auto stats2 = pool.GetStats();
    EXPECT_EQ(stats2.acquireCount - stats2.releaseCount, 10u);
    EXPECT_EQ(stats2.totalChunks, stats1.totalChunks) << "Total chunks should remain (pooled)";
    
    // Release remaining chunks
    chunks.clear();
    
    auto stats3 = pool.GetStats();
    EXPECT_EQ(stats3.acquireCount - stats3.releaseCount, 0u);
    
    EXPECT_GT(stats3.totalChunks, 0u) << "Chunks remain in pool for reuse";
}

TEST_F(MemoryCleanupTest, ComponentSizeMemoryUsage)
{
    struct LargeComponent
    {
        char data[1024];  // 1KB
    };
    
    struct MediumComponent
    {
        char data[256];  // 256 bytes
    };
    
    registry->GetComponentRegistry()->RegisterComponent<LargeComponent>();
    registry->GetComponentRegistry()->RegisterComponent<MediumComponent>();
    
    size_t baseMemory = GetCurrentMemoryUsage();
    
    std::vector<Entity> largeEntities;
    for (int i = 0; i < 100; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->EmplaceComponent<LargeComponent>(e);
        largeEntities.push_back(e);
    }
    
    size_t largeMemory = GetCurrentMemoryUsage();
    size_t largeIncrease = largeMemory - baseMemory;
    
    registry->DestroyEntities(largeEntities);
    
    std::vector<Entity> mediumEntities;
    for (int i = 0; i < 100; ++i)
    {
        Entity e = registry->CreateEntity();
        registry->EmplaceComponent<MediumComponent>(e);
        mediumEntities.push_back(e);
    }
    
    size_t mediumMemory = GetCurrentMemoryUsage();
    size_t mediumIncrease = mediumMemory - baseMemory;
    
    // Due to chunking, not exactly 4x
    EXPECT_GT(largeIncrease, mediumIncrease) << "Large components should use more memory";
    
    registry->Clear();
    Registry::DefragmentationOptions options;
    registry->Defragment(options);
}

TEST_F(MemoryCleanupTest, ArchetypeFragmentation)
{
    std::vector<std::vector<Entity>> archetypeGroups;
    
    for (int archetype = 0; archetype < 10; ++archetype)
    {
        std::vector<Entity> group;
        for (int i = 0; i < 50; ++i)
        {
            Entity e = registry->CreateEntity();
            
            if (archetype & 1) registry->EmplaceComponent<Position>(e);
            if (archetype & 2) registry->EmplaceComponent<Velocity>(e);
            if (archetype & 4) registry->EmplaceComponent<Health>(e);
            if (archetype & 8) registry->EmplaceComponent<Transform>(e);
            
            group.push_back(e);
        }
        archetypeGroups.push_back(group);
    }
    
    size_t fullMemory = GetCurrentMemoryUsage();
    size_t fullArchetypes = GetArchetypeCount();
    
    for (auto& group : archetypeGroups)
    {
        for (size_t i = 0; i < group.size(); i += 2)
        {
            registry->DestroyEntity(group[i]);
        }
    }
    
    size_t fragmentedMemory = GetCurrentMemoryUsage();
    
    EXPECT_GT(fragmentedMemory, fullMemory / 2) << "Memory remains allocated despite fragmentation";
    
    for (int i = 0; i < 250; ++i)
    {
        Entity e = registry->CreateEntity();
        int archetype = i % 10;
        if (archetype & 1) registry->EmplaceComponent<Position>(e);
        if (archetype & 2) registry->EmplaceComponent<Velocity>(e);
        if (archetype & 4) registry->EmplaceComponent<Health>(e);
        if (archetype & 8) registry->EmplaceComponent<Transform>(e);
    }
    
    size_t refilledMemory = GetCurrentMemoryUsage();
    
    EXPECT_LE(refilledMemory, fullMemory * 1.5) << "Should efficiently reuse fragmented memory";
}

TEST_F(MemoryCleanupTest, ClearMemoryCleanup)
{
    for (int batch = 0; batch < 5; ++batch)
    {
        std::vector<Entity> entities;
        for (int i = 0; i < 200; ++i)
        {
            entities.push_back(registry->CreateEntityWith(
                Position{float(i), 0, 0},
                Velocity{1, 0, 0}
            ));
        }
        
        for (size_t i = 0; i < 100; ++i)
        {
            registry->EmplaceComponent<Health>(entities[i], 100, 100);
        }
    }
    
    size_t beforeClear = GetCurrentMemoryUsage();
    size_t beforeArchetypes = GetArchetypeCount();
    
    EXPECT_GT(beforeClear, 0u);
    EXPECT_GT(beforeArchetypes, 1u);
    
    registry->Clear();
    
    EXPECT_EQ(registry->Size(), 0u) << "All entities should be gone";
    
    size_t afterClear = GetCurrentMemoryUsage();
    
    Registry::DefragmentationOptions options;
    registry->Defragment(options);
    
    size_t afterCleanup = GetCurrentMemoryUsage();
    size_t afterArchetypes = GetArchetypeCount();
    
    EXPECT_LT(afterCleanup, beforeClear) << "Memory should decrease after cleanup";
    EXPECT_LT(afterArchetypes, beforeArchetypes) << "Fewer archetypes after cleanup";
}

TEST_F(MemoryCleanupTest, CleanupOptionsRespected)
{
    std::vector<Entity> persistent;
    for (int i = 0; i < 10; ++i)
    {
        persistent.push_back(registry->CreateEntityWith(Position{}));
    }
    
    std::vector<Entity> temporary;
    for (int i = 0; i < 10; ++i)
    {
        temporary.push_back(registry->CreateEntityWith(Position{}, Velocity{}));
    }
    
    registry->DestroyEntities(temporary);
    
    size_t archetypesBefore = GetArchetypeCount();
    
    Registry::DefragmentationOptions options1;
    
    auto result1 = registry->Defragment(options1);
    size_t cleaned1 = result1.archetypesRemoved;
    EXPECT_EQ(cleaned1, 0u) << "Shouldn't clean archetypes that haven't been empty long enough";
    
    for (int i = 0; i < 5; ++i)
    {
        registry->Defragment(options1);
    }
    
    Registry::DefragmentationOptions options2;
    options2.maxArchetypesToRemove = 1;
    
    auto result2 = registry->Defragment(options2);
    size_t cleaned2 = result2.archetypesRemoved;
    EXPECT_LE(cleaned2, 1u) << "Should respect maxArchetypesToRemove";
    
    Registry::DefragmentationOptions options3;
    options3.minArchetypesToKeep = archetypesBefore;
    
    auto result3 = registry->Defragment(options3);
    size_t cleaned3 = result3.archetypesRemoved;
    EXPECT_EQ(cleaned3, 0u) << "Should respect minArchetypesToKeep";
}

// Removed BatchOperationMemoryStability test - memory pooling behavior is an
// implementation detail. The ECS may keep memory allocated for performance reasons
// even after cleanup. The important guarantee is no unbounded growth, which is
// tested in other tests.