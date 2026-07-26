#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <system_error>
#include "../TestComponents.hpp"
#include "Astra/Registry/Registry.hpp"

// Split from RegistryTest.cpp (2026-07-10 test-suite audit): the registry's
// core-operations tests and its Save/Load serialization tests are two
// separable themes. Fixture duplicated verbatim (< 20 lines) per the split
// policy; TEST bodies below are byte-identical to the ones removed from
// RegistryTest.cpp.
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

// ============================== Serialization Tests ==============================

TEST_F(RegistryTest, EmptyRegistrySerialization)
{
    using namespace Astra::Test;

    // Save empty registry to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify loaded registry is empty
    EXPECT_EQ(loadedRegistry->Size(), 0u);
    EXPECT_TRUE(loadedRegistry->IsEmpty());
}

TEST_F(RegistryTest, SingleEntitySerialization)
{
    using namespace Astra::Test;

    // Create entity with components
    Astra::Entity entity = registry->CreateEntityWith(
        Position{1.0f, 2.0f, 3.0f},
        Velocity{4.0f, 5.0f, 6.0f}
    );
    (void)entity;

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify loaded registry has one entity
    EXPECT_EQ(loadedRegistry->Size(), 1u);

    // Get entities from loaded registry
    auto view = loadedRegistry->CreateView<Position, Velocity>();
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos, Velocity& vel)
    {
        count++;
        EXPECT_FLOAT_EQ(pos.x, 1.0f);
        EXPECT_FLOAT_EQ(pos.y, 2.0f);
        EXPECT_FLOAT_EQ(pos.z, 3.0f);
        EXPECT_FLOAT_EQ(vel.dx, 4.0f);
        EXPECT_FLOAT_EQ(vel.dy, 5.0f);
        EXPECT_FLOAT_EQ(vel.dz, 6.0f);
    });
    EXPECT_EQ(count, 1u);
}

TEST_F(RegistryTest, MultipleEntitiesSerialization)
{
    using namespace Astra::Test;

    // Create multiple entities with different component combinations
    std::vector<Astra::Entity> entities;

    // 10 entities with Position only
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), float(i * 2), float(i * 3)}
        ));
    }

    // 10 entities with Position and Velocity
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i + 10), float(i * 2 + 10), float(i * 3 + 10)},
            Velocity{float(i), 0.0f, 0.0f}
        ));
    }

    // 10 entities with Position, Velocity, and Health
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i + 20), float(i * 2 + 20), float(i * 3 + 20)},
            Velocity{float(i + 10), 0.0f, 0.0f},
            Health{100 - i, 100}
        ));
    }

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify total entity count
    EXPECT_EQ(loadedRegistry->Size(), 30u);

    // Verify entities with Position only
    auto posOnlyView = loadedRegistry->CreateView<Position, Astra::Not<Velocity>>();
    size_t posOnlyCount = 0;
    posOnlyView.ForEach([&posOnlyCount](Astra::Entity, Position& pos)
    {
        posOnlyCount++;
        int i = int(pos.x);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(pos.y, float(i * 2));
        EXPECT_FLOAT_EQ(pos.z, float(i * 3));
    });
    EXPECT_EQ(posOnlyCount, 10u);

    // Verify entities with Position and Velocity but not Health
    auto posVelView = loadedRegistry->CreateView<Position, Velocity, Astra::Not<Health>>();
    size_t posVelCount = 0;
    posVelView.ForEach([&posVelCount](Astra::Entity, Position& pos, Velocity& vel)
    {
        posVelCount++;
        int i = int(pos.x) - 10;
        EXPECT_GE(i, 0);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(vel.dx, float(i));
    });
    EXPECT_EQ(posVelCount, 10u);

    // Verify entities with all three components
    auto allView = loadedRegistry->CreateView<Position, Velocity, Health>();
    size_t allCount = 0;
    allView.ForEach([&allCount](Astra::Entity, Position& pos, Velocity& vel, Health& health)
    {
        allCount++;
        int i = int(pos.x) - 20;
        EXPECT_GE(i, 0);
        EXPECT_LT(i, 10);
        EXPECT_FLOAT_EQ(vel.dx, float(i + 10));
        EXPECT_EQ(health.current, 100 - i);
        EXPECT_EQ(health.max, 100);
    });
    EXPECT_EQ(allCount, 10u);
}

TEST_F(RegistryTest, RelationshipSerialization)
{
    using namespace Astra::Test;

    // Create parent and children
    Astra::Entity parent = registry->CreateEntityWith(Position{0, 0, 0});
    Astra::Entity child1 = registry->CreateEntityWith(Position{1, 0, 0});
    Astra::Entity child2 = registry->CreateEntityWith(Position{2, 0, 0});
    Astra::Entity linked1 = registry->CreateEntityWith(Position{3, 0, 0});
    Astra::Entity linked2 = registry->CreateEntityWith(Position{4, 0, 0});

    // Set up relationships
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->AddLink(linked1, linked2);

    // Save to memory
    auto saveResult = registry->Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    // Verify entity count
    EXPECT_EQ(loadedRegistry->Size(), 5u);

    // We can't directly verify relationships without entity IDs being preserved
    // But we can verify that the relationship graph was deserialized
    // and contains the right number of relationships
    const auto& graph = loadedRegistry->GetRelationshipGraph();
    (void)graph;

    // Check that we have parent-child relationships
    // Note: We'd need to iterate through entities to find the actual relationships
    // since entity IDs are not preserved across serialization
}

TEST_F(RegistryTest, SerializationErrorHandling)
{
    using namespace Astra::Test;

    // Test loading with missing components
    {
        // Create entity with Position and Velocity
        registry->CreateEntityWith(
            Position{1.0f, 2.0f, 3.0f},
            Velocity{4.0f, 5.0f, 6.0f}
        );

        // Save to memory
        auto saveResult = registry->Save();
        ASSERT_TRUE(saveResult.IsOk());
        auto buffer = std::move(*saveResult.GetValue());

        // Create component registry WITHOUT Velocity registered
        auto incompleteRegistry = std::make_shared<Astra::ComponentRegistry>();
        incompleteRegistry->RegisterComponent<Position>();  // Missing Velocity!

        // Load should fail
        auto loadResult = Astra::Registry::Load(buffer, incompleteRegistry);
        EXPECT_TRUE(loadResult.IsErr());
    }

    // Test loading with corrupted data
    {
        std::vector<std::byte> corruptedData(100, std::byte{0xFF});

        auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
        componentRegistry->RegisterComponents<Position, Velocity>();

        auto loadResult = Astra::Registry::Load(std::span(corruptedData), componentRegistry);
        EXPECT_TRUE(loadResult.IsErr());
    }
}

// W1 unified paged entity record (Task 5): version and location now share one
// slot in EntityManager's record table, so on load EntityManager MUST restore
// versions before ArchetypeManager writes locations into those same slots
// (see the INVARIANT comment in Registry::LoadInternal). This round-trip test
// locks that ordering: it destroys a subset of entities before saving (so the
// restored table has both recycled versions and holes), then asserts BOTH
// liveness (IsValid) and location (a component value read through the
// restored archetype) are correct after load. Uses the existing
// Astra::Test::Position/Velocity types (near the TypeID ceiling -- no new
// component types).
TEST(RegistrySerialization, UnifiedRecordRoundTripPreservesLivenessAndLocation)
{
    using namespace Astra::Test;

    Astra::Registry registry;
    registry.GetComponentRegistry()->RegisterComponents<Position, Velocity>();

    std::vector<Astra::Entity> ents;
    for (int i = 0; i < 100; ++i)
    {
        ents.push_back(registry.CreateEntityWith(
            Position{float(i), 0.0f, 0.0f},
            Velocity{1.0f, 1.0f, 1.0f}
        ));
    }

    // Destroy every 10th entity so recycled versions and holes in the shared
    // record table are exercised across the round trip.
    for (int i = 0; i < 100; i += 10)
    {
        registry.DestroyEntity(ents[i]);
    }

    // Save to memory
    auto saveResult = registry.Save();
    ASSERT_TRUE(saveResult.IsOk());
    auto buffer = std::move(*saveResult.GetValue());

    // Create component registry for loading
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity>();

    // Load from memory
    auto loadResult = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loadResult.IsOk()) << "Failed to load registry, error: " << static_cast<int>(*loadResult.GetError());
    auto loadedRegistry = std::move(*loadResult.GetValue());

    for (int i = 0; i < 100; ++i)
    {
        const bool destroyed = (i % 10 == 0);
        EXPECT_EQ(loadedRegistry->IsValid(ents[i]), !destroyed) << "i=" << i;
        if (!destroyed)
        {
            const Position* p = loadedRegistry->GetComponent<Position>(ents[i]);
            ASSERT_NE(p, nullptr) << "i=" << i;
            EXPECT_FLOAT_EQ(p->x, float(i));   // location resolved correctly after load
        }
    }
}

// ===================== LZ4 per-column compression (Task 3) =====================

namespace
{
    // A single entity's Name column must exceed 4 MB so WriteCompressedBlock compresses
    // it into a MULTI-BLOCK LZ4 frame -- the >4MB case (C3) that previously saved but
    // never loaded. Two constraints drive the component choice:
    //   1. The storage engine hard-refuses any component whose one-entity in-memory
    //      footprint exceeds the pool's chunk ceiling (<=1 MB), so a >4MB POD column is
    //      impossible to store. Name's footprint is a std::string handle (~32 B); its
    //      SERIALIZED column -- the bytes SerializeColumn emits, exactly what compression
    //      wraps -- is the full string contents, which we make ~5 MB.
    //   2. The test binary sits at the 128-component-type ceiling, so the component must
    //      be one ALREADY registered elsewhere in the suite (using a type no other test
    //      touches would consume a fresh global TypeID and overflow a later test's
    //      ComponentMask). Astra::Test::Name is used across many suites -> zero new IDs.
    constexpr size_t kBigChars = 5'000'000;   // ~5 MB serialized column ( > 4 MB )

    // Deterministic, LZ4-friendly fill: repeats every 26 chars, so compression
    // demonstrably engages while the whole payload is still checked on load.
    std::string MakeBigString()
    {
        std::string s;
        s.resize(kBigChars);
        for (size_t i = 0; i < kBigChars; ++i)
            s[i] = static_cast<char>('A' + (i % 26));
        return s;   // heap buffer; NRVO -- no giant stack object
    }
}

// Save a one-entity world whose Name serializes to a >4MB column twice -- once LZ4,
// once None -- then assert (a) the LZ4 file is materially smaller (per-column
// compression engaged) and (b) both files load back byte-equal (the >4MB multi-block
// round trip that previously saved but never loaded -- C3).
TEST(RegistrySerialization, LZ4_CompressesAndRoundTripsLargeColumn)
{
    using namespace Astra::Test;
    namespace fs = std::filesystem;
    const fs::path pathLz4  = fs::temp_directory_path() / "astra_lz4_big.bin";
    const fs::path pathNone = fs::temp_directory_path() / "astra_none_big.bin";

    const std::string expected = MakeBigString();

    // Build the world once; the big string lives on the heap inside the component
    // (never a 5 MB stack object) and is filled through a mutable pointer.
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Name>();
    Astra::Entity e = reg.CreateEntity();
    ASSERT_TRUE(reg.EmplaceComponent<Name>(e));
    {
        Name* n = reg.GetComponent<Name>(e);
        ASSERT_NE(n, nullptr);
        n->value = expected;
    }

    // Save the same world twice with different compression modes.
    {
        Astra::Registry::SaveConfig cfg; cfg.compressionMode = Astra::CompressionMode::LZ4;
        ASSERT_TRUE(reg.Save(pathLz4, cfg).IsOk());
    }
    {
        Astra::Registry::SaveConfig cfg; cfg.compressionMode = Astra::CompressionMode::None;
        ASSERT_TRUE(reg.Save(pathNone, cfg).IsOk());
    }

    // The real RED: before Task 3 nothing compressed, so these files were equal.
    EXPECT_LT(fs::file_size(pathLz4), fs::file_size(pathNone))
        << "LZ4 save (" << fs::file_size(pathLz4) << " B) should be smaller than None ("
        << fs::file_size(pathNone) << " B) -- per-column compression did not engage";

    // Both files must load back with the column byte-equal to what we saved.
    auto verify = [&expected](const fs::path& p)
    {
        auto compReg = std::make_shared<Astra::ComponentRegistry>();
        compReg->RegisterComponents<Name>();
        auto loadResult = Astra::Registry::Load(p, compReg);
        ASSERT_TRUE(loadResult.IsOk())
            << "Load failed for " << p.string() << ", error "
            << static_cast<int>(*loadResult.GetError());
        auto loaded = std::move(*loadResult.GetValue());

        EXPECT_EQ(loaded->Size(), 1u);
        size_t seen = 0;
        bool valueEqual = false;
        loaded->CreateView<Name>().ForEach(
            [&](Astra::Entity, Name& n)
            {
                ++seen;
                valueEqual = (n.value == expected);   // full >4MB byte-equality
            });
        EXPECT_EQ(seen, 1u);
        EXPECT_TRUE(valueEqual) << "column diverged across round trip in " << p.string();
    };

    verify(pathLz4);
    verify(pathNone);

    std::error_code ec;
    fs::remove(pathLz4, ec);
    fs::remove(pathNone, ec);
}
