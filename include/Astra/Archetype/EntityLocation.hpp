#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "../Core/Base.hpp"

namespace Astra
{
    struct EntityLocation
    {
        // Aggregate on purpose (no user-provided constructor; the invalid sentinel
        // comes from the default member initialisers): MSVC x64 then materialises
        // the 8-byte value in a register and returns it with ONE 8-byte store, so an
        // out-of-line AllocateEntitySlot / MoveEntity* caller's 8-byte reload
        // store-forwards instead of stalling on two 4-byte stores. With the user-
        // provided constructors the type was not returned in RAX, and that reload
        // sat on the critical path of AddComponent/RemoveComponent (measured ~4.7 ns
        // per AddComponent, 2026-09-11 change-detection perf triage, Ruling O).
        // Layout (2 x uint32_t), sentinel, accessors and comparisons are unchanged;
        // nothing serialises this type.
        uint32_t chunkIndex  = std::numeric_limits<uint32_t>::max();
        uint32_t entityIndex = std::numeric_limits<uint32_t>::max();

        ASTRA_NODISCARD constexpr static EntityLocation Create(size_t chunkIndex, size_t entityIndex) noexcept
        {
            return EntityLocation{static_cast<uint32_t>(chunkIndex), static_cast<uint32_t>(entityIndex)};
        }

        ASTRA_NODISCARD constexpr size_t GetChunkIndex() const noexcept
        {
            return chunkIndex;
        }

        ASTRA_NODISCARD constexpr size_t GetEntityIndex() const noexcept
        {
            return entityIndex;
        }

        ASTRA_NODISCARD constexpr bool IsValid() const noexcept
        {
            return chunkIndex != std::numeric_limits<uint32_t>::max();
        }

        constexpr bool operator==(const EntityLocation& other) const noexcept
        {
            return chunkIndex == other.chunkIndex && entityIndex == other.entityIndex;
        }
        constexpr bool operator!=(const EntityLocation& other) const noexcept
        {
            return !(*this == other);
        }
        constexpr bool operator<(const EntityLocation& other) const noexcept
        {
            return chunkIndex < other.chunkIndex || (chunkIndex == other.chunkIndex && entityIndex < other.entityIndex);
        }
        constexpr bool operator>(const EntityLocation& other) const noexcept
        {
            return other < *this;
        }
        constexpr bool operator<=(const EntityLocation& other) const noexcept
        {
            return !(other < *this);
        }
        constexpr bool operator>=(const EntityLocation& other) const noexcept
        {
            return !(*this < other);
        }
    };

    // The perf property above is structural: keep it pinned.
    static_assert(std::is_aggregate_v<EntityLocation>,
        "EntityLocation must stay an aggregate (no user-provided constructors) so it is returned in a register -- see the note on the struct");
    static_assert(std::is_trivially_copyable_v<EntityLocation> && sizeof(EntityLocation) == 8,
        "EntityLocation must stay a trivially copyable 8-byte value");
    static_assert(EntityLocation{}.chunkIndex == std::numeric_limits<uint32_t>::max() && !EntityLocation{}.IsValid(),
        "a default-initialised EntityLocation must be the invalid sentinel");
}
