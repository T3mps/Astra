# Astra 3.4.1 — Release-Safety Remediation (Phase 1) — Design Spec

**Date:** 2026-07-11 · **Branch:** `remediation/3.4.1` (from `dev`) · **Version:** 3.4.1 (patch)
**Source of findings:** the 2026-07-11 full-codebase review (`docs/reviews/2026-07-11-astra-full-review.md`).

## Goal

Eliminate the undefined behavior reachable in **ordinary Release/Dist use** — with no adversary and no threads — while leaving the hot path untouched and adding runtime cost only where it is provably near-free and prevents memory corruption. This is Phase 1 of a multi-round remediation; Themes B (concurrency), C (Load trust boundary), D (alignment), E (type identity), and J (Registry API footguns) are explicitly deferred to later rounds (see Out of Scope).

## Governing principle (the rule every fix follows)

Each existing `ASTRA_ASSERT` guard is classified as one of:

- **Invariant** — only an Astra bug can violate it; a correct caller and the runtime cannot cause it (e.g. an internally-computed `index < m_size` in swap-and-pop). → Stays `ASTRA_ASSERT`-only. Unchanged. No Release cost, and a Release check would be pointless.
- **Condition** — can occur with a perfectly correct caller: allocation failure (OOM), capacity exhaustion (a large project registering `> MAX_COMPONENTS` types), an expired `weak_ptr` during teardown, or dynamic/wrong-type input (reflection). → Handled in **all** build configs: an `ASTRA_UNLIKELY`-hinted early return of `nullptr`/`Result::Err`/`false`/no-op. On the cold paths where these live, a never-taken predicted branch costs essentially nothing.

The bug being fixed is that the current code uses `ASTRA_ASSERT` for **conditions**, not just invariants — so in Release the guard vanishes and the code proceeds into UB, contradicting the project's own "graceful in ALL configs" contract. The README behavioral-contract wording stays as-is (this work makes the code match it).

## Scope — work items

### A. Theme-A guard sweep (assert-only → all-config for conditions)

| # | Site | Condition guarded | Fix |
|---|------|-------------------|-----|
| A1 | `ResourceStorage::Set<T>` / `Emplace<T>` (`ResourceStorage.hpp:130-244`) | `id >= MAX_COMPONENTS`; `AllocateMemory` OOM; expired `m_componentRegistry` `weak_ptr` | All-config `ASTRA_UNLIKELY` guards → return `nullptr`/no-op. Never placement-new at `nullptr`. Set `slot.isValid = true` only after storage is fully established (mirror the SetByID fix already shipped in 3.4). |
| A2 | `Reflection::FieldInfo::Get<T>` / `Set<T>` / `GetPtr<T>` (`FieldInfo.hpp:72-123`) | wrong `T` (hash mismatch); `Set` on a `const` field; `GetPtr` on a const field | All-config: `Get<T>` returns a value-initialized `T{}` on mismatch; `Set<T>` is a no-op on mismatch or `isConst` (never calls an empty `std::function` → no `std::terminate`); non-const `GetPtr<T>` returns `nullptr` when `isConst`, and both overloads return `nullptr` on type mismatch. Reflection is dynamic access, so a wrong `T` is *input*, not a caller bug. |
| A3 | `ComponentRegistry` over-alignment cap (`ComponentRegistry.hpp:116-117`) | `alignof(T) > CACHE_LINE_SIZE` | All-config reject (match the paired `MAX_COMPONENTS` guard, already all-config). |
| A4 | `TypeContext` ID-space exhaustion (`TypeContext.hpp:70-86`) | `uint16` id-space wrap | All-config guard (mirror the ComponentRegistry pattern) — refuse rather than silently wrap-and-collide. |
| A5 | `ArchetypeManager` batch chunk-alloc OOM (`ArchetypeManager.hpp:1167`) | recoverable allocation failure | **Inverse fix:** stop `ASTRA_ASSERT(false, …)` from aborting Debug on a recoverable OOM; fail gracefully (skip/return) in all configs. |
| A6 | `Delegate` copy of a small move-only functor (`Delegate.hpp:52-274`) | stored functor is not copy-constructible | All-config: a copy of such a `Delegate` yields an **empty** delegate (`explicit operator bool() == false`, `m_invoker == nullptr`) rather than "valid but uninitialized." Reachable via `MulticastDelegate::Register(const DelegateType&)`. |
| A7 | `Result::operator*` (`Result.hpp:144-157`) | deref on an `Err` result | **Keep Release unchecked** (matches `std::expected::operator*`, which is UB-on-error). Add the **Debug-only** `ASTRA_ASSERT(m_hasValue, …)` its `operator->`/`As<T>()` siblings already have, so Debug catches misuse. Document "check `IsOk()` first." |

The sweep is grouped into cohesive tasks by subsystem (ResourceStorage; Reflection; Component/TypeContext registration; ArchetypeManager; Delegate) for implementation and review.

### B. Ordinary-Release correctness bugs

- **B1 — Root-archetype round-trip** (`ArchetypeManager.hpp:698,738-741,805-811`). Serialize the root (zero-component) archetype and repopulate it on Deserialize, so entities created with no components survive Save/Load instead of becoming dangling entity-map entries (which currently cause count underflow + OOB swap-and-pop on a later `DestroyEntity`). Plain data-loss/UB on **trusted** saves.
- **B2 — Large-component chunk fit-check** (`Archetype::Initialize`, `Archetype.hpp:157-161`). When a single entity's component footprint exceeds the usable chunk space, `Initialize` currently clamps `entitiesPerChunk` to 1 with no fit verification → Release heap overflow into the neighboring chunk. Fix: detect non-fit and fail gracefully (Initialize leaves the archetype uninitialized / the Deserialize path returns `Result::Err`), rather than clamping-and-overflowing.
- **B3 — `View::Size()` / `Empty()` UAF** (`View.hpp:143-156`). Both iterate `m_archetypes` without the refresh (`EnsureArchetypes`) that `ForEach`/`begin()` perform, so after `Defragment()` frees a cached archetype they dereference freed memory. Fix: refresh before reading, exactly as the iteration path does. (This was under-rated as a "stale-count caveat" in 3.4; it is a use-after-free.)
- **B4 — `CommandBuffer::Execute` partial rollback** (`CommandBuffer.hpp:773-784`). `RollbackAllocatedEntities` destroys entities via `EntityManager::Destroy` including ones already committed into an archetype earlier in the same `Execute()`, orphaning live archetype rows. Fix: track which recorded-created entities have been committed and roll back only the uncommitted remainder, per the documented contract.
- **B5 — `Registry::Clear()` orphans live Views/Relations** (`Registry.hpp:1004-1017`). `Clear()` swaps in a fresh `ArchetypeManager`, leaving every pre-existing `View`/`Relations` pointing at the old object and silently frozen on stale data forever. Fix: clear the existing `ArchetypeManager`'s contents in place and bump its change/removal counters, so cached views refresh to empty instead of freezing. (`IsValid()` cannot currently detect the orphaned state; in-place clear removes the failure mode.)

### C. Iteration-safety fixes (make the common patterns safe; document the hot one)

- **C1 — `MulticastDelegate::Invoke`** (`Delegate.hpp:391-412`). Dispatch over a stable snapshot (or a generation-guarded index) so a Signal handler that registers/unregisters itself during dispatch is safe (self-removing one-shot listeners are a normal pattern). Cold-ish path; a per-dispatch copy is acceptable.
- **C2 — `Relations::ForEachChild` / `ForEachDescendant` / `ForEachAncestor` / `ForEachLink`** (`Relations.hpp:138-241`, `RelationshipGraph.hpp` traversal). Snapshot the child/descendant/link set before the callback loop so the natural "destroy each child" callback (swap-and-pop / rehash mid-iteration) is safe.
- **C3 — `View` iteration** (`View.hpp:263-308`, `ViewIterator.hpp`). Do **not** snapshot (can't cheaply copy a whole result set on the hot path). Instead: document that structural mutation is not permitted during `ForEach`, and that the supported way to mutate structure while iterating is to **record changes into a `CommandBuffer` and `Execute()` after the loop** — the mechanism that already exists for exactly this. Add a Debug-only reentrancy/mutation assert. No Release cost.

## Testing & verification

TDD per fix. For every item whose condition is **forceable**, write a **Release-mode RED test first**, watch it fail (UB/crash/wrong-result), apply the fix, watch it pass:

- Reflection wrong-`T` `Get`/`Set`, const-field `Set`, const `GetPtr` (A2) — trivially forceable.
- `View::Size()`/`Empty()` after `Defragment()` (B3); zero-component `CreateEntity()` Save/Load round-trip (B1); oversized-component registration fit (B2); `Clear()`-then-use-a-cached-view (B5); CommandBuffer mid-Execute partial rollback (B4).
- Destroy-each-child (C2) and self-unregistering-handler-during-dispatch (C1) UAFs.
- `ResourceStorage` / `ComponentRegistry` / `TypeContext` exhaustion where reachable in a test (A1/A3/A4).

For the genuinely **un-forceable** conditions (true OOM, expired `weak_ptr`): fix by inspection, mirroring the established 3.4 pattern (the SetByID OOM fix), and note it in the task report — same policy 3.4 used for its OOM/defensive fixes.

**Gate for every task:** full suite passes in **Debug, Release, AND Dist**; `AstraCompile16`/`AstraCompile64` build in all configs. No new archive-format version (patch release).

## Mechanics

- **Execution:** `superpowers:subagent-driven-development`, exactly as 3.4 — one implementer subagent per task (TDD, atomic commit, self-review), a spec+quality review subagent after each task (fix Critical/Important, re-review), a progress ledger, and a final whole-branch review on the most capable model, ending in `superpowers:finishing-a-development-branch`.
- **Commit hygiene:** tracked source only (`include/`, `tests/`, `docs/`, `premake5.lua`); `ide/`/`Astra.sln` are gitignored — regenerate locally with `premake5 vs2022` after adding test files, do not `git add` them.
- **Task count:** ~10–13 (Theme-A sweep ≈ 4–5 tasks by subsystem; B1–B5 ≈ one task each; C1–C3 ≈ one or two tasks). The plan (writing-plans) will finalize the breakdown and ordering.

## Out of scope (deferred to later phases — tracked, not planned here)

- **Theme B — concurrency/thread-safety model** (illusory atomics, unsynchronized structural mutation, `ParallelForEach`, system scheduling). Needs a model decision first.
- **Theme C — `Registry::Load` trust boundary** (validate all stream-supplied counts/indices/IDs; non-overflowing bounds checks; allocation/decompression caps; a Load fuzz harness). The B1/B2 fixes touch adjacent code but only for the *trusted-round-trip* bugs, not adversarial hardening.
- **Theme D — alignment plumbing** (chunk-pool per-component alignment, SmallVector heap-grow alignment, SBO `alignas` vs `SBO_SIZE` on ARM64, CommandBuffer/Delegate SBO alignment on 32-bit).
- **Theme E — type-identity collision** for identically-named anonymous-namespace types (`TypeID::Hash`).
- **Theme H — portability** on non-default configs (32-bit `size_t` `hash >> 57`, bare-MSVC SIMD detection, non-standard entity version widths).
- **Theme J — Registry API footguns** (misleading `Registry(const&, Config)`, `void`-returning `CreateEntities` silent no-op, batch `ComponentAdded` `nullptr`, ById dead-handle filtering, tag-component signal drops, null-`View` ctor crash).
- Performance items (SplitHash mixing, `reserve` 2× over-allocation, per-chunk descriptor overhead), leaks on remove/move paths (Theme G), and test-coverage backfill beyond the Phase-1 fixes.

## Success criteria

1. Every Theme-A **condition** guard (A1–A6) rejects gracefully in Debug, Release, and Dist; invariants and the hot path are unchanged; A7 keeps Release-fast with a Debug assert added.
2. B1–B5 fixed, each with a Release test that goes RED before the fix and GREEN after (where forceable).
3. C1/C2 make self-unregister and destroy-each-child safe (tested); C3 documents the CommandBuffer-deferred pattern with a Debug reentrancy assert.
4. Full suite green in Debug + Release + Dist; no archive-format change; version bumped to 3.4.1.
5. Final whole-branch review clean (Critical/Important resolved); branch ready to merge to `dev`.
