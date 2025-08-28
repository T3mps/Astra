#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
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
#include "../Core/Simd.hpp"
#include "../Core/TypeID.hpp"
#include "../Entity/Entity.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "ArchetypeChunkPool.hpp"

namespace Astra
{
    class ArchetypeManager;
    class Registry;

    using ArchetypeChunk = ArchetypeChunkPool::Chunk;

    struct EntityLocation
    {
        uint32_t chunkIndex;
        uint32_t entityIndex;

        constexpr EntityLocation() noexcept :
            chunkIndex(std::numeric_limits<uint32_t>::max()),
            entityIndex(std::numeric_limits<uint32_t>::max())
        {}

        constexpr EntityLocation(uint32_t chunk, uint32_t entity) noexcept : chunkIndex(chunk), entityIndex(entity) {}

        ASTRA_NODISCARD constexpr static EntityLocation Create(size_t chunkIndex, size_t entityIndex) noexcept
        {
            return EntityLocation(static_cast<uint32_t>(chunkIndex), static_cast<uint32_t>(entityIndex));
        }

        ASTRA_NODISCARD constexpr size_t GetChunkIndex() const noexcept
        {
            return chunkIndex;
        }

        ASTRA_NODISCARD constexpr size_t GetEntityIndex() const noexcept
        {
            return entityIndex;
        }

        ASTRA_NODISCARD constexpr bool IsValid() const noexcept
        {
            return chunkIndex != std::numeric_limits<uint32_t>::max();
        }

        constexpr bool operator==(const EntityLocation& other) const noexcept 
        { 
            return chunkIndex == other.chunkIndex && entityIndex == other.entityIndex; 
        }
        constexpr bool operator!=(const EntityLocation& other) const noexcept 
        { 
            return !(*this == other); 
        }
        constexpr bool operator<(const EntityLocation& other) const noexcept 
        { 
            return chunkIndex < other.chunkIndex || (chunkIndex == other.chunkIndex && entityIndex < other.entityIndex); 
        }
        constexpr bool operator>(const EntityLocation& other) const noexcept 
        { 
            return other < *this; 
        }
        constexpr bool operator<=(const EntityLocation& other) const noexcept 
        { 
            return !(other < *this); 
        }
        constexpr bool operator>=(const EntityLocation& other) const noexcept 
        { 
            return !(*this < other); 
        }
    };
    
    template<Component... Components>
    ASTRA_NODISCARD constexpr ComponentMask MakeComponentMask() noexcept
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
            m_entitiesPerChunk(0),
            m_entitiesPerChunkShift(0),
            m_entitiesPerChunkMask(0),
            m_initialized(false)
        {}
        
        ~Archetype() = default;
        
        void Initialize(const std::vector<ComponentDescriptor>& componentDescriptors)
        {
            if (m_initialized) ASTRA_UNLIKELY
                return;

            m_componentDescriptors = componentDescriptors;

            size_t totalOffset = 0;
            size_t perEntitySize = 0;

            for (size_t i = 0; i < m_componentDescriptors.size(); ++i)
            {
                if (m_componentDescriptors[i].size == 0)
                {
                    continue;
                }
                    
                totalOffset = (totalOffset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
                totalOffset += CACHE_LINE_SIZE;

                perEntitySize += m_componentDescriptors[i].size;
            }

            size_t chunkSize = m_chunkPool ? m_chunkPool->GetChunkSize() : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
            size_t remainingSpace = chunkSize - totalOffset;
            size_t maxEntities = perEntitySize > 0 ? remainingSpace / perEntitySize : 256;

            // Round down to nearest power of 2 for fast modulo/division operations
            m_entitiesPerChunk = maxEntities > 0 ? std::bit_floor(maxEntities) : 1;
            m_entitiesPerChunk = std::max(size_t(1), m_entitiesPerChunk);

            m_entitiesPerChunkMask = m_entitiesPerChunk - 1;
            m_entitiesPerChunkShift = std::countr_zero(m_entitiesPerChunk);

            m_initialized = true;

            auto chunk = m_chunkPool->CreateChunk(m_entitiesPerChunk, m_componentDescriptors);
            if (!chunk) ASTRA_UNLIKELY
            {
                m_initialized = false;
                return;
            }
            m_chunks.emplace_back(std::move(chunk));
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

            // Calculate and allocate needed chunks upfront
            size_t remainingCapacity = GetRemainingCapacity();
            if (count > remainingCapacity) ASTRA_UNLIKELY
            {
                size_t additionalNeeded = count - remainingCapacity;
                size_t newChunksNeeded = (additionalNeeded + m_entitiesPerChunk - 1) >> m_entitiesPerChunkShift;

                for (size_t i = 0; i < newChunksNeeded; ++i)
                {
                    auto chunk = m_chunkPool->CreateChunk(m_entitiesPerChunk, m_componentDescriptors);
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        return locations;
                    }
                    m_chunks.emplace_back(std::move(chunk));
                }
            }

            size_t entityIndex = 0;
            size_t chunkIndex = m_firstNonFullChunkIndex;

            while (entityIndex < count && chunkIndex < m_chunks.size()) ASTRA_LIKELY
            {
                auto& chunk = m_chunks[chunkIndex];
                size_t available = m_entitiesPerChunk - chunk->GetCount();

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
                size_t additionalNeeded = count - remainingCapacity;
                size_t newChunksNeeded = (additionalNeeded + m_entitiesPerChunk - 1) >> m_entitiesPerChunkShift;

                for (size_t i = 0; i < newChunksNeeded; ++i)
                {
                    auto chunk = m_chunkPool->CreateChunk(m_entitiesPerChunk, m_componentDescriptors);
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        return locations;
                    }
                    m_chunks.emplace_back(std::move(chunk));
                }
            }

            for (size_t i = 0; i < count; ++i)
            {
                auto [chunkIndex, wasCreated] = GetOrCreateChunk();
                if (chunkIndex == INVALID_CHUNK_INDEX) ASTRA_UNLIKELY
                {
                    break;
                }

                auto& chunk = m_chunks[chunkIndex];
                size_t entityIndex = chunk->GetCount();
                chunk->GetEntities().push_back(entities[i]);
                chunk->SetCount(chunk->GetCount() + 1);
                
                auto componentTuple = generator(i);
                
                using TupleType = decltype(componentTuple);
                constexpr size_t tupleSize = std::tuple_size_v<TupleType>;
                
                [&]<std::size_t... Is>(std::index_sequence<Is...>)
                {
                    for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                    {
                        const auto& info = chunk->GetComponentArrays()[id];
                        if (!info.isValid || info.base == nullptr)
                        {
                            continue;
                        }
                        
                        bool willBeConstructed = ((TypeID<std::decay_t<std::tuple_element_t<Is, TupleType>>>::Value() == id) || ...);
                        
                        if (!willBeConstructed)
                        {
                            void* ptr = static_cast<std::byte*>(info.base) + entityIndex * info.stride;
                            info.descriptor.DefaultConstruct(ptr);
                        }
                    }
                    
                    ((chunk->ConstructComponentAt(entityIndex, std::get<Is>(std::move(componentTuple)))), ...);
                }(std::make_index_sequence<tupleSize>{});
                
                ++m_entityCount;
                
                if (chunk->IsFull()) ASTRA_UNLIKELY
                {
                    m_firstNonFullChunkIndex = chunkIndex + 1;
                }
                
                locations.push_back(EntityLocation::Create(chunkIndex, entityIndex));
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
                m_chunks.pop_back();

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
                    m_chunks.pop_back();
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

            Entity srcEntity = srcArchetype.m_chunks[srcChunkIndex]->GetEntity(srcEntityIndex);

            auto& dstChunk = m_chunks[dstChunkIndex];
            auto& srcChunk = srcArchetype.m_chunks[srcChunkIndex];
            const auto& dstArrays = dstChunk->GetComponentArrays();
            const auto& srcArrays = srcChunk->GetComponentArrays();

            for (const auto& dstDesc : m_componentDescriptors)
            {
                ComponentID id = dstDesc.id;
                const auto& dstInfo = dstArrays[id];
                
                void* dstPtr = static_cast<std::byte*>(dstInfo.base) + dstEntityIndex * dstInfo.stride;

                const auto& srcInfo = srcArrays[id];
                if (srcInfo.isValid) ASTRA_LIKELY
                {
                    void* srcPtr = static_cast<std::byte*>(srcInfo.base) + srcEntityIndex * srcInfo.stride;
                    dstInfo.descriptor.MoveConstruct(dstPtr, srcPtr);
                }
                else ASTRA_UNLIKELY
                {
                    dstInfo.descriptor.DefaultConstruct(dstPtr);
                }
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

            // Group by chunk for efficient processing
            FlatMap<size_t, std::vector<size_t>> chunkBatches;

            for (const auto& location : locations)
            {
                size_t chunkIndex = location.GetChunkIndex();
                size_t entityIndex = location.GetEntityIndex();
                chunkBatches[chunkIndex].push_back(entityIndex);
            }

            // Batch construct component in each chunk
            for (auto& [chunkIndex, indices] : chunkBatches)
            {
                ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");
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
            size_t currentCapacity = m_chunks.size() * m_entitiesPerChunk;

            if (required > currentCapacity) ASTRA_UNLIKELY
            {
                // Ceiling division: ceil(a/b) = floor((a + b - 1) / b)
                // For power of 2: ceil(a/b) = (a + b - 1) >> log2(b)
                size_t neededChunks = (required - currentCapacity + m_entitiesPerChunk - 1) >> m_entitiesPerChunkShift;
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
                remaining += m_entitiesPerChunk - m_chunks[i]->GetCount();
            }
            return remaining;
        }

        ASTRA_NODISCARD float GetFragmentationLevel() const noexcept
        {
            if (m_chunks.empty() || m_entityCount == 0)
                return 0.0f;
            
            // Calculate optimal chunk count (if perfectly packed)
            size_t optimalChunkCount = (m_entityCount + m_entitiesPerChunk - 1) / m_entitiesPerChunk;
            
            // Fragmentation = (actual chunks - optimal chunks) / actual chunks
            return static_cast<float>(m_chunks.size() - optimalChunkCount) / static_cast<float>(m_chunks.size());
        }
        
        void Serialize(BinaryWriter& writer) const
        {
            // Write archetype metadata - serialize the bitmap's words
            for (size_t i = 0; i < ComponentMask::WORD_COUNT; ++i)
            {
                writer(m_mask.Data()[i]);
            }
            writer(m_entityCount);
            writer(m_entitiesPerChunk);
            writer(static_cast<uint32_t>(m_chunks.size()));
            
            // Write component descriptors
            writer(static_cast<uint32_t>(m_componentDescriptors.size()));
            for (const auto& desc : m_componentDescriptors)
            {
                writer(desc.hash);  // Write stable hash instead of runtime ID
                writer(desc.size);
                writer(desc.alignment);
                writer(desc.version);
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
                    
                    size_t arraySize = chunkEntityCount * desc.size;
                    
                    // Use component's serialization function if available
                    if (desc.serializeVersioned || desc.serialize)
                    {
                        // For custom serialization, we can't compress the whole array
                        // as each component is serialized individually
                        if (desc.serializeVersioned)
                        {
                            for (size_t i = 0; i < chunkEntityCount; ++i)
                            {
                                void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                                desc.serializeVersioned(writer, componentPtr);
                            }
                        }
                        else
                        {
                            for (size_t i = 0; i < chunkEntityCount; ++i)
                            {
                                void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                                desc.serialize(writer, componentPtr);
                            }
                        }
                    }
                    else if (desc.is_trivially_copyable)
                    {
                        // For POD types, compress the entire array if beneficial
                        // WriteCompressedBlock will automatically handle compression threshold
                        writer.WriteCompressedBlock(componentArray, arraySize);
                    }
                    else
                    {
                        // Should not happen - components should be serializable
                        ASTRA_ASSERT(false, "Component type is not serializable");
                    }
                }
            }
        }
        
        static std::unique_ptr<Archetype> Deserialize(BinaryReader& reader, const std::vector<ComponentDescriptor>& registryDescriptors, ArchetypeChunkPool* componentPool = nullptr)
        {
            // Read archetype metadata - deserialize the bitmap's words
            ComponentMask mask;
            for (size_t i = 0; i < ComponentMask::WORD_COUNT; ++i)
            {
                reader(mask.Data()[i]);
            }
            
            size_t entityCount;
            size_t entitiesPerChunk;
            uint32_t chunkCount;
            reader(entityCount);
            reader(entitiesPerChunk);
            reader(chunkCount);
            
            // Read component descriptors
            uint32_t descriptorCount;
            reader(descriptorCount);
            std::vector<ComponentDescriptor> descriptors;
            descriptors.reserve(descriptorCount);
            
            for (uint32_t i = 0; i < descriptorCount; ++i)
            {
                uint64_t hash;
                size_t size, alignment;
                uint32_t version;
                reader(hash)(size)(alignment)(version);
                
                // Find matching descriptor from registry by hash
                auto it = std::find_if(registryDescriptors.begin(), registryDescriptors.end(), [hash](const auto& desc) { return desc.hash == hash; });
                
                if (it != registryDescriptors.end())
                {
                    descriptors.push_back(*it);
                }
                else
                {
                    // Component not registered - cannot deserialize
                    return nullptr;
                }
            }
            
            // Create new archetype
            auto archetype = std::make_unique<Archetype>(mask);
            archetype->m_chunkPool = componentPool;
            archetype->Initialize(descriptors);
            
            if (!archetype->IsInitialized())
            {
                return nullptr;
            }
            
            // Clear the pre-allocated chunk
            archetype->m_chunks.clear();
            archetype->m_entityCount = 0;
            
            // Read each chunk's data
            for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            {
                uint32_t chunkEntityCount;
                reader(chunkEntityCount);
                
                // Create new chunk
                auto chunk = componentPool ? componentPool->CreateChunk(entitiesPerChunk, descriptors) : nullptr;
                if (!chunk)
                {
                    // Out of memory - cannot continue
                    return nullptr;
                }
                
                // Read entities array
                for (uint32_t i = 0; i < chunkEntityCount; ++i)
                {
                    Entity entity;
                    reader(entity);
                    chunk->AddEntity(entity);
                }
                
                // Read component arrays (SOA layout)
                for (const auto& desc : descriptors)
                {
                    void* componentArray = chunk->GetComponentArrayByID(desc.id);
                    if (!componentArray) continue;
                    
                    size_t arraySize = chunkEntityCount * desc.size;
                    
                    if (desc.deserializeVersioned || desc.deserialize)
                    {
                        // For custom deserialization, components are not compressed
                        // as they were serialized individually
                        if (desc.deserializeVersioned)
                        {
                            for (uint32_t i = 0; i < chunkEntityCount; ++i)
                            {
                                void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                                desc.deserializeVersioned(reader, componentPtr);
                            }
                        }
                        else
                        {
                            for (uint32_t i = 0; i < chunkEntityCount; ++i)
                            {
                                void* componentPtr = static_cast<char*>(componentArray) + (i * desc.size);
                                desc.deserialize(reader, componentPtr);
                            }
                        }
                    }
                    else if (desc.is_trivially_copyable)
                    {
                        // POD types may be compressed - use ReadCompressedBlock
                        auto result = reader.ReadCompressedBlock();
                        if (result.IsErr())
                        {
                            // Error reading compressed block
                            return nullptr;
                        }
                        
                        auto& data = *result.GetValue();
                        if (data.size() != arraySize)
                        {
                            // Size mismatch - data corruption
                            return nullptr;
                        }
                        
                        // Copy decompressed data to component array
                        std::memcpy(componentArray, data.data(), arraySize);
                    }
                }
                
                archetype->m_chunks.push_back(std::move(chunk));
            }
            
            archetype->m_entityCount = entityCount;
            
            return archetype;
        }
        
        ASTRA_NODISCARD bool ShouldCoalesce(float utilizationThreshold = 0.5f) const
        {
            if (m_chunks.size() <= 1) ASTRA_LIKELY return false;
            
            // Simple heuristic: coalesce if we have sparse chunks
            for (size_t i = 1; i < m_chunks.size(); ++i)
            {
                float utilization = static_cast<float>(m_chunks[i]->GetCount()) / m_entitiesPerChunk;
                if (utilization < utilizationThreshold)
                {
                    return true;
                }
            }
            return false;
        }

        std::pair<size_t, std::vector<std::pair<Entity, EntityLocation>>> CoalesceChunks(float utilizationThreshold = 0.5f)
        {
            std::vector<std::pair<Entity, EntityLocation>> allMovedEntities;
            if (m_chunks.size() <= 1) ASTRA_LIKELY return {0, allMovedEntities};

            // Single pass: find sparse chunks and calculate total entities to move
            std::vector<std::pair<size_t, float>> sparseChunks;
            size_t totalEntitiesToMove = 0;
            size_t totalAvailableSpace = 0;
            
            for (size_t i = 0; i < m_chunks.size(); ++i)
            {
                size_t count = m_chunks[i]->GetCount();
                float utilization = static_cast<float>(count) / m_entitiesPerChunk;
                
                if (i > 0 && utilization < utilizationThreshold)  // Skip first chunk for sparse check
                {
                    sparseChunks.emplace_back(i, utilization);
                    totalEntitiesToMove += count;
                }
                
                // Calculate available space in all chunks
                // We count space in all chunks because sparse chunks can consolidate into each other
                size_t available = m_entitiesPerChunk - count;
                if (available > 0)
                {
                    totalAvailableSpace += available;
                }
            }

            // Early exit: no sparse chunks
            if (sparseChunks.empty()) ASTRA_LIKELY return {0, allMovedEntities};
            
            // Early exit: not enough space to consolidate meaningfully
            if (totalAvailableSpace < totalEntitiesToMove / 2)
            {
                return {0, allMovedEntities};
            }

            // Only sort if we have multiple sparse chunks
            if (sparseChunks.size() > 1)
            {
                std::sort(sparseChunks.begin(), sparseChunks.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            }

            // Try to pack entities from sparse chunks into denser ones
            for (const auto& [sparseIndex, _] : sparseChunks)
            {
                auto& sparseChunk = m_chunks[sparseIndex];
                size_t entitiesToMove = sparseChunk->GetCount();

                if (entitiesToMove == 0) continue;

                // Find destination chunks with available space
                for (size_t destIndex = 0; destIndex < m_chunks.size(); ++destIndex)
                {
                    if (destIndex == sparseIndex) continue;

                    auto& destChunk = m_chunks[destIndex];
                    size_t available = m_entitiesPerChunk - destChunk->GetCount();

                    if (available > 0)
                    {
                        size_t toMove = std::min(available, entitiesToMove);

                        // Move entities from sparse to destination chunk
                        auto movedEntities = MoveEntitiesBetweenChunks(sparseIndex, destIndex, toMove);
                        allMovedEntities.insert(allMovedEntities.end(), movedEntities.begin(), movedEntities.end());

                        entitiesToMove -= toMove;
                        if (entitiesToMove == 0) break;
                    }
                }
            }

            // Early exit: if no entities were moved, no chunks will be freed
            if (allMovedEntities.empty())
            {
                return {0, allMovedEntities};
            }

            // Remove empty chunks
            size_t chunksFreed = 0;
            for (size_t i = m_chunks.size() - 1; i > 0; --i)  // Keep first chunk
            {
                if (m_chunks[i]->IsEmpty())
                {
                    m_chunks.erase(m_chunks.begin() + i);
                    ++chunksFreed;
                }
            }

            return {chunksFreed, allMovedEntities};
        }
        
        ASTRA_NODISCARD bool IsInitialized() const noexcept { return m_initialized; }
        ASTRA_NODISCARD const ComponentMask& GetMask() const noexcept { return m_mask; }

        ASTRA_NODISCARD size_t GetEntityCount() const noexcept { return m_entityCount; }
        ASTRA_NODISCARD size_t GetChunkCount() const noexcept { return m_chunks.size(); }
        ASTRA_NODISCARD size_t GetComponentCount() const noexcept { return m_componentCount; }
        ASTRA_NODISCARD size_t GetChunkEntityCount(size_t chunkIndex) const noexcept { return (chunkIndex < m_chunks.size()) ? m_chunks[chunkIndex]->GetCount() : 0; }
        ASTRA_NODISCARD size_t GetEntitiesPerChunk() const noexcept { return m_entitiesPerChunk; }

        ASTRA_NODISCARD const std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>>& GetChunks() const { return m_chunks; }
        ASTRA_NODISCARD const std::vector<ComponentDescriptor>& GetComponentDescriptors() const { return m_componentDescriptors; }

        void SetComponentPool(ArchetypeChunkPool* pool) { m_chunkPool = pool; }

    private:
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
                size_t additionalNeeded = count - remainingCapacity;
                size_t newChunksNeeded = (additionalNeeded + m_entitiesPerChunk - 1) >> m_entitiesPerChunkShift;

                // Pre-allocate all chunks needed - ensure we can complete the operation
                std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> newChunks;
                newChunks.reserve(newChunksNeeded);
                
                for (size_t i = 0; i < newChunksNeeded; ++i)
                {
                    auto chunk = m_chunkPool->CreateChunk(m_entitiesPerChunk, m_componentDescriptors);
                    if (!chunk) ASTRA_UNLIKELY
                    {
                        // Failed to allocate all required chunks - return empty to indicate failure
                        return {};
                    }
                    newChunks.push_back(std::move(chunk));
                }
                
                // All chunks allocated successfully - add them to our chunks vector
                for (auto& chunk : newChunks)
                {
                    m_chunks.emplace_back(std::move(chunk));
                }
            }

            size_t entityIndex = 0;
            size_t chunkIndex = m_firstNonFullChunkIndex;

            while (entityIndex < count && chunkIndex < m_chunks.size()) ASTRA_LIKELY
            {
                auto& chunk = m_chunks[chunkIndex];
                size_t available = m_entitiesPerChunk - chunk->GetCount();

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
            const auto& entities = chunk->GetEntities();

            for (size_t i = 0; i < count; ++i)
            {
                func(entities[i], GetComponentValue<Components>(std::get<Is>(arrays), i)...);
            }
        }
        
        std::pair<size_t, bool> GetOrCreateChunk()
        {
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
            
            auto chunk = m_chunkPool->CreateChunk(m_entitiesPerChunk, m_componentDescriptors);
            if (!chunk) ASTRA_UNLIKELY
            {
                return {INVALID_CHUNK_INDEX, false};
            }
            
            m_chunks.emplace_back(std::move(chunk));
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
            chunk->GetEntities().resize(chunk->GetCount() + 1);
            size_t entityIndex = chunk->GetCount();
            chunk->SetCount(chunk->GetCount() + 1);
            
            chunk->GetEntities()[entityIndex] = entity;
            
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
                
                // Move components using O(1) lookups
                const auto& srcArrays = srcChunk->GetComponentArrays();
                const auto& destArrays = destChunk->GetComponentArrays();
                
                for (ComponentID id = 0; id < MAX_COMPONENTS; ++id)
                {
                    const auto& srcInfo = srcArrays[id];
                    if (!srcInfo.isValid)
                    {
                        continue;
                    }
                    
                    void* srcPtr = static_cast<std::byte*>(srcInfo.base) + srcEntityIndex * srcInfo.stride;
                    void* destPtr = static_cast<std::byte*>(destArrays[id].base) + destEntityIndex * srcInfo.stride;
                    
                    // Use move constructor to transfer component data
                    srcInfo.descriptor.MoveConstruct(destPtr, srcPtr);
                    srcInfo.descriptor.Destruct(srcPtr);
                }
                
                // Remove entity from source chunk's entity vector
                srcChunk->GetEntities().pop_back();
            }
            
            // Update chunk counts
            srcChunk->SetCount(srcCount - count);
            destChunk->SetCount(destCount + count);
            
            return movedEntities;
        }

        ASTRA_NODISCARD size_t GetEntitiesPerChunkShift() const noexcept { return m_entitiesPerChunkShift; }
        ASTRA_NODISCARD size_t GetEntitiesPerChunkMask() const noexcept { return m_entitiesPerChunkMask; }

        ComponentMask m_mask;
        size_t m_componentCount;  // Cached component count for fast access
        std::vector<ComponentDescriptor> m_componentDescriptors;
        std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> m_chunks;
        size_t m_entityCount;
        size_t m_entitiesPerChunk;
        size_t m_entitiesPerChunkShift;     // For fast division via bit shift (log2(m_entitiesPerChunk))
        size_t m_entitiesPerChunkMask;      // For fast modulo operations (m_entitiesPerChunk - 1)
        size_t m_firstNonFullChunkIndex = 0;  // Track first chunk with available space for O(1) lookup
        bool m_initialized;
        ArchetypeChunkPool* m_chunkPool = nullptr;

        friend class ArchetypeManager;
        friend class Registry;
    };
}
