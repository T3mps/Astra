#pragma once

#include <limits>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "Entity.hpp"

namespace Astra
{
    class EntityIDStack
    {
    public:
        using IDType = Entity::StorageType;
        using VersionType = Entity::VersionType;
        
        static constexpr IDType INVALID_ID = Entity::ID_MASK;
        static constexpr VersionType NULL_VERSION = 0;
        static constexpr VersionType INITIAL_VERSION = 1;
        
        struct RecycledEntry
        {
            IDType id;
            VersionType nextVersion;
        };
        
        struct VersionedID
        {
            IDType id;
            VersionType version;
            
            // Support for structured bindings
            template<std::size_t I>
            auto get() const
            {
                if constexpr (I == 0) return id;
                else if constexpr (I == 1) return version;
            }
        };
        
        EntityIDStack() = default;
        
        ASTRA_NODISCARD VersionedID Allocate() noexcept
        {
            if (!m_recycledIDs.empty())
            {
                auto entry = m_recycledIDs.back();
                m_recycledIDs.pop_back();
                return {entry.id, entry.nextVersion};
            }
            
            if (m_nextID > Entity::ID_MASK)
            {
                return {INVALID_ID, NULL_VERSION};
            }
            
            return {m_nextID++, INITIAL_VERSION};
        }
        
        template<typename OutputIt>
        size_t AllocateBatch(size_t count, OutputIt out) noexcept
        {
            size_t allocated = 0;
            
            // First, use recycled IDs
            size_t fromRecycled = std::min(count, m_recycledIDs.size());
            for (size_t i = 0; i < fromRecycled; ++i)
            {
                auto entry = m_recycledIDs.back();
                m_recycledIDs.pop_back();
                *out++ = VersionedID{entry.id, entry.nextVersion};
                ++allocated;
            }
            
            // Then allocate fresh IDs
            size_t remaining = count - fromRecycled;
            size_t availableFresh = (Entity::ID_MASK + 1) - m_nextID;
            size_t toAllocate = std::min(remaining, availableFresh);
            
            IDType startID = m_nextID;
            m_nextID += static_cast<IDType>(toAllocate);
            
            for (size_t i = 0; i < toAllocate; ++i)
            {
                *out++ = VersionedID{startID + static_cast<IDType>(i), INITIAL_VERSION};
                ++allocated;
            }
            
            return allocated;
        }
        
        void Recycle(IDType id, VersionType nextVersion) noexcept
        {
            ASTRA_ASSERT(id <= Entity::ID_MASK, "Invalid ID for recycling");
            m_recycledIDs.push_back({id, nextVersion});
        }
        
        void Recycle(IDType id, VersionType nextVersion, bool) noexcept
        {
            Recycle(id, nextVersion);  // Ignore preferLocal in simple design
        }
        
        template<typename InputIt>
        void RecycleBatch(InputIt first, InputIt last)
        {
            size_t count = std::distance(first, last);
            m_recycledIDs.reserve(m_recycledIDs.size() + count);
            
            for (auto it = first; it != last; ++it)
            {
                m_recycledIDs.push_back(*it);
            }
        }
        
        ASTRA_NODISCARD size_t RecycledCount() const noexcept
        {
            return m_recycledIDs.size();
        }
        
        ASTRA_NODISCARD bool HasAvailable() const noexcept
        {
            return !m_recycledIDs.empty() || m_nextID <= Entity::ID_MASK;
        }
        
        void Reserve(size_t capacity)
        {
            m_recycledIDs.reserve(capacity);
        }
        
        void HintDestroyCount(size_t count)
        {
            m_recycledIDs.reserve(m_recycledIDs.size() + count);
        }
        
        void Clear() noexcept
        {
            m_recycledIDs.clear();
            m_nextID = 0;
        }
        
        void ShrinkToFit()
        {
            m_recycledIDs.shrink_to_fit();
        }
        
        void GetAllRecycledEntries(std::vector<RecycledEntry>& outEntries) const
        {
            outEntries.clear();
            outEntries.reserve(m_recycledIDs.size());
            outEntries.insert(outEntries.end(), m_recycledIDs.begin(), m_recycledIDs.end());
        }
        
        void RestoreRecycledEntries(const std::vector<RecycledEntry>& entries)
        {
            m_recycledIDs.clear();
            m_recycledIDs.reserve(entries.size());
            for (const auto& entry : entries)
            {
                m_recycledIDs.push_back(entry);
            }
        }

        ASTRA_NODISCARD IDType GetNextID() const noexcept
        {
            return m_nextID;
        }

        void SetNextID(IDType id)
        {
            m_nextID = id;
        }

    private:
        SmallVector<RecycledEntry, 256> m_recycledIDs;  // 256 entries inline (~1.25KB)
        IDType m_nextID = 0;
    };
}

// Template specializations for structured bindings support
namespace std
{
    template<>
    struct tuple_size<Astra::EntityIDStack::VersionedID> : std::integral_constant<std::size_t, 2> {};
    
    template<std::size_t I>
    struct tuple_element<I, Astra::EntityIDStack::VersionedID>
    {
        using type = std::conditional_t<I == 0, Astra::Entity::StorageType, Astra::Entity::VersionType>;
    };
}