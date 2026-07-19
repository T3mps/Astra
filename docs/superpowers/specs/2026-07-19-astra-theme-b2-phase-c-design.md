# Theme B2 — Phase C: Resource Access Declaration + Conflict Analysis — Design

> **Status:** approved (brainstorm 2026-07-19). Feeds `superpowers:writing-plans`.
> **Parent spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-concurrency-design.md` (the whole-B2 design; Phase C delivers its **§10 — resource access declaration + conflict analysis**).
> **Builds on:** Phase A (deferred-command core) + Phase B (thread-safe registration + chunk-parallel deferral), merged to `dev` @ `342f05c`.

## Goal

Fold **resource** access into the scheduler's conflict analysis so systems that share a resource with a write on either side are never placed in the same parallel group. Today the plan builder only reasons about component masks, so two systems that both write the same resource but touch disjoint components are wrongly run concurrently — a data race on the shared singleton. Phase C closes that gap by mirroring the existing component-mask mechanism for resources, plus a per-resource `ConcurrentReadSafe` opt-out for genuinely non-thread-safe state.

## Scope

Full parent-spec §10:

1. New `ReadsResources<R...>` / `WritesResources<R...>` traits composing into `SystemTraits`.
2. `resourceReads` / `resourceWrites` masks in `SystemMetadata`, filled by `ExtractSystemTraits`.
3. `BuildExecutionPlan` extended to conflict on a shared resource with a write on either side, and to treat a resource-only system as a real (groupable) system rather than a solo/no-trait system.
4. A per-resource `ResourceTraits<T>::ConcurrentReadSafe` (default `true`); when `false`, even two readers serialize.

Everything else in the whole-B2 spec stays out of Phase C: explicit `Before`/`After` ordering (§11), ambiguity detection (§12), sync-point barriers (§13), lambda polish (§15). Ambiguity detection (§12, Phase D) depends on this module — it reports conflicting-and-unordered system pairs, and "conflict" is only complete once resources are in the conflict predicate; C therefore precedes D.

## Global constraints (unchanged from Phase A/B)

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT`.
- **Additive, no break.** Existing `void(Registry&)` systems, view-lambdas, `void(SystemContext&)` systems, and `SystemTraits<Reads<...>, Writes<...>, Exclusive>` all keep compiling and scheduling identically. A system that declares no resource traits behaves exactly as today. No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include. The traits live beside the existing `Reads`/`Writes` in `System.hpp`.
- Namespace `Astra`. All three build configs stay green.

---

## Design

### Resource identity: reuse the ComponentID space, in separate masks

`ResourceStorage` already keys every resource by `TypeID<T>::Value()` — the same dense `ComponentID` (0..`MAX_COMPONENTS`-1) space components use. A resource type and a component type are distinct types, so they receive distinct `ComponentID`s from the shared `TypeContext` counter. Phase C therefore represents resource access with the existing `ComponentMask` (`Bitmap<MAX_COMPONENTS>`) type, in **separate** `resourceReads` / `resourceWrites` fields distinct from the component `reads` / `writes`.

Keeping the masks separate is load-bearing: it makes a component-vs-resource false conflict impossible. A type `T` used both as an entity component and as a resource occupies one `ComponentID` bit, but component access is compared only against `reads`/`writes` and resource access only against `resourceReads`/`resourceWrites` — the two never intersect. This is correct, because touching `T`-as-a-component (entity columns) and `T`-as-a-resource (the singleton slot) are genuinely different memory.

**Constraint (shared with today):** components and resources together must fit in `MAX_COMPONENTS` (128) type-ids. This is a pre-existing property of `ResourceStorage` (resources already consume `ComponentID`s), not introduced here; Phase C only reads those ids into masks.

### Module 1 — Traits (`System.hpp`)

Mirror the component traits:

```cpp
template<typename... Resources> struct ReadsResources  { using type = std::tuple<Resources...>; };
template<typename... Resources> struct WritesResources { using type = std::tuple<Resources...>; };
```

`SystemTraits<...>` gains `ReadsResourceTypes` / `WritesResourceTypes` (a `tuple_cat` of the resource traits across the pack, via `Detail::TraitReadsResources` / `Detail::TraitWritesResources` mirroring the existing `TraitReads` / `TraitWrites`). Composable in any order alongside `Reads`/`Writes`/`Exclusive`:

```cpp
using MyTraits = SystemTraits<Reads<Position>, WritesResources<PhysicsWorld>, ReadsResources<Time>, Exclusive>;
```

A per-resource-type customization point declares thread-safety:

```cpp
template<typename T> struct ResourceTraits { static constexpr bool ConcurrentReadSafe = true; };
// opt out for a non-thread-safe resource:
template<> struct ResourceTraits<GpuUploadQueue> { static constexpr bool ConcurrentReadSafe = false; };
```

### Module 2 — Metadata (`SystemMetadata.hpp`)

Add two fields beside the component masks:

```cpp
ComponentMask resourceReads;
ComponentMask resourceWrites;
```

### Module 3 — Trait extraction (`SystemScheduler.hpp` `ExtractSystemTraits`)

Fill the resource masks with the same `MakeComponentMask<R>()` path used for components (it sets the bit for `TypeID<R>::Value()`).

**The `ConcurrentReadSafe` fold.** For each `R` in `ReadsResourceTypes`, if `ResourceTraits<R>::ConcurrentReadSafe` is `false`, set `R`'s bit in `resourceWrites` instead of `resourceReads`. Writes always go to `resourceWrites`. This makes the existing write-involved conflict predicate catch read/read for a non-thread-safe resource **for free** — a non-`ConcurrentReadSafe` read behaves like a write for conflict purposes, which is exactly the intended semantics. (Rejected alternatives: a third `resourceSerialReads` mask + an extra predicate term; consulting `ResourceTraits` per shared bit at conflict time, which would break the O(1) mask-intersection model.)

### Module 4 — Plan builder (`SystemScheduler.hpp` `BuildExecutionPlan`)

Two changes to the O(n) contiguous-run grouper:

1. **Conflict predicate** — the group carries `groupResourceReads` / `groupResourceWrites` accumulators alongside the component ones, and the run breaks when a candidate conflicts on components **or** resources:
   `(sysJ.resourceWrites & groupResourceWrites) | (sysJ.resourceWrites & groupResourceReads) | (sysJ.resourceReads & groupResourceWrites)` is non-empty.
2. **"No declared hints → solo" predicate** — today a system with `reads.None() && writes.None()` is treated as a no-trait solo system. This must extend to include the resource masks, so a **resource-only system** (touches a resource, no components) is groupable rather than wrongly forced into its own group. A system is "no-trait solo" only if all four masks are `None`.

`Exclusive` is unaffected: an `Exclusive` system still runs solo regardless of resource access.

---

## Prior art & positioning

Phase C's §10 is modeled on **Unreal Mass**, and the decisions track it closely:

- **Resources kept separate from components** (separate masks) matches Mass's fragments-vs-subsystems split with `ProcessorRequirements`. It diverges from **flecs**, where a resource *is* a singleton component and needs no separate dimension — a divergence chosen deliberately because Astra keeps resources in a separate `ResourceStorage`, not as entity components.
- **`ConcurrentReadSafe` + `ResourceTraits<T>` specialization** is directly Mass's `TMassExternalSubsystemTraits<T>` (`ThreadSafeWrite` / `bThreadSafeRead`). **Bevy** and **flecs** have no read-serialization concept — Bevy leans on Rust's borrow rules and handles unsafe state with thread-pinning (`NonSend`). C++ has no `Send`/`Sync` inference, so a manual per-type declaration (the Mass/Astra approach) is the only option.
- **Manual `ReadsResources`/`WritesResources` declaration** matches Mass and is the C++ norm; **Bevy** auto-derives all access from system parameters (Rust type system) and **Unity DOTS** enforces it with a runtime safety system. Astra can auto-derive component access for view-lambdas but not resource access (resources are reached through `Registry` methods, not params), so explicit resource traits are necessary.
- **EnTT** has no scheduler or resource conflict analysis at all — Astra's entire B2 line, Phase C included, is the differentiator over EnTT.

## Boundaries & limitations

- **Advisory masks (deliberate flexibility-over-safety trade).** Resource traits are declaration-only scheduling hints, exactly like the component masks. The system still reads/writes resources through the existing `Registry` resource API; there is **no tripwire** for a system that touches a resource it did not declare, so a mis-declaration is a latent race the scheduler cannot catch. This is consistent with Astra's existing component value-access masks (the B1 Debug tripwire catches undeclared *structural* changes, not undeclared *value* access) and is what lets the mechanism work with opaque `void(Registry&)` systems. **Bevy** (type-derived access, cannot be mis-declared) and **Unity** (a runtime safety system that throws) are stricter here. A Unity-style Debug resource-access tripwire is a possible future hardening; it is **not** Phase C.
- **Chunk-parallel resource writes (documented, not enforced).** Resource conflict analysis serializes *systems*, not the chunk-workers *within* one system. A system that writes a shared resource from inside `ctx.ParallelForEach` chunk bodies races itself. Safe pattern: read resources in chunk bodies; write/accumulate a resource only outside `ParallelForEach`. (**Bevy** prevents this at compile time via Rust's `Sync` bound on `par_iter` closures; C++ cannot, so it is documented.)

## Out of scope (deferred follow-ons)

- **Thread-affinity / main-thread pinning.** `ConcurrentReadSafe = false` *serializes* access to a resource but does not guarantee it runs on a *specific* thread. Genuinely thread-affine state — a GPU context, a non-thread-safe C library with thread-local state, a main-thread-only API — is safe only on one thread, and serialization across arbitrary workers is insufficient. Both **Bevy** (`NonSend`) and **Mass** (`GameThreadOnly`) additionally support pinning such access to the main/game thread. Astra Phase C covers *non-thread-safe-but-thread-agnostic* state (serialize); *thread-affine* state (pin) is a recognized, deferred follow-on, because thread pinning is a larger executor feature (whole systems, not just resources, get pinned to a thread).
- Explicit `Before`/`After` ordering (§11), ambiguity detection (§12), sync-point barriers (§13), lambda polish (§15) — each a later phase. A Debug resource-access misuse tripwire (see Boundaries).

## Testing

- **Core acceptance (the gap this closes):** two systems that both `WritesResources<R>` with **disjoint component masks** must **not** share a parallel group (assert plan grouping). Two `ReadsResources<R>` (default `ConcurrentReadSafe`) with disjoint components **may** share a group. A read/write resource pair serializes.
- **`ConcurrentReadSafe`:** two readers of a `ResourceTraits<R>::ConcurrentReadSafe == false` resource must **not** share a group; two readers of a default (safe) resource **may**.
- **Resource-only system:** a system that declares only resource traits (no components) groups with a compatible resource-only peer and is not forced solo.
- **Regression:** the full suite stays green; component-only grouping and systems with no resource traits behave identically; `Exclusive` still runs solo.
- **TypeID-ceiling constraint (from a recent Critical fix):** the test binary already sits near the `MAX_COMPONENTS = 128` process-global `TypeID` ceiling, so these tests must declare only a **small** number of fresh resource/component types (each `ReadsResources<R>` consumes a `ComponentID` via `TypeID<R>::Value()`). See `[[astra-flatmap-pointer-stability]]`.
- Reuse `tests/Support/TestWorkerPool.hpp` and the B1 multi-system harness; assertions are on the execution plan grouping (`GetExecutionPlan()`), not on wall-clock parallelism.

## Design decisions (brainstorm 2026-07-19)

1. **Full §10 including `ConcurrentReadSafe`** (user-approved) — the core read/write conflict analysis plus the per-resource opt-out; cheap to add via the read-as-write fold and awkward to retrofit later.
2. **`ResourceTraits<T>` external specialization** for `ConcurrentReadSafe` (user-approved, over an intrusive static member) — non-intrusive, works for types you don't own, matches Mass's `TMassExternalSubsystemTraits<T>` and Astra's existing `SerializationTraits<T>` pattern.
3. **`ConcurrentReadSafe` expressed by folding a non-safe read into the write mask** (over a third mask + extra predicate term, or per-bit trait lookup) — keeps the conflict test a pure O(1) mask intersection.
4. **Separate `resourceReads`/`resourceWrites` masks** (reusing `ComponentMask`), not folded into the component masks — prevents component-vs-resource false conflicts.
5. **Resource-only systems are groupable** — the "no declared hints → solo" predicate includes the resource masks.
6. **Advisory masks, no access tripwire** (deliberate; see Boundaries).
7. **Thread-affinity / `GameThreadOnly` deferred** (see Out of scope).
