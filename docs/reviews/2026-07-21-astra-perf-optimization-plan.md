# Astra Performance Optimization Plan — competitor-source-derived (2026-07-21)

Synthesized from a 4-way source dissection of **flecs** (same archetype model, primary reference),
**EnTT** (sparse-set, structural-cheapness reference), **Unreal Mass** (AAA archetype+chunk), and
**Unity DOTS** (canonical 16 KB-chunk design). Full per-library dissections and mapping tables live in
`scratchpad/dissect-{flecs,entt,mass,dots}.md`.

Goal: close the same-machine benchmark gap to flecs (`bench-compare/RESULTS.md`). Pre-W1, Astra was #2 on
iteration and dead-last on all structural churn — **behind flecs on the SAME storage model**, so these are
optimization gaps, not architectural limits.

**Status (2026-07-22): W1, W6, W2 landed** (W1 on `perf/w1-unified-entity-record`; W6+W2 on
`perf/w6-w2-archetype-edges`, benchmark-confirmed). Table below is the pre-W1 baseline that motivated this
plan; see Phase A for W1's measured after-numbers and Phase B for W6+W2's. **Next: Phase C (W5⇒W4⇒W3⇒W7).**

| op (pre-W1 baseline) | Astra | flecs | ratio | EnTT |
|---|---|---|---|---|
| add-component | 279 | 60 | **4.6×** | 12.5 |
| create (2 comp) | 367 | 105 | **3.5×** | 44 |
| remove-component | 220 | 35 | 6.4× | 16 |
| random_get | 147 | 63 | 2.3× | 32 |
| iterate2comp | 1.54 | 0.89 | 1.7× | 1.18 group |

---

## 0. The decisive reframe: the benchmark measures the IMMEDIATE single-entity path

`bench_astra.cpp` does every structural op as a **per-entity call in a tight loop** —
`CreateEntityWith(...)`, `AddComponent<Health>(e)`, `RemoveComponent<Velocity>(e)`, `GetComponent<Position>(e)`.
It **never** touches a command buffer or batch API.

This cleanly separates the four sources into two buckets:

- **flecs teaches the immediate path.** flecs's 60 ns add is fast *without* deferral
  (`flecs_add_id`'s fast path is array-indexed edge + memcpy move; the dissection confirms the immediate
  add does not depend on `ecs_defer_begin`). **These are the techniques that move our benchmark.**
- **Mass / DOTS teach the deferred path.** `FMassCommandBuffer` / `EntityCommandBuffer` batch structural
  changes and coalesce archetype moves at a sync point. This is a large **real-world** win and a
  feature-parity item — but it does **not** move this microbenchmark, because the benchmark calls the
  immediate API. (Notably, Astra *already has* the batch machinery — see §5 — and just doesn't route the
  command buffer through it.)

**Consequence for prioritization:** to close the benchmark gap to flecs, implement flecs's immediate-path
techniques first. Treat command-buffer batching as a parallel, roadmap-level workstream (real workloads +
feature), not a benchmark mover.

Realistic target = **flecs parity** (same model). EnTT's 12.5 ns add / 44 ns create are
**model-inherent** (single sparse-set pool, no chunk move) and are NOT reachable by an archetype ECS —
do not chase them (§6).

---

## 1. Root causes (independently confirmed across sources)

| # | Root cause (Astra) | file:line | Confirmed by | Gaps it drives |
|---|---|---|---|---|
| R1 | `std::unordered_map<Entity,EntityRecord>` entity→location: hash + bucket-chase per op, **heap node alloc per create** | `ArchetypeManager.hpp:1473` | flecs (paged record), EnTT (paged sparse), Mass (dense array), DOTS (EntityComponentStore) — **all 4** | random_get, create, add, remove |
| R2 | Add/remove edge lookup = **two** `FlatMap` probes, outer keyed by `Archetype*` → **H2≡1 SIMD-filter degeneracy** → linear probe | `ArchetypeGraph.hpp:41-53,127-128`; consumed at `ArchetypeManager.hpp:985` | flecs (array-indexed edge embedded in table) | add, remove |
| R3 | Transition move loops **0..128** per entity and always calls per-element `MoveConstruct`/`Destruct` via fn-ptr — **no trivial memcpy path** | `Archetype.hpp:1433`; `ArchetypeChunkPool.hpp:365-396` | flecs (`EcsTableIsComplex`→`fast_move` memcpy merge-join), Mass (`Memcpy` common fragments) | add, remove, create |
| R4 | Create's `AddEntity` **scans 0..128 and `DefaultConstruct`s each present component via fn-ptr, then the value overwrites it** (double work) | `ArchetypeChunkPool.hpp:110-130` | flecs (`flecs_table_fast_append`, no ctor for POD) | create |
| R5 | Per-chunk metadata bloat: `std::array<ComponentArrayInfo,128>` embeds a full ~176 B `ComponentDescriptor` **by value** (~23 KB/16 KB chunk) + duplicate `m_componentDescriptors` vector; indexed by absolute ComponentID → forces the 0..128 scans | `ArchetypeChunkPool.hpp:478-484,557-559` | flecs (`ecs_column_t`={data,ti\*} 16 B, ti shared), Mass (offset table once per archetype), DOTS (chunk header ~24 B, offsets once per archetype) — **all 3 archetype libs** | iterate (cache), create; *enabler* for R3/R4 |
| R6 | `SplitHash` forwards raw `std::hash` (identity for pointers) so H2 is constant for pointer keys | `FlatMap.hpp:~918`; `Swiss.hpp:~45` | flecs (implicitly — it uses array indices, not pointer-keyed maps) | any pointer-keyed FlatMap |

---

## 2. Fix → gap impact matrix

Legend: ●●● dominant, ●● large, ● contributory.

| Work item | create | add | remove | random_get | iterate2 | effort | risk |
|---|---|---|---|---|---|---|---|
| **W1** Paged direct-index entity→location array (replace `m_entityMap`) | ●● | ● | ● | ●●● | — | M | Low-Med |
| **W2** Array-indexed edge cache embedded in `Archetype` (replace `ArchetypeGraph` double-map) | — | ●●● | ●●● | — | — | M | Low-Med |
| **W3** Trivial memcpy move over N present cols (`isComplex` flag) | ● | ●● | ●● | — | — | M | Med |
| **W4** Fast-append: drop construct-then-overwrite, iterate N | ●●● | — | — | — | — | S-M | Low |
| **W5** Metadata-once-per-archetype (`const ComponentDescriptor*` + packed [N]); drop 23 KB + dup vector | ● | ● | ● | ● | ●● | M-L | Med |
| **W6** `SplitHash` pointer mixing (cheap hygiene) | — | ●* | ●* | — | ●(edge maps) | S | Low |
| **W7** Compact `int16 id→column[]` + get fast path (record→component hop) | — | — | — | ●● | ● | S-M | Low |

\*W6's add/remove benefit is subsumed once W2 removes the pointer-keyed edge map; keep W6 as cheap
hygiene for any remaining pointer-keyed maps.

W1 appears in every structural op **and** dominates random_get; W2 is the single biggest add/remove lever;
W4 is the single biggest create lever. Those three are the benchmark movers for the user's named
priorities (add + create).

---

## 3. Prioritized phases

### Phase A — Entity index + hash hygiene  *(surgical, broadest ROI, lands first)*
- **W1 — Paged direct-index entity→location. ✅ DONE (2026-07-22, `perf/w1-unified-entity-record`).**
  Replaced `ArchetypeManager::m_entityMap` (`unordered_map`, was :1473) with a paged/segmented direct-index
  array of `EntityRecord{archetype, location, version}`, indexed by `entity.GetID()`, version-validated —
  unified with the existing `Entity/EntityTable.hpp` `Segment`/`segIdx = id>>shift` machinery (previously
  used only for versions, now also carries archetype+location). ~40 call sites migrated
  (`ArchetypeManager.hpp` create/add/remove/get/has + serialization); public `Registry` API unchanged.
  - Removed: per-op hash + bucket-chase; **per-create heap node allocation**.
  - **Measured (median of 3 runs, `bench-compare/`, vs pre-W1 baseline above):**
    - random_get: **147.0 → 62.2 ns (2.36× faster)** — beat the ~80 ns estimate; now only 1.2× behind flecs (was 2.3×).
    - create (2 comp): **366.8 → 129.0 ns (2.84× faster)** — now 1.4× behind flecs (was 3.5×).
    - add component: **278.6 → 150.5 ns (1.85× faster)** — now 2.8× behind flecs (was 4.6×) — largest remaining gap, W2's target.
    - remove component: **220.4 → 105.6 ns (2.09× faster)** — now 3.3× behind flecs (was 6.4×).
    - iterate 1/2/3 comp: unchanged within run-to-run noise (0.456/1.538/1.457 → 0.436/1.173/1.142 ns) —
      confirms the record table is off the iteration hot path, as expected; no regression.
  - Verified `GetComponent`/`HasComponent` route through `m_records->GetRecord(id)`
    (`ArchetypeManager.hpp:512-529`) — version check and location read share the one fetched `EntityRecord`.
  - Full numbers: `bench-compare/RESULTS.md` (updated 2026-07-22).
- **W6 — `SplitHash` pointer mixing. ✅ DONE (2026-07-22, `perf/w6-w2-archetype-edges`).**
  Replaced `SplitHash`'s raw `std::hash` forwarding with an `fmix64`-style avalanche mix before the H1/H2
  split, so pointer-keyed (and other low-entropy) `FlatMap` keys no longer degenerate the SIMD H2 filter.

### Phase B — Archetype edge arrays  *(the add/remove killer)* — ✅ DONE (2026-07-22, `perf/w6-w2-archetype-edges`)
- **W2 — Embed array-indexed edges in `Archetype`.** Add `Archetype* addEdge[MAX_COMPONENTS]` +
  `removeEdge[MAX_COMPONENTS]` (dense, id-indexed) with an optional small map for any high ids; warm add
  becomes `from->addEdge[id]` — no hashing, no outer pointer-keyed map. Retire the
  `FlatMap<Archetype*,FlatMap<ComponentID,Archetype*>>` in `ArchetypeGraph.hpp:127-128`; rewire
  `GetArchetypeWithModified` (`ArchetypeManager.hpp:985`). Edge-invalidation hooks already exist
  (`RemoveEdgesTo`/`RemoveEdgesFrom`) and map cleanly onto array clears.
  - Optional follow-on (flecs #7): cache the precomputed add/removed-id diff on the edge.
  - Expected (original estimate, against the then-current **pre-W1** baseline): add 279→~90; remove
    220→~60. Largest single structural win. (Once W1 landed, the actual starting point moved to
    150.5/105.6 — see the rebased prediction in the Measured bullet below.)
  - **Implemented as:** lazily-allocated `Archetype* m_addEdges[MAX_COMPONENTS]` / `m_removeEdges[...]`
    directly on `Archetype` (`Archetype.hpp:1061-1087`, `GetAddEdge`/`SetAddEdge`/`GetRemoveEdge`/
    `SetRemoveEdge`/`ClearEdgesTo`); `ArchetypeGraph.hpp` deleted; `GetArchetypeWithAdded`/
    `GetArchetypeWithRemoved` (`ArchetypeManager.hpp:1092-1106`) rewired to read the edge directly off
    `from`. Verified in source (not a leftover path).
  - **Measured (median of 6 interleaved before/after runs, same session, `bench-compare/`; see
    `bench-compare/RESULTS.md` "W6+W2" section for full methodology):**
    - add component: **152.5 → 131.5 ns (13.7% faster)** — rebased onto the actual pre-W2 (post-W1)
      starting point (152.5, not the stale 279 above), the predicted target was ~100-110 ns (≈27-33%
      reduction); the measured 13.7% came in below that. The archetype-edge lookup was a real but not
      dominant cost. Residual gap to flecs: 2.41× (down from 2.79× pre-W2: 152.5/54.6, this session's own
      fresh medians).
    - remove component: **106.0 → 86.4 ns (18.4% faster)** — rebased prediction was ~70 ns (≈34%
      reduction); measured 18.4% came in below that too. Residual gap to flecs: 2.64× (down from 3.24×
      pre-W2: 106.0/32.7).
    - create / random_get / iterate: flat within noise, as expected (edge cache isn't on those paths).
    - The win is consistent, not noise: across all 6 paired runs the "after" add/remove samples were
      strictly below every "before" sample (non-overlapping distributions).
    - **Honest gap vs the plan's Phase-B estimate:** the original line above (279→~90, 220→~60) targeted
      the stale pre-W1 baseline (~35-40% reduction from those numbers); rebased onto the actual pre-W2
      (post-W1) starting point the predicted reduction was ≈27-34% (add ~27-33%, remove ~34%); measured
      13.7%/18.4% came in below even that rebased range. The remaining cost is the transition **move**
      itself (per-element `MoveConstruct`/`Destruct` via function pointer, `Archetype.hpp:1433` /
      `ArchetypeChunkPool.hpp:365-396`) — unchanged by W2, and now the clearest lever, which is exactly
      **W3**'s (Phase C) target.

### Phase C — Chunk storage modernization  *(invasive core refactor; create + iterate + memory; SDD with opus, 3-config verify)*
Bundle these — they share the same layout change and the 0..128→0..N cleanup:
- **W5 — Metadata once per archetype.** `ComponentArrayInfo` holds `const ComponentDescriptor*` (shared,
  like flecs `ti`) instead of by-value; chunk carries a packed `[N]` present-columns array
  `{void* base; uint32 stride; const ComponentDescriptor* desc;}` rather than 128 absolute-id slots; drop
  the duplicate `m_componentDescriptors` vector. Kills ~23 KB/chunk and lets every hot loop iterate `0..N`.
- **W4 — Fast-append.** In `AddEntity` (`ArchetypeChunkPool.hpp:110`), skip `DefaultConstruct` when the
  caller emplaces (or `memset` trivial types), iterate the N present columns. Expected: create 367→~150.
- **W3 — Trivial memcpy move.** Per-archetype `isComplex` flag (any non-trivially-relocatable component);
  trivial archetypes memcpy each of the N columns via a size-specialised copy; merge-join over the two
  archetypes' sorted id lists (no per-id `isValid` test). Keep the per-element path for complex types.
  Expected: amplifies W2 on add/remove; helps create.
- **W7 — Compact `int16 id→column[]` + get fast path.** Resolve record→component via an int16 index into a
  small per-archetype array + base-pointer arithmetic, not a walk of the 176 B `ComponentArrayInfo`.
  Expected: random_get 147→~70 (combined with W1); helps iterate.

### Phase D — Deferred/strategic  *(NOT benchmark movers — roadmap features)*
- **Wire `CommandBuffer::Execute` through the existing batch path.** Astra already has
  `GroupEntitiesByArchetype` (`ArchetypeManager.hpp:~1220`), `BatchMoveEntitiesInternal` (~:1243), and
  `BatchMoveComponentsFrom` contiguous-run memcpy (`ArchetypeChunkPool.hpp:220-266`), used by the
  `AddComponents(span)`/`RemoveComponents(span)` APIs — but `Commands/CommandBuffer.hpp` `Execute()` replays
  commands **one at a time**. Route replay through group-by-archetype + batch-move (Mass/DOTS pattern).
  Big win for real-world batched workloads; wiring work, not new architecture.
- **Per-chunk change versions** (DOTS): per-archetype SoA version table stamped on RW access; lets systems
  skip unchanged chunks. This is the **change-detection feature** on the remediation roadmap AND a future
  iteration lever — new subsystem, deferred.

---

## 4. Do NOT chase (model-inherent — from the EnTT dissection)
- EnTT's **12.5 ns add / 16 ns remove**: single sparse-set pool touch, no chunk move. Impossible once
  archetypes co-locate an entity's components in one chunk. Target flecs's 60/35, not EnTT's.
- EnTT's **44 ns zero-component create**: entity can exist in no storage; archetypes always need a slot.
- EnTT **owning-group** machinery: Astra chunks already provide permanent SoA contiguity for free — which
  is exactly why `iterate2` (1.54) already sits near EnTT's owning-group (1.18), far ahead of its view (2.37).

---

## 5. Validation protocol
After each phase, rebuild and re-run the head-to-head (`bench-compare/`, `build_one.bat`) and update
`RESULTS.md`. Gate on: (a) the three test suites stay green in Debug/Release/Dist; (b) no regression on the
iteration ops; (c) the targeted op moves toward the flecs number. Keep each W-item an atomic, separately
benchmarkable change so wins/regressions are attributable.

## 6. Expected end-state (rough, if A–C land)
add ~80-90 · remove ~50-60 · create ~120-150 · random_get ~70-80 · iterate2 ~1.0-1.2 —
i.e. roughly **flecs parity on the same model**, which is the stated goal.

## 7. Suggested execution order
~~W1~~ ✅ done (2026-07-22) → ~~W6~~ ✅ done (2026-07-22) → ~~W2~~ ✅ done (2026-07-22) →
**Phase C (next): W5 ⇒ W4 ⇒ W3 ⇒ W7**, beginning with W5. Reuse the SDD model
(brainstorm→spec→plan→SDD; opus on the core storage diffs in Phase C; independent 3-config verify;
finish = merge-to-dev-local-FF, delete branch, don't push). W1/W6/W2 were small enough to each land as
their own SDD unit with quick, independently-benchmarkable wins; Phase C is the larger, invasive chunk-
storage refactor (shared layout change across W5/W4/W3/W7) and should be scoped/sequenced accordingly —
W5 lands first since W4/W3/W7 build on its layout change; W3 (trivial memcpy move) is the clearest lever
on add/remove per the W6+W2 measured results above once that groundwork is in place.

---

### Appendix — source dissections
- `scratchpad/dissect-flecs.md` — table graph, `flecs_table_fast_move`/`fast_append`, `ecs_get_low_id`, full mapping table.
- `scratchpad/dissect-entt.md` — `basic_sparse_set` paged index, transferable vs model-inherent.
- `scratchpad/dissect-mass.md` — `FMassCommandBuffer` batch-by-archetype, dense entity array, offset-table-once.
- `scratchpad/dissect-dots.md` — EntityCommandBuffer playback, chunk header, change versions.
