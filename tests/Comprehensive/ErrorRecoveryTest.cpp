#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <atomic>
#include "Astra/Registry/Registry.hpp"
#include "../TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class ErrorRecoveryTest : public ::testing::Test
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
};

// Component with tracking for construction/destruction
struct TrackedComponent
{
    static std::atomic<int> constructorCalls;
    static std::atomic<int> destructorCalls;
    static std::atomic<int> copyConstructorCalls;
    static std::atomic<int> moveConstructorCalls;
    
    int value;
    
    TrackedComponent() : value(0) { constructorCalls++; }
    explicit TrackedComponent(int v) : value(v) { constructorCalls++; }
    TrackedComponent(const TrackedComponent& other) : value(other.value) { copyConstructorCalls++; }
    TrackedComponent(TrackedComponent&& other) noexcept : value(other.value) { moveConstructorCalls++; }
    
    TrackedComponent& operator=(const TrackedComponent&) = default;
    TrackedComponent& operator=(TrackedComponent&&) = default;
    
    ~TrackedComponent() { destructorCalls++; }
    
    // Serialization support (needed because of user-defined special members)
    template<typename Archive>
    void Serialize(Archive& ar)
    {
        ar(value);
    }
    
    static void ResetCounters()
    {
        constructorCalls = 0;
        destructorCalls = 0;
        copyConstructorCalls = 0;
        moveConstructorCalls = 0;
    }
};

std::atomic<int> TrackedComponent::constructorCalls{0};
std::atomic<int> TrackedComponent::destructorCalls{0};
std::atomic<int> TrackedComponent::copyConstructorCalls{0};
std::atomic<int> TrackedComponent::moveConstructorCalls{0};

TEST_F(ErrorRecoveryTest, PartialBatchCreationFailure)
{
    const size_t initialCount = 1000;
    std::vector<Entity> initialEntities;
    for (size_t i = 0; i < initialCount; ++i)
    {
        initialEntities.push_back(registry->CreateEntity());
    }
    
    const size_t batchSize = 100000;
    std::vector<Entity> batchEntities(batchSize);
    
    size_t created = 0;
    // CreateEntities might fail silently if resources are exhausted
    registry->CreateEntitiesWith<Position, Velocity>(batchSize, batchEntities,
        [&created](size_t i) {
            created++;
            return std::make_tuple(
                Position{float(i), float(i * 2), float(i * 3)},
                Velocity{float(i * 10), 0, 0}
            );
        });
    
    for (const auto& entity : initialEntities)
    {
        EXPECT_TRUE(registry->IsValid(entity)) << "Initial entity should still be valid";
    }
    
    size_t validCount = 0;
    for (size_t i = 0; i < batchEntities.size(); ++i)
    {
        if (batchEntities[i].IsValid() && registry->IsValid(batchEntities[i]))
        {
            validCount++;
            EXPECT_NE(registry->GetComponent<Position>(batchEntities[i]), nullptr);
            EXPECT_NE(registry->GetComponent<Velocity>(batchEntities[i]), nullptr);
        }
    }
    
    EXPECT_GT(validCount, 0u) << "At least some entities should have been created";
    EXPECT_LE(validCount, batchSize) << "Can't create more than requested";
}

TEST_F(ErrorRecoveryTest, ComponentAdditionFailureRecovery)
{
    registry->GetComponentRegistry()->RegisterComponent<TrackedComponent>();
    TrackedComponent::ResetCounters();
    
    Entity entity = registry->CreateEntity();
    ASSERT_TRUE(registry->IsValid(entity));
    
    registry->EmplaceComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    registry->EmplaceComponent<TrackedComponent>(entity, 42);
    
    EXPECT_EQ(TrackedComponent::constructorCalls, 1);
    
    EXPECT_TRUE(registry->IsValid(entity));
    Position* pos = registry->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f) << "Original component should be unchanged";
    
    TrackedComponent* tracked = registry->GetComponent<TrackedComponent>(entity);
    ASSERT_NE(tracked, nullptr);
    EXPECT_EQ(tracked->value, 42);
    
    registry->DestroyEntity(entity);
    EXPECT_EQ(TrackedComponent::destructorCalls, 1) << "Destructor should be called once";
}

TEST_F(ErrorRecoveryTest, EntityDestructionDuringIteration)
{
    std::vector<Entity> entities;
    for (int i = 0; i < 100; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), 0, 0},
            Health{i, 100}
        ));
    }
    
    auto view = registry->CreateView<Position, Health>();
    
    // Collect entities to destroy during iteration (safe pattern)
    std::vector<Entity> toDestroy;
    size_t count = 0;
    view.ForEach([&count, &toDestroy](Entity e, Position& pos, Health&) {
        count++;
        
        if (int(pos.x) % 10 == 0)
        {
            toDestroy.push_back(e);
        }
    });
    
    EXPECT_EQ(count, 100u);
    
    for (Entity e : toDestroy)
    {
        registry->DestroyEntity(e);
    }
    
    for (int i = 0; i < 100; i += 10)
    {
        EXPECT_FALSE(registry->IsValid(entities[i])) << "Entity " << i << " should be destroyed";
    }
    
    for (int i = 1; i < 100; ++i)
    {
        if (i % 10 != 0)
        {
            EXPECT_TRUE(registry->IsValid(entities[i])) << "Entity " << i << " should still be valid";
        }
    }
}

TEST_F(ErrorRecoveryTest, InvalidEntityOperations)
{
    Entity invalid;
    EXPECT_FALSE(invalid.IsValid());
    
    // EmplaceComponent now returns void, so we test the behavior differently
    registry->EmplaceComponent<Position>(invalid, 1.0f, 2.0f, 3.0f);
    EXPECT_EQ(registry->GetComponent<Position>(invalid), nullptr); // Component wasn't added
    EXPECT_FALSE(registry->RemoveComponent<Position>(invalid));
    EXPECT_FALSE(registry->IsValid(invalid));
    
    Entity entity = registry->CreateEntity();
    Entity::StorageType id = entity.GetID();
    Entity::VersionType version = entity.GetVersion();
    registry->DestroyEntity(entity);
    
    // EmplaceComponent on destroyed entity should do nothing
    registry->EmplaceComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    EXPECT_EQ(registry->GetComponent<Position>(entity), nullptr); // Component wasn't added
    EXPECT_FALSE(registry->RemoveComponent<Position>(entity));
    EXPECT_EQ(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_FALSE(registry->IsValid(entity));
    
    // Might reuse ID with different version
    Entity newEntity = registry->CreateEntity();
    if (newEntity.GetID() == id)
    {
        EXPECT_NE(newEntity.GetVersion(), version) << "Recycled entity should have different version";
    }
    
    EXPECT_FALSE(registry->IsValid(entity));
}

TEST_F(ErrorRecoveryTest, ComponentRemovalDuringTransition)
{
    Entity entity = registry->CreateEntity();
    registry->EmplaceComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    registry->EmplaceComponent<Velocity>(entity, 4.0f, 5.0f, 6.0f);
    registry->EmplaceComponent<Health>(entity, 100, 100);
    
    EXPECT_FALSE(registry->RemoveComponent<Transform>(entity));
    
    EXPECT_TRUE(registry->IsValid(entity));
    EXPECT_NE(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_NE(registry->GetComponent<Velocity>(entity), nullptr);
    EXPECT_NE(registry->GetComponent<Health>(entity), nullptr);
    
    EXPECT_TRUE(registry->RemoveComponent<Velocity>(entity));
    EXPECT_TRUE(registry->RemoveComponent<Health>(entity));
    
    EXPECT_TRUE(registry->IsValid(entity));
    EXPECT_NE(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Velocity>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Health>(entity), nullptr);
    
    EXPECT_FALSE(registry->RemoveComponent<Velocity>(entity));
    
    EXPECT_TRUE(registry->IsValid(entity));
}

TEST_F(ErrorRecoveryTest, BatchDestructionWithInvalidEntities)
{
    std::vector<Entity> validEntities;
    for (int i = 0; i < 50; ++i)
    {
        validEntities.push_back(registry->CreateEntity());
    }
    
    std::vector<Entity> mixedEntities;
    mixedEntities.push_back(Entity::Invalid());  // Invalid
    mixedEntities.insert(mixedEntities.end(), validEntities.begin(), validEntities.begin() + 10);
    mixedEntities.push_back(Entity::Invalid());  // Invalid
    mixedEntities.insert(mixedEntities.end(), validEntities.begin() + 10, validEntities.begin() + 20);
    mixedEntities.push_back(Entity::Invalid());
    
    size_t initialSize = registry->Size();
    
    registry->DestroyEntities(mixedEntities);
    
    for (size_t i = 0; i < 20; ++i)
    {
        EXPECT_FALSE(registry->IsValid(validEntities[i])) << "Entity " << i << " should be destroyed";
    }
    
    for (size_t i = 20; i < 50; ++i)
    {
        EXPECT_TRUE(registry->IsValid(validEntities[i])) << "Entity " << i << " should still be valid";
    }
    
    EXPECT_EQ(registry->Size(), initialSize - 20);
}

TEST_F(ErrorRecoveryTest, RapidArchetypeTransitions)
{
    registry->GetComponentRegistry()->RegisterComponent<TrackedComponent>();
    
    Entity entity = registry->CreateEntity();
    
    for (int i = 0; i < 100; ++i)
    {
        registry->EmplaceComponent<Position>(entity);
        registry->EmplaceComponent<Velocity>(entity);
        registry->EmplaceComponent<Health>(entity);
        registry->EmplaceComponent<TrackedComponent>(entity, i);
        
        registry->RemoveComponent<Velocity>(entity);
        registry->RemoveComponent<TrackedComponent>(entity);
        
        registry->EmplaceComponent<Transform>(entity);
        registry->EmplaceComponent<Physics>(entity);
        
        registry->RemoveComponent<Position>(entity);
        registry->RemoveComponent<Health>(entity);
        registry->RemoveComponent<Transform>(entity);
        registry->RemoveComponent<Physics>(entity);
    }
    
    EXPECT_TRUE(registry->IsValid(entity));
    
    EXPECT_EQ(registry->GetComponent<Position>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Velocity>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Health>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Transform>(entity), nullptr);
}

TEST_F(ErrorRecoveryTest, ComponentDataIntegrityAfterTransitions)
{
    Entity entity = registry->CreateEntity();
    registry->EmplaceComponent<Position>(entity, 1.0f, 2.0f, 3.0f);
    registry->EmplaceComponent<Velocity>(entity, 4.0f, 5.0f, 6.0f);
    registry->EmplaceComponent<Health>(entity, 100, 150);
    
    Position* pos = registry->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_FLOAT_EQ(pos->x, 1.0f);
    EXPECT_FLOAT_EQ(pos->y, 2.0f);
    EXPECT_FLOAT_EQ(pos->z, 3.0f);
    
    registry->EmplaceComponent<Transform>(entity);
    registry->RemoveComponent<Velocity>(entity);
    registry->EmplaceComponent<Player>(entity);
    registry->RemoveComponent<Health>(entity);
    registry->EmplaceComponent<Enemy>(entity);
    
    pos = registry->GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_FLOAT_EQ(pos->x, 1.0f) << "Position.x should be preserved after transitions";
    EXPECT_FLOAT_EQ(pos->y, 2.0f) << "Position.y should be preserved after transitions";
    EXPECT_FLOAT_EQ(pos->z, 3.0f) << "Position.z should be preserved after transitions";
    
    Transform* transform = registry->GetComponent<Transform>(entity);
    ASSERT_NE(transform, nullptr);
    
    EXPECT_EQ(registry->GetComponent<Velocity>(entity), nullptr);
    EXPECT_EQ(registry->GetComponent<Health>(entity), nullptr);
}

TEST_F(ErrorRecoveryTest, EntityManagerFragmentationRecovery)
{
    std::vector<Entity> wave1, wave2, wave3;
    
    for (int i = 0; i < 1000; ++i)
    {
        wave1.push_back(registry->CreateEntity());
        wave2.push_back(registry->CreateEntity());
        wave3.push_back(registry->CreateEntity());
    }
    
    for (size_t i = 0; i < wave1.size(); i += 2)
    {
        registry->DestroyEntity(wave1[i]);
    }
    for (size_t i = 1; i < wave2.size(); i += 2)
    {
        registry->DestroyEntity(wave2[i]);
    }
    for (size_t i = 0; i < wave3.size(); i += 3)
    {
        registry->DestroyEntity(wave3[i]);
    }
    
    // Should reuse fragmented slots
    std::vector<Entity> recycled;
    for (int i = 0; i < 1500; ++i)
    {
        Entity e = registry->CreateEntity();
        EXPECT_TRUE(e.IsValid());
        recycled.push_back(e);
    }
    
    for (size_t i = 1; i < wave1.size(); i += 2)
    {
        EXPECT_TRUE(registry->IsValid(wave1[i]));
    }
    for (size_t i = 0; i < wave2.size(); i += 2)
    {
        EXPECT_TRUE(registry->IsValid(wave2[i]));
    }
    
    registry->Clear();
    EXPECT_EQ(registry->Size(), 0u);
    
    Entity afterClear = registry->CreateEntity();
    EXPECT_TRUE(registry->IsValid(afterClear));
}