#pragma once

#include <deque>
#include <memory>
#include <string_view>
#include <type_traits>

#include "../Container/FlatMap.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Reflection/MetaRegistry.hpp"
#include "../Serialization/BinaryArchive.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "Component.hpp"

namespace Astra
{
    class ComponentRegistry
    {
    public:
        template<Component T>
        void RegisterComponent()
        {
            ComponentID id = TypeID<T>::Value();
            if (m_components.Contains(id))
                return;
                
            ComponentDescriptor desc;
            desc.id = id;
            // Empty components should report size 0 to avoid memory allocation
            desc.size = std::is_empty_v<T> ? 0 : sizeof(T);
            desc.alignment = std::is_empty_v<T> ? 1 : alignof(T);
            
            desc.hash = TypeID<T>::Hash();
            
            #ifdef ASTRA_BUILD_DEBUG
            if (auto it = m_hashToID.Find(desc.hash); it != m_hashToID.end())
            {
                auto existing = GetComponentDescriptor(it->second);
                if (existing)
                {
                    // We're comparing type names to detect collision
                    // If names are different but hash is same = collision!
                    auto currentName = TypeID<T>::Name();
                    std::string_view existingName = existing->name ? existing->name : "";
                    ASTRA_ASSERT(currentName == existingName, "Hash collision detected! Components have same hash but different types");
                }
            }
            #endif
            
            auto nameView = TypeID<T>::Name();
            m_componentNames.emplace_back(nameView);
            desc.name = m_componentNames.back().c_str();
            
            desc.version = SerializationTraits<T>::Version;
            desc.minVersion = SerializationTraits<T>::MinVersion;
            
            desc.is_trivially_copyable = std::is_trivially_copyable_v<T>;
            desc.is_copy_constructible = std::is_copy_constructible_v<T>;
            desc.is_nothrow_move_constructible = std::is_nothrow_move_constructible_v<T>;
            desc.is_nothrow_default_constructible = std::is_nothrow_default_constructible_v<T>;
            desc.is_empty = std::is_empty_v<T>;
            
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

            // Also link MetaRegistry to ComponentID for reverse lookup
            if (desc.meta)
            {
                MetaRegistry::Instance().LinkToComponent(desc.hash, id);
            }

            m_components[id] = desc;
            m_hashToID[desc.hash] = id;
        }

        template<Component... Components>
        void RegisterComponents()
        {
            constexpr size_t count = sizeof...(Components);
            if (count == 0) return;

            m_components.Reserve(m_components.Size() + count);

            (RegisterComponent<Components>(), ...);
        }
        
        ASTRA_NODISCARD const ComponentDescriptor* GetComponentDescriptor(ComponentID id) const
        {
            auto it = m_components.Find(id);
            return it != m_components.end() ? &it->second : nullptr;
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

        ASTRA_NODISCARD const FlatMap<ComponentID, ComponentDescriptor>& GetAllComponentIDs() const
        {
            return m_components;
        }
        
        ASTRA_NODISCARD size_t Size() const
        {
            return m_components.Size();
        }

        void GetAllDescriptors(std::vector<ComponentDescriptor>& descriptors) const
        {
            descriptors.clear();
            if (m_components.Empty())
                return;

            descriptors.reserve(m_components.Size());
            for (const auto& [id, desc] : m_components)
            {
                descriptors.push_back(desc);
            }
        }

    private:
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

        FlatMap<ComponentID, ComponentDescriptor> m_components;
        FlatMap<uint64_t, ComponentID> m_hashToID;
        // Use deque instead of vector to prevent pointer invalidation when adding new names.
        // Vector reallocation would invalidate all c_str() pointers stored in ComponentDescriptor::name
        std::deque<std::string> m_componentNames;
    };
}