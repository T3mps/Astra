#pragma once

// Read-only Registry introspection facade (AstraStudio MVP, spec 2026-07-27).
// Opt-in: NOT included by Astra.hpp. Snapshots internals into POD structs so
// tools never hold pointers into live engine state. This header is the single
// sanctioned place that walks ArchetypeManager/Archetype/ColumnMeta for
// inspection; keep tool code out of engine internals.

#include <cstdint>
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

    // Physical placement of one storage column inside one chunk's arena.
    struct ChunkColumnLayout
    {
        size_t offset = 0;                    // cache-line-aligned base offset
        size_t bytes = 0;                     // stride * capacity
        size_t disabledOffset = SIZE_MAX;     // disabled-bit words region, or SIZE_MAX
        size_t disabledBytes = 0;
        uint32_t disabledCount = 0;
    };

    struct ChunkInfo
    {
        size_t count = 0;
        size_t capacity = 0;
        size_t chunkBytes = 0;                       // arena size
        std::vector<ChunkColumnLayout> columns;      // parallel to ArchetypeInfo::columns
        // Accounting; invariant: the four sum exactly to chunkBytes.
        size_t columnBytes = 0, padBytes = 0, bitsBytes = 0, slackBytes = 0;
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
        size_t bytesUsed = 0;                // layout-derived (unchanged semantics)
        size_t bytesAllocated = 0;           // layout-derived (unchanged semantics)
        size_t bytesReserved = 0;            // true arena footprint: sum of chunkBytes
    };

    struct RegistrySnapshot
    {
        size_t entityCount = 0;
        size_t archetypeCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;
        size_t bytesAllocated = 0;
        size_t bytesReserved = 0;
    };

    struct InspectorSnapshot
    {
        RegistrySnapshot registry;
        std::vector<ArchetypeInfo> archetypes;
        size_t cacheLineBytes = 0;           // so panels never include engine internals
    };

    // Capture-into: clears and refills `out`, retaining outer vector capacity.
    // Runs every studio frame -- POD work proportional to chunks x columns,
    // never to entity count; per-entity copies live in CaptureChunkDetail
    // (selected chunk only).
    inline void Capture(Registry& registry, InspectorSnapshot& out)
    {
        out.registry = RegistrySnapshot{};
        out.archetypes.clear();
        out.cacheLineBytes = CACHE_LINE_SIZE;

        ArchetypeManager* manager = registry.GetArchetypeManager();
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!manager || !components)
            return;

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
                if (!chunk)
                    continue;
                ChunkInfo ch;
                ch.count = chunk->GetCount();
                ch.capacity = chunk->GetCapacity();
                ch.chunkBytes = chunk->GetChunkBytes();

                // Columns first (ascending offsets), then their disabled-bit
                // regions (carved after all columns, in enableable-ordinal order,
                // which is also ascending) -- cursor walk yields pad as the gaps.
                size_t cursor = 0;
                ch.columns.reserve(meta.columnCount);
                for (uint16_t c = 0; c < meta.columnCount; ++c)
                {
                    ChunkColumnLayout cl;
                    cl.offset = chunk->GetColumnOffset(c);
                    cl.bytes = size_t(meta.columns[c].stride) * ch.capacity;
                    cl.disabledOffset = chunk->GetDisabledWordsOffset(c);
                    if (cl.disabledOffset != SIZE_MAX)
                    {
                        // ArchetypeChunk::WordsForCapacity is private; mirror its formula
                        // inline, matching the existing duplication in Archetype.hpp's
                        // own capacity math (ComputeLayoutBytesForCapacity et al.).
                        cl.disabledBytes = ((ch.capacity + 63) / 64) * 8;
                        cl.disabledCount = chunk->GetDisabledCount(static_cast<int>(c));
                    }
                    ch.columnBytes += cl.bytes;
                    ch.padBytes += cl.offset - cursor;
                    cursor = cl.offset + cl.bytes;
                    ch.columns.push_back(cl);
                }
                for (const ChunkColumnLayout& cl : ch.columns)
                {
                    if (cl.disabledOffset == SIZE_MAX)
                        continue;
                    ch.padBytes += cl.disabledOffset - cursor;
                    ch.bitsBytes += cl.disabledBytes;
                    cursor = cl.disabledOffset + cl.disabledBytes;
                }
                ch.slackBytes = ch.chunkBytes - cursor;

                info.bytesUsed += ch.count * rowStride;
                info.bytesAllocated += ch.capacity * rowStride;
                info.bytesReserved += ch.chunkBytes;
                info.chunks.push_back(std::move(ch));
            }

            out.registry.entityCount += info.entityCount;
            out.registry.chunkCount += info.chunkCount;
            out.registry.bytesUsed += info.bytesUsed;
            out.registry.bytesAllocated += info.bytesAllocated;
            out.registry.bytesReserved += info.bytesReserved;
            out.archetypes.push_back(std::move(info));
        }
        out.registry.archetypeCount = out.archetypes.size();
    }

    ASTRA_NODISCARD inline InspectorSnapshot Capture(Registry& registry)
    {
        InspectorSnapshot snap;
        Capture(registry, snap);
        return snap;
    }

    // Per-entity data for ONE chunk -- the panel's selected chunk only, so the
    // per-frame cost is O(one chunk's capacity), never O(total entities).
    struct ChunkDetail
    {
        std::vector<Entity> entities;                       // row -> Entity
        std::vector<std::vector<uint64_t>> disabledWords;   // by column ordinal; empty = not enableable
    };

    // Same-thread, same-frame as Capture, so snapshot indices stay consistent.
    // archetypeIndex counts non-null archetypes exactly as Capture does.
    ASTRA_NODISCARD inline bool CaptureChunkDetail(Registry& registry, size_t archetypeIndex,
                                   size_t chunkIndex, ChunkDetail& out)
    {
        out.entities.clear();
        out.disabledWords.clear();

        ArchetypeManager* manager = registry.GetArchetypeManager();
        if (!manager)
            return false;

        Archetype* target = nullptr;
        size_t index = 0;
        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;
            if (index++ == archetypeIndex)
            {
                target = archetype;
                break;
            }
        }
        if (!target)
            return false;

        const auto& chunks = target->GetChunks();
        if (chunkIndex >= chunks.size() || !chunks[chunkIndex])
            return false;
        const auto& chunk = chunks[chunkIndex];

        out.entities = chunk->GetEntities();

        const ArchetypeColumnMeta& meta = target->GetColumnMeta();
        out.disabledWords.resize(meta.columnCount);
        // ArchetypeChunk::WordsForCapacity is private; mirror its formula inline,
        // matching the existing duplication in Capture() above.
        const size_t words = (chunk->GetCapacity() + 63) / 64;
        for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
        {
            const uint16_t c = meta.enableableColumns[e];
            const uint64_t* w = chunk->GetDisabledWords(static_cast<int>(c));
            // An enableable column always carves a word region; a null here
            // would leave this column indistinguishable from non-enableable.
            ASTRA_ASSERT(w != nullptr, "Enableable column has no disabled-word region");
            if (w)
                out.disabledWords[c].assign(w, w + words);
        }
        return true;
    }
} // namespace Astra::Debug
