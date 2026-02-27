#pragma once

/**
 * @file Reflection.hpp
 * @brief Main include header for Astra's reflection system.
 *
 * The reflection system provides runtime type introspection for Astra types,
 * enabling features like:
 * - Field enumeration and dynamic access
 * - Type-erased get/set operations
 * - Attribute-based metadata (ranges, tooltips, categories)
 * - Enum to/from string conversion
 * - JSON Schema generation
 *
 * ## Quick Start
 *
 * @code
 * #include <Astra/Reflection/Reflection.hpp>
 *
 * // Define a component
 * struct Transform
 * {
 *     float x, y, z;
 *     float rotation;
 * };
 *
 * // Register it for reflection
 * ASTRA_REFLECT_TYPE(Transform)
 *     ASTRA_REFLECT_FIELD(Transform, x)
 *         ASTRA_REFLECT_ATTR(Range, -1000.0, 1000.0)
 *         ASTRA_REFLECT_ATTR(Tooltip, "X position in world space")
 *     ASTRA_REFLECT_FIELD(Transform, y)
 *     ASTRA_REFLECT_FIELD(Transform, z)
 *     ASTRA_REFLECT_FIELD(Transform, rotation)
 *         ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Degrees)
 * ASTRA_END_REFLECT_TYPE()
 *
 * // Use reflection at runtime
 * void InspectTransform(Transform& t)
 * {
 *     const auto* meta = Astra::GetMeta<Transform>();
 *     if (!meta) return;
 *
 *     meta->ForEachField([&](const Astra::FieldInfo& field) {
 *         if (field.isArithmetic)
 *         {
 *             float value = field.Get<float>(&t);
 *             std::cout << field.GetDisplayName() << " = " << value << "\n";
 *         }
 *     });
 * }
 * @endcode
 *
 * ## Enum Reflection
 *
 * @code
 * enum class DamageType { Physical, Fire, Ice, Lightning };
 *
 * ASTRA_REFLECT_ENUM(DamageType)
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Physical)
 *     ASTRA_REFLECT_ENUM_VALUE_NAMED(DamageType, Fire, "Fire Damage")
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Ice)
 *     ASTRA_REFLECT_ENUM_VALUE(DamageType, Lightning)
 * ASTRA_END_REFLECT_ENUM()
 *
 * // Use at runtime
 * DamageType type = DamageType::Fire;
 * const auto* meta = Astra::GetMeta<DamageType>();
 * auto name = meta->EnumToString(static_cast<int64_t>(type));
 * // name == "Fire"
 * @endcode
 *
 * ## Available Attributes
 *
 * - `Range`: Numeric min/max/step constraints
 * - `Hidden`: Hide field from UI
 * - `ReadOnly`: Make field read-only in UI
 * - `DisplayName`: Custom display name
 * - `Tooltip`: Help text for UI
 * - `Category`: Group fields in UI
 * - `Serializable`: Control serialization
 * - `ColorFormat`: RGB/RGBA/HSV color hints
 * - `AngleFormat`: Degrees/radians hints
 * - `Multiline`: Multiline string editing
 * - `FilePath`: File browser hints
 * - `DragSpeed`: Numeric drag speed
 * - `Deprecated`: Mark as deprecated
 * - `Precision`: Decimal places for display
 *
 * ## Integration with ECS
 *
 * The reflection system integrates with Astra's ECS through MetaRegistry:
 *
 * @code
 * // Link reflection metadata to component registration
 * auto& metaReg = Astra::MetaRegistry::Instance();
 * metaReg.LinkToComponent(Astra::TypeID<Transform>::Hash(),
 *                         Astra::TypeID<Transform>::Value());
 *
 * // Later, get component by hash
 * void* comp = registry.GetComponentByHash(entity, Astra::TypeID<Transform>::Hash());
 * const auto* meta = Astra::GetMeta<Transform>();
 * meta->ForEachField(...);
 * @endcode
 */

// Core reflection infrastructure
#include "Attribute.hpp"
#include "FieldInfo.hpp"
#include "EnumInfo.hpp"
#include "TypeMeta.hpp"
#include "MetaRegistry.hpp"

// Registration macros
#include "Macros.hpp"

// Type traits for containers
#include "ContainerTraits.hpp"

// JSON Schema generation
#include "JsonSchema.hpp"
