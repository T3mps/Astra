#pragma once

#include <cstdint>
#include <limits>

#include "../Core/Base.hpp"

namespace Astra
{
    struct EntityLocation
    {
        uint32_t chunkIndex;
        uint32_t entityIndex;

        constexpr EntityLocation() noexcept :
            chunkIndex(std::numeric_limits<uint32_t>::max()),
            entityIndex(std::numeric_limits<uint32_t>::max())
        {}

        constexpr EntityLocation(uint32_t chunk, uint32_t entity) noexcept : chunkIndex(chunk), entityIndex(entity) {}

        ASTRA_NODISCARD constexpr static EntityLocation Create(size_t chunkIndex, size_t entityIndex) noexcept
        {
            return EntityLocation(static_cast<uint32_t>(chunkIndex), static_cast<uint32_t>(entityIndex));
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
}
