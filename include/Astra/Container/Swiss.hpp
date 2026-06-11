#pragma once

#include <functional>
#include <cstdint>
#include "../Entity/Entity.hpp"

namespace Astra
{
    // Shared SwissTable utilities for FlatMap and FlatSet
    
    // Hash selection template
    template<typename Key>
    struct SelectHash
    {
        using Type = std::hash<Key>;
    };
    
    // Specialize for Entity to use our optimized hash
    template<>
    struct SelectHash<Entity>
    {
        using Type = EntityHash;
    };
    
    // SwissTable metadata constants
    namespace SwissTable
    {
        // Metadata byte values
        static constexpr uint8_t EMPTY = 0x80;     // 10000000
        static constexpr uint8_t DELETED = 0xFE;   // 11111110
        static constexpr uint8_t SENTINEL = 0xFF;  // 11111111
        
        // Group size for SIMD operations
        static constexpr size_t GROUP_SIZE = 16;
        
        // Maximum load factor (87.5%)
        static constexpr float MAX_LOAD_FACTOR = 0.875f;
        
        // Hash splitting functions
        inline uint8_t H2(size_t hash) noexcept
        {
            // Use top 7 bits for metadata. Group::Match assumes the probe byte
            // is in [1, 127], but degenerate hashers can produce all-zero high
            // bits, so fold 0 to 1 here. This guarantees the valid range
            // centrally for every hasher (collisions on 1 are legal).
            uint8_t h2 = static_cast<uint8_t>(hash >> 57) & 0x7F;
            return h2 == 0 ? static_cast<uint8_t>(1) : h2;
        }
        
        inline size_t H1(size_t hash, size_t capacity) noexcept
        {
            // Use bottom 57 bits for position
            return hash & (capacity - 1);
        }
        
        // Check if metadata byte represents a full slot
        inline bool IsFull(uint8_t meta) noexcept
        {
            return meta < EMPTY;
        }
        
        // Check if metadata byte represents an empty slot
        inline bool IsEmpty(uint8_t meta) noexcept
        {
            return meta == EMPTY;
        }
        
        // Check if metadata byte represents a deleted slot
        inline bool IsDeleted(uint8_t meta) noexcept
        {
            return meta == DELETED;
        }
        
        // Check if metadata byte represents an empty or deleted slot
        inline bool IsEmptyOrDeleted(uint8_t meta) noexcept
        {
            return meta >= DELETED;
        }
    }
    
} // namespace Astra