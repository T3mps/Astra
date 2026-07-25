#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include <numeric>
#include <set>
#include <unordered_set>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Commands/CommandBuffer.hpp"
#include "Astra/Registry/Registry.hpp"
#include "Astra/Registry/View.hpp"

// Enableable-components suite (spec 2026-07-25 §2/§5). EnA / EnB are the suite's
// two ASTRA_ENABLEABLE components -- reuse them, do NOT introduce new types
// (TypeID ceiling). CreateEntity<EnA>() auto-registers them.
using EnA = Astra::Test::Hierarchy;
using EnB = Astra::Test::Timer;

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

// View::ForEach carries a Debug-only reentrancy guard: it compares the
// ArchetypeManager's structural-change counter before and after the loop and
// ASTRA_ASSERTs it is unchanged. A normal ForEach that only reads entities and
// mutates existing component VALUES (never structure) must complete cleanly
// and produce correct results without ever tripping that guard. Deferring
// structural changes into a CommandBuffer and Executing it AFTER the loop --
// the documented remedy -- must also not trip it, since recording into a
// CommandBuffer does not touch the ArchetypeManager; only Execute() does.
TEST_F(ViewTest, ForEachNoStructuralMutationDoesNotTripReentrancyGuard)
{
    using namespace Astra::Test;

    constexpr int kEntityCount = 20;
    std::vector<Astra::Entity> entities;
    entities.reserve(kEntityCount);
    for (int i = 0; i < kEntityCount; ++i)
    {
        entities.push_back(registry->CreateEntityWith(
            Position{float(i), 0.0f, 0.0f}, Velocity{float(i * 2), 0.0f, 0.0f}));
    }

    auto view = registry->CreateView<Position, Velocity>();

    // Normal (non-structural) ForEach: accumulate values and mutate component
    // VALUES in place. Must complete without tripping the reentrancy guard.
    float positionSum = 0.0f;
    float velocitySum = 0.0f;
    size_t visited = 0;
    view.ForEach([&](Astra::Entity, Position& pos, Velocity& vel)
    {
        positionSum += pos.x;
        velocitySum += vel.dx;
        pos.x += 1.0f;  // in-place value mutation, not a structural change
        ++visited;
    });

    ASSERT_EQ(visited, entities.size());

    float expectedPositionSum = 0.0f;
    float expectedVelocitySum = 0.0f;
    for (int i = 0; i < kEntityCount; ++i)
    {
        expectedPositionSum += float(i);
        expectedVelocitySum += float(i * 2);
    }
    EXPECT_FLOAT_EQ(positionSum, expectedPositionSum);
    EXPECT_FLOAT_EQ(velocitySum, expectedVelocitySum);

    // Confirm the in-place value mutation actually took effect (pos.x += 1).
    for (int i = 0; i < kEntityCount; ++i)
    {
        Position* pos = registry->GetComponent<Position>(entities[i]);
        ASSERT_NE(pos, nullptr);
        EXPECT_FLOAT_EQ(pos->x, float(i) + 1.0f);
    }

    // Deferred structural mutation: record RemoveComponent<Velocity> for every
    // entity while iterating, then Execute() AFTER the loop returns. This must
    // not trip the guard either, since the buffer only mutates the registry
    // during Execute() -- never during recording.
    Astra::CommandBuffer cmdBuffer(registry.get());
    size_t recorded = 0;
    view.ForEach([&](Astra::Entity e, Position&, Velocity&)
    {
        cmdBuffer.RemoveComponent<Velocity>(e);
        ++recorded;
    });
    EXPECT_EQ(recorded, entities.size());

    auto result = cmdBuffer.Execute();
    ASSERT_TRUE(result.IsOk());

    // Velocity was removed from every entity, so the view should now be empty.
    size_t afterCount = 0;
    view.ForEach([&](Astra::Entity, Position&, Velocity&)
    {
        ++afterCount;
    });
    EXPECT_EQ(afterCount, 0u);
}

// Theme J Fix 4: constructing a View over a null manager must not crash.
TEST_F(ViewTest, NullManagerViewIsEmptyNotCrash)
{
    using namespace Astra::Test;

    Astra::View<Position> nullView(nullptr);

    EXPECT_FALSE(nullView.IsValid());

    int n = 0;
    nullView.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 0);
}

// ===================== Enableable-components query filtering (spec §5) =====================

// A default query skips disabled components; Size() reflects only the enabled.
TEST_F(ViewTest, DefaultQueryExcludesDisabled)
{
    auto e1 = registry->CreateEntity<EnA>(); auto e2 = registry->CreateEntity<EnA>();
    registry->SetEnabled<EnA>(e2, false);
    auto view = registry->CreateView<EnA>();
    std::vector<Astra::Entity> seen;
    view.ForEach([&](Astra::Entity e, EnA&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], e1);
    EXPECT_EQ(view.Size(), 1u);
}

// IncludeDisabled<T> opts the whole view out of enabled filtering.
TEST_F(ViewTest, IncludeDisabledOptOutSeesEverything)
{
    auto e1 = registry->CreateEntity<EnA>(); auto e2 = registry->CreateEntity<EnA>();
    (void)e1; (void)e2;
    registry->SetEnabled<EnA>(e2, false);
    auto view = registry->CreateView<Astra::IncludeDisabled<EnA>>();
    size_t n = 0;
    view.ForEach([&](Astra::Entity, EnA&) { ++n; });
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(view.Size(), 2u);
}

// Two enableable required columns intersect: only enabled-in-BOTH is visited.
TEST_F(ViewTest, MultiEnableableIntersection)
{
    auto both    = registry->CreateEntity<EnA, EnB>();
    auto aOff    = registry->CreateEntity<EnA, EnB>(); registry->SetEnabled<EnA>(aOff, false);
    auto bOff    = registry->CreateEntity<EnA, EnB>(); registry->SetEnabled<EnB>(bOff, false);
    auto neither = registry->CreateEntity<EnA, EnB>();
    registry->SetEnabled<EnA>(neither, false); registry->SetEnabled<EnB>(neither, false);
    size_t n = 0; Astra::Entity onlyHit{};
    registry->CreateView<EnA, EnB>().ForEach([&](Astra::Entity e, EnA&, EnB&) { ++n; onlyHit = e; });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(onlyHit, both);
}

// Run-scan must handle bits on / around word boundaries (63/64/65/127/128) and
// a fully-disabled word.
TEST_F(ViewTest, WordBoundaryRunScan)
{
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 200; ++i) es.push_back(registry->CreateEntity<EnA>());
    for (int i : {0, 63, 64, 65, 127, 128}) registry->SetEnabled<EnA>(es[i], false);
    std::set<int> disabledIdx = {0, 63, 64, 65, 127, 128};
    size_t n = 0;
    registry->CreateView<EnA>().ForEach([&](Astra::Entity e, EnA&) {
        ++n;
        for (int i : disabledIdx) EXPECT_NE(e, es[i]);
    });
    EXPECT_EQ(n, 200u - disabledIdx.size());
    // Fully disable [64,128) and re-count.
    for (int i = 64; i < 128; ++i) registry->SetEnabled<EnA>(es[i], false);
    size_t m = 0;
    registry->CreateView<EnA>().ForEach([&](Astra::Entity, EnA&) { ++m; });
    std::set<int> all(disabledIdx); for (int i = 64; i < 128; ++i) all.insert(i);
    EXPECT_EQ(m, 200u - all.size());
}

// An enableable Optional<T> pointer is null while disabled, non-null while enabled.
TEST_F(ViewTest, OptionalEnableableReportsNullWhileDisabled)
{
    using namespace Astra::Test;
    auto e = registry->CreateEntity<Position, EnA>();
    registry->SetEnabled<EnA>(e, false);
    registry->CreateView<Position, Astra::Optional<EnA>>().ForEach(
        [&](Astra::Entity, Position&, EnA* a) { EXPECT_EQ(a, nullptr); });
    registry->SetEnabled<EnA>(e, true);
    registry->CreateView<Position, Astra::Optional<EnA>>().ForEach(
        [&](Astra::Entity, Position&, EnA* a) { EXPECT_NE(a, nullptr); });
}

// Visit order is a function of storage layout only, not toggle history.
TEST_F(ViewTest, IterationOrderIndependentOfToggleHistory)
{
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i) es.push_back(registry->CreateEntity<EnA>());
    auto visit = [&] {
        std::vector<Astra::Entity> order;
        registry->CreateView<EnA>().ForEach([&](Astra::Entity e, EnA&) { order.push_back(e); });
        return order;
    };
    for (int i = 0; i < 300; i += 2) registry->SetEnabled<EnA>(es[i], false);
    for (int i = 0; i < 300; i += 2) registry->SetEnabled<EnA>(es[i], true);
    auto a = visit();
    for (int r = 0; r < 2; ++r)
        for (int i = 1; i < 300; i += 2) { registry->SetEnabled<EnA>(es[i], false); registry->SetEnabled<EnA>(es[i], true); }
    auto b = visit();
    EXPECT_EQ(a, b);
}

// ParallelForEach applies the same per-chunk filter (runs sequentially here: the
// default Registry injects no scheduler, so this exercises the inline fallback,
// which still validates filtering).
TEST_F(ViewTest, ParallelForEachRespectsDisabled)
{
    for (int i = 0; i < 5000; ++i)
    {
        auto e = registry->CreateEntity<EnA>();
        if (i % 3 == 0) registry->SetEnabled<EnA>(e, false);
    }
    std::atomic<size_t> n{0};
    registry->CreateView<EnA>().ParallelForEach([&](Astra::Entity, EnA&) { n.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(n.load(), 5000u - (5000u + 2) / 3);
}