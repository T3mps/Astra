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
