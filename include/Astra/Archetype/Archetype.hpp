#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/Bitmap.hpp"
#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Platform.hpp"
#include "../Core/Result.hpp"
#include "../Serialization/SerializationError.hpp"
#include "../Core/Simd.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "ArchetypeChunkPool.hpp"
#include "EntityLocation.hpp"

namespace Astra
{
    class ArchetypeManager;
    class Registry;

    template<Component... Components>
    ASTRA_NODISCARD ComponentMask MakeComponentMask() noexcept
    {
        ComponentMask mask{};
        ((mask.Set(TypeID<Components>::Value())), ...);
        return mask;
    }
    
    class Archetype
    {
    public:
        explicit Archetype(ComponentMask mask) :
            m_mask(mask),
            m_componentCount(mask.Count()),
            m_entityCount(0),
            m_initialized(false)
        {}
        
        ~Archetype() = default;

    private:
        // Builds m_columnMeta from m_componentDescriptors: excludes tags (desc.size == 0) from
        // columns, sorts columns ascending by id (required by the merge-join used for
        // cross-archetype moves), maps id -> column index, and flags the archetype complex if
        // any column is not trivially copyable. Called once, from Initialize(), immediately
        // after m_componentDescriptors is set.
        void BuildColumnMeta()
        {
            m_columnMeta = ArchetypeColumnMeta{};   // resets idToColumn to -1, columnCount/isComplex to 0/false
            for (const ComponentDescriptor& desc : m_componentDescriptors)
            {
                if (desc.size == 0) continue;       // tag: no storage column (idToColumn stays -1)
                const uint16_t col = m_columnMeta.columnCount++;
                m_columnMeta.columns[col] = { desc.id, static_cast<uint32_t>(desc.size), &desc };
                if (!desc.is_trivially_copyable) m_columnMeta.isComplex = true;
            }
            // Guarantee ascending-by-id column order (merge-join requirement in W3). If
            // m_componentDescriptors is already id-ascending this is a no-op; sort defensively.
            std::sort(m_columnMeta.columns, m_columnMeta.columns + m_columnMeta.columnCount,
                      [](const ArchetypeColumnMeta::ColumnDesc& a, const ArchetypeColumnMeta::ColumnDesc& b)
                      { return a.id < b.id; });
            for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
                m_columnMeta.idToColumn[m_columnMeta.columns[c].id] = static_cast<int16_t>(c);

            // Record which columns opted into ASTRA_ENABLEABLE, in final (post-sort)
            // ordinal order. enableableColumnCount == 0 is the zero-cost early-out the
            // chunk carve + every preservation site takes for non-enableable archetypes.
            m_columnMeta.enableableColumnCount = 0;
            for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
            {
                if (m_columnMeta.columns[c].descriptor->isEnableable)
                    m_columnMeta.enableableColumns[m_columnMeta.enableableColumnCount++] = c;
            }
        }

        // Exact (non-pow2) capacity for a chunk of chunkBytes, under the same
        // conservative alignment-overhead estimate Initialize() uses. Zero-size
        // archetypes (tag-only / the root archetype every registry carries) store
        // their entities in the chunk's side vector rather than in chunk memory:
        // give them chunkBytes/64 slots, which reproduces the legacy 256-at-16KB.
        ASTRA_NODISCARD size_t ComputeCapacityForBytes(size_t chunkBytes) const noexcept
        {
            if (m_perEntitySize == 0)
            {
                return chunkBytes >> 6;
            }
            const size_t usable = chunkBytes > m_alignmentOverhead ? chunkBytes - m_alignmentOverhead : 0;
            size_t cap = usable / m_perEntitySize;

            // Enableable columns carve disabled-bit words INSIDE the chunk (Task 2),
            // which the column-only estimate above does not account for. Archetypes
            // with NO enableable column skip this entirely and keep the exact legacy
            // value (verified: enableableColumnCount == 0 => early return of `cap`).
            // Otherwise shrink `cap` until the precise carve layout (columns + word
            // regions, mirrored by ComputeLayoutBytesForCapacity) fits chunkBytes,
            // using an overshoot-proportional step so even tiny components converge in
            // a couple of iterations, then reclaim any slack the step overshot.
            if (m_columnMeta.enableableColumnCount > 0 && cap > 0)
            {
                size_t layout = ComputeLayoutBytesForCapacity(cap);
                while (cap > 0 && layout > chunkBytes)
                {
                    // Each unit of cap contributes >= m_perEntitySize column bytes, so
                    // this step can never under-shrink into a non-terminating loop.
                    size_t dec = (layout - chunkBytes) / m_perEntitySize;
                    if (dec == 0) dec = 1;
                    cap -= std::min(cap, dec);
                    layout = ComputeLayoutBytesForCapacity(cap);
                }
                while (ComputeLayoutBytesForCapacity(cap + 1) <= chunkBytes)
                    ++cap;
            }
            return cap;
        }

        // Exact byte footprint of a chunk holding `cap` entities, byte-for-byte
        // mirroring ArchetypeChunk::InitializeColumns: cache-line-aligned column
        // blocks followed by 8-byte-aligned disabled-word regions for each enableable
        // column. The single source of truth the capacity math shrinks against so the
        // chunk's own `offset <= m_chunkSize` carve assert can never trip.
        ASTRA_NODISCARD size_t ComputeLayoutBytesForCapacity(size_t cap) const noexcept
        {
            size_t offset = 0;
            for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
            {
                offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                offset += static_cast<size_t>(m_columnMeta.columns[c].stride) * cap;
            }
            const size_t words = (cap + 63) / 64;
            for (uint16_t e = 0; e < m_columnMeta.enableableColumnCount; ++e)
            {
                offset = (offset + 7) & ~size_t(7);
                offset += words * 8;
            }
            return offset;
        }

        // Byte size a chunk must be allocated at to be GUARANTEED to hold exactly
        // `cap` entities once ComputeCapacityForBytes re-derives its capacity, i.e.
        // the conservative column estimate (perEntitySize*cap + alignmentOverhead)
        // PLUS an upper bound on the enableable word regions. Callers that size a
        // chunk from a known capacity (Deserialize) MUST use this rather than the raw
        // column formula, or the word carve would shrink the re-derived capacity below
        // `cap` and overflow. Zero-size archetypes keep the legacy entity-vector sizing.
        ASTRA_NODISCARD size_t ChunkBytesToHold(size_t cap) const noexcept
        {
            if (m_perEntitySize == 0)
                return std::max<size_t>(64, cap << 6);
            size_t bytes = cap * m_perEntitySize + m_alignmentOverhead;
            const uint16_t ec = m_columnMeta.enableableColumnCount;
            if (ec > 0)
            {
                const size_t words = (cap + 63) / 64;
                bytes += static_cast<size_t>(ec) * (words * 8 + 8);   // +8/col: 8-byte-align slack upper bound
            }
            return bytes;
        }

        // Grow-as-populate (Phase 2 Unit C part 2): size each NEW chunk from the
        // archetype's current data footprint. Empty archetype -> minChunkBytes;
        // geometric ramp (~1.5x total per chunk at divisor 2) toward maxChunkBytes.
        // Zero-size archetypes keep the legacy fixed chunk size (entities live in
        // the side vector; there is nothing to ramp).
        //
        // Fit-one-entity floor: the ramp target computed from the data footprint
        // (raw) is never allowed to fall below oneEntityBytes, one entity's own
        // footprint under the same conservative alignment estimate
        // ComputeCapacityForBytes subtracts. Without this floor, the FIRST chunk
        // of any archetype is always sized from raw == 0 (m_totalCapacity == 0),
        // which clamps straight to minChunkBytes (4KB) regardless of how big a
        // single entity actually is -- so any component whose footprint exceeded
        // ~4KB usable made Initialize's refusal path below trip, even though a
        // 512KB-capable pool could easily have fit that one entity. Flooring at
        // oneEntityBytes guarantees a chunk sized to fit >= 1 entity is always
        // attempted first; the min/max clamp still applies around it, so small
        // components keep their 4KB first chunk and big components get a first
        // chunk just big enough (up to maxChunkBytes).
        ASTRA_NODISCARD size_t NextChunkBytes() const noexcept
        {
            if (!m_chunkPool)
                return ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
            if (m_perEntitySize == 0)
                return m_chunkPool->GetChunkSize();
            const size_t dataBytes = m_totalCapacity * m_perEntitySize;
            const size_t raw = dataBytes / m_chunkPool->GetGrowDivisor();
            const size_t oneEntityBytes = m_perEntitySize + m_alignmentOverhead;
            const size_t target = std::max(raw, oneEntityBytes);
            return std::clamp(target, m_chunkPool->GetMinChunkBytes(), m_chunkPool->GetMaxChunkBytes());
        }

        // THE single chunk-creation path: computes the chunk's exact capacity from
        // its byte size, allocates it, and keeps m_totalCapacity in step. Returns
        // nullptr (never throws) when no layout is possible or the pool is out.
        ArchetypeChunk* AppendChunk(size_t chunkBytes)
        {
            const size_t capacity = ComputeCapacityForBytes(chunkBytes);
            if (capacity == 0 || !m_chunkPool) ASTRA_UNLIKELY
            {
                return nullptr;
            }

            auto chunk = m_chunkPool->CreateChunk(capacity, chunkBytes, &m_columnMeta);
            if (!chunk) ASTRA_UNLIKELY
            {
                return nullptr;
            }

            m_totalCapacity += capacity;
            m_chunks.emplace_back(std::move(chunk));
            return m_chunks.back().get();
        }

        // Drops the trailing chunk, keeping m_totalCapacity in step. Every chunk
        // removal must go through here (or recompute the sum) or the running
        // total silently drifts above the real capacity.
        void PopBackChunk()
        {
            ASTRA_ASSERT(!m_chunks.empty(), "PopBackChunk on an empty chunk list");
            const size_t capacity = m_chunks.back()->GetCapacity();
            ASTRA_ASSERT(m_totalCapacity >= capacity, "m_totalCapacity underflow");
            m_totalCapacity -= capacity;
            m_chunks.pop_back();
        }

    public:
        void Initialize(const std::vector<ComponentDescriptor>& componentDescriptors)
        {
            if (m_initialized) ASTRA_UNLIKELY
                return;

            m_componentDescriptors = componentDescriptors;
            // m_columnMeta.descriptor pointers point into m_componentDescriptors above, which is
            // a std::vector set once here and never mutated afterward -- see the stability note
            // on m_columnMeta's declaration.
            BuildColumnMeta();

            size_t perEntitySize = 0;

            // Count non-empty components for alignment overhead estimation
            size_t nonEmptyComponents = 0;
            for (size_t i = 0; i < m_componentDescriptors.size(); ++i)
            {
                if (m_componentDescriptors[i].size == 0)
                {
                    continue;
                }
                perEntitySize += m_componentDescriptors[i].size;
                ++nonEmptyComponents;
            }

            // Estimate alignment overhead: each component array (except first) needs up to
            // (CACHE_LINE_SIZE - 1) bytes of padding for cache-line alignment.
            // Use conservative estimate of (numComponents - 1) * CACHE_LINE_SIZE for padding.
            size_t alignmentOverhead = nonEmptyComponents > 1
                ? (nonEmptyComponents - 1) * CACHE_LINE_SIZE
                : 0;

            m_perEntitySize = perEntitySize;
            m_alignmentOverhead = alignmentOverhead;

            // Grow-as-populate: the archetype is empty (m_totalCapacity == 0), so
            // this always clamps to minChunkBytes for non-zero-size archetypes.
            const size_t chunkBytes = NextChunkBytes();

            // A single entity's footprint must fit in the usable chunk space. The old
            // code clamped the per-chunk count to 1 and proceeded even when
            // perEntitySize exceeded that space, so writing that one entity's
            // components later overflowed past the chunk's actual allocation into
            // neighboring memory. NextChunkBytes() floors its ramp target at one
            // entity's footprint (perEntitySize + alignmentOverhead), so the first
            // chunk it proposes is always big enough to hold one entity UNLESS that
            // footprint itself exceeds the pool's maxChunkBytes ceiling (the clamp's
            // upper bound wins over the floor). This check therefore no longer fires
            // merely because a component is bigger than some particular chunk size
            // in the ramp -- it fires only when there is no chunk size this pool
            // could ever produce, at any point in the ramp, that could hold even one
            // entity. Refuse to initialize instead of clamping-and-overflowing.
            // Leave m_initialized == false and create no chunk; GetOrCreateChunk()
            // and the batch-add paths check m_initialized and refuse to create a
            // chunk that could never legally hold even one entity, so callers
            // degrade gracefully (entity creation succeeds but the entity never
            // gets this component) instead of overflowing.
            if (perEntitySize > 0 && ComputeCapacityForBytes(chunkBytes) == 0) ASTRA_UNLIKELY
            {
                m_initialized = false;
                return;
            }

            m_initialized = true;

            if (!AppendChunk(chunkBytes)) ASTRA_UNLIKELY
            {
                m_initialized = false;
                return;
            }
        }

        EntityLocation AddEntity(Entity entity)
        {
            return AddEntityInternal(entity, [&](auto& chunk, Entity e)
            {
                return chunk->AddEntity(e);
            });
        }
        
        template<typename... Components>
        EntityLocation AddEntityWith(Entity entity, Components&&... components)
        {
            return AddEntityInternal(entity, [&](auto& chunk, Entity e)
            {
                return chunk->AddEntityWithComponents(e, std::forward<Components>(components)...);
            });
        }
        
        std::vector<EntityLocation> AddEntities(std::span<const Entity> entities)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return {};

            std::vector<EntityLocation> locations;
            locations.reserve(count);

            // Calculate and allocate needed chunks upfront. Chunk capacities can
            // differ, so grow by asking each newly appended chunk what it added
            // rather than dividing by a uniform per-chunk count.
            size_t remainingCapacity = GetRemainingCapacity();
            if (count > remainingCapacity) ASTRA_UNLIKELY
            {
                if (!m_initialized) ASTRA_UNLIKELY
                {
                    // See GetOrCreateChunk(): this archetype never found a valid
                    // per-chunk layout and can never hold an entity.
                    return locations;
                }

                // Recompute NextChunkBytes() every iteration (not hoisted): each
                // appended chunk grows m_totalCapacity, so sizes must ramp within
                // this one batch, not just across separate AddEntities calls.
                while (remainingCapacity < count)
                {
                    ArchetypeChunk* chunk = AppendChunk(NextChunkBytes());
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        return locations;
                    }
                    remainingCapacity += chunk->GetCapacity();
                }
            }

            size_t entityIndex = 0;
            size_t chunkIndex = m_firstNonFullChunkIndex;

            while (entityIndex < count && chunkIndex < m_chunks.size()) ASTRA_LIKELY
            {
                auto& chunk = m_chunks[chunkIndex];
                size_t available = chunk->GetCapacity() - chunk->GetCount();

                if (available > 0) ASTRA_LIKELY
                {
                    size_t toAdd = std::min(available, count - entityIndex);
                    size_t startIndex = chunk->GetCount();

                    chunk->BatchAddEntities(entities.subspan(entityIndex, toAdd));

                    for (size_t i = 0; i < toAdd; ++i)
                    {
                        locations.push_back(EntityLocation::Create(chunkIndex, startIndex + i));
                    }

                    entityIndex += toAdd;

                    if (chunk->IsFull() && chunkIndex == m_firstNonFullChunkIndex) ASTRA_UNLIKELY
                    {
                        m_firstNonFullChunkIndex = chunkIndex + 1;
                    }
                }

                ++chunkIndex;
            }

            m_entityCount += entityIndex;
            return locations;
        }

        template<std::invocable<size_t> Generator>
        std::vector<EntityLocation> AddEntitiesWith(std::span<const Entity> entities, Generator&& generator)
        {
            size_t count = entities.size();
            if (count == 0) ASTRA_UNLIKELY
                return {};

            std::vector<EntityLocation> locations;
            locations.reserve(count);

            size_t remainingCapacity = GetRemainingCapacity();
            if (count > remainingCapacity) ASTRA_UNLIKELY
            {
                if (!m_initialized) ASTRA_UNLIKELY
                {
                    // See GetOrCreateChunk(): this archetype never found a valid
                    // per-chunk layout and can never hold an entity.
                    return locations;
                }

                // Recompute NextChunkBytes() every iteration -- see AddEntities.
                while (remainingCapacity < count)
                {
                    ArchetypeChunk* chunk = AppendChunk(NextChunkBytes());
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        return locations;
                    }
                    remainingCapacity += chunk->GetCapacity();
                }
            }

            // Deduce the generator's component tuple ONCE (the generator is never
            // called here -- the contract is exactly one invocation per entity, in
            // index order, inside the run loop below).
            using TupleType = std::decay_t<std::invoke_result_t<Generator&, size_t>>;
            constexpr size_t tupleSize = std::tuple_size_v<TupleType>;

            const ArchetypeColumnMeta& cm = m_columnMeta;

            // Uncovered storage columns: those the generator tuple does NOT produce.
            // On this path the archetype was built from the tuple's exact component set,
            // so this is normally empty -- computed ONCE (not per entity) and kept as a
            // defensive guard so a column the tuple misses is still default-constructed
            // rather than left as raw (zeroed) bytes.
            uint16_t uncovered[MAX_COMPONENTS];
            uint16_t uncoveredCount = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                for (uint16_t c = 0; c < cm.columnCount; ++c)
                {
                    const ComponentID id = cm.columns[c].id;
                    const bool covered = ((TypeID<std::decay_t<std::tuple_element_t<Is, TupleType>>>::Value() == id) || ...);
                    if (!covered) ASTRA_UNLIKELY
                        uncovered[uncoveredCount++] = c;
                }
            }(std::make_index_sequence<tupleSize>{});

            // Chunk-run loop: claim a contiguous run of slots in the current non-full
            // chunk, hoist that chunk's typed column bases ONCE for the run, then
            // move-construct each entity's components directly through those typed
            // pointers (no idToColumn resolution, no per-element fn-ptr indirection).
            size_t produced = 0;
            while (produced < count)
            {
                auto [chunkIndex, wasCreated] = GetOrCreateChunk();
                if (chunkIndex == INVALID_CHUNK_INDEX) ASTRA_UNLIKELY
                {
                    break;
                }

                ArchetypeChunk* chunk = m_chunks[chunkIndex].get();
                const size_t startSlot = chunk->GetCount();
                const size_t capacity  = chunk->GetCapacity();
                const size_t runLen    = std::min(count - produced, capacity - startSlot);
                ASTRA_ASSERT(runLen > 0 && startSlot + runLen <= capacity,
                             "AddEntitiesWith: run exceeds chunk capacity");

                // One bulk entity-handle append + one count bump for the whole run.
                std::vector<Entity>& entityVec = chunk->GetEntities();
                entityVec.insert(entityVec.end(),
                                 entities.begin() + produced,
                                 entities.begin() + produced + runLen);
                chunk->SetCount(startSlot + runLen);

                // Hoist the run's typed column bases ONCE. Empty (tag) components yield
                // a null base -- never dereferenced; their placement-new is elided below.
                auto bases = [&]<std::size_t... Is>(std::index_sequence<Is...>)
                {
                    return std::tuple{ chunk->template GetComponentArray<std::decay_t<std::tuple_element_t<Is, TupleType>>>()... };
                }(std::make_index_sequence<tupleSize>{});

                for (size_t r = 0; r < runLen; ++r)
                {
                    const size_t slot = startSlot + r;
                    ASTRA_ASSERT(slot < capacity, "AddEntitiesWith: slot out of chunk capacity");

                    auto componentTuple = generator(produced + r);   // exactly once, in order

                    [&]<std::size_t... Is>(std::index_sequence<Is...>)
                    {
                        // Move-construct each covered element straight into its slot
                        // through the hoisted typed base (ConstructComponentAt semantics
                        // without the id->column lookup).
                        (([&]
                        {
                            using ElemT = std::decay_t<std::tuple_element_t<Is, TupleType>>;
                            if constexpr (!std::is_empty_v<ElemT>)
                            {
                                ::new (static_cast<void*>(std::get<Is>(bases) + slot))
                                    ElemT(std::get<Is>(std::move(componentTuple)));
                            }
                        }()), ...);
                    }(std::make_index_sequence<tupleSize>{});

                    // Defensive: default-construct any column the tuple did not cover.
                    for (uint16_t u = 0; u < uncoveredCount; ++u)
                    {
                        const uint16_t c = uncovered[u];
                        cm.columns[c].descriptor->DefaultConstruct(chunk->GetColumnPointer(c, slot));
                    }

                    locations.push_back(EntityLocation::Create(chunkIndex, slot));
                }

                produced += runLen;
                m_entityCount += runLen;

                // Chunk transition: advance the non-full cursor past a now-full chunk
                // (IsFull => chunkIndex + 1), matching AddEntities' convention.
                if (chunk->IsFull() && chunkIndex == m_firstNonFullChunkIndex) ASTRA_UNLIKELY
                {
                    m_firstNonFullChunkIndex = chunkIndex + 1;
                }
            }

            return locations;
        }

        std::optional<Entity> RemoveEntity(EntityLocation location)
        {
            size_t chunkIndex = location.GetChunkIndex();
            size_t entityIndex = location.GetEntityIndex();

            ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");

            // Remove from chunk - chunk handles the swap-and-pop
            auto movedEntity = m_chunks[chunkIndex]->RemoveEntity(entityIndex);

            --m_entityCount;

            // Update first non-full chunk index if this chunk now has space
            if (chunkIndex < m_firstNonFullChunkIndex && !m_chunks[chunkIndex]->IsFull()) ASTRA_UNLIKELY
            {
                m_firstNonFullChunkIndex = chunkIndex;
            }

            if (chunkIndex == m_chunks.size() - 1 && chunkIndex > 0 && m_chunks[chunkIndex]->IsEmpty()) ASTRA_UNLIKELY
            {
                PopBackChunk();

                if (m_firstNonFullChunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                {
                    m_firstNonFullChunkIndex = m_chunks.size() > 0 ? m_chunks.size() - 1 : 0;
                }
            }

            return movedEntity;
        }

        std::vector<std::pair<Entity, EntityLocation>> RemoveEntities(std::span<const EntityLocation> locations, bool deferChunkCleanup = false)
        {
            if (locations.empty()) ASTRA_UNLIKELY
                return {};

            std::vector<std::pair<Entity, EntityLocation>> movedEntities;
            movedEntities.reserve(locations.size());

            std::vector<EntityLocation> sortedLocations(locations.begin(), locations.end());
            std::sort(sortedLocations.begin(), sortedLocations.end(), std::greater<EntityLocation>());

            size_t lowestModifiedChunk = std::numeric_limits<size_t>::max();

            for (EntityLocation location : sortedLocations)
            {
                size_t chunkIndex = location.GetChunkIndex();
                size_t entityIndex = location.GetEntityIndex();

                if (chunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                {
                    continue;
                }

                if (entityIndex >= m_chunks[chunkIndex]->GetCount()) ASTRA_UNLIKELY
                {
                    continue;  // Stale or duplicated location
                }

                auto movedEntity = m_chunks[chunkIndex]->RemoveEntity(entityIndex);
                if (movedEntity) ASTRA_LIKELY
                {
                    EntityLocation newEntityLocation = EntityLocation::Create(chunkIndex, entityIndex);
                    movedEntities.emplace_back(*movedEntity, newEntityLocation);
                }

                --m_entityCount;
                lowestModifiedChunk = std::min(lowestModifiedChunk, chunkIndex);
            }

            if (lowestModifiedChunk < m_firstNonFullChunkIndex && lowestModifiedChunk < m_chunks.size()) ASTRA_UNLIKELY
            {
                if (!m_chunks[lowestModifiedChunk]->IsFull()) ASTRA_LIKELY
                {
                    m_firstNonFullChunkIndex = lowestModifiedChunk;
                }
            }

            if (!deferChunkCleanup) ASTRA_LIKELY
            {
                while (!m_chunks.empty() && m_chunks.back()->IsEmpty() && m_chunks.size() > 1) ASTRA_UNLIKELY
                {
                    PopBackChunk();
                }
            }

            if (m_firstNonFullChunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
            {
                m_firstNonFullChunkIndex = m_chunks.size() > 0 ? m_chunks.size() - 1 : 0;
            }

            return movedEntities;
        }

        ASTRA_NODISCARD Entity GetEntity(EntityLocation location) const 
        { 
            size_t chunkIndex = location.GetChunkIndex();
            size_t entityIndex = location.GetEntityIndex();
            ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");
            return m_chunks[chunkIndex]->GetEntity(entityIndex);
        }

        void MoveEntityFrom(EntityLocation dstEntityLocation, Archetype& srcArchetype, EntityLocation srcEntityLocation)
        {
            size_t dstChunkIndex = dstEntityLocation.GetChunkIndex();
            size_t dstEntityIndex = dstEntityLocation.GetEntityIndex();
            size_t srcChunkIndex = srcEntityLocation.GetChunkIndex();
            size_t srcEntityIndex = srcEntityLocation.GetEntityIndex();

            ASTRA_ASSERT(dstChunkIndex < m_chunks.size(), "Destination chunk index out of bounds");
            ASTRA_ASSERT(srcChunkIndex < srcArchetype.m_chunks.size(), "Source chunk index out of bounds");

            auto& dstChunk = m_chunks[dstChunkIndex];
            auto& srcChunk = srcArchetype.m_chunks[srcChunkIndex];
            const ArchetypeColumnMeta& dm = m_columnMeta;
            const ArchetypeColumnMeta& sm = srcArchetype.m_columnMeta;

            // Merge-join over the two archetypes' storage columns -- both sorted ascending by id
            // (BuildColumnMeta guarantees this). For each destination column: a matching source
            // column is moved (std::memcpy for a trivially-copyable component, MoveConstruct
            // otherwise); a destination-only column is default-constructed. Source-only columns
            // (present in src, absent from dst) are simply advanced past -- their slots are
            // destructed by the caller's source-entity removal, so this function only ever
            // CONSTRUCTS into the destination (never destructs the source).
            //
            // The per-column is_trivially_copyable check is the CORRECTNESS GATE, not a mere
            // optimization: memcpy of a move-only / non-trivially-relocatable component (e.g. one
            // owning a unique_ptr, or with lifetime-counting invariants) would skip its move ctor
            // and corrupt it. It must NOT be widened to a blanket memcpy.
            uint16_t a = 0, b = 0;
            while (a < dm.columnCount)
            {
                const ComponentID dId = dm.columns[a].id;
                // dstEntityIndex is < the dst chunk's count here: AllocateEntitySlot (invoked by
                // MoveEntityInternal before this runs) already bumped the destination slot's
                // count, so the count-asserting GetColumnPointer is safe on the destination.
                void* dstPtr = dstChunk->GetColumnPointer(a, dstEntityIndex);

                // Advance src past any ids strictly less than dId (src-only columns: dropped).
                while (b < sm.columnCount && sm.columns[b].id < dId) ++b;

                if (b < sm.columnCount && sm.columns[b].id == dId) ASTRA_LIKELY   // matched: move src -> dst
                {
                    void* srcPtr = srcChunk->GetColumnPointer(b, srcEntityIndex);
                    const ComponentDescriptor& desc = *dm.columns[a].descriptor;
                    if (desc.is_trivially_copyable) std::memcpy(dstPtr, srcPtr, dm.columns[a].stride);
                    else                            desc.MoveConstruct(dstPtr, srcPtr);
                    // Disabled-bit carry (Task 2): shared enableable column keeps its
                    // state. dst slot is freshly allocated (born enabled); the src bit
                    // is cleared by the caller's source swap-remove. Both column a (dst)
                    // and b (src) share id dId => same enableable-ness.
                    if (desc.isEnableable) ASTRA_UNLIKELY
                        dstChunk->SetDisabled(a, dstEntityIndex, srcChunk->IsDisabled(b, srcEntityIndex));
                    ++b;
                }
                else ASTRA_UNLIKELY                                              // dst-only: default-construct
                {
                    dm.columns[a].descriptor->DefaultConstruct(dstPtr);
                }
                ++a;
            }
        }

        template<Component T>
        ASTRA_NODISCARD T* GetComponent(EntityLocation location)
        {
            ComponentID id = TypeID<T>::Value();
            if (!m_mask.Test(id)) ASTRA_UNLIKELY
                return nullptr;

            size_t chunkIndex = location.GetChunkIndex();
            size_t entityIndex = location.GetEntityIndex();

            ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");
            ASTRA_ASSERT(entityIndex < m_chunks[chunkIndex]->GetCount(), "Entity index out of bounds");

            return m_chunks[chunkIndex]->GetComponent<T>(entityIndex);
        }

        template<typename T>
        void SetComponent(EntityLocation location, T&& value)
        {
            static_assert(Component<std::decay_t<T>>, "T must be a Component");

            std::decay_t<T>* ptr = GetComponent<std::decay_t<T>>(location);
            ASTRA_ASSERT(ptr != nullptr, "Component pointer is null");
            *ptr = std::forward<T>(value);
        }

        template<Component T>
        void SetComponents(std::span<const EntityLocation> locations, const T& value)
        {
            if (locations.empty())
                return;

            // Make a local copy to avoid any span/reference issues
            std::vector<EntityLocation> locationsCopy(locations.begin(), locations.end());

            // Group by chunk for efficient processing
            FlatMap<size_t, std::vector<size_t>> chunkBatches;
            chunkBatches.Reserve(8);  // Pre-allocate to reduce rehashing

            for (const auto& location : locationsCopy)
            {
                size_t chunkIndex = location.GetChunkIndex();
                size_t entityIndex = location.GetEntityIndex();

                // Validate before inserting
                if (chunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                    continue;

                chunkBatches[chunkIndex].push_back(entityIndex);
            }

            // Batch construct component in each chunk
            for (auto& [chunkIndex, indices] : chunkBatches)
            {
                if (!ASTRA_ENSURE(chunkIndex < m_chunks.size(), "Chunk index out of bounds")) ASTRA_UNLIKELY
                    continue;  // Skip invalid chunk indices
                m_chunks[chunkIndex]->BatchConstructComponent<T>(indices, value);
            }
        }

        template<Component C>
        ASTRA_NODISCARD bool HasComponent() const { return m_mask.Test(TypeID<C>::Value()); }
        ASTRA_NODISCARD bool HasComponent(ComponentID id) const { return m_mask.Test(id); }

        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            if (m_entityCount == 0 || m_chunks.empty()) ASTRA_UNLIKELY
                return;
            
            const size_t numChunks = m_chunks.size();
            
            for (size_t i = 0; i < numChunks; ++i)
            {
                auto& chunk = m_chunks[i];
                const size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                {
                    continue;
                }
                
                // Prefetch next chunk's data while processing current chunk
                if (i + 1 < numChunks) ASTRA_LIKELY
                {
                    auto& nextChunk = m_chunks[i + 1];
                    if (nextChunk->GetCount() > 0)
                    {
                        // Prefetch the entity array and first component array of next chunk
                        Simd::Ops::PrefetchT0(&nextChunk->GetEntities()[0]);
                        if constexpr (sizeof...(Components) > 0)
                        {
                            using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
                            Simd::Ops::PrefetchT0(nextChunk->GetComponentArray<FirstComponent>());
                        }
                    }
                }
                
                ForEachImpl<Components...>(chunk.get(), count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
            }
        }
        
        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEachChunk(size_t chunkIndex, Func&& func)
        {
            if (chunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                return;
                
            auto& chunk = m_chunks[chunkIndex];
            const size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            ForEachImpl<Components...>(chunk.get(), count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }

        void EnsureCapacity(size_t additionalCount)
        {
            size_t required = m_entityCount + additionalCount;

            if (required > m_totalCapacity) ASTRA_UNLIKELY
            {
                // Vector-reserve estimate only: chunk capacities may differ under
                // grow-as-populate ramping, so this sizes off the NEXT chunk's
                // projected capacity and lets the actual growth paths append
                // however many are really needed.
                const size_t perChunk = std::max<size_t>(1, ComputeCapacityForBytes(NextChunkBytes()));
                const size_t neededChunks = (required - m_totalCapacity + perChunk - 1) / perChunk;
                m_chunks.reserve(m_chunks.size() + neededChunks);
            }
        }

        ASTRA_NODISCARD size_t GetRemainingCapacity() const
        {
            if (m_chunks.empty()) ASTRA_UNLIKELY
                return 0;

            size_t remaining = 0;
            for (size_t i = m_firstNonFullChunkIndex; i < m_chunks.size(); ++i)
            {
                remaining += m_chunks[i]->GetCapacity() - m_chunks[i]->GetCount();
            }
            return remaining;
        }

        ASTRA_NODISCARD float GetFragmentationLevel() const noexcept
        {
            if (m_chunks.empty() || m_entityCount == 0)
                return 0.0f;

            // Fill-based: 0 == perfectly packed, 1 == all wasted space. With
            // per-chunk capacities there is no single "optimal chunk count" to
            // compare against, and unused slots are what defragmentation actually
            // reclaims -- so measure them directly.
            if (m_totalCapacity == 0) ASTRA_UNLIKELY
                return 0.0f;

            return 1.0f - static_cast<float>(m_entityCount) / static_cast<float>(m_totalCapacity);
        }
        
        void Serialize(BinaryWriter& writer) const
        {
            // Write archetype metadata - serialize the bitmap's words
            for (size_t i = 0; i < ComponentMask::WORD_COUNT; ++i)
            {
                writer(m_mask.Data()[i]);
            }
            writer(static_cast<uint64_t>(m_entityCount));

            // Field layout is unchanged, but its MEANING is now the maximum
            // per-chunk entity count rather than a uniform per-chunk capacity:
            // chunks no longer share a capacity, and the bound is all the reader
            // ever uses this field for (it validates each chunkEntityCount
            // against it, then sizes each chunk to an exact fit). Floored at 1 so
            // an all-empty archetype still round-trips through the reader's
            // `chunkEntityCount > entitiesPerChunk` guard.
            uint64_t maxChunkEntityCount = 1;
            for (const auto& chunk : m_chunks)
            {
                if (chunk) ASTRA_LIKELY
                    maxChunkEntityCount = std::max(maxChunkEntityCount, static_cast<uint64_t>(chunk->GetCount()));
            }
            writer(maxChunkEntityCount);

            writer(static_cast<uint32_t>(m_chunks.size()));

            // Write component descriptors
            writer(static_cast<uint32_t>(m_componentDescriptors.size()));
            for (const auto& desc : m_componentDescriptors)
            {
                writer(desc.hash);  // Write stable hash instead of runtime ID
                writer(static_cast<uint64_t>(desc.size));
                writer(static_cast<uint64_t>(desc.alignment));
                writer(desc.version);

                // IM-9 (format v4): record whether this component carries a per-chunk
                // disabled-bit section (written below iff desc.isEnableable). Recording
                // presence EXPLICITLY makes it a property of the ARCHIVE rather than of
                // the loading build -- so a component whose ASTRA_ENABLEABLE status
                // differs between the saving and loading builds no longer desyncs the
                // reader from the byte stream. Written unconditionally in the current
                // (v4) format; the reader consumes it iff the archive is v4+.
                writer(static_cast<uint8_t>(desc.isEnableable ? 1 : 0));
            }
            
            // Write each chunk's data
            for (const auto& chunk : m_chunks)
            {
                if (!chunk) continue;
                
                // Write chunk metadata
                size_t chunkEntityCount = chunk->GetCount();
                writer(static_cast<uint32_t>(chunkEntityCount));
                
                // Write entities array
                const auto& entities = chunk->GetEntities();
                for (size_t i = 0; i < chunkEntityCount; ++i)
                {
                    writer(entities[i]);
                }
                
                // Write component arrays (SOA layout)
                for (const auto& desc : m_componentDescriptors)
                {
                    void* componentArray = chunk->GetComponentArrayByID(desc.id);
                    if (!componentArray) continue;

                    // (componentArray null / tag columns already skipped by the
                    //  `if (!componentArray) continue;` above)
                    if (writer.GetCompressionMode() == CompressionMode::LZ4)
                    {
                        // Per-column block (orthogonal to versioning): serialize the
                        // column (data + disabled section) into a sub-buffer with
                        // checksum off, then compress that buffer as ONE block into
                        // the main stream. WriteCompressedBlock stores raw when below
                        // threshold / incompressible, so tiny columns never inflate.
                        // The sub-writer's memory ctor defaults to None mode, so the
                        // inner SerializeColumn writes plain (uncompressed) bytes;
                        // compression happens exactly once, here in the outer stream.
                        std::vector<std::byte> colBuf;
                        {
                            BinaryWriter sub(colBuf);
                            sub.SetChecksumEnabled(false);
                            SerializeColumn(sub, chunk.get(), desc, chunkEntityCount);
                            sub.Flush();
                        }
                        writer.WriteCompressedBlock(colBuf.data(), colBuf.size());
                    }
                    else
                    {
                        // None mode: byte-identical to the pre-compression format --
                        // SerializeColumn writes straight into the main stream.
                        SerializeColumn(writer, chunk.get(), desc, chunkEntityCount);
                    }
                }
            }
        }
        
        static Result<std::unique_ptr<Archetype>, SerializationError> Deserialize(BinaryReader& reader, const std::vector<ComponentDescriptor>& registryDescriptors, ArchetypeChunkPool* componentPool = nullptr)
        {
            using ResultType = Result<std::unique_ptr<Archetype>, SerializationError>;

            // Read archetype metadata - deserialize the bitmap's words
            ComponentMask mask;
            for (size_t i = 0; i < ComponentMask::WORD_COUNT; ++i)
            {
                reader(mask.Data()[i]);
            }

            if (reader.HasError())
            {
                return ResultType::Err(reader.GetError());
            }

            uint64_t entityCount;
            uint64_t entitiesPerChunk;
            uint32_t chunkCount;
            reader(entityCount);
            reader(entitiesPerChunk);
            reader(chunkCount);

            if (reader.HasError())
            {
                return ResultType::Err(reader.GetError());
            }

            // Bound chunkCount against the remaining buffer via the reader's
            // width-agnostic count-bound helper (chunkCount is a uint32_t on disk;
            // Archetype::Serialize writes it via writer(static_cast<uint32_t>(...))).
            // Each chunk's only guaranteed fixed prefix is its 4-byte chunkEntityCount
            // field; everything after it is variable-length.
            if (reader.CountExceedsRemaining(chunkCount, sizeof(uint32_t)))
            {
                return ResultType::Err(SerializationError::CorruptedData);
            }

            // Read component descriptors
            uint32_t descriptorCount;
            reader(descriptorCount);

            if (reader.HasError())
            {
                return ResultType::Err(reader.GetError());
            }

            // Same bound, sized to a descriptor entry's fixed on-disk fields:
            // hash(8) + size(8) + alignment(8) + version(4) = 28 bytes, plus a 1-byte
            // has-disabled-section flag (IM-9) present only from format v4 onward,
            // written unconditionally by Archetype::Serialize for every descriptor.
            const uint64_t kMinBytesPerDescriptor = sizeof(uint64_t) * 3 + sizeof(uint32_t)
                + (reader.GetVersion() >= 4 ? sizeof(uint8_t) : 0);
            if (reader.CountExceedsRemaining(descriptorCount, kMinBytesPerDescriptor))
            {
                return ResultType::Err(SerializationError::CorruptedData);
            }

            std::vector<ComponentDescriptor> descriptors;
            descriptors.reserve(descriptorCount);

            // IM-9: per-descriptor "archive carries a disabled-bit section for this
            // column" flags, kept in lockstep with `descriptors` (both are pushed
            // together only when the hash resolves). Drives section CONSUMPTION on the
            // chunk-read path below so it depends on the archive, not the local build.
            std::vector<uint8_t> diskHasDisabledSection;
            diskHasDisabledSection.reserve(descriptorCount);

            for (uint32_t i = 0; i < descriptorCount; ++i)
            {
                uint64_t hash;
                uint64_t size, alignment;
                uint32_t version;
                reader(hash)(size)(alignment)(version);

                // IM-9: format v4+ records a per-descriptor has-disabled-section flag
                // right after version. Pre-v4 archives have no such byte and never wrote
                // a disabled section, so it defaults to 0 (absent) for them.
                uint8_t hasDisabledSection = 0;
                if (reader.GetVersion() >= 4)
                {
                    reader(hasDisabledSection);
                }

                if (reader.HasError())
                {
                    return ResultType::Err(reader.GetError());
                }

                // Find matching descriptor from registry by hash
                auto it = std::find_if(registryDescriptors.begin(), registryDescriptors.end(), [hash](const auto& desc) { return desc.hash == hash; });

                if (it != registryDescriptors.end())
                {
                    descriptors.push_back(*it);
                    diskHasDisabledSection.push_back(hasDisabledSection);
                }
                else
                {
                    // Component not registered - cannot deserialize
                    // The component with this hash needs to be registered before deserialization
                    return ResultType::Err(SerializationError::UnknownComponent);
                }
            }

            // CR-4: the on-disk ComponentMask words (read above into `mask`) hold the
            // SAVING run's ComponentID bit positions. ComponentIDs are assigned per-run
            // and are NOT stable across processes, so constructing the archetype from
            // those raw bits desyncs Has<T>/queries/the archetype-map key when the
            // archive is loaded in a different run. The per-descriptor block, by
            // contrast, is keyed on the stable TypeID::Hash() and was just resolved to
            // THIS run's descriptors (each carrying this run's desc.id, tags included).
            // Rebuild the mask from those resolved ids so it is correct in the loading
            // run; the raw disk words are consumed off the wire but their VALUES are not
            // trusted.
            ComponentMask localMask;
            for (const auto& d : descriptors)
            {
                localMask.Set(d.id);
            }

            // Cross-process integrity check: the saved mask's popcount is run-
            // independent (it counts how many components the archetype has, not which
            // ids), so it must equal the descriptor count regardless of process. A
            // mismatch means the mask half and the descriptor half of the record
            // disagree -> corrupt/crafted input.
            if (mask.Count() != descriptors.size())
            {
                return ResultType::Err(SerializationError::CorruptedData);
            }

            // Validate that the saved per-chunk layout fits the pool we will
            // allocate from — a save produced with a larger chunk than this pool
            // can ever produce must fail cleanly instead of overflowing chunk
            // memory. Bound against the pool's grow-as-populate CEILING
            // (maxChunkBytes), not its legacy fixed chunkSize: chunks are written
            // at whatever size the archetype had ramped to at save time (up to
            // maxChunkBytes), so a save with large ramped chunks must still load;
            // old 16KB-era saves still pass, since 16KB < the 512KB default cap.
            {
                size_t perEntitySize = 0;
                size_t nonEmptyComponents = 0;
                for (const auto& d : descriptors)
                {
                    if (d.size == 0) continue;
                    perEntitySize += d.size;
                    ++nonEmptyComponents;
                }
                size_t alignmentOverhead = nonEmptyComponents > 1
                    ? (nonEmptyComponents - 1) * CACHE_LINE_SIZE
                    : 0;
                size_t poolChunkSize = componentPool ? componentPool->GetMaxChunkBytes()
                                                     : ArchetypeChunkPool::DEFAULT_MAX_CHUNK_BYTES;
                if (static_cast<size_t>(entitiesPerChunk) * perEntitySize + alignmentOverhead > poolChunkSize)
                {
                    return ResultType::Err(SerializationError::SizeMismatch);
                }

                // The guard above is vacuous for a zero-component (or all-empty-
                // component) archetype: perEntitySize == 0 makes the product 0
                // regardless of entitiesPerChunk, so it never rejects anything.
                // Every registry always carries such a root archetype, so this is
                // not an edge case. Chunk::Chunk unconditionally does
                // m_entities.reserve(entitiesPerChunk) for the entity-handle array,
                // independent of component count, so bound entitiesPerChunk against
                // that array's own footprint fitting in the chunk regardless of
                // perEntitySize -- otherwise a corrupted entitiesPerChunk drives an
                // unbounded reserve() that throws std::bad_alloc uncaught out of
                // Registry::Load.
                if (static_cast<uint64_t>(entitiesPerChunk) > static_cast<uint64_t>(poolChunkSize) / std::max<size_t>(1, sizeof(Entity)))
                {
                    return ResultType::Err(SerializationError::SizeMismatch);
                }
            }

            // Create new archetype from the run-local mask rebuilt above (CR-4), not
            // the untrusted raw disk mask.
            auto archetype = std::make_unique<Archetype>(localMask);
            archetype->m_chunkPool = componentPool;
            archetype->Initialize(descriptors);

            if (!archetype->IsInitialized())
            {
                return ResultType::Err(SerializationError::CorruptedData);
            }
            
            // Clear the pre-allocated chunk. m_totalCapacity is the running sum
            // over m_chunks, so it has to be reset alongside it.
            archetype->m_chunks.clear();
            archetype->m_totalCapacity = 0;
            archetype->m_entityCount = 0;

            // Read each chunk's data
            for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            {
                uint32_t chunkEntityCount;
                reader(chunkEntityCount);

                if (reader.HasError())
                {
                    return ResultType::Err(reader.GetError());
                }

                // A chunk cannot legally hold more entities than the pool chunk it
                // will be allocated from (entitiesPerChunk, already validated above
                // to fit the pool's chunk size). Without this guard, a corrupted or
                // crafted chunkEntityCount drives the entity-add loop and the
                // component-array reads below into writing past the end of the
                // chunk's fixed-size heap arena -- and AddEntity's own capacity
                // check is an ASTRA_ASSERT, which compiles out in Release/Dist,
                // leaving the overflow live in shipping builds.
                if (chunkEntityCount > entitiesPerChunk)
                {
                    return ResultType::Err(SerializationError::CorruptedData);
                }

                // Create the chunk at an EXACT fit for the entities it actually
                // carries, through the archetype so m_totalCapacity stays correct
                // (m_chunks was cleared above). This is location-safe: entity slots
                // stay [0, count) within each chunk, so every serialized
                // EntityRecord (chunkIndex, entityIndex) still resolves identically.
                // The reconstructed archetype's Initialize() above already built its
                // m_columnMeta (via BuildColumnMeta) from `descriptors`, and its
                // m_componentDescriptors (which the meta's descriptor pointers
                // reference) is set once and never reassigned, so that pointer is
                // stable for the life of every chunk created here.
                const size_t capacity = std::max<size_t>(1, chunkEntityCount);
                // ChunkBytesToHold (not the raw column formula) so that when an
                // enableable archetype's chunk re-derives its capacity via
                // ComputeCapacityForBytes, the carved disabled-word regions do not
                // shrink it below `capacity` and overflow. Reduces to the legacy
                // formula exactly when there are no enableable columns.
                const size_t chunkBytes = archetype->ChunkBytesToHold(capacity);

                ArchetypeChunk* chunk = archetype->AppendChunk(chunkBytes);
                if (!chunk)
                {
                    // Out of memory (or no valid layout) - cannot continue
                    return ResultType::Err(SerializationError::OutOfMemory);
                }

                // Read entities array
                for (uint32_t i = 0; i < chunkEntityCount; ++i)
                {
                    Entity entity;
                    reader(entity);
                    chunk->AddEntity(entity);
                }

                if (reader.HasError())
                {
                    return ResultType::Err(reader.GetError());
                }

                // Read component arrays (SOA layout). Indexed (not range-for) so each
                // descriptor's disk has-disabled-section flag can be looked up by the
                // same ordinal (IM-9).
                for (size_t di = 0; di < descriptors.size(); ++di)
                {
                    const ComponentDescriptor& desc = descriptors[di];
                    void* componentArray = chunk->GetComponentArrayByID(desc.id);
                    if (!componentArray) continue;

                    // Mirror of the write path (Serialize above): LZ4 archives wrap
                    // each column in a compressed block, so read+decompress it into a
                    // memory sub-reader (checksum off, matching the writer) and let
                    // DeserializeColumn consume the plain bytes. None archives feed
                    // the main reader directly -- byte-identical to the old format.
                    ResultType colResult = ResultType::Ok(nullptr);
                    // Compressed columns exist only in v5+ archives. A v<=4 file predates
                    // per-column compression, so its columns are always plain -- even when
                    // its header carries a CompressionMode::LZ4 flag (the flag was written
                    // but never acted on before v5, so last-build v4 "LZ4" files hold plain
                    // columns). Gate on version AND mode so those legacy files read as raw
                    // (spec 4), never misparsed through ReadCompressedBlock.
                    if (reader.GetVersion() >= 5 && reader.GetCompressionMode() == CompressionMode::LZ4)
                    {
                        auto blk = reader.ReadCompressedBlock();
                        if (blk.IsErr())
                            return ResultType::Err(SerializationError::CorruptedData);
                        const auto& bytes = *blk.GetValue();   // std::vector<uint8_t>
                        BinaryReader sub(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                        sub.SetChecksumEnabled(false);
                        colResult = DeserializeColumn(sub, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      diskHasDisabledSection[di]);
                    }
                    else
                    {
                        colResult = DeserializeColumn(reader, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      diskHasDisabledSection[di]);
                    }
                    if (colResult.IsErr())
                        return colResult;
                }

                // AppendChunk already installed the chunk in archetype->m_chunks.
            }

            archetype->m_entityCount = static_cast<size_t>(entityCount);

            return ResultType::Ok(std::move(archetype));
        }
        
        // Rebuild-style compaction (Phase 2 Unit D): repack every live entity
        // into fresh chunks sized for the CURRENT live count (the study formula
        // applied directly -- N is known here), free all old chunks (TLSF
        // coalesces them), and report EVERY entity's new location so the caller
        // rewrites all records. Column moves memcpy whole runs for trivially
        // copyable columns and MoveConstruct+Destruct element-wise otherwise.
        //
        // Replaces the old ShouldCoalesce/CoalesceChunks pair, which erased empty
        // middle chunks from m_chunks -- shifting every later chunk's index -- but
        // reported new locations only for the entities it moved, leaving entities
        // in the shifted chunks with stale chunkIndex records (a since-confirmed
        // out-of-bounds/aliasing defect). Reporting every entity's location makes
        // that whole class of bug impossible.
        std::pair<size_t, std::vector<std::pair<Entity, EntityLocation>>> CompactChunks()
        {
            std::vector<std::pair<Entity, EntityLocation>> newLocations;
            if (m_chunks.size() <= 1 || m_entityCount == 0)
                return {0, std::move(newLocations)};

            const size_t oldChunkCount = m_chunks.size();
            // Fit-one-entity floor (mirrors NextChunkBytes): without it, a fragmented
            // low-live-count archetype whose per-entity footprint exceeds the raw
            // liveBytes/divisor value would compute a targetBytes too small to hold
            // even one entity, so ComputeCapacityForBytes(targetBytes) == 0 below and
            // compaction aborts every time it is tried -- a silent permanent no-op for
            // that archetype. Flooring the pre-clamp value at oneEntityBytes (then
            // clamping min/max, same order NextChunkBytes uses -- clamping oneEntityBytes
            // directly could otherwise present std::clamp with lo > hi when
            // oneEntityBytes exceeds maxChunkBytes) guarantees a chunk sized to fit
            // >= 1 entity is always attempted; components bigger than the pool's cap
            // still legitimately hit capacity == 0 below and abort.
            const size_t targetBytes = [this]
            {
                if (!m_chunkPool || m_perEntitySize == 0)
                    return m_chunkPool ? m_chunkPool->GetChunkSize() : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
                const size_t liveBytes = m_entityCount * m_perEntitySize;
                const size_t raw = liveBytes / m_chunkPool->GetGrowDivisor();
                const size_t oneEntityBytes = m_perEntitySize + m_alignmentOverhead;
                const size_t target = std::max(raw, oneEntityBytes);
                return std::clamp(target, m_chunkPool->GetMinChunkBytes(), m_chunkPool->GetMaxChunkBytes());
            }();

            // Build the new chunk list on the side; on ANY allocation failure,
            // abort untouched (compaction is an optimization, not an obligation).
            std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> newChunks;
            const size_t capacity = ComputeCapacityForBytes(targetBytes);
            if (capacity == 0) ASTRA_UNLIKELY
                return {0, std::move(newLocations)};
            const size_t chunkCountNeeded = (m_entityCount + capacity - 1) / capacity;
            newChunks.reserve(chunkCountNeeded);
            for (size_t i = 0; i < chunkCountNeeded; ++i)
            {
                auto chunk = m_chunkPool->CreateChunk(capacity, targetBytes, &m_columnMeta);
                if (!chunk) ASTRA_UNLIKELY
                    return {0, std::move(newLocations)};   // old chunks untouched
                newChunks.emplace_back(std::move(chunk));
            }

            newLocations.reserve(m_entityCount);
            size_t dstChunk = 0, dstIndex = 0;
            for (auto& src : m_chunks)
            {
                const size_t srcCount = src->GetCount();
                size_t srcIndex = 0;
                while (srcIndex < srcCount)
                {
                    if (dstIndex == capacity) { ++dstChunk; dstIndex = 0; }
                    auto& dst = newChunks[dstChunk];
                    const size_t run = std::min(srcCount - srcIndex, capacity - dstIndex);

                    // Entities: bulk-append the run.
                    auto& srcEntities = src->GetEntities();
                    auto& dstEntities = dst->GetEntities();
                    for (size_t k = 0; k < run; ++k)
                    {
                        const Entity e = srcEntities[srcIndex + k];
                        dstEntities.push_back(e);
                        newLocations.emplace_back(e, EntityLocation::Create(dstChunk, dstIndex + k));
                    }

                    // Components: per column, memcpy the whole run when trivially
                    // copyable, else per-element MoveConstruct + Destruct.
                    for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
                    {
                        const ComponentID id = m_columnMeta.columns[c].id;
                        const uint32_t stride = m_columnMeta.columns[c].stride;
                        const ComponentDescriptor& desc = *m_columnMeta.columns[c].descriptor;
                        std::byte* srcPtr = static_cast<std::byte*>(src->GetComponentArrayByID(id)) + srcIndex * stride;
                        std::byte* dstPtr = static_cast<std::byte*>(dst->GetComponentArrayByID(id)) + dstIndex * stride;
                        if (desc.is_trivially_copyable)
                        {
                            std::memcpy(dstPtr, srcPtr, run * static_cast<size_t>(stride));
                        }
                        else
                        {
                            for (size_t k = 0; k < run; ++k)
                            {
                                desc.MoveConstruct(dstPtr + k * stride, srcPtr + k * stride);
                                desc.Destruct(srcPtr + k * stride);
                            }
                        }

                        // Disabled-bit carry (Task 2, spec §12.5): src and dst are
                        // chunks of the SAME archetype, so column ordinal `c` matches on
                        // both. dst is a fresh chunk (born enabled); copy each moved
                        // slot's bit. src bits need no clearing -- the old chunks are
                        // freed wholesale below.
                        if (desc.isEnableable) ASTRA_UNLIKELY
                        {
                            for (size_t k = 0; k < run; ++k)
                                dst->SetDisabled(c, dstIndex + k, src->IsDisabled(c, srcIndex + k));
                        }
                    }

                    dst->SetCount(dst->GetCount() + run);
                    srcIndex += run;
                    dstIndex += run;
                }
                // Every element was moved out (trivial columns need no Destruct;
                // complex ones were destructed above): make the chunk inert so its
                // destructor doesn't re-destruct moved-from slots.
                src->GetEntities().clear();
                src->SetCount(0);
            }

            m_chunks = std::move(newChunks);   // old chunks free here -> TLSF coalesces
            m_totalCapacity = chunkCountNeeded * capacity;
            // Every new chunk except possibly the last is packed exactly full. Point
            // m_firstNonFullChunkIndex at the last chunk when it has room; when the
            // live count is an exact multiple of capacity the last chunk is itself
            // full, so point one past the end -- mirroring the "chunk became full ->
            // chunkIndex + 1" convention AllocateEntitySlot/AddEntities already use.
            // GetOrCreateChunk tolerates either (it rescans from this index and
            // appends when nothing is free), but the precise value spares the next
            // allocation a wasted IsFull() probe. (The brief's literal code used a
            // flat size()-1; this refinement is documented in the task report.)
            m_firstNonFullChunkIndex = m_chunks.back()->IsFull() ? m_chunks.size() : m_chunks.size() - 1;

            const size_t freed = oldChunkCount > m_chunks.size() ? oldChunkCount - m_chunks.size() : 0;
            return {freed, std::move(newLocations)};
        }

        ASTRA_NODISCARD bool IsInitialized() const noexcept { return m_initialized; }
        ASTRA_NODISCARD const ComponentMask& GetMask() const noexcept { return m_mask; }

        ASTRA_NODISCARD size_t GetEntityCount() const noexcept { return m_entityCount; }
        ASTRA_NODISCARD size_t GetChunkCount() const noexcept { return m_chunks.size(); }
        ASTRA_NODISCARD size_t GetComponentCount() const noexcept { return m_componentCount; }
        ASTRA_NODISCARD size_t GetChunkEntityCount(size_t chunkIndex) const noexcept { return (chunkIndex < m_chunks.size()) ? m_chunks[chunkIndex]->GetCount() : 0; }
        // Sum of the live chunks' capacities. Chunks are no longer uniformly
        // sized, so this replaces `chunkCount * entitiesPerChunk` everywhere.
        ASTRA_NODISCARD size_t GetTotalCapacity() const noexcept { return m_totalCapacity; }

        ASTRA_NODISCARD const std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>>& GetChunks() const { return m_chunks; }
        ASTRA_NODISCARD const std::vector<ComponentDescriptor>& GetComponentDescriptors() const { return m_componentDescriptors; }
        ASTRA_NODISCARD const ArchetypeColumnMeta& GetColumnMeta() const noexcept { return m_columnMeta; }

        void SetComponentPool(ArchetypeChunkPool* pool) { m_chunkPool = pool; }

        ASTRA_NODISCARD Archetype* GetAddEdge(ComponentID id) const noexcept
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
            return m_addEdges ? m_addEdges[id] : nullptr;
        }
        ASTRA_NODISCARD Archetype* GetRemoveEdge(ComponentID id) const noexcept
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
            return m_removeEdges ? m_removeEdges[id] : nullptr;
        }
        void SetAddEdge(ComponentID id, Archetype* to)
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
            if (!m_addEdges) m_addEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);  // value-inits to nullptr
            m_addEdges[id] = to;
        }
        void SetRemoveEdge(ComponentID id, Archetype* to)
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
            if (!m_removeEdges) m_removeEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);
            m_removeEdges[id] = to;
        }
        void ClearEdgesTo(Archetype* target) noexcept
        {
            if (m_addEdges)    for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_addEdges[i]    == target) m_addEdges[i]    = nullptr;
            if (m_removeEdges) for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_removeEdges[i] == target) m_removeEdges[i] = nullptr;
        }
        // Drop ALL cached transition edges (both directions). Used when every edge
        // target has been freed en masse (e.g. Deserialize replaces the archetype set)
        // so lazy recompute repopulates correctly instead of reading freed archetypes.
        void ClearAllEdges() noexcept
        {
            m_addEdges.reset();
            m_removeEdges.reset();
        }

    private:
        // Writes one component column's per-chunk data: custom serialize
        // (versioned or plain) or a compressed trivially-copyable block,
        // followed by the column's disabled-bit section iff it is enableable.
        // Extracted verbatim from Serialize()'s per-column loop body (Task 2 of
        // the LZ4 per-column compression plan, NO behavior change) so Task 3's
        // compressed-column path can reuse it. Caller guarantees
        // chunk->GetComponentArrayByID(desc.id) is non-null (null/tag columns
        // are skipped before calling).
        void SerializeColumn(BinaryWriter& w, ArchetypeChunk* chunk, const ComponentDescriptor& desc, size_t chunkEntityCount) const
        {
            void* componentArray = chunk->GetComponentArrayByID(desc.id);

            // Per-element serialize. Every registered component has serializeVersioned
            // (ComponentRegistry.hpp), so the first arm always runs; the plain-serialize
            // arm is kept for completeness. The former `is_trivially_copyable ->
            // WriteCompressedBlock(rawArray)` bypass was REMOVED with the LZ4
            // per-column wrapper (compression is now orthogonal to versioning and lives
            // in Serialize()'s column loop, never here) -- it silently skipped
            // versioning for POD columns and never actually ran anyway.
            // Every registered component gets both serialize hooks set unconditionally
            // (ComponentRegistry), so the branch below always runs. Assert the precondition
            // in Debug: a descriptor with neither hook would silently write zero bytes for
            // chunkEntityCount elements -- a stream desync with no other trip.
            ASTRA_ASSERT(desc.serializeVersioned || desc.serialize,
                "SerializeColumn: component descriptor has no serialize hook");
            if (desc.serializeVersioned || desc.serialize)
            {
                // For custom serialization, we can't compress the whole array
                // as each component is serialized individually
                if (desc.serializeVersioned)
                {
                    for (size_t i = 0; i < chunkEntityCount; ++i)
                    {
                        void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                        desc.serializeVersioned(w, componentPtr);
                    }
                }
                else
                {
                    for (size_t i = 0; i < chunkEntityCount; ++i)
                    {
                        void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                        desc.serialize(w, componentPtr);
                    }
                }
            }

            // Enableable-components (Task 4, format v4): persist this column's
            // per-chunk disabled-bit state immediately after its component data.
            // Zero-cost for non-enableable columns (invariant 1) -- the branch
            // below is skipped entirely unless the descriptor opted into
            // ASTRA_ENABLEABLE, so a zero-enableable archetype's serialized size
            // is unchanged by this feature (beyond the version-constant bump).
            if (desc.isEnableable)
            {
                // Tag components (desc.size == 0) never reach here -- their
                // componentArray is null and the caller's null-skip already
                // skipped them -- so this ordinal is always a real storage column.
                const int col = m_columnMeta.idToColumn[desc.id];
                w(chunk->GetDisabledCount(col));

                const uint64_t* words = chunk->GetDisabledWords(col);
                // Word count mirrors the EXACT capacity Deserialize will
                // reconstruct this chunk at (max(1, chunkEntityCount)), not this
                // (possibly larger, not-yet-full) live chunk's own capacity. Any
                // bit at or beyond chunkEntityCount is guaranteed zero by the
                // disabled-bit invariant (spec 14.2), so truncating to the
                // reader's exact-fit word count loses no information.
                const size_t wordCapacity = std::max<size_t>(1, chunkEntityCount);
                const size_t wordCount = (wordCapacity + 63) / 64;
                for (size_t wi = 0; wi < wordCount; ++wi)
                {
                    w(words[wi]);
                }
            }
        }

        // Reads one component column's per-chunk data (custom deserialize,
        // versioned or plain, or a compressed trivially-copyable block) into
        // the chunk's component array, then (iff hasDisabledSection) reads,
        // validates, and applies the column's disabled-bit section. Extracted
        // verbatim from Deserialize()'s per-column loop body (Task 2 of the LZ4
        // per-column compression plan, NO behavior change) so Task 3's
        // compressed-column path can reuse it. Caller guarantees
        // chunk->GetComponentArrayByID(desc.id) is non-null (null/tag columns
        // are skipped before calling); hasDisabledSection is the per-descriptor
        // disk flag (diskHasDisabledSection[di], IM-9), not this build's
        // desc.isEnableable. Static (like Deserialize itself): archetype is
        // passed explicitly rather than accessed via `this`.
        static Result<std::unique_ptr<Archetype>, SerializationError> DeserializeColumn(BinaryReader& r, ArchetypeChunk* chunk, Archetype* archetype, const ComponentDescriptor& desc, size_t chunkEntityCount, bool hasDisabledSection)
        {
            using ResultType = Result<std::unique_ptr<Archetype>, SerializationError>;

            void* componentArray = chunk->GetComponentArrayByID(desc.id);

            // Per-element deserialize (mirror of SerializeColumn). The former
            // `is_trivially_copyable -> ReadCompressedBlock` arm was REMOVED with the
            // LZ4 per-column wrapper: decompression now happens once, in Deserialize()'s
            // column loop, which hands this helper the already-decompressed plain bytes.
            // Mirror of SerializeColumn's precondition assert: a hookless descriptor would
            // silently read zero bytes for chunkEntityCount elements -> stream desync.
            ASTRA_ASSERT(desc.deserializeVersioned || desc.deserialize,
                "DeserializeColumn: component descriptor has no deserialize hook");
            if (desc.deserializeVersioned || desc.deserialize)
            {
                // For custom deserialization, components are not compressed
                // as they were serialized individually
                if (desc.deserializeVersioned)
                {
                    for (uint32_t i = 0; i < chunkEntityCount; ++i)
                    {
                        void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                        desc.deserializeVersioned(r, componentPtr);
                    }
                }
                else
                {
                    for (uint32_t i = 0; i < chunkEntityCount; ++i)
                    {
                        void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                        desc.deserialize(r, componentPtr);
                    }
                }

                if (r.HasError())
                {
                    return ResultType::Err(r.GetError());
                }
            }

            // Enableable-components (Task 4, format v4): mirror-image of the
            // write side above. When the ARCHIVE recorded a disabled-bit section
            // for this column (hasDisabledSection -- the per-descriptor flag read
            // from the descriptor block), a disabledCount + word section follows
            // this column's component data. Consume it iff that flag is set --
            // NOT iff THIS build marks the component enableable (IM-9):
            // recording presence in the stream keeps the reader byte-synchronized
            // even when a component's ASTRA_ENABLEABLE status differs between the
            // saving and loading builds. The flag is only ever set for v4+
            // archives (pre-v4 defaults it to 0), which subsumes the old explicit
            // version gate; pre-v4 archives never wrote this section for ANY
            // column, so the chunk's word region stays zero-init from
            // AppendChunk/InitializeColumns above and every entity comes back
            // enabled (invariant 8's "legacy loads all-enabled" contract).
            //
            // The section is always VALIDATED (refuse-not-trust, spec 14 invariant
            // 8): a disabledCount that doesn't match popcount(words), or any bit
            // set at or beyond this chunk's live entity count, is corrupted data
            // and fails the whole load rather than loading it silently.
            if (hasDisabledSection)
            {
                const size_t wordCapacity = std::max<size_t>(1, static_cast<size_t>(chunkEntityCount));
                const size_t wordCount = (wordCapacity + 63) / 64;

                uint32_t diskDisabledCount = 0;
                r(diskDisabledCount);
                if (r.HasError())
                {
                    return ResultType::Err(r.GetError());
                }

                // Read into a local buffer first and validate BEFORE touching the
                // live chunk -- a rejected section must not leave the chunk's real
                // word region partially written (the archetype is discarded on Err
                // either way, but this keeps the write atomic/all-or-nothing,
                // matching the compressed-block read above).
                std::vector<uint64_t> diskWords(wordCount, 0);
                uint32_t popcount = 0;
                for (size_t w = 0; w < wordCount; ++w)
                {
                    r(diskWords[w]);
                    popcount += static_cast<uint32_t>(std::popcount(diskWords[w]));
                }
                if (r.HasError())
                {
                    return ResultType::Err(r.GetError());
                }

                if (diskDisabledCount != popcount)
                {
                    return ResultType::Err(SerializationError::CorruptedData);
                }

                for (size_t i = static_cast<size_t>(chunkEntityCount); i < wordCount * 64; ++i)
                {
                    if ((diskWords[i >> 6] >> (i & 63)) & 1ull)
                    {
                        return ResultType::Err(SerializationError::CorruptedData);
                    }
                }

                if (desc.isEnableable)
                {
                    // This build still stores per-entity disabled bits for the
                    // type -- apply the validated section to the live chunk.
                    const int col = archetype->m_columnMeta.idToColumn[desc.id];
                    uint64_t* liveWords = chunk->GetDisabledWords(col);
                    std::memcpy(liveWords, diskWords.data(), wordCount * sizeof(uint64_t));
                    chunk->m_columns[col].disabledCount = diskDisabledCount;
                }
                // else: the archive carried disabled bits, but THIS build no
                // longer marks the component ASTRA_ENABLEABLE. The bytes are
                // consumed and validated (stream stays in sync) then dropped;
                // every entity of this type loads enabled -- graceful schema
                // evolution for a column that lost ASTRA_ENABLEABLE.
            }

            return ResultType::Ok(nullptr);
        }

        static constexpr size_t INVALID_CHUNK_INDEX = std::numeric_limits<size_t>::max();

        template<typename AddFunc>
        EntityLocation AddEntityInternal(Entity entity, AddFunc&& addFunc)
        {
            auto [chunkIndex, wasCreated] = GetOrCreateChunk();
            if (chunkIndex == INVALID_CHUNK_INDEX) ASTRA_UNLIKELY
            {
                return EntityLocation();
            }

            size_t entityIndex = addFunc(m_chunks[chunkIndex], entity);
            ++m_entityCount;

            if (m_chunks[chunkIndex]->IsFull()) ASTRA_UNLIKELY
            {
                m_firstNonFullChunkIndex = chunkIndex + 1;
            }

            return EntityLocation::Create(chunkIndex, entityIndex);
        }

        std::vector<EntityLocation> BatchMoveEntitiesFrom(std::span<const Entity> entities, Archetype& srcArchetype, std::span<const EntityLocation> srcLocations)
        {
            ASTRA_ASSERT(entities.size() == srcLocations.size(), "Entity and location array size mismatch");
            size_t count = entities.size();
            if (count == 0)
                return {};

            std::vector<EntityLocation> dstLocations;
            dstLocations.reserve(count);

            size_t remainingCapacity = GetRemainingCapacity();
            if (count > remainingCapacity) ASTRA_UNLIKELY
            {
                if (!m_initialized) ASTRA_UNLIKELY
                {
                    // See GetOrCreateChunk(): this archetype never found a valid
                    // per-chunk layout and can never hold an entity.
                    return {};
                }

                // All-or-nothing growth, as before: if any chunk in the run cannot
                // be allocated, unwind the ones already appended so a failed batch
                // move leaves the archetype exactly as it found it. NextChunkBytes()
                // is recomputed every iteration so sizes ramp within this batch.
                const size_t chunksBefore = m_chunks.size();

                while (remainingCapacity < count)
                {
                    ArchetypeChunk* chunk = AppendChunk(NextChunkBytes());
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        // Failed to allocate all required chunks - return empty to indicate failure
                        while (m_chunks.size() > chunksBefore)
                        {
                            PopBackChunk();
                        }
                        return {};
                    }
                    remainingCapacity += chunk->GetCapacity();
                }
            }

            size_t entityIndex = 0;
            size_t chunkIndex = m_firstNonFullChunkIndex;

            while (entityIndex < count && chunkIndex < m_chunks.size()) ASTRA_LIKELY
            {
                auto& chunk = m_chunks[chunkIndex];
                size_t available = chunk->GetCapacity() - chunk->GetCount();

                if (available > 0) ASTRA_LIKELY
                {
                    size_t toAdd = std::min(available, count - entityIndex);
                    size_t startIndex = chunk->GetCount();

                    for (size_t i = 0; i < toAdd; ++i)
                    {
                        chunk->GetEntities().push_back(entities[entityIndex + i]);
                        dstLocations.push_back(EntityLocation::Create(chunkIndex, startIndex + i));
                    }
                    chunk->SetCount(chunk->GetCount() + toAdd);

                    entityIndex += toAdd;

                    // Update first non-full chunk if this one is now full
                    if (chunk->IsFull() && chunkIndex == m_firstNonFullChunkIndex) ASTRA_UNLIKELY
                    {
                        m_firstNonFullChunkIndex = chunkIndex + 1;
                    }
                }

                ++chunkIndex;
            }

            m_entityCount += entityIndex;

            // Check if allocation was successful for all entities
            if (dstLocations.size() != entities.size())
            {
                // Partial allocation - only process what we got
                // This shouldn't happen in normal operation but handle it gracefully
                return dstLocations;
            }

            // Group by chunks for efficient batch processing
            struct ChunkBatch
            {
                size_t srcChunkIndex;
                size_t dstChunkIndex;
                SmallVector<size_t, 32> srcIndices;  // Use SmallVector to avoid allocations
                SmallVector<size_t, 32> dstIndices;
            };

            FlatMap<uint64_t, ChunkBatch> batches;
            batches.Reserve(16);  // Pre-allocate for typical case

            for (size_t i = 0; i < dstLocations.size(); ++i)
            {
                // Check if source location is valid
                if (!srcLocations[i].IsValid())
                {
                    // Skip invalid source locations
                    continue;
                }

                size_t srcChunkIndex = srcLocations[i].GetChunkIndex();
                size_t srcEntityIndex = srcLocations[i].GetEntityIndex();
                size_t dstChunkIndex = dstLocations[i].GetChunkIndex();
                size_t dstEntityIndex = dstLocations[i].GetEntityIndex();

                // Validate source and destination locations
                ASTRA_ASSERT(srcChunkIndex < srcArchetype.m_chunks.size(), "Source chunk index out of bounds");
                ASTRA_ASSERT(dstChunkIndex < m_chunks.size(), "Destination chunk index out of bounds");

                // Create unique key for src-dst chunk pair
                uint64_t key = (uint64_t(srcChunkIndex) << 32) | dstChunkIndex;

                auto& batch = batches[key];
                batch.srcChunkIndex = srcChunkIndex;
                batch.dstChunkIndex = dstChunkIndex;
                batch.srcIndices.push_back(srcEntityIndex);
                batch.dstIndices.push_back(dstEntityIndex);
            }

            // Calculate components to move (only shared components)
            // This is the intersection of source and destination masks
            ComponentMask componentsToMove = srcArchetype.GetMask() & GetMask();

            // If there are no components to move (e.g., moving from root archetype),
            // we're done - entities are already allocated
            if (componentsToMove.None())
            {
                return dstLocations;
            }

            // Batch move components for each chunk pair
            for (auto& [key, batch] : batches)
            {
                // Validate chunk indices
                if (batch.srcChunkIndex >= srcArchetype.m_chunks.size() || batch.dstChunkIndex >= m_chunks.size())
                {
                    ASTRA_ASSERT(false, "Invalid chunk index in batch move");
                    continue;
                }

                auto& srcChunk = srcArchetype.m_chunks[batch.srcChunkIndex];
                auto& dstChunk = m_chunks[batch.dstChunkIndex];

                dstChunk->BatchMoveComponentsFrom(batch.dstIndices, *srcChunk, batch.srcIndices, componentsToMove);
            }

            return dstLocations;
        }

        ASTRA_NODISCARD std::pair<ArchetypeChunk*, size_t> ResolveLocation(EntityLocation location)
        {
            size_t chunkIndex = location.GetChunkIndex();
            size_t entityIndex = location.GetEntityIndex();
            ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");
            return {m_chunks[chunkIndex].get(), entityIndex};
        }

        // Helper to get component value - handles empty components specially
        template<typename T>
        ASTRA_FORCEINLINE static auto& GetComponentValue(T* array, size_t index)
        {
            if constexpr (std::is_empty_v<T>)
            {
                // Empty components (tags) have no data, array pointer is nullptr
                static T emptyInstance{};
                return emptyInstance;
            }
            else
            {
                ASTRA_ASSERT(array != nullptr, "Component array is null");
                return array[index];
            }
        }
        
        template<typename... Components, typename Func, size_t... Is>
        ASTRA_FORCEINLINE void ForEachImpl(ArchetypeChunk* chunk, size_t count, Func&& func, std::index_sequence<Is...>)
        {
            auto arrays = std::tuple{chunk->GetComponentArray<Components>()...};
            (void)arrays; // silence -Wunused-but-set-variable when Components is empty
            const auto& entities = chunk->GetEntities();

            for (size_t i = 0; i < count; ++i)
            {
                func(entities[i], GetComponentValue<Components>(std::get<Is>(arrays), i)...);
            }
        }
        
        std::pair<size_t, bool> GetOrCreateChunk()
        {
            if (!m_initialized) ASTRA_UNLIKELY
            {
                // Initialize() never found a valid per-chunk layout for this component
                // set (single-entity footprint exceeds the usable chunk space) - refuse
                // to create a chunk that could never legally hold even one entity
                // instead of overflowing into neighboring memory.
                return {INVALID_CHUNK_INDEX, false};
            }

            size_t chunkIndex = m_firstNonFullChunkIndex;

            if (chunkIndex < m_chunks.size() && !m_chunks[chunkIndex]->IsFull()) ASTRA_LIKELY
            {
                return {chunkIndex, false};
            }
            
            for (chunkIndex = m_firstNonFullChunkIndex; chunkIndex < m_chunks.size(); ++chunkIndex)
            {
                if (!m_chunks[chunkIndex]->IsFull()) ASTRA_LIKELY
                {
                    m_firstNonFullChunkIndex = chunkIndex;
                    return {chunkIndex, false};
                }
            }
            
            if (!AppendChunk(NextChunkBytes())) ASTRA_UNLIKELY
            {
                return {INVALID_CHUNK_INDEX, false};
            }

            chunkIndex = m_chunks.size() - 1;
            m_firstNonFullChunkIndex = chunkIndex;

            return {chunkIndex, true};
        }

        EntityLocation AllocateEntitySlot(Entity entity)
        {
            auto [chunkIndex, wasCreated] = GetOrCreateChunk();
            if (chunkIndex == INVALID_CHUNK_INDEX) ASTRA_UNLIKELY
            {
                return EntityLocation();
            }

            auto* chunk = m_chunks[chunkIndex].get();

            ASTRA_ASSERT(chunk->GetCount() < chunk->GetCapacity(), "Chunk is full");
            size_t entityIndex = chunk->GetCount();
            chunk->GetEntities().push_back(entity);   // capacity pre-reserved at chunk creation: never reallocates
            chunk->SetCount(entityIndex + 1);

            ++m_entityCount;
            
            if (chunk->IsFull()) ASTRA_UNLIKELY
            {
                m_firstNonFullChunkIndex = chunkIndex + 1;
            }

            return EntityLocation::Create(chunkIndex, entityIndex);
        }
        
        std::vector<std::pair<Entity, EntityLocation>> MoveEntitiesBetweenChunks(size_t srcChunkIndex, size_t destChunkIndex, size_t count)
        {
            std::vector<std::pair<Entity, EntityLocation>> movedEntities;
            movedEntities.reserve(count);
            
            auto& srcChunk = m_chunks[srcChunkIndex];
            auto& destChunk = m_chunks[destChunkIndex];
            
            // Get the last 'count' entities from source chunk
            size_t srcCount = srcChunk->GetCount();
            size_t destCount = destChunk->GetCount();
            
            for (size_t i = 0; i < count; ++i)
            {
                size_t srcEntityIndex = srcCount - i - 1;
                size_t destEntityIndex = destCount + i;
                
                // Get entity from source chunk
                Entity entity = srcChunk->GetEntity(srcEntityIndex);
                
                // Add entity to destination chunk's entity vector
                destChunk->GetEntities().push_back(entity);
                
                EntityLocation destEntityLocation = EntityLocation::Create(destChunkIndex, destEntityIndex);
                movedEntities.emplace_back(entity, destEntityLocation);
                
                // Move components column by column. Both chunks belong to this
                // archetype, so they share m_columnMeta (identical column layout).
                // destEntityIndex is transiently >= destChunk's count here (the count
                // is bumped after the loop), so resolve the base directly and index it
                // rather than going through GetComponentPointer's count-bounded assert.
                for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
                {
                    const ComponentID id = m_columnMeta.columns[c].id;
                    const uint32_t stride = m_columnMeta.columns[c].stride;
                    const ComponentDescriptor& desc = *m_columnMeta.columns[c].descriptor;

                    void* srcPtr = static_cast<std::byte*>(srcChunk->GetComponentArrayByID(id)) + srcEntityIndex * stride;
                    void* destPtr = static_cast<std::byte*>(destChunk->GetComponentArrayByID(id)) + destEntityIndex * stride;

                    // Use move constructor to transfer component data
                    desc.MoveConstruct(destPtr, srcPtr);
                    desc.Destruct(srcPtr);

                    // Disabled-bit carry (Task 2): same archetype => same column ordinal
                    // on both chunks. dst slot is freshly counted (born enabled); the
                    // src bit is cleared right after via the pop below shrinking count.
                    // NOTE: this method currently has no live callers (referenced only
                    // in a comment); the carry is here so every entity-relocation path
                    // is uniformly covered if it is ever revived.
                    if (desc.isEnableable) ASTRA_UNLIKELY
                    {
                        destChunk->SetDisabled(c, destEntityIndex, srcChunk->IsDisabled(c, srcEntityIndex));
                        srcChunk->SetDisabled(c, srcEntityIndex, false);
                    }
                }

                // Remove entity from source chunk's entity vector
                srcChunk->GetEntities().pop_back();
            }
            
            // Update chunk counts
            srcChunk->SetCount(srcCount - count);
            destChunk->SetCount(destCount + count);
            
            return movedEntities;
        }

        ComponentMask m_mask;
        size_t m_componentCount;  // Cached component count for fast access
        std::vector<ComponentDescriptor> m_componentDescriptors;
        // Built once by BuildColumnMeta() in Initialize(); columns[*].descriptor points into
        // m_componentDescriptors above, which is set once and never reassigned afterward.
        ArchetypeColumnMeta m_columnMeta;
        std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> m_chunks;
        size_t m_entityCount;
        // Chunks of one archetype no longer share a capacity (Phase 2), so the
        // uniform m_entitiesPerChunk + shift/mask trio is gone. What Initialize
        // establishes is the LAYOUT (bytes per entity plus the conservative
        // per-column padding estimate); a chunk's capacity is then derived from
        // its own byte size, and the archetype only caches the running sum.
        size_t m_perEntitySize = 0;         // summed non-empty component sizes
        size_t m_alignmentOverhead = 0;     // conservative per-chunk column padding estimate
        size_t m_totalCapacity = 0;         // sum of m_chunks[i]->GetCapacity(), maintained incrementally
        size_t m_firstNonFullChunkIndex = 0;  // Track first chunk with available space for O(1) lookup
        bool m_initialized;
        ArchetypeChunkPool* m_chunkPool = nullptr;

        // Add/remove transition edges, indexed by ComponentID (< MAX_COMPONENTS).
        // Lazily allocated on first edge; nullptr slot = no cached edge; freed with the archetype.
        std::unique_ptr<Archetype*[]> m_addEdges;
        std::unique_ptr<Archetype*[]> m_removeEdges;

        friend class ArchetypeManager;
        friend class Registry;
    };
}
