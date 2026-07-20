# Theme B2 — Phase E+F: Sync-Point Barriers + Lambda Polish — Design

> **Status:** approved (brainstorm 2026-07-20). Feeds `superpowers:writing-plans`.
> **Parent spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-concurrency-design.md` (the whole-B2 design; this phase delivers its **§13 — sync-point placement** and **§15 — lambda-system polish**).
> **Builds on:** Phase A (deferred-command core) + Phase B (thread-safe registration + chunk-parallel deferral) + Phase C (resource access + conflict analysis) + Phase D (Before/After ordering + ambiguity detection), merged to `dev` @ `d7c9970`.

## Goal

Two independent, remaining B2 items, bundled into one work item because §15 is a small self-contained cleanup that does not warrant its own cycle and is orthogonal to §13:

- **§13 (Phase E) — Sync-point barriers.** Let the user insert an explicit `SyncPoint` fence between systems so a spawn-then-process-in-one-run pattern works: all deferred structural changes recorded before the fence are applied before the systems after it run. Today the scheduler flushes **once** at the `depth==0` sync point (after the whole run), so a system cannot see, in the same frame, entities an earlier system deferred-created.
- **§15 (Phase F) — Lambda polish.** Remove the vestigial, untested optional/pointer lambda-parameter path in `LambdaSystemWrapper` that mis-classifies a `const T*` param as a write and would build a malformed view; replace it with a `static_assert` that rejects non-reference component params.

The two modules touch disjoint code (E: `SystemScheduler` / `Execute` / command-flush; F: `LambdaSystemWrapper` trait inference in `System.hpp`) and have no dependency on each other; per-task reviews keep them cleanly gated.

## Scope

**Phase E (§13):**
1. A `SyncPoint` registration-time fence: a new `AddSyncPoint()` method that partitions the registered systems into ordered **segments**.
2. Each system carries a `segmentIndex`; `BuildExecutionPlan` groups within a segment only (a group never spans a segment), and reorders (Phase D) using only intra-segment edges.
3. `Execute` flushes deferred commands at each segment boundary (mid-run), not only at `depth==0`; the final segment's flush is today's `depth==0` flush.

**Phase F (§15):**
4. A `static_assert` in `LambdaSystemWrapper` rejecting any non-reference component parameter (pointer/value/optional), removing the mis-inferring `const T*` path.

**Out of scope (deferred, per parent spec):** automatic access-derived sync-point insertion (Bevy 0.13 — requires the full access graph, least predictable; explicit barriers + single-at-end cover the correctness need for v1); an `Optional<T>` lambda-parameter feature (F removes the vestigial path rather than completing it — YAGNI); the per-invocation `View`-rebuild perf optimization (noted, not fixed).

## Global constraints (unchanged from Phase A–D)

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT`.
- **Additive, no break.** Existing `void(Registry&)` / `void(SystemContext&)` systems, view-lambdas, and all `SystemTraits<...>` compositions keep compiling and scheduling identically. **With no `SyncPoint`, behavior is byte-identical to today** (one segment = the whole schedule = one flush at end). No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include.
- Namespace `Astra`. All three build configs stay green.

---

## Design

### Phase E — Sync-point barriers

#### Module 1 — The `SyncPoint` API (`SystemScheduler`)

```cpp
ASTRA_NODISCARD Result<void, SystemError> AddSyncPoint();
```

- Records a fence at the current end of the registration sequence — it sits between the systems registered before it and those registered after. Positional and no-arg (chosen over a named barrier or a per-system tag: it matches Astra's positional registration model and Bevy's `apply_deferred`/set-boundary style).
- Fails only with `SystemError::SchedulerExecuting` if called during `Execute` (uniform-graceful policy, same as `AddSystem`). Sets `m_needsRebuild`.
- Consecutive `AddSyncPoint()`s, or one before any system, produce an **empty segment** — harmless: its flush is a no-op (nothing was recorded).
- Implementation note: a fence is recorded as the count of systems registered so far (a boundary position), or equivalently each subsequently-registered system's `segmentIndex` is incremented. `RemoveSystem` / `Clear` keep the fence bookkeeping consistent (the plan will specify; likely the simplest representation is a running `m_currentSegment` counter stamped into each system's metadata at registration, plus a segment count).

#### Module 2 — Segment model (`SystemMetadata` + `BuildExecutionPlan`)

- `SystemMetadata` gains `size_t segmentIndex` (the number of `SyncPoint`s registered before this system).
- `BuildExecutionPlan` partitions the systems by `segmentIndex` and, **within each segment**, runs Phase D's existing pipeline unchanged — `ComputeScheduleOrder` (stable topological reorder using only intra-segment edges) + the contiguous-run grouper (mask conflict, resource conflict, `Exclusive`, edge-barrier). A group **never spans a segment boundary**.
- `scheduleOrder` becomes **segment-major**: every system in segment *k* ranks below every system in segment *k+1*. Within a segment, the Phase D topological rank orders them. (So the deferred `SortKey` primary still increases monotonically with actual execution order across the whole run — segments first, topo rank within.)
- The execution plan is now a **sequence of segments, each a list of parallel groups**. Representation and the `GetExecutionPlan()` accessor are a plan-level decision (keep a flattened `vector<vector<size_t>>` for backward-compat with existing grouping tests **and** expose segment boundaries — e.g. a `GetSegmentCount()` / `GetExecutionSegments()` — for the new tests; the plan will pick the least-disruptive shape).

#### Module 3 — Cross-segment ordering edges

An ordering edge (`Before`/`After`) to a system in a **different** segment does **not** reorder: the fence already fixes cross-segment order absolutely, so only **intra-segment** edges feed `ComputeScheduleOrder`. A cross-segment edge is therefore ignored for scheduling (a consistent one is redundant; a contradicting one — demanding reverse segment order — cannot be honored because a fence is absolute). *A Debug-only warn on a contradicting cross-segment edge is a possible future enhancement; it is **not** v1.*

#### Module 4 — Mid-run flush (`SystemScheduler::Execute`)

Today `Execute` builds one `SystemExecutionContext` with all groups, calls `executor->Execute(context)` once, then flushes `m_commandBuffer->ExecuteSorted()` + `Clear()` at `depth==0`. Phase E generalizes this to **one flush per segment**:

```
for each segment s in order:
    context.parallelGroups = s.groups     // that segment's groups (referencing original system indices)
    executor->Execute(context)            // Sequential/ParallelExecutor UNCHANGED — runs the groups it is handed
    flush: m_commandBuffer->ExecuteSorted()   // applies s's deferred commands in SortKey order
    accumulate m_lastDeferredErrors += GetDeferredErrors()
    m_commandBuffer->Clear()
```

- The **executor is unchanged** — it runs whatever groups it is given; the scheduler orchestrates the segment loop and the between-segment flushes.
- **Placeholder resolution** (Phase A) happens at each segment's flush, so segment *k+1* sees the real entities segment *k* deferred-created — the spawn-then-process guarantee.
- **Determinism** is preserved: within a segment, `ExecuteSorted` applies in `SortKey` order exactly as today; across segments, the segment order is absolute. `scheduleOrder`'s segment-major assignment keeps the sort key monotonic with execution order.
- The **error channel** accumulates: `m_lastDeferredErrors` is cleared once at `Execute` start (as today) and appended-to after each segment flush (today it is assigned once; the plan changes it to accumulate).
- The `ExecutionGuard` depth semantics are unchanged (still about reentrant `Execute`); the intermediate flushes happen while `depth==1`, and the final flush still coincides with `depth` returning to 0. (The plan will confirm the guard scoping still reads cleanly with intermediate flushes; the flushes are driven by the segment loop, not the depth counter.)

#### Module 5 — Determinism / backward-compat

With **no** `SyncPoint`, there is exactly one segment spanning all systems, so: one group-partition (identical to Phase D), one flush at the end (identical to today), and `segmentIndex == 0` for every system (so `scheduleOrder` is exactly Phase D's). **Byte-identical to `dev` @ `d7c9970`.** Every Phase A–D test — including the determinism gates — stays green unchanged. The new behavior is reachable only by calling `AddSyncPoint()`.

### Phase F — Lambda polish (`System.hpp` `LambdaSystemWrapper`)

The wrapper infers each component parameter's read/write access from const-ness:
- `IsReadOnly<T> = std::is_const_v<std::remove_reference_t<T>>`
- `BaseType<T> = std::remove_const_t<std::remove_reference_t<T>>`

For a **reference** param this is correct (`const Position&` → read; `Position&` → write). For a **pointer** param it is wrong: `std::remove_reference_t<const Position*>` is `const Position*` (a pointer, no reference to strip), `std::is_const_v<const Position*>` is `false` (the *pointer* is not const), so a `const Position*` is mis-classified as a **write**, and `BaseType` yields `const Position*` (top-level `remove_const` does not strip the pointee's const), producing a malformed `CreateView<const Position*>`. No shipping system uses pointer params, so this path is vestigial and untested.

**Fix:** add a `static_assert` in `LambdaSystemWrapper` that each component parameter (every parameter after the leading `Entity`) is an **lvalue reference** (`T&` or `const T&`), rejecting pointers, by-value, and optional/nullable forms with a clear diagnostic (e.g. *"System lambda component parameters must be `T&` or `const T&`; pointer/by-value/optional parameters are not supported"*). This removes the mis-inferring path by construction. No `Optional<T>` feature is implemented (YAGNI — the reference path is correct and tested-by-use).

---

## Prior art & positioning

- **Explicit `SyncPoint` fence** matches **Bevy**'s `apply_deferred` / system-set sync points and **flecs**'s explicit `ecs_defer_end` boundaries. Astra keeps the default single-flush-at-end (cheapest) and adds explicit fences for the spawn-then-process case, deferring Bevy-0.13-style *automatic* access-derived barrier insertion — the same v1 scope choice the parent spec §13 records. **Unreal Mass** applies command buffers at fixed processing-phase boundaries (a coarser, phase-based version of the same idea). **EnTT** has no scheduler, so no concept here.
- **Rejecting non-reference lambda params** matches every mainstream ECS view-iteration API (Bevy queries, flecs `each`, EnTT `view::each`) — all take references; none support a nullable/pointer component param in the basic iteration path. Removing Astra's half-built pointer path aligns it with the norm.

## Boundaries & limitations

- **A `SyncPoint` is a hard serial point** (documented cost): the scheduler fully flushes and applies all pending structural changes at each fence before the next segment starts, so barriers reduce available parallelism. Use them only where the same-frame spawn-then-process ordering is actually needed.
- **Cross-segment ordering edges are silently ignored** for scheduling (Module 3). A contradicting cross-segment edge is not diagnosed in v1.
- **Advisory access masks are unchanged** (Phase C/D stance): a `SyncPoint` flushes *structural* deferred changes; it does not add any access verification.

## Testing

`tests/System/SystemSchedulerTest.cpp` (append — existing file, **no premake regen**). Reuse existing component types and the `TestWorkerPool` harness where a real executor is needed. Ordering edges are Hash-keyed (no ComponentID pressure); introduce at most one or two fresh component types if genuinely needed (mind the 128-TypeID ceiling — see `[[astra-flatmap-pointer-stability]]`).

**Phase E:**
- **Segment partition:** `AddSystem … AddSyncPoint() … AddSystem` ⇒ groups never span the fence; assert the plan/segment structure (systems before the fence are in earlier segment(s) than those after).
- **Spawn-then-process (the core acceptance):** a segment-0 context system defers creating entities; a segment-1 system reads them via a view — after `Execute`, the segment-1 system observed the created entities **the same frame** (assert via an effect the segment-1 system records, or the final world state), which it would NOT with a single end-of-run flush.
- **Intra-segment reorder still works** across a fence (Phase D edges within a segment reorder; a cross-segment edge does not reorder — fence dominates).
- **Empty segment** (two consecutive `AddSyncPoint()`s, or one before any system) is a harmless no-op flush.
- **Determinism / backward-compat:** with no `SyncPoint`, the plan + deferred apply order are byte-identical to Phase D (the existing determinism gates stay green); a barriered schedule that defers structural changes is byte-identical across repeated runs under `TestWorkerPool`.
- **Error channel:** a deferred error in an early segment is surfaced through `GetLastDeferredErrors()` and does not abort later segments.

**Phase F:**
- A view-lambda with `const T&` / `T&` params still compiles and schedules (read/write inference unchanged) — regression.
- A lambda with a pointer/by-value component param fails to compile with the `static_assert` message. (A `static_assert` failure is a *compile* test — verify by a temporarily-added TU or a documented compile-fail check; the plan will specify how to demonstrate RED without leaving a non-compiling test in the suite.)

## Design decisions (brainstorm 2026-07-20, all user-approved)

1. **E and F bundled into one work item** — F is a small self-contained YAGNI removal orthogonal to E; per-task SDD reviews keep them separately gated.
2. **`SyncPoint` is a FENCE** (partitions the schedule into ordered segments; reorder within a segment, never across) — the only coherent model once Phase D reorder exists; over a flush-only marker that leaves "before/after the barrier" ambiguous.
3. **Positional no-arg `AddSyncPoint()`** — matches the positional registration model; over a named barrier or a per-system tag.
4. **Cross-segment ordering edges silently ignored** (fence dominates); a contradicting-edge Debug warn is a deferred enhancement.
5. **Executor unchanged; the scheduler orchestrates the segment loop + between-segment flushes** — keeps `ISystemExecutor` simple.
6. **`scheduleOrder` is segment-major** so the deferred `SortKey` primary stays monotonic with execution order and per-segment `ExecuteSorted` remains correct.
7. **F removes the pointer/optional path via `static_assert`** (rejecting non-reference component params) — over implementing `Optional<T>` end-to-end (YAGNI).
8. **Auto access-derived barrier insertion + the `View`-rebuild perf item stay deferred** (parent spec §13/§15).
