#include <algorithm>
#include <bit>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <span>
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
    posView.ForEach([&](Astra::Entity, Position& pos) {
        posCount++;
        EXPECT_GE(pos.x, 0.0f);
        EXPECT_LT(pos.x, 10.0f);
    });
    EXPECT_EQ(posCount, 10u); // All entities have Position
    
    // Create view for Position and Velocity
    auto posVelView = registry->CreateView<Position, Velocity>();
    
    size_t posVelCount = 0;
    posVelView.ForEach([&](Astra::Entity, Position& pos, Velocity&) {
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
    
    auto entityHandler = signals->On<Astra::Events::EntityCreated>().Register([&](const Astra::Events::EntityCreated&)
    {
        entityCreatedCount++;
    });
    
    auto componentHandler = signals->On<Astra::Events::ComponentAdded>().Register([&](const Astra::Events::ComponentAdded&)
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
    
    // EmplaceComponent returns false (graceful) for a stale/invalid handle and adds nothing.
    EXPECT_FALSE(registry->EmplaceComponent<Position>(invalidEntity));
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

// Theme J Fix 2: Registry is non-copyable; component registrations are shared explicitly.
TEST_F(RegistryTest, SharesComponentRegistryNotState)
{
    using namespace Astra::Test;

    static_assert(!std::is_copy_constructible_v<Astra::Registry>, "Registry must be non-copyable");
    static_assert(!std::is_copy_assignable_v<Astra::Registry>, "Registry must be non-copy-assignable");

    // Original has an entity; a shared-registry world must NOT inherit its state.
    Astra::Entity original = registry->CreateEntityWith(Position{9.0f, 0.0f, 0.0f});
    ASSERT_TRUE(registry->IsValid(original));

    Astra::Registry::Config config;
    Astra::Registry world2(registry->ShareComponentRegistry(), config);

    // Shares the component registry (same ComponentID space)...
    EXPECT_EQ(world2.GetComponentRegistry(), registry->GetComponentRegistry());
    // ...but has independent entity state.
    EXPECT_FALSE(world2.IsValid(original));
    EXPECT_EQ(world2.Size(), 0u);

    // world2 can create entities with the shared registrations.
    Astra::Entity e = world2.CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(world2.IsValid(e));
    Position* pos = world2.GetComponent<Position>(e);
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
    registry->CreateEntitiesWith<Position, Velocity>(batchSize, entities,
        [](size_t i)
        {
            return std::make_tuple(Position{float(i), float(i * 2), float(i * 3)}, Velocity{float(i * 10), 0.0f, 0.0f});
        });

    // Verify all entities were created
    EXPECT_EQ(registry->Size(), batchSize);
    
    // Verify batch destruction
    registry->DestroyEntities(entities);
    EXPECT_EQ(registry->Size(), 0u);
}

// Characterization tests for the chunk-run bulk-create rewrite (Lever 3 Task 2).
// These lock current observable behavior BEFORE the rewrite: every batch-created
// entity must own ITS OWN generator values across chunk boundaries (a mis-hoisted
// column base or wrong run offset would surface as an entity reading a neighbor's
// value), and a move-only component must be constructed exactly once per entity and
// torn down to full balance (a doubled/leaked move would desync s_live).
TEST_F(RegistryTest, BatchCreateValuesSurviveChunkBoundaries)
{
    using namespace Astra::Test;
    // Enough entities to span multiple chunks (grow-as-populate ramps from 4KB).
    constexpr size_t kCount = 3000;
    std::vector<Astra::Entity> ents(kCount);
    size_t created = registry->CreateEntitiesWith<Position, Velocity>(
        kCount, ents,
        [](size_t i) { return std::tuple{Position{float(i), 0.f, 0.f},
                                         Velocity{float(i) * 2.f, 0.f, 0.f}}; });
    ASSERT_EQ(created, kCount);
    for (size_t i = 0; i < kCount; ++i)
    {
        auto* p = registry->GetComponent<Position>(ents[i]);
        auto* v = registry->GetComponent<Velocity>(ents[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        ASSERT_NE(v, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));             // every entity owns ITS generator values
        EXPECT_FLOAT_EQ(v->dx, float(i) * 2.f);      // Velocity's field is dx (not x)
    }
}

TEST_F(RegistryTest, BatchCreateLifetimeBalanceMoveOnly)
{
    using namespace Astra::Test;
    const int base = Tracked::s_live;
    {
        constexpr size_t kCount = 500;
        std::vector<Astra::Entity> ents(kCount);
        size_t created = registry->CreateEntitiesWith<Tracked>(
            kCount, ents,
            [](size_t i) { return std::tuple{Tracked{int(i)}}; });
        ASSERT_EQ(created, kCount);
        EXPECT_EQ(Tracked::s_live, base + int(kCount));      // constructed exactly once each
        EXPECT_EQ(registry->GetComponent<Tracked>(ents[123])->value, 123);
        for (auto e : ents) registry->DestroyEntity(e);
    }
    EXPECT_EQ(Tracked::s_live, base);                        // full teardown balance
}

// Serialization tests split out (2026-07-10 test-suite audit) into
// tests/Registry/RegistrySerializationTest.cpp.

// Theme J Fix 1: CreateEntities/CreateEntitiesWith report the number actually created.
TEST_F(RegistryTest, CreateEntitiesReturnsCreatedCount)
{
    using namespace Astra::Test;

    // Adequate span -> returns count, creates them.
    std::vector<Astra::Entity> ents(5);
    size_t n = registry->CreateEntities<Position>(5, ents);
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(registry->Size(), 5u);

    // Too-small span -> returns 0, creates nothing.
    std::vector<Astra::Entity> tooSmall(2);
    size_t before = registry->Size();
    size_t n2 = registry->CreateEntities<Position>(5, tooSmall);
    EXPECT_EQ(n2, 0u);
    EXPECT_EQ(registry->Size(), before);

    // CreateEntitiesWith too-small span -> returns 0.
    size_t n3 = registry->CreateEntitiesWith<Position>(5, tooSmall,
        [](size_t i) { return std::make_tuple(Position{float(i), 0.0f, 0.0f}); });
    EXPECT_EQ(n3, 0u);
}

// Theme J Fix 3: batch create emits ComponentAdded with the real component pointer, not null.
TEST_F(RegistryTest, BatchCreateEmitsRealComponentPointer)
{
    using namespace Astra::Test;

    registry->EnableSignals(Astra::Signal::ComponentAdded);
    auto* signals = registry->GetSignalManager();

    void* captured = nullptr;
    float capturedX = -1.0f;
    auto handler = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& e)
        {
            captured = e.component;
            if (e.component)
                capturedX = static_cast<Position*>(e.component)->x;
        });

    std::vector<Astra::Entity> ents(3);
    registry->CreateEntitiesWith<Position>(3, ents,
        [](size_t i) { return std::make_tuple(Position{float(i) + 10.0f, 0.0f, 0.0f}); });

    EXPECT_NE(captured, nullptr);   // BUG passes nullptr
    EXPECT_GE(capturedX, 10.0f);    // a real, generated value was readable through the pointer

    signals->On<Astra::Events::ComponentAdded>().Unregister(handler);
}

// Theme F2: AddComponentByID/RemoveComponentByID must emit signals for a tag (size==0)
// component, with a non-null (sentinel) pointer.
TEST_F(RegistryTest, ByIdTagEmitsAddAndRemoveSignals)
{
    using namespace Astra::Test;

    registry->EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    auto* signals = registry->GetSignalManager();

    void* addedPtr = nullptr;
    bool removedFired = false;
    void* removedPtr = nullptr;
    auto ha = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& e) { addedPtr = e.component; });
    auto hr = signals->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& e) { removedFired = true; removedPtr = e.component; });

    Astra::Entity e = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});

    // Add the tag through the type-erased path (data=nullptr, dataSize=0 for a tag).
    ASSERT_TRUE(registry->AddComponentByID(e, Astra::TypeID<Player>::Value(), nullptr, 0));
    // Pre-fix: the tag-Add signal is dropped, so addedPtr still holds the Position pointer
    // captured during CreateEntityWith above (!= sentinel) -> RED. Post-fix: the tag-Add
    // emits the shared sentinel. Asserting equality to the sentinel pins the contract.
    EXPECT_EQ(addedPtr, Astra::EmptyComponentSentinel());

    ASSERT_TRUE(registry->RemoveComponentByID(e, Astra::TypeID<Player>::Value()));
    EXPECT_TRUE(removedFired);       // BUG: never fires
    EXPECT_EQ(removedPtr, Astra::EmptyComponentSentinel());

    signals->On<Astra::Events::ComponentAdded>().Unregister(ha);
    signals->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}

// Theme F3: GetComponentByHash for a present tag must agree with HasComponentByHash
// (non-null), not return nullptr.
TEST_F(RegistryTest, GetComponentByHashTagAgreesWithHas)
{
    using namespace Astra::Test;

    Astra::Entity e = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(registry->AddComponentByID(e, Astra::TypeID<Player>::Value(), nullptr, 0));

    uint64_t playerHash = Astra::TypeID<Player>::Hash();
    EXPECT_TRUE(registry->HasComponentByHash(e, playerHash));
    EXPECT_NE(registry->GetComponentByHash(e, playerHash), nullptr);   // BUG: nullptr contradicts Has
    EXPECT_EQ(registry->GetComponentByHash(e, playerHash), Astra::EmptyComponentSentinel());  // present tag -> sentinel

    // A component the entity does NOT have still returns nullptr.
    EXPECT_EQ(registry->GetComponentByHash(e, Astra::TypeID<Enemy>::Hash()), nullptr);
}

// Phase 2 Unit D. On pre-CompactChunks code this test EXPOSES the stale-
// chunkIndex defect in CoalesceChunks (middle-chunk erase shifts later chunks
// without updating their records). Run it at RED to confirm; CompactChunks
// makes it pass by reporting every live entity's new location.
TEST_F(RegistryTest, DefragmentPreservesEveryComponentValue)
{
    // Enough entities for several chunks under the 4KB-first ramp.
    constexpr uint32_t kCount = 8000;
    std::vector<Astra::Entity> entities;
    entities.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        entities.push_back(registry->CreateEntityWith(Astra::Test::Position{float(i), float(i * 2), float(i * 3)}));
    }

    // Punch a hole: destroy a contiguous creation-order band (lands in the
    // middle chunks) so fill ratio drops below the 0.5 trigger.
    for (uint32_t i = kCount / 4; i < (3 * kCount) / 4; ++i)
    {
        registry->DestroyEntity(entities[i]);
    }

    auto result = registry->Defragment();
    EXPECT_GT(result.entitiesMoved, 0u);

    // EVERY survivor must still resolve to ITS OWN component values.
    for (uint32_t i = 0; i < kCount / 4; ++i)
    {
        auto* p = registry->GetComponent<Astra::Test::Position>(entities[i]);
        ASSERT_NE(p, nullptr) << "entity " << i << " lost its component";
        EXPECT_EQ(p->x, float(i)) << "entity " << i << " resolves to another entity's data";
    }
    for (uint32_t i = (3 * kCount) / 4; i < kCount; ++i)
    {
        auto* p = registry->GetComponent<Astra::Test::Position>(entities[i]);
        ASSERT_NE(p, nullptr) << "entity " << i << " lost its component";
        EXPECT_EQ(p->x, float(i)) << "entity " << i << " resolves to another entity's data";
    }
}

// Move-only lifetime balance across compaction (mirrors Phase C's Tracked
// s_live guard): compaction must MoveConstruct+Destruct (or memcpy trivials),
// never duplicate or leak.
TEST_F(RegistryTest, DefragmentBalancesMoveOnlyLifetimes)
{
    const auto baseline = Astra::Test::Tracked::s_live;   // match s_live's declared type
    constexpr uint32_t kCount = 4000;
    std::vector<Astra::Entity> entities;
    entities.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        entities.push_back(registry->CreateEntityWith(Astra::Test::Tracked{int(i)}));
    }
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount);

    for (uint32_t i = 0; i < kCount; i += 2)   // half, spread across chunks
    {
        registry->DestroyEntity(entities[i]);
    }
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount / 2);

    registry->Defragment();
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount / 2) << "compaction leaked or double-destroyed";

    for (uint32_t i = 1; i < kCount; i += 2)
    {
        auto* t = registry->GetComponent<Astra::Test::Tracked>(entities[i]);
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->value, int(i));
    }
}

// Characterization tests for GetComponent, pinning per-entity VALUE correctness
// (not just non-null) across swap-remove, defragment/compaction, in-range-absent,
// and stale-handle paths. These lock current behavior ahead of the hot-path rewrite
// that reads rec->chunk directly: a wrong cached chunk pointer would surface here as
// an entity reading a NEIGHBOR's value, which the per-entity x-value asserts catch.
TEST_F(RegistryTest, GetComponentAfterSwapRemove)
{
    using namespace Astra::Test;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i)
    {
        auto e = registry->CreateEntity();
        registry->EmplaceComponent<Position>(e, float(i), 0.f, 0.f);
        es.push_back(e);
    }
    registry->DestroyEntity(es[10]);              // former last entity swaps into slot 10
    for (int i = 0; i < 50; ++i)
    {
        if (i == 10) continue;
        auto* p = registry->GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));          // each entity still reads ITS OWN value
    }
}

TEST_F(RegistryTest, GetComponentAfterDefragment)
{
    using namespace Astra::Test;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 60; ++i)
    {
        auto e = registry->CreateEntity();
        registry->EmplaceComponent<Position>(e, float(i), 0.f, 0.f);
        es.push_back(e);
    }
    for (int i = 0; i < 60; i += 2)
        registry->DestroyEntity(es[i]);
    registry->Defragment();                       // CompactChunks moves survivors
    for (int i = 1; i < 60; i += 2)
    {
        auto* p = registry->GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));
    }
}

TEST_F(RegistryTest, GetComponentAbsentAndStale)
{
    using namespace Astra::Test;
    auto e = registry->CreateEntity();
    registry->EmplaceComponent<Position>(e, 1.f, 2.f, 3.f);
    EXPECT_EQ(registry->GetComponent<Velocity>(e), nullptr);   // in-range absent (idToColumn < 0 path)
    auto dead = registry->CreateEntity();
    registry->EmplaceComponent<Position>(dead, 9.f, 9.f, 9.f);
    registry->DestroyEntity(dead);
    EXPECT_EQ(registry->GetComponent<Position>(dead), nullptr); // stale handle (version guard)
}

// ======================= Enableable components (Task 2) =======================
//
// EnA / EnB are the suite's two ASTRA_ENABLEABLE components (spec §2). Task 1
// opted Hierarchy + Timer in; Task 2 stores + toggles + preserves their per-chunk
// disabled bits. Do NOT introduce new component types -- reuse these.
using EnA = Astra::Test::Hierarchy;
using EnB = Astra::Test::Timer;

// Invariant seam (spec §14.2): on every enableable column of a chunk,
//   disabledCount == popcount(disabledWords)  AND  every bit at a slot >= count is 0.
// Placed as a free helper (the file has no prior invariant-helper idiom).
static void ExpectDisabledInvariant(Astra::ArchetypeChunk& chunk, const Astra::ArchetypeColumnMeta& meta)
{
    const size_t count = chunk.GetCount();
    const size_t capacity = chunk.GetCapacity();
    const size_t wordCount = (capacity + 63) / 64;
    for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
    {
        const int col = static_cast<int>(meta.enableableColumns[e]);
        const uint64_t* words = chunk.GetDisabledWords(col);
        ASSERT_NE(words, nullptr) << "enableable column " << col << " must carry disabled words";

        uint32_t pop = 0;
        for (size_t w = 0; w < wordCount; ++w)
            pop += static_cast<uint32_t>(std::popcount(words[w]));
        EXPECT_EQ(pop, chunk.GetDisabledCount(col))
            << "disabledCount != popcount on column " << col;

        // No bit may be set at a slot at or beyond the live count.
        for (size_t i = count; i < wordCount * 64; ++i)
            EXPECT_FALSE(chunk.IsDisabled(col, i))
                << "tail bit set at slot " << i << " (count=" << count << ") on column " << col;
    }
}

static void ExpectAllChunksInvariant(Astra::Registry& reg)
{
    for (Astra::Archetype* arch : reg.GetArchetypeManager()->GetArchetypes())
    {
        const Astra::ArchetypeColumnMeta& meta = arch->GetColumnMeta();
        if (meta.enableableColumnCount == 0)
            continue;
        for (const auto& chunkPtr : arch->GetChunks())
            ExpectDisabledInvariant(*chunkPtr, meta);
    }
}

TEST_F(RegistryTest, EnableToggleBehaviorTable)
{
    auto e = registry->CreateEntity<EnA>();
    EXPECT_TRUE(registry->IsEnabled<EnA>(e));                    // born enabled
    EXPECT_TRUE(registry->SetEnabled<EnA>(e, false));           // disable: applied
    EXPECT_FALSE(registry->IsEnabled<EnA>(e));
    EXPECT_TRUE(registry->SetEnabled<EnA>(e, false));           // idempotent: true, no change
    EXPECT_TRUE(registry->GetComponent<EnA>(e) != nullptr);     // existence never lies
    EXPECT_TRUE(registry->HasComponent<EnA>(e));
    auto missing = registry->CreateEntity();                    // no EnA
    EXPECT_FALSE(registry->SetEnabled<EnA>(missing, false));
    EXPECT_FALSE(registry->IsEnabled<EnA>(missing));
    registry->DestroyEntity(e);
    EXPECT_FALSE(registry->SetEnabled<EnA>(e, true));           // stale: no-op false
    ExpectAllChunksInvariant(*registry);
}

TEST_F(RegistryTest, EnableSignalsFireOnlyOnGenuineChange)
{
    registry->EnableSignals(Astra::Signal::ComponentEnabled);
    registry->EnableSignals(Astra::Signal::ComponentDisabled);
    int enabled = 0, disabled = 0;
    registry->GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const auto&) { ++disabled; });
    registry->GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const auto&) { ++enabled; });
    auto e = registry->CreateEntity<EnA>();
    registry->SetEnabled<EnA>(e, false);
    registry->SetEnabled<EnA>(e, false);      // idempotent: silent
    registry->SetEnabled<EnA>(e, true);
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
}

TEST_F(RegistryTest, DisabledBitSurvivesSwapRemove)
{
    // Disable a NON-last entity, destroy the last one in the same chunk (swap
    // fills the vacated slot), and verify both entities' states by identity.
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(registry->CreateEntity<EnA>());
    registry->SetEnabled<EnA>(es[2], false);
    registry->DestroyEntity(es[7]);
    EXPECT_FALSE(registry->IsEnabled<EnA>(es[2]));
    for (int i = 0; i < 7; ++i) if (i != 2) EXPECT_TRUE(registry->IsEnabled<EnA>(es[i]));
    // Now destroy a MIDDLE entity so the swapped-in survivor was the disabled one's neighbor:
    registry->SetEnabled<EnA>(es[6], false);
    registry->DestroyEntity(es[2]);           // slot 2 refilled by the (disabled) last entity or a survivor
    EXPECT_FALSE(registry->IsEnabled<EnA>(es[6]));   // identity-tracked, wherever it now lives
    ExpectAllChunksInvariant(*registry);
}

TEST_F(RegistryTest, DisabledBitSurvivesArchetypeTransition)
{
    using namespace Astra::Test;
    auto e = registry->CreateEntity<EnA>();
    registry->SetEnabled<EnA>(e, false);
    registry->AddComponent(e, Position{1, 2, 3});     // transition: EnA carries its bit
    EXPECT_FALSE(registry->IsEnabled<EnA>(e));
    registry->RemoveComponent<Position>(e);           // transition back
    EXPECT_FALSE(registry->IsEnabled<EnA>(e));
    registry->AddComponent(e, EnB{});                 // fresh component: born enabled
    EXPECT_TRUE(registry->IsEnabled<EnB>(e));
    EXPECT_FALSE(registry->IsEnabled<EnA>(e));         // untouched by EnB's arrival
    ExpectAllChunksInvariant(*registry);
}

TEST_F(RegistryTest, BatchCreateBornEnabled)
{
    constexpr size_t kCount = 3000;                   // spans chunks (grow-as-populate)
    std::vector<Astra::Entity> ents(kCount);
    size_t created = registry->CreateEntitiesWith<EnA>(kCount, std::span{ents},
        [](size_t) { return std::tuple{EnA{}}; });
    ASSERT_EQ(created, kCount);
    for (auto e : ents) ASSERT_TRUE(registry->IsEnabled<EnA>(e));
    ExpectAllChunksInvariant(*registry);
}

// Invariant seam #1: after a 100-toggle random walk, popcount == disabledCount
// and no tail bit is set, with every entity's state matching the model.
TEST_F(RegistryTest, DisabledInvariantAfter100ToggleWalk)
{
    constexpr size_t N = 64;
    std::vector<Astra::Entity> es;
    for (size_t i = 0; i < N; ++i) es.push_back(registry->CreateEntity<EnA>());

    std::mt19937 rng(0xC0FFEEu);
    std::vector<bool> disabled(N, false);
    for (int step = 0; step < 100; ++step)
    {
        const size_t k = rng() % N;
        const bool enable = (rng() & 1u) != 0;
        registry->SetEnabled<EnA>(es[k], enable);
        disabled[k] = !enable;
    }
    for (size_t i = 0; i < N; ++i)
        EXPECT_EQ(registry->IsEnabled<EnA>(es[i]), !disabled[i]) << "model mismatch at " << i;
    ExpectAllChunksInvariant(*registry);
}

// Invariant seam #2: after a destroy-half loop, survivors keep their state and
// the bookkeeping stays consistent (swap-remove carry + count).
TEST_F(RegistryTest, DisabledInvariantAfterDestroyHalf)
{
    constexpr size_t N = 500;
    std::vector<Astra::Entity> es;
    for (size_t i = 0; i < N; ++i) es.push_back(registry->CreateEntity<EnA>());
    for (size_t i = 0; i < N; ++i) if ((i % 2) == 0) registry->SetEnabled<EnA>(es[i], false);
    // Destroy the odd (enabled) half; the disabled evens survive.
    for (size_t i = 1; i < N; i += 2) registry->DestroyEntity(es[i]);
    for (size_t i = 0; i < N; i += 2) EXPECT_FALSE(registry->IsEnabled<EnA>(es[i])) << "survivor " << i;
    ExpectAllChunksInvariant(*registry);
}

// Invariant seam #3 (spec §12.5): add/remove-component churn over disabled holders
// then Registry::Defragment() (which drives Archetype::CompactChunks). Every
// identity-tracked IsEnabled state must be unchanged across the compaction, and
// the per-chunk invariant must hold afterward.
TEST_F(RegistryTest, DisabledInvariantSurvivesChurnAndDefragment)
{
    using namespace Astra::Test;
    constexpr size_t N = 3000;                        // spans chunks
    std::vector<Astra::Entity> es(N);
    size_t created = registry->CreateEntitiesWith<EnA>(N, std::span{es},
        [](size_t) { return std::tuple{EnA{}}; });
    ASSERT_EQ(created, N);

    std::vector<bool> expectedEnabled(N, true);
    for (size_t i = 0; i < N; ++i)
        if ((i % 3) == 0) { registry->SetEnabled<EnA>(es[i], false); expectedEnabled[i] = false; }

    // Two archetype transitions per churned entity (EnA -> EnA+Position -> EnA):
    // the disabled bit must carry through MoveAndAdd and MoveEntityFrom both ways.
    for (size_t i = 0; i < N; i += 3)
    {
        registry->AddComponent(es[i], Position{1, 2, 3});
        registry->RemoveComponent<Position>(es[i]);
    }
    for (size_t i = 0; i < N; ++i)
        ASSERT_EQ(registry->IsEnabled<EnA>(es[i]), expectedEnabled[i]) << "churn lost bit at " << i;

    // chunkUtilizationThreshold == 1.0 => fragmentationThreshold 0.0 => CompactChunks
    // runs on every archetype with more than one chunk, regardless of fill.
    Astra::Registry::DefragmentationOptions opts;
    opts.chunkUtilizationThreshold = 1.0f;
    registry->Defragment(opts);

    for (size_t i = 0; i < N; ++i)
        EXPECT_EQ(registry->IsEnabled<EnA>(es[i]), expectedEnabled[i]) << "defrag lost bit at " << i;
    ExpectAllChunksInvariant(*registry);
}
