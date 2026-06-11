#include <gtest/gtest.h>
#include "Astra/Entity/Entity.hpp"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <limits>

class EntityTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test default construction
TEST_F(EntityTest, DefaultConstruction)
{
    Astra::Entity entity;
    
    EXPECT_FALSE(entity.IsValid());
    EXPECT_FALSE(static_cast<bool>(entity));
    EXPECT_EQ(entity, Astra::Entity::Invalid());
}

// Test construction with ID and version
TEST_F(EntityTest, ConstructionWithIDAndVersion)
{
    Astra::Entity entity(100, 5);
    
    EXPECT_TRUE(entity.IsValid());
    EXPECT_TRUE(static_cast<bool>(entity));
    EXPECT_EQ(entity.GetID(), 100u);
    EXPECT_EQ(entity.GetVersion(), 5u);
}

// Test construction with raw value
TEST_F(EntityTest, ConstructionWithRawValue)
{
    // For 32-bit entity with 8-bit version:
    // Version 5 << 24 = 0x05000000
    // ID 100 = 0x64
    // Combined = 0x05000064
    Astra::Entity::StorageType rawValue = (static_cast<Astra::Entity::StorageType>(5) << Astra::Entity::VERSION_SHIFT) | 100;
    Astra::Entity entity(rawValue);
    
    EXPECT_TRUE(entity.IsValid());
    EXPECT_EQ(entity.GetID(), 100u);
    EXPECT_EQ(entity.GetVersion(), 5u);
    EXPECT_EQ(entity.GetValue(), rawValue);
}

// Test invalid entity constant
TEST_F(EntityTest, InvalidEntityConstant)
{
    Astra::Entity invalid = Astra::Entity::Invalid();
    
    EXPECT_FALSE(invalid.IsValid());
    EXPECT_TRUE(invalid.IsInvalid());
    EXPECT_EQ(invalid.GetValue(), std::numeric_limits<Astra::Entity::StorageType>::max());
}

// Test ID and version extraction
TEST_F(EntityTest, IDAndVersionExtraction)
{
    // Test various combinations
    struct TestCase
    {
        Astra::Entity::StorageType id;
        Astra::Entity::VersionType version;
    };
    
    TestCase cases[] = {
        {0, 0},
        {1, 0},
        {0, 1},
        {12345, 42},
        {Astra::Entity::ID_MASK, 0},  // Max ID
        {0, 255},  // Max version for 8-bit
        {Astra::Entity::ID_MASK, 255}  // Max both
    };
    
    for (const auto& tc : cases)
    {
        Astra::Entity entity(tc.id, tc.version);
        EXPECT_EQ(entity.GetID(), tc.id);
        EXPECT_EQ(entity.GetVersion(), tc.version);
    }
}

// Test ID masking (ensure ID doesn't overflow into version bits)
TEST_F(EntityTest, IDMasking)
{
    // Try to create entity with ID that would overflow into version bits
    Astra::Entity::StorageType overflowID = Astra::Entity::ID_MASK + 1;
    Astra::Entity entity(overflowID, 0);
    
    // ID should be masked to fit
    EXPECT_EQ(entity.GetID(), 0u);  // Overflow bit is masked off
    EXPECT_EQ(entity.GetVersion(), 0u);
}

// Test version incrementing
TEST_F(EntityTest, NextVersion)
{
    Astra::Entity entity(100, 5);
    Astra::Entity next = entity.NextVersion();
    
    EXPECT_EQ(next.GetID(), 100u);
    EXPECT_EQ(next.GetVersion(), 6u);
}

// Test version overflow handling
TEST_F(EntityTest, VersionOverflow)
{
    // Create entity with max version for the current Entity type
    Astra::Entity entity(100, Astra::Entity::VERSION_MASK);
    Astra::Entity next = entity.NextVersion();
    
    // Should return invalid entity on overflow
    EXPECT_FALSE(next.IsValid());
    EXPECT_EQ(next, Astra::Entity::Invalid());
}

// Test equality operators
TEST_F(EntityTest, EqualityOperators)
{
    Astra::Entity e1(100, 5);
    Astra::Entity e2(100, 5);
    Astra::Entity e3(100, 6);
    Astra::Entity e4(101, 5);
    
    // Same entity
    EXPECT_EQ(e1, e2);
    EXPECT_TRUE(e1 == e2);
    
    // Different version
    EXPECT_NE(e1, e3);
    EXPECT_FALSE(e1 == e3);
    
    // Different ID
    EXPECT_NE(e1, e4);
    EXPECT_FALSE(e1 == e4);
    
    // Comparison with raw value
    Astra::Entity::StorageType rawValue = e1.GetValue();
    EXPECT_TRUE(e1 == rawValue);
    EXPECT_TRUE(rawValue == e1);
}

// Test comparison operators
TEST_F(EntityTest, ComparisonOperators)
{
    Astra::Entity e1(100, 5);
    Astra::Entity e2(101, 5);
    Astra::Entity e3(100, 6);
    
    // Less than by ID
    EXPECT_TRUE(e1 < e2);
    EXPECT_FALSE(e2 < e1);
    
    // Less than by version (same ID)
    EXPECT_TRUE(e1 < e3);
    EXPECT_FALSE(e3 < e1);
    
    // Greater than
    EXPECT_TRUE(e2 > e1);
    EXPECT_FALSE(e1 > e2);
    
    // Not less/greater when equal
    Astra::Entity e4(100, 5);
    EXPECT_FALSE(e1 < e4);
    EXPECT_FALSE(e1 > e4);
}

// Test type conversions
TEST_F(EntityTest, TypeConversions)
{
    Astra::Entity entity(100, 5);
    
    // Implicit conversion to underlying type
    Astra::Entity::StorageType value = entity;
    EXPECT_EQ(value, entity.GetValue());
    
    // Explicit conversion to other numeric types
    auto asUint64 = static_cast<uint64_t>(entity);
    EXPECT_EQ(asUint64, static_cast<uint64_t>(entity.GetValue()));
    
    auto asInt = static_cast<int>(entity);
    EXPECT_EQ(asInt, static_cast<int>(entity.GetValue()));
}

// Test bool conversion
TEST_F(EntityTest, BoolConversion)
{
    Astra::Entity valid(100, 5);
    Astra::Entity invalid;
    
    // Valid entity converts to true
    EXPECT_TRUE(static_cast<bool>(valid));
    if (!valid)
    {
        FAIL() << "Valid entity should convert to true";
    }
    
    // Invalid entity converts to false
    EXPECT_FALSE(static_cast<bool>(invalid));
    if (invalid)
    {
        FAIL() << "Invalid entity should convert to false";
    }
}

// Test use in STL containers
TEST_F(EntityTest, STLContainerUsage)
{
    // Vector
    std::vector<Astra::Entity> entities;
    entities.push_back(Astra::Entity(1, 1));
    entities.push_back(Astra::Entity(2, 1));
    entities.push_back(Astra::Entity(3, 1));
    
    EXPECT_EQ(entities.size(), 3u);
    EXPECT_EQ(entities[0].GetID(), 1u);
    EXPECT_EQ(entities[2].GetID(), 3u);
    
    // Unordered set with custom hash
    std::unordered_set<Astra::Entity, Astra::EntityHash> entitySet;
    entitySet.insert(Astra::Entity(1, 1));
    entitySet.insert(Astra::Entity(2, 1));
    entitySet.insert(Astra::Entity(1, 1));  // Duplicate
    
    EXPECT_EQ(entitySet.size(), 2u);
    EXPECT_TRUE(entitySet.count(Astra::Entity(1, 1)) > 0);
    EXPECT_TRUE(entitySet.count(Astra::Entity(2, 1)) > 0);
    EXPECT_FALSE(entitySet.count(Astra::Entity(3, 1)) > 0);
}

// Test EntityHash
TEST_F(EntityTest, EntityHash)
{
    Astra::EntityHash hasher;
    
    Astra::Entity e1(100, 5);
    Astra::Entity e2(100, 5);
    Astra::Entity e3(100, 6);
    Astra::Entity e4(101, 5);
    
    // Same entities should have same hash
    EXPECT_EQ(hasher(e1), hasher(e2));
    
    // Different entities should (likely) have different hashes
    // Note: Hash collisions are possible but unlikely
    size_t hash1 = hasher(e1);
    size_t hash3 = hasher(e3);
    size_t hash4 = hasher(e4);
    
    // At least one should be different (very high probability)
    EXPECT_TRUE(hash1 != hash3 || hash1 != hash4);
    
    // Hash should be non-zero for valid entities
    EXPECT_NE(hasher(e1), 0u);
    
    // Test H2 byte is never 0 (for SwissTable compatibility)
    for (int i = 0; i < 100; ++i)
    {
        Astra::Entity entity(i, i % 256);
        size_t hash = hasher(entity);
        EXPECT_NE((hash & 0x7F), 0u) << "H2 byte should never be 0 for entity " << i;
    }
}

// Test std::hash specialization
TEST_F(EntityTest, StdHashSpecialization)
{
    std::hash<Astra::Entity> stdHasher;
    Astra::EntityHash customHasher;
    
    Astra::Entity entity(100, 5);
    
    // std::hash should use EntityHash internally
    EXPECT_EQ(stdHasher(entity), customHasher(entity));
    
    // Test with unordered_map using std::hash
    std::unordered_map<Astra::Entity, std::string> entityMap;
    entityMap[Astra::Entity(1, 1)] = "first";
    entityMap[Astra::Entity(2, 1)] = "second";
    
    EXPECT_EQ(entityMap[Astra::Entity(1, 1)], "first");
    EXPECT_EQ(entityMap[Astra::Entity(2, 1)], "second");
}

// Test Entity traits concept
TEST_F(EntityTest, EntityTraitsConcept)
{
    // Verify common EntityTraits configurations satisfy the concept
    using Traits32 = Astra::EntityTraits<32, 8>;
    static_assert(Astra::Detail::EntityTraitsConcept<Traits32>);
    
    // Verify 64-bit configuration
    using Traits64 = Astra::EntityTraits<64, 32>;
    static_assert(Astra::Detail::EntityTraitsConcept<Traits64>);
    
    // Check trait values for 32-bit
    static_assert(Traits32::ID_BITS == 24);
    static_assert(Traits32::VERSION_SHIFT == 24);
    static_assert(std::is_same_v<Traits32::StorageType, uint32_t>);
    static_assert(std::is_same_v<Traits32::VersionType, uint8_t>);
    
    // Check trait values for 64-bit
    static_assert(Traits64::ID_BITS == 32);
    static_assert(Traits64::VERSION_SHIFT == 32);
    static_assert(std::is_same_v<Traits64::StorageType, uint64_t>);
    static_assert(std::is_same_v<Traits64::VersionType, uint32_t>);
}

// Test edge cases
TEST_F(EntityTest, EdgeCases)
{
    // Entity with ID 0 and version 1 (minimum valid)
    Astra::Entity minValid(0, 1);
    EXPECT_TRUE(minValid.IsValid());
    EXPECT_EQ(minValid.GetID(), 0u);
    EXPECT_EQ(minValid.GetVersion(), 1u);
    
    // Max valid ID with minimum version
    Astra::Entity maxId(Astra::Entity::ID_MASK, 1);
    EXPECT_TRUE(maxId.IsValid());
    EXPECT_EQ(maxId.GetID(), Astra::Entity::ID_MASK);
    
    // Max valid version for the current Entity type
    Astra::Entity maxVer(0, Astra::Entity::VERSION_MASK);
    EXPECT_TRUE(maxVer.IsValid());
    EXPECT_EQ(maxVer.GetVersion(), Astra::Entity::VERSION_MASK);
    
    // Max ID with max version would be INVALID
    // So test max ID with max-1 version
    Astra::Entity nearMaxBoth(Astra::Entity::ID_MASK, Astra::Entity::VERSION_MASK - 1);
    EXPECT_TRUE(nearMaxBoth.IsValid());
    EXPECT_EQ(nearMaxBoth.GetID(), Astra::Entity::ID_MASK);
    EXPECT_EQ(nearMaxBoth.GetVersion(), Astra::Entity::VERSION_MASK - 1);
    
    // Verify that max ID + max version is indeed INVALID
    Astra::Entity shouldBeInvalid(Astra::Entity::ID_MASK, Astra::Entity::VERSION_MASK);
    EXPECT_FALSE(shouldBeInvalid.IsValid());
    EXPECT_EQ(shouldBeInvalid, Astra::Entity::Invalid());
}

// Test constexpr operations
TEST_F(EntityTest, ConstexprOperations)
{
    // These should all be compile-time constants
    constexpr Astra::Entity e1;
    constexpr Astra::Entity e2(100, 5);
    constexpr Astra::Entity e3(12345);
    
    constexpr bool valid = e2.IsValid();
    constexpr auto id = e2.GetID();
    constexpr auto version = e2.GetVersion();
    constexpr auto value = e2.GetValue();

    static_assert(!e1.IsValid());
    static_assert(e2.IsValid());
    static_assert(id == 100);
    static_assert(version == 5);
    (void)valid; (void)value;
    
    constexpr bool equal = (e2 == e2);
    constexpr bool notEqual = (e2 != e3);
    static_assert(equal);
    static_assert(notEqual);
}

// Performance characteristics test
TEST_F(EntityTest, PerformanceCharacteristics)
{
    // Entity should be trivially copyable for performance
    static_assert(std::is_trivially_copyable_v<Astra::Entity>);
    // Entity should be standard layout
    static_assert(std::is_standard_layout_v<Astra::Entity>);
    
    // Entity operations should be noexcept
    Astra::Entity e(100, 5);
    static_assert(noexcept(e.IsValid()));
    static_assert(noexcept(e.GetID()));
    static_assert(noexcept(e.GetVersion()));
    static_assert(noexcept(e.GetValue()));
    static_assert(noexcept(e.NextVersion()));
}

// Test multiple entity versions tracking
TEST_F(EntityTest, VersionTracking)
{
    std::vector<Astra::Entity> versions;
    
    Astra::Entity entity(100, 1);
    versions.push_back(entity);
    
    // Create multiple versions
    for (int i = 0; i < 10; ++i)
    {
        entity = entity.NextVersion();
        if (entity.IsValid())
        {
            versions.push_back(entity);
        }
    }
    
    // All should have same ID, different versions
    for (size_t i = 0; i < versions.size(); ++i)
    {
        EXPECT_EQ(versions[i].GetID(), 100u);
        EXPECT_EQ(versions[i].GetVersion(), i + 1);
        
        // All versions should be different entities
        for (size_t j = i + 1; j < versions.size(); ++j)
        {
            EXPECT_NE(versions[i], versions[j]);
        }
    }
}

// Test batch entity operations
TEST_F(EntityTest, BatchOperations)
{
    const size_t batchSize = 1000;
    std::vector<Astra::Entity> entities;
    entities.reserve(batchSize);
    
    // Create batch of entities
    for (size_t i = 0; i < batchSize; ++i)
    {
        entities.emplace_back(static_cast<Astra::Entity::StorageType>(i), 
                            static_cast<Astra::Entity::VersionType>(i % 256));
    }
    
    // Verify all are unique
    std::unordered_set<Astra::Entity, Astra::EntityHash> uniqueSet(entities.begin(), entities.end());
    EXPECT_EQ(uniqueSet.size(), batchSize);
    
    // Test batch validity checking
    size_t validCount = 0;
    for (const auto& entity : entities)
    {
        if (entity.IsValid())
        {
            ++validCount;
        }
    }
    EXPECT_EQ(validCount, batchSize);
}