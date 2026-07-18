# Theme B2 — Phase B: Thread-Safe Registration + Chunk-Parallel Deferral — Design

> **Status:** approved (brainstorm 2026-07-17). Feeds `superpowers:writing-plans`.
> **Parent spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-concurrency-design.md` (the whole-B2 design; Phase B delivers its **§9 chunk-parallel iteration + sort-key stamping** module, plus a **new registration-safety module** discovered during Phase A execution).
> **Builds on:** Phase A (deferred-command core), merged to `dev` @ `c2b53b2` — composite `SortKey{insertionOrder, iterationIndex, recordSequence}`, `ParallelCommandBuffer::ExecuteSorted()`, `void(SystemContext&)` systems, depth==0 flush, per-system error channel, worker-safe placeholder entities.

## Goal

Let a system iterate a view **across worker threads by chunk** and defer structural changes safely and deterministically from each chunk — `ctx.ParallelForEach(view, [](Entity, A&, const B&, SystemContext& sub){ sub.Commands()... })` — with per-chunk commands tagged by `chunkIndex` so the flush recovers a deterministic apply order. This is the flecs/Bevy/Unity-DOTS-class capability that makes B2 a *complete* parallel scheduler. It is unsafe until component registration is worker-safe, so Phase B ships that fix first.

## Scope

Two modules, in order:

1. **Thread-safe component registration** (prerequisite; hardens the current system-level parallel path too).
2. **Chunk-parallel deferral** (parent-spec §9).

Everything else in the whole-B2 spec stays out of Phase B: resources (§10), explicit ordering (§11), ambiguity detection (§12), explicit barriers (§13), lambda polish (§15) — each a later phase.

## Global constraints (unchanged from Phase A)

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT` (compiles out under `NDEBUG`). Mutexes are permitted (already used in `ParallelCommandBuffer`); a lock-acquisition failure is catastrophic, like OOM.
- **Additive, no break.** `void(Registry&)` systems, view-lambdas, the standalone eager `CommandBuffer`, and Phase A's `void(SystemContext&)` + `Commands()` path all keep working unchanged. Phase A's determinism gate and error channel stay green.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no `System/` or `Commands/` include.
- **Determinism is a contract** (parent-spec §14): the applied order of deferred structural changes, and placeholder id assignment, are identical across runs independent of thread count/scheduling. Phase B extends this to the chunk-parallel path.
- Everything opt-in via `IWorkScheduler`; a null scheduler ⇒ sequential inline fallback. Astra spawns no threads.

---

## Module 1 — Thread-safe component registration

### Problem

`ComponentRegistry::RegisterComponent<T>()` (`include/Astra/Component/ComponentRegistry.hpp:23`) reads `m_components.Contains(id)` then, on a miss, writes three shared containers in `RegisterComponentImpl`: the `m_components` `FlatMap`, the `m_hashToID` `FlatMap`, and the `m_componentNames` `std::deque`. None of this is synchronized. `CommandBuffer::AddComponent<T>` calls `RegisterComponent<T>()` at **record** time, so two systems on different worker threads each first-registering a **different** component type concurrently is a data race (map rehash/resize under a concurrent reader → heap corruption); the **same** type concurrently races too. Chunk-parallel (Module 2) makes this pervasive — one system's body now records across many workers at once.

Phase A shipped a doc-only note ("register a type before first adding it from concurrent systems"); all Phase A determinism tests pre-register on the main thread to dodge it. Module 1 removes the footgun.

### Design — a first-registration guard

Localized to `ComponentRegistry`:

- Add a per-component-id **atomic "registered" flag** (an array of `std::atomic<bool>` / an atomic bitset sized `ASTRA_MAX_COMPONENTS`) and a `std::mutex`.
- `RegisterComponent<T>()`:
  - **Warm path (lock-free):** atomic-load the flag for `id = TypeID<T>::Value()`; if set (acquire), return. This **replaces** the current `m_components.Contains(id)` hashmap lookup — a cheaper check and, crucially, one that does not read the `FlatMap` while another thread may be writing it.
  - **Cold path (first registration of this type):** take the mutex, re-check the flag under the lock (double-checked), run `RegisterComponentImpl<T>(id)`, then store the flag with release ordering. The lock is taken at most once per type per process; after warm-up it is never contended.
- `RegisterComponentImpl` itself is unchanged; it just runs under the mutex on the cold path.
- **Visibility:** all registrations performed during the parallel phase are published (release) before the single-threaded flush reads descriptors (acquire) via `GetComponentDescriptor` at apply time. During the parallel phase the only `ComponentRegistry` mutation is registration (now guarded); read-only view iteration and component-value reads do not touch the registry `FlatMap`.
- `ReRegisterComponent` (hot-reload path) keeps its unconditional-rebuild semantics but must also take the mutex to avoid racing a concurrent `RegisterComponent`.

### Copyability check (confirm in the plan)

A `std::mutex` / `std::atomic` array makes `ComponentRegistry` non-copyable and non-movable. **The plan must confirm `ComponentRegistry` is not required to be copied/moved** (grep for copies of `ComponentRegistry` / the `Registry` copy path — this intersects the known Theme-J "`Registry(const&, Config)` is not a real copy" item). If a copy is genuinely required, provide a hand-written copy that value-copies the containers and default-initializes the sync primitives (a fresh registry's flags reflect its copied contents). Prefer confirming no copy is needed.

### Test

`tests/Component/ComponentRegistryConcurrencyTest.cpp` (**new** → premake regen): N workers under the real `TestWorkerPool` concurrently register M distinct component types **and** repeatedly register the same type; assert no crash, each type registered exactly once, and every descriptor is intact (id/size/alignment/function-pointers correct). Loop enough iterations to make a regression to the unguarded version reliably crash.

---

## Module 2 — Chunk-parallel deferral (parent-spec §9)

### API (spec-approved)

```cpp
ctx.ParallelForEach(view, [](Entity e, Position& p, const Velocity& v, SystemContext& sub) {
    // in-place value mutation of p is chunk-local and safe;
    sub.Commands().AddComponent<Trail>(e, ...);   // deferred structural change, worker-safe
});
```

The user body receives `(Entity, components..., SystemContext& sub)` as the trailing parameter; `sub.iterationIndex == chunkIndex`.

### Mechanism

- `SystemContext::ParallelForEach(view, func)` wraps the existing `View::ParallelForEach` (`include/Astra/Registry/View.hpp:115`), which already splits iteration across workers by chunk and exposes `chunkIndex` (`View.hpp:167-173`).
- **Per-chunk sub-context.** Inside each chunk's worker execution, construct a `SystemContext` sub-context: same `insertionOrder` as the parent system, `iterationIndex = chunkIndex`, its own `recordSequence` starting at 0, and its `CommandBuffer&` = the chunk-worker's buffer via `parallelBuffer.GetThreadBuffer()` **called on the chunk-worker thread** (the exact pattern Phase A's executor uses one level up). Recorded commands are stamped `(insertionOrder, chunkIndex, recordSequence)`.
- **Determinism.** Keys stay globally unique — `insertionOrder` per system, `chunkIndex` per chunk, `recordSequence` monotonic within a chunk — so the Phase A `std::stable_sort` in `ExecuteSorted` recovers a single deterministic apply order regardless of which worker processed which chunk. This is exactly the purpose of the `iterationIndex` field reserved in Phase A. Placeholder resolution and the per-system error channel compose unchanged (attribution is by `insertionOrder`, still the parent system).

### SystemContext exposure — additive (design decision, option 2)

Phase A's `SystemContext` holds `CommandBuffer& m_commands` (the parent worker's buffer, resolved once at construction) and its `Commands()` path stays **byte-for-byte unchanged**. Phase B **adds** a nullable `ParallelCommandBuffer* m_parallelBuffer`, read **only** by `ParallelForEach` to construct per-chunk sub-contexts. For a non-chunk system the pointer is dormant. The executor already holds the `ParallelCommandBuffer` (it constructs the `SystemContext`), so it passes it in. Each per-chunk sub-context is itself an ordinary `SystemContext` holding its chunk-worker's `CommandBuffer&` — so sub-contexts use the same fixed-ref model as Phase A. (Rejected alternatives: replacing the ref with the pointer and resolving per-call — disturbs Phase A's reviewed `Commands()` path; a separate `ChunkContext` type — splits the recording API and still needs the pointer reachable.)

### Nesting behavior

`ParallelForEach` is called from inside a running system body, which may itself be executing on a worker inside a multi-member parallel group. When already inside a worker, the nested `ParallelFor` runs **inline** (the scheduler/`TestWorkerPool` inside-worker guard prevents deadlock) — it still stamps `chunkIndex` and stays fully deterministic; only the degree of additional parallelism drops. Chunk-parallelism fans out fully when the system runs solo. **The plan must confirm the exact nested-`ParallelFor` behavior of the live `IWorkScheduler`/`TestWorkerPool`** (inline vs re-entrant dispatch) and that a null scheduler runs the body inline over all chunks in order.

### Value mutation coexistence

A chunk body may both mutate component values in place (the `Position&`/`Velocity&` params — chunk-local, governed by the system's declared access + grouping, exactly as `View::ForEach` today) **and** defer structural changes via `sub.Commands()`. The two are orthogonal: value writes are immediate and chunk-local; structural changes are deferred to the sorted flush. If threading the sub-context through the existing `ParallelForEach` value-mutation path needs glue, that glue is in-scope (not an iteration rewrite), per parent-spec §9.

### Test

`tests/System/SystemContextTest.cpp` (append):
1. **Correctness:** a `void(SystemContext&)` system calls `ctx.ParallelForEach` over a large multi-chunk view and defers a structural change per entity (e.g. add a tag / create a related entity); after `Execute`, assert every intended change landed.
2. **Chunk-parallel determinism gate:** run that system N times from identical initial state under the real multi-threaded `TestWorkerPool`; assert a byte-identical world snapshot (entity ids + component values, sorted) every run — the Phase A 50-run gate pattern, extended to the chunk-parallel path. Pre-register components (Module 1 also removes the need, but keep the pattern). A `chunkIndex`-stamping or sub-context regression makes it flake.

---

## Determinism contract (what Phase B adds)

Phase A's contract now holds for **chunk-parallel** deferral too: given the same initial world, systems, and input, the applied order of all deferred structural changes and placeholder id assignment is identical across runs, independent of thread count and which worker processed which chunk — because every recorded command carries a globally-unique `(insertionOrder, chunkIndex, recordSequence)` key and the flush is a single-threaded stable-sorted apply. Component *value* writes remain in-place and are governed by declared access + grouping, not by this order.

## Testing summary

- Module 1: concurrent-registration stress test (new file → premake regen).
- Module 2: chunk-parallel correctness test + N-run chunk-parallel determinism gate (append to `SystemContextTest.cpp`).
- All three build configs (Debug/Release/Dist) stay green; the Phase A determinism gate and error-channel tests keep passing. Baseline entering Phase B: **611 Debug / 609 Release+Dist**.

## Out of scope (later phases)

Resources in conflict analysis (§10), explicit `Before`/`After` (§11), ambiguity detection (§12), explicit sync-point barriers (§13), lambda-system polish (§15). Also deferred: automatic access-derived barrier insertion; the structural "register-on-flush" alternative to Module 1 (thread-safe registration was chosen instead).

## Design decisions (brainstorm 2026-07-17)

1. **Scope:** race fix + chunk-parallel as **one** work item — the race is a prerequisite for safe chunk-parallel recording (user-approved).
2. **Race fix approach:** thread-safe lazy registration (atomic-flag warm path + mutex on first registration) — universal, localized, robust for any concurrent path; chosen over register-on-flush thunks and trait-declared pre-registration (user-approved).
3. **SystemContext exposure:** additive nullable `ParallelCommandBuffer*` used only by `ParallelForEach`; Phase A's `CommandBuffer&`/`Commands()` untouched (user-approved, option 2 over replace/separate-type).
4. **Nesting:** `ParallelForEach` from a system already on a worker runs inline; deterministic regardless.
5. **Confirm-against-live-code (for the plan):** `ComponentRegistry` copyability; the live `IWorkScheduler`/`TestWorkerPool` nested-`ParallelFor` behavior; the exact `View::ParallelForEach` value-mutation signature and how cleanly the sub-context threads through it (parent-spec §19 grounding checks).
