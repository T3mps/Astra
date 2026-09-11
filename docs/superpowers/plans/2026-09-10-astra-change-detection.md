# Astra Change Detection (Stage 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a system ask "which entities' `T` changed (or were added) since I last ran" with a free per-chunk tier for every component and an opt-in exact per-entity tier, so Arcane can delete its hand-rolled dirty trackers.

**Architecture:** A process-relative `Tick` counter lives in `ArchetypeManager` (Registry forwards `CurrentTick`/`AdvanceTick`; the scheduler advances it once per parallel group). Every chunk carves a `Tick` version per storage column in its arena; every write path Astra controls stamps the destination chunk, and a view whose access to `T` is non-const stamps `T`'s column in each chunk it enters. Types that declare `AstraChangeTracked = true` additionally carve `{added, changed}` ticks per entity (same shape as the enableable disabled-word carve); views hand those types out as `Mut<T>` whose implicit `T&` conversion marks. `Changed<T>`/`Added<T>` are match-only query modifiers next to `With<T>` that reject whole chunks by version before any per-entity work, then (tracked types only) run-scan the tick column unioned with the enabled-run scan.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln` via premake5), GoogleTest, `bench-compare/` head-to-head harness.

**Spec:** `docs/superpowers/specs/2026-09-10-astra-change-detection-design.md` (user-approved design 2026-09-10). Evidence: `bench-compare/spike-changeticks/spike-notes.md`.

## Global Constraints

- **Branch:** `feat/change-detection` off dev @ `4ac19ff`. Local only, NEVER push. Finish = opus whole-branch review (chunk layout + scheduler + iteration) -> fix wave -> authoritative 3-config -> **confirm with user** -> FF-merge to dev, delete branch. Sanitizer lane runs on the next push (not in this movement).
- **Build (PowerShell):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<CFG> -p:Platform=x64 -m` with `<CFG>` in {Debug, Release, Dist}. Tests: `.\bin\<CFG>-windows-x86_64\AstraTest\AstraTest.exe`. `-t:AstraTest` does NOT work (solution folders) -- build the whole solution. Adding a `.cpp` under `tests/` REQUIRES `D:\dev\_shared\tools\premake5 vs2022` first (files are globbed at generation time). Stale PDB lock -> `taskkill /F /IM mspdbsrv.exe`. IDE clang diagnostics are false positives; judge only by MSBuild. `CompressionTest.PerformanceBenchmark` is a known load flake.
- **Baseline test counts** (dev @ `fa0900a`, last recorded): **Debug 881 / Release 878 / Dist 878** -- re-verify on the branch base before Task 1 and record the real numbers in the ledger. Deltas govern over absolutes.
- **Archive format is UNCHANGED (stays v5).** No tick or version is written to disk. Any task that touches `Archetype::Serialize`/`SerializeColumn`/`DeserializeColumn` bytes is out of scope.
- **Zero cost when unused (spec §2.7), structurally:** no per-entity tick storage unless `IsChangeTrackedV<T>`; a view that names no `Changed`/`Added` instantiates no filter code (`if constexpr` gate, exactly like `HasEnabledFilter`); a view whose every yielded component is `const` stamps nothing (compile-time empty column list); the coarse stamp on a non-const view is one plain store per mutable column per visited chunk.
- **TypeID ceiling:** the test binary is compiled at `ASTRA_MAX_COMPONENTS=192` (premake5.lua:96) and is near it. This plan mints EXACTLY two new test component types, `Astra::Test::TrackedPos` and `Astra::Test::TrackedVel` (Task 6). Everything else reuses `tests/TestComponents.hpp` types (`Position`, `Velocity`, `Health`, `Transform`, `Timer`/`Hierarchy` = the two enableable types).
- **Ticks:** `Tick` is `uint32_t`; `0` means "never"; the counter starts at `1` and skips `0` on wraparound; "a newer than b" is `int32_t(a - b) > 0` (age bound 2^31 runs, documented, no rescan).
- **Planning deviations from the spec (flag to the user at review; each is documented in the task that makes it):**
  1. **Tick advance is once per parallel GROUP, not once per system run** (Task 5). Systems in one group have no read/write conflict by construction, so ordering among them is undefined anyway; sharing one tick keeps `CurrentTick()` exact for view-entry stamping while a group runs concurrently (per-system atomic ticks would let a sibling's tick leak into a system's own stamps and make it see its own writes next frame). Every observable guarantee in spec §3.6 holds.
  2. **Chunk versions live in the chunk arena, tracked tick pointers live in `Column`** (Task 2/6): `Tick* m_columnVersion` (one 8-byte-aligned region, `columnCount * 4` bytes, carved right after the column data) plus `EntityTicks* Column::ticks` (mirrors `Column::disabledWords`; +8 bytes per `Column` slot of fixed per-chunk metadata, same accepted cost class as the enableable feature's +8).
  3. **`Changed<T>`/`Added<T>` are filter-only and iteration-only this stage** (Task 4): `T` must ALSO be listed as a required (bare/`const`/`IncludeDisabled`) component (spec §3.6 refusal, so the spec's §3.4 example must read `CreateView<const WorldTransform, Changed<WorldTransform>, SpriteRenderer, Not<Hidden>>`); `Size/Empty/Contains/Get/Single/begin/end` on a change-filtered view are compile-time refused (they would need a tick too). Tag types are refused (no column, no version).
  4. **Same-archetype relocations (CompactChunks, MoveEntitiesBetweenChunks) fold the newer version and copy ticks** (Task 2/6) instead of stamping with the current tick: without carrying the version, an entity written at tick 100 and compacted into a fresh chunk (version 0) would be MISSED by a reader whose last run was 90 -- a false negative the spec forbids.
  5. **`Optional<T>` for a tracked non-const `T` marks the entity unconditionally when present** (Task 7): the yield is a raw `T*`, so there is no conversion hook; use `Optional<const T>` to avoid the false positive.
- **Model recipe (SDD):** OPUS for Tasks 2, 3, 4, 5, 6, 7, 8 (chunk layout, view hot path, scheduler, per-entity filter) and the final whole-branch review; sonnet for Tasks 1 and 9.
- **Bench gate (Task 9 only):** `bench-compare/build_one.bat` with the full-opt recipe from `bench-compare/RESULTS.md:574` (`/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /DASTRA_BUILD_DIST /I..\include /I..\tests /link /LTCG`), typeperf quiet check, 6 interleaved rounds baseline<->branch, medians with [min,max]; non-overlapping-bands rule on the flat-watch ops (create, create_batch, add_component, remove_component, destroy, random_get, iterate1/2/3). Results appended to `bench-compare/RESULTS.md`.

---

## File Structure

**New**
- `include/Astra/Core/Tick.hpp` -- `Tick`, `IsNewer`, `EntityTicks`, `TickContext` concept. Zero dependencies beyond `<cstdint>`/`<concepts>` so chunk, query, view, registry and system headers can all include it.
- `tests/Registry/ChangeDetectionTest.cpp` -- ticks, chunk versions, write-side stamping, filters (untracked + tracked), moves, deserialize, zero-cost checks (Tasks 1-4, 6-8).
- `tests/System/ChangeDetectionSchedulerTest.cpp` -- `SystemContext` ticks, executor group ticks, preference for `SystemContext&`, registry switch (Task 5).
- `tests/Registry/ChangeDetectionAcceptanceTest.cpp` -- the Arcane-shaped 5%-of-1M acceptance test (Task 9).

**Modified**
- `include/Astra/Archetype/ArchetypeChunkPool.hpp` -- `ArchetypeColumnMeta::trackedColumns`; `ArchetypeChunk`: `m_columnVersion` carve + `GetColumnVersion/StampColumn/StampAllColumns/FoldColumnVersion`; `Column::ticks` carve + `GetTicks/IsTracked/InitTicks/CopyTicks`; tick params on `AddEntity/AddEntityWithComponents/BatchAddEntities`; tick carry in `RemoveEntity`/`BatchMoveComponentsFrom`.
- `include/Astra/Archetype/Archetype.hpp` -- `m_tickSource`/`Now()`; `BuildColumnMeta` tracked ordinals; layout math (`ComputeCapacityForBytes`/`ComputeLayoutBytesForCapacity`/`ChunkBytesToHold`/`Initialize` overhead); stamps on `AllocateEntitySlot`/`AddEntitiesWith`/`BatchMoveEntitiesFrom`/`Deserialize`; fold+copy in `CompactChunks`/`MoveEntitiesBetweenChunks`; tick copy in `MoveEntityFrom`; `ForEachStamped`/`ForEachChunkStamped`.
- `include/Astra/Archetype/ArchetypeManager.hpp` -- `m_tick`, `CurrentTick/AdvanceTick`, tick source wiring (root, `GetOrCreateArchetype`, `Deserialize`); tick copy in `MoveAndAdd`/`MoveAndAddByID`; `GetComponentMut<T>`; `MarkWritten`.
- `include/Astra/Component/Component.hpp` -- `ChangeTrackedTraits<T>`/`IsChangeTrackedV<T>`; `ComponentDescriptor::isChangeTracked`.
- `include/Astra/Component/ComponentRegistry.hpp` -- `MakeDescriptor` assigns `isChangeTracked`.
- `include/Astra/Registry/Query.hpp` -- `Changed<T>`, `Added<T>`, `Mut<T>`, classifier/access/mask plumbing, `ChangeTermsAreRequired`.
- `include/Astra/Registry/View.hpp` -- entry stamping, `HasChangeFilter`/`HasTrackedYield` gates, `ForEach(ctx, fn)`/`ParallelForEach(ctx, fn)`/`Since(tick)`, chunk reject + per-entity tier in `VisitChunkFiltered`, `Mut` yield, `Get` tuple shape.
- `include/Astra/Registry/ViewIterator.hpp` -- range-for stamps mutable columns per chunk (takes a `Tick`).
- `include/Astra/Registry/Registry.hpp` -- `CurrentTick/AdvanceTick/Modified<T>/SetIfNeq<T>`; non-const `GetComponent<T>` stamps+marks.
- `include/Astra/System/SystemContext.hpp` -- `LastRun()/ThisRun()`, ctor params, `ParallelForEach(view, fn)` passes `LastRun()`.
- `include/Astra/System/SystemMetadata.hpp` -- `mutable Tick lastRun`.
- `include/Astra/System/SystemExecutor.hpp` -- `BeginSystemGroup`, `DispatchSystem` ticks, executors call `BeginSystemGroup` per group.
- `include/Astra/System/SystemScheduler.hpp` -- `AddSystem<System T>` excludes `ContextSystem`; registry-switch resets `lastRun`.
- `include/Astra/Astra.hpp` -- include `Core/Tick.hpp`.
- `tests/TestComponents.hpp` -- `TrackedPos`, `TrackedVel`.
- `bench-compare/bench_astra.cpp` -- `run_change_detection()` (Astra-only ops).
- `bench-compare/RESULTS.md`, `README.md`, the spec's status line.

---

## Task 1: `Tick`, the Registry counter, and the `IsNewer` compare

**Files:**
- Create: `include/Astra/Core/Tick.hpp`
- Modify: `include/Astra/Astra.hpp` (add `#include "Core/Tick.hpp"` after `Core/TypeID.hpp`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (member + two accessors, near `GetStructuralChangeCounter` ~:640)
- Modify: `include/Astra/Registry/Registry.hpp` (two forwarders, next to `GetArchetypeManager()` ~:1310)
- Create: `tests/Registry/ChangeDetectionTest.cpp`

**Interfaces:**
- Produces: `Astra::Tick` (= `uint32_t`), `constexpr bool Astra::IsNewer(Tick a, Tick b) noexcept`, `struct Astra::EntityTicks { Tick added; Tick changed; }`, concept `Astra::TickContext<C>` (`c.LastRun()` convertible to `Tick`), `Tick ArchetypeManager::CurrentTick() const noexcept`, `Tick ArchetypeManager::AdvanceTick() noexcept`, `Tick Registry::CurrentTick() const noexcept`, `Tick Registry::AdvanceTick() noexcept`. Every later task consumes these names verbatim.

- [ ] **Step 1: Record the branch baseline.** On dev @ `4ac19ff`: `git switch -c feat/change-detection`. Build Debug, run `AstraTest.exe`, write the total into the ledger (expected ~881). Do not proceed on a red baseline.

- [ ] **Step 2: Write the failing tests.** Create `tests/Registry/ChangeDetectionTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;

// ---- Task 1: Tick, IsNewer, Registry counter ------------------------------

TEST(ChangeDetectionTick, IsNewerIsSignedDifferenceAndTreatsZeroAsNever)
{
    static_assert(std::is_same_v<Tick, uint32_t>);
    static_assert(Astra::IsNewer(2u, 1u));
    static_assert(!Astra::IsNewer(1u, 1u));
    static_assert(!Astra::IsNewer(1u, 2u));
    static_assert(Astra::IsNewer(1u, 0u));            // anything stamped is newer than "never"
    static_assert(!Astra::IsNewer(0u, 0u));           // never vs never: not newer
    // Wraparound: a stamp that wrapped past 2^32 still orders after a big tick.
    constexpr Tick big = 0xFFFFFFF0u;
    static_assert(Astra::IsNewer(Tick(big + 16u), big));         // 0x00000000 after wrap
    static_assert(Astra::IsNewer(Tick(1u), Tick(0xFFFFFFFFu)));
    static_assert(!Astra::IsNewer(Tick(0x80000000u), Tick(0u)));  // exactly 2^31 ahead reads as older (documented bound)
    SUCCEED();
}

TEST(ChangeDetectionTick, RegistryCounterStartsAtOneAndIsMonotonic)
{
    Astra::Registry reg;
    EXPECT_EQ(reg.CurrentTick(), 1u);
    EXPECT_EQ(reg.AdvanceTick(), 2u);
    EXPECT_EQ(reg.CurrentTick(), 2u);
    Tick prev = reg.CurrentTick();
    for (int i = 0; i < 1000; ++i)
    {
        Tick t = reg.AdvanceTick();
        EXPECT_TRUE(Astra::IsNewer(t, prev));
        prev = t;
    }
    // The ArchetypeManager owns the counter; Registry forwards.
    EXPECT_EQ(reg.GetArchetypeManager()->CurrentTick(), reg.CurrentTick());
}

TEST(ChangeDetectionTick, EntityTicksIsTwoTicksTightlyPacked)
{
    static_assert(sizeof(Astra::EntityTicks) == 8);
    static_assert(alignof(Astra::EntityTicks) == 4);
    Astra::EntityTicks t{};
    EXPECT_EQ(t.added, 0u);
    EXPECT_EQ(t.changed, 0u);
}
```

- [ ] **Step 3: Regenerate the solution and confirm the test fails to compile.** Run `D:\dev\_shared\tools\premake5 vs2022`, then build Debug. Expected: errors naming `Astra::Tick` / `CurrentTick`.

- [ ] **Step 4: Create `include/Astra/Core/Tick.hpp`:**

```cpp
#pragma once

#include <concepts>
#include <cstdint>

#include "Base.hpp"   // ASTRA_NODISCARD

namespace Astra
{
    /**
     * Change-detection time (spec 2026-09-10 §3.1). A Tick is a process-relative,
     * monotonically increasing counter owned by the Registry's ArchetypeManager
     * and advanced once per system group by the scheduler (or by hand via
     * Registry::AdvanceTick for unscheduled use). Tick 0 means "never": a fresh
     * chunk or entity has never been stamped. Nothing here is serialized.
     */
    using Tick = uint32_t;

    /**
     * Two ticks compared by SIGNED difference so the counter may wrap: `a` is
     * newer than `b` iff a - b, read as int32_t, is positive. Ages beyond 2^31
     * system runs misread (documented bound; a periodic clamp is a follow-up).
     * Stamped(>=1) vs never(0) is always newer; never vs never is not.
     */
    ASTRA_NODISCARD constexpr bool IsNewer(Tick a, Tick b) noexcept
    {
        return static_cast<int32_t>(a - b) > 0;
    }

    /**
     * Per-entity ticks for a change-tracked column (spec §3.2, opt-in tier):
     * `added` = the tick the component was added to this entity, `changed` = the
     * tick of the last mark. 8 bytes per entity per tracked column, carved into
     * the chunk arena beside the enableable disabled words.
     */
    struct EntityTicks
    {
        Tick added   = 0;
        Tick changed = 0;
    };

    /**
     * Anything a tick-aware view can take its "since" from: SystemContext, or a
     * test stand-in. A concept (not the concrete SystemContext) so View.hpp does
     * not have to include System/SystemContext.hpp (which includes Registry.hpp,
     * which includes View.hpp).
     */
    template<typename C>
    concept TickContext = requires(const C& c)
    {
        { c.LastRun() } -> std::convertible_to<Tick>;
    };
}
```

- [ ] **Step 5: Add the counter to `ArchetypeManager`.** In `include/Astra/Archetype/ArchetypeManager.hpp` add `#include "../Core/Tick.hpp"` with the other Core includes. Next to `GetStructuralChangeCounter()` (~:640) add:

```cpp
        // ---- Change-detection time (spec 2026-09-10 §3.1) ----
        // The one counter every stamp in this registry reads. Advanced by the
        // scheduler between system groups (SystemExecutor::BeginSystemGroup) or by
        // Registry::AdvanceTick for unscheduled use; NEVER advanced concurrently
        // with a running system (plain, non-atomic by contract). Starts at 1 so a
        // zero-initialised chunk/entity (tick 0 == "never") is older than any stamp.
        ASTRA_NODISCARD Tick CurrentTick() const noexcept { return m_tick; }
        Tick AdvanceTick() noexcept
        {
            if (++m_tick == 0) ASTRA_UNLIKELY
                m_tick = 1;   // skip "never" on wraparound
            return m_tick;
        }
```

and the member next to `m_structuralChangeCounter` (~:1640): `Tick m_tick = 1;   // change-detection time; see CurrentTick()`.

- [ ] **Step 6: Forward from `Registry`.** In `include/Astra/Registry/Registry.hpp` next to `GetArchetypeManager()`:

```cpp
        // Change-detection time (spec 2026-09-10 §3.1): forwarded from the owning
        // ArchetypeManager so views (which hold the manager) and the registry agree.
        ASTRA_NODISCARD Tick CurrentTick() const noexcept { return m_archetypeManager->CurrentTick(); }
        Tick AdvanceTick() noexcept { return m_archetypeManager->AdvanceTick(); }
```

Add `#include "Core/Tick.hpp"` to `include/Astra/Astra.hpp` right after `#include "Core/TypeID.hpp"`.

- [ ] **Step 7: Build Debug, run `AstraTest.exe --gtest_filter=ChangeDetectionTick.*`.** Expected: 3 PASS. Then run the full suite: baseline + 3.

- [ ] **Step 8: Commit.**

```bash
git add include/Astra/Core/Tick.hpp include/Astra/Astra.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(core): Tick, IsNewer, EntityTicks and the Registry change-detection counter (Stage 3 task 1)"
```

---
## Task 2: Per-column chunk versions and stamping on every structural write path (OPUS)

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`ArchetypeChunk`: member, carve, accessors, tick params on `AddEntity`/`AddEntityWithComponents`/`BatchAddEntities`)
- Modify: `include/Astra/Archetype/Archetype.hpp` (`m_tickSource`/`Now()`, `Initialize` overhead, `ComputeLayoutBytesForCapacity`, `AllocateEntitySlot`, `AddEntity`/`AddEntityWith`/`AddEntities`/`AddEntitiesWith`, `BatchMoveEntitiesFrom`, `Deserialize`, `CompactChunks`, `MoveEntitiesBetweenChunks`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (tick source wiring at the three archetype-creation sites)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append), `tests/Registry/ChunkPoolTest.cpp` (append one raw-chunk test)

**Interfaces:**
- Consumes: `Tick`, `IsNewer`, `ArchetypeManager::m_tick` (Task 1).
- Produces (all later tasks use these exact names):
  - `ArchetypeChunk`: `Tick GetColumnVersion(int column) const noexcept`; `void StampColumn(int column, Tick tick) noexcept`; `void StampAllColumns(Tick tick) noexcept`; `void FoldColumnVersion(int column, Tick other) noexcept` (keeps the newer); `size_t GetColumnVersionOffset() const` (inspector); `size_t AddEntity(Entity, Tick tick = 1)`; `template<typename... C> size_t AddEntityWithComponents(Entity, Tick tick, C&&...)`; `void BatchAddEntities(std::span<const Entity>, Tick tick = 1)`.
  - `Archetype`: `void SetTickSource(const Tick*) noexcept`; `Tick Now() const noexcept` (1 when unwired); `static Deserialize(BinaryReader&, const std::vector<ComponentDescriptor>&, ArchetypeChunkPool* = nullptr, const Tick* tickSource = nullptr)`.
- Invariant this task establishes: **every chunk slot that receives an entity (create, batch create, generator create, cross-archetype move, batch move, deserialize) has every storage column of that chunk stamped with `Now()` before the call returns; a same-archetype relocation carries the newer of the two versions.** Cross-archetype moves need no code here: their destination slot comes from `AllocateEntitySlot` (single) or the `BatchMoveEntitiesFrom` slot-claim loop (batch), both stamped below.

- [ ] **Step 1: Write the failing tests.** Append to `tests/Registry/ChangeDetectionTest.cpp` (below the Task 1 tests):

```cpp
#include <Astra/Commands/CommandBuffer.hpp>

using Astra::Test::Velocity;
using Astra::Test::Health;

namespace
{
    // Version of T's column in the chunk that currently holds `e`.
    template<typename T>
    Tick VersionOf(Astra::Registry& reg, Astra::Entity e)
    {
        const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) { ADD_FAILURE() << "entity not located"; return 0; }
        const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<T>::Value()];
        if (col < 0) { ADD_FAILURE() << "T has no storage column"; return 0; }
        return rec->chunk->GetColumnVersion(col);
    }

    void AdvanceTo(Astra::Registry& reg, Tick t)
    {
        while (reg.CurrentTick() < t) reg.AdvanceTick();
    }
}

// ---- Task 2: chunk column versions + structural-write stamping ------------

TEST(ChangeDetectionChunkVersion, EveryCreatePathStampsAllColumnsWithCurrentTick)
{
    Astra::Registry reg;
    AdvanceTo(reg, 5);

    auto a = reg.CreateEntity<Position, Velocity>();
    EXPECT_EQ(VersionOf<Position>(reg, a), 5u);
    EXPECT_EQ(VersionOf<Velocity>(reg, a), 5u);

    AdvanceTo(reg, 6);
    auto b = reg.CreateEntityWith(Position{1, 2, 3}, Velocity{});
    EXPECT_EQ(VersionOf<Position>(reg, b), 6u);
    EXPECT_EQ(VersionOf<Velocity>(reg, b), 6u);

    AdvanceTo(reg, 7);
    std::vector<Astra::Entity> batch(64);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(64, std::span{batch}), 64u);
    EXPECT_EQ(VersionOf<Position>(reg, batch.back()), 7u);

    AdvanceTo(reg, 8);
    std::vector<Astra::Entity> gen(64);
    ASSERT_EQ(reg.CreateEntitiesWith<Position, Velocity>(64, std::span{gen},
        [](size_t i) { return std::tuple{Position{float(i), 0, 0}, Velocity{}}; }), 64u);
    EXPECT_EQ(VersionOf<Velocity>(reg, gen.front()), 8u);
    EXPECT_EQ(VersionOf<Velocity>(reg, gen.back()), 8u);
}

TEST(ChangeDetectionChunkVersion, AddAndRemoveComponentStampTheDestinationChunk)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position>();
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.AddComponent<Velocity>(e, Velocity{1, 1, 1}));
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 3u);   // the new column
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);   // the carried column: destination chunk stamped

    AdvanceTo(reg, 4);
    ASSERT_TRUE(reg.EmplaceComponent<Health>(e, 10, 10));
    EXPECT_EQ(VersionOf<Health>(reg, e), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);

    AdvanceTo(reg, 5);
    ASSERT_TRUE(reg.RemoveComponent<Health>(e));
    EXPECT_EQ(VersionOf<Position>(reg, e), 5u);   // remove is a move too: destination stamped
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 5u);
}

TEST(ChangeDetectionChunkVersion, BatchAddAndDeferredAddStampTheDestinationChunk)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(32);
    ASSERT_EQ(reg.CreateEntities<Position>(32, std::span{ents}), 32u);

    AdvanceTo(reg, 3);
    reg.AddComponents<Velocity>(std::span{ents}, Velocity{});   // batch move path
    EXPECT_EQ(VersionOf<Velocity>(reg, ents[0]), 3u);
    EXPECT_EQ(VersionOf<Position>(reg, ents[31]), 3u);

    AdvanceTo(reg, 4);
    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(ents[0], Health{5, 5});                    // AddComponentByID at flush
    cmd.Execute();
    EXPECT_EQ(VersionOf<Health>(reg, ents[0]), 4u);
    EXPECT_EQ(VersionOf<Position>(reg, ents[0]), 4u);
}

TEST(ChangeDetectionChunkVersion, CompactionCarriesTheNewerVersionNotZeroNotNow)
{
    Astra::Registry reg;
    AdvanceTo(reg, 9);
    std::vector<Astra::Entity> ents(3000);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(3000, std::span{ents}), 3000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u) << "need >1 chunk for compaction to run";

    // Destroy 70% so the archetype crosses the 0.5 fragmentation threshold.
    AdvanceTo(reg, 10);
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < ents.size(); ++i) if (i % 10 < 7) doomed.push_back(ents[i]);
    reg.DestroyEntities(std::span{doomed});

    AdvanceTo(reg, 11);
    auto res = reg.Defragment();
    ASSERT_GT(res.entitiesMoved, 0u) << "compaction did not run; the test setup must fragment harder";

    for (size_t i = 0; i < ents.size(); ++i)
    {
        if (i % 10 < 7) continue;
        EXPECT_EQ(VersionOf<Position>(reg, ents[i]), 9u);   // folded from the source chunk, not 0, not 11
    }
}

TEST(ChangeDetectionChunkVersion, DeserializeStampsEveryRestoredChunkWithTheLoadersTick)
{
    std::vector<std::byte> buffer;
    {
        Astra::Registry reg;
        AdvanceTo(reg, 40);
        std::vector<Astra::Entity> ents(500);
        ASSERT_EQ(reg.CreateEntities<Position, Velocity>(500, std::span{ents}), 500u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        buffer = std::move(*saved.GetValue());
    }
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<Position, Velocity>();
    auto loaded = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg = **loaded.GetValue();
    EXPECT_EQ(reg.CurrentTick(), 1u);   // a fresh registry: nothing tick-related is serialized

    size_t seen = 0;
    reg.CreateView<const Position>().ForEach([&](Astra::Entity e, const Position&)
    {
        ++seen;
        EXPECT_EQ(VersionOf<Position>(reg, e), 1u);   // everything reads as changed-since-never once
        EXPECT_EQ(VersionOf<Velocity>(reg, e), 1u);
    });
    EXPECT_EQ(seen, 500u);
}

TEST(ChangeDetectionChunkVersion, EnableableArchetypeStillFitsItsCarve)
{
    // Net for the layout math: the precise fit loop runs for enableable archetypes
    // and must now account for the version region too. In Debug, InitializeColumns'
    // `offset <= m_chunkSize` assert is the tripwire; in Release, the population
    // simply must survive chunk growth.
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(20000);
    ASSERT_EQ(reg.CreateEntities<Astra::Test::Timer, Position>(20000, std::span{ents}), 20000u);
    size_t n = 0;
    reg.CreateView<const Astra::Test::Timer, const Position>().ForEach([&](const Astra::Test::Timer&, const Position&) { ++n; });
    EXPECT_EQ(n, 20000u);
}
```

Append to `tests/Registry/ChunkPoolTest.cpp` (reuses its `MakeSingleColumnMeta()`):

```cpp
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
```

- [ ] **Step 2: Build Debug; expect compile errors** naming `GetColumnVersion`, `AddEntity(Entity, Tick)`.

- [ ] **Step 3: `ArchetypeChunk` storage + accessors.** In `include/Astra/Archetype/ArchetypeChunkPool.hpp`:

3a. Add `#include "../Core/Tick.hpp"` with the other Core includes.

3b. Move ctor: add `m_columnVersion(other.m_columnVersion)` to the initializer list (the pointer targets `m_memory`, which is taken over).

3c. Public accessors, placed right after the enableable block (`SetDisabled`):

```cpp
        // ===================== Change detection: chunk column versions =====================
        // One Tick per storage column (spec 2026-09-10 §3.2, coarse tier -- every
        // component pays 4 bytes per chunk per column). `column` is a storage-column
        // ORDINAL (m_meta->columns index), never a ComponentID. The region is carved
        // in the chunk arena right after the column data (see InitializeColumns) and
        // is zero-initialised: version 0 == "never stamped". Stamps are plain stores.

        ASTRA_NODISCARD ASTRA_FORCEINLINE Tick GetColumnVersion(int column) const noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columnVersion[column];
        }

        ASTRA_FORCEINLINE void StampColumn(int column, Tick tick) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            m_columnVersion[column] = tick;
        }

        // Every storage column at once: the structural-write stamp (a slot in this
        // chunk just received an entity -- create, move, deserialize).
        ASTRA_FORCEINLINE void StampAllColumns(Tick tick) noexcept
        {
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
                m_columnVersion[c] = tick;
        }

        // Same-archetype relocation carry (CompactChunks / MoveEntitiesBetweenChunks):
        // keep whichever of the two versions is newer so a recently-written entity
        // that gets repacked into a fresh chunk is never read as unchanged.
        ASTRA_FORCEINLINE void FoldColumnVersion(int column, Tick other) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            if (IsNewer(other, m_columnVersion[column]))
                m_columnVersion[column] = other;
        }

        // Inspector accessor: byte offset of the version region within the arena.
        ASTRA_NODISCARD size_t GetColumnVersionOffset() const
        {
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(m_columnVersion) -
                                       static_cast<const std::byte*>(m_memory));
        }
```

3d. Member, next to `Column m_columns[MAX_COMPONENTS]{};`:

```cpp
        Tick* m_columnVersion{nullptr};              // arena-carved, [0, columnCount) live; 0 == never
```

3e. `InitializeColumns()`: insert between the column loop and the "Second loop" (disabled words) so the region order is columns -> versions -> disabled words (later: -> tick columns):

```cpp
            // Change-detection (spec 2026-09-10 §3.2): one Tick per storage column,
            // 8-byte aligned, immediately after the column data. Chunk memory was just
            // zeroed, so every column is born at version 0 == "never stamped" for free.
            // Archetype::ComputeLayoutBytesForCapacity mirrors this carve byte-for-byte,
            // and Archetype::Initialize folds its upper bound into m_alignmentOverhead so
            // the conservative capacity estimate stays a guaranteed fit.
            offset = (offset + 7) & ~size_t(7);
            m_columnVersion = reinterpret_cast<Tick*>(static_cast<std::byte*>(m_memory) + offset);
            offset += static_cast<size_t>(m_meta->columnCount) * sizeof(Tick);
```

3f. Tick parameters on the three chunk-level entity-adding entry points:
- `size_t AddEntity(Entity entity, Tick tick = 1)` -- after the DefaultConstruct loop add `StampAllColumns(tick);`.
- `template<typename... Components> size_t AddEntityWithComponents(Entity entity, Tick tick, Components&&... components)` -- after the `ConstructComponentAt` fold add `StampAllColumns(tick);`. (The tick sits BEFORE the pack so the pack stays a trailing deduction.)
- `void BatchAddEntities(std::span<const Entity> entities, Tick tick = 1)` -- after `m_count += count;` add `StampAllColumns(tick);`.

The default `= 1` exists only for hand-built chunks in tests (`ChunkPoolTest`); every Archetype-level caller passes `Now()` explicitly (Step 4).

- [ ] **Step 4: `Archetype` tick source, layout math, and stamps.** In `include/Astra/Archetype/Archetype.hpp`:

4a. Members (next to `m_chunkPool`) and accessors (public, next to `SetComponentPool`):

```cpp
        // Change-detection time source: points at the owning ArchetypeManager's
        // counter (stable: the manager is non-movable). Null for a hand-built
        // archetype (tests), which then stamps with 1 -- "stamped at least once".
        const Tick* m_tickSource = nullptr;
```
```cpp
        void SetTickSource(const Tick* source) noexcept { m_tickSource = source; }
        ASTRA_NODISCARD Tick Now() const noexcept { return m_tickSource ? *m_tickSource : Tick{1}; }
```

4b. `Initialize(...)`: after `alignmentOverhead` is computed, add the version-region bound so the conservative estimate remains a guaranteed fit (the carve is `align8 + columnCount*4`, and `columnCount == nonEmptyComponents`):

```cpp
            // Change-detection version region (ArchetypeChunk::InitializeColumns):
            // 8-byte align slack + one Tick per storage column. Folded into the
            // overhead so ComputeCapacityForBytes' estimate stays a guaranteed fit.
            alignmentOverhead += 8 + nonEmptyComponents * sizeof(Tick);
```

4c. `ComputeLayoutBytesForCapacity(cap)`: between the column loop and the `words` computation:

```cpp
            offset = (offset + 7) & ~size_t(7);                                  // version region
            offset += static_cast<size_t>(m_columnMeta.columnCount) * sizeof(Tick);
```

`ChunkBytesToHold` needs no change (its `m_alignmentOverhead` term now includes the bound).

4d. Structural-write stamps -- every site that claims a slot:
- `AddEntity`: lambda body becomes `return chunk->AddEntity(e, Now());`.
- `AddEntityWith`: `return chunk->AddEntityWithComponents(e, Now(), std::forward<Components>(components)...);`.
- `AddEntities`: `chunk->BatchAddEntities(entities.subspan(entityIndex, toAdd), Now());`.
- `AddEntitiesWith`: right after `chunk->SetCount(startSlot + runLen);` add `chunk->StampAllColumns(Now());`.
- `AllocateEntitySlot`: right after `chunk->SetCount(entityIndex + 1);` add `chunk->StampAllColumns(Now());   // the destination of every single-entity move (MoveEntityFrom/MoveAndAdd/MoveAndAddByID) is stamped here`.
- `BatchMoveEntitiesFrom`: right after `chunk->SetCount(chunk->GetCount() + toAdd);` add `chunk->StampAllColumns(Now());`.
- `Deserialize`: new trailing parameter `const Tick* tickSource = nullptr`; after `archetype->m_chunkPool = componentPool;` add `archetype->SetTickSource(tickSource);`; the entity loop becomes `chunk->AddEntity(entity, archetype->Now());`. This IS the spec's "a load stamps every chunk changed": each restored chunk's columns carry the loading registry's current tick.

4e. Same-archetype carries:
- `CompactChunks`: after the per-column move loop inside the `while (srcIndex < srcCount)` run loop (i.e. once per run, after the `for (uint16_t c ...)` loop ends), add:

```cpp
                    // Change-detection carry (plan deviation 4): the fresh dst chunk is at
                    // version 0; fold in the source chunk's version per column so a recent
                    // write is not lost by repacking. Ordinals match (same archetype).
                    for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
                        dst->FoldColumnVersion(c, src->GetColumnVersion(c));
```
- `MoveEntitiesBetweenChunks`: inside the per-column loop, after the enableable carry, add `destChunk->FoldColumnVersion(c, srcChunk->GetColumnVersion(c));`.

- [ ] **Step 5: Wire the tick source in `ArchetypeManager`.** In `include/Astra/Archetype/ArchetypeManager.hpp`:
- Constructor: after `m_rootArchetype->m_chunkPool = &m_chunkPool;` add `m_rootArchetype->SetTickSource(&m_tick);`.
- `GetOrCreateArchetype`: after `ptr->m_chunkPool = &m_chunkPool;` add `ptr->SetTickSource(&m_tick);`.
- `Deserialize`: `Archetype::Deserialize(reader, registryDescriptors, &m_chunkPool, &m_tick)`.

`m_tick` is declared after `m_chunkPool` in the member list; taking its address in the constructor body is fine (all members are constructed by then).

- [ ] **Step 6: Build Debug; run `AstraTest.exe --gtest_filter=ChangeDetection*:ChunkPoolTest.*`.** Expected: all PASS. If `CompactionCarriesTheNewerVersionNotZeroNotNow` reports `entitiesMoved == 0`, raise the destroy ratio (the archetype must exceed the default 0.5 fragmentation threshold and hold >1 chunk); do not change the assertion on version 9.

- [ ] **Step 7: Full suite, all three configs.** Expected: baseline + 7. Pay attention to `ArchetypeTest`/`ChunkPoolTest` capacity assertions: `alignmentOverhead` grew by `8 + 4*columnCount` bytes, so a chunk's derived capacity may drop by at most one entity for tiny components; any test that hard-codes a capacity must be re-derived from the layout math and the change reported (none is expected -- `ArchetypeTest.cpp:84` compares against the chunk's own capacity, `ChunkPoolTest.cpp:38` passes the capacity explicitly).

- [ ] **Step 8: Commit.**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp tests/Registry/ChangeDetectionTest.cpp tests/Registry/ChunkPoolTest.cpp
git commit -m "feat(archetype): per-column chunk versions carved in the arena; every structural write stamps its destination (Stage 3 task 2)"
```

---
## Task 3: View-entry stamping and the Registry accessor write side (OPUS)

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`ForEachStamped`, `ForEachChunkStamped`, shared `ForEachBody`)
- Modify: `include/Astra/Registry/View.hpp` (mutable-column resolution, stamps in `ForEachImpl`/`ForEachWithOptional`/`ParallelForEachChunkImpl`/`ParallelForEachChunkWithOptional`/`VisitChunkFiltered`/`Get`/`begin`)
- Modify: `include/Astra/Registry/ViewIterator.hpp` (`Iterator` takes a `Tick`, stamps in `CacheChunkState`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`GetComponentMut<T>`, `MarkWritten`)
- Modify: `include/Astra/Registry/Registry.hpp` (non-const `GetComponent<T>` -> `GetComponentMut`; `Modified<T>`; `SetIfNeq<T>`)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append)

**Interfaces:**
- Consumes: `ArchetypeChunk::StampColumn`, `Archetype::Now()`, `ArchetypeManager::CurrentTick()` (Tasks 1-2).
- Produces:
  - `Detail::IsMutableYield<C>` (constexpr bool: `!std::is_const_v<C> && !std::is_empty_v<std::remove_const_t<C>>`), in `Query.hpp`'s `Detail` namespace.
  - `Archetype::ForEachStamped<Components...>(Func&&)` / `ForEachChunkStamped<Components...>(size_t chunkIndex, Func&&)` -- identical loops to `ForEach`/`ForEachChunk` plus a stamp of every mutable-yield column per visited chunk.
  - `ArchetypeManager::GetComponentMut<T>(Entity) -> T*` (stamps `T`'s column; Task 7 adds the per-entity mark) and `ArchetypeManager::MarkWritten(const EntityRecord* rec, ComponentID id) -> bool` (stamps; false if the record does not hold `id` as a storage column).
  - `Registry::Modified<T>(Entity) -> bool`, `Registry::SetIfNeq<T>(Entity, const T&) -> bool` (requires `std::equality_comparable<T>`).
  - `ViewIterable<...>::Iterator(Archetype* const*, size_t, Tick now)`.
- Behavioural rule (spec §3.3 table): a non-const yield of `T` stamps `T`'s column in every chunk the view visits, whether or not the callback writes; a const yield never stamps; `Get`/`Single` stamp the one entity's chunk for each non-const requested component; non-const `Registry::GetComponent<T>` stamps, the const overload does not; `Modified<T>` stamps; `SetIfNeq<T>` stamps only when `!=`.

- [ ] **Step 1: Write the failing tests.** Append to `tests/Registry/ChangeDetectionTest.cpp`:

```cpp
// ---- Task 3: view-entry stamping + Registry accessor write side ------------

TEST(ChangeDetectionStamp, NonConstViewStampsEveryVisitedChunkConstViewStampsNone)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(2000, std::span{ents}), 2000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u);

    AdvanceTo(reg, 3);
    reg.CreateView<const Position, const Velocity>().ForEach([](const Position&, const Velocity&) {});
    for (auto& chunk : arch->GetChunks())
    {
        const auto& cm = arch->GetColumnMeta();
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Position>::Value()]), 2u);   // untouched
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Velocity>::Value()]), 2u);
    }

    AdvanceTo(reg, 4);
    reg.CreateView<Position, const Velocity>().ForEach([](Position&, const Velocity&) { /* writes nothing */ });
    for (auto& chunk : arch->GetChunks())
    {
        const auto& cm = arch->GetColumnMeta();
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Position>::Value()]), 4u);   // stamped by access, not by writing
        EXPECT_EQ(chunk->GetColumnVersion(cm.idToColumn[Astra::TypeID<Velocity>::Value()]), 2u);   // const: untouched
    }

    AdvanceTo(reg, 5);
    reg.CreateView<const Position, Velocity>().ParallelForEach([](const Position&, Velocity&) {});
    for (auto& chunk : arch->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(arch->GetColumnMeta().idToColumn[Astra::TypeID<Velocity>::Value()]), 5u);

    AdvanceTo(reg, 6);
    for (auto [e, p] : reg.CreateView<Position>()) { (void)e; (void)p; }   // range-for stamps too
    for (auto& chunk : arch->GetChunks())
        EXPECT_EQ(chunk->GetColumnVersion(arch->GetColumnMeta().idToColumn[Astra::TypeID<Position>::Value()]), 6u);
}

TEST(ChangeDetectionStamp, OptionalAndEnabledFilteredPathsStampMutableColumns)
{
    using EnA = Astra::Test::Hierarchy;   // enableable: forces the filtered path
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position, Velocity, EnA>();
    auto b = reg.CreateEntity<Position, EnA>();
    (void)b;

    AdvanceTo(reg, 3);
    reg.CreateView<const Position, Astra::Optional<Velocity>, Astra::With<EnA>>().ForEach(
        [](const Position&, Velocity*) {});
    EXPECT_EQ(VersionOf<Velocity>(reg, a), 3u);   // present optional, non-const: stamped
    EXPECT_EQ(VersionOf<Position>(reg, a), 2u);   // const required: not

    AdvanceTo(reg, 4);
    reg.CreateView<Position, EnA>().ForEach([](Position&, EnA&) {});   // enabled-filtered path
    EXPECT_EQ(VersionOf<Position>(reg, a), 4u);
    EXPECT_EQ(VersionOf<EnA>(reg, a), 4u);
}

TEST(ChangeDetectionStamp, GetAndSingleStampOnlyNonConstRequests)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position, Velocity>();

    AdvanceTo(reg, 3);
    auto vRead = reg.CreateView<const Position, const Velocity>();
    ASSERT_TRUE(vRead.Get(e).IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    auto vWrite = reg.CreateView<Position, const Velocity>();
    ASSERT_TRUE(vWrite.Get(e).IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_EQ(VersionOf<Velocity>(reg, e), 2u);

    AdvanceTo(reg, 4);
    ASSERT_TRUE(vWrite.Single().IsOk());
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);
}

TEST(ChangeDetectionStamp, RegistryGetComponentNonConstStampsConstDoesNot)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<Position>();

    AdvanceTo(reg, 3);
    const Astra::Registry& creg = reg;
    ASSERT_NE(creg.GetComponent<Position>(e), nullptr);
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);

    ASSERT_NE(reg.GetComponent<Position>(e), nullptr);
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
}

TEST(ChangeDetectionStamp, ModifiedStampsAndSetIfNeqStampsOnlyOnInequality)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(Position{1, 2, 3});

    AdvanceTo(reg, 3);
    EXPECT_TRUE(reg.Modified<Position>(e));
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_FALSE(reg.Modified<Velocity>(e));               // absent component: false, nothing stamped
    EXPECT_FALSE(reg.Modified<Position>(Astra::Entity{}));   // invalid handle: false

    AdvanceTo(reg, 4);
    EXPECT_FALSE(reg.SetIfNeq<Health>(e, Health{1, 1}));     // absent: false
    EXPECT_FALSE(reg.SetIfNeq<Position>(e, Position{1, 2, 3}));   // equal: no store, no stamp
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_TRUE(reg.SetIfNeq<Position>(e, Position{9, 2, 3}));    // different: stored + stamped
    EXPECT_EQ(VersionOf<Position>(reg, e), 4u);
    EXPECT_FLOAT_EQ(std::as_const(reg).GetComponent<Position>(e)->x, 9.0f);
}
```

`Position` has no `operator==` today; `SetIfNeq` requires `std::equality_comparable<T>`. Add to `tests/TestComponents.hpp` inside `struct Position`: `bool operator==(const Position&) const = default;` (a defaulted comparison adds no TypeID and changes no layout). Do the same for `Health` (used above only to exercise the absent path -- the requires-clause still needs it comparable).

- [ ] **Step 2: Build Debug; expect compile errors** on `Modified`, `SetIfNeq`, and `ViewIterable::Iterator`'s new argument.

- [ ] **Step 3: `Detail::IsMutableYield` in `Query.hpp`** (inside `namespace Detail`, above `ArgAccess`):

```cpp
        // A yielded component whose access is non-const and which has storage. Only
        // these columns get the coarse change-detection stamp when a view enters a
        // chunk (spec 2026-09-10 §2.3): const yields never stamp, tags have no column.
        template<typename C>
        inline constexpr bool IsMutableYield = !std::is_const_v<C> && !std::is_empty_v<std::remove_const_t<C>>;
```

- [ ] **Step 4: `Archetype::ForEachStamped` / `ForEachChunkStamped`.** In `Archetype.hpp`, make the existing `ForEach`/`ForEachChunk` thin wrappers over a private `ForEachBody<Stamp, Components...>` so the two loops cannot drift:

```cpp
        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func) { ForEachBody<false, Components...>(std::forward<Func>(func)); }

        // Same loop as ForEach, plus the coarse change-detection stamp: every
        // mutable-yield column (non-const, has storage) of every visited chunk is
        // stamped with Now() BEFORE the chunk is iterated (spec §3.3 row 1). A view
        // whose yields are all const instantiates the plain loop (empty column list).
        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEachStamped(Func&& func) { ForEachBody<true, Components...>(std::forward<Func>(func)); }

        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEachChunk(size_t chunkIndex, Func&& func) { ForEachChunkBody<false, Components...>(chunkIndex, std::forward<Func>(func)); }

        template<Component... Components, std::invocable<Entity, Components&...> Func>
        ASTRA_FORCEINLINE void ForEachChunkStamped(size_t chunkIndex, Func&& func) { ForEachChunkBody<true, Components...>(chunkIndex, std::forward<Func>(func)); }
```

Private helpers (place next to `ForEachImpl`):

```cpp
        // Column ordinals of the mutable-yield components, resolved once per call.
        // -1 for a const or tag component (skipped by StampMutableColumns).
        template<typename... Components>
        ASTRA_FORCEINLINE void ResolveMutableColumns(int* cols) const noexcept
        {
            size_t i = 0;
            ((cols[i++] = Detail::IsMutableYield<Components>
                ? m_columnMeta.idToColumn[TypeID<std::remove_const_t<Components>>::Value()]
                : -1), ...);
        }

        template<size_t N>
        ASTRA_FORCEINLINE static void StampMutableColumns(ArchetypeChunk* chunk, const int (&cols)[N], Tick now) noexcept
        {
            for (size_t i = 0; i < N; ++i)
                if (cols[i] >= 0) chunk->StampColumn(cols[i], now);
        }

        template<bool Stamp, Component... Components, typename Func>
        ASTRA_FORCEINLINE void ForEachBody(Func&& func)
        {
            if (m_entityCount == 0 || m_chunks.empty()) ASTRA_UNLIKELY
                return;

            constexpr bool AnyMutable = (Detail::IsMutableYield<Components> || ...);
            [[maybe_unused]] int cols[sizeof...(Components) == 0 ? 1 : sizeof...(Components)];
            [[maybe_unused]] Tick now = 0;
            if constexpr (Stamp && AnyMutable)
            {
                ResolveMutableColumns<Components...>(cols);
                now = Now();
            }

            const size_t numChunks = m_chunks.size();
            for (size_t i = 0; i < numChunks; ++i)
            {
                auto& chunk = m_chunks[i];
                const size_t count = chunk->GetCount();
                if (count == 0) ASTRA_UNLIKELY
                    continue;

                // (existing prefetch block, verbatim)

                if constexpr (Stamp && AnyMutable)
                    StampMutableColumns(chunk.get(), cols, now);

                ForEachImpl<Components...>(chunk.get(), count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
            }
        }

        template<bool Stamp, Component... Components, typename Func>
        ASTRA_FORCEINLINE void ForEachChunkBody(size_t chunkIndex, Func&& func)
        {
            if (chunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                return;
            auto& chunk = m_chunks[chunkIndex];
            const size_t count = chunk->GetCount();
            if (count == 0) ASTRA_UNLIKELY
                return;

            constexpr bool AnyMutable = (Detail::IsMutableYield<Components> || ...);
            if constexpr (Stamp && AnyMutable)
            {
                int cols[sizeof...(Components)];
                ResolveMutableColumns<Components...>(cols);
                StampMutableColumns(chunk.get(), cols, Now());
            }
            ForEachImpl<Components...>(chunk.get(), count, std::forward<Func>(func), std::index_sequence_for<Components...>{});
        }
```

The `(... || ...)` fold with an empty pack is `false` -- fine. `Archetype.hpp` already includes `Component.hpp`; `Detail::IsMutableYield` lives in `Query.hpp`, which `Archetype.hpp` does not include -- move the trait into `Component.hpp` (same `Detail` namespace) instead of `Query.hpp` if the include order bites; report which.

- [ ] **Step 5: View stamps.** In `View.hpp`:

5a. `ForEachImpl` / `ParallelForEachChunkImpl` no-optional fast path: call `archetype->ForEachStamped<Required...>(...)` / `archetype->ForEachChunkStamped<Required...>(chunkIndex, ...)` instead of `ForEach`/`ForEachChunk`.

5b. Add a private helper used by the three View-owned chunk loops:

```cpp
        // Coarse change-detection stamp for a chunk this view is about to iterate:
        // every non-const required column, plus every non-const optional column that
        // is PRESENT on this archetype. Compiles to nothing for an all-const view.
        template<size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void StampChunkForYields(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm,
                                                   const std::array<bool, sizeof...(OptIs)>& hasOptional, Tick now,
                                                   std::index_sequence<ReqIs...>, std::index_sequence<OptIs...>) const
        {
            ((Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>>
                ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()], now)
                : void()), ...);
            (((Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && hasOptional[OptIs])
                ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()], now)
                : void()), ...);
        }
```

Use `if constexpr` inside small per-index helpers if MSVC rejects the ternary-with-void shape. Call it in `ForEachWithOptional` and `ParallelForEachChunkWithOptional` right after the `count == 0` check (with `const Tick now = m_archetypeManager->CurrentTick();` hoisted above the chunk loop and `const auto& cm = archetype->GetColumnMeta();`), and in `VisitChunkFiltered` right after `cm` is fetched (add a `Tick now` parameter to `VisitChunkFiltered`, hoisted by its two callers).

5c. `Get(Entity)`: after `VisibleRecord` succeeds and before `MakeAccessTuple`, stamp: build `hasOptional` from `rec->archetype->HasComponent<...>()` and call `StampChunkForYields(rec->chunk, rec->archetype->GetColumnMeta(), hasOptional, m_archetypeManager->CurrentTick(), ...)`. `Single()` reuses `Get`.

5d. `begin()`: `return Iterator(m_archetypes.data(), m_archetypes.size(), m_archetypeManager->CurrentTick());`.

- [ ] **Step 6: `ViewIterator.hpp`.** `Iterator(Archetype* const* archetypes, size_t archetypeCount, Tick now = 1)` storing `Tick m_now = 1;`. In `CacheChunkState`, before the component-array capture:

```cpp
                // Coarse change-detection stamp (spec §3.3 row 1): range-for yields
                // references like ForEach, so every non-const component column is
                // stamped on chunk entry. All-const iteration compiles to nothing.
                if constexpr ((Detail::IsMutableYield<Components> || ...))
                {
                    const ArchetypeColumnMeta& cm = archetype->GetColumnMeta();
                    ((Detail::IsMutableYield<Components>
                        ? chunk->StampColumn(cm.idToColumn[TypeID<std::remove_const_t<Components>>::Value()], m_now)
                        : void()), ...);
                }
```

Include `../Core/Tick.hpp` and whichever header now owns `IsMutableYield`.

- [ ] **Step 7: Registry accessor write side.** In `ArchetypeManager.hpp` (public, next to `GetComponent<T>(Entity)`):

```cpp
        // Non-const component access IS a write for change detection (spec §3.3):
        // stamp T's column in the entity's chunk. Task 7 adds the per-entity mark
        // for change-tracked T. Same validation and result as GetComponent<T>.
        template<Component T>
        ASTRA_NODISCARD T* GetComponentMut(Entity entity)
        {
            T* ptr = GetComponent<T>(entity);
            if constexpr (!std::is_empty_v<T>)
            {
                if (ptr) ASTRA_LIKELY
                {
                    const EntityRecord* rec = m_records->GetRecord(entity.GetID());   // validated by GetComponent above
                    rec->chunk->StampColumn(rec->archetype->GetColumnMeta().idToColumn[TypeID<T>::Value()], m_tick);
                }
            }
            return ptr;
        }

        // Explicit "I wrote T on this entity" for raw-pointer code (Registry::Modified).
        // Returns false for a stale handle, an absent component, or a tag (no column).
        bool MarkWritten(Entity entity, ComponentID id)
        {
            const EntityRecord* rec = GetEntityRecord(entity);
            if (!rec || !rec->chunk || id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return false;
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0) ASTRA_UNLIKELY
                return false;
            rec->chunk->StampColumn(col, m_tick);
            return true;
        }
```

In `Registry.hpp`: the non-const `GetComponent<T>` body becomes `return m_archetypeManager->GetComponentMut<T>(entity);` (const overload unchanged). Add after `HasComponent`:

```cpp
        // ---- Change detection, explicit write side (spec §3.3) ----
        // For code that holds a raw T* across frames: declare the write. Stamps T's
        // chunk column (and marks the entity for a change-tracked T -- Task 7).
        template<Component T>
        bool Modified(Entity entity)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            return m_archetypeManager->MarkWritten(entity, TypeID<T>::Value());
        }

        // Compare-then-store opt-in: assigns and stamps ONLY when `value != current`.
        // Returns true iff a store happened. Deliberately not the default mark path
        // (the compare costs more than the store it skips -- spec §2.4).
        template<Component T>
        requires std::equality_comparable<T>
        bool SetIfNeq(Entity entity, const T& value)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            T* current = m_archetypeManager->GetComponent<T>(entity);   // non-stamping fetch
            if (!current) return false;
            if (*current == value) return false;
            *current = value;
            m_archetypeManager->MarkWritten(entity, TypeID<T>::Value());
            return true;
        }
```

- [ ] **Step 8: Build Debug; run `--gtest_filter=ChangeDetection*`.** Expected PASS. Then the full suite in all three configs: baseline + 12 (Tasks 1-3). Watch `ViewTest`/`ViewEnrichmentTest`/`ParallelIterationTest` for anything that asserted a view leaves chunk memory untouched -- none is expected.

- [ ] **Step 9: Commit.**

```bash
git add include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp include/Astra/Registry/ViewIterator.hpp include/Astra/Registry/Registry.hpp include/Astra/Component/Component.hpp tests/TestComponents.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(view): non-const access stamps chunk versions on entry; Registry Modified/SetIfNeq and stamping GetComponent (Stage 3 task 3)"
```

---

## Task 4: `Changed<T>` / `Added<T>` filters with chunk reject, `Since`, and `ForEach(ctx, fn)` (OPUS)

**Files:**
- Modify: `include/Astra/Registry/Query.hpp` (modifier structs, `IsModifier`/`ExtractComponent`/`ArgAccess` specialisations, classifier lists, `GetChangeMask`, `Matches`, `ChangeTermsAreRequired`)
- Modify: `include/Astra/Registry/View.hpp` (`HasChangeFilter`/`HasChunkFilter`, tick-taking entry points, `SinceView`, chunk reject, refusals)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append)

**Interfaces:**
- Consumes: `Tick`, `IsNewer`, `TickContext` (Task 1); `ArchetypeChunk::GetColumnVersion` (Task 2); `VisitChunkFiltered(..., Tick now, ...)` (Task 3).
- Produces:
  - `template<typename T> struct Astra::Changed;` `template<typename T> struct Astra::Added;` -- match-only, zero access footprint, refused for tags.
  - `QueryBuilder::GetChangeMask()`; `Matches` requires Required | With | Change.
  - `View::HasChangeFilter` (public `static constexpr bool`), `View::ForEach(const Ctx&, Func&&)` / `View::ParallelForEach(const Ctx&, Func&&)` for any `TickContext Ctx`; `View::Since(Tick) -> SinceView`, with `SinceView::ForEach(Func&&)` / `SinceView::ParallelForEach(Func&&)`; `View::ParallelForEachWithContext(Tick since, Factory&&, Body&&)` (Task 5's `SystemContext::ParallelForEach` calls this).
  - Compile-time refusals (static_assert): `ForEach(fn)`/`ParallelForEach(fn)`/`ParallelForEachWithContext(factory, body)`/`Size`/`Empty`/`Contains`/`Get`/`Single`/`begin`/`end` on a change-filtered view; `Changed<T>` whose `T` is not also a required component of the view.
- Semantics at chunk granularity (this task; Task 8 adds the per-entity tier): a chunk passes iff for EVERY `Changed<T>`/`Added<T>` term `IsNewer(version[T], since)`; a passing chunk yields every (enabled) entity.

- [ ] **Step 1: Write the failing tests.** Append to `tests/Registry/ChangeDetectionTest.cpp`:

```cpp
// ---- Task 4: Changed<T>/Added<T> at chunk granularity ----------------------

namespace
{
    struct FakeCtx { Tick last; Tick LastRun() const noexcept { return last; } };
    static_assert(Astra::TickContext<FakeCtx>);

    template<typename V>
    size_t CountSince(V& view, Tick since)
    {
        size_t n = 0;
        view.Since(since).ForEach([&](auto&&...) { ++n; });
        return n;
    }
}

TEST(ChangeDetectionFilter, ChangedYieldsStampedChunksAndSkipsUnstamped)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(2000, std::span{ents}), 2000u);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    ASSERT_GT(arch->GetChunks().size(), 1u);

    auto changed = reg.CreateView<const Position, Astra::Changed<Position>>();

    // First run: since 0 sees everything (create stamped every chunk at tick 2).
    EXPECT_EQ(CountSince(changed, 0), 2000u);
    // Since the creation tick itself: nothing is newer than 2.
    EXPECT_EQ(CountSince(changed, 2), 0u);

    // Write exactly one entity at tick 3: its whole CHUNK reads as changed (chunk granularity).
    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.Modified<Position>(ents[0]));
    const size_t chunk0Count = arch->GetChunks()[0]->GetCount();
    EXPECT_EQ(CountSince(changed, 2), chunk0Count);
    EXPECT_EQ(CountSince(changed, 3), 0u);

    // A non-const view over Position at tick 4 stamps every chunk -> everything changed since 3.
    AdvanceTo(reg, 4);
    reg.CreateView<Position>().ForEach([](Position&) {});
    EXPECT_EQ(CountSince(changed, 3), 2000u);
    EXPECT_EQ(CountSince(changed, 4), 0u);
}

TEST(ChangeDetectionFilter, ForEachWithContextUsesLastRunAndParallelMatchesSerial)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(2000, std::span{ents}), 2000u);
    AdvanceTo(reg, 3);
    reg.CreateView<Velocity>().ForEach([](Velocity&) {});   // stamps Velocity everywhere at 3

    auto v = reg.CreateView<const Position, const Velocity, Astra::Changed<Velocity>>();
    size_t serial = 0, parallel = 0;
    v.ForEach(FakeCtx{2}, [&](const Position&, const Velocity&) { ++serial; });
    v.ParallelForEach(FakeCtx{2}, [&](const Position&, const Velocity&) { ++parallel; });
    EXPECT_EQ(serial, 2000u);
    EXPECT_EQ(parallel, 2000u);

    serial = 0;
    v.ForEach(FakeCtx{3}, [&](const Position&, const Velocity&) { ++serial; });
    EXPECT_EQ(serial, 0u);
}

TEST(ChangeDetectionFilter, AddedIsChunkGranularForUntrackedTypesAndCombinesWithNotWith)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto a = reg.CreateEntity<Position>();
    auto b = reg.CreateEntity<Position, Health>();
    (void)a;

    AdvanceTo(reg, 3);
    ASSERT_TRUE(reg.AddComponent<Velocity>(b, Velocity{}));   // b moves into {Position, Health, Velocity} at 3

    // Added<Velocity>: b's new chunk was stamped at 3.
    auto added = reg.CreateView<const Velocity, Astra::Added<Velocity>>();
    EXPECT_EQ(CountSince(added, 2), 1u);
    EXPECT_EQ(CountSince(added, 3), 0u);

    // Filters compose with Not/With and the enabled filter's chunk skip.
    auto composed = reg.CreateView<const Position, Astra::Changed<Position>, Astra::With<Velocity>, Astra::Not<Astra::Test::Name>>();
    EXPECT_EQ(CountSince(composed, 2), 1u);   // only b (a has no Velocity)
}

TEST(ChangeDetectionFilter, ChangedTermCountsTowardArchetypeMatchingAndAccess)
{
    // Changed<T> requires T for MATCHING (like With) but adds nothing to ViewAccess.
    using V = Astra::View<const Position, Astra::Changed<Position>, Astra::Added<Position>>;
    static_assert(V::HasChangeFilter);
    static_assert(std::tuple_size_v<Astra::ViewAccess<V>::Writes> == 0);
    static_assert(std::tuple_size_v<Astra::ViewAccess<V>::Reads> == 1);   // from `const Position` only
    using Plain = Astra::View<const Position>;
    static_assert(!Plain::HasChangeFilter);
    SUCCEED();
}
```

- [ ] **Step 2: Build Debug; expect compile errors** naming `Astra::Changed`, `Since`, `HasChangeFilter`.

- [ ] **Step 3: `Query.hpp` plumbing.**

3a. Forward-declare `template<typename T> struct Changed; template<typename T> struct Added;` with the other forward declarations. Add `IsModifier` and `ExtractComponent` specialisations for both (same shape as `With<T>`). Add `ArgAccess<Changed<T>>` / `ArgAccess<Added<T>>` with empty `Read`/`Write` (same as `With`).

3b. In `QueryClassifier`: `using ChangedComponents = typename FilterByModifier<Changed, QueryArgs...>::type; using AddedComponents = typename FilterByModifier<Added, QueryArgs...>::type;`.

3c. The structs, next to `With`:

```cpp
    // Change-detection filters (spec 2026-09-10 §3.4). Match-only like With<T>: T must
    // be present, T is NOT yielded, zero ViewAccess footprint. A chunk passes iff T's
    // column version is newer than the querying tick ("since"); for a change-tracked T
    // (AstraChangeTracked) the per-entity ticks are then scanned too. Iteration-only
    // this stage: use ForEach(ctx, fn) / Since(tick).ForEach(fn). T must ALSO be listed
    // as a required component of the same view (enforced in View).
    template<typename T>
    struct Changed
    {
        static_assert(Component<T>, "Changed can only be used with valid components");
        static_assert(!std::is_empty_v<T>, "Changed<T>: a tag has no storage column and therefore no version to compare; "
                                           "track a non-empty component instead");
    };
    template<typename T>
    struct Added
    {
        static_assert(Component<T>, "Added can only be used with valid components");
        static_assert(!std::is_empty_v<T>, "Added<T>: a tag has no storage column and therefore no version to compare");
    };
```

3d. `QueryBuilder`: add `static ComponentMask GetChangeMask() { return MakeMaskFromTuple<typename Classifier::ChangedComponents>() | MakeMaskFromTuple<typename Classifier::AddedComponents>(); }` and in `Matches` use `GetRequiredMask() | GetWithMask() | GetChangeMask()`.

3e. `Detail::ChangeTermsAreRequired<QueryArgs...>`: true iff every `T` in `ChangedComponents`/`AddedComponents` appears (modulo const) in `RequiredComponents`:

```cpp
        template<typename T, typename Tuple> struct TupleContainsBare;
        template<typename T, typename... Us>
        struct TupleContainsBare<T, std::tuple<Us...>>
            : std::bool_constant<(std::is_same_v<std::remove_const_t<T>, std::remove_const_t<Us>> || ...)> {};

        template<typename ReqTuple, typename TermTuple> struct AllTermsIn;
        template<typename ReqTuple, typename... Ts>
        struct AllTermsIn<ReqTuple, std::tuple<Ts...>>
            : std::bool_constant<(TupleContainsBare<Ts, ReqTuple>::value && ...)> {};

        template<typename... QueryArgs>
        inline constexpr bool ChangeTermsAreRequired =
            AllTermsIn<typename QueryClassifier<QueryArgs...>::RequiredComponents, typename QueryClassifier<QueryArgs...>::ChangedComponents>::value &&
            AllTermsIn<typename QueryClassifier<QueryArgs...>::RequiredComponents, typename QueryClassifier<QueryArgs...>::AddedComponents>::value;
```

- [ ] **Step 4: `View.hpp`.**

4a. Type sets and gates, after `HasEnabledFilter`:

```cpp
        // ================= Change-detection filters (spec 2026-09-10 §3.4) =================
        using ChangedTypes = typename Detail::QueryClassifier<QueryArgs...>::ChangedComponents;
        using AddedTypes   = typename Detail::QueryClassifier<QueryArgs...>::AddedComponents;
        static_assert(Detail::ChangeTermsAreRequired<QueryArgs...>,
            "Changed<T>/Added<T>: T must also be listed as a required component of this view "
            "(e.g. CreateView<const T, Changed<T>, ...>) -- filter what you fetch");
    public:
        static constexpr bool HasChangeFilter = (std::tuple_size_v<ChangedTypes> + std::tuple_size_v<AddedTypes>) > 0;
    private:
        // Any per-chunk filter at all: routes iteration through VisitChunkFiltered.
        static constexpr bool HasChunkFilter = HasEnabledFilter || HasChangeFilter;
```

Replace every `if constexpr (!HasEnabledFilter)` fast-path gate in `ForEachImpl`/`ParallelForEachChunkImpl` with `!HasChunkFilter`.

4b. Public entry points. Rename the existing bodies to private `ForEachSince(Tick since, Func&&)` / `ParallelForEachSince(Tick since, Func&&)` (thread `since` down to `VisitChunkFiltered`), then:

```cpp
        template<typename Func>
        ASTRA_FORCEINLINE void ForEach(Func&& func)
        {
            static_assert(!HasChangeFilter,
                "This view has a Changed<T>/Added<T> filter and needs a 'since' tick: call "
                "ForEach(ctx, fn) from a SystemContext system, or Since(tick).ForEach(fn).");
            ForEachSince(Tick{0}, std::forward<Func>(func));
        }
        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ForEach(const Ctx& ctx, Func&& func) { ForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        template<typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(Func&& func)
        {
            static_assert(!HasChangeFilter, "(same message as ForEach, naming ParallelForEach(ctx, fn) / Since(tick).ParallelForEach(fn))");
            ParallelForEachSince(Tick{0}, std::forward<Func>(func));
        }
        template<TickContext Ctx, typename Func>
        ASTRA_FORCEINLINE void ParallelForEach(const Ctx& ctx, Func&& func) { ParallelForEachSince(ctx.LastRun(), std::forward<Func>(func)); }

        // Explicit-tick form for Registry&-only systems and tests.
        class SinceView
        {
        public:
            SinceView(View& v, Tick since) noexcept : m_view(&v), m_since(since) {}
            template<typename Func> void ForEach(Func&& func)         { m_view->ForEachSince(m_since, std::forward<Func>(func)); }
            template<typename Func> void ParallelForEach(Func&& func) { m_view->ParallelForEachSince(m_since, std::forward<Func>(func)); }
        private:
            View* m_view;
            Tick  m_since;
        };
        ASTRA_NODISCARD SinceView Since(Tick since) noexcept { return SinceView(*this, since); }
```

`ParallelForEachWithContext(Factory&&, Body&&)`: static_assert `!HasChangeFilter` and delegate to a new `ParallelForEachWithContext(Tick since, Factory&&, Body&&)` that threads `since` to `ParallelForEachChunkImpl`. Inside `ParallelForEachSince`, the two `return ForEach(adapted)` fallbacks become `return ForEachSince(since, adapted)`.

4c. Iteration-only refusals: add `static_assert(!HasChangeFilter, "Changed<T>/Added<T> views are iteration-only this stage: Size/Empty/Contains/Get/Single/range-for need a tick; use ForEach(ctx, fn) or Since(tick).ForEach(fn)");` as the first line of `Size()`, `Empty()`, `Contains()`, `Get()`, `Single()`, `begin()`, `end()`. (Non-template members of a class template: the assert fires only when the member is used, exactly like the existing `begin()` guard.)

4d. Chunk reject in `VisitChunkFiltered` (add `Tick since` alongside the Task-3 `Tick now`), as the FIRST step after the `count == 0` check and `cm` fetch:

```cpp
            if constexpr (HasChangeFilter)
            {
                // Chunk reject first, always (spec §2.2): a chunk whose T column is not
                // newer than `since` for ANY Changed/Added term has nothing for us.
                if (!ChangeChunkPasses(chunk, cm, since)) ASTRA_UNLIKELY
                    return;
            }
```

with:

```cpp
        template<typename... Ts>
        ASTRA_FORCEINLINE static bool AllNewer(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since, std::tuple<Ts...>*) noexcept
        {
            return (IsNewer(chunk->GetColumnVersion(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), since) && ...);
        }
        ASTRA_FORCEINLINE static bool ChangeChunkPasses(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, Tick since) noexcept
        {
            return AllNewer(chunk, cm, since, static_cast<ChangedTypes*>(nullptr))
                && AllNewer(chunk, cm, since, static_cast<AddedTypes*>(nullptr));
        }
```

`CollectArchetypes`' `queryComponentCount` must include `GetChangeMask()`: `(GetRequiredMask() | GetWithMask() | GetChangeMask()).Count()`.

- [ ] **Step 5: Build Debug; run `--gtest_filter=ChangeDetection*`.** Expected PASS. Full suite three configs: baseline + 16.

- [ ] **Step 6: Scratch compile-fail check (no test file).** In the scratchpad, compile a TU (`cl /std:c++20 /D__SSE2__ /D__SSE4_2__ /arch:AVX /Zc:__cplusplus /I<repo>\include /c`) containing, one at a time: (a) `reg.CreateView<const Position, Astra::Changed<Position>>().ForEach([](const Position&){});` -- expect the "needs a 'since' tick" message; (b) `Astra::View<Astra::Changed<Position>>` -- expect "must also be listed as a required component"; (c) `Astra::Changed<Astra::Test::Player>` -- expect the tag message. Paste the three error lines into the task report; delete the scratch TU.

- [ ] **Step 7: Commit.**

```bash
git add include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(query): Changed<T>/Added<T> filters with chunk-version reject, Since(tick) and ForEach(ctx, fn) (Stage 3 task 4)"
```

---
## Task 5: `SystemContext` ticks, per-group tick advance, `SystemContext&` preference (OPUS)

**Files:**
- Modify: `include/Astra/System/SystemContext.hpp` (`LastRun/ThisRun`, ctor params, `ParallelForEach(view, fn)` passes `LastRun()`)
- Modify: `include/Astra/System/SystemMetadata.hpp` (`mutable Tick lastRun = 0`)
- Modify: `include/Astra/System/SystemExecutor.hpp` (`BeginSystemGroup`, `DispatchSystem`, both executors)
- Modify: `include/Astra/System/SystemScheduler.hpp` (`AddSystem<System T>` requires `!ContextSystem<T>`; registry-switch `lastRun` reset)
- Create: `tests/System/ChangeDetectionSchedulerTest.cpp`

**Interfaces:**
- Consumes: `Tick` (Task 1), `Registry::AdvanceTick/CurrentTick` (Task 1), `View::ForEach(ctx, fn)` / `View::ParallelForEachWithContext(Tick, Factory, Body)` / `View::HasChangeFilter` (Task 4).
- Produces: `Tick SystemContext::LastRun() const noexcept`, `Tick SystemContext::ThisRun() const noexcept`; `SystemContext(Registry&, CommandBuffer&, uint32_t insertionOrder, uint32_t iterationIndex, ParallelCommandBuffer*, Tick lastRun, Tick thisRun)` (7-arg; the existing 3- and 5-arg ctors delegate with `lastRun = 0`, `thisRun = reg.CurrentTick()`); `SystemMetadata::lastRun`; `inline void Astra::BeginSystemGroup(const SystemExecutionContext&)`.
- **Deviation 1 (see Global Constraints):** the tick advances once per parallel GROUP, in `BeginSystemGroup`, called by both shipped executors before dispatching a group. `DispatchSystem` reads `registry->CurrentTick()` as `thisRun`. Contract for custom `ISystemExecutor` implementations: call `BeginSystemGroup(context)` before dispatching each group (Arcane uses the shipped `ParallelExecutor` -- verified by `git grep ISystemExecutor` in the Arcane repo on 2026-09-10 -- so no consumer change).
- Guarantees (spec §3.6): B running in a later group than A in the same frame sees A's writes as changed; A on the next frame does not see its own writes; a system's first run has `LastRun() == 0` and sees everything.

- [ ] **Step 1: Write the failing tests.** Create `tests/System/ChangeDetectionSchedulerTest.cpp`:

```cpp
#include <atomic>
#include <vector>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;
using Astra::Test::Velocity;

namespace
{
    // Writer: non-const view over Position (stamps every chunk it visits).
    struct WritePos : Astra::SystemTraits<Astra::Writes<Position>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.GetRegistry().CreateView<Position>().ForEach([](Position& p) { p.x += 1.0f; });
        }
    };

    // Reader: counts entities whose Position changed since this system's last run.
    struct ReadChanged : Astra::SystemTraits<Astra::Reads<Position>>
    {
        static inline size_t s_seen = 0;
        static inline std::vector<std::pair<Tick, Tick>> s_ticks;   // (LastRun, ThisRun) per run
        void operator()(Astra::SystemContext& ctx)
        {
            s_ticks.emplace_back(ctx.LastRun(), ctx.ThisRun());
            auto v = ctx.GetRegistry().CreateView<const Position, Astra::Changed<Position>>();
            s_seen = 0;
            v.ForEach(ctx, [](const Position&) { ++s_seen; });
        }
    };

    // A type offering BOTH signatures: the scheduler must prefer SystemContext&.
    struct Both
    {
        static inline int s_registryCalls = 0;
        static inline int s_contextCalls = 0;
        void operator()(Astra::Registry&) { ++s_registryCalls; }
        void operator()(Astra::SystemContext& ctx) { ++s_contextCalls; EXPECT_GT(ctx.ThisRun(), 0u); }
    };
}

TEST(ChangeDetectionScheduler, StandaloneContextHasLastRunZeroAndThisRunCurrentTick)
{
    Astra::Registry reg;
    reg.AdvanceTick(); reg.AdvanceTick();   // 3
    Astra::CommandBuffer cmds(&reg);
    Astra::SystemContext ctx(reg, cmds, 0u);
    EXPECT_EQ(ctx.LastRun(), 0u);
    EXPECT_EQ(ctx.ThisRun(), reg.CurrentTick());
}

TEST(ChangeDetectionScheduler, WriterThenReaderInOneFrameSeesWritesAndNotItsOwnNextFrame)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(3000);
    ASSERT_EQ(reg.CreateEntities<Position, Velocity>(3000, std::span{ents}), 3000u);

    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WritePos>().IsOk());      // writes Position -> group 0
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());   // reads Position  -> group 1 (conflict)
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);

    const Tick before = reg.CurrentTick();
    s.Execute(reg);                                   // frame 1
    EXPECT_EQ(ReadChanged::s_seen, 3000u);            // first run: LastRun 0 sees everything
    ASSERT_EQ(ReadChanged::s_ticks.size(), 1u);
    EXPECT_EQ(ReadChanged::s_ticks[0].first, 0u);
    EXPECT_GT(ReadChanged::s_ticks[0].second, before);
    EXPECT_EQ(reg.CurrentTick(), before + 2);         // one advance per GROUP (deviation 1)

    s.Execute(reg);                                   // frame 2: writer ran again in group 0 -> reader sees all again
    EXPECT_EQ(ReadChanged::s_seen, 3000u);
    EXPECT_EQ(ReadChanged::s_ticks[1].first, ReadChanged::s_ticks[0].second);   // LastRun == previous ThisRun

    // Remove the writer: the reader must now see NOTHING (its own previous run wrote nothing).
    ASSERT_TRUE(s.RemoveSystem<WritePos>());
    s.Execute(reg);                                   // plan rebuilt -> lastRun reset to 0 (documented): sees everything once
    EXPECT_EQ(ReadChanged::s_seen, 3000u);
    s.Execute(reg);                                   // and then nothing
    EXPECT_EQ(ReadChanged::s_seen, 0u);
}

TEST(ChangeDetectionScheduler, ReaderBeforeWriterInFrameSeesPreviousFramesWrites)
{
    // Ordering: reader in group 0, writer in group 1 (Before edge). Frame N's writes
    // (stamped with group 1's tick) must be visible to frame N+1's reader, whose
    // LastRun is frame N's group-0 tick -- older than group 1's stamp.
    struct WriteAfter : Astra::SystemTraits<Astra::Writes<Position>, Astra::After<ReadChanged>>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            ctx.GetRegistry().CreateView<Position>().ForEach([](Position& p) { p.x += 1.0f; });
        }
    };
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ(reg.CreateEntities<Position>(500, std::span{ents}), 500u);
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());
    ASSERT_TRUE(s.AddSystem<WriteAfter>().IsOk());
    s.Execute(reg);                       // reader (sees all: first run), then writer
    s.Execute(reg);                       // reader must see the previous frame's writes
    EXPECT_EQ(ReadChanged::s_seen, 500u);
}

TEST(ChangeDetectionScheduler, ParallelGroupSharesOneTickAndGroupsAdvance)
{
    struct WA : Astra::SystemTraits<Astra::Writes<Position>>
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };
    struct WB : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };
    struct WA2 : Astra::SystemTraits<Astra::Writes<Position>>   // conflicts with WA -> next group
    {
        static inline std::atomic<Tick> s_tick{0};
        void operator()(Astra::SystemContext& ctx) { s_tick.store(ctx.ThisRun()); }
    };
    Astra::Registry reg;
    (void)reg.CreateEntity<Position, Velocity>();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WA>().IsOk());
    ASSERT_TRUE(s.AddSystem<WB>().IsOk());
    ASSERT_TRUE(s.AddSystem<WA2>().IsOk());
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);

    Astra::ParallelExecutor exec(std::make_shared<Astra::Testing::TestWorkerPool>());
    s.Execute(reg, &exec);
    EXPECT_EQ(WA::s_tick.load(), WB::s_tick.load());                 // same group, same tick
    EXPECT_TRUE(Astra::IsNewer(WA2::s_tick.load(), WA::s_tick.load()));   // next group is newer
}

TEST(ChangeDetectionScheduler, SystemContextOverloadPreferredWhenBothExist)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<Both>().IsOk());
    s.Execute(reg);
    EXPECT_EQ(Both::s_contextCalls, 1);
    EXPECT_EQ(Both::s_registryCalls, 0);
}

TEST(ChangeDetectionScheduler, RegistrySwitchResetsLastRun)
{
    Astra::Registry a, b;
    (void)a.CreateEntity<Position>();
    (void)b.CreateEntity<Position>();
    ReadChanged::s_ticks.clear();
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<ReadChanged>().IsOk());
    s.Execute(a);
    s.Execute(a);
    EXPECT_NE(ReadChanged::s_ticks.back().first, 0u);
    s.Execute(b);                                        // ticks of `a` mean nothing in `b`
    EXPECT_EQ(ReadChanged::s_ticks.back().first, 0u);    // reset: sees everything once
    EXPECT_EQ(ReadChanged::s_seen, 1u);
}

TEST(ChangeDetectionScheduler, ContextParallelForEachPassesLastRunToChangeFilteredViews)
{
    struct Reader : Astra::SystemTraits<Astra::Reads<Position>>
    {
        static inline std::atomic<size_t> s_seen{0};
        void operator()(Astra::SystemContext& ctx)
        {
            auto v = ctx.GetRegistry().CreateView<const Position, Astra::Changed<Position>>();
            s_seen.store(0);
            ctx.ParallelForEach(v, [](Astra::Entity, const Position&, Astra::SystemContext&) { s_seen.fetch_add(1); });
        }
    };
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<Position>(2000, std::span{ents}), 2000u);
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<Reader>().IsOk());
    s.Execute(reg);
    EXPECT_EQ(Reader::s_seen.load(), 2000u);   // first run sees everything
    s.Execute(reg);
    EXPECT_EQ(Reader::s_seen.load(), 0u);      // nothing changed since
}
```

- [ ] **Step 2: `premake5 vs2022`, build Debug; expect compile errors** on `LastRun`, `ThisRun`.

- [ ] **Step 3: `SystemMetadata.hpp`.** Add `#include "../Core/Tick.hpp"` and, after `segmentIndex`:

```cpp
        // Change-detection (spec 2026-09-10 §3.5): the tick of this system's previous
        // run; 0 == never ran. `mutable` because SystemExecutor::DispatchSystem writes
        // it through the const SystemExecutionContext& every ISystemExecutor receives;
        // distinct systems own distinct elements, so a parallel group never races on
        // it. Lives in the scheduler's cached context copy (SystemExecutionContext::
        // metadata) and therefore RESETS TO 0 whenever the plan is rebuilt (Add/
        // RemoveSystem): every system then sees everything once on its next run --
        // a documented false positive, never a false negative.
        mutable Tick lastRun = 0;
```

- [ ] **Step 4: `SystemContext.hpp`.**

```cpp
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder) noexcept
            : SystemContext(reg, cmds, insertionOrder, 0u, nullptr, Tick{0}, reg.CurrentTick()) {}

        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder,
                      uint32_t iterationIndex, ParallelCommandBuffer* parallelBuffer) noexcept
            : SystemContext(reg, cmds, insertionOrder, iterationIndex, parallelBuffer, Tick{0}, reg.CurrentTick()) {}

        // Full ctor (change detection, spec §3.5): the scheduler passes this system's
        // previous-run tick and the tick assigned to this run (== the registry's
        // CurrentTick for the group being dispatched).
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder,
                      uint32_t iterationIndex, ParallelCommandBuffer* parallelBuffer,
                      Tick lastRun, Tick thisRun) noexcept
            : m_registry(reg), m_commands(cmds), m_insertionOrder(insertionOrder),
              m_iterationIndex(iterationIndex), m_parallelBuffer(parallelBuffer),
              m_lastRun(lastRun), m_thisRun(thisRun) {}

        // Change-detection time for this run: filters compare against LastRun()
        // (0 on a system's first run => everything reads as changed); ThisRun() is
        // what this run's writes are stamped with.
        [[nodiscard]] Tick LastRun() const noexcept { return m_lastRun; }
        [[nodiscard]] Tick ThisRun() const noexcept { return m_thisRun; }
```

Members: `Tick m_lastRun = 0; Tick m_thisRun = 0;`. In `ParallelForEach(view, func)`: capture `const Tick lastRun = m_lastRun, thisRun = m_thisRun;` and build sub-contexts with the 7-arg ctor (`..., base + w, pcb, lastRun, thisRun`); call `view.ParallelForEachWithContext(lastRun, factory, body)` (the tick-taking overload from Task 4) so a change-filtered view gets its `since` automatically.

- [ ] **Step 5: `SystemExecutor.hpp`.**

```cpp
    /**
     * Change-detection time (spec §3.5, plan deviation 1): advance the registry's
     * tick ONCE per parallel group, before any member is dispatched. Members of one
     * group have no read/write conflict, so they share the group's tick; a system's
     * stamps therefore always carry its own ThisRun() even while siblings run
     * concurrently. Called by SequentialExecutor/ParallelExecutor; a custom
     * ISystemExecutor MUST call it before dispatching each group.
     */
    inline void BeginSystemGroup(const SystemExecutionContext& context)
    {
        context.registry->AdvanceTick();
    }

    inline void DispatchSystem(const SystemExecutionContext& context, size_t systemIdx)
    {
        const SystemMetadata& md = context.metadata[systemIdx];
        const Tick thisRun = context.registry->CurrentTick();   // the group's tick (BeginSystemGroup)
        const Tick lastRun = md.lastRun;
        if (context.contextSystems[systemIdx])
        {
            SystemContext sysCtx(*context.registry,
                context.commandBuffer->GetThreadBuffer(),
                static_cast<uint32_t>(md.scheduleOrder),
                0u, context.commandBuffer,
                lastRun, thisRun);
            context.contextSystems[systemIdx](sysCtx);
        }
        else
        {
            context.systems[systemIdx](*context.registry);
        }
        md.lastRun = thisRun;   // mutable; this system's own element only
    }
```

Both executors: add `BeginSystemGroup(context);` as the first statement inside `for (const auto& group : context.parallelGroups)` (in `ParallelExecutor`, before the `group.size() == 1 || !m_scheduler` branch so both arms share it).

- [ ] **Step 6: `SystemScheduler.hpp`.**
- The class-typed `template<System T, typename... Args> AddSystem(Args&&...)` gets `requires (!ContextSystem<T>)` so a type with both signatures resolves to the `ContextSystem T` overload (spec §2.6 preference). Update its doc comment ("T can satisfy at most one" is no longer the mechanism).
- In `Execute(Registry&, ISystemExecutor*)`, inside the `if (!m_commandBuffer || m_commandBufferRegistry != &registry)` rebind block, add:

```cpp
                // Change detection: ticks are per-registry, so a registry switch makes
                // every cached lastRun meaningless -- reset so each system sees
                // everything once against the new registry (never a false negative).
                for (auto& md : m_context.metadata)
                    md.lastRun = 0;
```

(`BuildExecutionPlan` runs before this block, so the reset lands on the current cache.)

- [ ] **Step 7: Build Debug; run `--gtest_filter=ChangeDetectionScheduler.*`.** Expected: 8 PASS. Full suite three configs: baseline + 24. `SystemSchedulerTest`/`SystemContextTest` must be untouched in count and result.

- [ ] **Step 8: Commit.**

```bash
git add include/Astra/System/SystemContext.hpp include/Astra/System/SystemMetadata.hpp include/Astra/System/SystemExecutor.hpp include/Astra/System/SystemScheduler.hpp tests/System/ChangeDetectionSchedulerTest.cpp
git commit -m "feat(system): SystemContext LastRun/ThisRun, per-group tick advance, SystemContext& preferred (Stage 3 task 5)"
```

---

## Task 6: Opt-in `AstraChangeTracked` trait and per-entity tick columns (OPUS)

**Files:**
- Modify: `include/Astra/Component/Component.hpp` (trait + descriptor flag)
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (`MakeDescriptor` ~:348 assigns the flag)
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`ArchetypeColumnMeta::trackedColumns`; `Column::ticks`; carve; accessors; init/copy at every slot-writing site in the chunk)
- Modify: `include/Astra/Archetype/Archetype.hpp` (`BuildColumnMeta`, layout math, `InitTicks` at every slot-claim site, copies in `MoveEntityFrom`/`CompactChunks`/`MoveEntitiesBetweenChunks`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (copies in `MoveAndAdd`/`MoveAndAddByID`)
- Modify: `tests/TestComponents.hpp` (`TrackedPos`, `TrackedVel`)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append)

**Interfaces:**
- Consumes: `EntityTicks`, `Tick` (Task 1); the chunk carve/stamp machinery (Task 2).
- Produces:
  - `Astra::ChangeTrackedTraits<T>::value`, `Astra::IsChangeTrackedV<T>` (same `if constexpr (requires ...)` shape as `EnableableTraits`); a tracked tag is a compile-time error.
  - `ComponentDescriptor::isChangeTracked` (defaults false; factory-assigned).
  - `ArchetypeColumnMeta::trackedColumns[MAX_COMPONENTS]`, `trackedColumnCount`.
  - `ArchetypeChunk`: `EntityTicks* GetTicks(int column) noexcept` (nullptr when untracked), `bool IsTracked(int column) const noexcept`, `void InitTicks(size_t index, Tick tick) noexcept` (all tracked columns: `added = changed = tick`), `void CopyTicks(int dstColumn, size_t dstIndex, const ArchetypeChunk& src, int srcColumn, size_t srcIndex) noexcept` (no-op unless both tracked), `size_t GetTicksOffset(uint16_t column) const` (inspector; `size_t max` when untracked).
  - Test types `Astra::Test::TrackedPos { static constexpr bool AstraChangeTracked = true; float x,y,z; }` and `Astra::Test::TrackedVel { static constexpr bool AstraChangeTracked = true; static constexpr bool AstraEnableable = true; float dx,dy,dz; }` (TrackedVel is ALSO enableable so Task 8 can test the enabled+changed union with no third type).
- Invariant: **every slot that receives an entity has `added = changed = Now()` for every tracked column, unless the entity arrived by a move, in which case the source slot's ticks are copied for every SHARED tracked column (a newly ADDED tracked component keeps `Now()`).** Same-archetype relocations copy. Deserialize gets `Now()` via `AddEntity` (Task 2), which is the spec's "everything reads as changed once".

- [ ] **Step 1: Test components.** In `tests/TestComponents.hpp`, after `RelocationCanary`:

```cpp
    // Change-detection suite (spec 2026-09-10 §3.2): the ONLY two change-tracked
    // types in the test binary (TypeID budget). TrackedVel is also enableable so the
    // enabled+changed union run-scan can be tested without a third type.
    struct TrackedPos
    {
        static constexpr bool AstraChangeTracked = true;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        TrackedPos() = default;
        TrackedPos(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
        bool operator==(const TrackedPos&) const = default;
    };
    struct TrackedVel
    {
        static constexpr bool AstraChangeTracked = true;
        static constexpr bool AstraEnableable = true;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        TrackedVel() = default;
        TrackedVel(float a, float b, float c) : dx(a), dy(b), dz(c) {}
        bool operator==(const TrackedVel&) const = default;
    };
    static_assert(Component<TrackedPos> && Component<TrackedVel>);
```

- [ ] **Step 2: Write the failing tests.** Append to `tests/Registry/ChangeDetectionTest.cpp`:

```cpp
// ---- Task 6: opt-in trait + per-entity tick columns ----------------------

using Astra::Test::TrackedPos;
using Astra::Test::TrackedVel;

namespace
{
    template<typename T>
    Astra::EntityTicks TicksOf(Astra::Registry& reg, Astra::Entity e)
    {
        const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
        if (!rec || !rec->chunk) { ADD_FAILURE() << "entity not located"; return {}; }
        const int col = rec->archetype->GetColumnMeta().idToColumn[Astra::TypeID<T>::Value()];
        auto* ticks = rec->chunk->GetTicks(col);
        if (!ticks) { ADD_FAILURE() << "T is not tracked"; return {}; }
        return ticks[rec->location.GetEntityIndex()];
    }

    namespace TrackedTraitDetail
    {
        struct Spelled { static constexpr bool AstraChangeTracked = true; int v; };
        struct Plain   { int v; };
        struct Spec    { int v; };
    }
}
template<> struct Astra::ChangeTrackedTraits<TrackedTraitDetail::Spec> { static constexpr bool value = true; };

TEST(ChangeDetectionTracked, TraitDetectsBothSpellingsAndDescriptorSnapshotsIt)
{
    static_assert(Astra::IsChangeTrackedV<TrackedTraitDetail::Spelled>);
    static_assert(Astra::IsChangeTrackedV<TrackedTraitDetail::Spec>);
    static_assert(!Astra::IsChangeTrackedV<TrackedTraitDetail::Plain>);
    static_assert(!Astra::IsChangeTrackedV<int>);
    static_assert(Astra::IsChangeTrackedV<const TrackedPos>);
    static_assert(!Astra::IsChangeTrackedV<Position>);

    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<TrackedPos, Position>();
    EXPECT_TRUE(reg.GetComponentRegistry()->GetComponentDescriptor(Astra::TypeID<TrackedPos>::Value())->isChangeTracked);
    EXPECT_FALSE(reg.GetComponentRegistry()->GetComponentDescriptor(Astra::TypeID<Position>::Value())->isChangeTracked);
}

TEST(ChangeDetectionTracked, UntrackedColumnsCarveNoTicksTrackedOnesDo)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, TrackedPos>();
    const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    const auto& cm = rec->archetype->GetColumnMeta();
    EXPECT_EQ(cm.trackedColumnCount, 1u);
    EXPECT_FALSE(rec->chunk->IsTracked(cm.idToColumn[Astra::TypeID<Position>::Value()]));
    EXPECT_EQ(rec->chunk->GetTicks(cm.idToColumn[Astra::TypeID<Position>::Value()]), nullptr);
    EXPECT_TRUE(rec->chunk->IsTracked(cm.idToColumn[Astra::TypeID<TrackedPos>::Value()]));
    EXPECT_EQ(rec->chunk->GetTicksOffset(static_cast<uint16_t>(cm.idToColumn[Astra::TypeID<TrackedPos>::Value()])) % 8, 0u);

    // A plain archetype has zero tracked columns: the zero-cost early-out.
    auto p = reg.CreateEntity<Position, Velocity>();
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(p)->archetype->GetColumnMeta().trackedColumnCount, 0u);
}

TEST(ChangeDetectionTracked, CreateAndAddInitialiseAddedAndChangedToNow)
{
    Astra::Registry reg;
    AdvanceTo(reg, 5);
    auto a = reg.CreateEntity<TrackedPos>();
    EXPECT_EQ(TicksOf<TrackedPos>(reg, a).added, 5u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, a).changed, 5u);

    auto b = reg.CreateEntityWith(TrackedPos{1, 2, 3});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, b).added, 5u);

    std::vector<Astra::Entity> batch(300);
    ASSERT_EQ(reg.CreateEntitiesWith<TrackedPos, TrackedVel>(300, std::span{batch},
        [](size_t) { return std::tuple{TrackedPos{}, TrackedVel{}}; }), 300u);
    EXPECT_EQ(TicksOf<TrackedVel>(reg, batch.back()).added, 5u);

    AdvanceTo(reg, 6);
    auto c = reg.CreateEntity<Position>();
    ASSERT_TRUE(reg.AddComponent<TrackedPos>(c, TrackedPos{}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).added, 6u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).changed, 6u);

    AdvanceTo(reg, 7);
    Astra::CommandBuffer cmd(&reg);
    cmd.AddComponent(c, TrackedVel{});
    cmd.Execute();
    EXPECT_EQ(TicksOf<TrackedVel>(reg, c).added, 7u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, c).added, 6u);   // carried component keeps its ticks
}

TEST(ChangeDetectionTracked, TicksTravelAcrossArchetypeMovesSwapRemoveAndCompaction)
{
    Astra::Registry reg;
    AdvanceTo(reg, 3);
    std::vector<Astra::Entity> ents(1000);
    ASSERT_EQ(reg.CreateEntities<TrackedPos, Position>(1000, std::span{ents}), 1000u);

    // Give ents[500] distinct ticks by adding it later... instead: re-create it later.
    AdvanceTo(reg, 4);
    auto late = reg.CreateEntity<TrackedPos, Position>();
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Cross-archetype add: ticks copied (added stays 4, not 5).
    AdvanceTo(reg, 5);
    ASSERT_TRUE(reg.AddComponent<Velocity>(late, Velocity{}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).changed, 4u);

    // Cross-archetype remove: same.
    AdvanceTo(reg, 6);
    ASSERT_TRUE(reg.RemoveComponent<Velocity>(late));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Batch add (BatchMoveComponentsFrom): ticks copied for the shared tracked column.
    AdvanceTo(reg, 7);
    std::vector<Astra::Entity> some(ents.begin(), ents.begin() + 100);
    reg.AddComponents<Velocity>(std::span{some}, Velocity{});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, some[0]).added, 3u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, some[99]).added, 3u);

    // Swap-remove within a chunk: destroy the entity in front of `late` in its chunk;
    // `late` (or whoever fills the hole) must keep its own ticks.
    AdvanceTo(reg, 8);
    const auto* recLate = reg.GetArchetypeManager()->GetEntityRecord(late);
    const auto* chunk = recLate->chunk;
    const size_t lateIdx = recLate->location.GetEntityIndex();
    ASSERT_GT(lateIdx, 0u);
    Astra::Entity victim = chunk->GetEntity(0);
    ASSERT_NE(victim, late);
    // Make `late` the tail so it is the one swapped down: destroy everything after it.
    std::vector<Astra::Entity> tail;
    for (size_t i = lateIdx + 1; i < chunk->GetCount(); ++i) tail.push_back(chunk->GetEntity(i));
    reg.DestroyEntities(std::span{tail});
    reg.DestroyEntity(victim);                          // late swaps into slot 0
    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(late)->location.GetEntityIndex(), 0u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, late).added, 4u);

    // Compaction: fragment the {TrackedPos, Position} archetype hard, defragment, ticks survive.
    AdvanceTo(reg, 9);
    std::vector<Astra::Entity> doomed;
    for (size_t i = 0; i < ents.size(); ++i) if (i % 10 < 8) doomed.push_back(ents[i]);
    reg.DestroyEntities(std::span{doomed});
    auto res = reg.Defragment();
    ASSERT_GT(res.entitiesMoved, 0u);
    for (size_t i = 0; i < ents.size(); ++i)
        if (i % 10 >= 8 && i >= 100 && reg.IsValid(ents[i]))
            EXPECT_EQ(TicksOf<TrackedPos>(reg, ents[i]).added, 3u);
}

TEST(ChangeDetectionTracked, DeserializeSetsEveryTrackedEntityToTheLoadersTick)
{
    std::vector<std::byte> buffer;
    {
        Astra::Registry reg;
        AdvanceTo(reg, 30);
        std::vector<Astra::Entity> ents(400);
        ASSERT_EQ(reg.CreateEntities<TrackedPos, Position>(400, std::span{ents}), 400u);
        auto saved = reg.Save();
        ASSERT_TRUE(saved.IsOk());
        buffer = std::move(*saved.GetValue());
    }
    auto componentRegistry = std::make_shared<Astra::ComponentRegistry>();
    componentRegistry->RegisterComponents<TrackedPos, Position>();
    auto loaded = Astra::Registry::Load(buffer, componentRegistry);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg = **loaded.GetValue();
    size_t n = 0;
    reg.CreateView<const TrackedPos>().ForEach([&](Astra::Entity e, const TrackedPos&)
    {
        ++n;
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).added, 1u);
        EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 1u);
    });
    EXPECT_EQ(n, 400u);
}
```

- [ ] **Step 3: Build Debug; expect compile errors** on `ChangeTrackedTraits`, `isChangeTracked`, `GetTicks`.

- [ ] **Step 4: Trait + descriptor.** In `Component.hpp`, directly under `IsEnableableV`:

```cpp
    /**
     * Opt-in exact change tracking (spec 2026-09-10 §3.2). A component is
     * change-tracked iff it declares `static constexpr bool AstraChangeTracked = true`
     * or specializes Astra::ChangeTrackedTraits<T>. Tracked columns carry per-entity
     * {added, changed} ticks (8 B per entity per tracked column) and are handed out as
     * Mut<T> by non-const views; everything else pays only the free per-chunk version.
     * Same if-constexpr-gated shape as EnableableTraits (MSVC short-circuit rationale).
     */
    template<typename T>
    struct ChangeTrackedTraits
    {
        static constexpr bool value = []() constexpr
        {
            if constexpr (requires { { T::AstraChangeTracked } -> std::convertible_to<bool>; })
                return T::AstraChangeTracked;
            else
                return false;
        }();
    };

    template<typename T>
    inline constexpr bool IsChangeTrackedV = ChangeTrackedTraits<std::remove_const_t<T>>::value;
```

`ComponentDescriptor`: `bool isChangeTracked = false;   // defaults false (hand-built descriptors stay safe); the registry factory assigns IsChangeTrackedV<T>` next to `isEnableable`. In `ComponentRegistry::MakeDescriptor` (~:348), after `desc.isEnableable = IsEnableableV<T>;`:

```cpp
            static_assert(!(IsChangeTrackedV<T> && std::is_empty_v<T>),
                "AstraChangeTracked on a tag (empty) component is meaningless: a tag has no value to change. "
                "Use Added<T>/Signal::ComponentAdded for presence, or give the component data.");
            desc.isChangeTracked = IsChangeTrackedV<T>;
```

- [ ] **Step 5: Column meta + chunk carve.** In `ArchetypeChunkPool.hpp`:

5a. `ArchetypeColumnMeta`: after `enableableColumnCount`:

```cpp
        // Change-tracked columns (spec 2026-09-10 §3.2): ordinals whose component opted
        // into AstraChangeTracked. Only these carve per-chunk {added, changed} tick
        // columns and copy ticks on relocation; trackedColumnCount == 0 is the zero-cost
        // early-out every untracked archetype takes. Built by Archetype::BuildColumnMeta.
        uint16_t  trackedColumns[MAX_COMPONENTS]{};
        uint16_t  trackedColumnCount{0};
```

5b. `Column`: add `EntityTicks* ticks{nullptr};   // change-tracked columns only; nullptr otherwise` (the +8 B/slot cost noted in the Column comment -- extend that comment to say the deferral now covers three fields).

5c. `InitializeColumns()`: after the disabled-word loop, a third carve:

```cpp
            // Third loop: one {added, changed} tick column per CHANGE-TRACKED column,
            // 8-byte aligned. Zero-init == tick 0 == "never"; every slot is initialised
            // to Now() when an entity lands in it (InitTicks) or copied on a move.
            for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
            {
                const uint16_t c = m_meta->trackedColumns[t];
                offset = (offset + 7) & ~size_t(7);
                m_columns[c].ticks = reinterpret_cast<EntityTicks*>(static_cast<std::byte*>(m_memory) + offset);
                offset += m_capacity * sizeof(EntityTicks);
            }
```

5d. Accessors (public, after the version block from Task 2):

```cpp
        // ===================== Change detection: per-entity ticks (tracked columns) =====================
        ASTRA_NODISCARD ASTRA_FORCEINLINE EntityTicks* GetTicks(int column) noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].ticks;
        }
        ASTRA_NODISCARD ASTRA_FORCEINLINE const EntityTicks* GetTicks(int column) const noexcept
        {
            ASTRA_ASSERT(column >= 0 && column < m_meta->columnCount, "column ordinal out of range");
            return m_columns[column].ticks;
        }
        ASTRA_NODISCARD ASTRA_FORCEINLINE bool IsTracked(int column) const noexcept
        {
            return m_columns[column].ticks != nullptr;
        }

        // A slot just received a NEW component set (create / batch create / added
        // component): every tracked column reads added == changed == tick.
        ASTRA_FORCEINLINE void InitTicks(size_t index, Tick tick) noexcept
        {
            for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
                m_columns[m_meta->trackedColumns[t]].ticks[index] = EntityTicks{tick, tick};
        }

        // Relocation carry: copy one slot's ticks for one shared tracked column. The two
        // columns share a ComponentID (hence tracked-ness); no-op when untracked.
        ASTRA_FORCEINLINE void CopyTicks(int dstColumn, size_t dstIndex,
                                         const ArchetypeChunk& src, int srcColumn, size_t srcIndex) noexcept
        {
            EntityTicks* d = m_columns[dstColumn].ticks;
            const EntityTicks* s = src.m_columns[srcColumn].ticks;
            if (d && s) ASTRA_UNLIKELY
                d[dstIndex] = s[srcIndex];
        }

        ASTRA_NODISCARD size_t GetTicksOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            const EntityTicks* t = m_columns[column].ticks;
            if (!t) return std::numeric_limits<size_t>::max();
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(t) - static_cast<const std::byte*>(m_memory));
        }
```

5e. Chunk-level slot writers:
- `AddEntity(entity, tick)`: after `StampAllColumns(tick);` add `InitTicks(index, tick);`.
- `AddEntityWithComponents(entity, tick, ...)`: same, `InitTicks(index, tick);`.
- `BatchAddEntities(entities, tick)`: after the count bump: `for (size_t i = start; i < m_count; ++i) InitTicks(i, tick);` where `start` is the pre-bump count (the loop is empty for untracked metas after one compare per slot -- fold it under `if (m_meta->trackedColumnCount) ASTRA_UNLIKELY` to keep create_batch's hot path a single compare).
- `RemoveEntity(index)`: in the enableable carry loop's sibling, add a tracked carry with identical shape:

```cpp
            // Tick carry: the moved (last) entity's ticks fill the vacated slot. No tail
            // clear is needed (ticks are re-initialised when a slot is reused).
            if (index != lastIndex) ASTRA_LIKELY
                for (uint16_t t = 0; t < m_meta->trackedColumnCount; ++t)
                {
                    const uint16_t c = m_meta->trackedColumns[t];
                    m_columns[c].ticks[index] = m_columns[c].ticks[lastIndex];
                }
```
- `BatchMoveComponentsFrom(...)`: inside the per-column loop after the disabled-bit carry: `if (desc.isChangeTracked) ASTRA_UNLIKELY for (i) CopyTicks(c, dstIndices[i], srcChunk, sc, srcIndices[i]);`.

- [ ] **Step 6: Archetype + manager.**
- `BuildColumnMeta`: after the enableable loop, fill `trackedColumns` from `descriptor->isChangeTracked` in ordinal order.
- `ComputeCapacityForBytes`: the precise-fit condition becomes `if ((m_columnMeta.enableableColumnCount > 0 || m_columnMeta.trackedColumnCount > 0) && cap > 0)`.
- `ComputeLayoutBytesForCapacity`: after the disabled-word loop: `for (t < trackedColumnCount) { offset = align8(offset); offset += cap * sizeof(EntityTicks); }`.
- `ChunkBytesToHold`: `bytes += trackedColumnCount * (cap * sizeof(EntityTicks) + 8);`.
- `AllocateEntitySlot`: after `chunk->StampAllColumns(Now());` add `chunk->InitTicks(entityIndex, Now());`.
- `AddEntitiesWith`: after the run's `StampAllColumns`, `if (cm.trackedColumnCount) ASTRA_UNLIKELY for (r < runLen) chunk->InitTicks(startSlot + r, Now());`.
- `BatchMoveEntitiesFrom` slot-claim loop: same per-slot `InitTicks` for `[startIndex, startIndex + toAdd)` (the later `BatchMoveComponentsFrom` copy overwrites the shared columns; the newly added column keeps `Now()`).
- `MoveEntityFrom`: in the matched branch after the disabled carry: `if (desc.isChangeTracked) ASTRA_UNLIKELY dstChunk->CopyTicks(a, dstEntityIndex, *srcChunk, b, srcEntityIndex);`.
- `CompactChunks`: inside the per-column loop after the disabled carry: `if (desc.isChangeTracked) ASTRA_UNLIKELY for (k < run) dst->CopyTicks(c, dstIndex + k, *src, c, srcIndex + k);`.
- `MoveEntitiesBetweenChunks`: `if (desc.isChangeTracked) ASTRA_UNLIKELY destChunk->CopyTicks(c, destEntityIndex, *srcChunk, c, srcEntityIndex);`.
- `ArchetypeManager::MoveAndAdd` and `MoveAndAddByID`: in the shared-column branch after the disabled carry: `if (desc.isChangeTracked) ASTRA_UNLIKELY dstChunk->CopyTicks(c, dstEntityIdx, *srcChunk, sc, srcEntityIdx);`. The `id == newComponentId` branch keeps the `InitTicks` value from `AllocateEntitySlot` (added == changed == Now()).

- [ ] **Step 7: Build Debug; run `--gtest_filter=ChangeDetection*`.** Expected PASS. If `TicksTravelAcrossArchetypeMovesSwapRemoveAndCompaction`'s swap-remove section cannot make `late` the tail (chunk boundaries), simplify to: record `late`'s ticks, destroy the entity at slot 0 of `late`'s chunk, then check the entity now at slot 0 has the ticks its own record says it should -- compare against a `std::unordered_map<Entity, EntityTicks>` snapshot taken before the destroy. Full suite three configs: baseline + 29.

- [ ] **Step 8: Commit.**

```bash
git add include/Astra/Component/Component.hpp include/Astra/Component/ComponentRegistry.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp tests/TestComponents.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(component): opt-in AstraChangeTracked trait; per-entity {added,changed} tick columns carved and carried like disabled bits (Stage 3 task 6)"
```

---
## Task 7: `Mut<T>` and the marking accessors (OPUS)

**Files:**
- Modify: `include/Astra/Registry/Query.hpp` (`Mut<T>`)
- Modify: `include/Astra/Registry/View.hpp` (`HasTrackedYield` gate, `Mut` yield in the view-owned chunk loops, `Get` tuple shape, optional-tracked mark, adapter message)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`GetComponentMut`/`MarkWritten` mark the entity for tracked `T`)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append)

**Interfaces:**
- Consumes: `EntityTicks`, `ArchetypeChunk::GetTicks/IsTracked` (Task 6), `Detail::IsMutableYield` (Task 3), the view chunk loops (Tasks 3-4).
- Produces:
  - `template<typename T> class Astra::Mut` -- `Mut(T* value, EntityTicks* ticks, Tick now) noexcept`; `operator T&() noexcept` (marks); `T& Write() noexcept` (marks); `const T& Read() const noexcept` (never marks); `T* operator->() noexcept` (marks); `bool SetIfNeq(const T& v)` (marks only when `v != current`; requires `std::equality_comparable<T>`); `bool IsAdded(Tick since) const noexcept`; `bool IsChanged(Tick since) const noexcept`; `static_assert(IsChangeTrackedV<T>)` -- `Mut<Untracked>` is a compile-time error.
  - `View::HasTrackedYield` (private constexpr): any required or optional arg that is `IsMutableYield` AND `IsChangeTrackedV`.
  - Yield rules: required non-const tracked `T` -> `Mut<T>` (an lvalue, so `auto&` lambdas bind); required otherwise unchanged (`T&`/`const T&`); `Optional<T>` unchanged (`T*`), but a present non-const tracked optional marks the entity unconditionally (deviation 5). `View::AccessTuple` element for a required non-const tracked `T` is `Mut<T>` (by value) instead of `T*`.
  - Marking = `ticks[i].changed = now` (one store; the chunk version was stamped on chunk entry).

- [ ] **Step 1: Write the failing tests.** Append:

```cpp
// ---- Task 7: Mut<T> + marking accessors -----------------------------------

TEST(ChangeDetectionMut, ConversionAndWriteMarkReadDoesNotSetIfNeqMarksOnlyOnChange)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(TrackedPos{1, 2, 3});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);

    AdvanceTo(reg, 3);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { (void)p.Read(); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // Read() never marks
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 3u);         // ...but the chunk was still stamped (coarse tier)

    AdvanceTo(reg, 4);
    reg.CreateView<TrackedPos>().ForEach([](TrackedPos& p) { p.x += 1.0f; });   // legacy lambda: implicit conversion marks
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);

    AdvanceTo(reg, 5);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { p->y = 9.0f; });   // operator-> marks
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);

    AdvanceTo(reg, 6);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { EXPECT_FALSE(p.SetIfNeq(TrackedPos{2, 9, 3})); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);   // equal: no mark
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p) { EXPECT_TRUE(p.SetIfNeq(TrackedPos{7, 9, 3})); });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 6u);

    AdvanceTo(reg, 7);
    reg.CreateView<TrackedPos>().ForEach([](Astra::Mut<TrackedPos> p)
    {
        EXPECT_TRUE(p.IsChanged(5));  EXPECT_FALSE(p.IsChanged(6));
        EXPECT_TRUE(p.IsAdded(1));    EXPECT_FALSE(p.IsAdded(2));
        p.Write().z = 0.0f;
    });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 7u);

    // const yield stays a plain const T& (no Mut, no mark, no stamp).
    AdvanceTo(reg, 8);
    reg.CreateView<const TrackedPos>().ForEach([](const TrackedPos&) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 7u);
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 7u);
}

TEST(ChangeDetectionMut, GenericAndEntityLeadingLambdasStillCompileAndMark)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<TrackedPos, Position>();
    AdvanceTo(reg, 3);
    size_t n = 0;
    reg.CreateView<TrackedPos, const Position>().ForEach([&](Astra::Entity, auto& p, const Position&) { ++n; p.Write().x = 1.0f; });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);
    AdvanceTo(reg, 4);
    reg.CreateView<TrackedPos, const Position>().ParallelForEach([](TrackedPos& p, const Position&) { p.x = 2.0f; });
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);
}

TEST(ChangeDetectionMut, GetTupleYieldsMutForTrackedAndMarksOnUse)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntity<TrackedPos, Position>();
    auto v = reg.CreateView<TrackedPos, const Position>();
    static_assert(std::is_same_v<std::tuple_element_t<0, decltype(v)::AccessTuple>, Astra::Mut<TrackedPos>>);
    static_assert(std::is_same_v<std::tuple_element_t<1, decltype(v)::AccessTuple>, const Position*>);

    AdvanceTo(reg, 3);
    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto& [mp, pp] = *r.GetValue();
    (void)pp;
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // handing out Mut does not mark
    mp.Write().x = 5.0f;
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);
    EXPECT_EQ(VersionOf<TrackedPos>(reg, e), 3u);         // Get stamped the chunk on the non-const request
}

TEST(ChangeDetectionMut, OptionalNonConstTrackedMarksWhenPresentConstDoesNot)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto with    = reg.CreateEntity<Position, TrackedPos>();
    auto without = reg.CreateEntity<Position>();
    (void)without;
    AdvanceTo(reg, 3);
    reg.CreateView<const Position, Astra::Optional<TrackedPos>>().ForEach([](const Position&, TrackedPos*) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, with).changed, 3u);   // deviation 5: present + non-const => marked
    AdvanceTo(reg, 4);
    reg.CreateView<const Position, Astra::Optional<const TrackedPos>>().ForEach([](const Position&, const TrackedPos*) {});
    EXPECT_EQ(TicksOf<TrackedPos>(reg, with).changed, 3u);
}

TEST(ChangeDetectionMut, RegistryAccessorsMarkTrackedEntities)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(TrackedPos{1, 1, 1});

    AdvanceTo(reg, 3);
    ASSERT_NE(std::as_const(reg).GetComponent<TrackedPos>(e), nullptr);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 2u);   // const: no mark
    ASSERT_NE(reg.GetComponent<TrackedPos>(e), nullptr);
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 3u);   // non-const: marked

    AdvanceTo(reg, 4);
    EXPECT_TRUE(reg.Modified<TrackedPos>(e));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);

    AdvanceTo(reg, 5);
    EXPECT_FALSE(reg.SetIfNeq<TrackedPos>(e, TrackedPos{1, 1, 1}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 4u);
    EXPECT_TRUE(reg.SetIfNeq<TrackedPos>(e, TrackedPos{2, 1, 1}));
    EXPECT_EQ(TicksOf<TrackedPos>(reg, e).changed, 5u);
}
```

- [ ] **Step 2: Build Debug; expect compile errors** naming `Astra::Mut`.

- [ ] **Step 3: `Mut<T>` in `Query.hpp`** (after the modifier structs; include `../Core/Tick.hpp`):

```cpp
    /**
     * Mutable handle to a change-TRACKED component (spec 2026-09-10 §3.3). Handed out
     * by views in place of `T&` when T is requested non-const and IsChangeTrackedV<T>.
     * Any mutable access marks the entity's `changed` tick with the current run's tick
     * (one store); Read() never marks. The implicit `T&` conversion keeps existing
     * `[](T& t)` lambdas compiling unchanged -- at the cost of marking even when the
     * body only reads (accepted false positive; use Read() when it matters).
     */
    template<typename T>
    class Mut
    {
        static_assert(IsChangeTrackedV<T>,
            "Mut<T> is only handed out for change-tracked components (static constexpr bool AstraChangeTracked = true); "
            "an untracked component is yielded as plain T&");
        static_assert(!std::is_const_v<T>, "Mut<const T> is meaningless: request `const T` and receive `const T&`");

    public:
        Mut(T* value, EntityTicks* ticks, Tick now) noexcept : m_value(value), m_ticks(ticks), m_now(now) {}

        ASTRA_FORCEINLINE operator T&() noexcept              { m_ticks->changed = m_now; return *m_value; }
        ASTRA_FORCEINLINE T& Write() noexcept                 { m_ticks->changed = m_now; return *m_value; }
        ASTRA_FORCEINLINE T* operator->() noexcept            { m_ticks->changed = m_now; return m_value; }
        ASTRA_NODISCARD ASTRA_FORCEINLINE const T& Read() const noexcept { return *m_value; }

        // Compare-then-store opt-in (spec §2.4): stores + marks ONLY when v != current.
        bool SetIfNeq(const T& v) requires std::equality_comparable<T>
        {
            if (*m_value == v) return false;
            *m_value = v;
            m_ticks->changed = m_now;
            return true;
        }

        ASTRA_NODISCARD bool IsAdded(Tick since) const noexcept   { return IsNewer(m_ticks->added, since); }
        ASTRA_NODISCARD bool IsChanged(Tick since) const noexcept { return IsNewer(m_ticks->changed, since); }

    private:
        T*           m_value;
        EntityTicks* m_ticks;
        Tick         m_now;
    };
```

- [ ] **Step 4: View yield.** In `View.hpp`:

4a. Gate, next to `HasChangeFilter`:

```cpp
        // Any yielded, non-const, change-tracked component: routes iteration through
        // the view-owned chunk loops (Archetype::ForEachStamped cannot yield Mut) and
        // switches the required yield to Mut<T>. False for every pre-existing view.
        template<typename Tuple> struct AnyTrackedYield;
        template<typename... Ts> struct AnyTrackedYield<std::tuple<Ts...>>
            : std::bool_constant<((Detail::IsMutableYield<Ts> && IsChangeTrackedV<Ts>) || ...)> {};
        static constexpr bool HasTrackedYield = AnyTrackedYield<RequiredTypes>::value || AnyTrackedYield<OptionalTypes>::value;
```

4b. `ForEachImpl` / `ParallelForEachChunkImpl`: the Archetype fast path is taken only when `!HasChunkFilter && !HasTrackedYield && sizeof...(Optional) == 0`; otherwise the existing `ForEachWithOptional` / `ParallelForEachChunkWithOptional` loop runs (it already handles zero optionals) or, with a chunk filter, `VisitChunkFiltered`.

4c. Per-required yield. Replace `RequiredElement(std::get<ReqIs>(reqPtrs), i)` in `InvokeEntityCallback` and `InvokeEntityCallbackFiltered` with a tracked-aware yield. Add next to `RequiredElement`:

```cpp
        template<typename R> struct YieldType { using type = R&; };
        template<typename R> requires (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
        struct YieldType<R> { using type = Mut<R>; };

        // reqTicks[k] is the tick column for required k (nullptr unless tracked).
        template<size_t K, typename ReqTuple>
        ASTRA_FORCEINLINE typename YieldType<std::tuple_element_t<K, RequiredTypes>>::type
        YieldRequired(const ReqTuple& reqPtrs, EntityTicks* const* reqTicks, size_t i, Tick now) const noexcept
        {
            using R = std::tuple_element_t<K, RequiredTypes>;
            if constexpr (Detail::IsMutableYield<R> && IsChangeTrackedV<R>)
                return Mut<R>(&std::get<K>(reqPtrs)[i], reqTicks[K] + i, now);
            else
                return RequiredElement(std::get<K>(reqPtrs), i);
        }
```

The callback invocation materialises the yields as lvalues so `auto&` and `T&` parameters both bind:

```cpp
        template<typename EntitiesVec, typename ReqTuple, typename OptTuple, typename Func, size_t... ReqIs, size_t... OptIs>
        ASTRA_FORCEINLINE void InvokeEntityCallback(const EntitiesVec& entities, const ReqTuple& reqPtrs, const OptTuple& optPtrs,
                                                    size_t count, Func&& func, std::index_sequence<ReqIs...> rs, std::index_sequence<OptIs...> os,
                                                    EntityTicks* const* reqTicks = nullptr, EntityTicks* const* optTicks = nullptr, Tick now = 0)
        {
            if constexpr (!HasTrackedYield)
            {
                for (size_t i = 0; i < count; ++i)   // pre-existing body, byte-identical
                    func(entities[i], RequiredElement(std::get<ReqIs>(reqPtrs), i)..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...);
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    MarkTrackedOptionals(optPtrs, optTicks, i, now, os);
                    std::tuple<typename YieldType<std::tuple_element_t<ReqIs, RequiredTypes>>::type...> req{ YieldRequired<ReqIs>(reqPtrs, reqTicks, i, now)... };
                    std::apply([&](auto&... r) { func(entities[i], r..., (std::get<OptIs>(optPtrs) ? &std::get<OptIs>(optPtrs)[i] : nullptr)...); }, req);
                }
            }
        }
```

(`std::tuple<T&...>` elements are references; `std::apply` with `auto&...` yields `T&` for those and `Mut<T>&` for the by-value elements. Mirror the same change in `InvokeEntityCallbackFiltered` over `[begin, end)`.) `MarkTrackedOptionals`: for each optional `K` with `IsMutableYield && IsChangeTrackedV`, `if (std::get<K>(optPtrs)) optTicks[K][i].changed = now;`.

4d. Tick-column resolution at each chunk (only when `HasTrackedYield`): in `ForEachWithOptional`, `ParallelForEachChunkWithOptional`, `VisitChunkFiltered`, after the pointer tuples:

```cpp
            [[maybe_unused]] EntityTicks* reqTicks[sizeof...(ReqIs) == 0 ? 1 : sizeof...(ReqIs)] = {};
            [[maybe_unused]] EntityTicks* optTicks[sizeof...(OptIs) == 0 ? 1 : sizeof...(OptIs)] = {};
            if constexpr (HasTrackedYield)
            {
                ((reqTicks[ReqIs] = (Detail::IsMutableYield<std::tuple_element_t<ReqIs, RequiredTypes>> && IsChangeTrackedV<std::tuple_element_t<ReqIs, RequiredTypes>>)
                    ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<ReqIs, RequiredTypes>>>::Value()]) : nullptr), ...);
                ((optTicks[OptIs] = (hasOptional[OptIs] && Detail::IsMutableYield<std::tuple_element_t<OptIs, OptionalTypes>> && IsChangeTrackedV<std::tuple_element_t<OptIs, OptionalTypes>>)
                    ? chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<std::tuple_element_t<OptIs, OptionalTypes>>>::Value()]) : nullptr), ...);
            }
```

and pass `reqTicks, optTicks, now` into the invoke helpers.

4e. `Get` tuple: `AccessTupleImpl` maps a required `R` to `std::conditional_t<Detail::IsMutableYield<R> && IsChangeTrackedV<R>, Mut<R>, R*>`; `BindRequired<R>` returns `Mut<R>(ptr, rec->chunk->GetTicks(col) + idx, now)` in the tracked case (`now = m_archetypeManager->CurrentTick()`); `BindOptional<O>` marks when present, non-const and tracked. `Single()` is unchanged (reuses `Get`).

4f. `MakeEntityOptionalAdapter`'s static_assert message: append "A change-tracked non-const component is yielded as Mut<T> (converts to T&; use .Read()/.Write()/.SetIfNeq())."

- [ ] **Step 5: Registry accessor marks.** In `ArchetypeManager::GetComponentMut<T>`: after `StampColumn`, `if constexpr (IsChangeTrackedV<T>) rec->chunk->GetTicks(col)[rec->location.GetEntityIndex()].changed = m_tick;`. In `MarkWritten(entity, id)`: after `StampColumn`, `if (EntityTicks* t = rec->chunk->GetTicks(col)) t[rec->location.GetEntityIndex()].changed = m_tick;` (runtime null test: the type-erased path has no `T`).

- [ ] **Step 6: Build Debug; run `--gtest_filter=ChangeDetection*`.** Expected PASS. Full suite three configs: baseline + 34. `LambdaSystemWrapper`-based view-lambda systems and `SystemParam` binder views over untracked types must compile and behave identically (`SystemSchedulerTest`, `SystemParamTest`).

- [ ] **Step 7: Scratch compile-fail check:** `Astra::Mut<Position>` (untracked) -> expect the "only handed out for change-tracked components" message. Record the error line; delete the scratch TU.

- [ ] **Step 8: Commit.**

```bash
git add include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp include/Astra/Archetype/ArchetypeManager.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(view): Mut<T> for change-tracked components -- implicit T& marks, Read() does not; accessors mark (Stage 3 task 7)"
```

---

## Task 8: Per-entity filter tier for tracked types (OPUS)

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (`VisitChunkFiltered` excluded-mask build; `ForEachEnabledRun` word-set union)
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append)

**Interfaces:**
- Consumes: `Detail::ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount, size_t count, fn)` (existing, `EnabledRuns.hpp`), `ArchetypeChunk::GetTicks` (Task 6), `ChangedTypes`/`AddedTypes`/`ChangeChunkPasses` (Task 4).
- Produces: inside `VisitChunkFiltered`, after the chunk reject: for every `Changed<T>` with `IsChangeTrackedV<T>` and every `Added<T>` with `IsChangeTrackedV<T>`, an "excluded" bit-set over `[0, count)` (SET bit == entity fails the term) is built from the tick column and unioned with the enabled-filter word sets, so the per-entity branch is taken in runs (spec §3.4 step 3). Untracked terms stay chunk-granular (Task 4 behaviour). `HasTrackedChangeTerm` (private constexpr) gates all of it.

- [ ] **Step 1: Write the failing tests.** Append:

```cpp
// ---- Task 8: per-entity tier (tracked Changed/Added, union with enabled runs) ----

TEST(ChangeDetectionTrackedFilter, ChangedIsPerEntityInsideAStampedChunk)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(500);
    ASSERT_EQ(reg.CreateEntities<TrackedPos, Position>(500, std::span{ents}), 500u);

    // Mark exactly 7 scattered entities at tick 3 (the chunk gets stamped by GetComponent).
    AdvanceTo(reg, 3);
    std::vector<size_t> marked = {0, 3, 64, 65, 127, 300, 499};
    for (size_t i : marked) reg.GetComponent<TrackedPos>(ents[i])->x = 1.0f;

    auto v = reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>();
    std::vector<Astra::Entity> seen;
    v.Since(2).ForEach([&](Astra::Entity e, const TrackedPos&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), marked.size());               // per-entity precision, not the whole chunk
    for (size_t k = 0; k < marked.size(); ++k) EXPECT_EQ(seen[k], ents[marked[k]]);   // ascending index order

    EXPECT_EQ(CountSince(v, 3), 0u);
    EXPECT_EQ(CountSince(v, 0), 500u);                   // first run still sees everything
}

TEST(ChangeDetectionTrackedFilter, AddedIsPerEntityAndIgnoresLaterChanges)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> old(200);
    ASSERT_EQ(reg.CreateEntities<TrackedPos, Position>(200, std::span{old}), 200u);
    AdvanceTo(reg, 3);
    auto fresh = reg.CreateEntity<TrackedPos, Position>();    // lands in the same archetype/chunk, added == 3
    for (auto e : old) reg.GetComponent<TrackedPos>(e)->x = 2.0f;   // changed == 3 for the old ones too

    auto added = reg.CreateView<const TrackedPos, Astra::Added<TrackedPos>>();
    std::vector<Astra::Entity> seen;
    added.Since(2).ForEach([&](Astra::Entity e, const TrackedPos&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], fresh);

    auto changed = reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>();
    EXPECT_EQ(CountSince(changed, 2), 201u);
}

TEST(ChangeDetectionTrackedFilter, UnionWithEnabledRunsSkipsDisabledAndUnchanged)
{
    // TrackedVel is BOTH change-tracked and enableable: the run scan must be the
    // intersection of enabled AND changed, visited in ascending order.
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(300);
    ASSERT_EQ(reg.CreateEntities<TrackedVel, Position>(300, std::span{ents}), 300u);
    AdvanceTo(reg, 3);
    for (size_t i = 0; i < 300; i += 2) reg.GetComponent<TrackedVel>(ents[i])->dx = 1.0f;   // even: changed
    for (size_t i = 0; i < 300; i += 3) ASSERT_TRUE(reg.SetEnabled<TrackedVel>(ents[i], false));   // multiples of 3: disabled

    auto v = reg.CreateView<const TrackedVel, Astra::Changed<TrackedVel>>();
    std::vector<Astra::Entity> seen;
    v.Since(2).ForEach([&](Astra::Entity e, const TrackedVel&) { seen.push_back(e); });
    size_t expected = 0;
    for (size_t i = 0; i < 300; ++i) if (i % 2 == 0 && i % 3 != 0) ++expected;
    EXPECT_EQ(seen.size(), expected);
    for (size_t k = 1; k < seen.size(); ++k) EXPECT_LT(seen[k - 1].GetID(), seen[k].GetID());   // ascending

    // IncludeDisabled lifts the enabled filter: every even entity, disabled or not.
    auto all = reg.CreateView<Astra::IncludeDisabled<TrackedVel>, Astra::Changed<TrackedVel>>();
    EXPECT_EQ(CountSince(all, 2), 150u);
}

TEST(ChangeDetectionTrackedFilter, MixedTrackedAndUntrackedTermsAndParallelParity)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(2000);
    ASSERT_EQ(reg.CreateEntities<TrackedPos, Position>(2000, std::span{ents}), 2000u);
    AdvanceTo(reg, 3);
    for (size_t i = 0; i < 2000; i += 10) reg.GetComponent<TrackedPos>(ents[i])->x = 1.0f;   // 200 tracked marks

    // Untracked Position term: chunk-granular (every chunk was stamped at creation, tick 2 -> not newer than 2).
    auto both = reg.CreateView<const TrackedPos, const Position, Astra::Changed<TrackedPos>, Astra::Changed<Position>>();
    EXPECT_EQ(CountSince(both, 2), 0u);   // Position's chunk versions are 2 -> reject
    AdvanceTo(reg, 4);
    reg.CreateView<Position>().ForEach([](Position&) {});   // stamp Position everywhere at 4
    EXPECT_EQ(CountSince(both, 3), 0u);   // TrackedPos marks were at 3, not newer than 3
    EXPECT_EQ(CountSince(both, 2), 200u); // chunk passes on both; per-entity on TrackedPos

    std::atomic<size_t> par{0};
    both.Since(2).ParallelForEach([&](const TrackedPos&, const Position&) { par.fetch_add(1); });
    EXPECT_EQ(par.load(), 200u);
}
```

- [ ] **Step 2: Build Debug; run `--gtest_filter=ChangeDetectionTrackedFilter.*`.** Expected: the first three FAIL (chunk granularity yields whole chunks); the fourth's untracked assertions may already pass.

- [ ] **Step 3: Implement.** In `View.hpp`:

3a. Gate:

```cpp
        template<typename Tuple> struct AnyTracked;
        template<typename... Ts> struct AnyTracked<std::tuple<Ts...>> : std::bool_constant<(IsChangeTrackedV<Ts> || ...)> {};
        static constexpr bool HasTrackedChangeTerm = AnyTracked<ChangedTypes>::value || AnyTracked<AddedTypes>::value;
```

3b. Excluded-mask builder (private):

```cpp
        // Per-entity tier (spec §3.4 step 3): for one tracked term, set the bit of
        // every entity whose tick is NOT newer than `since`. Branchless per slot; the
        // resulting word set is unioned with the enabled-filter sets so the callback
        // is invoked in maximal runs of (enabled AND changed) entities.
        template<bool UseAdded>
        ASTRA_FORCEINLINE static void ExcludeNotNewer(const EntityTicks* ticks, size_t count, Tick since, uint64_t* excluded, bool& anyExcluded) noexcept
        {
            for (size_t i = 0; i < count; ++i)
            {
                const Tick t = UseAdded ? ticks[i].added : ticks[i].changed;
                const uint64_t bit = static_cast<uint64_t>(!IsNewer(t, since));
                excluded[i >> 6] |= bit << (i & 63);
                anyExcluded |= (bit != 0);
            }
        }

        template<bool UseAdded, typename... Ts>
        ASTRA_FORCEINLINE static void BuildExcluded(ArchetypeChunk* chunk, const ArchetypeColumnMeta& cm, size_t count, Tick since,
                                                    uint64_t* excluded, bool& anyExcluded, std::tuple<Ts...>*) noexcept
        {
            ((IsChangeTrackedV<Ts>
                ? ExcludeNotNewer<UseAdded>(chunk->GetTicks(cm.idToColumn[TypeID<std::remove_const_t<Ts>>::Value()]), count, since, excluded, anyExcluded)
                : void()), ...);
        }
```

3c. In `VisitChunkFiltered`, after the chunk reject and BEFORE `ResolveRequiredFilter`, widen the word-set array and build the mask:

```cpp
            constexpr size_t NReq = std::tuple_size_v<EnabledRequiredFilter>;
            const uint64_t* wordSets[NReq + 1];               // enabled sets + (optional) excluded set
            bool allZero = true;
            const bool anyReqFull = ResolveRequiredFilter(chunk, cm, count, wordSets, allZero, std::make_index_sequence<NReq>{});
            if (anyReqFull) ASTRA_UNLIKELY
                return;
            size_t setCount = NReq;

            [[maybe_unused]] SmallVector<uint64_t, 64> excluded;   // 64 inline words = 4096 slots before a heap step
            if constexpr (HasTrackedChangeTerm)
            {
                excluded.assign((count + 63) >> 6, 0ull);
                bool anyExcluded = false;
                BuildExcluded<false>(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<ChangedTypes*>(nullptr));
                BuildExcluded<true >(chunk, cm, count, since, excluded.data(), anyExcluded, static_cast<AddedTypes*>(nullptr));
                if (anyExcluded)
                {
                    wordSets[setCount++] = excluded.data();
                    allZero = false;                       // a per-entity constraint exists: no Tier-1 whole-chunk call
                }
            }
```

Then the existing Tier-1 (`allZero`) call stays, and the Tier-3 call becomes `Detail::ForEachEnabledRun(wordSets, setCount, count, ...)`. `ResolveRequiredFilter`'s `reqWords` parameter type is already `const uint64_t**`; passing `wordSets` (size `NReq + 1`) is fine -- it only writes `[0, NReq)`. `ForEachEnabledRun` masks bits at or beyond `count` itself, so the tail of the last word needs no pre-clearing. Include `../Container/SmallVector.hpp` in `View.hpp`.

- [ ] **Step 4: Build Debug; run `--gtest_filter=ChangeDetection*`.** Expected PASS (ascending-order assertions included: `ForEachEnabledRun` visits runs in index order). Full suite three configs: baseline + 38.

- [ ] **Step 5: Commit.**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ChangeDetectionTest.cpp
git commit -m "feat(view): per-entity Changed/Added tier for change-tracked types -- tick-column exclusion mask unioned with enabled runs (Stage 3 task 8)"
```

---
## Task 9: Zero-cost checks, bench gate, acceptance test, docs, finish (sonnet; bench verdict reviewed by the controller)

**Files:**
- Test: `tests/Registry/ChangeDetectionTest.cpp` (append zero-cost checks)
- Create: `tests/Registry/ChangeDetectionAcceptanceTest.cpp`
- Modify: `bench-compare/bench_astra.cpp` (`run_change_detection()`), `bench-compare/RESULTS.md` (new section)
- Modify: `README.md` (Query Modifiers + a "Change detection" subsection under Advanced Features)
- Modify: `docs/superpowers/specs/2026-09-10-astra-change-detection-design.md` (status line + the §3.4 example correction + the five deviations recorded as "implemented as")

**Interfaces:**
- Consumes: everything above.
- Produces: the merge-gate evidence -- flat-watch bench table, `Changed<T>` 0/10/50/100 % table, acceptance test, docs.

- [ ] **Step 1: Zero-cost checks (spec §4.8).** Append to `tests/Registry/ChangeDetectionTest.cpp`:

```cpp
// ---- Task 9: zero-cost when unused -----------------------------------------

TEST(ChangeDetectionZeroCost, PlainViewsInstantiateNoFilterOrTrackedYieldPath)
{
    using Plain   = Astra::View<Position, const Velocity>;
    using Filtered = Astra::View<const Position, Astra::Changed<Position>>;
    static_assert(!Plain::HasChangeFilter);
    static_assert(Filtered::HasChangeFilter);
    // An all-const view stamps nothing: IsMutableYield is false for every arg.
    static_assert(!Astra::Detail::IsMutableYield<const Position>);
    static_assert(Astra::Detail::IsMutableYield<Position>);
    static_assert(!Astra::Detail::IsMutableYield<Astra::Test::Player>);   // tag: no column, never stamped
    // Untracked types are yielded as plain references, never Mut.
    static_assert(std::is_same_v<std::tuple_element_t<0, Plain::AccessTuple>, Position*>);
    SUCCEED();
}

TEST(ChangeDetectionZeroCost, UntrackedArchetypeCarvesNoTickColumnsAndVersionRegionIsSmall)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity>();
    const auto* rec = reg.GetArchetypeManager()->GetEntityRecord(e);
    const auto& cm = rec->archetype->GetColumnMeta();
    EXPECT_EQ(cm.trackedColumnCount, 0u);
    for (uint16_t c = 0; c < cm.columnCount; ++c)
    {
        EXPECT_FALSE(rec->chunk->IsTracked(c));
        EXPECT_EQ(rec->chunk->GetTicksOffset(c), std::numeric_limits<size_t>::max());
    }
    // Version region: exactly columnCount Ticks, 8-byte aligned, inside the arena.
    EXPECT_EQ(rec->chunk->GetColumnVersionOffset() % 8, 0u);
    EXPECT_LE(rec->chunk->GetColumnVersionOffset() + cm.columnCount * sizeof(Tick), rec->chunk->GetChunkBytes());
}
```

- [ ] **Step 2: Acceptance test (spec §4.10).** Create `tests/Registry/ChangeDetectionAcceptanceTest.cpp` (regen premake):

```cpp
// Arcane-shaped acceptance (spec 2026-09-10 §4.10): a "transform propagation" pass
// over N entities where 5% change per frame. The Changed<T> path must visit only the
// chunks that hold changed entities and produce exactly the brute-force result.
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <random>
#include <unordered_set>
#include "../TestComponents.hpp"

using Astra::Tick;
using Astra::Test::Position;      // stands in for LocalTransform (untracked: coarse tier)
using Astra::Test::TrackedPos;    // stands in for WorldTransform (tracked: exact tier)

namespace
{
#ifdef ASTRA_BUILD_DEBUG
    constexpr size_t kN = 100'000;   // Debug: keep the suite fast
#else
    constexpr size_t kN = 1'000'000;
#endif
}

TEST(ChangeDetectionAcceptance, FivePercentPerFrameVisitsOnlyChangedChunksAndMatchesBruteForce)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> ents(kN);
    ASSERT_EQ(reg.CreateEntitiesWith<Position, TrackedPos>(kN, std::span{ents},
        [](size_t i) { return std::tuple{Position{float(i), 0, 0}, TrackedPos{float(i), 0, 0}}; }), kN);
    auto* arch = reg.GetArchetypeManager()->GetEntityRecord(ents[0])->archetype;
    const size_t chunkCount = arch->GetChunks().size();
    ASSERT_GT(chunkCount, 20u);

    std::mt19937 rng(0xC0FFEE);
    auto propagate = reg.CreateView<const Position, TrackedPos, Astra::Changed<Position>>();

    for (int frame = 0; frame < 5; ++frame)
    {
        const Tick since = reg.CurrentTick();
        reg.AdvanceTick();

        // "Physics" writes 5% of LocalTransforms, CLUSTERED (as a real scene is: a moving
        // region of entities), via the explicit raw-pointer contract.
        std::unordered_set<uint32_t> changed;
        const size_t start = std::uniform_int_distribution<size_t>(0, kN - kN / 20)(rng);
        for (size_t i = start; i < start + kN / 20; ++i)
        {
            reg.GetComponent<Position>(ents[i])->x += 1.0f;   // non-const Get stamps the chunk
            changed.insert(ents[i].GetID());
        }

        // Brute force: every entity whose Position moved this frame.
        // Changed<Position> path: visit only stamped chunks, copy into WorldTransform.
        size_t visited = 0;
        std::vector<size_t> visitedChunks(chunkCount, 0);
        reg.AdvanceTick();
        propagate.Since(since).ForEach([&](Astra::Entity e, const Position& p, Astra::Mut<TrackedPos> w)
        {
            ++visited;
            visitedChunks[reg.GetArchetypeManager()->GetEntityRecord(e)->location.GetChunkIndex()] = 1;
            w.Write().x = p.x;
        });

        // Chunk granularity: every changed entity is visited, plus at most the rest of
        // the chunks it touched (~5% of chunks + 1 boundary chunk).
        size_t touchedChunks = 0;
        for (size_t v : visitedChunks) touchedChunks += v;
        EXPECT_LE(touchedChunks, chunkCount / 20 + 2);
        EXPECT_GE(visited, changed.size());
        EXPECT_LE(visited, (chunkCount / 20 + 2) * arch->GetChunks()[0]->GetCapacity());

        // Result parity with brute force: every changed entity's WorldTransform equals its LocalTransform.
        for (size_t i = start; i < start + kN / 20; ++i)
        {
            const auto& creg = reg;
            EXPECT_FLOAT_EQ(creg.GetComponent<TrackedPos>(ents[i])->x, creg.GetComponent<Position>(ents[i])->x);
        }

        // And downstream (render) sees exactly the propagated set through the EXACT tier.
        size_t rendered = 0;
        reg.CreateView<const TrackedPos, Astra::Changed<TrackedPos>>().Since(since).ForEach([&](const TrackedPos&) { ++rendered; });
        EXPECT_EQ(rendered, visited);   // Mut::Write marked exactly the visited entities
    }
}
```

Build all three configs; the Release/Dist runs must finish in well under a second per frame (record wall time in the report).

- [ ] **Step 3: Bench ops.** In `bench-compare/bench_astra.cpp`, add an Astra-only block (components: `Position`/`Velocity` from `bench_common.hpp`; a file-local tracked twin):

```cpp
// ---- Change detection (2026-09, Astra-only: neither EnTT nor flecs has an equivalent op) ----
struct TrackedVelocity { static constexpr bool AstraChangeTracked = true; float x, y, z; };

template<typename Vel>
static void run_cd_variant(const char* prefix, size_t N)
{
    State s = make_empty();
    s.ents.reserve(N);
    for (size_t i = 0; i < N; ++i)
        s.ents.push_back(s.reg->CreateEntityWith(Position{(float)i, 0, 0}, Vel{1, 1, 1}));

    auto view = s.reg->CreateView<const Position, const Vel, Astra::Changed<Vel>>();
    for (int pct : {0, 10, 50, 100})
    {
        char op[64];
        std::snprintf(op, sizeof op, "%s_changed_%d", prefix, pct);
        report(op, N, median_persistent(7, [&]() {
            // Setup (measured, like the spike): mark the first pct% of entities in creation order (clustered).
            const Astra::Tick since = s.reg->CurrentTick();
            s.reg->AdvanceTick();
            const size_t k = N * pct / 100;
            for (size_t i = 0; i < k; ++i) s.reg->Modified<Vel>(s.ents[i]);
            s.reg->AdvanceTick();
            double acc = 0;
            view.Since(since).ForEach([&](const Position& p, const Vel& v) { acc += p.x + v.x; });
            g_sink += acc;
        }), N);
    }
}

static void run_change_detection()
{
    run_cd_variant<Velocity>("cd_untracked", 1'000'000);          // coarse tier only
    run_cd_variant<TrackedVelocity>("cd_tracked", 1'000'000);     // coarse reject + per-entity scan
}
```

Call `run_change_detection();` from `main()` after `run_system_tick_seq();`. The marking pass is inside the measured region deliberately (the spike measured the same shape); note it in RESULTS.md.

- [ ] **Step 4: Bench gate -- flat-watch.** Build two `bench_astra.exe` binaries with the identical full-opt recipe (Global Constraints): the BASELINE from a scratch worktree at `4ac19ff` (`git worktree add --detach <scratch> 4ac19ff`; copy `bench-compare/build_one.bat` and adjust its `cd`), the BRANCH from the working tree. Check the machine is quiet (`typeperf "\Processor(_Total)\% Processor Time" -sc 5` < ~10 %). Run 6 interleaved rounds baseline -> branch, appending `roundN,`-prefixed CSV rows to `bench-compare/cd_gate.csv` (untracked). Compute per-op medians with [min, max] for: create, create_batch, add_component, remove_component, destroy, random_get, iterate1, iterate2, iterate3, parallel_iterate2, system_tick_seq. **Rule:** a flat-watch op passes when the two [min, max] bands overlap. Expected sensitive ops and what to do if a band separates:
  - `random_get` (non-const `GetComponent` now stamps one word per call): also measure the const overload (`std::as_const(*s.reg).GetComponent<Position>(...)`) as `random_get_const` on the branch; report both. If the non-const path is out of noise, that is a real finding for the user (the spec mandates the stamp) -- do NOT silently switch the bench.
  - `iterate1/2/3` (per-chunk stamp on a mutable view): must be within noise (spike: 1.70 vs 1.87 ns baseline).
  - `create`/`create_batch` (`StampAllColumns` + `InitTicks` per slot/run): the tracked-column loop is a single compare for untracked archetypes.
  Record the table in `bench-compare/RESULTS.md` under a new `## Change detection (Stage 3, <date>, branch feat/change-detection @ <sha>)` section together with the `cd_untracked_changed_*` / `cd_tracked_changed_*` rows (branch only) and the recipe line. Any separated band is a STOP for the controller to escalate, not something to "fix" in this task.

- [ ] **Step 5: Docs.**
- `README.md`: in "Query Modifiers" add `Changed<T>` / `Added<T>` with the filter-what-you-fetch rule and the `ForEach(ctx, fn)` / `Since(tick)` forms; add an "Advanced Features > Change detection" subsection covering: the two tiers and their costs (4 B/chunk/column free; 8 B/entity/tracked column opt-in via `AstraChangeTracked`), automatic coarse stamping from non-const access (declare read-only views `const`), `Mut<T>` (`Read()`/`Write()`/`SetIfNeq()`/`IsChanged()`), `Registry::Modified`/`SetIfNeq`, tick semantics (`LastRun`/`ThisRun`, per-group advance, first run sees everything, load reads as changed once, 2^31 bound), the documented false positives, and the iteration-only limitation.
- Spec: set the status line to `implemented on feat/change-detection (plan docs/superpowers/plans/2026-09-10-astra-change-detection.md)`; correct the §3.4 example to list `const WorldTransform`; append a short "Implemented as" note per Global-Constraints deviation 1-5.

- [ ] **Step 6: Full 3-config run + commit.** Expected: baseline + 41 (Debug) / baseline + 41 (Release/Dist).

```bash
git add tests/Registry/ChangeDetectionTest.cpp tests/Registry/ChangeDetectionAcceptanceTest.cpp bench-compare/bench_astra.cpp bench-compare/RESULTS.md README.md docs/superpowers/specs/2026-09-10-astra-change-detection-design.md
git commit -m "test+bench+docs: change-detection zero-cost checks, 5%-of-1M acceptance, bench gate tables, README (Stage 3 task 9)"
```

- [ ] **Step 7: Finish (controller).** OPUS whole-branch review with the lens: (a) chunk layout -- every carve mirrored in `ComputeLayoutBytesForCapacity`/`ChunkBytesToHold`/`Initialize` overhead, every slot-writing site stamps and inits/copies ticks, no path leaves a live slot at tick 0 except a genuinely fresh chunk; (b) scheduler -- `BeginSystemGroup` on every group path, `lastRun` writes never race, registry switch resets; (c) iteration -- all-const views compile to the pre-existing loop, `HasChangeFilter`/`HasTrackedYield` gates hold, Mut lvalue-ness, run-scan order. Fix wave, independent 3-config verify, **confirm with the user**, then `git switch dev && git merge --ff-only feat/change-detection && git branch -d feat/change-detection`. Do not push. Update the program memory afterwards (Stage 3 shipped; Arcane adoption movement next).

---

## Self-Review (completed during planning)

**Spec coverage.** §3.1 Time -> Task 1 (`Tick`, counter, `IsNewer`), Task 5 (`LastRun`/`ThisRun`, scheduler). §3.2 Storage -> Task 2 (chunk versions), Task 6 (trait, tracked columns, moves), Task 2+6 (`Deserialize` via `AddEntity(entity, Now())`). §3.3 Write side, every row of the table -> Task 3 (ForEach/ParallelForEach/range-for stamps; Get/Single; `GetComponent` non-const vs const; `Modified`; `SetIfNeq`), Task 2 (`Set/Emplace/Add`, Commands apply, moves), Task 7 (`Mut<T>` marks; accessor marks), `SetEnabled` untouched (nothing to do). §3.4 Read side -> Task 4 (modifiers, chunk reject, untracked granularity, `Since`, `ForEach(ctx)`), Task 8 (tracked run-scan unioned with enabled runs; `Added`). §3.5 Scheduler -> Task 5 (`lastRun`, ctx build, preference, `System` concept handled by `requires (!ContextSystem<T>)`, `ForEach(ctx)`; plain `ForEach` on a filtered view is a compile error). §3.6 edges -> Task 1 (tick 0/wrap), Task 3+7 (false positives documented), Task 5 (parallel stamping/`AdvanceTick` serialisation, two-systems-one-frame), Task 4 (compile-time refusals for `Changed<T>` not required / tags), Task 7 (`Mut<Untracked>` refused), Task 6 (tracked tag refused). §3.7 API list -> all present. §3.8 non-goals respected (no removal buffers, no serialized ticks, no observers, no Arcane change). §4 tests 1-10 -> 1: Task 1; 2: Tasks 2-3; 3: Task 7; 4: Task 4; 5: Task 8; 6: Task 5; 7: Tasks 2+6; 8: Task 9; 9: Task 9; 10: Task 9. §5 sequencing (1)-(6) -> Tasks 1/5, 2-3, 4, 6, 7-8, 9.

**Deviations** are listed once in Global Constraints and again where made (Tasks 2, 4, 5, 7) and are recorded into the spec in Task 9.

**Placeholder scan.** No TBD/TODO; every code step carries the code; the two "same message as" notes in Task 4 Step 4b refer to a message spelled out two lines above in the same step.

**Type consistency.** `Tick`/`IsNewer`/`EntityTicks`/`TickContext` (Task 1) used verbatim in Tasks 2-9; `GetColumnVersion/StampColumn/StampAllColumns/FoldColumnVersion` (Task 2) used in Tasks 3-4; `AddEntity(Entity, Tick)`/`AddEntityWithComponents(Entity, Tick, C&&...)`/`BatchAddEntities(span, Tick)` (Task 2) called in Tasks 2 and 6; `Now()`/`SetTickSource` (Task 2) used in Tasks 3 and 6; `IsMutableYield` (Task 3) used in Tasks 3, 7, 9; `ForEachStamped`/`ForEachChunkStamped` (Task 3) called in View Task 3/7; `GetComponentMut`/`MarkWritten` (Task 3) extended in Task 7; `HasChangeFilter`/`ChangedTypes`/`AddedTypes`/`ChangeChunkPasses`/`VisitChunkFiltered(..., Tick now, Tick since, ...)`/`ForEachSince`/`ParallelForEachSince`/`ParallelForEachWithContext(Tick, ...)` (Task 4) used in Tasks 5, 7, 8; `GetTicks/IsTracked/InitTicks/CopyTicks/GetTicksOffset`/`trackedColumnCount`/`isChangeTracked` (Task 6) used in Tasks 7-9; `Mut<T>` ctor `(T*, EntityTicks*, Tick)` (Task 7) matches `YieldRequired`/`BindRequired`; `SystemContext` 7-arg ctor (Task 5) matches `DispatchSystem` and the `ParallelForEach` factory; `TrackedPos`/`TrackedVel` (Task 6) are the only new test types and are used in Tasks 6-9.
