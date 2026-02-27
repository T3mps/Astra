#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/TypeID.hpp"
#include "Component.hpp"
#include "ComponentRegistry.hpp"

namespace Astra
{
    /**
     * @brief Storage for singleton resources (global components)
     * 
     * Resources are singleton components that exist globally rather than being attached
     * to entities. Common examples include Time, Input, RenderSettings, etc.
     * 
     * Uses a sparse-dense storage pattern with swap-and-pop removal to maintain
     * cache locality and prevent fragmentation.
     * 
     * Memory allocation strategy:
     * - Small resources (≤64 bytes): Stored inline using Small Buffer Optimization
     * - Large resources: Allocated directly from heap
     */
    class ResourceStorage
    {
    public:
        // Small Buffer Optimization threshold (cache line size)
        static constexpr size_t SBO_SIZE = CACHE_LINE_SIZE;
        
        // Configuration for resource storage behavior
        struct Config
        {
            size_t initialResourceCapacity = 32; // Initial capacity for resource vector
        };

        // Constructor with just registry (backward compatibility)
        explicit ResourceStorage(std::weak_ptr<ComponentRegistry> registry) : ResourceStorage(registry, Config{}) {}
        
        // Constructor with config
        ResourceStorage(std::weak_ptr<ComponentRegistry> registry, const Config& config) :
            m_componentRegistry(registry),
            m_config(config)
        {
            ASTRA_ASSERT(!registry.expired(), "Component registry cannot be null");
            m_sparse.fill(INVALID_INDEX);
            m_resources.reserve(config.initialResourceCapacity);
        }

        ~ResourceStorage()
        {
            Clear();
        }

        ResourceStorage(const ResourceStorage&) = delete;
        ResourceStorage& operator=(const ResourceStorage&) = delete;

        ResourceStorage(ResourceStorage&& other) noexcept :
            m_componentRegistry(std::move(other.m_componentRegistry)),
            m_resources(std::move(other.m_resources)),
            m_sparse(std::move(other.m_sparse))
        {
            other.m_sparse.fill(INVALID_INDEX);
        }

        ResourceStorage& operator=(ResourceStorage&& other) noexcept
        {
            if (this != &other)
            {
                Clear();
                m_componentRegistry = std::move(other.m_componentRegistry);
                m_resources = std::move(other.m_resources);
                m_sparse = std::move(other.m_sparse);
                other.m_sparse.fill(INVALID_INDEX);
            }
            return *this;
        }

        template<typename T>
        ASTRA_NODISCARD T* Get() noexcept
        {
            ComponentID id = TypeID<T>::Value();
            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX) [[unlikely]]
                return nullptr;

            ASTRA_ASSERT(index < m_resources.size(), "Invalid resource index");
            auto& slot = m_resources[index];
            ASTRA_ASSERT(slot.isValid, "Invalid resource slot");

            // Return pointer to resource data
            return reinterpret_cast<T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
        }

        template<typename T>
        ASTRA_NODISCARD const T* Get() const noexcept
        {
            ComponentID id = TypeID<T>::Value();
            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX) [[unlikely]]
                return nullptr;

            ASTRA_ASSERT(index < m_resources.size(), "Invalid resource index");
            const auto& slot = m_resources[index];
            ASTRA_ASSERT(slot.isValid, "Invalid resource slot");

            // Return pointer to resource data
            return reinterpret_cast<const T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
        }

        template<typename T>
        T* Set(T&& resource)
        {

            ComponentID id = TypeID<T>::Value();
            ASTRA_ASSERT(id < MAX_COMPONENTS, "Component ID out of range");

            uint16_t index = m_sparse[id];

            if (index == INVALID_INDEX)
            {
                // New resource - allocate slot
                index = static_cast<uint16_t>(m_resources.size());
                ASTRA_ASSERT(index < INVALID_INDEX, "Too many resources");
                
                m_sparse[id] = index;
                m_resources.emplace_back();

                auto& slot = m_resources[index];
                slot.id = id;
                slot.size = sizeof(T);
                
                auto registry = m_componentRegistry.lock();
                ASTRA_ASSERT(registry, "Component registry expired");
                registry->RegisterComponent<T>();
                slot.descriptor = registry->GetComponentDescriptor(id);
                ASTRA_ASSERT(slot.descriptor, "Failed to get component descriptor");
                slot.isValid = true;

                // Decide between inline storage and heap allocation
                if constexpr (sizeof(T) <= SBO_SIZE)
                {
                    // Use inline storage
                    slot.isHeap = false;
                    new (slot.storage.inlineData) T(std::forward<T>(resource));
                }
                else
                {
                    // Allocate from heap
                    slot.isHeap = true;
                    AllocResult result = AllocateMemory(sizeof(T), alignof(T));
                    ASTRA_ASSERT(result.ptr, "Failed to allocate memory for resource");
                    slot.storage.heapPtr = result.ptr;
                    new (slot.storage.heapPtr) T(std::forward<T>(resource));
                }
            }
            else
            {
                // Update existing resource
                auto& slot = m_resources[index];
                ASTRA_ASSERT(slot.isValid, "Invalid resource slot");
                ASTRA_ASSERT(slot.size == sizeof(T), "Resource size mismatch");
                
                // Update existing resource
                T* existing = reinterpret_cast<T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
                *existing = std::forward<T>(resource);
                return existing;
            }

            // Return pointer to newly created resource
            auto& slot = m_resources[index];
            return reinterpret_cast<T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
        }

        template<typename T>
        ASTRA_NODISCARD bool Has() const noexcept
        {
            ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS)
                return false;
            
            uint16_t index = m_sparse[id];
            return index != INVALID_INDEX && index < m_resources.size() && m_resources[index].isValid;
        }

        template<typename T, typename... Args>
        T* Emplace(Args&&... args)
        {

            ComponentID id = TypeID<T>::Value();
            ASTRA_ASSERT(id < MAX_COMPONENTS, "Component ID out of range");

            uint16_t index = m_sparse[id];

            if (index == INVALID_INDEX)
            {
                // New resource - allocate slot
                index = static_cast<uint16_t>(m_resources.size());
                ASTRA_ASSERT(index < INVALID_INDEX, "Too many resources");
                
                m_sparse[id] = index;
                m_resources.emplace_back();

                auto& slot = m_resources[index];
                slot.id = id;
                slot.size = sizeof(T);
                
                auto registry = m_componentRegistry.lock();
                ASTRA_ASSERT(registry, "Component registry expired");
                registry->RegisterComponent<T>();
                slot.descriptor = registry->GetComponentDescriptor(id);
                ASTRA_ASSERT(slot.descriptor, "Failed to get component descriptor");
                slot.isValid = true;

                // Decide between inline storage and heap allocation
                if constexpr (sizeof(T) <= SBO_SIZE)
                {
                    // Construct in-place with inline storage
                    slot.isHeap = false;
                    new (slot.storage.inlineData) T(std::forward<Args>(args)...);
                }
                else
                {
                    // Allocate from heap and construct in-place
                    slot.isHeap = true;
                    AllocResult result = AllocateMemory(sizeof(T), alignof(T));
                    ASTRA_ASSERT(result.ptr, "Failed to allocate memory for resource");
                    slot.storage.heapPtr = result.ptr;
                    new (slot.storage.heapPtr) T(std::forward<Args>(args)...);
                }
            }
            else
            {
                // Update existing resource - destroy old and construct new
                auto& slot = m_resources[index];
                ASTRA_ASSERT(slot.isValid, "Invalid resource slot");
                ASTRA_ASSERT(slot.size == sizeof(T), "Resource size mismatch");
                
                // Destroy existing resource
                if (slot.descriptor)
                {
                    if (slot.isHeap)
                    {
                        slot.descriptor->Destruct(slot.storage.heapPtr);
                    }
                    else
                    {
                        slot.descriptor->Destruct(slot.storage.inlineData);
                    }
                }
                
                // Construct new resource in-place
                T* existing = reinterpret_cast<T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
                new (existing) T(std::forward<Args>(args)...);
                return existing;
            }

            // Return pointer to newly created resource
            auto& slot = m_resources[index];
            return reinterpret_cast<T*>(slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData);
        }

        template<typename T>
        void Remove()
        {
            ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS)
                return;

            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX)
                return;

            ASTRA_ASSERT(index < m_resources.size(), "Invalid resource index");
            auto& slot = m_resources[index];
            
            if (!slot.isValid)
                return;

            // Destruct the resource
            if (slot.descriptor)
            {
                if (slot.isHeap)
                {
                    slot.descriptor->Destruct(slot.storage.heapPtr);
                    FreeMemory(slot.storage.heapPtr, slot.size);
                    slot.storage.heapPtr = nullptr;
                }
                else
                {
                    slot.descriptor->Destruct(slot.storage.inlineData);
                }
            }
            
            slot.isValid = false;

            // Swap-and-pop to maintain density
            size_t lastIndex = m_resources.size() - 1;
            if (index != lastIndex)
            {
                m_resources[index] = std::move(m_resources[lastIndex]);
                m_sparse[m_resources[index].id] = index;
            }

            m_resources.pop_back();
            m_sparse[id] = INVALID_INDEX;
        }

        void Clear()
        {
            for (auto& slot : m_resources)
            {
                if (slot.isValid && slot.descriptor)
                {
                    if (slot.isHeap)
                    {
                        slot.descriptor->Destruct(slot.storage.heapPtr);
                        FreeMemory(slot.storage.heapPtr, slot.size);
                        slot.storage.heapPtr = nullptr;
                    }
                    else
                    {
                        slot.descriptor->Destruct(slot.storage.inlineData);
                    }
                    slot.isValid = false;
                }
            }

            m_resources.clear();
            m_sparse.fill(INVALID_INDEX);
        }

        ASTRA_NODISCARD size_t Size() const noexcept
        {
            return m_resources.size();
        }

        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_resources.empty();
        }

        ASTRA_NODISCARD size_t GetMemoryUsage() const noexcept
        {
            size_t totalSize = sizeof(ResourceStorage) + (m_resources.capacity() * sizeof(ResourceSlot));

            // Add heap-allocated resource sizes
            for (const auto& slot : m_resources)
            {
                if (slot.isValid && slot.isHeap)
                {
                    totalSize += slot.size;
                }
            }

            return totalSize;
        }

        // ====================== Reflection Integration ======================

        /**
         * Gets a resource by type hash (for reflection/runtime access).
         * @param typeHash XXHash64 of the resource type name
         * @return Pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetResourceByHash(uint64_t typeHash)
        {
            auto registry = m_componentRegistry.lock();
            if (!registry)
                return nullptr;

            auto result = registry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return nullptr;

            ComponentID id = *result.GetValue();
            if (id >= MAX_COMPONENTS)
                return nullptr;

            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX || index >= m_resources.size())
                return nullptr;

            auto& slot = m_resources[index];
            if (!slot.isValid)
                return nullptr;

            return slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData;
        }

        /**
         * Gets a resource by type hash (const version).
         * @param typeHash XXHash64 of the resource type name
         * @return Const pointer to the resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetResourceByHash(uint64_t typeHash) const
        {
            return const_cast<ResourceStorage*>(this)->GetResourceByHash(typeHash);
        }

        /**
         * Checks if a resource exists by type hash.
         * @param typeHash XXHash64 of the resource type name
         * @return true if the resource exists
         */
        ASTRA_NODISCARD bool HasResourceByHash(uint64_t typeHash) const
        {
            auto registry = m_componentRegistry.lock();
            if (!registry)
                return false;

            auto result = registry->GetComponentIDFromHash(typeHash);
            if (result.IsErr())
                return false;

            ComponentID id = *result.GetValue();
            if (id >= MAX_COMPONENTS)
                return false;

            uint16_t index = m_sparse[id];
            return index != INVALID_INDEX && index < m_resources.size() && m_resources[index].isValid;
        }

        /**
         * Gets all resource descriptors.
         * Useful for editor/inspector UI that needs to enumerate all resources.
         * @return Vector of ComponentDescriptor pointers for all active resources
         */
        ASTRA_NODISCARD std::vector<const ComponentDescriptor*> GetAllResources() const
        {
            std::vector<const ComponentDescriptor*> result;
            result.reserve(m_resources.size());

            for (const auto& slot : m_resources)
            {
                if (slot.isValid && slot.descriptor)
                {
                    result.push_back(slot.descriptor);
                }
            }

            return result;
        }

        /**
         * Gets the resource data pointer for a given ComponentID.
         * @param id ComponentID of the resource
         * @return Pointer to resource data, or nullptr if not found
         */
        ASTRA_NODISCARD void* GetByID(ComponentID id)
        {
            if (id >= MAX_COMPONENTS)
                return nullptr;

            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX || index >= m_resources.size())
                return nullptr;

            auto& slot = m_resources[index];
            if (!slot.isValid)
                return nullptr;

            return slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData;
        }

        /**
         * Gets the resource data pointer for a given ComponentID (const version).
         * @param id ComponentID of the resource
         * @return Const pointer to resource data, or nullptr if not found
         */
        ASTRA_NODISCARD const void* GetByID(ComponentID id) const
        {
            return const_cast<ResourceStorage*>(this)->GetByID(id);
        }

        /**
         * Type-erased resource setting for use by CommandBuffer.
         * Sets a resource using the component ID and raw data pointer.
         * The component must already be registered in the ComponentRegistry.
         *
         * @param id The ComponentID of the resource
         * @param data Pointer to the source resource data (will be copy-constructed)
         * @param dataSize Size of the resource data (for validation)
         * @return true if resource was set successfully, false otherwise
         */
        bool SetByID(ComponentID id, const void* data, size_t dataSize)
        {
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return false;

            const ComponentDescriptor* desc = registry->GetComponentDescriptor(id);
            if (!desc) ASTRA_UNLIKELY
                return false;

            // Validate data size matches component size
            if (dataSize != desc->size && desc->size > 0) ASTRA_UNLIKELY
                return false;

            uint16_t index = m_sparse[id];

            if (index == INVALID_INDEX)
            {
                // New resource - allocate slot
                index = static_cast<uint16_t>(m_resources.size());
                if (index >= INVALID_INDEX) ASTRA_UNLIKELY
                    return false;

                m_sparse[id] = index;
                m_resources.emplace_back();

                auto& slot = m_resources[index];
                slot.id = id;
                slot.size = static_cast<uint16_t>(desc->size);
                slot.descriptor = desc;
                slot.isValid = true;

                // Decide between inline storage and heap allocation
                if (desc->size <= SBO_SIZE)
                {
                    slot.isHeap = false;
                    desc->ConstructWith(slot.storage.inlineData, data);
                }
                else
                {
                    slot.isHeap = true;
                    AllocResult result = AllocateMemory(desc->size, desc->alignment);
                    if (!result.ptr) ASTRA_UNLIKELY
                        return false;
                    slot.storage.heapPtr = result.ptr;
                    desc->ConstructWith(slot.storage.heapPtr, data);
                }
            }
            else
            {
                // Update existing resource
                auto& slot = m_resources[index];
                if (!slot.isValid) ASTRA_UNLIKELY
                    return false;

                // Copy assign the new data
                void* existing = slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData;
                if (desc->copyAssign)
                {
                    desc->copyAssign(existing, data);
                }
                else if (desc->is_trivially_copyable)
                {
                    std::memcpy(existing, data, desc->size);
                }
                else
                {
                    return false;  // Can't copy-assign
                }
            }

            return true;
        }

        /**
         * Type-erased resource removal for use by CommandBuffer.
         *
         * @param id The ComponentID of the resource to remove
         * @return true if resource was removed, false if it didn't exist
         */
        bool RemoveByID(ComponentID id)
        {
            if (id >= MAX_COMPONENTS)
                return false;

            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX)
                return false;

            if (index >= m_resources.size()) ASTRA_UNLIKELY
                return false;

            auto& slot = m_resources[index];

            if (!slot.isValid)
                return false;

            // Destruct the resource
            if (slot.descriptor)
            {
                if (slot.isHeap)
                {
                    slot.descriptor->Destruct(slot.storage.heapPtr);
                    FreeMemory(slot.storage.heapPtr, slot.size);
                    slot.storage.heapPtr = nullptr;
                }
                else
                {
                    slot.descriptor->Destruct(slot.storage.inlineData);
                }
            }

            slot.isValid = false;

            // Swap-and-pop to maintain density
            size_t lastIndex = m_resources.size() - 1;
            if (index != lastIndex)
            {
                m_resources[index] = std::move(m_resources[lastIndex]);
                m_sparse[m_resources[index].id] = index;
            }

            m_resources.pop_back();
            m_sparse[id] = INVALID_INDEX;

            return true;
        }

    private:
        struct ResourceSlot
        {
            union Storage
            {
                alignas(64) std::byte inlineData[SBO_SIZE];  // Small Buffer Optimization
                void* heapPtr;                               // Pointer to heap allocation
            };
            
            Storage storage;
            const ComponentDescriptor* descriptor;  // Pointer instead of value (8 vs 48 bytes)
            ComponentID id;
            uint16_t size;      // Actual size of the resource
            bool isHeap : 1;    // true if using heapPtr, false if using inlineData
            bool isValid : 1;   // true if slot contains valid resource
            
            ResourceSlot() : descriptor(nullptr), id(0), size(0), isHeap(false), isValid(false) {}
        };

        static constexpr uint16_t INVALID_INDEX = std::numeric_limits<uint16_t>::max();

        std::weak_ptr<ComponentRegistry> m_componentRegistry;
        std::vector<ResourceSlot> m_resources;
        std::array<uint16_t, MAX_COMPONENTS> m_sparse;
        Config m_config;
    };
} // namespace Astra
