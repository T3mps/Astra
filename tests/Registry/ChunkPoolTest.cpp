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
            // Same rationale for teardown: ~ArchetypeChunk destructs every live
            // element, and Destruct() only skips the (null here) destruct fn ptr
            // when the trait says the type is trivially destructible. Needed as
            // soon as a test leaves an entity live in a hand-built chunk.
            d.is_trivially_destructible = true;
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

// ArchetypeChunkPool is deliberately non-copyable AND non-movable. Every
// outstanding chunk embeds a raw ChunkDeleter::pool back-pointer that the pool
// cannot enumerate or fix up, so relocating a pool with live chunks would
// dangle those deleters -> use-after-free / double-free on return. The sole
// owner (ArchetypeManager) is itself non-movable, so the pool never needs to
// move. This test pins that contract (previously two tests exercised the move
// ops by hand-redirecting each ChunkDeleter::pool -- that manual fixup WAS the
// hazard, so the move ops were deleted and the tests replaced with this).
TEST(ChunkPoolTest, IsNonCopyableAndNonMovable)
{
    static_assert(!std::is_copy_constructible_v<Astra::ArchetypeChunkPool>,
                  "ArchetypeChunkPool must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<Astra::ArchetypeChunkPool>,
                  "ArchetypeChunkPool must not be copy-assignable");
    static_assert(!std::is_move_constructible_v<Astra::ArchetypeChunkPool>,
                  "ArchetypeChunkPool must not be move-constructible "
                  "(would dangle outstanding ChunkDeleter back-pointers)");
    static_assert(!std::is_move_assignable_v<Astra::ArchetypeChunkPool>,
                  "ArchetypeChunkPool must not be move-assignable "
                  "(would dangle outstanding ChunkDeleter back-pointers)");
    SUCCEED();
}

TEST(ChunkPoolTest, FreshChunkColumnsStartAtNeverAndAddEntityStamps)
{
    Astra::ArchetypeChunkPool pool;
    auto meta = MakeSingleColumnMeta();
    auto c = pool.CreateChunk(64, pool.GetChunkSize(), &meta);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->GetColumnVersion(0), 0u);            // zero-init == never stamped
    c->AddEntity(Astra::Entity{}, Astra::Tick{7});
    EXPECT_EQ(c->GetColumnVersion(0), 7u);
    c->StampColumn(0, 9);
    EXPECT_EQ(c->GetColumnVersion(0), 9u);
    c->FoldColumnVersion(0, 4);                        // older: ignored
    EXPECT_EQ(c->GetColumnVersion(0), 9u);
    c->FoldColumnVersion(0, 12);                       // newer: taken
    EXPECT_EQ(c->GetColumnVersion(0), 12u);
    // The version region sits after the column data, 8-byte aligned, inside the arena.
    EXPECT_EQ(c->GetColumnVersionOffset() % 8, 0u);
    EXPECT_GE(c->GetColumnVersionOffset(), c->GetColumnOffset(0) + 16u * 64u);
    EXPECT_LT(c->GetColumnVersionOffset(), c->GetChunkBytes());
}
