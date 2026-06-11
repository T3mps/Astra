#pragma once

#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <span>
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
     * Binary reader for deserialization
     * Supports both file and memory sources with validation
     */
    class BinaryReader : public BinaryArchive
    {
    public:
        /**
         * Create a reader for file input
         */
        explicit BinaryReader(const std::filesystem::path& path)
            : m_position(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerSize(0)
        {
            m_file.open(path, std::ios::binary | std::ios::in | std::ios::ate);
            if (!m_file.is_open())
            {
                m_error = SerializationError::IOError;
                return;
            }
            
            // Get file size
            m_size = static_cast<size_t>(m_file.tellg());
            m_file.seekg(0, std::ios::beg);
            
            // For small files, read everything into memory
            if (m_size < 10 * 1024 * 1024) // 10MB threshold
            {
                m_buffer.resize(m_size);
                m_file.read(reinterpret_cast<char*>(m_buffer.data()), m_size);
                m_file.close();
                m_data = std::span<const std::byte>(m_buffer);
            }
        }
        
        /**
         * Create a reader for memory input
         */
        explicit BinaryReader(std::span<const std::byte> data)
            : m_data(data)
            , m_size(data.size())
            , m_position(0)
            , m_checksumEnabled(true)
            , m_runningChecksum(0)
            , m_headerSize(0)
        {
        }
        
        /**
         * Create a reader from vector
         */
        explicit BinaryReader(const std::vector<std::byte>& data)
            : BinaryReader(std::span<const std::byte>(data))
        {
        }
        
        ~BinaryReader()
        {
            if (m_file.is_open())
            {
                m_file.close();
            }
        }
        
        [[nodiscard]] bool IsLoading() const noexcept override { return true; }
        
        /**
         * Read raw bytes
         */
        void ReadBytes(void* data, size_t size)
        {
            if (m_error != SerializationError::None) return;
            
            if (m_position + size > m_size)
            {
                m_error = SerializationError::CorruptedData;
                return;
            }
            
            std::byte* bytes = static_cast<std::byte*>(data);
            
            if (!m_data.empty())
            {
                // Memory mode
                std::memcpy(bytes, m_data.data() + m_position, size);
            }
            else if (m_file.is_open())
            {
                // File mode
                m_file.read(reinterpret_cast<char*>(bytes), size);
                if (!m_file.good())
                {
                    m_error = SerializationError::IOError;
                    return;
                }
            }
            else
            {
                m_error = SerializationError::IOError;
                return;
            }
            
            // Update checksum if enabled and we're past the header
            if (m_checksumEnabled && m_position >= m_headerSize && m_headerSize > 0)
            {
                m_runningChecksum = Checksum::CRC32(data, size, m_runningChecksum);
            }
            
            m_position += size;
        }
        
        /**
         * Read and validate the binary header
         */
        Result<BinaryHeader, SerializationError> ReadHeader()
        {
            BinaryHeader header;
            ReadBytes(&header, sizeof(BinaryHeader));
            
            if (HasError())
            {
                return Result<BinaryHeader, SerializationError>::Err(m_error);
            }
            
            if (!header.IsValid())
            {
                m_error = SerializationError::InvalidMagic;
                return Result<BinaryHeader, SerializationError>::Err(SerializationError::InvalidMagic);
            }
            
            if (!header.IsVersionSupported())
            {
                m_error = SerializationError::UnsupportedVersion;
                return Result<BinaryHeader, SerializationError>::Err(SerializationError::UnsupportedVersion);
            }
            
            if (!header.IsEndianCompatible())
            {
                m_error = SerializationError::EndiannessMismatch;
                return Result<BinaryHeader, SerializationError>::Err(SerializationError::EndiannessMismatch);
            }
            
            m_version = header.version;
            m_headerSize = sizeof(BinaryHeader);
            m_expectedChecksum = header.dataChecksum;
            m_runningChecksum = 0;  // Reset checksum after reading header
            m_compressionMode = header.GetCompressionMode();
            
            return Result<BinaryHeader, SerializationError>::Ok(std::move(header));
        }
        
        /**
         * Read a potentially compressed block of data
         * Automatically handles decompression if needed
         */
        Result<std::vector<uint8_t>, SerializationError> ReadCompressedBlock()
        {
            if (m_error != SerializationError::None)
            {
                return Result<std::vector<uint8_t>, SerializationError>::Err(m_error);
            }
            
            // Read sizes
            uint32_t originalSize = 0;
            uint32_t compressedSize = 0;
            (*this)(originalSize);
            (*this)(compressedSize);
            
            if (HasError())
            {
                return Result<std::vector<uint8_t>, SerializationError>::Err(m_error);
            }
            
            if (compressedSize == 0)
            {
                // Data is uncompressed
                std::vector<uint8_t> data(originalSize);
                ReadBytes(data.data(), originalSize);
                
                if (HasError())
                {
                    return Result<std::vector<uint8_t>, SerializationError>::Err(m_error);
                }
                
                return Result<std::vector<uint8_t>, SerializationError>::Ok(std::move(data));
            }
            else
            {
                // Data is compressed
                std::vector<uint8_t> compressedData(compressedSize);
                ReadBytes(compressedData.data(), compressedSize);
                
                if (HasError())
                {
                    return Result<std::vector<uint8_t>, SerializationError>::Err(m_error);
                }
                
                // Decompress
                auto result = Compression::DecompressBlock(compressedData.data(), compressedSize);
                if (result.IsErr())
                {
                    m_error = SerializationError::CorruptedData;
                    return Result<std::vector<uint8_t>, SerializationError>::Err(SerializationError::CorruptedData);
                }
                
                auto& decompressed = *result.GetValue();
                
                // Verify size matches
                if (decompressed.size() != originalSize)
                {
                    m_error = SerializationError::SizeMismatch;
                    return Result<std::vector<uint8_t>, SerializationError>::Err(SerializationError::SizeMismatch);
                }
                
                return Result<std::vector<uint8_t>, SerializationError>::Ok(std::move(decompressed));
            }
        }
        
        /**
         * Deserialize POD types
         */
        template<typename T>
        requires std::is_trivially_copyable_v<T>
        BinaryReader& operator()(T& value)
        {
            ReadBytes(&value, sizeof(T));
            return *this;
        }
        
        /**
         * Deserialize types with custom Serialize method
         */
        template<typename T>
        requires HasSerializeMethod<T, BinaryReader>
        BinaryReader& operator()(T& value)
        {
            value.Serialize(*this);
            return *this;
        }
        
        
        /**
         * Deserialize strings
         */
        BinaryReader& operator()(std::string& str)
        {
            size_t len;
            (*this)(len);
            
            if (len > m_size - m_position)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }
            
            str.resize(len);
            ReadBytes(str.data(), len);
            return *this;
        }
        
        /**
         * Deserialize vectors
         */
        template<typename T>
        BinaryReader& operator()(std::vector<T>& vec)
        {
            size_t size;
            (*this)(size);
            
            // Sanity check to prevent huge allocations
            // For POD types, we can check the exact size needed
            // For non-POD types, just check that we have some data left
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                if (size * sizeof(T) > (m_size - m_position))
                {
                    m_error = SerializationError::CorruptedData;
                    return *this;
                }
            }
            else
            {
                // For non-POD types, just check for reasonable size
                // We can't know the actual serialized size without reading
                const size_t maxReasonableSize = 1000000; // 1 million elements max
                if (size > maxReasonableSize)
                {
                    m_error = SerializationError::CorruptedData;
                    return *this;
                }
            }
            
            vec.clear();
            vec.reserve(size);
            
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                // Read all at once for POD types
                vec.resize(size);
                ReadBytes(vec.data(), size * sizeof(T));
            }
            else
            {
                // Read one by one for complex types
                for (size_t i = 0; i < size; ++i)
                {
                    T item;
                    (*this)(item);
                    if (HasError()) break; // Stop on error
                    vec.push_back(std::move(item));
                }
            }
            return *this;
        }
        
        /**
         * Deserialize std::array
         */
        template<typename T, size_t N>
        BinaryReader& operator()(std::array<T, N>& arr)
        {
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                // Read all at once for POD types
                ReadBytes(arr.data(), N * sizeof(T));
            }
            else
            {
                // Read one by one for complex types
                for (auto& item : arr)
                {
                    (*this)(item);
                    if (HasError()) break;
                }
            }
            return *this;
        }
        
        /**
         * Deserialize std::pair
         */
        template<typename T1, typename T2>
        BinaryReader& operator()(std::pair<T1, T2>& p)
        {
            (*this)(p.first)(p.second);
            return *this;
        }
        
        /**
         * Helper for deserializing tuples
         */
        template<typename Tuple, size_t... Is>
        void DeserializeTupleImpl(Tuple& t, std::index_sequence<Is...>)
        {
            ((*this)(std::get<Is>(t)), ...);
        }
        
        /**
         * Deserialize std::tuple
         */
        template<typename... Args>
        BinaryReader& operator()(std::tuple<Args...>& t)
        {
            DeserializeTupleImpl(t, std::index_sequence_for<Args...>{});
            return *this;
        }
        
        /**
         * Deserialize std::optional
         */
        template<typename T>
        BinaryReader& operator()(std::optional<T>& opt)
        {
            bool hasValue;
            (*this)(hasValue);
            if (hasValue)
            {
                T value;
                (*this)(value);
                opt = std::move(value);
            }
            else
            {
                opt = std::nullopt;
            }
            return *this;
        }
        
        /**
         * Deserialize std::map
         */
        template<typename K, typename V, typename Compare, typename Allocator>
        BinaryReader& operator()(std::map<K, V, Compare, Allocator>& map)
        {
            size_t size;
            (*this)(size);
            
            // Sanity check
            const size_t maxMapSize = 10000000; // 10 million entries max
            if (size > maxMapSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }
            
            map.clear();
            for (size_t i = 0; i < size; ++i)
            {
                K key;
                V value;
                (*this)(key)(value);
                if (HasError()) break;
                map.emplace(std::move(key), std::move(value));
            }
            return *this;
        }
        
        /**
         * Deserialize std::unordered_map
         */
        template<typename K, typename V, typename Hash, typename Equal, typename Allocator>
        BinaryReader& operator()(std::unordered_map<K, V, Hash, Equal, Allocator>& map)
        {
            size_t size;
            (*this)(size);
            
            // Sanity check
            const size_t maxMapSize = 10000000; // 10 million entries max
            if (size > maxMapSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }
            
            map.clear();
            map.reserve(size);
            for (size_t i = 0; i < size; ++i)
            {
                K key;
                V value;
                (*this)(key)(value);
                if (HasError()) break;
                map.emplace(std::move(key), std::move(value));
            }
            return *this;
        }
        
        /**
         * Deserialize
         */
        template<typename T, typename Compare, typename Allocator>
        BinaryReader& operator()(std::set<T, Compare, Allocator>& set)
        {
            size_t size;
            (*this)(size);
            
            // Sanity check
            const size_t maxSetSize = 10000000; // 10 million entries max
            if (size > maxSetSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }
            
            set.clear();
            for (size_t i = 0; i < size; ++i)
            {
                T item;
                (*this)(item);
                if (HasError()) break;
                set.insert(std::move(item));
            }
            return *this;
        }
        
        /**
         * Deserialize std::unordered_set
         */
        template<typename T, typename Hash, typename Equal, typename Allocator>
        BinaryReader& operator()(std::unordered_set<T, Hash, Equal, Allocator>& set)
        {
            size_t size;
            (*this)(size);
            
            // Sanity check
            const size_t maxSetSize = 10000000; // 10 million entries max
            if (size > maxSetSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }
            
            set.clear();
            set.reserve(size);
            for (size_t i = 0; i < size; ++i)
            {
                T item;
                (*this)(item);
                if (HasError()) break;
                set.insert(std::move(item));
            }
            return *this;
        }
        
        /**
         * Read a component hash
         */
        uint64_t ReadComponentHash()
        {
            uint64_t hash = 0;
            (*this)(hash);
            return hash;
        }
        
        /**
         * Read a versioned component with SerializationTraits
         * This reads the component hash, version, and data with migration support
         */
        template<typename T>
        Result<void, SerializationError> ReadVersionedComponent(T& component)
        {
            // Read and verify component hash
            uint64_t storedHash = ReadComponentHash();
            uint64_t expectedHash = TypeID<T>::Hash();
            
            if (storedHash != expectedHash)
            {
                // Component type mismatch
                m_error = SerializationError::UnknownComponent;
                return Result<void, SerializationError>::Err(SerializationError::UnknownComponent);
            }
            
            // Read component version
            uint32_t storedVersion;
            (*this)(storedVersion);
            
            if (HasError())
            {
                return Result<void, SerializationError>::Err(m_error);
            }
            
            // Check version compatibility
            if (storedVersion < SerializationTraits<T>::MinVersion)
            {
                // Version too old to migrate
                m_error = SerializationError::UnsupportedVersion;
                return Result<void, SerializationError>::Err(SerializationError::UnsupportedVersion);
            }
            
            // Read component data
            if (storedVersion == SerializationTraits<T>::Version)
            {
                // Current version - direct read
                if constexpr (SerializationTraits<T>::HasCustomSerializer)
                {
                    // Use custom serialization from traits - call with specific BinaryReader type
                    if constexpr (requires { SerializationTraits<T>::Serialize(*this, component); })
                    {
                        SerializationTraits<T>::Serialize(*this, component);
                    }
                    else
                    {
                        static_assert(sizeof(T) == 0, "SerializationTraits must provide Serialize(BinaryReader&, T&)");
                    }
                }
                else if constexpr (HasSerializeMethod<T, BinaryReader>)
                {
                    component.Serialize(*this);
                }
                else if constexpr (std::is_trivially_copyable_v<T>)
                {
                    (*this)(component);
                }
                else
                {
                    static_assert(sizeof(T) == 0, "Type cannot be deserialized");
                }
            }
            else
            {
                // Old version - need migration
                // This requires SerializationTraits to have a Migrate method
                if constexpr (requires { SerializationTraits<T>::Migrate(*this, component, storedVersion); })
                {
                    SerializationTraits<T>::Migrate(*this, component, storedVersion);
                }
                else
                {
                    // No migration path available
                    m_error = SerializationError::UnsupportedVersion;
                    return Result<void, SerializationError>::Err(SerializationError::UnsupportedVersion);
                }
            }
            
            if (HasError())
            {
                return Result<void, SerializationError>::Err(m_error);
            }
            
            return Result<void, SerializationError>::Ok();
        }
        
        /**
         * Skip alignment padding
         */
        void SkipPadding(size_t alignment)
        {
            size_t padding = (alignment - (m_position % alignment)) % alignment;
            Skip(padding);
        }
        
        /**
         * Skip bytes
         */
        void Skip(size_t bytes)
        {
            if (m_position + bytes > m_size)
            {
                m_error = SerializationError::CorruptedData;
                return;
            }
            
            if (!m_data.empty())
            {
                // Memory mode - just advance position
                m_position += bytes;
            }
            else if (m_file.is_open())
            {
                // File mode - seek forward
                m_file.seekg(bytes, std::ios::cur);
                if (!m_file.good())
                {
                    m_error = SerializationError::IOError;
                }
                m_position += bytes;
            }
        }
        
        /**
         * Peek at bytes without advancing position
         */
        template<typename T>
        requires std::is_trivially_copyable_v<T>
        T Peek()
        {
            T value;
            size_t savedPos = m_position;
            (*this)(value);
            m_position = savedPos;
            return value;
        }
        
        /**
         * Get current position
         */
        [[nodiscard]] size_t GetPosition() const noexcept { return m_position; }
        
        /**
         * Get total size
         */
        [[nodiscard]] size_t GetSize() const noexcept { return m_size; }
        
        /**
         * Get remaining bytes
         */
        [[nodiscard]] size_t GetRemaining() const noexcept 
        { 
            return m_position < m_size ? m_size - m_position : 0; 
        }
        
        /**
         * Check for errors
         */
        [[nodiscard]] bool HasError() const noexcept { return m_error != SerializationError::None; }
        [[nodiscard]] SerializationError GetError() const noexcept { return m_error; }
        
        /**
         * Verify checksum after reading all data
         * Call this after finishing all read operations to verify data integrity
         */
        Result<void, SerializationError> VerifyChecksum()
        {
            if (!m_checksumEnabled || m_headerSize == 0)
            {
                // Checksum not enabled or no header was read
                return Result<void, SerializationError>::Ok();
            }
            
            if (m_runningChecksum != m_expectedChecksum)
            {
                m_error = SerializationError::ChecksumMismatch;
                return Result<void, SerializationError>::Err(SerializationError::ChecksumMismatch);
            }
            
            return Result<void, SerializationError>::Ok();
        }
        
        /**
         * Enable/disable checksum verification
         */
        void SetChecksumEnabled(bool enabled) { m_checksumEnabled = enabled; }
        [[nodiscard]] bool IsChecksumEnabled() const noexcept { return m_checksumEnabled; }
        [[nodiscard]] uint32_t GetChecksum() const noexcept { return m_runningChecksum; }
        [[nodiscard]] uint32_t GetExpectedChecksum() const noexcept { return m_expectedChecksum; }
        
    private:
        std::ifstream m_file;
        std::span<const std::byte> m_data;
        std::vector<std::byte> m_buffer;  // For small files loaded into memory
        size_t m_size;
        size_t m_position;
        SerializationError m_error = SerializationError::None;
        
        // Checksum support
        bool m_checksumEnabled;
        uint32_t m_runningChecksum;
        uint32_t m_expectedChecksum;
        size_t m_headerSize;
        
        // Compression support
        CompressionMode m_compressionMode = CompressionMode::None;
    };
}