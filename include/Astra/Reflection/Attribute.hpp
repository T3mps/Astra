#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "../Core/TypeID.hpp"

namespace Astra
{
    /**
     * Base class for all reflection attributes.
     * Attributes provide metadata about types and fields for runtime introspection.
     */
    class Attribute
    {
    public:
        virtual ~Attribute() = default;

        /**
         * Returns the type hash of the concrete attribute class.
         * Used for type-safe attribute queries.
         */
        ASTRA_NODISCARD virtual uint64_t GetTypeHash() const noexcept = 0;

        /**
         * Returns the attribute type name for debugging/serialization.
         */
        ASTRA_NODISCARD virtual std::string_view GetTypeName() const noexcept = 0;
    };

    /**
     * CRTP base for attribute types that automatically provides type hash and name.
     * Derive your custom attributes from this class:
     *
     * struct MyAttribute : AttributeBase<MyAttribute> {
     *     int myValue;
     * };
     */
    template<typename Derived>
    class AttributeBase : public Attribute
    {
    public:
        ASTRA_NODISCARD uint64_t GetTypeHash() const noexcept override
        {
            return TypeID<Derived>::Hash();
        }

        ASTRA_NODISCARD std::string_view GetTypeName() const noexcept override
        {
            return TypeID<Derived>::Name();
        }

        /**
         * Static type hash for compile-time queries.
         */
        static constexpr uint64_t StaticTypeHash() noexcept
        {
            return TypeID<Derived>::Hash();
        }
    };

    // ============================================================================
    // Common Attributes
    // ============================================================================

    /**
     * Specifies a valid numeric range for a field.
     * Used by editors to clamp values and provide appropriate UI controls.
     */
    struct Range : AttributeBase<Range>
    {
        double min;
        double max;
        double step;

        constexpr Range(double minVal, double maxVal, double stepVal = 0.0) noexcept
            : min(minVal), max(maxVal), step(stepVal) {}
    };

    /**
     * Marks a field as hidden from editor/inspector UI.
     * The field can still be accessed programmatically.
     */
    struct Hidden : AttributeBase<Hidden>
    {
        constexpr Hidden() noexcept = default;
    };

    /**
     * Marks a field as read-only in editor UI.
     * The field is displayed but cannot be modified through the UI.
     */
    struct ReadOnly : AttributeBase<ReadOnly>
    {
        constexpr ReadOnly() noexcept = default;
    };

    /**
     * Provides a custom display name for a field in editor UI.
     * If not specified, the field's code name is used.
     */
    struct DisplayName : AttributeBase<DisplayName>
    {
        std::string_view name;

        constexpr DisplayName(std::string_view displayName) noexcept
            : name(displayName) {}
    };

    /**
     * Provides a tooltip/help text for a field.
     * Displayed when hovering over the field in editor UI.
     */
    struct Tooltip : AttributeBase<Tooltip>
    {
        std::string_view text;

        constexpr Tooltip(std::string_view tooltipText) noexcept
            : text(tooltipText) {}
    };

    /**
     * Groups a field under a named category in editor UI.
     * Fields with the same category are displayed together.
     */
    struct Category : AttributeBase<Category>
    {
        std::string_view category;

        constexpr Category(std::string_view categoryName) noexcept
            : category(categoryName) {}
    };

    /**
     * Controls whether a field should be serialized.
     * By default, all reflected fields are serializable.
     */
    struct Serializable : AttributeBase<Serializable>
    {
        bool value;

        constexpr explicit Serializable(bool isSerializable = true) noexcept
            : value(isSerializable) {}
    };

    /**
     * Specifies the color format for color fields.
     * Used by editors to provide color picker UI.
     */
    struct ColorFormat : AttributeBase<ColorFormat>
    {
        enum class Format : uint8_t
        {
            RGB,
            RGBA,
            HSV,
            Hex
        };

        Format format;
        bool hasAlpha;

        constexpr ColorFormat(Format fmt = Format::RGBA, bool alpha = true) noexcept
            : format(fmt), hasAlpha(alpha) {}
    };

    /**
     * Specifies that a float field represents an angle.
     * Used by editors to provide appropriate UI (slider, dial, etc.)
     */
    struct AngleFormat : AttributeBase<AngleFormat>
    {
        enum class Unit : uint8_t
        {
            Radians,
            Degrees
        };

        Unit unit;

        constexpr AngleFormat(Unit angleUnit = Unit::Degrees) noexcept
            : unit(angleUnit) {}
    };

    /**
     * Specifies that a string field should use multiline editing.
     */
    struct Multiline : AttributeBase<Multiline>
    {
        int minLines;
        int maxLines;

        constexpr Multiline(int min = 3, int max = 10) noexcept
            : minLines(min), maxLines(max) {}
    };

    /**
     * Specifies that a string field is a file path.
     * Used by editors to provide file browser UI.
     */
    struct FilePath : AttributeBase<FilePath>
    {
        std::string_view filter;      // e.g., "*.png;*.jpg"
        std::string_view defaultPath; // Starting directory
        bool saveDialog;              // true for save, false for open

        constexpr FilePath(std::string_view fileFilter = "*.*",
                          std::string_view defaultDir = "",
                          bool isSaveDialog = false) noexcept
            : filter(fileFilter), defaultPath(defaultDir), saveDialog(isSaveDialog) {}
    };

    /**
     * Specifies the slider/drag speed for numeric fields.
     */
    struct DragSpeed : AttributeBase<DragSpeed>
    {
        float speed;

        constexpr explicit DragSpeed(float dragSpeed = 1.0f) noexcept
            : speed(dragSpeed) {}
    };

    /**
     * Marks a type or field as deprecated with an optional message.
     */
    struct Deprecated : AttributeBase<Deprecated>
    {
        std::string_view message;

        constexpr Deprecated(std::string_view deprecationMessage = "") noexcept
            : message(deprecationMessage) {}
    };

    /**
     * Specifies display precision for floating point fields.
     */
    struct Precision : AttributeBase<Precision>
    {
        int decimalPlaces;

        constexpr explicit Precision(int decimals = 3) noexcept
            : decimalPlaces(decimals) {}
    };
}
