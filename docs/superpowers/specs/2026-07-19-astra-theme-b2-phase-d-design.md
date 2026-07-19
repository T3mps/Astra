# Theme B2 — Phase D: Explicit Before/After Ordering + Ambiguity Detection — Design

> **Status:** approved (brainstorm 2026-07-19). Feeds `superpowers:writing-plans`.
> **Parent spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-concurrency-design.md` (the whole-B2 design; Phase D delivers its **§11 — explicit Before/After ordering** and **§12 — ambiguity detection**).
> **Builds on:** Phase A (deferred-command core) + Phase B (thread-safe registration + chunk-parallel deferral) + Phase C (resource access declaration + conflict analysis), merged to `dev` @ `ce75f6f`.

## Goal

Let a system declare hard execution ordering with `Before<T>` / `After<T>` traits, and have the scheduler **honor it by reordering** systems (not merely validating). Then — opt-in — **warn** when two systems that mutably conflict (share a component or a resource with a write on either side) have a relative order determined *only* by an insertion-order accident, with no explicit edge fixing it. This is the Bevy differentiator, adapted to Astra's contiguous-run grouping model. Phase D consumes Phase C's completed conflict predicate (component **and** resource) directly.

Today systems execute in registration (insertion) order, and any two conflicting systems are sequenced into separate parallel groups by that order. The order is deterministic but **accidental**: re-registering the same systems in a different order silently changes behavior. Phase D makes ordering *declarable* and makes the accidental-order case *detectable*.

## Scope

Full parent-spec §11 + §12:

1. New `Before<S...>` / `After<S...>` traits composing into `SystemTraits`; a runtime reorder (stable topological sort) in `BuildExecutionPlan` that honors the edges.
2. Deterministic, recoverable cycle handling (report + break + a `Result`-returning validation accessor).
3. Deferred-command apply order that follows the *execution* (schedule) order, not raw registration order, once the two diverge.
4. `AmbiguousWith<S...>` suppression trait + an opt-in ambiguity detector that reports (via the diagnostics seam) each conflicting-and-unordered system pair.

Everything else in the whole-B2 spec stays out of Phase D: sync-point barriers (§13, Phase E), lambda polish (§15, Phase F).

## Design decisions (brainstorm 2026-07-19, all user-approved)

1. **Reorder, not validate-only** — `Before`/`After` edges actually move systems via a stable topological sort (honor edges; break ties by insertion order so unconstrained systems keep today's order). This is the Bevy/flecs model and the honest feature, chosen over a validate-only variant that would make the traits a mere assertion.
2. **§11 and §12 ship together** in one Phase D — ambiguity detection reuses the very edge graph the ordering engine builds plus Phase C's conflict predicate, so they are one cohesive "declare and check ordering" feature.
3. **Deferred apply order follows execution order** — the deferred-command `SortKey`'s primary field is stamped from a new per-system **schedule rank** (topological position) instead of the raw insertion index. Backward-compatible by construction: with no edges the schedule rank equals the insertion order, so all existing determinism tests stay byte-identical.
4. **Cycles are recoverable + reported** (over fatal-in-Debug) — a `Before`/`After` cycle is a configuration error, not memory corruption, so it is handled `ENSURE`-style: reported through the diagnostics seam, broken deterministically so `Execute` still runs, and additionally surfaced through a `Result<void, SystemError>` validation accessor. Consistent with B1's uniform-graceful misuse policy and the assert seam's UE-5.8 model; also the only variant testable in all three configs (fatal-in-Debug would need a Debug-only death test).
5. **Edge-as-grouping-barrier** — an explicit ordering edge between two systems forces them into different parallel groups **even when their masks are disjoint** (an ordering edge demands serialization). This is the Bevy semantic.
6. **Separate `scheduleOrder` field** (not renumbering `insertionOrder`) — `insertionOrder` remains the stable registration index used as the topo tiebreak, the ambiguity-report identity, and diagnostics; `scheduleOrder` is the new sort-rank.
7. **Ambiguity reporting via `ASTRA_LOG_WARN`**, opt-in and off by default — it is a development aid, not an error.
8. **Trait-only API** — no runtime `.before()` builder (YAGNI, consistent with `Reads`/`Writes`/`Exclusive`/`ReadsResources`).

## Global constraints (unchanged from Phase A/B/C)

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT`. A cycle is surfaced as a value + a log, never a throw.
- **Additive, no break.** Existing `void(Registry&)` / `void(SystemContext&)` systems, view-lambdas, and `SystemTraits<Reads<...>, Writes<...>, ReadsResources<...>, Exclusive>` all keep compiling and scheduling identically. A system that declares no ordering traits behaves exactly as today (empty edge lists ⇒ topological order = insertion order = identity permutation). No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include. The traits live beside the existing `Reads`/`Writes` in `System.hpp`.
- Namespace `Astra`. All three build configs stay green.
- **TypeID ceiling is a non-issue for Phase D:** ordering edges reference *system* types, keyed by `TypeID<T>::Hash()` (the 64-bit key `m_systemIndices` already uses) — **not** `TypeID<T>::Value()`. So edges consume **zero** dense ComponentIDs; Phase D tests do not press the 128-ComponentID ceiling that constrained Phase C. (See `[[astra-flatmap-pointer-stability]]`.)

---

## Design

### Module A — Ordering traits (`System.hpp`)

Three new variadic traits, composing into `SystemTraits` beside `Reads`/`Writes`/`ReadsResources`/`WritesResources`/`Exclusive`, mirroring the Phase C trait pattern exactly:

```cpp
template<typename... Systems> struct Before        { using type = std::tuple<Systems...>; };
template<typename... Systems> struct After         { using type = std::tuple<Systems...>; };
template<typename... Systems> struct AmbiguousWith { using type = std::tuple<Systems...>; };
```

`SystemTraits<...>` gains `BeforeTypes` / `AfterTypes` / `AmbiguousWithTypes` (each a `tuple_cat` across the pack via `Detail::TraitBefore` / `Detail::TraitAfter` / `Detail::TraitAmbiguousWith`, mirroring the existing `TraitReads` / `TraitReadsResources`). Composable in any order:

```cpp
using MyTraits = SystemTraits<Writes<Position>, After<PhysicsSystem>, Before<RenderSystem>, AmbiguousWith<AudioSystem>>;
```

The `S...` reference **named system types** — structs that derive `SystemTraits`, or context-system structs. Lambda systems are anonymous types, so they can neither declare edges nor be edge *targets*; a documented, Bevy-consistent limitation (Bevy requires a system set / type to name in `.before()`).

### Module B — Metadata (`SystemMetadata.hpp`)

Add three resolved-edge id-lists plus the schedule rank:

```cpp
// Explicit ordering edges, resolved to the target systems' TypeID::Hash()
// (the same 64-bit key m_systemIndices uses). Filled by ExtractSystemTraits;
// resolved to indices and used to build the ordering DAG in BuildExecutionPlan.
SmallVector<uint64_t> beforeIds;
SmallVector<uint64_t> afterIds;
SmallVector<uint64_t> ambiguousWithIds;

// Position of this system in the topological execution order (filled by
// BuildExecutionPlan). Equals insertionOrder when no ordering edges exist.
// Primary key of the deferred-command SortKey (replaces insertionOrder there).
size_t scheduleOrder = 0;
```

`insertionOrder` is unchanged in meaning (stable registration index). `SmallVector` matches the codebase's small-container idiom; the lists are empty for the overwhelmingly common no-ordering case.

### Module C — Trait extraction (`SystemScheduler.hpp` `ExtractSystemTraits`)

`requires`-guarded (exactly like Phase C's resource extraction), fold each type in `BeforeTypes` / `AfterTypes` / `AmbiguousWithTypes` into the corresponding id-list by pushing `TypeID<Each>::Hash()`:

```cpp
if constexpr (requires { typename T::BeforeTypes; typename T::AfterTypes; typename T::AmbiguousWithTypes; })
{
    ExtractSystemIdList<typename T::BeforeTypes>(metadata.beforeIds);
    ExtractSystemIdList<typename T::AfterTypes>(metadata.afterIds);
    ExtractSystemIdList<typename T::AmbiguousWithTypes>(metadata.ambiguousWithIds);
}
```

`ExtractSystemIdList<Tuple>` is a small `index_sequence` fold that pushes `TypeID<std::tuple_element_t<Is, Tuple>>::Hash()` for each element — the resource-mask fold's structural twin, but writing hashes to a `SmallVector` instead of bits to a mask.

### Module D — Topological scheduling (`SystemScheduler.hpp` `BuildExecutionPlan`)

`m_systems` stays physically in registration order (so `m_systemIndices` and `RemoveSystem`'s renumbering are untouched). `BuildExecutionPlan` gains, before the existing grouping loop:

1. **Resolve edges to current indices** via `m_systemIndices`. Direction convention: `After<T>` on system `S` ⇒ edge `T → S` (T runs before S); `Before<T>` on `S` ⇒ edge `S → T` (S runs before T). An id that does not resolve (target system not registered in *this* scheduler) is **silently ignored** — legitimate conditional registration, not an error.
2. **Stable topological sort** — Kahn's algorithm with an insertion-order-ordered ready set (a system becomes ready when all its predecessors are placed; among simultaneously-ready systems, the lowest `insertionOrder` goes first). This produces `order[]`, a permutation of system indices that honors every edge and, among unconstrained systems, preserves registration order. With no edges it is the identity permutation.
3. **Cycle handling** — if Kahn's cannot place all systems, a cycle exists. Break it deterministically (drop the back-edge into the lowest-`insertionOrder` member of the remaining unplaced set, re-run, repeat until placed), record `m_scheduleHadCycle = true` (with the offending members for the log), and continue. The fallback order is fully deterministic and reproducible across rebuilds.
4. **Assign `scheduleOrder`** — `m_systems[order[k]].metadata.scheduleOrder = k`.
5. **Group over `order`** using Phase C's existing conflict/solo predicate (component + resource masks, `declaresAccess`, `Exclusive`), iterated in `order` sequence rather than raw registration order — **plus a new barrier term**: a candidate system cannot join the current run if it has an explicit ordering edge to/from any current-group member. Because `order` is topologically sorted, this reduces to: break the run at the candidate if any of its ordering *predecessors* is in the current group. Edge-related systems thus never share a parallel group, even with disjoint masks. Groups still store `m_systems` indices; `SystemExecutor` is unchanged.

The grouper remains O(n) over `order`; the topological sort is O(n + e) with e = resolved edges; both n and e are tiny (tens).

### Module E — Deferred apply order (`SortKey` primary = `scheduleOrder`)

The deferred-command `SortKey` is `(primary, iterationIndex, recordSequence)`. Phase D stamps `primary` from the executing system's `scheduleOrder` instead of its `insertionOrder`. The executor constructs each `SystemContext` with `scheduleOrder` (a one-site change to where the sort rank comes from). Consequences:

- When `Before`/`After` reorders systems, their deferred structural changes apply in the order the systems actually ran (execution order), matching intent.
- **Backward-compatible by construction:** with no ordering edges, `scheduleOrder == insertionOrder` for every system, so every stamped `SortKey` is byte-identical to today's — the Phase A/B determinism gates stay green unchanged.
- The Phase B iteration-index *banding* (`m_nextIterationBase`) is orthogonal (it lives in `iterationIndex`, the 2nd key field) and is unaffected.

### Module F — Cycle surfacing (`SystemScheduler`)

- New `SystemError::OrderingCycle`.
- `BuildExecutionPlan` stores the last build's cycle status (`m_scheduleHadCycle` + the involved member ids).
- New public `ASTRA_NODISCARD Result<void, SystemError> ValidateSchedule()` — forces a build if `m_needsRebuild`, then returns `Err(SystemError::OrderingCycle)` if the last build detected a cycle, else `Ok()`. This is the programmatic check (usable in tests and dev assertions).
- On detection, the diagnostics seam reports it: `ASTRA_LOG_ERROR` (or `ASTRA_ENSURE`) naming the cycle members and the dropped edge. `Execute` still runs with the deterministic fallback order and never aborts.

### Module G — Ambiguity detection (`SystemScheduler`, §12)

- Opt-in via `void SetAmbiguityReporting(bool)` (default **off**). A stored flag.
- After a build (only when the flag is on, and once per rebuild — not per `Execute`, so no per-frame spam), for every unordered pair of systems `(a, b)`:
  - **Conflict test** — reuse Phase C's predicate: they share a component **or** a resource with a write on either side. (Non-conflicting pairs are never ambiguous.)
  - **Unordered test** — neither reaches the other in the ordering DAG. Compute reachability over `order`'s resolved edges (transitive: `A before B before C` leaves `A`/`C` ordered, not ambiguous). A direct or transitive edge either way ⇒ ordered ⇒ not reported.
  - **Suppression** — if either declares `AmbiguousWith` the other (`a.ambiguousWithIds` contains `b`'s hash, or vice versa) ⇒ suppressed.
  - Otherwise ⇒ **report** via `ASTRA_LOG_WARN`, naming both systems (by `insertionOrder` / type id) and the specific conflicting component or resource.
- Cost: `O(n²)` pairwise × reachability, n = system count (tens) — trivial, and only paid when the flag is enabled.

**Why pairs are always sequential:** a conflicting pair always lands in different groups (the conflict breaks the contiguous run), so they already run sequentially. The "ambiguity" is that *which* runs first came from the topo/insertion accident, not a declared edge — exactly the latent order-dependence §12 targets.

---

## Prior art & positioning

Phase D is Bevy's model, expressed in Astra's trait vocabulary:

- **`Before`/`After` reorder + `AmbiguousWith` suppression + an ambiguity checker** is directly Bevy's `.before()` / `.after()` / `.ambiguous_with()` + its `ScheduleBuildSettings::ambiguity_detection`. Bevy is the only mainstream ECS that *reports* ambiguities — the differentiator Phase D brings to Astra.
- **flecs** has pipelines/phases for ordering but no ambiguity reporting; **Unreal Mass** has explicit processor ordering (`ExecuteBefore`/`ExecuteAfter`) but no ambiguity concept; **Unity DOTS** orders via `UpdateBefore`/`UpdateAfter` attributes + a runtime safety system (which throws on undeclared access, not on ambiguous order); **EnTT** has neither ordering nor analysis. Phase D therefore widens the B2 line's lead over EnTT and matches or exceeds the others on ordering diagnostics.
- **Reorder over validate-only** matches every mainstream ECS: none of Bevy/flecs/Unity/Mass require the user to hand-order registration to match declared intent.

## Boundaries & limitations

- **Advisory, opt-in ambiguity (dev aid).** The ambiguity report is off by default, never an error, and never blocks execution — consistent with Astra's advisory-masks stance (Phase C) and the "development aid" framing in the parent spec §12. It surfaces latent order-dependence; it does not prevent it.
- **Silent cycle degradation risk.** A cycle is logged and surfaced via `ValidateSchedule()`, but if nobody reads the log and nobody calls `ValidateSchedule()`, the scheduler *silently* falls back to a deterministic order. This is deliberately better than aborting, and the ambiguity report is a second net (the broken-edge systems, now unordered, re-surface there if they conflict).
- **Unknown edge targets are ignored, not flagged.** An edge to a system not registered in this scheduler adds no constraint. A typo'd target therefore silently does nothing; a Debug-only "unknown ordering target" log is a possible future hardening, **not** Phase D.
- **Lambda systems cannot participate in ordering** (anonymous types — no trait surface, not nameable as a target). Wrap ordering-sensitive logic in a named system struct.

## Out of scope (deferred follow-ons)

- **Automatic, access-derived sync-point insertion** (§13, Phase E) — Phase D does not touch barrier placement.
- **Lambda-system polish** (§15, Phase F).
- A runtime `.before()` / `.after()` builder form (trait-only for now).
- A Debug tripwire for unknown ordering targets.
- Reporting the ambiguity set programmatically (a structured accessor beyond the log) — the log channel is the v1; a `GetAmbiguities()` accessor is a possible follow-up if a tooling consumer appears.

## Testing

`tests/System/SystemSchedulerTest.cpp` (append — the existing file, so **no premake regen**), reusing named system structs and asserting on `GetExecutionPlan()` grouping / `ValidateSchedule()`, not wall-clock parallelism. Reuse existing component types where possible; ordering edges consume no ComponentIDs (Hash-keyed), so the TypeID ceiling is not a concern.

- **Reorder:** an `After<C>` on a system registered *before* `C` moves it after `C` (assert group order); `Before<T>` symmetric; unconstrained systems preserve insertion order; a no-edge schedule is unchanged.
- **Edge-as-barrier:** two systems with disjoint masks but an explicit edge land in different groups in the edge direction.
- **Cycle:** a 2- and a 3-system cycle ⇒ `ValidateSchedule()` returns `OrderingCycle`; `Execute` still runs; the fallback order is identical across repeated builds; the report fires.
- **Unknown target:** `After<Unregistered>` ⇒ ignored, `ValidateSchedule()` is `Ok`, no reorder.
- **Deferred apply order:** a reordered pair that defers overlapping ops on one real entity applies in execution order (schedule rank), and a no-edge schedule's deferred flush stays byte-identical to Phase C's.
- **Ambiguity:** fires for an unordered conflicting pair — one component case **and** one resource case (proving Phase C's predicate feeds it); silent when an explicit edge (direct **and** transitive) orders the pair; silent when `AmbiguousWith`-suppressed; silent when the flag is off.
- **Regression:** the full suite stays green; `void(Registry&)` and no-trait systems schedule identically; `Exclusive` still runs solo.

## To confirm during planning (grounding checks)

- Exact `SystemContext` construction site(s) in `SystemExecutor` where `insertionOrder` is threaded into the `SortKey`, to swap in `scheduleOrder` (Phase B wired this; confirm the single point).
- `SmallVector` include/availability in `SystemMetadata.hpp` (it already includes `Container/Bitmap.hpp`; confirm the small-vector header/path).
- The precise current `BuildExecutionPlan` grouping loop bounds after Phase C (`SystemScheduler.hpp` ~:461–508 on `dev` @ `ce75f6f`) to insert the topo pre-pass and the edge-barrier term cleanly.
- Whether `ValidateSchedule()` should also be what `Execute` consults internally (single build path) vs. a thin wrapper — pick the one that keeps a single `BuildExecutionPlan` entry point.
