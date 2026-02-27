#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../Container/FlatMap.hpp"
#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"
#include "Attribute.hpp"
#include "EnumInfo.hpp"
#include "FieldInfo.hpp"

namespace Astra
{
    /**
     * Complete metadata about a reflected type.
     * Provides runtime introspection capabilities including field enumeration,
     * type-erased construction/destruction, and attribute queries.
     */
    class TypeMeta
    {
    public:
        // ====================================================================
        // Type identification
        // ====================================================================

        uint64_t typeHash;              // XXHash64 of type name
        std::string_view typeName;      // Type name as it appears in code
        size_t size;                    // sizeof(T)
        size_t alignment;               // alignof(T)

        // ====================================================================
        // Type classification
        // ====================================================================

        bool isClass;                   // Is this a class/struct?
        bool isEnum;                    // Is this an enum?
        bool isTrivial;                 // Is trivially copyable?
        bool isPolymorphic;             // Has virtual functions?
        bool isAbstract;                // Is abstract class?
        bool isDefaultConstructible;    // Can default construct?
        bool isCopyConstructible;       // Can copy construct?
        bool isMoveConstructible;       // Can move construct?

        // ====================================================================
        // Lifecycle functions
        // ====================================================================

        // Default construct at ptr (placement new)
        std::function<void(void* ptr)> defaultConstruct;

        // Copy construct at dst from src
        std::function<void(void* dst, const void* src)> copyConstruct;

        // Move construct at dst from src
        std::function<void(void* dst, void* src)> moveConstruct;

        // Destruct at ptr
        std::function<void(void* ptr)> destruct;

        // Copy assign dst from src
        std::function<void(void* dst, const void* src)> copyAssign;

        // Move assign dst from src
        std::function<void(void* dst, void* src)> moveAssign;

        // ====================================================================
        // Fields and enums
        // ====================================================================

        std::vector<FieldInfo> fields;                      // All reflected fields
        FlatMap<uint64_t, size_t> fieldsByHash;            // Field lookup by name hash
        std::unique_ptr<EnumInfo> enumInfo;                // Enum metadata (if isEnum)
        std::vector<std::unique_ptr<Attribute>> attributes; // Type-level attributes
        std::vector<std::unique_ptr<Attribute>> fieldAttributeStorage; // Owns field attributes

        // ====================================================================
        // Field query methods
        // ====================================================================

        /**
         * Gets a field by name.
         * @param name Field name
         * @return Pointer to FieldInfo, or nullptr if not found
         */
        ASTRA_NODISCARD const FieldInfo* GetField(std::string_view name) const
        {
            uint64_t hash = Detail::XXHash::XXHash64(name);
            return GetFieldByHash(hash);
        }

        /**
         * Gets a field by name hash.
         * @param hash XXHash64 of field name
         * @return Pointer to FieldInfo, or nullptr if not found
         */
        ASTRA_NODISCARD const FieldInfo* GetFieldByHash(uint64_t hash) const
        {
            auto it = fieldsByHash.Find(hash);
            if (it != fieldsByHash.end())
            {
                return &fields[it->second];
            }
            return nullptr;
        }

        /**
         * Gets a field by index.
         * @param index Field index
         * @return Pointer to FieldInfo, or nullptr if out of bounds
         */
        ASTRA_NODISCARD const FieldInfo* GetFieldByIndex(size_t index) const
        {
            if (index < fields.size())
            {
                return &fields[index];
            }
            return nullptr;
        }

        /**
         * Gets the number of reflected fields.
         */
        ASTRA_NODISCARD size_t GetFieldCount() const noexcept
        {
            return fields.size();
        }

        /**
         * Checks if the type has a field with the given name.
         * @param name Field name
         * @return true if field exists
         */
        ASTRA_NODISCARD bool HasField(std::string_view name) const
        {
            return GetField(name) != nullptr;
        }

        /**
         * Iterates over all fields.
         * @tparam Func Callback type
         * @param func Callback invoked for each field
         */
        template<typename Func>
        void ForEachField(Func&& func) const
        {
            for (const auto& field : fields)
            {
                func(field);
            }
        }

        // ====================================================================
        // Field value access
        // ====================================================================

        /**
         * Gets a field value by name.
         * @tparam T Expected field type
         * @param instance Pointer to the instance
         * @param fieldName Name of the field
         * @return The field value, or nullopt if field not found
         */
        template<typename T>
        ASTRA_NODISCARD std::optional<T> GetFieldValue(const void* instance, std::string_view fieldName) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field || field->typeHash != TypeID<T>::Hash())
            {
                return std::nullopt;
            }
            return field->Get<T>(instance);
        }

        /**
         * Sets a field value by name.
         * @tparam T Field type
         * @param instance Pointer to the instance
         * @param fieldName Name of the field
         * @param value Value to set
         * @return true if the value was set successfully
         */
        template<typename T>
        bool SetFieldValue(void* instance, std::string_view fieldName, const T& value) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field || field->typeHash != TypeID<T>::Hash() || field->isConst)
            {
                return false;
            }
            field->Set<T>(instance, value);
            return true;
        }

        /**
         * Gets a field value as std::any by name.
         * @param instance Pointer to the instance
         * @param fieldName Name of the field
         * @return The field value wrapped in std::any, or empty any if not found
         */
        ASTRA_NODISCARD std::any GetFieldValueAny(const void* instance, std::string_view fieldName) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field)
            {
                return std::any{};
            }
            return field->GetAny(instance);
        }

        /**
         * Sets a field value from std::any by name.
         * @param instance Pointer to the instance
         * @param fieldName Name of the field
         * @param value Value to set
         * @return true if the value was set successfully
         */
        bool SetFieldValueAny(void* instance, std::string_view fieldName, const std::any& value) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field)
            {
                return false;
            }
            return field->SetAny(instance, value);
        }

        // ====================================================================
        // Attribute queries
        // ====================================================================

        /**
         * Gets a type-level attribute.
         * @tparam A Attribute type
         * @return Pointer to the attribute, or nullptr if not found
         */
        template<typename A>
        ASTRA_NODISCARD const A* GetAttribute() const
        {
            static_assert(std::is_base_of_v<Attribute, A>,
                "A must derive from Attribute");

            constexpr uint64_t attrHash = TypeID<A>::Hash();
            for (const auto& attr : attributes)
            {
                if (attr && attr->GetTypeHash() == attrHash)
                {
                    return static_cast<const A*>(attr.get());
                }
            }
            return nullptr;
        }

        /**
         * Checks if the type has an attribute.
         * @tparam A Attribute type
         * @return true if the attribute exists
         */
        template<typename A>
        ASTRA_NODISCARD bool HasAttribute() const
        {
            return GetAttribute<A>() != nullptr;
        }

        /**
         * Iterates over all type-level attributes.
         * @tparam Func Callback type
         * @param func Callback invoked for each attribute
         */
        template<typename Func>
        void ForEachAttribute(Func&& func) const
        {
            for (const auto& attr : attributes)
            {
                if (attr)
                {
                    func(*attr);
                }
            }
        }

        // ====================================================================
        // Object lifecycle
        // ====================================================================

        /**
         * Default constructs an object at the given address.
         * @param ptr Address where the object should be constructed
         * @return true if construction succeeded
         */
        bool Construct(void* ptr) const
        {
            if (defaultConstruct && isDefaultConstructible)
            {
                defaultConstruct(ptr);
                return true;
            }
            return false;
        }

        /**
         * Copy constructs an object at dst from src.
         * @param dst Destination address
         * @param src Source object
         * @return true if construction succeeded
         */
        bool CopyConstruct(void* dst, const void* src) const
        {
            if (copyConstruct && isCopyConstructible)
            {
                copyConstruct(dst, src);
                return true;
            }
            return false;
        }

        /**
         * Move constructs an object at dst from src.
         * @param dst Destination address
         * @param src Source object (will be moved from)
         * @return true if construction succeeded
         */
        bool MoveConstruct(void* dst, void* src) const
        {
            if (moveConstruct && isMoveConstructible)
            {
                moveConstruct(dst, src);
                return true;
            }
            return false;
        }

        /**
         * Destructs an object at the given address.
         * @param ptr Address of the object to destruct
         */
        void Destruct(void* ptr) const
        {
            if (destruct)
            {
                destruct(ptr);
            }
        }

        /**
         * Copy assigns an object.
         * @param dst Destination object
         * @param src Source object
         * @return true if assignment succeeded
         */
        bool CopyAssign(void* dst, const void* src) const
        {
            if (copyAssign && isCopyConstructible)
            {
                copyAssign(dst, src);
                return true;
            }
            return false;
        }

        /**
         * Move assigns an object.
         * @param dst Destination object
         * @param src Source object (will be moved from)
         * @return true if assignment succeeded
         */
        bool MoveAssign(void* dst, void* src) const
        {
            if (moveAssign && isMoveConstructible)
            {
                moveAssign(dst, src);
                return true;
            }
            return false;
        }

        // ====================================================================
        // Enum helpers
        // ====================================================================

        /**
         * Converts an enum value to string (only valid if isEnum is true).
         * @param value The enum value
         * @return String representation, or nullopt if not valid
         */
        ASTRA_NODISCARD std::optional<std::string_view> EnumToString(int64_t value) const
        {
            if (isEnum && enumInfo)
            {
                return enumInfo->ToString(value);
            }
            return std::nullopt;
        }

        /**
         * Converts a string to enum value (only valid if isEnum is true).
         * @param name The string name
         * @return Enum value, or nullopt if not valid
         */
        ASTRA_NODISCARD std::optional<int64_t> EnumFromString(std::string_view name) const
        {
            if (isEnum && enumInfo)
            {
                return enumInfo->FromString(name);
            }
            return std::nullopt;
        }

        /**
         * Gets the EnumInfo for this type (only valid if isEnum is true).
         * @return Pointer to EnumInfo, or nullptr if not an enum
         */
        ASTRA_NODISCARD const EnumInfo* GetEnumInfo() const
        {
            return enumInfo.get();
        }
    };

    namespace Detail
    {
        /**
         * Builder class for constructing TypeMeta.
         * Used internally by the registration macros.
         */
        template<typename T>
        class TypeMetaBuilder
        {
        public:
            TypeMetaBuilder()
            {
                m_meta.typeHash = TypeID<T>::Hash();
                m_meta.typeName = TypeID<T>::Name();
                m_meta.size = sizeof(T);
                m_meta.alignment = alignof(T);

                // Type traits
                m_meta.isClass = std::is_class_v<T>;
                m_meta.isEnum = std::is_enum_v<T>;
                m_meta.isTrivial = std::is_trivially_copyable_v<T>;
                m_meta.isPolymorphic = std::is_polymorphic_v<T>;
                m_meta.isAbstract = std::is_abstract_v<T>;
                m_meta.isDefaultConstructible = std::is_default_constructible_v<T>;
                m_meta.isCopyConstructible = std::is_copy_constructible_v<T>;
                m_meta.isMoveConstructible = std::is_move_constructible_v<T>;

                // Lifecycle functions
                if constexpr (std::is_default_constructible_v<T>)
                {
                    m_meta.defaultConstruct = [](void* ptr) {
                        new (ptr) T();
                    };
                }

                if constexpr (std::is_copy_constructible_v<T>)
                {
                    m_meta.copyConstruct = [](void* dst, const void* src) {
                        new (dst) T(*static_cast<const T*>(src));
                    };

                    m_meta.copyAssign = [](void* dst, const void* src) {
                        *static_cast<T*>(dst) = *static_cast<const T*>(src);
                    };
                }

                if constexpr (std::is_move_constructible_v<T>)
                {
                    m_meta.moveConstruct = [](void* dst, void* src) {
                        new (dst) T(std::move(*static_cast<T*>(src)));
                    };

                    m_meta.moveAssign = [](void* dst, void* src) {
                        *static_cast<T*>(dst) = std::move(*static_cast<T*>(src));
                    };
                }

                m_meta.destruct = [](void* ptr) {
                    static_cast<T*>(ptr)->~T();
                };
            }

            /**
             * Adds a field to the type metadata.
             * @tparam FieldType Type of the field
             * @tparam FieldPtr Pointer-to-member
             * @param name Field name
             * @return Reference to this builder for chaining
             */
            template<typename FieldType, FieldType T::*FieldPtr>
            TypeMetaBuilder& Field(std::string_view name)
            {
                FieldInfo field = MakeFieldInfo<T, FieldType, FieldPtr>(name);
                uint64_t hash = field.nameHash;
                m_meta.fieldsByHash[hash] = m_meta.fields.size();
                m_meta.fields.push_back(std::move(field));
                return *this;
            }

            /**
             * Adds an attribute to the last added field.
             * @tparam A Attribute type
             * @tparam Args Attribute constructor argument types
             * @param args Attribute constructor arguments
             * @return Reference to this builder for chaining
             */
            template<typename A, typename... Args>
            TypeMetaBuilder& Attr(Args&&... args)
            {
                if (!m_meta.fields.empty())
                {
                    // Store the attribute in TypeMeta's storage so it lives with the metadata
                    auto attr = std::make_unique<A>(std::forward<Args>(args)...);
                    m_meta.fields.back().attributes.push_back(attr.get());
                    m_meta.fieldAttributeStorage.push_back(std::move(attr));
                }
                return *this;
            }

            /**
             * Adds a type-level attribute.
             * @tparam A Attribute type
             * @tparam Args Attribute constructor argument types
             * @param args Attribute constructor arguments
             * @return Reference to this builder for chaining
             */
            template<typename A, typename... Args>
            TypeMetaBuilder& TypeAttr(Args&&... args)
            {
                m_meta.attributes.push_back(std::make_unique<A>(std::forward<Args>(args)...));
                return *this;
            }

            /**
             * Sets enum metadata (for enum types).
             * @param info EnumInfo structure
             * @return Reference to this builder for chaining
             */
            TypeMetaBuilder& Enum(EnumInfo info)
            {
                m_meta.enumInfo = std::make_unique<EnumInfo>(std::move(info));
                m_meta.isEnum = true;
                return *this;
            }

            /**
             * Builds and returns the TypeMeta.
             * The builder should not be used after calling Build().
             */
            TypeMeta Build()
            {
                return std::move(m_meta);
            }

            /**
             * Gets a reference to the TypeMeta being built.
             * Useful for registration that stores the meta in a global registry.
             */
            ASTRA_NODISCARD TypeMeta& GetMeta() { return m_meta; }
            ASTRA_NODISCARD const TypeMeta& GetMeta() const { return m_meta; }

        private:
            TypeMeta m_meta;
        };
    }
}
