# Change detection: two-tier change ticks with `Changed<T>` / `Added<T>` filters

**Date:** 2026-09-10
**Status:** implemented on feat/change-detection (plan docs/superpowers/plans/2026-09-10-astra-change-detection.md)
**Program:** Bevy-style system queries, Stage 3 (Stage 1 View enrichment @ `638aa12`, Stage 2 SystemParam binder @ `3b5ef5b`). Origin: [[astra-north-star]] ergonomics pillar; the single biggest step toward "A" per the 2026-09-10 assessment.
**Evidence base (preserved in `bench-compare/spike-changeticks/`):** `cd-research.md` (Bevy / Unity DOTS / flecs / EnTT, cited), `cd-mockups.md` (three API mockups with Arcane before/after), `spike.cpp` + `results.csv` + `spike-notes.md` (throwaway synthetic spike, one unpinned box, directional), and the decision page https://claude.ai/code/artifact/49a7e945-f69f-4c0e-b1cc-37448a12996d .

## 1. Problem

Astra cannot answer "did this component change since I last looked". Arcane hand-rolls
the answer twice: a shadow/dirty array in `TransformPropagationSystem`
(`Arcane/ArcaneClient/src/Arcane/Scene/TransformSystems.hpp:84-89,314-333`, comment
"Astra has no component change tracking") and `appliedScale != lt.scale` plus a
`PreviousTransform` write-back in `PhysicsSystem.hpp:428-449,495-530`. Its render
submission (`RenderSystems.hpp:37-94`) re-lerps every visible sprite every frame with
no skip path. `Signal::ComponentUpdated` is declared and never emitted. No tick or
frame counter exists anywhere in Registry, SystemScheduler, or SystemContext.

Every write path that hands out live memory is unobservable after the call returns:
`View::ForEach` (`T&`), `View::Get`/`Single` (tuples of `T*`), `Registry::GetComponent<T>`
(non-const `T*`; a const overload returns `const T*`). Only `Set`/`Emplace`/`Add`,
Commands apply, deserialize, and archetype moves are fully under Astra's control.

## 2. Decisions (user-approved, 2026-09-10)

Priority order: performance first, ergonomics a close second. Vetted by mockups,
a perf spike, and engine research rather than picked from a menu.

1. **Two tiers.** A coarse per-chunk version per column for EVERY component (free,
   4 B per chunk per column), plus an opt-in precise tier of per-entity
   `{added, changed}` ticks for types that declare `AstraChangeTracked = true`
   (8 B per entity per tracked column, the type author's explicit cost).
2. **Chunk reject first, always.** `Changed<T>`/`Added<T>` skip chunks whose version
   is not newer than the querying system's last-run tick before any per-entity work.
   Measured: scattered 50% changes go from 3.59 to 0.28 ns/entity with the reject,
   after which per-entity and per-chunk models converge (0.28 vs 0.26).
3. **Coarse stamping is automatic from access.** A view whose access to `T` is
   non-const stamps `T`'s version in every chunk it visits (DOTS model); every
   Registry write path stamps the destination chunk. Keyed off const-qualification
   (`ViewAccess`), never off hand-written `Reads<...>` traits.
4. **Exact marking for tracked types is automatic and unconditional.** Views hand out
   `Mut<T>` for a non-const tracked `T`; implicit conversion to `T&` marks, so existing
   lambdas compile unchanged; `.Read()` does not mark. The non-const
   `GetComponent<T>` marks a tracked type; the const overload never does.
   `Modified<T>(e)` exists for raw-pointer code. No compare-then-store on the mark
   path (measured slower: 2.11-2.16 vs 1.97 ns); `SetIfNeq` is an explicit opt-in.
5. **Ticks are process-relative and advance once per system run.** Registry owns the
   counter; the scheduler advances it; `SystemContext` carries `LastRun()`/`ThisRun()`.
   Nothing is serialized; a load stamps every chunk changed so systems re-process once.
6. **Signature policy.** The `System` concept keeps accepting `Registry&`. The scheduler
   prefers an `operator()(SystemContext&)` overload when present; only that overload
   receives ticks. A `Registry&` system may filter with an explicit tick it stores.
7. **Zero cost when unused.** No per-entity storage without opt-in; no filter code
   instantiated in a view that names no `Changed`/`Added`; a coarse stamp is one store
   per chunk per writing pass (within noise of baseline).

Rejected: exact-only per-entity ticks for all types (Bevy: 8 B always-on, a
12-byte component loses 25-40% chunk capacity); explicit-only marking (flecs/EnTT:
silent false negatives when a caller forgets, the bug class Arcane's trackers exist to
avoid); compare-then-store as the default mark (slower on this hardware).

## 3. Design

### 3.1 Time

```cpp
using Tick = uint32_t;
class Registry {
    ASTRA_NODISCARD Tick CurrentTick() const noexcept;   // monotonic, starts at 1 (0 = never)
    Tick AdvanceTick() noexcept;                         // scheduler calls once per system run
    // Signed-difference compare: a is newer than b iff int32_t(a - b) > 0.
    // Age bound 2^31 runs; documented, no periodic rescan in this stage.
};
struct SystemContext {   // existing: GetRegistry(), Commands()
    ASTRA_NODISCARD Tick LastRun() const noexcept;   // this system's previous run (0 first time)
    ASTRA_NODISCARD Tick ThisRun() const noexcept;   // tick assigned to this run
};
```

The scheduler stores `lastRun` per system in `SystemMetadata`; before invoking a system
it does `thisRun = registry.AdvanceTick()`, builds the context, runs, then
`lastRun = thisRun`. Standalone (unscheduled) use: `Registry::AdvanceTick()` is public;
views take an explicit tick (§3.4).

### 3.2 Storage

- **Chunk versions (all columns).** `ArchetypeChunk` gains `Tick* m_columnVersion`
  (one per column ordinal, carved in the chunk arena like `disabledWords`, 8-byte
  aligned), zero-initialised. `Stamp(column, tick)` is a plain store.
- **Entity ticks (opt-in).** `ArchetypeColumnMeta` gains `trackedColumns[]`/
  `trackedColumnCount` mirroring `enableableColumns[]`. For each tracked column a
  chunk carves `struct { Tick added; Tick changed; }[capacity]` beside the disabled
  words. `IsTracked(column)` branches on a null pointer exactly like `IsDisabled`.
- **Opt-in trait.** `ChangeTrackedTraits<T>` / `IsChangeTrackedV<T>` via
  `if constexpr (requires { { T::AstraChangeTracked } -> convertible_to<bool>; })`,
  same shape and MSVC rationale as `IsEnableableV<T>` (Component.hpp:49-68). A tag
  (empty) type may not be tracked (nothing to change) -- compile-time refusal like the
  enableable-tag rule.
- **Moves.** Archetype transitions and swap-remove copy the entity's ticks with it
  (as `SetDisabled` copies bits at ArchetypeChunkPool.hpp:252,391) and stamp the
  destination chunk's version for every column the entity carries. A newly added
  component gets `added = changed = CurrentTick()`.
- **Serialization.** Not written (archive format stays v5). On `Deserialize` every
  restored chunk's versions and every tracked entity's ticks are set to the current
  tick: everything reads as changed once after a load.

### 3.3 Write side

| Path | Chunk version | Entity ticks (tracked types) |
|---|---|---|
| `View::ForEach`/`ParallelForEach`, non-const `T` | stamp per visited chunk (before iterating it) | via `Mut<T>` conversion/`.Write()`; untouched by `.Read()` |
| `View::Get`/`Single` tuples | stamp the entity's chunk on a non-const request | `Mut<T>` in the tuple for tracked `T` |
| `Registry::GetComponent<T>` non-const | stamp entity's chunk | mark entity |
| `Registry::GetComponent<T>` const | none | none |
| `Set`/`Emplace`/`Add`/`Replace`, Commands apply | stamp | `added` (on add) and `changed` |
| `Registry::Modified<T>(e)` | stamp | mark |
| `Registry::SetIfNeq<T>(e, value)` | only if `!=` | only if `!=` |
| `Deserialize`, archetype move | stamp destination | copy (move) / current tick (load) |
| `SetEnabled` | none (a bit, not a value) | none |

`Mut<T>` (Query.hpp):

```cpp
template<typename T> class Mut {
public:
    operator T&() noexcept;          // marks changed, returns the reference (legacy lambdas)
    T& Write() noexcept;             // marks changed
    const T& Read() const noexcept;  // never marks
    T* operator->() noexcept;        // marks
    bool SetIfNeq(const T& v);       // marks only when v != current
    ASTRA_NODISCARD bool IsAdded(Tick since) const noexcept;
    ASTRA_NODISCARD bool IsChanged(Tick since) const noexcept;
};
```
Marking = `ticks[i].changed = thisRun` (one store; the chunk version was already
stamped when the chunk was entered). `Mut<T>` is handed out only for tracked `T`
requested non-const; untracked `T` keeps receiving `T&` exactly as today.

### 3.4 Read side: `Changed<T>` / `Added<T>`

Query modifiers next to `With<T>`/`Not<T>` (Query.hpp:308-356). They require `T`
(match-only like `With`), contribute nothing to `ViewAccess`, and carry a tick:

```cpp
auto v = ctx.GetRegistry().CreateView<const WorldTransform, Changed<WorldTransform>, SpriteRenderer, Not<Hidden>>();
v.ForEach(ctx, [](const WorldTransform& wt, SpriteRenderer& sr) { ... });   // since = ctx.LastRun()
v.Since(tick).ForEach(...);                                                   // explicit tick (Registry&-only systems)
```

Evaluation per chunk, gated `if constexpr (HasChangeFilter)` like `HasEnabledFilter`:
1. **Chunk reject:** for every `Changed<T>`/`Added<T>` term, if `!IsNewer(version[T], since)` skip the chunk.
2. **Untracked `T`:** yield every (enabled) entity in the chunk -- chunk granularity, documented.
3. **Tracked `T`:** run-scan the tick column (`IsNewer(ticks[i].changed, since)` /
   `.added`), unioned with the enabled-run scan when both filters are present, so the
   per-entity branch is taken in runs (`ForEachEnabledRun` shape, EnabledRuns.hpp:29).

`Added<T>` on an untracked `T` is chunk-granular as well (any add stamps the chunk).
Removal detection (`RemovedComponents`) is out of scope; `Signal::ComponentRemoved`
already exists.

### 3.5 Scheduler and SystemContext

- `SystemMetadata` gains `Tick lastRun = 0`. Executor: `thisRun = reg.AdvanceTick()`;
  `SystemContext ctx{reg, commands, lastRun, thisRun}`; invoke; `lastRun = thisRun`.
- Invocation preference: if `system(SystemContext&)` is well-formed, call it; else the
  existing `system(Registry&)`. The `System` concept becomes "either form".
- `ForEach(ctx, fn)` overloads on views read `ctx.LastRun()`; plain `ForEach(fn)` on a
  view with a change filter and no `Since()` is a compile-time error naming `Since`.

### 3.6 Error handling and edges

- Tick 0 means "never": a fresh chunk/entity is not "changed since 0" until stamped;
  `LastRun()==0` on a system's first run makes every stamped chunk match (first run
  sees everything), which is the Bevy semantic.
- Wraparound: `IsNewer(a,b) = int32_t(a-b) > 0`; a system that has not run for 2^31
  system runs misreads. Documented; a periodic clamp is a follow-up if ever needed.
- A view that writes `T` stamps chunks even when the lambda writes nothing: accepted
  false positives at chunk granularity (DOTS accepts the same). Declare read-only views
  const to avoid it. Arcane's `RenderSubmissionSystem` view must become const before
  adoption (it binds non-const today while declaring `Reads<...>`).
- `Mut<T>` implicit conversion marks even when the caller only read: accepted false
  positive at entity granularity (Bevy accepts the same); use `.Read()` when it matters.
- Raw pointers held across frames are the caller's responsibility: call `Modified<T>`.
- `ParallelForEach`: chunk stamps happen on the worker that visits the chunk (a plain
  store to a chunk-local word, no contention); entity ticks are per-slot stores.
  `AdvanceTick` is never called concurrently (scheduler-serialised).
- Enableable interplay: disabled entities are not visited, so they are never marked by
  a view; `SetEnabled` does not stamp.
- Two systems in one frame: because ticks advance per system run, B running after A
  sees A's writes as changed; A on the next frame does not see its own writes again.
- Compile-time refusals: `Changed<T>` where `T` is not in the view's required set;
  `Mut<T>` requested for an untracked type (use `T&`); tracked tag type.

### 3.7 API changes

- `+ Registry::CurrentTick/AdvanceTick/Modified<T>/SetIfNeq<T>`; `+ Tick`.
- `+ SystemContext::LastRun/ThisRun`; `SystemMetadata::lastRun`; executor preference
  for `SystemContext&`; `System` concept widened (additive).
- `+ Changed<T>`, `Added<T>`, `Mut<T>`, `View::Since(Tick)`, `View::ForEach(ctx, fn)`.
- `+ ChangeTrackedTraits<T>` / `IsChangeTrackedV<T>` / `AstraChangeTracked`.
- Chunk: `+ m_columnVersion`, `+ tracked tick columns`, `ArchetypeColumnMeta::trackedColumns`.
- Behaviour change: a non-const view over a tracked type receives `Mut<T>` (source
  compatible through the implicit conversion). Archive format unchanged.
- `Signal::ComponentUpdated` stays unemitted (observers are not this stage).

### 3.8 Non-goals

Removal buffers; per-field change detection; serialized ticks; observers/reactive
queries; automatic `SetIfNeq`; any Arcane change (adoption happens together with
module residency in one later Arcane movement, per the user's 2026-09-10 decision).

## 4. Testing

Reuse existing test types; new tracked types cost ComponentIDs, so at most two new
reflected+tracked test components (`Astra::Test::TrackedPos`/`TrackedVel`), budgeted
against the 192 ceiling.

1. Tick: `AdvanceTick` monotonic; `IsNewer` wraparound at the 2^31 boundary.
2. Chunk versions: non-const view stamps every visited chunk; const view stamps none;
   `Set`/`Emplace`/`Add`/Commands stamp the entity's chunk; move stamps destination.
3. Tracked ticks: `Mut<T>` conversion marks, `.Read()` does not, `SetIfNeq` marks only
   on change; non-const `GetComponent` marks, const does not; `Modified` marks.
4. Filters, untracked type: `Changed<T>` yields all entities of stamped chunks and
   none of unstamped; `Since()` explicit tick; first run sees everything.
5. Filters, tracked type: per-entity precision inside a stamped chunk; `Added<T>` only
   for entities added since; interplay with enabled filtering (union run scan).
6. Scheduler: two systems in one frame, A writes then B reads `Changed` (sees), A next
   frame (does not see its own); `SystemContext&` preferred over `Registry&`.
7. Moves: ticks travel with the entity across archetypes and swap-remove; a deserialized
   registry reads everything as changed exactly once.
8. Zero cost: a view with no change filter instantiates no filter path (static_assert
   on the trait); untracked type carves no tick column (`IsTracked == false`).
9. Bench (Dist, `bench-compare` recipe): iterate2 with a non-const view before/after
   (chunk stamp must be within noise); `Changed<T>` at 0/10/50/100% changed with and
   without tracked precision; record in RESULTS.md.
10. Acceptance (Arcane-shaped, in Astra): a "transform propagation" style test on
    1M entities where 5% change per frame; the `Changed<T>` version visits ~5% of
    chunks and matches the brute-force result.

Gates: TDD per task, three-config green, sanitizer lane on the next push, opus
whole-branch review (chunk layout + scheduler + iteration), FF-merge to dev, no push.

## 5. Sequencing

One Astra movement (plan via writing-plans, fresh session): (1) `Tick`, Registry
counter, `SystemContext` ticks, scheduler preference + `lastRun`; (2) chunk versions +
stamping on every write path + view-entry stamping; (3) `Changed/Added` filters with
chunk reject (untracked granularity) + `Since`/`ForEach(ctx)`; (4) opt-in trait, tick
columns, moves/deserialize; (5) `Mut<T>`, marking accessors, `Modified`/`SetIfNeq`,
per-entity filter tier; (6) benchmarks + acceptance test + docs. Arcane adoption
(change detection + module residency together, ABI bump, delete PreviousTransform and
the shadow arrays, const the render view) is a separate movement afterwards.

## 6. Implemented as (deviations and controller rulings recorded during Tasks 1-9)

The plan's Global Constraints flagged five deviations from this spec's literal text ahead of
implementation; all five shipped exactly as flagged, and are recorded here against the sections
they touch. Five further controller rulings were made during implementation to resolve cases this
spec under-specified, and two more (M, O) in the post-review fix wave; they are additive
clarifications, not deviations from any explicit rule above.

**Deviation 1 (§3.1/§3.5 — Task 5).** Tick advance is once per parallel execution-plan GROUP, not
once per individual system run. Systems within one group have no read/write conflict by
construction (that is what makes them a group), so ordering among them is undefined regardless;
sharing one tick keeps `CurrentTick()` exact for every view-entry stamp taken while the group runs
concurrently. A per-system atomic tick would let a sibling system's tick leak into another
system's own stamps within the same group and make a system see its own writes on its very next
run — worse, not better, than the group-shared tick. Every observable guarantee in §3.6 holds
under this scheme.

**Deviation 2 (§3.2 — Tasks 2/6).** Chunk versions (`Tick* m_columnVersion`) live in the chunk
arena as specified; tracked per-entity tick pointers (`EntityTicks* Column::ticks`) live on
`Column` rather than being carved as a second arena region — mirrors exactly how
`Column::disabledWords` already works for enableable components, at the same accepted +8-bytes-
per-`Column`-slot fixed-metadata cost the enableable feature already pays.

**Deviation 3 (§3.4/§3.6 — Task 4).** `Changed<T>`/`Added<T>` are filter-only AND iteration-only
this stage: `T` must also be listed as a required term (bare/`const`/`IncludeDisabled`) in the
same view — hence the §3.4 example above now reads `CreateView<const WorldTransform,
Changed<WorldTransform>, SpriteRenderer, Not<Hidden>>`, not the original text's
`CreateView<Changed<WorldTransform>, ...>` (which named `wt` in the lambda with nothing in the
view actually fetching it). `Size`/`Empty`/`Contains`/`Get`/`Single`/range-for on a
change-filtered view are all compile-time refused (each would need a tick with no natural source);
use `ForEach(ctx, fn)` or `Since(tick).ForEach(fn)`. Tag types are refused for the same reason
§3.2 gives: no column, no version to compare.

**Deviation 4 (§3.2 — Tasks 2/6).** Same-archetype relocations (`CompactChunks`,
`MoveEntitiesBetweenChunks`) fold the *newer* of the source/destination column version and copy
per-entity ticks across, rather than stamping with the tick current at compaction time. Stamping
with "now" would be a false negative: an entity written at tick 100 and later compacted into a
fresh (version-0) chunk would read as unchanged to a reader whose last run was tick 90, which §3.6
explicitly forbids. Folding the newer version closes that gap.

**Deviation 5 (§3.3/§3.6 — Task 7).** `Optional<T>` for a tracked, non-const `T` marks the entity
unconditionally when the optional is present, rather than only when the caller actually writes
through it. The yield is a raw `T*` (no `Mut<T>` wrapper is possible through a bare pointer), so
there is no conversion hook to gate the mark on real use — the same class of accepted false
positive §3.6 already documents for `Mut<T>`'s implicit conversion. Request `Optional<const T>`
to read a present-but-tracked optional without marking it.

**Ruling E (Task 7 fix round 1 — §3.3 row 2, `Single()`).** `View::Single()`'s internal
count/locate pass, which walks every candidate chunk to determine Empty vs. exactly-one vs.
MultipleMatched, runs completely unstamped and unmarked at both tiers — counting is not a write.
Only the one entity actually returned gets stamped/marked, and that happens through the ordinary
`Get(entity)` call `Single()` makes once it has its answer. Nothing is touched on the `Empty` or
`MultipleMatched` paths, and no chunk other than the returned entity's is touched on success.

**Ruling F (§3.3 row 1, enableable-filtered `ForEach`).** A stamped, non-const `ForEach` over a
view that is also enabled-filtered stamps every chunk it *visits*, including a chunk whose
enabled-entity intersection for that pass turns out to be empty — matching the spec's literal
"every chunk it visits" at chunk granularity, not per actually-yielded run. A follow-up may narrow
this to stamp only on the first non-empty run within a chunk; not done in this stage.

**Ruling G (§3.5, scheduler).** `SystemScheduler::Execute` advances the tick once more per
execution-plan segment *after* the segment's executor has returned and *before* the deferred
command-buffer flush for that segment. Without this, a command flushed after a segment — or any
write the caller's own code makes to the registry once `Execute()` has returned — could read as
not-newer-than the `lastRun` the segment's systems just recorded, a false negative §3.6 does not
sanction. The extra advance guarantees flushed commands and any post-`Execute()` write are always
newer than every system's `lastRun`.

**Ruling H (§3.5, scheduler).** At the start of every `Execute()` call, if a system's cached
`lastRun` is not strictly older (`IsNewer`) than the registry's current tick, `Execute()` resets
every system's `lastRun` to 0 before running. This closes a false-negative gap the pointer-identity
registry-switch check alone cannot catch: a `Registry` destroyed and a new one constructed at the
same memory address (or simply restarted at tick 1, the common case for two freshly-constructed
registries) would otherwise leave every cached `lastRun` looking newer than or equal to the new
registry's ticks, silently hiding the new registry's first frame of changes from every system.
Known residual (final review): a registry re-created at the same address that has ALREADY been
ticked past every cached `lastRun` by another driver before this scheduler runs again evades both
the pointer-identity check and this monotonicity check; closing it needs a registry generation id
(follow-up), not a tick comparison.

**Ruling I (§3.6, iteration).** Range-for over a view that yields a change-tracked component
non-const is a compile-time error, not a silent under-report. The range-for iterator hands back a
plain `T&` (unlike `ForEach`, which hands out `Mut<T>`) and has no hook on which to mark a change,
so — rather than let a tracked type go through range-for and quietly never mark — it is refused at
compile time, naming `ForEach` (yields `Mut<T>`) or a `const T` request as the fix. A range-for
over `const T` (tracked or not) is unaffected and marks nothing, by design.

**Ruling M (§1/§3.3, Relations traversals — final review Important #1, fix wave).** The write-path
inventory in §3.3 missed one path that hands out live memory: `Registry::GetRelations<C...>(e)`'s
`ForEachChild`/`ForEachDescendant`/`ForEachAncestor`/`ForEachLink` and `ParallelForEachDescendant`
yield every required `C` as `C&`. They now yield through the same rule as views: a non-const,
non-tag `C` is fetched via `ArchetypeManager::GetComponentMut<C>` (stamps `C`'s column version in
the entity's chunk; marks the entity's `changed` tick when `C` is change-tracked), a `const C`
or a tag through the plain non-stamping read. Cost: one extra record lookup per non-const yield
on a cold path; the parallel variant issues the same stores from workers (same-value column
stamps, one owner per entity's ticks — the pattern `View::ParallelForEach` already relies on).
The table in §3.3 should be read with the row "Relations traversal yields `C&`: stamps (and marks
a tracked `C`); `const C&`: never". A raw pointer or reference kept past the traversal still
needs `Registry::Modified<T>` for a later write, as with views.

**Ruling O (§2.7 zero-cost, `EntityLocation` — perf triage, fix wave).** The
`add_component`/`remove_component` regression the bench gate recorded (+8 ns / +2.7 ns per op
against the branch base) was not the stamp stores: the stamp + per-entity tick init pushed
`Archetype::AllocateEntitySlot` past MSVC's inline budget at both hot callers, and once out of
line the `EntityLocation` it returns — a struct with user-provided constructors — came back
through memory as two 4-byte stores that the caller's 8-byte reload could not store-forward.
Mitigation: `EntityLocation` is now an aggregate (default member initialisers supply the
sentinel; `Create` brace-initialises; layout, accessors, comparisons unchanged; `static_assert`s
pin aggregate/trivially-copyable/8 bytes), so it is returned in a register with one store.
Measured on the reduced harness: add 48.7 → 43.9 ns, remove 29.8 → 28.9 ns. Residual ≈ +2.7 ns
(+7 %) on add and ≈ +2.4 ns on remove vs the branch base is the out-of-line call plus the stamp
instructions themselves — the structural cost of "stamp every column of the destination chunk on
every structural move" — and stays. Not done, by ruling: no `ASTRA_FORCEINLINE` on
`AllocateEntitySlot` (it only relocates the out-of-line boundary and cascades), no hoisting of the
stamp loop's locals (MSVC turns the loop into a `rep stosd` fill, +12–15 ns), no per-chunk
stamp memo (buys 0–2 ns and adds a tick-wrap corner case).
