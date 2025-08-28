#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>
#include "../Container/Bitmap.hpp"

namespace Astra
{
    using ComponentID = std::uint16_t;

    inline constexpr ComponentID INVALID_COMPONENT = std::numeric_limits<ComponentID>::max();

    #ifndef ASTRA_MAX_COMPONENTS
        #define ASTRA_MAX_COMPONENTS 128u
    #endif

    constexpr std::size_t MAX_COMPONENTS = ASTRA_MAX_COMPONENTS;
    
    using ComponentMask = Bitmap<MAX_COMPONENTS>;

    template<typename T>
    concept Component = std::is_nothrow_move_assignable_v<std::remove_const_t<T>> &&
                        std::is_nothrow_destructible_v<std::remove_const_t<T>> &&
                        std::is_move_constructible_v<std::remove_const_t<T>> &&
                        std::is_move_assignable_v<std::remove_const_t<T>> &&
                        std::is_destructible_v<std::remove_const_t<T>>;

    class BinaryWriter;
    class BinaryReader;
    
    struct ComponentDescriptor
    {
        using ConstructFn = void(void*);
        using DestructFn = void(void*);
        using CopyConstructFn = void(void*, const void*);
        using MoveConstructFn = void(void*, void*);
        using MoveAssignFn = void(void*, void*);
        using CopyAssignFn = void(void*, const void*);
        using ConstructWithFn = void(void*, const void*);  // Construct with value
        
        using SerializeFn = void(BinaryWriter&, void*);              // Non-const for unified Serialize method
        using DeserializeFn = void(BinaryReader&, void*);
        using SerializeVersionedFn = void(BinaryWriter&, void*);     // Non-const for unified Serialize method
        using DeserializeVersionedFn = bool(BinaryReader&, void*);  // Returns false on error

        ComponentID id;
        size_t size;
        size_t alignment;
        uint64_t hash;           // XXHash64 of component type name
        const char* name;        // Component type name (for debugging)
        uint32_t version;        // Current component version
        uint32_t minVersion;     // Minimum supported version for migration
        bool is_trivially_copyable;
        bool is_copy_constructible;
        bool is_nothrow_move_constructible;
        bool is_nothrow_default_constructible;
        bool is_empty;
        ConstructFn* defaultConstruct;
        DestructFn* destruct;
        CopyConstructFn* copyConstruct;
        MoveConstructFn* moveConstruct;
        MoveAssignFn* moveAssign;
        CopyAssignFn* copyAssign;
        ConstructWithFn* constructWith;            // Construct with provided value
        SerializeFn* serialize;                    // Basic serialization
        DeserializeFn* deserialize;                // Basic deserialization
        SerializeVersionedFn* serializeVersioned;  // Versioned serialization
        DeserializeVersionedFn* deserializeVersioned; // Versioned deserialization with migration
        inline void DefaultConstruct(void* ptr) const
        {
            if (is_trivially_copyable && is_nothrow_default_constructible)
            {
#ifdef ASTRA_BUILD_DEBUG
                std::memset(ptr, 0, size);
#endif
            }
            else
            {
                defaultConstruct(ptr);
            }
        }
        
        inline void ConstructWith(void* ptr, const void* value) const
        {
            if (constructWith)
            {
                constructWith(ptr, value);
            }
            else if (copyConstruct)
            {
                // Fallback to copy construct if no specialized constructWith
                copyConstruct(ptr, value);
            }
            else
            {
                // Should not happen for properly registered components
                defaultConstruct(ptr);
            }
        }
        
        inline void BatchDefaultConstruct(void* ptr, size_t count) const
        {
            if (is_empty)
            {
            }
            
            if (is_trivially_copyable && is_nothrow_default_constructible)
            {
                std::memset(ptr, 0, count * size);
            }
            else
            {
                std::byte* p = static_cast<std::byte*>(ptr);
                for (size_t i = 0; i < count; ++i)
                {
                    defaultConstruct(p + i * size);
                }
            }
        }
        
        inline void MoveConstruct(void* dst, void* src) const
        {
            if (is_trivially_copyable)
            {
                std::memcpy(dst, src, size);
            }
            else
            {
                moveConstruct(dst, src);
            }
        }
        
        inline void Destruct(void* ptr) const
        {
            destruct(ptr);
        }
    };
}