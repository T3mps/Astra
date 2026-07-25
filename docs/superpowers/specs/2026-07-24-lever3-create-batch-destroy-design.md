# Lever 3 — create_batch + destroy (perf-arc finale)

**Date:** 2026-07-24
**Baseline:** dev @ `5aa9df9` (Definitive Scoreboard). Branch: `perf/lever3-create-destroy` off dev HEAD at execution time. Local only — never push.
**Status:** Design approved by user (relations mechanism: "both, staged" — empty-graph check ships, per-entity flag is a designed-but-deferred appendix; approach A: both sub-levers, one branch, measured separately).

## 1. Context and evidence (verified in source @ 5aa9df9)

The Definitive Scoreboard (RESULTS.md 2026-07-24, items-parity-validated, non-overlapping
bands) left exactly two large same-model gaps vs flecs:

- **destroy: 26.14 vs 14.58 ns (~1.8x behind).** `Registry::DestroyEntity`
  (`Registry.hpp:222-232`) performs THREE walks of the same unified record —
  `IsValid` (:224), `AM::RemoveEntity`'s validation (`ArchetypeManager.hpp:189-208`,
  identical guard), and `EntityManager::Destroy` (:231, its own table walk for the
  version bump) — plus `RelationshipGraph::OnEntityDestroyed`
  (`RelationshipGraph.hpp:309+`) which pays **three FlatMap probes**
  (`m_parents.Contains`, `m_children.Find`, `m_links.Find`) on EVERY destroy even when
  the graph has never held a relationship. flecs's analog is a table-flag bit test.
  Estimated split of the 11.6 ns gap: ~2-4 ns walks, ~5-8 ns probes.
- **create_batch: 34.06 vs 17.17 ns/entity (~2.0x behind).**
  `Archetype::AddEntitiesWith` (`Archetype.hpp:331+`) pre-grows capacity correctly but
  then runs an ENTITY-MAJOR loop: per entity `GetOrCreateChunk()` (first-non-full
  scan), individual `push_back` + `SetCount`, generator tuple, per-element component
  writes; then `AM::AddEntitiesWith` (`ArchetypeManager.hpp:172-187`) runs a SECOND
  per-entity loop using the resolving `SetRecordLocation` overload — a
  `GetChunks()[chunkIndex]` hop per entity. flecs's `ecs_bulk_init` is column/run
  oriented. Lever-2's B4 fixed the single-entity `AllocateEntitySlot`; this is its
  batch-path sibling, never visited.

Lever-2 precedent (validate-once + waste-kill) closed remove from 10% behind to 23%
ahead; both mechanisms here are the same classes.

## 2. Goal and success criteria

- **Targets (predictions, not promises; misses reported honestly):** destroy at/within
  noise of flecs (~14.6 ns); create_batch at/within noise of flecs (~17.2 ns/entity).
- **No regression elsewhere:** create (per-entity), add/remove, iteration, random_get
  within noise — shared code alert: the chunk-run path may be shared by `AddEntities`
  (no-generator) if natural; `DestroyEntities` (span) is NOT in scope (unmeasured;
  follow-up note).
- 3-config green at every checkpoint; baseline counts (dev @ 5aa9df9, from Lever-2's
  authoritative run — docs-only commits since): **Debug 739 / Release 737 / Dist 737**;
  re-verify at branch time.
- Bench: the definitive harness (items column) is the instrument; checkpoints run the
  relevant op subset + a final full sweep; 6 interleaved rounds, quiet gate, paired
  same-session medians, non-overlapping bands for claims.

## 3. Destroy sub-lever

New `Registry::DestroyEntity` shape (exact code at plan time):

1. `EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);` — the single
   validated fetch (version + archetype checks; `ArchetypeManager.hpp:257+`). Null ⇒
   return. Subsumes `IsValid` (same unified record slot — W1; Lever-2 precedent).
2. `if (m_signalManager.IsSignalEnabled(Signal::EntityDestroyed)) Emit(...)` — hoisted
   gate; Emit already self-gates, so observable behavior is identical.
3. `m_archetypeManager->RemoveEntity(entity, rec)` — NEW internal record-taking
   overload: skips the re-fetch, then today's exact body (swap-remove via chunk,
   movedRec fixup through the funnel, `ClearRecordLocation`). The existing
   entity-only overload remains for other callers and DELEGATES: it performs its own
   validated fetch, then calls the record-taking overload.
4. `if (!m_relationshipGraph->Empty()) m_relationshipGraph->OnEntityDestroyed(entity);`
   — NEW `RelationshipGraph::Empty()` = `m_parents`/`m_children`/`m_links` all empty
   (three size loads). When the graph is non-empty, today's exact per-entity path runs
   unchanged. (Verify-item: FlatMap exposes `Empty()`/`Size()`; add a trivial
   `Empty()` if missing.)
5. `m_entityManager.Destroy(entity, rec)` — NEW internal record-completing overload:
   EM still performs the version bump + freelist push itself (**W1 invariant: EM alone
   writes `version` — unchanged**), it just skips its own table re-walk. The existing
   overload remains.

Behavior table (must hold exactly): invalid/stale entity ⇒ no-op, no signal (today:
IsValid false ⇒ return; after: null validated record ⇒ return). Valid entity ⇒ signal
iff enabled (payload = entity value, unchanged), storage removed, relations cleaned
(iff graph non-empty — and a relation-holding entity makes the graph non-empty by
construction, so cleanup can never be skipped for an entity that needs it), version
bumped. Verify-item: a validated record (version match + archetype non-null) implies
"located" for every creation path (AddEntity/batch/deserialize all assign an archetype
before the entity is observable) — confirm at plan time.

### 3.1 Deferred appendix (designed, NOT built): per-entity relations flag

flecs-equivalent mechanism for mixed worlds: a `uint8_t flags` in `EntityRecord`'s
4-byte tail padding (32B `alignas(32)` layout unchanged — static_asserts stay), bit 0 =
"has ever had a relationship"; set in `SetParent`/`AddLink`(+child-side), cleared
lazily or never (conservative); destroy tests the bit instead of `Empty()`.
Maintenance sites: the RelationshipGraph mutation surface (~4-6 methods). Record-field
discipline: the flag is NOT a storage field (archetype/chunk/location) — it would need
its own write rule beside the funnel (spec'd then). **Deferred behind evidence:** build
only if a mixed-world destroy benchmark (not currently in the suite) shows the
empty-graph check insufficient. This appendix is the design of record for that day.

## 4. create_batch sub-lever

`Archetype::AddEntitiesWith` (and the shared shape in `AddEntities` where it falls out
naturally) becomes CHUNK-RUN based:

- Capacity pre-grow stays as-is (`Archetype.hpp:340-360`).
- Outer loop over runs: `run = min(remaining, currentChunk->capacity - count)`; claim
  the run once — bulk-append the entity range into `m_entities`
  (`insert(end, first, first+run)`; capacity pre-reserved per chunk), ONE
  `SetCount(count + run)`; advance `m_firstNonFullChunkIndex` bookkeeping once per
  chunk transition, not per entity.
- Component writes: hoist typed column base pointers once per run
  (`chunk->GetComponentArray<T>()` per component, typed — the ordinal accessor is
  private/untyped and stays out of this path); per entity in the run:
  `auto tuple = generator(globalIndex);` (contract unchanged: called exactly once per
  entity, in index order), then write each element `base[c] + slot*stride` via typed
  move-assignment/placement — `Components...` are compile-time known ⇒ NO descriptor
  fn-ptrs anywhere on this path. (Non-trivial component types still get correct
  construction — placement-new from the tuple element, i.e. move-CONSTRUCT into raw
  slot memory, matching what the current per-entity path does; verify the current
  path's exact construction semantics at plan time and preserve them.)
- Locations: emitted per entity as today (interface unchanged:
  `std::vector<EntityLocation>` return).
- `AM::AddEntitiesWith`'s record loop keeps its interface but hoists the chunk
  pointer across run boundaries: track `lastChunkIndex`; on change, re-derive
  `archetype->GetChunks()[chunkIndex].get()` once; write records through the 4-arg
  funnel overload (`SetRecordLocation(rec, archetype, chunk, loc)`). Funnel
  discipline unchanged — same helpers, fewer redundant derivations.
- `GetOrCreateRecord` per entity remains (paged-table segment walk is already cheap
  and ID-order... bench IDs are sequential ⇒ same segment; no change designed — noted
  as a possible micro-follow-up only if the checkpoint shows the record loop hot).

## 5. Testing

- NEW: batch-create value integrity ACROSS a chunk boundary — N spanning ≥2 chunks
  (use a large-ish component or enough entities), every entity reads back its own
  generator values (the run-splitting arithmetic is the main risk). Batch-create
  record invariant — reuse `ExpectChunkInvariant` over a batch-created set (Lever-1
  test helper). Both RED-able only via characterization (behavior-preserving rewrite)
  — disclosed, house style; the chunk-boundary test is NEW COVERAGE (may be genuinely
  red-able against a seeded bug — do not manufacture; characterization-first).
- NEW: destroy behavior table tests (valid/stale/invalid x signal on/off — mirror the
  Lever-2 SignalContract idiom in `SignalLifetimeTest.cpp`, reusing its local types).
- NEW, RED-able intent guard: destroy-with-relations — SetParent a family, destroy the
  parent, assert children orphaned + graph version semantics unchanged; this test
  FAILS if the empty-graph early-out is mis-scoped (e.g. inverted or per-entity).
  Must PASS pre-change (characterization) AND stay green post-change.
- Existing nets: desync assert sweep, `Tracked::s_live` lifetime guards, Lever-2
  SignalContract tests, 739-test suite.
- **TypeID ceiling: NO new component types in tests** — reuse `Astra::Test::*` and
  existing file-local types.

## 6. Process

SDD on `perf/lever3-create-destroy`: Task order = destroy sub-lever (sonnet) →
checkpoint 1 (destroy + neighbors op subset) → create_batch sub-lever (OPUS — invasive
archetype rewrite) → checkpoint 2 (create_batch + create + full-sweep) → final
validation + RESULTS.md addendum (sonnet) → OPUS whole-branch review → fix wave if
needed → authoritative 3-config → **user merge gate** → FF-merge local, delete branch,
don't push → memory update. Bench = definitive harness, full-opt flags, quiet gate,
6 interleaved rounds, paired same-session, non-overlapping bands.

## 7. Risks

- Destroy restructure changes which layer rejects an op — the §3 behavior table is the
  contract; signal/relations semantics pinned by tests. Single-writer model, no
  concurrency dimension.
- The chunk-run rewrite touches the same invariant surface Phase C hardened —
  construction semantics for non-trivial components must match the current path
  exactly (`s_live` net + value tests); the run arithmetic off-by-one class is covered
  by the chunk-boundary test.
- Relations early-out: the only semantic risk is skipping cleanup — impossible by
  construction (non-empty graph ⇒ no skip), pinned by the RED-able intent guard.
- create_batch's 2x target is ambitious; the honest floor is "large measured
  improvement + updated scoreboard", per program convention.
