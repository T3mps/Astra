# random_get: record → direct-chunk-pointer

**Date:** 2026-07-24
**Status:** Approved (user, 2026-07-24) — design amended after source re-verification
**Baseline:** dev @ `ec1d5ce`; tests Debug 724 / Release 722 / Dist 722; quiet-machine 3-way bench (N=1M, medians): random_get Astra 69.7ns vs flecs 62.8 vs EnTT ~32.

## 1. Problem

`Registry::GetComponent<T>(entity)` resolves entity → component through ~8 dependent loads:

1. `EntityTable` segment pointer → `EntityRecord` (validate version + archetype) — `EntityTable.hpp:160`
2. `rec->archetype` dereference → `m_mask.Test(id)` — `Archetype.hpp:575`
3. `m_chunks` vector data pointer → `unique_ptr` slot at `chunkIndex` → chunk pointer — `Archetype.hpp:584`
4. `chunk->m_meta` → `idToColumn[id]` → `m_columns[col].base + index*stride` → component — `ArchetypeChunkPool.hpp:507-516`

Steps 2–3 (the `Archetype` dereference and the `m_chunks[i]` hop) are removable: the chunk pointer can live in the `EntityRecord` itself, and the mask test is redundant for non-tag components (§4.5). Post-change chain ≈ 5 dependent loads; the `Archetype` object leaves the hot path entirely.

Astra is ~11% behind flecs on random_get (69.7 vs 62.8ns); EnTT's 32ns shows the headroom for shorter chains.

## 2. Goal & success criteria

- random_get median ≤ ~63ns (flecs parity or better) under the quiet-machine protocol (5–7 interleaved rounds, medians, N=1M).
- create / add / remove / iterate within noise of baseline (the funnel adds one L1-hot load to some structural paths; expect unmeasurable).
- 3-config test suite green vs baseline 724/722/722.
- No on-disk format change (locations never serialize — verified: rebuilt at load).

## 3. Non-goals

- Iteration-path changes (lever 2, separate).
- `EntityLocation` type change (Option C — rejected, §9).
- Command buffer / batch API changes.

## 4. Design

### 4.1 EntityRecord layout (Option A — user-approved)

```cpp
struct alignas(32) EntityRecord
{
    Archetype*          archetype = nullptr;  // null ⇒ no location assigned
    ArchetypeChunk*     chunk     = nullptr;  // cached: archetype->m_chunks[location.chunkIndex].get()
    EntityLocation      location;             // {chunkIndex, entityIndex}
    Entity::VersionType version   = 0;        // 0 ⇒ dead/empty (NULL_VERSION)
};
```

24B → 32B. `alignas(32)` guarantees no record ever straddles a cache line (today 1/3 of 24B records do). Existing `static_assert`s (trivially copyable / trivially destructible) unchanged and still hold.

**Invariant (the one rule):** whenever `archetype != nullptr` and `location.IsValid()`,
`chunk == archetype->GetChunks()[location.GetChunkIndex()].get()`. Enforced structurally by the write funnel (§4.3) and checked by a debug assert on the read path (§4.4).

**Memory:** +8B/entity (1M entities: 24MB → 32MB table). `EntityTable` is size-aware — `SegmentBytes() = entitiesPerSegment * sizeof(EntityRecord)` (`EntityTable.hpp:498`); 64K-entity segments go 1.5MB → 2MB = still exactly one 2MB huge page. Zero table-logic changes.

**Alignment plumbing (verified — nothing to change):** huge-page path is 64B-aligned (`EntityTable.hpp:606`) and `SegmentBytes()` is a multiple of 32 (pow2 count × 32B); heap fallback is `std::make_unique<EntityRecord[]>(cap)` (`EntityTable.hpp:459`), and C++17 aligned-new honors `alignas(32)` automatically. Segment `Reset`/construction fill whole `EntityRecord{}` values (null state — funnel-consistent), and `EntityTable::Create/Destroy` write only `version` (lines 121/141/203-207) — no funnel bypass, no serialization of raw records anywhere in the file.

### 4.2 Prerequisite: hoist `Chunk` to top-level `ArchetypeChunk`

`ArchetypeChunk` is currently an alias for the *nested* `ArchetypeChunkPool::Chunk` (`Archetype.hpp:43`); nested types cannot be forward-declared, and `EntityRecord.hpp` is a leaf header that must not include the pool. Mechanical hoist:

- Move the class out of `ArchetypeChunkPool` as top-level `class ArchetypeChunk` (same file, same body; `friend class ArchetypeChunkPool` retained).
- Inside the pool: `using Chunk = ArchetypeChunk;` — all 19 internal `Chunk` references keep compiling.
- `Archetype.hpp:43` alias becomes redundant (delete or keep as no-op alias).
- `ChunkDeleter` stays nested (only used where the pool is visible; the record stores a raw pointer).
- External qualified uses of `ArchetypeChunkPool::Chunk`: none in code (verified — only docs/comments).
- `EntityRecord.hpp` adds `class ArchetypeChunk;` forward declaration.

### 4.3 Write funnel — all record-storage writes go through helpers

All location writes live in `ArchetypeManager` (+ `EntityTable::SetRecord`); `Archetype` ops only *return* moved-entity lists which the manager applies (verified by include-wide grep). Changes:

- **`EntityTable::SetRecord`** gains the chunk: `SetRecord(IDType id, Archetype* archetype, ArchetypeChunk* chunk, EntityLocation location)`. Signature change ⇒ every caller breaks loudly and must supply it.
- **`ArchetypeManager` internal helpers** (the only sanctioned way to write a record's storage fields; never touch `version`):

```cpp
// Caller already holds the chunk (create paths, Deserialize chunk-walk):
static void SetRecordLocation(EntityRecord* rec, Archetype* arch,
                              ArchetypeChunk* chunk, EntityLocation loc);
// Resolve the chunk from archetype+location (move/fixup paths; the chunk is
// L1-hot at these sites — it was just written by the move):
static void SetRecordLocation(EntityRecord* rec, Archetype* arch, EntityLocation loc);
// Destroy/clear: archetype = nullptr, chunk = nullptr, location = {}:
static void ClearRecordLocation(EntityRecord* rec);
```

The resolving overload asserts `loc.IsValid()` and `chunkIndex < chunks.size()`.

**Write-site inventory (all 16, verified by grep 2026-07-24; every one routes through a helper):**

| Site (`ArchetypeManager.hpp`) | What it is | Helper |
|---|---|---|
| 83–85 | `AddEntity` create | chunk-taking or resolving |
| 104–106 | `AddEntityWith` create | 〃 |
| 168–170 | `AddEntities` batch create | 〃 |
| 187–189 | `AddEntitiesWith` batch create | 〃 |
| 207 | swap-remove moved-entity fixup | resolving |
| 212 | `RemoveEntity` record clear | `ClearRecordLocation` |
| 245 | batch-remove moved-entity fixup | resolving |
| 255 | batch-remove record clear | `ClearRecordLocation` |
| 269 | `SetEntityLocation` → `SetRecord` | widened `SetRecord` |
| 981–983 | `Deserialize` record wiring (chunk-walk) | chunk-taking |
| 1242–1244 | `MoveAndAdd` batch destination | chunk-taking or resolving |
| 1271 | batch-move moved-entity fixup | resolving |
| 1356–1358 | batch apply new locations | resolving |
| 1382 | `CompactChunks`/defrag moved-list apply | resolving |
| 1145–1159 | transition move (**`oldLoc` aliases the live record** — both the `movedRec` fixup at 1153 and the aliased writes at 1157–1159) | resolving |
| 1415–1429 | transition move ByID (same aliasing) | resolving |

### 4.4 Hot-path rewrite

`ArchetypeManager::GetComponent<T>` (`ArchetypeManager.hpp:514`):

```cpp
EntityRecord* rec = m_records->GetRecord(entity.GetID());
if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
    return nullptr;
if constexpr (std::is_empty_v<T>)
{
    return rec->archetype->GetComponent<T>(rec->location);      // tags need the mask
}
else
{
    ComponentID id = TypeID<T>::Value();
    if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY      // INVALID_COMPONENT (Theme-E refusal):
        return nullptr;                            // preserve the old guarded-Test nullptr
    ASTRA_ASSERT(rec->chunk ==
                 rec->archetype->GetChunks()[rec->location.GetChunkIndex()].get(),
                 "EntityRecord chunk/location desync");
    return rec->chunk->GetComponent<T>(rec->location.GetEntityIndex());
}
```

The `id >= MAX_COMPONENTS` guard is load-bearing, not defensive decoration: the old path's safety for over-ceiling/collision-refused ids came from `Bitmap::Test`'s internal range guard (`Bitmap.hpp:61` — `index >= Bits ⇒ false`), which the mask-skip removes. Without it, `idToColumn[INVALID_COMPONENT]` is an OOB read in Release (the `ASTRA_ASSERT` in `GetComponentPointer` compiles out). `INVALID_COMPONENT = numeric_limits<ComponentID>::max()` (`Component.hpp:14`) ≥ `MAX_COMPONENTS`, so the guard catches it; cost is one register compare + never-taken branch — zero memory traffic.

The debug assert makes the entire existing test suite a desync detector in Debug.

`HasComponent` unchanged (mask test, no location involved).

### 4.5 Mask-test elimination — equivalence argument

For non-empty `T`, skipping `m_mask.Test(id)` is exactly behavior-preserving:

- `desc.size = std::is_empty_v<T> ? 0 : sizeof(T)` (`ComponentRegistry.hpp:147`), and `BuildColumnMeta` excludes columns iff `desc.size == 0` (`Archetype.hpp:76`). So *tag ⟺ `is_empty_v<T>` ⟺ no column*.
- Therefore for non-empty `T`: component in mask ⟺ column built ⟺ `idToColumn[id] >= 0`. Absent ⇒ `GetComponentPointer` returns `nullptr` (`ArchetypeChunkPool.hpp:513`) ⇒ `GetComponent<T>` returns `nullptr` — identical to the old mask-fail result.
- Empty `T` diverges (`idToColumn == -1` whether present or not; `Chunk::GetComponent` would return the shared static instance even when the archetype lacks the tag) ⇒ tags keep the old mask-tested path via `if constexpr`.
- Out-of-range ids (`INVALID_COMPONENT` from Theme-E refusal) were previously handled by `Bitmap::Test`'s range guard ⇒ re-added explicitly as the `id >= MAX_COMPONENTS` guard in §4.4. The Registry ByID paths need no such guard — all four sites receive `componentId` from validated sources (descriptor lookup, hash map, `ForEachComponent`), never an unchecked id.

### 4.6 Registry ByID/hash paths

`Registry.hpp:505, 565, 638, 756`: replace `record->archetype->GetChunks()[record->location.GetChunkIndex()]` with `record->chunk`. Each site already validates the record first (they fetch via `GetEntityRecord`, which returns non-null only when version matches and archetype is set — `ArchetypeManager.hpp:260`); their defensive `chunkIndex < chunks.size()` bounds checks convert to `record->chunk != nullptr` checks. The tag/mask logic at these sites (`Registry.hpp:628`) is unrelated to the hop and stays. `GetComponentArrayByID` + entityIndex arithmetic unchanged. Multi-get `GetComponents<Ts...>` benefits automatically via the rewritten single-get.

### 4.7 Invalidation completeness

The cached pointer can only go stale where the location already changes — no new invalidation class exists:

- **Structural moves** (create, transition add/remove, swap-remove, batch ops, `CompactChunks`, `Defragment`, `Deserialize`): all already rewrite `location` at the 16 funneled sites; the chunk pointer updates in the same call.
- **Chunk memory never moves in place:** TLSF blocks are address-stable for their lifetime; `CompactChunks` rebuilds into *new* chunks and reports every live entity (records rewritten).
- **Chunk free:** `PopBackChunk` only frees empty chunks — no live record can reference one.
- **Destroy:** version bump already invalidates the handle; `ClearRecordLocation` additionally nulls `chunk` alongside `archetype` (no dangling pointer at rest).
- **Entity created but not yet placed:** record has `version` set, `archetype == nullptr`, `chunk == nullptr` — the read-path guard already rejects it.

## 5. Testing (TDD, per SDD)

New targeted tests (reuse `Astra::Test::*` / existing component types — TypeID ceiling):

1. Get-after-swap-remove: remove a non-last entity, verify the swapped-in (former last) entity's `GetComponent` returns its own values.
2. Get-after-`CompactChunks` (via `Registry::Defragment`): all surviving entities readable with correct values.
3. Get-after-save/load: `GetComponent` correct on the deserialized registry.
4. Tag component: present ⇒ non-null (static instance), absent ⇒ null — exercises the `is_empty` branch both ways.
5. Absent non-empty component ⇒ `nullptr` (exercises the `idToColumn < 0` path that replaced the mask test).
6. Stale handle (destroyed entity) ⇒ `nullptr`.
7. `alignof(EntityRecord) == 32` / `sizeof(EntityRecord) == 32` static_asserts.

**Testability note on the `id >= MAX_COMPONENTS` guard:** a direct test needs a type whose `TypeID` is `INVALID_COMPONENT`, which requires exhausting the 128-id ceiling — the test binary deliberately sits *near* the ceiling with slack reserved (Theme E), so burning the remaining ids would break the suite. The guard is covered by review + the Debug `ASTRA_ASSERT` in `GetComponentPointer`; test 5 covers the adjacent in-range-absent path.

Plus: full 3-config suite (Debug run doubles as desync-assert sweep) green vs 724/722/722.

## 6. Bench validation

Quiet-machine protocol: 5–7 interleaved rounds Astra/EnTT/flecs, medians, N=1M (recipe in `bench-compare/RESULTS.md`). Success per §2. Watch structural ops for funnel-load regressions (expect noise-level). Update `RESULTS.md` with the A/B.

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Future code writes `location` bypassing the funnel → stale `chunk` UAF | Funnel helpers are the only in-tree pattern; read-path debug assert catches desync across the whole suite; comment on `EntityRecord` states the invariant |
| A missed 17th write site today | Inventory grep-verified 2026-07-24; `SetRecord` signature change breaks any missed `SetRecord` caller at compile time; Debug suite + assert sweeps the rest |
| Heap-fallback segment ignores `alignas(32)` | Closed — verified: `make_unique<EntityRecord[]>` uses C++17 aligned-new (`EntityTable.hpp:459`); test 7 asserts `alignof`/`sizeof` |
| Mask-skip drops `Bitmap::Test`'s range guard → Release OOB on `INVALID_COMPONENT` | Closed — explicit `id >= MAX_COMPONENTS` guard in the fast path (§4.4/§4.5) |
| Structural-path regression from resolve loads | Chunk is L1-hot at every resolve site (just written by the move); bench A/B confirms |
| Hoist breaks hidden `ArchetypeChunkPool::Chunk` users | Grep found zero code uses outside the alias; compiler catches any stragglers |

## 8. Expected outcome

Dependent-load chain 8 → ~5; `Archetype` deref off the hot path; no cache-line straddle. Prediction: random_get 69.7 → low-60s or better (flecs 62.8; the remaining gap to EnTT's 32 is model-inherent sparse-set vs archetype indirection). Honest caveat: the Phase-2 sweep's +16% prediction for random_get missed (fixed-size confound); this lever attacks the indirection depth directly, which is what the miss pointed at — but the number is a prediction, not a promise.

## 9. Alternatives considered

- **B — replace `chunkIndex` with `chunk*` (24B record, derive location):** no redundant state, but keeps index-keyed APIs via per-call derivation and needs chunk-index bookkeeping; superseded by A once the write funnel made A's invariant structural. Not chosen by user.
- **C — re-key `EntityLocation` as `{chunk*, entityIndex}`:** rejected on evidence: two removal algorithms sort `EntityLocation`s by *index order* as a correctness requirement (`Archetype.hpp:454` descending swap-remove; `ArchetypeManager.hpp:1316` batch-destroy ordering). Pointer ordering compiles but is silently wrong, so C needs chunk-index bookkeeping anyway — a 121-occurrence migration for the identical hot path, with its purity advantage punctured. A also keeps C reachable later.
