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
#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
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

        explicit ArchetypeChunkPool(const Config& config) : m_config(config), m_freeList(nullptr)
        {
            ASTRA_ASSERT(m_config.chunkSize >= MIN_CHUNK_SIZE && m_config.chunkSize <= MAX_CHUNK_SIZE, "Chunk size must be between 4KB and 1MB");
            ASTRA_ASSERT((m_config.chunkSize & (m_config.chunkSize - 1)) == 0, "Chunk size must be a power of 2");
            
            if (m_config.chunksPerBlock == 0)
            {
                m_config.chunksPerBlock = std::max(size_t(1), HUGE_PAGE_SIZE / m_config.chunkSize);
            }
            if (m_config.maxChunks < m_config.chunksPerBlock)
            {
                m_config.maxChunks = m_config.chunksPerBlock;
            }
            
            // Reserve space for maximum possible blocks to prevent reallocation
            // This ensures ChunkNode pointers remain stable
            size_t maxBlocks = (m_config.maxChunks + m_config.chunksPerBlock - 1) / m_config.chunksPerBlock;
            m_blocks.reserve(maxBlocks);
            
            for (size_t i = 0; i < m_config.initialBlocks; ++i)
            {
                AllocateBlock();
            }
        }
        
        ~ArchetypeChunkPool()
        {
            // Clear the map first (no need to maintain it during destruction)
            m_memoryToNode.Clear();
            
            for (const auto& block : m_blocks)
            {
                FreeMemory(block.memory, block.size, block.usedHugePages);
            }
        }
        
        ArchetypeChunkPool(const ArchetypeChunkPool&) = delete;
        ArchetypeChunkPool& operator=(const ArchetypeChunkPool&) = delete;
        
        ArchetypeChunkPool(ArchetypeChunkPool&& other) noexcept :
            m_config(other.m_config),
            m_blocks(std::move(other.m_blocks)),
            m_freeList(other.m_freeList),
            m_memoryToNode(std::move(other.m_memoryToNode)),
            m_totalChunks(other.m_totalChunks.load()),
            m_freeChunks(other.m_freeChunks.load()),
            m_acquireCount(other.m_acquireCount.load()),
            m_releaseCount(other.m_releaseCount.load()),
            m_blockAllocations(other.m_blockAllocations.load()),
            m_failedAcquires(other.m_failedAcquires.load())
        {
            other.m_freeList = nullptr;
        }
        
        ArchetypeChunkPool& operator=(ArchetypeChunkPool&& other) noexcept
        {
            if (this != &other)
            {
                for (const auto& block : m_blocks)
                {
                    FreeMemory(block.memory, block.size, block.usedHugePages);
                }
                
                m_config = other.m_config;
                m_blocks = std::move(other.m_blocks);
                m_memoryToNode = std::move(other.m_memoryToNode);
                m_freeList = other.m_freeList;
                m_totalChunks.store(other.m_totalChunks.load());
                m_freeChunks.store(other.m_freeChunks.load());
                m_acquireCount.store(other.m_acquireCount.load());
                m_releaseCount.store(other.m_releaseCount.load());
                m_blockAllocations.store(other.m_blockAllocations.load());
                m_failedAcquires.store(other.m_failedAcquires.load());
                other.m_freeList = nullptr;
            }
            return *this;
        }

        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(size_t entitiesPerChunk, const ArchetypeColumnMeta* meta)
        {
            void* memory = AcquireMemory();
            if (!memory) ASTRA_UNLIKELY
            {
                return nullptr;
            }

            auto* chunk = new Chunk(entitiesPerChunk, meta, memory, m_config.chunkSize);
            ChunkDeleter deleter{this, memory};
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, deleter);
        }

        void ReturnChunk(void* memory)
        {
            if (!memory) ASTRA_UNLIKELY
                return;
            
            // O(1) lookup using FlatMap
            auto it = m_memoryToNode.Find(memory);
            if (it == m_memoryToNode.end()) ASTRA_UNLIKELY
            {
                // This should never happen in normal operation
                // but we need to handle it gracefully in all builds
                ASTRA_ASSERT(false, "Returned chunk not found in memory map");
                return;  // Always return, not just in debug builds
            }
            
            ChunkNode* node = it->second;
            
            // Validate block index before use
            if (node->blockIndex >= m_blocks.size()) ASTRA_UNLIKELY
            {
                // Block was removed during defragmentation
                ASTRA_ASSERT(false, "Block index out of range - block was likely removed");
                return;
            }
            
            // Track block usage (atomic decrement for thread safety)
            m_blocks[node->blockIndex].usedChunks.fetch_sub(1, std::memory_order_relaxed);
            
            // Mark for lazy clearing instead of clearing now
            node->needsClear = true;
            
            // Add to free list
            node->next = m_freeList;
            m_freeList = node;
            
            m_freeChunks.fetch_add(1, std::memory_order_relaxed);
            m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        }
        
        ASTRA_NODISCARD size_t GetChunkSize() const { return m_config.chunkSize; }
        
        ASTRA_NODISCARD Stats GetStats() const
        {
            Stats snapshot;
            snapshot.totalChunks = m_totalChunks.load(std::memory_order_relaxed);
            snapshot.freeChunks = m_freeChunks.load(std::memory_order_relaxed);
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
            
            // Can't defragment if we have no blocks
            if (m_blocks.empty())
                return result;
            
            // Identify completely empty blocks
            std::vector<size_t> emptyBlockIndices;
            for (size_t i = 0; i < m_blocks.size(); ++i)
            {
                size_t used = m_blocks[i].usedChunks.load(std::memory_order_acquire);
                if (used == 0)
                {
                    emptyBlockIndices.push_back(i);
                }
                else
                {
                    result.chunksInUse += used;
                }
            }
            
            // Keep at least one block as reserve to avoid allocation thrashing
            size_t blocksToRelease = 0;
            if (emptyBlockIndices.size() > 1)
            {
                // Keep one empty block, release the rest
                blocksToRelease = emptyBlockIndices.size() - 1;
            }
            else if (emptyBlockIndices.size() == 1 && m_blocks.size() > 1)
            {
                // We have other blocks with chunks in use, can release the empty one
                blocksToRelease = 1;
            }
            
            if (blocksToRelease == 0)
            {
                result.blocksKept = m_blocks.size();
                return result;
            }
            
            // Remove nodes from free list for blocks we're releasing
            ChunkNode* newFreeList = nullptr;
            ChunkNode* newFreeListTail = nullptr;

            // Build new free list without nodes from blocks being released
            ChunkNode* current = m_freeList;
            while (current)
            {
                bool shouldKeep = true;

                // Check if this node belongs to a block being released
                for (size_t i = 0; i < blocksToRelease; ++i)
                {
                    if (current->blockIndex == emptyBlockIndices[i])
                    {
                        shouldKeep = false;
                        break;
                    }
                }
                
                ChunkNode* next = current->next;
                
                if (shouldKeep)
                {
                    if (!newFreeList)
                    {
                        newFreeList = current;
                        newFreeListTail = current;
                    }
                    else
                    {
                        newFreeListTail->next = current;
                        newFreeListTail = current;
                    }
                    current->next = nullptr;
                }
                
                current = next;
            }
            
            m_freeList = newFreeList;
            
            // Track which blocks we actually release
            std::vector<size_t> actuallyReleasedIndices;
            actuallyReleasedIndices.reserve(blocksToRelease);
            
            // Process blocks to be released
            for (size_t i = 0; i < blocksToRelease; ++i)
            {
                size_t idx = emptyBlockIndices[i];
                auto& block = m_blocks[idx];
                
                // Double-check that block is truly empty (use acquire to synchronize with Release operations)
                // A chunk was acquired after our initial check -- expected under concurrent
                // Acquire/Release, not a guard failure. Skip the block and move on.
                size_t blockUsed = block.usedChunks.load(std::memory_order_acquire);
                if (blockUsed != 0) ASTRA_UNLIKELY
                {
                    continue;
                }
                
                // Remove all nodes from this block from the memory map
                for (size_t j = 0; j < block.chunkCount; ++j)
                {
                    m_memoryToNode.Erase(block.nodes[j].memory);
                }
                
                result.bytesFreed += block.size;
                result.blocksReleased++;
                
                // Free the memory
                FreeMemory(block.memory, block.size, block.usedHugePages);
                
                // Update statistics
                m_totalChunks.fetch_sub(block.chunkCount, std::memory_order_relaxed);
                m_freeChunks.fetch_sub(block.chunkCount, std::memory_order_relaxed);
                
                // Track that we actually released this block
                actuallyReleasedIndices.push_back(idx);
            }
            
            // Batch remove blocks using erase-remove idiom for better performance
            // Mark blocks to remove by setting memory to nullptr
            for (size_t idx : actuallyReleasedIndices)
            {
                m_blocks[idx].memory = nullptr;
            }
            
            // Remove all marked blocks in one pass
            m_blocks.erase(
                std::remove_if(m_blocks.begin(), m_blocks.end(),
                    [](const BlockInfo& block) { return block.memory == nullptr; }),
                m_blocks.end()
            );
            
            // Update block indices for all blocks that may have shifted position
            // After removal, blocks that were after removed blocks have new indices
            // We need to update all blocks, as we don't know which ones shifted
            for (size_t i = 0; i < m_blocks.size(); ++i)
            {
                for (size_t j = 0; j < m_blocks[i].chunkCount; ++j)
                {
                    m_blocks[i].nodes[j].blockIndex = i;
                }
            }
            
            result.blocksKept = m_blocks.size();
            
            return result;
        }
        
    private:
        // Non-intrusive free list node to avoid corrupting chunk memory
        struct ChunkNode
        {
            void* memory = nullptr;      // Pointer to the actual chunk memory
            ChunkNode* next = nullptr;   // Next free chunk
            bool needsClear = false;      // Whether chunk needs clearing before use
            size_t blockIndex = 0;        // Which block this chunk belongs to
            
            // Explicit default constructor to ensure initialization
            ChunkNode() : memory(nullptr), next(nullptr), needsClear(false), blockIndex(0) {}
        };
        
        struct BlockInfo
        {
            void* memory = nullptr;
            size_t size = 0;
            size_t chunkCount = 0;
            bool usedHugePages = false;
            std::unique_ptr<ChunkNode[]> nodes;  // Heap-allocated nodes for stable addresses
            std::atomic<size_t> usedChunks{0};   // Number of chunks currently in use (atomic for thread safety)

            // Need explicit move operations because atomic is not moveable
            BlockInfo() = default;
            BlockInfo(BlockInfo&& other) noexcept
                : memory(other.memory)
                , size(other.size)
                , chunkCount(other.chunkCount)
                , usedHugePages(other.usedHugePages)
                , nodes(std::move(other.nodes))
                , usedChunks(other.usedChunks.load(std::memory_order_relaxed))
            {
                other.memory = nullptr;
                other.size = 0;
                other.chunkCount = 0;
                other.usedHugePages = false;
            }
            BlockInfo& operator=(BlockInfo&& other) noexcept
            {
                if (this != &other)
                {
                    memory = other.memory;
                    size = other.size;
                    chunkCount = other.chunkCount;
                    usedHugePages = other.usedHugePages;
                    nodes = std::move(other.nodes);
                    usedChunks.store(other.usedChunks.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    other.memory = nullptr;
                    other.size = 0;
                    other.chunkCount = 0;
                    other.usedHugePages = false;
                }
                return *this;
            }
            BlockInfo(const BlockInfo&) = delete;
            BlockInfo& operator=(const BlockInfo&) = delete;
        };

        void* AcquireMemory()
        {
            // Check free list first
            if (m_freeList) ASTRA_LIKELY
            {
                ChunkNode* node = m_freeList;
                m_freeList = node->next;
                
                // Clear memory only if needed (was previously used)
                if (node->needsClear)
                {
                    std::memset(node->memory, 0, m_config.chunkSize);
                    node->needsClear = false;
                }
                
                // Track block usage (atomic increment for thread safety)
                m_blocks[node->blockIndex].usedChunks.fetch_add(1, std::memory_order_relaxed);
                
                void* memory = node->memory;
                node->next = nullptr;  // Clear the link
                
                m_freeChunks.fetch_sub(1, std::memory_order_relaxed);
                m_acquireCount.fetch_add(1, std::memory_order_relaxed);
                
                return memory;
            }
            
            if (m_totalChunks < m_config.maxChunks) ASTRA_UNLIKELY
            {
                if (AllocateBlock())
                {
                    return AcquireMemory();
                }
            }
            
            m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        bool AllocateBlock()
        {
            size_t remainingCapacity = m_config.maxChunks - m_totalChunks;
            if (remainingCapacity == 0) ASTRA_UNLIKELY
                return false;
            
            size_t chunksToAllocate = std::min(m_config.chunksPerBlock, remainingCapacity);
            size_t blockSize = chunksToAllocate * m_config.chunkSize;
            
            // Blocks are cache-line aligned; chunk bases inherit this, so the
            // strongest alignment a component array can rely on is
            // CACHE_LINE_SIZE (64). (Windows VirtualAlloc gives 64KB anyway;
            // this matters on the POSIX posix_memalign fallback.)
            constexpr size_t BLOCK_ALIGNMENT = CACHE_LINE_SIZE;
            AllocFlags flags = AllocFlags::ZeroMem;
            if (m_config.useHugePages)
            {
                flags = flags | AllocFlags::HugePages;
            }

            AllocResult result = AllocateMemory(blockSize, BLOCK_ALIGNMENT, flags);
            if (!result.ptr) ASTRA_UNLIKELY
                return false;
            
            BlockInfo blockInfo;
            blockInfo.memory = result.ptr;
            blockInfo.size = result.size;
            blockInfo.chunkCount = chunksToAllocate;
            blockInfo.usedHugePages = result.usedHugePages;
            
            // Create chunk nodes for non-intrusive list
            blockInfo.nodes = std::make_unique<ChunkNode[]>(chunksToAllocate);
            auto* chunks = static_cast<std::byte*>(result.ptr);
            
            size_t blockIndex = m_blocks.size();  // Index this block will have
            for (size_t i = 0; i < chunksToAllocate; ++i)
            {
                blockInfo.nodes[i].memory = chunks + i * m_config.chunkSize;
                blockInfo.nodes[i].needsClear = false;  // Already zeroed by AllocateMemory
                blockInfo.nodes[i].next = nullptr;
                blockInfo.nodes[i].blockIndex = blockIndex;
            }
            
            // Store the old free list head before we move the block
            ChunkNode* oldFreeList = m_freeList;
            
            // Ensure we don't trigger reallocation which would invalidate pointers
            ASTRA_ASSERT(m_blocks.size() < m_blocks.capacity(), 
                "Block vector would reallocate, invalidating node pointers in FlatMap");
            
            m_blocks.push_back(std::move(blockInfo));
            
            // Now set up the linked list in the newly added block
            auto& newBlock = m_blocks.back();
            ASTRA_ASSERT(newBlock.nodes != nullptr, "Nodes array is null after move");
            for (size_t i = 0; i < chunksToAllocate; ++i)
            {
                ASTRA_ASSERT(newBlock.nodes[i].memory != nullptr, "Node memory is null");
                // Add to memory-to-node map for fast lookup
                m_memoryToNode.Insert({newBlock.nodes[i].memory, &newBlock.nodes[i]});
                
                if (i < chunksToAllocate - 1)
                {
                    newBlock.nodes[i].next = &newBlock.nodes[i + 1];
                }
                else
                {
                    // Last node points to the old free list head
                    newBlock.nodes[i].next = oldFreeList;
                }
            }
            
            // Update free list head to point to first node of new block
            m_freeList = &newBlock.nodes[0];
            
            m_totalChunks.fetch_add(chunksToAllocate, std::memory_order_relaxed);
            m_freeChunks.fetch_add(chunksToAllocate, std::memory_order_relaxed);
            m_blockAllocations.fetch_add(1, std::memory_order_relaxed);
            
            return true;
        }
        
        Config m_config;
        SmallVector<BlockInfo, 16> m_blocks;
        ChunkNode* m_freeList;  // Head of free list
        
        // Fast lookup from memory pointer to ChunkNode
        // Using FlatMap since this is "build once (on block allocation), query many (on every return)"
        FlatMap<void*, ChunkNode*> m_memoryToNode;
        
        std::atomic<size_t> m_totalChunks{0};
        std::atomic<size_t> m_freeChunks{0};
        std::atomic<size_t> m_acquireCount{0};
        std::atomic<size_t> m_releaseCount{0};
        std::atomic<size_t> m_blockAllocations{0};
        std::atomic<size_t> m_failedAcquires{0};
    };
}
