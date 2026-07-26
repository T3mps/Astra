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
         *
         * The frame is decoded into a SINGLE shared output buffer that spans all
         * blocks. smallz4 emits Block-Independence=0 frames: a match in one block
         * may back-reference into the decompressed tail of the previous block
         * (up to 64KB, the 16-bit offset limit). Decoding each 4MB block into its
         * own buffer would make those cross-block matches unresolvable, so any
         * payload >4MB (multi-block) would fail. Keeping one running buffer lets
         * every match window reach back across block boundaries (fixes C3).
         *
         * @param compressed Compressed data including frame header
         * @param compressedSize Total size including header
         * @param expectedSize Known decompressed size; caps total output growth to
         *        defeat decompression-bomb amplification (I11). Pass SIZE_MAX when
         *        the size is unknown (leaves output uncapped).
         * @return Decompressed data or error
         */
        static Result<std::vector<uint8_t>, SerializationError> DecompressFrame(
            const uint8_t* compressed,
            size_t compressedSize,
            size_t expectedSize = SIZE_MAX)
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
            if (expectedSize != SIZE_MAX)
            {
                // I11: expectedSize comes from an on-disk uint32 (untrusted). Reserving it
                // outright lets a ~30-byte crafted block claim ~4GB and pre-commit it -- a
                // memory bomb (and a bad_alloc under -fno-exceptions is std::terminate). A
                // valid LZ4 frame cannot expand more than ~255x its compressed bytes, so
                // clamp the reservation to that bound: for a legitimate stream this equals
                // expectedSize (no perf loss); a bomb is capped to the real input size. The
                // per-loop expectedSize cap below remains the exactness/correctness guard.
                output.reserve(std::min<size_t>(expectedSize, static_cast<size_t>(compressedSize) * 256));
            }
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
                    // Decode straight into the shared buffer so cross-block matches
                    // resolve against previously-decoded blocks (C3), capped (I11).
                    if (!DecompressBlockInto(src, blockSize, output, expectedSize))
                        return Err(SerializationError::CorruptedData);
                }
                else
                {
                    // Uncompressed block - copy directly (still capped, I11)
                    if (output.size() + blockSize > expectedSize)
                        return Err(SerializationError::CorruptedData);
                    output.insert(output.end(), src, src + blockSize);
                }

                src += blockSize;
            }

            return Ok(std::move(output));
        }

    private:
        /**
         * Decode a single LZ4 block, APPENDING into the running frame buffer.
         *
         * Matches resolve against the whole of `output` (which already holds every
         * previously-decoded block), so back-references that reach into the prior
         * block's tail work correctly. Total output growth is capped at
         * `expectedSize` inside the copy/match loop, so a crafted block cannot
         * expand unboundedly (~255x amplification) before a check fires.
         *
         * @return true on success, false if the block is corrupt or would overflow.
         */
        static bool DecompressBlockInto(
            const uint8_t* compressed,
            size_t compressedSize,
            std::vector<uint8_t>& output,
            size_t expectedSize)
        {
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
                            return false;
                        addLen = *src++;
                        literalLength += addLen;
                    } while (addLen == 255);
                }

                // Copy literals
                if (literalLength > 0)
                {
                    if (src + literalLength > srcEnd)
                        return false;

                    // Cap total output growth (I11)
                    if (output.size() + literalLength > expectedSize)
                        return false;

                    output.insert(output.end(), src, src + literalLength);
                    src += literalLength;
                }

                // Check if we're done
                if (src >= srcEnd)
                    break;

                // Read match offset (little-endian 16-bit)
                if (src + 2 > srcEnd)
                    return false;

                uint16_t offset = src[0] | (uint16_t(src[1]) << 8);
                src += 2;

                // Match window spans previously-decoded blocks: compare against the
                // full running buffer, not just this block (C3).
                if (offset == 0 || offset > output.size())
                    return false;

                // Add minimum match length
                matchLength += 4;

                // Handle extended match length
                if (matchLength == 19) // 15 + 4
                {
                    uint8_t addLen;
                    do {
                        if (src >= srcEnd)
                            return false;
                        addLen = *src++;
                        matchLength += addLen;
                    } while (addLen == 255);
                }

                // Cap total output growth (I11)
                if (output.size() + matchLength > expectedSize)
                    return false;

                // Copy match from back-reference (may overlap for RLE). Read each byte into
                // a local BEFORE push_back: binding a reference into `output` across a
                // reallocating push_back is UB (the reference is invalidated). matchPos + i
                // is recomputed each iteration, so overlapping/RLE matches decode correctly.
                size_t matchPos = output.size() - offset;
                for (size_t i = 0; i < matchLength; ++i)
                {
                    const uint8_t b = output[matchPos + i];
                    output.push_back(b);
                }
            }

            return true;
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