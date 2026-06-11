#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <span>

#include "../../Core/Result.hpp"
#include "../SerializationError.hpp"

namespace Astra::Compression::Detail
{
    /**
     * Minimal LZ4 decompressor implementation
     * Compatible with LZ4 block format as produced by smallz4
     * 
     * LZ4 Format Overview:
     * - Sequence of tokens followed by literals and/or matches
     * - Token byte: [LLLL|MMMM] where L=literal length-1, M=match length-4
     * - If LLLL==15, additional bytes follow for literal length
     * - If MMMM==15, additional bytes follow for match length
     * - Literals are copied verbatim
     * - Matches copy from earlier in the output (offset by 1-65535 bytes)
     */
    class LZ4Decoder
    {
    public:
        /**
         * Decompress LZ4 block format data
         * @param compressed Compressed data
         * @param compressedSize Size of compressed data
         * @param uncompressedSize Expected output size (must be exact)
         * @return Decompressed data or error
         */
        static Result<std::vector<uint8_t>, SerializationError> Decompress(
            const uint8_t* compressed,
            size_t compressedSize,
            size_t uncompressedSize)
        {
            if (!compressed || compressedSize == 0)
                return Err(SerializationError::CorruptedData);

            std::vector<uint8_t> output;
            output.reserve(uncompressedSize);

            const uint8_t* src = compressed;
            const uint8_t* srcEnd = compressed + compressedSize;

            while (src < srcEnd)
            {
                // Read token
                if (src >= srcEnd)
                    return Err(SerializationError::CorruptedData);
                    
                uint8_t token = *src++;
                
                // Extract literal and match lengths
                size_t literalLength = (token >> 4) & 0x0F;
                size_t matchLength = (token & 0x0F);

                // Handle extended literal length
                if (literalLength == 15)
                {
                    uint8_t addLen;
                    do {
                        if (src >= srcEnd)
                            return Err(SerializationError::CorruptedData);
                        addLen = *src++;
                        literalLength += addLen;
                    } while (addLen == 255);
                }

                // Copy literals
                if (literalLength > 0)
                {
                    if (src + literalLength > srcEnd)
                        return Err(SerializationError::CorruptedData);
                        
                    // Safety check: don't exceed expected size
                    if (output.size() + literalLength > uncompressedSize)
                        return Err(SerializationError::CorruptedData);
                        
                    output.insert(output.end(), src, src + literalLength);
                    src += literalLength;
                }

                // Check if we're done (last sequence has no match)
                if (src >= srcEnd)
                    break;

                // Read match offset (little-endian 16-bit)
                if (src + 2 > srcEnd)
                    return Err(SerializationError::CorruptedData);
                    
                uint16_t offset = src[0] | (uint16_t(src[1]) << 8);
                src += 2;

                if (offset == 0 || offset > output.size())
                    return Err(SerializationError::CorruptedData);

                // Add minimum match length
                matchLength += 4;

                // Handle extended match length
                if (matchLength == 19) // 15 + 4
                {
                    uint8_t addLen;
                    do {
                        if (src >= srcEnd)
                            return Err(SerializationError::CorruptedData);
                        addLen = *src++;
                        matchLength += addLen;
                    } while (addLen == 255);
                }

                // Copy match from back-reference
                if (output.size() + matchLength > uncompressedSize)
                    return Err(SerializationError::CorruptedData);

                // Copy match bytes (may overlap for RLE)
                size_t matchPos = output.size() - offset;
                for (size_t i = 0; i < matchLength; ++i)
                {
                    output.push_back(output[matchPos + i]);
                }
            }

            // Verify we got exactly the expected size
            if (output.size() != uncompressedSize)
                return Err(SerializationError::CorruptedData);

            return Ok(std::move(output));
        }

        /**
         * Decompress LZ4 frame format (with 4-byte header)
         * @param compressed Compressed data including frame header
         * @param compressedSize Total size including header
         * @return Decompressed data or error
         */
        static Result<std::vector<uint8_t>, SerializationError> DecompressFrame(
            const uint8_t* compressed,
            size_t compressedSize)
        {
            if (!compressed || compressedSize < 8) // Min: 4-byte header + 4-byte size
                return Err(SerializationError::CorruptedData);

            const uint8_t* src = compressed;

            // Check magic number (legacy or modern)
            if (src[0] == 0x02 && src[1] == 0x21 && src[2] == 0x4C && src[3] == 0x18)
            {
                // Legacy format - magic only
                src += 4;
            }
            else if (src[0] == 0x04 && src[1] == 0x22 && src[2] == 0x4D && src[3] == 0x18)
            {
                // Modern format - skip frame descriptor
                if (compressedSize < 11) // Magic(4) + FLG(1) + BD(1) + HC(1) + BlockSize(4)
                    return Err(SerializationError::InvalidMagic);
                src += 7; // Skip magic + flags + block size + header checksum
            }
            else
            {
                return Err(SerializationError::InvalidMagic);
            }

            std::vector<uint8_t> output;
            const uint8_t* srcEnd = compressed + compressedSize;

            // Process blocks
            while (src + 4 <= srcEnd)
            {
                // Read block size (little-endian)
                uint32_t blockSize = src[0] | (uint32_t(src[1]) << 8) | 
                                    (uint32_t(src[2]) << 16) | (uint32_t(src[3]) << 24);
                src += 4;

                // End marker (size = 0)
                if (blockSize == 0)
                    break;

                // Check if block is uncompressed (high bit set)
                bool isCompressed = (blockSize & 0x80000000) == 0;
                blockSize &= 0x7FFFFFFF;

                if (src + blockSize > srcEnd)
                    return Err(SerializationError::CorruptedData);

                if (isCompressed)
                {
                    // For compressed blocks, we need to know uncompressed size
                    // This is tricky - smallz4 doesn't store it per block
                    // We'll have to decompress and see
                    
                    // Try decompressing with a reasonable max size (e.g., 4MB)
                    const size_t maxBlockSize = 4 * 1024 * 1024;
                    auto result = Decompress(src, blockSize, maxBlockSize);
                    
                    // Since we don't know exact size, we need a different approach
                    // Let's decompress to a temporary buffer and stop when input consumed
                    auto blockResult = DecompressBlock(src, blockSize);
                    if (blockResult.IsErr())
                        return blockResult;
                        
                    auto& blockData = *blockResult.GetValue();
                    output.insert(output.end(), blockData.begin(), blockData.end());
                }
                else
                {
                    // Uncompressed block - copy directly
                    output.insert(output.end(), src, src + blockSize);
                }

                src += blockSize;
            }

            return Ok(std::move(output));
        }

    private:
        /**
         * Decompress a single block without knowing output size
         * Stops when input is consumed
         */
        static Result<std::vector<uint8_t>, SerializationError> DecompressBlock(
            const uint8_t* compressed,
            size_t compressedSize)
        {
            std::vector<uint8_t> output;
            output.reserve(compressedSize * 3); // Assume ~3x expansion initially

            const uint8_t* src = compressed;
            const uint8_t* srcEnd = compressed + compressedSize;

            while (src < srcEnd)
            {
                // Read token
                if (src >= srcEnd)
                    break; // Reached end normally
                    
                uint8_t token = *src++;
                
                // Extract literal and match lengths
                size_t literalLength = (token >> 4) & 0x0F;
                size_t matchLength = (token & 0x0F);

                // Handle extended literal length
                if (literalLength == 15)
                {
                    uint8_t addLen;
                    do {
                        if (src >= srcEnd)
                            return Err(SerializationError::CorruptedData);
                        addLen = *src++;
                        literalLength += addLen;
                    } while (addLen == 255);
                }

                // Copy literals
                if (literalLength > 0)
                {
                    if (src + literalLength > srcEnd)
                        return Err(SerializationError::CorruptedData);
                        
                    output.insert(output.end(), src, src + literalLength);
                    src += literalLength;
                }

                // Check if we're done
                if (src >= srcEnd)
                    break;

                // Read match offset (little-endian 16-bit)
                if (src + 2 > srcEnd)
                    return Err(SerializationError::CorruptedData);
                    
                uint16_t offset = src[0] | (uint16_t(src[1]) << 8);
                src += 2;

                if (offset == 0 || offset > output.size())
                    return Err(SerializationError::CorruptedData);

                // Add minimum match length
                matchLength += 4;

                // Handle extended match length
                if (matchLength == 19) // 15 + 4
                {
                    uint8_t addLen;
                    do {
                        if (src >= srcEnd)
                            return Err(SerializationError::CorruptedData);
                        addLen = *src++;
                        matchLength += addLen;
                    } while (addLen == 255);
                }

                // Copy match from back-reference
                size_t matchPos = output.size() - offset;
                for (size_t i = 0; i < matchLength; ++i)
                {
                    output.push_back(output[matchPos + i]);
                }
            }

            return Ok(std::move(output));
        }

        // Helper for Result returns
        template<typename T>
        static Result<T, SerializationError> Ok(T&& value)
        {
            return Result<T, SerializationError>::Ok(std::forward<T>(value));
        }

        static Result<std::vector<uint8_t>, SerializationError> Err(SerializationError error)
        {
            return Result<std::vector<uint8_t>, SerializationError>::Err(error);
        }
    };
}