#include <gtest/gtest.h>
#include <vector>
#include <unordered_set>
#include <queue>
#include <random>
#include "Astra/Registry/Registry.hpp"
#include "Astra/Registry/Relations.hpp"
#include "../TestComponents.hpp"

using namespace Astra;
using namespace Astra::Test;

class ComplexRelationshipTest : public ::testing::Test
{
protected:
    std::unique_ptr<Registry> registry;
    
    void SetUp() override
    {
        registry = std::make_unique<Registry>();
        
        // Register test components
        auto componentRegistry = registry->GetComponentRegistry();
        componentRegistry->RegisterComponents<Position, Velocity, Health, Transform, Name>();
    }
    
    void TearDown() override
    {
        registry.reset();
    }
    
    // Helper to verify hierarchy consistency
    bool VerifyHierarchyConsistency(Entity root)
    {
        std::unordered_set<Entity> visited;
        std::queue<Entity> toCheck;
        toCheck.push(root);
        visited.insert(root);
        
        while (!toCheck.empty())
        {
            Entity current = toCheck.front();
            toCheck.pop();
            
            auto relations = registry->GetRelations(current);
            
            // Check parent-child consistency
            for (Entity child : relations.GetChildren())
            {
                auto childRelations = registry->GetRelations(child);
                if (childRelations.GetParent() != current)
                {
                    return false; // Child's parent doesn't match
                }
                
                if (visited.find(child) == visited.end())
                {
                    visited.insert(child);
                    toCheck.push(child);
                }
            }
        }
        
        return true;
    }
};

// Test very deep hierarchy (1000+ levels)
TEST_F(ComplexRelationshipTest, VeryDeepHierarchy)
{
    const size_t depth = 1500;
    std::vector<Entity> chain;
    
    // Create deep chain
    for (size_t i = 0; i < depth; ++i)
    {
        chain.push_back(registry->CreateEntity());
    }
    
    // Link them in a chain
    for (size_t i = 1; i < depth; ++i)
    {
        registry->SetParent(chain[i], chain[i - 1]);
    }
    
    // Verify parent-child relationships
    for (size_t i = 1; i < depth; ++i)
    {
        auto relations = registry->GetRelations(chain[i]);
        EXPECT_EQ(relations.GetParent(), chain[i - 1]);
    }
    
    // Traverse from root to leaf
    size_t traversalDepth = 0;
    auto rootRelations = registry->GetRelations(chain[0]);
    rootRelations.ForEachDescendant([&traversalDepth](Entity, size_t depth) {
        traversalDepth = std::max(traversalDepth, depth);
    });
    
    EXPECT_EQ(traversalDepth, depth - 1) << "Should traverse entire hierarchy";
    
    // Test removing middle entity
    Entity middle = chain[depth / 2];
    registry->DestroyEntity(middle);
    
    // Children should be orphaned
    if (depth / 2 + 1 < depth)
    {
        auto orphanRelations = registry->GetRelations(chain[depth / 2 + 1]);
        EXPECT_FALSE(orphanRelations.GetParent().IsValid());
    }
}

// Test very wide hierarchy (1000+ children per node)
TEST_F(ComplexRelationshipTest, VeryWideHierarchy)
{
    const size_t width = 1500;
    
    Entity root = registry->CreateEntity();
    std::vector<Entity> children;
    
    // Create many children
    for (size_t i = 0; i < width; ++i)
    {
        Entity child = registry->CreateEntity();
        registry->SetParent(child, root);
        children.push_back(child);
    }
    
    // Verify all children are linked
    auto rootRelations = registry->GetRelations(root);
    size_t childCount = 0;
    for (Entity child : rootRelations.GetChildren())
    {
        childCount++;
        EXPECT_TRUE(registry->IsValid(child));
    }
    
    EXPECT_EQ(childCount, width);
    
    // Each child should have root as parent
    for (Entity child : children)
    {
        auto childRelations = registry->GetRelations(child);
        EXPECT_EQ(childRelations.GetParent(), root);
    }
    
    // Add second level (grandchildren)
    std::vector<Entity> grandchildren;
    for (size_t i = 0; i < 10; ++i)
    {
        for (Entity parent : children)
        {
            if (i == 0 || (i < 3 && parent.GetID() % 100 == 0))
            {
                Entity grandchild = registry->CreateEntity();
                registry->SetParent(grandchild, parent);
                grandchildren.push_back(grandchild);
            }
        }
    }
    
    // Verify hierarchy iteration finds all descendants
    size_t descendantCount = 0;
    size_t maxDepth = 0;
    rootRelations.ForEachDescendant([&descendantCount, &maxDepth](Entity, size_t depth) {
        descendantCount++;
        maxDepth = std::max(maxDepth, depth);
    });
    EXPECT_LE(maxDepth, 2u) << "Max depth should be 2";
    
    EXPECT_EQ(descendantCount, children.size() + grandchildren.size());
}

// Test diamond-shaped hierarchy
TEST_F(ComplexRelationshipTest, DiamondHierarchy)
{
    //     root
    //    /    \
    //   A      B
    //    \    /
    //     child
    
    Entity root = registry->CreateEntity();
    Entity a = registry->CreateEntity();
    Entity b = registry->CreateEntity();
    Entity child = registry->CreateEntity();
    
    registry->SetParent(a, root);
    registry->SetParent(b, root);
    
    // Child can only have one parent
    registry->SetParent(child, a);
    
    // Try to set second parent (should replace first)
    registry->SetParent(child, b);
    
    auto childRelations = registry->GetRelations(child);
    EXPECT_EQ(childRelations.GetParent(), b) << "Should have B as parent (last set)";
    
    // A should not have child anymore
    auto aRelations = registry->GetRelations(a);
    size_t aChildCount = 0;
    for (Entity c : aRelations.GetChildren())
    {
        aChildCount++;
    }
    EXPECT_EQ(aChildCount, 0u);
    
    // B should have child
    auto bRelations = registry->GetRelations(b);
    bool hasChild = false;
    for (Entity c : bRelations.GetChildren())
    {
        if (c == child) hasChild = true;
    }
    EXPECT_TRUE(hasChild);
}

// Test multiple intersecting circular references
TEST_F(ComplexRelationshipTest, MultipleCircularReferences)
{
    // Create complex graph with multiple cycles
    //   A -> B -> C
    //   ^    |    |
    //   |    v    |
    //   D <- E <- F
    //   |         ^
    //   +---------|
    
    Entity a = registry->CreateEntity();
    Entity b = registry->CreateEntity();
    Entity c = registry->CreateEntity();
    Entity d = registry->CreateEntity();
    Entity e = registry->CreateEntity();
    Entity f = registry->CreateEntity();
    
    // Create links forming multiple cycles
    registry->AddLink(a, b);
    registry->AddLink(b, c);
    registry->AddLink(c, f);
    registry->AddLink(f, e);
    registry->AddLink(e, d);
    registry->AddLink(d, a);
    registry->AddLink(b, e);
    registry->AddLink(d, f);
    
    // Verify all links are bidirectional
    auto relA = registry->GetRelations(a);
    auto relB = registry->GetRelations(b);
    auto relD = registry->GetRelations(d);
    auto relF = registry->GetRelations(f);
    
    bool aLinkedToB = false, bLinkedToA = false;
    bool dLinkedToF = false, fLinkedToD = false;
    
    for (Entity e : relA.GetLinks()) if (e == b) aLinkedToB = true;
    for (Entity e : relB.GetLinks()) if (e == a) bLinkedToA = true;
    for (Entity e : relD.GetLinks()) if (e == f) dLinkedToF = true;
    for (Entity e : relF.GetLinks()) if (e == d) fLinkedToD = true;
    
    EXPECT_TRUE(aLinkedToB && bLinkedToA);
    EXPECT_TRUE(dLinkedToF && fLinkedToD);
    
    // Check that we can traverse without infinite loops
    auto aRelations = registry->GetRelations(a);
    std::unordered_set<Entity> reachable;
    
    // Simple BFS to find all reachable entities
    std::queue<Entity> toVisit;
    toVisit.push(a);
    reachable.insert(a);
    
    while (!toVisit.empty() && reachable.size() < 100)
    {
        Entity current = toVisit.front();
        toVisit.pop();
        
        auto currentRelations = registry->GetRelations(current);
        for (Entity linked : currentRelations.GetLinks())
        {
            if (reachable.find(linked) == reachable.end())
            {
                reachable.insert(linked);
                toVisit.push(linked);
            }
        }
    }
    
    // All entities should be reachable from A
    EXPECT_EQ(reachable.size(), 6u);
    EXPECT_TRUE(reachable.count(a));
    EXPECT_TRUE(reachable.count(b));
    EXPECT_TRUE(reachable.count(c));
    EXPECT_TRUE(reachable.count(d));
    EXPECT_TRUE(reachable.count(e));
    EXPECT_TRUE(reachable.count(f));
}

// Test mixed hierarchy and link relationships
TEST_F(ComplexRelationshipTest, MixedHierarchyAndLinks)
{
    // Create tree structure
    Entity root = registry->CreateEntity();
    Entity branch1 = registry->CreateEntity();
    Entity branch2 = registry->CreateEntity();
    Entity leaf1 = registry->CreateEntity();
    Entity leaf2 = registry->CreateEntity();
    Entity leaf3 = registry->CreateEntity();
    
    // Set up hierarchy
    registry->SetParent(branch1, root);
    registry->SetParent(branch2, root);
    registry->SetParent(leaf1, branch1);
    registry->SetParent(leaf2, branch1);
    registry->SetParent(leaf3, branch2);
    
    // Add cross-links between branches
    registry->AddLink(leaf1, leaf3);
    registry->AddLink(branch1, branch2);
    registry->AddLink(leaf2, root); // Link to ancestor
    
    // Verify hierarchy is maintained
    EXPECT_TRUE(VerifyHierarchyConsistency(root));
    
    // Verify links don't affect parent-child
    auto leaf1Relations = registry->GetRelations(leaf1);
    auto leaf3Relations = registry->GetRelations(leaf3);
    EXPECT_EQ(leaf1Relations.GetParent(), branch1);
    
    bool leaf1LinkedToLeaf3 = false;
    for (Entity e : leaf1Relations.GetLinks())
        if (e == leaf3) leaf1LinkedToLeaf3 = true;
    EXPECT_TRUE(leaf1LinkedToLeaf3);
    
    // Test destroying entity with both parents and links
    registry->DestroyEntity(branch1);
    
    // Children should be orphaned
    auto leaf1NewRelations = registry->GetRelations(leaf1);
    EXPECT_FALSE(leaf1NewRelations.GetParent().IsValid());
    
    // But link should remain
    auto newLeaf1Relations = registry->GetRelations(leaf1);
    bool stillLinked = false;
    for (Entity e : newLeaf1Relations.GetLinks())
        if (e == leaf3) stillLinked = true;
    EXPECT_TRUE(stillLinked);
}

// Test forest of trees (multiple root nodes)
TEST_F(ComplexRelationshipTest, ForestOfTrees)
{
    const size_t treeCount = 100;
    const size_t nodesPerTree = 50;
    
    std::vector<Entity> roots;
    std::vector<std::vector<Entity>> trees;
    
    // Create multiple trees
    for (size_t t = 0; t < treeCount; ++t)
    {
        Entity root = registry->CreateEntity();
        roots.push_back(root);
        
        std::vector<Entity> tree;
        tree.push_back(root);
        
        // Create tree structure
        for (size_t i = 1; i < nodesPerTree; ++i)
        {
            Entity node = registry->CreateEntity();
            // Parent is a random earlier node in the tree
            size_t parentIdx = i / 2; // Simple binary tree structure
            registry->SetParent(node, tree[parentIdx]);
            tree.push_back(node);
        }
        
        trees.push_back(tree);
    }
    
    // Link some trees together
    for (size_t i = 0; i < treeCount - 1; ++i)
    {
        // Link leaf nodes between adjacent trees
        registry->AddLink(trees[i].back(), trees[i + 1].back());
    }
    
    // Verify each tree
    for (size_t t = 0; t < treeCount; ++t)
    {
        EXPECT_TRUE(VerifyHierarchyConsistency(roots[t]));
        
        // Count nodes in tree
        size_t nodeCount = 0;
        auto treeRelations = registry->GetRelations(roots[t]);
        treeRelations.ForEachDescendant([&nodeCount](Entity, size_t) {
            nodeCount++;
        });
        
        EXPECT_EQ(nodeCount, nodesPerTree - 1) << "Tree " << t << " has wrong node count";
    }
}

// Test relationship stability during rapid changes
TEST_F(ComplexRelationshipTest, RapidRelationshipChanges)
{
    std::vector<Entity> entities;
    for (int i = 0; i < 100; ++i)
    {
        entities.push_back(registry->CreateEntity());
    }
    
    // Random number generator for chaos
    std::mt19937 rng(12345); // Fixed seed for reproducibility
    std::uniform_int_distribution<size_t> dist(0, entities.size() - 1);
    
    // Perform many random operations
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        size_t idx1 = dist(rng);
        size_t idx2 = dist(rng);
        
        if (idx1 == idx2) continue;
        
        switch (iteration % 4)
        {
        case 0:
            // Set parent
            registry->SetParent(entities[idx1], entities[idx2]);
            break;
        case 1:
            // Remove parent
            registry->RemoveParent(entities[idx1]);
            break;
        case 2:
            // Add link
            registry->AddLink(entities[idx1], entities[idx2]);
            break;
        case 3:
            // Remove link
            registry->RemoveLink(entities[idx1], entities[idx2]);
            break;
        }
    }
    
    // Verify all entities are still valid
    for (Entity e : entities)
    {
        EXPECT_TRUE(registry->IsValid(e));
    }
    
    // Verify no crashes when iterating relationships
    for (Entity e : entities)
    {
        auto relations = registry->GetRelations(e);
        
        // Count children
        size_t childCount = 0;
        for (Entity child : relations.GetChildren())
        {
            childCount++;
            EXPECT_TRUE(registry->IsValid(child));
        }
        
        // Count links
        size_t linkCount = 0;
        for (Entity linked : relations.GetLinks())
        {
            linkCount++;
            EXPECT_TRUE(registry->IsValid(linked));
        }
    }
}

// Test orphaning behavior
TEST_F(ComplexRelationshipTest, OrphaningBehavior)
{
    // Create hierarchy
    Entity grandparent = registry->CreateEntity();
    Entity parent = registry->CreateEntity();
    Entity child1 = registry->CreateEntity();
    Entity child2 = registry->CreateEntity();
    Entity grandchild = registry->CreateEntity();
    
    registry->SetParent(parent, grandparent);
    registry->SetParent(child1, parent);
    registry->SetParent(child2, parent);
    registry->SetParent(grandchild, child1);
    
    // Also add some links
    registry->AddLink(child1, child2);
    registry->AddLink(parent, grandchild);
    
    // Destroy parent
    registry->DestroyEntity(parent);
    
    // Children should be orphaned but still valid
    EXPECT_TRUE(registry->IsValid(child1));
    EXPECT_TRUE(registry->IsValid(child2));
    
    auto child1Relations = registry->GetRelations(child1);
    auto child2Relations = registry->GetRelations(child2);
    
    EXPECT_FALSE(child1Relations.GetParent().IsValid()) << "Child1 should be orphaned";
    EXPECT_FALSE(child2Relations.GetParent().IsValid()) << "Child2 should be orphaned";
    
    // Grandchild should still have child1 as parent
    auto grandchildRelations = registry->GetRelations(grandchild);
    EXPECT_EQ(grandchildRelations.GetParent(), child1);
    
    // Links between children should remain
    auto c1Relations = registry->GetRelations(child1);
    bool childrenLinked = false;
    for (Entity e : c1Relations.GetLinks())
        if (e == child2) childrenLinked = true;
    EXPECT_TRUE(childrenLinked);
    
    // Link from destroyed parent should be gone (parent is destroyed, can't check its links)
}

// Test relationship graph memory cleanup
TEST_F(ComplexRelationshipTest, RelationshipMemoryCleanup)
{
    // Create and destroy many relationships
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        std::vector<Entity> entities;
        
        // Create entities with complex relationships
        for (int i = 0; i < 1000; ++i)
        {
            entities.push_back(registry->CreateEntity());
        }
        
        // Create parent-child relationships
        for (size_t i = 1; i < entities.size(); ++i)
        {
            registry->SetParent(entities[i], entities[i / 2]);
        }
        
        // Create many links
        for (size_t i = 0; i < entities.size(); ++i)
        {
            for (size_t j = i + 1; j < std::min(i + 5, entities.size()); ++j)
            {
                registry->AddLink(entities[i], entities[j]);
            }
        }
        
        // Destroy all entities
        registry->DestroyEntities(entities);
        
        // Verify cleanup
        EXPECT_EQ(registry->Size(), 0u) << "All entities should be destroyed in iteration " << iteration;
    }
    
    // Create one final entity to verify system still works
    Entity survivor = registry->CreateEntity();
    EXPECT_TRUE(registry->IsValid(survivor));
}