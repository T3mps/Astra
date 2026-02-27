#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_set>
#include <chrono>
#include "Astra/Registry/Registry.hpp"
#include "Astra/Registry/Relations.hpp"
#include "../TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class RelationsTest : public ::testing::Test
{
protected:
    std::unique_ptr<Registry> registry;
    
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
        
        // Register test components
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Player, Enemy, Physics>();
    }
    
    void TearDown() override
    {
        registry.reset();
    }
};

// Test unfiltered Relations
TEST_F(RelationsTest, UnfilteredRelations)
{
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntity();
    Entity child2 = registry->CreateEntity();
    Entity child3 = registry->CreateEntity();
    
    // Set up parent-child relationships
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(child3, parent);
    
    // Get unfiltered relations for parent
    auto relations = registry->GetRelations(parent);
    
    // Check children
    auto children = relations.GetChildren();
    EXPECT_EQ(children.size(), 3u);
    EXPECT_TRUE(std::find(children.begin(), children.end(), child1) != children.end());
    EXPECT_TRUE(std::find(children.begin(), children.end(), child2) != children.end());
    EXPECT_TRUE(std::find(children.begin(), children.end(), child3) != children.end());
    
    // Check parent from child's perspective
    auto childRelations = registry->GetRelations(child1);
    Entity retrievedParent = childRelations.GetParent();
    EXPECT_EQ(retrievedParent, parent);
}

// Test filtered Relations with required components
TEST_F(RelationsTest, FilteredRelationsRequired)
{
    Entity parent = registry->CreateEntityWith(Position{0, 0, 0});
    Entity child1 = registry->CreateEntityWith(Position{1, 0, 0}, Velocity{1, 0, 0});
    Entity child2 = registry->CreateEntityWith(Position{2, 0, 0});
    Entity child3 = registry->CreateEntityWith(Velocity{3, 0, 0}); // No Position
    
    // Set up relationships
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(child3, parent);
    
    // Get relations filtered by Position component
    auto relations = registry->GetRelations<Position>(parent);
    
    // Should only get children with Position
    auto children = relations.GetChildren();
    size_t count = 0;
    for (Entity child : children)
    {
        count++;
        EXPECT_NE(registry->GetComponent<Position>(child), nullptr);
    }
    EXPECT_EQ(count, 2u); // child1 and child2
}

// Test filtered Relations with Not modifier
TEST_F(RelationsTest, FilteredRelationsNot)
{
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntityWith(Position{1, 0, 0}, Enemy{});
    Entity child2 = registry->CreateEntityWith(Position{2, 0, 0});
    Entity child3 = registry->CreateEntity<Enemy>();
    
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(child3, parent);
    
    // Get relations excluding Enemy entities
    auto relations = registry->GetRelations<Not<Enemy>>(parent);
    
    // Should only get child2 (no Enemy component)
    auto children = relations.GetChildren();
    size_t count = 0;
    for (Entity child : children)
    {
        count++;
        EXPECT_FALSE(registry->HasComponent<Enemy>(child));
    }
    EXPECT_EQ(count, 1u);
}

// Test filtered Relations with Any modifier
TEST_F(RelationsTest, FilteredRelationsAny)
{
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntity<Player>();
    Entity child2 = registry->CreateEntity<Enemy>();
    Entity child3 = registry->CreateEntity<Position>();  // Neither Player nor Enemy
    Entity child4 = registry->CreateEntity<Player, Enemy>(); // Both
    
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(child3, parent);
    registry->SetParent(child4, parent);
    
    // Get relations with Any<Player, Enemy>
    auto relations = registry->GetRelations<Any<Player, Enemy>>(parent);
    
    // Should get child1, child2, and child4 (have at least one)
    auto children = relations.GetChildren();
    size_t count = 0;
    for (Entity child : children)
    {
        count++;
        bool hasPlayer = registry->HasComponent<Player>(child);
        bool hasEnemy = registry->HasComponent<Enemy>(child);
        EXPECT_TRUE(hasPlayer || hasEnemy);
    }
    EXPECT_EQ(count, 3u);
}

// Test filtered Relations with OneOf modifier
TEST_F(RelationsTest, FilteredRelationsOneOf)
{
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntity<Player>();
    Entity child2 = registry->CreateEntity<Enemy>();
    Entity child3 = registry->CreateEntity<Position>();  // Neither
    Entity child4 = registry->CreateEntity<Player, Enemy>(); // Both (should be excluded)
    
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(child3, parent);
    registry->SetParent(child4, parent);
    
    // Get relations with OneOf<Player, Enemy>
    auto relations = registry->GetRelations<OneOf<Player, Enemy>>(parent);
    
    // Should only get child1 and child2 (exactly one)
    auto children = relations.GetChildren();
    size_t count = 0;
    for (Entity child : children)
    {
        count++;
        bool hasPlayer = registry->HasComponent<Player>(child);
        bool hasEnemy = registry->HasComponent<Enemy>(child);
        EXPECT_TRUE(hasPlayer != hasEnemy); // XOR - exactly one
    }
    EXPECT_EQ(count, 2u);
}

// Test links with filtering
TEST_F(RelationsTest, FilteredLinks)
{
    Entity entity1 = registry->CreateEntityWith(Position{1, 0, 0});
    Entity entity2 = registry->CreateEntityWith(Position{2, 0, 0}, Enemy{});
    Entity entity3 = registry->CreateEntity<Enemy>();
    Entity entity4 = registry->CreateEntityWith(Position{4, 0, 0});
    
    // Create links
    registry->AddLink(entity1, entity2);
    registry->AddLink(entity1, entity3);
    registry->AddLink(entity1, entity4);
    
    // Get links filtered by Position component
    auto relations = registry->GetRelations<Position>(entity1);
    auto links = relations.GetLinks();
    
    // Should only get entity2 and entity4 (have Position)
    size_t count = 0;
    for (Entity linked : links)
    {
        count++;
        EXPECT_NE(registry->GetComponent<Position>(linked), nullptr);
    }
    EXPECT_EQ(count, 2u);
    
    // Get links excluding Enemy
    auto relationsNoEnemy = registry->GetRelations<Not<Enemy>>(entity1);
    auto linksNoEnemy = relationsNoEnemy.GetLinks();
    
    // Should only get entity4 (no Enemy)
    count = 0;
    for (Entity linked : linksNoEnemy)
    {
        count++;
        EXPECT_FALSE(registry->HasComponent<Enemy>(linked));
    }
    EXPECT_EQ(count, 1u);
}

// Test parent filtering
TEST_F(RelationsTest, FilteredParent)
{
    Entity parent1 = registry->CreateEntity<Position>();
    Entity parent2 = registry->CreateEntity<Enemy>();
    Entity child1 = registry->CreateEntity();
    Entity child2 = registry->CreateEntity();
    
    // Set parents
    registry->SetParent(child1, parent1);
    registry->SetParent(child2, parent2);
    
    // Get parent with Position filter
    auto relations1 = registry->GetRelations<Position>(child1);
    Entity filteredParent1 = relations1.GetParent();
    EXPECT_EQ(filteredParent1, parent1); // Parent has Position
    
    auto relations2 = registry->GetRelations<Position>(child2);
    Entity filteredParent2 = relations2.GetParent();
    EXPECT_FALSE(filteredParent2.IsValid()); // Parent doesn't have Position
    
    // Get parent excluding Enemy
    auto relationsNoEnemy = registry->GetRelations<Not<Enemy>>(child2);
    Entity parentNoEnemy = relationsNoEnemy.GetParent();
    EXPECT_FALSE(parentNoEnemy.IsValid()); // Parent has Enemy
}

// Test descendants traversal
TEST_F(RelationsTest, DescendantsTraversal)
{
    // Create hierarchy:
    //     root
    //    /    \
    //  child1  child2
    //   |       |
    // grand1  grand2
    
    Entity root = registry->CreateEntityWith(Position{0, 0, 0});
    Entity child1 = registry->CreateEntityWith(Position{1, 0, 0});
    Entity child2 = registry->CreateEntityWith(Position{2, 0, 0});
    Entity grand1 = registry->CreateEntityWith(Position{1, 1, 0});
    Entity grand2 = registry->CreateEntityWith(Position{2, 1, 0});
    
    registry->SetParent(child1, root);
    registry->SetParent(child2, root);
    registry->SetParent(grand1, child1);
    registry->SetParent(grand2, child2);
    
    // Get all descendants
    auto relations = registry->GetRelations(root);
    
    std::unordered_set<Entity> foundEntities;
    size_t maxDepth = 0;
    
    relations.ForEachDescendant([&](Entity entity, size_t depth) {
        foundEntities.insert(entity);
        maxDepth = std::max(maxDepth, depth);
        
        if (depth == 1)
        {
            // Direct children
            EXPECT_TRUE(entity == child1 || entity == child2);
        }
        else if (depth == 2)
        {
            // Grandchildren
            EXPECT_TRUE(entity == grand1 || entity == grand2);
        }
    });
    
    EXPECT_EQ(foundEntities.size(), 4u);
    EXPECT_EQ(maxDepth, 2u);
}

// Test descendants with filtering
TEST_F(RelationsTest, FilteredDescendants)
{
    Entity root = registry->CreateEntity();
    Entity child1 = registry->CreateEntityWith(Position{1, 0, 0});
    Entity child2 = registry->CreateEntity<Enemy>();
    Entity grand1 = registry->CreateEntityWith(Position{1, 1, 0});
    Entity grand2 = registry->CreateEntityWith(Position{2, 1, 0}, Enemy{});
    
    registry->SetParent(child1, root);
    registry->SetParent(child2, root);
    registry->SetParent(grand1, child1);
    registry->SetParent(grand2, child2);
    
    // Get descendants with Position
    auto relations = registry->GetRelations<Position>(root);
    
    size_t count = 0;
    relations.ForEachDescendant([&](Entity entity, size_t, Position&) {
        count++;
        // Position is automatically provided as parameter when filtering
    });
    // Should find: child1, grand1, and grand2 (all have Position)
    // Note: child2 is skipped but its children are still checked
    EXPECT_EQ(count, 3u);
}

// Test ancestors traversal
TEST_F(RelationsTest, AncestorsTraversal)
{
    Entity root = registry->CreateEntityWith(Position{0, 0, 0});
    Entity parent = registry->CreateEntityWith(Position{1, 0, 0});
    Entity child = registry->CreateEntityWith(Position{2, 0, 0});
    
    registry->SetParent(parent, root);
    registry->SetParent(child, parent);
    
    // Get ancestors of child
    auto relations = registry->GetRelations(child);
    
    std::vector<Entity> foundAncestors;
    relations.ForEachAncestor([&](Entity entity, size_t depth) {
        foundAncestors.push_back(entity);
        if (depth == 1)
        {
            EXPECT_EQ(entity, parent);
        }
        else if (depth == 2)
        {
            EXPECT_EQ(entity, root);
        }
    });
    
    EXPECT_EQ(foundAncestors.size(), 2u);
}

// Test ancestors with filtering
TEST_F(RelationsTest, FilteredAncestors)
{
    Entity root = registry->CreateEntity<Enemy>();
    Entity parent = registry->CreateEntityWith(Position{1, 0, 0});
    Entity child = registry->CreateEntity();
    
    registry->SetParent(parent, root);
    registry->SetParent(child, parent);
    
    // Get ancestors excluding Enemy
    auto relations = registry->GetRelations<Not<Enemy>>(child);
    
    size_t count = 0;
    relations.ForEachAncestor([&](Entity entity, size_t) {
        count++;
        EXPECT_FALSE(registry->HasComponent<Enemy>(entity));
    });
    EXPECT_EQ(count, 1u); // Only parent (root has Enemy)
}

// Test complex filtering combinations
TEST_F(RelationsTest, ComplexFiltering)
{
    Entity root = registry->CreateEntity();
    
    // Create children with various component combinations
    Entity child1 = registry->CreateEntityWith(Position{}, Player{});
    Entity child2 = registry->CreateEntityWith(Position{}, Enemy{});
    Entity child3 = registry->CreateEntityWith(Position{}, Health{});
    Entity child4 = registry->CreateEntity<Player, Health>();
    Entity child5 = registry->CreateEntity<Enemy, Health>();
    
    registry->SetParent(child1, root);
    registry->SetParent(child2, root);
    registry->SetParent(child3, root);
    registry->SetParent(child4, root);
    registry->SetParent(child5, root);
    
    // Complex query: Position AND (Player OR Enemy) AND NOT Health
    auto relations = registry->GetRelations<Position, Any<Player, Enemy>, Not<Health>>(root);
    auto children = relations.GetChildren();
    
    size_t count = 0;
    for (Entity child : children)
    {
        count++;
        // Must have Position
        EXPECT_NE(registry->GetComponent<Position>(child), nullptr);
        // Must have Player or Enemy
        bool hasPlayer = registry->HasComponent<Player>(child);
        bool hasEnemy = registry->HasComponent<Enemy>(child);
        EXPECT_TRUE(hasPlayer || hasEnemy);
        // Must NOT have Health
        EXPECT_EQ(registry->GetComponent<Health>(child), nullptr);
    }
    
    // Should match child1 and child2
    EXPECT_EQ(count, 2u);
}

// Test empty filtering results
TEST_F(RelationsTest, EmptyFilterResults)
{
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntity<Position>();
    Entity child2 = registry->CreateEntity<Velocity>();
    
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    
    // Filter for component none of them have
    auto relations = registry->GetRelations<Health>(parent);
    auto children = relations.GetChildren();
    
    EXPECT_TRUE(children.empty());
    
    // Links when no entities match filter
    Entity other = registry->CreateEntity<Enemy>();
    registry->AddLink(parent, other);
    
    auto linksRelations = registry->GetRelations<Player>(parent);
    auto links = linksRelations.GetLinks();
    
    EXPECT_TRUE(links.empty());
}

// Test bidirectional link filtering
TEST_F(RelationsTest, BidirectionalLinkFiltering)
{
    Entity entity1 = registry->CreateEntityWith(Position{}, Player{});
    Entity entity2 = registry->CreateEntityWith(Position{}, Enemy{});
    Entity entity3 = registry->CreateEntityWith(Health{});
    
    registry->AddLink(entity1, entity2);
    registry->AddLink(entity1, entity3);
    
    // From entity1's perspective, filter for Position
    auto relations1 = registry->GetRelations<Position>(entity1);
    auto links1 = relations1.GetLinks();
    
    size_t count = 0;
    for (Entity linked : links1)
    {
        count++;
        EXPECT_NE(registry->GetComponent<Position>(linked), nullptr);
    }
    EXPECT_EQ(count, 1u); // Only entity2
    
    // From entity2's perspective, filter for Player
    auto relations2 = registry->GetRelations<Player>(entity2);
    auto links2 = relations2.GetLinks();
    
    count = 0;
    for (Entity linked : links2)
    {
        count++;
        EXPECT_TRUE(registry->HasComponent<Player>(linked));
    }
    EXPECT_EQ(count, 1u); // Only entity1
}

// Test performance with large hierarchies
TEST_F(RelationsTest, LargeHierarchyPerformance)
{
    // Create a deep hierarchy
    Entity root = registry->CreateEntityWith(Position{0, 0, 0});
    Entity current = root;
    
    const size_t depth = 100;
    std::vector<Entity> entities;
    entities.push_back(root);
    
    for (size_t i = 1; i < depth; ++i)
    {
        Entity child = registry->CreateEntityWith(Position{float(i), 0, 0});
        registry->SetParent(child, current);
        entities.push_back(child);
        current = child;
    }
    
    // Traverse all descendants from root
    auto relations = registry->GetRelations(root);
    
    size_t count = 0;
    size_t maxDepth = 0;
    relations.ForEachDescendant([&](Entity, size_t depth) {
        count++;
        maxDepth = std::max(maxDepth, depth);
    });
    
    EXPECT_EQ(count, depth - 1); // All except root
    EXPECT_EQ(maxDepth, depth - 1);
    
    // Traverse ancestors from leaf
    auto leafRelations = registry->GetRelations(entities.back());
    
    count = 0;
    maxDepth = 0;
    leafRelations.ForEachAncestor([&](Entity, size_t depth) {
        count++;
        maxDepth = std::max(maxDepth, depth);
    });
    
    EXPECT_EQ(count, depth - 1); // All except leaf itself
    EXPECT_EQ(maxDepth, depth - 1);
}

// Test circular hierarchy handling
TEST_F(RelationsTest, CircularHierarchyHandling)
{
    // Test that circular hierarchies are now prevented
    Entity a = registry->CreateEntity();
    Entity b = registry->CreateEntity();
    Entity c = registry->CreateEntity();

    // Create chain: a -> b -> c (a is root, b is child of a, c is child of b)
    registry->SetParent(b, a);
    registry->SetParent(c, b);

    // Verify the chain is correct
    EXPECT_EQ(registry->GetParent(b), a);
    EXPECT_EQ(registry->GetParent(c), b);
    EXPECT_FALSE(registry->GetParent(a).IsValid()); // a has no parent

    // Try to create cycle: a -> c (would make a -> b -> c -> a)
    // This should be rejected because a is an ancestor of c
    registry->SetParent(a, c);

    // Verify cycle was prevented - a should still have no parent
    EXPECT_FALSE(registry->GetParent(a).IsValid());

    // Verify the original chain is still intact
    EXPECT_EQ(registry->GetParent(b), a);
    EXPECT_EQ(registry->GetParent(c), b);

    // Test traversal works correctly with the valid (non-cyclic) hierarchy
    auto relations = registry->GetRelations(a);

    size_t descendantCount = 0;
    relations.ForEachDescendant([&](Entity, size_t) {
        descendantCount++;
    });

    // Should visit b and c (descendants of a)
    EXPECT_EQ(descendantCount, 2u);

    // Test ancestor traversal from c
    auto cRelations = registry->GetRelations(c);

    size_t ancestorCount = 0;
    cRelations.ForEachAncestor([&](Entity, size_t) {
        ancestorCount++;
    });

    // Should visit b and a (ancestors of c)
    EXPECT_EQ(ancestorCount, 2u);
}