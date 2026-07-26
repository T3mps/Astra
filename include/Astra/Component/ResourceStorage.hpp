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
     * - Every resource's payload lives on the heap at a POINTER-STABLE address.
     *   The dense vector stores only a heap pointer per slot, so the two ordinary
     *   relocations of that vector — reallocation on growth, and swap-and-pop on
     *   removal — move the *pointer*, never the payload bytes. A `T*` returned by
     *   Get/Set/Emplace/GetByID therefore stays valid until that specific resource
     *   is removed, immune to unrelated adds/removes. (Small inline storage was
     *   removed precisely because it dangled every returned pointer on relocation.)
     */
    class ResourceStorage
    {
    public:
        // Retained for API compatibility. No longer selects a storage path — all
        // resources are heap-allocated for pointer stability (see class docs).
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
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;
            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX) [[unlikely]]
                return nullptr;

            ASTRA_ASSERT(index < m_resources.size(), "Invalid resource index");
            auto& slot = m_resources[index];
            if (!slot.isValid) ASTRA_UNLIKELY
                return nullptr;

            // Return pointer-stable heap payload
            return reinterpret_cast<T*>(slot.ptr);
        }

        template<typename T>
        ASTRA_NODISCARD const T* Get() const noexcept
        {
            ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;
            uint16_t index = m_sparse[id];
            if (index == INVALID_INDEX) [[unlikely]]
                return nullptr;

            ASTRA_ASSERT(index < m_resources.size(), "Invalid resource index");
            const auto& slot = m_resources[index];
            if (!slot.isValid) ASTRA_UNLIKELY
                return nullptr;

            // Return pointer-stable heap payload
            return reinterpret_cast<const T*>(slot.ptr);
        }

        template<typename T>
        T* Set(T&& resource)
        {

            ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;

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

                registry->RegisterComponent<T>();
                const ComponentDescriptor* desc = registry->GetComponentDescriptor(id);
                if (!desc) ASTRA_UNLIKELY
                    return nullptr;
                slot.descriptor = *desc;

                // Allocate pointer-stable heap storage and construct in place.
                AllocResult result = AllocateMemory(sizeof(T), alignof(T));
                if (!result.ptr) ASTRA_UNLIKELY
                    return nullptr;
                slot.ptr = result.ptr;
                new (slot.ptr) T(std::forward<T>(resource));

                // Only mark the slot valid once its storage is fully established —
                // a failed heap allocation above must leave the slot invalid so
                // teardown never frees a garbage pointer. (Mirrors SetByID/Deserialize.)
                slot.isValid = true;
            }
            else
            {
                // Update existing resource
                auto& slot = m_resources[index];
                if (!slot.isValid) ASTRA_UNLIKELY
                    return nullptr;
                ASTRA_ASSERT(slot.size == sizeof(T), "Resource size mismatch");

                // Update existing resource
                T* existing = reinterpret_cast<T*>(slot.ptr);
                *existing = std::forward<T>(resource);
                return existing;
            }

            // Return pointer to newly created resource
            auto& slot = m_resources[index];
            return reinterpret_cast<T*>(slot.ptr);
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
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return nullptr;

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

                registry->RegisterComponent<T>();
                const ComponentDescriptor* desc = registry->GetComponentDescriptor(id);
                if (!desc) ASTRA_UNLIKELY
                    return nullptr;
                slot.descriptor = *desc;

                // Allocate pointer-stable heap storage and construct in place.
                AllocResult result = AllocateMemory(sizeof(T), alignof(T));
                if (!result.ptr) ASTRA_UNLIKELY
                    return nullptr;
                slot.ptr = result.ptr;
                new (slot.ptr) T(std::forward<Args>(args)...);

                // Only mark the slot valid once its storage is fully established —
                // a failed heap allocation above must leave the slot invalid so
                // teardown never frees a garbage pointer. (Mirrors SetByID/Deserialize.)
                slot.isValid = true;
            }
            else
            {
                // Update existing resource - destroy old and construct new
                auto& slot = m_resources[index];
                if (!slot.isValid) ASTRA_UNLIKELY
                    return nullptr;
                ASTRA_ASSERT(slot.size == sizeof(T), "Resource size mismatch");

                // Destroy existing resource (slot is valid, so its descriptor is set)
                slot.descriptor.Destruct(slot.ptr);

                // Construct new resource in-place (heap block is stable and correctly sized)
                T* existing = reinterpret_cast<T*>(slot.ptr);
                new (existing) T(std::forward<Args>(args)...);
                return existing;
            }

            // Return pointer to newly created resource
            auto& slot = m_resources[index];
            return reinterpret_cast<T*>(slot.ptr);
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

            // Destruct + free the resource (slot is valid, so its descriptor value is set)
            slot.descriptor.Destruct(slot.ptr);
            FreeMemory(slot.ptr, slot.size);
            slot.ptr = nullptr;
            slot.isValid = false;

            // Swap-and-pop to maintain density. This moves the surviving slot's heap
            // POINTER (not the payload bytes), so any T* previously returned for that
            // resource stays valid.
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
                if (slot.isValid)
                {
                    slot.descriptor.Destruct(slot.ptr);
                    FreeMemory(slot.ptr, slot.size);
                    slot.ptr = nullptr;
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

            // Every resource payload is heap-allocated, so add each valid slot's size.
            for (const auto& slot : m_resources)
            {
                if (slot.isValid)
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

            return slot.ptr;
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
            auto registry = m_componentRegistry.lock();
            if (!registry)
                return result;
            result.reserve(m_resources.size());

            for (const auto& slot : m_resources)
            {
                if (slot.isValid)
                {
                    // Return the registry's stable descriptor pointer, not a pointer into
                    // the (relocatable) resource vector's by-value slot copy.
                    if (const ComponentDescriptor* desc = registry->GetComponentDescriptor(slot.id))
                        result.push_back(desc);
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

            return slot.ptr;
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
                slot.descriptor = *desc;

                // Allocate pointer-stable heap storage and construct in place. An
                // empty/tag resource (size 0) still gets a unique, non-null, stable
                // one-byte address so GetByID stays non-null while it is present.
                AllocResult result = AllocateMemory(desc->size == 0 ? 1 : desc->size, desc->alignment);
                if (!result.ptr) ASTRA_UNLIKELY
                    return false;
                slot.ptr = result.ptr;
                desc->ConstructWith(slot.ptr, data);

                // Only mark the slot valid once its storage is fully established —
                // a failed heap allocation above must leave the slot invalid so
                // teardown never frees a garbage pointer. (Mirrors Deserialize.)
                slot.isValid = true;
            }
            else
            {
                // Update existing resource
                auto& slot = m_resources[index];
                if (!slot.isValid) ASTRA_UNLIKELY
                    return false;

                // Copy assign the new data
                void* existing = slot.ptr;
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

            // Destruct + free the resource (slot is valid, so its descriptor value is set)
            slot.descriptor.Destruct(slot.ptr);
            FreeMemory(slot.ptr, slot.size);
            slot.ptr = nullptr;
            slot.isValid = false;

            // Swap-and-pop to maintain density. This moves the surviving slot's heap
            // POINTER (not the payload bytes), so any pointer previously returned for
            // that resource stays valid.
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

        // ====================== Serialization (archive v2+) ======================

        void Serialize(BinaryWriter& writer) const
        {
            uint32_t count = 0;
            for (const auto& slot : m_resources)
            {
                if (slot.isValid) ++count;
            }
            writer(count);

            for (const auto& slot : m_resources)
            {
                if (!slot.isValid) continue;
                writer(slot.descriptor.hash);
                slot.descriptor.serializeVersioned(writer, slot.ptr);
            }
        }

        // Restores resources previously written by Serialize. Every stored
        // type must already be registered in the ComponentRegistry (looked up
        // by stable hash); returns false on unknown types or stream errors.
        bool Deserialize(BinaryReader& reader)
        {
            uint32_t count = 0;
            reader(count);
            if (reader.HasError()) return false;

            auto registry = m_componentRegistry.lock();
            if (!registry) return false;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t hash = 0;
                reader(hash);
                if (reader.HasError()) return false;

                const ComponentDescriptor* desc = registry->GetComponentDescriptorByHash(hash);
                if (!desc || desc->id >= MAX_COMPONENTS)
                    return false;   // caller maps this to UnknownComponent

                // Allocate a slot (same layout policy as SetByID)…
                uint16_t index = m_sparse[desc->id];
                void* dst = nullptr;
                if (index == INVALID_INDEX)
                {
                    index = static_cast<uint16_t>(m_resources.size());
                    if (index >= INVALID_INDEX) return false;
                    m_sparse[desc->id] = index;
                    m_resources.emplace_back();

                    auto& slot = m_resources[index];
                    slot.id = desc->id;
                    slot.size = static_cast<uint16_t>(desc->size);
                    slot.descriptor = *desc;

                    // Allocate pointer-stable heap storage. An empty/tag resource
                    // (size 0) still gets a unique, non-null, stable one-byte address.
                    AllocResult result = AllocateMemory(desc->size == 0 ? 1 : desc->size, desc->alignment);
                    if (!result.ptr) return false;
                    slot.ptr = result.ptr;
                    dst = result.ptr;

                    // Only mark the slot valid once its storage is fully
                    // established — a failed heap allocation above must leave
                    // the slot invalid so teardown never frees a garbage pointer.
                    slot.isValid = true;
                    // …default-construct, then deserialize over it.
                    desc->DefaultConstruct(dst);
                }
                else
                {
                    auto& slot = m_resources[index];
                    dst = slot.ptr;
                }

                if (!desc->deserializeVersioned(reader, dst))
                    return false;
            }
            return !reader.HasError();
        }

    private:
        struct ResourceSlot
        {
            // Pointer-stable heap allocation holding the resource payload. The
            // dense vector may relocate this slot (growth) or swap-pop it into
            // another index; only this pointer moves, never the payload bytes,
            // so a T* handed back to a caller stays valid until the resource is
            // removed.
            void* ptr;
            ComponentDescriptor descriptor;  // Stored BY VALUE: teardown Destruct() must not
                                             // depend on the ComponentRegistry outliving this
                                             // slot, and a pointer-into-registry was a
                                             // dangling-pointer hazard across a rehash.
            ComponentID id;
            uint16_t size;      // Actual size of the resource
            bool isValid;       // true if slot contains valid resource

            ResourceSlot() : ptr(nullptr), descriptor{}, id(0), size(0), isValid(false) {}

            ResourceSlot(ResourceSlot&& other) noexcept :
                ptr(other.ptr), descriptor(other.descriptor), id(other.id),
                size(other.size), isValid(other.isValid)
            {
                // Steal the heap pointer; the payload object never moves in memory.
                other.ptr = nullptr;
                other.isValid = false;
            }

            ResourceSlot& operator=(ResourceSlot&& other) noexcept
            {
                if (this != &other)
                {
                    // Release any resource we already own before taking the source's.
                    if (isValid)
                    {
                        descriptor.Destruct(ptr);
                        FreeMemory(ptr, size);
                    }
                    ptr = other.ptr;
                    descriptor = other.descriptor;
                    id = other.id;
                    size = other.size;
                    isValid = other.isValid;
                    // Steal the heap pointer; the payload object never moves in memory.
                    other.ptr = nullptr;
                    other.isValid = false;
                }
                return *this;
            }
            ResourceSlot(const ResourceSlot&) = delete;
            ResourceSlot& operator=(const ResourceSlot&) = delete;
        };

        static constexpr uint16_t INVALID_INDEX = std::numeric_limits<uint16_t>::max();

        std::weak_ptr<ComponentRegistry> m_componentRegistry;
        std::vector<ResourceSlot> m_resources;
        std::array<uint16_t, MAX_COMPONENTS> m_sparse;
        Config m_config;
    };
} // namespace Astra
