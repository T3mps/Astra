#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <span>

#include "../../Core/Result.hpp"
#include "../SerializationError.hpp"
#include "Internal/smallz4.hpp"
#include "LZ4Decoder.hpp"

namespace Astra::Compression
{
    /**
     * Compression level controls the speed/ratio tradeoff
     * Optimized for real-time use cases (game saves, editor operations)
     */
    enum class CompressionLevel : uint8_t
    {
        Fastest = 1,    // Greedy mode, ~100+ MB/s, good for real-time
        Fast = 3,       // Still greedy, ~90-100 MB/s (default)
        Balanced = 6    // Lazy evaluation, ~60-80 MB/s, better compression
        // Note: Optimal parsing mode removed as it's too slow for real-time use
    };
    
    /**
     * Compressed block header for streaming decompression
     * Stored before each compressed block to enable proper decompression
     */
    struct BlockHeader
    {
        uint32_t uncompressedSize;  // Original size before compression
        uint32_t compressedSize;    // Size after compression (excluding this header)
        
        BlockHeader() = default;
        BlockHeader(uint32_t uncompressed, uint32_t compressed)
            : uncompressedSize(uncompressed), compressedSize(compressed) {}
    };
    
    static_assert(sizeof(BlockHeader) == 8, "BlockHeader must be 8 bytes");
    
    namespace Detail
    {
        /**
         * Memory buffer state for smallz4 compression callbacks
         */
        struct CompressionBuffer
        {
            const uint8_t* inputData;
            size_t inputSize;
            size_t inputPos;
            std::vector<uint8_t>* outputData;
            
            CompressionBuffer(const uint8_t* input, size_t size, std::vector<uint8_t>* output)
                : inputData(input), inputSize(size), inputPos(0), outputData(output) {}
        };
        
        /**
         * Callback for smallz4 to read from memory buffer
         */
        inline size_t ReadFromMemory(void* data, size_t numBytes, void* userPtr)
        {
            auto* buffer = static_cast<CompressionBuffer*>(userPtr);
            size_t available = buffer->inputSize - buffer->inputPos;
            size_t toRead = std::min(numBytes, available);
            
            if (toRead > 0 && data != nullptr)
            {
                std::memcpy(data, buffer->inputData + buffer->inputPos, toRead);
                buffer->inputPos += toRead;
            }
            
            return toRead;
        }
        
        /**
         * Callback for smallz4 to write to memory buffer
         */
        inline void WriteToMemory(const void* data, size_t numBytes, void* userPtr)
        {
            auto* buffer = static_cast<CompressionBuffer*>(userPtr);
            if (data != nullptr && numBytes > 0)
            {
                const auto* bytes = static_cast<const uint8_t*>(data);
                buffer->outputData->insert(buffer->outputData->end(), bytes, bytes + numBytes);
            }
        }
    }
    
    /**
     * Compress data using LZ4 with configurable compression level
     * @param data Pointer to data to compress
     * @param size Size of data in bytes
     * @param level Compression level (default: Fast)
     * @return Compressed data in LZ4 frame format
     */
    inline std::vector<uint8_t> CompressLZ4(const void* data, size_t size, 
                                           CompressionLevel level = CompressionLevel::Fast)
    {
        if (data == nullptr || size == 0)
        {
            return {};
        }
        
        std::vector<uint8_t> compressed;
        // Reserve some space to avoid reallocations 
        // LZ4 worst case is slightly larger than input, but typically 2-3x smaller
        compressed.reserve(size / 2 + 64);
        
        Detail::CompressionBuffer buffer(
            static_cast<const uint8_t*>(data), 
            size, 
            &compressed
        );
        
        // Convert compression level to chain length
        unsigned short chainLength;
        switch (level)
        {
            case CompressionLevel::Fastest:
                chainLength = 1;
                break;
            case CompressionLevel::Fast:
            default:
                chainLength = smallz4::ShortChainsGreedy;
                break;
            case CompressionLevel::Balanced:
                chainLength = smallz4::ShortChainsLazy;
                break;
        }
        
        smallz4::lz4(
            Detail::ReadFromMemory, 
            Detail::WriteToMemory,
            chainLength,
            false,  // Use modern format (not legacy)
            &buffer
        );
        
        return compressed;
    }
    
    /**
     * Decompress LZ4 frame format data
     * This handles the full LZ4 frame with header and blocks
     * @param compressedData Pointer to compressed data
     * @param compressedSize Size of compressed data
     * @param expectedSize Known decompressed size; caps output growth to defeat
     *        decompression-bomb amplification. Pass SIZE_MAX when unknown.
     * @return Decompressed data or error
     */
    inline Result<std::vector<uint8_t>, SerializationError> DecompressLZ4(
        const void* compressedData,
        size_t compressedSize,
        size_t expectedSize = SIZE_MAX)
    {
        if (compressedData == nullptr || compressedSize == 0)
        {
            return Result<std::vector<uint8_t>, SerializationError>::Err(
                SerializationError::CorruptedData);
        }

        // Use our custom decoder for decompression
        return Detail::LZ4Decoder::DecompressFrame(
            static_cast<const uint8_t*>(compressedData),
            compressedSize,
            expectedSize
        );
    }
    
    /**
     * Compress data and prepend a BlockHeader
     * This is what we'll use in BinaryWriter for archetype compression
     * @param data Pointer to data to compress
     * @param size Size of data in bytes
     * @param level Compression level (default: Fast)
     * @return Buffer containing [BlockHeader][Compressed Data]
     */
    inline std::vector<uint8_t> CompressBlock(const void* data, size_t size,
                                             CompressionLevel level = CompressionLevel::Fast)
    {
        if (data == nullptr || size == 0)
        {
            return {};
        }
        
        // Compress the data
        auto compressed = CompressLZ4(data, size, level);
        
        // Create result with header
        std::vector<uint8_t> result;
        result.reserve(sizeof(BlockHeader) + compressed.size());
        
        // Write header
        BlockHeader header(static_cast<uint32_t>(size), 
                          static_cast<uint32_t>(compressed.size()));
        
        const uint8_t* headerBytes = reinterpret_cast<const uint8_t*>(&header);
        result.insert(result.end(), headerBytes, headerBytes + sizeof(BlockHeader));
        
        // Write compressed data
        result.insert(result.end(), compressed.begin(), compressed.end());
        
        return result;
    }
    
    /**
     * Decompress data with BlockHeader
     * This is what we'll use in BinaryReader for archetype decompression
     * @param blockData Pointer to [BlockHeader][Compressed Data]
     * @param blockSize Total size including header
     * @return Decompressed data or error
     */
    inline Result<std::vector<uint8_t>, SerializationError> DecompressBlock(
        const void* blockData,
        size_t blockSize)
    {
        if (blockData == nullptr || blockSize < sizeof(BlockHeader))
        {
            return Result<std::vector<uint8_t>, SerializationError>::Err(
                SerializationError::CorruptedData);
        }
        
        // Read header
        const BlockHeader* header = static_cast<const BlockHeader*>(blockData);
        
        // Validate sizes
        if (blockSize != sizeof(BlockHeader) + header->compressedSize)
        {
            return Result<std::vector<uint8_t>, SerializationError>::Err(
                SerializationError::CorruptedData);
        }
        
        // Decompress. The header records the exact original size, so thread it
        // through as the output cap -- a corrupt/crafted block cannot expand
        // past what it claimed to decompress to (~255x amplification defense).
        const uint8_t* compressedData = static_cast<const uint8_t*>(blockData) + sizeof(BlockHeader);
        return DecompressLZ4(compressedData, header->compressedSize, header->uncompressedSize);
    }
    
    /**
     * Check if data might be LZ4 compressed
     * Checks for LZ4 frame magic numbers
     */
    inline bool IsLZ4Compressed(const void* data, size_t size)
    {
        if (data == nullptr || size < 4)
            return false;
            
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        
        // Check for both legacy and modern magic numbers
        bool isLegacy = (bytes[0] == 0x02 && bytes[1] == 0x21 && 
                         bytes[2] == 0x4C && bytes[3] == 0x18);
        bool isModern = (bytes[0] == 0x04 && bytes[1] == 0x22 && 
                         bytes[2] == 0x4D && bytes[3] == 0x18);
        
        return isLegacy || isModern;
    }
    
    /**
     * Calculate compression ratio
     * @return Ratio of original size to compressed size (e.g., 2.0 = 50% reduction)
     */
    inline float CompressionRatio(size_t originalSize, size_t compressedSize)
    {
        if (compressedSize == 0)
            return 0.0f;
        return static_cast<float>(originalSize) / static_cast<float>(compressedSize);
    }
}