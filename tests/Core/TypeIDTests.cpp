#include <gtest/gtest.h>
#include <Astra/Core/TypeID.hpp>
#include <iostream>
#include <set>

namespace
{
    struct Position { float x, y, z; };
    struct Velocity { float dx, dy, dz; };
    struct Health { int current, max; };
    struct Transform { float matrix[16]; };
    
    namespace Game
    {
        struct Player { int id; };
        struct Enemy { int health; };
    }
    
    template<typename T>
    struct TemplatedComponent { T value; };
}

class TypeIDTests : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TypeIDTests, RuntimeIDsAreUnique)
{
    using namespace Astra;
    
    auto posId = TypeID<Position>::Value();
    auto velId = TypeID<Velocity>::Value();
    auto healthId = TypeID<Health>::Value();
    
    EXPECT_NE(posId, velId);
    EXPECT_NE(posId, healthId);
    EXPECT_NE(velId, healthId);
    
    // Same type should always return same ID
    EXPECT_EQ(TypeID<Position>::Value(), posId);
    EXPECT_EQ(TypeID<Position>::Value(), TypeID<Position>::Value());
}

TEST_F(TypeIDTests, TypeNamesAreExtracted)
{
    using namespace Astra;
    
    auto posName = TypeID<Position>::Name();
    auto velName = TypeID<Velocity>::Name();
    
    // Names should contain the type name (exact format varies by compiler)
    EXPECT_NE(posName.find("Position"), std::string_view::npos) 
        << "Position name: " << posName;
    EXPECT_NE(velName.find("Velocity"), std::string_view::npos)
        << "Velocity name: " << velName;
    
    // Different types should have different names
    EXPECT_NE(posName, velName);
}

TEST_F(TypeIDTests, NamespacedTypesWork)
{
    using namespace Astra;
    
    auto playerName = TypeID<Game::Player>::Name();
    auto enemyName = TypeID<Game::Enemy>::Name();
    
    // Should include namespace info
    EXPECT_NE(playerName.find("Player"), std::string_view::npos)
        << "Player name: " << playerName;
    EXPECT_NE(enemyName.find("Enemy"), std::string_view::npos)
        << "Enemy name: " << enemyName;
    
    // Should be different
    EXPECT_NE(playerName, enemyName);
}

TEST_F(TypeIDTests, TemplatedTypesWork)
{
    using namespace Astra;
    
    auto intName = TypeID<TemplatedComponent<int>>::Name();
    auto floatName = TypeID<TemplatedComponent<float>>::Name();
    
    // Should include template info
    EXPECT_NE(intName.find("TemplatedComponent"), std::string_view::npos)
        << "Int template name: " << intName;
    EXPECT_NE(floatName.find("TemplatedComponent"), std::string_view::npos)
        << "Float template name: " << floatName;
    
    // Different template parameters should give different names
    EXPECT_NE(intName, floatName);
}

TEST_F(TypeIDTests, HashesAreStable)
{
    using namespace Astra;
    
    // Hashes should be compile-time constants
    constexpr uint64_t posHash1 = TypeID<Position>::Hash();
    constexpr uint64_t posHash2 = TypeID<Position>::Hash();
    
    EXPECT_EQ(posHash1, posHash2);
    
    // Same type should always produce same hash
    EXPECT_EQ(TypeID<Position>::Hash(), TypeID<Position>::Hash());
}

TEST_F(TypeIDTests, HashesAreUnique)
{
    using namespace Astra;
    
    // Collect hashes from various types
    std::set<uint64_t> hashes;
    
    hashes.insert(TypeID<Position>::Hash());
    hashes.insert(TypeID<Velocity>::Hash());
    hashes.insert(TypeID<Health>::Hash());
    hashes.insert(TypeID<Transform>::Hash());
    hashes.insert(TypeID<Game::Player>::Hash());
    hashes.insert(TypeID<Game::Enemy>::Hash());
    hashes.insert(TypeID<TemplatedComponent<int>>::Hash());
    hashes.insert(TypeID<TemplatedComponent<float>>::Hash());
    hashes.insert(TypeID<TemplatedComponent<double>>::Hash());
    
    // All hashes should be unique (no collisions)
    EXPECT_EQ(hashes.size(), 9u) << "Hash collision detected!";
}

TEST_F(TypeIDTests, HashCheckWorks)
{
    using namespace Astra;
    
    uint64_t posHash = TypeID<Position>::Hash();
    uint64_t velHash = TypeID<Velocity>::Hash();
    
    EXPECT_TRUE(TypeID<Position>::HasHash(posHash));
    EXPECT_FALSE(TypeID<Position>::HasHash(velHash));
    EXPECT_TRUE(TypeID<Velocity>::HasHash(velHash));
    EXPECT_FALSE(TypeID<Velocity>::HasHash(posHash));
}

TEST_F(TypeIDTests, XXHashProducesExpectedValues)
{
    using namespace Astra::Detail::XXHash;
    
    // Test vectors from XXHash specification
    // These ensure our implementation matches the reference
    
    // Empty string
    constexpr uint64_t emptyHash = XXHash64("", 0, 0);
    EXPECT_EQ(emptyHash, 0xEF46DB3751D8E999ULL);
    
    // Single character
    constexpr uint64_t aHash = XXHash64("a", 1, 0);
    EXPECT_EQ(aHash, 0xD24EC4F1A98C6E5BULL);
    
    // Known string
    constexpr uint64_t helloHash = XXHash64("Hello", 5, 0);
    EXPECT_NE(helloHash, 0u); // Just verify it produces something
    
    // With different seeds
    constexpr uint64_t seed1 = XXHash64("test", 4, 1);
    constexpr uint64_t seed2 = XXHash64("test", 4, 2);
    EXPECT_NE(seed1, seed2);
}

TEST_F(TypeIDTests, CompileTimeConstexpr)
{
    using namespace Astra;
    
    // These should all be compile-time evaluable
    constexpr std::string_view name = TypeID<Position>::Name();
    constexpr uint64_t hash = TypeID<Position>::Hash();
    constexpr bool hasHash = TypeID<Position>::HasHash(hash);
    
    // Use them to ensure they're actually evaluated
    static_assert(hash != 0);
    static_assert(hasHash);
    static_assert(!name.empty());
    
    EXPECT_TRUE(true); // Test passes if it compiles
}
