#include <gtest/gtest.h>
#include <cstring>
#include <span>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Archetype/ArchetypeManager.hpp"
#include "Astra/Entity/EntityTable.hpp"
#include "Astra/Serialization/BinaryArchive.hpp"
#include "Astra/Serialization/BinaryReader.hpp"
#include "Astra/Serialization/BinaryWriter.hpp"

class ArchetypeManagerTest : public ::testing::Test
{
protected:
    // The manager now validates/locates through a shared EntityRecord table that
    // EntityManager owns in production. These standalone tests own it in the
    // fixture; declared first so it outlives `manager` (which holds a raw pointer).
    Astra::EntityTable m_table;
    std::shared_ptr<Astra::ComponentRegistry> componentRegistry;
    std::unique_ptr<Astra::ArchetypeManager> manager;
    std::vector<Astra::Entity> testEntities;

    void SetUp() override
    {
        // Create component registry first
        componentRegistry = std::make_shared<Astra::ComponentRegistry>();

        // Register test components (only the ones actually used by this
        // file's tests - Transform/Name/Physics/Player/Enemy were registered
        // but never exercised by any TEST_F here; trimmed in the 2026-07-10
        // test-suite audit).
        using namespace Astra::Test;
        componentRegistry->RegisterComponents<Position, Velocity, Health>();

        // Seed a version for every id these tests use (they build handles as
        // Entity(id, 1)). Without this the manager's version-checked find-guard
        // would reject every lookup, since a fresh table has version 0 in all
        // slots. In production EntityManager::Create does this via SetVersion.
        for (Astra::Entity::StorageType id = 0; id < 10000; ++id)
            m_table.SetVersion(id, 1);

        // Create manager with the component registry and the shared record table
        manager = std::make_unique<Astra::ArchetypeManager>(
            componentRegistry, Astra::ArchetypeChunkPool::Config{}, &m_table);

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

// Task 2 (record->chunk cache): the chunk-pointer invariant every located record
// must satisfy (spec 4.1). Whenever a record is located, its cached `chunk` must be
// EXACTLY archetype->GetChunks()[location.GetChunkIndex()].get(); a stale pointer
// here is a use-after-free on the get hot path.
static void ExpectChunkInvariant(Astra::ArchetypeManager* m, Astra::Entity e)
{
    const Astra::EntityRecord* rec = m->GetEntityRecord(e);
    ASSERT_NE(rec, nullptr);
    ASSERT_NE(rec->archetype, nullptr);
    ASSERT_TRUE(rec->location.IsValid());
    ASSERT_LT(rec->location.GetChunkIndex(), rec->archetype->GetChunks().size());
    EXPECT_EQ(rec->chunk,
              rec->archetype->GetChunks()[rec->location.GetChunkIndex()].get());
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_CreateTransitionRemove)
{
    using namespace Astra::Test;
    Astra::Entity e = testEntities[0];
    manager->AddEntity(e);                       // create (root archetype)
    ExpectChunkInvariant(manager.get(), e);
    manager->AddComponent<Position>(e, 1.f, 2.f, 3.f);   // transition add
    ExpectChunkInvariant(manager.get(), e);
    manager->AddComponent<Velocity>(e);          // second transition
    ExpectChunkInvariant(manager.get(), e);
    manager->RemoveComponent<Velocity>(e);       // transition remove
    ExpectChunkInvariant(manager.get(), e);
    manager->RemoveEntity(e);                    // clear: all storage fields null
    const Astra::EntityRecord* rec = manager->GetEntityRecord(e);
    // GetEntityRecord version-guards on archetype != null, so a fully cleared record
    // reads back as nullptr -- fetch the raw slot to inspect the storage fields.
    rec = m_table.GetRecord(e.GetID());
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->archetype, nullptr);
    EXPECT_EQ(rec->chunk, nullptr);
    EXPECT_FALSE(rec->location.IsValid());
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_SwapRemoveFixup)
{
    using namespace Astra::Test;
    // Fill one archetype so removing a middle entity swap-moves the last one.
    for (int i = 0; i < 50; ++i)
    {
        manager->AddEntity(testEntities[i]);
        manager->AddComponent<Position>(testEntities[i], float(i), 0.f, 0.f);
    }
    manager->RemoveEntity(testEntities[10]);     // last entity swaps into slot 10
    for (int i = 0; i < 50; ++i)
    {
        if (i == 10) continue;
        ExpectChunkInvariant(manager.get(), testEntities[i]);
    }
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_Defragment)
{
    using namespace Astra;
    using namespace Astra::Test;

    // Enough Position entities to span multiple chunks: a fresh chunk is floored at
    // 4KB (~170 Position entities) and CompactChunks only acts on >1 chunk. This
    // reuses only Position (no new component types) and builds handles directly (the
    // fixture seeds versions for ids 0..10000).
    constexpr int N = 600;
    std::vector<Entity> ents;
    ents.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        Entity e(static_cast<uint32_t>(i), 1);
        manager->AddEntity(e);
        manager->AddComponent<Position>(e, float(i), 0.f, 0.f);
        ents.push_back(e);
    }

    Archetype* arch = manager->GetEntityRecord(ents[0])->archetype;
    ASSERT_NE(arch, nullptr);
    ASSERT_GT(arch->GetChunks().size(), 1u);   // precondition: multi-chunk archetype

    // Drive the exact record-rewrite path Registry::Defragment uses: CompactChunks
    // repacks every live entity into FRESH chunks (freeing the old ones) and reports
    // every one's new location. Those must flow back through SetEntityLocation (funnel
    // site d) to re-cache each record's chunk -- otherwise the cached pointer dangles
    // at a just-freed old chunk (UAF). Same locations, different chunk objects, so
    // this specifically catches a chunk pointer that was NOT refreshed.
    auto [chunksFreed, movedEntities] = arch->CompactChunks();
    (void)chunksFreed;
    ASSERT_FALSE(movedEntities.empty());
    for (const auto& [entity, newLocation] : movedEntities)
        manager->SetEntityLocation(entity, arch, newLocation);

    for (int i = 0; i < N; ++i)
        ExpectChunkInvariant(manager.get(), ents[i]);
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_Deserialize)
{
    using namespace Astra;
    using namespace Astra::Test;
    for (int i = 0; i < 30; ++i)
    {
        manager->AddEntity(testEntities[i]);
        manager->AddComponent<Position>(testEntities[i], float(i), 0.f, 0.f);
    }

    // Serialize the populated manager into an in-memory archive (header + payload).
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        BinaryHeader header;                     // stamps magic/current version/endianness
        writer.WriteHeader(header);
        manager->Serialize(writer);
        writer.FinalizeHeader();
        ASSERT_FALSE(writer.HasError());
    }

    // Deserialize into a fresh manager over its own record table. In production
    // EntityManager restores versions before ArchetypeManager::Deserialize; mirror
    // that by seeding versions for the reloaded ids (same as the fixture does).
    EntityTable table2;
    for (Entity::StorageType id = 0; id < 200; ++id)
        table2.SetVersion(id, 1);
    ArchetypeManager manager2(componentRegistry, ArchetypeChunkPool::Config{}, &table2);

    BinaryReader reader{std::span<const std::byte>(buffer)};
    ASSERT_TRUE(reader.ReadHeader().IsOk());
    ASSERT_TRUE(manager2.Deserialize(reader));

    // Every reloaded record must have its chunk pointer re-derived (funnel site e).
    for (int i = 0; i < 30; ++i)
        ExpectChunkInvariant(&manager2, testEntities[i]);
}

// Lever 3 Task 2 (chunk-run bulk create + run-hoisted record writes): a batch
// AddEntitiesWith spanning multiple chunks must leave every record's cached chunk
// pointer EXACTLY GetChunks()[chunkIndex].get(). The manager's record loop derives
// that pointer once per chunk run and feeds the 4-arg SetRecordLocation funnel; a
// stale or mis-hoisted pointer here is a use-after-free on the get hot path. This
// is the record-invariant assertion loop the brief calls for -- it lives here (not
// in RegistryTest) because ExpectChunkInvariant is file-local to this fixture.
// Reuses only Position/Velocity (no new component types); handles are seeded 0..10000.
TEST_F(ArchetypeManagerTest, RecordChunkInvariant_BatchAddEntitiesWith)
{
    using namespace Astra;
    using namespace Astra::Test;

    constexpr int N = 3000;   // spans multiple 4KB-floored chunks
    std::vector<Entity> ents;
    ents.reserve(N);
    for (int i = 0; i < N; ++i)
        ents.emplace_back(static_cast<uint32_t>(i), 1);

    manager->AddEntitiesWith<Position, Velocity>(
        std::span<const Entity>(ents),
        [](size_t i) { return std::make_tuple(Position{float(i), 0.f, 0.f},
                                              Velocity{float(i) * 2.f, 0.f, 0.f}); });

    Archetype* arch = manager->GetEntityRecord(ents[0])->archetype;
    ASSERT_NE(arch, nullptr);
    ASSERT_GT(arch->GetChunks().size(), 1u);   // precondition: batch spans >1 chunk

    for (int i = 0; i < N; ++i)
    {
        ExpectChunkInvariant(manager.get(), ents[i]);
        // Per-entity value integrity across the run-hoisted writes.
        const EntityRecord* rec = manager->GetEntityRecord(ents[i]);
        ASSERT_NE(rec, nullptr);
        Position* p = arch->GetComponent<Position>(rec->location);
        Velocity* v = arch->GetComponent<Velocity>(rec->location);
        ASSERT_NE(p, nullptr);
        ASSERT_NE(v, nullptr);
        EXPECT_FLOAT_EQ(p->x, float(i));
        EXPECT_FLOAT_EQ(v->dx, float(i) * 2.f);
    }
}

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
    
    // Create second manager with shared component registry and its own record table
    Astra::EntityTable table2;
    for (Astra::Entity::StorageType id = 0; id < 200; ++id)
        table2.SetVersion(id, 1);
    Astra::ArchetypeManager manager2(componentRegistry, Astra::ArchetypeChunkPool::Config{}, &table2);

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

    // Beyond the seeded id range: record-absent (rec == nullptr) find-guard arm.
    // The fixture only seeds versions for ids [0, 10000), all of which live in
    // the default 65536-entity segment 0; id 70000 falls in segment 1, which was
    // never created, so GetRecord returns nullptr.
    Astra::Entity beyondRangeEntity(70000, 1);

    // Try to get component from a never-seeded entity
    EXPECT_EQ(manager->GetComponent<Position>(beyondRangeEntity), nullptr);

    // Try to add component to a never-seeded entity
    EXPECT_EQ(manager->AddComponent<Position>(beyondRangeEntity), nullptr);

    // Try to remove component from a never-seeded entity
    EXPECT_FALSE(manager->RemoveComponent<Position>(beyondRangeEntity));

    // Try to remove a never-seeded entity
    manager->RemoveEntity(beyondRangeEntity); // Should not crash

    // Seeded id but wrong version: version-mismatch find-guard arm. The fixture
    // seeds id 1 with version 1; version 7 doesn't match the stored version.
    Astra::Entity versionMismatchEntity(1, 7);

    // Try to get component from a version-mismatched entity
    EXPECT_EQ(manager->GetComponent<Position>(versionMismatchEntity), nullptr);

    // Try to add component to a version-mismatched entity
    EXPECT_EQ(manager->AddComponent<Position>(versionMismatchEntity), nullptr);

    // Try to remove component from a version-mismatched entity
    EXPECT_FALSE(manager->RemoveComponent<Position>(versionMismatchEntity));

    // Try to remove a version-mismatched entity
    manager->RemoveEntity(versionMismatchEntity); // Should not crash
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

// Theme G Fix 1: swap-and-pop removal must destruct the moved-from source slot.
TEST_F(ArchetypeManagerTest, RemoveEntityDestructsMovedFromSourceSlot)
{
    using Astra::Test::Tracked;
    componentRegistry->RegisterComponents<Tracked>();
    Tracked::s_live = 0;

    Astra::Entity e0(200, 1), e1(201, 1), e2(202, 1);
    manager->AddEntityWith(e0, Tracked{10});
    manager->AddEntityWith(e1, Tracked{11});
    manager->AddEntityWith(e2, Tracked{12});
    ASSERT_EQ(Tracked::s_live, 3);

    // Remove the first (non-tail) entity: swap-and-pop moves e2's Tracked into
    // slot 0, leaving the last slot as a moved-from object that must be destructed.
    manager->RemoveEntity(e0);

    EXPECT_EQ(Tracked::s_live, 2);   // BUG leaves 3 (moved-from source slot never destructed)
    EXPECT_EQ(manager->GetComponent<Tracked>(e2)->value, 12);
}

// Theme G Fix 3: MoveAndAddByID must construct a move-only new component.
TEST_F(ArchetypeManagerTest, MoveAndAddByIDMoveOnlyComponentPreservesValue)
{
    using Astra::Test::Position;
    using Astra::Test::Tracked;
    componentRegistry->RegisterComponents<Tracked>();
    Tracked::s_live = 0;

    Astra::Entity e(210, 1);
    manager->AddEntityWith(e, Position{1.0f, 2.0f, 3.0f});   // e now in {Position}

    // Add the move-only component via the type-erased path (as CommandBuffer flush does).
    Tracked src{42};
    const bool ok = manager->AddComponentByID(
        e, Astra::TypeID<Tracked>::Value(), &src, sizeof(Tracked));
    ASSERT_TRUE(ok);

    ASSERT_TRUE(manager->HasComponent<Tracked>(e));
    Tracked* t = manager->GetComponent<Tracked>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->value, 42);   // BUG leaves 0 (slot never constructed; chunk is zeroed)
}

// Theme G Fix 4: RemoveComponents<T> must report the true moved count, and must
// not lose entities, when the destination archetype cannot allocate a chunk.
TEST_F(ArchetypeManagerTest, RemoveComponentsReportsActualCountOnChunkExhaustion)
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;

    // Pool room for root (chunk #1) + {Position,Velocity} (chunk #2) only; the
    // remove target {Position} cannot get a 3rd chunk.
    Astra::ArchetypeChunkPool::Config poolConfig;
    poolConfig.chunkSize      = 4096;   // MIN_CHUNK_SIZE
    poolConfig.chunksPerBlock = 1;
    poolConfig.maxChunks      = 2;
    poolConfig.useHugePages   = false;

    Astra::EntityTable localTable;
    for (Astra::Entity::StorageType id = 300; id < 305; ++id)
        localTable.SetVersion(id, 1);
    Astra::ArchetypeManager localManager(componentRegistry, poolConfig, &localTable);

    std::vector<Astra::Entity> ents;
    for (int i = 0; i < 5; ++i)
    {
        Astra::Entity e(300 + i, 1);
        localManager.AddEntityWith(e, Position{1, 2, 3}, Velocity{4, 5, 6});
        ents.push_back(e);
    }
    ASSERT_TRUE(localManager.HasComponent<Velocity>(ents[0]));   // src built OK (1 chunk)

    // Removing Velocity needs a fresh chunk for {Position}; the pool is exhausted,
    // so nothing actually moves.
    std::span<Astra::Entity> span(ents.data(), ents.size());
    size_t removed = localManager.RemoveComponents<Velocity>(span);

    EXPECT_EQ(removed, 0u);   // BUG reports 5
    for (auto e : ents)
    {
        EXPECT_TRUE(localManager.HasComponent<Velocity>(e));   // entities untouched in src
    }
}

// W2: after an empty archetype is defragmented away, cached edges to it must be
// invalidated (no dangling pointer) -- subsequent transitions must recompute
// correctly. This test passes both before and after the per-archetype-edge
// rewire (the old ArchetypeGraph also invalidated on removal); its guarding
// value is the POST-rewire path, where invalidation lives in the manager's
// scan-all ClearEdgesTo that must run BEFORE the archetype's unique_ptr is reset.
TEST_F(ArchetypeManagerTest, EdgesInvalidatedWhenArchetypeDefragmented)
{
    using namespace Astra;
    using namespace Astra::Test;

    Entity e0(1, 1);
    Entity e1(2, 1);
    manager->AddEntityWith<Position>(e0, Position{1, 0, 0});   // archetype {Position}
    manager->AddEntityWith<Position>(e1, Position{2, 0, 0});

    // Transition e0 {Position} -> {Position,Velocity}: caches {Position}'s add-edge for Velocity.
    manager->AddComponent<Velocity>(e0, Velocity{7, 7, 7});
    // Empty the {Position,Velocity} archetype again.
    manager->RemoveComponent<Velocity>(e0);

    // Force removal of the now-empty {Position,Velocity} archetype (default keeps >= 8).
    ArchetypeManager::DefragmentOptions opts;
    opts.minArchetypesToKeep = 1;
    auto result = manager->Defragment(opts);
    EXPECT_GE(result.emptyArchetypesRemoved, 1u);

    // The cached {Position}->Velocity add-edge now points at a freed archetype IF
    // invalidation failed. A correct recompute yields e1 with the right Velocity.
    Velocity* v = manager->AddComponent<Velocity>(e1, Velocity{9, 9, 9});
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(v->dx, 9.0f);              // Velocity fields are dx/dy/dz
    Position* p = manager->GetComponent<Position>(e1);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 2.0f);               // still correct after the transition
}

// Review Task 5 (latent UAF): loading a v2-format archive OVER a manager that
// has already cached a root transition edge must null that edge. Deserialize
// mass-frees every non-root archetype (the pop_back loop) but carries the root
// at index 0 over as a reused object. The v3 branch replaces index 0 outright,
// so a v3 round-trip is safe -- and does NOT exercise this bug. The v2-and-
// earlier branch (firstArchetypeIndex == 1) never touches index 0, so pre-fix
// the carried-over root kept its cached add-edge pointing at the just-freed
// {Position} archetype -- a dangling pointer the next root AddComponent would
// follow into freed memory (UAF).
//
// This is a deterministic state-check + behavioral path-exerciser. The root-
// edge-is-null EXPECT below FAILS pre-fix (stale non-null pointer survives the
// v2 load) and PASSES post-fix (ClearAllEdges nulled it) -- a genuine RED->GREEN
// guard that never DEREFERENCES the freed archetype (it only compares the stored
// pointer value against nullptr, which is well-defined against the still-live
// root). Full UAF detection -- actually following the dangling edge -- needs an
// ASan CI run, a tracked deferred follow-up.
TEST_F(ArchetypeManagerTest, V2LoadNullsSurvivingRootStaleEdges)
{
    using namespace Astra::Test;

    // Capture the root archetype via a component-less entity that stays in it.
    // The root object lives at m_archetypes[0] and survives Deserialize, so this
    // raw pointer remains valid across the load.
    Astra::Entity keeper(1, 1);
    manager->AddEntity(keeper);
    Astra::Archetype* root = manager->GetEntityRecord(keeper)->archetype;
    ASSERT_NE(root, nullptr);

    // Cache a root add-edge for Position: adding Position to a root-resident
    // entity transitions root -> {Position} and stores that edge on the root.
    Astra::Entity mover(2, 1);
    manager->AddEntity(mover);
    ASSERT_NE(manager->AddComponent<Position>(mover), nullptr);
    const Astra::ComponentID posId = Astra::TypeID<Position>::Value();
    ASSERT_NE(root->GetAddEdge(posId), nullptr);   // sanity: the edge is cached

    // Build a minimal v2-format archive. Only the header's version field is load-
    // bearing here -- it drives ArchetypeManager::Deserialize down the v2 branch
    // (firstArchetypeIndex == 1), which carries the root over instead of replacing
    // it. The payload is an empty archetype/entity set (archetypeCount == 0), which
    // keeps the crafted buffer trivial while still exercising the v2 path: the bug
    // is about the PRE-LOAD cached edge to the {Position} archetype (freed by
    // Deserialize's mass-free), independent of what the archive itself contains.
    std::vector<std::byte> buf;
    {
        Astra::BinaryHeader header;                  // ctor stamps magic/endianness/current version...
        header.version = 2;                           // ...override to the pre-v3 format
        const auto* raw = reinterpret_cast<const std::byte*>(&header);
        buf.insert(buf.end(), raw, raw + sizeof(header));

        Astra::BinaryWriter writer(buf);             // memory mode appends after the header bytes
        writer(static_cast<uint32_t>(0));             // archetypeCount = 0
        writer(static_cast<uint32_t>(0));             // entityCount    = 0
        ASSERT_FALSE(writer.HasError());
    }

    Astra::BinaryReader reader{std::span<const std::byte>(buf)};
    ASSERT_TRUE(reader.ReadHeader().IsOk());           // sets the reader's version to 2
    ASSERT_EQ(reader.GetVersion(), 2u);                // confirm we drive the v2 branch
    ASSERT_TRUE(manager->Deserialize(reader));

    // Deterministic guard: the {Position} archetype the root's add-edge pointed at
    // was freed by Deserialize's mass-free. Pre-fix the v2 branch left the edge
    // dangling (non-null); post-fix ClearAllEdges nulled it. Comparison only -- the
    // freed archetype is never dereferenced.
    EXPECT_EQ(root->GetAddEdge(posId), nullptr);

    // Behavioral path-exerciser: a post-load root transition must recompute cleanly
    // rather than follow the stale edge. Pre-fix this is the actual UAF site
    // (GetArchetypeWithAdded would return the freed archetype); post-fix the null
    // edge forces a correct recompute into a fresh {Position} archetype.
    Astra::Entity fresh(3, 1);
    manager->AddEntity(fresh);
    Position* p = manager->AddComponent<Position>(fresh, 1.0f, 2.0f, 3.0f);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 1.0f);
    EXPECT_FLOAT_EQ(p->y, 2.0f);
    EXPECT_FLOAT_EQ(p->z, 3.0f);
    EXPECT_EQ(manager->GetComponent<Position>(fresh), p);
}

// W4: fast-append must not leave provided components default/zeroed, and must write each
// entity's values into the correct packed column (no cross-column or stale-slot corruption).
TEST_F(ArchetypeManagerTest, FastAppendPreservesAllProvidedValues)
{
    using namespace Astra;
    using namespace Astra::Test;

    constexpr int N = 500;
    std::vector<Entity> ents;
    for (int i = 0; i < N; ++i)
    {
        Entity e(static_cast<uint32_t>(i + 1), 1);
        manager->AddEntityWith<Position, Velocity>(
            e, Position{float(i), float(i) + 0.5f, float(i) + 0.25f}, Velocity{float(-i), 0.0f, 0.0f});
        ents.push_back(e);
    }

    for (int i = 0; i < N; ++i)
    {
        Position* p = manager->GetComponent<Position>(ents[i]);
        Velocity* v = manager->GetComponent<Velocity>(ents[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        ASSERT_NE(v, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));
        EXPECT_FLOAT_EQ(p->y, float(i) + 0.5f);
        EXPECT_FLOAT_EQ(p->z, float(i) + 0.25f);
        EXPECT_FLOAT_EQ(v->dx, float(-i));
        EXPECT_FLOAT_EQ(v->dy, 0.0f);
        EXPECT_FLOAT_EQ(v->dz, 0.0f);
    }
}

// W3: trivially-copyable components move via memcpy across an archetype transition and keep
// their exact values. Position is trivially copyable, so the cross-archetype move takes the
// std::memcpy fast path; this guards that the bit-copy lands the right bytes in the right slot.
TEST_F(ArchetypeManagerTest, TrivialTransitionMovePreservesValues)
{
    using namespace Astra;
    using namespace Astra::Test;

    Entity e(2, 1);
    manager->AddEntityWith<Position>(e, Position{3, 4, 5});   // archetype {Position}
    manager->AddComponent<Velocity>(e, Velocity{6, 7, 8});    // transition memcpy's Position across

    Position* p = manager->GetComponent<Position>(e);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 3.0f);
    EXPECT_FLOAT_EQ(p->y, 4.0f);
    EXPECT_FLOAT_EQ(p->z, 5.0f);

    Velocity* v = manager->GetComponent<Velocity>(e);
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(v->dx, 6.0f);
    EXPECT_FLOAT_EQ(v->dy, 7.0f);
    EXPECT_FLOAT_EQ(v->dz, 8.0f);
}

// W3 (LOAD-BEARING): a non-trivially-relocatable component MUST take the MoveConstruct path
// across a transition, never memcpy. Tracked::s_live counts live instances; MoveConstruct does
// ++s_live but a bitwise memcpy does not -- so after the source slot is destructed a buggy
// memcpy leaves s_live imbalanced by one. `value` alone can't catch it (both copy the int);
// s_live can.
//
// This test exercises BOTH cross-archetype move paths with Tracked as the moved (matched) non-
// trivial column: AddComponent routes through MoveAndAdd (the add-transition move), and
// RemoveComponent routes through MoveEntityFrom (the remove-transition move this task rewrites
// with the merge-join + memcpy fast path). Without the RemoveComponent leg no test touches
// MoveEntityFrom's matched-column path, so a blanket memcpy there would pass the whole suite --
// the RemoveComponent<Position> + s_live guard below is what makes the is_trivially_copyable
// gate load-bearing (verified RED against a deliberately un-gated blanket memcpy).
TEST_F(ArchetypeManagerTest, ComplexTransitionMoveUsesMoveConstructNotMemcpy)
{
    using namespace Astra;
    using namespace Astra::Test;

    // Tracked is not registered by the fixture (SetUp only registers Position/Velocity/Health);
    // register it here, matching the other Tracked tests in this file. Registration constructs
    // no instances, so it leaves s_live untouched.
    componentRegistry->RegisterComponents<Tracked>();

    const int baseLive = Tracked::s_live;
    Entity e(3, 1);
    manager->AddEntityWith<Tracked>(e, Tracked{42});          // archetype {Tracked}; net +1 live
    ASSERT_EQ(Tracked::s_live, baseLive + 1);

    // Add transition {Tracked} -> {Tracked, Position} (MoveAndAdd): MoveConstruct Tracked into
    // the new archetype (++s_live), then the source slot is destructed by the caller (--s_live).
    // Net change ZERO.
    manager->AddComponent<Position>(e, Position{1, 2, 3});
    EXPECT_EQ(Tracked::s_live, baseLive + 1) << "memcpy of a move-only type imbalances s_live";
    Tracked* t = manager->GetComponent<Tracked>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->value, 42);                                  // value survives the move

    // Remove transition {Tracked, Position} -> {Tracked} (MoveEntityFrom, this task's merge-join):
    // Tracked is a MATCHED non-trivial column -> it must MoveConstruct (++s_live), then the source
    // slot is destructed (--s_live) -> net ZERO. Position is source-only and is dropped. A blanket
    // memcpy of Tracked here would skip the ++ and leave s_live one short.
    ASSERT_TRUE(manager->RemoveComponent<Position>(e));
    EXPECT_EQ(Tracked::s_live, baseLive + 1) << "MoveEntityFrom bit-copied a move-only column";
    Tracked* t2 = manager->GetComponent<Tracked>(e);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->value, 42);                                 // value survives the reverse move
    EXPECT_EQ(manager->GetComponent<Position>(e), nullptr);  // source-only Position was dropped

    manager->RemoveEntity(e);                                 // destructs the one live Tracked
    EXPECT_EQ(Tracked::s_live, baseLive) << "destroy must return the live count to baseline";
}

// W3: a trivially-copyable MATCHED column moved through MoveEntityFrom (the remove path)
// keeps its exact value via the memcpy fast path.
TEST_F(ArchetypeManagerTest, RemovePathTrivialMovePreservesValues)
{
    using namespace Astra;
    using namespace Astra::Test;
    Entity e(41, 1);
    manager->AddEntityWith<Position, Velocity>(e, Position{9, 8, 7}, Velocity{1, 2, 3});
    manager->RemoveComponent<Velocity>(e);            // {Position,Velocity} -> {Position} via MoveEntityFrom
    Position* p = manager->GetComponent<Position>(e); // Position = matched trivial column, memcpy'd across
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 9.0f);
    EXPECT_FLOAT_EQ(p->y, 8.0f);
    EXPECT_FLOAT_EQ(p->z, 7.0f);
    EXPECT_EQ(manager->GetComponent<Velocity>(e), nullptr);  // Velocity removed
}

// B1: ComponentDescriptor::is_trivially_destructible mirrors is_trivially_copyable /
// is_trivially_default_constructible -- the trait-driven factory in ComponentRegistry
// must populate it per-type so Destruct() can skip the indirect fn-ptr call for
// trivially-destructible components (Position) while still routing non-trivial ones
// (Tracked, which has a user-defined destructor doing s_live bookkeeping) through
// the real destructor.
TEST_F(ArchetypeManagerTest, DescriptorTrivialDestructibilityFlags)
{
    using namespace Astra::Test;
    // Position: plain aggregate -> trivially destructible.
    // Tracked: user-defined destructor (s_live bookkeeping) -> NOT trivially destructible.
    componentRegistry->RegisterComponent<Position>();
    componentRegistry->RegisterComponent<Tracked>();
    const auto* posDesc = componentRegistry->GetComponentDescriptor(Astra::TypeID<Position>::Value());
    const auto* trkDesc = componentRegistry->GetComponentDescriptor(Astra::TypeID<Tracked>::Value());
    ASSERT_NE(posDesc, nullptr);
    ASSERT_NE(trkDesc, nullptr);
    EXPECT_TRUE(posDesc->is_trivially_destructible);
    EXPECT_FALSE(trkDesc->is_trivially_destructible);
    static_assert(std::is_trivially_destructible_v<Position>);
    static_assert(!std::is_trivially_destructible_v<Tracked>);
}
