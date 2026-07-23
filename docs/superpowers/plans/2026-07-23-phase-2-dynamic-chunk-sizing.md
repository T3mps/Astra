# Phase 2 — Dynamic Chunk Sizing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Variable-capacity chunks that grow as an archetype populates (`clamp(archetypeBytes/2, 4KB, 512KB)`), backed by a header-only C++20 TLSF allocator over huge-page arenas, with compaction-on-defrag reclaiming memory.

**Architecture:** Four units in dependency order: (A) `Tlsf` — an isolated byte allocator faithful to Matt Conte's reference, adapted for permanent 64B payload alignment via a size-congruence rule; (B) `ArchetypeChunkPool` rewired onto it behavior-preserving at fixed size; (C) per-chunk-capacity addressing in `Archetype` (no `EntityLocation`/record/on-disk change) + the grow-as-populate policy; (D) rebuild-style `CompactChunks` replacing `CoalesceChunks` (also fixing a suspected pre-existing stale-chunkIndex bug).

**Tech Stack:** C++20 header-only, MSVC (MSBuild 3-config) + GoogleTest; `Mosaic::Bits` for bit-scans; existing `AllocateMemory`/`FreeMemory` huge-page primitives.

**Spec:** `docs/superpowers/specs/2026-07-23-phase-2-dynamic-chunk-sizing-design.md` (read it first).

## Global Constraints

- Branch: `perf/phase-2-dynamic-chunk-sizing` off `dev @ cb68bf3` (or current dev head). Never push; merge is user-gated at the end.
- Header-only, exception-free (`nullptr`/`Result` on failure, never throw), RTTI-free, C++20, ASCII comments. Match surrounding style (Allman braces, `m_` members, `ASTRA_NODISCARD`/`ASTRA_ASSERT`/`ASTRA_LIKELY`).
- Baseline test counts (dev `cb68bf3`): **Debug 697 / Release 695 / Dist 695**. Gate = all configs green + intended new tests; the 2-test Debug delta is `EXPECT_DEATH`-only, expected.
- Build: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` (PowerShell; also Release/Dist). If `Astra.sln` is missing (fresh worktree) or after ADDING files, regen: `D:\dev\_shared\tools\premake5 vs2022` from repo root.
- Tests: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`. Known flake: a LONE `CompressionTest.PerformanceBenchmark` failure = rerun isolated, not a regression.
- **TypeID ceiling:** the test binary is near the 128-component ceiling. New tests MUST reuse existing component types (`tests/TestComponents.hpp` — `Astra::Test::Position`, `Astra::Test::Tracked`, etc.), never define fresh component structs. `Tlsf` tests are componentless (unaffected).
- Commit messages end with the project's standard `Co-Authored-By: Claude` trailer; one commit per green step as written in each task.

## File Structure

| File | Role |
|---|---|
| `include/Astra/Core/Tlsf.hpp` | **NEW** — TLSF allocator (Unit A). Pure structure over caller-provided arenas; never calls OS memory APIs. |
| `tests/Core/TlsfTest.cpp` | **NEW** — Tlsf unit suite (componentless; heap-buffer arenas). |
| `include/Astra/Archetype/ArchetypeChunkPool.hpp` | Rewired: `Tlsf` + arena records replace `BlockInfo`/`ChunkNode`/free-list/`m_memoryToNode` (Unit B); `CreateChunk` gains byte-size param (Unit C); config gains sizing knobs (Unit C). |
| `tests/Registry/ChunkPoolTest.cpp` | **NEW** — pool-level TLSF-backed tests (Unit B). |
| `include/Astra/Archetype/Archetype.hpp` | Per-chunk capacity addressing, `AppendChunk`/`ComputeCapacityForBytes`/`NextChunkBytes`, `CompactChunks`, serialization tweaks (Units C, D). |
| `include/Astra/Archetype/ArchetypeManager.hpp` | `GetArchetypeMemoryUsage` per-chunk bytes (Unit C). |
| `include/Astra/Registry/Registry.hpp` | `GetFragmentationLevel` fill-based; `Defragment` calls `CompactChunks` (Units C, D). |
| `tests/Registry/ArchetypeTest.cpp`, `tests/Comprehensive/*` | Mechanical updates where uniform-capacity assumptions were asserted. |

Verified ground truth the tasks rely on (do NOT re-derive): `EntityLocation` stores `(chunkIndex, entityIndex)` directly — nothing decodes a flat index; `m_entitiesPerChunk`+shift/mask are capacity arithmetic only; `Chunk` already stores `m_capacity` and `m_chunkSize`; the pool is one shared instance (`ArchetypeManager.hpp:1523`); `Registry::Defragment` (Registry.hpp:1124) is `CoalesceChunks`' only caller.

---

### Task 1: `Tlsf` core — single-arena Allocate/Free with coalescing [OPUS recommended]

**Files:**
- Create: `include/Astra/Core/Tlsf.hpp`
- Create: `tests/Core/TlsfTest.cpp`

**Interfaces:**
- Consumes: `Mosaic::Bits::FindLastSet` (1-indexed; 0 when v==0), `Mosaic::Bits::CountTrailingZeros` (returns bit-width when v==0) from `<Mosaic/Bits.hpp>`; `SmallVector` from `../Container/SmallVector.hpp`; `ASTRA_*` macros from `Base.hpp`.
- Produces (later tasks rely on these exact signatures):
  - `class Astra::Tlsf` — movable, non-copyable.
  - `bool AddArena(void* mem, size_t bytes) noexcept` — `mem` must be 64B-aligned; Tlsf never owns memory.
  - `void* Allocate(size_t bytes) noexcept` — 64B-aligned payload; `nullptr` on OOM/0/too-large.
  - `void Free(void* ptr) noexcept` — O(1), coalesces physically-adjacent free neighbors; null-safe.
  - `size_t GetFreeBytes() const noexcept`, `size_t GetArenaCount() const noexcept`, `bool Validate() const noexcept` (heap-walk integrity check for tests/debug).

**Porting reference** (fetched from `https://github.com/mattconte/tlsf/blob/master/tlsf.c`; embed-quoted here so the implementer needs no network). The C reference routines to stay faithful to:

```c
/* mapping_insert */          /* mapping_search */
if (size < SMALL_BLOCK_SIZE)  if (size >= SMALL_BLOCK_SIZE) {
{ fl = 0;                       const size_t round =
  sl = size /                     (1 << (fls(size) - SL_INDEX_COUNT_LOG2)) - 1;
    (SMALL_BLOCK_SIZE /           size += round; }
     SL_INDEX_COUNT); }         mapping_insert(size, fli, sli);
else
{ fl = fls(size);
  sl = (size >> (fl - SL_INDEX_COUNT_LOG2)) ^ (1 << SL_INDEX_COUNT_LOG2);
  fl -= (FL_INDEX_SHIFT - 1); }

/* block_split */
remaining = offset_to_block(block_to_ptr(block), size - overhead);
remain_size = block_size(block) - (size + overhead);
/* block_absorb */  prev->size += block_size(block) + overhead; block_link_next(prev);
/* merge_prev */    if prev-free: remove(prev); absorb(prev, block)
/* merge_next */    if next free: remove(next); absorb(block, next)
/* add_pool */      first block: set size, free, prev-used, insert;
                    sentinel = link_next(first): size 0, used, prev-free.
```

**The one deliberate deviation (from the spec):** consecutive payloads obey `payload_{n+1} = payload_n + size_n + 8`. Keep **every block size ≡ 56 (mod 64)** (`AdjustRequestSize(b) = align_up(b + 8, 64) - 8`, min 56) and place each arena's first payload 64B-aligned ⇒ all payloads permanently 64B-aligned, closed under split/merge. No memalign path.

- [ ] **Step 1: Create branch**

```powershell
git checkout dev; git pull --ff-only 2>$null; git checkout -b perf/phase-2-dynamic-chunk-sizing
```

- [ ] **Step 2: Write the failing test file**

`tests/Core/TlsfTest.cpp` (complete file):

```cpp
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
    EXPECT_GE(live.size(), 14u);     // ~15 × 4KB from 64KB minus overhead
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
```

- [ ] **Step 3: Regen projects, build, verify the tests FAIL to compile**

```powershell
D:\dev\_shared\tools\premake5 vs2022
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: FAIL — `Astra/Core/Tlsf.hpp: No such file or directory`.

- [ ] **Step 4: Implement `include/Astra/Core/Tlsf.hpp`**

Complete implementation (this is the reviewed core — deviations from it need justification in the task report):

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include <Mosaic/Bits.hpp>

#include "../Container/SmallVector.hpp"
#include "Base.hpp"

namespace Astra
{
    // Two-Level Segregated Fit (TLSF) allocator.
    //
    // Structurally faithful to Matt Conte's public-domain reference
    // (https://github.com/mattconte/tlsf), re-expressed as a header-only,
    // exception-free C++20 module. One deliberate adaptation: every block size
    // is kept congruent to ALIGN_SIZE - kOverhead (mod ALIGN_SIZE), which makes
    // every payload permanently 64-byte aligned without the reference's
    // memalign gap-trim path (see AdjustRequestSize / AddArena).
    //
    // Tlsf manages caller-provided memory regions (arenas). It never touches
    // OS memory itself: the owner (ArchetypeChunkPool) acquires huge-page
    // arenas via AllocateMemory and registers them with AddArena.
    class Tlsf
    {
    public:
        static constexpr size_t ALIGN_SIZE_LOG2 = 6;
        static constexpr size_t ALIGN_SIZE = size_t(1) << ALIGN_SIZE_LOG2;              // 64

    private:
        static constexpr size_t SL_INDEX_COUNT_LOG2 = 5;
        static constexpr size_t SL_INDEX_COUNT = size_t(1) << SL_INDEX_COUNT_LOG2;      // 32
        static constexpr size_t FL_INDEX_SHIFT = SL_INDEX_COUNT_LOG2 + ALIGN_SIZE_LOG2; // 11
        static constexpr size_t FL_INDEX_MAX = 26;                                      // block sizes < 64MB
        static constexpr size_t FL_INDEX_COUNT = FL_INDEX_MAX - FL_INDEX_SHIFT + 1;     // 16
        static constexpr size_t SMALL_BLOCK_SIZE = size_t(1) << FL_INDEX_SHIFT;         // 2048

        // Boundary-tagged block header. prevPhys overlays the tail of the
        // previous block's payload and is valid only while that block is free;
        // nextFree/prevFree overlay this block's own payload while IT is free.
        struct BlockHeader
        {
            BlockHeader* prevPhys;
            size_t size;             // block size | kFreeBit | kPrevFreeBit
            BlockHeader* nextFree;
            BlockHeader* prevFree;
        };

        static constexpr size_t kFreeBit = size_t(1);
        static constexpr size_t kPrevFreeBit = size_t(2);
        static constexpr size_t kOverhead = sizeof(size_t);                             // 8
        static constexpr size_t kStartOffset = sizeof(BlockHeader*) + sizeof(size_t);   // 16
        static constexpr size_t kMinBlock = ALIGN_SIZE - kOverhead;                     // 56
        static constexpr size_t kMaxRequest = size_t(1) << (FL_INDEX_MAX - 1);          // 32MB
        static constexpr size_t kMinArenaBytes = 4096;

        struct ArenaInfo
        {
            void* base;
            size_t bytes;
            BlockHeader* first;
            size_t mainSize;   // the fully-free block size; equality means "arena fully free"
        };

    public:
        Tlsf() = default;
        Tlsf(const Tlsf&) = delete;
        Tlsf& operator=(const Tlsf&) = delete;

        Tlsf(Tlsf&& other) noexcept { MoveFrom(other); }
        Tlsf& operator=(Tlsf&& other) noexcept
        {
            if (this != &other) MoveFrom(other);
            return *this;
        }

        ASTRA_NODISCARD void* Allocate(size_t bytes) noexcept
        {
            const size_t adjusted = AdjustRequestSize(bytes);
            if (adjusted == 0 || adjusted > kMaxRequest) ASTRA_UNLIKELY
                return nullptr;

            int fl = 0, sl = 0;
            MappingSearch(adjusted, fl, sl);
            BlockHeader* block = SearchSuitable(fl, sl);
            if (!block) ASTRA_UNLIKELY
                return nullptr;

            ASTRA_ASSERT(BlockSize(block) >= adjusted, "TLSF: search returned undersized block");
            RemoveFreeBlock(block, fl, sl);
            TrimFree(block, adjusted);
            MarkAsUsed(block);
            return ToPtr(block);
        }

        void Free(void* ptr) noexcept
        {
            if (!ptr) ASTRA_UNLIKELY
                return;
            BlockHeader* block = FromPtr(ptr);
            ASTRA_ASSERT(!IsFree(block), "TLSF: double free");
            MarkAsFree(block);
            block = MergePrev(block);
            block = MergeNext(block);
            InsertBlock(block);
        }

        // Registers [mem, mem+bytes) as one free region. mem must be 64B-aligned.
        bool AddArena(void* mem, size_t bytes) noexcept
        {
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(mem) & (ALIGN_SIZE - 1)) == 0,
                         "TLSF: arena base must be 64B-aligned");
            if (bytes < kMinArenaBytes || bytes > (size_t(1) << FL_INDEX_MAX)) ASTRA_UNLIKELY
                return false;

            // First payload at base+64; its header starts kStartOffset before it.
            // Main size: largest value == 56 (mod 64) leaving room for front pad
            // (64) and the sentinel header (16) behind it: ((bytes-128) & ~63) + 56.
            std::byte* base = static_cast<std::byte*>(mem);
            BlockHeader* block = reinterpret_cast<BlockHeader*>(base + ALIGN_SIZE - kStartOffset);
            const size_t mainSize = ((bytes - 2 * ALIGN_SIZE) & ~(ALIGN_SIZE - 1)) + kMinBlock;

            block->prevPhys = nullptr;
            block->size = mainSize | kFreeBit;          // free, prev "used"
            InsertBlock(block);

            BlockHeader* sentinel = NextBlock(block);
            sentinel->prevPhys = block;
            sentinel->size = kPrevFreeBit;              // size 0, used, prev free

            m_arenas.push_back(ArenaInfo{mem, bytes, block, mainSize});
            return true;
        }

        // Arenas whose whole span is one free block again (releasable to the OS).
        template<typename F>
        void ForEachFullyFreeArena(F&& fn) const
        {
            for (const ArenaInfo& a : m_arenas)
            {
                if (IsFree(a.first) && BlockSize(a.first) == a.mainSize)
                    fn(a.base, a.bytes);
            }
        }

        // Detaches a fully-free arena so the caller can FreeMemory it.
        bool RemoveArena(void* base) noexcept
        {
            for (size_t i = 0; i < m_arenas.size(); ++i)
            {
                ArenaInfo& a = m_arenas[i];
                if (a.base != base)
                    continue;
                if (!IsFree(a.first) || BlockSize(a.first) != a.mainSize)
                    return false;   // still carved up: refuse
                RemoveBlock(a.first);
                a = m_arenas.back();
                m_arenas.pop_back();
                return true;
            }
            return false;
        }

        ASTRA_NODISCARD size_t GetFreeBytes() const noexcept { return m_freeBytes; }
        ASTRA_NODISCARD size_t GetArenaCount() const noexcept { return m_arenas.size(); }

        // Test/debug integrity walk: boundary tags, the 56-mod-64 size law,
        // free-bit consistency, full coalescing, sentinel placement, free-byte
        // accounting. Not for hot paths.
        ASTRA_NODISCARD bool Validate() const noexcept
        {
            size_t freeBytesSeen = 0;
            for (const ArenaInfo& a : m_arenas)
            {
                const BlockHeader* b = a.first;
                bool prevFree = false;
                for (;;)
                {
                    const size_t size = BlockSize(b);
                    if (size == 0)   // sentinel
                    {
                        if (IsFree(b)) return false;
                        if (IsPrevFree(b) != prevFree) return false;
                        const std::byte* end = reinterpret_cast<const std::byte*>(b) + kStartOffset;
                        if (end > static_cast<const std::byte*>(a.base) + a.bytes) return false;
                        break;
                    }
                    if (size % ALIGN_SIZE != ALIGN_SIZE - kOverhead) return false;
                    if (size < kMinBlock) return false;
                    if (IsPrevFree(b) != prevFree) return false;
                    if (IsFree(b) && prevFree) return false;   // adjacent frees must have merged
                    if (IsFree(b)) freeBytesSeen += size;
                    const BlockHeader* next = NextBlockConst(b);
                    if (IsFree(b) && next->prevPhys != b) return false;
                    prevFree = IsFree(b);
                    b = next;
                }
            }
            return freeBytesSeen == m_freeBytes;
        }

    private:
        void MoveFrom(Tlsf& other) noexcept
        {
            m_flBitmap = other.m_flBitmap;
            for (size_t i = 0; i < FL_INDEX_COUNT; ++i) m_slBitmap[i] = other.m_slBitmap[i];
            for (size_t i = 0; i < FL_INDEX_COUNT; ++i)
                for (size_t j = 0; j < SL_INDEX_COUNT; ++j)
                    m_blocks[i][j] = other.m_blocks[i][j];
            m_arenas = std::move(other.m_arenas);
            m_freeBytes = other.m_freeBytes;

            other.m_flBitmap = 0;
            for (size_t i = 0; i < FL_INDEX_COUNT; ++i) other.m_slBitmap[i] = 0;
            for (size_t i = 0; i < FL_INDEX_COUNT; ++i)
                for (size_t j = 0; j < SL_INDEX_COUNT; ++j)
                    other.m_blocks[i][j] = nullptr;
            other.m_arenas.clear();
            other.m_freeBytes = 0;
        }

        // ---- block primitives -------------------------------------------------
        ASTRA_NODISCARD static size_t BlockSize(const BlockHeader* b) noexcept { return b->size & ~(kFreeBit | kPrevFreeBit); }
        static void SetBlockSize(BlockHeader* b, size_t s) noexcept { b->size = s | (b->size & (kFreeBit | kPrevFreeBit)); }
        ASTRA_NODISCARD static bool IsFree(const BlockHeader* b) noexcept { return (b->size & kFreeBit) != 0; }
        ASTRA_NODISCARD static bool IsPrevFree(const BlockHeader* b) noexcept { return (b->size & kPrevFreeBit) != 0; }
        ASTRA_NODISCARD static void* ToPtr(BlockHeader* b) noexcept { return reinterpret_cast<std::byte*>(b) + kStartOffset; }
        ASTRA_NODISCARD static BlockHeader* FromPtr(void* p) noexcept { return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(p) - kStartOffset); }
        ASTRA_NODISCARD static BlockHeader* NextBlock(BlockHeader* b) noexcept
        {
            return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(ToPtr(b)) + BlockSize(b) - kOverhead);
        }
        ASTRA_NODISCARD static const BlockHeader* NextBlockConst(const BlockHeader* b) noexcept
        {
            return reinterpret_cast<const BlockHeader*>(
                reinterpret_cast<const std::byte*>(b) + kStartOffset + BlockSize(b) - kOverhead);
        }
        static void LinkNext(BlockHeader* b) noexcept { NextBlock(b)->prevPhys = b; }

        static void MarkAsFree(BlockHeader* b) noexcept
        {
            BlockHeader* next = NextBlock(b);
            next->prevPhys = b;
            next->size |= kPrevFreeBit;
            b->size |= kFreeBit;
        }
        static void MarkAsUsed(BlockHeader* b) noexcept
        {
            NextBlock(b)->size &= ~kPrevFreeBit;
            b->size &= ~kFreeBit;
        }

        // ---- size adjustment & mapping ---------------------------------------
        ASTRA_NODISCARD static size_t AdjustRequestSize(size_t bytes) noexcept
        {
            if (bytes == 0) return 0;
            const size_t aligned = ((bytes + kOverhead + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1)) - kOverhead;
            return aligned < kMinBlock ? kMinBlock : aligned;
        }

        ASTRA_NODISCARD static int Fls(size_t v) noexcept { return Mosaic::Bits::FindLastSet(v) - 1; }
        ASTRA_NODISCARD static int Ffs(uint32_t v) noexcept { return v ? Mosaic::Bits::CountTrailingZeros(v) : -1; }

        static void MappingInsert(size_t size, int& fl, int& sl) noexcept
        {
            if (size < SMALL_BLOCK_SIZE)
            {
                fl = 0;
                sl = static_cast<int>(size / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT));
            }
            else
            {
                const int f = Fls(size);
                sl = static_cast<int>((size >> (f - SL_INDEX_COUNT_LOG2)) ^ (size_t(1) << SL_INDEX_COUNT_LOG2));
                fl = f - (static_cast<int>(FL_INDEX_SHIFT) - 1);
            }
        }

        static void MappingSearch(size_t size, int& fl, int& sl) noexcept
        {
            if (size >= SMALL_BLOCK_SIZE)
            {
                const size_t round = (size_t(1) << (Fls(size) - SL_INDEX_COUNT_LOG2)) - 1;
                size += round;
            }
            MappingInsert(size, fl, sl);
        }

        // ---- free lists -------------------------------------------------------
        void InsertFreeBlock(BlockHeader* block, int fl, int sl) noexcept
        {
            BlockHeader* current = m_blocks[fl][sl];
            block->nextFree = current;
            block->prevFree = nullptr;
            if (current) current->prevFree = block;
            m_blocks[fl][sl] = block;
            m_flBitmap |= (uint32_t(1) << fl);
            m_slBitmap[fl] |= (uint32_t(1) << sl);
            m_freeBytes += BlockSize(block);
        }

        void RemoveFreeBlock(BlockHeader* block, int fl, int sl) noexcept
        {
            BlockHeader* prev = block->prevFree;
            BlockHeader* next = block->nextFree;
            if (next) next->prevFree = prev;
            if (prev) prev->nextFree = next;
            if (m_blocks[fl][sl] == block)
            {
                m_blocks[fl][sl] = next;
                if (!next)
                {
                    m_slBitmap[fl] &= ~(uint32_t(1) << sl);
                    if (!m_slBitmap[fl])
                        m_flBitmap &= ~(uint32_t(1) << fl);
                }
            }
            m_freeBytes -= BlockSize(block);
        }

        void InsertBlock(BlockHeader* block) noexcept
        {
            int fl, sl;
            MappingInsert(BlockSize(block), fl, sl);
            InsertFreeBlock(block, fl, sl);
        }

        void RemoveBlock(BlockHeader* block) noexcept
        {
            int fl, sl;
            MappingInsert(BlockSize(block), fl, sl);
            RemoveFreeBlock(block, fl, sl);
        }

        BlockHeader* SearchSuitable(int& fl, int& sl) noexcept
        {
            if (fl >= static_cast<int>(FL_INDEX_COUNT)) ASTRA_UNLIKELY
                return nullptr;
            uint32_t slMap = m_slBitmap[fl] & (~uint32_t(0) << sl);
            if (!slMap)
            {
                const uint32_t flMap = m_flBitmap & (~uint32_t(0) << (fl + 1));
                if (!flMap) return nullptr;
                fl = Ffs(flMap);
                slMap = m_slBitmap[fl];
            }
            sl = Ffs(slMap);
            return m_blocks[fl][sl];
        }

        // ---- split & merge ----------------------------------------------------
        ASTRA_NODISCARD static bool CanSplit(BlockHeader* block, size_t size) noexcept
        {
            return BlockSize(block) >= size + kOverhead + kMinBlock;
        }

        static BlockHeader* Split(BlockHeader* block, size_t size) noexcept
        {
            BlockHeader* remaining = reinterpret_cast<BlockHeader*>(
                static_cast<std::byte*>(ToPtr(block)) + size - kOverhead);
            const size_t remainSize = BlockSize(block) - (size + kOverhead);
            remaining->size = remainSize;   // fresh header: flags clear
            SetBlockSize(block, size);
            MarkAsFree(remaining);
            return remaining;
        }

        void TrimFree(BlockHeader* block, size_t size) noexcept
        {
            if (CanSplit(block, size))
            {
                BlockHeader* remaining = Split(block, size);
                LinkNext(block);
                remaining->size |= kPrevFreeBit;   // block is still free-flagged here
                InsertBlock(remaining);
            }
        }

        ASTRA_NODISCARD static BlockHeader* Absorb(BlockHeader* prev, BlockHeader* block) noexcept
        {
            prev->size += BlockSize(block) + kOverhead;   // prev's flags preserved
            LinkNext(prev);
            return prev;
        }

        BlockHeader* MergePrev(BlockHeader* block) noexcept
        {
            if (IsPrevFree(block))
            {
                BlockHeader* prev = block->prevPhys;
                RemoveBlock(prev);
                block = Absorb(prev, block);
            }
            return block;
        }

        BlockHeader* MergeNext(BlockHeader* block) noexcept
        {
            BlockHeader* next = NextBlock(block);
            if (IsFree(next))
            {
                RemoveBlock(next);
                block = Absorb(block, next);
            }
            return block;
        }

        uint32_t m_flBitmap = 0;
        uint32_t m_slBitmap[FL_INDEX_COUNT] = {};
        BlockHeader* m_blocks[FL_INDEX_COUNT][SL_INDEX_COUNT] = {};
        SmallVector<ArenaInfo, 8> m_arenas;
        size_t m_freeBytes = 0;
    };
}
```

Implementation notes for the executor:
- `Free`'s `MarkAsFree` before `MergePrev` matches the reference order (`block_mark_as_free` → merges → insert).
- If `SmallVector` lacks `pop_back`/`back`, check its API in `include/Astra/Container/SmallVector.hpp` and use the closest equivalent (it is used with `push_back` throughout the codebase).
- `MergeNext` on the last real block sees the used sentinel ⇒ never merges past the arena end; sentinel `size==0` means `NextBlock(sentinel)` is never taken (sentinel is never freed).

- [ ] **Step 5: Build Debug, run the Tlsf suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=TlsfTest.*
```
Expected: `[  PASSED  ] 7 tests.`

- [ ] **Step 6: Run the FULL Debug suite (no collateral damage)**

```powershell
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: 697 + 7 = **704 passed** (Debug).

- [ ] **Step 7: Commit**

```powershell
git add include/Astra/Core/Tlsf.hpp tests/Core/TlsfTest.cpp
git commit -m "feat(core): TLSF allocator - header-only C++20 port of mattconte/tlsf with 64B payload alignment via size congruence (Phase 2 Unit A)"
```

---

### Task 2: `Tlsf` arena lifecycle — fully-free detection, RemoveArena, multi-arena

**Files:**
- Modify: `include/Astra/Core/Tlsf.hpp` (only if Task 1 review found gaps — `ForEachFullyFreeArena`/`RemoveArena` are already specified there)
- Test: `tests/Core/TlsfTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `Tlsf` public API.
- Produces: verified behavior later tasks rely on — `ForEachFullyFreeArena(fn(void* base, size_t bytes))` lists exactly the releasable arenas; `RemoveArena(base)` refuses while any block in the arena is live; allocation spills across arenas.

- [ ] **Step 1: Append the failing tests**

```cpp
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

TEST(TlsfTest, RemoveArenaOnlyWhenFullyFree)
{
    ArenaBuffer buf(1 << 20);
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(buf.base, buf.bytes));

    void* p = tlsf.Allocate(4096);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(tlsf.RemoveArena(buf.base));   // live block: refuse
    tlsf.Free(p);
    EXPECT_TRUE(tlsf.RemoveArena(buf.base));
    EXPECT_EQ(tlsf.GetArenaCount(), 0u);
    EXPECT_EQ(tlsf.GetFreeBytes(), 0u);
    EXPECT_EQ(tlsf.Allocate(4096), nullptr);    // nothing left to allocate from
}

TEST(TlsfTest, AllocationSpillsAcrossArenas)
{
    ArenaBuffer a(1 << 16), b(1 << 16);   // 2 x 64KB
    Astra::Tlsf tlsf;
    ASSERT_TRUE(tlsf.AddArena(a.base, a.bytes));
    ASSERT_TRUE(tlsf.AddArena(b.base, b.bytes));
    EXPECT_EQ(tlsf.GetArenaCount(), 2u);

    std::vector<void*> live;
    for (;;)
    {
        void* p = tlsf.Allocate(16 * 1024);
        if (!p) break;
        live.push_back(p);
    }
    EXPECT_GE(live.size(), 6u);   // ~3 x 16KB per 64KB arena
    EXPECT_TRUE(tlsf.Validate());
    for (void* p : live) tlsf.Free(p);
    size_t n = 0;
    tlsf.ForEachFullyFreeArena([&n](void*, size_t) { ++n; });
    EXPECT_EQ(n, 2u);
}
```

- [ ] **Step 2: Build + run** — same commands as Task 1 Step 5, filter `TlsfTest.*`. Expected: **10 passed**. If a test fails, fix `Tlsf.hpp` (these APIs were specified in Task 1; failures here are bugs, not missing features).

- [ ] **Step 3: Commit**

```powershell
git add tests/Core/TlsfTest.cpp include/Astra/Core/Tlsf.hpp
git commit -m "test(core): TLSF arena lifecycle - fully-free detection, RemoveArena refusal, multi-arena spill"
```

---

### Task 3: Pool rewire onto TLSF — behavior-preserving at fixed chunk size [OPUS recommended]

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (private machinery + `CreateChunk`/`ReturnChunk`/`Defragment`/ctor/dtor/moves; `Chunk` class and `CreateChunk` SIGNATURE unchanged)
- Create: `tests/Registry/ChunkPoolTest.cpp`
- Possibly adjust: `tests/Comprehensive/MemoryCleanupTest.cpp`, `tests/Comprehensive/ResourceExhaustionTest.cpp` (stats-semantics asserts)

**Interfaces:**
- Consumes: `Tlsf` (Task 1/2); existing `AllocateMemory(size, CACHE_LINE_SIZE, flags) -> AllocResult{ptr,size,usedHugePages}` / `FreeMemory(ptr,size,usedHugePages)` from `Core/Memory.hpp`; `HUGE_PAGE_SIZE` (2MB).
- Produces (unchanged externally): `CreateChunk(size_t entitiesPerChunk, const ArchetypeColumnMeta*) -> unique_ptr<Chunk, ChunkDeleter>`; `ReturnChunk(void*)`; `GetChunkSize()`; `GetStats()`; `Defragment() -> DefragmentResult`. Internal addition Task 4 will use: `void* AllocateChunkBytes(size_t chunkBytes)` (private).

**What gets DELETED:** `ChunkNode`, `BlockInfo`, `m_blocks`, `m_freeList`, `m_memoryToNode`, `AllocateBlock()`, `AcquireMemory()`, and `ReturnChunk`'s node lookup. `Config` keeps ALL existing fields (NSDMIs unchanged) — `chunksPerBlock` becomes the arena-sizing hint, `initialBlocks` pre-allocates arenas, `maxChunks` stays the live-chunk cap.

- [ ] **Step 1: Write the failing pool test**

`tests/Registry/ChunkPoolTest.cpp` (complete file):

```cpp
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
```

- [ ] **Step 2: Regen + build + run new tests against the OLD pool** — they must PASS already (except possibly `DefragmentReleasesEmptyArenasKeepingReserve`, whose block-vs-arena counts differ; note actual behavior). This pins current semantics BEFORE the rewire:

```powershell
D:\dev\_shared\tools\premake5 vs2022
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ChunkPoolTest.*
```

Commit the pinning tests: `git add tests/Registry/ChunkPoolTest.cpp; git commit -m "test(pool): pin ArchetypeChunkPool create/return/cap/defrag semantics before TLSF rewire"`

- [ ] **Step 3: Rewire the pool internals**

Replace the private machinery of `ArchetypeChunkPool` with:

```cpp
    private:
        struct ArenaRecord
        {
            void* memory = nullptr;
            size_t size = 0;
            bool usedHugePages = false;
        };

        bool GrowArena(size_t minBytes)
        {
            // Arena sizing: the legacy chunksPerBlock x chunkSize hint, at least
            // one huge page, and always enough for the request + TLSF bookkeeping
            // (front pad 64 + sentinel 16 + rounding; 256 is comfortably safe).
            constexpr size_t kArenaOverhead = 256;
            const size_t want = std::max(m_config.chunksPerBlock * m_config.chunkSize,
                                         minBytes + kArenaOverhead);
            // Do NOT pre-round to huge pages here: AllocateMemory already rounds
            // internally when it takes the huge-page path (size >= 2MB), and small
            // arena hints (e.g. chunksPerBlock=4 in tests) must stay small for
            // parity with the old per-block allocation. Register r.size (actual).
            AllocFlags flags = AllocFlags::None;
            if (m_config.useHugePages)
            {
                flags = flags | AllocFlags::HugePages;
            }
            AllocResult r = AllocateMemory(want, CACHE_LINE_SIZE, flags);
            if (!r.ptr) ASTRA_UNLIKELY
                return false;
            if (!m_tlsf.AddArena(r.ptr, r.size)) ASTRA_UNLIKELY
            {
                FreeMemory(r.ptr, r.size, r.usedHugePages);
                return false;
            }
            m_arenas.push_back(ArenaRecord{r.ptr, r.size, r.usedHugePages});
            m_blockAllocations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        void* AllocateChunkBytes(size_t chunkBytes)
        {
            if (m_totalChunks.load(std::memory_order_relaxed) >= m_config.maxChunks) ASTRA_UNLIKELY
            {
                m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            void* p = m_tlsf.Allocate(chunkBytes);
            if (!p)
            {
                if (!GrowArena(chunkBytes)) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
                p = m_tlsf.Allocate(chunkBytes);
                if (!p) ASTRA_UNLIKELY
                {
                    m_failedAcquires.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
            }
            m_totalChunks.fetch_add(1, std::memory_order_relaxed);
            m_acquireCount.fetch_add(1, std::memory_order_relaxed);
            return p;
        }

        Config m_config;
        Tlsf m_tlsf;
        SmallVector<ArenaRecord, 16> m_arenas;
        std::atomic<size_t> m_totalChunks{0};       // LIVE chunks (semantics change, see GetStats)
        std::atomic<size_t> m_acquireCount{0};
        std::atomic<size_t> m_releaseCount{0};
        std::atomic<size_t> m_blockAllocations{0};  // arenas allocated
        std::atomic<size_t> m_failedAcquires{0};
```

Public methods become:

```cpp
        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(size_t entitiesPerChunk, const ArchetypeColumnMeta* meta)
        {
            void* memory = AllocateChunkBytes(m_config.chunkSize);
            if (!memory) ASTRA_UNLIKELY
                return nullptr;
            auto* chunk = new Chunk(entitiesPerChunk, meta, memory, m_config.chunkSize);
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, ChunkDeleter{this, memory});
        }

        void ReturnChunk(void* memory)
        {
            if (!memory) ASTRA_UNLIKELY
                return;
            m_tlsf.Free(memory);
            m_totalChunks.fetch_sub(1, std::memory_order_relaxed);
            m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        }

        ASTRA_NODISCARD Stats GetStats() const
        {
            Stats s;
            const size_t live = m_totalChunks.load(std::memory_order_relaxed);
            const size_t freeEquivalent = m_tlsf.GetFreeBytes() / m_config.chunkSize;
            s.totalChunks = live + freeEquivalent;   // preserves the old "capacity in chunks" reading
            s.freeChunks = freeEquivalent;
            s.acquireCount = m_acquireCount.load(std::memory_order_relaxed);
            s.releaseCount = m_releaseCount.load(std::memory_order_relaxed);
            s.blockAllocations = m_blockAllocations.load(std::memory_order_relaxed);
            s.failedAcquires = m_failedAcquires.load(std::memory_order_relaxed);
            return s;
        }

        DefragmentResult Defragment()
        {
            DefragmentResult result;
            SmallVector<void*, 8> releasable;
            m_tlsf.ForEachFullyFreeArena([&releasable](void* base, size_t) { releasable.push_back(base); });

            // Mirror old policy: if EVERY arena is free keep one as reserve;
            // otherwise all fully-free arenas can go.
            size_t startIndex = (releasable.size() == m_arenas.size() && !releasable.empty()) ? 1 : 0;
            for (size_t i = startIndex; i < releasable.size(); ++i)
            {
                void* base = releasable[i];
                if (!m_tlsf.RemoveArena(base)) ASTRA_UNLIKELY
                    continue;
                for (size_t j = 0; j < m_arenas.size(); ++j)
                {
                    if (m_arenas[j].memory == base)
                    {
                        result.bytesFreed += m_arenas[j].size;
                        FreeMemory(m_arenas[j].memory, m_arenas[j].size, m_arenas[j].usedHugePages);
                        m_arenas[j] = m_arenas.back();
                        m_arenas.pop_back();
                        ++result.blocksReleased;
                        break;
                    }
                }
            }
            result.blocksKept = m_arenas.size();
            result.chunksInUse = m_totalChunks.load(std::memory_order_relaxed);
            return result;
        }
```

Constructor: keep the existing asserts + `chunksPerBlock==0 -> HUGE_PAGE_SIZE/chunkSize` normalization; replace the `initialBlocks` loop body with `GrowArena(0)`. Destructor: `for (auto& a : m_arenas) FreeMemory(a.memory, a.size, a.usedHugePages);`. Move ctor/assign: move `m_config`, `m_tlsf`, `m_arenas`, and the atomics via load/store (existing pattern), freeing own arenas first in move-assign. Keep the `ChunkDeleter` and `Chunk` classes byte-for-byte unchanged. Keep the member ordering constraint: the pool member in `ArchetypeManager` must continue to be declared BEFORE the archetype list (destruction order: archetypes first).

- [ ] **Step 4: Build Debug + run pool tests, then FULL suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ChunkPoolTest.*:*MemoryCleanup*:*ResourceExhaustion*
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: all green. `MemoryCleanupTest` asserts `stats2.totalChunks == stats1.totalChunks` after destruction — the `live + freeBytes/chunkSize` formula preserves this. If any Comprehensive test still asserts old block semantics, adjust THAT assert minimally with a comment referencing this task.

- [ ] **Step 5: 3-config verify**

Run MSBuild + AstraTest for Release and Dist. Expected: Release/Dist = 695 baseline + new tests, Debug = 697 + new.

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Archetype/ArchetypeChunkPool.hpp tests/
git commit -m "refactor(pool): rewire ArchetypeChunkPool onto TLSF arenas - fixed chunk size, behavior-preserving (Phase 2 Unit B)"
```

---

### Task 4: Variable-capacity chunk addressing + exact (non-pow2) capacity [OPUS recommended]

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (members, `Initialize`, all capacity arithmetic, `Serialize`/`Deserialize`)
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`CreateChunk` gains `chunkBytes`; `Chunk::GetChunkBytes()` accessor)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`GetArchetypeMemoryUsage`)
- Modify: `include/Astra/Registry/Registry.hpp` (`GetFragmentationLevel`)
- Modify: `tests/Registry/ChunkPoolTest.cpp` (Task 3's tests use the 2-arg `CreateChunk` this task deletes — update to `CreateChunk(64, pool.GetChunkSize(), &meta)`)
- Test: `tests/Registry/ArchetypeTest.cpp` (update uniform-capacity assumptions; add exact-capacity + round-trip tests)

**Interfaces:**
- Consumes: Task 3's pool (`AllocateChunkBytes`).
- Produces (later tasks rely on):
  - `ArchetypeChunkPool::CreateChunk(size_t capacity, size_t chunkBytes, const ArchetypeColumnMeta* meta)` — NEW signature (3 args; the 2-arg form is deleted).
  - `Chunk::GetChunkBytes() const noexcept -> size_t`.
  - `Archetype::GetTotalCapacity() const noexcept -> size_t` (sum of chunk capacities, maintained incrementally).
  - `Archetype::ComputeCapacityForBytes(size_t chunkBytes) const -> size_t` (private).
  - `Archetype::AppendChunk(size_t chunkBytes) -> ArchetypeChunk*` (private; nullptr on failure) — THE single chunk-creation path.
  - REMOVED: `m_entitiesPerChunk`, `m_entitiesPerChunkShift`, `m_entitiesPerChunkMask`, `GetEntitiesPerChunk()`, `GetEntitiesPerChunkShift()`, `GetEntitiesPerChunkMask()`.

- [ ] **Step 1: Write failing tests (append to `tests/Registry/ArchetypeTest.cpp`)**

```cpp
// Phase 2 (dynamic chunk sizing) - exact capacity replaces pow2 bit_floor.
TEST_F(ArchetypeTest, ChunkCapacityIsExactNotPow2)
{
    // Position (12B float x3) in a 16KB chunk: exact fit is > the old
    // bit_floor value. With per-column cache-line padding reserved, the
    // capacity must be usable/perEntitySize, not rounded down to a power of 2.
    Astra::Archetype archetype(Astra::MakeComponentMask<Astra::Test::Position>());
    archetype.SetComponentPool(&m_pool);
    archetype.Initialize({m_registry->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value())});
    ASSERT_TRUE(archetype.IsInitialized());

    const auto& chunks = archetype.GetChunks();
    ASSERT_FALSE(chunks.empty());
    const size_t chunkBytes = chunks[0]->GetChunkBytes();
    const size_t cap = chunks[0]->GetCapacity();
    // Single non-empty column -> alignmentOverhead 0 -> exact division. Since
    // sizeof(Position)=12 never divides a pow2 chunk evenly into a pow2 count,
    // exact division proves the bit_floor rounding is gone. Size-agnostic on
    // purpose: still holds after Task 5 shrinks the first chunk to 4KB.
    EXPECT_EQ(cap, chunkBytes / sizeof(Astra::Test::Position));
    EXPECT_GT(cap, std::bit_floor(cap) == cap ? size_t(0) : std::bit_floor(cap));
    EXPECT_EQ(archetype.GetTotalCapacity(), cap);
}

TEST_F(ArchetypeTest, TotalCapacityTracksChunkCreation)
{
    Astra::Archetype archetype(Astra::MakeComponentMask<Astra::Test::Position>());
    archetype.SetComponentPool(&m_pool);
    archetype.Initialize({m_registry->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value())});
    ASSERT_TRUE(archetype.IsInitialized());

    const size_t firstCap = archetype.GetTotalCapacity();
    // Fill past the first chunk; total capacity must grow by whole chunks.
    for (size_t i = 0; i < firstCap + 1; ++i)
    {
        archetype.AddEntity(Astra::Entity(static_cast<uint32_t>(i), 1));
    }
    EXPECT_GT(archetype.GetTotalCapacity(), firstCap);
    EXPECT_EQ(archetype.GetEntityCount(), firstCap + 1);
}
```

Adapt the test fixture names/setup to the file's existing `ArchetypeTest` fixture (it already constructs a pool + registry — reuse its members; do NOT create new component types). Note: if `GetComponentDescriptor` has a different name in the fixture's usage, mirror how the file's existing tests obtain descriptors (e.g. ArchetypeTest.cpp:60-80).

- [ ] **Step 2: Build; verify the new tests FAIL** (no `GetTotalCapacity` yet). Expected compile error — that IS the red state.

- [ ] **Step 3: Implement the addressing swap**

3a. `ArchetypeChunkPool` — new `CreateChunk` signature + accessor on `Chunk`:

```cpp
        // (in class Chunk, public:)
        ASTRA_NODISCARD size_t GetChunkBytes() const noexcept { return m_chunkSize; }

        // (pool; REPLACES the 2-arg overload)
        std::unique_ptr<Chunk, ChunkDeleter> CreateChunk(size_t capacity, size_t chunkBytes, const ArchetypeColumnMeta* meta)
        {
            ASTRA_ASSERT(capacity > 0, "Chunk capacity must be positive");
            void* memory = AllocateChunkBytes(chunkBytes);
            if (!memory) ASTRA_UNLIKELY
                return nullptr;
            auto* chunk = new Chunk(capacity, meta, memory, chunkBytes);
            return std::unique_ptr<Chunk, ChunkDeleter>(chunk, ChunkDeleter{this, memory});
        }
```

3b. `Archetype` members — replace `m_entitiesPerChunk`/`m_entitiesPerChunkShift`/`m_entitiesPerChunkMask` with:

```cpp
        size_t m_perEntitySize = 0;        // summed non-empty component sizes
        size_t m_alignmentOverhead = 0;    // conservative per-chunk column padding estimate
        size_t m_totalCapacity = 0;        // sum of chunk capacities, maintained incrementally
```

3c. Capacity helpers + the single creation path (private):

```cpp
        // Exact (non-pow2) capacity for a chunk of chunkBytes, under the same
        // conservative alignment-overhead estimate Initialize() uses. Zero-size
        // archetypes (tag-only/root) store entities in the side vector, not chunk
        // memory: give them chunkBytes/64 slots (== the legacy 256 at 16KB).
        ASTRA_NODISCARD size_t ComputeCapacityForBytes(size_t chunkBytes) const noexcept
        {
            if (m_perEntitySize == 0)
                return chunkBytes >> 6;
            const size_t usable = chunkBytes > m_alignmentOverhead ? chunkBytes - m_alignmentOverhead : 0;
            return usable / m_perEntitySize;
        }

        // The ONE place chunks are created: computes capacity, allocates, updates
        // m_totalCapacity. Returns nullptr on layout/pool failure.
        ArchetypeChunk* AppendChunk(size_t chunkBytes)
        {
            const size_t capacity = ComputeCapacityForBytes(chunkBytes);
            if (capacity == 0 || !m_chunkPool) ASTRA_UNLIKELY
                return nullptr;
            auto chunk = m_chunkPool->CreateChunk(capacity, chunkBytes, &m_columnMeta);
            if (!chunk) ASTRA_UNLIKELY
                return nullptr;
            m_totalCapacity += capacity;
            m_chunks.emplace_back(std::move(chunk));
            return m_chunks.back().get();
        }

    public:
        ASTRA_NODISCARD size_t GetTotalCapacity() const noexcept { return m_totalCapacity; }
```

3d. `Initialize` (lines ~94-165) — keep descriptor/meta setup; replace the sizing tail:

```cpp
            m_perEntitySize = perEntitySize;
            m_alignmentOverhead = alignmentOverhead;

            const size_t chunkBytes = m_chunkPool ? m_chunkPool->GetChunkSize()
                                                  : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
            // Same refusal semantics as before: no valid per-chunk layout at this
            // chunk size means never create a chunk that cannot hold one entity.
            if (perEntitySize > 0 && ComputeCapacityForBytes(chunkBytes) == 0) ASTRA_UNLIKELY
            {
                m_initialized = false;
                return;
            }
            m_initialized = true;
            if (!AppendChunk(chunkBytes)) ASTRA_UNLIKELY
            {
                m_initialized = false;
                return;
            }
```

3e. Mechanical swaps (every remaining `m_entitiesPerChunk*` use):

| Site (current line) | Replacement |
|---|---|
| `AddEntities` ~197-215 + `AddEntitiesWith` ~266-284 + `AddEntityInternal` ~1180-1196 upfront loops | `if (!m_initialized) return ...;` then `while (GetRemainingCapacity() < count) { if (!AppendChunk(chunkBytes)) break/return partial; }` with `chunkBytes` from `m_chunkPool->GetChunkSize()` (Task 5 swaps this to `NextChunkBytes()`) |
| `available = m_entitiesPerChunk - chunk->GetCount()` (224, 1218, 1021, 1057) | `chunk->GetCapacity() - chunk->GetCount()` |
| `EnsureCapacity` 611-620 | `if (required > m_totalCapacity) m_chunks.reserve(m_chunks.size() + (required - m_totalCapacity + cap0 - 1) / cap0);` where `cap0 = std::max<size_t>(1, ComputeCapacityForBytes(m_chunkPool ? m_chunkPool->GetChunkSize() : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE))` — vector-reserve estimate only |
| `GetRemainingCapacity` 629-631 | loop `remaining += m_chunks[i]->GetCapacity() - m_chunks[i]->GetCount();` |
| `GetFragmentationLevel` 636-646 | `return m_totalCapacity == 0 ? 0.0f : 1.0f - float(m_entityCount) / float(m_totalCapacity);` (fill-based; 0 = perfectly packed, same trigger semantics) |
| `ShouldCoalesce`/`CoalesceChunks` utilization math (989, 1011, 1021, 1057) | per-chunk `GetCapacity()` (whole functions replaced in Task 6; keep them compiling now) |
| `GetOrCreateChunk` 1364 guard + 1389 create | guard `if (!m_initialized) return {INVALID_CHUNK_INDEX,false};` create via `AppendChunk(m_chunkPool->GetChunkSize())` returning its index |
| `m_entitiesPerChunk == 0` guards (197, 266, 1180) | `!m_initialized` |
| `GetEntitiesPerChunk/Shift/Mask` accessors (1100, 1485-1486) | DELETE |
| `CoalesceChunks` chunk-erase (1081-1088) | after erasing, recompute `m_totalCapacity` by summing remaining chunks (temporary until Task 6 replaces the function) |

3f. Serialization (`Serialize` 656 / `Deserialize` 730-975): on-disk field layout unchanged.
- `Serialize`: replace `writer(uint64_t(m_entitiesPerChunk))` with the per-chunk-count bound the reader checks against:
```cpp
            uint64_t maxChunkEntityCount = 1;
            for (const auto& chunk : m_chunks)
                if (chunk) maxChunkEntityCount = std::max(maxChunkEntityCount, static_cast<uint64_t>(chunk->GetCount()));
            writer(maxChunkEntityCount);
```
- `Deserialize`: the two guards (833, 849) keep working unchanged (they bound the read value against the pool chunk size; old saves carry old capacities which still pass). Replace the per-chunk `CreateChunk(entitiesPerChunk, meta)` call (898) with exact-fit creation through the archetype so `m_totalCapacity` stays correct:
```cpp
                const size_t capacity = std::max<size_t>(1, chunkEntityCount);
                const size_t chunkBytes = archetype->m_perEntitySize == 0
                    ? std::max<size_t>(64, capacity << 6)
                    : capacity * archetype->m_perEntitySize + archetype->m_alignmentOverhead;
                // (m_chunks was cleared above; AppendChunk recreates with exact fit)
                ArchetypeChunk* chunk = archetype->AppendChunk(chunkBytes);
                if (!chunk)
                {
                    return ResultType::Err(SerializationError::OutOfMemory);
                }
```
  and reset `archetype->m_totalCapacity = 0;` next to the existing `m_chunks.clear()` (866). Entity slots stay `[0, count)` per chunk, so every serialized `EntityRecord (chunkIndex, entityIndex)` still resolves identically — this is why exact-fit loading is location-safe.

3g. `ArchetypeManager::GetArchetypeMemoryUsage` (559-570): replace `chunkCount * chunkSize` with a loop `for (const auto& chunk : entry.archetype->GetChunks()) total += chunk->GetChunkBytes();`.

3h. `Registry::GetFragmentationLevel` (1055-1086): replace the `GetEntitiesPerChunk` optimal-chunk math with fill-based:
```cpp
            size_t totalEntities = 0, totalCapacity = 0;
            for (const auto* arch : archetypes)
            {
                totalEntities += arch->GetEntityCount();
                totalCapacity += arch->GetTotalCapacity();
            }
            if (totalCapacity == 0) return 0.0f;
            return 1.0f - static_cast<float>(totalEntities) / static_cast<float>(totalCapacity);
```

3i. Update `tests/Registry/ArchetypeTest.cpp` uniform-capacity usages (lines 74-78, 307, 518, 546, 610, 679, 764, 812, 939): replace `archetype.GetEntitiesPerChunk()` with `archetype.GetChunks()[0]->GetCapacity()` (chunk sizing is still uniform in this task, so "capacity of chunk 0" preserves each test's intent); line 812's serialize-equality becomes chunk-count + entity-count equality. Keep each test's assertion story intact — these are mechanical substitutions, not test rewrites.

- [ ] **Step 4: Build Debug; run Archetype + serialization suites, then FULL suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ArchetypeTest.*:*Serialization*:*RoundTrip*
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: all green including the 2 new tests, and specifically the save/load round-trip suites (the exact-fit Deserialize path) and `RootArchetypeRoundTripTest`.

- [ ] **Step 5: 3-config verify** (Release + Dist build & full run).

- [ ] **Step 6: Commit**

```powershell
git add include/Astra tests/Registry/ArchetypeTest.cpp
git commit -m "refactor(archetype): per-chunk-capacity addressing - drop uniform m_entitiesPerChunk shift/mask, exact non-pow2 capacity, exact-fit deserialize (Phase 2 Unit C part 1)"
```

---

### Task 5: Grow-as-populate sizing policy

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (Config knobs + accessors)
- Modify: `include/Astra/Archetype/Archetype.hpp` (`NextChunkBytes()`, growth-site wiring, Deserialize guard cap)
- Test: `tests/Registry/ArchetypeTest.cpp` (append)

**Interfaces:**
- Consumes: Task 4's `AppendChunk`/`ComputeCapacityForBytes`/`GetTotalCapacity`.
- Produces:
  - `ArchetypeChunkPool::Config` gains `size_t minChunkBytes = MIN_CHUNK_SIZE; size_t maxChunkBytes = 512 * 1024; size_t growDivisor = 2;` (NSDMIs — zero churn at call sites).
  - Pool accessors: `GetMinChunkBytes()`, `GetMaxChunkBytes()`, `GetGrowDivisor()` (each `const noexcept -> size_t`).
  - `Archetype::NextChunkBytes() const noexcept -> size_t` (private) — the grow-as-populate formula.

- [ ] **Step 1: Write failing tests (append to ArchetypeTest.cpp)**

```cpp
TEST_F(ArchetypeTest, GrowAsPopulateRampsChunkBytes)
{
    Astra::Archetype archetype(Astra::MakeComponentMask<Astra::Test::Position>());
    archetype.SetComponentPool(&m_pool);
    archetype.Initialize({m_registry->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value())});
    ASSERT_TRUE(archetype.IsInitialized());

    // First chunk: archetype empty -> clamps to minChunkBytes (4KB).
    const auto& chunks = archetype.GetChunks();
    ASSERT_FALSE(chunks.empty());
    EXPECT_EQ(chunks[0]->GetChunkBytes(), 4096u);

    // Populate enough to force several chunks; sizes must be non-decreasing
    // and eventually exceed the first chunk's size (the geometric ramp).
    for (uint32_t i = 0; i < 20000; ++i)
    {
        archetype.AddEntity(Astra::Entity(i, 1));
    }
    ASSERT_GE(chunks.size(), 3u);
    for (size_t c = 1; c < chunks.size(); ++c)
    {
        EXPECT_GE(chunks[c]->GetChunkBytes(), chunks[c - 1]->GetChunkBytes())
            << "chunk " << c << " shrank";
    }
    EXPECT_GT(chunks.back()->GetChunkBytes(), chunks[0]->GetChunkBytes());
}

TEST_F(ArchetypeTest, SmallArchetypeStaysAtMinimumChunk)
{
    Astra::Archetype archetype(Astra::MakeComponentMask<Astra::Test::Position>());
    archetype.SetComponentPool(&m_pool);
    archetype.Initialize({m_registry->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value())});
    ASSERT_TRUE(archetype.IsInitialized());

    for (uint32_t i = 0; i < 100; ++i)   // 100 x 12B ~ 1.2KB: fits chunk 0
    {
        archetype.AddEntity(Astra::Entity(i, 1));
    }
    EXPECT_EQ(archetype.GetChunks().size(), 1u);
    EXPECT_EQ(archetype.GetChunks()[0]->GetChunkBytes(), 4096u);
}
```

- [ ] **Step 2: Build; verify the ramp test FAILS** (first chunk is currently `GetChunkSize()` = 16KB). Expected: `EXPECT_EQ(..., 4096)` fails with actual 16384.

- [ ] **Step 3: Implement**

3a. Pool `Config` + accessors (asserts in ctor: `minChunkBytes >= MIN_CHUNK_SIZE`, `maxChunkBytes <= MAX_CHUNK_SIZE`, `minChunkBytes <= maxChunkBytes`, `growDivisor >= 1`):

```cpp
            size_t minChunkBytes = MIN_CHUNK_SIZE;    // 4KB — first/smallest chunk
            size_t maxChunkBytes = 512 * 1024;        // sizing cap (study: 512KB, not 1MB)
            size_t growDivisor = 2;                   // chunk ~ archetypeBytes / growDivisor
```
```cpp
        ASTRA_NODISCARD size_t GetMinChunkBytes() const noexcept { return m_config.minChunkBytes; }
        ASTRA_NODISCARD size_t GetMaxChunkBytes() const noexcept { return m_config.maxChunkBytes; }
        ASTRA_NODISCARD size_t GetGrowDivisor() const noexcept { return m_config.growDivisor; }
```

3b. `Archetype::NextChunkBytes()` (private, near `ComputeCapacityForBytes`):

```cpp
        // Grow-as-populate (spec section 4, Unit C): size each NEW chunk from the
        // archetype's current data footprint. Empty archetype -> minChunkBytes;
        // geometric ramp (~1.5x total per chunk at divisor 2) toward maxChunkBytes.
        // Zero-size archetypes keep the legacy fixed chunk size (entities live in
        // the side vector; there is nothing to ramp).
        ASTRA_NODISCARD size_t NextChunkBytes() const noexcept
        {
            if (!m_chunkPool)
                return ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
            if (m_perEntitySize == 0)
                return m_chunkPool->GetChunkSize();
            const size_t dataBytes = m_totalCapacity * m_perEntitySize;
            const size_t raw = dataBytes / m_chunkPool->GetGrowDivisor();
            return std::clamp(raw, m_chunkPool->GetMinChunkBytes(), m_chunkPool->GetMaxChunkBytes());
        }
```

3c. Swap every growth site Task 4 left on `GetChunkSize()` to `NextChunkBytes()`: `Initialize`'s first `AppendChunk`, the three batch `while (GetRemainingCapacity() < count)` loops (recompute `NextChunkBytes()` INSIDE the loop — sizes must ramp within one large batch), `GetOrCreateChunk`, `EnsureCapacity`'s `cap0` estimate. `Deserialize` does NOT use `NextChunkBytes` (exact-fit stays).

3d. `Deserialize` guards (833, 849): replace `componentPool->GetChunkSize()` with `componentPool->GetMaxChunkBytes()` (and the no-pool fallback `ArchetypeChunkPool::DEFAULT_CHUNK_SIZE` with `512 * 1024`), so saves written with large ramped chunks load; old 16KB-era saves still pass (smaller than the cap).

- [ ] **Step 4: Build + run** — new tests pass; FULL Debug suite green. Watch specifically: `SystemContextTest` lines 961-1183 assert `totalChunks >= 8` across archetypes (parallel scheduling shape) — ramped chunks hold MORE entities so chunk counts DROP. If those asserts fail, raise the entity counts in those tests (mechanically — the tests exercise multi-chunk parallelism, so they need "enough entities for >= 8 chunks under 4KB-first ramping"; a comment referencing this task).

- [ ] **Step 5: 3-config verify.**

- [ ] **Step 6: Commit**

```powershell
git add include/Astra tests/
git commit -m "feat(archetype): grow-as-populate chunk sizing - clamp(archetypeBytes/divisor, 4KB, 512KB), ramp within batches (Phase 2 Unit C part 2)"
```

---

### Task 6: Compaction-on-defrag — rebuild-style `CompactChunks` [OPUS recommended]

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (replace `ShouldCoalesce`+`CoalesceChunks` with `CompactChunks`)
- Modify: `include/Astra/Registry/Registry.hpp` (line 1159 call site)
- Test: `tests/Registry/RegistryTest.cpp` (append; uses `Astra::Test::Position` + `Astra::Test::Tracked` only)

**Interfaces:**
- Consumes: Tasks 4-5 (`AppendChunk` mechanics, `NextChunkBytes` clamps, per-chunk capacity, `MoveEntitiesBetweenChunks`-style column moves via `m_columnMeta`).
- Produces: `Archetype::CompactChunks() -> std::pair<size_t, std::vector<std::pair<Entity, EntityLocation>>>` — `{chunksFreed, newLocations}` where `newLocations` covers **every** live entity (rebuild semantics). `ShouldCoalesce`/`CoalesceChunks` are DELETED.

**Background — suspected pre-existing bug this task also fixes (verify at RED):** `CoalesceChunks` (Archetype.hpp:1081-1088) erases empty middle chunks, shifting every later chunk's index, but reports new locations only for entities it MOVED — entities in shifted chunks keep stale `chunkIndex` records (e.g. chunks `[half, sparse, half]`: sparse drains into chunk 0, chunk 1 is erased, chunk 2's entities' records still say index 2). Rebuild-style compaction reports every entity's location, making the class of bug impossible.

- [ ] **Step 1: Write the value-integrity RED test**

Append to `tests/Registry/RegistryTest.cpp` (mirror the file's fixture conventions):

```cpp
// Phase 2 Unit D. On pre-CompactChunks code this test EXPOSES the stale-
// chunkIndex defect in CoalesceChunks (middle-chunk erase shifts later chunks
// without updating their records). Run it at RED to confirm; CompactChunks
// makes it pass by reporting every live entity's new location.
TEST_F(RegistryTest, DefragmentPreservesEveryComponentValue)
{
    // Enough entities for several chunks under the 4KB-first ramp.
    constexpr uint32_t kCount = 8000;
    std::vector<Astra::Entity> entities;
    entities.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        entities.push_back(m_registry->CreateEntityWith(Astra::Test::Position{float(i), float(i * 2), float(i * 3)}));
    }

    // Punch a hole: destroy a contiguous creation-order band (lands in the
    // middle chunks) so fill ratio drops below the 0.5 trigger.
    for (uint32_t i = kCount / 4; i < (3 * kCount) / 4; ++i)
    {
        m_registry->DestroyEntity(entities[i]);
    }

    auto result = m_registry->Defragment();
    EXPECT_GT(result.entitiesMoved, 0u);

    // EVERY survivor must still resolve to ITS OWN component values.
    for (uint32_t i = 0; i < kCount / 4; ++i)
    {
        auto* p = m_registry->GetComponent<Astra::Test::Position>(entities[i]);
        ASSERT_NE(p, nullptr) << "entity " << i << " lost its component";
        EXPECT_EQ(p->x, float(i)) << "entity " << i << " resolves to another entity's data";
    }
    for (uint32_t i = (3 * kCount) / 4; i < kCount; ++i)
    {
        auto* p = m_registry->GetComponent<Astra::Test::Position>(entities[i]);
        ASSERT_NE(p, nullptr) << "entity " << i << " lost its component";
        EXPECT_EQ(p->x, float(i)) << "entity " << i << " resolves to another entity's data";
    }
}

// Move-only lifetime balance across compaction (mirrors Phase C's Tracked
// s_live guard): compaction must MoveConstruct+Destruct (or memcpy trivials),
// never duplicate or leak.
TEST_F(RegistryTest, DefragmentBalancesMoveOnlyLifetimes)
{
    const auto baseline = Astra::Test::Tracked::s_live;   // match s_live's declared type
    constexpr uint32_t kCount = 4000;
    std::vector<Astra::Entity> entities;
    entities.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        entities.push_back(m_registry->CreateEntityWith(Astra::Test::Tracked{int(i)}));
    }
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount);

    for (uint32_t i = 0; i < kCount; i += 2)   // half, spread across chunks
    {
        m_registry->DestroyEntity(entities[i]);
    }
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount / 2);

    m_registry->Defragment();
    EXPECT_EQ(Astra::Test::Tracked::s_live, baseline + kCount / 2) << "compaction leaked or double-destroyed";

    for (uint32_t i = 1; i < kCount; i += 2)
    {
        auto* t = m_registry->GetComponent<Astra::Test::Tracked>(entities[i]);
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->value, int(i));
    }
}
```

- [ ] **Step 2: Build + run at RED; RECORD the outcome**

```powershell
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=RegistryTest.DefragmentPreservesEveryComponentValue:RegistryTest.DefragmentBalancesMoveOnlyLifetimes
```
Expected: `DefragmentPreservesEveryComponentValue` FAILS (bug repro) — if it PASSES, investigate whether the destroy pattern triggered coalescing at all (the trigger needs `GetFragmentationLevel() >= 0.5`; adjust the destroyed band, not the assertions) and record findings in the task report either way.

- [ ] **Step 3: Implement `CompactChunks`, delete `ShouldCoalesce`/`CoalesceChunks`**

```cpp
        // Rebuild-style compaction (Phase 2 Unit D): repack every live entity
        // into fresh chunks sized for the CURRENT live count (the study formula
        // applied directly — N is known here), free all old chunks (TLSF
        // coalesces them), and report EVERY entity's new location so the caller
        // rewrites all records. Column moves memcpy whole runs for trivially
        // copyable columns and MoveConstruct+Destruct element-wise otherwise.
        std::pair<size_t, std::vector<std::pair<Entity, EntityLocation>>> CompactChunks()
        {
            std::vector<std::pair<Entity, EntityLocation>> newLocations;
            if (m_chunks.size() <= 1 || m_entityCount == 0)
                return {0, std::move(newLocations)};

            const size_t oldChunkCount = m_chunks.size();
            const size_t targetBytes = [this]
            {
                if (!m_chunkPool || m_perEntitySize == 0)
                    return m_chunkPool ? m_chunkPool->GetChunkSize() : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
                const size_t liveBytes = m_entityCount * m_perEntitySize;
                return std::clamp(liveBytes / m_chunkPool->GetGrowDivisor(),
                                  m_chunkPool->GetMinChunkBytes(), m_chunkPool->GetMaxChunkBytes());
            }();

            // Build the new chunk list on the side; on ANY allocation failure,
            // abort untouched (compaction is an optimization, not an obligation).
            std::vector<std::unique_ptr<ArchetypeChunk, ArchetypeChunkPool::ChunkDeleter>> newChunks;
            const size_t capacity = ComputeCapacityForBytes(targetBytes);
            if (capacity == 0) ASTRA_UNLIKELY
                return {0, std::move(newLocations)};
            const size_t chunkCountNeeded = (m_entityCount + capacity - 1) / capacity;
            newChunks.reserve(chunkCountNeeded);
            for (size_t i = 0; i < chunkCountNeeded; ++i)
            {
                auto chunk = m_chunkPool->CreateChunk(capacity, targetBytes, &m_columnMeta);
                if (!chunk) ASTRA_UNLIKELY
                    return {0, std::move(newLocations)};   // old chunks untouched
                newChunks.emplace_back(std::move(chunk));
            }

            newLocations.reserve(m_entityCount);
            size_t dstChunk = 0, dstIndex = 0;
            for (auto& src : m_chunks)
            {
                const size_t srcCount = src->GetCount();
                size_t srcIndex = 0;
                while (srcIndex < srcCount)
                {
                    if (dstIndex == capacity) { ++dstChunk; dstIndex = 0; }
                    auto& dst = newChunks[dstChunk];
                    const size_t run = std::min(srcCount - srcIndex, capacity - dstIndex);

                    // Entities: bulk-append the run.
                    auto& srcEntities = src->GetEntities();
                    auto& dstEntities = dst->GetEntities();
                    for (size_t k = 0; k < run; ++k)
                    {
                        const Entity e = srcEntities[srcIndex + k];
                        dstEntities.push_back(e);
                        newLocations.emplace_back(e, EntityLocation::Create(dstChunk, dstIndex + k));
                    }

                    // Components: per column, memcpy the whole run when trivially
                    // copyable, else per-element MoveConstruct + Destruct.
                    for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
                    {
                        const ComponentID id = m_columnMeta.columns[c].id;
                        const uint32_t stride = m_columnMeta.columns[c].stride;
                        const ComponentDescriptor& desc = *m_columnMeta.columns[c].descriptor;
                        std::byte* srcPtr = static_cast<std::byte*>(src->GetComponentArrayByID(id)) + srcIndex * stride;
                        std::byte* dstPtr = static_cast<std::byte*>(dst->GetComponentArrayByID(id)) + dstIndex * stride;
                        if (desc.is_trivially_copyable)
                        {
                            std::memcpy(dstPtr, srcPtr, run * static_cast<size_t>(stride));
                        }
                        else
                        {
                            for (size_t k = 0; k < run; ++k)
                            {
                                desc.MoveConstruct(dstPtr + k * stride, srcPtr + k * stride);
                                desc.Destruct(srcPtr + k * stride);
                            }
                        }
                    }

                    dst->SetCount(dst->GetCount() + run);
                    srcIndex += run;
                    dstIndex += run;
                }
                // Every element was moved out (trivial columns need no Destruct;
                // complex ones were destructed above): make the chunk inert so its
                // destructor doesn't re-destruct moved-from slots.
                src->GetEntities().clear();
                src->SetCount(0);
            }

            m_chunks = std::move(newChunks);   // old chunks free here -> TLSF coalesces
            m_totalCapacity = chunkCountNeeded * capacity;
            m_firstNonFullChunkIndex = m_chunks.empty() ? 0 : m_chunks.size() - 1;

            const size_t freed = oldChunkCount > m_chunks.size() ? oldChunkCount - m_chunks.size() : 0;
            return {freed, std::move(newLocations)};
        }
```
Delete `ShouldCoalesce` (982-996) and `CoalesceChunks` (998-1091) entirely. Add `#include <cstring>` if Archetype.hpp lacks it.

- [ ] **Step 4: Rewire `Registry::Defragment` (1159)**

```cpp
                    auto [chunksFreed, movedEntities] = arch->CompactChunks();
```
(The surrounding loop — `SetEntityLocation` per pair, counters, incremental budget — stays byte-for-byte; `movedEntities` now covers every live entity of the archetype, which the existing loop already handles.) The gate above it (1154-1156) keeps working: Task 4 made `GetFragmentationLevel()` fill-based, so `archFragmentation >= 1 - chunkUtilizationThreshold` IS the spec's `fill < 0.5` trigger.

- [ ] **Step 5: Grep for orphaned callers** — `grep -rn "CoalesceChunks\|ShouldCoalesce" include/ tests/` must return nothing (fix any test that exercised the old API to use `Defragment()`-level behavior; the files found at planning time: tests/Comprehensive/{ComponentLifecycle,MemoryCleanup,ResourceExhaustion}Test.cpp, tests/Registry/{ArchetypeManager,Archetype,Registry,ViewInvalidation}Test.cpp mention `Defragment` — most call the Registry API, which is unchanged).

- [ ] **Step 6: Build + run at GREEN**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: FULL Debug suite green including both new tests (the RED test now passes).

- [ ] **Step 7: 3-config verify.**

- [ ] **Step 8: Commit**

```powershell
git add include/Astra tests/
git commit -m "feat(archetype): rebuild-style CompactChunks replaces CoalesceChunks - every-entity location report fixes stale-chunkIndex defect, TLSF coalescing reclaims freed chunks (Phase 2 Unit D)"
```

---

### Task 7: Validation — 3-config, re-sweep, 3-way bench, memory-waste check, docs

**Files:**
- Modify: `bench-compare/bench_sweep.cpp` (sweep `growDivisor`/`maxChunkBytes` instead of fixed `chunkSize`), `bench-compare/RESULTS.md`
- Modify: `docs/superpowers/specs/2026-07-23-phase-2-dynamic-chunk-sizing-design.md` (record measured outcomes in a short "Results" addendum)

**Interfaces:** consumes the finished branch; produces the evidence for the merge decision.

- [ ] **Step 1: Authoritative 3-config suite on the final commit** — MSBuild + full AstraTest for Debug/Release/Dist. Record exact counts (baseline 697/695/695 + new tests). ALL green or STOP and fix.

- [ ] **Step 2: Rebuild + run the 3-way benchmark**

```powershell
cd bench-compare
cmd /c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /DNDEBUG /EHsc /nologo /I..\include /I..\vendor\Mosaic\include bench_astra.cpp advapi32.lib'
# run each exe 5x from bench-compare/, take medians:
.\bench_astra.exe; .\bench_astra.exe; .\bench_astra.exe; .\bench_astra.exe; .\bench_astra.exe
.\bench_flecs.exe; .\bench_flecs.exe
.\bench_entt.exe; .\bench_entt.exe
```
Success criteria (vs the clean baseline: create 51.4 / add 57.1 / remove 40.6 / random_get 65.4 / iterate1 0.509 / iterate2 0.993 / iterate3 1.050; flecs 95.9 / 54.4 / 34.1 / 57.0 / 0.390 / 0.812 / 0.889):
- iterate1, iterate2, random_get: **at or ahead of flecs** (study predicted +51%/+15%/+16%).
- create: stays ahead of flecs; add/remove/iterate3: no regression beyond noise.
- Record medians in `bench-compare/RESULTS.md` with a Phase-2 section. If a metric misses, measure twice more before concluding; report honestly either way.

- [ ] **Step 3: Divisor mini-sweep** — adapt `bench_sweep.cpp` to sweep `Config.growDivisor ∈ {1,2,4}` × `maxChunkBytes ∈ {256KB,512KB}` at N ∈ {1e3,1e5,1e6} for the 7 ops; confirm divisor 2 + 512KB cap remain optimal (lock or amend the NSDMIs with data; note the outcome in RESULTS.md).

- [ ] **Step 4: Memory-waste check** — small-archetype footprint via a quick test or bench probe: a 100-entity Position archetype must hold exactly ONE 4KB chunk (`GetArchetypeMemoryUsage` ≈ 4KB + fixed overhead, vs 16KB before). Assert in a test if not already covered by Task 5's `SmallArchetypeStaysAtMinimumChunk`.

- [ ] **Step 5: Docs + commit**

```powershell
git add bench-compare/RESULTS.md docs/
git commit -m "perf(bench): Phase 2 dynamic chunk sizing results - 3-way bench, divisor sweep, memory-waste check"
```

- [ ] **Step 6: Hand off** — request the final whole-branch review (opus) per subagent-driven-development; merge to dev is user-gated (local FF, delete branch, don't push — the established finish protocol).

---

## Self-Review Notes (already applied)

1. **Spec coverage:** Unit A → Tasks 1-2; Unit B → Task 3; Unit C → Tasks 4-5; Unit D → Task 6; validation section → Task 7; spec's "drop pow2" → Task 4; "ramp within batches" → Task 5 Step 3c; "on-disk format unchanged + exact-fit load" → Task 4 Step 3f; alignment contract → Task 1 size-congruence + `Validate()`.
2. **Known-risk callouts for reviewers:** Task 4 Step 3f (serialization `maxChunkEntityCount` semantics vs old readers — forward-compat not promised, backward verified by round-trip suites); Task 6's abort-on-OOM leaves old chunks untouched (no partial state); pool-before-archetypes member order unchanged (Task 3).
3. **Type consistency check:** `CreateChunk(capacity, chunkBytes, meta)` (3-arg) is introduced in Task 4 and used by Tasks 4-6; Tasks 1-3 use the legacy 2-arg form. `GetTotalCapacity`/`GetChunkBytes`/`NextChunkBytes`/`CompactChunks` names match across Tasks 4-7.
