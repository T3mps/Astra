#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
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
#include "../Entity/Entity.hpp"
#include "../Core/Memory.hpp"
#include "../Platform/Hardware.hpp"

namespace Astra
{
    class ArchetypeChunkPool
    {
    public:
        class Chunk;
        
        static constexpr size_t DEFAULT_CHUNK_SIZE = 16 * 1024;  // 16KB default
        static constexpr size_t MIN_CHUNK_SIZE = 4 * 1024;       // 4KB minimum
        static constexpr size_t MAX_CHUNK_SIZE = 1024 * 1024;    // 1MB maximum
        
        // Configuration for pool behavior
        struct Config
        {
            size_t chunkSize = DEFAULT_CHUNK_SIZE; // Size of each chunk (must be power of 2)
            size_t chunksPerBlock = 64; // Chunks allocated together
            size_t maxChunks = 4096;    // Maximum chunks
            size_t initialBlocks = 0;   // Pre-allocate this many blocks
            bool useHugePages = true;   // Try to use huge pages for allocations
            
            // GCC fix: explicit default constructor for brace initialization
            Config() = default;
        };
        
        struct Stats
        {
            size_t totalChunks = 0;      // Total chunks allocated
            size_t freeChunks = 0;       // Currently available chunks
            size_t acquireCount = 0;     // Total acquires
            size_t releaseCount = 0;     // Total releases
            size_t blockAllocations = 0; // Number of block allocations
            size_t failedAcquires = 0;   // Acquires that failed (pool exhausted)
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
                    // Delete chunk first (calls destructor)
                    delete chunk;
                    
                    // Then return memory to pool
                    if (pool && memory) ASTRA_LIKELY
                    {
                        pool->ReleaseChunk(memory);
                    }
                }
            }
        };
        
        // The Chunk class - manages component storage in SoA layout
        class Chunk
        {
        public:
            // Enable move
            Chunk(Chunk&& other) noexcept
                : m_memory(std::exchange(other.m_memory, nullptr))
                , m_capacity(other.m_capacity)
                , m_count(other.m_count)
                , m_componentDescriptors(std::move(other.m_componentDescriptors))
                , m_componentOffsets(std::move(other.m_componentOffsets))
                , m_arrayBases(std::move(other.m_arrayBases))
                , m_entities(std::move(other.m_entities))
                , m_componentArrays(std::move(other.m_componentArrays))
                , m_chunkSize(other.m_chunkSize)
            {}
            
            // Disable copy
            Chunk(const Chunk&) = delete;
            Chunk& operator=(const Chunk&) = delete;

            ~Chunk()
            {
                // Destruct all active components using O(1) lookups
                for (size_t i = 0; i < m_count; ++i)
                {
                    for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                    {
                        const auto& info = m_componentArrays[id];
                        if (!info.isValid) continue;

                        void* ptr = static_cast<std::byte*>(info.base) + i * info.stride;
                        info.descriptor.Destruct(ptr);
                    }
                }
                // Memory is returned to pool by ChunkDeleter
            }

            /**
             * Add entity to chunk (assumes space available)
             * @return Index within chunk
             */
            size_t AddEntity(Entity entity)
            {
                assert(m_count < m_capacity);
                size_t index = m_count++;
                
                // Store entity handle
                m_entities.push_back(entity);
                
                // Default construct components using O(1) lookups
                for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                {
                    const auto& info = m_componentArrays[id];
                    if (!info.isValid) continue;
                    
                    if (info.descriptor.is_empty) ASTRA_UNLIKELY
                    {
                        // Empty types don't need initialization
                        continue;
                    }
                    
                    void* ptr = static_cast<std::byte*>(info.base) + index * info.stride;
                    info.descriptor.DefaultConstruct(ptr);
                }
                
                return index;
            }
            
            void BatchAddEntities(std::span<const Entity> entities)
            {
                size_t count = entities.size();
                assert(m_count + count <= m_capacity);

                // Add entities
                m_entities.insert(m_entities.end(), entities.begin(), entities.end());

                // Batch default construct components using O(1) lookups
                for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                {
                    const auto& info = m_componentArrays[id];
                    if (!info.isValid) continue;

                    std::byte* startPtr = static_cast<std::byte*>(info.base) + m_count * info.stride;
                    info.descriptor.BatchDefaultConstruct(startPtr, count);
                }

                m_count += count;
            }
            
            /**
             * Batch move components from another chunk
             * @param dstIndices Where to place components in this chunk
             * @param srcChunk Source chunk to move from
             * @param srcIndices Which entities to move from source
             * @param componentsToMove Mask of components to move
             */
            void BatchMoveComponentsFrom(
                std::span<const size_t> dstIndices,
                const Chunk& srcChunk,
                std::span<const size_t> srcIndices,
                const ComponentMask& componentsToMove)
            {
                assert(dstIndices.size() == srcIndices.size());
                size_t count = dstIndices.size();
                
                // Move each component type in batch
                // Early exit if no components to move
                if (componentsToMove.None()) return;
                
                // Find first and last set bits to reduce iteration range
                ComponentID firstSet = 0;
                ComponentID lastSet = MAX_COMPONENTS - 1;
                
                // Find first set bit
                for (; firstSet < MAX_COMPONENTS && !componentsToMove.Test(firstSet); ++firstSet);
                // Find last set bit
                for (; lastSet > firstSet && !componentsToMove.Test(lastSet); --lastSet);
                
                // Only iterate through the range that has set bits
                for (ComponentID id = firstSet; id <= lastSet; ++id)
                {
                    if (!componentsToMove.Test(id)) continue;
                    
                    const auto& dstInfo = m_componentArrays[id];
                    const auto& srcInfo = srcChunk.m_componentArrays[id];
                    
                    if (!dstInfo.isValid || !srcInfo.isValid) continue;
                    
                    // Check if we can do a fast batch copy for trivially copyable types
                    if (dstInfo.descriptor.is_trivially_copyable && 
                        AreIndicesContiguous(dstIndices) && 
                        AreIndicesContiguous(srcIndices))
                    {
                        // Fast path: use memcpy for contiguous ranges
                        void* dstPtr = static_cast<std::byte*>(dstInfo.base) + dstIndices[0] * dstInfo.stride;
                        void* srcPtr = static_cast<std::byte*>(srcInfo.base) + srcIndices[0] * srcInfo.stride;
                        std::memcpy(dstPtr, srcPtr, count * dstInfo.stride);
                    }
                    else
                    {
                        // Slow path: move components individually
                        for (size_t i = 0; i < count; ++i)
                        {
                            void* dstPtr = static_cast<std::byte*>(dstInfo.base) + dstIndices[i] * dstInfo.stride;
                            void* srcPtr = static_cast<std::byte*>(srcInfo.base) + srcIndices[i] * srcInfo.stride;
                            
                            if (dstInfo.descriptor.is_trivially_copyable)
                            {
                                // Use memcpy for POD types
                                std::memcpy(dstPtr, srcPtr, dstInfo.stride);
                            }
                            else
                            {
                                // Use move constructor for non-POD types
                                dstInfo.descriptor.MoveConstruct(dstPtr, srcPtr);
                            }
                        }
                    }
                }
            }
            
            // Helper to check if indices are contiguous
            static bool AreIndicesContiguous(std::span<const size_t> indices)
            {
                if (indices.size() <= 1) return true;
                for (size_t i = 1; i < indices.size(); ++i)
                {
                    if (indices[i] != indices[i-1] + 1) return false;
                }
                return true;
            }
            
            /**
             * Batch construct a specific component with given value
             * @param indices Where to construct the component
             * @param value Value to construct with
             */
            template<Component T>
            void BatchConstructComponent(
                std::span<const size_t> indices,
                const T& value)
            {
                ComponentID id = TypeID<T>::Value();
                const auto& info = m_componentArrays[id];
                
                if (!info.isValid) return;
                
                // Optimize for trivially copyable types
                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    // Check if indices are contiguous for super fast path
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
                        // Super fast path: construct first, then memcpy to rest
                        T* firstPtr = static_cast<T*>(static_cast<void*>(
                            static_cast<std::byte*>(info.base) + indices[0] * info.stride));
                        new (firstPtr) T(value);
                        
                        // Copy to remaining contiguous slots
                        for (size_t i = 1; i < indices.size(); ++i)
                        {
                            T* ptr = static_cast<T*>(static_cast<void*>(
                                static_cast<std::byte*>(info.base) + indices[i] * info.stride));
                            std::memcpy(ptr, firstPtr, sizeof(T));
                        }
                    }
                    else
                    {
                        // Fast path for non-contiguous POD types
                        for (size_t idx : indices)
                        {
                            assert(idx < m_capacity);
                            T* ptr = static_cast<T*>(static_cast<void*>(
                                static_cast<std::byte*>(info.base) + idx * info.stride));
                            std::memcpy(ptr, &value, sizeof(T));
                        }
                    }
                }
                else
                {
                    // Slow path: construct each component
                    for (size_t idx : indices)
                    {
                        assert(idx < m_capacity);
                        T* ptr = static_cast<T*>(static_cast<void*>(
                            static_cast<std::byte*>(info.base) + idx * info.stride));
                        new (ptr) T(value);
                    }
                }
            }
            
            /**
             * Batch move a specific component from source chunk
             * @param dstIndices Where to place components
             * @param srcChunk Source chunk
             * @param srcIndices Which components to move
             */
            template<Component T>
            void BatchMoveComponent(
                std::span<const size_t> dstIndices,
                const Chunk& srcChunk,
                std::span<const size_t> srcIndices)
            {
                assert(dstIndices.size() == srcIndices.size());
                ComponentID id = TypeID<T>::Value();
                
                const auto& dstInfo = m_componentArrays[id];
                const auto& srcInfo = srcChunk.m_componentArrays[id];
                
                if (!dstInfo.isValid || !srcInfo.isValid) return;
                
                // Batch move specific component type
                for (size_t i = 0; i < dstIndices.size(); ++i)
                {
                    T* dstPtr = static_cast<T*>(static_cast<void*>(
                        static_cast<std::byte*>(dstInfo.base) + dstIndices[i] * dstInfo.stride));
                    T* srcPtr = static_cast<T*>(static_cast<void*>(
                        static_cast<std::byte*>(srcInfo.base) + srcIndices[i] * srcInfo.stride));
                    new (dstPtr) T(std::move(*srcPtr));
                }
            }

            /**
             * Remove entity from chunk (swap with last)
             * @return The entity that was moved to fill the gap (if any)
             */
            std::optional<Entity> RemoveEntity(size_t index)
            {
                assert(index < m_count);
                
                const size_t lastIndex = m_count - 1;
                std::optional<Entity> movedEntity;
                
                if (index != lastIndex) ASTRA_LIKELY
                {
                    // Move last entity to this position
                    m_entities[index] = m_entities[lastIndex];
                    movedEntity = m_entities[index];
                    
                    // Move components using O(1) lookups
                    for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                    {
                        const auto& info = m_componentArrays[id];
                        if (!info.isValid) continue;
                        
                        void* dstPtr = static_cast<std::byte*>(info.base) + index * info.stride;
                        void* srcPtr = static_cast<std::byte*>(info.base) + lastIndex * info.stride;
                        
                        // Destruct destination, move from source
                        info.descriptor.Destruct(dstPtr);
                        info.descriptor.MoveConstruct(dstPtr, srcPtr);
                    }
                }
                else
                {
                    // Just destruct the last entity's components using O(1) lookups
                    for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                    {
                        const auto& info = m_componentArrays[id];
                        if (!info.isValid) continue;
                        
                        void* ptr = static_cast<std::byte*>(info.base) + lastIndex * info.stride;
                        info.descriptor.Destruct(ptr);
                    }
                }
                
                // Remove last entity
                m_entities.pop_back();
                --m_count;
                
                return movedEntity;
            }
            
            /**
             * Get component pointer for specific entity
             */
            template<Component T>
            T* GetComponent(size_t index)
            {
                assert(index < m_count);
                ComponentID id = TypeID<T>::Value();
                return static_cast<T*>(GetComponentPointer(id, index));
            }
            
            /**
             * Get base pointer to component array
             * Supports both const and non-const access based on template parameter
             */
            template<Component T>
            ASTRA_FORCEINLINE auto GetComponentArray()
            {
                using BaseType = std::remove_const_t<T>;
                ComponentID id = TypeID<BaseType>::Value();
                
                if constexpr (std::is_const_v<T>)
                {
                    return reinterpret_cast<const BaseType*>(m_componentArrays[id].base);
                }
                else
                {
                    return reinterpret_cast<BaseType*>(m_componentArrays[id].base);
                }
            }
            
            /**
             * Const overload for getting component array
             */
            template<Component T>
            ASTRA_FORCEINLINE const std::remove_const_t<T>* GetComponentArray() const
            {
                using BaseType = std::remove_const_t<T>;
                ComponentID id = TypeID<BaseType>::Value();
                return reinterpret_cast<const BaseType*>(m_componentArrays[id].base);
            }
            
            /**
             * Get component pointer using cached array base (optimized)
             */
            void* GetComponentPointerCached(size_t componentIndex, size_t entityIndex) const
            {
                assert(componentIndex < m_arrayBases.size());
                assert(entityIndex < m_count);
                return static_cast<std::byte*>(m_arrayBases[componentIndex]) + entityIndex * m_componentDescriptors[componentIndex].size;
            }
            
            /**
             * Get component array base by component index
             */
            template<typename T>
            T* GetComponentArrayByIndex(size_t componentIndex)
            {
                assert(componentIndex < m_arrayBases.size());
                return reinterpret_cast<T*>(m_arrayBases[componentIndex]);
            }
            
            /**
             * Get component array by ID (direct access)
             */
            void* GetComponentArrayById(ComponentID id) const
            {
                return m_componentArrays[id].base;
            }
            
            ASTRA_NODISCARD bool IsFull() const noexcept { return m_count >= m_capacity; }
            ASTRA_NODISCARD bool IsEmpty() const noexcept { return m_count == 0; }
            ASTRA_NODISCARD size_t GetCount() const noexcept { return m_count; }
            ASTRA_NODISCARD size_t GetCapacity() const noexcept { return m_capacity; }
            ASTRA_NODISCARD Entity GetEntity(size_t index) const { assert(index < m_count); return m_entities[index]; }
            ASTRA_NODISCARD const std::vector<Entity>& GetEntities() const { return m_entities; }
            ASTRA_FORCEINLINE ASTRA_NODISCARD std::vector<Entity>& GetEntities() { return m_entities; }
            
            // Optimized component array info for O(1) lookups
            struct ComponentArrayInfo
            {
                void* base{nullptr};
                size_t stride{0};  // Component size for pointer arithmetic
                ComponentDescriptor descriptor{};  // Full descriptor for O(1) component operations
                bool isValid{false};  // Whether this component exists in the archetype
            };
            
            ASTRA_NODISCARD const auto& GetComponentArrays() const { return m_componentArrays; }
            
            // Used for chunk coalescing
            void SetCount(size_t count) noexcept { m_count = count; }
            
            void* GetComponentPointer(ComponentID id, size_t index) const
            {
                // O(1) lookup with pre-cached stride!
                const auto& info = m_componentArrays[id];
                if (!info.base) ASTRA_UNLIKELY return nullptr;
                
                return static_cast<std::byte*>(info.base) + index * info.stride;
            }
            
        private:
            // Private constructor - only callable from factory
            Chunk(size_t entitiesPerChunk, const std::vector<ComponentDescriptor>& componentDescriptors, void* memory, size_t chunkSize)
                : m_memory(memory)
                , m_capacity(entitiesPerChunk)
                , m_count(0)
                , m_componentDescriptors(componentDescriptors)
                , m_chunkSize(chunkSize)
            {
                // Reserve space for entities
                m_entities.reserve(m_capacity);
                
                // Calculate layout for SoA within chunk
                CalculateLayout();

                // Clear memory
                std::memset(m_memory, 0, m_chunkSize);

                // Pre-compute array base pointers and strides
                m_arrayBases.resize(m_componentDescriptors.size());
                for (size_t i = 0; i < m_componentDescriptors.size(); ++i)
                {
                    void* arrayBase = static_cast<std::byte*>(m_memory) + m_componentOffsets[m_componentDescriptors[i].id];
                    m_arrayBases[i] = arrayBase;
                    // Store full descriptor info for O(1) access by ComponentID
                    m_componentArrays[m_componentDescriptors[i].id] = {
                        arrayBase, 
                        m_componentDescriptors[i].size,
                        m_componentDescriptors[i],
                        true
                    };
                }
            }

            void CalculateLayout()
            {
                size_t offset = 0;
                
                for (const auto& comp : m_componentDescriptors)
                {
                    // Align offset to cache line boundary for better cache performance
                    // This prevents false sharing between component arrays
                    offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                    
                    // Store offset for this component's array - direct array access
                    m_componentOffsets[comp.id] = offset;
                    
                    // Advance by total size of component array
                    offset += comp.size * m_capacity;
                }
                
                // Ensure we don't exceed chunk size
                assert(offset <= m_chunkSize);
            }

            void* m_memory;
            size_t m_capacity;  // Max entities per chunk
            size_t m_count;     // Current entity count
            std::vector<Entity> m_entities;  // Entity handles stored in this chunk
            std::vector<ComponentDescriptor> m_componentDescriptors;
            std::array<size_t, MAX_COMPONENTS> m_componentOffsets{};  // Direct array access by ComponentID
            std::array<ComponentArrayInfo, MAX_COMPONENTS> m_componentArrays{};   // Direct pointers + stride indexed by ComponentID
            std::vector<void*> m_arrayBases;  // Cached base pointers indexed by component index
            size_t m_chunkSize;  // Size of this chunk
            
            friend class ArchetypeChunkPool;
        };
        
    // GCC fix: Provide both explicit config constructor and default constructor
    explicit ArchetypeChunkPool() : ArchetypeChunkPool(Config()) {}
    
    explicit ArchetypeChunkPool(Config config) :
            m_config(config),
            m_freeList(nullptr)
        {
            // Validate chunk size (must be power of 2 and within range)
            ASTRA_ASSERT(m_config.chunkSize >= MIN_CHUNK_SIZE && m_config.chunkSize <= MAX_CHUNK_SIZE, 
                         "Chunk size must be between 4KB and 1MB");
            ASTRA_ASSERT((m_config.chunkSize & (m_config.chunkSize - 1)) == 0, 
                         "Chunk size must be a power of 2");
            
            // Validate configuration
            if (m_config.chunksPerBlock == 0)
            {
                // If not specified, adjust based on chunk size to target ~1MB blocks
                size_t targetBlockSize = 1024 * 1024;
                m_config.chunksPerBlock = std::max(size_t(1), targetBlockSize / m_config.chunkSize);
            }
            if (m_config.maxChunks < m_config.chunksPerBlock)
            {
                m_config.maxChunks = m_config.chunksPerBlock;
            }
            
            // Pre-allocate initial blocks if requested
            for (size_t i = 0; i < m_config.initialBlocks; ++i)
            {
                AllocateBlock();
            }
        }
        
        ~ArchetypeChunkPool()
        {
            // Free all allocated blocks
            for (const auto& block : m_blocks)
            {
                FreeMemory(block.memory, block.size, block.usedHugePages);
            }
        }
        
        // Disable copy
        ArchetypeChunkPool(const ArchetypeChunkPool&) = delete;
        ArchetypeChunkPool& operator=(const ArchetypeChunkPool&) = delete;
        
        // Enable move
        ArchetypeChunkPool(ArchetypeChunkPool&& other) noexcept :
            m_config(other.m_config),
            m_blocks(std::move(other.m_blocks)),
            m_freeList(other.m_freeList),
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
                // Free existing blocks
                for (const auto& block : m_blocks)
                {
                    FreeMemory(block.memory, block.size, block.usedHugePages);
                }
                
                m_config = other.m_config;
                m_blocks = std::move(other.m_blocks);
                m_freeList = other.m_freeList;
                // Copy atomic values
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
        
        /**
         * Create a chunk configured for the given component layout
         * @param entitiesPerChunk Maximum entities this chunk can hold
         * @param componentDescriptors Component layout for the chunk
         * @return Unique pointer to configured chunk, or nullptr if pool exhausted
         */
        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(
            size_t entitiesPerChunk, 
            const std::vector<ComponentDescriptor>& componentDescriptors)
        {
            void* memory = AcquireMemory();
            if (!memory) ASTRA_UNLIKELY
            {
                return nullptr;
            }
            
            // Create chunk with allocated memory and custom deleter
            auto* chunk = new Chunk(entitiesPerChunk, componentDescriptors, memory, m_config.chunkSize);
            ChunkDeleter deleter{this, memory};
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, deleter);
        }
        
        /**
         * Release a chunk back to the pool
         * Note: Usually called automatically by ChunkDeleter
         */
        void ReleaseChunk(void* memory)
        {
            if (!memory) ASTRA_UNLIKELY
                return;
            
            // Clear the memory before returning to pool
            std::memset(memory, 0, m_config.chunkSize);
            
            // Add to free list
            auto* node = reinterpret_cast<FreeNode*>(memory);
            node->next = m_freeList;
            m_freeList = node;
            
            m_freeChunks.fetch_add(1, std::memory_order_relaxed);
            m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        }
        
        /**
         * Get the configured chunk size for this pool
         */
        ASTRA_NODISCARD size_t GetChunkSize() const { return m_config.chunkSize; }
        
        /**
         * Get current pool statistics
         */
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
        
    private:
        // Node in the free list
        struct FreeNode
        {
            FreeNode* next;
        };
        
        // Block allocation info
        struct BlockInfo
        {
            void* memory = nullptr;
            size_t size = 0;
            size_t chunkCount = 0;
            bool usedHugePages = false;
        };
        
        /**
         * Acquire raw memory from the pool
         */
        void* AcquireMemory()
        {
            // Check free list first
            if (m_freeList) ASTRA_LIKELY
            {
                FreeNode* node = m_freeList;
                m_freeList = m_freeList->next;
                
                m_freeChunks.fetch_sub(1, std::memory_order_relaxed);
                m_acquireCount.fetch_add(1, std::memory_order_relaxed);
                
                return node;
            }
            
            // Free list empty, try to allocate new block
            if (m_totalChunks < m_config.maxChunks) ASTRA_UNLIKELY
            {
                if (AllocateBlock())
                {
                    // Retry acquire after allocating block
                    return AcquireMemory();
                }
            }
            
            // Pool exhausted
            m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        
        /**
         * Allocate a new block of chunks
         */
        bool AllocateBlock()
        {
            size_t remainingCapacity = m_config.maxChunks - m_totalChunks;
            if (remainingCapacity == 0) ASTRA_UNLIKELY
                return false;
            
            size_t chunksToAllocate = std::min(m_config.chunksPerBlock, remainingCapacity);
            size_t blockSize = chunksToAllocate * m_config.chunkSize;
            
            // Allocate block using huge pages if configured
            AllocFlags flags = AllocFlags::ZeroMem;
            if (m_config.useHugePages)
            {
                flags = flags | AllocFlags::HugePages;
            }
            
            AllocResult result = AllocateMemory(blockSize, CACHE_LINE_SIZE, flags);
            if (!result.ptr) ASTRA_UNLIKELY
            {
                return false;
            }
            
            // Create block info
            BlockInfo blockInfo;
            blockInfo.memory = result.ptr;
            blockInfo.size = result.size;
            blockInfo.chunkCount = chunksToAllocate;
            blockInfo.usedHugePages = result.usedHugePages;
            
            // Add all chunks to free list
            auto* chunks = static_cast<std::byte*>(result.ptr);
            for (size_t i = 0; i < chunksToAllocate; ++i)
            {
                auto* node = reinterpret_cast<FreeNode*>(chunks + i * m_config.chunkSize);
                node->next = m_freeList;
                m_freeList = node;
            }
            
            // Update statistics
            m_totalChunks.fetch_add(chunksToAllocate, std::memory_order_relaxed);
            m_freeChunks.fetch_add(chunksToAllocate, std::memory_order_relaxed);
            m_blockAllocations.fetch_add(1, std::memory_order_relaxed);
            
            // Store block
            m_blocks.push_back(blockInfo);
            
            return true;
        }
        
        Config m_config;
        SmallVector<BlockInfo, 16> m_blocks;
        FreeNode* m_freeList;
        
        // Atomic statistics
        std::atomic<size_t> m_totalChunks{0};
        std::atomic<size_t> m_freeChunks{0};
        std::atomic<size_t> m_acquireCount{0};
        std::atomic<size_t> m_releaseCount{0};
        std::atomic<size_t> m_blockAllocations{0};
        std::atomic<size_t> m_failedAcquires{0};
    };
}