# W6 + W2 — Hash Hygiene + Array-Indexed Archetype Edges — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the archetype-edge-lookup cost from the add/remove path — W6 makes every `FlatMap` hasher-robust (avalanche mix), W2 replaces the pointer-keyed edge map with per-archetype `ComponentID`-indexed arrays (single index, no hashing).

**Architecture:** W6 adds an `fmix64` finalizer in `SwissTable::Mix`, called by `FlatMap::SplitHash`. W2 moves add/remove edges into each `Archetype` as lazily-allocated `std::unique_ptr<Archetype*[]>` arrays; `ArchetypeManager` reads `from->GetAddEdge(id)` and owns edge invalidation on archetype deletion; `ArchetypeGraph` is deleted.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln` via MSBuild, 3 configs), GoogleTest.

**Spec:** `docs/superpowers/specs/2026-07-22-w6-w2-archetype-edges-design.md`

## Global Constraints

- **Header-only, C++20.** No new dependencies. Exception-free (no `throw`), RTTI-off (no `<any>`/`typeid`/`dynamic_cast`). Match surrounding `ASTRA_*` / `noexcept` / `ASTRA_ASSERT` style.
- **Edges are a runtime cache — never serialized.** No on-disk format change from either part.
- **`MAX_COMPONENTS` (128) is the `ComponentID` ceiling** — all ids fit one array; no high-id fallback.
- **UAF hazard (W2):** on archetype deletion, every cached edge pointing to it must be nulled **before** it is freed. The invalidation scan must run in the reference-cleanup pass, before the `unique_ptr.reset()` that frees it.
- **W6 changes `FlatMap` iteration order** (hash-dependent). No Astra persistence may depend on it — the serialization suite is the net.
- **`ArchetypeManager` must never write `EntityRecord::version`** (unchanged from W1; this work doesn't touch entity records).
- **TypeID ceiling:** tests reuse `tests/TestComponents.hpp` (`Astra::Test::Position/Velocity/Health`) and existing component types; register no new ones. `Archetype`/edge unit tests take a bare `ComponentMask` and register zero components.
- **3-config green:** MSVC **Debug, Release, Dist** all pass. Build the whole solution (`-t:AstraTest` does not work).

**Build / test / bench commands:**
```bash
# Build one config (repeat Debug/Release/Dist):
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
# Run tests:
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter='FlatMapTest.*'
# Bench (Task 4): rebuild bench_astra via bench-compare/build_one.bat, run all three exes.
```
No new source **files** are added by any task (only edits to existing files + one file deletion), so **no premake regeneration is needed**. Judge success ONLY by the MSVC build + GoogleTest — the IDE clang/clangd diagnostics (missing `Mosaic/Platform.hpp`, `MOSAIC_NODISCARD`, `gtest not found`, spurious unused-include/undeclared) are KNOWN false positives. Stale PDB lock → `taskkill //F //IM mspdbsrv.exe` + clean rebuild.

Branch: `perf/w6-w2-archetype-edges` (off dev `13ee47b`).

---

### Task 1: W6 — avalanche mix in `SplitHash` + distribution test

**Files:**
- Modify: `include/Astra/Container/Swiss.hpp` (add `SwissTable::Mix`)
- Modify: `include/Astra/Container/FlatMap.hpp:918` (`SplitHash` calls `Mix`)
- Test: `tests/Container/FlatMapInternalsTest.cpp` (append two tests — existing file)

**Interfaces:**
- Produces: `Astra::SwissTable::Mix(std::size_t) -> std::size_t` (inline, in the `SwissTable` namespace next to `H1`/`H2`).

- [ ] **Step 1: Write the failing distribution + functional tests**

Append to `tests/Container/FlatMapInternalsTest.cpp` (add `#include "Astra/Container/Swiss.hpp"` and `#include <cstdint>` near the top if not present):

```cpp
// W6: the avalanche mix must spread H2 for identity/pointer-like (power-of-two-strided)
// keys. Pre-W6, H2(aligned) collapsed to a single value (SIMD group filter no-op).
TEST_F(FlatMapTest, SplitHashMixSpreadsH2ForAlignedKeys)
{
    std::set<uint8_t> h2values;
    for (std::size_t i = 1; i <= 1000; ++i)
    {
        std::size_t aligned = i * 16;  // 16-byte-aligned, like heap pointers
        h2values.insert(Astra::SwissTable::H2(Astra::SwissTable::Mix(aligned)));
    }
    EXPECT_GT(h2values.size(), 100u) << "H2 not well-distributed after Mix";
}

// W6: functional guard — many aligned-pointer keys insert/find correctly.
TEST_F(FlatMapTest, AlignedPointerKeysResolveCorrectly)
{
    Astra::FlatMap<void*, int> map;
    std::vector<void*> keys;
    for (int i = 1; i <= 2000; ++i)
    {
        void* k = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i) * 16);
        keys.push_back(k);
        map[k] = i;
    }
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        auto it = map.Find(keys[i]);
        ASSERT_NE(it, map.end());
        EXPECT_EQ(it->second, static_cast<int>(i) + 1);
    }
    EXPECT_EQ(map.Size(), keys.size());
}
```

- [ ] **Step 2: Build Debug and confirm `SplitHashMixSpreadsH2ForAlignedKeys` FAILS to compile**

Run: build Debug. Expected: FAIL — `SwissTable::Mix` does not exist yet.

- [ ] **Step 3: Add `SwissTable::Mix`**

In `include/Astra/Container/Swiss.hpp`, inside `namespace SwissTable`, immediately after `H1` (around line 59):

```cpp
// Avalanche finalizer (murmur3 fmix64): spread all bits so both H1 (low bits,
// position) and H2 (top 7 bits, tag) get full entropy even from identity hashers
// (raw pointers, small ints). Without this, pointer keys share their top bits =>
// H2 is constant => the SIMD group filter degenerates to a linear probe.
inline std::size_t Mix(std::size_t hash) noexcept
{
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}
```

- [ ] **Step 4: Call `Mix` from `SplitHash`**

In `include/Astra/Container/FlatMap.hpp:918`, change `SplitHash` to mix before splitting:

```cpp
static std::pair<std::size_t, std::uint8_t> SplitHash(std::size_t hash) noexcept
{
    hash = SwissTable::Mix(hash);            // W6: avalanche for identity/pointer hashers
    std::uint8_t h2 = SwissTable::H2(hash);
    return {hash, h2};
}
```

Leave `EntityHash`'s manual H2-fixup (`Entity.hpp:179-183`) in place (now redundant, harmless).

- [ ] **Step 5: Build Debug + run the FlatMap tests**

Run: build Debug, then `AstraTest.exe --gtest_filter='FlatMapTest.*'`. Expected: PASS (both new tests + existing FlatMap tests).

- [ ] **Step 6: Full Debug suite (iteration-order-change regression gate)**

Run: `AstraTest.exe` (whole suite, Debug). Expected: all green — especially the serialization tests (`RegistrySerializationTest`, `BinarySerialization*`, `FormatV2Test`, `LoadRobustnessTest`) and container fuzz tests, which would surface any hidden dependence on `FlatMap` iteration order. If a serialization test regresses, a persisted structure depends on map order — STOP and report (this is a real finding, not a test to adjust).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Container/Swiss.hpp include/Astra/Container/FlatMap.hpp tests/Container/FlatMapInternalsTest.cpp
git commit -m "perf(container): avalanche-mix FlatMap hashes (fmix64) so identity/pointer keys distribute"
```

---

### Task 2: W2a — `Archetype` edge storage + accessors + unit test (additive)

Adds the per-archetype edge arrays and their accessors. Purely additive — `ArchetypeGraph` is still present and used, so the tree compiles and passes throughout. The manager rewire is Task 3.

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (edge members + accessors; ensure `#include <memory>`)
- Test: `tests/Registry/ArchetypeTest.cpp` (append a unit test — existing file)

**Interfaces:**
- Produces (public methods on `Archetype`, `ComponentID` params):
  - `Archetype* GetAddEdge(ComponentID) const noexcept` / `GetRemoveEdge(ComponentID) const noexcept` — `nullptr` if unset.
  - `void SetAddEdge(ComponentID, Archetype*)` / `SetRemoveEdge(ComponentID, Archetype*)` — lazy-allocates the array.
  - `void ClearEdgesTo(Archetype* target) noexcept` — nulls every add/remove slot equal to `target`.

- [ ] **Step 1: Write the failing unit test**

Append to `tests/Registry/ArchetypeTest.cpp` (match its existing includes/namespace usage):

```cpp
// W2: per-archetype edge arrays — lazy alloc, get/set, and targeted invalidation.
TEST(Archetype, EdgeStorageLazyGetSetAndClear)
{
    using namespace Astra;
    Archetype a(ComponentMask{});
    Archetype b(ComponentMask{});
    Archetype c(ComponentMask{});

    EXPECT_EQ(a.GetAddEdge(5), nullptr);      // lazy: nothing allocated yet
    EXPECT_EQ(a.GetRemoveEdge(5), nullptr);

    a.SetAddEdge(5, &b);
    a.SetRemoveEdge(7, &c);
    a.SetAddEdge(9, &c);
    EXPECT_EQ(a.GetAddEdge(5), &b);
    EXPECT_EQ(a.GetRemoveEdge(7), &c);
    EXPECT_EQ(a.GetAddEdge(9), &c);
    EXPECT_EQ(a.GetAddEdge(6), nullptr);      // untouched slot

    a.ClearEdgesTo(&c);                       // null only edges pointing at c
    EXPECT_EQ(a.GetAddEdge(5), &b);           // -> b, unaffected
    EXPECT_EQ(a.GetRemoveEdge(7), nullptr);   // was -> c, cleared
    EXPECT_EQ(a.GetAddEdge(9), nullptr);      // was -> c, cleared
}
```

- [ ] **Step 2: Build Debug, confirm the test FAILS to compile**

Expected: FAIL — the edge methods don't exist.

- [ ] **Step 3: Add the edge members + accessors to `Archetype`**

In `include/Astra/Archetype/Archetype.hpp`: ensure `#include <memory>` is present. Add the accessors to the **public** section (near `GetMask()` / other public methods):

```cpp
ASTRA_NODISCARD Archetype* GetAddEdge(ComponentID id) const noexcept
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    return m_addEdges ? m_addEdges[id] : nullptr;
}
ASTRA_NODISCARD Archetype* GetRemoveEdge(ComponentID id) const noexcept
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    return m_removeEdges ? m_removeEdges[id] : nullptr;
}
void SetAddEdge(ComponentID id, Archetype* to)
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    if (!m_addEdges) m_addEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);  // value-inits to nullptr
    m_addEdges[id] = to;
}
void SetRemoveEdge(ComponentID id, Archetype* to)
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    if (!m_removeEdges) m_removeEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);
    m_removeEdges[id] = to;
}
void ClearEdgesTo(Archetype* target) noexcept
{
    if (m_addEdges)    for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_addEdges[i]    == target) m_addEdges[i]    = nullptr;
    if (m_removeEdges) for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_removeEdges[i] == target) m_removeEdges[i] = nullptr;
}
```

Add the members to the **private** section (near the other private members ~line 1416):

```cpp
// Add/remove transition edges, indexed by ComponentID (< MAX_COMPONENTS).
// Lazily allocated on first edge; nullptr slot = no cached edge; freed with the archetype.
std::unique_ptr<Archetype*[]> m_addEdges;
std::unique_ptr<Archetype*[]> m_removeEdges;
```

(The defaulted `~Archetype()` frees them; the ctor needs no change — the `unique_ptr`s default to null.)

- [ ] **Step 4: Build Debug + run the Archetype test**

Run: build Debug, `AstraTest.exe --gtest_filter='Archetype.*'`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp tests/Registry/ArchetypeTest.cpp
git commit -m "feat(archetype): per-archetype lazy edge arrays (GetAddEdge/SetAddEdge/ClearEdgesTo)"
```

---

### Task 3: W2b — rewire `ArchetypeManager` to per-archetype edges + delete `ArchetypeGraph`

Switch the edge lookup to the new arrays, move invalidation into the manager, and remove `ArchetypeGraph` entirely. Atomic: deleting `m_edgeGraph` + the header makes every remaining reference a compile error (the completeness net).

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (rewire `GetArchetypeWithModified`, invalidation, remove member + include + `Clear()` call)
- Modify: `include/Astra/Astra.hpp` (remove the `ArchetypeGraph.hpp` include)
- Delete: `include/Astra/Archetype/ArchetypeGraph.hpp`
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (append an invalidation integration test)

**Interfaces:**
- Consumes: `Archetype::{GetAddEdge, GetRemoveEdge, SetAddEdge, SetRemoveEdge, ClearEdgesTo}` (Task 2).

- [ ] **Step 1: Write the failing invalidation integration test**

Append to `tests/Registry/ArchetypeManagerTest.cpp` (uses the fixture: `manager`, `componentRegistry` with `Position/Velocity/Health`, `m_table` seeded for ids [0,10000), `Astra::Test::*` components). Confirm the exact `AddComponent`/`RemoveComponent`/`GetComponent` signatures against the file's existing `TEST_F`s and the `DefragmentOptions` fields (`ArchetypeManager.hpp:633-638`) before finalizing:

```cpp
// W2: after an empty archetype is defragmented away, cached edges to it must be
// invalidated (no dangling pointer) — subsequent transitions must recompute correctly.
TEST_F(ArchetypeManagerTest, EdgesInvalidatedWhenArchetypeDefragmented)
{
    using namespace Astra;
    using namespace Astra::Test;

    Entity e0(1, 1);
    Entity e1(2, 1);
    manager->AddEntityWith<Position>(e0, Position{1, 0, 0});   // archetype {Position}
    manager->AddEntityWith<Position>(e1, Position{2, 0, 0});

    // Transition e0 {Position} -> {Position,Velocity}: caches {Position}'s add-edge for Velocity.
    manager->AddComponent<Velocity>(e0, Velocity{7, 7, 7});
    // Empty the {Position,Velocity} archetype again.
    manager->RemoveComponent<Velocity>(e0);

    // Force removal of the now-empty {Position,Velocity} archetype (default keeps >= 8).
    ArchetypeManager::DefragmentOptions opts;
    opts.minArchetypesToKeep = 1;              // adjust field name/value to the real struct
    auto result = manager->Defragment(opts);
    EXPECT_GE(result.emptyArchetypesRemoved, 1u);

    // The cached {Position}->Velocity add-edge now points at a freed archetype IF
    // invalidation failed. A correct recompute yields e1 with the right Velocity.
    Velocity* v = manager->AddComponent<Velocity>(e1, Velocity{9, 9, 9});
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(v->x, 9.0f);
    Position* p = manager->GetComponent<Position>(e1);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 2.0f);               // still correct after the transition
}
```

- [ ] **Step 2: Build Debug — expect PASS-or-COMPILE (test compiles against Task 2's API; the rewire is next)**

The test compiles now (uses public manager API). It should PASS even pre-rewire (old graph still invalidates). Its real value is guarding the post-rewire path. Run it: `AstraTest.exe --gtest_filter='ArchetypeManagerTest.EdgesInvalidated*'` → PASS.

- [ ] **Step 3: Rewire `GetArchetypeWithModified` to the per-archetype edges**

In `include/Astra/Archetype/ArchetypeManager.hpp`, change `GetArchetypeWithModified` (`:1020`) so the `getEdge`/`setEdge` callbacks take `from` directly (drop the graph param):

```cpp
template<typename GetEdgeFunc, typename SetEdgeFunc, typename MaskOp>
Archetype* GetArchetypeWithModified(Archetype* from, ComponentID componentId,
                                    GetEdgeFunc&& getEdge, SetEdgeFunc&& setEdge, MaskOp&& maskOp)
{
    if (Archetype* target = getEdge(from, componentId)) ASTRA_LIKELY
        return target;
    // ... unchanged mask/lookup/create body ...
    setEdge(from, componentId, to);   // BOTH cache sites (was :1034 and :1079)
    return to;
}
```
And the wrappers (`:1084`, `:1092`):
```cpp
Archetype* GetArchetypeWithAdded(Archetype* from, ComponentID componentId)
{
    return GetArchetypeWithModified(from, componentId,
        [](Archetype* f, ComponentID id) { return f->GetAddEdge(id); },
        [](Archetype* f, ComponentID id, Archetype* to) { f->SetAddEdge(id, to); },
        [](ComponentMask& mask, ComponentID id) { mask.Set(id); });
}
Archetype* GetArchetypeWithRemoved(Archetype* from, ComponentID componentId)
{
    return GetArchetypeWithModified(from, componentId,
        [](Archetype* f, ComponentID id) { return f->GetRemoveEdge(id); },
        [](Archetype* f, ComponentID id, Archetype* to) { f->SetRemoveEdge(id, to); },
        [](ComponentMask& mask, ComponentID id) { mask.Reset(id); });
}
```

- [ ] **Step 4: Replace edge invalidation on archetype deletion**

At the reference-cleanup pass (`ArchetypeManager.hpp:729-731`), replace
`m_edgeGraph.RemoveEdgesTo(archetype); m_edgeGraph.RemoveEdgesFrom(archetype);` with a scan-all that nulls incoming edges **before** the archetype is freed (the `reset()` is a later pass, so all archetypes are still alive here):

```cpp
// Null any cached edge (in any surviving archetype) that points to `archetype`
// before it is freed in the next pass — a lingering edge would dangle (UAF).
// Its own outgoing edges are freed when its unique_ptr is reset.
for (auto& entry : m_archetypes)
    if (entry.archetype && entry.archetype.get() != archetype)
        entry.archetype->ClearEdgesTo(archetype);
```

- [ ] **Step 5: Remove the `ArchetypeGraph` member, its `Clear()` call, and the includes; delete the header**

- Delete `m_edgeGraph.Clear();` (`ArchetypeManager.hpp:613`).
- Delete the member `ArchetypeGraph m_edgeGraph;` (`:1517`).
- Remove `#include "ArchetypeGraph.hpp"` from `ArchetypeManager.hpp` and `#include ".../ArchetypeGraph.hpp"` from `include/Astra/Astra.hpp`.
- `git rm include/Astra/Archetype/ArchetypeGraph.hpp`.
- Build Debug; fix any remaining `m_edgeGraph` / `ArchetypeGraph` compile error (the deleted member/header makes every leftover reference a hard error — the completeness net). Grep the tree once more for `ArchetypeGraph`/`m_edgeGraph` to confirm none remain in src.

- [ ] **Step 6: Full 3-config build + full suite**

Build Debug/Release/Dist; run `AstraTest.exe` each. Expected: all green — especially `ArchetypeTest`, `ArchetypeManagerTest` (incl. the new invalidation test), `RegistryTest`, `ViewInvalidationTest`, `RootArchetypeRoundTripTest`, and the serialization suite. Add/remove component operations exercise the rewired edge path throughout.

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Astra.hpp tests/Registry/ArchetypeManagerTest.cpp
git rm include/Astra/Archetype/ArchetypeGraph.hpp
git commit -m "perf(archetype): array-indexed per-archetype edges; delete ArchetypeGraph; own invalidation"
```

---

### Task 4: Benchmark, update RESULTS.md

**Files:**
- Modify: `bench-compare/RESULTS.md`
- Modify: `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` (mark W6+W2 done)

- [ ] **Step 1: Rebuild the Astra benchmark + run all three**

Rebuild `bench_astra.exe` (public API unchanged, so `bench_astra.cpp` compiles as-is):
`cmd //c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /DNDEBUG /EHsc /I..\include /I..\vendor\Mosaic\include bench_astra.cpp advapi32.lib'`
Run `./bench_astra.exe`, `./bench_entt.exe`, `./bench_flecs.exe` (EnTT/flecs unchanged). Capture the CSV; median of a few Astra runs.

- [ ] **Step 2: Update RESULTS.md + sanity-check**

Update the Astra row(s) in `bench-compare/RESULTS.md`. Expected: **add ~150→~100-110, remove ~106→~70**; create/random_get roughly unchanged from W1 (edge lookup isn't on those paths); iteration unchanged. Be honest — if add/remove didn't move, verify `GetArchetypeWithAdded/Removed` actually read `from->GetAddEdge` (not a leftover path). Note the residual add/remove gap to flecs (53/32) is the transition *move* — W3's target.

- [ ] **Step 3: Mark W6+W2 done in the perf plan**

In `docs/reviews/2026-07-21-astra-perf-optimization-plan.md`, note W6 and W2 landed with the measured numbers; next = Phase C (W5/W4/W3/W7).

- [ ] **Step 4: Commit**

```bash
git add bench-compare/RESULTS.md docs/reviews/2026-07-21-astra-perf-optimization-plan.md
git commit -m "perf(bench): record W6+W2 results (add/remove edge-lookup removed)"
```

---

## Finish (SDD close-out)
After all tasks pass in Debug/Release/Dist, fast-forward-merge `perf/w6-w2-archetype-edges` into `dev` locally, delete the branch, do not push. Confirm with the user before merging. (Controller updates the perf memory.)

---

## Self-review — spec coverage
- W6 avalanche mix in SplitHash → Task 1 (Steps 3-4). ✅
- W6 testability via `SwissTable::Mix` + H2 distinctness → Task 1 Step 1. ✅
- W6 iteration-order-change gate (serialization suite) → Task 1 Step 6. ✅
- W2 per-archetype lazy edge arrays + accessors + ClearEdgesTo → Task 2. ✅
- W2 rewire `GetArchetypeWithModified` to `from->GetAddEdge` → Task 3 Step 3. ✅
- W2 invalidation before free (UAF hazard) → Task 3 Step 4 + integration test Step 1. ✅
- W2 delete `ArchetypeGraph` + remove member/include/Clear → Task 3 Step 5. ✅
- No serialization/format change (edges are runtime cache) → not serialized; serialization suite is the W6 net. ✅
- MAX_COMPONENTS ceiling → single array, asserts in accessors. ✅
- 3-config verification → Task 1 Step 6 (Debug), Task 3 Step 6 (3-config). ✅
- Benchmark + RESULTS → Task 4. ✅
- No-new-files / no premake regen → all tasks edit existing files + one deletion. ✅
