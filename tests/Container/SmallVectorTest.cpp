#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <string>
#include <vector>
#include "Astra/Container/SmallVector.hpp"

class SmallVectorTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic construction and size
TEST_F(SmallVectorTest, BasicConstruction)
{
    // Default constructor
    Astra::SmallVector<int, 4> sv1;
    EXPECT_TRUE(sv1.empty());
    EXPECT_EQ(sv1.size(), 0u);
    EXPECT_GE(sv1.capacity(), 4u);
    
    // Size constructor
    Astra::SmallVector<int, 4> sv2(5);
    EXPECT_EQ(sv2.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(sv2[i], 0);
    }
    
    // Size and value constructor
    Astra::SmallVector<int, 4> sv3(3, 42);
    EXPECT_EQ(sv3.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(sv3[i], 42);
    }
    
    // Initializer list
    Astra::SmallVector<int, 4> sv4{1, 2, 3, 4, 5};
    EXPECT_EQ(sv4.size(), 5u);
    EXPECT_EQ(sv4[0], 1);
    EXPECT_EQ(sv4[1], 2);
    EXPECT_EQ(sv4[2], 3);
    EXPECT_EQ(sv4[3], 4);
    EXPECT_EQ(sv4[4], 5);
    
    // Range constructor
    std::vector<int> src{10, 20, 30};
    Astra::SmallVector<int, 4> sv5(src.begin(), src.end());
    EXPECT_EQ(sv5.size(), 3u);
    EXPECT_EQ(sv5[0], 10);
    EXPECT_EQ(sv5[1], 20);
    EXPECT_EQ(sv5[2], 30);
}

// Test small buffer optimization
TEST_F(SmallVectorTest, SmallBufferOptimization)
{
    Astra::SmallVector<int, 8> sv;
    
    // Should use inline storage for <= 8 elements
    for (int i = 0; i < 8; ++i)
    {
        sv.push_back(i);
    }
    EXPECT_EQ(sv.size(), 8u);
    EXPECT_EQ(sv.capacity(), 8u); // Should still be using inline storage
    
    // Adding 9th element should trigger heap allocation
    sv.push_back(8);
    EXPECT_EQ(sv.size(), 9u);
    EXPECT_GT(sv.capacity(), 8u); // Should have grown
    
    // Verify all elements still correct
    for (int i = 0; i < 9; ++i)
    {
        EXPECT_EQ(sv[i], i);
    }
}

// Test element access
TEST_F(SmallVectorTest, ElementAccess)
{
    Astra::SmallVector<int, 4> sv{10, 20, 30, 40, 50};
    
    // operator[]
    EXPECT_EQ(sv[0], 10);
    EXPECT_EQ(sv[4], 50);
    sv[2] = 35;
    EXPECT_EQ(sv[2], 35);
    
    // at()
    EXPECT_EQ(sv.at(1), 20);
    sv.at(3) = 45;
    EXPECT_EQ(sv.at(3), 45);
    
    // front/back
    EXPECT_EQ(sv.front(), 10);
    EXPECT_EQ(sv.back(), 50);
    sv.front() = 15;
    sv.back() = 55;
    EXPECT_EQ(sv.front(), 15);
    EXPECT_EQ(sv.back(), 55);
    
    // data()
    int* ptr = sv.data();
    EXPECT_EQ(ptr[0], 15);
    EXPECT_EQ(ptr[4], 55);
}

// Test push_back and emplace_back
TEST_F(SmallVectorTest, PushBackEmplaceBack)
{
    Astra::SmallVector<std::string, 2> sv;
    
    // push_back lvalue
    std::string s1 = "hello";
    sv.push_back(s1);
    EXPECT_EQ(sv.size(), 1u);
    EXPECT_EQ(sv[0], "hello");
    
    // push_back rvalue
    sv.push_back("world");
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv[1], "world");
    
    // emplace_back
    sv.emplace_back(5, 'x');  // Construct string with 5 'x's
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv[2], "xxxxx");
    
    // Trigger heap allocation
    sv.emplace_back("overflow");
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_GT(sv.capacity(), 2u);
}

// Test pop_back
TEST_F(SmallVectorTest, PopBack)
{
    Astra::SmallVector<int, 4> sv{1, 2, 3, 4, 5};
    
    sv.pop_back();
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_EQ(sv.back(), 4);
    
    sv.pop_back();
    sv.pop_back();
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv.back(), 2);
}

// Test insert operations
TEST_F(SmallVectorTest, InsertOperations)
{
    Astra::SmallVector<int, 4> sv{1, 2, 3};
    
    // Insert single element
    auto it = sv.insert(sv.begin() + 1, 10);
    EXPECT_EQ(*it, 10);
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[1], 10);
    EXPECT_EQ(sv[2], 2);
    EXPECT_EQ(sv[3], 3);
    
    // Insert multiple elements
    it = sv.insert(sv.begin() + 2, 2, 20);
    EXPECT_EQ(*it, 20);
    EXPECT_EQ(sv.size(), 6u);
    EXPECT_EQ(sv[2], 20);
    EXPECT_EQ(sv[3], 20);
    
    // Insert at end
    sv.insert(sv.end(), 30);
    EXPECT_EQ(sv.back(), 30);
    EXPECT_EQ(sv.size(), 7u);
}

// Test emplace
TEST_F(SmallVectorTest, Emplace)
{
    Astra::SmallVector<std::pair<int, std::string>, 2> sv;
    
    sv.emplace(sv.begin(), 1, "first");
    EXPECT_EQ(sv.size(), 1u);
    EXPECT_EQ(sv[0].first, 1);
    EXPECT_EQ(sv[0].second, "first");
    
    sv.emplace(sv.end(), 2, "second");
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv[1].second, "second");
    
    sv.emplace(sv.begin() + 1, 3, "middle");
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv[1].first, 3);
    EXPECT_EQ(sv[1].second, "middle");
}

// Test erase operations
TEST_F(SmallVectorTest, EraseOperations)
{
    Astra::SmallVector<int, 8> sv{1, 2, 3, 4, 5, 6, 7, 8};
    
    // Erase single element
    auto it = sv.erase(sv.begin() + 2);
    EXPECT_EQ(sv.size(), 7u);
    EXPECT_EQ(*it, 4);
    EXPECT_EQ(sv[2], 4);
    
    // Erase range
    it = sv.erase(sv.begin() + 1, sv.begin() + 4);
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_EQ(*it, 6);
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[1], 6);
    EXPECT_EQ(sv[2], 7);
    EXPECT_EQ(sv[3], 8);
    
    // Erase from end
    sv.erase(sv.end() - 1);
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv.back(), 7);
}

// Test resize
TEST_F(SmallVectorTest, Resize)
{
    Astra::SmallVector<int, 4> sv{1, 2, 3};
    
    // Resize larger (default construct)
    sv.resize(5);
    EXPECT_EQ(sv.size(), 5u);
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[1], 2);
    EXPECT_EQ(sv[2], 3);
    EXPECT_EQ(sv[3], 0);
    EXPECT_EQ(sv[4], 0);
    
    // Resize larger with value
    sv.resize(7, 99);
    EXPECT_EQ(sv.size(), 7u);
    EXPECT_EQ(sv[5], 99);
    EXPECT_EQ(sv[6], 99);
    
    // Resize smaller
    sv.resize(2);
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[1], 2);
}

// Test reserve and capacity
TEST_F(SmallVectorTest, ReserveAndCapacity)
{
    Astra::SmallVector<int, 4> sv;
    
    EXPECT_GE(sv.capacity(), 4u);
    
    // Reserve within small buffer
    sv.reserve(4);
    EXPECT_GE(sv.capacity(), 4u);
    
    // Reserve beyond small buffer
    sv.reserve(10);
    EXPECT_GE(sv.capacity(), 10u);
    
    // Add elements
    for (int i = 0; i < 8; ++i)
    {
        sv.push_back(i);
    }
    
    size_t cap = sv.capacity();
    sv.reserve(5); // Less than current size, should do nothing
    EXPECT_EQ(sv.capacity(), cap);
}

// Test shrink_to_fit
TEST_F(SmallVectorTest, ShrinkToFit)
{
    Astra::SmallVector<int, 4> sv;
    
    // Add many elements to trigger heap allocation
    for (int i = 0; i < 10; ++i)
    {
        sv.push_back(i);
    }
    EXPECT_GT(sv.capacity(), 10u);
    
    // Remove elements
    sv.resize(3);
    sv.shrink_to_fit();
    
    // Should be back in small buffer
    EXPECT_EQ(sv.capacity(), 4u);
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv[0], 0);
    EXPECT_EQ(sv[1], 1);
    EXPECT_EQ(sv[2], 2);
}

// Test clear
TEST_F(SmallVectorTest, Clear)
{
    Astra::SmallVector<std::string, 2> sv{"hello", "world", "test"};
    
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_FALSE(sv.empty());
    
    sv.clear();
    EXPECT_EQ(sv.size(), 0u);
    EXPECT_TRUE(sv.empty());
    
    // Capacity should be unchanged
    EXPECT_GT(sv.capacity(), 0u);
    
    // Can add elements again
    sv.push_back("new");
    EXPECT_EQ(sv.size(), 1u);
    EXPECT_EQ(sv[0], "new");
}

// Test iterators
TEST_F(SmallVectorTest, Iterators)
{
    Astra::SmallVector<int, 4> sv{1, 2, 3, 4, 5};
    
    // Forward iteration
    int sum = 0;
    for (auto it = sv.begin(); it != sv.end(); ++it)
    {
        sum += *it;
    }
    EXPECT_EQ(sum, 15);
    
    // Const iteration
    const Astra::SmallVector<int, 4>& csv = sv;
    sum = 0;
    for (auto it = csv.cbegin(); it != csv.cend(); ++it)
    {
        sum += *it;
    }
    EXPECT_EQ(sum, 15);
    
    // Reverse iteration
    std::vector<int> reversed;
    for (auto it = sv.rbegin(); it != sv.rend(); ++it)
    {
        reversed.push_back(*it);
    }
    EXPECT_EQ(reversed.size(), 5u);
    EXPECT_EQ(reversed[0], 5);
    EXPECT_EQ(reversed[1], 4);
    EXPECT_EQ(reversed[2], 3);
    EXPECT_EQ(reversed[3], 2);
    EXPECT_EQ(reversed[4], 1);
    
    // Range-based for
    sum = 0;
    for (int val : sv)
    {
        sum += val;
    }
    EXPECT_EQ(sum, 15);
}

// Test copy operations
TEST_F(SmallVectorTest, CopyOperations)
{
    Astra::SmallVector<int, 4> sv1{1, 2, 3, 4, 5};
    
    // Copy constructor
    Astra::SmallVector<int, 4> sv2(sv1);
    EXPECT_EQ(sv2.size(), sv1.size());
    for (size_t i = 0; i < sv1.size(); ++i)
    {
        EXPECT_EQ(sv2[i], sv1[i]);
    }
    
    // Copy assignment
    Astra::SmallVector<int, 4> sv3;
    sv3 = sv1;
    EXPECT_EQ(sv3.size(), sv1.size());
    for (size_t i = 0; i < sv1.size(); ++i)
    {
        EXPECT_EQ(sv3[i], sv1[i]);
    }
    
    // Modify copy doesn't affect original
    sv2[0] = 100;
    EXPECT_EQ(sv1[0], 1);
    EXPECT_EQ(sv2[0], 100);
}

// Test move operations
TEST_F(SmallVectorTest, MoveOperations)
{
    // Move from small buffer
    {
        Astra::SmallVector<std::string, 4> sv1{"a", "b", "c"};
        Astra::SmallVector<std::string, 4> sv2(std::move(sv1));
        
        EXPECT_EQ(sv2.size(), 3u);
        EXPECT_EQ(sv2[0], "a");
        EXPECT_EQ(sv2[1], "b");
        EXPECT_EQ(sv2[2], "c");
        EXPECT_EQ(sv1.size(), 0u);
    }
    
    // Move from heap
    {
        Astra::SmallVector<std::string, 2> sv1{"a", "b", "c", "d", "e"};
        Astra::SmallVector<std::string, 2> sv2(std::move(sv1));
        
        EXPECT_EQ(sv2.size(), 5u);
        EXPECT_EQ(sv2[0], "a");
        EXPECT_EQ(sv2[4], "e");
        EXPECT_EQ(sv1.size(), 0u);
    }
    
    // Move assignment
    {
        Astra::SmallVector<int, 4> sv1{1, 2, 3, 4, 5};
        Astra::SmallVector<int, 4> sv2{10, 20};
        
        sv2 = std::move(sv1);
        EXPECT_EQ(sv2.size(), 5u);
        EXPECT_EQ(sv2[0], 1);
        EXPECT_EQ(sv2[4], 5);
        EXPECT_EQ(sv1.size(), 0u);
    }
}

// Test swap
TEST_F(SmallVectorTest, Swap)
{
    // Both small
    {
        Astra::SmallVector<int, 8> sv1{1, 2, 3};
        Astra::SmallVector<int, 8> sv2{10, 20, 30, 40};
        
        sv1.swap(sv2);
        
        EXPECT_EQ(sv1.size(), 4u);
        EXPECT_EQ(sv1[0], 10);
        EXPECT_EQ(sv1[3], 40);
        
        EXPECT_EQ(sv2.size(), 3u);
        EXPECT_EQ(sv2[0], 1);
        EXPECT_EQ(sv2[2], 3);
    }
    
    // Both heap
    {
        Astra::SmallVector<int, 2> sv1{1, 2, 3, 4, 5};
        Astra::SmallVector<int, 2> sv2{10, 20, 30};
        
        sv1.swap(sv2);
        
        EXPECT_EQ(sv1.size(), 3u);
        EXPECT_EQ(sv1[0], 10);
        
        EXPECT_EQ(sv2.size(), 5u);
        EXPECT_EQ(sv2[0], 1);
    }
    
    // One small, one heap
    {
        Astra::SmallVector<int, 4> sv1{1, 2};
        Astra::SmallVector<int, 4> sv2{10, 20, 30, 40, 50};
        
        sv1.swap(sv2);
        
        EXPECT_EQ(sv1.size(), 5u);
        EXPECT_EQ(sv1[0], 10);
        EXPECT_EQ(sv1[4], 50);
        
        EXPECT_EQ(sv2.size(), 2u);
        EXPECT_EQ(sv2[0], 1);
        EXPECT_EQ(sv2[1], 2);
    }
}

// Test comparison operators
TEST_F(SmallVectorTest, ComparisonOperators)
{
    Astra::SmallVector<int, 4> sv1{1, 2, 3};
    Astra::SmallVector<int, 4> sv2{1, 2, 3};
    Astra::SmallVector<int, 4> sv3{1, 2, 4};
    Astra::SmallVector<int, 4> sv4{1, 2};
    
    // Equality
    EXPECT_TRUE(sv1 == sv2);
    EXPECT_FALSE(sv1 == sv3);
    EXPECT_FALSE(sv1 == sv4);
    
    // Inequality
    EXPECT_FALSE(sv1 != sv2);
    EXPECT_TRUE(sv1 != sv3);
    
    // Less than
    EXPECT_FALSE(sv1 < sv2);
    EXPECT_TRUE(sv1 < sv3);
    EXPECT_FALSE(sv1 < sv4);
    
    // Less than or equal
    EXPECT_TRUE(sv1 <= sv2);
    EXPECT_TRUE(sv1 <= sv3);
    
    // Greater than
    EXPECT_FALSE(sv1 > sv2);
    EXPECT_FALSE(sv1 > sv3);
    EXPECT_TRUE(sv1 > sv4);
    
    // Greater than or equal
    EXPECT_TRUE(sv1 >= sv2);
    EXPECT_FALSE(sv1 >= sv3);
}

// Test assign operations
TEST_F(SmallVectorTest, AssignOperations)
{
    Astra::SmallVector<int, 4> sv;
    
    // Assign count and value
    sv.assign(5, 42);
    EXPECT_EQ(sv.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(sv[i], 42);
    }
    
    // Assign from range
    std::vector<int> src{10, 20, 30};
    sv.assign(src.begin(), src.end());
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv[0], 10);
    EXPECT_EQ(sv[1], 20);
    EXPECT_EQ(sv[2], 30);
    
    // Assign from initializer list
    sv.assign({1, 2, 3, 4});
    EXPECT_EQ(sv.size(), 4u);
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[3], 4);
}

// Test with non-trivial types
TEST_F(SmallVectorTest, NonTrivialTypes)
{
    Astra::SmallVector<std::string, 2> sv;
    
    sv.push_back("hello");
    sv.push_back("world");
    sv.push_back("test");  // Triggers heap allocation
    
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sv[0], "hello");
    EXPECT_EQ(sv[1], "world");
    EXPECT_EQ(sv[2], "test");
    
    sv.erase(sv.begin() + 1);
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv[0], "hello");
    EXPECT_EQ(sv[1], "test");
    
    sv.clear();
    EXPECT_TRUE(sv.empty());
}

// Test with different small buffer sizes
TEST_F(SmallVectorTest, DifferentBufferSizes)
{
    // Very small buffer
    {
        Astra::SmallVector<int, 1> sv{1, 2, 3};
        EXPECT_EQ(sv.size(), 3u);
        EXPECT_GT(sv.capacity(), 1u);
    }
    
    // Medium buffer
    {
        Astra::SmallVector<int, 16> sv;
        for (int i = 0; i < 16; ++i)
        {
            sv.push_back(i);
        }
        EXPECT_EQ(sv.size(), 16u);
        EXPECT_EQ(sv.capacity(), 16u); // Should still be in small buffer
    }
    
    // Large buffer
    {
        Astra::SmallVector<int, 64> sv;
        for (int i = 0; i < 32; ++i)
        {
            sv.push_back(i);
        }
        EXPECT_EQ(sv.size(), 32u);
        EXPECT_EQ(sv.capacity(), 64u); // Should still be in small buffer
    }
}

// Test algorithm compatibility
TEST_F(SmallVectorTest, AlgorithmCompatibility)
{
    Astra::SmallVector<int, 4> sv{3, 1, 4, 1, 5, 9, 2, 6};
    
    // Sort
    std::sort(sv.begin(), sv.end());
    EXPECT_EQ(sv[0], 1);
    EXPECT_EQ(sv[1], 1);
    EXPECT_EQ(sv.back(), 9);
    
    // Find
    auto it = std::find(sv.begin(), sv.end(), 5);
    EXPECT_NE(it, sv.end());
    EXPECT_EQ(*it, 5);
    
    // Count
    auto count = std::count(sv.begin(), sv.end(), 1);
    EXPECT_EQ(count, 2);
    
    // Accumulate
    int sum = std::accumulate(sv.begin(), sv.end(), 0);
    EXPECT_EQ(sum, 31);
    
    // Reverse
    std::reverse(sv.begin(), sv.end());
    EXPECT_EQ(sv[0], 9);
    EXPECT_EQ(sv.back(), 1);
}

// Test edge cases
TEST_F(SmallVectorTest, EdgeCases)
{
    // Empty vector operations
    {
        Astra::SmallVector<int, 4> sv;
        EXPECT_TRUE(sv.empty());
        EXPECT_EQ(sv.size(), 0u);
        EXPECT_EQ(sv.begin(), sv.end());
        
        sv.clear(); // Clear on empty
        EXPECT_TRUE(sv.empty());
        
        sv.resize(0); // Resize to 0 on empty
        EXPECT_TRUE(sv.empty());
    }
    
    // Single element
    {
        Astra::SmallVector<int, 4> sv{42};
        EXPECT_EQ(sv.size(), 1u);
        EXPECT_EQ(sv.front(), 42);
        EXPECT_EQ(sv.back(), 42);
        
        sv.pop_back();
        EXPECT_TRUE(sv.empty());
    }
    
    // Exact small buffer size
    {
        Astra::SmallVector<int, 4> sv{1, 2, 3, 4};
        EXPECT_EQ(sv.size(), 4u);
        EXPECT_EQ(sv.capacity(), 4u);
        
        // One more triggers growth
        sv.push_back(5);
        EXPECT_EQ(sv.size(), 5u);
        EXPECT_GT(sv.capacity(), 4u);
    }
}

// Test deduction guide
TEST_F(SmallVectorTest, DeductionGuide)
{
    // Deduction from initializer list
    Astra::SmallVector sv1{1, 2, 3};  // Should deduce Astra::SmallVector<int, 3>
    EXPECT_EQ(sv1.size(), 3u);
    EXPECT_EQ(sv1[0], 1);
    
    Astra::SmallVector sv2{std::string("hello")};  // Should deduce Astra::SmallVector<std::string, 1>
    EXPECT_EQ(sv2.size(), 1u);
    EXPECT_EQ(sv2[0], "hello");
}

// Insert-fill of a non-trivial type must not move_backward/fill into
// uninitialized storage. Exercises the count >= tail (else) path of
// insert(pos, count, value).
TEST_F(SmallVectorTest, InsertFillWithNonTrivialType)
{
    Astra::SmallVector<std::string, 4> v;
    v.push_back("a");
    v.push_back("b");
    v.push_back("c");
    // Insert 3 copies at offset 1: tail = 3 - 1 = 2, count = 3, so
    // count >= tail — the whole tail lands in raw space beyond the current end.
    v.insert(v.begin() + 1, 3, std::string("X"));
    ASSERT_EQ(v.size(), 6u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "X");
    EXPECT_EQ(v[2], "X");
    EXPECT_EQ(v[3], "X");
    EXPECT_EQ(v[4], "b");
    EXPECT_EQ(v[5], "c");
}

TEST_F(SmallVectorTest, InsertFillCountLessThanTail)
{
    // 5 elements, insert 2 at offset 1: tail = 5 - 1 = 4, count = 2, so
    // count < tail — exercises the std::move_backward path of insert(pos, count, value).
    Astra::SmallVector<std::string, 4> v;
    for (const char* s : { "a", "b", "c", "d", "e" }) v.push_back(s);
    ASSERT_EQ(v.size(), 5u);

    v.insert(v.begin() + 1, 2, std::string("X"));

    ASSERT_EQ(v.size(), 7u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "X");
    EXPECT_EQ(v[2], "X");
    EXPECT_EQ(v[3], "b");
    EXPECT_EQ(v[4], "c");
    EXPECT_EQ(v[5], "d");
    EXPECT_EQ(v[6], "e");
}

namespace {
// Move-observable, lifetime-counted element for self-aliasing tests.
struct AliasProbe
{
    static inline int liveCount = 0;
    static constexpr int kMovedFrom = -1;
    int value = 0;

    AliasProbe() { ++liveCount; }
    explicit AliasProbe(int v) : value(v) { ++liveCount; }
    AliasProbe(const AliasProbe& o) : value(o.value) { ++liveCount; }
    AliasProbe(AliasProbe&& o) noexcept : value(o.value) { o.value = kMovedFrom; ++liveCount; }
    AliasProbe& operator=(const AliasProbe& o) { value = o.value; return *this; }
    AliasProbe& operator=(AliasProbe&& o) noexcept { value = o.value; o.value = kMovedFrom; return *this; }
    ~AliasProbe() { --liveCount; }
};
} // namespace

// SBO -> heap reallocation: v.push_back(v[0]) must copy the original value,
// not read the moved-from inline slot. (Deterministic RED pre-fix: kMovedFrom.)
TEST_F(SmallVectorTest, SelfAliasingPushBackForcesReallocation)
{
    Astra::SmallVector<AliasProbe, 4> v;
    for (int i = 0; i < 4; ++i)
        v.push_back(AliasProbe(i));           // inline full: size == capacity == 4
    ASSERT_EQ(v.size(), 4u);
    ASSERT_EQ(v.capacity(), 4u);

    v.push_back(v[0]);                        // self-alias, forces reallocation
    EXPECT_GT(v.capacity(), 4u);              // grew off inline storage
    ASSERT_EQ(v.size(), 5u);
    EXPECT_EQ(v.back().value, 0);             // original value, not kMovedFrom
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(v[i].value, i);             // pre-existing elements intact
}

// Already-heap buffer: reallocation frees the storage the argument points into.
TEST_F(SmallVectorTest, SelfAliasingEmplaceBackHeapRealloc)
{
    Astra::SmallVector<AliasProbe, 2> v;
    for (int i = 0; i < 3; ++i)
        v.emplace_back(i);                    // 3rd add forces SBO -> heap
    while (v.size() < v.capacity())
        v.emplace_back(static_cast<int>(v.size()));  // fill exactly to capacity
    ASSERT_EQ(v.size(), v.capacity());

    v.emplace_back(v[0]);                     // self-alias, heap realloc frees old buffer
    EXPECT_EQ(v.back().value, 0);             // must be 0, not freed-memory garbage
}

// Middle insert shifts elements before the argument is read.
TEST_F(SmallVectorTest, SelfAliasingMiddleInsert)
{
    Astra::SmallVector<AliasProbe, 8> v;
    for (int i = 0; i < 6; ++i)
        v.push_back(AliasProbe(i));           // 0..5, spare capacity (no realloc)

    v.insert(v.begin() + 2, v[5]);            // insert a copy of v[5] at index 2
    ASSERT_EQ(v.size(), 7u);
    EXPECT_EQ(v[2].value, 5);                 // inserted value correct
    EXPECT_EQ(v[0].value, 0);
    EXPECT_EQ(v[1].value, 1);
    EXPECT_EQ(v[3].value, 2);                 // original v[2] shifted right
    EXPECT_EQ(v[6].value, 5);                 // original v[5] shifted right
}

// No leak / no double-free across self-aliasing reallocation + middle insert.
TEST_F(SmallVectorTest, SelfAliasingNoLeakOrDoubleFree)
{
    AliasProbe::liveCount = 0;
    {
        Astra::SmallVector<AliasProbe, 4> v;
        for (int i = 0; i < 4; ++i)
            v.push_back(AliasProbe(i));
        v.push_back(v[0]);                    // self-alias + realloc
        v.emplace(v.begin() + 1, v[3]);       // self-alias middle insert
        EXPECT_GT(AliasProbe::liveCount, 0);
    }
    EXPECT_EQ(AliasProbe::liveCount, 0);      // every construction matched by a destruction
}