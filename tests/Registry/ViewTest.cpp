#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <unordered_set>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Registry/Registry.hpp"
#include "Astra/Registry/View.hpp"

class ViewTest : public ::testing::Test
{
protected:
    std::unique_ptr<Astra::Registry> registry;
    
    void SetUp() override 
    {
        registry = std::make_unique<Astra::Registry>();
        
        // Register test components
        using namespace Astra::Test;
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name, Physics, Player, Enemy, Static, RenderData>();
    }
    
    void TearDown() override 
    {
        registry.reset();
    }
    
    // Helper to create test entities
    void CreateTestEntities()
    {
        using namespace Astra::Test;
        
        // Create diverse set of entities with different component combinations
        for (int i = 0; i < 100; ++i)
        {
            Astra::Entity e = registry->CreateEntity();
            
            // All entities get Position
            registry->EmplaceComponent<Position>(e, float(i), float(i * 2), float(i * 3));
            
            // Every 2nd entity gets Velocity
            if (i % 2 == 0)
            {
                registry->EmplaceComponent<Velocity>(e, float(i * 10), 0.0f, 0.0f);
            }
            
            // Every 3rd entity gets Health
            if (i % 3 == 0)
            {
                registry->EmplaceComponent<Health>(e, i, 100);
            }
            
            // Every 5th entity is Static
            if (i % 5 == 0)
            {
                registry->EmplaceComponent<Static>(e);
            }
            
            // Every 7th entity is Renderable
            if (i % 7 == 0)
            {
                registry->EmplaceComponent<RenderData>(e);
            }
            
            // First 10 entities are Players
            if (i < 10)
            {
                registry->EmplaceComponent<Player>(e);
            }
            
            // Entities 90-99 are Enemies
            if (i >= 90)
            {
                registry->EmplaceComponent<Enemy>(e);
            }
        }
    }
};

// Test basic view creation and iteration
TEST_F(ViewTest, BasicViewIteration)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // Create view for Position only
    auto view = registry->CreateView<Position>();
    
    // Count entities with ForEach
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos) {
        count++;
        EXPECT_GE(pos.x, 0.0f);
        EXPECT_LT(pos.x, 100.0f);
    });
    
    EXPECT_EQ(count, 100u); // All entities have Position
}

// Test view with multiple required components
TEST_F(ViewTest, MultipleRequiredComponents)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // View for Position AND Velocity
    auto view = registry->CreateView<Position, Velocity>();
    
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos, Velocity&) {
        count++;
        // Only even indices should have both
        EXPECT_EQ(int(pos.x) % 2, 0);
    });
    
    EXPECT_EQ(count, 50u); // Every 2nd entity
}

// Test view with Not modifier
TEST_F(ViewTest, NotModifier)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // View for Position but NOT Static
    auto view = registry->CreateView<Position, Astra::Not<Static>>();
    
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos) {
        count++;
        // Should not be divisible by 5 (Static entities)
        EXPECT_NE(int(pos.x) % 5, 0);
    });
    
    EXPECT_EQ(count, 80u); // 100 - 20 static entities
}

// Test view with Optional modifier
TEST_F(ViewTest, OptionalModifier)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // View for Position with optional Health
    auto view = registry->CreateView<Position, Astra::Optional<Health>>();
    
    size_t totalCount = 0;
    size_t withHealthCount = 0;
    
    view.ForEach([&](Astra::Entity, Position& pos, Health* health) {
        totalCount++;
        if (health != nullptr)
        {
            withHealthCount++;
            // Health is on every 3rd entity
            EXPECT_EQ(int(pos.x) % 3, 0);
            EXPECT_EQ(health->current, int(pos.x));
        }
    });
    
    EXPECT_EQ(totalCount, 100u); // All entities have Position
    EXPECT_EQ(withHealthCount, 34u); // Every 3rd entity (0, 3, 6, ..., 99)
}

// Test view with Any modifier
TEST_F(ViewTest, AnyModifier)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // View for Position with any of Player or Enemy
    auto view = registry->CreateView<Position, Astra::Any<Player, Enemy>>();
    
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position& pos) {
        count++;
        // Should be either < 10 (Player) or >= 90 (Enemy)
        EXPECT_TRUE(pos.x < 10.0f || pos.x >= 90.0f);
    });
    
    EXPECT_EQ(count, 20u); // 10 Players + 10 Enemies
}

// Test view with multiple modifiers combined
TEST_F(ViewTest, CombinedModifiers)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // Complex view: Position, optional Velocity, not Static, any of Player or Enemy
    auto view = registry->CreateView<Position, 
                                     Astra::Optional<Velocity>, 
                                     Astra::Not<Static>,
                                     Astra::Any<Player, Enemy>>();
    
    size_t count = 0;
    size_t withVelocityCount = 0;
    
    view.ForEach([&](Astra::Entity, Position& pos, Velocity* vel) {
        count++;
        
        // Must be Player or Enemy
        EXPECT_TRUE(pos.x < 10.0f || pos.x >= 90.0f);
        
        // Must not be Static (not divisible by 5)
        EXPECT_NE(int(pos.x) % 5, 0);
        
        if (vel != nullptr)
        {
            withVelocityCount++;
        }
    });
    
    // Players: 0-9, exclude 0 and 5 (static) = 8
    // Enemies: 90-99, exclude 90 and 95 (static) = 8
    EXPECT_EQ(count, 16u);
}

// Test empty view
TEST_F(ViewTest, EmptyView)
{
    using namespace Astra::Test;
    
    // Don't create any entities
    auto view = registry->CreateView<Position>();
    
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position&)
    {
        count++;
    });
    
    EXPECT_EQ(count, 0u);
}

// Test view invalidation after entity changes
TEST_F(ViewTest, ViewInvalidation)
{
    using namespace Astra::Test;
    
    // Create initial entities
    Astra::Entity e1 = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    Astra::Entity e2 = registry->CreateEntityWith(Position{4.0f, 5.0f, 6.0f}, Velocity{1.0f, 0.0f, 0.0f});
    
    auto view = registry->CreateView<Position, Velocity>();
    
    // Initially should have 1 entity (e2)
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position&, Velocity&) {
        count++;
    });
    EXPECT_EQ(count, 1u);
    
    // Add Velocity to e1
    registry->EmplaceComponent<Velocity>(e1, 2.0f, 0.0f, 0.0f);
    
    // View should now see 2 entities
    count = 0;
    view.ForEach([&count](Astra::Entity, Position&, Velocity&) {
        count++;
    });
    EXPECT_EQ(count, 2u);
    
    // Remove Velocity from e2
    registry->RemoveComponent<Velocity>(e2);
    
    // Back to 1 entity
    count = 0;
    view.ForEach([&count](Astra::Entity, Position&, Velocity&) {
        count++;
    });
    EXPECT_EQ(count, 1u);
}

// Test performance characteristics
TEST_F(ViewTest, PerformanceCharacteristics)
{
    using namespace Astra::Test;
    
    // Create many entities for performance testing
    const size_t entityCount = 10000;
    for (size_t i = 0; i < entityCount; ++i)
    {
        Astra::Entity e = registry->CreateEntity();
        registry->EmplaceComponent<Position>(e, float(i), 0.0f, 0.0f);
        
        if (i % 2 == 0)
        {
            registry->EmplaceComponent<Velocity>(e, 1.0f, 0.0f, 0.0f);
        }
    }
    
    auto view = registry->CreateView<Position, Velocity>();
    
    // Test ForEach performance
    size_t forEachCount = 0;
    view.ForEach([&forEachCount](Astra::Entity, Position& pos, Velocity& vel) {
        forEachCount++;
        pos.x += vel.dx; // Simple operation
    });
    
    EXPECT_EQ(forEachCount, entityCount / 2);
}

// Test view with no matching entities
TEST_F(ViewTest, NoMatchingEntities)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // Create a view that won't match any entities
    // Position AND Player AND Enemy (no entity is both Player and Enemy)
    auto view = registry->CreateView<Position, Player, Enemy>();
    
    size_t count = 0;
    view.ForEach([&count](Astra::Entity, Position&, Player&, Enemy&) {
        count++;
    });
    
    EXPECT_EQ(count, 0u);
}

// Test view size estimation
TEST_F(ViewTest, ViewSizeEstimation)
{
    using namespace Astra::Test;
    
    CreateTestEntities();
    
    // Different views should iterate over different numbers of entities
    auto allView = registry->CreateView<Position>();
    auto velocityView = registry->CreateView<Position, Velocity>();
    auto healthView = registry->CreateView<Position, Health>();
    auto complexView = registry->CreateView<Position, Velocity, Health>();
    
    auto countEntities = [](auto& view) {
        size_t count = 0;
        view.ForEach([&count](auto...) { count++; });
        return count;
    };
    
    EXPECT_EQ(countEntities(allView), 100u);
    EXPECT_EQ(countEntities(velocityView), 50u);
    EXPECT_EQ(countEntities(healthView), 34u);
    EXPECT_EQ(countEntities(complexView), 17u); // Entities with all three
}

// Test modification during iteration
TEST_F(ViewTest, ModificationDuringIteration)
{
    using namespace Astra::Test;
    
    // Create test entities
    std::vector<Astra::Entity> entities;
    for (int i = 0; i < 10; ++i)
    {
        entities.push_back(registry->CreateEntityWith(Position{float(i), 0.0f, 0.0f}));
    }
    
    auto view = registry->CreateView<Position>();
    
    // Modify components during iteration
    view.ForEach([](Astra::Entity, Position& pos) {
        pos.x *= 2.0f;
    });
    
    // Verify modifications
    for (int i = 0; i < 10; ++i)
    {
        Position* pos = registry->GetComponent<Position>(entities[i]);
        EXPECT_EQ(pos->x, float(i * 2));
    }
}