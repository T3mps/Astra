#pragma once

#include <cstdint>
#include <cstring>
#include "../Component/Component.hpp"
#include "../Entity/Entity.hpp"

namespace Astra
{
    /**
     * Command types for the CommandBuffer byte buffer storage.
     * Each command type has a corresponding payload struct that follows the header.
     */
    enum class CommandType : uint16_t
    {
        // Entity commands
        CreateEntity,
        DestroyEntity,
        CreateEntities,
        DestroyEntities,

        // Component commands (templated at record time, type-erased at execution)
        AddComponent,
        RemoveComponent,
        AddComponentBatch,
        RemoveComponentBatch,

        // Relationship commands
        SetParent,
        RemoveParent,
        AddChild,
        RemoveChild,
        RemoveAllChildren,
        AddLink,
        RemoveLink,

        // Resource commands
        SetResource,
        RemoveResource,
        ClearResources
    };

    /**
     * Command header stored at the beginning of each command in the byte buffer.
     * Total size: 8 bytes (aligned)
     */
    struct CommandHeader
    {
        CommandType type;           // 2 bytes
        uint16_t flags;             // 2 bytes - reserved for future use
        uint32_t totalSize;         // 4 bytes - total command size including header and payload
    };
    static_assert(sizeof(CommandHeader) == 8, "CommandHeader must be 8 bytes");
    static_assert(offsetof(CommandHeader, type) == 0, "CommandHeader::type offset");
    static_assert(offsetof(CommandHeader, flags) == 2, "CommandHeader::flags offset");
    static_assert(offsetof(CommandHeader, totalSize) == 4, "CommandHeader::totalSize offset");

    // Composite key that makes deferred-command flush order deterministic (spec §6/§14).
    struct SortKey
    {
        uint32_t insertionOrder = 0;  // recording system's plan order (high bits)
        uint32_t iterationIndex = 0;  // 0 for ordinary deferral; chunk index under ParallelForEach (Phase B)
        uint32_t recordSequence = 0;  // per-(system,iterationIndex) monotonic tiebreak = exact record order
        friend bool operator<(const SortKey& a, const SortKey& b) noexcept
        {
            if (a.insertionOrder != b.insertionOrder) return a.insertionOrder < b.insertionOrder;
            if (a.iterationIndex != b.iterationIndex) return a.iterationIndex < b.iterationIndex;
            return a.recordSequence < b.recordSequence;
        }
    };

    /**
     * Theme B2 Task 4: attributes a deferred-command failure to the system
     * that recorded (or explicitly reported) it, via SortKey::insertionOrder.
     *
     * `ApplyCommandAt()` returning false is ALWAYS a LOGICAL failure -- the
     * op's target entity/component state doesn't permit it (e.g. the entity
     * was destroyed by an earlier command in the same sorted flush). This
     * build is exception-free and RTTI-off, so a genuinely unrecoverable
     * failure (operator new(std::nothrow) aside, an allocation failure that
     * escapes as bad_alloc) is uncatchable and terminates the process
     * outright rather than returning false through this path -- there is no
     * separate "fatal" bool value ApplyCommandAt could express. Every false
     * is therefore a logical, skippable failure: ExecuteSorted() skips it,
     * records one of these, and continues the flush rather than aborting it.
     */
    struct DeferredCommandError
    {
        // SortKey::insertionOrder of the system that recorded the failed
        // command (or called SystemContext::ReportError() directly).
        uint32_t systemInsertionOrder = 0;

        enum class Reason : uint8_t
        {
            InvalidTargetEntity  // target entity was invalid/already destroyed when the op was applied
        };
        Reason reason = Reason::InvalidTargetEntity;
    };

    // ============= Command Payloads =============

    /**
     * Payload for CreateEntity command.
     * The entity is pre-allocated, this just records it for the archetype system.
     */
    struct CreateEntityPayload
    {
        Entity entity;
    };

    /**
     * Payload for DestroyEntity command.
     */
    struct DestroyEntityPayload
    {
        Entity entity;
    };

    /**
     * Payload for CreateEntities batch command.
     * Followed by entityCount Entity values in memory.
     */
    struct CreateEntitiesPayload
    {
        uint32_t entityCount;
        // Followed by: Entity entities[entityCount]
    };

    /**
     * Payload for DestroyEntities batch command.
     * Followed by entityCount Entity values in memory.
     */
    struct DestroyEntitiesPayload
    {
        uint32_t entityCount;
        // Followed by: Entity entities[entityCount]
    };

    /**
     * Destructor function type for component cleanup.
     */
    using ComponentDestructorFn = void(*)(void*);

    /**
     * Payload for AddComponent command.
     * Component data is stored inline after this struct.
     */
    struct AddComponentPayload
    {
        Entity entity;
        ComponentID componentId;
        uint16_t dataSize;
        uint16_t dataAlignment;
        ComponentDestructorFn destructor;
        // Followed by: aligned component data of dataSize bytes

        static_assert(sizeof(ComponentDestructorFn) == 8, "Function pointer must be 8 bytes");

        void* GetDataPtr()
        {
            // Data follows immediately after this struct, aligned
            std::byte* base = reinterpret_cast<std::byte*>(this + 1);
            size_t offset = reinterpret_cast<uintptr_t>(base) % dataAlignment;
            if (offset != 0)
            {
                base += dataAlignment - offset;
            }
            return base;
        }

        const void* GetDataPtr() const
        {
            return const_cast<AddComponentPayload*>(this)->GetDataPtr();
        }
    };

    /**
     * Payload for RemoveComponent command.
     */
    struct RemoveComponentPayload
    {
        Entity entity;
        ComponentID componentId;
    };

    /**
     * Payload for AddComponentBatch command.
     * Stores entities followed by single component value.
     */
    struct AddComponentBatchPayload
    {
        ComponentID componentId;
        uint16_t dataSize;
        uint16_t dataAlignment;
        uint32_t entityCount;
        ComponentDestructorFn destructor;
        // Followed by: Entity entities[entityCount]
        // Followed by: aligned component data of dataSize bytes

        Entity* GetEntitiesPtr()
        {
            return reinterpret_cast<Entity*>(this + 1);
        }

        const Entity* GetEntitiesPtr() const
        {
            return reinterpret_cast<const Entity*>(this + 1);
        }

        void* GetDataPtr()
        {
            std::byte* base = reinterpret_cast<std::byte*>(GetEntitiesPtr() + entityCount);
            size_t offset = reinterpret_cast<uintptr_t>(base) % dataAlignment;
            if (offset != 0)
            {
                base += dataAlignment - offset;
            }
            return base;
        }

        const void* GetDataPtr() const
        {
            return const_cast<AddComponentBatchPayload*>(this)->GetDataPtr();
        }
    };

    /**
     * Payload for RemoveComponentBatch command.
     * Followed by entityCount Entity values.
     */
    struct RemoveComponentBatchPayload
    {
        ComponentID componentId;
        uint16_t padding;
        uint32_t entityCount;
        // Followed by: Entity entities[entityCount]

        Entity* GetEntitiesPtr()
        {
            return reinterpret_cast<Entity*>(this + 1);
        }

        const Entity* GetEntitiesPtr() const
        {
            return reinterpret_cast<const Entity*>(this + 1);
        }
    };

    /**
     * Payload for SetParent command.
     */
    struct SetParentPayload
    {
        Entity child;
        Entity parent;
    };

    /**
     * Payload for RemoveParent command.
     */
    struct RemoveParentPayload
    {
        Entity child;
    };

    /**
     * Payload for AddChild command (same as SetParent with reversed semantics).
     */
    struct AddChildPayload
    {
        Entity parent;
        Entity child;
    };

    /**
     * Payload for RemoveChild command.
     */
    struct RemoveChildPayload
    {
        Entity parent;
        Entity child;
    };

    /**
     * Payload for RemoveAllChildren command.
     */
    struct RemoveAllChildrenPayload
    {
        Entity parent;
    };

    /**
     * Payload for AddLink command.
     */
    struct AddLinkPayload
    {
        Entity a;
        Entity b;
    };

    /**
     * Payload for RemoveLink command.
     */
    struct RemoveLinkPayload
    {
        Entity a;
        Entity b;
    };

    /**
     * Payload for SetResource command.
     * Resource data is stored inline after this struct.
     */
    struct SetResourcePayload
    {
        ComponentID componentId;
        uint16_t dataSize;
        uint16_t dataAlignment;
        uint16_t padding;
        ComponentDestructorFn destructor;
        // Followed by: aligned resource data of dataSize bytes

        void* GetDataPtr()
        {
            std::byte* base = reinterpret_cast<std::byte*>(this + 1);
            size_t offset = reinterpret_cast<uintptr_t>(base) % dataAlignment;
            if (offset != 0)
            {
                base += dataAlignment - offset;
            }
            return base;
        }

        const void* GetDataPtr() const
        {
            return const_cast<SetResourcePayload*>(this)->GetDataPtr();
        }
    };

    /**
     * Payload for RemoveResource command.
     */
    struct RemoveResourcePayload
    {
        ComponentID componentId;
    };

    /**
     * Payload for ClearResources command.
     * No additional data needed.
     */
    struct ClearResourcesPayload
    {
        // Empty - no data needed
    };

    // ============= Helper Functions =============

    /**
     * Calculate aligned size for placing data after a struct.
     */
    inline constexpr size_t AlignUp(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    /**
     * Calculate total command size for AddComponent.
     */
    inline size_t CalculateAddComponentSize(size_t dataSize, size_t dataAlignment)
    {
        size_t payloadStart = sizeof(CommandHeader);
        size_t dataStart = payloadStart + sizeof(AddComponentPayload);
        dataStart = AlignUp(dataStart, dataAlignment);
        return dataStart + dataSize;
    }

    /**
     * Calculate total command size for AddComponentBatch.
     */
    inline size_t CalculateAddComponentBatchSize(size_t entityCount, size_t dataSize, size_t dataAlignment)
    {
        size_t payloadStart = sizeof(CommandHeader);
        size_t entitiesStart = payloadStart + sizeof(AddComponentBatchPayload);
        size_t dataStart = entitiesStart + entityCount * sizeof(Entity);
        dataStart = AlignUp(dataStart, dataAlignment);
        return dataStart + dataSize;
    }

    /**
     * Calculate total command size for SetResource.
     */
    inline size_t CalculateSetResourceSize(size_t dataSize, size_t dataAlignment)
    {
        size_t payloadStart = sizeof(CommandHeader);
        size_t dataStart = payloadStart + sizeof(SetResourcePayload);
        dataStart = AlignUp(dataStart, dataAlignment);
        return dataStart + dataSize;
    }

} // namespace Astra
