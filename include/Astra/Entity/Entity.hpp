#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#include "../Core/Simd.hpp"

// ============================================================================
// Entity Configuration
// ============================================================================
// Users can configure entity sizing by defining these before including Astra:
//
// #define ASTRA_ENTITY_BITS 32           // Total bits (16, 32, or 64)
// #define ASTRA_ENTITY_VERSION_BITS 8    // Version bits
//
// Common configurations:
// - Small: 16-bit total, 4-bit version = 4K entities, 16 versions
// - Default: 32-bit total, 8-bit version = 16M entities, 256 versions
// - Large: 64-bit total, 32-bit version = 4B entities, 4B versions
// - Custom: 64-bit total, 16-bit version = 281T entities, 65K versions
// ============================================================================

namespace Astra
{
    namespace Detail
    {
        template<typename T>
        concept EntityTraitsConcept = requires
        {
            typename T::StorageType;
            typename T::VersionType;

            { T::ID_BITS } -> std::convertible_to<std::size_t>;
            { T::VERSION_SHIFT } -> std::convertible_to<std::size_t>;
            { T::ID_MASK } -> std::convertible_to<typename T::StorageType>;
            { T::VERSION_MASK } -> std::convertible_to<typename T::StorageType>;
            { T::INVALID } -> std::convertible_to<typename T::StorageType>;

            requires std::unsigned_integral<typename T::StorageType>;
            requires std::unsigned_integral<typename T::VersionType>;
        };

        template<EntityTraitsConcept Traits>
        class BasicEntity
        {
        public:
            using StorageType = typename Traits::StorageType;
            using VersionType = typename Traits::VersionType;

            static constexpr auto ID_MASK       = Traits::ID_MASK;
            static constexpr auto VERSION_MASK  = Traits::VERSION_MASK;
            static constexpr auto VERSION_SHIFT = Traits::VERSION_SHIFT;

            constexpr BasicEntity() noexcept : m_entity{Traits::INVALID} {}
            constexpr explicit BasicEntity(StorageType value) noexcept : m_entity{value} {}
            constexpr BasicEntity(StorageType id, VersionType version) noexcept: m_entity{(static_cast<StorageType>(version) << VERSION_SHIFT) | (id & ID_MASK)} {}

            ASTRA_NODISCARD constexpr explicit operator bool() const noexcept { return IsValid(); }

            ASTRA_NODISCARD constexpr operator StorageType() const noexcept { return m_entity; }

            ASTRA_NODISCARD constexpr bool operator==(const BasicEntity& other) const noexcept = default;
            ASTRA_NODISCARD constexpr bool operator==(StorageType value) const noexcept { return m_entity == value; }
            ASTRA_NODISCARD friend constexpr bool operator==(StorageType value, const BasicEntity& entity) noexcept { return entity.m_entity == value; }

            ASTRA_NODISCARD constexpr bool operator<(const BasicEntity& other) const noexcept { return m_entity < other.m_entity; }
            ASTRA_NODISCARD constexpr bool operator>(const BasicEntity& other) const noexcept { return m_entity > other.m_entity; }
            ASTRA_NODISCARD constexpr bool operator<=(const BasicEntity& other) const noexcept { return m_entity <= other.m_entity; }
            ASTRA_NODISCARD constexpr bool operator>=(const BasicEntity& other) const noexcept { return m_entity >= other.m_entity; }
            
            ASTRA_NODISCARD constexpr auto operator<=>(const BasicEntity& other) const noexcept = default;

            template<typename T>
            ASTRA_NODISCARD constexpr explicit operator T() const noexcept
                requires std::convertible_to<StorageType, T> && (!std::same_as<T, bool>)
            { 
                return static_cast<T>(m_entity);
            }

            ASTRA_NODISCARD constexpr BasicEntity NextVersion() const noexcept
            {
                const auto currentVersion = GetVersion();
                const auto currentID = GetID();

                if (currentVersion >= VERSION_MASK)
                {
                    return Invalid();
                }

                return BasicEntity(currentID, currentVersion + 1);
            }

            ASTRA_NODISCARD constexpr StorageType GetID() const noexcept { return m_entity & ID_MASK; }
            ASTRA_NODISCARD constexpr VersionType GetVersion() const noexcept { return static_cast<VersionType>(m_entity >> VERSION_SHIFT) & VERSION_MASK; }
            ASTRA_NODISCARD constexpr StorageType GetValue() const noexcept { return m_entity; }

            ASTRA_NODISCARD constexpr bool IsValid() const noexcept { return m_entity != Traits::INVALID; }
            ASTRA_NODISCARD constexpr bool IsInvalid() const noexcept { return m_entity == Traits::INVALID; }
            
            ASTRA_NODISCARD static constexpr BasicEntity Invalid() noexcept { return BasicEntity{Traits::INVALID}; }

        private:
            StorageType m_entity;
        };
    }

    template<std::size_t TotalBits, std::size_t VersionBits>
    struct EntityTraits
    {
        static_assert(TotalBits == 16 || TotalBits == 32 || TotalBits == 64, "Only 16, 32 or 64 bit variants supported");
        static_assert(VersionBits < TotalBits, "Version bits must be less than total bits");

        using StorageType = std::conditional_t<TotalBits == 16, std::uint16_t,
                                std::conditional_t<TotalBits == 32, std::uint32_t, std::uint64_t>>;
        using VersionType = std::conditional_t<VersionBits <= 8, std::uint8_t,
                                std::conditional_t<VersionBits <= 16, std::uint16_t, std::uint32_t>>;

        static constexpr std::size_t ID_BITS = TotalBits - VersionBits;
        static constexpr std::size_t VERSION_SHIFT = ID_BITS;
        static constexpr StorageType ID_MASK = (StorageType{1} << ID_BITS) - 1;
        static constexpr StorageType VERSION_MASK = (StorageType{1} << VersionBits) - 1;
        static constexpr StorageType INVALID = std::numeric_limits<StorageType>::max();
        // Max ID that can be safely allocated without collision with INVALID
        // When maxID is combined with maxVersion, it equals INVALID, so we reserve it
        static constexpr StorageType MAX_SAFE_ID = ID_MASK - 1;
    };

#ifndef ASTRA_ENTITY_BITS
    #define ASTRA_ENTITY_BITS 32LLU
#endif
#ifndef ASTRA_ENTITY_VERSION_BITS
    #define ASTRA_ENTITY_VERSION_BITS 8LLU
#endif

    static_assert(ASTRA_ENTITY_BITS == 16 || ASTRA_ENTITY_BITS == 32 || ASTRA_ENTITY_BITS == 64, "ASTRA_ENTITY_BITS must be 16, 32, or 64");
    static_assert(ASTRA_ENTITY_VERSION_BITS < ASTRA_ENTITY_BITS, "ASTRA_ENTITY_VERSION_BITS must be less than ASTRA_ENTITY_BITS");
    static_assert(ASTRA_ENTITY_VERSION_BITS <= 32, "ASTRA_ENTITY_VERSION_BITS cannot exceed 32");
    
    using Entity = Detail::BasicEntity<EntityTraits<ASTRA_ENTITY_BITS, ASTRA_ENTITY_VERSION_BITS>>;
    
#ifndef ASTRA_MAX_ENTITIES
    // Max ID (ID_MASK) is reserved to prevent collision with INVALID sentinel
    // so max allocatable entities is ID_MASK (IDs 0 to ID_MASK-1)
    #define ASTRA_MAX_ENTITIES (Entity::ID_MASK)
#endif

    constexpr std::size_t MAX_ENTITIES = ASTRA_MAX_ENTITIES;

    struct EntityHash
    {
        std::size_t operator()(const Entity& entity) const noexcept
        {
            uint64_t hash = entity.GetValue();
            hash = Simd::Ops::HashCombine(hash, 0x9E3779B97F4A7C15ULL);
            // Swiss table H2 extracts high 7 bits: (hash >> 57) & 0x7F
            // H2 must be in range [1, 127] for proper match operations
            // Check if high 7 bits are zero and fix them
            // Note: SwissTable::H2 (Container/Swiss.hpp) now guarantees this
            // centrally for every hasher; this fixup is kept as redundant hardening.
            if (((hash >> 57) & 0x7F) == 0)
            {
                // Set bit 57 to ensure H2 != 0
                hash |= (1ULL << 57);
            }
            return hash;
        }
    };
}

namespace std
{
    template<>
    struct hash<Astra::Entity>
    {
        ASTRA_NODISCARD std::size_t operator()(const Astra::Entity& entity) const noexcept
        {
            return Astra::EntityHash{}(entity);
        }
    };
}
