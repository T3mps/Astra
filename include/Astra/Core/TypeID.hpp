#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "../Component/Component.hpp"
#include "../Core/Platform.hpp"
#include "Base.hpp"
#include "TypeContext.hpp"
#include "TypeNameCanonical.hpp"

#if defined(__cpp_rtti) || defined(_CPPRTTI)
#include <typeinfo>
#endif

namespace Astra
{
    // Forward declaration so Detail::TypeIDStorage::Value() can name it under
    // two-phase lookup (gcc/clang); defined after struct TypeID below.
    template<typename T>
    ASTRA_NODISCARD TypeIdentity MakeTypeIdentity() noexcept;

    namespace Detail
    {
        // Compile-time XXHash64 implementation for type name hashing
        // Based on XXHash specification: https://github.com/Cyan4973/xxHash
        namespace XXHash
        {
            inline constexpr uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
            inline constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
            inline constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
            inline constexpr uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
            inline constexpr uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

            constexpr uint64_t RotateLeft(uint64_t value, int count) noexcept
            {
                return (value << count) | (value >> (64 - count));
            }

            constexpr uint64_t Round(uint64_t acc, uint64_t input) noexcept
            {
                acc += input * PRIME64_2;
                acc = RotateLeft(acc, 31);
                acc *= PRIME64_1;
                return acc;
            }

            constexpr uint64_t MergeRound(uint64_t acc, uint64_t val) noexcept
            {
                val = Round(0, val);
                acc ^= val;
                acc = acc * PRIME64_1 + PRIME64_4;
                return acc;
            }

            constexpr uint64_t XXHash64(const char* data, size_t len, uint64_t seed = 0) noexcept
            {
                const char* const end = data + len;
                uint64_t h64;

                if (len >= 32)
                {
                    const char* const limit = end - 32;
                    uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
                    uint64_t v2 = seed + PRIME64_2;
                    uint64_t v3 = seed + 0;
                    uint64_t v4 = seed - PRIME64_1;

                    do
                    {
                        // Process 32 bytes per iteration
                        uint64_t k1 = 0, k2 = 0, k3 = 0, k4 = 0;
                        
                        // Read 8 bytes at a time (little-endian)
                        for (int i = 0; i < 8; ++i) {
                            k1 |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (i * 8);
                            k2 |= static_cast<uint64_t>(static_cast<uint8_t>(data[i + 8])) << (i * 8);
                            k3 |= static_cast<uint64_t>(static_cast<uint8_t>(data[i + 16])) << (i * 8);
                            k4 |= static_cast<uint64_t>(static_cast<uint8_t>(data[i + 24])) << (i * 8);
                        }

                        v1 = Round(v1, k1);
                        v2 = Round(v2, k2);
                        v3 = Round(v3, k3);
                        v4 = Round(v4, k4);
                        
                        data += 32;
                    } while (data <= limit);

                    h64 = RotateLeft(v1, 1) + RotateLeft(v2, 7) + 
                          RotateLeft(v3, 12) + RotateLeft(v4, 18);
                    
                    h64 = MergeRound(h64, v1);
                    h64 = MergeRound(h64, v2);
                    h64 = MergeRound(h64, v3);
                    h64 = MergeRound(h64, v4);
                }
                else
                {
                    h64 = seed + PRIME64_5;
                }

                h64 += len;

                // Process remaining bytes
                while (end - data >= 8)
                {
                    uint64_t k1 = 0;
                    for (int i = 0; i < 8; ++i) {
                        k1 |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (i * 8);
                    }
                    
                    k1 *= PRIME64_2;
                    k1 = RotateLeft(k1, 31);
                    k1 *= PRIME64_1;
                    h64 ^= k1;
                    h64 = RotateLeft(h64, 27) * PRIME64_1 + PRIME64_4;
                    data += 8;
                }

                if (end - data >= 4)
                {
                    uint32_t k1 = 0;
                    for (int i = 0; i < 4; ++i) {
                        k1 |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (i * 8);
                    }
                    
                    h64 ^= k1 * PRIME64_1;
                    h64 = RotateLeft(h64, 23) * PRIME64_2 + PRIME64_3;
                    data += 4;
                }

                while (data < end)
                {
                    h64 ^= static_cast<uint8_t>(*data) * PRIME64_5;
                    h64 = RotateLeft(h64, 11) * PRIME64_1;
                    data++;
                }

                // Finalization
                h64 ^= h64 >> 33;
                h64 *= PRIME64_2;
                h64 ^= h64 >> 29;
                h64 *= PRIME64_3;
                h64 ^= h64 >> 32;

                return h64;
            }

            constexpr uint64_t XXHash64(std::string_view str, uint64_t seed = 0) noexcept
            {
                return XXHash64(str.data(), str.size(), seed);
            }
        }

        // Index of the first ';' in s that is not nested inside <>, (), [], {}
        // or a character literal; s.size() when there is none.
        constexpr size_t FindTopLevelSemicolon(std::string_view s) noexcept
        {
            int depth = 0;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '\'')
                {
                    // Skip a character literal (a char NTTP may print as '<' or '\'').
                    for (++i; i < s.size() && s[i] != '\''; ++i)
                    {
                        if (s[i] == '\\')
                            ++i;
                    }
                    continue;
                }
                if (c == '<' || c == '(' || c == '[' || c == '{')
                    ++depth;
                else if (c == '>' || c == ')' || c == ']' || c == '}')
                    --depth;
                else if (c == ';' && depth == 0)
                    return i;
            }
            return s.size();
        }

        // The compiler's own spelling of T, sliced out of the pretty-function
        // string. Differs between compilers; see TypeNameCanonical.hpp.
        template<typename T>
        constexpr std::string_view RawTypeName() noexcept
        {
            #if defined(ASTRA_COMPILER_MSVC)
                // MSVC: __FUNCSIG__ gives "class std::basic_string_view<...> __cdecl
                // Astra::Detail::RawTypeName<struct MyClass>(void) noexcept"
                constexpr std::string_view funcName = __FUNCSIG__;
                constexpr std::string_view prefix = "RawTypeName<";
                constexpr std::string_view suffix = ">(void)";
            #elif defined(ASTRA_COMPILER_CLANG)
                // Clang: __PRETTY_FUNCTION__ gives "std::string_view Astra::Detail::RawTypeName() [T = MyClass]"
                constexpr std::string_view funcName = __PRETTY_FUNCTION__;
                constexpr std::string_view prefix = "RawTypeName() [T = ";
                constexpr std::string_view suffix = "]";
            #elif defined(ASTRA_COMPILER_GCC)
                // GCC: __PRETTY_FUNCTION__ gives "constexpr std::string_view
                // Astra::Detail::RawTypeName() [with T = MyClass; std::string_view = ...]"
                constexpr std::string_view funcName = __PRETTY_FUNCTION__;
                constexpr std::string_view prefix = "RawTypeName() [with T = ";
                constexpr std::string_view suffix = "]";
            #else
                #error "Unsupported compiler for compile-time type name extraction"
            #endif

            // Find the start of the type name
            size_t start = funcName.find(prefix);
            if (start == std::string_view::npos)
                return "Unknown";
            start += prefix.length();

            // Find the end of the type name
            size_t end = funcName.rfind(suffix);
            if (end == std::string_view::npos || end <= start)
                return "Unknown";

            #if defined(ASTRA_COMPILER_GCC)
                // GCC lists the function's other template-dependent bindings after
                // T, ';'-separated: "[with T = Foo; std::string_view =
                // std::basic_string_view<char>]". Without this cut every GCC name
                // (and so every TypeHash) carries that tail, and name-keyed lookups
                // (MetaRegistry::GetByName, Registry::GetComponentByName) never match.
                end = start + FindTopLevelSemicolon(funcName.substr(start, end - start));
            #endif

            return funcName.substr(start, end - start);
        }

        template<typename T>
        constexpr auto MakeCanonicalTypeNameScratch() noexcept
        {
            constexpr std::string_view raw = RawTypeName<T>();
            CanonicalTypeNameBuffer<CanonicalTypeNameCapacity(raw.size())> buffer{};
            buffer.size = CanonicalizeTypeName(raw, buffer.chars.data(), buffer.chars.size());
            return buffer;
        }

        template<typename T>
        inline constexpr auto kCanonicalTypeNameScratch = MakeCanonicalTypeNameScratch<T>();

        // Exact-size, NUL-terminated storage for the canonical name.
        template<typename T>
        constexpr auto MakeCanonicalTypeName() noexcept
        {
            constexpr size_t size = kCanonicalTypeNameScratch<T>.size;
            static_assert(size <= kCanonicalTypeNameScratch<T>.chars.size(),
                "Astra: canonical type name exceeds its scratch capacity");
            std::array<char, size + 1> chars{};
            for (size_t i = 0; i < size; ++i)
                chars[i] = kCanonicalTypeNameScratch<T>.chars[i];
            return chars;
        }

        template<typename T>
        inline constexpr auto kCanonicalTypeName = MakeCanonicalTypeName<T>();

        // Compile-time type name in Astra's canonical, compiler-independent
        // spelling (TypeNameCanonical.hpp). This is what TypeID<T>::Name()
        // returns and what TypeID<T>::Hash() hashes.
        template<typename T>
        constexpr std::string_view TypeNameInternal() noexcept
        {
            return std::string_view(kCanonicalTypeName<T>.data(), kCanonicalTypeName<T>.size() - 1);
        }

        template<typename T>
        constexpr uint64_t TypeHash() noexcept
        {
            constexpr std::string_view name = TypeNameInternal<T>();
            return XXHash::XXHash64(name);
        }
        
        template<typename T>
        class TypeIDStorage
        {
        public:
            // IDs are assigned by the active TypeContext, keyed by the STABLE
            // type-name hash, so every module sharing one context agrees on
            // the ID. The result is cached in a per-module magic static --
            // C++11 magic statics guarantee thread-safe one-time resolution.
            // Not noexcept: first resolution may allocate in the context.
            ASTRA_NODISCARD static ComponentID Value()
            {
                static const ComponentID s_id =
                    GetTypeContext()->GetOrAssignComponentID(
                        TypeHash<T>(), TypeNameInternal<T>(), MakeTypeIdentity<T>());
                return s_id;
            }
        };
    }

    // NOTE: type identity derives from the compiler pretty-name, so every type
    // must have a UNIQUE unqualified name. Two distinct types that share a
    // pretty-name (e.g. same-named types in anonymous namespaces in different
    // translation units) collide; the collision is DETECTED at id assignment and
    // the second type is refused with a logged error (see
    // TypeContext::GetOrAssignComponentID). Detection is complete wherever RTTI is
    // enabled; in RTTI-off builds it catches every collision whose types differ in
    // size/alignment/triviality (the memory-corrupting cases), but two distinct
    // types with identical layout evade detection (logical mislabel, not corruption).
    template<typename T>
    struct TypeID
    {
        using Type = std::decay_t<T>;

        // Runtime-assigned unique ID for this type (fast, but not stable across runs).
        // Assigned by the active TypeContext; not noexcept (first use may allocate).
        ASTRA_NODISCARD static ComponentID Value()
        {
            return Detail::TypeIDStorage<Type>::Value();
        }

        // Compile-time type name in Astra's canonical spelling: the same string on
        // MSVC, GCC and Clang, across recompiles, for every type outside the
        // exceptions listed in TypeNameCanonical.hpp (standard-library templates
        // with defaulted arguments, enumerator template arguments, data-model
        // dependent aliases such as std::int64_t, lambdas).
        ASTRA_NODISCARD static constexpr std::string_view Name() noexcept
        {
            return Detail::TypeNameInternal<Type>();
        }

        // Compile-time hash of Name(): the serialization identity binary archives
        // persist, so it is stable exactly where Name() is (see above).
        // Uses XXHash64 for excellent distribution and virtually zero collision risk
        ASTRA_NODISCARD static constexpr uint64_t Hash() noexcept
        {
            return Detail::TypeHash<Type>();
        }

        // Check if the compile-time hash matches a given value
        ASTRA_NODISCARD static constexpr bool HasHash(uint64_t hash) noexcept
        {
            return Hash() == hash;
        }
    };

    // Build the per-type discriminator used by TypeContext to DETECT identity
    // collisions (see TypeContext::GetOrAssignComponentID). Structural fields are
    // always present (all configs); the RTTI discriminator is added only where RTTI
    // is enabled. Never enters the hash/id/wire format.
    template<typename T>
    ASTRA_NODISCARD inline TypeIdentity MakeTypeIdentity() noexcept
    {
        TypeIdentity id;
        id.size  = static_cast<uint32_t>(sizeof(T));
        id.align = static_cast<uint32_t>(alignof(T));
        id.flags = static_cast<uint8_t>(
            (std::is_trivially_copyable_v<T>    ? TIF_TriviallyCopyable     : 0) |
            (std::is_trivially_destructible_v<T> ? TIF_TriviallyDestructible : 0) |
            (std::is_empty_v<T>                 ? TIF_Empty                 : 0));
#if defined(__cpp_rtti) || defined(_CPPRTTI)
        // Hash the RTTI MANGLED name (not the pointer): an owned value that survives
        // the source image's unload (see TypeIdentity). MSVC's raw_name() carries the
        // per-TU tag operator== compares; the Itanium name() matches down to one
        // documented residual -- see TypeContext::IsTypeIdentityCollision.
    #if defined(_MSC_VER)
        id.rttiName = Detail::XXHash::XXHash64(std::string_view(typeid(T).raw_name()));
    #else
        id.rttiName = Detail::XXHash::XXHash64(std::string_view(typeid(T).name()));
    #endif
#endif
        return id;
    }
}
