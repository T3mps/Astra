# Enableable Components Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DOTS-style enable/disable for opt-in component types — O(1) toggle, enabled-only default query filtering with `IncludeDisabled<T>` opt-out, deterministic iteration, zero cost on every path that doesn't use the feature.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-25-enableable-components-design.md` (user-approved; §14 binding invariants govern). Disabled-bit words (SET = disabled; zero-init = enabled) carved into chunk memory per enableable column + per-column `disabledCount`; three-tier chunk filter (untouched loop / skip / `countr_zero` run-scan); trait is compile-time, so a query with no enableable filtered types compiles to the EXISTING loop via `if constexpr` — the zero-cost claim is structural. Preservation at the same swap-remove/transition sites Levers 2-3 hardened. Gated `ComponentEnabled/Disabled` signals; deferred `SetEnabled` command; serialization format bump with refuse-on-corrupt validation.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln`), GoogleTest, definitive bench harness in `bench-compare/`.

## Global Constraints

- Branch: `feat/enableable-components` off dev HEAD (record SHA in ledger). Local only — NEVER push. Finish = opus whole-branch review → fix wave → authoritative 3-config → **confirm with user** → FF-merge local, delete branch.
- Build (PowerShell): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<CFG> -p:Platform=x64 -m`. Tests: `.\bin\<CFG>-windows-x86_64\AstraTest\AstraTest.exe`. Baseline (dev @ `3cd12ef`): **Debug 749 / Release 747 / Dist 747** — re-verify before Task 1. Deltas: T1 +2, T2 +8, T3 +7, T4 +3, T5 +3, T6 +0 (deltas govern over absolutes). Known `[critical] ... Cycle detected` stderr line = expected. Stale PDB → `taskkill /F /IM mspdbsrv.exe`. IDE clang diagnostics = false positives.
- **Spec §14 invariants (binding, verbatim lens for reviewers):** (1) zero cost when unused — no words for non-enableable columns, no filter machinery for queries with empty enableable set, no added instructions on create/batch-create; (2) SET bit == DISABLED, zero-init == enabled, `disabledCount == popcount(words)`, bits beyond chunk count are zero; (3) enabled state preserved across swap-remove/transitions/compaction/defragment, fresh components born enabled; (4) `Has`/`GetComponent` ignore bits — only default queries, `IsEnabled`, `Size()` observe them; (5) iteration visit order independent of toggle history; (6) signals fire only on genuine state change, one site, gated off by default; (7) existing mutation threading rules; deferred toggles flush in SortKey order; (8) serialization refuses count/popcount mismatch and out-of-range bits, legacy loads all-enabled; (9) **zero new test component types** (deviation requires disclosure).
- **TypeID ceiling:** tests mark EXISTING `Astra::Test` components enableable (Task 1 picks two; must NOT be `Position`/`Velocity` — the record-invariant and bench-adjacent tests depend on their layout math staying put).
- Bench gate (Task 6 ONLY): rebuild `bench-compare/bench_astra.exe` only (full-opt recipe from RESULTS.md ~:574 **+ `/I..\tests`**; build_one.bat via PowerShell tool), typeperf quiet gate, 6 interleaved rounds astra→flecs→entt appending `roundN,`-prefixed CSV to `bench-compare/lever_ec_gate.csv` (untracked). Flat-watch ops (create/create_batch/add/remove/destroy/random_get/iterate1/2/3) vs the Lever-3 RESULTS.md same-session medians; paired same-session medians [min,max]; non-overlapping bands rule. ALL must be flat.
- Model recipe (SDD): sonnet T1/T4/T5/T6; **OPUS T2 (chunk storage + preservation) and T3 (query filtering hot path)** and the final whole-branch review.

---

### Task 1: Trait, descriptor plumbing, signal pair

**Files:**
- Modify: `include/Astra/Component/Component.hpp` (trait + `ComponentDescriptor::isEnableable` + factory assignment)
- Modify: `include/Astra/Core/Signal.hpp` (Signal enum bits + Events structs)
- Modify: `tests/TestComponents.hpp` (mark two existing components enableable)
- Test: `tests/Component/` — put the two new tests beside the existing component/registry test file for descriptors (grep `ComponentDescriptor` under tests/ and use that file; report which)

**Interfaces:**
- Consumes: nothing.
- Produces: `Astra::EnableableTraits<T>` + `Astra::IsEnableableV<T>` (constexpr bool); `ComponentDescriptor::isEnableable`; `Signal::ComponentEnabled`/`Signal::ComponentDisabled`; `Events::ComponentEnabled`/`Events::ComponentDisabled` (fields mirror `Events::ComponentAdded` — entity + component identity, read that struct first and match it exactly); two enableable test components (report WHICH two — later tasks use them).

- [ ] **Step 1: Write the two failing tests** (trait detection + descriptor snapshot). In the descriptor test file found above:

```cpp
// File scope (specializations cannot target function-local types). Compile-time
// only — never registered, zero TypeIDs consumed.
namespace EnableableTraitTestDetail
{
    struct MemberSpelled { static constexpr bool AstraEnableable = true; int v; };
    struct Plain         { int v; };
    struct SpecSpelled   { int v; };
}
template<> struct Astra::EnableableTraits<EnableableTraitTestDetail::SpecSpelled>
{
    static constexpr bool value = true;
};

TEST(EnableableTrait, DetectionCoversBothSpellingsAndNegative)
{
    using namespace Astra;
    using namespace EnableableTraitTestDetail;
    static_assert(IsEnableableV<MemberSpelled>);
    static_assert(IsEnableableV<SpecSpelled>);     // specialization escape hatch
    static_assert(!IsEnableableV<Plain>);
    static_assert(!IsEnableableV<int>);
    SUCCEED();
}

TEST(EnableableTrait, DescriptorSnapshotsTrait)
{
    // The two suite components Task 1 marks enableable must snapshot true;
    // an unmarked sibling stays false. Use the registry's descriptor factory
    // the same way the file's existing descriptor tests do (mirror their
    // construction idiom — do NOT hand-roll descriptors).
    // Pseudocode shape (adapt the factory call spelling to the file):
    //   auto dEn  = <factory><Astra::Test::EnableableA>();
    //   auto dOff = <factory><Astra::Test::Position>();
    //   EXPECT_TRUE(dEn.isEnableable);
    //   EXPECT_FALSE(dOff.isEnableable);
}
```

(`LocalMember`/`LocalPlain` are compile-time-only — never registered, zero TypeIDs consumed. `EnableableA` here is a stand-in name: Step 3 picks the real components and this test names them.)

- [ ] **Step 2: Run to verify failure.** Expected: compile FAIL (`IsEnableableV` undefined).

- [ ] **Step 3: Implement.**

3a. `include/Astra/Component/Component.hpp`, above `ComponentDescriptor`:

```cpp
    /**
     * Opt-in enableability (spec 2026-07-25 §2). A component is enableable iff
     * it declares `static constexpr bool AstraEnableable = true` or specializes
     * Astra::EnableableTraits<T>. Enableable columns carry per-chunk disabled
     * bits; everything else pays nothing.
     */
    template<typename T>
    struct EnableableTraits
    {
        static constexpr bool value = requires { { T::AstraEnableable } -> std::convertible_to<bool>; } && T::AstraEnableable;
    };

    template<typename T>
    inline constexpr bool IsEnableableV = EnableableTraits<std::remove_const_t<T>>::value;
```

3b. `ComponentDescriptor` (:36+): add `bool isEnableable = false;` beside the other trait bools (respect the file's comment noting which bool carries a default and why — this one defaults false so hand-built descriptors stay safe, same rationale as `is_trivially_destructible`). In the descriptor factory (the function that fills `is_trivially_copyable` etc. — read it), assign `isEnableable = IsEnableableV<T>;`.

3c. `include/Astra/Core/Signal.hpp`: append to `enum class Signal` (:21, next free bits after the existing entries — read the enum to find them):

```cpp
        ComponentEnabled  = 1 << <next>,
        ComponentDisabled = 1 << <next+1>,
```

and add Events structs mirroring `Events::ComponentAdded`'s exact field set (:99-108 — copy its shape, swap the `flag`):

```cpp
        struct ComponentEnabled
        {
            static constexpr Signal flag = Signal::ComponentEnabled;
            // same fields as ComponentAdded — copy them verbatim
        };
        struct ComponentDisabled
        {
            static constexpr Signal flag = Signal::ComponentDisabled;
            // same fields as ComponentAdded — copy them verbatim
        };
```

3d. `tests/TestComponents.hpp`: pick TWO existing non-tag, non-empty components that are NOT `Position`/`Velocity` and are not layout-asserted anywhere (grep each candidate for `sizeof`/chunk-count assertions in tests before choosing; `Health`-like value components are the intended shape). Add to each:

```cpp
        static constexpr bool AstraEnableable = true;   // enableable-components suite opt-in (spec §2)
```

Update `EnableableTrait.DescriptorSnapshotsTrait` to name the two chosen types. Record the choice in your report — Tasks 2-5 reuse exactly these.

- [ ] **Step 4: Run both tests — PASS. Full Debug suite — expected +2 (751). Release/Dist: 749 each.**

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Component/Component.hpp include/Astra/Core/Signal.hpp tests/TestComponents.hpp tests/Component/<descriptor test file>
git commit -m "feat(component): opt-in enableable trait + descriptor snapshot + gated ComponentEnabled/Disabled signal pair"
```

---

### Task 2: Chunk storage, toggle API, preservation (OPUS)

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`Column` fields; ctor carve loop :481-490; chunk accessors; swap-remove + batch-move bit carry — read `RemoveEntity`/swap site and `BatchMoveEntitiesFrom` :176-221 first)
- Modify: `include/Astra/Archetype/Archetype.hpp` (capacity-for-bytes math — find the function that computes entitiesPerChunk from chunkBytes (grep `chunkBytes` / the grow-as-populate clamp); merge-join transition move :600-611 bit carry; `RemoveEntity` swap-remove count/bit bookkeeping if it lives archetype-side — read first and report where each landed)
- Modify: `include/Astra/Registry/Registry.hpp` (`SetEnabled`/`IsEnabled` + ByID + signal site)
- Test: `tests/Registry/RegistryTest.cpp` (behavior table + preservation; reuse Task 1's two enableable components)

**Interfaces:**
- Consumes: `ComponentDescriptor::isEnableable`, `IsEnableableV<T>`, the Events pair (Task 1); `AM::GetEntityRecord` validated fetch; records carry direct chunk pointer + index (W1/Lever-1).
- Produces (Tasks 3-5 depend on these EXACT names): on `ArchetypeChunk`: `uint64_t* GetDisabledWords(int column) noexcept` (nullptr if column not enableable), `uint32_t GetDisabledCount(int column) const noexcept`, `bool IsDisabled(int column, size_t index) const noexcept`, `bool SetDisabled(int column, size_t index, bool disabled) noexcept` (returns true iff state CHANGED; updates count); on `Registry`: `template<Component T> bool SetEnabled(Entity, bool)`, `template<Component T> bool IsEnabled(Entity) const`, `bool SetEnabledByID(Entity, ComponentID, bool)`, `bool IsEnabledByID(Entity, ComponentID) const`; test seam `ExpectDisabledInvariant(chunk...)` asserting `disabledCount == popcount` + tail-bits-zero (place beside the file's existing invariant-helper idiom).

Requirements (spec §3/§4/§8 — you design the exact code within them):

- `Column` gains `uint64_t* disabledWords = nullptr;` and `uint32_t disabledCount = 0;` (uint32: capacity can exceed 65535 at the 512KB cap). Growth of the Chunk-object footprint is accepted (recorded Phase-C packed-[N] deferral now covers it — note this in a comment where `Column` is declared).
- **Carve** (ctor :481-490): after the existing column loop, a second loop carves `ceil(capacity/64) * 8` bytes per ENABLEABLE column (8-byte aligned suffices), assigning `disabledWords`; non-enableable columns keep nullptr. The existing `offset <= m_chunkSize` assert stays as the net. Chunk memory is already zeroed → words start all-enabled with no writes (invariant 1: create paths gain nothing).
- **Capacity math**: the archetype-side capacity computation must account for word bytes so the carve can't overflow: after computing the candidate capacity with the existing math, verify `totalBytes(capacity)` (cache-line-aligned columns + 8-aligned word regions for enableable columns) fits `chunkBytes`, decrementing capacity until it fits (bounded loop; archetypes with zero enableable columns take the existing path UNCHANGED — verify by inspection and say so in the report).
- **Toggle path**: `Registry::SetEnabled<T>` — `static_assert(IsEnableableV<T>, ...)`; one validated record fetch; `idToColumn` for the column; `chunk->SetDisabled(col, idx, !enable)`; on CHANGE only, fire the gated signal (`IsSignalEnabled` check then `Emit`, exactly the `DestroyEntity` idiom at `Registry.hpp:233`); behavior table rows for stale entity / missing component return false, no signal. `IsEnabled<T>` mirrors (missing/stale → false). ByID variants share the same body via the id (typed forms delegate). Doc comments state the scheduling contract (spec §9): SetEnabled counts as WRITE access on T, IsEnabled/filtering as read — same threading rules as other immediate mutations.
- **Swap-remove carry**: at the chunk swap-remove site — moved (last) entity's bit copied into the vacated slot, tail slot cleared, `disabledCount` adjusted iff the REMOVED entity was disabled (and iff moved entity's bit differs — get the arithmetic right for the moved==removed single-entity case).
- **Transition carry**: in the ordinal merge-join move (:600-611) and `BatchMoveEntitiesFrom` (:176-221): for columns present on BOTH sides, dst bit := src bit (and counts); src side cleanup handled by its swap-remove/clear path. Fresh columns: no writes (born enabled). Removed columns: bits vanish with the column.
- **CompactChunks/Defragment**: these rebuild/move entities through the paths above — verify by reading that no additional site copies entities without passing through a covered site; if one exists, cover it and name it in the report.

- [ ] **Step 1: Behavior-table + preservation tests FIRST** (RED where they exercise new API — expected compile-fail initially; write, then implement, then GREEN — the suite's TDD shape for new-API tasks). Eight tests in `tests/Registry/RegistryTest.cpp` (adapt fixture idioms; `EnA`/`EnB` = Task 1's two enableable components — use their real names):

```cpp
TEST_F(RegistryTest, EnableToggleBehaviorTable)
{
    auto e = registry.CreateEntity<EnA>();
    EXPECT_TRUE(registry.IsEnabled<EnA>(e));                    // born enabled
    EXPECT_TRUE(registry.SetEnabled<EnA>(e, false));            // disable: applied
    EXPECT_FALSE(registry.IsEnabled<EnA>(e));
    EXPECT_TRUE(registry.SetEnabled<EnA>(e, false));            // idempotent: true, no change
    EXPECT_TRUE(registry.GetComponent<EnA>(e) != nullptr);      // existence never lies
    EXPECT_TRUE(registry.HasComponent<EnA>(e));                 // (spell per Registry API)
    auto missing = registry.CreateEntity();                     // no EnA
    EXPECT_FALSE(registry.SetEnabled<EnA>(missing, false));
    EXPECT_FALSE(registry.IsEnabled<EnA>(missing));
    registry.DestroyEntity(e);
    EXPECT_FALSE(registry.SetEnabled<EnA>(e, true));            // stale: no-op false
}

TEST_F(RegistryTest, EnableSignalsFireOnlyOnGenuineChange)
{
    registry.EnableSignals(Astra::Signal::ComponentEnabled);
    registry.EnableSignals(Astra::Signal::ComponentDisabled);
    int enabled = 0, disabled = 0;
    registry.GetSignalManager()->On<Astra::Events::ComponentDisabled>().Register(
        [&](const auto&) { ++disabled; });
    registry.GetSignalManager()->On<Astra::Events::ComponentEnabled>().Register(
        [&](const auto&) { ++enabled; });
    auto e = registry.CreateEntity<EnA>();
    registry.SetEnabled<EnA>(e, false);
    registry.SetEnabled<EnA>(e, false);      // idempotent: silent
    registry.SetEnabled<EnA>(e, true);
    EXPECT_EQ(disabled, 1);
    EXPECT_EQ(enabled, 1);
}

TEST_F(RegistryTest, DisabledBitSurvivesSwapRemove)
{
    // Disable a NON-last entity, destroy the last one in the same chunk (swap
    // fills the vacated slot), and verify both entities' states by identity.
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 8; ++i) es.push_back(registry.CreateEntity<EnA>());
    registry.SetEnabled<EnA>(es[2], false);
    registry.DestroyEntity(es[7]);
    EXPECT_FALSE(registry.IsEnabled<EnA>(es[2]));
    for (int i = 0; i < 7; ++i) if (i != 2) EXPECT_TRUE(registry.IsEnabled<EnA>(es[i]));
    // Now destroy a MIDDLE entity so the swapped-in survivor was the disabled one's neighbor:
    registry.SetEnabled<EnA>(es[6], false);
    registry.DestroyEntity(es[2]);           // slot 2 refilled by the (disabled) last entity or a survivor
    EXPECT_FALSE(registry.IsEnabled<EnA>(es[6]));   // identity-tracked, wherever it now lives
}

TEST_F(RegistryTest, DisabledBitSurvivesArchetypeTransition)
{
    auto e = registry.CreateEntity<EnA>();
    registry.SetEnabled<EnA>(e, false);
    registry.AddComponent(e, Position{1, 2, 3});     // transition: EnA carries its bit
    EXPECT_FALSE(registry.IsEnabled<EnA>(e));
    registry.RemoveComponent<Position>(e);           // transition back
    EXPECT_FALSE(registry.IsEnabled<EnA>(e));
    registry.AddComponent(e, EnB{});                 // fresh component: born enabled
    EXPECT_TRUE(registry.IsEnabled<EnB>(e));
    EXPECT_FALSE(registry.IsEnabled<EnA>(e));        // untouched by EnB's arrival
}

TEST_F(RegistryTest, BatchCreateBornEnabled)
{
    constexpr size_t kCount = 3000;                  // spans chunks (grow-as-populate)
    std::vector<Astra::Entity> ents(kCount);
    size_t created = registry.CreateEntitiesWith<EnA>(kCount, std::span{ents},
        [](size_t) { return std::tuple{EnA{}}; });
    ASSERT_EQ(created, kCount);
    for (auto e : ents) ASSERT_TRUE(registry.IsEnabled<EnA>(e));
}
```

plus three invariant-seam tests (place the `ExpectDisabledInvariant` helper per the file's helper idiom): after a 100-toggle random walk; after a destroy-half loop; after add/remove-component churn over disabled holders **followed by `Registry::Defragment()` (or the current compaction entry point — grep `CompactChunks` callers) with identity-tracked IsEnabled states asserted unchanged across it** — each asserting `disabledCount == popcount(words)` and tail-bits-zero on every chunk of the touched archetypes. (This is the spec §12.5 CompactChunks/Defragment preservation coverage.)

- [ ] **Step 2: Implement per the requirements block above.**
- [ ] **Step 3: Filters GREEN; full Debug suite** — expected +8 (759). Release/Dist 757. Any count/popcount seam failure = bookkeeping bug: STOP, do not paper over.
- [ ] **Step 4: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "feat(archetype): disabled-bit chunk words + O(1) Registry toggle API - zero-init born-enabled, preservation across swap-remove and transitions"
```

---

### Task 3: Query filtering (OPUS)

**Files:**
- Modify: `include/Astra/Registry/Query.hpp` (`IncludeDisabled<T>` modifier: declare beside `Optional`/`Not` :17-19; classify in `Detail::IsModifier`/`ExtractComponent`/`QueryClassifier` — the component is REQUIRED for matching, excluded from enabled filtering; read the classifier before designing)
- Modify: `include/Astra/Registry/View.hpp` (`ForEachImpl`/`ForEachWithOptional` :435-480, `ParallelForEachChunk*` :482+, `Size()` :310)
- Create: `include/Astra/Registry/EnabledRuns.hpp` (small Detail helper — run extraction; keep View.hpp from growing)
- Test: `tests/Registry/ViewTest.cpp` — or the file where View filtering tests live (grep `CreateView` under tests/ for the densest file; report which)

**Interfaces:**
- Consumes: Task 2's chunk accessors (`GetDisabledWords`, `GetDisabledCount`, exact names above); `IsEnableableV<T>`; `idToColumn`; Task 1's `EnA`/`EnB`.
- Produces: `Astra::IncludeDisabled<T>` view modifier; `Detail::ForEachEnabledRun(const uint64_t* const* wordSets, size_t setCount, size_t count, Fn&& fn)` in EnabledRuns.hpp — invokes `fn(size_t begin, size_t end)` for each maximal run of indices `< count` where NO set has a bit; used by both serial and parallel paths.

Requirements (spec §5 — design the exact metaprogram within them):

- **Compile-time gate**: the set of a view's enabled-filtered types = required+optional components that are `IsEnableableV` and NOT wrapped `IncludeDisabled`. `if constexpr (that set is empty)` → the existing loops compile UNCHANGED (byte-identical instantiation — invariant 1). All current suite/bench views take this branch.
- **Per-chunk three-tier** (filtered instantiation only): all relevant columns `disabledCount == 0` → existing body over `[0, count)`; any REQUIRED relevant column `disabledCount == count` → skip chunk; else gather the relevant REQUIRED columns' word pointers and run `Detail::ForEachEnabledRun`, feeding runs to the existing body. Runs = maximal spans where `~(OR of words)` has consecutive set bits; tail bits ≥ count masked out. `countr_zero`-based scan (Mosaic BitSet idiom, `vendor/Mosaic/include/Mosaic/BitSet.hpp:61-77`, generalized to ranges).
- **Optional components** that are enableable (and not IncludeDisabled): the pointer handed to the callback is nulled per entity while disabled. This needs a per-entity bit test only inside mixed chunks and only for enableable optionals — keep it out of the required-only instantiation (a second `if constexpr`).
- **`Size()`**: for filtered views, exact — subtract per-chunk disabled unions via popcount walk in mixed chunks (all-clear chunks use counts). Unfiltered views unchanged.
- **ParallelForEach**: same per-chunk logic inside the chunk worker; work partitioning unchanged.
- **Determinism**: visit order = archetype order, chunk order, ascending index within runs — structurally guaranteed; the test pins it anyway.

- [ ] **Step 1: Tests FIRST** (new-API RED shape). Seven, in the View test file:

```cpp
TEST_F(ViewTest, DefaultQueryExcludesDisabled)
{
    auto e1 = registry.CreateEntity<EnA>(); auto e2 = registry.CreateEntity<EnA>();
    registry.SetEnabled<EnA>(e2, false);
    auto view = registry.CreateView<EnA>();
    std::vector<Astra::Entity> seen;
    view.ForEach([&](Astra::Entity e, EnA&) { seen.push_back(e); });
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], e1);
    EXPECT_EQ(view.Size(), 1u);
}

TEST_F(ViewTest, IncludeDisabledOptOutSeesEverything)
{
    auto e1 = registry.CreateEntity<EnA>(); auto e2 = registry.CreateEntity<EnA>();
    registry.SetEnabled<EnA>(e2, false);
    auto view = registry.CreateView<Astra::IncludeDisabled<EnA>>();
    size_t n = 0;
    view.ForEach([&](Astra::Entity, EnA&) { ++n; });
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(view.Size(), 2u);
}

TEST_F(ViewTest, MultiEnableableIntersection)
{
    // enabled(EnA) AND enabled(EnB): 4 entities, one per quadrant.
    auto both  = registry.CreateEntity<EnA, EnB>();
    auto aOff  = registry.CreateEntity<EnA, EnB>(); registry.SetEnabled<EnA>(aOff, false);
    auto bOff  = registry.CreateEntity<EnA, EnB>(); registry.SetEnabled<EnB>(bOff, false);
    auto neither = registry.CreateEntity<EnA, EnB>();
    registry.SetEnabled<EnA>(neither, false); registry.SetEnabled<EnB>(neither, false);
    size_t n = 0; Astra::Entity onlyHit{};
    registry.CreateView<EnA, EnB>().ForEach([&](Astra::Entity e, EnA&, EnB&) { ++n; onlyHit = e; });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(onlyHit, both);
}

TEST_F(ViewTest, WordBoundaryRunScan)
{
    // 200 entities; disable indices 0, 63, 64, 65, 127, 128 and a full word [64,128).
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 200; ++i) es.push_back(registry.CreateEntity<EnA>());
    for (int i : {0, 63, 64, 65, 127, 128}) registry.SetEnabled<EnA>(es[i], false);
    std::set<int> disabledIdx = {0, 63, 64, 65, 127, 128};
    size_t n = 0;
    registry.CreateView<EnA>().ForEach([&](Astra::Entity e, EnA&) {
        ++n;
        // identity check: no disabled entity is ever visited
        for (int i : disabledIdx) EXPECT_NE(e, es[i]);
    });
    EXPECT_EQ(n, 200u - disabledIdx.size());
    // full-word stretch: disable [64,128) entirely, re-count
    for (int i = 64; i < 128; ++i) registry.SetEnabled<EnA>(es[i], false);
    size_t m = 0;
    registry.CreateView<EnA>().ForEach([&](Astra::Entity, EnA&) { ++m; });
    std::set<int> all(disabledIdx); for (int i = 64; i < 128; ++i) all.insert(i);
    EXPECT_EQ(m, 200u - all.size());
}

TEST_F(ViewTest, OptionalEnableableReportsNullWhileDisabled)
{
    auto e = registry.CreateEntity<Position, EnA>();
    registry.SetEnabled<EnA>(e, false);
    registry.CreateView<Position, Astra::Optional<EnA>>().ForEach(
        [&](Astra::Entity, Position&, EnA* a) { EXPECT_EQ(a, nullptr); });
    registry.SetEnabled<EnA>(e, true);
    registry.CreateView<Position, Astra::Optional<EnA>>().ForEach(
        [&](Astra::Entity, Position&, EnA* a) { EXPECT_NE(a, nullptr); });
}

TEST_F(ViewTest, IterationOrderIndependentOfToggleHistory)
{
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 300; ++i) es.push_back(registry.CreateEntity<EnA>());
    auto visit = [&] {
        std::vector<Astra::Entity> order;
        registry.CreateView<EnA>().ForEach([&](Astra::Entity e, EnA&) { order.push_back(e); });
        return order;
    };
    // History A: disable evens then re-enable. History B: disable odds twice, re-enable.
    for (int i = 0; i < 300; i += 2) registry.SetEnabled<EnA>(es[i], false);
    for (int i = 0; i < 300; i += 2) registry.SetEnabled<EnA>(es[i], true);
    auto a = visit();
    for (int r = 0; r < 2; ++r)
        for (int i = 1; i < 300; i += 2) { registry.SetEnabled<EnA>(es[i], false); registry.SetEnabled<EnA>(es[i], true); }
    auto b = visit();
    EXPECT_EQ(a, b);
}

TEST_F(ViewTest, ParallelForEachRespectsDisabled)
{
    for (int i = 0; i < 5000; ++i)
    {
        auto e = registry.CreateEntity<EnA>();
        if (i % 3 == 0) registry.SetEnabled<EnA>(e, false);
    }
    std::atomic<size_t> n{0};
    registry.CreateView<EnA>().ParallelForEach([&](Astra::Entity, EnA&) { n.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(n.load(), 5000u - (5000u + 2) / 3);
}
```

(Adapt callback arities/entity-parameter conventions and the fixture to the file's idiom — read two neighboring tests first. `EnA`/`EnB` = Task 1's chosen names. If the suite runs `ParallelForEach` sequentially without a scheduler, the last test still validates filtering — note it in the report.)

- [ ] **Step 2: Implement** per the requirements block (EnabledRuns.hpp helper first — pure function, unit-testable through the view tests).
- [ ] **Step 3: GREEN; full Debug suite** — expected +7 (766). Release/Dist 764.
- [ ] **Step 4: Commit**

```bash
git add include/Astra/Registry/Query.hpp include/Astra/Registry/View.hpp include/Astra/Registry/EnabledRuns.hpp tests/Registry/<view test file>
git commit -m "feat(view): enabled-only default query filtering - compile-time gate, three-tier chunk filter, IncludeDisabled opt-out, run-scan iteration"
```

---

### Task 4: Serialization

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`Serialize` :786+, `Deserialize` :883+ — read the chunk-section write/read loops in full first; locate the archetype/registry binary format version constant by grepping `version` in both functions and `Registry.hpp` Save/Load — the W2-era code distinguishes v2/v3 loads, so the constant and its gate exist)
- Test: `tests/Serialization/` — the binary round-trip test file (grep `BinarySerializationTests`)

**Interfaces:**
- Consumes: Task 2's chunk accessors; format version constant (report its name and old→new value).
- Produces: format vN+1 with per-enableable-column `disabledCount` + words per chunk section; loader accepts vN (all-enabled) and vN+1; refuses corrupt bits.

- [ ] **Step 1: Three tests FIRST** (round-trip is RED until implemented; legacy/corrupt tests written against the format helpers the file already uses):

```cpp
TEST(BinarySerializationTests, EnabledBitsRoundTrip)
{
    // Save a registry with mixed enabled/disabled EnA across >1 chunk;
    // load into a fresh registry; every entity's IsEnabled matches by identity.
}
TEST(BinarySerializationTests, LegacyFormatLoadsAllEnabled)
{
    // A capture saved WITHOUT bits (previous version constant) loads with every
    // enableable component enabled. Use the suite's existing legacy-fixture
    // mechanism (the v2/v3 tests show the idiom — mirror it; if legacy bytes are
    // generated by writing with the old version constant via a test seam, do that).
}
TEST(BinarySerializationTests, CorruptDisabledBitsRefused)
{
    // Tamper a saved buffer: (a) disabledCount != popcount(words); (b) a set bit
    // >= chunk entity count. Both loads must FAIL with a SerializationError,
    // not silently load. Mirror the file's existing tamper-test idiom (the
    // LoadRobustness / checksum tests show the byte-patching approach).
}
```

(Write real bodies per the file's round-trip/tamper idioms — the comments above are the requirements, the file's existing tests are the style guide. Checksums: if the format checksums chunk sections, re-fix the checksum after tampering the way the existing tamper tests do, so the test exercises the VALIDATION, not the checksum.)

- [ ] **Step 2: Implement.** Writer: per chunk, for each enableable column, write `disabledCount` then the words (exact word count from capacity). Reader vN+1: read + validate (`count == popcount`, no bit ≥ chunk entity count → else `SerializationError` per the file's error taxonomy); reader vN: skip (words absent, zero-init already all-enabled). Bump the version constant.
- [ ] **Step 3: GREEN; full Debug suite** — expected +3 (769). Release/Dist 767. (CompressionTest.PerformanceBenchmark flake: rerun isolated if it trips.)
- [ ] **Step 4: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp tests/Serialization/<file>
git commit -m "feat(serialization): persist disabled bits per enableable column - format bump, legacy all-enabled, refuse corrupt counts"
```

---

### Task 5: Deferred SetEnabled command

**Files:**
- Modify: `include/Astra/Commands/Command.hpp` (CommandType entry + payload)
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (record API, executor, dispatch case, placeholder resolution — grep `ResolvePlaceholders` and add the entity field there like other single-entity payloads)
- Test: `tests/Commands/CommandBufferTest.cpp`

**Interfaces:**
- Consumes: `Registry::SetEnabledByID` (Task 2); `IsEnableableV<T>` (Task 1).
- Produces: `CommandBuffer::SetEnabled<T>(Entity, bool)`.

- [ ] **Step 1: Payload + type.** `Command.hpp`: append `SetEnabled` to `CommandType` (append-only — values are not serialized); payload beside the other single-entity payloads:

```cpp
    /**
     * Payload for SetEnabled command (enableable components, spec 2026-07-25 §7).
     */
    struct SetEnabledPayload
    {
        Entity entity;
        ComponentID componentId;
        uint8_t enable;   // bool, fixed-width for the POD payload
    };
```

- [ ] **Step 2: Tests FIRST** (RED — API absent):

```cpp
TEST_F(CommandBufferTest, DeferredSetEnabledAppliesAtFlush)
{
    Entity e = registry->CreateEntity();
    registry->AddComponent(e, EnA{});
    cmdBuffer->SetEnabled<EnA>(e, false);
    EXPECT_TRUE(registry->IsEnabled<EnA>(e));        // not yet applied
    ASSERT_TRUE(cmdBuffer->Execute().IsOk());
    EXPECT_FALSE(registry->IsEnabled<EnA>(e));
}

TEST_F(CommandBufferTest, DeferredSetEnabledOnStaleTargetReportsError)
{
    Entity e = registry->CreateEntity();
    registry->AddComponent(e, EnA{});
    ParallelCommandBuffer pcb(registry.get());
    auto& buf = pcb.GetThreadBuffer();
    buf.SetNextSortKey(SortKey{1, 0, 0});
    buf.DestroyEntity(e);
    buf.SetNextSortKey(SortKey{2, 0, 0});
    buf.SetEnabled<EnA>(e, false);                    // target dies earlier in the same flush
    ASSERT_TRUE(pcb.ExecuteSorted().IsOk());
    const auto& errs = pcb.GetDeferredErrors();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].systemInsertionOrder, 2u);
}

TEST_F(CommandBufferTest, DeferredSetEnabledEagerFailureFailsBuffer)
{
    Entity dead = registry->CreateEntity();
    registry->DestroyEntity(dead);
    cmdBuffer->SetEnabled<EnA>(dead, false);
    EXPECT_TRUE(cmdBuffer->Execute().IsErr());
}
```

(`EnA` = Task 1's first enableable component; adapt idioms per file.)

- [ ] **Step 3: Implement.** Record API (mirror `RemoveComponent`'s shape — header + POD payload, `StampCommand`, no inline data): `static_assert(IsEnableableV<DecayedT>, ...)`; executor calls `m_registry->SetEnabledByID(cmd->entity, cmd->componentId, cmd->enable != 0)` and returns its bool; dispatch case; placeholder resolution for `entity`.
- [ ] **Step 4: GREEN; full Debug suite** — expected +3 (772). Release/Dist 770.
- [ ] **Step 5: Commit**

```bash
git add include/Astra/Commands/Command.hpp include/Astra/Commands/CommandBuffer.hpp tests/Commands/CommandBufferTest.cpp
git commit -m "feat(commands): deferred SetEnabled - POD payload, deterministic flush, deferred-error attribution"
```

---

### Task 6: Bench gate (flat-watch)

**Files:**
- None committed. `bench-compare/lever_ec_gate.csv` stays untracked. Report-only task.

- [ ] **Step 1:** Rebuild `bench-compare/bench_astra.exe` ONLY (full-opt recipe, `/I..\tests`, PowerShell for build_one.bat). flecs/entt exes unchanged.
- [ ] **Step 2:** Quiet gate (`typeperf "\Processor(_Total)\% Processor Time" -sc 8`, under ~15-20%).
- [ ] **Step 3:** 6 interleaved rounds astra→flecs→entt → `lever_ec_gate.csv`.
- [ ] **Step 4:** Compare paired same-session medians [min,max] for create/create_batch/add/remove/destroy/random_get/iterate1/2/3 vs the Lever-3 section of `bench-compare/RESULTS.md`. ALL flat (non-overlapping-bands rule for any claimed change). Any non-noise regression = STOP and report — the compile-time gate (invariant 1) should make regression impossible; a real one means the gate leaked.
- [ ] **Step 5:** Write the table + verdict into your report (no commit; the controller carries the verdict into the merge gate).

---

### Finishing (SDD flow)

OPUS whole-branch review (per-task Minors roll-up; lens = spec §14's nine invariants verbatim + the Task-6 flat verdict) → ONE fix subagent if findings → authoritative 3-config on the final code commit → **confirm with user** → FF-merge dev local → delete branch → don't push → update memory (astra-north-star progress: first Tier-2-adjacent ergonomics/feature arc; astra-perf-optimization: flat-gate result).
