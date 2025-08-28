#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <concepts>
#include <vector>
#include <span>

#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/Simd.hpp"
#include "SerializationError.hpp"

namespace Astra
{
    /**
     * CRC32 checksum utilities using SIMD when available
     */
    namespace Checksum
    {
        /**
         * Calculate CRC32 for a buffer
         * Uses hardware CRC32 instructions when available
         */
        inline uint32_t CRC32(const void* data, size_t size, uint32_t crc = 0)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            uint64_t result = crc;
            
            // Process 8 bytes at a time when possible
            while (size >= 8)
            {
                uint64_t value;
                std::memcpy(&value, bytes, sizeof(uint64_t));
                result = Simd::Ops::HashCombine(result, value);
                bytes += 8;
                size -= 8;
            }
            
            // Process remaining bytes
            if (size > 0)
            {
                uint64_t value = 0;
                std::memcpy(&value, bytes, size);
                result = Simd::Ops::HashCombine(result, value);
            }
            
            return static_cast<uint32_t>(result);
        }
    }
    
    /**
     * Binary format version history:
     * v1: Initial format with component hashing and compression support
     */
    inline constexpr uint16_t BINARY_FORMAT_VERSION = 1;
    inline constexpr char BINARY_MAGIC[6] = "ASTRA";
    
    /**
     * Compression modes for binary archives
     * Value of 0 means no compression, any other value indicates
     * both that compression is enabled and which algorithm to use
     */
    enum class CompressionMode : uint8_t
    {
        None = 0,      // No compression
        LZ4 = 1,       // LZ4 compression
        // Reserved for future algorithms (2 = zstd, etc.)
    };
    
    /**
     * Header structure for binary archives
     * Aligned to 32 bytes for cache efficiency and future expansion
     */
    ASTRA_PACK_BEGIN
    struct BinaryHeader
    {
        char magic[5];               // "ASTRA" - 5 bytes
        uint16_t version;            // Format version - 2 bytes
        uint8_t endianness;          // 0 = little, 1 = big - 1 byte
        uint32_t archetypeCount;     // Number of archetypes - 4 bytes
        uint32_t entityCount;        // Total entity count - 4 bytes
        uint32_t dataChecksum;       // CRC32 of data after header - 4 bytes
        uint8_t compressionMode;     // CompressionMode enum - 1 byte
        uint8_t reserved[11];        // Reserved for future expansion - 11 bytes
        // Total: 5 + 2 + 1 + 4 + 4 + 4 + 1 + 11 = 32 bytes
        
        BinaryHeader() noexcept
        {
            std::memcpy(magic, BINARY_MAGIC, 5);
            version = BINARY_FORMAT_VERSION;
            endianness = IsLittleEndian() ? 0 : 1;
            archetypeCount = 0;
            entityCount = 0;
            dataChecksum = 0;
            compressionMode = static_cast<uint8_t>(CompressionMode::None);
            std::memset(reserved, 0, sizeof(reserved));
        }
        
        [[nodiscard]] bool IsValid() const noexcept
        {
            return std::memcmp(magic, BINARY_MAGIC, 5) == 0;
        }
        
        [[nodiscard]] bool IsVersionSupported() const noexcept
        {
            return version <= BINARY_FORMAT_VERSION;
        }
        
        [[nodiscard]] bool IsEndianCompatible() const noexcept
        {
            return endianness == (IsLittleEndian() ? 0 : 1);
        }
        
        [[nodiscard]] bool IsCompressed() const noexcept
        {
            return compressionMode != static_cast<uint8_t>(CompressionMode::None);
        }
        
        [[nodiscard]] CompressionMode GetCompressionMode() const noexcept
        {
            return static_cast<CompressionMode>(compressionMode);
        }
        
    private:
        [[nodiscard]] static bool IsLittleEndian() noexcept
        {
            uint32_t test = 1;
            return *reinterpret_cast<uint8_t*>(&test) == 1;
        }
    };
    ASTRA_PACK_END
    
    static_assert(sizeof(BinaryHeader) == 32, "BinaryHeader must be exactly 32 bytes");
    
    /**
     * Type trait to detect if a type has a Serialize method
     */
    template<typename T, typename Archive>
    concept HasSerializeMethod = requires(T& t, const T& ct, Archive& ar)
    {
        { t.Serialize(ar) } -> std::same_as<void>;
    } || requires(const T& ct, Archive& ar)
    {
        { ct.Serialize(ar) } -> std::same_as<void>;
    };
    
    /**
     * Base class for binary archive operations
     * Provides common functionality for both reading and writing
     */
    class BinaryArchive
    {
    public:
        BinaryArchive() = default;
        virtual ~BinaryArchive() = default;
        
        BinaryArchive(const BinaryArchive&) = delete;
        BinaryArchive& operator=(const BinaryArchive&) = delete;
        BinaryArchive(BinaryArchive&&) = default;
        BinaryArchive& operator=(BinaryArchive&&) = default;
        
        /**
         * Get the current archive version
         */
        [[nodiscard]] uint16_t GetVersion() const noexcept { return m_version; }
        
        /**
         * Check if we're in loading mode
         */
        [[nodiscard]] virtual bool IsLoading() const noexcept = 0;
        
        /**
         * Check if we're in saving mode
         */
        [[nodiscard]] bool IsSaving() const noexcept { return !IsLoading(); }
        
    protected:
        uint16_t m_version = BINARY_FORMAT_VERSION;
    };
    
    /**
     * Serialization traits for automatic type handling
     * Users can specialize this for their types to provide:
     * - Component version number
     * - Custom serialization logic
     * - Migration from old versions
     */
    template<typename T>
    struct SerializationTraits
    {
        // Type characteristics
        static constexpr bool IsTrivial = std::is_trivially_copyable_v<T>;
        static constexpr bool HasCustomSerializer = false;
        
        // Component version - increment when format changes
        static constexpr uint32_t Version = 1;
        
        // Minimum supported version for backward compatibility
        static constexpr uint32_t MinVersion = 1;
        
        // Default implementation for POD types
        template<typename Archive>
        static void Serialize(Archive& ar, T& value)
        {
            if constexpr (IsTrivial)
            {
                if (ar.IsLoading())
                {
                    ar.ReadBytes(&value, sizeof(T));
                }
                else
                {
                    ar.WriteBytes(&value, sizeof(T));
                }
            }
            else
            {
                static_assert(HasCustomSerializer, "Non-trivial type requires custom serialization");
            }
        }
    };
}
