#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <array>

#include "../Component/Component.hpp"
#include "../Core/Platform.hpp"
#include "Base.hpp"

namespace Astra
{
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

        // Cross-platform compile-time type name extraction
        template<typename T>
        constexpr std::string_view TypeNameInternal() noexcept
        {
            #if defined(ASTRA_COMPILER_MSVC)
                // MSVC: __FUNCSIG__ gives "auto __cdecl TypeName<class MyClass>(void)"
                constexpr std::string_view funcName = __FUNCSIG__;
                constexpr std::string_view prefix = "TypeNameInternal<";
                constexpr std::string_view suffix = ">(void)";
            #elif defined(ASTRA_COMPILER_CLANG)
                // Clang: __PRETTY_FUNCTION__ gives "std::string_view TypeName() [T = MyClass]"
                constexpr std::string_view funcName = __PRETTY_FUNCTION__;
                constexpr std::string_view prefix = "TypeNameInternal() [T = ";
                constexpr std::string_view suffix = "]";
            #elif defined(ASTRA_COMPILER_GCC)
                // GCC: __PRETTY_FUNCTION__ gives "constexpr std::string_view TypeName() [with T = MyClass]"
                constexpr std::string_view funcName = __PRETTY_FUNCTION__;
                constexpr std::string_view prefix = "TypeNameInternal() [with T = ";
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

            // Extract the type name
            std::string_view typeName = funcName.substr(start, end - start);

            // Clean up common prefixes (optional)
            // Remove "class ", "struct ", "enum " prefixes on MSVC
            #if defined(ASTRA_COMPILER_MSVC)
                if (typeName.starts_with("class "))
                    typeName.remove_prefix(6);
                else if (typeName.starts_with("struct "))
                    typeName.remove_prefix(7);
                else if (typeName.starts_with("enum "))
                    typeName.remove_prefix(5);
            #endif

            return typeName;
        }

        template<typename T>
        constexpr uint64_t TypeHash() noexcept
        {
            constexpr std::string_view name = TypeNameInternal<T>();
            return XXHash::XXHash64(name);
        }
        
        class TypeIDGenerator
        {
        public:
            ASTRA_NODISCARD static ComponentID Next() noexcept
            {
                // We only need atomicity, not ordering. Each type's ID is
                // generated once during static initialization and cached.
                // The C++11 magic statics guarantee thread-safe initialization
                // of the static variable in TypeIDStorage::Value().
                return s_nextId.fetch_add(1, std::memory_order_relaxed);
            }

        private:
            inline static std::atomic<ComponentID> s_nextId{0};
        };

        template<typename T>
        class TypeIDStorage
        {
        public:
            ASTRA_NODISCARD static ComponentID Value() noexcept
            {
                static const ComponentID s_id = TypeIDGenerator::Next();
                return s_id;
            }
        };
    }

    template<typename T>
    struct TypeID
    {
        using Type = std::decay_t<T>;

        // Runtime-assigned unique ID for this type (fast, but not stable across runs)
        ASTRA_NODISCARD static ComponentID Value() noexcept
        {
            return Detail::TypeIDStorage<Type>::Value();
        }

        // Compile-time type name (stable across platforms and compilations)
        ASTRA_NODISCARD static constexpr std::string_view Name() noexcept
        {
            return Detail::TypeNameInternal<Type>();
        }

        // Compile-time hash of type name (stable across platforms and compilations)
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

    template<typename T>
    inline const ComponentID TypeID_v = TypeID<T>::Value();
}
