#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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
        if ((op & 511) == 0) ASSERT_TRUE(tlsf.Validate()) << "op " << op;
    }
    for (void* p : live) tlsf.Free(p);
    EXPECT_EQ(tlsf.GetFreeBytes(), initialFree);
    EXPECT_TRUE(tlsf.Validate());
}
