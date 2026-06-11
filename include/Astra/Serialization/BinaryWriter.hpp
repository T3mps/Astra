#pragma once

#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <optional>
#include <utility>
#include <tuple>

#include "BinaryArchive.hpp"
#include "Compression/Compression.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"

namespace Astra
{
    /**
     * Binary writer for serialization
     * Supports both file and memory targets with buffering
     */
    class BinaryWriter : public BinaryArchive
    {
    public:
        /**
         * Configuration for compression
         */
        struct Config
        {
            CompressionMode compressionMode = CompressionMode::None;
            Compression::CompressionLevel compressionLevel = Compression::CompressionLevel::Fast;
            size_t compressionThreshold = 1024; // Only compress blocks larger than this
        };
        
        /**
         * Create a writer for file output
         */
        explicit BinaryWriter(const std::filesystem::path& path, size_t bufferSize = 65536)
            : m_buffer(bufferSize)
            , m_bufferPos(0)
            , m_totalBytesWritten(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerPosition(0)
            , m_compressionMode(CompressionMode::None)
            , m_compressionLevel(Compression::CompressionLevel::Fast)
            , m_compressionThreshold(1024)
        {
            m_file.open(path, std::ios::binary | std::ios::out);
            if (!m_file.is_open())
            {
                m_error = SerializationError::IOError;
            }
        }
        
        /**
         * Create a writer for file output with config
         */
        BinaryWriter(const std::filesystem::path& path, const Config& config, size_t bufferSize = 65536)
            : m_buffer(bufferSize)
            , m_bufferPos(0)
            , m_totalBytesWritten(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerPosition(0)
            , m_compressionMode(config.compressionMode)
            , m_compressionLevel(config.compressionLevel)
            , m_compressionThreshold(config.compressionThreshold)
        {
            m_file.open(path, std::ios::binary | std::ios::out);
            if (!m_file.is_open())
            {
                m_error = SerializationError::IOError;
            }
        }
        
        /**
         * Create a writer for memory output
         */
        explicit BinaryWriter(std::vector<std::byte>& output, size_t reserveSize = 65536)
            : m_output(&output)
            , m_bufferPos(0)
            , m_totalBytesWritten(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerPosition(0)
            , m_compressionMode(CompressionMode::None)
            , m_compressionLevel(Compression::CompressionLevel::Fast)
            , m_compressionThreshold(1024)
        {
            m_output->reserve(reserveSize);
        }
        
        /**
         * Create a writer for memory output with config
         */
        BinaryWriter(std::vector<std::byte>& output, const Config& config, size_t reserveSize = 65536)
            : m_output(&output)
            , m_bufferPos(0)
            , m_totalBytesWritten(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerPosition(0)
            , m_compressionMode(config.compressionMode)
            , m_compressionLevel(config.compressionLevel)
            , m_compressionThreshold(config.compressionThreshold)
        {
            m_output->reserve(reserveSize);
        }
        
        ~BinaryWriter()
        {
            Flush();
            if (m_file.is_open())
            {
                m_file.close();
            }
        }
        
        [[nodiscard]] bool IsLoading() const noexcept override { return false; }
        
        /**
         * Write raw bytes
         */
        void WriteBytes(const void* data, size_t size)
        {
            if (m_error != SerializationError::None) return;
            
            const std::byte* bytes = static_cast<const std::byte*>(data);
            
            // Track total bytes written first for proper checksum calculation
            size_t startPosition = m_totalBytesWritten;
            m_totalBytesWritten += size;
            
            // Update checksum if enabled and we're past the header
            if (m_checksumEnabled && startPosition >= sizeof(BinaryHeader))
            {
                m_runningChecksum = Checksum::CRC32(data, size, m_runningChecksum);
            }
            
            if (m_file.is_open())
            {
                // File mode with buffering
                size_t bytesRemaining = size;
                const std::byte* currentBytes = bytes;
                while (bytesRemaining > 0)
                {
                    size_t available = m_buffer.size() - m_bufferPos;
                    size_t toWrite = std::min(bytesRemaining, available);
                    
                    std::memcpy(m_buffer.data() + m_bufferPos, currentBytes, toWrite);
                    m_bufferPos += toWrite;
                    currentBytes += toWrite;
                    bytesRemaining -= toWrite;
                    
                    if (m_bufferPos == m_buffer.size())
                    {
                        Flush();
                    }
                }
            }
            else if (m_output)
            {
                // Memory mode - direct write
                m_output->insert(m_output->end(), bytes, bytes + size);
            }
        }
        
        /**
         * Write the binary header (will be finalized with checksum later)
         */
        void WriteHeader(BinaryHeader& header)
        {
            m_headerPosition = m_totalBytesWritten;
            m_header = header;
            // Set compression mode in header
            m_header.compressionMode = static_cast<uint8_t>(m_compressionMode);
            WriteBytes(&m_header, sizeof(BinaryHeader));
            // Reset checksum calculation to start after header
            m_runningChecksum = 0;
        }
        
        /**
         * Finalize the header with the calculated checksum
         * Call this after all data has been written
         */
        void FinalizeHeader()
        {
            if (m_error != SerializationError::None) return;
            
            m_header.dataChecksum = m_runningChecksum;

            if (m_file.is_open())
            {
                // Flush any buffered data first
                Flush();
                
                // Seek back to header position
                m_file.seekp(m_headerPosition, std::ios::beg);
                m_file.write(reinterpret_cast<const char*>(&m_header), sizeof(BinaryHeader));
                
                // Seek back to end
                m_file.seekp(0, std::ios::end);
            }
            else if (m_output)
            {
                // Update header in memory
                std::memcpy(m_output->data() + m_headerPosition, &m_header, sizeof(BinaryHeader));
            }
        }
        
        /**
         * Write a potentially compressed block of data
         * If compression is enabled and data size exceeds threshold, it will be compressed
         * Otherwise, it will be written uncompressed
         */
        void WriteCompressedBlock(const void* data, size_t size)
        {
            if (m_error != SerializationError::None) return;
            
            if (m_compressionMode == CompressionMode::LZ4 && size >= m_compressionThreshold)
            {
                // Compress the data
                auto compressed = Compression::CompressBlock(data, size, m_compressionLevel);
                
                // Check if compression actually helped (with some margin for metadata)
                if (!compressed.empty() && compressed.size() < size * 0.9)
                {
                    // Write compressed size marker followed by compressed data
                    uint32_t compressedSize = static_cast<uint32_t>(compressed.size());
                    uint32_t originalSize = static_cast<uint32_t>(size);
                    
                    // Write sizes
                    (*this)(originalSize);
                    (*this)(compressedSize);
                    
                    // Write compressed data
                    WriteBytes(compressed.data(), compressed.size());
                    return;
                }
            }
            
            // Write uncompressed (either compression disabled or didn't help)
            uint32_t originalSize = static_cast<uint32_t>(size);
            uint32_t compressedSize = 0; // 0 indicates uncompressed
            
            (*this)(originalSize);
            (*this)(compressedSize);
            WriteBytes(data, size);
        }
        
        /**
         * Serialize POD types
         */
        template<typename T>
        requires std::is_trivially_copyable_v<T>
        BinaryWriter& operator()(const T& value)
        {
            WriteBytes(&value, sizeof(T));
            return *this;
        }
        
        /**
         * Serialize types with custom Serialize method
         */
        template<typename T>
        requires HasSerializeMethod<T, BinaryWriter>
        BinaryWriter& operator()(T& value)
        {
            value.Serialize(*this);
            return *this;
        }
        
        /**
         * Serialize strings
         */
        BinaryWriter& operator()(const std::string& str)
        {
            size_t len = str.size();
            (*this)(len);
            WriteBytes(str.data(), len);
            return *this;
        }
        
        /**
         * Serialize vectors
         */
        template<typename T>
        BinaryWriter& operator()(const std::vector<T>& vec)
        {
            size_t size = vec.size();
            (*this)(size);
            
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                // Write all at once for POD types
                WriteBytes(vec.data(), size * sizeof(T));
            }
            else
            {
                // Write one by one for complex types
                for (const auto& item : vec)
                {
                    (*this)(item);
                }
            }
            return *this;
        }
        
        /**
         * Serialize std::array
         */
        template<typename T, size_t N>
        BinaryWriter& operator()(const std::array<T, N>& arr)
        {
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                // Write all at once for POD types
                WriteBytes(arr.data(), N * sizeof(T));
            }
            else
            {
                // Write one by one for complex types
                for (const auto& item : arr)
                {
                    (*this)(item);
                }
            }
            return *this;
        }
        
        /**
         * Serialize std::pair
         */
        template<typename T1, typename T2>
        BinaryWriter& operator()(const std::pair<T1, T2>& p)
        {
            (*this)(p.first)(p.second);
            return *this;
        }
        
        /**
         * Serialize std::tuple
         */
        template<typename... Args>
        BinaryWriter& operator()(const std::tuple<Args...>& t)
        {
            std::apply([this](const auto&... args) {
                ((*this)(args), ...);
            }, t);
            return *this;
        }
        
        /**
         * Serialize std::optional
         */
        template<typename T>
        BinaryWriter& operator()(const std::optional<T>& opt)
        {
            bool hasValue = opt.has_value();
            (*this)(hasValue);
            if (hasValue)
            {
                (*this)(*opt);
            }
            return *this;
        }
        
        /**
         * Serialize std::map
         */
        template<typename K, typename V, typename Compare, typename Allocator>
        BinaryWriter& operator()(const std::map<K, V, Compare, Allocator>& map)
        {
            size_t size = map.size();
            (*this)(size);
            for (const auto& [key, value] : map)
            {
                (*this)(key)(value);
            }
            return *this;
        }
        
        /**
         * Serialize std::unordered_map
         */
        template<typename K, typename V, typename Hash, typename Equal, typename Allocator>
        BinaryWriter& operator()(const std::unordered_map<K, V, Hash, Equal, Allocator>& map)
        {
            size_t size = map.size();
            (*this)(size);
            for (const auto& [key, value] : map)
            {
                (*this)(key)(value);
            }
            return *this;
        }
        
        /**
         * Serialize std::set
         */
        template<typename T, typename Compare, typename Allocator>
        BinaryWriter& operator()(const std::set<T, Compare, Allocator>& set)
        {
            size_t size = set.size();
            (*this)(size);
            for (const auto& item : set)
            {
                (*this)(item);
            }
            return *this;
        }
        
        /**
         * Serialize std::unordered_set
         */
        template<typename T, typename Hash, typename Equal, typename Allocator>
        BinaryWriter& operator()(const std::unordered_set<T, Hash, Equal, Allocator>& set)
        {
            size_t size = set.size();
            (*this)(size);
            for (const auto& item : set)
            {
                (*this)(item);
            }
            return *this;
        }
        
        /**
         * Write a component hash
         */
        void WriteComponentHash(uint64_t hash)
        {
            (*this)(hash);
        }
        
        /**
         * Write a versioned component with SerializationTraits
         * This writes the component version, then the component data
         */
        template<typename T>
        void WriteVersionedComponent(T& component)
        {
            // Write component hash for identification
            uint64_t hash = TypeID<T>::Hash();
            WriteComponentHash(hash);
            
            // Write component version
            uint32_t version = SerializationTraits<T>::Version;
            (*this)(version);
            
            // Write component data using traits
            if constexpr (SerializationTraits<T>::HasCustomSerializer)
            {
                // Use custom serialization from traits
                if constexpr (requires { SerializationTraits<T>::Serialize(*this, component); })
                {
                    SerializationTraits<T>::Serialize(*this, component);
                }
                else
                {
                    static_assert(sizeof(T) == 0, "SerializationTraits must provide Serialize(BinaryWriter&, T&)");
                }
            }
            else if constexpr (HasSerializeMethod<T, BinaryWriter>)
            {
                // Use Serialize method
                component.Serialize(*this);
            }
            else if constexpr (std::is_trivially_copyable_v<T>)
            {
                // POD type
                (*this)(component);
            }
            else
            {
                static_assert(sizeof(T) == 0, "Type cannot be serialized");
            }
        }
        
        /**
         * Write alignment padding
         */
        void WritePadding(size_t alignment)
        {
            size_t currentPos = m_totalBytesWritten;
            size_t padding = (alignment - (currentPos % alignment)) % alignment;
            
            if (padding > 0)
            {
                static const std::byte zeros[16] = {std::byte(0)};
                while (padding > 0)
                {
                    size_t toWrite = std::min(padding, sizeof(zeros));
                    WriteBytes(zeros, toWrite);
                    padding -= toWrite;
                }
            }
        }
        
        /**
         * Flush buffer to file
         */
        void Flush()
        {
            if (m_file.is_open() && m_bufferPos > 0)
            {
                m_file.write(reinterpret_cast<const char*>(m_buffer.data()), m_bufferPos);
                if (!m_file.good())
                {
                    m_error = SerializationError::IOError;
                }
                m_bufferPos = 0;
            }
        }
        
        /**
         * Get total bytes written
         */
        [[nodiscard]] size_t GetBytesWritten() const noexcept { return m_totalBytesWritten; }
        
        /**
         * Check for errors
         */
        [[nodiscard]] bool HasError() const noexcept { return m_error != SerializationError::None; }
        [[nodiscard]] SerializationError GetError() const noexcept { return m_error; }
        
        /**
         * Enable/disable checksum calculation
         */
        void SetChecksumEnabled(bool enabled) { m_checksumEnabled = enabled; }
        [[nodiscard]] bool IsChecksumEnabled() const noexcept { return m_checksumEnabled; }
        [[nodiscard]] uint32_t GetChecksum() const noexcept { return m_runningChecksum; }
        
    private:
        std::ofstream m_file;
        std::vector<std::byte>* m_output = nullptr;
        std::vector<std::byte> m_buffer;
        size_t m_bufferPos;
        size_t m_totalBytesWritten;
        SerializationError m_error = SerializationError::None;
        
        // Checksum support
        bool m_checksumEnabled;
        uint32_t m_runningChecksum;
        size_t m_headerPosition;
        BinaryHeader m_header;
        
        // Compression settings
        CompressionMode m_compressionMode;
        Compression::CompressionLevel m_compressionLevel;
        size_t m_compressionThreshold;
    };
}