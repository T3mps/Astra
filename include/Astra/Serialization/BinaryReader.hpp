#pragma once

#include <algorithm>
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
                m_runningChecksum = m_usePortableChecksum
                    ? Checksum::Portable(data, size, m_runningChecksum)
                    : Checksum::CRC32(data, size, m_runningChecksum);   // v1 archives, same-ISA only
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
            m_usePortableChecksum = (m_version >= 2);
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
                if (originalSize > Remaining())
                {
                    m_error = SerializationError::CorruptedData;
                    return Result<std::vector<uint8_t>, SerializationError>::Err(SerializationError::CorruptedData);
                }
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
                // Data is compressed. Bound compressedSize against the remaining
                // buffer BEFORE allocating -- unlike the uncompressed branch above,
                // this branch used to allocate std::vector<uint8_t>(compressedSize)
                // first and only let the ReadBytes call below discover the
                // truncation. compressedSize is a raw uint32_t straight off the
                // wire, so a corrupted save can claim up to ~4GB: the allocation
                // (and its zero-init) happened regardless of how few bytes actually
                // remained, before any bounds check got a chance to reject it. The
                // compressed bytes must be present in the buffer for a valid save,
                // so this can never false-reject a legitimate one.
                if (compressedSize > Remaining())
                {
                    m_error = SerializationError::CorruptedData;
                    return Result<std::vector<uint8_t>, SerializationError>::Err(SerializationError::CorruptedData);
                }

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
            uint64_t len;
            (*this)(len);

            if (len > m_size - m_position)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            str.resize(static_cast<size_t>(len));
            ReadBytes(str.data(), static_cast<size_t>(len));
            return *this;
        }
        
        /**
         * Deserialize vectors
         */
        template<typename T>
        BinaryReader& operator()(std::vector<T>& vec)
        {
            uint64_t size;
            (*this)(size);

            // Take the bulk path only for hook-free trivially-copyable T; a type that
            // is trivially copyable AND defines a Serialize hook must be read element-
            // wise so the hook is honored symmetrically with the writer (IM-8).
            constexpr bool kBulk = std::is_trivially_copyable_v<T> && !HasSerializeMethod<T, BinaryReader>;

            // Sanity check to prevent huge allocations
            // For the bulk (POD) path we know the exact per-element on-disk size;
            // otherwise every element consumes at least 1 byte, so reject any count
            // that cannot possibly fit in what remains BEFORE reserving (IM-12).
            if constexpr (kBulk)
            {
                if (size > (m_size - m_position) / sizeof(T))
                {
                    m_error = SerializationError::CorruptedData;
                    return *this;
                }
            }
            else
            {
                if (CountExceedsRemaining(size, 1))
                {
                    m_error = SerializationError::CorruptedData;
                    return *this;
                }
            }

            vec.clear();
            vec.reserve(static_cast<size_t>(size)); // bounded by the checks above

            if constexpr (kBulk)
            {
                // Read all at once for POD types
                vec.resize(static_cast<size_t>(size));
                ReadBytes(vec.data(), static_cast<size_t>(size) * sizeof(T));
            }
            else
            {
                // Read one by one for complex types (or hook-bearing POD types)
                for (uint64_t i = 0; i < size; ++i)
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
            // See the vector overload: hook-bearing trivially-copyable T must be read
            // element-wise so its Serialize hook is honored (IM-8).
            if constexpr (std::is_trivially_copyable_v<T> && !HasSerializeMethod<T, BinaryReader>)
            {
                // Read all at once for POD types
                ReadBytes(arr.data(), N * sizeof(T));
            }
            else
            {
                // Read one by one for complex types (or hook-bearing POD types)
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
            uint64_t size;
            (*this)(size);

            // Sanity check
            const uint64_t maxMapSize = 10000000; // 10 million entries max
            if (size > maxMapSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            map.clear();
            for (uint64_t i = 0; i < size; ++i)
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
            uint64_t size;
            (*this)(size);

            // Sanity check
            const uint64_t maxMapSize = 10000000; // 10 million entries max
            if (size > maxMapSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            // Every entry writes a key and a value, i.e. at least 1 byte on disk;
            // reject a count that cannot fit in what remains BEFORE reserving so a
            // tiny corrupt input cannot drive a huge speculative allocation (IM-12).
            if (CountExceedsRemaining(size, 1))
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            map.clear();
            map.reserve(static_cast<size_t>(size));
            for (uint64_t i = 0; i < size; ++i)
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
            uint64_t size;
            (*this)(size);

            // Sanity check
            const uint64_t maxSetSize = 10000000; // 10 million entries max
            if (size > maxSetSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            set.clear();
            for (uint64_t i = 0; i < size; ++i)
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
            uint64_t size;
            (*this)(size);

            // Sanity check
            const uint64_t maxSetSize = 10000000; // 10 million entries max
            if (size > maxSetSize)
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            // Every element writes at least 1 byte on disk; reject a count that cannot
            // fit in what remains BEFORE reserving so a tiny corrupt input cannot drive
            // a huge speculative allocation (IM-12).
            if (CountExceedsRemaining(size, 1))
            {
                m_error = SerializationError::CorruptedData;
                return *this;
            }

            set.clear();
            set.reserve(static_cast<size_t>(size));
            for (uint64_t i = 0; i < size; ++i)
            {
                T item;
                (*this)(item);
                if (HasError()) break;
                set.insert(std::move(item));
            }
            return *this;
        }
        
        // Bytes not yet consumed. Invariant m_position <= m_size (ReadBytes enforces it) → no underflow.
        [[nodiscard]] size_t Remaining() const noexcept { return m_size - m_position; }

        // True if a count of `count` elements, each at least `minBytesPerElement` on disk, cannot fit in
        // what remains -- i.e. the count is impossible for this buffer. Overflow-safe (divide, not multiply).
        // N elements need at least N*minBytesPerElement bytes downstream, so N > Remaining()/per cannot be
        // real; this is the standard "length <= remaining" length-prefix validation rule, applied at
        // whatever width the on-disk count field actually is (every real count on the wire is a uint32_t,
        // not the uint64_t this used to force via a dedicated read) after the caller has already read it.
        [[nodiscard]] bool CountExceedsRemaining(uint64_t count, size_t minBytesPerElement) const noexcept
        {
            const size_t per = (minBytesPerElement == 0) ? 1 : minBytesPerElement;
            return count > static_cast<uint64_t>(Remaining() / per);
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

            // Skipped bytes MUST feed the running checksum exactly as ReadBytes does:
            // WritePadding emits real zero bytes through WriteBytes (folded into the
            // writer's checksum), so a reader that merely advances past them would
            // always mismatch the checksum (IM-11). Mirror ReadBytes' guard, which is
            // evaluated against the start position before it advances.
            const bool updateChecksum = m_checksumEnabled && m_position >= m_headerSize && m_headerSize > 0;

            if (!m_data.empty())
            {
                // Memory mode - checksum the bytes in place, then advance
                if (updateChecksum)
                {
                    const void* skipped = m_data.data() + m_position;
                    m_runningChecksum = m_usePortableChecksum
                        ? Checksum::Portable(skipped, bytes, m_runningChecksum)
                        : Checksum::CRC32(skipped, bytes, m_runningChecksum);
                }
                m_position += bytes;
            }
            else if (m_file.is_open())
            {
                if (updateChecksum)
                {
                    // File mode - read-and-discard so the checksum sees the skipped
                    // bytes (a bare seek would bypass the accumulation).
                    std::array<std::byte, 512> scratch;
                    size_t remaining = bytes;
                    while (remaining > 0)
                    {
                        size_t chunk = std::min(remaining, scratch.size());
                        m_file.read(reinterpret_cast<char*>(scratch.data()), static_cast<std::streamsize>(chunk));
                        if (!m_file.good())
                        {
                            m_error = SerializationError::IOError;
                            return;
                        }
                        m_runningChecksum = m_usePortableChecksum
                            ? Checksum::Portable(scratch.data(), chunk, m_runningChecksum)
                            : Checksum::CRC32(scratch.data(), chunk, m_runningChecksum);
                        remaining -= chunk;
                    }
                }
                else
                {
                    // File mode - seek forward (no checksum to maintain)
                    m_file.seekg(bytes, std::ios::cur);
                    if (!m_file.good())
                    {
                        m_error = SerializationError::IOError;
                    }
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
            T value{};
            size_t savedPos = m_position;
            uint32_t savedChecksum = m_runningChecksum;   // Peek must not contaminate the checksum
            (*this)(value);
            if (m_data.empty() && m_file.is_open())
            {
                // File mode: rewind the stream cursor too.
                m_file.seekg(static_cast<std::streamoff>(savedPos), std::ios::beg);
            }
            m_position = savedPos;
            m_runningChecksum = savedChecksum;
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

        /**
         * The compression mode read from the file header (see ReadHeader).
         * Used by column deserializers to choose the compressed vs inline path.
         */
        [[nodiscard]] CompressionMode GetCompressionMode() const noexcept { return m_compressionMode; }
        
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
        bool m_usePortableChecksum = true;
        uint32_t m_expectedChecksum;
        size_t m_headerSize;
        
        // Compression support
        CompressionMode m_compressionMode = CompressionMode::None;
    };
}