#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "Astra/Container/FlatMap.hpp"
#include "Astra/Entity/Entity.hpp"

// Split from FlatMapTest.cpp (2026-07-10 test-suite audit): the public-API
// CRUD tests and the SwissTable-internals tests (tombstones, rehashing,
// group-boundary handling, collision handling, randomized differential
// stress) are two separable themes. Fixture duplicated verbatim (< 20 lines)
// per the split policy; TEST bodies below are byte-identical to the ones
// removed from FlatMapTest.cpp.
class FlatMapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

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
