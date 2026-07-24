#include <gtest/gtest.h>

#include <Astra/Archetype/ArchetypeChunkPool.hpp>

namespace
{
    // A minimal single-column meta (one 16-byte trivial payload) for raw pool tests.
    // Uses a static ComponentDescriptor so no ComponentID is registered (TypeID ceiling).
    Astra::ArchetypeColumnMeta MakeSingleColumnMeta()
    {
        static Astra::ComponentDescriptor desc = []
        {
            Astra::ComponentDescriptor d{};
            d.id = 0;
            d.size = 16;
            d.alignment = 8;
            d.is_trivially_copyable = true;
            // Required: DefaultConstruct() memsets when this is true, instead of
            // calling the (null, in this hand-built descriptor) defaultConstruct fn.
            d.is_trivially_default_constructible = true;
            return d;
        }();
        Astra::ArchetypeColumnMeta meta;
        meta.columnCount = 1;
        meta.columns[0] = {desc.id, 16u, &desc};
        meta.idToColumn[0] = 0;
        return meta;
    }
}

TEST(ChunkPoolTest, CreateReturnReuseCycle)
{
    Astra::ArchetypeChunkPool pool;   // default 16KB fixed config
    auto meta = MakeSingleColumnMeta();

    auto c1 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(c1->GetCapacity(), 64u);

    auto stats = pool.GetStats();
    EXPECT_EQ(stats.acquireCount, 1u);

    c1.reset();   // ReturnChunk via deleter
    stats = pool.GetStats();
    EXPECT_EQ(stats.releaseCount, 1u);

    auto c2 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    ASSERT_NE(c2, nullptr);
    stats = pool.GetStats();
    EXPECT_EQ(stats.acquireCount, 2u);
}

TEST(ChunkPoolTest, MaxChunksCapRefusesFurtherChunks)
{
    Astra::ArchetypeChunkPool::Config config;
    // chunksPerBlock MUST be lowered alongside maxChunks: the constructor
    // normalizes `maxChunks = max(maxChunks, chunksPerBlock)`, so leaving the
    // default 128 here would silently raise the cap back to 128 (verified
    // against the pre-TLSF pool while pinning these semantics).
    config.chunksPerBlock = 2;
    config.maxChunks = 2;
    Astra::ArchetypeChunkPool pool(config);
    auto meta = MakeSingleColumnMeta();

    auto c1 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    auto c2 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    ASSERT_NE(c1, nullptr);
    ASSERT_NE(c2, nullptr);
    auto c3 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    EXPECT_EQ(c3, nullptr);
    EXPECT_GT(pool.GetStats().failedAcquires, 0u);
}

TEST(ChunkPoolTest, DefragmentReleasesEmptyArenasKeepingReserve)
{
    Astra::ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 4;   // small arenas so several get created
    Astra::ArchetypeChunkPool pool(config);
    auto meta = MakeSingleColumnMeta();

    std::vector<decltype(pool.CreateChunk(64, pool.GetChunkSize(), &meta))> chunks;
    for (int i = 0; i < 16; ++i)
    {
        auto c = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
        ASSERT_NE(c, nullptr);
        chunks.push_back(std::move(c));
    }
    chunks.clear();   // everything returned

    auto result = pool.Defragment();
    EXPECT_GT(result.blocksReleased, 0u);   // arenas released...
    EXPECT_GE(result.blocksKept, 1u);       // ...but one kept as reserve
    EXPECT_GT(result.bytesFreed, 0u);

    // Pool still usable after release.
    auto c = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    EXPECT_NE(c, nullptr);
}

// Pins the reserve policy for the case DefragmentReleasesEmptyArenasKeepingReserve
// does NOT cover: fully-free arenas coexisting with an arena still in use. The
// old block pool always kept one empty block in reserve even while other
// blocks were live; a prior version of the TLSF rewire only kept a reserve
// when EVERY arena was free, releasing all of them otherwise (0 reserve in
// this mixed scenario). This test fails under that older policy and passes
// under the restored one.
TEST(ChunkPoolTest, DefragmentKeepsOneReserveArenaEvenWhenOthersAreInUse)
{
    Astra::ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 2;   // empirically 1 chunk per arena at the default 16KB chunk size
    Astra::ArchetypeChunkPool pool(config);
    auto meta = MakeSingleColumnMeta();

    auto c1 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    auto c2 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    auto c3 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    ASSERT_NE(c1, nullptr);
    ASSERT_NE(c2, nullptr);
    ASSERT_NE(c3, nullptr);

    // Return the first two chunks: their arenas become fully free. Keep c3
    // alive: its arena stays in use. Mixed occupancy -- 2 free arenas and 1
    // live arena, simultaneously.
    c1.reset();
    c2.reset();

    auto result = pool.Defragment();

    // Of the 2 fully-free arenas, exactly 1 is released and 1 is kept as
    // reserve, plus the 1 arena still backing c3 -- NOT "release both because
    // some arena is still in use".
    EXPECT_EQ(result.blocksReleased, 1u);
    EXPECT_EQ(result.blocksKept, 2u);
    EXPECT_GT(result.bytesFreed, 0u);
    EXPECT_EQ(result.chunksInUse, 1u);

    // Pool still usable (the reserve arena can serve a new chunk).
    auto c4 = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    EXPECT_NE(c4, nullptr);
}

// Move-construction must transfer real ownership of arenas/Tlsf state: the
// moved-to pool has to be fully functional, and the moved-from pool has to
// report empty and be safe to destroy on its own (no double-free of the
// arenas it no longer owns).
TEST(ChunkPoolTest, MoveConstructTransfersArenasAndInertsSource)
{
    Astra::ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 4;
    auto meta = MakeSingleColumnMeta();

    Astra::ArchetypeChunkPool poolA(config);
    auto held = poolA.CreateChunk(64, poolA.GetChunkSize(), &meta);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(poolA.GetStats().acquireCount, 1u);

    Astra::ArchetypeChunkPool poolB(std::move(poolA));

    // The moved-from pool must read as empty -- its arenas and Tlsf state
    // moved into poolB -- and remain safe to destroy at end of scope.
    auto statsA = poolA.GetStats();
    EXPECT_EQ(statsA.acquireCount, 0u);
    EXPECT_EQ(statsA.releaseCount, 0u);
    EXPECT_EQ(statsA.totalChunks, 0u);

    // The moved-to pool genuinely owns the transferred state: prove it by
    // creating AND returning a fresh chunk through it.
    auto c2 = poolB.CreateChunk(64, poolB.GetChunkSize(), &meta);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(poolB.GetStats().acquireCount, 2u);   // the pre-move chunk + this one
    c2.reset();
    EXPECT_EQ(poolB.GetStats().releaseCount, 1u);

    // `held`'s backing memory now belongs to poolB (its arena moved along with
    // the Tlsf/ArenaRecord bookkeeping). ArchetypeChunkPool has no way to find
    // and fix up outstanding ChunkDeleters on its own, so redirect this one by
    // hand to the real new owner before returning it -- the same fixup any
    // caller relocating a pool with live chunks outstanding would need to do.
    held.get_deleter().pool = &poolB;
    held.reset();
    EXPECT_EQ(poolB.GetStats().releaseCount, 2u);

    // poolA destructs here (end of scope) with zero arenas: must not
    // double-free anything poolB now owns.
}

// Move-assignment onto a pool that already owns arenas must release the
// destination's own state (not leak it) before taking over the source's.
TEST(ChunkPoolTest, MoveAssignReplacesDestinationOwnershipWithoutLeaking)
{
    Astra::ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 4;
    auto meta = MakeSingleColumnMeta();

    Astra::ArchetypeChunkPool poolA(config);
    auto a1 = poolA.CreateChunk(64, poolA.GetChunkSize(), &meta);
    ASSERT_NE(a1, nullptr);
    EXPECT_EQ(poolA.GetStats().acquireCount, 1u);

    Astra::ArchetypeChunkPool poolB(config);
    auto b1 = poolB.CreateChunk(64, poolB.GetChunkSize(), &meta);
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(poolB.GetStats().acquireCount, 1u);
    b1.reset();   // return through poolB while it is still its own owner

    // poolB already owns an arena (freed but still registered) at this point;
    // move-assign must release it -- not leak it -- before taking poolA's state.
    poolB = std::move(poolA);

    EXPECT_EQ(poolB.GetStats().acquireCount, 1u);   // poolA's a1; poolB's own history discarded
    // GetStats().totalChunks is live + free-equivalent (a capacity estimate,
    // see GetStats' comment), not a live-chunk count, so check the live count
    // directly via Defragment's chunksInUse instead: exactly poolA's 1 live
    // chunk, not poolB's discarded history and not a double-counted leak.
    EXPECT_EQ(poolB.Defragment().chunksInUse, 1u);

    auto statsA = poolA.GetStats();
    EXPECT_EQ(statsA.acquireCount, 0u);
    EXPECT_EQ(statsA.totalChunks, 0u);

    // a1's memory now belongs to poolB; redirect its deleter (see
    // MoveConstructTransfersArenasAndInertsSource above for why) and confirm
    // returning it through the real owner works cleanly.
    a1.get_deleter().pool = &poolB;
    a1.reset();
    EXPECT_EQ(poolB.GetStats().releaseCount, 1u);

    // Pool still usable post-assignment.
    auto c = poolB.CreateChunk(64, poolB.GetChunkSize(), &meta);
    EXPECT_NE(c, nullptr);
}
