# ArchetypeManager::Deserialize entity-id guard (design)

**Date:** 2026-07-27
**Status:** Approved (P0 #1 from the 2026-07-27 full review; fix design verified against current code)
**Scope:** one guarded lookup + corruption tests. Single SDD task.

## 1. The defect (verified in current code)

`ArchetypeManager::Deserialize`'s entity-to-archetype mapping loop validates `archetypeIndex`/`chunkIndex`/`entityIndex` against the restored layout (ArchetypeManager.hpp:1022-1028) but passes the **raw wire entity id** to a *creating* accessor:

```cpp
EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());   // :1032
```

`GetOrCreateRecord` → `GetOrCreateSegment` computes `segIdx = id >> entitiesPerSegmentShift` and **resizes the segment index to accommodate it** (EntityTable.hpp:171-175). None of the guards `EntityManager::Deserialize` meticulously applies to wire ids (`id < Entity::ID_MASK` at EntityManager.hpp:517/589; `segIdx < kMaxSegmentsOnLoad = 1<<20` at :533/:603) are applied here. Consequences of a crafted/corrupt mapping id:

- **Default 32-bit-id build:** bounded but real memory amplification (segment-index resize up to ~1M entries + a segment allocation) *and* the load **silently succeeds** on corrupt data — a dead-record write instead of a rejected save.
- **64-bit-id build (`AstraCompile64` config):** `segIdx` is attacker-controlled up to 2^44+ → unbounded resize → uncaught `std::bad_alloc` escaping the exception-free `Registry::Load` — process termination (DoS) from untrusted input. This is the P0.

Also relevant: the loop's own comment (:1030) states the invariant "Segments already exist (EntityManager restored versions first)" — meaning a *creating* accessor is wrong by the function's own contract, not just unsafe.

## 2. The fix (lookup-don't-create + aliveness cross-check)

Replace the creating accessor with the existing **non-creating, null-returning, allocation-free** lookup and reject the load on any mismatch:

```cpp
// Wire entity ids are untrusted. EntityManager::Deserialize restored every
// live entity's segment+version BEFORE this runs (see the invariant note
// above), so a legitimate mapping's record must already exist AND carry the
// same version. GetRecord is non-creating and allocation-free for ANY id --
// GetSegment bounds-checks segIdx against the existing segment index
// (EntityTable.hpp:563) -- so a crafted huge id (the 64-bit unbounded-resize
// DoS) and a mapping to a dead/never-restored entity both fail the load
// instead of allocating or silently corrupting (2026-07-27 review P0).
EntityRecord* rec = m_records->GetRecord(entity.GetID());
if (!rec || rec->version == 0 || rec->version != entity.GetVersion())
    return false;
```

Why this beats mirroring the `ID_MASK`/`kMaxSegmentsOnLoad` guards:
- **Allocation-free for arbitrary hostile ids in every config/width** — `GetSegment` bounds-checks `segIdx >= m_segmentIndex.size()` → nullptr (EntityTable.hpp:560-585, both overloads). No constants duplicated, no width-dependent reasoning.
- **Strictly stronger:** also delivers the review's "cross-check the mapped entity is in the restored alive set" — a mapping for an entity EntityManager didn't restore fails via null segment (id outside restored segments) or version mismatch (dead slot `version == 0`, stale version, or crafted `version == 0` wire entity — all three rejected by the predicate).
- **Matches the function's stated invariant** (:1030) — the accessor now *enforces* what the comment only asserted.

`GetRecord` is already used elsewhere in ArchetypeManager (:204); `EntityRecord.version == 0` is the documented dead/empty marker (EntityRecord.hpp:15,30). The other `GetOrCreateRecord` call sites in ArchetypeManager (:83, :103, :166, :197) are live entity-creation paths taking trusted, freshly-allocated ids — out of scope, unchanged.

## 3. Tests (`tests/Serialization/LoadRobustnessTest.cpp` — extend, follow its existing save-corrupt-load conventions; reuse existing test component types only, 128-TypeID ceiling)

1. **Huge-id mapping corruption:** build a small registry, Save, locate a mapping record's entity id in the buffer (or re-serialize with a patched id), set it to a huge value (max representable), Load → must return the error path (no crash, no success). On current code this FAILS (load succeeds after allocating) = RED.
2. **Dead/never-restored-id corruption:** patch a mapping entity id to a valid-range id that was never alive in the save (e.g. beyond the restored id range but within an existing segment, or version-bumped) → Load must fail. RED on current code (silent success).
3. **Round-trip regression:** existing Save→Load green tests must stay green (the new predicate must not reject legitimate saves — versions restored by EntityManager match wire versions by construction).

**64-bit note (no test theater):** the 64-bit DoS is closed *by construction* — `GetRecord`'s bounds check is width-independent and allocation-free, so no separate 64-bit runtime lane is added (the `AstraCompile64` compile gate still covers the width building). Documented here as the honest argument.

## 4. Constraints

MSBuild recipe as ledgered; 3-config green; only `ArchetypeManager.hpp` (the one call site + comment) and `LoadRobustnessTest.cpp` change; TDD (tests RED first); Result/graceful-failure semantics preserved (`return false` feeds the existing typed error path); OPUS review (core serialization diff).
