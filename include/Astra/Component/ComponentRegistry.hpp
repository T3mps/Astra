#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>

#include "../Container/FlatMap.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Reflection/MetaRegistry.hpp"
#include "../Reflection/FieldVisitor.hpp"
#include "../Serialization/BinaryArchive.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "Component.hpp"

namespace Astra
{
    class ComponentRegistry
    {
    public:
        ComponentRegistry()
        {
            // m_hashToID (hash -> id) holds at most MAX_COMPONENTS entries and is
            // never erased; reserve past the ceiling so registration never
            // rehashes it. Descriptor POINTER stability is guaranteed structurally
            // by m_components being a fixed directly-indexed array (see the member
            // declaration), so GetComponentDescriptor() pointers stay valid for the
            // registry's life no matter how many types register.
            m_hashToID.Reserve(MAX_COMPONENTS * 2);
        }

        // A type whose identity collided with an already-registered type (see
        // TypeContext::GetOrAssignComponentID) resolves to INVALID_COMPONENT and
        // is refused here -- no descriptor is written for it.
        template<Component T>
        void RegisterComponent()
        {
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY  // guard before indexing m_registered
                return;
            if (m_registered[id].load(std::memory_order_acquire))
                return;                                // warm path: lock-free
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            if (m_registered[id].load(std::memory_order_relaxed))
                return;                                // double-check under lock
            RegisterComponentImpl<T>(id);              // may refuse (over-aligned) -- that's fine
            m_registered[id].store(true, std::memory_order_release);  // "attempt resolved for id"
        }

        // Hot-reload path: rebuilds the descriptor unconditionally so its function
        // pointers target the currently loaded module. The id is stable across
        // reloads because TypeID resolves through the shared TypeContext by hash.
        template<Component T>
        void ReRegisterComponent()
        {
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return;
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            RegisterComponentImpl<T>(id);
            m_registered[id].store(true, std::memory_order_release);
        }

        // Bulk, single-threaded setup path: do NOT call from workers. Each
        // per-type RegisterComponent() below re-locks m_registrationMutex, so
        // this must NOT hold the lock across the fan-out (std::mutex is
        // non-recursive -- that would deadlock).
        template<Component... Components>
        void RegisterComponents()
        {
            constexpr size_t count = sizeof...(Components);
            if (count == 0) return;

            (RegisterComponent<Components>(), ...);
        }
        
        ASTRA_NODISCARD const ComponentDescriptor* GetComponentDescriptor(ComponentID id) const
        {
            // Directly-indexed, pointer-stable: &m_components[id] never moves for
            // the registry's lifetime, so callers may cache it safely.
            return (id < MAX_COMPONENTS && m_present.Test(id)) ? &m_components[id] : nullptr;
        }
        
        ASTRA_NODISCARD const ComponentDescriptor* GetComponentDescriptorByHash(uint64_t hash) const
        {
            auto hashIt = m_hashToID.Find(hash);
            if (hashIt == m_hashToID.end())
                return nullptr;
            return GetComponentDescriptor(hashIt->second);
        }
        
        ASTRA_NODISCARD Result<ComponentID, std::string_view> GetComponentIDFromHash(uint64_t hash) const
        {
            auto it = m_hashToID.Find(hash);
            if (it == m_hashToID.end())
                return Result<ComponentID, std::string_view>::Err("Unknown component hash");
            return Result<ComponentID, std::string_view>::Ok(it->second);
        }

        // Invokes fn(ComponentID, const ComponentDescriptor&) for every registered
        // component in ascending id order. Replaces the former GetAllComponentIDs()
        // that leaked the internal container (and its unstable FlatMap iterators).
        template<typename Fn>
        void ForEachComponent(Fn&& fn) const
        {
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                if (m_present.Test(id))
                    fn(static_cast<ComponentID>(id), m_components[id]);
        }

        ASTRA_NODISCARD size_t Size() const
        {
            return m_present.Count();
        }

        void GetAllDescriptors(std::vector<ComponentDescriptor>& descriptors) const
        {
            descriptors.clear();
            descriptors.reserve(m_present.Count());
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                if (m_present.Test(id))
                    descriptors.push_back(m_components[id]);
        }

    private:
        template<Component T>
        void RegisterComponentImpl(ComponentID id)
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS,
                         "Component ID space exhausted (MAX_COMPONENTS); raise ASTRA_MAX_COMPONENTS");
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                // Refuse registration so failure is observable (descriptor
                // lookup returns nullptr; Registry::AddComponent returns
                // nullptr) instead of silently corrupting ComponentMask bits.
                return;
            }

            ComponentDescriptor desc;
            desc.id = id;
            // Empty components should report size 0 to avoid memory allocation
            desc.size = std::is_empty_v<T> ? 0 : sizeof(T);
            desc.alignment = std::is_empty_v<T> ? 1 : alignof(T);
            // All-config guard: chunk storage can only honor alignments up to
            // CACHE_LINE_SIZE; refuse registration so failure is observable
            // (descriptor lookup returns nullptr; Registry::AddComponent returns
            // nullptr) instead of handing back a descriptor the storage will
            // misalign. Must run BEFORE the ASTRA_ASSERT below so Debug builds
            // degrade the same way as Release/Dist (mirrors FieldInfo.hpp's
            // Get/Set/GetPtr guard-before-assert ordering).
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
            {
                return;
            }
            ASTRA_ASSERT(desc.alignment <= CACHE_LINE_SIZE,
                         "Component alignment above 64 bytes is not supported by chunk storage");

            desc.hash = TypeID<T>::Hash();

            // m_componentNames grows by one entry per (re)registration -- accepted cost
            // (tiny strings, rare path) in exchange for pointer-stable c_str() storage.
            auto nameView = TypeID<T>::Name();
            m_componentNames.emplace_back(nameView);
            desc.name = m_componentNames.back().c_str();

            desc.version = SerializationTraits<T>::Version;
            desc.minVersion = SerializationTraits<T>::MinVersion;

            desc.is_trivially_copyable = std::is_trivially_copyable_v<T>;
            desc.is_copy_constructible = std::is_copy_constructible_v<T>;
            desc.is_nothrow_move_constructible = std::is_nothrow_move_constructible_v<T>;
            desc.is_nothrow_default_constructible = std::is_nothrow_default_constructible_v<T>;
            desc.is_trivially_default_constructible = std::is_trivially_default_constructible_v<T>;
            desc.is_trivially_destructible = std::is_trivially_destructible_v<T>;
            desc.is_empty = std::is_empty_v<T>;
            desc.isEnableable = IsEnableableV<T>;

            // An enableable component must carry storage: the disabled bit lives
            // per-entity in the chunk's column data, and an empty/tag type has no
            // column (idToColumn == -1) to hang that bit off of. A runtime guard
            // would make such a type silently unfilterable instead of catching the
            // mistake at the call site, so refuse it at compile time.
            static_assert(!(std::is_empty_v<T> && IsEnableableV<T>),
                "An enableable component must have storage: an empty/tag type has no column to carry "
                "a disabled bit. Use a non-empty component, or a separate enableable marker with a byte of state.");

            desc.defaultConstruct = &DefaultConstruct<T>;
            desc.destruct = &Destruct<T>;
            desc.moveConstruct = &MoveConstruct<T>;
            desc.moveAssign = &MoveAssign<T>;

            if constexpr (std::is_copy_constructible_v<T>)
            {
                desc.copyConstruct = &CopyConstruct<T>;
                desc.copyAssign = &CopyAssign<T>;
                desc.constructWith = &ConstructWith<T>;
            }
            else
            {
                desc.copyConstruct = nullptr;
                desc.copyAssign = nullptr;
                desc.constructWith = nullptr;
            }

            desc.serialize = &Serialize<T>;
            desc.deserialize = &Deserialize<T>;
            desc.serializeVersioned = &SerializeVersioned<T>;
            desc.deserializeVersioned = &DeserializeVersioned<T>;

            // Link to reflection metadata if type is registered with MetaRegistry
            desc.meta = MetaRegistry::Instance().Get<T>();

            // Reflection-driven visitor slot: null unless the type is reflected.
            desc.visitFields = desc.meta ? &VisitFields<T> : nullptr;

            // Also link MetaRegistry to ComponentID for reverse lookup
            if (desc.meta)
            {
                MetaRegistry::Instance().LinkToComponent(desc.hash, id);
            }

            // Directly indexed; the array slot is pointer-stable for life.
            m_components[id] = desc;
            m_present.Set(id);
            m_hashToID[desc.hash] = id;
        }

        template<typename T>
        static void DefaultConstruct(void* ptr)
        {
            new (ptr) T{};
        }

        template<typename T>
        static void Destruct(void* ptr)
        {
            static_cast<T*>(ptr)->~T();
        }

        template<typename T>
        static void CopyConstruct(void* dst, const void* src)
        {
            new (dst) T(*static_cast<const T*>(src));
        }

        template<typename T>
        static void MoveConstruct(void* dst, void* src)
        {
            new (dst) T(std::move(*static_cast<T*>(src)));
        }

        template<typename T>
        static void MoveAssign(void* dst, void* src)
        {
            *static_cast<T*>(dst) = std::move(*static_cast<T*>(src));
        }

        template<typename T>
        static void CopyAssign(void* dst, const void* src)
        {
            *static_cast<T*>(dst) = *static_cast<const T*>(src);
        }
        
        template<typename T>
        static void ConstructWith(void* dst, const void* src)
        {
            // Use placement new with copy constructor for optimal construction
            new (dst) T(*static_cast<const T*>(src));
        }

        template<typename T>
        static void Serialize(BinaryWriter& writer, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            writer(*component);
        }

        template<typename T>
        static void Deserialize(BinaryReader& reader, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            reader(*component);
        }

        template<typename T>
        static void SerializeVersioned(BinaryWriter& writer, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            writer.WriteVersionedComponent(*component);
        }

        template<typename T>
        static bool DeserializeVersioned(BinaryReader& reader, void* ptr)
        {
            T* component = static_cast<T*>(ptr);
            auto result = reader.ReadVersionedComponent(*component);
            return result.IsOk();
        }

        template<typename T>
        static void VisitFields(void* instance, IFieldVisitor& visitor)
        {
            const TypeMeta* meta = MetaRegistry::Instance().Get<T>();
            if (!meta) return;
            for (const FieldInfo& field : meta->fields)
            {
                if (!field.IsSerializable()) continue;  // honors Serializable(false)
                visitor.Visit(field, instance);
            }
        }

        // Descriptors live in a directly-indexed, POINTER-STABLE array: ComponentID
        // is a dense 0..MAX_COMPONENTS-1 index (ComponentMask bit == ComponentID),
        // so a hash map keyed by it was both slower and unstable -- a FlatMap rehash
        // moved the slot a cached descriptor pointer referenced, dangling it (the
        // ResourceStorage teardown segfault). A fixed array never moves, so every
        // GetComponentDescriptor() pointer stays valid for the registry's life.
        std::array<ComponentDescriptor, MAX_COMPONENTS> m_components{};
        ComponentMask m_present;  // which ids in m_components hold a registered descriptor
        FlatMap<uint64_t, ComponentID> m_hashToID;  // hash -> id (sparse; stays a hash map)
        // Use deque instead of vector to prevent pointer invalidation when adding new names.
        // Vector reallocation would invalidate all c_str() pointers stored in ComponentDescriptor::name
        std::deque<std::string> m_componentNames;

        // First-registration guard. m_registered[id] is set once per type; the
        // warm path is a lock-free acquire-load (replacing the old Contains()
        // lookup). m_registrationMutex serializes the one-time cold path so two
        // workers first-registering different types can't race the containers.
        // non-copyable: holds a registration mutex + atomics (ComponentRegistry
        // is only ever held via std::shared_ptr; never copied/moved by value).
        std::mutex m_registrationMutex;
        std::atomic<bool> m_registered[MAX_COMPONENTS] = {};
    };
}