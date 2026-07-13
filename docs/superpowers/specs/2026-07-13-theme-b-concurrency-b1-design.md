# Theme B — Concurrency & Systems, Part B1 (Honest Scheduler) — Design Spec

**Date:** 2026-07-13
**Status:** Approved (design)
**Origin:** Theme B / must-fix item 9 of the 2026-07-11 full-codebase review (`docs/reviews/2026-07-11-astra-full-review.md`, §"Concurrency & Systems"), Critical C1 + Important I1–I5 + the subsystem minors. Review §7 Phase 3: "commit to a threading model, document it, enforce/de-advertise accordingly."

## Threading-model decision (the frame for everything below)

Astra is **single-threaded by default with zero out-of-the-box thread-safety guarantees**, and its concurrency story is a **clean, wide-open seam** the host owns — the EnTT philosophy. Where Astra goes *beyond* a pure seam is the built-in `SystemScheduler`/`ParallelExecutor`, which auto-groups systems by component mask and runs them concurrently against a shared `Registry`. That auto-decision is exactly where C1's over-promise lives.

The chosen direction is **keep the built-in scheduler as an honest, opt-in convenience** (the flecs/Bevy-class "batteries-included but replaceable" differentiator over EnTT), delivered in two committed steps:

- **B1 (this spec):** make the built-in scheduler *honest and safe* with **no breaking API change**, and shape every API choice so B2 is *additive*. Stops the corruption; documents the real contract.
- **B2 (separate later cycle):** the differentiator — wire the already-built `ParallelCommandBuffer` into the executor via a `SystemContext`, so structural changes defer and merge at sync points and structural systems parallelize safely. Expand access sets (resources/out-of-band, à la Unreal Mass `ProcessorRequirements`). Own brainstorm→spec→plan.

### Layering the decision preserves (pay-for-what-you-use)

Three orthogonal, independently-replaceable layers plus one shared building block:

1. **Job-system seam — `IWorkScheduler`** ("how/where work runs"). Host-provided (e.g. enkiTS adapter). The *only* thing that touches real threads. Inject none → everything runs sequentially inline, zero thread/sync overhead.
2. **Execution/dispatch — `ISystemExecutor`** (`SequentialExecutor`/`ParallelExecutor`): glue between a plan and the seam. Replaceable.
3. **Scheduling policy — `SystemScheduler`** ("what may run together"): our built-in. Fully replaceable, or unused.
4. **Shared building block — `CommandBuffer`/`ParallelCommandBuffer`**: usable by our scheduler, the host's, or standalone.

The job-system seam is decoupled from the policy layer, so a host writes its enkiTS `IWorkScheduler` adapter **once** and it drives *either* our stack *or* the host's own loop, with the same command buffer. Include hygiene already enforces pay-for-what-you-use: `Registry.hpp` includes neither `System/` nor `Commands/` (verified) — dependency is strictly `System`/`Commands` → `Registry`, never the reverse — so a user who only touches `Registry` never compiles the scheduler.

---

## Problem (what B1 fixes)

`ParallelExecutor::Execute` (`include/Astra/System/SystemExecutor.hpp:39-60`) runs every system in a `parallelGroup` concurrently against the same live `Registry&`. A group is admitted purely on disjoint component read/write masks (`SystemScheduler.hpp:293-311, 346-365`). Because SoA storage keeps each component's array separate and the archetype vector stable, two *pure* mask-disjoint systems are genuinely safe against each other. The hazard is narrow but real and un-guarded:

- **C1 (Critical / must-fix 9):** a mask cannot express a **structural change** (create/destroy entity, add/remove component) or **out-of-band access** (`registry.Get<Other>()`, a resource). A grouped system doing either races the archetype vector realloc, torn `m_generation` reads, and half-moved entity data → corruption. Structural mutation inside systems is common; the only safety signal is the traits, and the layer offers no enforcement, no wired command buffer, not even a warning.
- **I1:** `ExecutionGuard`/`m_isExecuting` (`SystemScheduler.hpp:27-48`) is a check-then-act (TOCTOU), not exclusion; the `std::atomic<bool>` + "prevents use-after-free" comment advertise a guarantee the code doesn't provide.
- **I2:** the guard is a bool, not a counter: a reentrant `Execute()` clears the flag on the inner return while the outer is still iterating (`SystemScheduler.hpp:34-37, 162`).
- **I3:** greedy grouping reorders independent systems relative to insertion order, visible even under `SequentialExecutor` (`S0:wA, S1:wA, S2:wB` → `[[S0,S2],[S1]]`), silently breaking any implicit (resource/event/side-effect) ordering.
- **I4:** `IWorkScheduler`'s contract (`Core/WorkScheduler.hpp:25-32`) specifies only the *backward* happens-before edge; every parallel path also relies on the unspecified *forward* edge.
- **I5 (registration half):** `AddSystem` does raw `new T(...)` and returns `void` — a failed alloc under `-fno-exceptions` terminates or stores null, and duplicate / during-execution rejections are silent.
- **Minors:** `IsReadOnly` misclassifies `const T*` optionals as writes (`System.hpp:78`); systems keyed by 64-bit `TypeID::Hash()` collide silently (`SystemScheduler.hpp:57`); `metadata.insertionOrder` goes stale after `RemoveSystem` (`:126-136`).

Zero dedicated concurrency/scheduler tests exist today.

---

## Section 1 — Safety & enforcement model (C1)

**Principle:** declared component masks are a **promise of purity**; the scheduler *trusts the promise and ships a Debug tripwire*. This is the honest EnTT-style contract, and it matches how the mature engines split the concern (Bevy derives exclusivity from `&mut World`; Unreal Mass declares it via `bRequiresGameThreadExecution`; both defer structural changes to a command buffer).

**Scheduling by system class:**

| System | Registry access | Default | Escape |
|---|---|---|---|
| Declares `SystemTraits<Reads<…>, Writes<…>>` (class or lambda) | full `Registry&` (masks are an unverified promise) | parallel-eligible | mark **`Exclusive`** → solo group |
| No traits | unknown | solo group (already conservative) | — |
| `Exclusive` | full | solo group (nothing runs concurrently with it) | — |

**The `Exclusive` marker** — the single new piece of surface. A composable tag added to the `SystemTraits<…>` pack (the industry-aligned home: typed, cohesive with access declaration, extensible for B2), matching the existing inheritance-based declaration form (`struct MoveSystem : Astra::SystemTraits<Reads<Velocity>, Writes<Position>> { … }`, per `benchmark/Benchmark.cpp:52`):

```cpp
struct SpawnSystem : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>
{
    void operator()(Astra::Registry& registry);  // may create/destroy/add/remove freely — runs solo
};
```

The `SystemTraits` specialization parses the pack, extracts `Reads`/`Writes` as today, and surfaces `static constexpr bool RequiresExclusive` (true iff an `Exclusive` tag is present). The scheduler reads that flag; a direct `static constexpr bool RequiresExclusive = true;` member is *also* sniffed as an ergonomic alias (covers Mass-familiar users and systems not using `SystemTraits` inheritance). Mechanically it produces the same *effect* the no-trait path already produces (solo group), but must be keyed off `RequiresExclusive` — **not** off empty masks — because an exclusive system may carry non-empty masks (`Writes<Position>, Exclusive`). So the flag is threaded through `HasConflict` (an exclusive system conflicts with everything) and the group-accept logic (`groupAcceptsMore = false` when the group's opener is exclusive; an exclusive `j` never joins an existing group) ⇒ it lands alone. Adjacent groups still parallelize; because groups are sequential barriers, nothing runs concurrently with an exclusive system.

*Note — the `SystemTraits` change is the most intricate part of B1:* today's fixed specializations (`SystemTraits<Reads…, Writes…>` / `<Reads…>` / `<Writes…>`) must become a pack-scan that detects and strips an `Exclusive` tag in any position before matching `Reads`/`Writes`, while still surfacing `ReadsComponents`/`WritesComponents`/`HasTraits` so `HasSystemTraits_v` keeps working (incl. an `Exclusive`-only, mask-less form).

**Enforcement tripwire (Debug only, zero Release cost).** In the built-in `ParallelExecutor`, snapshot the archetype structural-change counter immediately before dispatching a *multi-member* group (i.e. before the `ParallelFor` call) and immediately after its barrier (after `ParallelFor` returns — the backward happens-before edge makes every worker's increments visible). If it changed, some grouped system performed an undeclared structural change → `ASTRA_ASSERT` naming the group. Deferred changes (via `CommandBuffer`) don't bump the counter during the group, so the safe pattern never false-trips — the assert *rewards* deferral, which is precisely B2's model.

*Access wiring (small new surface the plan must add):* the counter is `ArchetypeManager::m_structuralChangeCounter` (`ArchetypeManager.hpp:1400`), today **private** with `friend class View` (`:1405`) and no getter — `View` reads it directly (`View.hpp:108`). Add a public `ASTRA_NODISCARD uint32_t GetStructuralChangeCounter() const noexcept` on `ArchetypeManager`; the executor reaches it via the already-public `Registry::GetArchetypeManager()` (`Registry.hpp:1026`). Guard the snapshot reads themselves (not just the `ASTRA_ASSERT`) under `ASTRA_BUILD_DEBUG` so Release pays nothing and `SystemExecutor.hpp` only needs the fuller `Registry`/`ArchetypeManager` include on the debug path. The tripwire lives only in the built-in `ParallelExecutor` (a custom `ISystemExecutor` opts out) and only fires for concurrently-dispatched multi-member groups — `SequentialExecutor` and size-1 groups run in-order and are inherently safe.

**Named limitation (documented, not hidden):** the counter tripwire is *best-effort*. It fires only on **archetype-set** changes — a new archetype created, or an empty one removed by defragmentation — because those are the only events that bump `m_structuralChangeCounter`. It therefore does **not** catch structural mutations confined to already-existing archetypes (a repeat `CreateEntity` of an existing shape, an add/remove-component transition between two pre-existing archetypes, or a `DestroyEntity` that doesn't empty its archetype), **nor** a system reading/writing a component it didn't declare but another grouped system owns (a mask-completeness breach). The contract remains "declare *all* access + mark structural systems `Exclusive`"; the tripwire is a Debug aid that reliably catches the canonical novel-shape spawner, not a complete verifier. TSan is the tool for the mask class (unavailable on MSVC — see §6); B2's access-set expansion + wired command buffer is the real fix.

**Default stays "parallelize declared-mask systems"** — not exclusive-by-default — so the flecs/Bevy-style user with proper declared access keeps their parallelism; the promise+tripwire is the honest middle.

---

## Section 2 — `ExecutionGuard` correctness (I1 + I2)

The fix matches the philosophy (no built-in guarantee) rather than fighting it — **no mutex** (it would tax the single-threaded default path and advertise exclusion we don't provide). The mature engines use a *mode*, not a lock (flecs readonly/defer, Bevy scheduler-managed access); B1 delivers the honest stand-in and the through-line to B2's real mode.

1. **Document the single-writer contract.** System-set mutation (`Add`/`Remove`/`Clear`) must not race `Execute`, same rule as the `Registry` itself. That — not the flag — is what makes it safe.
2. **Downgrade the flag's advertised role (I1), and make misuse handling uniform.** Rewrite the comment: the flag does **not** provide mutual exclusion against external threads. It exists to make the one practical mistake safe — a system, on a worker, calling `RemoveSystem`/`AddSystem`/`Clear` mid-frame, which no-ops (and, for `AddSystem`, returns `Err(SchedulerExecuting)`) via the dispatch happens-before (now guaranteed by the I4 fix). **Drop the `ASTRA_ASSERT(!IsExecuting())` misuse-asserts from `Add`/`Remove`/`Clear`, and the duplicate-registration `ASTRA_ASSERT(false, …)`.** Rationale (**decision 2026-07-13, supersedes the earlier "keep the assert in Debug" wording**): once I5 (§4) gives registration a typed error channel (`Result<void, SystemError>`), an `assert`-and-abort on the *same* condition contradicts it — asserts mean "this can never legitimately happen; halt," while a `Result` means "this happens at runtime; handle it." Keeping both makes the error channel unreachable/untestable in Debug (the caller never gets to handle `AlreadyRegistered`/`SchedulerExecuting`). The graceful `if (IsExecuting()) return …;` guard (not the assert) is what makes it safe, so dropping the asserts loses a Debug *notification*, not a guarantee, and yields one uniform, all-config-testable contract. The **only** retained/added assert here is the reentrant-`Execute` tripwire (item 3) — reentrant `Execute` has no error channel and is a genuine unsupported invariant.
3. **Make it reentrancy-correct (I2).** Replace `std::atomic<bool> m_isExecuting` with `std::atomic<int> m_executionDepth`; the guard increments on entry / decrements on exit; `IsExecuting() == depth > 0`. Stays `atomic` for the well-defined worker read. **Reentrant `Execute()` is allowed** (the depth counter makes it *safe*) but fires a Debug `ASTRA_ASSERT` flagging it as suspect and belonging in an `Exclusive` context (Bevy gates schedule re-entry behind `&mut World`; we tripwire rather than build gating machinery in B1).

**Forward-compat:** `m_executionDepth` returning to zero is the designated B2 sync point — B2 flushes the `ParallelCommandBuffer` there, mirroring flecs's merge-at-`defer==0`.

---

## Section 3 — Ordering determinism (I3)

Adopt the **Unity/Mass instinct: an insertion-order-stable plan** (over the Bevy "non-deterministic + ambiguity-detector" route, which to do *honestly* would require building the detector in B1).

- **Constrain the greedy** so a system never leapfrogs an earlier unscheduled system into an earlier group. `S0:wA, S1:wA, S2:wB` → `[[S0],[S1,S2]]` (same group count, same parallelism), and **Sequential and Parallel executors produce identical group-granularity order.** The naive user relying on declaration order is no longer silently betrayed, and the plan is deterministic → trivially unit-testable.
- **Document the contract** in Bevy's vocabulary: (a) only component-mask dependencies are honored; (b) *within* a parallel group, order is concurrent/unspecified; (c) a hidden dependency between two mask-independent systems in the same group is a **"system-order ambiguity"** — not honored; express it via masks now, or explicit `Before`/`After` in B2.
- Honest tradeoff: no-leapfrog can leave a parallel slot idle that aggressive greedy would backfill. For B1 (honesty/correctness over raw throughput — throughput is the injected scheduler's job) that is the right trade.

**Forward-compat:** B2's explicit `Before`/`After`/`chain` constraints become a topological sort with insertion order as the stable tiebreak — exactly Unity's model, extending this substrate.

---

## Section 4 — Seam contract (I4) + registration error channel (I5)

**I4 — one comment change, zero implementation impact.** Extend `IWorkScheduler::ParallelFor`'s memory-model clause (`Core/WorkScheduler.hpp:25-32`) to require a happens-before edge from the `ParallelFor` call site to the start of *every* `fn` invocation (the forward edge), in addition to the existing backward edge. Every real scheduler already provides it — it's what `std::for_each(std::execution::par, …)` and TBB `parallel_for` guarantee; we make the written contract match what correctness silently assumes.

**I5 — registration half only** (per-system runtime error channel deferred to B2's `SystemContext`). Make `AddSystem` *failable*, satisfying the project's "recoverable → graceful in all configs" contract:

- nothrow allocation (`new (std::nothrow) T(...)`);
- return `[[nodiscard]] Result<void, SystemError>` (matching `CommandBuffer`'s `Result<void, CommandError>` house style — chosen over `bool` for consistency) with

```cpp
enum class SystemError { AlreadyRegistered, AllocationFailed, SchedulerExecuting };
```

Source-compatible: existing `scheduler.AddSystem<Foo>();` calls still compile (`[[nodiscard]]` warns, never breaks). The silent duplicate / during-execution paths now report through this channel. Applies to both the `System T` and lambda `AddSystem` overloads and `AddSystemInternal` (`SystemScheduler.hpp:50, 106-111, 383-432`).

---

## Section 5 — In-file minors + forward-compat contract

**Fold into B1:**
- **Stale `insertionOrder` (`SystemScheduler.hpp:126-136`)** — the plan orders by *vector position* (`i`), not `metadata.insertionOrder`, and `erase` preserves relative vector order, so §3's stable plan is already correct across removals. Re-sync the stored field in the existing index-fixup loop so it isn't misleading (can't drop it — it's public `SystemExecutionContext.metadata` surface).
- **Hash-collision note (`SystemScheduler.hpp:57`)** — add the assert-noting comment (astronomically unlikely, but it's a hash keying a registry).

**Document-only** (changing them costs more than it's worth or perturbs integrator surface):
- `BuildExecutionPlan` O(n³) worst case — fine for tens of systems, rebuilt only on `m_needsRebuild`.
- `context.metadata` copied per `Execute` — intentional public surface for custom integrations; note the cost.
- `GetExecutionPlan() const` mutates via `const_cast` — fine under the single-writer contract; document "not during `Execute`."

**Deferred — a dedicated "lambda-system polish" pass** (two entangled items, both touching `LambdaSystemWrapper`):
- The **lambda view-cache** (`System.hpp:114-135`, fresh `View` per invocation → per-frame archetype re-scan) — pure perf with a registry-identity subtlety.
- The **`IsReadOnly` pointer-to-const minor + optional-param support** (`System.hpp:78`). Plan-time trace found the `const T*` classification bug can't be fixed in isolation: `BaseType` doesn't strip the pointer and `ExtractAndExecute` builds `CreateView<const T*>` (malformed) — optional/nullable params are not actually implemented in the wrapper. A partial `IsReadOnly`-only fix would yield wrong masks or fail to compile, with nothing functional to test. Correct fix = implement optional-as-pointer end-to-end (`Optional<T>`-backed), out of B1's honesty-and-correctness scope. The current behavior is *safe* (over-conservative serialization of a feature no system uses today), so deferral costs nothing.

**The B2 forward-compat contract (why B1 is additive):**
1. `Exclusive` is a composable tag in a variadic `SystemTraits`; B2 adds resource/out-of-band access declarations and deferred-structural intent to the *same* block without breaking existing `SystemTraits` uses.
2. The system signature stays `void(Registry&)` in B1; B2 *adds* `void(SystemContext&)` as a second accepted overload, never a replacement.
3. `m_executionDepth == 0` is the designated B2 command-buffer sync point.
4. `SystemExecutionContext` is append-only; B1 fields (incl. `metadata`) are never repurposed/removed.
5. `SystemError` is extensible; B2 adds runtime/per-system variants.

---

## Section 6 — Test plan (the suite that today is essentially empty)

Conventions: reuse `tests/Support/TestWorkerPool.hpp` as the injected `IWorkScheduler` (review confirms it correct); **test behavioral correctness and verify Debug tripwires by inspection** (the existing `ResourceStorageGuardTest` convention — no death-test reliance); gate on all three configs. No TSan on MSVC → concurrency hazards surfaced via the Debug structural-counter tripwire + repeated-run stress, not TSan (documented limitation).

**A. Conflict analysis + plan construction** (deterministic, no threads):
- `HasConflict`: write-write / read-write / write-read → conflict; read-read → none; no-trait → conflict.
- Non-conflicting grouped; conflicting separated; no-trait → solo.
- `Exclusive` → solo group even beside disjoint-mask systems (§1).
- Insertion-order-stable: `S0:wA, S1:wA, S2:wB` → `[[S0],[S1,S2]]` (§3).
- `dependsOnEarlier` ordering preserved; identical registration → identical plan.

**B. Executor behavior & equivalence:**
- Sequential and Parallel yield identical group-granularity order (§3).
- Pure disjoint-mask systems genuinely run concurrently under a multi-thread `TestWorkerPool` (feature still works) (§1).
- Single-system / no-scheduler short-circuits inline (preserved).

**C. Exclusive safety — the C1 repro, now safe (§1):** an `Exclusive` spawning system beside pure systems runs solo; with a multi-thread pool and N-iteration stress, final state stays consistent (entity counts/data correct).

**D. `ExecutionGuard` (§2):** Add/Remove/Clear during `Execute` → no-op + `Err(SchedulerExecuting)`, set unchanged; reentrant `Execute` keeps `IsExecuting()` truthful through nesting and outer completes; worker-thread `RemoveSystem` no-ops; `RemoveSystem` middle-removal leaves survivors executing correctly (delegates valid).

**E. Registration error channel (§4):** duplicate → `Err(AlreadyRegistered)`; during-execution → `Err(SchedulerExecuting)`; success → `Ok`. (`AllocationFailed` by inspection — not portably forceable.)

**Infra:** likely new `tests/System/SystemSchedulerTest.cpp` (+ possibly `SystemExecutorTest.cpp`) → regen `premake5 vs2022`, never `git add ide/`; keep each file's `TEST`/`TEST_F` macro consistent; baseline 551, gate on "all configs green + intended new tests."

---

## Non-goals (YAGNI / deferred)

- **B2 in its entirety:** `SystemContext&` system form, wiring `ParallelCommandBuffer` into the executor, access-set expansion (resources + structural intent), merge/flush points, deterministic deferred-apply order, per-system runtime error channel, explicit `Before`/`After`/`chain` ordering + ambiguity detection.
- **The custom assert/verify seam:** a full `ASTRA_ASSERT`/`ASTRA_VERIFY` overhaul (debug-break + formatted messages + settable `SetAssertHandler` + no logging dependency) is worthwhile but orthogonal and library-wide (32+ files). Queued as its **own** work item, sequenced **after B1** (B1 does not need it; its proposed `ASTRA_ASSERT(condition, ...)` signature is backward-compatible with today's two-arg form, so B1's new asserts migrate in that later sweep for free).
- No mutex / no built-in thread-safety guarantee added to any core type.
- No `BuildExecutionPlan` algorithmic-complexity work.
- **Lambda-system polish pass (deferred, see §5):** lambda view-cache; `IsReadOnly` pointer-to-const minor + real optional/nullable-param (`Optional<T>`) support in `LambdaSystemWrapper`.

## Gate

- Whole solution builds and the full `AstraTest` suite is green in **Debug, Release, and Dist** (the C1 hazard and the guard are Debug-diagnosed but the behavioral contracts must hold in all configs).
- New scheduler/executor tests present and passing; existing tests unaffected; no public API break (existing `AddSystem`/`SystemTraits`/system call sites still compile).
- Commit tracked source only (`include/`, `tests/`, `docs/`); `ide/`, `Astra.sln`, `Makefile`, `*.make` gitignored. New test files → regen premake, never `git add ide/`.
