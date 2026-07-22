# W1 — Unified Paged Entity Record (validate + locate in one lookup)

**Date:** 2026-07-21
**Status:** Approved design → ready for implementation planning
**Source:** Work item **W1** of `docs/reviews/2026-07-21-astra-perf-optimization-plan.md`
**Related:** `docs/reviews/2026-07-21-fresh-review/`, `bench-compare/RESULTS.md`

---

## 1. Context & goal

The same-machine head-to-head (`bench-compare/RESULTS.md`) shows Astra is behind flecs — the *same
archetype storage model* — on every structural op:

| op | Astra | flecs | ratio |
|---|---|---|---|
| create (2 comp) | 367 | 105 | 3.5× |
| add component | 279 | 60 | 4.6× |
| remove component | 220 | 35 | 6.4× |
| random_get | 147 | 63 | 2.3× |

The competitor-source dissection (flecs/EnTT/Mass/DOTS — all four) converged on one shared root cause:
Astra resolves an entity's storage location through
`std::unordered_map<Entity, EntityRecord> m_entityMap` (`ArchetypeManager.hpp:1473`) — a hash + bucket
walk on **every** create/add/remove/get, plus a **heap node allocation per create**. flecs, EnTT, Mass,
and DOTS all use a **paged, id-indexed array** instead (two array indexes, no hashing).

**W1 replaces that map with a paged, id-indexed record table** and, because Astra's entity id is a dense
recycled index, goes one step further per the approved design: it **unifies** the record with the liveness
version so a single paged slot answers both "is this handle valid?" and "where does it live?".

**Primary target:** random_get 147 → ~80 ns (removes the hash entirely; validate + locate share a cache
line). **Secondary:** create/add/remove shed the per-op hash and the per-create heap allocation.

Non-goal for W1: matching EnTT's 12.5 ns add / 44 ns create — those are *model-inherent* to sparse-set
storage (no chunk move) and unreachable for an archetype ECS. The realistic bar is flecs parity, pursued
across W1–W7 of the perf plan.

---

## 2. Current state (verified against source)

**Entity identity** (`Entity/Entity.hpp`). Default config = 32-bit total / 8-bit version ⇒ **24-bit id**,
`MAX_ENTITIES = ID_MASK ≈ 16.7M`. `Entity::GetID()` is a dense, recycled index — exactly a direct-index
key. Configurable via `ASTRA_ENTITY_BITS` / `ASTRA_ENTITY_VERSION_BITS`; the design must stay correct for
any configured width.

**Two separate lookups today.** `Registry` validates then locates on every op:

```cpp
// Registry.hpp — the pattern repeated across get/add/remove/create:
if (!m_entityManager.IsValid(entity)) return ...;      // paged version check (cheap)
return m_archetypeManager->GetComponent<T>(entity);    // m_entityMap.find(): hash + bucket walk (costly)
```

- **Liveness** is owned by `EntityManager` (`Entity/EntityManager.hpp`):
  - `m_idStack` (`EntityIDStack`) — id recycling + the "next version" of a recycled id. Source of truth on
    allocate/recycle.
  - `m_table` (`EntityTable`) — paged `id → VersionType`. `Create` → `idStack.Allocate()` then
    `m_table.SetVersion`; `Destroy` → `m_table.Destroy` + `idStack.Recycle`; `IsValid` → `m_table.IsAlive`.
- **Location** is owned by `ArchetypeManager`: `std::unordered_map<Entity, EntityRecord> m_entityMap`
  (`ArchetypeManager.hpp:1473`), where `EntityRecord = { Archetype* archetype; EntityLocation location; }`
  (`ArchetypeManager.hpp:38-42`) and `EntityLocation = { uint32 chunkIndex; uint32 entityIndex; }`
  (`Archetype.hpp:44`, 8 bytes).

**The paging pattern already exists.** `EntityTable` (`Entity/EntityTable.hpp`) is a paged segmented array:
`id >> shift` segment index (`m_segmentIndex`), on-demand segment allocation, a **segment pool** for reuse,
**huge-page** backing (2 MB), autorelease of empty segments (keyed on `aliveCount`), and an iterator that
yields `(id, version)`. It currently stores `VersionType` per slot. **`EntityTable` is consumed only by
`EntityManager`** (verified: no other includers besides `Astra.hpp` umbrella), so widening its payload is
fully contained.

**Registry is non-movable.** Copy is `= delete`d and the user-declared `~Registry() = default;` suppresses
implicit moves (`Registry.hpp:82-85`). `m_entityManager` is a **value member declared before**
`m_archetypeManager` (`Registry.hpp:1612-1614`), so its address is stable for the Registry's lifetime and is
constructed first. ⇒ A raw pointer from `ArchetypeManager` into a table owned by `EntityManager` is safe with
no shared/weak_ptr indirection on the hot path.

**~40 `m_entityMap` touch sites** in `ArchetypeManager.hpp` (insert/find/erase/iterate/clear/reserve), incl.
the swap-remove back-patch in `RemoveEntity` (~:186-198), the batch remove/move paths
(~:210-239, ~:1188-1362), the `GetEntityRecord` accessor (:244, returns `const EntityRecord*`), and the
serialize/deserialize paths (~:744-831, :927).

---

## 3. Architecture

Fold the liveness version and the location record into **one paged, id-indexed table of `EntityRecord`**,
owned by `EntityManager`, pointed into by `ArchetypeManager`.

```
                         ┌──────────────────────── Registry (non-movable) ────────────────────────┐
                         │  IsValid(e)  ─────────────────────────────────► locate/mutate           │
                         │      │                                              │                    │
                         ▼      ▼                                              ▼                    │
        EntityManager ──owns──► EntityRecordTable  ◄──raw ptr (injected)── ArchetypeManager         │
          m_idStack             (paged id → EntityRecord)                    (no m_entityMap)        │
          version API           { Archetype* arch; EntityLocation loc;       location API           │
          (Create/Destroy/        VersionType version; }                     (SetRecord/GetRecord)  │
           IsValid)             reuses EntityTable segment machinery                                 │
                         └──────────────────────────────────────────────────────────────────────────┘
```

**Ownership = Mechanic P (chosen).** `EntityManager` owns the table (it already owns liveness);
`ArchetypeManager` holds a raw `EntityRecordTable*` injected by `Registry` at construction. Rejected
alternative *Mechanic Q* (a standalone table owned by `Registry`, referenced by both) is conceptually
cleaner but requires refactoring `EntityManager` off owning its table — larger blast radius for no hot-path
benefit given P is move-safe here.

---

## 4. Component design

### 4.1 `EntityRecord` (the unified slot)

```cpp
struct EntityRecord
{
    Archetype*     archetype = nullptr;                 // 8B  — null ⇒ no location assigned yet
    EntityLocation location;                            // 8B  — {chunkIndex, entityIndex}
    VersionType    version   = EntityTable::NULL_VERSION;// 1–4B — 0 (NULL_VERSION) ⇒ dead/empty slot
};                                                      // 24B (8-aligned) at default 8-bit version
```

- `version == NULL_VERSION (0)` is the single occupancy/liveness sentinel (same value `EntityTable` uses
  today), so no separate "occupied" flag is needed and `aliveCount` bookkeeping is unchanged.
- `EntityRecord` moves from `ArchetypeManager` into a shared header (e.g. alongside the table type) so both
  `EntityManager` and `ArchetypeManager` can name it. The public `EntityRecord`/`GetEntityRecord` shape seen
  by callers is preserved (adds a `version` field; existing readers of `archetype`/`location` are unaffected).

### 4.2 `EntityRecordTable` (widened `EntityTable`)

Widen `EntityTable`'s payload from `VersionType` to `EntityRecord`, keeping **all** proven machinery
(segment index, on-demand alloc, segment pool, huge-page backing, autorelease, iterator). Expose two API
faces over the same slots:

- **Version / liveness face** (used by `EntityManager`, semantics identical to today):
  `SetVersion(id, v)`, `GetVersion(id)`, `IsAlive(id, v)`, `Destroy(id)`, `AliveCount()`, `begin()/end()`
  (iterate live entities as `(id, version)`), `Clear()`, `Reserve()`, `ShrinkToFit()`. These read/write
  `slot.version` and drive `aliveCount`/autorelease exactly as the current `EntityTable`.
- **Location face** (used by `ArchetypeManager`):
  `EntityRecord* GetRecord(id)` (nullptr if segment absent), `SetRecord(id, arch, loc)` /
  `SetLocation(id, arch, loc)` (writes `slot.archetype/.location`; does **not** touch `version`),
  `ClearLocation(id)` (on entity removal from an archetype without destroy, if needed).

**Occupancy contract.** `version` is written by the liveness face (Create/Destroy); `archetype/location` by
the location face. A slot is live iff `version != NULL_VERSION`. `Destroy(id)` clears `version` (kills the
handle) but leaving stale `archetype/location` is harmless because every read is version-gated.

**Segment sizing (must fix).** The current constants assume a 1-byte payload:
`SEGMENT_SIZE = 64 KB`, `SEGMENTS_PER_HUGE_PAGE = HUGE_PAGE_SIZE / SEGMENT_SIZE = 32`, and huge-page offset
`m_nextHugePageSegment * SEGMENT_SIZE`. With a 24-byte `EntityRecord`, a 64K-entity segment is **1.5 MB**, so
only one fits in a 2 MB huge page and the 64 KB stride is wrong. **Derive segment byte-size and
huge-page-fit from `sizeof(EntityRecord)`** (segment bytes = `entitiesPerSegment * sizeof(EntityRecord)`;
segments-per-huge-page = `HUGE_PAGE_SIZE / segmentBytes`, clamped ≥ 0 with graceful heap fallback when a
segment exceeds the huge page). Keep `entitiesPerSegment` configurable (default 64K) so callers can trade
segment granularity vs. count.

### 4.3 `EntityManager` changes

- Replace `EntityTable m_table` with the widened `EntityRecordTable` (owned by value, as now).
- `Create`/`CreateBatch`/`Destroy`/`DestroyBatch`/`IsValid`/`GetVersion`/`Size`/`Empty`/iterator/`Serialize`
  keep operating through the **version/liveness face** — behavior unchanged.
- Expose the table to `Registry` for injection into `ArchetypeManager`, e.g.
  `EntityRecordTable& GetRecordTable() noexcept;` (or `EntityRecordTable*`). `GetEntityManager()` already
  exists on `Registry` for external access.

### 4.4 `ArchetypeManager` changes

- Delete `std::unordered_map<Entity, EntityRecord> m_entityMap`; add `EntityRecordTable* m_records = nullptr`.
- Accept the pointer via the constructor (preferred — `m_entityManager` is constructed before
  `m_archetypeManager`) or a Registry-called setter immediately after construction.
- Migrate the ~40 touch sites (mechanical):
  - `m_entityMap[e] = {arch, loc}` → `m_records->SetRecord(e.GetID(), arch, loc)`.
  - `m_entityMap.find(e)` → `m_records->GetRecord(e.GetID())` guarded by
    `record && record->version == e.GetVersion() && record->archetype != nullptr`. The version check
    preserves stale-handle rejection independent of the upstream `IsValid`; the `archetype != nullptr` check
    reproduces the old map's exact "entry exists ⇔ entity is located" semantics (see the destroy-ordering
    invariant below), so a slot that is alive-but-unlocated reads as not-found.
  - `m_entityMap[e].location = newLoc` (swap-remove back-patch at ~:193, ~:1213, ~:1321, ~:1362) →
    `m_records->GetRecord(movedId)->location = newLoc`.
  - `m_entityMap.erase(e)` → clear the **location fields only** (`archetype = nullptr`, invalidate
    `location`). **ArchetypeManager must never write `version`.**
  - Serialize iteration over `m_entityMap` → iterate the table's occupied slots (reuse/extend the existing
    iterator to expose `(id, EntityRecord)`); `reserve/clear` → table equivalents.
- `GetEntityRecord(entity)` keeps returning `const EntityRecord*` from `m_records->GetRecord(id)`
  (version-checked).

### 4.5 `Registry` wiring

- After both members exist (guaranteed by declaration order), inject
  `&m_entityManager.GetRecordTable()` into `m_archetypeManager`. No orchestration change: `Registry` already
  does `IsValid` (version face) then the `ArchetypeManager` call (location face) on the same slot — now a warm
  cache hit instead of a hash lookup.
- Verify the three shared-`ComponentRegistry` constructors all perform the injection.

### 4.6 Serialization

Keep the on-disk format equivalent and the per-manager split: `EntityManager::Serialize` writes idStack +
versions (via the version face); `ArchetypeManager` writes per-entity `archetype/location` + archetype
payload (iterating the table's occupied slots instead of the map). Deserialize rebuilds the table (set
versions to restore liveness, then set locations). Add an explicit save→load round-trip test.

---

## 5. Semantics preserved

- **Stale-handle rejection.** Today the `unordered_map<Entity,…>` key includes the version, so a recycled-id
  stale handle misses. The unified table validates `slot.version == entity.GetVersion()`, giving identical
  rejection. Registry-level ops are additionally gated by `IsValid`; ArchetypeManager-internal finds keep the
  version guard so the class stays self-validating.
- **Thread-safety.** Structural changes are already effectively single-threaded (the current `m_entityMap` is
  a plain `unordered_map`; the deterministic scheduler applies structural changes at sync points). The table
  inherits the same assumption — **no new concurrency requirement**. Read-only parallel iteration is unaffected
  (Views walk chunks directly, not the record table).
- **Destroy-ordering invariant (correctness-critical).** `Registry::DestroyEntity` runs
  `ArchetypeManager::RemoveEntity` (clears `archetype/location`) **then** `EntityManager::Destroy`.
  `EntityManager::Destroy` reads `GetVersion(id)` and requires it to still equal the handle's version in order
  to compute the recycled next-version and recycle the id. Therefore `ArchetypeManager` must **not** clear or
  write `version` during `RemoveEntity` — only `EntityManager::Destroy` mutates `version`. The brief internal
  window where a slot is `version`-live but `archetype == nullptr` is single-threaded, never escapes
  `DestroyEntity`, and reads as not-found via the §4.4 find-guard.
- **Public API.** `Registry`/`ArchetypeManager` signatures unchanged; `EntityRecord` gains a `version` field
  but existing `archetype`/`location` readers are source-compatible.

---

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Segment payload grows ~24× (24 B vs 1 B) → huge-page/offset math breaks | Derive segment bytes + huge-page fit from `sizeof(EntityRecord)`; heap fallback when a segment exceeds the huge page (§4.2). Covered by a unit test that forces multi-segment allocation. |
| Autorelease frees a segment that still holds location data | `aliveCount` is driven only by `version` transitions (unchanged); locations in a fully-dead segment are unreachable (all reads version-gated), so releasing is safe. |
| A location read on a not-yet-created segment | `GetRecord` returns nullptr for absent segments; callers already null-check (they `find`-then-check today). |
| Other consumers of entity records exist (e.g. RelationshipGraph) | Enumerate consumers during planning; Views iterate chunks directly (unaffected). Any consumer of `GetEntityRecord` is source-compatible. |
| Config with very wide id (64-bit, few version bits) makes a dense table huge | Table is paged/on-demand — only touched segments allocate; behavior matches today's `EntityTable`. |

---

## 7. Out of scope (deliberately deferred)

- **Literal single lookup.** Fetching the record once in `Registry` and threading it into the
  `ArchetypeManager` call (so validate + locate is *one* index, not two on the same warm line) — a further
  micro-opt; the base design already removes the hash. Revisit if benchmarks show the second index is
  material.
- **Mechanic Q** (Registry-owned standalone table).
- **W2–W7** of the perf plan (edge arrays, memcpy move, metadata-once, etc.) — separate work items.
- Command-buffer batch wiring (Phase D) — not a benchmark mover.

---

## 8. Testing & validation

1. **Unit tests (new)** for `EntityRecordTable`: paged get/set/erase; occupancy via version; stale-handle
   rejection (recycled id, wrong version); recycle-after-destroy; multi-segment allocation (forces
   huge-page-fit path); iterator over occupied slots; serialize round-trip.
2. **Existing suites green** in **Debug / Release / Dist** (independent 3-config verify per the SDD model).
   Watch the TypeID-ceiling caveat: reuse existing component types in tests, don't register many fresh ones.
3. **Benchmark.** Re-run `bench-compare/` (`build_one.bat`), update `RESULTS.md`. Expected: random_get
   147 → ~80 ns; create/add/remove drop by the hash + per-create-alloc slice (full flecs parity awaits
   W2/W3/W5).
4. **No iteration regression** (iterate1/2/3 unchanged — the record table is off the iteration hot path).

## 9. Success criteria

- All three test configs pass; new `EntityRecordTable` tests pass.
- `m_entityMap` (and its per-op hashing + per-create heap allocation) is gone; entity→location is a paged
  array index sharing the liveness slot.
- random_get measurably improved toward the ~80 ns target with no iteration regression; `RESULTS.md` updated.
- Public API and serialization format unchanged (round-trip test passes).

---

## Appendix — key source references

- `include/Astra/Entity/Entity.hpp` — id/version bit layout, `MAX_ENTITIES`.
- `include/Astra/Entity/EntityTable.hpp` — segment machinery to widen (EntityManager-only consumer).
- `include/Astra/Entity/EntityManager.hpp` — Create/Destroy/IsValid; owns `m_idStack` + `m_table`.
- `include/Astra/Entity/EntityIDStack.hpp` — id recycling + next-version source (unchanged by W1).
- `include/Astra/Archetype/ArchetypeManager.hpp:38-42` (`EntityRecord`), `:1473` (`m_entityMap`), ~40 touch sites.
- `include/Astra/Archetype/Archetype.hpp:44` — `EntityLocation`.
- `include/Astra/Registry/Registry.hpp:48-85` (ctors, non-movable), `:1612-1614` (member order), validate-then-locate call sites.
- Perf plan: `docs/reviews/2026-07-21-astra-perf-optimization-plan.md`. Dissections: `scratchpad/dissect-*.md`.
