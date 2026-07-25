# Lever 3 — create_batch + destroy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the last two same-model gaps vs flecs — destroy (26.14 → target ~14.6 ns) via validate-once + an empty-graph relations early-out, and create_batch (34.06 → target ~17.2 ns/entity) via a chunk-run bulk path.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-24-lever3-create-batch-destroy-design.md` (user-approved; §3 behavior table and §4 construction-semantics rule govern). Destroy: one validated record fetch feeds signal gate, a record-taking `AM::RemoveEntity` overload, a `RelationshipGraph::Empty()` early-out, and a record-completing `EntityManager::Destroy` overload (EM still owns the version write — W1). create_batch: `Archetype::AddEntitiesWith` goes entity-major → chunk-run (bulk slot claim, hoisted typed column bases, generator-per-entity preserved), and `AM::AddEntitiesWith`'s record loop hoists the chunk pointer across run boundaries through the 4-arg funnel overload.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln`), GoogleTest, definitive 3-way bench harness in `bench-compare/`.

## Global Constraints

- Branch: `perf/lever3-create-destroy` off dev HEAD (record the SHA in the ledger). Local only — NEVER push. Finish = opus whole-branch review → fix wave if needed → authoritative 3-config → **confirm with user** → FF-merge local, delete branch.
- Build (PowerShell, whole solution): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<CFG> -p:Platform=x64 -m`. Tests: `.\bin\<CFG>-windows-x86_64\AstraTest\AstraTest.exe`. Baseline (dev @ 5aa9df9/73faa67): **Debug 739 / Release 737 / Dist 737** — re-verify before Task 1. Lone `CompressionTest.PerformanceBenchmark` failure = known flake → rerun isolated. IDE clang diagnostics = false positives. Stale PDB → `taskkill /F /IM mspdbsrv.exe`.
- **TypeID ceiling: register NO new component types in tests** — reuse `Astra::Test::*` and `SignalLifetimeTest.cpp`'s file-local `Tracked`.
- **W1 invariant: `EntityManager` ALONE writes `EntityRecord::version`** — the new EM overload keeps the version write inside EM. **Funnel discipline: record storage fields only via `SetRecord`/`SetRecordLocation`/`ClearRecordLocation`** (the AM record loop keeps using the funnel — the optimization is fewer chunk-pointer derivations, not a bypass).
- **Spec §3 behavior table governs destroy semantics** (invalid/stale = no-op no-signal; valid = signal-iff-enabled, storage removed, relations cleaned iff graph non-empty, version bumped). **Spec §4 construction rule governs create_batch:** generator called exactly once per entity in index order; tuple elements MOVE-CONSTRUCTED into raw slots (`ConstructComponentAt` semantics); the `DefaultConstruct`-for-uncovered-columns guard is PRESERVED (defensive — on this path all columns are covered, keep the guard anyway).
- Bench checkpoints: definitive harness exes (rebuild bench_astra ONLY — flecs/entt exes unchanged; full-opt recipe **incl. `/I..\tests`**; PowerShell tool for build_one.bat), typeperf quiet gate, 6 interleaved rounds astra→flecs→entt, `roundN,` prefix, paired same-session medians [min,max], non-overlapping bands for claims. Reference values (Definitive Scoreboard): destroy astra 26.14 / flecs 14.58 / entt 47.34; create_batch astra 34.06 / flecs 17.17 / entt 44.85; create ~49 / ~88 / ~48 (must stay flat).
- Model recipe (SDD): sonnet Task 1 and Task 3; **OPUS Task 2** (invasive archetype rewrite) and the final whole-branch review.

---

### Task 1: Destroy sub-lever + checkpoint 1

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp:222-232` (`DestroyEntity`)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:189-208` (`RemoveEntity` — add record-taking overload)
- Modify: `include/Astra/Entity/EntityManager.hpp:106+` (`Destroy` — add record-completing overload)
- Modify: `include/Astra/Registry/RelationshipGraph.hpp` (add `Empty()`)
- Test: `tests/Registry/SignalLifetimeTest.cpp` (destroy behavior table)
- Test: `tests/Registry/RelationsTest.cpp` — or wherever the existing parent/child tests live (grep `SetParent` under tests/); add the destroy-with-relations guard beside them

**Interfaces:**
- Consumes: `AM::GetEntityRecord(Entity)` (validated fetch, `ArchetypeManager.hpp:257+`); `FlatMap::Empty()` (exists — `FlatMap.hpp:426`).
- Produces: `AM::RemoveEntity(Entity, EntityRecord*)`; `EntityManager::Destroy(Entity, EntityRecord*)`; `RelationshipGraph::Empty()`. Task 2 depends on nothing from this task.

- [ ] **Step 1: Characterization tests FIRST (must PASS on current code — behavior-preserving restructure; disclosed house style).**

In `tests/Registry/SignalLifetimeTest.cpp` (reuse its file-local `Tracked` + the SignalContract idiom):

```cpp
TEST(SignalContract, DestroyBehaviorTable)
{
    // Signal disabled (default): valid entity destroyed silently; stale/invalid = no-op.
    {
        Astra::Registry reg;
        int calls = 0;
        reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Register(
            [&](const Astra::Events::EntityDestroyed&) { ++calls; });
        auto e = reg.CreateEntity<Tracked>();
        reg.DestroyEntity(e);
        EXPECT_FALSE(reg.IsValid(e));                    // destroyed
        EXPECT_EQ(reg.GetComponent<Tracked>(e), nullptr);
        reg.DestroyEntity(e);                            // stale handle: no-op, no crash
        EXPECT_EQ(calls, 0);                             // disabled: nothing fired
    }
    // Signal enabled: fires exactly once per valid destroy, not for stale.
    {
        Astra::Registry reg;
        reg.EnableSignals(Astra::Signal::EntityDestroyed);
        int calls = 0;
        reg.GetSignalManager()->On<Astra::Events::EntityDestroyed>().Register(
            [&](const Astra::Events::EntityDestroyed&) { ++calls; });
        auto e = reg.CreateEntity<Tracked>();
        reg.DestroyEntity(e);
        EXPECT_EQ(calls, 1);
        reg.DestroyEntity(e);                            // stale: no second emission
        EXPECT_EQ(calls, 1);
    }
}
```

In the relations test file (mirror its fixture/creation conventions; assertion content is the requirement):

```cpp
TEST(RelationsDestroyGuard, DestroyParentStillOrphansChildrenWithFastPathPresent)
{
    Astra::Registry reg;
    auto parent = reg.CreateEntity();
    auto c1 = reg.CreateEntity();
    auto c2 = reg.CreateEntity();
    reg.SetParent(c1, parent);
    reg.SetParent(c2, parent);
    reg.DestroyEntity(parent);                 // graph NON-empty: cleanup must run
    EXPECT_FALSE(reg.IsValid(parent));
    // children live and orphaned (mirror the file's parent-query idiom, e.g. GetParent):
    EXPECT_TRUE(reg.IsValid(c1));
    EXPECT_TRUE(reg.IsValid(c2));
    EXPECT_EQ(reg.GetParent(c1), Astra::Entity::Invalid());   // adapt spelling to the file's API
    EXPECT_EQ(reg.GetParent(c2), Astra::Entity::Invalid());
}
```

Run the two filters — expected PASS — commit:
```bash
git add tests/Registry/SignalLifetimeTest.cpp tests/Registry/<relations test file>
git commit -m "test(registry): destroy behavior table + destroy-with-relations guard ahead of validate-once destroy"
```

- [ ] **Step 2: `RelationshipGraph::Empty()`**

```cpp
        // True when the graph has never held (or no longer holds) any relationship
        // of any kind -- the destroy fast path skips OnEntityDestroyed entirely.
        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return m_parents.Empty() && m_children.Empty() && m_links.Empty();
        }
```
(Place beside the other public queries; verify the three member names in-file and adapt spelling only if they differ.)

- [ ] **Step 3: `EntityManager::Destroy` record-completing overload**

Beside the existing `Destroy(Entity)` (`EntityManager.hpp:106`), add — keeping the existing overload UNCHANGED:

```cpp
        // Record-completing variant: the caller (Registry::DestroyEntity) already
        // holds the entity's VALIDATED record, so the IsValid/GetVersion re-walks
        // are skipped. The version write stays HERE -- EntityManager alone owns
        // EntityRecord::version (W1).
        bool Destroy(Entity entity, EntityRecord* rec) noexcept
        {
            ASTRA_ASSERT(rec == m_table.GetRecord(entity.GetID()), "record/entity mismatch");
            const VersionType currentVersion = entity.GetVersion();
            if (rec->version != currentVersion) ASTRA_UNLIKELY
            {
                return false;
            }
            const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);
            // ... IDENTICAL tail to Destroy(Entity) from the "Mark as destroyed" comment on:
            // copy the existing overload's remaining body verbatim (table mark + freelist push).
```
Copy the tail verbatim from the existing overload (read it in full first); if the tail uses `id`-keyed table calls, keep them (same slot).

- [ ] **Step 4: `AM::RemoveEntity` record-taking overload**

```cpp
        // Record-taking variant: caller already validated the record. Same body as
        // RemoveEntity(Entity) minus the fetch/guard.
        void RemoveEntity(Entity entity, EntityRecord* rec)
        {
            ASTRA_ASSERT(rec && rec->version == entity.GetVersion() && rec->archetype,
                         "RemoveEntity(rec): caller must pass a validated record");
            Archetype* archetype = rec->archetype;
            EntityLocation oldLocation = rec->location;

            if (auto movedEntity = archetype->RemoveEntity(oldLocation)) ASTRA_LIKELY
            {
                EntityRecord* movedRec = m_records->GetRecord(movedEntity->GetID());
                ASTRA_ASSERT(movedRec, "swap-moved entity must be live and located");
                SetRecordLocation(movedRec, archetype, oldLocation);
            }

            ClearRecordLocation(rec);   // location only -- EntityManager::Destroy owns version
        }
```
Rewire the existing `RemoveEntity(Entity)` (:189-208) to delegate: keep its fetch+guard, then call the new overload.

- [ ] **Step 5: `Registry::DestroyEntity` rewrite (:222-232)**

```cpp
        void DestroyEntity(Entity entity)
        {
            // Single validated fetch: version + archetype checks subsume the old
            // IsValid pre-check (same unified record slot -- W1).
            auto* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec) ASTRA_UNLIKELY
                return;

            if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed)) ASTRA_UNLIKELY
            {
                m_signalManager.Emit<Events::EntityDestroyed>(entity);
            }

            m_archetypeManager->RemoveEntity(entity, rec);

            // Empty-graph fast path: three size loads replace three hash probes.
            // A relation-holding entity makes the graph non-empty by construction,
            // so cleanup can never be skipped for an entity that needs it.
            if (!m_relationshipGraph->Empty()) ASTRA_UNLIKELY
            {
                m_relationshipGraph->OnEntityDestroyed(entity);
            }

            m_entityManager.Destroy(entity, rec);
        }
```
Verify-item from spec §3 (confirm before coding, note in report): `GetEntityRecord`'s guard (version + archetype non-null) — every creation path assigns an archetype before the entity is observable, so validated ⇒ located.

- [ ] **Step 6: Build Debug + FULL suite** — expected **741** (739 + 2). Then Release/Dist: **739 each**.

- [ ] **Step 7: Commit**
```bash
git add include/Astra/Registry/Registry.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Entity/EntityManager.hpp include/Astra/Registry/RelationshipGraph.hpp
git commit -m "perf(registry): validate-once DestroyEntity - single record fetch + record-taking RemoveEntity/Destroy overloads + empty-graph relations early-out"
```

- [ ] **Step 8: Checkpoint 1 — bench.** Rebuild bench_astra (full-opt recipe + `/I..\tests`), quiet gate, 6 interleaved rounds → `lever3_ckpt1.csv` (untracked). Report destroy/create/create_batch/add/remove/random_get medians [min,max] vs the reference values. Watch: destroy toward ~14.6; create/create_batch flat (untouched so far).

---

### Task 2: create_batch sub-lever (OPUS) + checkpoint 2

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp:331-410` (`AddEntitiesWith` chunk-run rewrite; extend to `AddEntities` ONLY if the shape shares naturally — do not force)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:172-187` (`AddEntitiesWith` record-loop chunk hoist)
- Test: `tests/Registry/RegistryTest.cpp` (chunk-boundary value integrity + batch record invariant)

**Interfaces:**
- Consumes: existing chunk API (`GetComponentArray<T>()`, `GetEntities()`, `SetCount`, `ConstructComponentAt` — read them first); the 4-arg funnel `SetRecordLocation(rec, arch, chunk, loc)`.
- Produces: no signature changes — `AddEntitiesWith` keeps `(span, generator) -> std::vector<EntityLocation>`.

- [ ] **Step 1: Characterization tests FIRST (PASS on current code).** In `tests/Registry/RegistryTest.cpp` (mirror fixture conventions; `CreateEntitiesWith` is the public entry — `Registry.hpp:184`):

```cpp
TEST_F(RegistryTest, BatchCreateValuesSurviveChunkBoundaries)
{
    using namespace Astra::Test;
    // Enough entities to span multiple chunks (grow-as-populate ramps from 4KB).
    constexpr size_t kCount = 3000;
    std::vector<Astra::Entity> ents(kCount);
    size_t created = registry.CreateEntitiesWith<Position, Velocity>(
        kCount, std::span{ents},
        [](size_t i) { return std::tuple{Position{float(i), 0.f, 0.f},
                                         Velocity{float(i) * 2.f, 0.f, 0.f}}; });
    ASSERT_EQ(created, kCount);
    for (size_t i = 0; i < kCount; ++i)
    {
        auto* p = registry.GetComponent<Position>(ents[i]);
        auto* v = registry.GetComponent<Velocity>(ents[i]);
        ASSERT_NE(p, nullptr) << "i=" << i;
        ASSERT_NE(v, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i));            // every entity owns ITS generator values
        EXPECT_FLOAT_EQ(v->x, float(i) * 2.f);
    }
}

TEST_F(RegistryTest, BatchCreateLifetimeBalanceMoveOnly)
{
    using namespace Astra::Test;
    const int base = Tracked::s_live;
    {
        constexpr size_t kCount = 500;
        std::vector<Astra::Entity> ents(kCount);
        size_t created = registry.CreateEntitiesWith<Tracked>(
            kCount, std::span{ents},
            [](size_t i) { return std::tuple{Tracked{int(i)}}; });
        ASSERT_EQ(created, kCount);
        EXPECT_EQ(Tracked::s_live, base + int(kCount));      // constructed exactly once each
        EXPECT_EQ(registry.GetComponent<Tracked>(ents[123])->value, 123);
        for (auto e : ents) registry.DestroyEntity(e);
    }
    EXPECT_EQ(Tracked::s_live, base);                        // full teardown balance
}
```
(Adapt the generator/tuple spelling to `CreateEntitiesWith`'s actual contract — read it + an existing caller first. If a record-invariant helper (`ExpectChunkInvariant`) is reachable from this file, add a third assertion loop with it; if it's file-local to ArchetypeManagerTest, put that loop THERE instead, reusing its fixture — say which in the report.)

Run filters — PASS — commit:
```bash
git add tests/Registry/RegistryTest.cpp [tests/Registry/ArchetypeManagerTest.cpp]
git commit -m "test(registry): batch-create chunk-boundary values + move-only lifetime balance ahead of chunk-run rewrite"
```

- [ ] **Step 2: Rewrite `Archetype::AddEntitiesWith` to chunk-run form.** Requirements (spec §4 — the OPUS implementer designs the exact loop within them):
  - Capacity pre-grow block (:340-360) unchanged.
  - Outer loop claims runs: `runLen = min(remaining, chunk capacity - chunk count)` on the current non-full chunk; per run ONE `GetEntities().insert(end, srcFirst, srcFirst+runLen)` + ONE `SetCount(count + runLen)`; `m_entityCount += runLen`; `m_firstNonFullChunkIndex` updated per chunk transition (preserve the existing `IsFull ⇒ chunkIndex+1` convention).
  - Per run, hoist typed bases: `auto bases = std::tuple{chunk->GetComponentArray<Components>()...};` ONCE.
  - Per entity in run: `auto componentTuple = generator(globalIndex);` (once, in order — CONTRACT), then move-construct each tuple element into `std::get<c>(bases) + slot` via placement-new (`ConstructComponentAt`-equivalent semantics, but through the hoisted typed pointer — no idToColumn, no fn-ptrs). PRESERVE the `DefaultConstruct`-uncovered-columns guard from the current fold (:380-397): compute the uncovered column set ONCE per archetype (not per entity), default-construct those slots per entity via their descriptors.
  - Locations vector filled per entity as today.
  - `ASTRA_ASSERT` on run arithmetic (slot < capacity) in Debug.
- [ ] **Step 3: Hoist the AM record loop (:182-186).** Track `lastChunkIndex = SIZE_MAX`; on `locations[i].GetChunkIndex()` change, re-derive `chunk = archetype->GetChunks()[ci].get()` once; write through the 4-arg funnel: `SetRecordLocation(rec, archetype, chunk, locations[i]);`. `GetOrCreateRecord` per entity stays.
- [ ] **Step 4: Build Debug + FULL suite** — expected **743** (741 + 2). Any desync-assert abort or `s_live` failure = rewrite bug: STOP and report, do not paper over. Then Release/Dist: **741 each**.
- [ ] **Step 5: Commit**
```bash
git add include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "perf(archetype): chunk-run bulk path for AddEntitiesWith - run-claimed slots, hoisted typed column bases, run-hoisted record writes"
```
- [ ] **Step 6: Checkpoint 2 — bench** (same protocol → `lever3_ckpt2.csv`): create_batch toward ~17.2; create/add/remove/destroy/random_get/iterate flat vs checkpoint 1.

---

### Task 3: Final validation + RESULTS.md addendum

**Files:**
- Modify: `bench-compare/RESULTS.md` (Lever 3 section — the ONLY bench-compare file ever committed)

- [ ] **Step 1:** Confirm authoritative 3-config = Task 2's run (Task 3 is docs-only; re-run only if code changed since).
- [ ] **Step 2:** Append a dated Lever 3 section before `## Reproduce`: branch, commits, checkpoint tables (baseline → ckpt1 → ckpt2 vs flecs/entt), per-sub-lever attribution, honest verdict vs both targets, scoreboard-delta summary (which Definitive-Scoreboard tuning targets are now closed), remaining targets (get_multi, relations_ancestors/children) noted as open.
- [ ] **Step 3:** Commit:
```bash
git add bench-compare/RESULTS.md
git commit -m "perf(bench): lever 3 A/B results - validate-once destroy + chunk-run batch create"
```

---

### Finishing (SDD flow)

OPUS whole-branch review (with the per-task Minors roll-up) → ONE fix subagent if findings → authoritative 3-config on the final code commit → **confirm with user** → FF-merge dev local → delete branch → don't push → update `[[astra-perf-optimization]]` memory (scoreboard-target closure status).
