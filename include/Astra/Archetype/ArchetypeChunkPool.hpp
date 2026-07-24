#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Tlsf.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    // Per-archetype column metadata, built once and shared by every chunk of the archetype.
    // The heavy ComponentDescriptor lives here once (via pointer into the archetype's canonical
    // list) instead of by-value in every chunk slot. Columns are storage-bearing components only
    // (tags excluded), sorted ascending by id so cross-archetype moves can merge-join.
    struct ArchetypeColumnMeta
    {
        struct ColumnDesc
        {
            ComponentID id{0};
            uint32_t stride{0};                          // == descriptor->size
            const ComponentDescriptor* descriptor{nullptr};
        };

        uint16_t  columnCount{0};                        // N: storage-bearing components
        bool      isComplex{false};                      // any column not trivially copyable
        ColumnDesc columns[MAX_COMPONENTS]{};            // [0, columnCount) valid, ascending id
        int16_t   idToColumn[MAX_COMPONENTS];            // id -> column index, or -1 (absent OR tag)

        ArchetypeColumnMeta() { for (auto& c : idToColumn) c = -1; }
    };

    class ArchetypeChunkPool
    {
    public:
        class Chunk;
        
        static constexpr size_t DEFAULT_CHUNK_SIZE = 16 * 1024;  // 16KB default (fits in L1 cache)
        static constexpr size_t MIN_CHUNK_SIZE = 4 * 1024;       // 4KB minimum
        static constexpr size_t MAX_CHUNK_SIZE = 1024 * 1024;    // 1MB maximum
        
        // Configuration for pool behavior
        struct Config
        {
            size_t chunkSize = DEFAULT_CHUNK_SIZE;
            size_t chunksPerBlock = 128;
            size_t maxChunks = 4096;
            size_t initialBlocks = 0;
            bool useHugePages = true;

            // Grow-as-populate sizing policy (Phase 2 Unit C part 2): each NEW
            // chunk an archetype appends is sized from its current data footprint,
            // clamped to [minChunkBytes, maxChunkBytes]. See Archetype::NextChunkBytes.
            size_t minChunkBytes = MIN_CHUNK_SIZE;    // 4KB - first/smallest chunk
            size_t maxChunkBytes = 512 * 1024;        // sizing cap (study: 512KB, not 1MB)
            size_t growDivisor = 2;                   // chunk ~ archetypeBytes / growDivisor
        };
        
        struct Stats
        {
            size_t totalChunks = 0;
            size_t freeChunks = 0;
            size_t acquireCount = 0;
            size_t releaseCount = 0;
            size_t blockAllocations = 0;
            size_t failedAcquires = 0;
        };
        
        // Custom deleter for chunks
        struct ChunkDeleter
        {
            ArchetypeChunkPool* pool = nullptr;
            void* memory = nullptr;
            
            void operator()(Chunk* chunk) const
            {
                if (chunk) ASTRA_LIKELY
                {
                    delete chunk;
                    
                    if (pool && memory) ASTRA_LIKELY
                    {
                        pool->ReturnChunk(memory);
                    }
                }
            }
        };
        
        class Chunk
        {
        public:
            Chunk(Chunk&& other) noexcept :
                m_memory(std::exchange(other.m_memory, nullptr)),
                m_capacity(other.m_capacity),
                m_count(other.m_count),
                m_entities(std::move(other.m_entities)),
                m_meta(other.m_meta),
                m_chunkSize(other.m_chunkSize)
            {
                // Column is trivially copyable; a memberwise byte copy of the packed
                // array is correct (the bases point into m_memory, which we took over).
                std::memcpy(m_columns, other.m_columns, sizeof(m_columns));
                // Make the moved-from chunk inert so its destructor touches nothing.
                other.m_meta = nullptr;
                other.m_count = 0;
            }
            
            Chunk(const Chunk&) = delete;
            Chunk& operator=(const Chunk&) = delete;

            ~Chunk()
            {
                if (!m_meta) ASTRA_UNLIKELY
                    return;  // moved-from (inert) chunk: nothing to destruct

                // Destruct every live element of every storage column.
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    std::byte* base = static_cast<std::byte*>(m_columns[c].base);
                    const uint32_t stride = m_columns[c].stride;
                    const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                    for (size_t i = 0; i < m_count; ++i)
                    {
                        desc.Destruct(base + i * stride);
                    }
                }
            }

            size_t AddEntity(Entity entity)
            {
                ASTRA_ASSERT(m_count < m_capacity, "Chunk is full, cannot add more entities");
                size_t index = m_count++;
                
                m_entities.push_back(entity);

                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    void* ptr = static_cast<std::byte*>(m_columns[c].base) + index * m_columns[c].stride;
                    m_meta->columns[c].descriptor->DefaultConstruct(ptr);
                }

                return index;
            }
            
            // Helper to construct a single component with a value at a specific index
            template<typename T>
            void ConstructComponentAt(size_t index, T&& value)
            {
                ComponentID id = TypeID<std::decay_t<T>>::Value();
                const int col = m_meta->idToColumn[id];

                if (col < 0) ASTRA_UNLIKELY
                {
                    return;  // absent or tag: no storage
                }

                const ComponentDescriptor& desc = *m_meta->columns[col].descriptor;
                void* ptr = static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;

                // Use ConstructWith for optimal construction with value
                if constexpr (std::is_lvalue_reference_v<T>)
                {
                    desc.ConstructWith(ptr, &value);
                }
                else
                {
                    // For rvalues, we need temporary storage. Move (not ConstructWith/copy)
                    // out of temp: it is a genuine local about to be destroyed regardless, and
                    // moveConstruct is set unconditionally for every Component (move-constructible
                    // is part of the concept), unlike constructWith/copyConstruct which are null
                    // for move-only types -- ConstructWith would silently fall back to
                    // DefaultConstruct for those, discarding the value (Theme G, found while
                    // wiring the move-only Tracked test component through this path).
                    using DecayedType = std::decay_t<T>;
                    DecayedType temp(std::forward<T>(value));
                    desc.MoveConstruct(ptr, &temp);
                }
            }
            
            // Add entity with components constructed directly with values
            template<typename... Components>
            size_t AddEntityWithComponents(Entity entity, Components&&... components)
            {
                ASTRA_ASSERT(m_count < m_capacity, "Chunk is full, cannot add more entities");
                size_t index = m_count++;
                
                m_entities.push_back(entity);
                
                // First, default construct all components that are in the archetype
                // but not provided in the parameter pack
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    const ComponentID id = m_meta->columns[c].id;

                    // Check if this component is in our parameter pack
                    bool willBeConstructed = ((TypeID<std::decay_t<Components>>::Value() == id) || ...);

                    if (!willBeConstructed)
                    {
                        void* ptr = static_cast<std::byte*>(m_columns[c].base) + index * m_columns[c].stride;
                        m_meta->columns[c].descriptor->DefaultConstruct(ptr);
                    }
                }
                
                // Now construct the provided components with their values
                ((ConstructComponentAt(index, std::forward<Components>(components))), ...);
                
                return index;
            }
            
            void BatchAddEntities(std::span<const Entity> entities)
            {
                size_t count = entities.size();
                ASTRA_ASSERT(m_count + count <= m_capacity, "Batch add would exceed chunk capacity");

                m_entities.insert(m_entities.end(), entities.begin(), entities.end());

                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    std::byte* startPtr = static_cast<std::byte*>(m_columns[c].base) + m_count * m_columns[c].stride;
                    m_meta->columns[c].descriptor->BatchDefaultConstruct(startPtr, count);
                }

                m_count += count;
            }
            
            void BatchMoveComponentsFrom(std::span<const size_t> dstIndices, const Chunk& srcChunk, std::span<const size_t> srcIndices, const ComponentMask& componentsToMove)
            {
                ASTRA_ASSERT(dstIndices.size() == srcIndices.size(), "Destination and source index arrays must have the same size");
                size_t count = dstIndices.size();
                
                if (componentsToMove.None()) return;

                // Iterate the destination's storage columns; only move those that are
                // in the move set AND present as storage in the source chunk.
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    const ComponentID id = m_meta->columns[c].id;
                    if (!componentsToMove.Test(id)) continue;

                    const int sc = srcChunk.m_meta->idToColumn[id];
                    if (sc < 0) continue;  // source lacks this component's storage

                    void* dstBase = m_columns[c].base;
                    void* srcBase = srcChunk.m_columns[sc].base;
                    const uint32_t stride = m_columns[c].stride;
                    const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                    if (desc.is_trivially_copyable && AreIndicesContiguous(dstIndices) && AreIndicesContiguous(srcIndices))
                    {
                        void* dstPtr = static_cast<std::byte*>(dstBase) + dstIndices[0] * stride;
                        void* srcPtr = static_cast<std::byte*>(srcBase) + srcIndices[0] * stride;
                        std::memcpy(dstPtr, srcPtr, count * stride);
                    }
                    else
                    {
                        for (size_t i = 0; i < count; ++i)
                        {
                            void* dstPtr = static_cast<std::byte*>(dstBase) + dstIndices[i] * stride;
                            void* srcPtr = static_cast<std::byte*>(srcBase) + srcIndices[i] * stride;

                            if (desc.is_trivially_copyable)
                            {
                                std::memcpy(dstPtr, srcPtr, stride);
                            }
                            else
                            {
                                desc.MoveConstruct(dstPtr, srcPtr);
                            }
                        }
                    }
                }
            }
            
            static bool AreIndicesContiguous(std::span<const size_t> indices)
            {
                if (indices.size() <= 1)
                    return true;
                for (size_t i = 1; i < indices.size(); ++i)
                {
                    if (indices[i] != indices[i-1] + 1)
                    {
                        return false;
                    }
                }
                return true;
            }
            
            template<Component T>
            void BatchConstructComponent(std::span<const size_t> indices, const T& value)
            {
                ComponentID id = TypeID<T>::Value();
                const int col = m_meta->idToColumn[id];

                // A column index < 0 means this component is absent from the archetype
                // OR is an empty (tag) component with no storage; either way there is
                // nothing to construct — never form a pointer from it.
                if (col < 0) ASTRA_UNLIKELY
                    return;

                void* base = m_columns[col].base;
                const uint32_t stride = m_columns[col].stride;

                // Debug validation
                for (size_t idx : indices)
                {
                    (void)idx; // only used by the assert below in debug builds
                    ASTRA_ASSERT(idx < m_capacity, "BatchConstructComponent: index out of capacity");
                    // Note: We allow idx >= m_count because entities might be in the process of being added
                    // ASTRA_ASSERT(idx < m_count, "BatchConstructComponent: index out of current count");
                }
                
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    bool contiguous = true;
                    for (size_t i = 1; i < indices.size(); ++i)
                    {
                        if (indices[i] != indices[i-1] + 1)
                        {
                            contiguous = false;
                            break;
                        }
                    }
                    
                    if (contiguous && indices.size() > 1)
                    {
                        T* firstPtr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + indices[0] * stride));
                        new (firstPtr) T(value);

                        for (size_t i = 1; i < indices.size(); ++i)
                        {
                            T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + indices[i] * stride));
                            std::memcpy(ptr, firstPtr, sizeof(T));
                        }
                    }
                    else
                    {
                        for (size_t idx : indices)
                        {
                            ASTRA_ASSERT(idx < m_capacity, "Index out of capacity");
                            T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + idx * stride));
                            std::memcpy(ptr, &value, sizeof(T));
                        }
                    }
                }
                else
                {
                    // Slow path: construct each component
                    for (size_t idx : indices)
                    {
                        ASTRA_ASSERT(idx < m_capacity, "Component index out of bounds");
                        T* ptr = static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base) + idx * stride));
                        new (ptr) T(value);
                    }
                }
            }

            std::optional<Entity> RemoveEntity(size_t index)
            {
                ASTRA_ASSERT(index < m_count, "Entity index out of bounds");
                
                const size_t lastIndex = m_count - 1;
                std::optional<Entity> movedEntity;
                
                if (index != lastIndex) ASTRA_LIKELY
                {
                    // Move last entity to this position
                    m_entities[index] = m_entities[lastIndex];
                    movedEntity = m_entities[index];
                    
                    // Move components column by column (every column has real storage)
                    for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                    {
                        std::byte* base = static_cast<std::byte*>(m_columns[c].base);
                        const uint32_t stride = m_columns[c].stride;
                        const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;

                        void* dstPtr = base + index * stride;
                        void* srcPtr = base + lastIndex * stride;

                        // Destruct destination, move from source, then destruct the
                        // moved-from source slot (mirrors Archetype::MoveEntitiesBetweenChunks).
                        desc.Destruct(dstPtr);
                        desc.MoveConstruct(dstPtr, srcPtr);
                        desc.Destruct(srcPtr);
                    }
                }
                else
                {
                    // Just destruct the last entity's components column by column
                    for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                    {
                        void* ptr = static_cast<std::byte*>(m_columns[c].base) + lastIndex * m_columns[c].stride;
                        m_meta->columns[c].descriptor->Destruct(ptr);
                    }
                }
                
                // Remove last entity
                m_entities.pop_back();
                --m_count;
                
                return movedEntity;
            }
            
            template<Component T>
            T* GetComponent(size_t index)
            {
                ASTRA_ASSERT(index < m_count, "Index out of count");
                ComponentID id = TypeID<T>::Value();
                void* ptr = GetComponentPointer(id, index);
                
                // For empty components, return a static instance
                if constexpr (std::is_empty_v<T>)
                {
                    if (ptr == nullptr)
                    {
                        static T emptyInstance{};
                        return &emptyInstance;
                    }
                }
                
                return static_cast<T*>(ptr);
            }
            
            template<Component T>
            ASTRA_FORCEINLINE auto GetComponentArray()
            {
                using BaseType = std::remove_const_t<T>;

                // For empty components (tags), return nullptr since they have no data
                // The iteration code will handle this specially
                if constexpr (std::is_empty_v<BaseType>)
                {
                    return static_cast<BaseType*>(nullptr);
                }
                else
                {
                    void* base = GetComponentArrayByID(TypeID<BaseType>::Value());
                    if constexpr (std::is_const_v<T>)
                    {
                        return reinterpret_cast<const BaseType*>(base);
                    }
                    else
                    {
                        return reinterpret_cast<BaseType*>(base);
                    }
                }
            }

            template<Component T>
            ASTRA_FORCEINLINE const std::remove_const_t<T>* GetComponentArray() const
            {
                using BaseType = std::remove_const_t<T>;

                // For empty components, return nullptr since they have no data
                if constexpr (std::is_empty_v<BaseType>)
                {
                    return nullptr;
                }
                else
                {
                    return reinterpret_cast<const BaseType*>(GetComponentArrayByID(TypeID<BaseType>::Value()));
                }
            }

            void* GetComponentArrayByID(ComponentID id) const
            {
                const int col = m_meta->idToColumn[id];
                return col < 0 ? nullptr : m_columns[col].base;
            }
            
            ASTRA_NODISCARD bool IsFull() const noexcept { return m_count >= m_capacity; }
            // Byte size of this chunk's arena. Chunks of one archetype no longer
            // share a single size (Phase 2), so memory accounting must sum this
            // per chunk instead of multiplying a chunk count by the pool's size.
            ASTRA_NODISCARD size_t GetChunkBytes() const noexcept { return m_chunkSize; }
            ASTRA_NODISCARD bool IsEmpty() const noexcept { return m_count == 0; }
            ASTRA_NODISCARD size_t GetCount() const noexcept { return m_count; }
            ASTRA_NODISCARD size_t GetCapacity() const noexcept { return m_capacity; }
            ASTRA_NODISCARD Entity GetEntity(size_t index) const { ASTRA_ASSERT(index < m_count, "Index out of count"); return m_entities[index]; }
            ASTRA_NODISCARD const std::vector<Entity>& GetEntities() const { return m_entities; }
            ASTRA_NODISCARD ASTRA_FORCEINLINE std::vector<Entity>& GetEntities() { return m_entities; }

            void SetCount(size_t count) noexcept { m_count = count; }

            void* GetComponentPointer(ComponentID id, size_t index) const
            {
                ASTRA_ASSERT(id < MAX_COMPONENTS, "ComponentID out of bounds");
                ASTRA_ASSERT(index < m_count, "Index out of bounds");

                const int col = m_meta->idToColumn[id];
                if (col < 0) ASTRA_UNLIKELY
                    return nullptr;  // absent or tag: no storage

                return static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;
            }
            
        private:
            friend class ArchetypeManager;
            
            Chunk(size_t entitiesPerChunk, const ArchetypeColumnMeta* meta, void* memory, size_t chunkSize) :
                m_memory(memory),
                m_capacity(entitiesPerChunk),
                m_count(0),
                m_meta(meta),
                m_chunkSize(chunkSize)
            {
                m_entities.reserve(m_capacity);
                std::memset(m_memory, 0, m_chunkSize);
                InitializeColumns();
            }

            // Assign each storage column a cache-line-aligned base within the chunk arena.
            // Column physical order follows m_meta->columns (ascending by id); tags carry
            // no column, so every column here has real storage.
            void InitializeColumns()
            {
                size_t offset = 0;
                for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                {
                    // Align offset to a cache line to avoid false sharing between columns.
                    offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                    m_columns[c].base   = static_cast<std::byte*>(m_memory) + offset;
                    m_columns[c].stride = m_meta->columns[c].stride;
                    offset += static_cast<size_t>(m_meta->columns[c].stride) * m_capacity;
                }
                ASTRA_ASSERT(offset <= m_chunkSize, "Component layout exceeds chunk size");
            }

            // Packed per-column storage descriptor. Fixed-capacity array (one slot per
            // possible component id) avoids a per-chunk heap allocation; only
            // [0, m_meta->columnCount) are live.
            struct Column
            {
                void* base{nullptr};
                uint32_t stride{0};
            };

            void* m_memory;
            size_t m_capacity;
            size_t m_count;
            std::vector<Entity> m_entities;
            const ArchetypeColumnMeta* m_meta{nullptr};  // shared per-archetype metadata (not owned)
            Column m_columns[MAX_COMPONENTS]{};          // [0, m_meta->columnCount) live
            size_t m_chunkSize;

            friend class ArchetypeChunkPool;
        };
        
        // Delegating overload instead of a default argument: gcc/clang reject a
        // default argument that needs Config's NSDMIs before the enclosing class is complete.
        ArchetypeChunkPool() : ArchetypeChunkPool(Config{}) {}

        explicit ArchetypeChunkPool(const Config& config) : m_config(config)
        {
            ASTRA_ASSERT(m_config.chunkSize >= MIN_CHUNK_SIZE && m_config.chunkSize <= MAX_CHUNK_SIZE, "Chunk size must be between 4KB and 1MB");
            ASTRA_ASSERT((m_config.chunkSize & (m_config.chunkSize - 1)) == 0, "Chunk size must be a power of 2");
            ASTRA_ASSERT(m_config.minChunkBytes >= MIN_CHUNK_SIZE, "minChunkBytes below the pool's absolute floor");
            ASTRA_ASSERT(m_config.maxChunkBytes <= MAX_CHUNK_SIZE, "maxChunkBytes above the pool's absolute ceiling");
            ASTRA_ASSERT(m_config.minChunkBytes <= m_config.maxChunkBytes, "minChunkBytes must not exceed maxChunkBytes");
            ASTRA_ASSERT(m_config.growDivisor >= 1, "growDivisor must be at least 1");

            if (m_config.chunksPerBlock == 0)
            {
                m_config.chunksPerBlock = std::max(size_t(1), HUGE_PAGE_SIZE / m_config.chunkSize);
            }
            if (m_config.maxChunks < m_config.chunksPerBlock)
            {
                m_config.maxChunks = m_config.chunksPerBlock;
            }

            // initialBlocks pre-allocates arenas. A failure here is not fatal:
            // the pool stays usable and CreateChunk retries the growth later.
            for (size_t i = 0; i < m_config.initialBlocks; ++i)
            {
                if (!GrowArena(0)) ASTRA_UNLIKELY
                    break;
            }
        }

        ~ArchetypeChunkPool()
        {
            // The Tlsf heap only indexes memory it was handed; releasing the
            // arenas here is what actually returns it to the OS.
            for (const ArenaRecord& arena : m_arenas)
            {
                FreeMemory(arena.memory, arena.size, arena.usedHugePages);
            }
        }

        ArchetypeChunkPool(const ArchetypeChunkPool&) = delete;
        ArchetypeChunkPool& operator=(const ArchetypeChunkPool&) = delete;

        ArchetypeChunkPool(ArchetypeChunkPool&& other) noexcept :
            m_config(other.m_config),
            m_tlsf(std::move(other.m_tlsf)),
            m_arenas(std::move(other.m_arenas)),
            m_totalChunks(other.m_totalChunks.load()),
            m_acquireCount(other.m_acquireCount.load()),
            m_releaseCount(other.m_releaseCount.load()),
            m_blockAllocations(other.m_blockAllocations.load()),
            m_failedAcquires(other.m_failedAcquires.load())
        {
            other.ResetCounters();
        }

        ArchetypeChunkPool& operator=(ArchetypeChunkPool&& other) noexcept
        {
            if (this != &other)
            {
                for (const ArenaRecord& arena : m_arenas)
                {
                    FreeMemory(arena.memory, arena.size, arena.usedHugePages);
                }
                m_arenas.clear();

                m_config = other.m_config;
                m_tlsf = std::move(other.m_tlsf);
                m_arenas = std::move(other.m_arenas);
                m_totalChunks.store(other.m_totalChunks.load());
                m_acquireCount.store(other.m_acquireCount.load());
                m_releaseCount.store(other.m_releaseCount.load());
                m_blockAllocations.store(other.m_blockAllocations.load());
                m_failedAcquires.store(other.m_failedAcquires.load());
                other.ResetCounters();
            }
            return *this;
        }

        // Creates a chunk of `chunkBytes` bytes holding up to `capacity` entities.
        // The two are independent parameters: the caller (Archetype) owns the
        // capacity-for-bytes layout math, the pool only owns the allocation.
        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(size_t capacity, size_t chunkBytes, const ArchetypeColumnMeta* meta)
        {
            ASTRA_ASSERT(capacity > 0, "Chunk capacity must be positive");

            void* memory = AllocateChunkBytes(chunkBytes);
            if (!memory) ASTRA_UNLIKELY
            {
                return nullptr;
            }

            auto* chunk = new Chunk(capacity, meta, memory, chunkBytes);
            ChunkDeleter deleter{this, memory};
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, deleter);
        }

        void ReturnChunk(void* memory)
        {
            if (!memory) ASTRA_UNLIKELY
                return;

            // Tlsf::Free is O(1) and coalesces with free physical neighbours, so
            // no side table from memory back to a node is needed any more.
            m_tlsf.Free(memory);

            ASTRA_ASSERT(m_totalChunks.load(std::memory_order_relaxed) > 0, "ReturnChunk without a matching CreateChunk");
            // Saturating decrement in ALL builds, not just Debug: ReturnChunk is
            // public, and the assert above compiles out in Release/Dist. An
            // unpaired or foreign call must not underflow m_totalChunks to
            // SIZE_MAX -- that would make AllocateChunkBytes's `m_totalChunks >=
            // m_config.maxChunks` gate permanently true, silently bricking the
            // pool (every future CreateChunk returns nullptr) for good.
            const size_t liveBefore = m_totalChunks.load(std::memory_order_relaxed);
            if (liveBefore > 0) ASTRA_LIKELY
            {
                m_totalChunks.fetch_sub(1, std::memory_order_relaxed);
            }
            m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        }

        ASTRA_NODISCARD size_t GetChunkSize() const { return m_config.chunkSize; }
        ASTRA_NODISCARD size_t GetMinChunkBytes() const noexcept { return m_config.minChunkBytes; }
        ASTRA_NODISCARD size_t GetMaxChunkBytes() const noexcept { return m_config.maxChunkBytes; }
        ASTRA_NODISCARD size_t GetGrowDivisor() const noexcept { return m_config.growDivisor; }

        ASTRA_NODISCARD Stats GetStats() const
        {
            Stats snapshot;
            // m_totalChunks counts LIVE chunks now (the block pool counted carved
            // capacity). Reporting live + "how many more chunks the free bytes
            // could serve" keeps the old reading of totalChunks as pool capacity,
            // which callers use to see that a returned chunk is retained, not
            // released to the OS.
            //
            // freeEquivalent is an UPPER BOUND on additional chunks servable
            // without growing, NOT an achievable count: free bytes can be
            // scattered as sub-chunk-sized tails across several arenas (TLSF's
            // boundary tags + good-fit search), so some of this "capacity" may
            // not be contiguous enough to actually serve a chunk. Do not change
            // this formula -- MemoryCleanupTest's
            // `stats2.totalChunks == stats1.totalChunks` assertion depends on
            // its exact arithmetic.
            const size_t live = m_totalChunks.load(std::memory_order_relaxed);
            const size_t freeEquivalent = m_tlsf.GetFreeBytes() / m_config.chunkSize;  // upper bound, see above
            snapshot.totalChunks = live + freeEquivalent;
            snapshot.freeChunks = freeEquivalent;
            snapshot.acquireCount = m_acquireCount.load(std::memory_order_relaxed);
            snapshot.releaseCount = m_releaseCount.load(std::memory_order_relaxed);
            snapshot.blockAllocations = m_blockAllocations.load(std::memory_order_relaxed);
            snapshot.failedAcquires = m_failedAcquires.load(std::memory_order_relaxed);
            return snapshot;
        }

        struct DefragmentResult
        {
            size_t blocksReleased = 0;
            size_t bytesFreed = 0;
            size_t blocksKept = 0;
            size_t chunksInUse = 0;
        };
        
        DefragmentResult Defragment()
        {
            DefragmentResult result;

            // Collect first: ForEachFullyFreeArena walks Tlsf's arena list by
            // reference, so RemoveArena must not run inside the callback.
            SmallVector<void*, 8> releasable;
            m_tlsf.ForEachFullyFreeArena([&releasable](void* base, size_t) { releasable.push_back(base); });

            // Old block-pool policy, restored: always keep ONE fully-free arena
            // in reserve (anti-thrash) whenever at least one exists -- even
            // while other arenas are still in use serving live chunks. Only the
            // remaining fully-free arenas are released.
            const size_t startIndex = releasable.empty() ? 0 : 1;
            for (size_t i = startIndex; i < releasable.size(); ++i)
            {
                void* base = releasable[i];

                // Find this pool's own record for the arena BEFORE detaching it
                // from Tlsf. Tlsf only ever reports arenas this pool registered,
                // so a miss here would mean the two records have diverged; by
                // checking first (rather than removing from Tlsf then failing to
                // find the record) a divergence leaves the arena registered and
                // owned by Tlsf instead of detaching-and-leaking it with no way
                // back. Unreachable by construction today.
                size_t j = 0;
                for (; j < m_arenas.size(); ++j)
                {
                    if (m_arenas[j].memory == base)
                        break;
                }
                if (j >= m_arenas.size()) ASTRA_UNLIKELY
                {
                    ASTRA_ASSERT(false, "TLSF reported an arena the pool has no record of");
                    continue;   // leave it registered in Tlsf: no detach, no leak
                }

                if (!m_tlsf.RemoveArena(base)) ASTRA_UNLIKELY
                    continue;   // became carved up again: leave it registered

                result.bytesFreed += m_arenas[j].size;
                FreeMemory(m_arenas[j].memory, m_arenas[j].size, m_arenas[j].usedHugePages);
                m_arenas[j] = m_arenas.back();
                m_arenas.pop_back();
                ++result.blocksReleased;
            }

            result.blocksKept = m_arenas.size();
            result.chunksInUse = m_totalChunks.load(std::memory_order_relaxed);
            return result;
        }

    private:
        // One OS region registered with the Tlsf heap. Tlsf never calls the OS
        // itself, so the pool keeps what it needs to hand the region back.
        struct ArenaRecord
        {
            void* memory = nullptr;
            size_t size = 0;
            bool usedHugePages = false;
        };

        // Acquires one region from the OS and registers it with the heap.
        // minBytes is the allocation that triggered the growth; the new arena
        // must be able to serve it.
        bool GrowArena(size_t minBytes)
        {
            // Arena sizing: the legacy chunksPerBlock x chunkSize hint, at least
            // enough for the request plus TLSF bookkeeping (front pad 64 +
            // sentinel 16 + rounding; 256 is comfortably safe), and always inside
            // the range AddArena accepts.
            constexpr size_t kArenaOverhead = 256;
            // Checked against MAX_REQUEST_BYTES (32MB), not MAX_ARENA_BYTES
            // (64MB): Allocate refuses any request whose internally-rounded
            // size exceeds MAX_REQUEST_BYTES, so a minBytes in
            // (MAX_REQUEST_BYTES, MAX_ARENA_BYTES] would pass an assert against
            // the arena ceiling, register an arena successfully, and then fail
            // the retry Allocate anyway -- silently, in Release. Harmless at
            // today's <=1MB chunk sizes; becomes reachable once chunk sizes vary.
            ASTRA_ASSERT(minBytes + kArenaOverhead <= Tlsf::MAX_REQUEST_BYTES, "Chunk request larger than the largest request TLSF can service");
            size_t want = std::max(m_config.chunksPerBlock * m_config.chunkSize, minBytes + kArenaOverhead);
            want = std::clamp(want, Tlsf::MIN_ARENA_BYTES, Tlsf::MAX_ARENA_BYTES);

            // Do NOT pre-round to huge pages here: AllocateMemory already rounds
            // internally when it takes the huge-page path (size >= 2MB), and small
            // arena hints (e.g. chunksPerBlock=4 in tests) must stay small for
            // parity with the old per-block allocation. Register r.size (actual).
            // No ZeroMem either: Chunk's constructor memsets its own bytes.
            AllocFlags flags = AllocFlags::None;
            if (m_config.useHugePages)
            {
                flags = flags | AllocFlags::HugePages;
            }

            AllocResult r = AllocateMemory(want, CACHE_LINE_SIZE, flags);
            if (!r.ptr) ASTRA_UNLIKELY
                return false;

            // AllocateMemory is at least cache-line aligned, which is what
            // AddArena demands; refuse-and-release rather than leak if it is not.
            if (!m_tlsf.AddArena(r.ptr, r.size)) ASTRA_UNLIKELY
            {
                FreeMemory(r.ptr, r.size, r.usedHugePages);
                return false;
            }

            m_arenas.push_back(ArenaRecord{r.ptr, r.size, r.usedHugePages});
            m_blockAllocations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Single allocation path for chunk storage. CreateChunk forwards the
        // caller's per-chunk byte size, which no longer has to be m_config.chunkSize.
        void* AllocateChunkBytes(size_t chunkBytes)
        {
            if (m_totalChunks.load(std::memory_order_relaxed) >= m_config.maxChunks) ASTRA_UNLIKELY
            {
                m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }

            void* p = m_tlsf.Allocate(chunkBytes);
            if (!p)
            {
                if (!GrowArena(chunkBytes)) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
                p = m_tlsf.Allocate(chunkBytes);
                if (!p) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
            }

            m_totalChunks.fetch_add(1, std::memory_order_relaxed);
            m_acquireCount.fetch_add(1, std::memory_order_relaxed);
            return p;
        }

        // Zeroes the counters of a moved-from pool so its GetStats() cannot
        // report chunks it no longer owns (its Tlsf and arenas are empty).
        void ResetCounters() noexcept
        {
            m_totalChunks.store(0, std::memory_order_relaxed);
            m_acquireCount.store(0, std::memory_order_relaxed);
            m_releaseCount.store(0, std::memory_order_relaxed);
            m_blockAllocations.store(0, std::memory_order_relaxed);
            m_failedAcquires.store(0, std::memory_order_relaxed);
        }

        Config m_config;
        Tlsf m_tlsf;
        SmallVector<ArenaRecord, 16> m_arenas;

        std::atomic<size_t> m_totalChunks{0};       // LIVE chunks (see GetStats)
        std::atomic<size_t> m_acquireCount{0};
        std::atomic<size_t> m_releaseCount{0};
        std::atomic<size_t> m_blockAllocations{0};  // arenas acquired from the OS
        std::atomic<size_t> m_failedAcquires{0};
    };
}
