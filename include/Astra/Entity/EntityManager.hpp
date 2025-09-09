#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Serialization/BinaryReader.hpp"
#include "../Serialization/BinaryWriter.hpp"
#include "../Serialization/SerializationError.hpp"
#include "Entity.hpp"
#include "EntityIDStack.hpp"
#include "EntityTable.hpp"

namespace Astra
{
    class EntityManager : public std::enable_shared_from_this<EntityManager>
    {
    public:
        using VersionType = Entity::VersionType;
        using IDType = Entity::IDType;
        
        struct Config
        {
            EntityTable::Config tableConfig;
            
            // GCC fix: Remove conflicting default constructor, parameterized constructor handles defaults  
            Config(IDType segmentSize = 65536) :
                tableConfig(segmentSize)
            {}
        };

        static constexpr VersionType NULL_VERSION = EntityTable::NULL_VERSION;
        static constexpr VersionType INITIAL_VERSION = EntityTable::INITIAL_VERSION;
        static constexpr IDType INVALID_ID = EntityIDStack::INVALID_ID;

        EntityManager() = default;

        explicit EntityManager(std::size_t capacity)
        {
            Reserve(capacity);
        }
        
        explicit EntityManager(const Config& config) :
            m_idStack(),
            m_table(config.tableConfig),
            m_config(config)
        {}

        ASTRA_NODISCARD Entity Create() noexcept
        {
            auto [id, version] = m_idStack.Allocate();
            
            if (id == INVALID_ID) ASTRA_UNLIKELY
            {
                return Entity::Invalid();
            }
            
            m_table.SetVersion(id, version);
            return Entity(id, version);
        }

        template<typename OutputIt>
        void CreateBatch(std::size_t count, OutputIt out) noexcept
        {
            if (count == 0)
                ASTRA_UNLIKELY return;
            
            // For small batches, simple loop is fine
            if (count < 32) ASTRA_LIKELY
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    *out++ = Create();
                }
                return;
            }
            
            // Large batch: allocate IDs in batch
            SmallVector<EntityIDStack::VersionedID, 256> allocations;
            allocations.resize(count);
            
            size_t allocated = m_idStack.AllocateBatch(count, allocations.begin());
            
            // Set versions in batch and create entities
            for (size_t i = 0; i < allocated; ++i)
            {
                auto [id, version] = allocations[i];
                m_table.SetVersion(id, version);
                *out++ = Entity(id, version);
            }
        }

        bool Destroy(Entity entity) noexcept
        {
            if (!IsValid(entity)) ASTRA_UNLIKELY
            {
                return false;
            }

            const IDType id = entity.GetID();
            const VersionType currentVersion = entity.GetVersion();
            
            // Verify version matches
            if (m_table.GetVersion(id) != currentVersion) ASTRA_UNLIKELY
            {
                return false;
            }

            // Calculate next version with wraparound
            VersionType nextVersion = currentVersion + 1;
            if (nextVersion == NULL_VERSION) ASTRA_UNLIKELY  // Wrap from 255 to 1
            {
                nextVersion = INITIAL_VERSION;
            }

            // Mark as destroyed in table
            m_table.Destroy(id);

            // Recycle the ID with next version
            m_idStack.Recycle(id, nextVersion, true);  // preferLocal = true for segment locality
            
            return true;
        }

        template<typename InputIt>
        std::size_t DestroyBatch(InputIt first, InputIt last) noexcept
        {
            const size_t estimatedCount = std::distance(first, last);
            
            // For small batches, use simple approach
            if (estimatedCount < 32) ASTRA_LIKELY
            {
                std::size_t destroyed = 0;
                for (auto it = first; it != last; ++it)
                {
                    if (Destroy(*it)) ASTRA_LIKELY
                    {
                        ++destroyed;
                    }
                }
                return destroyed;
            }
            
            // Large batch: collect valid entities and their recycling info
            SmallVector<EntityIDStack::RecycledEntry, 256> toRecycle;
            std::size_t destroyed = 0;
            
            for (auto it = first; it != last; ++it)
            {
                if (!IsValid(*it)) ASTRA_UNLIKELY continue;
                
                IDType id = it->GetID();
                VersionType currentVersion = it->GetVersion();
                
                // Verify version matches
                if (m_table.GetVersion(id) != currentVersion) ASTRA_UNLIKELY continue;
                
                // Calculate next version
                VersionType nextVersion = currentVersion + 1;
                if (nextVersion == NULL_VERSION) ASTRA_UNLIKELY
                {
                    nextVersion = INITIAL_VERSION;
                }
                
                // Mark as destroyed
                m_table.Destroy(id);
                toRecycle.push_back({id, nextVersion});
                ++destroyed;
            }
            
            // Batch recycle the IDs
            if (!toRecycle.empty())
            {
                m_idStack.RecycleBatch(toRecycle.begin(), toRecycle.end());
            }
            
            return destroyed;
        }

        ASTRA_NODISCARD bool IsValid(Entity entity) const noexcept
        {
            const IDType id = entity.GetID();
            const VersionType version = entity.GetVersion();
            
            if (version == NULL_VERSION) return false;
            return m_table.IsAlive(id, version);
        }

        ASTRA_NODISCARD VersionType GetVersion(IDType id) const noexcept
        {
            return m_table.GetVersion(id);
        }

        void Clear() noexcept
        {
            m_idStack.Clear();
            m_table.Clear();
        }

        void Reserve(std::size_t capacity)
        {
            m_idStack.Reserve(capacity);
            m_table.Reserve(capacity);
        }

        ASTRA_NODISCARD std::size_t Size() const noexcept
        {
            return m_table.AliveCount();
        }

        ASTRA_NODISCARD std::size_t Capacity() const noexcept
        {
            return m_idStack.GetNextID();
        }

        ASTRA_NODISCARD std::size_t RecycledCount() const noexcept
        {
            return m_idStack.RecycledCount();
        }

        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_table.AliveCount() == 0;
        }

        void ShrinkToFit()
        {
            m_idStack.ShrinkToFit();
            m_table.ShrinkToFit();
        }

        class iterator
        {
        private:
            EntityTable::iterator m_iter;

        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entity;
            using difference_type = std::ptrdiff_t;
            using pointer = const Entity*;
            using reference = Entity;

            explicit iterator(EntityTable::iterator iter) noexcept : m_iter(iter) {}

            ASTRA_NODISCARD Entity operator*() const noexcept
            {
                auto [id, version] = *m_iter;
                return Entity(id, version);
            }

            iterator& operator++() noexcept
            {
                ++m_iter;
                return *this;
            }

            iterator operator++(int) noexcept
            {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            ASTRA_NODISCARD bool operator==(const iterator& other) const noexcept
            {
                return m_iter == other.m_iter;
            }

            ASTRA_NODISCARD bool operator!=(const iterator& other) const noexcept
            {
                return !(*this == other);
            }
        };

        ASTRA_NODISCARD iterator begin() const noexcept
        {
            return iterator(m_table.begin());
        }

        ASTRA_NODISCARD iterator end() const noexcept
        {
            return iterator(m_table.end());
        }

        void Validate() const noexcept
        {
#ifdef ASTRA_BUILD_DEBUG
            // Verify that alive count matches iteration count
            std::size_t aliveCount = 0;
            for (auto it = begin(); it != end(); ++it)
            {
                ++aliveCount;
            }
            
            assert(aliveCount == Size() && "Alive count mismatch");
            assert(m_idStack.GetNextID() <= Entity::ID_MASK + 1 && "Next ID overflow");
#endif
        }

        void Serialize(BinaryWriter& writer) const
        {
            // Write configuration (only table config now)
            writer(m_config.tableConfig.entitiesPerSegment);
            writer(m_config.tableConfig.entitiesPerSegmentShift);
            writer(m_config.tableConfig.entitiesPerSegmentMask);
            writer(m_config.tableConfig.releaseThreshold);
            writer(m_config.tableConfig.autoRelease);
            writer(m_config.tableConfig.maxEmptySegments);
            
            // Write ID stack state
            writer(m_idStack.GetNextID());
            
            // Get and write all recycled entries
            std::vector<EntityIDStack::RecycledEntry> recycledEntries;
            m_idStack.GetAllRecycledEntries(recycledEntries);
            writer(static_cast<uint32_t>(recycledEntries.size()));
            for (const auto& entry : recycledEntries)
            {
                writer(entry.id);
                writer(entry.nextVersion);
            }
            
            // Write table state
            writer(static_cast<uint32_t>(m_table.AliveCount()));
            
            // Write all alive entities
            for (auto it = m_table.begin(); it != m_table.end(); ++it)
            {
                auto [id, version] = *it;
                writer(id);
                writer(version);
            }
        }

        static Result<std::unique_ptr<EntityManager>, SerializationError> Deserialize(BinaryReader& reader)
        {
            auto manager = std::make_unique<EntityManager>();
            
            // Read configuration (only table config now)
            reader(manager->m_config.tableConfig.entitiesPerSegment);
            reader(manager->m_config.tableConfig.entitiesPerSegmentShift);
            reader(manager->m_config.tableConfig.entitiesPerSegmentMask);
            reader(manager->m_config.tableConfig.releaseThreshold);
            reader(manager->m_config.tableConfig.autoRelease);
            reader(manager->m_config.tableConfig.maxEmptySegments);
            
            if (reader.HasError())
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
            }
            
            // Read ID stack state
            IDType nextFreshID;
            reader(nextFreshID);
            
            // Read recycled entries
            uint32_t recycledCount;
            reader(recycledCount);
            std::vector<EntityIDStack::RecycledEntry> recycledEntries;
            recycledEntries.reserve(recycledCount);
            
            for (uint32_t i = 0; i < recycledCount; ++i)
            {
                IDType id;
                VersionType nextVersion;
                reader(id);
                reader(nextVersion);
                recycledEntries.push_back({id, nextVersion});
            }
            
            // Read table state
            uint32_t aliveCount;
            reader(aliveCount);
            
            if (reader.HasError())
            {
                return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
            }
            
            // Recreate manager with configuration
            manager->m_idStack = EntityIDStack();
            manager->m_table = EntityTable(manager->m_config.tableConfig);
            
            // Restore ID stack state
            manager->m_idStack.SetNextID(nextFreshID);
            manager->m_idStack.RestoreRecycledEntries(recycledEntries);
            
            // Read and restore alive entities
            for (uint32_t i = 0; i < aliveCount; ++i)
            {
                IDType id;
                VersionType version;
                reader(id);
                reader(version);
                
                if (reader.HasError())
                {
                    return Result<std::unique_ptr<EntityManager>, SerializationError>::Err(reader.GetError());
                }
                
                manager->m_table.SetVersion(id, version);
            }
            
            return Result<std::unique_ptr<EntityManager>, SerializationError>::Ok(std::move(manager));
        }

    private:
        EntityIDStack m_idStack;
        EntityTable m_table;
        Config m_config;
    };
}