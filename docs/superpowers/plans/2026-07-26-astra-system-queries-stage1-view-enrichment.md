# Bevy-style System Queries — Stage 1: View Enrichment — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enrich the existing `View` with a match-only `With<T>` filter, a compile-time read/write access-harvesting trait, filter-aware random access (`Get`/`Single`/`Contains`), and entity-optional iteration — all purely additive.

**Architecture:** `With<T>` is a new query modifier handled almost entirely in `Query.hpp` (classifier + matching); it never enters the yielded set. A `ViewAccess<View<…>>` trait classifies the view's type args into read/write component sets (with `With`/`Not` contributing nothing) for Stage 2's scheduler. Random access reuses the existing `EntityRecord → Archetype::GetComponent(location)` path plus `QueryBuilder::Matches` and the chunk disabled-bit filter, returning `Result<tuple, QueryError>`. Entity-optional iteration wraps the user callback in a generic adapter that forwards `Entity` only when the callback accepts it.

**Tech Stack:** C++20 header-only; GoogleTest; premake5 + make/MSBuild; `Astra::Result<T,E>`; existing `View`/`Query.hpp`/`Archetype` machinery.

## Global Constraints

- **C++20, header-only.** All changes are in `include/Astra/**` headers. No new .cpp in the library.
- **Purely additive / zero-cost when unused.** A `With`-free view must compile and iterate byte-identically to today; existing `View`/`LambdaSystemWrapper` paths unchanged.
- **Safety-first lane:** no exceptions, no panics. Fallible results use `Astra::Result<T,E>` (`Result.hpp`: `Ok(v)`/`Err(e)`/`IsOk()`/`IsErr()`/`GetValue()→T*`/`GetError()→E*`).
- **Three-config green:** Debug / Release / Dist must all build and pass (`config=debug|release|dist`).
- **TypeID budget:** reuse existing `Astra::Test::*` component types (`tests/TestComponents.hpp`): `Position`, `Velocity`, `Health`, `Transform`, `Name`. The test binary sits near the 128-TypeID ceiling — **do not register new component types.** Filter tests map roles onto existing types: data = `Position` (write) / `const Velocity` (read); `With<Transform>`; `Not<Name>`; `Optional<Health>`.
- **Determinism:** iteration visit order unchanged (chunk order).
- **No `Query` type, no `Query`/`Without` aliases.** Vocabulary is `View`; filter pair is `With<T>` / `Not<T>`.

## File Structure

- **Modify `include/Astra/Registry/Query.hpp`:** add `With<T>` struct + modifier hooks; `WithComponents` classifier category; `QueryBuilder::GetWithMask()` + `Matches` update; `Detail` access-classification helpers (`ArgAccess`, `AccessReads`/`AccessWrites`).
- **Modify `include/Astra/Registry/View.hpp`:** `ViewAccess<View<…>>` trait specialization; entity-optional `ForEach`/`ParallelForEach` adapter; `QueryError` enum; `Contains`/`Get`/`Single` + private `VisibleRecord`/`EnabledVisible`/`MakeAccessTuple` helpers.
- **Create `tests/Registry/ViewEnrichmentTest.cpp`:** all Stage 1 tests (one file, appended per task).
- **Regenerate project files** after the file is created (Task 1), so the glob picks it up.

---

### Task 1: `With<T>` match-only modifier + matching

**Files:**
- Modify: `include/Astra/Registry/Query.hpp`
- Create: `tests/Registry/ViewEnrichmentTest.cpp`

**Interfaces:**
- Consumes: existing `Detail::IsModifier`, `Detail::ExtractComponent`, `Detail::QueryClassifier::FilterByModifier`, `QueryBuilder::MakeMaskFromTuple`, `QueryBuilder::GetRequiredMask`, `QueryBuilder::Matches`, `CollectArchetypes`.
- Produces: `Astra::With<T>` modifier; `Detail::QueryClassifier<…>::WithComponents` (a `std::tuple<…>`); `QueryBuilder<…>::GetWithMask() → ComponentMask`. `Matches` now requires required∪with.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/ViewEnrichmentTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

#include "../TestComponents.hpp"

using Astra::Test::Position;
using Astra::Test::Velocity;
using Astra::Test::Health;
using Astra::Test::Transform;
using Astra::Test::Name;

// ---- Task 1: With<T> ------------------------------------------------------

TEST(ViewWith, MatchesRequirePresenceButDoNotYield)
{
    Astra::Registry reg;
    auto withT   = reg.CreateEntity<Position, Transform>();   // matches With<Transform>
    auto without = reg.CreateEntity<Position>();              // no Transform -> excluded
    (void)withT; (void)without;

    auto v = reg.CreateView<Position, Astra::With<Transform>>();

    // Callback receives ONLY Position (Transform is not yielded).
    size_t n = 0;
    v.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 1u);   // only the Position+Transform entity
}

TEST(ViewWith, CombinesWithNotAndRequired)
{
    Astra::Registry reg;
    auto ok      = reg.CreateEntity<Position, Transform>();
    auto frozen  = reg.CreateEntity<Position, Transform, Name>();  // excluded by Not<Name>
    auto noWith  = reg.CreateEntity<Position>();                   // excluded by With<Transform>
    (void)ok; (void)frozen; (void)noWith;

    auto v = reg.CreateView<Position, Astra::With<Transform>, Astra::Not<Name>>();
    size_t n = 0;
    v.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 1u);   // only `ok`
}
```

- [ ] **Step 2: Run test to verify it fails**

Because the source file is new, first regenerate the project so the glob (`tests/**.cpp`) includes it:

Run (Windows): `scripts/generate_vs2022.bat` — or for the make build: `premake5 gmake2`
Then build+run:
Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewWith.*`
Expected: **FAIL to compile** — `Astra::With` is not a type (`With` undeclared).

- [ ] **Step 3: Implement `With<T>` in `Query.hpp`**

In `include/Astra/Registry/Query.hpp`:

(a) Add the forward declaration beside the other modifiers (near the top, with `Optional`/`Not`/`IncludeDisabled`/`Any`/`OneOf`):

```cpp
    template<typename T> struct With;
```

(b) Register it as a modifier — add beside the other `IsModifier` specializations:

```cpp
        template<typename T>
        struct IsModifier<With<T>> : std::true_type {};
```

(c) Extract its component — add beside the other `ExtractComponent` specializations:

```cpp
        template<typename T>
        struct ExtractComponent<With<T>>
        {
            using type = T;
        };
```

(d) Define the modifier struct beside the `Not`/`Optional` definitions (after the `Detail` block, where `Not<T>` is defined):

```cpp
    // Match-only filter: T must be present on the archetype for a match, but T is
    // NOT yielded to the callback and carries zero access footprint for scheduling
    // (ViewAccess ignores it). The positive twin of Not<T>.
    template<typename T>
    struct With
    {
        static_assert(Component<T>, "With can only be used with valid components");
    };
```

(e) In `Detail::QueryClassifier`, add the `WithComponents` category beside `ExcludedComponents`:

```cpp
            using WithComponents     = typename FilterByModifier<With, QueryArgs...>::type;
```

(f) In `QueryBuilder`, add `GetWithMask()` beside `GetExcludedMask()`:

```cpp
        // Match-only components (With<T>): required for matching, never yielded.
        static ComponentMask GetWithMask()
        {
            return MakeMaskFromTuple<typename Classifier::WithComponents>();
        }
```

(g) In `QueryBuilder::Matches`, require required∪with. Change the first check:

```cpp
            // Must have all required AND all With components
            if (!archetypeMask.HasAll(GetRequiredMask() | GetWithMask()))
                return false;
```

- [ ] **Step 4: Update `CollectArchetypes` lower-bound skip (same file family — `View.hpp`)**

In `include/Astra/Registry/View.hpp`, `CollectArchetypes()`, make the component-count lower bound include With components so the early skip stays correct:

```cpp
            const size_t queryComponentCount =
                (QueryBuilder::GetRequiredMask() | QueryBuilder::GetWithMask()).Count();
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewWith.*`
Expected: **PASS** (both `ViewWith` tests).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): add match-only With<T> query modifier (Stage 1 task 1)"
```

---

### Task 2: Entity-optional `ForEach` / `ParallelForEach`

**Files:**
- Modify: `include/Astra/Registry/View.hpp:109-201` (`ForEach`, `ParallelForEach`)
- Test: `tests/Registry/ViewEnrichmentTest.cpp` (append)

**Interfaces:**
- Consumes: existing `ForEachImpl`, `ParallelForEachChunkImpl`, all of which invoke `func(entity, comps…)`.
- Produces: `View::ForEach`/`ParallelForEach` accept callbacks of shape `(Entity, Comps…)` **or** `(Comps…)`.

- [ ] **Step 1: Write the failing test** (append)

```cpp
// ---- Task 2: entity-optional iteration ------------------------------------

TEST(ViewEntityOptional, ForEachWithAndWithoutEntityVisitSameSet)
{
    Astra::Registry reg;
    reg.CreateEntity<Position, Velocity>();
    reg.CreateEntity<Position, Velocity>();
    reg.CreateEntity<Position>();   // no Velocity: excluded from the view below

    auto v = reg.CreateView<Position, const Velocity>();

    size_t withEntity = 0, withoutEntity = 0;
    v.ForEach([&](Astra::Entity, Position&, const Velocity&) { ++withEntity; });
    v.ForEach([&](Position&, const Velocity&)               { ++withoutEntity; });

    EXPECT_EQ(withEntity, 2u);
    EXPECT_EQ(withoutEntity, withEntity);
}

TEST(ViewEntityOptional, ParallelForEachAcceptsEntityless)
{
    Astra::Registry reg;
    for (int i = 0; i < 10; ++i) reg.CreateEntity<Position, Velocity>();

    auto v = reg.CreateView<Position, const Velocity>();
    std::atomic<size_t> n{0};
    v.ParallelForEach([&](Position&, const Velocity&) { n.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(n.load(), 10u);   // sequential fallback (no scheduler injected) still runs the body
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewEntityOptional.*`
Expected: **FAIL to compile** — the entityless lambda is not invocable as `(Entity, Position&, const Velocity&)` and the current code has no adapter.

- [ ] **Step 3: Add the adapter helper to `View`**

In `include/Astra/Registry/View.hpp`, add a private static helper (near the other private helpers, e.g. above `ForEachImpl`):

```cpp
        // Wrap a user callback so it can be invoked as (Entity, Comps&...): if the
        // callback also accepts the leading Entity, forward it; otherwise drop it.
        // Additive: an existing (Entity, Comps...) body hits the first branch and is
        // called identically. Resolves once per fixed component pack (per view type).
        template<typename Func>
        ASTRA_FORCEINLINE static auto MakeEntityOptionalAdapter(Func&& func)
        {
            return [&func](Astra::Entity e, auto&&... comps)
            {
                if constexpr (std::is_invocable_v<Func&, Astra::Entity, decltype(comps)...>)
                    func(e, std::forward<decltype(comps)>(comps)...);
                else if constexpr (std::is_invocable_v<Func&, decltype(comps)...>)
                    func(std::forward<decltype(comps)>(comps)...);
                else
                    static_assert(sizeof(Func) == 0,
                        "View callback must be invocable as (Entity, Comps&...) or (Comps&...). "
                        "Component params must be 'T&' (write) or 'const T&' (read); "
                        "Optional<T> supplies a 'T*' argument.");
            };
        }
```

- [ ] **Step 4: Route `ForEach` and `ParallelForEach` through the adapter**

In `ForEach` (after `EnsureArchetypes()` / the empty check, before the archetype loop), build the adapter and pass it into the loop instead of `func`:

```cpp
            auto adapted = MakeEntityOptionalAdapter(func);
            for (Archetype* archetype : m_archetypes)
            {
                ForEachImpl(archetype, adapted, RequiredTypes{}, OptionalTypes{});
            }
```

In `ParallelForEach`, build `auto adapted = MakeEntityOptionalAdapter(func);` once after `EnsureArchetypes()`, and replace the `func` passed to the sequential fallbacks and to `ParallelForEachChunkImpl` with `adapted`. (The `MIN_ENTITIES_QUICK_CHECK` / null-scheduler / below-threshold fallbacks currently call `ForEach(std::forward<Func>(func))` — call `ForEach(adapted)` there so entity-optionality is preserved on the fallback path too; the scheduler dispatch lambda captures `adapted` and calls `ParallelForEachChunkImpl(archetype, chunkIndex, adapted, …)`.)

- [ ] **Step 5: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewEntityOptional.*:ViewWith.*`
Expected: **PASS** (new tests + Task 1 tests still green — regression check that the existing `(Entity, …)` shape is unaffected).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): entity-optional ForEach/ParallelForEach callbacks (Stage 1 task 2)"
```

---

### Task 3: `ViewAccess` read/write harvesting trait

**Files:**
- Modify: `include/Astra/Registry/Query.hpp` (per-arg classification in `Detail`)
- Modify: `include/Astra/Registry/View.hpp` (the `ViewAccess<View<…>>` specialization)
- Test: `tests/Registry/ViewEnrichmentTest.cpp` (append)

**Interfaces:**
- Consumes: `Astra::MakeComponentMask<Ts…>()`; `Astra::With`/`Not`/`Optional` markers; `Astra::View`.
- Produces: `Astra::ViewAccess<V>` with `using Reads = std::tuple<…>;`, `using Writes = std::tuple<…>;`, `static ComponentMask ReadMask();`, `static ComponentMask WriteMask();`. Rule: `const T` (bare) and `Optional<const T>` → read; `T` (bare) and `Optional<T>` and `IncludeDisabled<T>` → write; `With<T>`/`Not<T>`/`Any<…>`/`OneOf<…>` → nothing.

- [ ] **Step 1: Write the failing test** (append)

```cpp
// ---- Task 3: ViewAccess harvesting ----------------------------------------

TEST(ViewAccess, HarvestsReadsWritesAndIgnoresFilters)
{
    // Force ID assignment so both expected and actual masks reference the same ids.
    (void)Astra::MakeComponentMask<Position, Velocity, Health, Transform, Name>();

    using V = Astra::View<Position, const Velocity, Astra::With<Transform>,
                          Astra::Not<Name>, Astra::Optional<Health>>;

    const auto reads  = Astra::ViewAccess<V>::ReadMask();
    const auto writes = Astra::ViewAccess<V>::WriteMask();

    // Reads = Velocity (const data) + Health (Optional non-const is a WRITE though) ...
    // Health here is Optional<Health> (non-const) -> write. So reads = {Velocity}.
    EXPECT_EQ(reads,  (Astra::MakeComponentMask<Velocity>()));
    EXPECT_EQ(writes, (Astra::MakeComponentMask<Position, Health>()));

    // With<Transform> and Not<Name> contribute to NEITHER set.
    EXPECT_FALSE(reads.Test(Astra::TypeID<Transform>::Value()));
    EXPECT_FALSE(writes.Test(Astra::TypeID<Transform>::Value()));
    EXPECT_FALSE(reads.Test(Astra::TypeID<Name>::Value()));
    EXPECT_FALSE(writes.Test(Astra::TypeID<Name>::Value()));
}

TEST(ViewAccess, ConstOptionalIsRead)
{
    (void)Astra::MakeComponentMask<Position, Health>();
    using V = Astra::View<Position, Astra::Optional<const Health>>;
    EXPECT_TRUE(Astra::ViewAccess<V>::ReadMask().Test(Astra::TypeID<Health>::Value()));
    EXPECT_FALSE(Astra::ViewAccess<V>::WriteMask().Test(Astra::TypeID<Health>::Value()));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewAccess.*`
Expected: **FAIL to compile** — `Astra::ViewAccess` undeclared.

- [ ] **Step 3: Add per-arg classification to `Query.hpp`**

In `include/Astra/Registry/Query.hpp`, inside `namespace Detail`, after the modifier traits:

```cpp
        // Per-query-arg read/write classification for ViewAccess.
        // Default (bare data arg): const T -> read, T -> write.
        template<typename Arg>
        struct ArgAccess
        {
            using Read  = std::conditional_t<std::is_const_v<Arg>, std::tuple<std::remove_const_t<Arg>>, std::tuple<>>;
            using Write = std::conditional_t<std::is_const_v<Arg>, std::tuple<>, std::tuple<Arg>>;
        };
        // Optional<T>: yields a pointer; const T -> read, T -> write.
        template<typename T>
        struct ArgAccess<Optional<T>>
        {
            using Read  = std::conditional_t<std::is_const_v<T>, std::tuple<std::remove_const_t<T>>, std::tuple<>>;
            using Write = std::conditional_t<std::is_const_v<T>, std::tuple<>, std::tuple<T>>;
        };
        // IncludeDisabled<T>: yielded mutable (see GetRequired) -> write.
        template<typename T>
        struct ArgAccess<IncludeDisabled<T>>
        {
            using Read  = std::tuple<>;
            using Write = std::tuple<T>;
        };
        // Match-only / grouping modifiers: zero access footprint.
        template<typename T>    struct ArgAccess<With<T>> { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename T>    struct ArgAccess<Not<T>>  { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename... T> struct ArgAccess<Any<T...>>   { using Read = std::tuple<>; using Write = std::tuple<>; };
        template<typename... T> struct ArgAccess<OneOf<T...>> { using Read = std::tuple<>; using Write = std::tuple<>; };

        template<typename... Args>
        struct AccessReads  { using type = decltype(std::tuple_cat(std::declval<typename ArgAccess<Args>::Read>()...)); };
        template<typename... Args>
        struct AccessWrites { using type = decltype(std::tuple_cat(std::declval<typename ArgAccess<Args>::Write>()...)); };
```

- [ ] **Step 4: Add the `ViewAccess` trait to `View.hpp`**

In `include/Astra/Registry/View.hpp`, after the `View` class definition (still inside `namespace Astra`):

```cpp
    // Compile-time read/write access sets of a View's type args, for the scheduler
    // (Stage 2). With<T>/Not<T>/Any/OneOf contribute nothing (match-only). Masks are
    // built at runtime from the type-lists, mirroring how SystemScheduler harvests
    // component masks today.
    template<typename V> struct ViewAccess;

    template<typename... Args>
    struct ViewAccess<View<Args...>>
    {
        using Reads  = typename Detail::AccessReads<Args...>::type;
        using Writes = typename Detail::AccessWrites<Args...>::type;

        static ComponentMask ReadMask()  { return MaskOf(static_cast<Reads*>(nullptr)); }
        static ComponentMask WriteMask() { return MaskOf(static_cast<Writes*>(nullptr)); }

    private:
        template<typename... Ts>
        static ComponentMask MaskOf(std::tuple<Ts...>*) { return MakeComponentMask<Ts...>(); }
    };
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewAccess.*`
Expected: **PASS** (both `ViewAccess` tests).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): ViewAccess read/write harvesting trait (Stage 1 task 3)"
```

---

### Task 4: `QueryError` + `Contains` + visibility helpers

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (enum + `Contains` + private `VisibleRecord`/`EnabledVisible`)
- Test: `tests/Registry/ViewEnrichmentTest.cpp` (append)

**Interfaces:**
- Consumes: `m_archetypeManager->GetEntityRecord(Entity) → const EntityRecord*`; `EntityRecord{archetype, chunk, location}`; `Archetype::GetMask()`; `Archetype::GetColumnMeta()` (`.idToColumn[id]`); `ArchetypeChunk::IsDisabled(int col, size_t index)`; `EntityLocation::GetEntityIndex()`; `QueryBuilder::Matches`; existing `EnabledRequiredFilter` / `HasRequiredFilter`.
- Produces: `enum class Astra::QueryError { NotMatched, Empty, MultipleMatched };`; `bool View::Contains(Entity) const`; private `const EntityRecord* View::VisibleRecord(Entity) const` and `bool View::EnabledVisible(const EntityRecord*) const` (reused by Tasks 5–6).

- [ ] **Step 1: Write the failing test** (append)

```cpp
// ---- Task 4: Contains + QueryError ----------------------------------------

TEST(ViewContains, FilterAware)
{
    Astra::Registry reg;
    auto match   = reg.CreateEntity<Position, Velocity>();
    auto noVel   = reg.CreateEntity<Position>();
    auto excluded= reg.CreateEntity<Position, Velocity, Name>();

    auto v = reg.CreateView<Position, const Velocity, Astra::Not<Name>>();
    EXPECT_TRUE(v.Contains(match));
    EXPECT_FALSE(v.Contains(noVel));      // missing required Velocity
    EXPECT_FALSE(v.Contains(excluded));   // has excluded Name

    reg.DestroyEntity(match);
    EXPECT_FALSE(v.Contains(match));      // stale handle
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewContains.*`
Expected: **FAIL to compile** — `View::Contains` does not exist.

- [ ] **Step 3: Add `QueryError` + helpers + `Contains`**

In `include/Astra/Registry/View.hpp`, add the enum near the top of `namespace Astra` (before or after the `View` template — before is fine):

```cpp
    enum class QueryError { NotMatched, Empty, MultipleMatched };
```

Add these private helpers to `View` (near the other private members), plus the public `Contains`:

```cpp
    private:
        // The record iff `e` is alive AND structurally matches this view AND is
        // enabled-visible; else nullptr. No EnsureArchetypes needed — matching is
        // tested against the entity's OWN archetype mask.
        ASTRA_NODISCARD const EntityRecord* VisibleRecord(Entity e) const
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY return nullptr;
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(e);
            if (!rec) return nullptr;   // dead/absent (GetEntityRecord checks version + archetype)
            if (!QueryBuilder::Matches(rec->archetype->GetMask())) return nullptr;
            if constexpr (HasRequiredFilter)
            {
                if (!EnabledVisible(rec)) return nullptr;
            }
            return rec;
        }

        // False iff any required enableable component is DISABLED for this entity.
        // Only instantiated when HasRequiredFilter (guarded at the call site).
        ASTRA_NODISCARD bool EnabledVisible(const EntityRecord* rec) const
        {
            return EnabledVisibleImpl(rec, EnabledRequiredFilter{});
        }
        template<typename... Fs>
        ASTRA_NODISCARD bool EnabledVisibleImpl(const EntityRecord* rec, std::tuple<Fs...>) const
        {
            const ArchetypeColumnMeta& cm = rec->archetype->GetColumnMeta();
            const size_t idx = rec->location.GetEntityIndex();
            bool disabled = false;
            ((disabled = disabled || rec->chunk->IsDisabled(cm.idToColumn[TypeID<Fs>::Value()], idx)), ...);
            return !disabled;
        }

    public:
        ASTRA_NODISCARD bool Contains(Entity e) const
        {
            return VisibleRecord(e) != nullptr;
        }
```

Note: `EnabledRequiredFilter` is already a private member alias of `View`; `HasRequiredFilter` is the existing constexpr gate. When it is `false`, `EnabledVisibleImpl` is never instantiated.

- [ ] **Step 4: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewContains.*`
Expected: **PASS**.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): QueryError + filter-aware Contains (Stage 1 task 4)"
```

---

### Task 5: `Get(Entity) → Result<tuple, QueryError>`

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (`Get` + tuple-type + materialization helpers)
- Test: `tests/Registry/ViewEnrichmentTest.cpp` (append)

**Interfaces:**
- Consumes: `VisibleRecord` (Task 4); `Archetype::GetComponent<T>(EntityLocation) → T*`; `Archetype::HasComponent<T>() → bool`; `RequiredTypes`/`OptionalTypes`; `Result<T,E>`.
- Produces: `using View::AccessTuple = std::tuple<Required&…, Optional*…>;` and `Result<AccessTuple, QueryError> View::Get(Entity) const`.

- [ ] **Step 1: Write the failing test** (append)

```cpp
// ---- Task 5: Get ----------------------------------------------------------

TEST(ViewGet, ReturnsRefsAndPointers)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity>();
    reg.GetComponent<Position>(e)->x = 5.0f;
    reg.GetComponent<Velocity>(e)->dx = 2.0f;

    auto v = reg.CreateView<Position, const Velocity, Astra::Optional<Health>>();

    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto& [pos, vel, health] = *r.GetValue();
    EXPECT_FLOAT_EQ(pos.x, 5.0f);
    EXPECT_FLOAT_EQ(vel.dx, 2.0f);
    EXPECT_EQ(health, nullptr);        // Optional<Health> absent -> null

    pos.x = 9.0f;                      // write through the returned ref
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 9.0f);

    // pointer identity with GetComponent
    EXPECT_EQ(&pos, reg.GetComponent<Position>(e));
}

TEST(ViewGet, NotMatchedCases)
{
    Astra::Registry reg;
    auto noVel = reg.CreateEntity<Position>();
    auto v = reg.CreateView<Position, const Velocity>();

    auto r = v.Get(noVel);
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(*r.GetError(), Astra::QueryError::NotMatched);

    reg.DestroyEntity(noVel);
    EXPECT_TRUE(v.Get(noVel).IsErr());   // stale handle
}

TEST(ViewGet, OptionalPresentIsNonNull)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<Position, Velocity, Health>();
    auto v = reg.CreateView<Position, const Velocity, Astra::Optional<Health>>();
    auto r = v.Get(e);
    ASSERT_TRUE(r.IsOk());
    auto& [pos, vel, health] = *r.GetValue();
    (void)pos; (void)vel;
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health, reg.GetComponent<Health>(e));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewGet.*`
Expected: **FAIL to compile** — `View::Get` does not exist.

- [ ] **Step 3: Add the tuple type + materialization + `Get`**

In `include/Astra/Registry/View.hpp`, add to `View` (private type + helpers, then public `Get`):

```cpp
    private:
        // Random-access yield shape: each required -> a reference (const preserved),
        // each optional -> a pointer. Mirrors ForEach's yielded arguments.
        template<typename ReqTuple, typename OptTuple> struct AccessTupleImpl;
        template<typename... R, typename... O>
        struct AccessTupleImpl<std::tuple<R...>, std::tuple<O...>>
        {
            using type = std::tuple<R&..., O*...>;
        };

        template<typename R>
        ASTRA_FORCEINLINE R& BindRequired(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<R>;
            return *rec->archetype->GetComponent<Bare>(rec->location);   // Bare* binds to R& (adds const if any)
        }
        template<typename O>
        ASTRA_FORCEINLINE O* BindOptional(const EntityRecord* rec) const
        {
            using Bare = std::remove_const_t<O>;
            return rec->archetype->template HasComponent<Bare>()
                 ? rec->archetype->template GetComponent<Bare>(rec->location)
                 : nullptr;
        }

        template<typename... R, typename... O, size_t... Ri, size_t... Oi>
        ASTRA_FORCEINLINE auto MakeAccessTuple(const EntityRecord* rec,
                                               std::tuple<R...>, std::tuple<O...>,
                                               std::index_sequence<Ri...>, std::index_sequence<Oi...>) const
        {
            return typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type{
                BindRequired<std::tuple_element_t<Ri, RequiredTypes>>(rec)...,
                BindOptional<std::tuple_element_t<Oi, OptionalTypes>>(rec)...
            };
        }

    public:
        using AccessTuple = typename AccessTupleImpl<RequiredTypes, OptionalTypes>::type;

        ASTRA_NODISCARD Result<AccessTuple, QueryError> Get(Entity e) const
        {
            const EntityRecord* rec = VisibleRecord(e);
            if (!rec) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::NotMatched);
            return Result<AccessTuple, QueryError>::Ok(
                MakeAccessTuple(rec, RequiredTypes{}, OptionalTypes{},
                                std::make_index_sequence<std::tuple_size_v<RequiredTypes>>{},
                                std::make_index_sequence<std::tuple_size_v<OptionalTypes>>{}));
        }
```

Note: `GetComponent`/`HasComponent` are non-const `Archetype` methods; `rec->archetype` is a non-const `Archetype*` (only the `EntityRecord` is const), so calling them from a `const` `View` method is well-formed.

- [ ] **Step 4: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewGet.*`
Expected: **PASS** (all three `ViewGet` tests).

- [ ] **Step 5 (portability guard): build Release + Dist**

Run: `make AstraTest config=release && ./bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=View*`
Run: `make AstraTest config=dist && ./bin/Dist-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=View*`
Expected: **PASS** in both. (This is the reference-tuple-inside-`Result` cross-config check from the spec §5. If either fails to compile/run on the tuple-of-refs, apply the spec's defined fallback: change `AccessTupleImpl` to `std::tuple<R*..., O*...>` and update the tests to dereference required pointers — behavior identical, only call-site spelling changes. Disclose as a deviation.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): filter-aware Get(Entity) -> Result<tuple, QueryError> (Stage 1 task 5)"
```

---

### Task 6: `Single() → Result<tuple, QueryError>`

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (`Single`)
- Test: `tests/Registry/ViewEnrichmentTest.cpp` (append)

**Interfaces:**
- Consumes: `ForEach` (Task 2, entity-optional traversal, enabled-filter-aware), `Get` (Task 5).
- Produces: `Result<AccessTuple, QueryError> View::Single()`.

- [ ] **Step 1: Write the failing test** (append)

```cpp
// ---- Task 6: Single -------------------------------------------------------

TEST(ViewSingle, EmptyOneMany)
{
    Astra::Registry reg;
    auto v = reg.CreateView<Position, const Velocity>();

    EXPECT_TRUE(v.Single().IsErr());
    EXPECT_EQ(*v.Single().GetError(), Astra::QueryError::Empty);

    auto only = reg.CreateEntity<Position, Velocity>();
    reg.GetComponent<Position>(only)->x = 7.0f;
    {
        auto r = v.Single();
        ASSERT_TRUE(r.IsOk());
        auto& [pos, vel] = *r.GetValue();
        (void)vel;
        EXPECT_FLOAT_EQ(pos.x, 7.0f);
    }

    reg.CreateEntity<Position, Velocity>();   // now two
    auto r2 = v.Single();
    ASSERT_TRUE(r2.IsErr());
    EXPECT_EQ(*r2.GetError(), Astra::QueryError::MultipleMatched);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewSingle.*`
Expected: **FAIL to compile** — `View::Single` does not exist.

- [ ] **Step 3: Implement `Single`**

In `include/Astra/Registry/View.hpp`, add the public method (reuses the filter-aware `ForEach` to enumerate and `Get` to materialize — DRY):

```cpp
        ASTRA_NODISCARD Result<AccessTuple, QueryError> Single()
        {
            Entity found{};
            size_t count = 0;
            ForEach([&](Entity e, auto&&...) { if (count == 0) found = e; ++count; });
            if (count == 0) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::Empty);
            if (count > 1) ASTRA_UNLIKELY
                return Result<AccessTuple, QueryError>::Err(QueryError::MultipleMatched);
            return Get(found);   // exactly one visible match; Get re-validates and materializes
        }
```

Note: full traversal (no early break) is acceptable for Stage 1 — singleton sets are small; an early-exit optimization is a possible later refinement. `Single` is non-`const` because `ForEach` calls `EnsureArchetypes()`. The `[&](Entity e, auto&&...)` body is invocable with `(Entity, Comps…)`, so the adapter takes the entity branch.

- [ ] **Step 4: Run test to verify it passes**

Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewSingle.*`
Expected: **PASS**.

- [ ] **Step 5: Full Stage 1 suite, three configs**

Run: `make AstraTest config=debug   && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=View*`
Run: `make AstraTest config=release && ./bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=View*`
Run: `make AstraTest config=dist    && ./bin/Dist-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=View*`
Expected: **PASS** in all three.

Then run the **entire** suite in Debug to confirm no regression to existing View/lambda-system tests:
Run: `make AstraTest config=debug && ./bin/Debug-windows-x86_64/AstraTest/AstraTest.exe`
Expected: **PASS** (all pre-existing tests still green — the additive-only invariant).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ViewEnrichmentTest.cpp
git commit -m "feat(view): Single() -> Result<tuple, QueryError> (Stage 1 task 6)"
```

---

## Self-Review

**1. Spec coverage** (spec §3–§6, §9):
- §3 `With<T>` (Query.hpp touch-points 1–5, `View.hpp` count fix) → Task 1. ✓
- §4 access-harvesting trait, With/Not zero-footprint → Task 3. ✓
- §5 `Get`/`Single` → `Result<tuple, QueryError>`, `Contains` → bool, filter-aware incl. enableable → Tasks 4 (Contains + `EnabledVisible`), 5 (Get), 6 (Single). Cross-compiler fallback → Task 5 Step 5. ✓
- §6 entity-optional `ForEach`/`ParallelForEach` → Task 2. ✓
- §9 testing: reuse `Astra::Test::*` only (no new types); 3-config green (Task 5 Step 5, Task 6 Step 5); With matching + not-yielded (Task 1); harvesting masks (Task 3); Get/Single/Contains match/no-match/empty/multiple (Tasks 4–6); entity-optional both shapes + regression (Task 2). ✓
  - *Enableable-disabled random-access path:* `EnabledVisible` (Task 4) is wired and gated on `HasRequiredFilter`; there is no dedicated enableable-disabled `Get` test because it needs an enableable test component, and the TypeID budget forbids adding one. This is acceptable: the code path reuses the exact `idToColumn`/`IsDisabled` primitives already covered by the enableable-components suite, and `HasRequiredFilter` is `false` for all bench/most tests so the guard is exercised as a no-op. Noted as a known coverage gap, not a code gap.

**2. Placeholder scan:** No "TBD"/"handle edge cases"/"similar to Task N". Every code step shows full code; every run step shows an exact command + expected result. ✓

**3. Type consistency:**
- `QueryError { NotMatched, Empty, MultipleMatched }` — defined Task 4, used Tasks 4/5/6 identically. ✓
- `AccessTuple` / `AccessTupleImpl` — defined Task 5, reused by `Single` (Task 6) via `Get`. ✓
- `VisibleRecord`/`EnabledVisible` — defined Task 4, consumed by `Get` (Task 5). ✓
- `MakeEntityOptionalAdapter` — defined Task 2, used by `ForEach`/`ParallelForEach` (Task 2) and implicitly by `Single`'s `ForEach` call (Task 6). ✓
- `ViewAccess<V>::ReadMask()/WriteMask()`, `Detail::ArgAccess`/`AccessReads`/`AccessWrites` — defined Task 3, self-contained. ✓
- `With`/`GetWithMask`/`WithComponents` — defined Task 1, used by `Matches` and `CollectArchetypes`. ✓
