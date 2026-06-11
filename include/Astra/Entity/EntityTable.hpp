#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>
#include <cassert>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "Entity.hpp"

namespace Astra
{
    class EntityTable
    {
        struct Segment;

    public:
        using IDType = Entity::StorageType;
        using VersionType = Entity::VersionType;
        
        static constexpr VersionType NULL_VERSION = 0;      // Marks uninitialized/destroyed slots
        static constexpr VersionType INITIAL_VERSION = 1;   // First valid version
        
        // Memory constants
        static constexpr size_t SEGMENT_SIZE = 64 * 1024;           // 64KB per segment
        static constexpr size_t SEGMENTS_PER_HUGE_PAGE = HUGE_PAGE_SIZE / SEGMENT_SIZE; // 32 segments
        
        struct Config
        {
            IDType entitiesPerSegment = 65536;      // 64K entities = 64KB per segment (must be power of 2)
            IDType entitiesPerSegmentShift = 16;    // log2(65536) for fast division
            IDType entitiesPerSegmentMask = 65535;  // 65536 - 1 for fast modulo
            float releaseThreshold = 0.1f;          // Release when <10% used
            bool autoRelease = true;                // Auto-release empty segments
            size_t maxEmptySegments = 2;            // Keep some empty segments ready
            size_t maxPooledSegments = 4;           // Maximum segments to keep in pool for reuse
            bool useHugePages = true;                // Try to use huge pages for initial allocation
            
            Config(IDType segmentSize = 65536)
            {
                entitiesPerSegment = segmentSize > 0 ? std::bit_floor(segmentSize) : 1;
                entitiesPerSegment = std::max(IDType(1024), entitiesPerSegment);
                entitiesPerSegmentShift = static_cast<IDType>(std::countr_zero(entitiesPerSegment));
                entitiesPerSegmentMask = entitiesPerSegment - 1;
            }
        };

        EntityTable() : m_config(), m_hugePageMemory(nullptr), m_nextHugePageSegment(0)
        {
            AllocateHugePage();
        }
        
        explicit EntityTable(const Config& config) : m_config(config), m_hugePageMemory(nullptr), m_nextHugePageSegment(0)
        {
            if (config.useHugePages)
            {
                AllocateHugePage();
            }
        }
        
        ~EntityTable()
        {
            Clear();
            m_segmentPool.clear(); // Clear pool
            ReleaseHugePage();
        }
        
        // Delete copy operations
        EntityTable(const EntityTable&) = delete;
        EntityTable& operator=(const EntityTable&) = delete;
        
        // Move constructor
        EntityTable(EntityTable&& other) noexcept :
            m_segments(std::move(other.m_segments)),
            m_segmentIndex(std::move(other.m_segmentIndex)),
            m_segmentPool(std::move(other.m_segmentPool)),
            m_config(std::move(other.m_config)),
            m_totalAlive(other.m_totalAlive),
            m_hugePageMemory(other.m_hugePageMemory),
            m_usingHugePages(other.m_usingHugePages),
            m_nextHugePageSegment(other.m_nextHugePageSegment)
        {
            other.m_hugePageMemory = nullptr;
            other.m_totalAlive = 0;
            other.m_nextHugePageSegment = 0;
        }
        
        // Move assignment operator
        EntityTable& operator=(EntityTable&& other) noexcept
        {
            if (this != &other)
            {
                Clear();
                m_segmentPool.clear();
                ReleaseHugePage();
                
                m_config = std::move(other.m_config);
                m_segments = std::move(other.m_segments);
                m_segmentIndex = std::move(other.m_segmentIndex);
                m_segmentPool = std::move(other.m_segmentPool);
                m_totalAlive = other.m_totalAlive;
                m_hugePageMemory = other.m_hugePageMemory;
                m_usingHugePages = other.m_usingHugePages;
                m_nextHugePageSegment = other.m_nextHugePageSegment;
                
                other.m_hugePageMemory = nullptr;
                other.m_totalAlive = 0;
                other.m_nextHugePageSegment = 0;
            }
            return *this;
        }
        
        void SetVersion(IDType id, VersionType version)
        {
            auto* segment = GetOrCreateSegment(id);
            ASTRA_ASSERT(segment, "Failed to create segment");
            
            size_t localIdx = segment->ToLocal(id);
            VersionType oldVersion = segment->versions[localIdx];
            
            // Update alive count
            if (oldVersion == NULL_VERSION && version != NULL_VERSION)
            {
                ++segment->aliveCount;
                ++m_totalAlive;
            }
            else if (oldVersion != NULL_VERSION && version == NULL_VERSION)
            {
                --segment->aliveCount;
                --m_totalAlive;
                
                // Check if we should release the segment
                if (m_config.autoRelease && segment->aliveCount == 0) ASTRA_UNLIKELY
                {
                    MaybeReleaseSegments();
                }
            }
            
            segment->versions[localIdx] = version;
        }

        ASTRA_NODISCARD VersionType GetVersion(IDType id) const noexcept
        {
            const auto* segment = GetSegment(id);
            if (!segment) ASTRA_UNLIKELY
                return NULL_VERSION;
            size_t localIdx = segment->ToLocal(id);
            return segment->versions[localIdx];
        }

        ASTRA_NODISCARD bool IsAlive(IDType id, VersionType version) const noexcept
        {
            if (version == NULL_VERSION)
                return false;
            return GetVersion(id) == version;
        }

        VersionType Destroy(IDType id) noexcept
        {
            auto* segment = GetSegment(id);
            if (!segment) ASTRA_UNLIKELY
                return NULL_VERSION;
            
            size_t localIdx = segment->ToLocal(id);
            VersionType oldVersion = segment->versions[localIdx];
            
            if (oldVersion != NULL_VERSION)
            {
                segment->versions[localIdx] = NULL_VERSION;
                --segment->aliveCount;
                --m_totalAlive;
                
                if (m_config.autoRelease && segment->aliveCount == 0) ASTRA_UNLIKELY
                {
                    MaybeReleaseSegments();
                }
            }
            
            return oldVersion;
        }

        template<typename InputIt>
        void SetVersionBatch(InputIt first, InputIt last)
        {
            for (auto it = first; it != last; ++it)
            {
                SetVersion(it->first, it->second);
            }
        }

        ASTRA_NODISCARD size_t AliveCount() const noexcept
        {
            return m_totalAlive;
        }

        void Clear() noexcept
        {
            // Move reusable segments to pool before clearing
            for (auto& segment : m_segments)
            {
                if (segment && !segment->isFromHugePage && m_segmentPool.size() < m_config.maxPooledSegments)
                {
                    segment->Reset(0); // Reset with dummy baseID
                    m_segmentPool.push_back(std::move(segment));
                }
            }
            
            // Clear segments but don't release huge page memory
            m_segments.clear();
            m_segmentIndex.clear();
            m_totalAlive = 0;
            
            // Reset huge page allocation tracking but keep memory
            m_nextHugePageSegment = 0;
        }

        void Reserve(size_t entityCount)
        {
            size_t segmentsNeeded = (entityCount + m_config.entitiesPerSegmentMask) >> m_config.entitiesPerSegmentShift;
            m_segments.reserve(segmentsNeeded);
            m_segmentIndex.reserve(segmentsNeeded);
        }

        void MaybeReleaseSegments()
        {
            if (!m_config.autoRelease)
                return;
            
            size_t emptyCount = 0;
            
            for (size_t i = 0; i < m_segments.size(); ++i)
            {
                auto& segment = m_segments[i];
                if (!segment)
                {
                    continue;
                }
                
                if (segment->aliveCount == 0)
                {
                    ++emptyCount;
                    
                    if (emptyCount > m_config.maxEmptySegments)
                    {
                        size_t segBase = static_cast<size_t>(segment->baseID >> m_config.entitiesPerSegmentShift);
                        if (segBase < m_segmentIndex.size())
                        {
                            m_segmentIndex[segBase] = Segment::INVALID_SEGMENT;
                        }
                        
                        // Return to pool instead of deleting (unless pool is full)
                        if (!segment->isFromHugePage && m_segmentPool.size() < m_config.maxPooledSegments)
                        {
                            m_segmentPool.push_back(std::move(segment));
                        }
                        else
                        {
                            segment.reset();
                        }
                    }
                }
            }
            
            size_t nullCount = std::count(m_segments.begin(), m_segments.end(), nullptr);
            if (nullCount > m_segments.size() / 2)
            {
                m_segments.erase(
                    std::remove(m_segments.begin(), m_segments.end(), nullptr),
                    m_segments.end()
                );
                RebuildSegmentIndex();
            }
        }

        void ShrinkToFit()
        {
            m_segments.erase(
                std::remove(m_segments.begin(), m_segments.end(), nullptr),
                m_segments.end()
            );
            m_segments.shrink_to_fit();
            RebuildSegmentIndex();
        }

        class iterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = std::pair<IDType, VersionType>;
            using difference_type = std::ptrdiff_t;
            using pointer = const value_type*;
            using reference = value_type;
            
            iterator(const EntityTable* table, bool isEnd) noexcept :
                m_table(table),
                m_segmentIdx(isEnd ? table->m_segments.size() : 0),
                m_localIdx(0),
                m_currentSegment(nullptr)
            {
                if (!isEnd) ASTRA_LIKELY
                {
                    SkipToNextValid();
                }
            }
            
            ASTRA_NODISCARD value_type operator*() const noexcept
            {
                ASTRA_ASSERT(m_currentSegment, "Iterator out of range");
                IDType id = m_currentSegment->baseID + static_cast<IDType>(m_localIdx);
                VersionType version = m_currentSegment->versions[m_localIdx];
                ASTRA_ASSERT(version != NULL_VERSION, "Iterator on invalid entity");
                return {id, version};
            }
            
            iterator& operator++() noexcept
            {
                ++m_localIdx;
                SkipToNextValid();
                return *this;
            }
            
            iterator operator++(int) noexcept
            {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }
            
            ASTRA_NODISCARD bool operator==(const iterator& other) const noexcept
            {
                return m_table == other.m_table && m_segmentIdx == other.m_segmentIdx && m_localIdx == other.m_localIdx;
            }
            
            ASTRA_NODISCARD bool operator!=(const iterator& other) const noexcept
            {
                return !(*this == other);
            }

        private:
            void SkipToNextValid() noexcept
            {
                while (m_segmentIdx < m_table->m_segments.size())
                {
                    // Skip null segments
                    while (m_segmentIdx < m_table->m_segments.size() && !m_table->m_segments[m_segmentIdx]) ASTRA_UNLIKELY
                    {
                        ++m_segmentIdx;
                        m_localIdx = 0;
                    }

                    if (m_segmentIdx >= m_table->m_segments.size()) ASTRA_UNLIKELY
                    {
                        break;
                    }

                    m_currentSegment = m_table->m_segments[m_segmentIdx].get();

                    // Find next valid entity in current segment
                    while (m_localIdx < m_currentSegment->capacity) ASTRA_LIKELY
                    {
                        if (m_currentSegment->versions[m_localIdx] != NULL_VERSION) ASTRA_LIKELY
                        {
                            return; // Found valid entity
                        }
                        ++m_localIdx;
                    }

                    // Move to next segment
                    ++m_segmentIdx;
                    m_localIdx = 0;
                }

                // End iterator
                m_currentSegment = nullptr;
            }

            const EntityTable* m_table;
            size_t m_segmentIdx;
            size_t m_localIdx;
            const Segment* m_currentSegment;
        };
        
        ASTRA_NODISCARD iterator begin() const noexcept
        {
            return iterator(this, false);
        }
        
        ASTRA_NODISCARD iterator end() const noexcept
        {
            return iterator(this, true);
        }

    private:
        struct Segment
        {
            static constexpr size_t INVALID_SEGMENT = std::numeric_limits<size_t>::max();

            IDType baseID;                              // First ID in this segment (mutable for reuse)
            const IDType capacity;                      // Entities in this segment
            VersionType* versions;                      // Version array (either from huge page or heap)
            std::unique_ptr<VersionType[]> ownedMemory; // Only set if we own the memory
            size_t aliveCount = 0;                      // Number of alive entities
            bool isFromHugePage = false;                // True if memory is from huge page

            // Constructor for huge page allocation
            Segment(IDType base, IDType cap, VersionType* hugePageMemory) :
                baseID(base),
                capacity(cap),
                versions(hugePageMemory),
                ownedMemory(nullptr),
                isFromHugePage(true)
            {
                std::fill_n(versions, capacity, NULL_VERSION);
            }
            
            // Constructor for regular allocation
            explicit Segment(IDType base, IDType cap) :
                baseID(base),
                capacity(cap),
                versions(nullptr),
                ownedMemory(std::make_unique<VersionType[]>(cap)),
                isFromHugePage(false)
            {
                versions = ownedMemory.get();
                std::fill_n(versions, capacity, NULL_VERSION);
            }
            
            // Reset segment for reuse with new base ID
            void Reset(IDType newBaseID) noexcept
            {
                baseID = newBaseID;
                aliveCount = 0;
                std::fill_n(versions, capacity, NULL_VERSION);
            }

            bool Contains(IDType id) const noexcept
            {
                return id >= baseID && id < baseID + capacity;
            }

            size_t ToLocal(IDType id) const noexcept
            {
                ASTRA_ASSERT(Contains(id), "ID out of segment bounds");
                return id - baseID;
            }

            float Usage() const noexcept
            {
                return capacity > 0 ? static_cast<float>(aliveCount) / capacity : 0.0f;
            }
        };

        // Get segment for an ID, creating it if necessary
        Segment* GetOrCreateSegment(IDType id)
        {
            size_t segIdx = static_cast<size_t>(id >> m_config.entitiesPerSegmentShift);

            // Ensure segment index is large enough
            if (segIdx >= m_segmentIndex.size()) ASTRA_UNLIKELY
            {
                m_segmentIndex.resize(segIdx + 1, Segment::INVALID_SEGMENT);
            }

            // Create segment if it doesn't exist
            if (m_segmentIndex[segIdx] == Segment::INVALID_SEGMENT) ASTRA_UNLIKELY
            {
                IDType baseId = static_cast<IDType>(segIdx << m_config.entitiesPerSegmentShift);
                std::unique_ptr<Segment> segment;
                
                // Try to get from pool first
                if (!m_segmentPool.empty())
                {
                    segment = std::move(m_segmentPool.back());
                    m_segmentPool.pop_back();
                    segment->Reset(baseId);
                }
                // Try to allocate from huge page
                else if (m_hugePageMemory && m_nextHugePageSegment < SEGMENTS_PER_HUGE_PAGE)
                {
                    // Calculate offset into huge page
                    VersionType* segmentMemory = reinterpret_cast<VersionType*>(
                        m_hugePageMemory + (m_nextHugePageSegment * SEGMENT_SIZE));
                    
                    segment = std::make_unique<Segment>(baseId, m_config.entitiesPerSegment, segmentMemory);
                    m_nextHugePageSegment++;
                }
                else
                {
                    // Fall back to regular allocation
                    segment = std::make_unique<Segment>(baseId, m_config.entitiesPerSegment);
                }

                m_segments.push_back(std::move(segment));
                m_segmentIndex[segIdx] = m_segments.size() - 1;
            }

            size_t idx = m_segmentIndex[segIdx];
            ASTRA_ASSERT(idx < m_segments.size(), "Segment index out of bounds");
            return m_segments[idx].get();
        }

        // Get segment for an ID (const version, no creation)
        const Segment* GetSegment(IDType id) const noexcept
        {
            size_t segIdx = static_cast<size_t>(id >> m_config.entitiesPerSegmentShift);
            if (segIdx >= m_segmentIndex.size()) ASTRA_UNLIKELY
                return nullptr;
            size_t idx = m_segmentIndex[segIdx];
            if (idx == Segment::INVALID_SEGMENT) ASTRA_UNLIKELY
                return nullptr;

            ASTRA_ASSERT(idx < m_segments.size(), "Segment index out of bounds");
            return m_segments[idx].get();
        }

        // Get segment for an ID (non-const version, no creation)
        Segment* GetSegment(IDType id) noexcept
        {
            size_t segIdx = static_cast<size_t>(id >> m_config.entitiesPerSegmentShift);
            if (segIdx >= m_segmentIndex.size()) ASTRA_UNLIKELY
                return nullptr;
            size_t idx = m_segmentIndex[segIdx];
            if (idx == Segment::INVALID_SEGMENT) ASTRA_UNLIKELY
                return nullptr;

            ASTRA_ASSERT(idx < m_segments.size(), "Segment index out of bounds");
            return m_segments[idx].get();
        }

        void RebuildSegmentIndex()
        {
            std::fill(m_segmentIndex.begin(), m_segmentIndex.end(), Segment::INVALID_SEGMENT);

            for (size_t i = 0; i < m_segments.size(); ++i)
            {
                if (m_segments[i]) ASTRA_LIKELY
                {
                    size_t segIdx = static_cast<size_t>(m_segments[i]->baseID >> m_config.entitiesPerSegmentShift);
                    if (segIdx < m_segmentIndex.size())
                    {
                        m_segmentIndex[segIdx] = i;
                    }
                }
            }
        }

        void AllocateHugePage()
        {
            if (!m_config.useHugePages)
                return;
                
            // Try to allocate 2MB huge page
            AllocResult result = AllocateMemory(HUGE_PAGE_SIZE, 64, AllocFlags::HugePages | AllocFlags::ZeroMem);
            
            if (result.ptr)
            {
                m_hugePageMemory = static_cast<std::byte*>(result.ptr);
                m_usingHugePages = result.usedHugePages;
            }
        }
        
        void ReleaseHugePage()
        {
            if (m_hugePageMemory)
            {
                FreeMemory(m_hugePageMemory, HUGE_PAGE_SIZE, m_usingHugePages);
                m_hugePageMemory = nullptr;
            }
        }

        std::vector<std::unique_ptr<Segment>> m_segments;
        std::vector<size_t> m_segmentIndex; // ID -> segment lookup table
        std::vector<std::unique_ptr<Segment>> m_segmentPool; // Pool of reusable segments
        Config m_config;
        size_t m_totalAlive = 0;
        
        // Huge page memory management
        std::byte* m_hugePageMemory = nullptr;
        bool m_usingHugePages = false;
        size_t m_nextHugePageSegment;  // Next available segment in huge page
    };
}