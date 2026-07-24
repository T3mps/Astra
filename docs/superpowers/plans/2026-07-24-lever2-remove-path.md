# Lever 2 — Remove-Path Redundancy Elimination Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remove-path gap to flecs (37.4 vs 33.8 ns/op) by eliminating redundant record validations at the Registry seam (Phase A) and wasted work in the transition-move internals (Phase B: Destruct gate, direct column addressing, resize kill, fused move-out+backfill).

**Architecture:** Per spec `docs/superpowers/specs/2026-07-24-lever2-remove-path-design.md` (user-approved, all anchors source-verified). ArchetypeManager entry points become the single validation authority; Registry's per-entity structural paths stop pre-validating and hide signal-payload work behind hoisted `IsSignalEnabled` checks. Move internals get a `Destruct` triviality gate mirroring `MoveConstruct`/`DefaultConstruct`, ordinal-direct column addressing in the merge-join, a `push_back` slot append, and (checkpoint-gated) a fused single-pass move-out+backfill primitive for remove transitions.

**Tech Stack:** Header-only C++20, MSVC (premake5 `Astra.sln`), GoogleTest, 3-way bench harness in `bench-compare/`.

## Global Constraints

- Branch: `perf/lever2-remove-path` off dev @ `98ab690`. Local only — NEVER push. Finish = FF-merge to dev locally, delete branch (confirm with user first).
- Build (PowerShell, whole solution — `-t:AstraTest` does NOT work): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m`. Tests: `.\bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe`.
- Test baseline (dev @ `390fb10`/`98ab690`): **Debug 732 / Release 730 / Dist 730** (2-test delta = `EXPECT_DEATH`-only). Lone `CompressionTest.PerformanceBenchmark` failure = known flake → rerun isolated. IDE clang diagnostics = false positives; judge ONLY by MSVC. Stale PDB lock → `taskkill /F /IM mspdbsrv.exe`.
- **TypeID ceiling: register NO new component types in tests.** Reuse `Astra::Test::*` (`tests/TestComponents.hpp`: `Position`/`Velocity`/`Health` + move-only `Tracked` with `s_live`) and `SignalLifetimeTest.cpp`'s file-local `Tracked` (already registered).
- **Record-write funnel discipline (Lever 1 invariant):** any write to a record's `archetype`/`chunk`/`location` goes through `EntityTable::SetRecord` / `SetRecordLocation` / `ClearRecordLocation` — never direct. The Debug desync assert in `GetComponent` sweeps the whole suite.
- **Signal CONTRACT unchanged:** what is emitted, when (Removed BEFORE removal, Added after), and with what payload — identical. Only whether payload work happens when the signal is disabled changes. Spec §3.1 behavior table governs.
- `is_trivially_copyable` / `is_trivially_destructible` gates are CORRECTNESS boundaries — trivial fast paths must never apply to non-trivial components.
- No on-disk format change; no public API change. Exception-free; ASCII comments; Allman braces; `ASTRA_ASSERT`/`ASTRA_LIKELY` idioms.
- No new source FILES (tests go into existing files) ⇒ NO premake regen needed.
- Bench protocol (checkpoints 1-3): full-opt flags, PowerShell tool for the bench build (the Bash tool's `cmd /c` silently no-ops `build_one.bat` — known gotcha), typeperf quiet check (~<20% sustained), 6 interleaved rounds astra→flecs→entt, per-op medians, paired same-session comparison only. Baseline medians (dev @ 390fb10): Astra create 49.90 / add 48.63 / **remove 37.36** / random_get 60.03 / iterate1 0.484 / iterate2 1.046 / iterate3 0.997; flecs 89.54 / 50.99 / **33.84** / 56.21 / 0.434 / 0.885 / 1.010. Target: remove ≤ ~34 (prediction, not promise); everything else within noise.
- Model recipe (SDD): OPUS for Task 5 (B2) + the final whole-branch review; sonnet for Tasks 1, 2, 3, 6. Task 4 is a controller gate, not a subagent dispatch.

---

### Task 1: Phase A seam sweep (5 sites) + Lever-1 rider + signal-contract tests + checkpoint 1

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (:281-293 AddComponent, :295-307 EmplaceComponent, :309-324 RemoveComponent, :478-483 AddComponentByID head, :540-578 RemoveComponentByID)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (typed `GetComponent<T>` non-tag branch — the Lever-1 rider null-check)
- Test: `tests/Registry/SignalLifetimeTest.cpp` (contract tests; reuses its file-local `Tracked`)

**Interfaces:**
- Consumes: `ArchetypeManager::RemoveComponent<T>(Entity)` (validates version+archetype+mask, `ArchetypeManager.hpp:339-360`); `ArchetypeManager::AddComponent<T>` (returns nullptr for invalid/already-present, :274-301); `SignalManager::IsSignalEnabled(Signal)` (`Signal.hpp:262-265`); `reg.EnableSignals(...)` / `GetSignalManager()->On<E>().Register(...)` test idiom (`SignalLifetimeTest.cpp:25-42`).
- Produces: no signature changes anywhere — behavior-identical faster Registry paths. Later tasks depend on nothing from this task.

- [ ] **Step 1: Write the contract tests (characterization — they must PASS on the CURRENT code)**

This is a behavior-preserving refactor, so a red step is not honestly constructible; these tests pin the §3.1 behavior table so the rewrite cannot drift (same disclosed pattern as Lever 1 Task 3). Append to `tests/Registry/SignalLifetimeTest.cpp` (reuse its anonymous-namespace `Tracked`; note the existing `ComponentRemovedSeesLiveValue` already covers "present + enabled ⇒ emit-before-removal with live payload" — do NOT duplicate it):

```cpp
TEST(SignalContract, RemoveDisabledNeverEmitsAndReturnsMatchTheTable)
{
    Astra::Registry reg;   // signals disabled by default
    int calls = 0;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++calls; });

    auto e = reg.CreateEntity<Tracked>();
    EXPECT_TRUE(reg.RemoveComponent<Tracked>(e));    // present -> removed
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(e));   // now absent -> false

    auto dead = reg.CreateEntity<Tracked>();
    reg.DestroyEntity(dead);
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(dead)); // stale handle -> false

    EXPECT_EQ(calls, 0);   // disabled: handler registered but nothing may fire
}

TEST(SignalContract, RemoveEnabledAbsentAndStaleEmitNothing)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);
    int calls = 0;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved&) { ++calls; });

    auto noComp = reg.CreateEntity();                    // no Tracked on it
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(noComp));  // absent -> false, no emit

    auto dead = reg.CreateEntity<Tracked>();
    reg.DestroyEntity(dead);
    EXPECT_FALSE(reg.RemoveComponent<Tracked>(dead));    // stale -> false, no emit

    EXPECT_EQ(calls, 0);
}

TEST(SignalContract, AddEmitsOnlyWhenEnabledWithTheNewPointer)
{
    {
        Astra::Registry reg;
        reg.EnableSignals(Astra::Signal::ComponentAdded);
        const void* seen = nullptr;
        int seenValue = 0;
        reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
            [&](const Astra::Events::ComponentAdded& ev)
            {
                seen = ev.component;
                seenValue = static_cast<const Tracked*>(ev.component)->value;
            });

        auto e = reg.CreateEntity();
        reg.AddComponent<Tracked>(e, Tracked{7});
        ASSERT_NE(seen, nullptr);
        EXPECT_EQ(seenValue, 7);                              // payload live at emit time
        EXPECT_EQ(seen, reg.GetComponent<Tracked>(e));        // emitted ptr == the new component
    }
    {
        Astra::Registry reg;   // disabled
        int calls = 0;
        reg.GetSignalManager()->On<Astra::Events::ComponentAdded>().Register(
            [&](const Astra::Events::ComponentAdded&) { ++calls; });
        auto e = reg.CreateEntity();
        reg.AddComponent<Tracked>(e, Tracked{7});
        EXPECT_NE(reg.GetComponent<Tracked>(e), nullptr);     // added regardless
        EXPECT_EQ(calls, 0);                                  // but no emission
    }
}
```

Adapt member/API spellings to the file's existing conventions if they differ (e.g. if `Events::ComponentAdded`'s payload field is not named `component`, mirror `ComponentRemovedSeesLiveValue`'s usage). If `CreateEntity<Tracked>()`/`AddComponent<Tracked>(e, Tracked{7})` don't match Registry's API shape in this file, mirror the neighboring test's creation calls — assertion content is the requirement.

- [ ] **Step 2: Build Debug + run the new tests — verify they PASS on the old code**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=SignalContract.*
```
Expected: all new tests PASS (characterization). Commit them separately:

```bash
git add tests/Registry/SignalLifetimeTest.cpp
git commit -m "test(signal): contract tests pinning add/remove emission + return-value table ahead of validate-once seam"
```

- [ ] **Step 3: Rewrite the five Registry sites**

`include/Astra/Registry/Registry.hpp` — line anchors are pre-change; match by quoted code.

**(a) `AddComponent<T>` (:281-293)** — delete the `IsValid` pre-check only:

```cpp
template<Component T>
void AddComponent(Entity entity, const T& component)
{
    // ArchetypeManager validates the handle (version + archetype); an invalid
    // entity yields nullptr, so no pre-check is needed here.
    T* newComponent = m_archetypeManager->AddComponent<T>(entity, component);

    if (newComponent)
    {
        m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), newComponent);
    }
}
```

**(b) `EmplaceComponent<T>` (:295-307)** — same transformation:

```cpp
template<Component T, typename... Args>
void EmplaceComponent(Entity entity, Args&&... args)
{
    T* component = m_archetypeManager->AddComponent<T>(entity, std::forward<Args>(args)...);

    if (component)
    {
        m_signalManager.Emit<Events::ComponentAdded>(entity, TypeID<T>::Value(), component);
    }
}
```

**(c) `RemoveComponent<T>` (:309-324)** — two-armed on the signal flag; `IsValid` dropped on BOTH arms (the validated `GetComponent` subsumes it on the cold arm; AM's version check subsumes it on the hot arm):

```cpp
template<Component T>
bool RemoveComponent(Entity entity)
{
    if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved)) ASTRA_UNLIKELY
    {
        // Cold arm: the signal needs the PRE-removal pointer. The validated
        // GetComponent doubles as the liveness + presence guard (nullptr for
        // stale handles and absent components alike).
        T* component = m_archetypeManager->GetComponent<T>(entity);
        if (!component)
            return false;

        // Emit BEFORE removal: the pointer is only valid until the entity
        // migrates. Handlers must not retain it past their invocation.
        m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);

        return m_archetypeManager->RemoveComponent<T>(entity);
    }

    // Hot arm: ArchetypeManager validates once (version + archetype + mask).
    return m_archetypeManager->RemoveComponent<T>(entity);
}
```

**(d) `AddComponentByID` (:478-483 head)** — delete ONLY the `IsValid` pre-check (lines `if (!m_entityManager.IsValid(entity)) return false;`). The signal block below it is ALREADY hoisted behind `IsSignalEnabled` (verified at :485) — leave it byte-for-byte.

**(e) `RemoveComponentByID` (:540-578)** — delete the `IsValid` pre-check (:542-543), and simplify the duplicated flag re-check at :572: `if (componentPtr && m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))` becomes `if (componentPtr)` (`componentPtr` is only ever set inside the `IsSignalEnabled` block at :547 — the second check is dead). Everything else, including emit-before-removal order, stays.

- [ ] **Step 4: Apply the Lever-1 rider — typed `GetComponent` null-checks `rec->chunk`**

`include/Astra/Archetype/ArchetypeManager.hpp`, in `GetComponent<T>`'s non-tag branch (the body Lever 1 Task 3 installed): insert the guard between the `id >= MAX_COMPONENTS` check and the desync `ASTRA_ASSERT`:

```cpp
        // Defense-in-depth (Lever 1 final-review Minor #1): a record can hold
        // archetype != nullptr with chunk == nullptr only via SetEntityLocation
        // fed a degenerate location -- unreachable today, but the ByID sites
        // all guard, so the typed path matches them rather than null-deref.
        if (!rec->chunk) ASTRA_UNLIKELY
            return nullptr;
```

(The desync assert and `return rec->chunk->GetComponent<T>(...)` stay unchanged below it.)

- [ ] **Step 5: Build Debug + FULL suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: **732 baseline + 3 new = 735 pass.** The suite includes signal, serialization, and structural tests that exercise every rewritten site; the desync assert sweeps record coherence.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/Registry.hpp include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "perf(registry): validate-once structural paths - drop redundant IsValid/GetComponent pre-checks, hoist signal payload behind IsSignalEnabled; typed GetComponent null-checks rec->chunk (Lever 1 rider)"
```

- [ ] **Step 7: Checkpoint 1 — bench A/B (PowerShell tool, NOT Bash, for the build)**

```powershell
Set-Location D:\dev\starworks\Astra\bench-compare
cmd /c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /GL /DNDEBUG /DASTRA_BUILD_DIST /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /nologo /I..\include /I..\vendor\Mosaic\include bench_astra.cpp advapi32.lib /link /LTCG'
typeperf "\Processor(_Total)\% Processor Time" -sc 6
foreach ($i in 1..6) { .\bench_astra.exe | ForEach-Object { "round$i,$_" } | Add-Content ckpt1.csv; .\bench_flecs.exe | ForEach-Object { "round$i,$_" } | Add-Content ckpt1.csv; .\bench_entt.exe | ForEach-Object { "round$i,$_" } | Add-Content ckpt1.csv }
```
Compute per-op medians (Python `statistics.median` over the 6 values per lib/op). Record in your report: remove/add/create/random_get/iterate medians vs the Global Constraints baseline. Watch specifically: **remove and add should improve or hold; random_get must stay ~60 (the rider adds a predicted-not-taken branch — regression here = flag it).** ckpt1.csv is scratch — do NOT commit it.

---

### Task 2: B1 — `Destruct` triviality gate + flag tests

**Files:**
- Modify: `include/Astra/Component/Component.hpp` (:52-83 field block, :155-159 `Destruct`)
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (~:178 descriptor factory)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (flag assertions)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: `ComponentDescriptor::is_trivially_destructible` (bool, **default false** = today's behavior; only the trait-driven factory sets true). Task 5's fused primitive relies on `Destruct` being cheap for trivial types but does not reference the field directly.

- [ ] **Step 1: Write the failing test (honest RED — the field does not exist, so this fails to compile)**

In `tests/Registry/ArchetypeManagerTest.cpp` (fixture already owns a `ComponentRegistry` — mirror its member spelling; register the two types through the fixture's registry if `GetComponentDescriptor` requires prior registration, mirroring how neighboring tests obtain descriptors):

```cpp
TEST_F(ArchetypeManagerTest, DescriptorTrivialDestructibilityFlags)
{
    using namespace Astra::Test;
    // Position: plain aggregate -> trivially destructible.
    // Tracked: user-defined destructor (s_live bookkeeping) -> NOT trivially destructible.
    componentRegistry->RegisterComponent<Position>();
    componentRegistry->RegisterComponent<Tracked>();
    const auto* posDesc = componentRegistry->GetComponentDescriptor(Astra::TypeID<Position>::Value());
    const auto* trkDesc = componentRegistry->GetComponentDescriptor(Astra::TypeID<Tracked>::Value());
    ASSERT_NE(posDesc, nullptr);
    ASSERT_NE(trkDesc, nullptr);
    EXPECT_TRUE(posDesc->is_trivially_destructible);
    EXPECT_FALSE(trkDesc->is_trivially_destructible);
    static_assert(std::is_trivially_destructible_v<Position>);
    static_assert(!std::is_trivially_destructible_v<Tracked>);
}
```

- [ ] **Step 2: Build to verify RED**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: **compile FAILURE** — `ComponentDescriptor` has no member `is_trivially_destructible`.

- [ ] **Step 3: Add the field + populate + gate**

`include/Astra/Component/Component.hpp`:

1. In the field block, directly after `bool is_trivially_default_constructible;` (:63), add:
```cpp
        bool is_trivially_destructible = false;   // default false = always-call-fn-ptr (safe for hand-built descriptors)
```
2. Replace `Destruct` (:155-159):
```cpp
        inline void Destruct(void* ptr) const
        {
            if (size == 0) return;  // empty (tag) component: nothing to destruct (mirrors DefaultConstruct)
            if (is_trivially_destructible)
            {
                return;  // trivial destructor is a no-op: skip the indirect call entirely
            }
            destruct(ptr);
        }
```

`include/Astra/Component/ComponentRegistry.hpp`, beside `desc.is_trivially_default_constructible = ...` (:178):
```cpp
            desc.is_trivially_destructible = std::is_trivially_destructible_v<T>;
```

Then verify the factory is the ONLY `ComponentDescriptor` production site: `grep -rn "is_trivially_copyable\s*=" include/Astra/` — every hit assigning descriptor fields should be in `ComponentRegistry.hpp`. (`TypeMeta.hpp:502`'s `m_meta.destruct` is TypeMeta's own member, not a `ComponentDescriptor` — confirm and note in your report.) Any other production site found: set the new field there from the same trait, or leave it defaulted false and say so.

- [ ] **Step 4: Build + full Debug suite (GREEN)**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: **736 pass** (735 + 1). The existing `Tracked::s_live` lifetime tests are the guard that non-trivial destructors still run: `is_trivially_destructible_v<Tracked> == false` keeps every `Tracked` destruct on the fn-ptr path — a wrong gate shows up as an `s_live` imbalance.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Component/Component.hpp include/Astra/Component/ComponentRegistry.hpp tests/Registry/ArchetypeManagerTest.cpp
git commit -m "perf(component): Destruct gains the missing triviality gate (mirrors MoveConstruct/DefaultConstruct) - kills no-op indirect calls in swap-remove/destroy/compaction"
```

---

### Task 3: B3 ordinal column addressing + B4 slot append + checkpoint 2 (3-config + bench)

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (new `GetColumnPointer(uint16_t, size_t)` accessor beside `GetComponentPointer` ~:447)
- Modify: `include/Astra/Archetype/Archetype.hpp` (:541-566 `MoveEntityFrom` merge-join; :1531-1556 `AllocateEntitySlot`)

**Interfaces:**
- Consumes: Task 2's gated `Destruct` (no direct reference).
- Produces: `ArchetypeChunk::GetColumnPointer(uint16_t column, size_t index) -> void*` — ordinal-direct addressing (Phase C invariant: chunk columns are packed in `ArchetypeColumnMeta`'s ascending-id order; single chunk-creation path). **Task 5's fused primitive calls this on the dst chunk.**

No new tests: both changes are behavior-preserving under the full suite (every structural test moves entities through these paths; Lever 1's desync assert + Phase C's `s_live` guards are the nets). This is the same disclosed characterization pattern as prior refactor tasks.

- [ ] **Step 1: Add the ordinal accessor**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, in `ArchetypeChunk`'s public section beside `GetComponentPointer` (~:447):

```cpp
        // Ordinal-direct column addressing for merge-join callers that already
        // hold the column index (chunk columns are packed in m_meta's
        // ascending-id order -- Phase C invariant, single creation path).
        // Skips the idToColumn resolution GetComponentPointer performs.
        ASTRA_NODISCARD ASTRA_FORCEINLINE void* GetColumnPointer(uint16_t column, size_t index)
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            ASTRA_ASSERT(index < m_count, "Entity index out of bounds");
            return static_cast<std::byte*>(m_columns[column].base) + index * m_columns[column].stride;
        }
```

- [ ] **Step 2: Rewire `MoveEntityFrom`'s merge-join to ordinals**

`include/Astra/Archetype/Archetype.hpp` :541-566 — replace only the two `GetComponentPointer` calls; every comment and the gate stay:

```cpp
            uint16_t a = 0, b = 0;
            while (a < dm.columnCount)
            {
                const ComponentID dId = dm.columns[a].id;
                // dstEntityIndex is < the dst chunk's count here: AllocateEntitySlot (invoked by
                // MoveEntityInternal before this runs) already bumped the destination slot's
                // count, so the count-asserting GetColumnPointer is safe on the destination.
                void* dstPtr = dstChunk->GetColumnPointer(a, dstEntityIndex);

                // Advance src past any ids strictly less than dId (src-only columns: dropped).
                while (b < sm.columnCount && sm.columns[b].id < dId) ++b;

                if (b < sm.columnCount && sm.columns[b].id == dId) ASTRA_LIKELY   // matched: move src -> dst
                {
                    void* srcPtr = srcChunk->GetColumnPointer(b, srcEntityIndex);
                    const ComponentDescriptor& desc = *dm.columns[a].descriptor;
                    if (desc.is_trivially_copyable) std::memcpy(dstPtr, srcPtr, dm.columns[a].stride);
                    else                            desc.MoveConstruct(dstPtr, srcPtr);
                    ++b;
                }
                else ASTRA_UNLIKELY                                              // dst-only: default-construct
                {
                    dm.columns[a].descriptor->DefaultConstruct(dstPtr);
                }
                ++a;
            }
```

(Keep the big block comment above the loop — :529-540 — untouched.)

- [ ] **Step 3: B4 — `AllocateEntitySlot` append**

`include/Astra/Archetype/Archetype.hpp` :1539-1546 — replace the resize/index-write/count dance (capacity is pre-reserved: `m_entities.reserve(m_capacity)` at chunk init, `ArchetypeChunkPool.hpp:469`, and the assert guards full chunks):

```cpp
            auto* chunk = m_chunks[chunkIndex].get();

            ASTRA_ASSERT(chunk->GetCount() < chunk->GetCapacity(), "Chunk is full");
            size_t entityIndex = chunk->GetCount();
            chunk->GetEntities().push_back(entity);   // capacity pre-reserved at chunk creation: never reallocates
            chunk->SetCount(entityIndex + 1);
```

(The `++m_entityCount;` / `IsFull` block below stays unchanged.)

- [ ] **Step 4: Build Debug + FULL suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: **736 pass** — zero behavior change.

- [ ] **Step 5: 3-config (checkpoint 2 gate)**

```powershell
foreach ($cfg in 'Release','Dist') {
  & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=$cfg -p:Platform=x64 -m
  & ".\bin\$cfg-windows-x86_64\AstraTest\AstraTest.exe"
}
```
Expected: Release **734**, Dist **734** (730 baseline + 4 new minus... compute: baseline 730 + 3 SignalContract + 1 flag test = 734). Report exact counts.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp
git commit -m "perf(archetype): ordinal-direct column addressing in transition merge-join + push_back slot append (kills idToColumn re-resolution and double slot write)"
```

- [ ] **Step 7: Checkpoint 2 — bench (same protocol as Task 1 Step 7, output ckpt2.csv)**

Rebuild bench_astra (same command), quiet check, 6 interleaved rounds into `ckpt2.csv`, medians. Record: remove/add/create vs baseline AND vs checkpoint 1 (Phase A attribution vs safe-tranche attribution). Do not commit the csv.

---

### Task 4: B2 GATE — controller decision point (NO subagent dispatch)

After Task 3's checkpoint: compare the checkpoint-2 paired medians (Astra remove vs flecs remove, same session).

- If **Astra remove > flecs remove** (still behind): proceed directly to Task 5.
- If **Astra remove ≤ flecs remove** (at or ahead): STOP. Present the user the checkpoint numbers and ask whether B2 (Task 5) still lands or the lever finishes at Task 6. The spec (§4 B2 gate) reserves this decision for the user. Record the decision in the SDD ledger.

---

### Task 5: B2 — fused move-out + backfill (OPUS) + tests + checkpoint 3

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (new `MoveOutAndBackfill` beside `RemoveEntity` :319)
- Modify: `include/Astra/Archetype/Archetype.hpp` (factor `PostRemoveChunkBookkeeping` out of `RemoveEntity` :412-441; new `MoveOutAndRemove`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`MoveEntity` :1188-1194 rewired off `MoveEntityInternal`)
- Test: `tests/Registry/RegistryTest.cpp` (value-integrity + tail + lifetime characterization)

**Interfaces:**
- Consumes: Task 3's `ArchetypeChunk::GetColumnPointer(uint16_t, size_t)`; Task 2's gated `Destruct`.
- Produces: `ArchetypeChunk::MoveOutAndBackfill(size_t srcIndex, ArchetypeChunk& dstChunk, size_t dstIndex, const ArchetypeColumnMeta& dstMeta) -> std::optional<Entity>`; `Archetype::MoveOutAndRemove(EntityLocation src, Archetype& dst, EntityLocation dstLoc) -> std::optional<Entity>`; private `Archetype::PostRemoveChunkBookkeeping(size_t chunkIndex)`. `MoveEntityFrom`, `RemoveEntity`, and `MoveEntityInternal` all REMAIN (other callers: add-transitions, ByID moves, batch paths).

- [ ] **Step 1: Write the characterization tests (must PASS on the current code — behavior-preserving fusion; disclosed)**

In `tests/Registry/RegistryTest.cpp` (mirror the file's fixture conventions from Lever 1's `GetComponentAfterSwapRemove`; all types existing):

```cpp
TEST_F(RegistryTest, RemoveComponentSwapBackfillPreservesSurvivorValues)
{
    using namespace Astra::Test;
    // Pos+Vel+Health archetype; removing Health transitions each entity to
    // Pos+Vel and swap-backfills the source chunk -- the bench's exact shape.
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 50; ++i)
    {
        auto e = registry.CreateEntity();
        registry.AddComponent<Position>(e, float(i), 0.f, 0.f);
        registry.AddComponent<Velocity>(e, float(i) * 2.f, 0.f, 0.f);
        registry.AddComponent<Health>(e, Health{float(i) * 3.f});
        es.push_back(e);
    }
    ASSERT_TRUE(registry.RemoveComponent<Health>(es[10]));   // middle: forces a backfill
    for (int i = 0; i < 50; ++i)
    {
        auto* p = registry.GetComponent<Position>(es[i]);
        auto* v = registry.GetComponent<Velocity>(es[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        ASSERT_NE(v, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));            // every entity still reads ITS OWN values
        EXPECT_FLOAT_EQ(v->x, float(i) * 2.f);
        auto* h = registry.GetComponent<Health>(es[i]);
        if (i == 10) EXPECT_EQ(h, nullptr);
        else { ASSERT_NE(h, nullptr); EXPECT_FLOAT_EQ(h->value, float(i) * 3.f); }
    }
}

TEST_F(RegistryTest, RemoveComponentFromTailReportsNoMovedEntity)
{
    using namespace Astra::Test;
    // Tail slot: the fused path must take the no-backfill branch and every
    // record must stay coherent (the Debug desync assert verifies on reads).
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 5; ++i)
    {
        auto e = registry.CreateEntity();
        registry.AddComponent<Position>(e, float(i), 0.f, 0.f);
        registry.AddComponent<Health>(e, Health{1.f});
        es.push_back(e);
    }
    ASSERT_TRUE(registry.RemoveComponent<Health>(es[4]));    // last slot in its chunk
    for (int i = 0; i < 5; ++i)
    {
        auto* p = registry.GetComponent<Position>(es[i]);
        ASSERT_NE(p, nullptr);
        EXPECT_FLOAT_EQ(p->x, float(i));
    }
}

TEST_F(RegistryTest, RemoveComponentLifetimeBalanceWithNonTrivialSurvivorAndDropped)
{
    using namespace Astra::Test;
    // (1) Tracked SURVIVES the transition: non-trivial move-out + backfill.
    // (2) Tracked is the REMOVED component: dropped-column destruct.
    // s_live imbalance = a skipped/duplicated ctor/dtor anywhere in the pass.
    const int base = Tracked::s_live;
    {
        std::vector<Astra::Entity> es;
        for (int i = 0; i < 20; ++i)
        {
            auto e = registry.CreateEntity();
            registry.AddComponent<Position>(e, float(i), 0.f, 0.f);
            registry.EmplaceComponent<Tracked>(e, i);
            es.push_back(e);
        }
        EXPECT_EQ(Tracked::s_live, base + 20);
        ASSERT_TRUE(registry.RemoveComponent<Position>(es[3]));   // Tracked survives, moves out + backfills
        EXPECT_EQ(Tracked::s_live, base + 20);                     // moved, not leaked/double-freed
        EXPECT_EQ(registry.GetComponent<Tracked>(es[3])->value, 3);
        ASSERT_TRUE(registry.RemoveComponent<Tracked>(es[7]));    // Tracked is the dropped column
        EXPECT_EQ(Tracked::s_live, base + 19);                     // exactly one destructed
        for (int i = 0; i < 20; ++i)
        {
            if (i == 7) { EXPECT_EQ(registry.GetComponent<Tracked>(es[i]), nullptr); continue; }
            ASSERT_NE(registry.GetComponent<Tracked>(es[i]), nullptr) << "i=" << i;
            EXPECT_EQ(registry.GetComponent<Tracked>(es[i])->value, i);
        }
        for (auto e : es) registry.DestroyEntity(e);
    }
    EXPECT_EQ(Tracked::s_live, base);   // full teardown balance
}
```

(`Tracked` here is `Astra::Test::Tracked` — move-only, `EmplaceComponent` constructs in place; if the fixture's add idiom differs, mirror neighbors. If `Health`'s constructor shape differs from `Health{float}`, mirror `TestComponents.hpp`.)

Run the filter — expected PASS on old code — then commit:

```powershell
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=*RemoveComponentSwap*:*RemoveComponentFromTail*:*RemoveComponentLifetime*
```
```bash
git add tests/Registry/RegistryTest.cpp
git commit -m "test(registry): characterization for remove-transition swap-backfill (values/tail/lifetime) ahead of fused move-out"
```

- [ ] **Step 2: Add the chunk primitive**

`include/Astra/Archetype/ArchetypeChunkPool.hpp`, in `ArchetypeChunk` beside `RemoveEntity` (:319):

```cpp
        // Fused remove-transition primitive: moves this chunk's entity at srcIndex
        // out into dstChunk[dstIndex] (merge-joined against dstMeta) and swap-backfills
        // the vacated slot from the last entity, in ONE pass over this chunk's columns.
        // REMOVE TRANSITIONS ONLY: every dst column must exist here (dst mask is a
        // strict subset of src mask) -- asserted below. Lifetime rules mirror
        // RemoveEntity + MoveEntityFrom exactly; the per-column is_trivially_copyable
        // gate is the CORRECTNESS boundary (memcpy of a move-only component would skip
        // its move ctor and corrupt it -- must NOT be widened to a blanket memcpy).
        // Returns the entity swapped into srcIndex, or nullopt when srcIndex was the tail.
        std::optional<Entity> MoveOutAndBackfill(size_t srcIndex, ArchetypeChunk& dstChunk,
                                                 size_t dstIndex, const ArchetypeColumnMeta& dstMeta)
        {
            ASTRA_ASSERT(srcIndex < m_count, "Entity index out of bounds");
            ASTRA_ASSERT(dstIndex < dstChunk.m_count, "Destination index out of bounds");

            const size_t lastIndex = m_count - 1;
            const bool isTail = (srcIndex == lastIndex);

            uint16_t d = 0;   // dst meta cursor (both column sets ascending by id)
            for (uint16_t c = 0; c < m_meta->columnCount; ++c)
            {
                std::byte* base = static_cast<std::byte*>(m_columns[c].base);
                const uint32_t stride = m_columns[c].stride;
                const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;
                void* holePtr = base + srcIndex * stride;

                if (d < dstMeta.columnCount && dstMeta.columns[d].id == m_meta->columns[c].id)
                {
                    // Surviving column: move the departing entity's element out to dst.
                    void* dstPtr = dstChunk.GetColumnPointer(d, dstIndex);
                    if (desc.is_trivially_copyable) std::memcpy(dstPtr, holePtr, stride);
                    else                            desc.MoveConstruct(dstPtr, holePtr);
                    ++d;
                }
                // Whether moved-out residue (surviving column) or the removed
                // component's live value (dropped column), the hole slot is dead now.
                desc.Destruct(holePtr);

                if (!isTail)
                {
                    // Backfill hole <- last (same sequence RemoveEntity uses).
                    void* lastPtr = base + lastIndex * stride;
                    if (desc.is_trivially_copyable)
                    {
                        std::memcpy(holePtr, lastPtr, stride);
                    }
                    else
                    {
                        desc.MoveConstruct(holePtr, lastPtr);
                        desc.Destruct(lastPtr);
                    }
                }
            }
            ASTRA_ASSERT(d == dstMeta.columnCount,
                         "MoveOutAndBackfill requires dst columns to be a subset of src (remove transition)");

            std::optional<Entity> movedEntity;
            if (!isTail) ASTRA_LIKELY
            {
                m_entities[srcIndex] = m_entities[lastIndex];
                movedEntity = m_entities[srcIndex];
            }
            m_entities.pop_back();
            --m_count;

            return movedEntity;
        }
```

NOTE for the implementer: the trivially-copyable backfill deliberately skips both `Destruct` calls the non-trivial path performs — byte-identical to what `RemoveEntity` + the Task 2 gate already produce for trivial types. Compare against `RemoveEntity` (:319-364) line by line while implementing; the lifetime sequences must match exactly.

- [ ] **Step 3: Factor the archetype bookkeeping + add the orchestrator**

`include/Astra/Archetype/Archetype.hpp`:

1. Extract `RemoveEntity`'s post-chunk logic (:422-438) into a private helper, and have `RemoveEntity` call it:
```cpp
        // Shared post-removal bookkeeping for anything that vacated one slot in
        // m_chunks[chunkIndex]: count, first-non-full tracking, trailing-empty pop.
        void PostRemoveChunkBookkeeping(size_t chunkIndex)
        {
            --m_entityCount;

            if (chunkIndex < m_firstNonFullChunkIndex && !m_chunks[chunkIndex]->IsFull()) ASTRA_UNLIKELY
            {
                m_firstNonFullChunkIndex = chunkIndex;
            }

            if (chunkIndex == m_chunks.size() - 1 && chunkIndex > 0 && m_chunks[chunkIndex]->IsEmpty()) ASTRA_UNLIKELY
            {
                PopBackChunk();

                if (m_firstNonFullChunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                {
                    m_firstNonFullChunkIndex = m_chunks.size() > 0 ? m_chunks.size() - 1 : 0;
                }
            }
        }
```
`RemoveEntity` becomes:
```cpp
        std::optional<Entity> RemoveEntity(EntityLocation location)
        {
            size_t chunkIndex = location.GetChunkIndex();

            ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");

            // Remove from chunk - chunk handles the swap-and-pop
            auto movedEntity = m_chunks[chunkIndex]->RemoveEntity(location.GetEntityIndex());

            PostRemoveChunkBookkeeping(chunkIndex);

            return movedEntity;
        }
```
2. Add (public, near `MoveEntityFrom`):
```cpp
        // Remove-transition fast path: single-pass move-out of the surviving
        // columns into dstArchetype's freshly allocated slot + swap-backfill of
        // this archetype's vacated slot. dstArchetype's columns must be a subset
        // of this archetype's (remove transitions only).
        std::optional<Entity> MoveOutAndRemove(EntityLocation srcLocation, Archetype& dstArchetype,
                                               EntityLocation dstLocation)
        {
            size_t srcChunkIndex = srcLocation.GetChunkIndex();
            ASTRA_ASSERT(srcChunkIndex < m_chunks.size(), "Chunk index out of bounds");
            ASTRA_ASSERT(dstLocation.GetChunkIndex() < dstArchetype.m_chunks.size(), "Destination chunk index out of bounds");

            auto movedEntity = m_chunks[srcChunkIndex]->MoveOutAndBackfill(
                srcLocation.GetEntityIndex(),
                *dstArchetype.m_chunks[dstLocation.GetChunkIndex()],
                dstLocation.GetEntityIndex(),
                dstArchetype.m_columnMeta);

            PostRemoveChunkBookkeeping(srcChunkIndex);

            return movedEntity;
        }
```

- [ ] **Step 4: Rewire `MoveEntity` (ArchetypeManager) onto the fused path**

`include/Astra/Archetype/ArchetypeManager.hpp` — replace `MoveEntity` (:1188-1194). It no longer routes through `MoveEntityInternal` (which stays for `MoveEntityWithComponent` / ByID add-moves):

```cpp
        EntityLocation MoveEntity(Entity entity, EntityRecord& oldLoc, Archetype* newArchetype)
        {
            EntityLocation newEntityLocation = newArchetype->AllocateEntitySlot(entity);
            if (!newEntityLocation.IsValid()) ASTRA_UNLIKELY
            {
                return newEntityLocation;
            }

            // Capture the OLD archetype before oldLoc is reassigned: the swap-moved
            // entity stays in it, so its chunk must resolve against srcArchetype.
            Archetype* srcArchetype = oldLoc.archetype;

            std::optional<Entity> movedEntity;
            if (srcArchetype->IsInitialized() && newArchetype->IsInitialized()) ASTRA_LIKELY
            {
                // Fused: surviving-column move-out + swap-backfill, one pass.
                movedEntity = srcArchetype->MoveOutAndRemove(oldLoc.location, *newArchetype, newEntityLocation);
            }
            else
            {
                // Degenerate archetype (no storage): plain removal, nothing to move.
                movedEntity = srcArchetype->RemoveEntity(oldLoc.location);
            }

            if (movedEntity) ASTRA_LIKELY
            {
                EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                SetRecordLocation(movedRec, srcArchetype, oldLoc.location);
            }

            // oldLoc aliases the shared record for `entity`; writing it here IS the
            // record update (archetype/chunk/location only -- never version). Runs
            // AFTER the movedRec fixup, which resolves against the OLD archetype.
            SetRecordLocation(&oldLoc, newArchetype, newEntityLocation);

            return newEntityLocation;
        }
```

ORDER MATTERS (Lever 1 invariant): the movedRec fixup resolves against `srcArchetype` BEFORE `oldLoc` is reassigned — preserved above. `rec->version` is never written.

- [ ] **Step 5: Build Debug + FULL suite (the desync + s_live sweep)**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
.\bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
```
Expected: **739 pass** (736 + 3 from Step 1). Any "EntityRecord chunk/location desync" abort or `s_live` failure = a fusion bug — STOP and report, do not paper over.

- [ ] **Step 6: 3-config**

Build + run Release and Dist as in Task 3 Step 5. Expected: Release **737** / Dist **737**. Report exact counts.

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "perf(archetype): fused move-out+backfill for remove transitions - single pass over src columns replaces the MoveEntityFrom+RemoveEntity double walk"
```

- [ ] **Step 8: Checkpoint 3 — bench (same protocol, ckpt3.csv)**

Record remove/add/create/random_get medians vs checkpoint 2 (B2's isolated contribution) and vs baseline.

---

### Task 6: Final validation — RESULTS.md + ledger numbers

**Files:**
- Modify: `bench-compare/RESULTS.md` (new Lever 2 section)

**Interfaces:**
- Consumes: ckpt1/ckpt2/ckpt3 medians from the task reports (or ckpt1/ckpt2 if B2 was skipped at the Task 4 gate).
- Produces: the merge-gate evidence.

- [ ] **Step 1: Confirm the authoritative 3-config**

The authoritative run is the LAST code commit's 3-config (Task 5 Step 6, or Task 3 Step 5 if B2 was skipped). If any code changed since that run, re-run all three configs now and report counts. This task's own commit is docs-only.

- [ ] **Step 2: Append the Lever 2 section to `bench-compare/RESULTS.md`**

Dated section before the `## Reproduce` footer: branch, commits, the checkpoint table (baseline → ckpt1 → ckpt2 → ckpt3 medians for remove/add/create/random_get/iterate1/2/3, with flecs/EnTT same-session values), per-phase attribution (A vs safe-tranche vs B2), honest verdict vs the ≤~34ns target, and the Task 4 gate decision if B2 was skipped. Note ckpt csv files are untracked scratch.

- [ ] **Step 3: Commit**

```bash
git add bench-compare/RESULTS.md
git commit -m "perf(bench): lever 2 remove-path A/B results (validate-once seam + move-path micro-work)"
```

---

### Finishing (SDD flow — after final review)

Opus whole-branch review → fix wave if needed → authoritative 3-config on the final code commit → **confirm with user** → FF-merge to dev locally, delete branch, do NOT push → update `[[astra-perf-optimization]]` memory (include the checkpoint attributions).
