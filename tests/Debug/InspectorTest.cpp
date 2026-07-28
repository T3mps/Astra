#include <algorithm>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Player;   // empty tag
    using Astra::Test::Timer;      // AstraEnableable
    using Astra::Test::Hierarchy;  // AstraEnableable

    const Astra::Debug::ArchetypeInfo* FindBySignature(
        const Astra::Debug::InspectorSnapshot& snap, std::string_view needle)
    {
        for (const auto& a : snap.archetypes)
            if (a.signature.find(needle) != std::string::npos) return &a;
        return nullptr;
    }
}

TEST(Inspector, EmptyRegistrySnapshotIsEmpty)
{
    Astra::Registry reg;
    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.registry.entityCount, 0u);
    // The root (component-less) archetype may exist; every archetype must be empty.
    for (const auto& a : snap.archetypes)
        EXPECT_EQ(a.entityCount, 0u);
}

TEST(Inspector, SnapshotReportsArchetypesEntitiesAndColumns)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();
    for (int i = 0; i < 25; ++i)  (void)reg.CreateEntity<Position, Player>();

    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.registry.entityCount, 125u);
    EXPECT_GE(snap.registry.archetypeCount, 2u);
    EXPECT_GT(snap.registry.chunkCount, 0u);
    EXPECT_GT(snap.registry.bytesAllocated, 0u);

    const auto* pv = FindBySignature(snap, "Velocity");
    ASSERT_NE(pv, nullptr);
    EXPECT_EQ(pv->entityCount, 100u);
    ASSERT_EQ(pv->columns.size(), 2u);   // Position + Velocity (both storage-bearing)
    // Columns carry real descriptor data.
    for (const auto& c : pv->columns)
    {
        EXPECT_FALSE(c.name.empty());
        EXPECT_GT(c.size, 0u);
        EXPECT_EQ(c.stride, c.size);
        EXPECT_GT(c.alignment, 0u);
    }
    // Chunk bookkeeping is self-consistent.
    EXPECT_EQ(pv->chunks.size(), pv->chunkCount);
    size_t summed = 0;
    for (const auto& ch : pv->chunks) { EXPECT_LE(ch.count, ch.capacity); summed += ch.count; }
    EXPECT_EQ(summed, pv->entityCount);
    EXPECT_GE(pv->bytesAllocated, pv->bytesUsed);

    // Tag component: in the signature and tag count, NOT in storage columns.
    const auto* pp = FindBySignature(snap, "Player");
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->entityCount, 25u);
    EXPECT_EQ(pp->tagCount, 1u);
    EXPECT_EQ(pp->columns.size(), 1u);   // Position only; Player is empty/tag
    EXPECT_EQ(pp->componentCount, 2u);   // mask counts both
}

TEST(Inspector, ChunkColumnOffsetsAlignedAscendingWithinArena)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();

    Astra::ArchetypeManager* mgr = reg.GetArchetypeManager();
    ASSERT_NE(mgr, nullptr);
    bool sawChunk = false;
    for (Astra::Archetype* a : mgr->GetArchetypes())
    {
        if (!a || a->GetEntityCount() == 0) continue;
        const Astra::ArchetypeColumnMeta& meta = a->GetColumnMeta();
        for (const auto& chunk : a->GetChunks())
        {
            sawChunk = true;
            size_t prevEnd = 0;
            for (uint16_t c = 0; c < meta.columnCount; ++c)
            {
                const size_t off = chunk->GetColumnOffset(c);
                EXPECT_EQ(off % Astra::CACHE_LINE_SIZE, 0u);   // cache-line aligned base
                EXPECT_GE(off, prevEnd);                        // ascending, non-overlapping
                if (c == 0) EXPECT_EQ(off, 0u);                 // first column at arena start
                prevEnd = off + size_t(meta.columns[c].stride) * chunk->GetCapacity();
                EXPECT_LE(prevEnd, chunk->GetChunkBytes());
                // Position/Velocity are not enableable: sentinel expected.
                EXPECT_EQ(chunk->GetDisabledWordsOffset(c), SIZE_MAX);
            }
        }
    }
    EXPECT_TRUE(sawChunk);
}

TEST(Inspector, ChunkAccountingSumsToChunkBytes)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();

    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.cacheLineBytes, Astra::CACHE_LINE_SIZE);

    const auto* pv = FindBySignature(snap, "Velocity");
    ASSERT_NE(pv, nullptr);
    EXPECT_GT(pv->bytesReserved, 0u);
    EXPECT_GE(pv->bytesReserved, pv->bytesAllocated);   // arena >= layout-derived

    size_t reserved = 0;
    for (const auto& a : snap.archetypes) reserved += a.bytesReserved;
    EXPECT_EQ(snap.registry.bytesReserved, reserved);

    for (const auto& ch : pv->chunks)
    {
        ASSERT_EQ(ch.columns.size(), pv->columns.size());   // parallel arrays
        EXPECT_GT(ch.chunkBytes, 0u);
        size_t prevEnd = 0, cols = 0;
        for (size_t c = 0; c < ch.columns.size(); ++c)
        {
            const auto& cl = ch.columns[c];
            EXPECT_EQ(cl.offset % snap.cacheLineBytes, 0u);
            EXPECT_GE(cl.offset, prevEnd);
            EXPECT_EQ(cl.bytes, size_t(pv->columns[c].stride) * ch.capacity);
            EXPECT_EQ(cl.disabledOffset, SIZE_MAX);          // not enableable
            EXPECT_EQ(cl.disabledBytes, 0u);
            EXPECT_EQ(cl.disabledCount, 0u);
            prevEnd = cl.offset + cl.bytes;
            cols += cl.bytes;
        }
        EXPECT_EQ(ch.columnBytes, cols);
        EXPECT_EQ(ch.bitsBytes, 0u);
        EXPECT_EQ(ch.columnBytes + ch.padBytes + ch.bitsBytes + ch.slackBytes, ch.chunkBytes);
    }
}

TEST(Inspector, CaptureIntoMatchesByValueAndRefills)
{
    Astra::Registry reg;
    for (int i = 0; i < 50; ++i) (void)reg.CreateEntity<Position, Velocity>();

    Astra::Debug::InspectorSnapshot byValue = Astra::Debug::Capture(reg);
    Astra::Debug::InspectorSnapshot into;
    Astra::Debug::Capture(reg, into);

    EXPECT_EQ(into.registry.entityCount, byValue.registry.entityCount);
    EXPECT_EQ(into.registry.bytesReserved, byValue.registry.bytesReserved);
    ASSERT_EQ(into.archetypes.size(), byValue.archetypes.size());
    for (size_t i = 0; i < into.archetypes.size(); ++i)
    {
        EXPECT_EQ(into.archetypes[i].signature, byValue.archetypes[i].signature);
        EXPECT_EQ(into.archetypes[i].entityCount, byValue.archetypes[i].entityCount);
        EXPECT_EQ(into.archetypes[i].chunks.size(), byValue.archetypes[i].chunks.size());
    }

    // Refill the same object after mutation: results track the registry, no stale rows.
    for (int i = 0; i < 50; ++i) (void)reg.CreateEntity<Position>();
    Astra::Debug::Capture(reg, into);
    EXPECT_EQ(into.registry.entityCount, 100u);

    // A missing archetypes.clear() would leave stale rows from the pre-mutation
    // capture appended onto the new ones; catch that by diffing against a fresh
    // by-value capture of the same (post-mutation) registry state.
    auto fresh = Astra::Debug::Capture(reg);
    EXPECT_EQ(into.registry.archetypeCount, into.archetypes.size());
    ASSERT_EQ(into.archetypes.size(), fresh.archetypes.size());
    for (size_t i = 0; i < into.archetypes.size(); ++i)
    {
        EXPECT_EQ(into.archetypes[i].signature, fresh.archetypes[i].signature);
        EXPECT_EQ(into.archetypes[i].entityCount, fresh.archetypes[i].entityCount);
    }
}

TEST(Inspector, ChunkAccountingWithEnableableColumns)
{
    Astra::Registry reg;
    for (int i = 0; i < 50; ++i) (void)reg.CreateEntity<Position, Timer, Hierarchy>();

    auto snap = Astra::Debug::Capture(reg);
    const auto* pt = FindBySignature(snap, "Timer");
    ASSERT_NE(pt, nullptr);

    for (const auto& ch : pt->chunks)
    {
        // Bits regions are carved after ALL columns; find the last column's end.
        size_t lastColumnEnd = 0;
        for (const auto& cl : ch.columns)
            lastColumnEnd = std::max(lastColumnEnd, cl.offset + cl.bytes);

        const size_t expectedDisabledBytes = ((ch.capacity + 63) / 64) * 8;
        size_t enableableCount = 0, bitsSum = 0, prevDisabledOffset = 0;
        bool first = true;
        for (const auto& cl : ch.columns)
        {
            if (cl.disabledOffset == SIZE_MAX)
                continue;
            ++enableableCount;
            EXPECT_EQ(cl.disabledBytes, expectedDisabledBytes);
            EXPECT_GE(cl.disabledOffset, lastColumnEnd);
            if (!first)
                EXPECT_GT(cl.disabledOffset, prevDisabledOffset);   // strictly ascending
            first = false;
            prevDisabledOffset = cl.disabledOffset;
            bitsSum += cl.disabledBytes;
            EXPECT_EQ(cl.disabledCount, 0u);   // no SetEnabled calls: layout accounting only
        }
        EXPECT_EQ(enableableCount, 2u);   // Timer + Hierarchy; Position is not enableable
        EXPECT_EQ(ch.bitsBytes, bitsSum);
        EXPECT_GT(ch.bitsBytes, 0u);
        EXPECT_EQ(ch.columnBytes + ch.padBytes + ch.bitsBytes + ch.slackBytes, ch.chunkBytes);
    }
}
