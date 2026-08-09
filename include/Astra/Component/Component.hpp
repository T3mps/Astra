#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    class TypeMeta;          // Forward declaration for reflection integration
    class IFieldVisitor;     // Forward declaration for format-agnostic visitor seam

    // Per-module TypeMeta rebuild callback, retained by an owned component slot
    // so a later hot-reload/rebind can regenerate the meta against whichever
    // module is live. See Detail::MetaFactory<T> / Detail::BuildMetaThunk<T>
    // (MetaRegistry.hpp) for the adapter that targets this signature.
    using MetaBuildFn = TypeMeta (*)();

    /**
     * Opt-in enableability (spec 2026-07-25 §2). A component is enableable iff
     * it declares `static constexpr bool AstraEnableable = true` or specializes
     * Astra::EnableableTraits<T>. Enableable columns carry per-chunk disabled
     * bits; everything else pays nothing.
     */
    template<typename T>
    struct EnableableTraits
    {
        // NOTE: `requires {...} && T::AstraEnableable` (the spec's literal shape) does
        // NOT compile as a single expression -- the right operand of && is not inside
        // the requires-expression's SFINAE-protected scope, so for a T lacking the
        // member (or a non-class T like `int`) MSVC hard-errors resolving `T::AstraEnableable`
        // instead of short-circuiting. if constexpr genuinely gates instantiation of the
        // branch not taken, which is what the spec's short-circuit intent requires.
        static constexpr bool value = []() constexpr
        {
            if constexpr (requires { { T::AstraEnableable } -> std::convertible_to<bool>; })
                return T::AstraEnableable;
            else
                return false;
        }();
    };

    template<typename T>
    inline constexpr bool IsEnableableV = EnableableTraits<std::remove_const_t<T>>::value;

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
        using VisitFieldsFn = void(void* instance, IFieldVisitor& visitor);  // reflection-driven, format-agnostic

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
        bool is_trivially_default_constructible;
        bool is_trivially_destructible = false;   // ONLY trait bool with a default: false = always-call-fn-ptr, so a descriptor built outside the registry factory stays safe; siblings are factory-assigned only
        bool is_empty;
        bool isEnableable = false;   // defaults false (same rationale as is_trivially_destructible above): a hand-built descriptor stays safe; the registry factory assigns IsEnableableV<T>
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

        // Reflection integration - linked at registration time if type is reflected
        const TypeMeta* meta = nullptr;

        // Reflection-driven, format-agnostic serialization seam. Populated from
        // TypeMeta at registration; null when the type is not reflected. The
        // binary path (serialize/deserialize above) is unaffected.
        VisitFieldsFn* visitFields = nullptr;

        inline void DefaultConstruct(void* ptr) const
        {
            if (size == 0)
            {
                return;  // empty (tag) component: nothing to construct
            }
            if (is_trivially_default_constructible)
            {
                // Value-initialization of a trivially-default-constructible
                // type is zero-initialization; memset is the fast equivalent.
                // This must run in ALL configs: a chunk slot vacated by
                // swap-and-pop still holds the previous entity's bytes.
                std::memset(ptr, 0, size);
            }
            else
            {
                defaultConstruct(ptr);  // applies NSDMIs / user default ctor
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
            if (size == 0)
            {
                return;
            }
            if (is_trivially_default_constructible)
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
            if (size == 0) return;  // empty (tag) component: nothing to destruct (mirrors DefaultConstruct)
            if (is_trivially_destructible)
            {
                return;  // trivial destructor is a no-op: skip the indirect call entirely
            }
            destruct(ptr);
        }
    };

    // Stable, non-null pointer used as the "component pointer" for a present zero-size (tag)
    // component: by-hash/name Get returns it and by-ID signals fire with it, so Get(...) !=
    // nullptr agrees with Has(...). It points at a real static byte; callers must NOT read
    // through it (a tag has no data -- desc.size == 0). All tags share this one address; the
    // ComponentID carried alongside identifies the type.
    //
    // Note: a present tag deliberately has more than one distinct non-null "do-not-deref"
    // pointer convention across surfaces -- this sentinel on the by-hash/name Get and by-ID
    // signal paths, versus a per-type &emptyInstance on the typed AddComponent<T>/RemoveComponent<T>
    // signal paths. Both mean "present, no data"; do NOT compare a tag's component pointer for
    // identity across paths. (Registry::InspectEntity still reports nullptr for a tag -- a
    // tracked follow-up to unify on this sentinel.)
    inline void* EmptyComponentSentinel() noexcept
    {
        static std::byte sentinel{};
        return &sentinel;
    }
}