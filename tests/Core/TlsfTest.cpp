#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <Astra/Core/Tlsf.hpp>

namespace
{
    // Heap-backed, 64B-aligned arena buffer (portable; no OS pages needed).
    struct ArenaBuffer
    {
        std::unique_ptr<std::byte[]> raw;
        void* base = nullptr;
        size_t bytes = 0;

        explicit ArenaBuffer(size_t size) : raw(new std::byte[size + 64]), bytes(size)
        {
            auto p = reinterpret_cast<uintptr_t>(raw.get());
            base = reinterpret_cast<void*>((p + 63) & ~uintptr_t(63));
        }
    };

    bool IsAligned64(void* p) { return (reinterpret_cast<uintptr_t>(p) & 63) == 0; }

    bool InArena(void* p, const ArenaBuffer& buf)
    {
        auto addr = reinterpret_cast<uintptr_t>(p);
        auto begin = reinterpret_cast<uintptr_t>(buf.base);
        return addr >= begin && addr < begin + buf.bytes;
    }
}

TEST(TlsfTest, FreshArenaValidatesAndReportsFreeBytes)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    EXPECT_EQ(tlsf.GetArenaCount(), 1u);
    // Main block: largest size == 56 (mod 64) fitting front pad + sentinel.
    const size_t expected = (((1u << 20) - 128) & ~size_t(63)) + 56;
    EXPECT_EQ(tlsf.GetFreeBytes(), expected);
    EXPECT_TRUE(tlsf.Validate());
}

TEST(TlsfTest, AllocateReturnsAligned64Pointers)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));

    for (size_t size : {size_t(1), size_t(8), size_t(56), size_t(100),
                        size_t(4096), size_t(16384), size_t(100000)})
    {
        void* p = tlsf.Allocate(size);
        ASSERT_NE(p, nullptr) << "size " << size;
        EXPECT_TRUE(IsAligned64(p)) << "size " << size;
        EXPECT_TRUE(tlsf.Validate()) << "size " << size;
    }
}

TEST(TlsfTest, ZeroAndOversizeReturnNull)
{
    ArenaBuffer buf(1 << 16);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    EXPECT_EQ(tlsf.Allocate(0), nullptr);
    EXPECT_EQ(tlsf.Allocate(size_t(1) << 26), nullptr);   // beyond any arena
    // Pins the range-check-BEFORE-arithmetic ordering in AdjustRequestSize:
    // with the check after, bytes + 71 wraps and hands back a 56-byte block.
    EXPECT_EQ(tlsf.Allocate(~size_t(0)), nullptr);              // must not wrap into a tiny block
    EXPECT_EQ(tlsf.Allocate((size_t(1) << 25) + 1), nullptr);   // kMaxRequest (32MB) boundary
    EXPECT_TRUE(tlsf.Validate());
}

TEST(TlsfTest, FreeCoalescesNeighbors)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    const size_t initialFree = tlsf.GetFreeBytes();

    void* a = tlsf.Allocate(4096);
    void* b = tlsf.Allocate(4096);
    void* c = tlsf.Allocate(4096);
    ASSERT_TRUE(a && b && c);

    tlsf.Free(b);
    EXPECT_TRUE(tlsf.Validate());   // hole between two used blocks
    tlsf.Free(a);
    EXPECT_TRUE(tlsf.Validate());   // a+b must have merged (no adjacent free)
    tlsf.Free(c);
    EXPECT_TRUE(tlsf.Validate());   // everything merged back
    EXPECT_EQ(tlsf.GetFreeBytes(), initialFree);
}

TEST(TlsfTest, ExhaustionReturnsNullWithoutThrowing)
{
    ArenaBuffer buf(1 << 16);   // 64KB
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    const size_t initialFree = tlsf.GetFreeBytes();

    std::vector<void*> live;
    for (;;)
    {
        void* p = tlsf.Allocate(4096);
        if (!p) break;
        live.push_back(p);
    }
    EXPECT_GE(live.size(), 14u);     // ~15 x 4KB from 64KB minus overhead
    EXPECT_LE(live.size(), 16u);
    for (void* p : live) tlsf.Free(p);
    EXPECT_EQ(tlsf.GetFreeBytes(), initialFree);
    EXPECT_TRUE(tlsf.Validate());
}

TEST(TlsfTest, SameSizeReusesFreedBlock)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    void* a = tlsf.Allocate(16384);
    ASSERT_NE(a, nullptr);
    tlsf.Free(a);
    void* b = tlsf.Allocate(16384);
    EXPECT_EQ(a, b);   // good-fit finds the same block
}

// The case above passes even without the exact-fit peek, because the freed block
// coalesces with the arena's free tail and the merged block lands in a higher
// bucket. This one cannot coalesce -- both physical neighbors stay live -- so it
// only passes if Allocate peeks the bucket a same-size block is FILED in.
TEST(TlsfTest, StrandedHoleIsReusedBySameSize)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));

    void* a = tlsf.Allocate(16384);
    void* b = tlsf.Allocate(16384);
    void* c = tlsf.Allocate(16384);
    ASSERT_TRUE(a && b && c);

    tlsf.Free(b);                   // hole with live neighbors on both sides
    ASSERT_TRUE(tlsf.Validate());

    void* again = tlsf.Allocate(16384);
    EXPECT_EQ(again, b) << "stranded same-size hole was skipped; arena grows under churn";
    EXPECT_TRUE(tlsf.Validate());

    tlsf.Free(a);
    tlsf.Free(again);
    tlsf.Free(c);
    EXPECT_TRUE(tlsf.Validate());
}

TEST(TlsfTest, MultipleArenasServeAllocationsAndReportFullyFree)
{
    ArenaBuffer first(1 << 16);
    ArenaBuffer second(1 << 16);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(first.base, first.bytes));
    ASSERT_TRUE(tlsf.AddArena(second.base, second.bytes));
    EXPECT_EQ(tlsf.GetArenaCount(), 2u);
    const size_t initialFree = tlsf.GetFreeBytes();

    std::vector<void*> live;
    for (;;)
    {
        void* p = tlsf.Allocate(8192);
        if (!p) break;
        live.push_back(p);
        ASSERT_TRUE(IsAligned64(p));
    }
    ASSERT_TRUE(tlsf.Validate());

    // Both arenas must have been carved: one 64KB arena cannot serve them all.
    bool sawFirst = false, sawSecond = false;
    for (void* p : live)
    {
        sawFirst = sawFirst || InArena(p, first);
        sawSecond = sawSecond || InArena(p, second);
    }
    EXPECT_TRUE(sawFirst);
    EXPECT_TRUE(sawSecond);
    EXPECT_GE(live.size(), 12u);

    // Carved up: neither arena is releasable yet.
    size_t releasable = 0;
    tlsf.ForEachFullyFreeArena([&](void*, size_t) { ++releasable; });
    EXPECT_EQ(releasable, 0u);

    for (void* p : live) tlsf.Free(p);
    EXPECT_EQ(tlsf.GetFreeBytes(), initialFree);
    EXPECT_TRUE(tlsf.Validate());

    // Everything returned: both arenas report once each.
    std::vector<void*> reported;
    size_t reportedBytes = 0;
    tlsf.ForEachFullyFreeArena([&](void* base, size_t bytes) { reported.push_back(base); reportedBytes += bytes; });
    ASSERT_EQ(reported.size(), 2u);
    EXPECT_NE(reported[0], reported[1]);
    EXPECT_TRUE(reported[0] == first.base || reported[0] == second.base);
    EXPECT_TRUE(reported[1] == first.base || reported[1] == second.base);
    EXPECT_EQ(reportedBytes, first.bytes + second.bytes);
}

TEST(TlsfTest, RemoveArenaRefusedWhileCarvedAcceptedWhenFree)
{
    ArenaBuffer first(1 << 16);
    ArenaBuffer second(1 << 16);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(first.base, first.bytes));
    ASSERT_TRUE(tlsf.AddArena(second.base, second.bytes));

    // Allocate to exhaustion so BOTH arenas are definitely carved up (which one
    // a given request lands in is a free-list ordering detail, not a contract).
    std::vector<void*> live;
    for (;;)
    {
        void* p = tlsf.Allocate(4096);
        if (!p) break;
        live.push_back(p);
    }
    ASSERT_GE(live.size(), 20u);
    ASSERT_TRUE(tlsf.Validate());

    EXPECT_FALSE(tlsf.RemoveArena(first.base));
    EXPECT_FALSE(tlsf.RemoveArena(second.base));
    EXPECT_EQ(tlsf.GetArenaCount(), 2u);
    EXPECT_FALSE(tlsf.RemoveArena(nullptr));            // unknown base
    EXPECT_FALSE(tlsf.RemoveArena(&tlsf));              // unknown base
    EXPECT_EQ(tlsf.GetArenaCount(), 2u);
    EXPECT_TRUE(tlsf.Validate());

    for (void* p : live) tlsf.Free(p);
    ASSERT_TRUE(tlsf.Validate());

    const size_t freeBefore = tlsf.GetFreeBytes();
    ASSERT_TRUE(tlsf.RemoveArena(second.base));
    EXPECT_EQ(tlsf.GetArenaCount(), 1u);
    EXPECT_LT(tlsf.GetFreeBytes(), freeBefore);
    EXPECT_TRUE(tlsf.Validate());
    EXPECT_FALSE(tlsf.RemoveArena(second.base));        // already detached

    // The surviving arena still serves allocations.
    void* p = tlsf.Allocate(4096);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(InArena(p, first));
    tlsf.Free(p);

    ASSERT_TRUE(tlsf.RemoveArena(first.base));
    EXPECT_EQ(tlsf.GetArenaCount(), 0u);
    EXPECT_EQ(tlsf.GetFreeBytes(), 0u);
    EXPECT_TRUE(tlsf.Validate());
    EXPECT_EQ(tlsf.Allocate(64), nullptr);              // no arenas left
}

TEST(TlsfTest, MoveConstructionTransfersStateAndEmptiesSource)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf src;
    ASSERT_TRUE(src.AddArena(buf.base, buf.bytes));

    void* a = src.Allocate(4096);
    void* b = src.Allocate(16384);
    ASSERT_TRUE(a && b);
    const size_t freeBefore = src.GetFreeBytes();

    Astra::Tlsf dst(std::move(src));

    // State transferred.
    EXPECT_EQ(dst.GetArenaCount(), 1u);
    EXPECT_EQ(dst.GetFreeBytes(), freeBefore);
    EXPECT_TRUE(dst.Validate());

    // Source emptied, and still safe to use.
    EXPECT_EQ(src.GetArenaCount(), 0u);   // NOLINT: deliberate moved-from probe
    EXPECT_EQ(src.GetFreeBytes(), 0u);
    EXPECT_TRUE(src.Validate());
    EXPECT_EQ(src.Allocate(64), nullptr);
    src.Free(nullptr);
    EXPECT_TRUE(src.Validate());

    // Destination owns pointers minted by the source: freeing them coalesces
    // back to a pristine arena. (Block links live in arena memory, not in the
    // Tlsf object, so moving the object cannot invalidate them.)
    dst.Free(a);
    EXPECT_TRUE(dst.Validate());
    dst.Free(b);
    EXPECT_TRUE(dst.Validate());

    size_t releasable = 0;
    dst.ForEachFullyFreeArena([&](void* base, size_t bytes)
    {
        ++releasable;
        EXPECT_EQ(base, buf.base);
        EXPECT_EQ(bytes, buf.bytes);
    });
    EXPECT_EQ(releasable, 1u);

    void* again = dst.Allocate(4096);
    EXPECT_NE(again, nullptr);
    dst.Free(again);
    EXPECT_TRUE(dst.Validate());
}

TEST(TlsfTest, DeterministicStressPattern)
{
    ArenaBuffer buf(4 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));
    const size_t initialFree = tlsf.GetFreeBytes();

    // Fixed-seed LCG: deterministic, no <random>, no wall clock.
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() { state = state * 6364136223846793005ull + 1442695040888963407ull; return state >> 33; };

    std::vector<void*> live;
    for (int op = 0; op < 10000; ++op)
    {
        if (live.empty() || (next() & 3) != 0)   // 75% allocate
        {
            const size_t size = 56 + (next() % (64 * 1024));
            if (void* p = tlsf.Allocate(size)) live.push_back(p);
        }
        else
        {
            const size_t idx = next() % live.size();
            tlsf.Free(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
        if ((op & 511) == 0) { ASSERT_TRUE(tlsf.Validate()) << "op " << op; }
    }
    for (void* p : live) tlsf.Free(p);
    EXPECT_EQ(tlsf.GetFreeBytes(), initialFree);
    EXPECT_TRUE(tlsf.Validate());
}

// MultipleArenasServeAllocationsAndReportFullyFree exercises ForEachFullyFreeArena
// with two arenas, but never establishes the fresh-single-arena baseline (1 free
// arena before anything is carved) -- it only checks 0 (carved) then 2 (both
// freed). This closes that gap: single arena, 1 -> 0 -> 1 across one carve and
// one coalesce-back-to-whole free.
TEST(TlsfTest, FullyFreeArenaDetection)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));

    auto countFree = [&tlsf]
    {
        size_t n = 0;
        tlsf.ForEachFullyFreeArena([&n](void*, size_t) { ++n; });
        return n;
    };

    EXPECT_EQ(countFree(), 1u);            // fresh arena is fully free
    void* p = tlsf.Allocate(4096);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(countFree(), 0u);            // carved: not releasable
    tlsf.Free(p);
    EXPECT_EQ(countFree(), 1u);            // coalesced back: releasable again
}
