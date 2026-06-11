#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "Astra/Container/FlatMap.hpp"
#include "Astra/Entity/Entity.hpp"


class FlatMapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic insertion and retrieval
TEST_F(FlatMapTest, BasicInsertAndFind)
{
    Astra::FlatMap<int, std::string> map;
    
    // Insert some values
    auto [it1, inserted1] = map.Insert({1, "one"});
    EXPECT_TRUE(inserted1);
    EXPECT_EQ(it1->first, 1);
    EXPECT_EQ(it1->second, "one");
    
    auto [it2, inserted2] = map.Insert({2, "two"});
    EXPECT_TRUE(inserted2);
    
    auto [it3, inserted3] = map.Insert({3, "three"});
    EXPECT_TRUE(inserted3);
    
    // Try to insert duplicate
    auto [it4, inserted4] = map.Insert({1, "uno"});
    EXPECT_FALSE(inserted4);
    EXPECT_EQ(it4->second, "one"); // Original value unchanged
    
    // Find values
    auto found1 = map.Find(1);
    EXPECT_NE(found1, map.end());
    EXPECT_EQ(found1->second, "one");
    
    auto found2 = map.Find(2);
    EXPECT_NE(found2, map.end());
    EXPECT_EQ(found2->second, "two");
    
    auto notFound = map.Find(99);
    EXPECT_EQ(notFound, map.end());
    
    // Check size
    EXPECT_EQ(map.Size(), 3u);
    EXPECT_FALSE(map.Empty());
}

// Test operator[]
TEST_F(FlatMapTest, SubscriptOperator)
{
    Astra::FlatMap<std::string, int> map;
    
    // Insert via operator[]
    map["apple"] = 5;
    map["banana"] = 3;
    map["cherry"] = 7;
    
    EXPECT_EQ(map["apple"], 5);
    EXPECT_EQ(map["banana"], 3);
    EXPECT_EQ(map["cherry"], 7);
    
    // Modify via operator[]
    map["apple"] = 10;
    EXPECT_EQ(map["apple"], 10);
    
    // Access non-existent key creates default value
    EXPECT_EQ(map["date"], 0);
    EXPECT_EQ(map.Size(), 4u); // New entry created
}

// Test emplace
TEST_F(FlatMapTest, Emplace)
{
    Astra::FlatMap<int, std::string> map;
    
    auto [it1, inserted1] = map.Emplace(1, "first");
    EXPECT_TRUE(inserted1);
    EXPECT_EQ(it1->second, "first");
    
    // Emplace with construction
    auto [it2, inserted2] = map.Emplace(2, 6, 'x'); // Creates string(6, 'x')
    EXPECT_TRUE(inserted2);
    EXPECT_EQ(it2->second, "xxxxxx");
    
    // Emplace existing key
    auto [it3, inserted3] = map.Emplace(1, "changed");
    EXPECT_FALSE(inserted3);
    EXPECT_EQ(it3->second, "first"); // Original unchanged
}

// Test erase operations
TEST_F(FlatMapTest, EraseOperations)
{
    Astra::FlatMap<int, std::string> map;
    
    // Insert elements
    for (int i = 0; i < 10; ++i)
    {
        map[i] = "value_" + std::to_string(i);
    }
    
    EXPECT_EQ(map.Size(), 10u);
    
    // Erase by key
    size_t erased = map.Erase(5);
    EXPECT_EQ(erased, 1u);
    EXPECT_EQ(map.Size(), 9u);
    EXPECT_EQ(map.Find(5), map.end());
    
    // Erase non-existent key
    erased = map.Erase(99);
    EXPECT_EQ(erased, 0u);
    EXPECT_EQ(map.Size(), 9u);
    
    // Erase by iterator
    auto it = map.Find(3);
    EXPECT_NE(it, map.end());
    map.Erase(it);
    EXPECT_EQ(map.Size(), 8u);
    EXPECT_EQ(map.Find(3), map.end());
    
    // Clear all
    map.Clear();
    EXPECT_EQ(map.Size(), 0u);
    EXPECT_TRUE(map.Empty());
}

// Test iteration
TEST_F(FlatMapTest, Iteration)
{
    Astra::FlatMap<int, int> map;
    std::unordered_map<int, int> reference;
    
    // Insert elements
    for (int i = 0; i < 20; ++i)
    {
        int key = i * 7 % 23; // Pseudo-random keys
        int value = i * i;
        map[key] = value;
        reference[key] = value;
    }
    
    // Verify all elements via iteration
    for (const auto& [key, value] : map)
    {
        auto ref_it = reference.find(key);
        EXPECT_NE(ref_it, reference.end());
        EXPECT_EQ(value, ref_it->second);
    }
    
    // Count elements
    size_t count = 0;
    for (auto it = map.begin(); it != map.end(); ++it)
    {
        ++count;
    }
    EXPECT_EQ(count, map.Size());
    EXPECT_EQ(count, reference.size());
}

// Test copy constructor and assignment
TEST_F(FlatMapTest, CopyOperations)
{
    Astra::FlatMap<int, std::string> original;
    original[1] = "one";
    original[2] = "two";
    original[3] = "three";
    
    // Copy constructor
    Astra::FlatMap<int, std::string> copy1(original);
    EXPECT_EQ(copy1.Size(), original.Size());
    EXPECT_EQ(copy1[1], "one");
    EXPECT_EQ(copy1[2], "two");
    EXPECT_EQ(copy1[3], "three");
    
    // Copy assignment
    Astra::FlatMap<int, std::string> copy2;
    copy2[99] = "temp";
    copy2 = original;
    EXPECT_EQ(copy2.Size(), original.Size());
    EXPECT_EQ(copy2[1], "one");
    EXPECT_EQ(copy2.Find(99), copy2.end());
    
    // Modifying copy shouldn't affect original
    copy1[1] = "modified";
    EXPECT_EQ(original[1], "one");
    EXPECT_EQ(copy1[1], "modified");
}

// Test move constructor and assignment
TEST_F(FlatMapTest, MoveOperations)
{
    Astra::FlatMap<int, std::string> original;
    original[1] = "one";
    original[2] = "two";
    original[3] = "three";
    
    // Move constructor
    Astra::FlatMap<int, std::string> moved1(std::move(original));
    EXPECT_EQ(moved1.Size(), 3u);
    EXPECT_EQ(moved1[1], "one");
    EXPECT_EQ(original.Size(), 0u); // Original should be empty
    
    // Move assignment
    Astra::FlatMap<int, std::string> moved2;
    moved2 = std::move(moved1);
    EXPECT_EQ(moved2.Size(), 3u);
    EXPECT_EQ(moved2[2], "two");
    EXPECT_EQ(moved1.Size(), 0u);
}

// Test reserve and capacity
TEST_F(FlatMapTest, ReserveAndCapacity)
{
    Astra::FlatMap<int, int> map;
    
    // Initial capacity
    EXPECT_EQ(map.Capacity(), 0u);
    
    // Reserve space
    map.Reserve(100);
    EXPECT_GE(map.Capacity(), 100u);
    
    size_t cap_after_reserve = map.Capacity();
    
    // Insert elements - shouldn't trigger reallocation
    for (int i = 0; i < 50; ++i)
    {
        map[i] = i * 2;
    }
    
    EXPECT_EQ(map.Capacity(), cap_after_reserve);
    EXPECT_EQ(map.Size(), 50u);
}

// Test with custom types
TEST_F(FlatMapTest, CustomTypes)
{
    struct Point
    {
        int x, y;
        bool operator==(const Point& other) const
        {
            return x == other.x && y == other.y;
        }
    };
    
    struct PointHash
    {
        size_t operator()(const Point& p) const
        {
            return std::hash<int>()(p.x) ^ (std::hash<int>()(p.y) << 1);
        }
    };
    
    Astra::FlatMap<Point, std::string, PointHash> map;
    
    Point p1{0, 0};
    Point p2{1, 0};
    Point p3{0, 1};
    Point p4{1, 1};
    
    map[p1] = "origin";
    map[p2] = "right";
    map[p3] = "up";
    
    EXPECT_EQ(map[p1], "origin");
    EXPECT_EQ(map[p2], "right");
    EXPECT_EQ(map[p3], "up");
    
    auto found = map.Find(p4);
    EXPECT_EQ(found, map.end());
}

// Test large scale operations
TEST_F(FlatMapTest, LargeScaleOperations)
{
    Astra::FlatMap<int, int> map;
    const int N = 10000;
    
    // Insert many elements
    for (int i = 0; i < N; ++i)
    {
        map[i] = i * i;
    }
    
    EXPECT_EQ(map.Size(), static_cast<size_t>(N));
    
    // Verify all elements
    for (int i = 0; i < N; ++i)
    {
        auto it = map.Find(i);
        EXPECT_NE(it, map.end());
        EXPECT_EQ(it->second, i * i);
    }
    
    // Erase half
    for (int i = 0; i < N; i += 2)
    {
        map.Erase(i);
    }
    
    EXPECT_EQ(map.Size(), static_cast<size_t>(N / 2));
    
    // Verify remaining
    for (int i = 0; i < N; ++i)
    {
        auto it = map.Find(i);
        if (i % 2 == 0)
        {
            EXPECT_EQ(it, map.end());
        }
        else
        {
            EXPECT_NE(it, map.end());
            EXPECT_EQ(it->second, i * i);
        }
    }
}

// Test swap
TEST_F(FlatMapTest, Swap)
{
    Astra::FlatMap<int, std::string> map1;
    Astra::FlatMap<int, std::string> map2;
    
    map1[1] = "one";
    map1[2] = "two";
    
    map2[10] = "ten";
    map2[20] = "twenty";
    map2[30] = "thirty";
    
    map1.Swap(map2);
    
    EXPECT_EQ(map1.Size(), 3u);
    EXPECT_EQ(map1[10], "ten");
    EXPECT_EQ(map1[20], "twenty");
    EXPECT_EQ(map1[30], "thirty");
    
    EXPECT_EQ(map2.Size(), 2u);
    EXPECT_EQ(map2[1], "one");
    EXPECT_EQ(map2[2], "two");
}

// Test Contains method
TEST_F(FlatMapTest, Contains)
{
    Astra::FlatMap<std::string, int> map;
    map["apple"] = 1;
    map["banana"] = 2;
    
    EXPECT_TRUE(map.Contains("apple"));
    EXPECT_TRUE(map.Contains("banana"));
    EXPECT_FALSE(map.Contains("cherry"));
    
    map.Erase("apple");
    EXPECT_FALSE(map.Contains("apple"));
}

// Test TryGet methods
TEST_F(FlatMapTest, TryGet)
{
    Astra::FlatMap<int, std::string> map;
    map[1] = "one";
    map[2] = "two";
    
    // Non-const TryGet
    auto* value = map.TryGet(1);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "one");
    
    // Modify through pointer
    *value = "modified";
    EXPECT_EQ(map[1], "modified");
    
    // TryGet non-existent key
    auto* notFound = map.TryGet(99);
    EXPECT_EQ(notFound, nullptr);
    
    // Const TryGet
    const Astra::FlatMap<int, std::string>& constMap = map;
    const auto* constValue = constMap.TryGet(2);
    ASSERT_NE(constValue, nullptr);
    EXPECT_EQ(*constValue, "two");
}

// Test with Astra::Entity type (uses specialized hash)
TEST_F(FlatMapTest, EntityKeyType)
{
    Astra::FlatMap<Astra::Entity, std::string> map;
    
    Astra::Entity e1(1, 0);
    Astra::Entity e2(2, 0);
    Astra::Entity e3(3, 1);
    
    map[e1] = "entity1";
    map[e2] = "entity2";
    map[e3] = "entity3";
    
    EXPECT_EQ(map[e1], "entity1");
    EXPECT_EQ(map[e2], "entity2");
    EXPECT_EQ(map[e3], "entity3");
    
    // Test with invalid entity
    Astra::Entity invalid(Astra::Entity::Invalid());
    map[invalid] = "invalid";
    EXPECT_EQ(map[invalid], "invalid");
    
    EXPECT_EQ(map.Size(), 4u);
}

// Test tombstone handling (deletion markers)
TEST_F(FlatMapTest, TombstoneHandling)
{
    Astra::FlatMap<int, int> map;
    
    // Fill map
    for (int i = 0; i < 100; ++i)
    {
        map[i] = i;
    }
    
    // Erase many elements to create tombstones
    for (int i = 0; i < 100; i += 2)
    {
        map.Erase(i);
    }
    
    EXPECT_EQ(map.Size(), 50u);
    
    // Insert new elements - should handle tombstones correctly
    for (int i = 100; i < 150; ++i)
    {
        map[i] = i;
    }
    
    EXPECT_EQ(map.Size(), 100u);
    
    // Verify all expected elements exist
    for (int i = 0; i < 150; ++i)
    {
        if (i < 100 && i % 2 == 0)
        {
            EXPECT_EQ(map.Find(i), map.end());
        }
        else
        {
            auto it = map.Find(i);
            EXPECT_NE(it, map.end());
            EXPECT_EQ(it->second, i);
        }
    }
}

// Test rehashing behavior
TEST_F(FlatMapTest, RehashingBehavior)
{
    Astra::FlatMap<int, int> map;
    
    // Track capacity changes
    std::vector<size_t> capacityChanges;
    capacityChanges.push_back(map.Capacity());
    
    // Insert elements and track when rehashing occurs
    for (int i = 0; i < 1000; ++i)
    {
        map[i] = i;
        
        if (map.Capacity() != capacityChanges.back())
        {
            capacityChanges.push_back(map.Capacity());
        }
    }
    
    // Verify capacity grows as expected (power of 2)
    for (size_t i = 1; i < capacityChanges.size(); ++i)
    {
        EXPECT_GT(capacityChanges[i], capacityChanges[i-1]);
        // Check if capacity is power of 2
        size_t cap = capacityChanges[i];
        EXPECT_EQ(cap & (cap - 1), 0u) << "Capacity " << cap << " is not a power of 2";
    }
    
    // Verify all elements survived rehashing
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_EQ(map[i], i);
    }
}

// Test iterator validity after operations
TEST_F(FlatMapTest, IteratorValidity)
{
    Astra::FlatMap<int, std::string> map;
    
    // Insert initial elements
    for (int i = 0; i < 10; ++i)
    {
        map[i] = "value_" + std::to_string(i);
    }
    
    // Get iterator to element 5
    auto it = map.Find(5);
    ASSERT_NE(it, map.end());
    EXPECT_EQ(it->first, 5);
    
    // Erase different element - iterator should remain valid
    map.Erase(3);
    EXPECT_EQ(it->first, 5);
    EXPECT_EQ(it->second, "value_5");
    
    // Note: After rehashing, iterators become invalid
    // This is expected behavior for hash maps
}

// Test const correctness
TEST_F(FlatMapTest, ConstCorrectness)
{
    Astra::FlatMap<int, std::string> map;
    map[1] = "one";
    map[2] = "two";
    
    const Astra::FlatMap<int, std::string>& constMap = map;
    
    // Const find
    auto it = constMap.Find(1);
    EXPECT_NE(it, constMap.end());
    EXPECT_EQ(it->second, "one");
    
    // Const iteration
    size_t count = 0;
    for (const auto& [key, value] : constMap)
    {
        (void)key; (void)value;
        ++count;
    }
    EXPECT_EQ(count, 2u);
    
    // Const methods
    EXPECT_EQ(constMap.Size(), 2u);
    EXPECT_FALSE(constMap.Empty());
    EXPECT_TRUE(constMap.Contains(1));
}

// Test with string keys
TEST_F(FlatMapTest, StringKeys)
{
    Astra::FlatMap<std::string, int> map;
    
    // Insert with string literals
    map["hello"] = 1;
    map["world"] = 2;
    
    // Insert with string objects
    std::string key1 = "foo";
    std::string key2 = "bar";
    map[key1] = 3;
    map[key2] = 4;
    
    // Find with different string types
    EXPECT_EQ(map.Find("hello")->second, 1);
    EXPECT_EQ(map.Find(key1)->second, 3);
    
    // Contains with string view-like usage
    EXPECT_TRUE(map.Contains("world"));
    EXPECT_TRUE(map.Contains(key2));
}

// Test empty map operations
TEST_F(FlatMapTest, EmptyMapOperations)
{
    Astra::FlatMap<int, int> map;
    
    EXPECT_TRUE(map.Empty());
    EXPECT_EQ(map.Size(), 0u);
    EXPECT_EQ(map.begin(), map.end());
    
    // Operations on empty map
    EXPECT_EQ(map.Find(1), map.end());
    EXPECT_FALSE(map.Contains(1));
    EXPECT_EQ(map.Erase(1), 0u);
    
    map.Clear(); // Clear on empty map
    EXPECT_TRUE(map.Empty());
}

// Test collision handling
TEST_F(FlatMapTest, CollisionHandling)
{
    // Custom hash that causes collisions
    struct BadHash
    {
        size_t operator()(int key) const
        {
            return key % 10; // Many collisions
        }
    };
    
    Astra::FlatMap<int, int, BadHash> map;
    
    // Insert elements that will collide
    for (int i = 0; i < 100; ++i)
    {
        map[i] = i * i;
    }
    
    // Verify all elements are correctly stored despite collisions
    for (int i = 0; i < 100; ++i)
    {
        auto it = map.Find(i);
        ASSERT_NE(it, map.end());
        EXPECT_EQ(it->second, i * i);
    }
    
    EXPECT_EQ(map.Size(), 100u);
}

// Test with move-only values
TEST_F(FlatMapTest, MoveOnlyValues)
{
    Astra::FlatMap<int, std::unique_ptr<int>> map;
    
    // Insert move-only values
    map[1] = std::make_unique<int>(100);
    map[2] = std::make_unique<int>(200);
    
    EXPECT_EQ(*map[1], 100);
    EXPECT_EQ(*map[2], 200);
    
    // Move construct
    Astra::FlatMap<int, std::unique_ptr<int>> map2(std::move(map));
    EXPECT_EQ(*map2[1], 100);
    EXPECT_EQ(*map2[2], 200);
}

// Test group-based operations (SwissTable feature)
TEST_F(FlatMapTest, GroupBasedOperations)
{
    Astra::FlatMap<int, int> map;
    
    // Fill multiple groups (GROUP_SIZE = 16)
    for (int i = 0; i < 64; ++i) // 4 groups worth
    {
        map[i] = i;
    }
    
    EXPECT_EQ(map.Size(), 64u);
    
    // Operations should work correctly across group boundaries
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_TRUE(map.Contains(i));
        EXPECT_EQ(map[i], i);
    }
    
    // Erase across groups
    for (int i = 15; i < 48; i += 16) // Erase from different groups
    {
        map.Erase(i);
    }
    
    for (int i = 15; i < 48; i += 16)
    {
        EXPECT_FALSE(map.Contains(i));
    }
}

// Test iterator increment across groups
TEST_F(FlatMapTest, IteratorAcrossGroups)
{
    Astra::FlatMap<int, int> map;
    
    // Insert sparse elements across multiple groups
    for (int i = 0; i < 100; i += 5)
    {
        map[i] = i;
    }
    
    // Iterate and collect all keys
    std::set<int> keys;
    for (const auto& [key, value] : map)
    {
        keys.insert(key);
        EXPECT_EQ(value, key);
    }
    
    // Verify we got all keys
    EXPECT_EQ(keys.size(), 20u);
    for (int i = 0; i < 100; i += 5)
    {
        EXPECT_TRUE(keys.count(i));
    }
}

// Test random operations for stress testing
TEST_F(FlatMapTest, RandomOperations)
{
    Astra::FlatMap<int, int> map;
    std::unordered_map<int, int> reference;
    
    std::mt19937 rng(42); // Fixed seed for reproducibility
    std::uniform_int_distribution<int> keyDist(0, 999);
    std::uniform_int_distribution<int> opDist(0, 3);
    
    for (int i = 0; i < 10000; ++i)
    {
        int key = keyDist(rng);
        int op = opDist(rng);
        
        switch (op)
        {
            case 0: // Insert
            {
                int value = rng();
                map[key] = value;
                reference[key] = value;
                break;
            }
            case 1: // Find
            {
                auto it1 = map.Find(key);
                auto it2 = reference.find(key);
                
                if (it2 == reference.end())
                {
                    EXPECT_EQ(it1, map.end());
                }
                else
                {
                    ASSERT_NE(it1, map.end());
                    EXPECT_EQ(it1->second, it2->second);
                }
                break;
            }
            case 2: // Erase
            {
                size_t erased1 = map.Erase(key);
                size_t erased2 = reference.erase(key);
                EXPECT_EQ(erased1, erased2);
                break;
            }
            case 3: // Contains
            {
                bool contains1 = map.Contains(key);
                bool contains2 = reference.count(key) > 0;
                EXPECT_EQ(contains1, contains2);
                break;
            }
        }
    }
    
    // Final verification
    EXPECT_EQ(map.Size(), reference.size());
    
    for (const auto& [key, value] : reference)
    {
        auto it = map.Find(key);
        ASSERT_NE(it, map.end());
        EXPECT_EQ(it->second, value);
    }
}

// Test capacity constructor
TEST_F(FlatMapTest, CapacityConstructor)
{
    Astra::FlatMap<int, int> map(100);
    
    EXPECT_GE(map.Capacity(), 100u);
    EXPECT_EQ(map.Size(), 0u);
    EXPECT_TRUE(map.Empty());
    
    // Should not need rehashing for first 87 elements (load factor 0.875)
    size_t initialCapacity = map.Capacity();
    for (int i = 0; i < 87; ++i)
    {
        map[i] = i;
    }
    EXPECT_EQ(map.Capacity(), initialCapacity);
}

// Test edge cases with capacity
TEST_F(FlatMapTest, CapacityEdgeCases)
{
    // Zero capacity - default constructor
    Astra::FlatMap<int, int> map1;
    EXPECT_EQ(map1.Capacity(), 0u);
    
    // Insert triggers allocation
    map1[1] = 1;
    EXPECT_GT(map1.Capacity(), 0u);
    EXPECT_EQ(map1[1], 1);
    
    // Large capacity request
    Astra::FlatMap<int, int> map2;
    map2.Reserve(10000);
    EXPECT_GE(map2.Capacity(), 10000u);
}

// Test with custom allocator
template<typename T>
struct TrackingAllocator : std::allocator<T>
{
    using std::allocator<T>::allocator;
    
    template<typename U>
    struct rebind
    {
        using other = TrackingAllocator<U>;
    };
};

TEST_F(FlatMapTest, CustomAllocator)
{
    using CustomMap = Astra::FlatMap<int, int, std::hash<int>, std::equal_to<int>, 
                              TrackingAllocator<std::pair<const int, int>>>;
    CustomMap map;
    
    map[1] = 100;
    map[2] = 200;
    
    EXPECT_EQ(map[1], 100);
    EXPECT_EQ(map[2], 200);
}

// Test heterogeneous lookup (if supported)
TEST_F(FlatMapTest, HeterogeneousLookup)
{
    Astra::FlatMap<std::string, int> map;
    map["hello"] = 1;
    map["world"] = 2;
    
    // These should work with string literals
    EXPECT_TRUE(map.Contains("hello"));
    EXPECT_EQ(map.Find("world")->second, 2);
    
    // And with string objects
    std::string key = "hello";
    EXPECT_TRUE(map.Contains(key));
    EXPECT_EQ(map.Find(key)->second, 1);
}