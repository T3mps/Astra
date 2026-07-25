# Commands Subsystem Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Kill Commands C1 (byte-buffer bit-relocation of non-trivial components) by construction with a stable segmented TLSF-backed arena, erase C2 by deleting the vestigial merge path, surface C3 batch per-entity failures, and close the review's guards/dead-code/test gaps.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-24-commands-hardening-design.md` (user-approved; §10 invariants govern). New `CommandBlockArena` (Registry-owned, dedicated `Tlsf`, mutex on acquire/release only); `CommandByteBuffer` becomes a chain of stable blocks (bump-pointer, no-straddle, retained across `Clear()`); sort keys hold stable command pointers; `MergeFrom`/`MergeInto`/`ParallelCommandBuffer::Execute()` deleted; batch executors count and report per-entity failures.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln`), GoogleTest.

## Global Constraints

- Branch: `fix/commands-hardening` off dev HEAD (record SHA in the ledger). Local only — NEVER push. Finish = opus whole-branch review → fix wave if needed → authoritative 3-config → **confirm with user** → FF-merge local, delete branch.
- Build (PowerShell, whole solution): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<CFG> -p:Platform=x64 -m`. Tests: `.\bin\<CFG>-windows-x86_64\AstraTest\AstraTest.exe`. Baseline (dev @ `220903b`): **Debug 742 / Release 740 / Dist 740** — re-verify before Task 1. Expected deltas: Task 1 +0, Task 2 +4, Task 3 +2 (deltas govern over absolutes). A `[critical] ... Cycle detected in parent-child relationships` stderr line during the suite is a known expected emission. Stale PDB → `taskkill /F /IM mspdbsrv.exe`. IDE clang diagnostics are false positives.
- **No bench checkpoint** (spec §9): correctness branch; Commands ops are not in the bench matrix. Any step that would touch a measured hot path must say so in its report.
- **TypeID budget: exactly ONE new test component type (`RelocationCanary`, Task 2)**; every other test reuses `Astra::Test::*` / existing types.
- **Spec §10 invariants (binding):** (1) recorded command bytes are immutable + address-stable until Clear/destruction/consumption; (2) every non-trivial payload's destructor thunk runs exactly once, including rollback and mid-flush failure paths; (3) commands never straddle blocks; walks use `CommandHeader::totalSize` + per-block `used`; (4) `Clear()` retains blocks (steady state = zero arena calls); (5) `ExecuteSorted()` is the only parallel flush, determinism contract unchanged; (6) the Registry-owned arena is the only block source, its Tlsf never shared with the chunk pool, its mutex never taken on the record fast path; (7) one new test component type only.
- Model recipe (SDD): sonnet Task 1 + Task 3; **OPUS Task 2** (raw-memory storage rewrite) and the final whole-branch review.
- Watch-item: `CommandBlockArena` holds a `std::mutex`, making `Registry` definitively non-movable. That matches the documented W1-era intent ("Registry non-movable so stable"), but if any code turns out to move-assign `Registry` itself, the build will break loudly — STOP and report, don't work around.

---

### Task 1: Subtractive pass + compile-time guards

**Files:**
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (deletions at :33-46, :951-989, :1648-1673, :1823-1835; static_asserts at :415, :493, :696 vicinity; comments :56-61, :75, :842, :1555, :1592-1595)
- Modify: `include/Astra/Commands/Command.hpp` ONLY if the dead `Calculate*Size` functions live there (grep first)
- Test: `tests/Commands/CommandBufferTest.cpp:192-215` (convert PCB test to `ExecuteSorted`)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: `MergeFrom`/`MergeInto`/`ParallelCommandBuffer::Execute()` no longer exist — Task 2's rewrite must not port them. Tests may rely on the new static_asserts existing.

- [ ] **Step 1: Convert the PCB test to the deterministic flush (behavior-preserving; must PASS before and after this task).**

In `tests/Commands/CommandBufferTest.cpp:192-215`, `TEST_F(CommandBufferTest, ParallelCommandBuffer)`: replace the line

```cpp
    parallelBuffer.Execute();
```

with

```cpp
    ASSERT_TRUE(parallelBuffer.ExecuteSorted().IsOk());
```

(Everything else in the test stays. The per-worker buffers are deferred-mode, `ExecuteSorted` resolves their placeholders — same observable result.)

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=CommandBufferTest.ParallelCommandBuffer` (build Debug first)
Expected: PASS.

- [ ] **Step 2: Delete the four dead/vestigial API surfaces.** Read each region in full first — line numbers may have drifted a few lines:

1. `CommandBuffer::MergeFrom` (`CommandBuffer.hpp:951-989`, including its doc comment).
2. `ParallelCommandBuffer::MergeInto` (`CommandBuffer.hpp:1823-1835`, including doc comment).
3. `ParallelCommandBuffer::Execute()` (`CommandBuffer.hpp:1648-1673`, including doc comment). Update the `ParallelCommandBuffer` class doc comment (`:1592-1595`) — it says commands "are executed sequentially when Execute() is called"; now: "flushed deterministically via ExecuteSorted()".
4. `ExecutionResult` struct (`CommandBuffer.hpp`, the struct whose members appear at :33-46 — read up to find its opening brace/doc comment).
5. The never-called `CalculateAddComponentSize` / `CalculateAddComponentBatchSize` / `CalculateSetResourceSize` free functions. Locate first:

```bash
grep -rn "CalculateAddComponentSize\|CalculateAddComponentBatchSize\|CalculateSetResourceSize" include tests
```

Expected: definitions only, zero callers. Delete the definitions. If ANY caller appears, STOP and report (the review found none).

- [ ] **Step 3: Add the compile-time guards.**

In `CommandBuffer.hpp`, immediately after the existing alignment `static_assert` in each of `AddComponent` (:415), `AddComponents` (:493), `SetResource` (:696) add:

```cpp
            static_assert(sizeof(DecayedT) <= 0xFFFF,
                "CommandBuffer encodes payload size as uint16_t; components/resources larger "
                "than 65535 bytes must go through Registry directly");
```

(For `SetResource`, if its local type alias isn't `DecayedT`, match the alias that file position actually uses — read it.)

Inside `CommandByteBuffer` right after `ALIGNMENT` (:61):

```cpp
        static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= ALIGNMENT,
            "CommandByteBuffer requires operator-new alignment >= 16 (32-bit targets are unsupported)");
```

- [ ] **Step 4: Fix the stale comments.** `:75` "Align the size to 8 bytes" → "Align the size to ALIGNMENT (16) bytes"; the two "(buffer allocates with 8-byte alignment)" comments at `:842` and `:1555` → "(commands are stored at ALIGNMENT-byte stride)".

- [ ] **Step 5: Full 3-config build + suite.**

Expected: **Debug 742 / Release 740 / Dist 740** (flat — one test converted, none added/removed), all pass.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Commands/CommandBuffer.hpp tests/Commands/CommandBufferTest.cpp
# plus Command.hpp if Step 2.5 touched it
git commit -m "fix(commands): delete vestigial MergeFrom/MergeInto and nondeterministic PCB Execute, add size/alignment static_asserts, drop dead code"
```

---

### Task 2: C1 — CommandBlockArena + stable segmented CommandByteBuffer (OPUS)

**Files:**
- Create: `include/Astra/Commands/CommandBlockArena.hpp`
- Modify: `include/Astra/Registry/Registry.hpp` (include + member + accessor)
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (`CommandByteBuffer` rewrite :52-115; `CommandBuffer` ctor :159-164; `Execute` walk :794-857; `CommandKeys`/`ApplyCommandAt`/`ResolveAndApplyCommandAt` :866-923; `StampCommand` :1040-1054; `CleanupPendingCommands` :1511-1558; `m_commandKeys` decl :1583; `ParallelCommandBuffer::ExecuteSorted` Item plumbing :1724-1759)
- Modify: `tests/TestComponents.hpp` (add `RelocationCanary` after `Tracked`, ~:363)
- Test: `tests/Commands/CommandBufferTest.cpp` (4 new tests)

**Interfaces:**
- Consumes: Task 1's deletions (no MergeFrom/PCB-Execute to port). `Astra::Tlsf` (`Core/Tlsf.hpp`): `void* Allocate(size_t) noexcept`, `void Free(void*) noexcept`, `bool AddArena(void* mem, size_t bytes)` (64B-aligned mem), `MIN_ARENA_BYTES`/`MAX_ARENA_BYTES`/`MAX_REQUEST_BYTES`. `AllocateMemory(size, align, flags)` / `FreeMemory(ptr, size, usedHugePages)` from `Core/Memory.hpp` (mirror `ArchetypeChunkPool::GrowArena`, `ArchetypeChunkPool.hpp:814-860`, minus huge pages).
- Produces: `CommandBlockArena` with `struct BlockAlloc { std::byte* ptr; size_t bytes; }`, `BlockAlloc Acquire(size_t minBytes)`, `void Release(void* ptr)`; `Registry::GetCommandBlockArena() noexcept -> CommandBlockArena&`; `CommandBuffer::GetStorageBlockCount() const noexcept -> size_t`; `CommandKeys() -> const std::vector<std::pair<SortKey, std::byte*>>&`; `ApplyCommandAt(std::byte*)`, `ResolveAndApplyCommandAt(std::byte*, PlaceholderMap&)`. Task 3 depends only on unchanged executor signatures.

- [ ] **Step 1: Add `RelocationCanary` to `tests/TestComponents.hpp`** (after `Tracked`, inside `namespace Astra::Test`):

```cpp
    // 18. Relocation canary (Commands C1 regression guard): self-referential
    //     invariant self == &tag. A bitwise relocation of the containing buffer
    //     preserves `self` but moves `tag`, so any properly-run copy/move ctor
    //     afterwards sees src.self != &src.tag and counts the violation.
    //     Pointer COMPARISON only -- never dereferenced: no UB in the detection.
    struct RelocationCanary
    {
        static inline int s_violations = 0;
        static inline int s_live = 0;
        int value = 0;
        char tag = 0;
        char* self = &tag;

        RelocationCanary() { ++s_live; }
        explicit RelocationCanary(int v) : value(v) { ++s_live; }
        RelocationCanary(const RelocationCanary& o) : value(o.value)
        { s_violations += (o.self != &o.tag) ? 1 : 0; ++s_live; }
        RelocationCanary(RelocationCanary&& o) noexcept : value(o.value)
        { s_violations += (o.self != &o.tag) ? 1 : 0; ++s_live; }
        RelocationCanary& operator=(const RelocationCanary& o)
        { s_violations += (o.self != &o.tag) ? 1 : 0; value = o.value; return *this; }
        RelocationCanary& operator=(RelocationCanary&& o) noexcept
        { s_violations += (o.self != &o.tag) ? 1 : 0; value = o.value; return *this; }
        ~RelocationCanary() { --s_live; }

        // Mirror Tracked's Serialize rationale (RegisterComponents odr-instantiation).
        template<typename Archive>
        void Serialize(Archive& ar) { /* copy the body shape Tracked uses, for `value` */ }
    };
```

(Copy `Tracked`'s actual `Serialize` body idiom for the `value` field — read it at `tests/TestComponents.hpp:298+`. `tag`/`self` are NOT serialized.)

- [ ] **Step 2: Write the C1 RED test** in `tests/Commands/CommandBufferTest.cpp` (compiles against CURRENT code — do not reference any new API in this test):

```cpp
TEST_F(CommandBufferTest, NonTrivialComponentsSurviveBufferGrowth)
{
    using Astra::Test::RelocationCanary;
    RelocationCanary::s_violations = 0;
    const int liveBase = RelocationCanary::s_live;

    // ~64 encoded bytes per AddComponent command: 200 of them blow far past
    // the 4096-byte initial capacity, forcing storage growth mid-recording.
    constexpr int kCount = 200;
    std::vector<Entity> ents;
    ents.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        Entity e = cmdBuffer->CreateEntity();
        cmdBuffer->AddComponent(e, RelocationCanary{i});
        ents.push_back(e);
    }
    ASSERT_TRUE(cmdBuffer->Execute().IsOk());

    EXPECT_EQ(RelocationCanary::s_violations, 0)
        << "recorded component objects were bitwise-relocated by buffer growth (C1)";
    // Buffer payloads destructed by the post-Execute clear; archetype copies live.
    EXPECT_EQ(RelocationCanary::s_live, liveBase + kCount);
    for (int i = 0; i < kCount; ++i)
    {
        auto* c = registry->GetComponent<RelocationCanary>(ents[i]);
        ASSERT_NE(c, nullptr) << "i=" << i;
        EXPECT_EQ(c->value, i);
    }
    for (Entity e : ents) registry->DestroyEntity(e);
    EXPECT_EQ(RelocationCanary::s_live, liveBase);  // destructor thunk exactly-once overall
}
```

- [ ] **Step 3: Run it — expected FAIL (RED).**

Run: `.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=CommandBufferTest.NonTrivialComponentsSurviveBufferGrowth`
Expected: FAIL at the `s_violations == 0` assertion (vector reallocation bit-copied live canaries). Record the failure output in your report. Do NOT commit yet.

- [ ] **Step 4: Create `include/Astra/Commands/CommandBlockArena.hpp`:**

```cpp
#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Tlsf.hpp"

namespace Astra
{
    /**
     * Block source for CommandByteBuffer's stable segmented storage (one per
     * Registry). Hands out raw byte blocks that NEVER move until released --
     * the property the Commands C1 fix rests on.
     *
     * Threading: Acquire/Release serialize on m_mutex (per-worker
     * CommandBuffers may grow concurrently during parallel recording). The
     * mutex is never taken on the recording fast path: buffers retain their
     * blocks across Clear(), so steady-state frames make no arena calls.
     *
     * The Tlsf instance is dedicated to command blocks -- NEVER the archetype
     * chunk pool's (recording growth must not contend with structural churn).
     * Tlsf payloads are 64-byte aligned by its size-congruence law.
     */
    class CommandBlockArena
    {
    public:
        struct BlockAlloc
        {
            std::byte* ptr = nullptr;
            size_t bytes = 0;   // usable capacity (== the requested size)
        };

        CommandBlockArena() = default;
        CommandBlockArena(const CommandBlockArena&) = delete;
        CommandBlockArena& operator=(const CommandBlockArena&) = delete;

        ~CommandBlockArena()
        {
            // Command buffers must not outlive their Registry, so every block
            // is back by now; regions return to the OS wholesale.
            for (const auto& region : m_regions)
            {
                FreeMemory(region.base, region.bytes, /*usedHugePages=*/false);
            }
        }

        BlockAlloc Acquire(size_t minBytes)
        {
            std::lock_guard lock(m_mutex);
            void* p = m_tlsf.Allocate(minBytes);
            if (!p) ASTRA_UNLIKELY
            {
                if (!GrowArena(minBytes)) ASTRA_UNLIKELY
                    return {};
                p = m_tlsf.Allocate(minBytes);
                if (!p) ASTRA_UNLIKELY
                    return {};
            }
            return {static_cast<std::byte*>(p), minBytes};
        }

        void Release(void* ptr)
        {
            if (!ptr)
                return;
            std::lock_guard lock(m_mutex);
            m_tlsf.Free(ptr);
        }

    private:
        // 256KB per OS region: several worker buffers' full block ramps fit in
        // one region; tiny beside the chunk pool's arenas. Ordinary pages.
        static constexpr size_t kArenaBytes = 256 * 1024;

        struct Region
        {
            void* base;
            size_t bytes;
        };

        // Mirrors ArchetypeChunkPool::GrowArena (ArchetypeChunkPool.hpp:814),
        // minus huge pages. Caller holds m_mutex.
        bool GrowArena(size_t minBytes)
        {
            constexpr size_t kArenaOverhead = 256;   // TLSF front pad + sentinel + rounding
            ASTRA_ASSERT(minBytes + kArenaOverhead <= Tlsf::MAX_REQUEST_BYTES,
                         "command block larger than the largest request TLSF can service");
            size_t want = std::max(kArenaBytes, minBytes + kArenaOverhead);
            want = std::clamp(want, Tlsf::MIN_ARENA_BYTES, Tlsf::MAX_ARENA_BYTES);

            AllocResult r = AllocateMemory(want, CACHE_LINE_SIZE, AllocFlags::None);
            if (!r.ptr) ASTRA_UNLIKELY
                return false;
            if (!m_tlsf.AddArena(r.ptr, r.size)) ASTRA_UNLIKELY
            {
                FreeMemory(r.ptr, r.size, r.usedHugePages);
                return false;
            }
            m_regions.push_back(Region{r.ptr, r.size});
            return true;
        }

        std::mutex m_mutex;
        Tlsf m_tlsf;
        std::vector<Region> m_regions;
    };

} // namespace Astra
```

(Verify `CACHE_LINE_SIZE`, `AllocResult`, `AllocFlags` are reachable via `Core/Memory.hpp`/`Core/Base.hpp` exactly as `ArchetypeChunkPool.hpp` reaches them; adjust includes to match that file's pattern if needed.)

- [ ] **Step 5: Wire the arena into `Registry`** (`include/Astra/Registry/Registry.hpp`):

Add `#include "../Commands/CommandBlockArena.hpp"` to the include block (:10-29, alphabetical position). Add the member beside the other value members (find `m_resourceStorage`'s declaration; place after it):

```cpp
        CommandBlockArena m_commandBlockArena;
```

Add a public accessor next to the other component accessors (e.g. near `GetComponentRegistry`):

```cpp
        /**
         * Block source for CommandBuffer storage (Commands C1 fix): stable
         * blocks that never move until released. Command buffers bound to
         * this Registry must not outlive it.
         */
        [[nodiscard]] CommandBlockArena& GetCommandBlockArena() noexcept { return m_commandBlockArena; }
```

No constructor changes (default-constructed member).

- [ ] **Step 6: Rewrite `CommandByteBuffer` (`CommandBuffer.hpp:52-115`) to the stable block chain:**

```cpp
    /**
     * Internal segmented byte storage for commands. Commands are stored as
     * [Header][Payload] pairs inside a chain of STABLE blocks: once written,
     * a command's bytes never move until Clear()/destruction (Commands C1
     * fix -- growth acquires a fresh block instead of relocating). Commands
     * never straddle blocks; walks use CommandHeader::totalSize within each
     * block's [base, base+used) extent.
     */
    class CommandByteBuffer
    {
    public:
        static constexpr size_t DEFAULT_INITIAL_CAPACITY = 4096;
        static constexpr size_t MAX_BLOCK_BYTES = 64 * 1024;
        // Every command start is aligned to 16 within its block. Blocks come
        // from CommandBlockArena (TLSF payloads: 64B-aligned by its size-
        // congruence law), so block bases satisfy this with headroom.
        static constexpr size_t ALIGNMENT = 16;
        static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= ALIGNMENT,
            "CommandByteBuffer requires operator-new alignment >= 16 (32-bit targets are unsupported)");

        struct Block
        {
            std::byte* base = nullptr;
            size_t capacity = 0;
            size_t used = 0;
        };

        explicit CommandByteBuffer(CommandBlockArena* arena,
                                   size_t initialCapacity = DEFAULT_INITIAL_CAPACITY) :
            m_arena(arena),
            m_nextBlockBytes(initialCapacity)
        {}

        CommandByteBuffer(const CommandByteBuffer&) = delete;
        CommandByteBuffer& operator=(const CommandByteBuffer&) = delete;

        ~CommandByteBuffer()
        {
            if (m_arena)
            {
                for (Block& b : m_blocks)
                    m_arena->Release(b.base);
            }
        }

        /**
         * Allocate STABLE space for one command (never moves until
         * Clear()/destruction). Returns nullptr only on arena exhaustion.
         */
        std::byte* Allocate(size_t size)
        {
            size_t alignedSize = AlignUp(size, ALIGNMENT);

            // Advance-only: a command that doesn't fit the active block moves
            // to the NEXT block (never back), so walking blocks in order
            // always replays record order. Skipped remainders stay dead until
            // Clear() resets the cursors.
            while (m_activeBlock < m_blocks.size() &&
                   m_blocks[m_activeBlock].capacity - m_blocks[m_activeBlock].used < alignedSize)
            {
                ++m_activeBlock;
            }
            if (m_activeBlock == m_blocks.size())
            {
                if (!AcquireBlock(alignedSize)) ASTRA_UNLIKELY
                    return nullptr;
            }

            Block& b = m_blocks[m_activeBlock];
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(b.base) % ALIGNMENT) == 0,
                         "command block base must satisfy command alignment");
            std::byte* ptr = b.base + b.used;
            b.used += alignedSize;
            m_totalUsed += alignedSize;
            return ptr;
        }

        [[nodiscard]] const std::vector<Block>& Blocks() const noexcept { return m_blocks; }
        [[nodiscard]] size_t BlockCount() const noexcept { return m_blocks.size(); }
        [[nodiscard]] size_t Size() const noexcept { return m_totalUsed; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_totalUsed == 0; }

        /**
         * Reset every block's write cursor, KEEPING the blocks (retention:
         * steady-state record->flush->clear cycles make zero arena calls).
         */
        void Clear() noexcept
        {
            for (Block& b : m_blocks)
                b.used = 0;
            m_activeBlock = 0;
            m_totalUsed = 0;
        }

        /**
         * Ensure total capacity >= capacity by acquiring at most one block.
         */
        void Reserve(size_t capacity)
        {
            size_t total = 0;
            for (const Block& b : m_blocks)
                total += b.capacity;
            if (total < capacity)
                AcquireBlock(capacity - total);
        }

    private:
        bool AcquireBlock(size_t minBytes)
        {
            if (!m_arena) ASTRA_UNLIKELY
                return false;
            // Geometric ramp capped at MAX_BLOCK_BYTES; an oversized command
            // gets a block sized to fit it exactly.
            size_t request = std::max(m_nextBlockBytes, minBytes);
            CommandBlockArena::BlockAlloc alloc = m_arena->Acquire(request);
            if (!alloc.ptr) ASTRA_UNLIKELY
                return false;
            m_blocks.push_back(Block{alloc.ptr, alloc.bytes, 0});
            m_activeBlock = m_blocks.size() - 1;
            m_nextBlockBytes = std::min(request * 2, MAX_BLOCK_BYTES);
            return true;
        }

        CommandBlockArena* m_arena = nullptr;
        std::vector<Block> m_blocks;
        size_t m_activeBlock = 0;
        size_t m_nextBlockBytes = DEFAULT_INITIAL_CAPACITY;
        size_t m_totalUsed = 0;
    };
```

(This absorbs Task 1's `__STDCPP_DEFAULT_NEW_ALIGNMENT__` static_assert into the new class — keep exactly one copy. `Data()` is gone deliberately; the compiler will point at every caller you must convert in Steps 7-8. Delete `Reserve`'s old body along the way — the new one is above.)

- [ ] **Step 7: Convert `CommandBuffer`'s internals.** All in `CommandBuffer.hpp`:

7a. Constructor (:159-164) — bind the buffer to the Registry's arena:

```cpp
        explicit CommandBuffer(Registry* registry, bool deferredCreation = false) :
            m_registry(registry),
            m_buffer(registry ? &registry->GetCommandBlockArena() : nullptr),
            m_deferredCreation(deferredCreation)
        {
            ASTRA_ASSERT(registry != nullptr, "Registry cannot be null");
        }
```

7b. `Execute` (:794-857) — replace the flat walk (from `std::byte* ptr = m_buffer.Data();` through the end of the `while` loop) with a per-block walk; the failure block and everything after the loop stay identical:

```cpp
            m_lastExecutedCount = 0;
            PlaceholderMap placeholders;

            for (const CommandByteBuffer::Block& block : m_buffer.Blocks())
            {
                std::byte* ptr = block.base;
                std::byte* end = block.base + block.used;
                while (ptr < end)
                {
                    auto* header = reinterpret_cast<CommandHeader*>(ptr);
                    std::byte* payloadPtr = ptr + sizeof(CommandHeader);

                    if (m_deferredCreation)
                        ResolvePlaceholders(header->type, payloadPtr, placeholders);

                    bool success = ExecuteCommand(header->type, payloadPtr);

                    if (!success)
                    {
                        // ... keep the existing failure block verbatim ...
                    }

                    // Advance by aligned size (commands are stored at ALIGNMENT-byte stride)
                    ptr += AlignUp(static_cast<size_t>(header->totalSize), CommandByteBuffer::ALIGNMENT);
                    m_lastExecutedCount++;
                }
            }
```

(The old 16-alignment assert on `m_buffer.Data()` at :803-804 is superseded by the per-block assert in `Allocate` — delete it.)

7c. `CleanupPendingCommands` (:1511-1558) — same two-level conversion: wrap the existing `while (ptr < end)` body in `for (const CommandByteBuffer::Block& block : m_buffer.Blocks())` with `ptr = block.base; end = block.base + block.used;`. The switch body is unchanged.

7d. `StampCommand` (:1040-1054) — pointers are stable now; store them directly:

```cpp
        /**
         * Record the {SortKey, command pointer} descriptor for the command
         * whose header was just allocated at commandPtr. Block storage is
         * STABLE (C1 fix), so the raw pointer stays valid until
         * Clear()/destruction -- no offset indirection needed.
         */
        void StampCommand(std::byte* commandPtr)
        {
            SortKey key = m_hasCustomSortKey ? m_currentSortKey : SortKey{0, 0, m_autoSeq};
            ++m_autoSeq;
            m_commandKeys.emplace_back(key, commandPtr);
        }
```

Member (:1583): `std::vector<std::pair<SortKey, std::byte*>> m_commandKeys;`

7e. `CommandKeys` (:866-876): return type becomes `const std::vector<std::pair<SortKey, std::byte*>>&`; update its doc comment ("byte offset" → "stable command pointer").

7f. `ApplyCommandAt` / `ResolveAndApplyCommandAt` (:878-923) — take the pointer:

```cpp
        bool ApplyCommandAt(std::byte* command)
        {
            auto* header = reinterpret_cast<CommandHeader*>(command);
            std::byte* payloadPtr = command + sizeof(CommandHeader);
            return ExecuteCommand(header->type, payloadPtr);
        }

        bool ResolveAndApplyCommandAt(std::byte* command, PlaceholderMap& map)
        {
            auto* header = reinterpret_cast<CommandHeader*>(command);
            std::byte* payloadPtr = command + sizeof(CommandHeader);
            ResolvePlaceholders(header->type, payloadPtr, map);
            return ExecuteCommand(header->type, payloadPtr);
        }
```

(Update both doc comments: "byte offset ... from CommandKeys()" → "stable command pointer from CommandKeys(); must belong to THIS buffer".)

7g. Add the block-count accessor for tests/diagnostics, next to `GetMemoryUsage` (:1010):

```cpp
        /**
         * Number of storage blocks currently held (diagnostics/tests: the
         * retention contract -- Clear() keeps blocks -- is observable here).
         */
        [[nodiscard]] size_t GetStorageBlockCount() const noexcept { return m_buffer.BlockCount(); }
```

7h. `ParallelCommandBuffer::ExecuteSorted` (:1724-1759) — `Item` carries the pointer:

```cpp
            struct Item
            {
                SortKey key;
                CommandBuffer* buf;
                std::byte* cmd;
            };

            std::vector<Item> items;
            for (auto& b : m_buffers)
            {
                if (b)
                {
                    for (const auto& [key, cmd] : b->CommandKeys())
                    {
                        items.push_back({key, b.get(), cmd});
                    }
                }
            }
```

and the apply call becomes `it.buf->ResolveAndApplyCommandAt(it.cmd, perBufferMaps[it.buf])`.

- [ ] **Step 8: Build Debug; run the RED test — expected PASS (GREEN).**

Run: `--gtest_filter=CommandBufferTest.NonTrivialComponentsSurviveBufferGrowth`
Expected: PASS (violations 0 — nothing relocated).

- [ ] **Step 9: Add the three new-API tests** to `tests/Commands/CommandBufferTest.cpp`:

```cpp
TEST_F(CommandBufferTest, GrowthUsesNewBlocksAndClearRetainsThem)
{
    auto recordLoad = [&] {
        for (int i = 0; i < 200; ++i)
        {
            Entity e = cmdBuffer->CreateEntity();
            cmdBuffer->AddComponent(e, Position{float(i), 0.0f, 0.0f});
        }
    };

    recordLoad();
    EXPECT_GT(cmdBuffer->GetStorageBlockCount(), 1u)
        << "load must actually exercise growth or this test is vacuous";
    ASSERT_TRUE(cmdBuffer->Execute().IsOk());   // clears on success
    const size_t blocksAfterFirst = cmdBuffer->GetStorageBlockCount();
    EXPECT_GT(blocksAfterFirst, 1u);            // retention: Clear() kept them

    recordLoad();
    ASSERT_TRUE(cmdBuffer->Execute().IsOk());
    // Same load, second cycle: retained blocks absorb it with zero arena calls.
    EXPECT_EQ(cmdBuffer->GetStorageBlockCount(), blocksAfterFirst);
}

TEST_F(CommandBufferTest, RollbackSpansStorageBlocks)
{
    // First command fails at Execute -> whole-buffer failure path; every
    // eagerly-allocated entity is uncommitted and must be destroyed, across
    // MULTIPLE blocks (300 creates + the add far exceed the 4KB first block).
    cmdBuffer->AddComponent(Entity::Invalid(), Position{1.0f, 2.0f, 3.0f});
    std::vector<Entity> ents;
    for (int i = 0; i < 300; ++i)
        ents.push_back(cmdBuffer->CreateEntity());
    EXPECT_GT(cmdBuffer->GetStorageBlockCount(), 1u);

    auto result = cmdBuffer->Execute();
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(registry->Size(), 0u);
    for (Entity e : ents)
        EXPECT_FALSE(registry->IsValid(e));
}

TEST_F(CommandBufferTest, ParallelSortedFlushCarriesNonTrivialComponents)
{
    using Astra::Test::Name;
    ParallelCommandBuffer pcb(registry.get());
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&pcb, t] {
            auto& buf = pcb.GetThreadBuffer();
            for (int j = 0; j < kPerThread; ++j)
            {
                Entity e = buf.CreateEntity();
                // Long enough to be heap-backed on every std::string impl.
                buf.AddComponent(e, Name{"thread_" + std::to_string(t) +
                                         "_entity_" + std::to_string(j) +
                                         "_padded_beyond_any_sso_buffer"});
            }
        });
    }
    for (auto& th : threads) th.join();

    ASSERT_TRUE(pcb.ExecuteSorted().IsOk());
    EXPECT_TRUE(pcb.GetDeferredErrors().empty());
    EXPECT_EQ(registry->Size(), size_t(kThreads) * kPerThread);

    auto view = registry->CreateView<Name>();
    size_t count = 0;
    view.ForEach([&](Entity, Name& n) {
        EXPECT_NE(n.value.find("_padded_beyond_any_sso_buffer"), std::string::npos);
        ++count;
    });
    EXPECT_EQ(count, size_t(kThreads) * kPerThread);
}
```

(Adapt `Name`'s field spelling (`value`?) and the `ForEach` callback shape to the file's existing idioms — read a neighboring view test. If `<thread>` isn't already included by the test file, add it.)

- [ ] **Step 10: Full 3-config build + suite.**

Expected: **Debug 746 / Release 744 / Dist 744** (+4), all pass. Any `s_live` imbalance or desync assert = rewrite bug: STOP and report, do not paper over.

- [ ] **Step 11: Commit**

```bash
git add include/Astra/Commands/CommandBlockArena.hpp include/Astra/Commands/CommandBuffer.hpp include/Astra/Registry/Registry.hpp tests/TestComponents.hpp tests/Commands/CommandBufferTest.cpp
git commit -m "fix(commands): stable segmented TLSF-backed command storage - recorded commands never relocate (C1), blocks retained across Clear, pointer sort keys"
```

---

### Task 3: C3 — batch executors surface per-entity failures

**Files:**
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (`ExecuteAddComponentBatch`/`ExecuteRemoveComponentBatch` :1401-1432 region; `ExecuteCommand` dispatch — find its switch; one new member + accessor; `ExecuteSorted` failure branch)
- Test: `tests/Commands/CommandBufferTest.cpp` (2 new tests)

**Interfaces:**
- Consumes: Task 2's `ExecuteSorted` Item plumbing (`it.cmd`, `it.buf`).
- Produces: `CommandBuffer::GetLastBatchFailureCount() const noexcept -> size_t` (consumed by `ExecuteSorted` only).

- [ ] **Step 1: Write the two RED tests** in `tests/Commands/CommandBufferTest.cpp`:

```cpp
TEST_F(CommandBufferTest, BatchPartialFailureSurfacesPerEntityErrorsInSortedFlush)
{
    Entity e1 = registry->CreateEntity();
    Entity e2 = registry->CreateEntity();
    Entity e3 = registry->CreateEntity();

    ParallelCommandBuffer pcb(registry.get());
    auto& buf = pcb.GetThreadBuffer();

    buf.SetNextSortKey(SortKey{1, 0, 0});
    buf.DestroyEntity(e2);                       // applies BEFORE the batch (lower key)
    buf.SetNextSortKey(SortKey{2, 0, 0});
    Entity batch[] = {e1, e2, e3};
    buf.AddComponents<Position>(batch, Position{1.0f, 2.0f, 3.0f});

    ASSERT_TRUE(pcb.ExecuteSorted().IsOk());

    // Attempt-all semantics: survivors got the component...
    EXPECT_NE(registry->GetComponent<Position>(e1), nullptr);
    EXPECT_NE(registry->GetComponent<Position>(e3), nullptr);
    // ...and the one dead target produced exactly one attributed error.
    const auto& errs = pcb.GetDeferredErrors();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].systemInsertionOrder, 2u);

    // Same contract for the remove batch: e2 is dead, e1/e3 hold Position.
    auto& buf2 = pcb.GetThreadBuffer();
    buf2.SetNextSortKey(SortKey{3, 0, 0});
    buf2.RemoveComponents<Position>(batch);
    ASSERT_TRUE(pcb.ExecuteSorted().IsOk());
    const auto& errs2 = pcb.GetDeferredErrors();  // per-flush list, freshly cleared
    ASSERT_EQ(errs2.size(), 1u);
    EXPECT_EQ(errs2[0].systemInsertionOrder, 3u);
    EXPECT_EQ(registry->GetComponent<Position>(e1), nullptr);
    EXPECT_EQ(registry->GetComponent<Position>(e3), nullptr);
}

TEST_F(CommandBufferTest, BatchPartialFailureFailsEagerExecute)
{
    Entity e1 = registry->CreateEntity();
    Entity e2 = registry->CreateEntity();
    Entity e3 = registry->CreateEntity();
    registry->DestroyEntity(e2);                 // dead before recording

    Entity batch[] = {e1, e2, e3};
    cmdBuffer->AddComponents<Velocity>(batch, Velocity{1.0f, 2.0f, 3.0f});

    auto result = cmdBuffer->Execute();
    EXPECT_TRUE(result.IsErr());                 // >=1 entity failed => command failed
    // Attempt-all: the survivors were still processed before the failure surfaced.
    EXPECT_NE(registry->GetComponent<Velocity>(e1), nullptr);
    EXPECT_NE(registry->GetComponent<Velocity>(e3), nullptr);
}
```

(Adapt `SortKey` brace-init spelling and `AddComponents` span-argument shape to the file's existing usage — grep `SetNextSortKey` and `AddComponents` in the test tree first; `ExecuteSortedAppliesInKeyOrderNotRecordOrder` at `CommandBufferTest.cpp:375+` is the reference idiom.)

- [ ] **Step 2: Run both — expected FAIL (RED).**

Run: `--gtest_filter=CommandBufferTest.BatchPartialFailure*`
Expected: sorted test FAILS at `errs.size() == 1` (currently 0); eager test FAILS at `result.IsErr()` (currently Ok). Record output. Do NOT commit yet.

- [ ] **Step 3: Implement.**

3a. New member beside `m_lastExecutedCount` (:1570 region), plus accessor near `GetLastExecutedCount` (:863):

```cpp
        // Per-entity failure count from the most recently dispatched BATCH
        // command executor (0 for non-batch commands -- reset by
        // ExecuteCommand before every dispatch). Lets ExecuteSorted() report
        // one DeferredCommandError per failed entity instead of one per
        // failed batch.
        size_t m_lastBatchFailureCount = 0;
```

```cpp
        /**
         * Per-entity failure count of the most recently applied batch
         * command (see ExecuteSorted's per-entity error reporting).
         */
        [[nodiscard]] size_t GetLastBatchFailureCount() const noexcept { return m_lastBatchFailureCount; }
```

3b. In `ExecuteCommand` (find its dispatch switch), first statement of the function body:

```cpp
            m_lastBatchFailureCount = 0;
```

3c. Replace both batch executors (:1401-1432):

```cpp
        bool ExecuteAddComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<AddComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();
            const void* data = cmd->GetDataPtr();

            // Attempt-all, count failures: matches the single-entity
            // executor's failure contract, scaled to N (spec §4). An Invalid
            // slot counts as a failure exactly like the single-entity path.
            size_t failed = 0;
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] == Entity::Invalid() ||
                    !m_registry->AddComponentByID(entities[i], cmd->componentId, data, cmd->dataSize))
                {
                    ++failed;
                }
            }
            m_lastBatchFailureCount = failed;
            return failed == 0;
        }

        bool ExecuteRemoveComponentBatch(std::byte* payload)
        {
            auto* cmd = reinterpret_cast<RemoveComponentBatchPayload*>(payload);
            const Entity* entities = cmd->GetEntitiesPtr();

            size_t failed = 0;
            for (uint32_t i = 0; i < cmd->entityCount; ++i)
            {
                if (entities[i] == Entity::Invalid() ||
                    !m_registry->RemoveComponentByID(entities[i], cmd->componentId))
                {
                    ++failed;
                }
            }
            m_lastBatchFailureCount = failed;
            return failed == 0;
        }
```

3d. `ExecuteSorted`'s failure branch (Task 2 left it emitting one error) becomes per-entity:

```cpp
                if (!it.buf->ResolveAndApplyCommandAt(it.cmd, perBufferMaps[it.buf]))
                {
                    // Task 4 channel, scaled: a failed batch reports one error
                    // PER failed entity (same Reason the single-entity op
                    // yields); non-batch failures report exactly one.
                    const size_t failures = std::max<size_t>(size_t(1), it.buf->GetLastBatchFailureCount());
                    for (size_t f = 0; f < failures; ++f)
                    {
                        m_deferredErrors.push_back(
                            DeferredCommandError{it.key.insertionOrder, DeferredCommandError::Reason::InvalidTargetEntity});
                    }
                    continue;
                }
```

- [ ] **Step 4: Run both tests — expected PASS (GREEN).** Run: `--gtest_filter=CommandBufferTest.BatchPartialFailure*` → 2/2 PASS.

- [ ] **Step 5: Full 3-config build + suite.** Expected: **Debug 748 / Release 746 / Dist 746** (+2), all pass.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Commands/CommandBuffer.hpp tests/Commands/CommandBufferTest.cpp
git commit -m "fix(commands): batch executors attempt all entities and surface per-entity failures through the deferred-error channel (C3)"
```

---

### Finishing (SDD flow)

OPUS whole-branch review (with the per-task Minors roll-up; review lens: spec §10 invariants, destructor-exactly-once across every path, walk-order equivalence, no record-path regression) → ONE fix subagent if findings → authoritative 3-config on the final code commit → **confirm with user** → FF-merge dev local → delete branch → don't push → update memory (Commands C1/C2/C3 closed on the remediation roadmap; deferred items M5 + placeholder-ambiguity + chunk-grouped batches recorded).
