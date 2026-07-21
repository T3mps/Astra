#include <gtest/gtest.h>
#include <Astra/Reflection/AnyValue.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace
{
    // Test components
    struct ReflPosition
    {
        float x, y, z;
    };

    struct ReflVelocity
    {
        float dx, dy, dz;
    };

    struct ReflHealth
    {
        int current;
        int max;
        bool regenerating;
    };

    struct Player
    {
        std::string name;
        int level;
        float experience;
    };

    // Enum for testing
    enum class DamageType
    {
        Physical,
        Fire,
        Ice,
        Lightning
    };

    // Flags enum for testing
    enum class StatusFlags : uint32_t
    {
        None = 0,
        Poisoned = 1 << 0,
        Burning = 1 << 1,
        Frozen = 1 << 2,
        Stunned = 1 << 3
    };
}

// Reflect the test types - must be outside anonymous namespace for static initialization
ASTRA_REFLECT_TYPE(ReflPosition)
    ASTRA_REFLECT_FIELD(ReflPosition, x)
        ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
        ASTRA_REFLECT_ATTR(Tooltip, "X coordinate")
    ASTRA_REFLECT_FIELD(ReflPosition, y)
        ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
    ASTRA_REFLECT_FIELD(ReflPosition, z)
        ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(ReflVelocity)
    ASTRA_REFLECT_FIELD(ReflVelocity, dx)
    ASTRA_REFLECT_FIELD(ReflVelocity, dy)
    ASTRA_REFLECT_FIELD(ReflVelocity, dz)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(ReflHealth)
    ASTRA_REFLECT_FIELD(ReflHealth, current)
        ASTRA_REFLECT_ATTR(Range, 0.0, 10000.0)
        ASTRA_REFLECT_ATTR(DisplayName, "Current HP")
    ASTRA_REFLECT_FIELD(ReflHealth, max)
        ASTRA_REFLECT_ATTR(Range, 1.0, 10000.0)
        ASTRA_REFLECT_ATTR(DisplayName, "Maximum HP")
    ASTRA_REFLECT_FIELD(ReflHealth, regenerating)
        ASTRA_REFLECT_ATTR(Tooltip, "Whether health regenerates over time")
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(Player)
    ASTRA_REFLECT_FIELD(Player, name)
        ASTRA_REFLECT_ATTR(DisplayName, "Player Name")
    ASTRA_REFLECT_FIELD(Player, level)
        ASTRA_REFLECT_ATTR(Range, 1.0, 100.0)
    ASTRA_REFLECT_FIELD(Player, experience)
        ASTRA_REFLECT_ATTR(ReadOnly)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_ENUM(DamageType)
    ASTRA_REFLECT_ENUM_VALUE(DamageType, Physical)
    ASTRA_REFLECT_ENUM_VALUE_NAMED(DamageType, Fire, "Fire Damage")
    ASTRA_REFLECT_ENUM_VALUE(DamageType, Ice)
    ASTRA_REFLECT_ENUM_VALUE_FULL(DamageType, Lightning, "Lightning Damage", "Deals electrical damage")
ASTRA_END_REFLECT_ENUM()

ASTRA_REFLECT_ENUM(StatusFlags)
    ASTRA_REFLECT_ENUM_VALUE(StatusFlags, None)
    ASTRA_REFLECT_ENUM_VALUE(StatusFlags, Poisoned)
    ASTRA_REFLECT_ENUM_VALUE(StatusFlags, Burning)
    ASTRA_REFLECT_ENUM_VALUE(StatusFlags, Frozen)
    ASTRA_REFLECT_ENUM_VALUE(StatusFlags, Stunned)
    ASTRA_REFLECT_ENUM_FLAGS()
ASTRA_END_REFLECT_ENUM()

class ReflectionTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// TypeMeta Tests
// ============================================================================

TEST_F(ReflectionTest, TypeMetaRegistration)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    EXPECT_EQ(meta->typeHash, TypeID<ReflPosition>::Hash());
    EXPECT_NE(meta->typeName.find("ReflPosition"), std::string_view::npos);
    EXPECT_EQ(meta->size, sizeof(ReflPosition));
    EXPECT_EQ(meta->alignment, alignof(ReflPosition));
    EXPECT_TRUE(meta->isClass);
    EXPECT_FALSE(meta->isEnum);
    EXPECT_TRUE(meta->isTrivial);
}

TEST_F(ReflectionTest, TypeMetaLookupByHash)
{
    using namespace Astra;

    const TypeMeta* meta1 = GetMeta<ReflPosition>();
    const TypeMeta* meta2 = GetMeta(TypeID<ReflPosition>::Hash());

    ASSERT_NE(meta1, nullptr);
    ASSERT_NE(meta2, nullptr);
    EXPECT_EQ(meta1, meta2);
}

TEST_F(ReflectionTest, TypeMetaFields)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    EXPECT_EQ(meta->GetFieldCount(), 3u);
    EXPECT_TRUE(meta->HasField("x"));
    EXPECT_TRUE(meta->HasField("y"));
    EXPECT_TRUE(meta->HasField("z"));
    EXPECT_FALSE(meta->HasField("w"));
}

TEST_F(ReflectionTest, FieldInfoBasics)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* xField = meta->GetField("x");
    ASSERT_NE(xField, nullptr);

    EXPECT_EQ(xField->name, "x");
    EXPECT_EQ(xField->typeHash, TypeID<float>::Hash());
    EXPECT_EQ(xField->size, sizeof(float));
    EXPECT_EQ(xField->alignment, alignof(float));
    EXPECT_FALSE(xField->isConst);
    EXPECT_TRUE(xField->isArithmetic);
    EXPECT_FALSE(xField->isEnum);
}

// ============================================================================
// Field Access Tests
// ============================================================================

TEST_F(ReflectionTest, FieldGetSet)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    ReflPosition pos{1.0f, 2.0f, 3.0f};

    const FieldInfo* xField = meta->GetField("x");
    ASSERT_NE(xField, nullptr);

    // Test Get
    float x = xField->Get<float>(&pos);
    EXPECT_FLOAT_EQ(x, 1.0f);

    // Test Set
    xField->Set<float>(&pos, 100.0f);
    EXPECT_FLOAT_EQ(pos.x, 100.0f);
}

TEST_F(ReflectionTest, FieldGetPtr)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    ReflPosition pos{1.0f, 2.0f, 3.0f};

    const FieldInfo* yField = meta->GetField("y");
    ASSERT_NE(yField, nullptr);

    float* yPtr = yField->GetPtr<float>(&pos);
    EXPECT_FLOAT_EQ(*yPtr, 2.0f);

    *yPtr = 200.0f;
    EXPECT_FLOAT_EQ(pos.y, 200.0f);
}

TEST_F(ReflectionTest, FieldGetSetAny)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    ReflPosition pos{1.0f, 2.0f, 3.0f};

    const FieldInfo* zField = meta->GetField("z");
    ASSERT_NE(zField, nullptr);

    // Get as any
    std::any zAny = zField->GetAny(&pos);
    ASSERT_TRUE(zAny.has_value());
    EXPECT_FLOAT_EQ(std::any_cast<float>(zAny), 3.0f);

    // Set from any
    bool success = zField->SetAny(&pos, std::any(300.0f));
    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(pos.z, 300.0f);
}

TEST_F(ReflectionTest, FieldValueByName)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflHealth>();
    ASSERT_NE(meta, nullptr);

    ReflHealth health{100, 200, true};

    // Get value by name
    auto current = meta->GetFieldValue<int>(&health, "current");
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(*current, 100);

    // Set value by name
    bool success = meta->SetFieldValue<int>(&health, "max", 500);
    EXPECT_TRUE(success);
    EXPECT_EQ(health.max, 500);

    // Non-existent field
    auto nonexistent = meta->GetFieldValue<int>(&health, "mana");
    EXPECT_FALSE(nonexistent.has_value());
}

TEST_F(ReflectionTest, ForEachField)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    ReflPosition pos{1.0f, 2.0f, 3.0f};

    int fieldCount = 0;
    float sum = 0.0f;

    meta->ForEachField([&](const FieldInfo& field) {
        ++fieldCount;
        sum += field.Get<float>(&pos);
    });

    EXPECT_EQ(fieldCount, 3);
    EXPECT_FLOAT_EQ(sum, 6.0f);
}

// ============================================================================
// Attribute Tests
// ============================================================================

TEST_F(ReflectionTest, FieldAttributes)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* xField = meta->GetField("x");
    ASSERT_NE(xField, nullptr);

    // Check Range attribute
    const Range* range = xField->GetAttribute<Range>();
    ASSERT_NE(range, nullptr);
    EXPECT_DOUBLE_EQ(range->min, -1000.0);
    EXPECT_DOUBLE_EQ(range->max, 1000.0);

    // Check Tooltip attribute
    const Tooltip* tooltip = xField->GetAttribute<Tooltip>();
    ASSERT_NE(tooltip, nullptr);
    EXPECT_EQ(tooltip->text, "X coordinate");

    // Check HasAttribute
    EXPECT_TRUE(xField->HasAttribute<Range>());
    EXPECT_TRUE(xField->HasAttribute<Tooltip>());
    EXPECT_FALSE(xField->HasAttribute<Hidden>());
}

TEST_F(ReflectionTest, FieldDisplayName)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflHealth>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* currentField = meta->GetField("current");
    ASSERT_NE(currentField, nullptr);

    // Has custom display name
    EXPECT_EQ(currentField->GetDisplayName(), "Current HP");

    // Tooltip
    const FieldInfo* regenField = meta->GetField("regenerating");
    ASSERT_NE(regenField, nullptr);
    EXPECT_EQ(regenField->GetTooltip(), "Whether health regenerates over time");
}

TEST_F(ReflectionTest, ReadOnlyAttribute)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<Player>();
    ASSERT_NE(meta, nullptr);

    const FieldInfo* expField = meta->GetField("experience");
    ASSERT_NE(expField, nullptr);

    EXPECT_TRUE(expField->HasAttribute<ReadOnly>());
    EXPECT_TRUE(expField->IsReadOnly());

    const FieldInfo* levelField = meta->GetField("level");
    ASSERT_NE(levelField, nullptr);
    EXPECT_FALSE(levelField->IsReadOnly());
}

// ============================================================================
// Enum Tests
// ============================================================================

TEST_F(ReflectionTest, EnumRegistration)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    EXPECT_TRUE(meta->isEnum);
    ASSERT_NE(meta->enumInfo, nullptr);
    EXPECT_EQ(meta->enumInfo->Count(), 4u);
    EXPECT_FALSE(meta->enumInfo->isFlags);
}

TEST_F(ReflectionTest, EnumToString)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    auto name = meta->EnumToString(static_cast<int64_t>(DamageType::Fire));
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "Fire");

    auto unknown = meta->EnumToString(999);
    EXPECT_FALSE(unknown.has_value());
}

TEST_F(ReflectionTest, EnumFromString)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    auto value = meta->EnumFromString("Ice");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, static_cast<int64_t>(DamageType::Ice));

    auto unknown = meta->EnumFromString("Poison");
    EXPECT_FALSE(unknown.has_value());
}

TEST_F(ReflectionTest, EnumDisplayName)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    auto display = meta->enumInfo->GetDisplayName(static_cast<int64_t>(DamageType::Fire));
    ASSERT_TRUE(display.has_value());
    EXPECT_EQ(*display, "Fire Damage");

    // Physical doesn't have a custom display name
    auto physDisplay = meta->enumInfo->GetDisplayName(static_cast<int64_t>(DamageType::Physical));
    ASSERT_TRUE(physDisplay.has_value());
    EXPECT_EQ(*physDisplay, "Physical"); // Falls back to code name
}

TEST_F(ReflectionTest, EnumDescription)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    auto desc = meta->enumInfo->GetDescription(static_cast<int64_t>(DamageType::Lightning));
    EXPECT_EQ(desc, "Deals electrical damage");
}

TEST_F(ReflectionTest, FlagsEnum)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<StatusFlags>();
    ASSERT_NE(meta, nullptr);

    EXPECT_TRUE(meta->isEnum);
    ASSERT_NE(meta->enumInfo, nullptr);
    EXPECT_TRUE(meta->enumInfo->isFlags);
}

TEST_F(ReflectionTest, EnumValidation)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<DamageType>();
    ASSERT_NE(meta, nullptr);

    EXPECT_TRUE(meta->enumInfo->IsValid(static_cast<int64_t>(DamageType::Fire)));
    EXPECT_FALSE(meta->enumInfo->IsValid(999));
}

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST_F(ReflectionTest, DefaultConstruct)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    alignas(ReflPosition) std::byte storage[sizeof(ReflPosition)];

    bool success = meta->Construct(storage);
    EXPECT_TRUE(success);

    // Default constructed - values should be initialized
    // (C++ default initialization for float is indeterminate, but we use value-init)

    meta->Destruct(storage);
}

TEST_F(ReflectionTest, CopyConstruct)
{
    using namespace Astra;

    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    ReflPosition src{1.0f, 2.0f, 3.0f};
    alignas(ReflPosition) std::byte storage[sizeof(ReflPosition)];

    bool success = meta->CopyConstruct(storage, &src);
    EXPECT_TRUE(success);

    ReflPosition* pos = reinterpret_cast<ReflPosition*>(storage);
    EXPECT_FLOAT_EQ(pos->x, 1.0f);
    EXPECT_FLOAT_EQ(pos->y, 2.0f);
    EXPECT_FLOAT_EQ(pos->z, 3.0f);

    meta->Destruct(storage);
}

// ============================================================================
// Container Traits Tests
// ============================================================================

TEST_F(ReflectionTest, ContainerTraitsVector)
{
    using namespace Astra;

    EXPECT_TRUE(IsContainer_v<std::vector<int>>);
    EXPECT_TRUE(IsSequenceContainer_v<std::vector<int>>);
    EXPECT_FALSE(IsAssociativeContainer_v<std::vector<int>>);
    EXPECT_TRUE(HasContiguousStorage_v<std::vector<int>>);
    EXPECT_FALSE(HasFixedSize_v<std::vector<int>>);
}

TEST_F(ReflectionTest, ContainerTraitsArray)
{
    using namespace Astra;

    // Use typedef to avoid comma-in-macro issues
    using IntArray5 = std::array<int, 5>;

    EXPECT_TRUE(IsContainer_v<IntArray5>);
    EXPECT_TRUE(IsSequenceContainer_v<IntArray5>);
    EXPECT_TRUE(HasFixedSize_v<IntArray5>);
    EXPECT_EQ(ContainerTraits<IntArray5>::FixedSize, 5u);
}

TEST_F(ReflectionTest, ContainerTraitsMap)
{
    using namespace Astra;

    // Use typedef to avoid comma-in-macro issues
    using IntStringMap = std::map<int, std::string>;

    EXPECT_TRUE(IsContainer_v<IntStringMap>);
    EXPECT_FALSE(IsSequenceContainer_v<IntStringMap>);
    EXPECT_TRUE(IsAssociativeContainer_v<IntStringMap>);
    EXPECT_TRUE(IsMapContainer_v<IntStringMap>);
    EXPECT_FALSE(IsSetContainer_v<IntStringMap>);
}

TEST_F(ReflectionTest, NonContainerType)
{
    using namespace Astra;

    EXPECT_FALSE(IsContainer_v<int>);
    EXPECT_FALSE(IsContainer_v<ReflPosition>);
}

// ============================================================================
// JSON Schema Tests
// ============================================================================

TEST_F(ReflectionTest, JsonSchemaGeneration)
{
    using namespace Astra;

    std::string schema = GenerateJsonSchema<ReflPosition>();
    EXPECT_FALSE(schema.empty());

    // Check for expected content
    EXPECT_NE(schema.find("\"$schema\""), std::string::npos);
    EXPECT_NE(schema.find("ReflPosition"), std::string::npos);
    EXPECT_NE(schema.find("\"properties\""), std::string::npos);
    EXPECT_NE(schema.find("\"x\""), std::string::npos);
    EXPECT_NE(schema.find("\"y\""), std::string::npos);
    EXPECT_NE(schema.find("\"z\""), std::string::npos);
}

TEST_F(ReflectionTest, JsonSchemaWithRange)
{
    using namespace Astra;

    std::string schema = GenerateJsonSchema<ReflPosition>();

    // Check for range constraints
    EXPECT_NE(schema.find("\"minimum\""), std::string::npos);
    EXPECT_NE(schema.find("\"maximum\""), std::string::npos);
    EXPECT_NE(schema.find("-1000"), std::string::npos);
    EXPECT_NE(schema.find("1000"), std::string::npos);
}

TEST_F(ReflectionTest, JsonSchemaWithTooltip)
{
    using namespace Astra;

    std::string schema = GenerateJsonSchema<ReflPosition>();

    // Check for description from tooltip
    EXPECT_NE(schema.find("\"description\""), std::string::npos);
    EXPECT_NE(schema.find("X coordinate"), std::string::npos);
}

// ============================================================================
// Registry Integration Tests
// ============================================================================

TEST_F(ReflectionTest, RegistryGetComponentByHash)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();

    Entity entity = registry.CreateEntityWith(ReflPosition{10.0f, 20.0f, 30.0f});

    void* comp = registry.GetComponentByHash(entity, TypeID<ReflPosition>::Hash());
    ASSERT_NE(comp, nullptr);

    ReflPosition* pos = static_cast<ReflPosition*>(comp);
    EXPECT_FLOAT_EQ(pos->x, 10.0f);
    EXPECT_FLOAT_EQ(pos->y, 20.0f);
    EXPECT_FLOAT_EQ(pos->z, 30.0f);
}

TEST_F(ReflectionTest, RegistryHasComponentByHash)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();
    registry.GetComponentRegistry()->RegisterComponent<ReflVelocity>();

    Entity entity = registry.CreateEntityWith(ReflPosition{1.0f, 2.0f, 3.0f});

    EXPECT_TRUE(registry.HasComponentByHash(entity, TypeID<ReflPosition>::Hash()));
    EXPECT_FALSE(registry.HasComponentByHash(entity, TypeID<ReflVelocity>::Hash()));
}

TEST_F(ReflectionTest, RegistryGetEntityComponents)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();
    registry.GetComponentRegistry()->RegisterComponent<ReflVelocity>();

    Entity entity = registry.CreateEntityWith(
        ReflPosition{1.0f, 2.0f, 3.0f},
        ReflVelocity{0.1f, 0.2f, 0.3f}
    );

    auto components = registry.GetEntityComponents(entity);
    EXPECT_EQ(components.size(), 2u);

    // Verify we got the right components (order may vary)
    bool hasPosition = false;
    bool hasVelocity = false;
    for (const auto* desc : components)
    {
        if (desc->hash == TypeID<ReflPosition>::Hash()) hasPosition = true;
        if (desc->hash == TypeID<ReflVelocity>::Hash()) hasVelocity = true;
    }
    EXPECT_TRUE(hasPosition);
    EXPECT_TRUE(hasVelocity);
}

TEST_F(ReflectionTest, ReflectionWithECS)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();

    Entity entity = registry.CreateEntityWith(ReflPosition{1.0f, 2.0f, 3.0f});

    // Get component through reflection
    void* comp = registry.GetComponentByHash(entity, TypeID<ReflPosition>::Hash());
    ASSERT_NE(comp, nullptr);

    // Use TypeMeta to access fields
    const TypeMeta* meta = GetMeta<ReflPosition>();
    ASSERT_NE(meta, nullptr);

    // Read field through reflection
    auto xValue = meta->GetFieldValue<float>(comp, "x");
    ASSERT_TRUE(xValue.has_value());
    EXPECT_FLOAT_EQ(*xValue, 1.0f);

    // Modify field through reflection
    bool success = meta->SetFieldValue<float>(comp, "y", 100.0f);
    EXPECT_TRUE(success);

    // Verify modification
    ReflPosition* pos = static_cast<ReflPosition*>(comp);
    EXPECT_FLOAT_EQ(pos->y, 100.0f);
}

// ============================================================================
// MetaRegistry Tests
// ============================================================================

TEST_F(ReflectionTest, MetaRegistrySingleton)
{
    using namespace Astra;

    MetaRegistry& reg1 = MetaRegistry::Instance();
    MetaRegistry& reg2 = MetaRegistry::Instance();

    EXPECT_EQ(&reg1, &reg2);
}

TEST_F(ReflectionTest, IsReflected)
{
    using namespace Astra;

    EXPECT_TRUE(IsReflected<ReflPosition>());
    EXPECT_TRUE(IsReflected<ReflHealth>());
    EXPECT_TRUE(IsReflected<DamageType>());

    // Type that hasn't been registered
    struct UnreflectedType { int x; };
    EXPECT_FALSE(IsReflected<UnreflectedType>());
}

TEST_F(ReflectionTest, MetaRegistryCount)
{
    using namespace Astra;

    size_t count = MetaRegistry::Instance().GetRegisteredCount();
    // We registered ReflPosition, ReflVelocity, ReflHealth, Player, DamageType, StatusFlags
    EXPECT_GE(count, 6u);
}

// ============================================================================
// ECS Integration Tests
// ============================================================================

TEST_F(ReflectionTest, ComponentDescriptorHasTypeMeta)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();

    const ComponentDescriptor* desc = registry.GetComponentRegistry()->GetComponentDescriptor(TypeID<ReflPosition>::Value());
    ASSERT_NE(desc, nullptr);

    // ComponentDescriptor should have TypeMeta linked
    EXPECT_NE(desc->meta, nullptr);
    EXPECT_EQ(desc->meta->typeHash, TypeID<ReflPosition>::Hash());
    EXPECT_EQ(desc->meta->typeName, TypeID<ReflPosition>::Name());
}

TEST_F(ReflectionTest, MetaRegistryLinkToComponent)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();

    // Should be able to get ComponentID from type hash
    ComponentID id = MetaRegistry::Instance().GetComponentId(TypeID<ReflPosition>::Hash());
    EXPECT_EQ(id, TypeID<ReflPosition>::Value());

    // Should be able to get type hash from ComponentID
    uint64_t hash = MetaRegistry::Instance().GetTypeHash(TypeID<ReflPosition>::Value());
    EXPECT_EQ(hash, TypeID<ReflPosition>::Hash());
}

TEST_F(ReflectionTest, MetaRegistryGetByComponentId)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();

    // Should be able to get TypeMeta from ComponentID
    const TypeMeta* meta = MetaRegistry::Instance().GetByComponentId(TypeID<ReflPosition>::Value());
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->typeHash, TypeID<ReflPosition>::Hash());

    // Also test convenience function
    const TypeMeta* meta2 = GetMeta(TypeID<ReflPosition>::Value());
    EXPECT_EQ(meta, meta2);
}

TEST_F(ReflectionTest, InspectEntity)
{
    using namespace Astra;

    Registry registry;
    registry.GetComponentRegistry()->RegisterComponent<ReflPosition>();
    registry.GetComponentRegistry()->RegisterComponent<ReflVelocity>();

    Entity entity = registry.CreateEntityWith(
        ReflPosition{10.0f, 20.0f, 30.0f},
        ReflVelocity{1.0f, 2.0f, 3.0f}
    );

    auto components = registry.InspectEntity(entity);
    EXPECT_EQ(components.size(), 2u);

    // Find the ReflPosition component info
    const Registry::ComponentInfo* posInfo = nullptr;
    const Registry::ComponentInfo* velInfo = nullptr;
    for (const auto& info : components)
    {
        if (info.descriptor->hash == TypeID<ReflPosition>::Hash())
            posInfo = &info;
        if (info.descriptor->hash == TypeID<ReflVelocity>::Hash())
            velInfo = &info;
    }

    // Verify ReflPosition
    ASSERT_NE(posInfo, nullptr);
    ASSERT_NE(posInfo->descriptor, nullptr);
    ASSERT_NE(posInfo->meta, nullptr);
    ASSERT_NE(posInfo->data, nullptr);

    // Use reflection to read ReflPosition fields
    auto xValue = posInfo->meta->GetFieldValue<float>(posInfo->data, "x");
    ASSERT_TRUE(xValue.has_value());
    EXPECT_FLOAT_EQ(*xValue, 10.0f);

    // Verify ReflVelocity
    ASSERT_NE(velInfo, nullptr);
    ASSERT_NE(velInfo->descriptor, nullptr);
    ASSERT_NE(velInfo->meta, nullptr);
    ASSERT_NE(velInfo->data, nullptr);

    // Use reflection to modify ReflVelocity
    bool success = velInfo->meta->SetFieldValue<float>(velInfo->data, "dx", 100.0f);
    EXPECT_TRUE(success);

    // Verify the change
    auto* vel = registry.GetComponent<ReflVelocity>(entity);
    EXPECT_FLOAT_EQ(vel->dx, 100.0f);
}

TEST_F(ReflectionTest, InspectResources)
{
    using namespace Astra;

    Registry registry;

    // Set a resource
    registry.SetResource(ReflPosition{1.0f, 2.0f, 3.0f});

    auto resources = registry.InspectResources();
    EXPECT_EQ(resources.size(), 1u);

    // Find the ReflPosition resource
    ASSERT_EQ(resources.size(), 1u);
    const auto& resInfo = resources[0];

    ASSERT_NE(resInfo.descriptor, nullptr);
    ASSERT_NE(resInfo.meta, nullptr);
    ASSERT_NE(resInfo.data, nullptr);

    EXPECT_EQ(resInfo.descriptor->hash, TypeID<ReflPosition>::Hash());

    // Use reflection to read resource fields
    auto zValue = resInfo.meta->GetFieldValue<float>(resInfo.data, "z");
    ASSERT_TRUE(zValue.has_value());
    EXPECT_FLOAT_EQ(*zValue, 3.0f);
}

TEST_F(ReflectionTest, ResourceReflectionByHash)
{
    using namespace Astra;

    Registry registry;
    registry.SetResource(ReflPosition{5.0f, 10.0f, 15.0f});

    // Get resource by hash
    void* res = registry.GetResourceByHash(TypeID<ReflPosition>::Hash());
    ASSERT_NE(res, nullptr);

    ReflPosition* pos = static_cast<ReflPosition*>(res);
    EXPECT_FLOAT_EQ(pos->x, 5.0f);
    EXPECT_FLOAT_EQ(pos->y, 10.0f);
    EXPECT_FLOAT_EQ(pos->z, 15.0f);

    // Has resource by hash
    EXPECT_TRUE(registry.HasResourceByHash(TypeID<ReflPosition>::Hash()));
    EXPECT_FALSE(registry.HasResourceByHash(TypeID<ReflVelocity>::Hash()));
}

TEST_F(ReflectionTest, GetAllResources)
{
    using namespace Astra;

    Registry registry;
    registry.SetResource(ReflPosition{1.0f, 2.0f, 3.0f});
    registry.SetResource(ReflVelocity{0.1f, 0.2f, 0.3f});

    auto resources = registry.GetAllResources();
    EXPECT_EQ(resources.size(), 2u);

    bool hasPosition = false;
    bool hasVelocity = false;
    for (const auto* desc : resources)
    {
        if (desc->hash == TypeID<ReflPosition>::Hash()) hasPosition = true;
        if (desc->hash == TypeID<ReflVelocity>::Hash()) hasVelocity = true;
    }
    EXPECT_TRUE(hasPosition);
    EXPECT_TRUE(hasVelocity);
}

TEST_F(ReflectionTest, FullInspectionWorkflow)
{
    // This test demonstrates a complete editor-style inspection workflow
    using namespace Astra;

    Registry registry;

    // Create an entity with components
    Entity entity = registry.CreateEntityWith(
        ReflPosition{100.0f, 200.0f, 300.0f},
        ReflHealth{80, 100, true}
    );

    // Inspect the entity (like an editor would)
    auto components = registry.InspectEntity(entity);

    for (const auto& info : components)
    {
        // Skip if no reflection metadata
        if (!info.meta) continue;

        // Iterate through all fields
        info.meta->ForEachField([&](const FieldInfo& field) {
            // Get display name (falls back to code name)
            std::string_view displayName = field.GetDisplayName();
            EXPECT_FALSE(displayName.empty());

            // Check if field has range constraint
            if (const Range* range = field.GetRange())
            {
                EXPECT_LE(range->min, range->max);
            }

            // Check if field is read-only
            bool readOnly = field.IsReadOnly();
            (void)readOnly;  // Just verifying it compiles and runs

            // Get field value through reflection
            std::any value = field.GetAny(info.data);
            EXPECT_TRUE(value.has_value());
        });
    }
}

namespace
{
    // File-local lifetime counter (NOT a component: AnyValue uses TypeID<T>::Hash(),
    // which does not assign a dense ComponentID, so this consumes zero ceiling).
    struct AnyLifetime
    {
        static inline int s_live = 0;
        int tag = 0;
        AnyLifetime() { ++s_live; }
        explicit AnyLifetime(int t) : tag(t) { ++s_live; }
        AnyLifetime(const AnyLifetime& o) : tag(o.tag) { ++s_live; }
        AnyLifetime(AnyLifetime&& o) noexcept : tag(o.tag) { ++s_live; }
        AnyLifetime& operator=(const AnyLifetime&) = default;
        AnyLifetime& operator=(AnyLifetime&&) = default;
        ~AnyLifetime() { --s_live; }
    };

    // Larger than AnyValue's 16-byte inline buffer -> forces the heap path.
    struct AnyBig { double a, b, c, d; };   // 32 bytes

    // Over-aligned -> forces the aligned-heap path.
    struct alignas(64) AnyOver64 { std::byte pad[64]; };
}

TEST(AnyValueTest, ConstructAndTryCastMatch)
{
    Astra::AnyValue v(42);
    ASSERT_TRUE(v.HasValue());
    ASSERT_NE(v.TryCast<int>(), nullptr);
    EXPECT_EQ(*v.TryCast<int>(), 42);
    EXPECT_EQ(v.TypeHash(), Astra::TypeID<int>::Hash());
}

TEST(AnyValueTest, TryCastWrongTypeReturnsNull)
{
    Astra::AnyValue v(3.5f);
    EXPECT_NE(v.TryCast<float>(), nullptr);
    EXPECT_EQ(v.TryCast<int>(), nullptr);       // wrong T -> null, never UB
    EXPECT_EQ(v.TryCast<double>(), nullptr);
}

TEST(AnyValueTest, EmptyHasNoValue)
{
    Astra::AnyValue v;
    EXPECT_FALSE(v.HasValue());
    EXPECT_EQ(v.TryCast<int>(), nullptr);
}

TEST(AnyValueTest, HeapPathForLargeType)
{
    Astra::AnyValue v(AnyBig{1, 2, 3, 4});
    ASSERT_NE(v.TryCast<AnyBig>(), nullptr);
    EXPECT_EQ(v.TryCast<AnyBig>()->c, 3.0);
}

TEST(AnyValueTest, OverAlignedHeapPathIsAligned)
{
    Astra::AnyValue v{AnyOver64{}};
    ASSERT_NE(v.TryCast<AnyOver64>(), nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(v.TryCast<AnyOver64>()) % alignof(AnyOver64), 0u);
}

TEST(AnyValueTest, CopyIsIndependentAndBalancesLifetime)
{
    AnyLifetime::s_live = 0;
    {
        Astra::AnyValue a(AnyLifetime{7});
        Astra::AnyValue b = a;                  // copy
        ASSERT_NE(a.TryCast<AnyLifetime>(), nullptr);
        ASSERT_NE(b.TryCast<AnyLifetime>(), nullptr);
        EXPECT_EQ(b.TryCast<AnyLifetime>()->tag, 7);
        EXPECT_NE(a.TryCast<AnyLifetime>(), b.TryCast<AnyLifetime>());  // distinct storage
    }
    EXPECT_EQ(AnyLifetime::s_live, 0);          // no leak, no double-free
}

TEST(AnyValueTest, MoveTransfersAndLeavesSourceEmpty)
{
    AnyLifetime::s_live = 0;
    {
        Astra::AnyValue a(AnyLifetime{9});
        Astra::AnyValue b = std::move(a);
        EXPECT_FALSE(a.HasValue());
        ASSERT_NE(b.TryCast<AnyLifetime>(), nullptr);
        EXPECT_EQ(b.TryCast<AnyLifetime>()->tag, 9);
    }
    EXPECT_EQ(AnyLifetime::s_live, 0);
}
