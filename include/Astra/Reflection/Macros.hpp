#pragma once

#include "MetaRegistry.hpp"
#include "TypeMeta.hpp"
#include "EnumInfo.hpp"

/**
 * @file Macros.hpp
 * @brief Macros for registering types, fields, and enums with the reflection system.
 *
 * Usage example:
 * @code
 * struct Transform
 * {
 *     float x, y, z;
 *     float rotation;
 * };
 *
 * ASTRA_REFLECT_TYPE(Transform)
 *     ASTRA_REFLECT_FIELD(Transform, x)
 *         ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
 *         ASTRA_REFLECT_ATTR(Tooltip, "X position")
 *     ASTRA_REFLECT_FIELD(Transform, y)
 *         ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
 *     ASTRA_REFLECT_FIELD(Transform, z)
 *         ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
 *     ASTRA_REFLECT_FIELD(Transform, rotation)
 *         ASTRA_REFLECT_ATTR(Range, 0.0, 360.0)
 *         ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Degrees)
 * ASTRA_END_REFLECT_TYPE()
 *
 * enum class DamageType { Physical, Fire, Ice, Lightning };
 *
 * ASTRA_REFLECT_ENUM(DamageType)
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Physical)
 *     ASTRA_REFLECT_ENUM_VALUE_NAMED(DamageType, Fire, "Fire Damage")
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Ice)
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Lightning)
 * ASTRA_END_REFLECT_ENUM()
 * @endcode
 *
 * IMPORTANT: All type and enum reflection blocks MUST be terminated with
 * ASTRA_END_REFLECT_TYPE() or ASTRA_END_REFLECT_ENUM() respectively.
 */

// ============================================================================
// Internal helpers
// ============================================================================

// Concatenate tokens
#define ASTRA_CONCAT_IMPL(a, b) a##b
#define ASTRA_CONCAT(a, b) ASTRA_CONCAT_IMPL(a, b)

// Generate unique variable name
#define ASTRA_UNIQUE_NAME(prefix) ASTRA_CONCAT(prefix, __LINE__)

// ============================================================================
// Type reflection macros
// ============================================================================

/**
 * Begins type reflection registration.
 * Must be followed by field registrations and terminated with a semicolon.
 *
 * @param Type The type to reflect
 */
#define ASTRA_REFLECT_TYPE(Type) \
    static inline ::Astra::Detail::StaticTypeRegistrar<Type> \
        ASTRA_UNIQUE_NAME(_astra_reflect_##Type##_) = \
            ::Astra::Detail::StaticTypeRegistrar<Type>( \
                [](::Astra::Detail::TypeMetaBuilder<Type>& _astra_builder_) { \
                    (void)_astra_builder_

/**
 * Registers a field with the type being reflected.
 * Must be used within ASTRA_REFLECT_TYPE block.
 *
 * @param Type The containing type
 * @param FieldName The name of the field member
 */
#define ASTRA_REFLECT_FIELD(Type, FieldName) \
    ; _astra_builder_.Field<decltype(Type::FieldName), &Type::FieldName>(#FieldName)

/**
 * Adds an attribute to the previously registered field.
 * Must be used after ASTRA_REFLECT_FIELD.
 *
 * @param AttrType The attribute type (from Astra namespace)
 * @param ... Constructor arguments for the attribute
 */
#define ASTRA_REFLECT_ATTR(AttrType, ...) \
    .Attr<::Astra::AttrType>(__VA_ARGS__)

/**
 * Adds a type-level attribute.
 * Must be used within ASTRA_REFLECT_TYPE block.
 *
 * @param AttrType The attribute type (from Astra namespace)
 * @param ... Constructor arguments for the attribute
 */
#define ASTRA_REFLECT_TYPE_ATTR(AttrType, ...) \
    ; _astra_builder_.TypeAttr<::Astra::AttrType>(__VA_ARGS__)

/**
 * Ends type reflection registration.
 * Alternative to using a semicolon if you prefer explicit endings.
 */
#define ASTRA_REFLECT_TYPE_END() \
    ; });

// ============================================================================
// Enum reflection macros
// ============================================================================

/**
 * Begins enum reflection registration.
 * Must be followed by enum value registrations and terminated with a semicolon.
 *
 * @param EnumType The enum type to reflect
 */
#define ASTRA_REFLECT_ENUM(EnumType) \
    static inline ::Astra::Detail::StaticTypeRegistrar<EnumType> \
        ASTRA_UNIQUE_NAME(_astra_reflect_enum_##EnumType##_) = \
            ::Astra::Detail::StaticTypeRegistrar<EnumType>( \
                [](::Astra::Detail::TypeMetaBuilder<EnumType>& _astra_builder_) { \
                    ::Astra::Detail::EnumInfoBuilder<EnumType> _astra_enum_builder_; \
                    (void)_astra_enum_builder_

/**
 * Registers an enum value.
 * Must be used within ASTRA_REFLECT_ENUM block.
 *
 * @param EnumType The enum type
 * @param ValueName The enum value name
 */
#define ASTRA_REFLECT_ENUM_VALUE(EnumType, ValueName) \
    ; _astra_enum_builder_.Value(#ValueName, EnumType::ValueName)

/**
 * Registers an enum value with a custom display name.
 * Must be used within ASTRA_REFLECT_ENUM block.
 *
 * @param EnumType The enum type
 * @param ValueName The enum value name
 * @param DisplayName The display name string
 */
#define ASTRA_REFLECT_ENUM_VALUE_NAMED(EnumType, ValueName, DisplayName) \
    ; _astra_enum_builder_.Value(#ValueName, EnumType::ValueName, DisplayName)

/**
 * Registers an enum value with a custom display name and description.
 * Must be used within ASTRA_REFLECT_ENUM block.
 *
 * @param EnumType The enum type
 * @param ValueName The enum value name
 * @param DisplayName The display name string
 * @param Description The description string
 */
#define ASTRA_REFLECT_ENUM_VALUE_FULL(EnumType, ValueName, DisplayName, Description) \
    ; _astra_enum_builder_.Value(#ValueName, EnumType::ValueName, DisplayName, Description)

/**
 * Marks the enum as a flags/bitmask enum.
 * Must be used within ASTRA_REFLECT_ENUM block.
 */
#define ASTRA_REFLECT_ENUM_FLAGS() \
    ; _astra_enum_builder_.Flags()

/**
 * Ends enum reflection registration and registers with the MetaRegistry.
 * Alternative to using a semicolon if you prefer explicit endings.
 */
#define ASTRA_REFLECT_ENUM_END() \
    ; _astra_builder_.Enum(_astra_enum_builder_.Build()); });

// ============================================================================
// Alternative syntax macros (for those who prefer explicit endings)
// ============================================================================

/**
 * Alternative ending for ASTRA_REFLECT_ENUM that doesn't require a semicolon.
 * Builds the enum info and registers it with the type metadata.
 */
#define ASTRA_END_REFLECT_ENUM() \
    ; _astra_builder_.Enum(_astra_enum_builder_.Build()); });

/**
 * Alternative ending for ASTRA_REFLECT_TYPE that doesn't require a semicolon.
 */
#define ASTRA_END_REFLECT_TYPE() \
    ; });

// ============================================================================
// Manual registration helpers
// ============================================================================

namespace Astra
{
    /**
     * Manually registers a type with the reflection system.
     * Use this for types where macros are inconvenient (e.g., templates).
     *
     * @tparam T The type to register
     * @param builderFunc Function that configures the TypeMetaBuilder
     * @return Pointer to the registered TypeMeta
     */
    template<typename T, typename BuilderFunc>
    inline TypeMeta* ReflectType(BuilderFunc&& builderFunc)
    {
        Detail::TypeMetaBuilder<T> builder;
        builderFunc(builder);
        return MetaRegistry::Instance().Register(builder.Build());
    }

    /**
     * Manually registers an enum with the reflection system.
     *
     * @tparam EnumType The enum type to register
     * @param enumBuilderFunc Function that configures the EnumInfoBuilder
     * @return Pointer to the registered TypeMeta
     */
    template<typename EnumType, typename EnumBuilderFunc>
    inline TypeMeta* ReflectEnum(EnumBuilderFunc&& enumBuilderFunc)
    {
        Detail::TypeMetaBuilder<EnumType> builder;
        Detail::EnumInfoBuilder<EnumType> enumBuilder;
        enumBuilderFunc(enumBuilder);
        builder.Enum(enumBuilder.Build());
        return MetaRegistry::Instance().Register(builder.Build());
    }
}
