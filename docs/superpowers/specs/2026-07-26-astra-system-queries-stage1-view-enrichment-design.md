# Bevy-style System Queries — Stage 1: View Enrichment — Design (2026-07-26)

**Status:** user-approved design (this session).

**Goal:** Deliver the query-ergonomics foundation of the Bevy-parity system API by
**enriching the existing `View`** (no new type) with (a) a match-only `With<T>` filter,
(b) a compile-time read/write **access-harvesting trait** the scheduler will consume,
(c) filter-aware **random access** (`Get`/`Single`/`Contains`), and (d) **entity-optional
iteration** — all **purely additive**, with every existing `View`/lambda-system path
byte-identical in behavior. This is the north-star "ergonomics of Bevy" pillar,
built on the machinery that already exists (matched-archetype cache + incremental refresh
+ enableable-component filtering).

## 0. Program context — this is Stage 1 of 3

The full "Bevy-style system queries + auto-derived access" program was scoped this session
into three dependency-ordered sub-projects, each its own spec → plan → SDD → review →
merge (three-config green before the next begins):

| Stage | Sub-project | Depends on | Delivers |
|---|---|---|---|
| **1 (this spec)** | **View enrichment** — `With<T>`, access-harvesting trait, `Get`/`Single`/`Contains`, entity-optional `ForEach` | `View` (exists) | The query surface + the scheduler-facing access seam. Buildable + testable standalone via `CreateView<…>`. |
| 2 | **SystemParam binder** — register a plain function whose params are `View<…>` / `Res<T>` / `ResMut<T>` / `Commands`; union access → `SystemMetadata` → scheduler; per-frame param construction with persistent per-system state (view caching) | Stage 1 | "Auto-derived access across all params feeds the scheduler." |
| 3 | **Change detection** — per-component change ticks in chunk storage, `Changed<T>`/`Added<T>` filters, `Ref<T>`/`Mut<T>` | Stage 1 (filters) | Reactive systems. Heaviest; touches storage + serialization. |

Stage 1 introduces **no scheduler change and no new system-registration path.** It only
makes `View` express read/write intent and support random access. The access-harvesting
trait (§4) is built and unit-tested here but has no consumer until Stage 2.

## Evaluated alternatives (recorded, this session)

- **A separate `Query<…>` type distinct from `View`.** Declined (user): the two would be
  the same machinery. `Query` is not a second storage mechanism — it is `View` plus a
  data/filter distinction and random access. Enriching `View` in place avoids a second
  concept and a parallel API to keep in sync. `View` remains the one query type; the
  primitive/facade split lives in *usage*, not in *types*.
- **`Query` replaces `View` (rename/demote).** Declined: large breaking change across the
  lambda-system path and all existing code, for no capability gain.
- **`Get<T>` data-wrapper syntax** (`CreateView<Get<Position>, Get<const Velocity>, …>`).
  Declined: `View` already encodes read/write via `const`-qualification of the bare type
  arg (the shipped `LambdaSystemWrapper` path relies on it — see §2). A `Get<>` wrapper
  would be redundant ceremony on every component.
- **Bevy tuple syntax** (`CreateView<Data<Position&, const Velocity&>, Filter<…>>`).
  Declined: more verbose, references-in-type-lists, and again redundant with `View`'s
  existing const convention.
- **`using Query = View<…>` familiarity alias.** Declined (user): two names for one type
  is confusing. Vocabulary stays `View` everywhere. (The file `Query.hpp` keeps its name —
  it only holds the internal `QueryBuilder`/`QueryClassifier`; there is no user-facing
  `Query` type.)
- **A `Without<T>` alias for the existing `Not<T>`.** Declined: `Not<T>` already ships
  across the codebase and tests; aliasing it is the same two-names-one-concept confusion.
  The filter pair is **`With<T>` / `Not<T>`** (`With` is genuinely new; `Not` is untouched).

## 1. Scope (user decisions, this session)

| Decision | Choice |
|---|---|
| Program shape | **Full multi-param Bevy systems**, delivered as **3 staged specs**; this is **Stage 1** |
| Query vs View | **Enrich `View` in place** — no separate `Query` type, no rename |
| Data spelling | **`View`'s existing const convention**: bare `T` = write (`T&`), `const T` = read (`const T&`) |
| New filter | **`With<T>`** (match-only, not yielded, zero scheduling footprint); pairs with existing `Not<T>` |
| Random-access returns | **`Result<std::tuple<…>, QueryError>`** for both `Get` and `Single`; `Contains` → `bool` |
| Entity in body | **Entity-optional `ForEach`** — callback may include or omit the leading `Entity` |
| Naming | **No `Query` type, no `Query`/`Without` aliases** |
| Access harvesting | **Built + unit-tested in Stage 1**; **consumed in Stage 2** (no scheduler change now) |

## 2. Spelling — `View`'s existing const convention (verified)

No new spelling is introduced. `View` type args are bare component types; `const`
qualification encodes read intent:

```cpp
auto v = reg.CreateView<Position, const Velocity, With<Player>, Not<Frozen>, Optional<Health>>();
//                      write     read            match-only    exclude      yields Health*
v.ForEach([](Position& p, const Velocity& v, Health* h) { p.x += v.dx; });
```

- `Position` (bare) → yields `Position&` (write).
- `const Velocity` → yields `const Velocity&` (read).
- `With<Player>` → matched, **not** yielded (§3).
- `Not<Frozen>` → excluded (existing modifier).
- `Optional<Health>` → yields `Health*`, null when absent (existing modifier).

**Verified load-bearing fact:** `TypeID<T>` defines `using Type = std::decay_t<T>`
(`Core/TypeID.hpp`), so `TypeID<const Velocity>::Value()` resolves to the **same
`ComponentID`** as `Velocity` — column resolution is const-agnostic; the `const` only
rides through to the yielded reference type. The shipped `LambdaSystemWrapper` already
constructs `CreateView<const BaseType<Components>…>()`, proving const-qualified args work
end-to-end today. Stage 1 adds no new mechanism here; it only starts **harvesting** the
const-ness (§4).

## 3. `With<T>` — match-only filter (entirely in `Query.hpp`)

`With<T>` requires `T` on the archetype for matching but never enters the yielded set and
carries **zero access footprint** for scheduling (§4). Touch-points, all in
`include/Astra/Registry/Query.hpp`:

1. **`With<T>` struct** beside `Optional`/`Not`/`Any`/`OneOf`/`IncludeDisabled`, with the
   same `static_assert(Component<T>, …)`.
2. **`Detail::IsModifier<With<T>> → true`** — so it passes `ValidQueryArg` and is not
   double-counted as a bare required/yielded component by `ExtractComponents` /
   `GetRequired`.
3. **`Detail::ExtractComponent<With<T>> → T`** — so `T` joins `AllComponents` for the
   uniqueness bookkeeping.
4. **New `WithComponents` classifier category** in `Detail::QueryClassifier` via the
   existing `FilterByModifier<With, …>` mechanism.
5. **`QueryBuilder` gains `GetWithMask()`**, and **`QueryBuilder::Matches` requires it**:
   `archetypeMask.HasAll(GetRequiredMask() | GetWithMask())` (the exclude/Any/OneOf checks
   are unchanged).

**`View.hpp` needs no iteration change:** `With<T>` never appears in `RequiredTypes` (the
yielded tuple) or `OptionalTypes`, so `ForEachImpl`/`VisitChunkFiltered` are untouched;
matching already routes entirely through `QueryBuilder::Matches` inside
`CollectArchetypes`/`EnsureArchetypes`.

**Enableable interaction:** a `With<T>` where `T` is enableable is **not** enabled-filtered
in Stage 1 — `With` expresses presence-of-column, and the enabled-only filter set is built
from *required/optional* enableable components (`EnableableRequiredFilter` /
`FilterEnableable`), which `With` deliberately does not join. (A future "must be present
**and enabled**" filter is out of scope; recorded in §8.)

## 4. Read/write access-harvesting trait (built now, consumed in Stage 2)

A compile-time trait over a `View` type that yields its read and write component sets.
Lives with `View`/`Query.hpp` detail (exact location pinned by the plan).

- **Reads** = `const`-qualified data args **only** (`const T` → read of `T`), plus
  `const`-qualified `Optional<const T>`.
- **Writes** = non-`const` data args (`T` → write of `T`), plus non-`const`
  `Optional<T>`.
- **`With<T>` and `Not<T>` contribute NOTHING** — matching-only, zero read and zero write.
  This is the load-bearing property: a system that only *filters* on a component is never
  serialized against a system that writes it (Bevy/DOTS-correct — the reason `With` exists
  over include-and-ignore).

Shape (illustrative; final names pinned by the plan):

```cpp
template<class V> struct ViewAccess {
    using Reads  = /* tuple of decayed read component types  */;
    using Writes = /* tuple of decayed write component types */;
    static ComponentMask ReadMask();   // built like SystemScheduler::ExtractComponentMask
    static ComponentMask WriteMask();
};
```

Masks are built at runtime from the type-lists exactly as
`SystemScheduler::ExtractComponentMask` does today (component IDs are runtime-assigned),
so this trait produces **type-lists** plus thin mask builders; nothing is `constexpr`
that cannot be. Stage 1 verifies the trait by unit test only (no scheduler wiring).

## 5. Random access — `Get` / `Single` / `Contains`

All three are **filter-aware**: they honor required + `With` presence, `Not` exclusion,
and (when an enableable filter is active on the view) enabled-only visibility, so they
never disagree with `ForEach`/`Size`.

```cpp
enum class QueryError { NotMatched, Empty, MultipleMatched };

// Err(NotMatched) when the entity is absent/dead, lacks a required or With component,
// has an excluded component, or is disabled under this view's enabled filter.
Result<std::tuple<…>, QueryError> Get(Entity e);

// The single matching entity. Err(Empty) if zero match, Err(MultipleMatched) if >1.
Result<std::tuple<…>, QueryError> Single();

// Filter-aware presence; no data returned.
bool Contains(Entity e) const;
```

- **Yielded tuple mirrors `ForEach`:** required → refs (`const T&` / `T&`), `Optional<T>`
  → `T*`. `With`/`Not` never appear in the tuple.
- **`Get` mechanism:** resolve the entity's chunk + index via the existing
  `EntityRecord`/`GetComponent` path (one lookup — W1 + Lever 1 give a direct chunk
  pointer), test the entity's archetype mask against the view's required+With / excluded
  masks, apply the enabled filter if present, then materialize the tuple. Single filtered
  lookup vs today's N separate `GetComponent<T>` calls with **no** filter awareness (a
  `Frozen` entity currently still hands you its `Position`).
- **`Single` mechanism:** walk matched archetypes; first visible entity is the candidate;
  a second visible entity → `Err(MultipleMatched)`; none → `Err(Empty)`. Respects the
  enabled filter for the count.
- **Cross-compiler fallback (spec'd, not a surprise):** the yielded type is
  `std::tuple<refs…>` inside `Result`. If a reference-holding tuple inside `Result`
  misbehaves on any of the three configs (MSVC / clang / gcc), the defined fallback is
  `std::tuple<T*…>` (pointers; `Get` still returns `Err(NotMatched)`, `Single` still
  returns its errors). The plan chooses one and states it; behavior is identical, only the
  accessor spelling at the call site differs.

## 6. Entity-optional `ForEach`

Today `View::ForEach` always invokes `func(entity, comps…)`, forcing `[](Entity, …)`.
Stage 1 makes the leading `Entity` optional:

```cpp
v.ForEach([](Position& p, const Velocity& vel) { … });          // NEW: no Entity
v.ForEach([](Entity e, Position& p, const Velocity& vel) { … }); // existing, unchanged
```

- **Detection:** at the `ForEach` call site, prefer the entity-taking form when
  `std::invocable<Func, Entity, Comps…>` holds; otherwise use `std::invocable<Func,
  Comps…>`; otherwise a `static_assert` names the two accepted shapes. `Optional<T>`
  contributes a `T*` argument in both shapes.
- **Purely additive:** every existing `[](Entity, …)` body compiles and behaves
  identically; the entity-taking overload is preferred so no current call site changes
  meaning.
- **Applies to both `ForEach` and `ParallelForEach`** — the two plain iteration surfaces —
  so their callback contract stays uniform (a body written for one works on the other).
  `ParallelForEachWithContext` is **excluded** (§8): it threads a `SystemContext` sub-arg,
  so its callback shape is governed by `SystemContext`, not by this rule.
- This is the "entity-optional ForEach" ergonomics item recorded in the perf-optimization
  notes.

## 7. Zero-cost / additive invariants

1. A view with **no `With<T>`** compiles and iterates byte-identically to today (the
   `WithComponents` category is empty; `GetWithMask()` returns an empty mask; the extra
   `HasAll` term is against a zero mask).
2. The **existing lambda-system path** (`LambdaSystemWrapper`) is untouched and unaffected
   — it derives its own access from lambda params and calls `ForEach(entity, …)`, which
   the entity-optional overload still serves via the entity-taking form.
3. The **access-harvesting trait has no runtime presence** until Stage 2 wires it; it adds
   no cost to any current path.
4. `Get`/`Single`/`Contains` are **new surface** — they cost nothing unless called.
5. No change to `EntityRecord`, chunk layout, serialization format, or the scheduler in
   Stage 1.

## 8. Out of scope (recorded)

- The SystemParam binder, `Res<T>`/`ResMut<T>`/`Commands`, multi-param system registration
  (Stage 2).
- Change detection: `Changed<T>`/`Added<T>`/`Ref<T>`/`Mut<T>` and per-component change
  ticks (Stage 3).
- Any scheduler change or auto-parallelization from harvested access (Stage 2 consumes the
  trait).
- "Present **and enabled**" `With`-style filter for enableable components; disabled-only
  filters.
- `ParallelForEachWithContext` entity-optional support (it threads a sub-context; the
  callback shape is governed by `SystemContext`).

## 9. Testing plan

**TypeID budget: reuse existing `Astra::Test::*` component types** (`tests/TestComponents.hpp`)
— the test binary sits near the 128-TypeID ceiling; adding fresh component types is a
disclosed deviation only.

1. **`With<T>` matching:** entities with `T` match; without `T` do not; `T` is **not**
   yielded (callback arity/type check); `With<T>` + required + `Not<U>` combined; `With<T>`
   on a zero-size tag; multiple `With` in one view (AND semantics).
2. **Access-harvesting trait:** `ViewAccess<View<Position, const Velocity, With<Player>,
   Not<Frozen>, Optional<const Health>>>` produces Reads `{Velocity, Health}`, Writes
   `{Position}`, and `With`/`Not` contribute nothing (mask equality assertions).
3. **`Get`:** match returns the tuple with correct ref/ptr shape and identity (pointer
   equals `GetComponent`); non-match (missing required, missing `With`, has excluded,
   dead entity) → `Err(NotMatched)`; `Optional` present vs absent → non-null vs null;
   disabled-under-filter → `Err(NotMatched)`.
4. **`Single`:** zero → `Err(Empty)`; exactly one → `Ok(tuple)`; two+ → `Err(MultipleMatched)`;
   enabled filter reduces the count (disabled extras don't trip `MultipleMatched`).
5. **`Contains`:** true/false mirrors `Get` success/failure without materializing data;
   filter-aware.
6. **Entity-optional iteration (`ForEach` and `ParallelForEach`):** both callback shapes
   visit the identical entity set (identical order for `ForEach`); the same body works on
   both methods; `Optional<T>` arg present in both shapes; a body matching neither shape
   fails the `static_assert` (compile-time check per the suite's idiom).
7. **Additive/zero-cost:** a `With`-free view's collected-archetype set and visit order
   are unchanged (regression against an equivalent pre-change view).
8. **Three-config green** (Debug / Release / Dist). No bench gate required — Stage 1 adds
   no instructions to any measured path (§7); a spot-check that the flat-watch set is
   unchanged is optional/informational.

## 10. Binding invariants (for implementers and reviewers)

1. **Additive only:** no existing `View`/lambda-system behavior changes; a `With`-free
   view is byte-identical to today.
2. **`With<T>` is matched, never yielded, and has zero scheduling access footprint**
   (contributes to neither Reads nor Writes in `ViewAccess`).
3. **Filter-aware random access:** `Get`/`Single`/`Contains` agree with `ForEach`/`Size`
   on the visible-entity set, including enableable-disabled filtering.
4. **Const-agnostic column resolution:** read/write is a property of the yielded reference
   only; `TypeID<const T>` and `TypeID<T>` resolve to the same column (verified §2).
5. **`Result<tuple, QueryError>` for `Get` and `Single`;** `bool` for `Contains`; no
   exceptions, no panics (safety-first lane).
6. **No new `Query` type, no `Query`/`Without` aliases;** vocabulary is `View`, filter
   pair is `With`/`Not`.
7. **Access-harvesting trait is inert in Stage 1** — built and unit-tested, consumed only
   in Stage 2; adds no runtime cost now.
8. **TypeID budget:** reuse existing test component types; a new type is a disclosed
   deviation.
