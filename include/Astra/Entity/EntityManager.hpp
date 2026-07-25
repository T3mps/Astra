#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"
#include "Entity.hpp"
#include "EntityIDStack.hpp"
#include "EntityTable.hpp"

namespace Astra
{
    class EntityManager
    {
    public:
        using VersionType = Entity::VersionType;
        using IDType = Entity::StorageType;
        
        struct Config
        {
            EntityTable::Config tableConfig;
            
            Config(IDType segmentSize = EntityTable::Config::DEFAULT_ENTITIES_PER_SEGMENT) :
                tableConfig(segmentSize)
            {}
        };

        static constexpr VersionType NULL_VERSION = EntityTable::NULL_VERSION;
        static constexpr VersionType INITIAL_VERSION = EntityTable::INITIAL_VERSION;
        static constexpr IDType INVALID_ID = EntityIDStack::INVALID_ID;

        EntityManager() = default;

        explicit EntityManager(std::size_t capacity)
        {
            Reserve(capacity);
        }
        
        explicit EntityManager(const Config& config) :
            m_idStack(),
            m_table(config.tableConfig),
            m_config(config)
        {}

        ASTRA_NODISCARD Entity Create() noexcept
        {
            auto [id, version] = m_idStack.Allocate();
            
            if (id == INVALID_ID) ASTRA_UNLIKELY
            {
                return Entity::Invalid();
            }
            
            m_table.SetVersion(id, version);
            return Entity(id, version);
        }

        // Creates up to `count` entities; returns how many were actually
        // created (can be < count when the ID space is exhausted). Only the
        // first `return-value` slots of the output are written.
        template<typename OutputIt>
        std::size_t CreateBatch(std::size_t count, OutputIt out) noexcept
        {
            if (count == 0)
                ASTRA_UNLIKELY return 0;
            
            // For small batches, simple loop is fine
            if (count < 32) ASTRA_LIKELY
            {
                std::size_t created = 0;
                for (std::size_t i = 0; i < count; ++i)
                {
                    Entity e = Create();
                    if (!e.IsValid()) ASTRA_UNLIKELY
                        break;
                    *out++ = e;
                    ++created;
                }
                return created;
            }
            
            // Large batch: allocate IDs in batch
            SmallVector<EntityIDStack::VersionedID, 256> allocations;
            allocations.resize(count);
            
            size_t allocated = m_idStack.AllocateBatch(count, allocations.begin());
            
            // Set versions in batch and create entities
            for (size_t i = 0; i < allocated; ++i)
            {
                auto [id, version] = allocations[i];
                m_table.SetVersion(id, version);
                *out++ = Entity(id, version);
            }
            return allocated;
        }

        bool Destroy(Entity entity) noexcept
        {
            if (!IsValid(entity)) ASTRA_UNLIKELY
            {
                return false;
            }

            const IDType id = entity.GetID();
            const VersionType currentVersion = entity.GetVersion();
            
            // Verify version matches
            if (m_table.GetVersion(id) != currentVersion) ASTRA_UNLIKELY
            {
                return false;
            }

            // Calculate next version with wraparound (mask-correct for any VersionBits)
            const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);

            // Mark as destroyed in table
            m_table.Destroy(id);

            // Recycle the ID with next version
            m_idStack.Recycle(id, nextVersion, true);  // preferLocal = true for segment locality

            return true;
        }

        // Record-completing variant: the caller (Registry::DestroyEntity) already
        // holds the entity's VALIDATED record, so the IsValid/GetVersion re-walks
        // are skipped. The version write stays HERE -- EntityManager alone owns
        // EntityRecord::version (W1).
        bool Destroy(Entity entity, EntityRecord* rec) noexcept
        {
            ASTRA_ASSERT(rec == m_table.GetRecord(entity.GetID()), "record/entity mismatch");
            const VersionType currentVersion = entity.GetVersion();
            if (rec->version != currentVersion) ASTRA_UNLIKELY
            {
                return false;
            }

            const IDType id = entity.GetID();

            // Calculate next version with wraparound (mask-correct for any VersionBits)
            const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);

            // Mark as destroyed in table
            m_table.Destroy(id);

            // Recycle the ID with next version
            m_idStack.Recycle(id, nextVersion, true);  // preferLocal = true for segment locality

            return true;
        }

        template<typename InputIt>
        std::size_t DestroyBatch(InputIt first, InputIt last) noexcept
        {
            const size_t estimatedCount = std::distance(first, last);
            
            // For small batches, use simple approach
            if (estimatedCount < 32) ASTRA_LIKELY
            {
                std::size_t destroyed = 0;
                for (auto it = first; it != last; ++it)
                {
                    if (Destroy(*it)) ASTRA_LIKELY
                    {
                        ++destroyed;
                    }
                }
                return destroyed;
            }
            
            // Large batch: collect valid entities and their recycling info
            SmallVector<EntityIDStack::RecycledEntry, 256> toRecycle;
            std::size_t destroyed = 0;
            
            for (auto it = first; it != last; ++it)
            {
                if (!IsValid(*it)) ASTRA_UNLIKELY continue;
                
                IDType id = it->GetID();
                VersionType currentVersion = it->GetVersion();
                
                // Verify version matches
                if (m_table.GetVersion(id) != currentVersion) ASTRA_UNLIKELY continue;
                
                // Calculate next version (mask-correct for any VersionBits)
                const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                    currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);
                
                // Mark as destroyed
                m_table.Destroy(id);
                toRecycle.push_back({id, nextVersion});
                ++destroyed;
            }
            
            // Batch recycle the IDs
            if (!toRecycle.empty())
            {
                m_idStack.RecycleBatch(toRecycle.begin(), toRecycle.end());
            }
            
            return destroyed;
        }

        ASTRA_NODISCARD bool IsValid(Entity entity) const noexcept
        {
            const IDType id = entity.GetID();
            const VersionType version = entity.GetVersion();
            
            if (version == NULL_VERSION) return false;
            return m_table.IsAlive(id, version);
        }

        ASTRA_NODISCARD VersionType GetVersion(IDType id) const noexcept
        {
            return m_table.GetVersion(id);
        }

        // The shared paged EntityRecord table. ArchetypeManager is injected a
        // pointer to this same table so validate (version) and locate
        // (archetype/location) hit one paged slot. EntityManager owns its
        // lifecycle; ArchetypeManager only writes archetype/location, never version.
        ASTRA_NODISCARD EntityTable&       GetRecordTable()       noexcept { return m_table; }
        ASTRA_NODISCARD const EntityTable& GetRecordTable() const noexcept { return m_table; }

        void Clear() noexcept
        {
            m_idStack.Clear();
            m_table.Clear();
        }

        void Reserve(std::size_t capacity)
        {
            m_idStack.Reserve(capacity);
            m_table.Reserve(capacity);
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept
        {
            return m_table.AliveCount();
        }

        ASTRA_NODISCARD std::size_t Capacity() const noexcept
        {
            return m_idStack.GetNextID();
        }

        ASTRA_NODISCARD std::size_t RecycledCount() const noexcept
        {
            return m_idStack.RecycledCount();
        }

        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_table.AliveCount() == 0;
        }

        void ShrinkToFit()
        {
            m_idStack.ShrinkToFit();
            m_table.ShrinkToFit();
        }

        // Iterator support for range-based for loops
        class iterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entity;
            using difference_type = std::ptrdiff_t;
            using pointer = Entity*;
            using reference = Entity;
            
            iterator(EntityTable::iterator it) : m_it(it) {}
            
            reference operator*() const 
            {
                auto [id, version] = *m_it;
                return Entity(id, version);
            }
            
            iterator& operator++() 
            {
                ++m_it;
                return *this;
            }
            
            iterator operator++(int) 
            {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }
            
            bool operator==(const iterator& other) const { return m_it == other.m_it; }
            bool operator!=(const iterator& other) const { return m_it != other.m_it; }
            
        private:
            EntityTable::iterator m_it;
        };
        
        iterator begin() const { return iterator(m_table.begin()); }
        iterator end() const { return iterator(m_table.end()); }
        
        void Validate() const noexcept
        {
#ifdef ASTRA_BUILD_DEBUG
            // Verify that alive count matches table's alive count
            // Note: Can't iterate without iterators, so we trust the table's count
            ASTRA_ASSERT(m_table.AliveCount() == Size(), "Alive count mismatch");
            ASTRA_ASSERT(m_idStack.GetNextID() <= Entity::ID_MASK, "Next ID overflow");
#endif
        }

        void Serialize(BinaryWriter& writer) const
        {
            // Write configuration (only table config now)
            writer(m_config.tableConfig.entitiesPerSegment);
            writer(m_config.tableConfig.entitiesPerSegmentShift);
            writer(m_config.tableConfig.entitiesPerSegmentMask);
            writer(m_config.tableConfig.releaseThreshold);
            writer(m_config.tableConfig.autoRelease);
            writer(static_cast<uint64_t>(m_config.tableConfig.maxEmptySegments));
            
            // Write ID stack state
            writer(m_idStack.GetNextID());
            
            // Get and write all recycled entries
            std::vector<EntityIDStack::RecycledEntry> recycledEntries;
            m_idStack.GetAllRecycledEntries(recycledEntries);
            writer(static_cast<uint32_t>(recycledEntries.size()));
            for (const auto& entry : recycledEntries)
            {
                writer(entry.id);
                writer(entry.nextVersion);
            }
            
            // Write table state
            writer(static_cast<uint32_t>(m_table.AliveCount()));
            
            // Write all alive entities
            for (auto it = m_table.begin(); it != m_table.end(); ++it)
            {
                auto [id, version] = *it;
                writer(id);
                writer(version);
            }
        }

        static Result<std::unique_ptr<EntityManager>, SerializationError> Deserialize(BinaryReader& reader)
        {
            auto manager = std::make_unique<EntityManager>();
            
            // Read configuration (only table config now)
            reader(manager->m_config.tableConfig.entitiesPerSegment);
            reader(manager->m_config.tableConfig.entitiesPerSegmentShift);
            reader(manager->m_config.tableConfig.entitiesPerSegmentMask);
            reader(manager->m_config.tableConfig.releaseThreshold);
            reader(manager->m_config.tableConfig.autoRelease);
            uint64_t maxEmptySegments;
            reader(maxEmptySegments);
            manager->m_config.tableConfig.maxEmptySegments = static_cast<size_t>(maxEmptySegments);

            if (reader.HasError())
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
            }

            // Validate the segment-config trio BEFORE anything derives sizing or
            // indexing from it. EntityTable::GetOrCreateSegment computes
            // `segIdx = id >> entitiesPerSegmentShift` and then resizes
            // m_segmentIndex to segIdx + 1, and Segment allocates
            // `versions[entitiesPerSegment]` with ToLocal(id) = id - baseID guarded
            // only by an ASTRA_ASSERT (compiled out in Release/Dist). A valid save
            // (EntityManager::Serialize above) always writes a mutually consistent
            // triple: entitiesPerSegment is a power of two, shift ==
            // log2(entitiesPerSegment), and mask == entitiesPerSegment - 1.
            // Corrupting any one of the three decouples segment sizing from
            // indexing and can otherwise drive a multi-GB m_segmentIndex resize, a
            // heap OOB write into Segment::versions[], or the ToLocal fail-fast --
            // none of which is recoverable once reached. Reject unless all of the
            // following hold, mirroring the reader's existing failure convention.
            {
                const IDType entitiesPerSegment = manager->m_config.tableConfig.entitiesPerSegment;
                const IDType entitiesPerSegmentShift = manager->m_config.tableConfig.entitiesPerSegmentShift;
                const IDType entitiesPerSegmentMask = manager->m_config.tableConfig.entitiesPerSegmentMask;

                // (1) entitiesPerSegment must be nonzero and a power of two.
                const bool isPowerOfTwo = entitiesPerSegment != 0 &&
                    (entitiesPerSegment & static_cast<IDType>(entitiesPerSegment - 1)) == 0;

                // (2) entitiesPerSegmentShift must be a valid shift amount for
                // IDType (otherwise `1 << shift` and `id >> shift` are UB) and must
                // reproduce entitiesPerSegment exactly. Short-circuits before the
                // shift so an out-of-range amount is never evaluated.
                constexpr size_t kIDTypeBits = sizeof(IDType) * 8;
                const bool shiftInRange = entitiesPerSegmentShift < kIDTypeBits;
                const bool shiftMatches = shiftInRange &&
                    static_cast<IDType>(IDType{1} << entitiesPerSegmentShift) == entitiesPerSegment;

                // (3) entitiesPerSegmentMask must be entitiesPerSegment - 1.
                const bool maskMatches = entitiesPerSegmentMask == static_cast<IDType>(entitiesPerSegment - 1);

                // (4) Absolute sanity cap so a consistent-but-absurd triple can't
                // drive a huge per-segment allocation (entitiesPerSegment *
                // sizeof(VersionType) bytes for Segment::versions[]). This bounds
                // ONLY that one per-segment allocation, not the segment machinery
                // generally -- in particular it says nothing about how large
                // m_segmentIndex (ID -> segment lookup table) can grow, since that
                // is driven by id >> entitiesPerSegmentShift, not by
                // entitiesPerSegment itself. m_segmentIndex growth is bounded
                // separately by the kMaxSegmentsOnLoad segIdx cap applied to every
                // restored id below. EntityTable::Config::DEFAULT_ENTITIES_PER_SEGMENT
                // is 65536 (64K, the default 32-bit-ID/8-bit-version build); this cap
                // leaves comfortable headroom above that (and above any smaller
                // ID-space clamp for narrower ID configurations) while still keeping
                // the resulting allocation sane. Widened to uint64_t for the
                // comparison so this is correct across every IDType width the
                // library supports (16/32/64-bit), not just the default 32-bit one.
                constexpr uint64_t kMaxEntitiesPerSegment = uint64_t{1} << 22; // 4,194,304
                const bool withinSanityCap = static_cast<uint64_t>(entitiesPerSegment) <= kMaxEntitiesPerSegment;

                // (5) Lower bound: entitiesPerSegment must be at least the floor
                // EntityTable::Config's converting constructor clamps every
                // legitimately-constructed table to (see EntityTable.hpp -- 1024,
                // or the whole ID space if narrower). The trio-consistency checks
                // above accept a fully self-consistent but absurdly small triple
                // like {entitiesPerSegment=1, shift=0, mask=0} -- a save can never
                // legitimately produce one, since Serialize always writes whatever
                // EntityTable::Config the table was actually constructed with, and
                // that constructor never produces a value below this floor. With
                // shift==0, `id >> shift == id`, so a small-but-legitimate id
                // already makes segIdx == id -- this is the primary defense against
                // that (the kMaxSegmentsOnLoad segIdx cap below is the redundant
                // second layer, since it also catches this same triple once any
                // restored id is large enough).
                constexpr uint64_t kMinEntitiesPerSegment =
                    std::min<uint64_t>(1024, static_cast<uint64_t>(Entity::ID_MASK) + 1ull);
                const bool aboveFloor = static_cast<uint64_t>(entitiesPerSegment) >= kMinEntitiesPerSegment;

                if (!isPowerOfTwo || !shiftMatches || !maskMatches || !withinSanityCap || !aboveFloor)
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
                }
            }

            // Bounds the segment index a restored id can create (see the segIdx
            // checks in the recycled-entry and alive-entity restore loops below).
            // EntityTable::GetOrCreateSegment computes segIdx = id >>
            // entitiesPerSegmentShift and resizes m_segmentIndex (a
            // std::vector<size_t>) to segIdx + 1 -- unbounded by the segment-config
            // validation above, since that only constrains entitiesPerSegment
            // (see point (4)'s comment), not how large id >> shift can get. A valid
            // save allocates ids densely from 0, and entitiesPerSegment is always
            // >= the Config clamp floor (1024, enforced by check (5) above), so a
            // legitimate save's max segIdx is at most (max id) / 1024 -- for every
            // IDType width the library supports (16/32/64-bit), that is far below
            // this cap. Set well above any real save's segIdx while still bounding
            // the resize to a sane size (~1M entries * 8 bytes/entry = ~8MB).
            constexpr uint64_t kMaxSegmentsOnLoad = uint64_t{1} << 20; // 1,048,576 segments

            // Read ID stack state
            IDType nextFreshID;
            reader(nextFreshID);
            
            // Read recycled entries
            uint32_t recycledCount;
            reader(recycledCount);

            if (reader.HasError())
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
            }

            // Bound recycledCount against the remaining buffer via the reader's
            // width-agnostic count-bound helper (recycledCount is a uint32_t on
            // disk; Serialize above writes it via writer(static_cast<uint32_t>(
            // recycledEntries.size()))). Each recycled entry's fixed on-disk
            // footprint is id(IDType) + nextVersion(VersionType), written
            // unconditionally per entry in the loop below.
            constexpr uint64_t kMinBytesPerRecycledEntry = sizeof(IDType) + sizeof(VersionType);
            if (reader.CountExceedsRemaining(recycledCount, kMinBytesPerRecycledEntry))
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
            }

            std::vector<EntityIDStack::RecycledEntry> recycledEntries;
            recycledEntries.reserve(recycledCount);

            for (uint32_t i = 0; i < recycledCount; ++i)
            {
                IDType id;
                VersionType nextVersion;
                reader(id);
                reader(nextVersion);

                // Reject a corrupted id before it can be handed back out by a
                // later Allocate() and flow into EntityTable::SetVersion /
                // GetOrCreateSegment: those compute segIdx = id >> shift and
                // index/resize m_segmentIndex by it, and Segment::ToLocal's
                // bounds check is only an ASTRA_ASSERT (compiled out in
                // Release/Dist). A legitimately-recycled id is always <
                // Entity::ID_MASK -- Allocate()/AllocateBatch() never hand out
                // ID_MASK itself, it's reserved as the "IDs exhausted"
                // sentinel -- so anything >= ID_MASK cannot come from a valid
                // save.
                if (id >= Entity::ID_MASK)
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
                }

                // Bound the segment index this id would create if it were ever
                // recycled back out and reached EntityTable::SetVersion /
                // GetOrCreateSegment (segIdx = id >> entitiesPerSegmentShift,
                // which then resizes m_segmentIndex to segIdx + 1). This loop
                // itself never touches EntityTable -- RestoreRecycledEntries
                // below only feeds the free-id stack -- but a corrupt id (or an
                // abnormally small-but-internally-consistent segment config, see
                // check (5) above) must not be allowed to re-enter circulation
                // only to explode the very next time it's handed out by
                // Allocate(). entitiesPerSegmentShift is already range-validated
                // above, so the shift itself is well-defined.
                if ((static_cast<uint64_t>(id) >> manager->m_config.tableConfig.entitiesPerSegmentShift) >= kMaxSegmentsOnLoad)
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
                }

                recycledEntries.push_back({id, nextVersion});
            }

            // Read table state
            uint32_t aliveCount;
            reader(aliveCount);

            if (reader.HasError())
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
            }

            // Bound aliveCount against the remaining buffer using the same helper;
            // aliveCount is a uint32_t on disk (Serialize writes
            // static_cast<uint32_t>(m_table.AliveCount())) and each alive-entity
            // record's fixed on-disk footprint is id(IDType) + version(VersionType),
            // written unconditionally per entity in the loop below.
            constexpr uint64_t kMinBytesPerAliveEntity = sizeof(IDType) + sizeof(VersionType);
            if (reader.CountExceedsRemaining(aliveCount, kMinBytesPerAliveEntity))
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
            }

            // Recreate table with the restored configuration (not default)
            manager->m_table = EntityTable(manager->m_config.tableConfig);
            
            // Restore ID stack state
            manager->m_idStack.SetNextID(nextFreshID);
            manager->m_idStack.RestoreRecycledEntries(recycledEntries);
            
            // Read and restore alive entities
            for (uint32_t i = 0; i < aliveCount; ++i)
            {
                IDType id;
                VersionType version;
                reader(id);
                reader(version);
                
                if (reader.HasError())
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
                }

                // Reject before SetVersion(id, ...) resolves/creates a segment
                // and indexes into it -- see the recycled-entry check above
                // for why a legitimate id must be < Entity::ID_MASK. Without
                // this, a corrupted id near IDType max makes
                // Segment::Contains's `id < baseID + capacity` overflow-wrap,
                // firing Segment::ToLocal's ASTRA_ASSERT (an uncatchable abort
                // in Debug) or, if it happened to pass, driving an enormous
                // m_segmentIndex resize.
                if (id >= Entity::ID_MASK)
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
                }

                // Bound the segment index this id would create: a corrupt id (or
                // an abnormally small-but-internally-consistent segment config,
                // see check (5) above) must not drive m_segmentIndex.resize()
                // below to an unbounded size. A valid save allocates ids
                // densely, so segIdx stays tiny; this cap keeps the index
                // allocation bounded across every IDType width, without ever
                // rejecting a legitimate save (see kMaxSegmentsOnLoad's comment
                // above). entitiesPerSegmentShift is already range-validated
                // above, so the shift itself is well-defined.
                if ((static_cast<uint64_t>(id) >> manager->m_config.tableConfig.entitiesPerSegmentShift) >= kMaxSegmentsOnLoad)
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(SerializationError::CorruptedData);
                }

                manager->m_table.SetVersion(id, version);
            }
            
            return Result<std::unique_ptr<EntityManager>, SerializationError>::Ok(std::move(manager));
        }

    private:
        EntityIDStack m_idStack;
        EntityTable m_table;
        Config m_config;
    };
}
