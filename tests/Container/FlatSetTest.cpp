#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#include "Astra/Container/FlatSet.hpp"
#include "Astra/Entity/Entity.hpp"


class FlatSetTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic insertion and lookup
TEST_F(FlatSetTest, BasicInsertAndContains)
{
    Astra::FlatSet<int> set;
    
    // Insert some values
    auto [it1, inserted1] = set.Insert(1);
    EXPECT_TRUE(inserted1);
    EXPECT_EQ(*it1, 1);
    
    auto [it2, inserted2] = set.Insert(2);
    EXPECT_TRUE(inserted2);
    
    auto [it3, inserted3] = set.Insert(3);
    EXPECT_TRUE(inserted3);
    
    // Try to insert duplicate
    auto [it4, inserted4] = set.Insert(1);
    EXPECT_FALSE(inserted4);
    EXPECT_EQ(*it4, 1);
    
    // Check contains
    EXPECT_TRUE(set.Contains(1));
    EXPECT_TRUE(set.Contains(2));
    EXPECT_TRUE(set.Contains(3));
    EXPECT_FALSE(set.Contains(99));
    
    // Check size
    EXPECT_EQ(set.Size(), 3u);
    EXPECT_FALSE(set.IsEmpty());
}

// Test find operations
TEST_F(FlatSetTest, FindOperations)
{
    Astra::FlatSet<std::string> set;
    
    set.Insert("apple");
    set.Insert("banana");
    set.Insert("cherry");
    
    // Find existing elements
    auto found1 = set.Find("apple");
    EXPECT_NE(found1, set.end());
    EXPECT_EQ(*found1, "apple");
    
    auto found2 = set.Find("banana");
    EXPECT_NE(found2, set.end());
    EXPECT_EQ(*found2, "banana");
    
    // Find non-existent element
    auto notFound = set.Find("date");
    EXPECT_EQ(notFound, set.end());
}

// Test emplace
TEST_F(FlatSetTest, Emplace)
{
    Astra::FlatSet<std::string> set;
    
    // Emplace new elements
    auto [it1, inserted1] = set.Emplace("first");
    EXPECT_TRUE(inserted1);
    EXPECT_EQ(*it1, "first");
    
    // Emplace with construction
    auto [it2, inserted2] = set.Emplace(6, 'x'); // Creates string(6, 'x')
    EXPECT_TRUE(inserted2);
    EXPECT_EQ(*it2, "xxxxxx");
    
    // Emplace existing element
    auto [it3, inserted3] = set.Emplace("first");
    EXPECT_FALSE(inserted3);
    EXPECT_EQ(*it3, "first");
}

// Test erase operations
TEST_F(FlatSetTest, EraseOperations)
{
    Astra::FlatSet<int> set;
    
    // Insert elements
    for (int i = 0; i < 10; ++i)
    {
        set.Insert(i);
    }
    
    EXPECT_EQ(set.Size(), 10u);
    
    // Erase by value
    size_t erased = set.Erase(5);
    EXPECT_EQ(erased, 1u);
    EXPECT_EQ(set.Size(), 9u);
    EXPECT_FALSE(set.Contains(5));
    
    // Erase non-existent value
    erased = set.Erase(99);
    EXPECT_EQ(erased, 0u);
    EXPECT_EQ(set.Size(), 9u);
    
    // Erase by iterator
    auto it = set.Find(3);
    EXPECT_NE(it, set.end());
    set.Erase(it);
    EXPECT_EQ(set.Size(), 8u);
    EXPECT_FALSE(set.Contains(3));
    
    // Clear all
    set.Clear();
    EXPECT_EQ(set.Size(), 0u);
    EXPECT_TRUE(set.IsEmpty());
}

// Test iteration
TEST_F(FlatSetTest, Iteration)
{
    Astra::FlatSet<int> set;
    std::unordered_set<int> reference;
    
    // Insert elements
    for (int i = 0; i < 20; ++i)
    {
        int value = i * 7 % 23; // Pseudo-random values
        set.Insert(value);
        reference.insert(value);
    }
    
    // Verify all elements via iteration
    for (const auto& value : set)
    {
        EXPECT_TRUE(reference.count(value) > 0);
    }
    
    // Count elements
    size_t count = 0;
    for (auto it = set.begin(); it != set.end(); ++it)
    {
        ++count;
    }
    EXPECT_EQ(count, set.Size());
    EXPECT_EQ(count, reference.size());
}

// Test copy constructor and assignment
TEST_F(FlatSetTest, CopyOperations)
{
    Astra::FlatSet<int> original;
    original.Insert(1);
    original.Insert(2);
    original.Insert(3);
    
    // Copy constructor
    Astra::FlatSet<int> copy1(original);
    EXPECT_EQ(copy1.Size(), original.Size());
    EXPECT_TRUE(copy1.Contains(1));
    EXPECT_TRUE(copy1.Contains(2));
    EXPECT_TRUE(copy1.Contains(3));
    
    // Copy assignment
    Astra::FlatSet<int> copy2;
    copy2.Insert(99);
    copy2 = original;
    EXPECT_EQ(copy2.Size(), original.Size());
    EXPECT_TRUE(copy2.Contains(1));
    EXPECT_FALSE(copy2.Contains(99));
    
    // Modifying copy shouldn't affect original
    copy1.Insert(4);
    EXPECT_FALSE(original.Contains(4));
    EXPECT_TRUE(copy1.Contains(4));
}

// Test move constructor and assignment
TEST_F(FlatSetTest, MoveOperations)
{
    Astra::FlatSet<int> original;
    original.Insert(1);
    original.Insert(2);
    original.Insert(3);
    
    // Move constructor
    Astra::FlatSet<int> moved1(std::move(original));
    EXPECT_EQ(moved1.Size(), 3u);
    EXPECT_TRUE(moved1.Contains(1));
    EXPECT_EQ(original.Size(), 0u); // Original should be empty
    
    // Move assignment
    Astra::FlatSet<int> moved2;
    moved2 = std::move(moved1);
    EXPECT_EQ(moved2.Size(), 3u);
    EXPECT_TRUE(moved2.Contains(2));
    EXPECT_EQ(moved1.Size(), 0u);
}

// Test reserve and capacity
TEST_F(FlatSetTest, ReserveAndCapacity)
{
    Astra::FlatSet<int> set;
    
    // Initial capacity
    EXPECT_EQ(set.Capacity(), 0u);
    
    // Reserve space
    set.Reserve(100);
    EXPECT_GE(set.Capacity(), 100u);
    
    size_t cap_after_reserve = set.Capacity();
    
    // Insert elements - shouldn't trigger reallocation
    for (int i = 0; i < 50; ++i)
    {
        set.Insert(i);
    }
    
    EXPECT_EQ(set.Capacity(), cap_after_reserve);
    EXPECT_EQ(set.Size(), 50u);
}

// Test with Entity type (specific to Astra)
TEST_F(FlatSetTest, EntitySet)
{
    using namespace Astra;
    FlatSet<Entity> entities;
    
    // Create some test entities
    Entity e1{1, 0};
    Entity e2{2, 0};
    Entity e3{3, 0};
    Entity e4{1, 1}; // Same index as e1 but different version
    
    // Insert entities
    EXPECT_TRUE(entities.Insert(e1).second);
    EXPECT_TRUE(entities.Insert(e2).second);
    EXPECT_TRUE(entities.Insert(e3).second);
    EXPECT_TRUE(entities.Insert(e4).second); // Different entity despite same index
    
    // Check contains
    EXPECT_TRUE(entities.Contains(e1));
    EXPECT_TRUE(entities.Contains(e2));
    EXPECT_TRUE(entities.Contains(e3));
    EXPECT_TRUE(entities.Contains(e4));
    
    // Try to insert duplicate
    EXPECT_FALSE(entities.Insert(e1).second);
    
    EXPECT_EQ(entities.Size(), 4u);
    
    // Erase an entity
    entities.Erase(e2);
    EXPECT_FALSE(entities.Contains(e2));
    EXPECT_EQ(entities.Size(), 3u);
}

// Test large dataset performance characteristics
TEST_F(FlatSetTest, LargeDataset)
{
    Astra::FlatSet<int> set;
    const int N = 10000;
    
    // Reserve to avoid rehashing
    set.Reserve(N);
    
    // Insert many elements
    for (int i = 0; i < N; ++i)
    {
        auto [it, inserted] = set.Insert(i);
        EXPECT_TRUE(inserted);
    }
    
    EXPECT_EQ(set.Size(), static_cast<size_t>(N));
    
    // Verify all elements are present
    for (int i = 0; i < N; ++i)
    {
        EXPECT_TRUE(set.Contains(i));
    }
    
    // Remove every other element
    for (int i = 0; i < N; i += 2)
    {
        EXPECT_EQ(set.Erase(i), 1u);
    }
    
    EXPECT_EQ(set.Size(), static_cast<size_t>(N / 2));
    
    // Verify correct elements remain
    for (int i = 0; i < N; ++i)
    {
        if (i % 2 == 0)
        {
            EXPECT_FALSE(set.Contains(i));
        }
        else
        {
            EXPECT_TRUE(set.Contains(i));
        }
    }
}

// Test rehashing behavior
TEST_F(FlatSetTest, Rehashing)
{
    Astra::FlatSet<int> set;
    
    // Start with small set
    for (int i = 0; i < 10; ++i)
    {
        set.Insert(i);
    }
    
    size_t initial_capacity = set.Capacity();
    
    // Add more elements to trigger rehashing
    for (int i = 10; i < 100; ++i)
    {
        set.Insert(i);
    }
    
    // Capacity should have grown
    EXPECT_GT(set.Capacity(), initial_capacity);
    
    // All elements should still be present
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_TRUE(set.Contains(i));
    }
    
    EXPECT_EQ(set.Size(), 100u);
}

// Test with duplicates in batch insert
TEST_F(FlatSetTest, BatchInsertWithDuplicates)
{
    Astra::FlatSet<int> set;
    
    // Initial values
    set.Insert(1);
    set.Insert(2);
    set.Insert(3);
    
    // Try to insert multiple including duplicates
    std::vector<int> values = {2, 3, 4, 5, 3, 6, 1};
    
    size_t inserted = 0;
    for (int v : values)
    {
        if (set.Insert(v).second)
        {
            ++inserted;
        }
    }
    
    EXPECT_EQ(inserted, 3u); // Only 4, 5, 6 should be new
    EXPECT_EQ(set.Size(), 6u);
    
    // Verify correct elements
    for (int i = 1; i <= 6; ++i)
    {
        EXPECT_TRUE(set.Contains(i));
    }
}

// Test swap
TEST_F(FlatSetTest, Swap)
{
    Astra::FlatSet<int> set1;
    Astra::FlatSet<int> set2;
    
    set1.Insert(1);
    set1.Insert(2);
    set1.Insert(3);
    
    set2.Insert(10);
    set2.Insert(20);
    
    set1.Swap(set2);
    
    EXPECT_EQ(set1.Size(), 2u);
    EXPECT_TRUE(set1.Contains(10));
    EXPECT_TRUE(set1.Contains(20));
    
    EXPECT_EQ(set2.Size(), 3u);
    EXPECT_TRUE(set2.Contains(1));
    EXPECT_TRUE(set2.Contains(2));
    EXPECT_TRUE(set2.Contains(3));
}

// Test const correctness
TEST_F(FlatSetTest, ConstCorrectness)
{
    Astra::FlatSet<int> mutable_set;
    mutable_set.Insert(1);
    mutable_set.Insert(2);
    mutable_set.Insert(3);
    
    const Astra::FlatSet<int>& const_set = mutable_set;
    
    // These should compile with const set
    EXPECT_TRUE(const_set.Contains(1));
    EXPECT_EQ(const_set.Size(), 3u);
    EXPECT_FALSE(const_set.IsEmpty());
    
    // Const iteration
    size_t count = 0;
    for (const auto& value : const_set)
    {
        ++count;
        (void)value;
    }
    EXPECT_EQ(count, 3u);
    
    // Const find
    auto it = const_set.Find(2);
    EXPECT_NE(it, const_set.end());
    EXPECT_EQ(*it, 2);
}

// Test max load factor behavior
TEST_F(FlatSetTest, LoadFactor)
{
    Astra::FlatSet<int> set;
    
    // Reserve specific capacity
    set.Reserve(16);
    EXPECT_EQ(set.Capacity(), 16u);
    
    // Insert up to 87.5% capacity (14 elements for capacity 16)
    for (int i = 0; i < 14; ++i)
    {
        set.Insert(i);
    }
    
    // Should still be at same capacity
    EXPECT_EQ(set.Capacity(), 16u);
    
    // One more insert might trigger growth
    set.Insert(14);
    
    // Capacity might have grown (implementation dependent on exact load factor)
    EXPECT_GE(set.Capacity(), 15u);
}