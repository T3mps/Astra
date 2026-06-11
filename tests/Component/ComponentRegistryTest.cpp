#include <gtest/gtest.h>
#include "Astra/Component/ComponentRegistry.hpp"
#include "Astra/Serialization/BinaryWriter.hpp"
#include "Astra/Serialization/BinaryReader.hpp"
#include "../TestComponents.hpp"
#include <string>
#include <vector>
#include <memory>
#include <chrono>

class ComponentRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Use test components from shared header
using namespace Astra::Test;

// Test basic component registration
TEST_F(ComponentRegistryTest, BasicRegistration)
{
    Astra::ComponentRegistry registry;
    
    // Register a simple component
    registry.RegisterComponent<Position>();
    
    // Get component descriptor
    Astra::ComponentID id = Astra::TypeID<Position>::Value();
    const Astra::ComponentDescriptor* desc = registry.GetComponentDescriptor(id);
    
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->id, id);
    EXPECT_EQ(desc->size, sizeof(Position));
    EXPECT_EQ(desc->alignment, alignof(Position));
    EXPECT_TRUE(desc->is_trivially_copyable);
    EXPECT_FALSE(desc->is_empty);
}

// Test registering multiple components at once
TEST_F(ComponentRegistryTest, MultipleRegistration)
{
    Astra::ComponentRegistry registry;
    
    // Register multiple components
    registry.RegisterComponents<Position, Velocity, Health>();
    
    // Verify all are registered
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Position>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Velocity>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Health>::Value()), nullptr);
}

// Test duplicate registration (should be idempotent)
TEST_F(ComponentRegistryTest, DuplicateRegistration)
{
    Astra::ComponentRegistry registry;
    
    // Register component twice
    registry.RegisterComponent<Position>();
    const Astra::ComponentDescriptor* desc1 = registry.GetComponentDescriptor(Astra::TypeID<Position>::Value());
    
    registry.RegisterComponent<Position>();
    const Astra::ComponentDescriptor* desc2 = registry.GetComponentDescriptor(Astra::TypeID<Position>::Value());
    
    // Should be the same descriptor
    EXPECT_EQ(desc1, desc2);
    EXPECT_EQ(desc1->id, desc2->id);
}

// Test component type traits
TEST_F(ComponentRegistryTest, ComponentTypeTraits)
{
    Astra::ComponentRegistry registry;
    
    // Register components with different traits
    registry.RegisterComponents<Position, Name, Player, RenderData>();
    
    // Position - trivially copyable
    {
        const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Position>::Value());
        ASSERT_NE(desc, nullptr);
        EXPECT_TRUE(desc->is_trivially_copyable);
        EXPECT_TRUE(desc->is_nothrow_move_constructible);
        EXPECT_TRUE(desc->is_nothrow_default_constructible);
        EXPECT_FALSE(desc->is_empty);
    }
    
    // Name - non-trivially copyable (has std::string)
    {
        const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Name>::Value());
        ASSERT_NE(desc, nullptr);
        EXPECT_FALSE(desc->is_trivially_copyable);
        EXPECT_TRUE(desc->is_nothrow_move_constructible);
        EXPECT_FALSE(desc->is_empty);
    }
    
    // Player - empty component
    {
        const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Player>::Value());
        ASSERT_NE(desc, nullptr);
        EXPECT_TRUE(desc->is_trivially_copyable);
        EXPECT_TRUE(desc->is_empty);
        EXPECT_EQ(desc->size, 0u);  // Empty components should report size 0 for optimization
    }
    
    // RenderData - special alignment
    {
        const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<RenderData>::Value());
        ASSERT_NE(desc, nullptr);
        EXPECT_EQ(desc->alignment, 32u);
        EXPECT_TRUE(desc->is_trivially_copyable);
    }
}

// Test component operations through function pointers
TEST_F(ComponentRegistryTest, ComponentOperations)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Health>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Health>::Value());
    ASSERT_NE(desc, nullptr);
    
    // Test default construction
    alignas(Health) char buffer1[sizeof(Health)];
    desc->defaultConstruct(buffer1);
    Health* health1 = reinterpret_cast<Health*>(buffer1);
    EXPECT_EQ(health1->current, 100);
    EXPECT_EQ(health1->max, 100);
    
    // Test copy construction
    alignas(Health) char buffer2[sizeof(Health)];
    health1->current = 50;
    desc->copyConstruct(buffer2, buffer1);
    Health* health2 = reinterpret_cast<Health*>(buffer2);
    EXPECT_EQ(health2->current, 50);
    EXPECT_EQ(health2->max, 100);
    
    // Test move construction
    alignas(Health) char buffer3[sizeof(Health)];
    desc->moveConstruct(buffer3, buffer1);
    Health* health3 = reinterpret_cast<Health*>(buffer3);
    EXPECT_EQ(health3->current, 50);
    
    // Test copy assignment
    health3->current = 75;
    desc->copyAssign(buffer2, buffer3);
    EXPECT_EQ(health2->current, 75);
    
    // Test move assignment
    health3->current = 25;
    desc->moveAssign(buffer2, buffer3);
    EXPECT_EQ(health2->current, 25);
    
    // Test destruction
    desc->destruct(buffer1);
    desc->destruct(buffer2);
    desc->destruct(buffer3);
}

// Test with non-trivially copyable component
TEST_F(ComponentRegistryTest, NonTriviallyCopyableOperations)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Name>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Name>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_FALSE(desc->is_trivially_copyable);
    
    // Test default construction
    alignas(Name) char buffer1[sizeof(Name)];
    desc->defaultConstruct(buffer1);
    Name* name1 = reinterpret_cast<Name*>(buffer1);
    EXPECT_TRUE(name1->value.empty());
    
    // Set a value
    name1->value = "Test Name";
    
    // Test copy construction
    alignas(Name) char buffer2[sizeof(Name)];
    desc->copyConstruct(buffer2, buffer1);
    Name* name2 = reinterpret_cast<Name*>(buffer2);
    EXPECT_EQ(name2->value, "Test Name");
    
    // Test move construction
    alignas(Name) char buffer3[sizeof(Name)];
    desc->moveConstruct(buffer3, buffer1);
    Name* name3 = reinterpret_cast<Name*>(buffer3);
    EXPECT_EQ(name3->value, "Test Name");
    // Note: name1->value is now in moved-from state
    
    // Test copy assignment
    name3->value = "Modified";
    desc->copyAssign(buffer2, buffer3);
    EXPECT_EQ(name2->value, "Modified");
    
    // Clean up - important for non-trivial types
    desc->destruct(buffer1);
    desc->destruct(buffer2);
    desc->destruct(buffer3);
}

// Test with move-only component
TEST_F(ComponentRegistryTest, MoveOnlyComponent)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Resource>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Resource>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_FALSE(desc->is_trivially_copyable);
    EXPECT_FALSE(desc->is_copy_constructible);
    
    // Verify copy operations are null for move-only type
    EXPECT_EQ(desc->copyConstruct, nullptr);
    EXPECT_EQ(desc->copyAssign, nullptr);
    
    // But move operations should be valid
    EXPECT_NE(desc->moveConstruct, nullptr);
    EXPECT_NE(desc->moveAssign, nullptr);
    
    // Test default construction
    alignas(Resource) char buffer1[sizeof(Resource)];
    desc->defaultConstruct(buffer1);
    Resource* res1 = reinterpret_cast<Resource*>(buffer1);
    ASSERT_NE(res1->data, nullptr);
    EXPECT_EQ(*res1->data, 0);  // Resource default constructor initializes to 0
    
    // Set a value before moving
    *res1->data = 42;
    
    // Test move construction
    alignas(Resource) char buffer2[sizeof(Resource)];
    desc->moveConstruct(buffer2, buffer1);
    Resource* res2 = reinterpret_cast<Resource*>(buffer2);
    ASSERT_NE(res2->data, nullptr);
    EXPECT_EQ(*res2->data, 42);  // Should have the moved value
    EXPECT_EQ(res1->data, nullptr); // Moved from
    
    // Test move assignment
    alignas(Resource) char buffer3[sizeof(Resource)];
    desc->defaultConstruct(buffer3);
    Resource* res3 = reinterpret_cast<Resource*>(buffer3);
    *res3->data = 100;
    
    desc->moveAssign(buffer2, buffer3);
    ASSERT_NE(res2->data, nullptr);
    EXPECT_EQ(*res2->data, 100);
    EXPECT_EQ(res3->data, nullptr); // Moved from
    
    // Clean up
    desc->destruct(buffer1);
    desc->destruct(buffer2);
    desc->destruct(buffer3);
}

// Test empty component registration
TEST_F(ComponentRegistryTest, EmptyComponent)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Player>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Player>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_TRUE(desc->is_empty);
    EXPECT_EQ(desc->size, 0u);  // Empty components should report size 0 for optimization
    EXPECT_EQ(desc->alignment, 1u);  // Empty components should have alignment of 1
    
    // Operations should still work
    alignas(Player) char buffer[sizeof(Player)];
    desc->defaultConstruct(buffer);
    desc->destruct(buffer);
}

// Test getting non-registered component
TEST_F(ComponentRegistryTest, NonRegisteredComponent)
{
    Astra::ComponentRegistry registry;
    
    // Try to get a non-registered component
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Position>::Value());
    EXPECT_EQ(desc, nullptr);
}

// Test registering many components
TEST_F(ComponentRegistryTest, ManyComponents)
{
    Astra::ComponentRegistry registry;
    
    // Define many component types
    struct Component1 { int a; };
    struct Component2 { float b; };
    struct Component3 { double c; };
    struct Component4 { char d[16]; };
    struct Component5 { long e; };
    struct Component6 { short f; };
    struct Component7 { bool g; };
    struct Component8 { int h[4]; };
    
    // Register all at once
    registry.RegisterComponents<
        Component1, Component2, Component3, Component4,
        Component5, Component6, Component7, Component8,
        Position, Velocity, Health, Name, Player
    >();
    
    // Verify all are registered
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Component1>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Component8>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Position>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Name>::Value()), nullptr);
}

// Test component with arrays
TEST_F(ComponentRegistryTest, ComponentWithArrays)
{
    struct Transform
    {
        float matrix[16];
        float position[3];
        float rotation[4];
    };
    
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Transform>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Transform>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->size, sizeof(Transform));
    EXPECT_TRUE(desc->is_trivially_copyable);
    
    // Test operations
    alignas(Transform) char buffer1[sizeof(Transform)];
    desc->defaultConstruct(buffer1);
    Transform* t1 = reinterpret_cast<Transform*>(buffer1);
    
    // Set some values
    t1->matrix[0] = 1.0f;
    t1->position[0] = 10.0f;
    t1->rotation[0] = 0.5f;
    
    // Copy
    alignas(Transform) char buffer2[sizeof(Transform)];
    desc->copyConstruct(buffer2, buffer1);
    Transform* t2 = reinterpret_cast<Transform*>(buffer2);
    
    EXPECT_EQ(t2->matrix[0], 1.0f);
    EXPECT_EQ(t2->position[0], 10.0f);
    EXPECT_EQ(t2->rotation[0], 0.5f);
    
    desc->destruct(buffer1);
    desc->destruct(buffer2);
}

// Test component ID consistency
TEST_F(ComponentRegistryTest, ComponentIDConsistency)
{
    Astra::ComponentRegistry registry1;
    Astra::ComponentRegistry registry2;
    
    // Register in different order
    registry1.RegisterComponents<Position, Velocity, Health>();
    registry2.RegisterComponents<Health, Position, Velocity>();
    
    // IDs should be the same regardless of registration order
    auto posID1 = registry1.GetComponentDescriptor(Astra::TypeID<Position>::Value())->id;
    auto posID2 = registry2.GetComponentDescriptor(Astra::TypeID<Position>::Value())->id;
    EXPECT_EQ(posID1, posID2);
    
    auto velID1 = registry1.GetComponentDescriptor(Astra::TypeID<Velocity>::Value())->id;
    auto velID2 = registry2.GetComponentDescriptor(Astra::TypeID<Velocity>::Value())->id;
    EXPECT_EQ(velID1, velID2);
}

// Test function pointer validity
TEST_F(ComponentRegistryTest, FunctionPointerValidity)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponents<Position, Name, Resource>();
    
    // Check all function pointers are set
    for (auto id : {Astra::TypeID<Position>::Value(), 
                     Astra::TypeID<Name>::Value(),
                     Astra::TypeID<Resource>::Value()})
    {
        const auto* desc = registry.GetComponentDescriptor(id);
        ASSERT_NE(desc, nullptr);
        EXPECT_NE(desc->defaultConstruct, nullptr);
        EXPECT_NE(desc->destruct, nullptr);
        EXPECT_NE(desc->moveConstruct, nullptr);
        EXPECT_NE(desc->moveAssign, nullptr);
        
        // Copy operations should be set for copyable types
        if (id != Astra::TypeID<Resource>::Value()) // Resource is move-only
        {
            EXPECT_NE(desc->copyConstruct, nullptr);
            EXPECT_NE(desc->copyAssign, nullptr);
        }
        else
        {
            EXPECT_EQ(desc->copyConstruct, nullptr);
            EXPECT_EQ(desc->copyAssign, nullptr);
        }
    }
}

// Test edge case: component with reference member (should not compile)
// This is a compile-time test, so we comment it out
/*
TEST_F(ComponentRegistryTest, ComponentWithReference)
{
    struct BadComponent
    {
        int& ref;
    };
    
    Astra::ComponentRegistry registry;
    // This should not compile due to Component concept
    // registry.RegisterComponent<BadComponent>();
}
*/

// Test that empty RegisterComponents call is handled
TEST_F(ComponentRegistryTest, EmptyRegisterComponents)
{
    Astra::ComponentRegistry registry;
    
    // Should not crash or do anything
    registry.RegisterComponents<>();
    
    // Registry should still work
    registry.RegisterComponent<Position>();
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<Position>::Value()), nullptr);
}

// Test component with union
TEST_F(ComponentRegistryTest, ComponentWithUnion)
{
    union UnionData
    {
        int i;
        float f;
        char c[4];
    };
    
    struct ComponentWithUnion
    {
        UnionData data;
        int type;
    };
    
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<ComponentWithUnion>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<ComponentWithUnion>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->size, sizeof(ComponentWithUnion));
    EXPECT_TRUE(desc->is_trivially_copyable);
}

// Test registration idempotency
TEST_F(ComponentRegistryTest, RegistrationIdempotency)
{
    struct TestComp1 { int x; };
    struct TestComp2 { float y; };
    struct TestComp3 { double z; };
    struct TestComp4 { char w[8]; };
    struct TestComp5 { long a; };
    struct TestComp6 { short b; };
    struct TestComp7 { bool c; };
    struct TestComp8 { int d[2]; };
    struct TestComp9 { float e[3]; };
    struct TestComp10 { double f[4]; };
    
    Astra::ComponentRegistry registry;
    
    // Register components multiple times (should be idempotent)
    for (int i = 0; i < 100; ++i)
    {
        registry.RegisterComponents<
            TestComp1, TestComp2, TestComp3, TestComp4, TestComp5,
            TestComp6, TestComp7, TestComp8, TestComp9, TestComp10
        >();
    }
    
    // Verify all are registered
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<TestComp1>::Value()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptor(Astra::TypeID<TestComp10>::Value()), nullptr);
}

// Test serialization metadata
TEST_F(ComponentRegistryTest, SerializationMetadata)
{
    Astra::ComponentRegistry registry;
    
    // Register a test component
    registry.RegisterComponent<Position>();
    
    Astra::ComponentID id = Astra::TypeID<Position>::Value();
    const Astra::ComponentDescriptor* desc = registry.GetComponentDescriptor(id);
    
    ASSERT_NE(desc, nullptr);
    
    // Check serialization metadata
    EXPECT_EQ(desc->hash, Astra::TypeID<Position>::Hash());
    EXPECT_NE(desc->name, nullptr);
    EXPECT_EQ(desc->version, 1u);  // Default version from SerializationTraits
    EXPECT_EQ(desc->minVersion, 1u);  // Default min version
    
    // Check serialization function pointers
    EXPECT_NE(desc->serialize, nullptr);
    EXPECT_NE(desc->deserialize, nullptr);
    EXPECT_NE(desc->serializeVersioned, nullptr);
    EXPECT_NE(desc->deserializeVersioned, nullptr);
    
    // Check hash lookup
    const Astra::ComponentDescriptor* descByHash = registry.GetComponentDescriptorByHash(desc->hash);
    EXPECT_EQ(descByHash, desc);
    
    // Check ID from hash
    auto idResult = registry.GetComponentIDFromHash(desc->hash);
    EXPECT_TRUE(idResult.IsOk());
    EXPECT_EQ(*idResult.GetValue(), id);
}

// Test serialization functions work correctly
TEST_F(ComponentRegistryTest, SerializationFunctions)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Health>();
    
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Health>::Value());
    ASSERT_NE(desc, nullptr);
    
    // Create a health component
    Health health{75, 100};
    
    // Test basic serialization
    std::vector<std::byte> buffer;
    {
        Astra::BinaryWriter writer(buffer);
        desc->serialize(writer, &health);
    }
    
    // Test basic deserialization
    {
        Health readHealth{0, 0};
        Astra::BinaryReader reader(buffer);
        desc->deserialize(reader, &readHealth);
        
        EXPECT_EQ(readHealth.current, 75);
        EXPECT_EQ(readHealth.max, 100);
    }
    
    // Test versioned serialization
    buffer.clear();
    {
        Astra::BinaryWriter writer(buffer);
        desc->serializeVersioned(writer, &health);
    }
    
    // Test versioned deserialization
    {
        Health readHealth{0, 0};
        Astra::BinaryReader reader(buffer);
        bool success = desc->deserializeVersioned(reader, &readHealth);
        
        EXPECT_TRUE(success);
        EXPECT_EQ(readHealth.current, 75);
        EXPECT_EQ(readHealth.max, 100);
    }
}

// Test multiple component registration with serialization
TEST_F(ComponentRegistryTest, MultipleComponentSerialization)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponents<Position, Velocity, Health, Name>();
    
    // Verify all have serialization metadata
    for (auto id : {Astra::TypeID<Position>::Value(), 
                     Astra::TypeID<Velocity>::Value(),
                     Astra::TypeID<Health>::Value(),
                     Astra::TypeID<Name>::Value()})
    {
        const auto* desc = registry.GetComponentDescriptor(id);
        ASSERT_NE(desc, nullptr);
        
        EXPECT_NE(desc->hash, 0u);
        EXPECT_NE(desc->name, nullptr);
        EXPECT_NE(desc->serialize, nullptr);
        EXPECT_NE(desc->deserialize, nullptr);
        EXPECT_NE(desc->serializeVersioned, nullptr);
        EXPECT_NE(desc->deserializeVersioned, nullptr);
    }
    
    // Verify hash lookups work
    EXPECT_NE(registry.GetComponentDescriptorByHash(Astra::TypeID<Position>::Hash()), nullptr);
    EXPECT_NE(registry.GetComponentDescriptorByHash(Astra::TypeID<Name>::Hash()), nullptr);
    
    // Test unknown hash
    EXPECT_EQ(registry.GetComponentDescriptorByHash(0xDEADBEEF), nullptr);
    auto badResult = registry.GetComponentIDFromHash(0xDEADBEEF);
    EXPECT_TRUE(badResult.IsErr());
}

// Test GetAllComponents and GetComponentCount
TEST_F(ComponentRegistryTest, ComponentEnumeration)
{
    Astra::ComponentRegistry registry;
    
    EXPECT_EQ(registry.Size(), 0u);
    
    registry.RegisterComponents<Position, Velocity, Health>();
    
    EXPECT_EQ(registry.Size(), 3u);
    
    const auto& allComponents = registry.GetAllComponentIDs();
    EXPECT_EQ(allComponents.Size(), 3u);
    
    // Verify each component is in the map
    EXPECT_TRUE(allComponents.Contains(Astra::TypeID<Position>::Value()));
    EXPECT_TRUE(allComponents.Contains(Astra::TypeID<Velocity>::Value()));
    EXPECT_TRUE(allComponents.Contains(Astra::TypeID<Health>::Value()));
}

// Probe types at namespace scope to avoid MSVC concept/name issues with local structs
namespace Astra_Test_ReReg
{
    struct ReRegProbe { int v; };
    struct IdemProbe  { int v; };
    struct FreshProbe { int v; };
}

TEST(ComponentRegistryReRegister, ReRegisterOverwritesDescriptor)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Astra_Test_ReReg::ReRegProbe>();
    const auto* before = registry.GetComponentDescriptor(Astra::TypeID<Astra_Test_ReReg::ReRegProbe>::Value());
    ASSERT_NE(before, nullptr);
    const auto id   = before->id;
    const auto hash = before->hash;

    // Re-registration must keep the same id (hash-stable via TypeContext)
    // and rebuild the descriptor (in a real reload, the function pointers
    // move into the newly loaded module).
    registry.ReRegisterComponent<Astra_Test_ReReg::ReRegProbe>();
    const auto* after = registry.GetComponentDescriptor(id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->id, id);
    EXPECT_EQ(after->hash, hash);
    EXPECT_NE(after->defaultConstruct, nullptr);
    EXPECT_NE(after->destruct, nullptr);
}

TEST(ComponentRegistryReRegister, RegisterRemainsIdempotent)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<Astra_Test_ReReg::IdemProbe>();
    const size_t count = registry.Size();
    registry.RegisterComponent<Astra_Test_ReReg::IdemProbe>();
    EXPECT_EQ(registry.Size(), count);
}

TEST(ComponentRegistryReRegister, ReRegisterOnUnregisteredActsAsRegister)
{
    Astra::ComponentRegistry registry;
    registry.ReRegisterComponent<Astra_Test_ReReg::FreshProbe>();
    const auto* desc = registry.GetComponentDescriptor(Astra::TypeID<Astra_Test_ReReg::FreshProbe>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(registry.Size(), 1u);
}