#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>
#include "Astra/Container/Bitmap.hpp"


class BitmapTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic bit operations
TEST_F(BitmapTest, BasicBitOperations)
{
    Astra::Bitmap<128> bitmap;
    
    // Initially all bits should be clear
    EXPECT_TRUE(bitmap.None());
    EXPECT_FALSE(bitmap.Any());
    EXPECT_EQ(bitmap.Count(), 0u);
    
    // Set some bits
    bitmap.Set(0);
    bitmap.Set(63);
    bitmap.Set(64);
    bitmap.Set(127);
    
    // Test that bits are set
    EXPECT_TRUE(bitmap.Test(0));
    EXPECT_TRUE(bitmap.Test(63));
    EXPECT_TRUE(bitmap.Test(64));
    EXPECT_TRUE(bitmap.Test(127));
    EXPECT_FALSE(bitmap.Test(1));
    EXPECT_FALSE(bitmap.Test(50));
    
    // Test count
    EXPECT_EQ(bitmap.Count(), 4u);
    EXPECT_TRUE(bitmap.Any());
    EXPECT_FALSE(bitmap.None());
    
    // Reset some bits
    bitmap.Reset(0);
    bitmap.Reset(127);
    
    EXPECT_FALSE(bitmap.Test(0));
    EXPECT_FALSE(bitmap.Test(127));
    EXPECT_TRUE(bitmap.Test(63));
    EXPECT_TRUE(bitmap.Test(64));
    EXPECT_EQ(bitmap.Count(), 2u);
}

// Test boundary conditions
TEST_F(BitmapTest, BoundaryConditions)
{
    Astra::Bitmap<256> bitmap;

    // Test setting/testing out of range indices
    bitmap.Set(255);  // Valid - last bit
    EXPECT_TRUE(bitmap.Test(255));

    // Task 11: Set() now asserts loudly on out-of-range indices in Debug
    // (component ID space overflow guard) instead of silently ignoring them.
    // Release builds keep the old no-op fallback since ASTRA_ASSERT compiles out.
#ifdef ASTRA_BUILD_DEBUG
    EXPECT_DEATH({ bitmap.Set(256); }, "");
    EXPECT_DEATH({ bitmap.Set(1000); }, "");
#else
    bitmap.Set(256);  // Invalid - out of range
    EXPECT_FALSE(bitmap.Test(256));

    bitmap.Set(1000); // Invalid - way out of range
    EXPECT_FALSE(bitmap.Test(1000));
#endif

    // Test word boundaries (64-bit words)
    bitmap.Set(63);
    bitmap.Set(64);
    bitmap.Set(127);
    bitmap.Set(128);
    bitmap.Set(191);
    bitmap.Set(192);
    
    EXPECT_TRUE(bitmap.Test(63));
    EXPECT_TRUE(bitmap.Test(64));
    EXPECT_TRUE(bitmap.Test(127));
    EXPECT_TRUE(bitmap.Test(128));
    EXPECT_TRUE(bitmap.Test(191));
    EXPECT_TRUE(bitmap.Test(192));
}

// Test HasAll mask checking
TEST_F(BitmapTest, HasAllMaskChecking)
{
    Astra::Bitmap<128> bitmap;
    Astra::Bitmap<128> mask1;
    Astra::Bitmap<128> mask2;
    
    // Set up bitmap with some components
    bitmap.Set(0);  // Component 0
    bitmap.Set(5);  // Component 5
    bitmap.Set(10); // Component 10
    bitmap.Set(64); // Component 64
    
    // Mask1: Check for components 0 and 5
    mask1.Set(0);
    mask1.Set(5);
    EXPECT_TRUE(bitmap.HasAll(mask1));
    
    // Mask2: Check for components 0, 5, and 15 (missing 15)
    mask2.Set(0);
    mask2.Set(5);
    mask2.Set(15);
    EXPECT_FALSE(bitmap.HasAll(mask2));
    
    // Empty mask should always match
    Astra::Bitmap<128> emptyMask;
    EXPECT_TRUE(bitmap.HasAll(emptyMask));
    
    // Self should always match
    EXPECT_TRUE(bitmap.HasAll(bitmap));
}

// Test equality operator
TEST_F(BitmapTest, EqualityOperator)
{
    Astra::Bitmap<192> bitmap1;
    Astra::Bitmap<192> bitmap2;
    
    // Initially equal (both empty)
    EXPECT_EQ(bitmap1, bitmap2);
    
    // Set same bits
    bitmap1.Set(0);
    bitmap1.Set(100);
    bitmap1.Set(191);
    
    bitmap2.Set(0);
    bitmap2.Set(100);
    bitmap2.Set(191);
    
    EXPECT_EQ(bitmap1, bitmap2);
    
    // Make them different
    bitmap2.Set(50);
    EXPECT_NE(bitmap1, bitmap2);
    
    bitmap2.Reset(50);
    EXPECT_EQ(bitmap1, bitmap2);
}

// Test bitwise AND operator
TEST_F(BitmapTest, BitwiseAndOperator)
{
    Astra::Bitmap<128> bitmap1;
    Astra::Bitmap<128> bitmap2;
    
    bitmap1.Set(0);
    bitmap1.Set(5);
    bitmap1.Set(10);
    bitmap1.Set(70);
    
    bitmap2.Set(5);
    bitmap2.Set(10);
    bitmap2.Set(15);
    bitmap2.Set(70);
    
    auto result = bitmap1 & bitmap2;
    
    // Should only have bits 5, 10, and 70 set
    EXPECT_FALSE(result.Test(0));
    EXPECT_TRUE(result.Test(5));
    EXPECT_TRUE(result.Test(10));
    EXPECT_FALSE(result.Test(15));
    EXPECT_TRUE(result.Test(70));
    EXPECT_EQ(result.Count(), 3u);
}

// Test with different sizes
TEST_F(BitmapTest, DifferentSizes)
{
    // Small bitmap (single word)
    {
        Astra::Bitmap<32> small;
        small.Set(0);
        small.Set(31);
        EXPECT_EQ(small.Count(), 2u);
        EXPECT_TRUE(small.Test(0));
        EXPECT_TRUE(small.Test(31));
    }
    
    // Medium bitmap (2 words)
    {
        Astra::Bitmap<100> medium;
        medium.Set(0);
        medium.Set(63);
        medium.Set(64);
        medium.Set(99);
        EXPECT_EQ(medium.Count(), 4u);
    }
    
    // Large bitmap (multiple words)
    {
        Astra::Bitmap<512> large;
        for (size_t i = 0; i < 512; i += 64)
        {
            large.Set(i);
        }
        EXPECT_EQ(large.Count(), 8u);
    }
}

// Test hash function
TEST_F(BitmapTest, HashFunction)
{
    Astra::Bitmap<128> bitmap1;
    Astra::Bitmap<128> bitmap2;
    Astra::Bitmap<128> bitmap3;
    
    // Same content should have same hash
    bitmap1.Set(5);
    bitmap1.Set(10);
    
    bitmap2.Set(5);
    bitmap2.Set(10);
    
    EXPECT_EQ(bitmap1.GetHash(), bitmap2.GetHash());
    
    // Different content should (likely) have different hash
    bitmap3.Set(5);
    bitmap3.Set(11);
    
    EXPECT_NE(bitmap1.GetHash(), bitmap3.GetHash());
    
    // Test with hash container
    std::unordered_set<Astra::Bitmap<128>, Astra::BitmapHash<128>> bitmapSet;
    bitmapSet.insert(bitmap1);
    EXPECT_EQ(bitmapSet.count(bitmap2), 1u); // Should find equivalent bitmap
    EXPECT_EQ(bitmapSet.count(bitmap3), 0u); // Should not find different bitmap
}

// Test batch operations
TEST_F(BitmapTest, BatchHasAll)
{
    constexpr size_t BitmapCount = 10;
    Astra::Bitmap<128> bitmaps[BitmapCount];
    Astra::Bitmap<128> mask;
    
    // Set up mask
    mask.Set(1);
    mask.Set(5);
    
    // Set up bitmaps - some have all mask bits, some don't
    for (size_t i = 0; i < BitmapCount; ++i)
    {
        bitmaps[i].Set(1); // All have bit 1
        if (i % 2 == 0)
        {
            bitmaps[i].Set(5); // Even indices have bit 5
        }
    }
    
    uint32_t results = Astra::Bitmap<128>::BatchHasAll(bitmaps, BitmapCount, mask);
    
    // Check results - even indices should match
    for (size_t i = 0; i < BitmapCount; ++i)
    {
        bool expected = (i % 2 == 0);
        bool actual = (results & (1u << i)) != 0;
        EXPECT_EQ(actual, expected) << "Index: " << i;
    }
}

// Test all bits operations
TEST_F(BitmapTest, AllBitsOperations)
{
    Astra::Bitmap<64> bitmap;
    
    // Set all bits
    for (size_t i = 0; i < 64; ++i)
    {
        bitmap.Set(i);
    }
    
    EXPECT_EQ(bitmap.Count(), 64u);
    EXPECT_TRUE(bitmap.Any());
    EXPECT_FALSE(bitmap.None());
    
    // Clear all bits
    for (size_t i = 0; i < 64; ++i)
    {
        bitmap.Reset(i);
    }
    
    EXPECT_EQ(bitmap.Count(), 0u);
    EXPECT_FALSE(bitmap.Any());
    EXPECT_TRUE(bitmap.None());
}

// Test patterns
TEST_F(BitmapTest, BitPatterns)
{
    Astra::Bitmap<256> bitmap;
    
    // Set alternating bits
    for (size_t i = 0; i < 256; i += 2)
    {
        bitmap.Set(i);
    }
    
    EXPECT_EQ(bitmap.Count(), 128u);
    
    // Verify pattern
    for (size_t i = 0; i < 256; ++i)
    {
        if (i % 2 == 0)
        {
            EXPECT_TRUE(bitmap.Test(i)) << "Bit " << i << " should be set";
        }
        else
        {
            EXPECT_FALSE(bitmap.Test(i)) << "Bit " << i << " should be clear";
        }
    }
}