#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"

namespace Astra
{
    /**
     * Describes a single value within an enum.
     */
    struct EnumValue
    {
        std::string_view name;         // Value name as it appears in code
        int64_t value;                 // Numeric value
        std::string_view displayName;  // Optional display name for UI
        std::string_view description;  // Optional description/tooltip
    };

    /**
     * Contains complete metadata about an enum type.
     */
    struct EnumInfo
    {
        std::vector<EnumValue> values;   // All enum values
        bool isFlags;                    // True if this is a flags/bitmask enum
        bool isSigned;                   // True if the underlying type is signed
        size_t underlyingSize;           // Size of the underlying type in bytes

        /**
         * Converts an enum value to its string representation.
         * @param value The numeric enum value
         * @return The name string, or nullopt if not found
         */
        ASTRA_NODISCARD std::optional<std::string_view> ToString(int64_t value) const
        {
            if (isFlags && value != 0)
            {
                // For flags, we don't do compound flag decomposition here
                // Just look for exact match
                for (const auto& ev : values)
                {
                    if (ev.value == value)
                    {
                        return ev.name;
                    }
                }
                return std::nullopt;
            }

            for (const auto& ev : values)
            {
                if (ev.value == value)
                {
                    return ev.name;
                }
            }
            return std::nullopt;
        }

        /**
         * Converts a string to its enum value.
         * @param name The name string
         * @return The numeric value, or nullopt if not found
         */
        ASTRA_NODISCARD std::optional<int64_t> FromString(std::string_view name) const
        {
            for (const auto& ev : values)
            {
                if (ev.name == name)
                {
                    return ev.value;
                }
            }
            return std::nullopt;
        }

        /**
         * Gets the display name for an enum value.
         * @param value The numeric enum value
         * @return The display name, or the code name if no display name is set
         */
        ASTRA_NODISCARD std::optional<std::string_view> GetDisplayName(int64_t value) const
        {
            for (const auto& ev : values)
            {
                if (ev.value == value)
                {
                    return ev.displayName.empty() ? ev.name : ev.displayName;
                }
            }
            return std::nullopt;
        }

        /**
         * Gets the description for an enum value.
         * @param value The numeric enum value
         * @return The description, or empty string if not set
         */
        ASTRA_NODISCARD std::string_view GetDescription(int64_t value) const
        {
            for (const auto& ev : values)
            {
                if (ev.value == value)
                {
                    return ev.description;
                }
            }
            return {};
        }

        /**
         * Checks if a numeric value is a valid enum value.
         * @param value The numeric value to check
         * @return true if the value is valid
         */
        ASTRA_NODISCARD bool IsValid(int64_t value) const
        {
            if (isFlags)
            {
                // For flags, check if all bits are covered by known values
                int64_t allFlags = 0;
                for (const auto& ev : values)
                {
                    allFlags |= ev.value;
                }
                return (value & ~allFlags) == 0;
            }

            for (const auto& ev : values)
            {
                if (ev.value == value)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * Gets the count of enum values.
         */
        ASTRA_NODISCARD size_t Count() const noexcept
        {
            return values.size();
        }

        /**
         * Checks if the enum has a value with the given name.
         * @param name The name to check
         * @return true if found
         */
        ASTRA_NODISCARD bool HasValue(std::string_view name) const
        {
            for (const auto& ev : values)
            {
                if (ev.name == name)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * Gets the EnumValue struct for a given numeric value.
         * @param value The numeric value
         * @return Pointer to the EnumValue, or nullptr if not found
         */
        ASTRA_NODISCARD const EnumValue* GetValue(int64_t value) const
        {
            for (const auto& ev : values)
            {
                if (ev.value == value)
                {
                    return &ev;
                }
            }
            return nullptr;
        }

        /**
         * Gets the EnumValue struct for a given name.
         * @param name The value name
         * @return Pointer to the EnumValue, or nullptr if not found
         */
        ASTRA_NODISCARD const EnumValue* GetValueByName(std::string_view name) const
        {
            for (const auto& ev : values)
            {
                if (ev.name == name)
                {
                    return &ev;
                }
            }
            return nullptr;
        }

        /**
         * Iterates over all enum values.
         * @tparam Func Callback type
         * @param func Callback invoked for each value
         */
        template<typename Func>
        void ForEachValue(Func&& func) const
        {
            for (const auto& ev : values)
            {
                func(ev);
            }
        }

        // ====================================================================
        // Flags-specific methods
        // ====================================================================

        /**
         * For flags enums: decomposes a value into its constituent flags.
         * @param value The combined flag value
         * @return Vector of individual flag values that make up the combined value
         */
        ASTRA_NODISCARD std::vector<int64_t> DecomposeFlags(int64_t value) const
        {
            std::vector<int64_t> result;
            if (!isFlags)
            {
                if (IsValid(value))
                {
                    result.push_back(value);
                }
                return result;
            }

            // Sort by value descending to get largest flags first
            std::vector<const EnumValue*> sortedValues;
            sortedValues.reserve(values.size());
            for (const auto& ev : values)
            {
                if (ev.value != 0) // Skip zero value
                {
                    sortedValues.push_back(&ev);
                }
            }

            // Process from largest to smallest
            for (const auto* ev : sortedValues)
            {
                if ((value & ev->value) == ev->value)
                {
                    result.push_back(ev->value);
                    value &= ~ev->value;
                }
            }

            return result;
        }

        /**
         * For flags enums: combines multiple flag values.
         * @param flags Vector of flag values to combine
         * @return The combined flag value
         */
        ASTRA_NODISCARD int64_t CombineFlags(const std::vector<int64_t>& flags) const
        {
            int64_t result = 0;
            for (int64_t flag : flags)
            {
                result |= flag;
            }
            return result;
        }
    };

    namespace Detail
    {
        /**
         * Builder class for constructing EnumInfo.
         * Used internally by the registration macros.
         */
        template<typename EnumType>
        class EnumInfoBuilder
        {
        public:
            using UnderlyingType = std::underlying_type_t<EnumType>;

            EnumInfoBuilder()
            {
                m_info.isFlags = false;
                m_info.isSigned = std::is_signed_v<UnderlyingType>;
                m_info.underlyingSize = sizeof(UnderlyingType);
            }

            EnumInfoBuilder& Value(std::string_view name, EnumType value,
                                   std::string_view displayName = {},
                                   std::string_view description = {})
            {
                EnumValue ev;
                ev.name = name;
                ev.value = static_cast<int64_t>(static_cast<UnderlyingType>(value));
                ev.displayName = displayName;
                ev.description = description;
                m_info.values.push_back(ev);
                return *this;
            }

            EnumInfoBuilder& Flags()
            {
                m_info.isFlags = true;
                return *this;
            }

            ASTRA_NODISCARD EnumInfo Build() const
            {
                return m_info;
            }

        private:
            EnumInfo m_info;
        };
    }
}
