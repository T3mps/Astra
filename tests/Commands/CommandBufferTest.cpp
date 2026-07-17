#include <gtest/gtest.h>
#include <thread>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"

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

TEST_F(CommandBufferTest, RollbackPreservesCommittedEntities)
{
    // Entity e is created and committed to an archetype (via AddComponent)
    // *before* a later command in the same buffer fails. DestroyEntity is
    // recorded with Entity::Invalid(), which is accepted at record time but
    // rejected by ExecuteDestroyEntity at Execute() time, aborting the
    // buffer partway through and invoking RollbackAllocatedEntities().
    Entity e = cmdBuffer->CreateEntity();
    cmdBuffer->AddComponent(e, Position{1.0f, 2.0f, 3.0f});
    cmdBuffer->DestroyEntity(Entity::Invalid());

    auto result = cmdBuffer->Execute();
    ASSERT_TRUE(result.IsErr());

    // e was already committed to an archetype earlier in this same Execute()
    // call; the rollback of not-yet-committed allocated entities must not
    // touch it, so it must remain a live, valid entity with its component.
    EXPECT_TRUE(registry->IsValid(e));
    EXPECT_TRUE(registry->HasComponent<Position>(e));
}

TEST_F(CommandBufferTest, ExecuteSortedAppliesInKeyOrderNotRecordOrder)
{
    // Entities are created directly on the registry-owning thread (CreateEntity
    // must not be recorded from worker threads); only the deferred re-parent
    // commands below are recorded from worker threads.
    Entity child = registry->CreateEntity();
    Entity parentA = registry->CreateEntity();
    Entity parentB = registry->CreateEntity();

    ParallelCommandBuffer parallelBuffer(registry.get());

    // Both threads are launched before either is joined so their OS thread
    // identities are guaranteed to be live simultaneously -- if t1 were
    // joined before t2 started, the OS would be free to recycle t1's thread
    // id for t2, collapsing both into the same per-worker buffer slot and
    // spuriously failing the GetThreadCount()==2 assertion below.

    // Worker thread 1 records SetParent(child, parentA) FIRST (record order 0,
    // its buffer's own arrival order) but stamps it with a HIGHER sort key ->
    // under key-order flush this must apply SECOND.
    std::thread t1([&]()
    {
        auto& buf = parallelBuffer.GetThreadBuffer();
        buf.SetNextSortKey(SortKey{5, 0, 0});
        buf.SetParent(child, parentA);
    });

    // Worker thread 2 records SetParent(child, parentB) SECOND (a later wall-
    // clock record, and a distinct worker buffer) but stamps it with a LOWER
    // sort key -> under key-order flush this must apply FIRST.
    std::thread t2([&]()
    {
        auto& buf = parallelBuffer.GetThreadBuffer();
        buf.SetNextSortKey(SortKey{1, 0, 0});
        buf.SetParent(child, parentB);
    });

    t1.join();
    t2.join();

    ASSERT_EQ(parallelBuffer.GetThreadCount(), 2u);

    auto result = parallelBuffer.ExecuteSorted();
    ASSERT_TRUE(result.IsOk());

    // Key order (ascending insertionOrder) applies the key=1 command
    // (SetParent parentB) first, then the key=5 command (SetParent parentA)
    // second -- last-write-wins means parentA is the final parent. Plain
    // record/arrival order would have applied parentA first and parentB
    // second, leaving parentB as the final parent instead. Asserting parentA
    // therefore distinguishes a genuine key-order flush from a record-order
    // flush.
    EXPECT_EQ(registry->GetParent(child), parentA);
}

TEST_F(CommandBufferTest, RollbackDestroysOnlyUncommittedEntities)
{
    // White-box check on the commit-tracking split itself: create three
    // entities (all committed via CreateEntity executing successfully),
    // then fail a fourth command. All three must survive since they were
    // all committed before the failure point, regardless of how many.
    Entity e1 = cmdBuffer->CreateEntity();
    Entity e2 = cmdBuffer->CreateEntity();
    Entity e3 = cmdBuffer->CreateEntity();
    cmdBuffer->AddComponent(e1, Position{1.0f, 2.0f, 3.0f});
    cmdBuffer->AddComponent(e2, Position{4.0f, 5.0f, 6.0f});
    cmdBuffer->AddComponent(e3, Position{7.0f, 8.0f, 9.0f});
    cmdBuffer->DestroyEntity(Entity::Invalid());

    auto result = cmdBuffer->Execute();
    ASSERT_TRUE(result.IsErr());

    EXPECT_TRUE(registry->IsValid(e1));
    EXPECT_TRUE(registry->IsValid(e2));
    EXPECT_TRUE(registry->IsValid(e3));
    EXPECT_EQ(registry->Size(), 3u);
}