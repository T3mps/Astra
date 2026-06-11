#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class CommandBufferTest : public ::testing::Test
{
protected:
    std::unique_ptr<Registry> registry;
    std::unique_ptr<CommandBuffer> cmdBuffer;
    
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
        cmdBuffer = std::make_unique<CommandBuffer>(registry.get());
    }
};

TEST_F(CommandBufferTest, CreateEntityWithComponents)
{
    // Create entity through command buffer
    Entity tempEntity = cmdBuffer->CreateEntity();
    EXPECT_NE(tempEntity, Entity::Invalid());
    
    // Add components through command buffer
    cmdBuffer->AddComponent(tempEntity, Position{1.0f, 2.0f, 3.0f});
    cmdBuffer->AddComponent(tempEntity, Velocity{4.0f, 5.0f, 6.0f});
    
    // Execute commands
    cmdBuffer->Execute();
    
    // Verify entity was created and has components
    auto view = registry->CreateView<Position, Velocity>();
    int count = 0;
    view.ForEach([&count](Entity, const Position& pos, const Velocity& vel) {
        count++;
        EXPECT_EQ(pos.x, 1.0f);
        EXPECT_EQ(pos.y, 2.0f);
        EXPECT_EQ(pos.z, 3.0f);
        EXPECT_EQ(vel.dx, 4.0f);
        EXPECT_EQ(vel.dy, 5.0f);
        EXPECT_EQ(vel.dz, 6.0f);
    });
    EXPECT_EQ(count, 1);
}

TEST_F(CommandBufferTest, DestroyEntity)
{
    // Create entity directly
    Entity e = registry->CreateEntity();
    registry->AddComponent<Position>(e, {1.0f, 2.0f, 3.0f});
    
    // Destroy through command buffer
    cmdBuffer->DestroyEntity(e);
    cmdBuffer->Execute();
    
    // Verify entity is destroyed
    EXPECT_FALSE(registry->IsValid(e));
}

TEST_F(CommandBufferTest, BatchOperations)
{
    // Create multiple entities through command buffer
    Entity temp1 = cmdBuffer->CreateEntity();
    Entity temp2 = cmdBuffer->CreateEntity();
    Entity temp3 = cmdBuffer->CreateEntity();
    
    // Batch add components
    std::vector<Entity> entities = {temp1, temp2, temp3};
    cmdBuffer->AddComponents<Position>(entities, {10.0f, 20.0f, 30.0f});
    
    // Execute
    cmdBuffer->Execute();
    
    // Verify all entities have the component
    auto view = registry->CreateView<Position>();
    int count = 0;
    view.ForEach([&count](Entity, const Position& pos) {
        count++;
        EXPECT_EQ(pos.x, 10.0f);
        EXPECT_EQ(pos.y, 20.0f);
        EXPECT_EQ(pos.z, 30.0f);
    });
    EXPECT_EQ(count, 3);
}

TEST_F(CommandBufferTest, RemoveComponents)
{
    // Create entities with components
    Entity e1 = registry->CreateEntity();
    Entity e2 = registry->CreateEntity();
    registry->AddComponent<Position>(e1, {1.0f, 2.0f, 3.0f});
    registry->AddComponent<Position>(e2, {4.0f, 5.0f, 6.0f});
    registry->AddComponent<Velocity>(e1, {7.0f, 8.0f, 9.0f});
    registry->AddComponent<Velocity>(e2, {10.0f, 11.0f, 12.0f});
    
    // Remove component through command buffer
    cmdBuffer->RemoveComponent<Velocity>(e1);
    cmdBuffer->Execute();
    
    // Verify component removed
    EXPECT_FALSE(registry->HasComponent<Velocity>(e1));
    EXPECT_TRUE(registry->HasComponent<Position>(e1));
    EXPECT_TRUE(registry->HasComponent<Velocity>(e2));
}

TEST_F(CommandBufferTest, Relationships)
{
    // Create entities
    Entity parent = cmdBuffer->CreateEntity();
    Entity child1 = cmdBuffer->CreateEntity();
    Entity child2 = cmdBuffer->CreateEntity();
    
    // Set relationships
    cmdBuffer->SetParent(child1, parent);
    cmdBuffer->SetParent(child2, parent);
    
    // Execute
    cmdBuffer->Execute();
    
    // Verify relationships exist (need to get real entity IDs first)
    auto view = registry->CreateView<>();
    std::vector<Entity> realEntities;
    view.ForEach([&realEntities](Entity e) {
        realEntities.push_back(e);
    });
    
    EXPECT_EQ(realEntities.size(), 3u);
    
    // Check parent-child relationships
    for (size_t i = 1; i < realEntities.size(); ++i)
    {
        Entity realParent = registry->GetParent(realEntities[i]);
        EXPECT_NE(realParent, Entity::Invalid());
    }
}

TEST_F(CommandBufferTest, MixedCommands)
{
    // Mix different types of commands
    Entity temp1 = cmdBuffer->CreateEntity();
    Entity temp2 = cmdBuffer->CreateEntity();
    
    cmdBuffer->AddComponent(temp1, Position{1.0f, 2.0f, 3.0f});
    cmdBuffer->AddComponent(temp2, Velocity{4.0f, 5.0f, 6.0f});
    cmdBuffer->SetParent(temp2, temp1);
    cmdBuffer->AddLink(temp1, temp2);
    
    // Execute all at once
    cmdBuffer->Execute();
    
    // Verify everything worked
    auto view = registry->CreateView<>();
    int entityCount = 0;
    view.ForEach([&entityCount](Entity) {
        entityCount++;
    });
    EXPECT_EQ(entityCount, 2);
}

TEST_F(CommandBufferTest, ClearAndReuse)
{
    // First batch
    Entity temp1 = cmdBuffer->CreateEntity();
    cmdBuffer->AddComponent(temp1, Position{1.0f, 2.0f, 3.0f});
    cmdBuffer->Execute();
    
    // Verify first batch
    EXPECT_EQ(registry->Size(), 1u);
    
    // Second batch (buffer should be cleared after Execute)
    Entity temp2 = cmdBuffer->CreateEntity();
    cmdBuffer->AddComponent(temp2, Velocity{4.0f, 5.0f, 6.0f});
    cmdBuffer->Execute();
    
    // Verify second batch
    EXPECT_EQ(registry->Size(), 2u);
    
    // Manual clear and reuse
    cmdBuffer->Clear();
    EXPECT_TRUE(cmdBuffer->IsEmpty());
    
    Entity temp3 = cmdBuffer->CreateEntity();
    (void)temp3;
    cmdBuffer->Execute();
    EXPECT_EQ(registry->Size(), 3u);
}

TEST_F(CommandBufferTest, ParallelCommandBuffer)
{
    ParallelCommandBuffer parallelBuffer(registry.get());
    
    // Simulate parallel command recording
    auto& buffer1 = parallelBuffer.GetThreadBuffer();
    Entity temp1 = buffer1.CreateEntity();
    buffer1.AddComponent(temp1, Position{1.0f, 2.0f, 3.0f});
    
    auto& buffer2 = parallelBuffer.GetThreadBuffer(); // Same thread, should get same buffer
    Entity temp2 = buffer2.CreateEntity();
    buffer2.AddComponent(temp2, Velocity{4.0f, 5.0f, 6.0f});
    
    // Execute all buffers
    parallelBuffer.Execute();
    
    // Verify entities were created
    EXPECT_EQ(registry->Size(), 2u);
    
    auto posView = registry->CreateView<Position>();
    auto velView = registry->CreateView<Velocity>();
    EXPECT_EQ(posView.Size(), 1u);
    EXPECT_EQ(velView.Size(), 1u);
}

TEST_F(CommandBufferTest, EmplaceComponent)
{
    // Create entity and emplace components
    Entity temp = cmdBuffer->CreateEntity();
    cmdBuffer->EmplaceComponent<Position>(temp, 10.0f, 20.0f, 30.0f);
    cmdBuffer->EmplaceComponent<Velocity>(temp, 40.0f, 50.0f, 60.0f);
    
    // Execute
    cmdBuffer->Execute();
    
    // Verify components were emplaced
    auto view = registry->CreateView<Position, Velocity>();
    int count = 0;
    view.ForEach([&count](Entity, const Position& pos, const Velocity& vel) {
        count++;
        EXPECT_EQ(pos.x, 10.0f);
        EXPECT_EQ(pos.y, 20.0f);
        EXPECT_EQ(pos.z, 30.0f);
        EXPECT_EQ(vel.dx, 40.0f);
        EXPECT_EQ(vel.dy, 50.0f);
        EXPECT_EQ(vel.dz, 60.0f);
    });
    EXPECT_EQ(count, 1);
}

TEST_F(CommandBufferTest, EmplaceComponents)
{
    // Create multiple entities
    Entity temp1 = cmdBuffer->CreateEntity();
    Entity temp2 = cmdBuffer->CreateEntity();
    Entity temp3 = cmdBuffer->CreateEntity();
    
    // Batch emplace components
    std::vector<Entity> entities = {temp1, temp2, temp3};
    cmdBuffer->EmplaceComponents<Position>(entities, 100.0f, 200.0f, 300.0f);
    
    // Execute
    cmdBuffer->Execute();
    
    // Verify all entities have the emplaced component
    auto view = registry->CreateView<Position>();
    int count = 0;
    view.ForEach([&count](Entity, const Position& pos) {
        count++;
        EXPECT_EQ(pos.x, 100.0f);
        EXPECT_EQ(pos.y, 200.0f);
        EXPECT_EQ(pos.z, 300.0f);
    });
    EXPECT_EQ(count, 3);
}

TEST_F(CommandBufferTest, ResourceCommands)
{
    // Set a resource
    cmdBuffer->SetResource<Position>({1.0f, 2.0f, 3.0f});
    cmdBuffer->Execute();
    
    // Verify resource was set
    Position* res = registry->GetResource<Position>();
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->x, 1.0f);
    EXPECT_EQ(res->y, 2.0f);
    EXPECT_EQ(res->z, 3.0f);
    
    // Emplace a new resource
    cmdBuffer->EmplaceResource<Velocity>(4.0f, 5.0f, 6.0f);
    cmdBuffer->Execute();
    
    // Verify resource was emplaced
    Velocity* vel = registry->GetResource<Velocity>();
    ASSERT_NE(vel, nullptr);
    EXPECT_EQ(vel->dx, 4.0f);
    EXPECT_EQ(vel->dy, 5.0f);
    EXPECT_EQ(vel->dz, 6.0f);
    
    // Remove a resource
    cmdBuffer->RemoveResource<Position>();
    cmdBuffer->Execute();
    
    // Verify resource was removed
    EXPECT_EQ(registry->GetResource<Position>(), nullptr);
    EXPECT_NE(registry->GetResource<Velocity>(), nullptr);
    
    // Clear all resources
    cmdBuffer->ClearResources();
    cmdBuffer->Execute();
    
    // Verify all resources cleared
    EXPECT_EQ(registry->GetResource<Velocity>(), nullptr);
}

TEST_F(CommandBufferTest, ExtendedRelationshipCommands)
{
    // Create entities
    Entity parent = cmdBuffer->CreateEntity();
    Entity child1 = cmdBuffer->CreateEntity();
    Entity child2 = cmdBuffer->CreateEntity();
    Entity child3 = cmdBuffer->CreateEntity();
    
    // Use AddChild instead of SetParent
    cmdBuffer->AddChild(parent, child1);
    cmdBuffer->AddChild(parent, child2);
    cmdBuffer->AddChild(parent, child3);
    
    // Execute
    cmdBuffer->Execute();
    
    // Get real entity IDs
    auto view = registry->CreateView<>();
    std::vector<Entity> realEntities;
    view.ForEach([&realEntities](Entity e) {
        realEntities.push_back(e);
    });
    ASSERT_EQ(realEntities.size(), 4u);
    
    // Verify parent-child relationships
    Entity realParent = realEntities[0];
    auto children = registry->GetChildren(realParent);
    EXPECT_EQ(children.size(), 3u);
    
    // Remove a specific child
    cmdBuffer->RemoveChild(realParent, children[0]);
    cmdBuffer->Execute();
    
    // Verify child was removed
    auto remainingChildren = registry->GetChildren(realParent);
    EXPECT_EQ(remainingChildren.size(), 2u);
    
    // Remove all remaining children
    cmdBuffer->RemoveAllChildren(realParent);
    cmdBuffer->Execute();
    
    // Verify all children removed
    auto finalChildren = registry->GetChildren(realParent);
    EXPECT_EQ(finalChildren.size(), 0u);
}