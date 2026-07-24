#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

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
    // Why that works: headers are boundary tags placed kStartOffset bytes before
    // their payload, and the next header sits at payload + size - kOverhead, so
    //     payload_{n+1} = payload_n + size_n + kOverhead.
    // With every size == ALIGN_SIZE - kOverhead (mod ALIGN_SIZE) the stride
    // size_n + kOverhead is always a multiple of ALIGN_SIZE, so once the first
    // payload of an arena is 64B aligned every payload in it is, forever. The
    // rule is closed under split (remainder = size - request - kOverhead) and
    // under merge (size + other + kOverhead). Validate() enforces it.
    //
    // Tlsf manages caller-provided memory regions (arenas). It never touches
    // OS memory itself: the owner (ArchetypeChunkPool) acquires huge-page
    // arenas via AllocateMemory and registers them with AddArena.
    //
    // Not thread-safe; the owner serializes access.
    //
    // GOOD-FIT, NOT BEST-FIT (inherited from the reference -- callers must know):
    // MappingSearch rounds a request up to the top of its second-level bucket so
    // that ANY block in the located list is guaranteed big enough, which is what
    // makes the search O(1). The cost is that a free block whose size is not
    // exactly on a bucket boundary is NOT reachable by a request of that same
    // size -- the search starts one bucket higher. Under the size-congruence rule
    // above, sizes are always 56 (mod 64) while bucket boundaries are multiples of
    // 2^(fl+5) (>= 64), so for blocks >= SMALL_BLOCK_SIZE that is EVERY block:
    // an isolated free block of size S would never satisfy a later request for S.
    // That is fatal for an owner that recycles a handful of fixed sizes (the
    // chunk pool), so Allocate adds an EXACT-FIT PEEK on top of the reference: it
    // first checks the head of the bucket the request would itself be FILED in,
    // and takes it when it is big enough. One extra load, O(1), and the size test
    // keeps it safe. Same-size reuse therefore works; what remains good-fit (and
    // may still skip a usable block) is only the non-exact case, where the search
    // can carve fresh bytes rather than reuse a slightly larger stranded hole.
    // Defragmentation / arena release remain the answer for that residue.
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
        // Largest servable request. Half of kMaxArenaBytes on purpose: it keeps
        // the rounded search size (request + up to one bucket width) inside
        // FL_INDEX_COUNT, so SearchSuitable can never be handed an out-of-range
        // first-level index. Requests above it return nullptr.
        static constexpr size_t kMaxRequest = size_t(1) << (FL_INDEX_MAX - 1);          // 32MB
        static constexpr size_t kMinArenaBytes = 4096;
        static constexpr size_t kMaxArenaBytes = size_t(1) << FL_INDEX_MAX;             // 64MB

        static_assert(sizeof(void*) == 8, "Tlsf assumes a 64-bit pointer / size_t model");
        static_assert(kStartOffset == 16, "Tlsf block header prologue must be 16 bytes");
        static_assert(kMinBlock % ALIGN_SIZE == ALIGN_SIZE - kOverhead, "kMinBlock must satisfy the size-congruence law");

        struct ArenaInfo
        {
            void* base;
            size_t bytes;
            BlockHeader* first;
            size_t mainSize;   // the fully-free block size; equality means "arena fully free"
        };

    public:
        // Arena registration bounds. Exposed so an owner can size the regions it
        // hands to AddArena without duplicating the limit; AddArena refuses
        // anything outside [MIN_ARENA_BYTES, MAX_ARENA_BYTES].
        static constexpr size_t MIN_ARENA_BYTES = kMinArenaBytes;
        static constexpr size_t MAX_ARENA_BYTES = kMaxArenaBytes;

        // Largest single request Allocate can service (see kMaxRequest's
        // comment above the class for why it is half of MAX_ARENA_BYTES, not
        // equal to it). Exposed so an owner sizing a request against "the
        // biggest thing TLSF can hand back" checks against the real ceiling
        // instead of MAX_ARENA_BYTES -- a request in (MAX_REQUEST_BYTES,
        // MAX_ARENA_BYTES] would pass an arena-size check yet still be
        // refused by Allocate.
        static constexpr size_t MAX_REQUEST_BYTES = kMaxRequest;

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
            BlockHeader* block = nullptr;

            // Exact-fit peek. MappingSearch rounds up to guarantee any block in
            // the located list fits, which makes it skip the bucket a same-size
            // block is filed in (our sizes are never on a bucket boundary -- see
            // the header note on the congruence rule). Checking that bucket's
            // head first restores same-size reuse for the common case at the cost
            // of one load; the size test keeps it safe when the head is smaller.
            MappingInsert(adjusted, fl, sl);
            BlockHeader* head = m_blocks[fl][sl];
            if (head && BlockSize(head) >= adjusted)
            {
                // fl/sl already identify the bucket head is filed under, by
                // definition of being that bucket's list head.
                block = head;
            }
            else
            {
                MappingSearch(adjusted, fl, sl);
                block = SearchSuitable(fl, sl);
            }
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
            ASTRA_ASSERT(mem != nullptr, "TLSF: null arena");
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(mem) & (ALIGN_SIZE - 1)) == 0,
                         "TLSF: arena base must be 64B-aligned");
            if (!mem || (reinterpret_cast<uintptr_t>(mem) & (ALIGN_SIZE - 1)) != 0) ASTRA_UNLIKELY
                return false;
            if (bytes < kMinArenaBytes || bytes > kMaxArenaBytes) ASTRA_UNLIKELY
                return false;

            // First payload at base+64; its header starts kStartOffset before it.
            // Main size: largest value == 56 (mod 64) leaving room for the front
            // pad (64) and the 16-byte sentinel prologue behind it, i.e.
            // ((bytes-128) & ~63) + 56, which is always <= bytes - 72.
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
        // The callback MUST NOT mutate the arena set: it iterates m_arenas by
        // reference, so calling AddArena / RemoveArena from fn invalidates the
        // iteration. Collect the bases first, then act on them after returning.
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

        // Test/debug integrity walk. Two independent passes that must agree:
        //   1. physical walk of every arena -- boundary tags, the 56-mod-64 size
        //      law, payload alignment, prev-free consistency, full coalescing,
        //      sentinel placement, in-bounds addresses;
        //   2. logical walk of every free list -- bitmap/head coherence, doubly
        //      linked list integrity, each listed block free and filed under the
        //      bucket its size maps to.
        // Then free block count and byte total from both passes must match each
        // other and m_freeBytes. Not for hot paths.
        ASTRA_NODISCARD bool Validate() const noexcept
        {
            size_t freeBytesSeen = 0;
            size_t freeCountSeen = 0;
            for (const ArenaInfo& a : m_arenas)
            {
                const std::byte* arenaBegin = static_cast<const std::byte*>(a.base);
                const std::byte* arenaEnd = arenaBegin + a.bytes;
                // The first block must sit exactly where AddArena puts it, and
                // nothing physically precedes it, so its prev-free bit is clear.
                if (reinterpret_cast<const std::byte*>(a.first) != arenaBegin + ALIGN_SIZE - kStartOffset) return false;
                if (IsPrevFree(a.first)) return false;
                const BlockHeader* b = a.first;
                bool prevFree = false;
                for (;;)
                {
                    // Bounds first: a corrupt size must not walk us off the arena.
                    const std::byte* raw = reinterpret_cast<const std::byte*>(b);
                    if (raw < arenaBegin || raw + kStartOffset > arenaEnd) return false;

                    const size_t size = BlockSize(b);
                    if (size == 0)   // sentinel
                    {
                        if (IsFree(b)) return false;
                        if (IsPrevFree(b) != prevFree) return false;
                        break;
                    }
                    if (size % ALIGN_SIZE != ALIGN_SIZE - kOverhead) return false;
                    if (size < kMinBlock) return false;
                    if (IsPrevFree(b) != prevFree) return false;
                    if (IsFree(b) && prevFree) return false;   // adjacent frees must have merged
                    if ((reinterpret_cast<uintptr_t>(raw) + kStartOffset) % ALIGN_SIZE != 0) return false;
                    if (IsFree(b))
                    {
                        freeBytesSeen += size;
                        ++freeCountSeen;
                    }
                    // Bounds- and monotonicity-check next BEFORE dereferencing
                    // it: a corrupt-but-congruent size must not fault the
                    // integrity checker (the loop-top check comes too late).
                    const BlockHeader* next = NextBlockConst(b);
                    const std::byte* nextRaw = reinterpret_cast<const std::byte*>(next);
                    if (nextRaw <= raw) return false;
                    if (nextRaw < arenaBegin || nextRaw + kStartOffset > arenaEnd) return false;
                    if (IsFree(b) && next->prevPhys != b) return false;
                    prevFree = IsFree(b);
                    b = next;
                }
            }

            size_t listBytes = 0;
            size_t listCount = 0;
            for (int fl = 0; fl < static_cast<int>(FL_INDEX_COUNT); ++fl)
            {
                const bool flSet = (m_flBitmap & (uint32_t(1) << fl)) != 0;
                if ((m_slBitmap[fl] != 0) != flSet) return false;
                for (int sl = 0; sl < static_cast<int>(SL_INDEX_COUNT); ++sl)
                {
                    const bool slSet = (m_slBitmap[fl] & (uint32_t(1) << sl)) != 0;
                    const BlockHeader* head = m_blocks[fl][sl];
                    if (slSet != (head != nullptr)) return false;

                    const BlockHeader* prev = nullptr;
                    for (const BlockHeader* b = head; b != nullptr; b = b->nextFree)
                    {
                        if (b->prevFree != prev) return false;
                        if (!IsFree(b)) return false;
                        int mappedFl = 0, mappedSl = 0;
                        MappingInsert(BlockSize(b), mappedFl, mappedSl);
                        if (mappedFl != fl || mappedSl != sl) return false;
                        listBytes += BlockSize(b);
                        ++listCount;
                        prev = b;
                    }
                }
            }

            return freeBytesSeen == m_freeBytes && listBytes == m_freeBytes && listCount == freeCountSeen;
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
        // Rounds a user request up to the next size satisfying the congruence
        // law. Returns 0 for "cannot serve" (zero, or larger than any block the
        // two-level index can address) -- checked BEFORE the rounding arithmetic
        // so a huge request cannot wrap around into a tiny block size.
        ASTRA_NODISCARD static size_t AdjustRequestSize(size_t bytes) noexcept
        {
            if (bytes == 0 || bytes > kMaxRequest) ASTRA_UNLIKELY
                return 0;
            const size_t aligned = ((bytes + kOverhead + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1)) - kOverhead;
            return aligned < kMinBlock ? kMinBlock : aligned;
        }

        ASTRA_NODISCARD static int Fls(size_t v) noexcept { return Mosaic::Bits::FindLastSet(v) - 1; }
        // static_cast keeps the conditional's common type signed, so the -1
        // "no bit set" sentinel cannot round-trip through an unsigned type.
        ASTRA_NODISCARD static int Ffs(uint32_t v) noexcept { return v ? static_cast<int>(Mosaic::Bits::CountTrailingZeros(v)) : -1; }

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
                sl = static_cast<int>((size >> (f - static_cast<int>(SL_INDEX_COUNT_LOG2))) ^ (size_t(1) << SL_INDEX_COUNT_LOG2));
                fl = f - (static_cast<int>(FL_INDEX_SHIFT) - 1);
            }
            ASTRA_ASSERT(fl >= 0 && sl >= 0 && sl < static_cast<int>(SL_INDEX_COUNT), "TLSF: mapping out of range");
        }

        static void MappingSearch(size_t size, int& fl, int& sl) noexcept
        {
            if (size >= SMALL_BLOCK_SIZE)
            {
                const size_t round = (size_t(1) << (Fls(size) - static_cast<int>(SL_INDEX_COUNT_LOG2))) - 1;
                size += round;
            }
            MappingInsert(size, fl, sl);
        }

        // ---- free lists -------------------------------------------------------
        void InsertFreeBlock(BlockHeader* block, int fl, int sl) noexcept
        {
            ASTRA_ASSERT(fl >= 0 && fl < static_cast<int>(FL_INDEX_COUNT) && sl >= 0 && sl < static_cast<int>(SL_INDEX_COUNT),
                         "TLSF: free-list index out of range on insert");
            // Unreachable today, but asserts compile out in shipping builds and
            // this is a cold path: refuse rather than write outside m_blocks.
            if (fl < 0 || fl >= static_cast<int>(FL_INDEX_COUNT) || sl < 0 || sl >= static_cast<int>(SL_INDEX_COUNT)) ASTRA_UNLIKELY
                return;
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
            ASTRA_ASSERT(fl >= 0 && fl < static_cast<int>(FL_INDEX_COUNT) && sl >= 0 && sl < static_cast<int>(SL_INDEX_COUNT),
                         "TLSF: free-list index out of range on remove");
            ASTRA_ASSERT(m_freeBytes >= BlockSize(block), "TLSF: free-byte accounting underflow");
            // See InsertFreeBlock: cold-path guard for shipping builds.
            if (fl < 0 || fl >= static_cast<int>(FL_INDEX_COUNT) || sl < 0 || sl >= static_cast<int>(SL_INDEX_COUNT)) ASTRA_UNLIKELY
                return;
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
                ASTRA_ASSERT(slMap != 0, "TLSF: second-level bitmap desynced from first level");
                if (!slMap) ASTRA_UNLIKELY
                    return nullptr;
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
                ASTRA_ASSERT(prev != nullptr && IsFree(prev), "TLSF: prev-free bit set but prev block is not free");
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
