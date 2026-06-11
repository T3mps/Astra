#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "../Container/SmallVector.hpp"
#include "../Core/Base.hpp"
#include "../Core/Result.hpp"
#include "../Core/TypeID.hpp"
#include "../Registry/Registry.hpp"
#include "Command.hpp"

namespace Astra
{
    /**
     * Error types for command buffer execution.
     */
    enum class CommandError
    {
        None,
        InvalidRegistry,
        ExecutionFailed,
        AllocationFailed
    };

    /**
     * Result of command buffer execution.
     */
    struct ExecutionResult
    {
        CommandError error = CommandError::None;
        size_t executedCount = 0;
        size_t totalCount = 0;

        [[nodiscard]] bool IsOk() const noexcept { return error == CommandError::None; }
        [[nodiscard]] bool IsErr() const noexcept { return error != CommandError::None; }
    };

    /**
     * Internal byte buffer for storing commands contiguously.
     * Commands are stored as [Header][Payload] pairs.
     */
    class CommandByteBuffer
    {
    public:
        static constexpr size_t DEFAULT_INITIAL_CAPACITY = 4096;
        static constexpr size_t ALIGNMENT = 8;

        explicit CommandByteBuffer(size_t initialCapacity = DEFAULT_INITIAL_CAPACITY)
        {
            m_data.reserve(initialCapacity);
        }

        /**
         * Allocate space in the buffer for a command.
         * @param size Total size needed (header + payload + data)
         * @return Pointer to the allocated space, or nullptr if allocation failed
         */
        std::byte* Allocate(size_t size, size_t* outAlignedSize = nullptr)
        {
            // Align the size to 8 bytes
            size_t alignedSize = AlignUp(size, ALIGNMENT);

            size_t currentSize = m_data.size();
            m_data.resize(currentSize + alignedSize);

            if (outAlignedSize)
                *outAlignedSize = alignedSize;

            return m_data.data() + currentSize;
        }

        /**
         * Get pointer to the beginning of the buffer.
         */
        [[nodiscard]] std::byte* Data() noexcept { return m_data.data(); }
        [[nodiscard]] const std::byte* Data() const noexcept { return m_data.data(); }

        /**
         * Get the current size of the buffer in bytes.
         */
        [[nodiscard]] size_t Size() const noexcept { return m_data.size(); }

        /**
         * Check if the buffer is empty.
         */
        [[nodiscard]] bool IsEmpty() const noexcept { return m_data.empty(); }

        /**
         * Clear the buffer, but keep the allocated capacity.
         */
        void Clear() noexcept { m_data.clear(); }

        /**
         * Reserve capacity in the buffer.
         */
        void Reserve(size_t capacity) { m_data.reserve(capacity); }

    private:
        std::vector<std::byte> m_data;
    };

    /**
     * CommandBuffer stores deferred operations to be executed on a Registry.
     *
     * This implementation uses a type-erased byte buffer approach where commands and
     * component data are stored inline in a contiguous buffer. This avoids the UB issues
     * with lambda captures and provides better cache locality.
     *
     * IMPORTANT: Execution is NOT fully transactional. If a command fails during Execute():
     * - Commands that already executed successfully are NOT rolled back
     * - Only pre-allocated entities that haven't been processed yet are destroyed
     * - Use smaller command buffers if you need atomic all-or-nothing semantics
     * - Consider validating preconditions before adding commands
     *
     * Usage:
     *   CommandBuffer cmd(&registry);
     *   Entity e = cmd.CreateEntity();
     *   cmd.AddComponent(e, Position{1, 2, 3});
     *   auto result = cmd.Execute();
     *   if (result.IsErr()) { // handle error }
     */
    class CommandBuffer
    {
    public:
        using ExecutionError = CommandError;

        explicit CommandBuffer(Registry* registry) :
            m_registry(registry)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
        }

        ~CommandBuffer()
        {
            // Clean up any pending commands that have destructors
            CleanupPendingCommands();
        }

        // ============= Entity Commands =============

        /**
         * Create a new entity. The entity ID is allocated immediately,
         * but the entity is not added to the archetype system until Execute().
         */
        Entity CreateEntity()
        {
            if (!m_registry)
                return Entity::Invalid();

            auto& manager = m_registry->GetEntityManager();
            Entity entity = manager.Create();
            if (entity == Entity::Invalid())
                return Entity::Invalid();

            m_allocatedEntities.push_back(entity);

            // Write command to buffer
            size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntityPayload);
            size_t alignedSize = 0;
            std::byte* ptr = m_buffer.Allocate(totalSize, &alignedSize);

            auto* header = new (ptr) CommandHeader{CommandType::CreateEntity, 0, static_cast<uint32_t>(alignedSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntityPayload{entity};
            (void)header;
            (void)payload;

            m_commandCount++;
            return entity;
        }

        /**
         * Destroy an entity. The entity is destroyed when Execute() is called.
         */
        void DestroyEntity(Entity entity)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntityPayload);
            size_t alignedSize = 0;
            std::byte* ptr = m_buffer.Allocate(totalSize, &alignedSize);

            auto* header = new (ptr) CommandHeader{CommandType::DestroyEntity, 0, static_cast<uint32_t>(alignedSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) DestroyEntityPayload{entity};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Create multiple entities at once.
         */
        void CreateEntities(size_t count, Entity* outEntities)
        {
            if (!m_registry || count == 0)
                return;

            auto& manager = m_registry->GetEntityManager();
            manager.CreateBatch(count, outEntities);

            // Track for potential rollback
            for (size_t i = 0; i < count; ++i)
            {
                m_allocatedEntities.push_back(outEntities[i]);
            }

            // Calculate total size
            size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntitiesPayload) + count * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::CreateEntities, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntitiesPayload{static_cast<uint32_t>(count)};
            (void)header;

            // Copy entities after payload
            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, outEntities, count * sizeof(Entity));

            m_commandCount++;
        }

        /**
         * Destroy multiple entities at once.
         */
        void DestroyEntities(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t count = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(DestroyEntitiesPayload) + count * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::DestroyEntities, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) DestroyEntitiesPayload{static_cast<uint32_t>(count)};
            (void)header;

            // Copy entities after payload
            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, entities.data(), count * sizeof(Entity));

            m_commandCount++;
        }

        // ============= Component Commands =============

        /**
         * Add a component to an entity. The component data is stored inline in the buffer.
         */
        template<Component T>
        void AddComponent(Entity entity, T&& component)
        {
            using DecayedT = std::decay_t<T>;

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            // Calculate total size with alignment padding
            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(AddComponentPayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::AddComponent, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) AddComponentPayload{
                entity,
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                &DestructComponent<DecayedT>
            };
            (void)payload;

            // Write component data inline (properly aligned)
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(std::forward<T>(component));

            m_commandCount++;
        }

        /**
         * Emplace a component on an entity with constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceComponent(Entity entity, Args&&... args)
        {
            // Create the component and add it
            AddComponent<T>(entity, T(std::forward<Args>(args)...));
        }

        /**
         * Remove a component from an entity.
         */
        template<Component T>
        void RemoveComponent(Entity entity)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveComponentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveComponent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveComponentPayload{entity, TypeID<T>::Value()};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a component to multiple entities with the same value.
         */
        template<Component T>
        void AddComponents(std::span<const Entity> entities, const T& component)
        {
            if (entities.empty())
                return;

            using DecayedT = std::decay_t<T>;

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            size_t entityCount = entities.size();
            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(AddComponentBatchPayload);
            size_t entitiesSize = entityCount * sizeof(Entity);
            size_t dataOffset = AlignUp(headerSize + payloadSize + entitiesSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::AddComponentBatch, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) AddComponentBatchPayload{
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                static_cast<uint32_t>(entityCount),
                &DestructComponent<DecayedT>
            };

            // Copy entities
            Entity* entityDst = payload->GetEntitiesPtr();
            std::memcpy(entityDst, entities.data(), entitiesSize);

            // Write component data inline
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(component);

            m_commandCount++;
        }

        /**
         * Emplace a component on multiple entities with the same constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceComponents(std::span<const Entity> entities, Args&&... args)
        {
            AddComponents<T>(entities, T(std::forward<Args>(args)...));
        }

        /**
         * Remove a component from multiple entities.
         */
        template<Component T>
        void RemoveComponents(std::span<const Entity> entities)
        {
            if (entities.empty())
                return;

            size_t entityCount = entities.size();
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveComponentBatchPayload) + entityCount * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveComponentBatch, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveComponentBatchPayload{
                TypeID<T>::Value(),
                0,  // padding
                static_cast<uint32_t>(entityCount)
            };
            (void)header;

            // Copy entities
            Entity* entityDst = payload->GetEntitiesPtr();
            std::memcpy(entityDst, entities.data(), entityCount * sizeof(Entity));

            m_commandCount++;
        }

        // ============= Relationship Commands =============

        /**
         * Set the parent of an entity.
         */
        void SetParent(Entity child, Entity parent)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(SetParentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::SetParent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) SetParentPayload{child, parent};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a child to a parent (same as SetParent with reversed parameters).
         */
        void AddChild(Entity parent, Entity child)
        {
            SetParent(child, parent);
        }

        /**
         * Remove the parent from an entity.
         */
        void RemoveParent(Entity child)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveParentPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveParent, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveParentPayload{child};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove a specific child from a parent.
         */
        void RemoveChild(Entity parent, Entity child)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveChildPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveChild, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveChildPayload{parent, child};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove all children from a parent.
         */
        void RemoveAllChildren(Entity parent)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveAllChildrenPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveAllChildren, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveAllChildrenPayload{parent};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Add a bidirectional link between two entities.
         */
        void AddLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(AddLinkPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::AddLink, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) AddLinkPayload{a, b};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Remove a bidirectional link between two entities.
         */
        void RemoveLink(Entity a, Entity b)
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveLinkPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveLink, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveLinkPayload{a, b};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        // ============= Resource Commands =============

        /**
         * Set a global resource.
         */
        template<Component T>
        void SetResource(T&& resource)
        {
            using DecayedT = std::decay_t<T>;

            // Register component type
            m_registry->GetComponentRegistry()->RegisterComponent<DecayedT>();

            constexpr size_t dataSize = sizeof(DecayedT);
            constexpr size_t dataAlignment = alignof(DecayedT);

            size_t headerSize = sizeof(CommandHeader);
            size_t payloadSize = sizeof(SetResourcePayload);
            size_t dataOffset = AlignUp(headerSize + payloadSize, dataAlignment);
            size_t totalSize = dataOffset + dataSize;

            std::byte* ptr = m_buffer.Allocate(totalSize);

            // Write header
            auto* header = new (ptr) CommandHeader{CommandType::SetResource, 0, static_cast<uint32_t>(totalSize)};
            (void)header;

            // Write payload
            auto* payload = new (ptr + headerSize) SetResourcePayload{
                TypeID<DecayedT>::Value(),
                static_cast<uint16_t>(dataSize),
                static_cast<uint16_t>(dataAlignment),
                0,  // padding
                &DestructComponent<DecayedT>
            };
            (void)payload;

            // Write resource data inline
            void* dataPtr = ptr + dataOffset;
            new (dataPtr) DecayedT(std::forward<T>(resource));

            m_commandCount++;
        }

        /**
         * Emplace a global resource with constructor arguments.
         */
        template<Component T, typename... Args>
        void EmplaceResource(Args&&... args)
        {
            SetResource<T>(T(std::forward<Args>(args)...));
        }

        /**
         * Remove a global resource.
         */
        template<Component T>
        void RemoveResource()
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(RemoveResourcePayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::RemoveResource, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) RemoveResourcePayload{TypeID<T>::Value()};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        /**
         * Clear all global resources.
         */
        void ClearResources()
        {
            size_t totalSize = sizeof(CommandHeader) + sizeof(ClearResourcesPayload);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::ClearResources, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) ClearResourcesPayload{};
            (void)header;
            (void)payload;

            m_commandCount++;
        }

        // ============= Execution and Management =============

        /**
         * Execute all recorded commands.
         * On success, the buffer is cleared (if clearAfterExecution is true).
         *
         * On failure:
         * - Commands that already executed remain in effect (NOT rolled back)
         * - Pre-allocated entities that weren't processed yet are destroyed
         * - Component data in the buffer is cleaned up
         * - Returns error with m_lastExecutedCount set for debugging
         *
         * @param clearAfterExecution If true, clears the buffer after successful execution
         * @return Result indicating success or the type of failure
         */
        Result<void, ExecutionError> Execute(bool clearAfterExecution = true)
        {
            if (!m_registry)
            {
                RollbackAllocatedEntities();
                return Result<void, ExecutionError>::Err(ExecutionError::InvalidRegistry);
            }

            std::byte* ptr = m_buffer.Data();
            std::byte* end = ptr + m_buffer.Size();
            m_lastExecutedCount = 0;

            while (ptr < end)
            {
                auto* header = reinterpret_cast<CommandHeader*>(ptr);
                std::byte* payloadPtr = ptr + sizeof(CommandHeader);

                bool success = ExecuteCommand(header->type, payloadPtr);

                if (!success)
                {
                    // Partial execution occurred - clean up what we can
                    // Note: Already-executed commands are NOT rolled back
                    RollbackAllocatedEntities();
                    CleanupPendingCommands();
                    m_buffer.Clear();
                    m_commandCount = 0;
                    return Result<void, ExecutionError>::Err(ExecutionError::ExecutionFailed);
                }

                // Advance by aligned size (buffer allocates with 8-byte alignment)
                ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
                m_lastExecutedCount++;
            }

            // Success - clear allocated entities tracking
            m_allocatedEntities.clear();

            if (clearAfterExecution)
            {
                Clear();
            }

            return Result<void, ExecutionError>::Ok();
        }

        /**
         * Get the number of commands that were successfully executed in the last Execute() call.
         * Useful for debugging partial execution failures.
         */
        [[nodiscard]] size_t GetLastExecutedCount() const noexcept { return m_lastExecutedCount; }

        /**
         * Clear all pending commands without executing them.
         * Also cleans up any component data destructors.
         */
        void Clear()
        {
            CleanupPendingCommands();
            m_buffer.Clear();
            m_allocatedEntities.clear();
            m_commandCount = 0;
        }

        /**
         * Reserve space in the command buffer for the expected number of bytes.
         */
        void Reserve(size_t bytes)
        {
            m_buffer.Reserve(bytes);
        }

        /**
         * Merge commands from another buffer into this one.
         * The other buffer is left empty after the merge.
         */
        void MergeFrom(CommandBuffer&& other)
        {
            // Copy buffer data
            size_t otherSize = other.m_buffer.Size();
            if (otherSize > 0)
            {
                std::byte* dst = m_buffer.Allocate(otherSize);
                std::memcpy(dst, other.m_buffer.Data(), otherSize);
            }

            // Merge allocated entities
            m_allocatedEntities.insert(
                m_allocatedEntities.end(),
                other.m_allocatedEntities.begin(),
                other.m_allocatedEntities.end()
            );

            m_commandCount += other.m_commandCount;

            // Clear other buffer (don't call CleanupPendingCommands since we copied the data)
            other.m_buffer.Clear();
            other.m_allocatedEntities.clear();
            other.m_commandCount = 0;
        }

        /**
         * Get the number of commands in the buffer.
         */
        [[nodiscard]] size_t GetCommandCount() const noexcept
        {
            return m_commandCount;
        }

        /**
         * Check if the buffer is empty.
         */
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_commandCount == 0;
        }

        /**
         * Get the memory usage of the command buffer in bytes.
         */
        [[nodiscard]] size_t GetMemoryUsage() const noexcept
        {
            return m_buffer.Size() + m_allocatedEntities.capacity() * sizeof(Entity);
        }

        /**
         * Rollback all entities that were allocated but not yet added to archetypes.
         */
        void RollbackAllocatedEntities()
        {
            if (m_registry)
            {
                auto& manager = m_registry->GetEntityManager();
                for (Entity e : m_allocatedEntities)
                {
                    manager.Destroy(e);
                }
            }
            m_allocatedEntities.clear();
        }

    private:
        /**
         * Destructor function for component cleanup.
         */
        template<typename T>
        static void DestructComponent(void* ptr)
        {
            static_cast<T*>(ptr)->~T();
        }

        /**
         * Execute a single command from the buffer.
         */
        bool ExecuteCommand(CommandType type, std::byte* payload)
        {
            switch (type)
            {
                case CommandType::CreateEntity:
                    return ExecuteCreateEntity(payload);
                case CommandType::DestroyEntity:
                    return ExecuteDestroyEntity(payload);
                case CommandType::CreateEntities:
                    return ExecuteCreateEntities(payload);
                case CommandType::DestroyEntities:
                    return ExecuteDestroyEntities(payload);
                case CommandType::AddComponent:
                    return ExecuteAddComponent(payload);
                case CommandType::RemoveComponent:
                    return ExecuteRemoveComponent(payload);
                case CommandType::AddComponentBatch:
                    return ExecuteAddComponentBatch(payload);
                case CommandType::RemoveComponentBatch:
                    return ExecuteRemoveComponentBatch(payload);
                case CommandType::SetParent:
                    return ExecuteSetParent(payload);
                case CommandType::RemoveParent:
                    return ExecuteRemoveParent(payload);
                case CommandType::AddChild:
                    return ExecuteSetParent(payload);  // Same implementation
                case CommandType::RemoveChild:
                    return ExecuteRemoveChild(payload);
                case CommandType::RemoveAllChildren:
                    return ExecuteRemoveAllChildren(payload);
                case CommandType::AddLink:
                    return ExecuteAddLink(payload);
                case CommandType::RemoveLink:
                    return ExecuteRemoveLink(payload);
                case CommandType::SetResource:
                    return ExecuteSetResource(payload);
                case CommandType::RemoveResource:
                    return ExecuteRemoveResource(payload);
                case CommandType::ClearResources:
                    return ExecuteClearResources(payload);
                default:
                    return false;
            }
        }

        // ============= Command Executors =============

        bool ExecuteCreateEntity(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<CreateEntityPayload*>(payload);
            m_registry->GetArchetypeManager()->AddEntity(cmd->entity);
            m_registry->GetSignalManager()->Emit<Events::EntityCreated>(cmd->entity);
            return true;
        }

        bool ExecuteDestroyEntity(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<DestroyEntityPayload*>(payload);
            if (cmd->entity == Entity::Invalid())
                return false;
            m_registry->DestroyEntity(cmd->entity);
            return true;
        }

        bool ExecuteCreateEntities(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<CreateEntitiesPayload*>(payload);
            Entity* entities = reinterpret_cast<Entity*>(cmd + 1);

            auto* archetypeManager = m_registry->GetArchetypeManager();
            auto* signalManager = m_registry->GetSignalManager();

            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                archetypeManager->AddEntity(entities[i]);
                signalManager->Emit<Events::EntityCreated>(entities[i]);
            }
            return true;
        }

        bool ExecuteDestroyEntities(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<DestroyEntitiesPayload*>(payload);
            Entity* entities = reinterpret_cast<Entity*>(cmd + 1);

            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(cmd->entityCount);

            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    validEntities.push_back(entities[i]);
                }
            }

            if (!validEntities.empty())
            {
                m_registry->DestroyEntities(validEntities);
            }
            return true;
        }

        bool ExecuteAddComponent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentPayload*>(payload);

            if (cmd->entity == Entity::Invalid())
                return false;

            const void* data = cmd->GetDataPtr();
            return m_registry->AddComponentByID(cmd->entity, cmd->componentId, data, cmd->dataSize);
        }

        bool ExecuteRemoveComponent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentPayload*>(payload);
            if (cmd->entity == Entity::Invalid())
                return false;

            return m_registry->RemoveComponentByID(cmd->entity, cmd->componentId);
        }

        bool ExecuteAddComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();
            const void* data = cmd->GetDataPtr();

            // Use direct single-entity calls to avoid any span conversion issues
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    m_registry->AddComponentByID(entities[i], cmd->componentId, data, cmd->dataSize);
                }
            }
            return true;
        }

        bool ExecuteRemoveComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();

            // Use direct single-entity calls to avoid any span conversion issues
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] != Entity::Invalid())
                {
                    m_registry->RemoveComponentByID(entities[i], cmd->componentId);
                }
            }
            return true;
        }

        bool ExecuteSetParent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<SetParentPayload*>(payload);
            if (cmd->child == Entity::Invalid() || cmd->parent == Entity::Invalid())
                return false;
            m_registry->SetParent(cmd->child, cmd->parent);
            return true;
        }

        bool ExecuteRemoveParent(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveParentPayload*>(payload);
            if (cmd->child == Entity::Invalid())
                return false;
            m_registry->RemoveParent(cmd->child);
            return true;
        }

        bool ExecuteRemoveChild(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveChildPayload*>(payload);
            if (cmd->parent == Entity::Invalid() || cmd->child == Entity::Invalid())
                return false;
            m_registry->RemoveChild(cmd->parent, cmd->child);
            return true;
        }

        bool ExecuteRemoveAllChildren(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveAllChildrenPayload*>(payload);
            if (cmd->parent == Entity::Invalid())
                return false;
            m_registry->RemoveAllChildren(cmd->parent);
            return true;
        }

        bool ExecuteAddLink(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddLinkPayload*>(payload);
            if (cmd->a == Entity::Invalid() || cmd->b == Entity::Invalid())
                return false;
            m_registry->AddLink(cmd->a, cmd->b);
            return true;
        }

        bool ExecuteRemoveLink(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveLinkPayload*>(payload);
            if (cmd->a == Entity::Invalid() || cmd->b == Entity::Invalid())
                return false;
            m_registry->RemoveLink(cmd->a, cmd->b);
            return true;
        }

        bool ExecuteSetResource(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<SetResourcePayload*>(payload);
            const void* data = cmd->GetDataPtr();
            return m_registry->SetResourceByID(cmd->componentId, data, cmd->dataSize);
        }

        bool ExecuteRemoveResource(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveResourcePayload*>(payload);
            return m_registry->RemoveResourceByID(cmd->componentId);
        }

        bool ExecuteClearResources([[maybe_unused]] std::byte* payload)
        {
            m_registry->ClearResources();
            return true;
        }

        /**
         * Clean up any component data stored in pending commands.
         * Called during Clear() and destructor.
         */
        void CleanupPendingCommands()
        {
            std::byte* ptr = m_buffer.Data();
            std::byte* end = ptr + m_buffer.Size();

            while (ptr < end)
            {
                auto* header = reinterpret_cast<CommandHeader*>(ptr);
                std::byte* payloadPtr = ptr + sizeof(CommandHeader);

                // Only need to cleanup commands with inline component data
                switch (header->type)
                {
                    case CommandType::AddComponent:
                    {
                        auto* cmd = reinterpret_cast<AddComponentPayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    case CommandType::AddComponentBatch:
                    {
                        auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    case CommandType::SetResource:
                    {
                        auto* cmd = reinterpret_cast<SetResourcePayload*>(payloadPtr);
                        if (cmd->destructor)
                        {
                            cmd->destructor(cmd->GetDataPtr());
                        }
                        break;
                    }
                    default:
                        break;
                }

                // Advance by aligned size (buffer allocates with 8-byte alignment)
                ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
            }
        }

        Registry* m_registry;
        CommandByteBuffer m_buffer;
        std::vector<Entity> m_allocatedEntities;
        size_t m_commandCount = 0;
        size_t m_lastExecutedCount = 0;  // For debugging partial execution failures
    };

    /**
     * Thread-safe command buffer that provides per-thread buffers.
     * Commands from all threads are executed sequentially when Execute() is called.
     */
    class ParallelCommandBuffer
    {
    public:
        explicit ParallelCommandBuffer(Registry* registry) :
            m_registry(registry)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
            // Pre-reserve space for typical thread counts
            const size_t expectedThreads = std::thread::hardware_concurrency();
            m_buffers.reserve(expectedThreads);
        }

        /**
         * Get the command buffer for the current thread.
         * Creates a new buffer if one doesn't exist for this thread.
         *
         * Relies on stable OS-thread identity (thread_local cache): callers
         * running on fiber-based job systems must pin the fiber to its thread
         * while recording commands, or the buffer of a different thread may
         * be written concurrently.
         */
        CommandBuffer& GetThreadBuffer() const
        {
            // Fast path: check thread-local cache
            if (t_cache.context == this && t_cache.buffer != nullptr)
            {
                return *t_cache.buffer;
            }

            // Slow path: create new buffer for this thread
            return InitializeThreadBuffer();
        }

        /**
         * Execute all commands from all thread buffers.
         */
        Result<void, CommandBuffer::ExecutionError> Execute()
        {
            for (size_t i = 0; i < m_buffers.size(); ++i)
            {
                if (m_buffers[i] && !m_buffers[i]->IsEmpty())
                {
                    auto result = m_buffers[i]->Execute();
                    if (result.IsErr())
                    {
                        // Rollback remaining buffers' allocated entities
                        for (size_t j = i + 1; j < m_buffers.size(); ++j)
                        {
                            if (m_buffers[j])
                            {
                                m_buffers[j]->RollbackAllocatedEntities();
                            }
                        }
                        return result;
                    }
                }
            }
            return Result<void, CommandBuffer::ExecutionError>::Ok();
        }

        /**
         * Merge all thread buffers into a single target buffer.
         */
        void MergeInto(CommandBuffer& target)
        {
            for (auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    target.MergeFrom(std::move(*buffer));
                }
            }
        }

        /**
         * Clear all thread buffers.
         */
        void Clear()
        {
            for (auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    buffer->Clear();
                }
            }
        }

        /**
         * Get the total number of commands across all thread buffers.
         */
        [[nodiscard]] size_t GetCommandCount() const
        {
            size_t total = 0;
            for (const auto& buffer : m_buffers)
            {
                if (buffer)
                {
                    total += buffer->GetCommandCount();
                }
            }
            return total;
        }

        /**
         * Check if all thread buffers are empty.
         */
        [[nodiscard]] bool IsEmpty() const
        {
            for (const auto& buffer : m_buffers)
            {
                if (buffer && !buffer->IsEmpty())
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * Get the number of thread buffers that have been created.
         */
        [[nodiscard]] size_t GetThreadCount() const
        {
            return m_buffers.size();
        }

    private:
        CommandBuffer& InitializeThreadBuffer() const
        {
            // Allocate a new index for this thread
            const size_t index = m_nextIndex.fetch_add(1, std::memory_order_relaxed);

            // Lock only for vector modification
            std::unique_lock lock(m_mutex);

            // Ensure vector is large enough
            if (index >= m_buffers.size())
            {
                m_buffers.resize(index + 1);
            }

            // Create the buffer if it doesn't exist
            if (!m_buffers[index])
            {
                m_buffers[index] = std::make_unique<CommandBuffer>(m_registry);
            }

            CommandBuffer* buffer = m_buffers[index].get();

            // Unlock before updating thread-local cache
            lock.unlock();

            // Update thread-local cache
            t_cache.context = const_cast<ParallelCommandBuffer*>(this);
            t_cache.buffer = buffer;
            t_cache.index = index;

            return *buffer;
        }

        Registry* m_registry;
        mutable std::mutex m_mutex;
        mutable std::vector<std::unique_ptr<CommandBuffer>> m_buffers;
        mutable std::atomic<size_t> m_nextIndex{0};

        // Thread-local cache to avoid repeated lookups
        struct ThreadCache
        {
            ParallelCommandBuffer* context = nullptr;
            CommandBuffer* buffer = nullptr;
            size_t index = std::numeric_limits<size_t>::max();
        };

        static thread_local ThreadCache t_cache;
    };

    // Thread-local storage definition
    inline thread_local ParallelCommandBuffer::ThreadCache ParallelCommandBuffer::t_cache;

} // namespace Astra
