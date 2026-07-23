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

    auto c1 = pool.CreateChunk(64, &meta);
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(c1->GetCapacity(), 64u);

    auto stats = pool.GetStats();
    EXPECT_EQ(stats.acquireCount, 1u);

    c1.reset();   // ReturnChunk via deleter
    stats = pool.GetStats();
    EXPECT_EQ(stats.releaseCount, 1u);

    auto c2 = pool.CreateChunk(64, &meta);
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

    auto c1 = pool.CreateChunk(64, &meta);
    auto c2 = pool.CreateChunk(64, &meta);
    ASSERT_NE(c1, nullptr);
    ASSERT_NE(c2, nullptr);
    auto c3 = pool.CreateChunk(64, &meta);
    EXPECT_EQ(c3, nullptr);
    EXPECT_GT(pool.GetStats().failedAcquires, 0u);
}

TEST(ChunkPoolTest, DefragmentReleasesEmptyArenasKeepingReserve)
{
    Astra::ArchetypeChunkPool::Config config;
    config.chunksPerBlock = 4;   // small arenas so several get created
    Astra::ArchetypeChunkPool pool(config);
    auto meta = MakeSingleColumnMeta();

    std::vector<decltype(pool.CreateChunk(64, &meta))> chunks;
    for (int i = 0; i < 16; ++i)
    {
        auto c = pool.CreateChunk(64, &meta);
        ASSERT_NE(c, nullptr);
        chunks.push_back(std::move(c));
    }
    chunks.clear();   // everything returned

    auto result = pool.Defragment();
    EXPECT_GT(result.blocksReleased, 0u);   // arenas released...
    EXPECT_GE(result.blocksKept, 1u);       // ...but one kept as reserve
    EXPECT_GT(result.bytesFreed, 0u);

    // Pool still usable after release.
    auto c = pool.CreateChunk(64, &meta);
    EXPECT_NE(c, nullptr);
}
