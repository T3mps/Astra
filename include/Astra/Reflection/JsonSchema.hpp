#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"
#include "ContainerTraits.hpp"
#include "TypeMeta.hpp"

namespace Astra
{
    /**
     * Generates JSON Schema from TypeMeta for runtime type validation.
     * Produces JSON Schema Draft 2020-12 compatible output.
     */
    class JsonSchemaGenerator
    {
    public:
        struct Options
        {
            bool includeDescriptions = true;     // Include tooltip text as descriptions
            bool includeDefaults = false;        // Include default values (not supported yet)
            bool includeExamples = false;        // Include example values (not supported yet)
            bool prettyPrint = true;             // Format output with indentation
            int indentSpaces = 2;                // Number of spaces for indentation
        };

        // Delegating overload instead of a default argument: gcc/clang reject a
        // default argument that needs Options' NSDMIs before the enclosing class is complete.
        JsonSchemaGenerator() : JsonSchemaGenerator(Options{}) {}

        explicit JsonSchemaGenerator(const Options& options)
            : m_options(options) {}

        /**
         * Generates JSON Schema for a type.
         * @param meta TypeMeta to generate schema for
         * @return JSON Schema string
         */
        ASTRA_NODISCARD std::string Generate(const TypeMeta& meta) const
        {
            std::ostringstream ss;

            BeginObject(ss, 0);

            // Schema metadata
            WriteProperty(ss, "$schema", "https://json-schema.org/draft/2020-12/schema", 1);
            ss << ",\n";

            WriteProperty(ss, "title", std::string(meta.typeName), 1);
            ss << ",\n";

            WriteProperty(ss, "type", "object", 1);
            ss << ",\n";

            // Properties
            WriteIndent(ss, 1);
            ss << "\"properties\": {\n";

            bool first = true;
            for (const auto& field : meta.fields)
            {
                if (field.IsHidden())
                {
                    continue;
                }

                if (!first)
                {
                    ss << ",\n";
                }
                first = false;

                WriteFieldSchema(ss, field, 2);
            }

            ss << "\n";
            WriteIndent(ss, 1);
            ss << "}";

            // Required fields (all non-optional fields)
            std::vector<std::string_view> required;
            for (const auto& field : meta.fields)
            {
                if (!field.IsHidden())
                {
                    required.push_back(field.name);
                }
            }

            if (!required.empty())
            {
                ss << ",\n";
                WriteIndent(ss, 1);
                ss << "\"required\": [";

                for (size_t i = 0; i < required.size(); ++i)
                {
                    if (i > 0) ss << ", ";
                    ss << "\"" << required[i] << "\"";
                }

                ss << "]";
            }

            // additionalProperties: false for strict validation
            ss << ",\n";
            WriteProperty(ss, "additionalProperties", false, 1);

            EndObject(ss, 0);

            return ss.str();
        }

        /**
         * Generates JSON Schema for a type by looking it up in MetaRegistry.
         * @tparam T Type to generate schema for
         * @return JSON Schema string, or empty string if type not registered
         */
        template<typename T>
        ASTRA_NODISCARD std::string Generate() const
        {
            const TypeMeta* meta = GetMeta<T>();
            if (!meta)
            {
                return "";
            }
            return Generate(*meta);
        }

    private:
        Options m_options;

        void WriteIndent(std::ostringstream& ss, int level) const
        {
            if (m_options.prettyPrint)
            {
                for (int i = 0; i < level * m_options.indentSpaces; ++i)
                {
                    ss << ' ';
                }
            }
        }

        void BeginObject(std::ostringstream& ss, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "{\n";
        }

        void EndObject(std::ostringstream& ss, int indent) const
        {
            ss << "\n";
            WriteIndent(ss, indent);
            ss << "}";
        }

        void WriteProperty(std::ostringstream& ss, std::string_view name, std::string_view value, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "\"" << name << "\": \"" << EscapeString(value) << "\"";
        }

        void WriteProperty(std::ostringstream& ss, std::string_view name, bool value, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "\"" << name << "\": " << (value ? "true" : "false");
        }

        void WriteProperty(std::ostringstream& ss, std::string_view name, int64_t value, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "\"" << name << "\": " << value;
        }

        void WriteProperty(std::ostringstream& ss, std::string_view name, double value, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "\"" << name << "\": " << value;
        }

        std::string EscapeString(std::string_view str) const
        {
            std::string result;
            result.reserve(str.size());

            for (char c : str)
            {
                switch (c)
                {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += c; break;
                }
            }

            return result;
        }

        void WriteFieldSchema(std::ostringstream& ss, const FieldInfo& field, int indent) const
        {
            WriteIndent(ss, indent);
            ss << "\"" << field.name << "\": {\n";

            // Determine JSON type from field type
            std::string_view jsonType = GetJsonType(field);
            WriteProperty(ss, "type", jsonType, indent + 1);

            // Add description from tooltip
            if (m_options.includeDescriptions)
            {
                std::string_view tooltip = field.GetTooltip();
                if (!tooltip.empty())
                {
                    ss << ",\n";
                    WriteProperty(ss, "description", std::string(tooltip), indent + 1);
                }
            }

            // Add range constraints
            if (const Range* range = field.GetRange())
            {
                if (jsonType == "number" || jsonType == "integer")
                {
                    ss << ",\n";
                    WriteProperty(ss, "minimum", range->min, indent + 1);
                    ss << ",\n";
                    WriteProperty(ss, "maximum", range->max, indent + 1);

                    if (range->step > 0)
                    {
                        ss << ",\n";
                        WriteProperty(ss, "multipleOf", range->step, indent + 1);
                    }
                }
            }

            // Add enum constraints for enum fields
            if (field.isEnum)
            {
                const TypeMeta* enumMeta = GetMeta(field.typeHash);
                if (enumMeta && enumMeta->enumInfo)
                {
                    ss << ",\n";
                    WriteIndent(ss, indent + 1);
                    ss << "\"enum\": [";

                    bool first = true;
                    for (const auto& value : enumMeta->enumInfo->values)
                    {
                        if (!first) ss << ", ";
                        first = false;
                        ss << "\"" << value.name << "\"";
                    }

                    ss << "]";
                }
            }

            // Add array items schema
            if (field.isArray || field.isVector || field.isStdArray)
            {
                ss << ",\n";
                WriteIndent(ss, indent + 1);
                ss << "\"items\": {\n";
                // For now, just allow any type in arrays
                // A more complete implementation would introspect the element type
                WriteProperty(ss, "type", "object", indent + 2);
                ss << "\n";
                WriteIndent(ss, indent + 1);
                ss << "}";

                // Fixed size for std::array
                if (field.isStdArray && field.size > 0)
                {
                    // Can't easily determine array element count from FieldInfo
                    // Would need additional metadata
                }
            }

            ss << "\n";
            WriteIndent(ss, indent);
            ss << "}";
        }

        std::string_view GetJsonType(const FieldInfo& field) const
        {
            // Check for common types by hash
            static const uint64_t boolHash = TypeID<bool>::Hash();
            static const uint64_t intHash = TypeID<int>::Hash();
            static const uint64_t int8Hash = TypeID<int8_t>::Hash();
            static const uint64_t int16Hash = TypeID<int16_t>::Hash();
            static const uint64_t int32Hash = TypeID<int32_t>::Hash();
            static const uint64_t int64Hash = TypeID<int64_t>::Hash();
            static const uint64_t uint8Hash = TypeID<uint8_t>::Hash();
            static const uint64_t uint16Hash = TypeID<uint16_t>::Hash();
            static const uint64_t uint32Hash = TypeID<uint32_t>::Hash();
            static const uint64_t uint64Hash = TypeID<uint64_t>::Hash();
            static const uint64_t floatHash = TypeID<float>::Hash();
            static const uint64_t doubleHash = TypeID<double>::Hash();
            static const uint64_t stringHash = TypeID<std::string>::Hash();
            static const uint64_t stringViewHash = TypeID<std::string_view>::Hash();

            uint64_t hash = field.typeHash;

            if (hash == boolHash)
            {
                return "boolean";
            }

            if (hash == intHash || hash == int8Hash || hash == int16Hash ||
                hash == int32Hash || hash == int64Hash || hash == uint8Hash ||
                hash == uint16Hash || hash == uint32Hash || hash == uint64Hash)
            {
                return "integer";
            }

            if (hash == floatHash || hash == doubleHash)
            {
                return "number";
            }

            if (hash == stringHash || hash == stringViewHash)
            {
                return "string";
            }

            // Enums are represented as strings
            if (field.isEnum)
            {
                return "string";
            }

            // Arrays and vectors
            if (field.isArray || field.isVector || field.isStdArray)
            {
                return "array";
            }

            // Default to object for complex types
            return "object";
        }
    };

    /**
     * Convenience function to generate JSON Schema for a reflected type.
     * @tparam T Type to generate schema for
     * @param options Schema generation options
     * @return JSON Schema string, or empty string if type not registered
     */
    template<typename T>
    inline std::string GenerateJsonSchema(const JsonSchemaGenerator::Options& options = {})
    {
        return JsonSchemaGenerator(options).Generate<T>();
    }

    /**
     * Convenience function to generate JSON Schema from TypeMeta.
     * @param meta TypeMeta to generate schema for
     * @param options Schema generation options
     * @return JSON Schema string
     */
    inline std::string GenerateJsonSchema(const TypeMeta& meta, const JsonSchemaGenerator::Options& options = {})
    {
        return JsonSchemaGenerator(options).Generate(meta);
    }
}
