# Theme G — Missing destruct / leaks on move & remove paths

**Date:** 2026-07-20
**Review of record:** `docs/reviews/2026-07-11-astra-full-review.md` §Theme G (lines 116–118)
**Status:** design approved (user), ready for `writing-plans`
**Branch:** off `dev` (currently `3197518`); finish = merge-to-`dev` local FF, delete branch, do not push.

## 1. Problem

Four independent lifetime-management defects on the archetype/container move-and-remove
paths. Each leaks a non-trivial component/element, leaves memory uninitialized, or reports
a false success count. Every one has a *proven-correct sibling already in the tree* to
mirror, so the fixes are small and low-risk. None change the wire format or any public API
signature.

All fixes must be correct in **all three configs** (Debug / Release / Dist) and must not
introduce an assert-and-abort on a recoverable condition (the project's uniform-graceful
misuse policy).

## 2. The four fixes

### Fix 1 — `ArchetypeChunkPool::RemoveEntity` leaks the moved-from source slot
**Site:** `include/Astra/Archetype/ArchetypeChunkPool.hpp:345–396` (swap branch `352–374`).

The swap-and-pop branch (`index != lastIndex`) does, per component:
```cpp
info.descriptor.Destruct(dstPtr);            // destruct the removed entity's component
info.descriptor.MoveConstruct(dstPtr, srcPtr); // move the last entity into the hole
// MISSING: info.descriptor.Destruct(srcPtr);   // moved-from source slot never destructed
```
`pop_back()` then drops the entity record, so the moved-from object at `lastIndex` is
leaked. For a component with a non-trivial destructor (owns heap, file handle, etc.) this
leaks on **every non-tail removal**. The tail branch (`index == lastIndex`) already
destructs correctly, and the sibling `Archetype::MoveEntitiesBetweenChunks`
(`Archetype.hpp:1444–1446`) does `MoveConstruct` **then** `Destruct(src)`.

**Fix:** add `info.descriptor.Destruct(srcPtr);` after the `MoveConstruct` in the swap
branch. One line.

### Fix 2 — `FlatSet::Emplace` leaks the probe temporary
**Site:** `include/Astra/Container/FlatSet.hpp:420–563` (`temp` at `425–439`, dismiss at `557`).

`Emplace` constructs a probe temporary `*temp` to check for duplicates, guarded by a RAII
`Cleanup` that destructs it. After moving the value into the slot:
```cpp
std::allocator_traits<AllocatorType>::construct(m_alloc, m_slots[insertIdx].GetValue(), std::move(*temp));
cleanup.dismissed = true; // BUG: suppresses ~T() on the probe temporary
```
For a **copyable-non-movable `T`**, `std::move(*temp)` (an rvalue) binds to the *copy*
constructor (no move ctor exists; rvalue binds to `const T&`), so the slot gets a **copy**
and `*temp` remains fully live. Dismissing the cleanup then skips its `~T()` → a
whole-object leak on **every insert**. (For a normally-movable `T` the moved-from husk's
destructor is usually a no-op, so the bug is silent there, but skipping it is still
incorrect.)

**Fix:** remove the `dismissed` mechanism so `Cleanup` **always** destructs `*temp`. The
slot's object is separate, so no double-free. Verified correct on every exit path:
duplicate-found early returns (`470`, `489`), the `ASTRA_ENSURE`-fail return (`538`), and
the success return (`562`) — the temporary is always destructed exactly once, the slot copy
is never touched by `Cleanup`.

### Fix 3 — `ArchetypeManager::MoveAndAddByID` leaves a move-only new component uninitialized
**Site:** `include/Astra/Archetype/ArchetypeManager.hpp:1360–1418` (cascade `1391–1406`).

The type-erased CommandBuffer add path constructs the newly-added component from the
buffer's stored `componentData` via:
```cpp
if (newDesc.is_trivially_copyable)      std::memcpy(dstPtr, componentData, newDesc.size);
else if (newDesc.constructWith)         newDesc.constructWith(dstPtr, componentData);
else if (newDesc.copyConstruct)         newDesc.copyConstruct(dstPtr, componentData);
// MISSING else — a move-only component (all three null) leaves dstPtr as garbage
```
A move-only component **is reachable**: `CommandBuffer::AddComponent` move-constructs the
value into its own storage (`CommandBuffer.hpp:450`, `new (dataPtr) DecayedT(std::forward<T>(component))`),
then passes `&storage` as `componentData` at flush. With no `else`, `dstPtr` is left
uninitialized → UB when the component is later read or destructed.

**Fix:** append two fallbacks, preserving the existing copy-first order:
```cpp
else if (newDesc.moveConstruct)
    newDesc.moveConstruct(dstPtr, const_cast<void*>(componentData)); // move-only: preserve the value
else
    newDesc.DefaultConstruct(dstPtr);                                // never-UB floor
```
`componentData` points into the CommandBuffer's own storage, which is destructed after
flush (`CommandBuffer.hpp:~1791`), so moving from it is safe (the moved-from husk is then
destructed by the buffer). `DefaultConstruct` mirrors the ultimate floor already present in
the descriptor's own `ConstructWith` method (`Component.hpp:118`). Existing copyable paths
are unchanged.

### Fix 4 — `RemoveComponents<T>` count-desync + partial-move entity loss
**Sites:** `RemoveComponents<T>` `ArchetypeManager.hpp:344–369`; `BatchMoveEntitiesInternal`
`1243–1315`; the OOM bail `1280–1291`; the partial-return path in
`Archetype::BatchMoveEntitiesFrom` `Archetype.hpp:1221–1226`.

`RemoveComponents<T>` accumulates the count unconditionally:
```cpp
BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
removedCount += entityBatch.size();   // counts the full batch even if nothing moved
```
But `BatchMoveEntitiesInternal` calls `dstArchetype->BatchMoveEntitiesFrom(...)`, which
returns `{}` (empty) when chunk allocation fails — `BatchMoveEntitiesFrom` pre-allocates
all needed chunks and, on `CreateChunk` → `nullptr` (pool at `maxChunks`), returns empty
**before placing any entity** (`Archetype.hpp:1164–1177`). `BatchMoveEntitiesInternal` then
bails at `1280–1291` having moved **zero**. Result: `RemoveComponents` reports `N` removed
when `0` were — a false success count (the desync flagged in the review). Only the templated
batch path is affected; the `RemoveComponentsByID` / `AddComponentsByID` loops already count
per-success (`439`, `489`).

There is additionally a documented "shouldn't happen" partial path: if
`BatchMoveEntitiesFrom` ever returns a **non-empty but short** `newLocations`
(`Archetype.hpp:1221–1226`), `BatchMoveEntitiesInternal` remaps only the moved prefix
(`1298`, loop over `newLocations.size()`) but then `RemoveEntities(srcLocations, true)`
removes **all** source entities (`1304`) → the un-moved entities are removed from src yet
never placed in dst = silent entity loss / dangling `m_entityMap` entries.

**Fix (both, per approved scope):**
1. `BatchMoveEntitiesInternal` returns `size_t` = entities actually moved
   (`newLocations.size()`); returns `0` on the OOM bail.
2. The three wrappers (`BatchMoveEntitiesWithoutComponent`, `BatchMoveEntitiesWithComponent`,
   `BatchMoveEntitiesWithComponentByID`) propagate the returned count.
3. `RemoveComponents<T>` accumulates the **returned** count instead of `entityBatch.size()`.
   (`AddComponents<T>` at `285` is `void` — no count to fix — but shares the same root and
   benefits from the hardening below.)
4. Harden the partial path: `RemoveEntities` is passed only the **moved prefix** of
   `srcLocations` (`srcLocations` truncated to `newLocations.size()`); the existing
   `movedEntities` patch loop (`1307–1313`) keeps any swap-and-pop-relocated survivors'
   `m_entityMap` correct. Add `ASTRA_ENSURE(newLocations.size() == entityBatch.size(), …)`
   to flag the unexpected partial in Debug (diagnostic only — the prefix truncation makes it
   safe in all configs).

## 3. Design decisions (user-approved)

1. **Scope:** all four fixes ship as one Theme G work item (one brainstorm → spec → plan →
   SDD cycle). They are cohesive ("missing destruct / leak on move & remove"), small, and
   share leak-accounting test infrastructure.
2. **Fix 3 flavor:** `moveConstruct` branch **then** `DefaultConstruct` floor — preserves a
   move-only component's value instead of discarding it, while still guaranteeing no
   uninitialized memory. Chosen over a bare `DefaultConstruct` floor (which would silently
   default-construct a move-only add, losing the user's value).
3. **Fix 4 depth:** count truthfulness **and** partial-path hardening — prevents both the
   false count and the latent silent entity loss. Chosen over count-only.

## 4. Test strategy

Leak detection is by a static live-instance counter (`ctor ++s_live; dtor --s_live;`); a
leak leaves `s_live` above the true live count. Tests reset the counter in `SetUp` and run
single-threaded.

- **Shared move-only lifetime-counting component** — one new type in the `Astra::Test::`
  namespace (qualified name ⇒ distinct `TypeID` hash, no Theme-E collision; *defining* it is
  free — a `ComponentID` is only consumed on first use). Move-only, carries an `int value`.
  Serves **Fix 1** (removed via `MoveConstruct`; after removing a non-tail entity assert
  `s_live == liveCount`, RED leaves it one too high) and **Fix 3** (deferred
  `CommandBuffer::AddComponent`, flush, then assert the component holds the moved-in value
  **and** `s_live` accounting is correct — RED reads uninitialized memory).
- **Fix 2** — a **file-local copyable-non-movable** counted type (deleted move ctor). It is
  *not* an ECS component, so it consumes **zero** `ComponentID`s. A `FlatSet` of it with a
  trivial hash/equal; after one `Emplace` assert `s_live == 1` (RED: `2`).
- **Fix 4** — a `Registry` built with a tiny `chunkPoolConfig` (`maxChunks` and
  `chunksPerBlock` both small; note `maxChunks` is clamped up to `chunksPerBlock` at
  `ArchetypeChunkPool.hpp:570–572`) to deterministically exhaust the pool so the
  remove-target archetype cannot get a chunk. Assert `RemoveComponents<T>` returns the true
  moved count (RED: full batch size) and the un-moved entities still have the component and
  are intact.

New `.cpp` test files require regenerating `ide/` via `D:/dev/_shared/tools/premake5.exe vs2022`
(never `git add ide/`); appending to an existing test file does not. Match each file's
`TEST` vs `TEST_F` macro. Net TypeID cost: **+1** component ID.

## 5. Scope / non-goals

- **In scope:** the four fixes above + their regression tests + the one shared test
  component.
- **Out of scope:** the broader Theme-I iteration-UAF family, Theme-F empty/tag
  asymmetries, and any refactor of the batch-move machinery beyond the return-count and
  prefix-safe removal. Do not "finish the job" on unrelated `RemoveEntity`/`RemoveEntities`
  callers unless a leak of the same class is found there during implementation (if so, note
  and fix in the same spirit — mirror the correct sibling).

## 6. Acceptance criteria

- All three configs (Debug / Release / Dist) build clean and green; controller-verified
  independently on the merge candidate.
- New leak-accounting tests fail RED before each fix and pass GREEN after; each test would
  fail if the fix regressed.
- No new assert-and-abort on a recoverable condition; the added `ASTRA_ENSURE` is
  diagnostic-only and the safety (prefix truncation, always-destruct, move-then-default
  floor) holds in Release/Dist.
- No public API signature change; `RemoveComponents<T>`'s returned value becomes truthful.

## 7. Execution model

Reuse the proven SDD model: `writing-plans` → `subagent-driven-development`; fresh sonnet
implementer per task with a task-brief; **opus** review on every memory-safety diff (all
four fixes qualify); opus final whole-branch review; controller independent 3-config
build+test on the merge candidate; finish via `finishing-a-development-branch` = merge to
`dev` local FF, delete branch, do not push.

## 8. Files touched (anticipated)

- `include/Astra/Archetype/ArchetypeChunkPool.hpp` — Fix 1
- `include/Astra/Container/FlatSet.hpp` — Fix 2
- `include/Astra/Archetype/ArchetypeManager.hpp` — Fix 3 + Fix 4
- `tests/TestComponents.hpp` — shared move-only lifetime-counting component
- new test `.cpp` file(s) under `tests/Container/` (Fix 2) and `tests/Registry/` (Fixes 1,
  3, 4) — exact placement decided in the plan; regen `ide/` for new files
