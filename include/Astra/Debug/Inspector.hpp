#pragma once

// Read-only Registry introspection facade (AstraStudio MVP, spec 2026-07-27).
// Opt-in: NOT included by Astra.hpp. Snapshots internals into POD structs so
// tools never hold pointers into live engine state. This header is the single
// sanctioned place that walks ArchetypeManager/Archetype/ColumnMeta for
// inspection; keep tool code out of engine internals.

#include <string>
#include <vector>

#include "../Registry/Registry.hpp"

namespace Astra::Debug
{
    struct ColumnInfo
    {
        std::string name;
        ComponentID id{};
        size_t size = 0;
        size_t alignment = 0;
        uint32_t stride = 0;
        bool isEnableable = false;
    };

    struct ChunkInfo
    {
        size_t count = 0;
        size_t capacity = 0;
    };

    struct ArchetypeInfo
    {
        std::string signature;              // all mask components (tags included), " + "-joined; "(empty)" for the root
        size_t componentCount = 0;          // mask popcount
        size_t tagCount = 0;                // empty (tag) components in the mask
        std::vector<ColumnInfo> columns;    // storage-bearing only (tags excluded)
        std::vector<ChunkInfo> chunks;
        size_t entityCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;               // layout-derived: sum(count * rowStride) per chunk
        size_t bytesAllocated = 0;          // layout-derived: sum(capacity * rowStride) per chunk
    };

    struct RegistrySnapshot
    {
        size_t entityCount = 0;
        size_t archetypeCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;
        size_t bytesAllocated = 0;
    };

    struct InspectorSnapshot
    {
        RegistrySnapshot registry;
        std::vector<ArchetypeInfo> archetypes;
    };

    // Read-only walk. Takes Registry& (not const) only because
    // ArchetypeManager::GetArchetypes() is non-const today.
    inline InspectorSnapshot Capture(Registry& registry)
    {
        InspectorSnapshot snap;
        ArchetypeManager* manager = registry.GetArchetypeManager();
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!manager || !components)
            return snap;

        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;

            ArchetypeInfo info;
            const ComponentMask& mask = archetype->GetMask();
            info.componentCount = mask.Count();

            // Signature from mask bits (tags included).
            for (size_t bit = 0; bit < MAX_COMPONENTS; ++bit)
            {
                if (!mask.Test(bit))
                    continue;
                const ComponentDescriptor* desc =
                    components->GetComponentDescriptor(static_cast<ComponentID>(bit));
                const char* name = (desc && desc->name) ? desc->name : "?";
                if (!info.signature.empty())
                    info.signature += " + ";
                info.signature += name;
                if (desc && desc->is_empty)
                    ++info.tagCount;
            }
            if (info.signature.empty())
                info.signature = "(empty)";

            // Storage-bearing columns from the shared per-archetype metadata.
            const ArchetypeColumnMeta& meta = archetype->GetColumnMeta();
            size_t rowStride = 0;
            info.columns.reserve(meta.columnCount);
            for (uint16_t c = 0; c < meta.columnCount; ++c)
            {
                const auto& col = meta.columns[c];
                ColumnInfo ci;
                ci.id = col.id;
                ci.stride = col.stride;
                if (col.descriptor)
                {
                    ci.name = col.descriptor->name ? col.descriptor->name : "?";
                    ci.size = col.descriptor->size;
                    ci.alignment = col.descriptor->alignment;
                    ci.isEnableable = col.descriptor->isEnableable;
                }
                rowStride += col.stride;
                info.columns.push_back(std::move(ci));
            }

            info.entityCount = archetype->GetEntityCount();
            info.chunkCount = archetype->GetChunkCount();
            info.chunks.reserve(info.chunkCount);
            for (const auto& chunk : archetype->GetChunks())
            {
                ChunkInfo ch;
                ch.count = chunk->GetCount();
                ch.capacity = chunk->GetCapacity();
                info.bytesUsed += ch.count * rowStride;
                info.bytesAllocated += ch.capacity * rowStride;
                info.chunks.push_back(ch);
            }

            snap.registry.entityCount += info.entityCount;
            snap.registry.chunkCount += info.chunkCount;
            snap.registry.bytesUsed += info.bytesUsed;
            snap.registry.bytesAllocated += info.bytesAllocated;
            snap.archetypes.push_back(std::move(info));
        }
        snap.registry.archetypeCount = snap.archetypes.size();
        return snap;
    }
} // namespace Astra::Debug
