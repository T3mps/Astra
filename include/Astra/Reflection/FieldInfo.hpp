#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"
#include "AnyValue.hpp"
#include "Attribute.hpp"
#include "ContainerTraits.hpp"

namespace Astra
{
    /**
     * Describes a single field within a reflected type.
     * Provides type-erased access to field values and metadata.
     */
    struct FieldInfo
    {
        // Field identification
        std::string_view name;          // Field name as it appears in code
        uint64_t nameHash;              // XXHash64 of the field name
        uint64_t typeHash;              // XXHash64 of the field type

        // Memory layout
        size_t offset;                  // Byte offset from struct start
        size_t size;                    // Size of the field in bytes
        size_t alignment;               // Required alignment

        // Type traits
        bool isConst;                   // Is the field const-qualified?
        bool isPointer;                 // Is the field a pointer type?
        bool isReference;               // Is the field a reference type?
        bool isTrivial;                 // Is the field trivially copyable?
        bool isArithmetic;              // Is the field an arithmetic type?
        bool isEnum;                    // Is the field an enum type?
        bool isClass;                   // Is the field a class/struct type?
        bool isArray;                   // Is the field a C-style array?
        bool isStdArray;                // Is the field a std::array?
        bool isVector;                  // Is the field a std::vector?

        // Type-erased accessors
        // Getter: copies field value from instance to output buffer
        std::function<void(const void* instance, void* outValue)> getter;

        // Setter: copies value from input buffer to instance field
        std::function<void(void* instance, const void* inValue)> setter;

        // Getter returning AnyValue for dynamic (type-erased, RTTI-free) handling
        std::function<AnyValue(const void* instance)> getterAny;

        // Setter accepting AnyValue for dynamic (type-erased, RTTI-free) handling
        std::function<bool(void* instance, const AnyValue& value)> setterAny;

        // Attributes attached to this field
        std::vector<const Attribute*> attributes;

        // ====================================================================
        // Typed accessors
        // ====================================================================

        /**
         * Gets the field value with compile-time type checking.
         * @tparam T Expected field type
         * @param instance Pointer to the containing struct instance
         * @return The field value
         */
        template<typename T>
        ASTRA_NODISCARD T Get(const void* instance) const
        {
            // All-config guard: a wrong-T read against a differently-sized/typed
            // getter would corrupt `result`, and calling an unset getter is UB.
            // ASTRA_ASSERT below remains as a Debug-only diagnostic; it is not
            // the guard (Release/Dist must degrade safely, not just in Debug).
            if (TypeID<T>::Hash() != typeHash || !getter) ASTRA_UNLIKELY
                return T{};
            ASTRA_ASSERT(TypeID<T>::Hash() == typeHash, "Type mismatch in FieldInfo::Get");
            ASTRA_ASSERT(getter, "No getter function registered");

            T result{};
            getter(instance, &result);
            return result;
        }

        /**
         * Sets the field value with compile-time type checking.
         * @tparam T Field type
         * @param instance Pointer to the containing struct instance
         * @param value The value to set
         */
        template<typename T>
        void Set(void* instance, const T& value) const
        {
            // All-config guard: `setter` is an empty std::function for const
            // fields, so calling it unconditionally would invoke std::bad_function_call
            // (std::terminate under -fno-exceptions). Type mismatch and const-ness
            // must both be checked before ever calling `setter`, in every config.
            if (TypeID<T>::Hash() != typeHash || !setter || isConst) ASTRA_UNLIKELY
                return; // no-op: never call an empty std::function
            ASTRA_ASSERT(TypeID<T>::Hash() == typeHash, "Type mismatch in FieldInfo::Set");
            ASTRA_ASSERT(setter, "No setter function registered");
            ASTRA_ASSERT(!isConst, "Cannot set const field");

            setter(instance, &value);
        }

        /**
         * Gets a pointer to the field within the instance.
         * @tparam T Expected field type
         * @param instance Pointer to the containing struct instance
         * @return Pointer to the field
         */
        template<typename T>
        ASTRA_NODISCARD T* GetPtr(void* instance) const
        {
            // All-config guard: a const field must never hand back a writable
            // pointer, and a wrong-T request must not reinterpret the field's
            // bytes as a differently-sized/typed object.
            if (TypeID<T>::Hash() != typeHash || isConst) ASTRA_UNLIKELY
                return nullptr;
            ASTRA_ASSERT(TypeID<T>::Hash() == typeHash, "Type mismatch in FieldInfo::GetPtr");
            return reinterpret_cast<T*>(static_cast<std::byte*>(instance) + offset);
        }

        /**
         * Gets a const pointer to the field within the instance.
         * @tparam T Expected field type
         * @param instance Pointer to the containing struct instance
         * @return Const pointer to the field
         */
        template<typename T>
        ASTRA_NODISCARD const T* GetPtr(const void* instance) const
        {
            // All-config guard: a wrong-T request must not reinterpret the
            // field's bytes as a differently-sized/typed object.
            if (TypeID<T>::Hash() != typeHash) ASTRA_UNLIKELY
                return nullptr;
            ASTRA_ASSERT(TypeID<T>::Hash() == typeHash, "Type mismatch in FieldInfo::GetPtr");
            return reinterpret_cast<const T*>(static_cast<const std::byte*>(instance) + offset);
        }

        /**
         * Gets the field value as AnyValue for dynamic handling.
         * @param instance Pointer to the containing struct instance
         * @return The field value wrapped in AnyValue
         */
        ASTRA_NODISCARD AnyValue GetAny(const void* instance) const
        {
            if (getterAny)
            {
                return getterAny(instance);
            }
            return AnyValue{};
        }

        /**
         * Sets the field value from AnyValue for dynamic handling.
         * @param instance Pointer to the containing struct instance
         * @param value The value to set
         * @return true if the value was set successfully, false on type mismatch
         */
        bool SetAny(void* instance, const AnyValue& value) const
        {
            if (isConst || !setterAny)
            {
                return false;
            }
            return setterAny(instance, value);
        }

        // ====================================================================
        // Attribute queries
        // ====================================================================

        /**
         * Gets an attribute of the specified type.
         * @tparam A Attribute type to query
         * @return Pointer to the attribute, or nullptr if not found
         */
        template<typename A>
        ASTRA_NODISCARD const A* GetAttribute() const
        {
            static_assert(std::is_base_of_v<Attribute, A>,
                "A must derive from Attribute");

            constexpr uint64_t attrHash = TypeID<A>::Hash();
            for (const auto* attr : attributes)
            {
                if (attr && attr->GetTypeHash() == attrHash)
                {
                    return static_cast<const A*>(attr);
                }
            }
            return nullptr;
        }

        /**
         * Checks if the field has an attribute of the specified type.
         * @tparam A Attribute type to check
         * @return true if the attribute exists
         */
        template<typename A>
        ASTRA_NODISCARD bool HasAttribute() const
        {
            return GetAttribute<A>() != nullptr;
        }

        /**
         * Iterates over all attributes of a specific type.
         * @tparam A Attribute type to filter
         * @tparam Func Callback type
         * @param func Callback invoked for each matching attribute
         */
        template<typename A, typename Func>
        void ForEachAttribute(Func&& func) const
        {
            static_assert(std::is_base_of_v<Attribute, A>,
                "A must derive from Attribute");

            constexpr uint64_t attrHash = TypeID<A>::Hash();
            for (const auto* attr : attributes)
            {
                if (attr && attr->GetTypeHash() == attrHash)
                {
                    func(*static_cast<const A*>(attr));
                }
            }
        }

        /**
         * Iterates over all attributes.
         * @tparam Func Callback type
         * @param func Callback invoked for each attribute
         */
        template<typename Func>
        void ForEachAttribute(Func&& func) const
        {
            for (const auto* attr : attributes)
            {
                if (attr)
                {
                    func(*attr);
                }
            }
        }

        // ====================================================================
        // Utility methods
        // ====================================================================

        /**
         * Checks if this field should be serialized.
         * @return true if the field should be serialized
         */
        ASTRA_NODISCARD bool IsSerializable() const
        {
            if (const auto* attr = GetAttribute<Serializable>())
            {
                return attr->value;
            }
            return true; // Default to serializable
        }

        /**
         * Checks if this field is hidden from UI.
         * @return true if the field is hidden
         */
        ASTRA_NODISCARD bool IsHidden() const
        {
            return HasAttribute<Hidden>();
        }

        /**
         * Checks if this field is read-only.
         * @return true if the field is read-only
         */
        ASTRA_NODISCARD bool IsReadOnly() const
        {
            return isConst || HasAttribute<ReadOnly>();
        }

        /**
         * Gets the display name for this field.
         * @return Custom display name if set, otherwise the code name
         */
        ASTRA_NODISCARD std::string_view GetDisplayName() const
        {
            if (const auto* attr = GetAttribute<DisplayName>())
            {
                return attr->name;
            }
            return name;
        }

        /**
         * Gets the tooltip text for this field.
         * @return Tooltip text or empty string if not set
         */
        ASTRA_NODISCARD std::string_view GetTooltip() const
        {
            if (const auto* attr = GetAttribute<Tooltip>())
            {
                return attr->text;
            }
            return {};
        }

        /**
         * Gets the category for this field.
         * @return Category name or empty string if not set
         */
        ASTRA_NODISCARD std::string_view GetCategory() const
        {
            if (const auto* attr = GetAttribute<Category>())
            {
                return attr->category;
            }
            return {};
        }

        /**
         * Gets the valid range for this field (if set).
         * @return Range attribute or nullptr if not set
         */
        ASTRA_NODISCARD const Range* GetRange() const
        {
            return GetAttribute<Range>();
        }
    };

    namespace Detail
    {
        /**
         * Detects whether ContainerTraits<T> exposes a `static constexpr bool IsString`
         * member. Only the std::basic_string specializations currently define it;
         * std::string otherwise shares std::vector's IsSequence/HasContiguousStorage/
         * !HasFixedSize fingerprint in ContainerTraits, so this is needed to tell them
         * apart when deriving FieldInfo::isVector below.
         */
        template<typename T, typename = void>
        struct ContainerIsStringTrait : std::false_type {};

        template<typename T>
        struct ContainerIsStringTrait<T, std::void_t<decltype(ContainerTraits<T>::IsString)>>
            : std::bool_constant<ContainerTraits<T>::IsString> {};

        /**
         * Creates a FieldInfo for a member field.
         * This is used internally by the registration macros.
         */
        template<typename Class, typename FieldType, FieldType Class::*FieldPtr>
        FieldInfo MakeFieldInfo(std::string_view fieldName)
        {
            using DecayedType = std::decay_t<FieldType>;

            FieldInfo info{};

            info.name = fieldName;
            info.nameHash = Detail::XXHash::XXHash64(fieldName);
            info.typeHash = TypeID<DecayedType>::Hash();

            // Calculate offset using a null pointer trick
            info.offset = reinterpret_cast<size_t>(
                &(static_cast<Class*>(nullptr)->*FieldPtr));
            info.size = sizeof(FieldType);
            info.alignment = alignof(FieldType);

            // Type traits
            info.isConst = std::is_const_v<FieldType>;
            info.isPointer = std::is_pointer_v<FieldType>;
            info.isReference = std::is_reference_v<FieldType>;
            info.isTrivial = std::is_trivially_copyable_v<DecayedType>;
            info.isArithmetic = std::is_arithmetic_v<DecayedType>;
            info.isEnum = std::is_enum_v<DecayedType>;
            info.isClass = std::is_class_v<DecayedType>;
            info.isArray = std::is_array_v<FieldType>;
            // std::array is the only ContainerTraits specialization with HasFixedSize
            // set; std::vector is a contiguous, non-fixed-size sequence that is not
            // std::basic_string (which shares the same IsSequence/HasContiguousStorage/
            // !HasFixedSize fingerprint).
            info.isStdArray = ContainerTraits<DecayedType>::HasFixedSize;
            info.isVector = ContainerTraits<DecayedType>::IsSequence
                && ContainerTraits<DecayedType>::HasContiguousStorage
                && !ContainerTraits<DecayedType>::HasFixedSize
                && !ContainerIsStringTrait<DecayedType>::value;

            // Type-erased getter
            info.getter = [](const void* instance, void* outValue) {
                const Class* obj = static_cast<const Class*>(instance);
                *static_cast<DecayedType*>(outValue) = obj->*FieldPtr;
            };

            // Type-erased setter (only for non-const fields)
            if constexpr (!std::is_const_v<FieldType>)
            {
                info.setter = [](void* instance, const void* inValue) {
                    Class* obj = static_cast<Class*>(instance);
                    obj->*FieldPtr = *static_cast<const DecayedType*>(inValue);
                };
            }

            // AnyValue getter (RTTI-free)
            info.getterAny = [](const void* instance) -> AnyValue {
                const Class* obj = static_cast<const Class*>(instance);
                return AnyValue(obj->*FieldPtr);
            };

            // AnyValue setter (TryCast returns null on type mismatch -- never throws)
            if constexpr (!std::is_const_v<FieldType>)
            {
                info.setterAny = [](void* instance, const AnyValue& value) -> bool {
                    const DecayedType* ptr = value.TryCast<DecayedType>();
                    if (!ptr)
                    {
                        return false;
                    }
                    Class* obj = static_cast<Class*>(instance);
                    obj->*FieldPtr = *ptr;
                    return true;
                };
            }

            return info;
        }
    }
}
