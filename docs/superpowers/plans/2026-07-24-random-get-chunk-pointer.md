# random_get Record→Direct-Chunk-Pointer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cache the `ArchetypeChunk*` in `EntityRecord` behind a write-funnel so `GetComponent` skips the `archetype->m_chunks[chunkIndex]` hop and the mask test, cutting the random_get dependent-load chain from ~8 to ~5.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-24-random-get-chunk-pointer-design.md` (two review passes, all claims source-verified). Option A: `EntityRecord` widens 24→32B (`alignas(32)`, + `ArchetypeChunk* chunk`); every location write funnels through helpers that set `location` and `chunk` together; the non-tag get path reads `rec->chunk` directly with an `id >= MAX_COMPONENTS` guard replacing `Bitmap::Test`'s range guard; tags keep the mask-tested path. Prerequisite: hoist the nested `ArchetypeChunkPool::Chunk` to top-level `ArchetypeChunk` so `EntityRecord.hpp` can forward-declare it.

**Tech Stack:** Header-only C++20, MSVC (premake5-generated `Astra.sln`), GoogleTest, hand-rolled 3-way bench harness in `bench-compare/`.

## Global Constraints

- Branch: `perf/random-get-chunk-pointer` off dev @ `7abb325`. Local only — NEVER push. Finish = FF-merge to dev locally, delete branch (SDD finishing flow; confirm with user first).
- Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` (full path: `...\18\Community\MSBuild\Current\Bin\MSBuild.exe`; build the whole solution — `-t:AstraTest` does NOT work). Tests: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`.
- Test baseline (dev @ `ec1d5ce`): **Debug 724 / Release 722 / Dist 722**. Gate = all configs green + intended new tests; the 2-test Debug delta is `EXPECT_DEATH`-only (expected). `CompressionTest.PerformanceBenchmark` is a known flake — a lone failure of only that test = rerun isolated, not a regression.
- **TypeID ceiling: register NO new component types in tests.** Reuse `Astra::Test::*` (`tests/TestComponents.hpp`), `EmptyTagTest.cpp`'s file-local `Tag`/`TPos`, and `reinterpret_cast` dummy pointers (EntityTable never dereferences them).
- No on-disk format change (locations/records never serialize as bytes — verified in spec §4.1/§4.7).
- `rec->version` is EntityManager's alone — funnel helpers must NEVER write it (W1 invariant).
- Model recipe (SDD): opus for Tasks 2 and 3 + the final whole-branch review; sonnet fine for Tasks 1, 4, 5.
- IDE clang diagnostics are misconfigured false positives — judge only by the MSVC build.

---

### Task 1: Hoist `Chunk` to top-level `ArchetypeChunk`

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (class hoist)
- Modify: `include/Astra/Archetype/Archetype.hpp:43` (delete redundant alias)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: top-level `class Astra::ArchetypeChunk` (forward-declarable), `ArchetypeChunkPool::Chunk` still valid via nested `using Chunk = ArchetypeChunk;`. Task 2's `EntityRecord.hpp` forward-declares `class ArchetypeChunk;`.

Pure behavior-preserving refactor — no new tests; the 724-test Debug suite is the guard.

- [ ] **Step 1: Hoist the class**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`:
1. Above `class ArchetypeChunkPool` (line ~47), add `class ArchetypeChunkPool;` forward declaration (for the friend), then move the entire nested `Chunk` class body out to namespace scope as `class ArchetypeChunk { ... };`. Keep `friend class ArchetypeChunkPool;` (currently line ~568) and any private constructors exactly as-is.
2. If the hoisted class references pool-internal types defined above it inside the pool (e.g. config/meta structs), hoist those declarations with it, preserving order. `ArchetypeColumnMeta` and `Column` usage stays untouched.
3. Inside `class ArchetypeChunkPool`, first line of the public section: `using Chunk = ArchetypeChunk;` — all 19 internal `Chunk` references keep compiling. `ChunkDeleter` stays nested.

In `include/Astra/Archetype/Archetype.hpp`, delete line 43 (`using ArchetypeChunk = ArchetypeChunkPool::Chunk;`) — the name now exists at namespace scope.

- [ ] **Step 2: Build Debug + run full suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: build clean; **724 tests pass** (identical to baseline — zero behavior change).

- [ ] **Step 3: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp
git commit -m "refactor(archetype): hoist Chunk to top-level ArchetypeChunk (forward-declarable for EntityRecord)"
```

---

### Task 2: Widen EntityRecord + write funnel (all 16 sites)  — OPUS

**Files:**
- Modify: `include/Astra/Entity/EntityRecord.hpp` (add `chunk`, `alignas(32)`)
- Modify: `include/Astra/Entity/EntityTable.hpp:174-179` (`SetRecord` gains chunk param)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (funnel helpers + all 16 write sites)
- Test: `tests/Entity/EntityRecordTableTest.cpp` (layout asserts + 4-arg `SetRecord`)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (chunk-invariant tests)

**Interfaces:**
- Consumes: Task 1's top-level `class ArchetypeChunk`.
- Produces: `EntityRecord{Archetype* archetype; ArchetypeChunk* chunk; EntityLocation location; Entity::VersionType version;}` (32B, `alignas(32)`); `EntityTable::SetRecord(IDType id, Archetype* archetype, ArchetypeChunk* chunk, EntityLocation location)`; private static `ArchetypeManager::SetRecordLocation(EntityRecord*, Archetype*, ArchetypeChunk*, EntityLocation)` / `SetRecordLocation(EntityRecord*, Archetype*, EntityLocation)` (resolving) / `ClearRecordLocation(EntityRecord*)`. Task 3 reads `rec->chunk`; Task 4 reads `record->chunk`.

- [ ] **Step 1: Write the failing tests**

In `tests/Registry/ArchetypeManagerTest.cpp`, add (file-scope helper + tests; reuses `Astra::Test::Position`/`Velocity` — no new types):

```cpp
// The chunk-pointer invariant every located record must satisfy (spec §4.1):
static void ExpectChunkInvariant(Astra::ArchetypeManager* m, Astra::Entity e)
{
    auto* rec = m->GetEntityRecord(e);
    ASSERT_NE(rec, nullptr);
    ASSERT_NE(rec->archetype, nullptr);
    ASSERT_TRUE(rec->location.IsValid());
    ASSERT_LT(rec->location.GetChunkIndex(), rec->archetype->GetChunks().size());
    EXPECT_EQ(rec->chunk,
              rec->archetype->GetChunks()[rec->location.GetChunkIndex()].get());
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_CreateTransitionRemove)
{
    using namespace Astra::Test;
    Astra::Entity e = testEntities[0];
    manager->AddEntity(e);                       // create (root archetype)
    ExpectChunkInvariant(manager.get(), e);
    manager->AddComponent<Position>(e, 1.f, 2.f, 3.f);   // transition add
    ExpectChunkInvariant(manager.get(), e);
    manager->AddComponent<Velocity>(e);          // second transition
    ExpectChunkInvariant(manager.get(), e);
    manager->RemoveComponent<Velocity>(e);       // transition remove
    ExpectChunkInvariant(manager.get(), e);
    manager->RemoveEntity(e);                    // clear: all storage fields null
    auto* rec = manager->GetEntityRecord(e);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->archetype, nullptr);
    EXPECT_EQ(rec->chunk, nullptr);
    EXPECT_FALSE(rec->location.IsValid());
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_SwapRemoveFixup)
{
    using namespace Astra::Test;
    // Fill one archetype so removing a middle entity swap-moves the last one.
    for (int i = 0; i < 50; ++i)
    {
        manager->AddEntity(testEntities[i]);
        manager->AddComponent<Position>(testEntities[i], float(i), 0.f, 0.f);
    }
    manager->RemoveEntity(testEntities[10]);     // last entity swaps into slot 10
    for (int i = 0; i < 50; ++i)
    {
        if (i == 10) continue;
        ExpectChunkInvariant(manager.get(), testEntities[i]);
    }
}

TEST_F(ArchetypeManagerTest, RecordChunkInvariant_Defragment)
{
    using namespace Astra::Test;
    for (int i = 0; i < 60; ++i)
    {
        manager->AddEntity(testEntities[i]);
        manager->AddComponent<Position>(testEntities[i], float(i), 0.f, 0.f);
    }
    for (int i = 0; i < 60; i += 2)              // punch holes → compactable
        manager->RemoveEntity(testEntities[i]);
    manager->Defragment();                       // CompactChunks rewrites records
    for (int i = 1; i < 60; i += 2)
        ExpectChunkInvariant(manager.get(), testEntities[i]);
}
```

(If `manager->Defragment()` differs in name/signature, mirror the call the existing defrag test in this file uses — there is one from the W2 edge-invalidation work.)

Also add the deserialize-path invariant test (spec §5 item 3, record-level half). Mirror the existing Serialize/Deserialize round-trip test in this file (the `manager2`/`table2` pattern at ~line 317):

```cpp
TEST_F(ArchetypeManagerTest, RecordChunkInvariant_Deserialize)
{
    using namespace Astra::Test;
    for (int i = 0; i < 30; ++i)
    {
        manager->AddEntity(testEntities[i]);
        manager->AddComponent<Position>(testEntities[i], float(i), 0.f, 0.f);
    }
    // Serialize -> Deserialize into a fresh manager2/table2, exactly as the
    // existing round-trip test in this file does (reuse its writer/reader setup).
    // Then, for every reloaded entity:
    for (int i = 0; i < 30; ++i)
        ExpectChunkInvariant(manager2.get(), testEntities[i]);
}
```
(Adapt the middle section to the file's existing round-trip helper verbatim — the assertion loop at the end is the new content.)

In `tests/Entity/EntityRecordTableTest.cpp`:
1. Add layout asserts near the top:
```cpp
static_assert(sizeof(Astra::EntityRecord) == 32, "record must stay 2-per-cache-line");
static_assert(alignof(Astra::EntityRecord) == 32, "record must never straddle a cache line");
```
2. Add a chunk dummy helper beside `Fake()` and update every 3-arg `SetRecord` call:
```cpp
static ArchetypeChunk* FakeChunk(uintptr_t v) { return reinterpret_cast<ArchetypeChunk*>(v); }
// e.g.:
t.SetRecord(7, Fake(0x1234), FakeChunk(0x99), EntityLocation::Create(2, 3));
```
3. In `LocationRoundTripLeavesVersionUntouched`, also assert `EXPECT_EQ(r->chunk, FakeChunk(0x99));`.

- [ ] **Step 2: Build to verify RED**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: **compile FAILURE** — `EntityRecord` has no member `chunk`; `SetRecord` takes 3 args. (Compile-fail is this task's RED: the field and signature don't exist yet.)

- [ ] **Step 3: Widen EntityRecord**

`include/Astra/Entity/EntityRecord.hpp` — full new body:

```cpp
#pragma once

#include <type_traits>

#include "../Archetype/EntityLocation.hpp"
#include "Entity.hpp"

namespace Astra
{
    class Archetype;        // forward declarations — EntityRecord only stores pointers
    class ArchetypeChunk;

    // Unified entity slot: liveness (version) + storage location (archetype + location),
    // co-located so a single paged lookup can both validate a handle and locate it.
    // version == 0 (EntityTable::NULL_VERSION) marks a dead/empty slot; a slot is
    // "located" iff version matches the handle AND archetype != nullptr.
    //
    // `chunk` caches archetype->GetChunks()[location.GetChunkIndex()].get() so the
    // get hot path skips the chunks-vector hop. INVARIANT: whenever archetype != nullptr
    // and location.IsValid(), chunk points at that exact chunk. ALL writes to
    // archetype/chunk/location must go through EntityTable::SetRecord or
    // ArchetypeManager's SetRecordLocation/ClearRecordLocation helpers — never
    // assign the fields directly, or the cached pointer goes stale (UAF).
    // alignas(32): a 32B record must never straddle a cache line.
    struct alignas(32) EntityRecord
    {
        Archetype*           archetype = nullptr;  // null ⇒ no location assigned
        ArchetypeChunk*      chunk     = nullptr;  // cached chunk for the location
        EntityLocation       location;             // {chunkIndex, entityIndex}
        Entity::VersionType  version   = 0;        // 0 ⇒ dead/empty (NULL_VERSION)
    };

    static_assert(std::is_trivially_copyable_v<EntityRecord>,
        "EntityRecord must be trivially copyable so the paged table can memcpy/relocate slots");
    static_assert(std::is_trivially_destructible_v<EntityRecord>,
        "EntityRecord must be trivially destructible for cheap segment teardown");
}
```

- [ ] **Step 4: Widen EntityTable::SetRecord**

`include/Astra/Entity/EntityTable.hpp:174-179` — replace with:

```cpp
void SetRecord(IDType id, Archetype* archetype, ArchetypeChunk* chunk, EntityLocation location)
{
    EntityRecord* r = GetOrCreateRecord(id);   // does NOT change version
    r->archetype = archetype;
    r->chunk     = chunk;
    r->location  = location;
}
```

Also fix the stale segment-size comment at line 32 in passing: `// 64K entities × sizeof(EntityRecord) per segment (must be power of 2)`.

- [ ] **Step 5: Add the funnel helpers to ArchetypeManager**

In `include/Astra/Archetype/ArchetypeManager.hpp`, private section (near the record-discipline comment at ~1533):

```cpp
// The ONLY sanctioned writers of a record's storage fields (archetype/chunk/
// location). NEVER touch rec->version (EntityManager owns it). Spec §4.3.
static void SetRecordLocation(EntityRecord* rec, Archetype* arch,
                              ArchetypeChunk* chunk, EntityLocation loc) noexcept
{
    ASTRA_ASSERT(arch && chunk && loc.IsValid(), "SetRecordLocation: incomplete location");
    rec->archetype = arch;
    rec->chunk     = chunk;
    rec->location  = loc;
}

// Resolving overload: the chunk is L1-hot at every call site (the move that
// produced `loc` just wrote it), so this lookup is effectively free.
static void SetRecordLocation(EntityRecord* rec, Archetype* arch, EntityLocation loc)
{
    ASTRA_ASSERT(loc.IsValid() && loc.GetChunkIndex() < arch->GetChunks().size(),
                 "SetRecordLocation: location out of range");
    SetRecordLocation(rec, arch, arch->GetChunks()[loc.GetChunkIndex()].get(), loc);
}

static void ClearRecordLocation(EntityRecord* rec) noexcept
{
    rec->archetype = nullptr;
    rec->chunk     = nullptr;
    rec->location  = EntityLocation{};
}
```

- [ ] **Step 6: Route all 16 write sites through the funnel**

Line numbers are pre-change anchors from the spec inventory (§4.3); match by the quoted code. Transformations:

**(a) Create sites — 83-85 (`AddEntity`), 104-106 (`AddEntityWith`), 168-170 (`AddEntities` loop), 187-189 (`AddEntitiesWith` loop).** Each currently:
```cpp
EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
rec->archetype = archetype;
rec->location  = location;   // NEVER assign rec->version
```
becomes:
```cpp
EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
SetRecordLocation(rec, archetype, location);   // NEVER assign rec->version
```
(loop variants use `locations[i]` — same shape).

**(b) Swap-remove fixups — 207 (`RemoveEntity`), 245 (batch `RemoveEntities`).** The swapped/moved entity stays in the SAME archetype; re-derive its chunk from that archetype.

At 207 (single remove; `rec` is the entity being removed — read its archetype BEFORE the clear at 211-212, hoisting `Archetype* arch = rec->archetype;` above the `RemoveEntity` call if not already named):
```cpp
movedRec->location = oldLocation;
```
becomes:
```cpp
SetRecordLocation(movedRec, arch, oldLocation);
```

At 245 (batch; inside `for (auto& [archetype, entityBatch] : batches)` — the moved-entities fixup loop):
```cpp
if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
{
    rec->location = newEntityLocation;
}
```
becomes:
```cpp
if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
{
    SetRecordLocation(rec, archetype, newEntityLocation);
}
```

**(c) Clears — 212 (`RemoveEntity`), 255 (batch remove).** Currently:
```cpp
rec->archetype = nullptr;
rec->location  = EntityLocation{};
```
becomes:
```cpp
ClearRecordLocation(rec);
```

**(d) `SetEntityLocation` — 267-269.** Currently forwards to 3-arg `SetRecord`. New body (must tolerate null archetype / invalid location from any caller):
```cpp
void SetEntityLocation(Entity entity, Archetype* archetype, EntityLocation location)
{
    ArchetypeChunk* chunk =
        (archetype && location.IsValid() &&
         location.GetChunkIndex() < archetype->GetChunks().size())
            ? archetype->GetChunks()[location.GetChunkIndex()].get()
            : nullptr;
    m_records->SetRecord(entity.GetID(), archetype, chunk, location);   // archetype/chunk/location only
}
```

**(e) Deserialize wiring — 981-983.** Currently:
```cpp
rec->archetype = arch;
rec->location  = EntityLocation(chunkIndex, entityIndex);
```
becomes:
```cpp
SetRecordLocation(rec, arch, EntityLocation::Create(chunkIndex, entityIndex));
```
(the loop already walks `arch`'s chunks — the resolving overload re-derives the same pointer).

**(f) Batch destinations — 1242-1244 (`AllocateEntitySlot` dst), 1356-1358 (batch apply).** Currently:
```cpp
rec->archetype = dstArchetype;
rec->location  = dstLocation;   // or newLocations[i]
```
becomes:
```cpp
SetRecordLocation(rec, dstArchetype, dstLocation);   // or newLocations[i]
```

**(g) Moved-list fixups — 1271, 1382.** Both have `srcArchetype` in scope (verified in source).

At 1271 (descending-order swap-remove loop):
```cpp
movedRec->location = location;
```
becomes:
```cpp
SetRecordLocation(movedRec, srcArchetype, location);
```

At 1382 (post-`RemoveEntities(..., /*deferChunkCleanup=*/true)` fixup — safe to resolve: deferred cleanup only releases EMPTY chunks later, never ones holding these moved entities):
```cpp
if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
{
    rec->location = newLocation;
}
```
becomes:
```cpp
if (EntityRecord* rec = m_records->GetRecord(movedEntity.GetID())) ASTRA_LIKELY
{
    SetRecordLocation(rec, srcArchetype, newLocation);
}
```

**(h) Transition moves — 1145-1159 and 1415-1429.** `oldLoc` ALIASES the live record (`EntityRecord&`). Current shape:
```cpp
if (auto movedEntity = oldLoc.archetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
    movedRec->location = oldLoc.location;
// ...
oldLoc.archetype = newArchetype;
oldLoc.location = newEntityLocation;
```
becomes (ORDER MATTERS — the movedRec fixup resolves against the OLD archetype, so it must run before `oldLoc` is reassigned, as today):
```cpp
Archetype* srcArchetype = oldLoc.archetype;
if (auto movedEntity = srcArchetype->RemoveEntity(oldLoc.location)) ASTRA_LIKELY
    SetRecordLocation(movedRec, srcArchetype, oldLoc.location);
// ...
SetRecordLocation(&oldLoc, newArchetype, newEntityLocation);
```

After the sweep, verify no direct writes remain:
```bash
grep -nE "(->|\.)(location)\s*=" include/Astra/Archetype/ArchetypeManager.hpp
```
Expected: matches ONLY inside the three funnel helpers.

- [ ] **Step 7: Build + run suite to verify GREEN**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: build clean; **724 baseline tests + the new invariant/layout tests all pass**.

- [ ] **Step 8: Commit**

```bash
git add include/Astra/Entity/EntityRecord.hpp include/Astra/Entity/EntityTable.hpp include/Astra/Archetype/ArchetypeManager.hpp tests/Entity/EntityRecordTableTest.cpp tests/Registry/ArchetypeManagerTest.cpp
git commit -m "feat(entity): cache ArchetypeChunk* in EntityRecord behind a write funnel (32B alignas(32) record, all 16 sites routed)"
```

---

### Task 3: Hot-path rewrite (GetComponent) + characterization tests  — OPUS

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:513-521` (`GetComponent<T>`)
- Test: `tests/Registry/RegistryTest.cpp` (characterization: swap-remove/defrag/absent/stale gets)
- Test: `tests/Registry/EmptyTagTest.cpp` (tag present/absent through the `is_empty` branch)
- Test: `tests/Registry/RegistrySerializationTest.cpp` (post-load get — only if not already covered)

**Interfaces:**
- Consumes: Task 2's `rec->chunk` + invariant; existing `ArchetypeChunk::GetComponent<T>(size_t)` and `Archetype::GetComponent<T>(EntityLocation)`.
- Produces: no signature changes — behavior-identical faster `ArchetypeManager::GetComponent<T>(Entity)`.

- [ ] **Step 1: Write the characterization tests FIRST (they must pass on the OLD path)**

These pin current behavior so the rewrite can't drift. In `tests/Registry/RegistryTest.cpp` (reuse `Astra::Test::*` components; follow the file's existing fixture/creation pattern):

```cpp
TEST_F(RegistryTest, GetComponentAfterSwapRemove)
{
    using namespace Astra::Test;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i)
    {
        auto e = registry.CreateEntity();
        registry.AddComponent<Position>(e, float(i), 0.f, 0.f);
        es.push_back(e);
    }
    registry.DestroyEntity(es[10]);              // former last entity swaps into slot 10
    for (int i = 0; i < 50; ++i)
    {
        if (i == 10) continue;
        auto* p = registry.GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));         // each entity still reads ITS OWN value
    }
}

TEST_F(RegistryTest, GetComponentAfterDefragment)
{
    using namespace Astra::Test;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 60; ++i)
    {
        auto e = registry.CreateEntity();
        registry.AddComponent<Position>(e, float(i), 0.f, 0.f);
        es.push_back(e);
    }
    for (int i = 0; i < 60; i += 2)
        registry.DestroyEntity(es[i]);
    registry.Defragment();                       // CompactChunks moves survivors
    for (int i = 1; i < 60; i += 2)
    {
        auto* p = registry.GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));
    }
}

TEST_F(RegistryTest, GetComponentAbsentAndStale)
{
    using namespace Astra::Test;
    auto e = registry.CreateEntity();
    registry.AddComponent<Position>(e, 1.f, 2.f, 3.f);
    EXPECT_EQ(registry.GetComponent<Velocity>(e), nullptr);   // in-range absent (idToColumn < 0 path)
    auto dead = registry.CreateEntity();
    registry.AddComponent<Position>(dead, 9.f, 9.f, 9.f);
    registry.DestroyEntity(dead);
    EXPECT_EQ(registry.GetComponent<Position>(dead), nullptr); // stale handle (version guard)
}
```

(Adapt fixture/member names to the file's existing conventions — e.g. if the fixture exposes `registry` differently, mirror the neighboring tests.)

In `tests/Registry/EmptyTagTest.cpp` (reuses file-local `Tag`/`TPos` — NO new types). First check whether present/absent tag gets are already asserted; add only what's missing:

```cpp
TEST(EmptyTag, GetTagPresentAndAbsent)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity();
    reg.AddComponent<Tag>(a);
    EXPECT_NE(reg.GetComponent<Tag>(a), nullptr);   // present tag: shared static instance
    auto b = reg.CreateEntity();
    reg.AddComponent<TPos>(b, 1.f, 2.f, 3.f);
    EXPECT_EQ(reg.GetComponent<Tag>(b), nullptr);   // absent tag: mask-tested path says no
}
```

In `tests/Registry/RegistrySerializationTest.cpp`: check whether an existing round-trip test asserts `GetComponent` VALUES post-load. If yes, note it in the commit message and skip. If no, add one mirroring the file's existing save/load pattern with `EXPECT_FLOAT_EQ` value assertions on a handful of reloaded entities.

- [ ] **Step 2: Run to verify they PASS on the old path**

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=*GetComponentAfter*:*GetComponentAbsent*:*GetTagPresent*
```
Expected: PASS (characterization — these pin the behavior the rewrite must preserve). Commit them separately:

```bash
git add tests/Registry/RegistryTest.cpp tests/Registry/EmptyTagTest.cpp tests/Registry/RegistrySerializationTest.cpp
git commit -m "test(registry): characterization tests for GetComponent (swap-remove/defrag/absent/stale/tag) ahead of hot-path rewrite"
```

- [ ] **Step 3: Rewrite the hot path**

`include/Astra/Archetype/ArchetypeManager.hpp` — replace the body of `GetComponent<T>` (currently 513-521):

```cpp
template<Component T>
ASTRA_NODISCARD T* GetComponent(Entity entity)
{
    EntityRecord* rec = m_records->GetRecord(entity.GetID());
    if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
        return nullptr;

    if constexpr (std::is_empty_v<T>)
    {
        // Tags have no storage column (idToColumn == -1 whether present or not),
        // so presence MUST come from the archetype mask.
        return rec->archetype->GetComponent<T>(rec->location);
    }
    else
    {
        // Load-bearing guard, not decoration: the old path's safety for
        // over-ceiling/collision-refused ids (INVALID_COMPONENT, Theme E) came
        // from Bitmap::Test's internal range check, which this fast path skips.
        // Without it, idToColumn[id] is an OOB read in Release. Register-only
        // compare — zero memory traffic. Spec §4.4/§4.5.
        const ComponentID id = TypeID<T>::Value();
        if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            return nullptr;

        ASTRA_ASSERT(rec->chunk ==
                     rec->archetype->GetChunks()[rec->location.GetChunkIndex()].get(),
                     "EntityRecord chunk/location desync");
        return rec->chunk->GetComponent<T>(rec->location.GetEntityIndex());
    }
}
```

(`id` is computed but unused on the non-guard path — `GetComponent<T>` inside the chunk recomputes it; that's fine, `TypeID<T>::Value()` is a memoized static read and the optimizer folds the duplicate.)

- [ ] **Step 4: Build + full Debug suite (the desync-assert sweep)**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: ALL tests pass — every structural test in the suite now sweeps the desync assert; any Task-2 funnel miss aborts here with "EntityRecord chunk/location desync".

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "perf(archetype): GetComponent reads rec->chunk directly - kills m_chunks[i] hop + mask test (MAX_COMPONENTS guard preserves Bitmap::Test range safety)"
```

---

### Task 4: Registry ByID/hash paths read `record->chunk`

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` — 4 sites: ~495-506 (ComponentAdded signal), ~556-568 (ComponentRemoved signal), ~634-638 (`GetComponentByHash`), ~740-756 (`ForEachComponent` inspection)

**Interfaces:**
- Consumes: Task 2's `record->chunk` (all four sites already fetch via the validated `GetEntityRecord` — `ArchetypeManager.hpp:260`).
- Produces: no API change.

No new tests: the signal-emission and reflection suites cover these paths; Task 3's desync assert guards the pointer. All four `componentId`s come from validated sources (descriptor lookup / hash map / `ForEachComponent`) — no id guard needed (spec §4.5).

- [ ] **Step 1: Rewrite the four sites**

**(a) ~495-506** — currently:
```cpp
auto& chunks = record->archetype->GetChunks();
if (record->location.GetChunkIndex() < chunks.size())
{
    void* actualPtr = nullptr;
    if (desc->size == 0)
    {
        actualPtr = EmptyComponentSentinel();  // present tag: no data
    }
    else
    {
        void* compPtr = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
        actualPtr = compPtr ? static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size : nullptr;
    }
    ...
```
becomes:
```cpp
if (record->chunk)
{
    void* actualPtr = nullptr;
    if (desc->size == 0)
    {
        actualPtr = EmptyComponentSentinel();  // present tag: no data
    }
    else
    {
        void* compPtr = record->chunk->GetComponentArrayByID(componentId);
        actualPtr = compPtr ? static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size : nullptr;
    }
    ...
```

**(b) ~556-568** — same transformation (`auto& chunks = ...` + bounds check → `if (record->chunk)`; `chunks[...]->GetComponentArrayByID(componentId)` → `record->chunk->GetComponentArrayByID(componentId)`).

**(c) ~634-638** — currently:
```cpp
auto& chunks = record->archetype->GetChunks();
if (record->location.GetChunkIndex() >= chunks.size())
    return nullptr;

void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
```
becomes:
```cpp
if (!record->chunk)
    return nullptr;

void* compArray = record->chunk->GetComponentArrayByID(componentId);
```

**(d) ~740-756** — the outer guard `if (record->location.GetChunkIndex() >= chunks.size()) return result;` becomes `if (!record->chunk) return result;`; line 756 `chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(id)` becomes `record->chunk->GetComponentArrayByID(id)`. Delete each site's now-unused `auto& chunks = record->archetype->GetChunks();` (keep it only if something else in scope still uses it — at (d) check the lines between 742 and 756).

- [ ] **Step 2: Build + full Debug suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: all pass (signal + reflection + serialization suites exercise all four sites).

- [ ] **Step 3: Commit**

```bash
git add include/Astra/Registry/Registry.hpp
git commit -m "perf(registry): ByID/hash paths read record->chunk directly"
```

---

### Task 5: 3-config verify + benchmark A/B + RESULTS.md

**Files:**
- Modify: `bench-compare/RESULTS.md` (new A/B section)

**Interfaces:**
- Consumes: the completed branch; `bench-compare/` harness (untracked but present).
- Produces: measured random_get delta vs baseline 69.7ns (flecs 62.8); go/no-go evidence for the merge.

- [ ] **Step 1: Authoritative 3-config build + test**

```powershell
foreach ($cfg in 'Debug','Release','Dist') {
  & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=$cfg -p:Platform=x64 -m
  & ".\bin\$cfg-windows-x86_64\AstraTest\AstraTest.exe"
}
```
Expected: green in all three; counts = baseline 724/722/722 + this branch's new tests (report exact numbers). `CompressionTest.PerformanceBenchmark` alone failing = rerun isolated.

- [ ] **Step 2: Rebuild bench_astra against the branch**

```powershell
cd bench-compare
cmd /c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /DNDEBUG /EHsc /nologo /I..\include /I..\vendor\Mosaic\include bench_astra.cpp advapi32.lib'
```
Expected: `bench_astra.exe` builds clean (advapi32 required for huge-page privilege syms).

- [ ] **Step 3: Quiet-machine interleaved rounds**

On a settled machine, from `bench-compare/`, run 5-7 interleaved rounds:
```powershell
foreach ($i in 1..6) { .\bench_astra.exe; .\bench_flecs.exe; .\bench_entt.exe }
```
Take per-op MEDIANS across rounds. Success criteria (spec §2):
- **random_get ≤ ~63ns** (baseline 69.7; flecs 62.8) — the lever's target.
- create/add/remove/iterate within noise of baseline (48.6 / 52.3 / 38.3 / 0.451-1.24) — the funnel's extra L1 load must not show.
If random_get improves but misses ≤63, report the honest number — the spec calls the target a prediction, not a promise.

- [ ] **Step 4: Update RESULTS.md + commit**

Append a dated section to `bench-compare/RESULTS.md`: branch, commit, per-op medians table (Astra before/after, flecs, EnTT), delta %, and one paragraph interpreting vs the spec's §8 prediction.

```bash
git add bench-compare/RESULTS.md
git commit -m "perf(bench): random_get record->direct-chunk-pointer A/B results"
```

---

### Finishing (SDD flow — after final review)

Opus whole-branch review → fix wave if needed → authoritative 3-config on the final commit → **confirm with user** → FF-merge to dev locally, delete branch, do NOT push → update memory.
