# Astra Theme B2 — Parallel System Scheduler design

**Date:** 2026-07-17
**Status:** Design — awaiting review
**Branch:** `theme-b2` (off `dev` @ `1920092`)
**Builds on:** Theme B1 (merged) — opt-in `SystemScheduler`/`ParallelExecutor`, `IWorkScheduler` seam, `depth==0` sync-point marker, Debug structural-change tripwire, insertion-order-stable execution plan.

---

## 1. Motivation & scope

B1 delivered honest, opt-in **system-level** parallelism (run non-conflicting systems concurrently) and reserved the hooks for B2. B2 makes Astra a **complete, deterministic, access-aware parallel scheduler** — the flecs/Bevy/Unity-DOTS-class capability that EnTT deliberately omits (EnTT leaves both the command buffer and the scheduler to the user; that is Astra's differentiation opportunity).

B2 delivers, as one coherent scheduler:
- **Deferred structural changes** during parallel execution, via a `SystemContext` handed to each system, flushed **deterministically** at a sync point.
- **Both axes of parallelism**: system-level (B1) and **intra-system chunk-parallel** (one heavy system's iteration split across all cores — the parallelism that actually saturates hardware).
- **Full access-aware scheduling**: component read/write (B1) **plus resources / out-of-band state** folded into the same conflict analysis.
- **Ordering correctness**: explicit `Before`/`After` constraints and **ambiguity detection** (report unordered mutable conflicts — the Bevy differentiator).

**Research grounding** (`docs/` research 2026-07-16): the field consensus is defer-structural-changes → record per-worker lock-free → flush at framework sync points (hard barriers, minimize them) → recover determinism at the barrier. flecs's sharp line — *only structural changes are deferred; direct component value-writes stay in-place* — is adopted. Unity's **sort key** is the determinism mechanism, adapted to Astra.

## 2. What already exists vs what B2 adds

B2 is overwhelmingly **integration**, not construction. Confirmed in the code map (2026-07-16):

**Exists (wire together):**
- `ParallelExecutor` — system-level parallel dispatch via `IWorkScheduler::ParallelFor` grain-1 (each system runs whole on one worker). `SystemExecutor.hpp`.
- `View::ParallelForEach` — chunk-parallel iteration that **already computes the chunk index** (`ParallelForEachChunkImpl(archetype, chunkIndex, …)`, `View.hpp:115,351`). The sort-key index we need is already in hand.
- `CommandBuffer` + per-worker `ParallelCommandBuffer` — full structural-change API, `Execute()` flush, rollback-on-partial-failure. `Commands/CommandBuffer.hpp`. **Nothing in the system layer calls it today.**
- `depth==0` sync-point marker (`ExecutionGuard`/`m_executionDepth`) — a marker only; nothing runs there. `SystemScheduler.hpp`.
- `ResourceStorage` — keyed singleton state on the Registry; invisible to the scheduler today. `Component/ResourceStorage.hpp`.
- `SystemTraits<Reads<…>, Writes<…>, Exclusive>` → component masks in `SystemMetadata`, consumed by `BuildExecutionPlan`'s conflict analysis. `System/System.hpp`, `SystemScheduler.hpp`.
- Clean layering: `Registry.hpp` includes neither `System/` nor `Commands/` (pay-for-what-you-use). Preserved.

**B2 adds (the glue):**
- `SystemContext` type + the additive `void(SystemContext&)` system signature.
- A **composite sort key** on commands + flush-by-key at the sync point (today's flush is thread-arrival order — non-deterministic).
- **Placeholder entities** for deferred creation (today `ParallelCommandBuffer::CreateEntity` allocates the id eagerly at record time, owner-thread-only).
- A **per-system error channel**.
- Sort-key stamping wired into `View::ParallelForEach`'s existing chunk index.
- **Resource** read/write masks in `SystemMetadata` + conflict analysis.
- Explicit **`Before`/`After`** ordering + **ambiguity detection**.
- **Lambda-system polish** (remove the vestigial optional-param path).

## 3. Design principles (binding)

- **Pay-for-what-you-use.** `Registry.hpp` must continue to include neither `System/` nor `Commands/`. A consumer using only the Registry pays for none of B2. All B2 code lives in `System/` and `Commands/`, above Registry.
- **Opt-in, replaceable seam.** Everything routes through `IWorkScheduler` (a `Mosaic::IWorkScheduler` alias). A null scheduler ⇒ sequential inline fallback; Astra spawns no threads.
- **Exception-free & RTTI-off in shipping.** Errors are values (a per-system error channel / `Result`), never exceptions. No `try`/`catch`. Structural-safety checks that must hold in shipping are real `if`s, never `ASTRA_ASSERT` (which compiles out).
- **Additive, no break.** `void(Registry&)` systems keep working unchanged (no deferral). `void(SystemContext&)` is a new overload. The 602/600 test baseline must stay green.
- **Determinism is a first-class contract** (see §14), because reproducibility (lockstep/replay/debugging) is a core "serious ECS" promise.

## 4. Architecture overview

```
SystemScheduler  (registration, BuildExecutionPlan, ordering, ambiguity)
   │ owns the plan (groups) + per-system metadata (component+resource masks, Before/After)
   ▼
ISystemExecutor  (Sequential | Parallel)  — runs groups; at depth→0, FLUSHES the command buffer
   │ hands each system a …
   ▼
SystemContext  — { Registry&, this system's command-recording surface, insertionOrder, error sink }
   │ system defers structural changes → ParallelCommandBuffer (per-worker), each command tagged with the composite sort key
   │ system may call ctx.ParallelForEach(view, fn) → chunk-parallel; the chunk index is stamped into the sort key
   ▼
Sync point (depth==0)  — stable-sort all recorded commands by key, apply single-threaded, resolve placeholder entities, drain errors
```

Modules below are the unit boundaries; each is independently testable.

## 5. Module — SystemContext + the additive signature

`SystemContext` is a **lightweight, per-invocation handle** (not owning). It holds:
- `Registry&` — for the system's declared **reads/queries** and any *immediately-safe* work (reads never conflict with the deferral model).
- a reference to **this system's command-recording surface** (its slice of the `ParallelCommandBuffer`), exposed as `ctx.Commands()` returning a `CommandBuffer`-like recorder that tags each op with the sort key.
- the system's **`insertionOrder`** (the high bits of the sort key).
- an **error sink** (`ctx.ReportError(SystemError)` / structural failures routed here — see §8).
- the **iteration index** state used by `ctx.ParallelForEach` (§9).

New system concept (additive):
```cpp
// System.hpp — additive to the existing System concept (void(Registry&))
template<typename T>
concept ContextSystem = requires(T s, SystemContext& ctx) { { s(ctx) } -> std::same_as<void>; };
```
Registration (`SystemScheduler::AddSystem`) detects which signature the system/lambda has and stores the appropriate thunk (`Delegate<void(Registry&)>` vs `Delegate<void(SystemContext&)>`). Trait extraction (`Reads`/`Writes`/`Exclusive` + new resource traits) is unchanged for both.

**Reads vs deferral rule (flecs line, adopted):** a system reads/mutates *component values* in place via the Registry/View (fast path, no command). It **defers only structural changes** (create/destroy entity, add/remove component, relationship/resource structural ops) via `ctx.Commands()`. The Debug tripwire (B1) continues to catch an undeclared in-place structural change.

## 6. Module — Deferred structural changes + composite sort key

**Command format change:** each command carries a **composite sort key**
```
struct SortKey { uint32_t insertionOrder; uint32_t iterationIndex; uint32_t recordSequence; };
```
(packed into the command header; confirm header has room or widen it). `insertionOrder` = the recording system's plan order; `iterationIndex` = 0 for ordinary deferral, or the chunk index when recorded inside `ctx.ParallelForEach` (§9); `recordSequence` = a per-(system,iterationIndex) monotonic counter, giving exact record order as the final tiebreak.

**Flush (at the sync point, §13):** collect all commands from all per-worker buffers, **stable-sort by `SortKey`** (lexicographic: insertionOrder, then iterationIndex, then recordSequence), then apply **single-threaded** in that order. This replaces today's thread-arrival-order `ParallelCommandBuffer::Execute()`. Determinism is then independent of thread count/scheduling.

**Conflict semantics (choose explicitly, documented):** value-overwriting ops are **last-write-wins** in sorted order (consistent with Bevy `insert`/flecs `set`). An op on an entity destroyed earlier in the same flush is a **no-op that reports to the error channel** (not a panic — matches Astra's exception-free stance; the flush continues). Batching ops for the same entity to minimize archetype moves (flecs/Mass optimization) is a **permitted, non-observable** implementation detail, not part of the contract.

## 7. Module — Placeholder entities (deferred creation)

Today `ParallelCommandBuffer::CreateEntity` allocates the real id from the shared `EntityManager` at *record* time — unsafe from a worker thread. B2 decouples this (Unity's temporary-entity model):
- `ctx.Commands().CreateEntity(...)` returns a **placeholder `Entity`** (a reserved handle from a per-worker/local space, distinguishable from a real entity — e.g. a high-bit tag or a separate id domain).
- A system may use the placeholder in *subsequent deferred ops in the same buffer* (add component to it, set its parent, etc.).
- At flush, placeholders are **resolved to real ids** in sort-key order (deterministic id assignment), and every deferred op referencing a placeholder is rewritten to the resolved id.
- Passing a placeholder to the live Registry (immediate API) or across buffers is invalid → error channel.

This is the one genuinely new data-structure piece; it makes deferred creation safe from any worker and keeps id assignment deterministic.

## 8. Module — Per-system error channel

Deferred structural ops can fail at flush (op on a destroyed entity, an unresolved placeholder, an over-cap batch). Rather than abort, each failure is recorded as a `SystemError`-style value attributed to the **recording system** (via `insertionOrder`/system id) and surfaced after the flush — e.g. `SystemScheduler::Execute()` returns/accumulates a list the caller can inspect. Mirrors B1's failable-`AddSystem`→`Result` style. No exceptions, no partial-heap-corruption: the existing rollback path is retained for allocation failures; logical failures (destroyed-entity op) are skipped + reported.

## 9. Module — Chunk-parallel iteration + sort-key stamping

`View::ParallelForEach` already splits iteration across workers by chunk and knows the `chunkIndex`. B2 exposes it through the context:
```cpp
ctx.ParallelForEach(view, [](Entity e, A& a, const B& b, SystemContext& sub){ ... sub.Commands().AddComponent<C>(e, ...); });
```
Inside the parallel body, the context's `iterationIndex` is set to the current `chunkIndex`, so any command recorded there is tagged `(insertionOrder, chunkIndex, recordSequence)` → the stable-sort recovers deterministic apply order regardless of which worker processed which chunk. A per-worker command buffer is used for recording (no cross-thread contention), exactly as today.

**Scope note:** the underlying `View::ParallelForEach` exists; B2 adds the context-aware wrapper that (a) provides a per-chunk recording surface and (b) stamps the chunk index. If the value-mutation semantics of the existing `ParallelForEach` need adjustment to expose the sub-context cleanly, that is in-scope glue (not a rewrite of iteration).

## 10. Module — Resource access declaration + conflict analysis

Mirror the component mechanism exactly (Unreal Mass model, since Astra keeps resources separate from components):
- New traits `ReadsResources<R...>` / `WritesResources<R...>` compose into `SystemTraits` alongside `Reads`/`Writes`/`Exclusive` (variadic, any order).
- `ExtractSystemTraits` also folds resource types (keyed by their `ComponentID`/`TypeID`, as `ResourceStorage` already keys them) into new `resourceReads`/`resourceWrites` masks in `SystemMetadata`.
- `BuildExecutionPlan`'s conflict test additionally intersects resource masks: two systems conflict (cannot share a parallel group) if they share a component **or a resource** with a write on either side.
- **Per-resource concurrency trait** (Mass's `TMassExternalSubsystemTraits`): a resource type may declare `ConcurrentReadSafe` (default true) — if false, even two readers serialize. This covers genuinely non-thread-safe external state.

Closes the real gap: today two systems both writing the same resource but with disjoint component masks are wrongly placed in the same parallel group.

## 11. Module — Explicit Before/After ordering

Beyond insertion-order + mask-inferred grouping, a system may declare hard ordering:
- `Before<T...>` / `After<T...>` traits (compose into `SystemTraits`), and/or a runtime `AddSystem(...).Before<T>()` builder form — pick the trait form as primary for consistency with the rest of `SystemTraits` (confirm ergonomics in the plan).
- These add edges to the plan: `BuildExecutionPlan` topologically respects them when ordering groups (a `Before` edge forces the system into an earlier group than its target; a cycle is a registration error surfaced via the existing `Result<void, SystemError>` path).
- Ordering edges compose with mask-conflict grouping: an explicit edge can force serialization the masks wouldn't, and can resolve an otherwise-ambiguous pair (§12).

## 12. Module — Ambiguity detection

The Bevy-style differentiator, adapted to Astra's grouping model. After building the plan, detect **system pairs that have a mutable conflict** (share a component or resource with a write on either side) **whose relative order is determined only by insertion-order accident** — i.e. there is no explicit `Before`/`After` edge fixing their order and they are not forced apart by grouping in a way the user declared. Report each such pair, naming the conflicting component/resource, through the diagnostics seam (`ASTRA_LOG_WARN`) — **opt-in** (off by default; enabled via a scheduler flag), because it is a development aid.
- Suppression: an `AmbiguousWith<T...>` trait (Bevy's `ambiguous_with`) marks a pair as intentionally-order-independent, silencing the report — used only for genuine false positives, with a justifying comment.
- This surfaces latent order-dependence that today is silently resolved by insertion order — exactly the class of bug that makes parallel schedulers non-reproducible-in-spirit.

## 13. Module — Sync-point placement

- **Default:** a **single flush at `depth==0`** (the B1 marker) — all deferred structural changes apply once, after the whole scheduler run, before control returns. Simplest, one barrier, matches the reserved hook.
- **Explicit barriers:** the user may insert a **`SyncPoint`** between systems/groups (a registration-time marker) so a spawn-then-process-in-one-run pattern works: everything before the barrier flushes before the systems after it run. Each barrier is a hard serial point (documented cost).
- **Deferred to a follow-on:** *automatic*, access-derived sync-point insertion (Bevy 0.13's "insert a barrier only between a deferred-writer and a later reader of the same data"). It requires the full access graph and is the least predictable piece; explicit barriers + single-at-end cover the correctness need for v1.

## 14. Determinism contract (what B2 promises)

Given the same initial world, the same registered systems, and the same input, **the applied order of all deferred structural changes is identical across runs, independent of thread count and scheduling.** Concretely: commands sort by `(insertionOrder, iterationIndex, recordSequence)`; placeholder ids are assigned in that order; the flush is single-threaded. This holds for both ordinary deferral and chunk-parallel deferral. (Component *value* writes are in-place and are governed by the systems' declared access + grouping, not by this order — a write/write component conflict is prevented by grouping, not resolved by sort key.)

## 15. Module — Lambda-system polish

Remove the vestigial optional/nullable lambda-parameter path (`System.hpp` `IsReadOnly`/`BaseType` mishandling of `const T*`): no system uses it, and it currently mis-infers a `const T*` as a write and would build a malformed view. Replace with a `static_assert` that rejects pointer/optional params with a clear message (or, if the plan prefers, implement `Optional<T>` end-to-end — but the default per YAGNI is **remove**, since the value-reference path is correct and tested-by-use). Also note (do not necessarily fix here) the per-invocation `View` rebuild perf item flagged in B1.

## 16. Testing strategy

- Reuse `tests/Support/TestWorkerPool.hpp` (the sanctioned injected `IWorkScheduler`) and the B1 multi-system harness. No TSan on MSVC — hazards surface via the Debug structural tripwire + **repeated-run stress** (many frames) to shake out nondeterminism.
- **Determinism tests are the crux:** run a schedule that defers conflicting structural changes from multiple systems (and from chunk-parallel iteration) N times under the real `TestWorkerPool`; assert the resulting world is **byte-identical every run** (same entities, same components, same ids). A sort-key regression makes this flake — that is the signal.
- Placeholder-entity tests: defer create + add-to-created + set-parent-of-created in one buffer; assert correct resolution and deterministic ids.
- Resource-conflict tests: two systems writing the same resource with disjoint component masks must NOT share a parallel group (assert plan grouping).
- Ordering/ambiguity tests: `Before`/`After` produce the required order; a cycle is a registration error; the ambiguity report fires for an unordered mutable conflict and is silenced by `AmbiguousWith`.
- Error-channel tests: an op on a destroyed entity is skipped, reported, and does not abort the flush.
- The full suite (602/600 baseline) stays green; `void(Registry&)` systems unchanged.

## 17. Global constraints

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. Exception-free & RTTI-off in shipping.
- No public API break; `void(Registry&)` systems unchanged; baseline 602 Debug / 600 Release+Dist stays green.
- Namespace `Astra`. All new code in `System/` and `Commands/`; `Registry.hpp` gains no dependency on either.
- Everything opt-in via `IWorkScheduler`; null scheduler ⇒ sequential fallback (and then the sort-key flush is trivially in insertion order).
- Build via MSBuild whole-solution (`-t:AstraTest` does not work); new test files need `premake5 vs2022` regen.

## 18. Explicitly deferred (not in B2)

- Automatic access-derived sync-point insertion (Bevy 0.13). Explicit barriers cover v1.
- Static **verification** that a system's declared access matches its actual access (the "unverified promise" — B1's masks are trusted). The Debug structural tripwire remains the only enforcement.
- The per-invocation `View`-rebuild perf optimization (B1 leftover) — noted, not required.
- A full job-graph executor (arbitrary task DAG beyond system groups).

## 19. To confirm during planning (grounding checks)

- The command header's room for the 12-byte `SortKey` (widen vs pack). `Commands/Command.hpp`.
- The exact `View::ParallelForEach` value-mutation signature and how cleanly a per-chunk sub-context can be threaded through it. `View.hpp:115-173`.
- Whether `EntityManager` id space has a spare high bit for the placeholder domain, or a separate placeholder id space is cleaner. `Entity/Entity.hpp`.
- The precise `BuildExecutionPlan` conflict predicate to extend for resources + ordering edges. `SystemScheduler.hpp:276-323`.
