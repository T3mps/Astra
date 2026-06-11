#include <gtest/gtest.h>
#include <Astra/Registry/RelationshipGraph.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Entity/Entity.hpp>
#include <vector>
#include <set>

namespace
{
    using namespace Astra;
    
    // Helper to create test entities
    Entity CreateTestEntity(uint32_t id, uint8_t version = 1)
    {
        return Entity(id, version);
    }
}

class RelationshipGraphSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
    }
    
    void TearDown() override
    {
    }
};

TEST_F(RelationshipGraphSerializationTest, EmptyGraph)
{
    RelationshipGraph graph;
    
    // Serialize empty graph
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        EXPECT_EQ(newGraph.GetParentChildCount(), 0u);
        EXPECT_EQ(newGraph.GetParentCount(), 0u);
        EXPECT_EQ(newGraph.GetLinkedEntityCount(), 0u);
    }
}

TEST_F(RelationshipGraphSerializationTest, SingleParentChild)
{
    RelationshipGraph graph;
    
    Entity parent = CreateTestEntity(1);
    Entity child = CreateTestEntity(2);
    
    graph.SetParent(child, parent);
    
    EXPECT_EQ(graph.GetParent(child), parent);
    EXPECT_TRUE(graph.HasChildren(parent));
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        EXPECT_EQ(newGraph.GetParent(child), parent);
        EXPECT_TRUE(newGraph.HasChildren(parent));
        
        const auto& children = newGraph.GetChildren(parent);
        EXPECT_EQ(children.size(), 1u);
        EXPECT_EQ(children[0], child);
    }
}

TEST_F(RelationshipGraphSerializationTest, MultipleChildren)
{
    RelationshipGraph graph;
    
    Entity parent = CreateTestEntity(1);
    Entity child1 = CreateTestEntity(2);
    Entity child2 = CreateTestEntity(3);
    Entity child3 = CreateTestEntity(4);
    
    graph.SetParent(child1, parent);
    graph.SetParent(child2, parent);
    graph.SetParent(child3, parent);
    
    EXPECT_EQ(graph.GetChildren(parent).size(), 3u);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        const auto& children = newGraph.GetChildren(parent);
        EXPECT_EQ(children.size(), 3u);
        
        // Check all children are present
        std::set<Entity> childSet(children.begin(), children.end());
        EXPECT_TRUE(childSet.count(child1));
        EXPECT_TRUE(childSet.count(child2));
        EXPECT_TRUE(childSet.count(child3));
        
        // Check parent relationships
        EXPECT_EQ(newGraph.GetParent(child1), parent);
        EXPECT_EQ(newGraph.GetParent(child2), parent);
        EXPECT_EQ(newGraph.GetParent(child3), parent);
    }
}

TEST_F(RelationshipGraphSerializationTest, NestedHierarchy)
{
    RelationshipGraph graph;
    
    // Create a hierarchy: grandparent -> parent -> child
    Entity grandparent = CreateTestEntity(1);
    Entity parent = CreateTestEntity(2);
    Entity child = CreateTestEntity(3);
    
    graph.SetParent(parent, grandparent);
    graph.SetParent(child, parent);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        EXPECT_EQ(newGraph.GetParent(parent), grandparent);
        EXPECT_EQ(newGraph.GetParent(child), parent);
        EXPECT_FALSE(newGraph.HasParent(grandparent));
        
        EXPECT_TRUE(newGraph.HasChildren(grandparent));
        EXPECT_TRUE(newGraph.HasChildren(parent));
        EXPECT_FALSE(newGraph.HasChildren(child));
    }
}

TEST_F(RelationshipGraphSerializationTest, SingleLink)
{
    RelationshipGraph graph;
    
    Entity a = CreateTestEntity(1);
    Entity b = CreateTestEntity(2);
    
    graph.AddLink(a, b);
    
    EXPECT_TRUE(graph.AreLinked(a, b));
    EXPECT_TRUE(graph.AreLinked(b, a));  // Links are bidirectional
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        EXPECT_TRUE(newGraph.AreLinked(a, b));
        EXPECT_TRUE(newGraph.AreLinked(b, a));
        
        const auto& linksA = newGraph.GetLinks(a);
        EXPECT_EQ(linksA.size(), 1u);
        EXPECT_EQ(linksA[0], b);
        
        const auto& linksB = newGraph.GetLinks(b);
        EXPECT_EQ(linksB.size(), 1u);
        EXPECT_EQ(linksB[0], a);
    }
}

TEST_F(RelationshipGraphSerializationTest, MultipleLinks)
{
    RelationshipGraph graph;
    
    Entity hub = CreateTestEntity(1);
    Entity a = CreateTestEntity(2);
    Entity b = CreateTestEntity(3);
    Entity c = CreateTestEntity(4);
    
    // Create star topology with hub linked to all others
    graph.AddLink(hub, a);
    graph.AddLink(hub, b);
    graph.AddLink(hub, c);
    
    // Also link a and b
    graph.AddLink(a, b);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        // Check hub links
        const auto& hubLinks = newGraph.GetLinks(hub);
        EXPECT_EQ(hubLinks.size(), 3u);
        std::set<Entity> hubLinkSet(hubLinks.begin(), hubLinks.end());
        EXPECT_TRUE(hubLinkSet.count(a));
        EXPECT_TRUE(hubLinkSet.count(b));
        EXPECT_TRUE(hubLinkSet.count(c));
        
        // Check a links (hub and b)
        const auto& aLinks = newGraph.GetLinks(a);
        EXPECT_EQ(aLinks.size(), 2u);
        std::set<Entity> aLinkSet(aLinks.begin(), aLinks.end());
        EXPECT_TRUE(aLinkSet.count(hub));
        EXPECT_TRUE(aLinkSet.count(b));
        
        // Check b links (hub and a)
        const auto& bLinks = newGraph.GetLinks(b);
        EXPECT_EQ(bLinks.size(), 2u);
        
        // Check c links (only hub)
        const auto& cLinks = newGraph.GetLinks(c);
        EXPECT_EQ(cLinks.size(), 1u);
        EXPECT_EQ(cLinks[0], hub);
    }
}

TEST_F(RelationshipGraphSerializationTest, MixedRelationships)
{
    RelationshipGraph graph;
    
    // Create entities
    Entity root = CreateTestEntity(1);
    Entity parent1 = CreateTestEntity(2);
    Entity parent2 = CreateTestEntity(3);
    Entity child1 = CreateTestEntity(4);
    Entity child2 = CreateTestEntity(5);
    Entity child3 = CreateTestEntity(6);
    Entity standalone = CreateTestEntity(7);
    
    // Create hierarchy
    graph.SetParent(parent1, root);
    graph.SetParent(parent2, root);
    graph.SetParent(child1, parent1);
    graph.SetParent(child2, parent1);
    graph.SetParent(child3, parent2);
    
    // Add some links
    graph.AddLink(child1, child2);
    graph.AddLink(child2, child3);
    graph.AddLink(standalone, root);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        // Verify hierarchy
        EXPECT_EQ(newGraph.GetParent(parent1), root);
        EXPECT_EQ(newGraph.GetParent(parent2), root);
        EXPECT_EQ(newGraph.GetParent(child1), parent1);
        EXPECT_EQ(newGraph.GetParent(child2), parent1);
        EXPECT_EQ(newGraph.GetParent(child3), parent2);
        EXPECT_FALSE(newGraph.HasParent(root));
        EXPECT_FALSE(newGraph.HasParent(standalone));
        
        // Verify children
        EXPECT_EQ(newGraph.GetChildren(root).size(), 2u);
        EXPECT_EQ(newGraph.GetChildren(parent1).size(), 2u);
        EXPECT_EQ(newGraph.GetChildren(parent2).size(), 1u);
        
        // Verify links
        EXPECT_TRUE(newGraph.AreLinked(child1, child2));
        EXPECT_TRUE(newGraph.AreLinked(child2, child3));
        EXPECT_TRUE(newGraph.AreLinked(standalone, root));
        EXPECT_FALSE(newGraph.AreLinked(child1, child3));
    }
}

TEST_F(RelationshipGraphSerializationTest, LargeGraph)
{
    RelationshipGraph graph;
    
    // Create a large tree with multiple levels
    std::vector<Entity> entities;
    const int levelsCount = 4;
    const int childrenPerNode = 5;
    
    // Create entities
    int id = 1;
    for (int level = 0; level < levelsCount; ++level)
    {
        int nodesInLevel = 1;
        for (int i = 0; i < level; ++i)
        {
            nodesInLevel *= childrenPerNode;
        }
        
        for (int i = 0; i < nodesInLevel; ++i)
        {
            entities.push_back(CreateTestEntity(id++));
        }
    }
    
    // Build tree structure
    int parentOffset = 0;
    int childOffset = 1;
    
    for (int level = 0; level < levelsCount - 1; ++level)
    {
        int parentsInLevel = 1;
        for (int i = 0; i < level; ++i)
        {
            parentsInLevel *= childrenPerNode;
        }
        
        for (int parentIdx = 0; parentIdx < parentsInLevel; ++parentIdx)
        {
            Entity parent = entities[parentOffset + parentIdx];
            for (int childIdx = 0; childIdx < childrenPerNode; ++childIdx)
            {
                Entity child = entities[childOffset + parentIdx * childrenPerNode + childIdx];
                graph.SetParent(child, parent);
            }
        }
        
        parentOffset = childOffset;
        childOffset += parentsInLevel * childrenPerNode;
    }
    
    // Add some cross-links
    for (size_t i = 1; i < entities.size() - 1; i += 3)
    {
        graph.AddLink(entities[i], entities[i + 1]);
    }
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        // Verify root has no parent
        EXPECT_FALSE(newGraph.HasParent(entities[0]));
        
        // Verify root has correct number of children
        EXPECT_EQ(newGraph.GetChildren(entities[0]).size(), static_cast<size_t>(childrenPerNode));
        
        // Spot check some relationships
        // First level children
        for (int i = 1; i <= childrenPerNode; ++i)
        {
            EXPECT_EQ(newGraph.GetParent(entities[i]), entities[0]);
        }
        
        // Check some links exist
        for (size_t i = 1; i < entities.size() - 1; i += 3)
        {
            EXPECT_TRUE(newGraph.AreLinked(entities[i], entities[i + 1]));
        }
    }
}

TEST_F(RelationshipGraphSerializationTest, EntityVersions)
{
    RelationshipGraph graph;
    
    // Create entities with different versions
    Entity parent = CreateTestEntity(100, 5);   // Version 5
    Entity child1 = CreateTestEntity(101, 10);  // Version 10
    Entity child2 = CreateTestEntity(102, 255); // Max version
    
    graph.SetParent(child1, parent);
    graph.SetParent(child2, parent);
    graph.AddLink(child1, child2);
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        // Verify relationships with exact entity versions
        EXPECT_EQ(newGraph.GetParent(child1), parent);
        EXPECT_EQ(newGraph.GetParent(child2), parent);
        
        // Verify the exact entities (including versions)
        const auto& children = newGraph.GetChildren(parent);
        EXPECT_EQ(children.size(), 2u);
        
        // Check that versions are preserved
        bool foundChild1 = false;
        bool foundChild2 = false;
        for (Entity child : children)
        {
            if (child.GetID() == 101)
            {
                EXPECT_EQ(child.GetVersion(), 10);
                foundChild1 = true;
            }
            else if (child.GetID() == 102)
            {
                EXPECT_EQ(child.GetVersion(), 255);
                foundChild2 = true;
            }
        }
        EXPECT_TRUE(foundChild1);
        EXPECT_TRUE(foundChild2);
        
        // Check link preservation with versions
        EXPECT_TRUE(newGraph.AreLinked(child1, child2));
    }
}

TEST_F(RelationshipGraphSerializationTest, EmptyAfterClear)
{
    RelationshipGraph graph;
    
    // Add some relationships
    Entity e1 = CreateTestEntity(1);
    Entity e2 = CreateTestEntity(2);
    Entity e3 = CreateTestEntity(3);
    
    graph.SetParent(e2, e1);
    graph.AddLink(e2, e3);
    
    // Clear all relationships
    graph.Clear();
    
    // Serialize
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        graph.Serialize(writer);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Deserialize
    {
        BinaryReader reader(buffer);
        auto result = RelationshipGraph::Deserialize(reader);
        ASSERT_TRUE(result.IsOk());
        
        auto& newGraph = *result.GetValue();
        
        // Should be empty
        EXPECT_EQ(newGraph.GetParentChildCount(), 0u);
        EXPECT_EQ(newGraph.GetParentCount(), 0u);
        EXPECT_EQ(newGraph.GetLinkedEntityCount(), 0u);
        
        // No relationships should exist
        EXPECT_FALSE(newGraph.HasParent(e2));
        EXPECT_FALSE(newGraph.HasChildren(e1));
        EXPECT_FALSE(newGraph.AreLinked(e2, e3));
    }
}