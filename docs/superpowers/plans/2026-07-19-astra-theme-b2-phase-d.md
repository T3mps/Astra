# Theme B2 — Phase D: Before/After Ordering + Ambiguity Detection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a system declare hard execution ordering with `Before<T>`/`After<T>` traits (honored by a stable topological reorder), make deferred structural commands apply in that execution order, handle ordering cycles gracefully, and — opt-in — warn when two mutably-conflicting systems' order is only an insertion-order accident.

**Architecture:** Mirror the Phase C trait mechanism. New `Before`/`After`/`AmbiguousWith` traits compose into `SystemTraits`; `ExtractSystemTraits` resolves them into per-system `TypeID::Hash()` id-lists in `SystemMetadata`. `BuildExecutionPlan` gains a pre-pass that computes a stable topological order (Kahn's, insertion-order tiebreak, deterministic cycle-break), assigns each system a `scheduleOrder`, then runs the existing contiguous-run grouper over that order with a new "ordering edge = group barrier" break term. The deferred-command sort key is stamped from `scheduleOrder` instead of the raw insertion index. An opt-in post-build pass reports conflicting-and-unordered pairs through the diagnostics seam.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-19-astra-theme-b2-phase-d-design.md` (approved).

## Global Constraints

- Header-only C++20; MSVC-primary. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a shipping check is a real `if`, never `ASTRA_ASSERT`. A cycle is a value + a log, never a throw.
- **Additive, no break.** Existing `void(Registry&)` / `void(SystemContext&)` systems, view-lambdas, and `SystemTraits<Reads<...>, Writes<...>, ReadsResources<...>, Exclusive>` all keep compiling and scheduling identically. A system that declares no ordering traits behaves exactly as today (empty edge lists ⇒ topological order = insertion order = identity permutation ⇒ `scheduleOrder == insertionOrder` ⇒ byte-identical deferred sort keys). No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include.
- **TypeID ceiling is a NON-issue here:** ordering edges reference *system* types, keyed by `TypeID<T>::Hash()` (the 64-bit key `m_systemIndices` uses) — **not** `TypeID<T>::Value()`. Edges consume zero dense ComponentIDs. Reuse existing component types (`Position`, etc.) in tests where a component is needed. (See `[[astra-flatmap-pointer-stability]]`.)
- Namespace `Astra`. IDE/clang-tidy diagnostics are misconfigured false positives (expects Clang 20; "no gtest"; "Mosaic/Platform.hpp not found") — **judge only by the MSVC build.**
- **Container choice:** the edge id-lists use `std::vector<uint64_t>` (not `SmallVector`, which the spec suggested). `SystemMetadata.hpp` already `#include <vector>`, the lists are cold and populated only for the rare system that declares edges, and `SystemExecutionContext` already uses `std::vector` — so this avoids a new dependency at zero practical cost. Intentional, minor deviation from the spec.
- All file:line anchors are from the tree at `dev` @ `ce75f6f`; **confirm each against the live tree before editing.**

**Build (per config, whole solution):**
`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`
Appending to the existing `tests/System/SystemSchedulerTest.cpp` needs **no** premake regen. Stale-PDB `mspdbsrv.exe` lock → `taskkill //F //IM mspdbsrv.exe` + rebuild. `CompressionTest.PerformanceBenchmark` is a known Release/Dist timing flake (a lone failure of only that test is not a regression; rerun isolated). Baseline entering Phase D: **624 Debug / 622 Release / 622 Dist**, all green.

## File Structure

- `include/Astra/System/System.hpp` — Task 1: `Before`/`After`/`AmbiguousWith` structs + `Detail::TraitBefore`/`TraitAfter`/`TraitAmbiguousWith` + `SystemTraits::BeforeTypes`/`AfterTypes`/`AmbiguousWithTypes`.
- `include/Astra/System/SystemMetadata.hpp` — Task 1: `beforeIds`/`afterIds`/`ambiguousWithIds` + `scheduleOrder` fields.
- `include/Astra/System/SystemScheduler.hpp` — Task 1: `ExtractSystemIdList`, `ComputeScheduleOrder`, grouping over the topo order; Task 2: edge-as-barrier; Task 3: `SystemError::OrderingCycle`, `ValidateSchedule`, cycle log; Task 5: ambiguity flag + detector.
- `include/Astra/System/SystemExecutor.hpp` — Task 4: swap the `SortKey` primary source from `insertionOrder` to `scheduleOrder`.
- `tests/System/SystemSchedulerTest.cpp` — every task appends tests (existing file → no regen).

---

### Task 1: Ordering traits + metadata + extraction + topological reorder

The core engine: declaring `Before`/`After` reorders systems, observable via `GetExecutionPlan()` group order.

**Files:**
- Modify: `include/Astra/System/System.hpp` (`Reads`/`Writes` ~:21-25, `Detail` ~:31-37, `SystemTraits` ~:40-47)
- Modify: `include/Astra/System/SystemMetadata.hpp` (after `requiresExclusive` ~:36)
- Modify: `include/Astra/System/SystemScheduler.hpp` (`ExtractSystemTraits` ~:433-448, `BuildExecutionPlan` ~:490-553, class private members ~:673)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces (consumed by later tasks): `Astra::Before<S...>` / `Astra::After<S...>` / `Astra::AmbiguousWith<S...>`; `SystemTraits<...>::BeforeTypes`/`AfterTypes`/`AmbiguousWithTypes`; `SystemMetadata::beforeIds`/`afterIds`/`ambiguousWithIds` (`std::vector<uint64_t>` of `TypeID::Hash()`); `SystemMetadata::scheduleOrder` (`size_t`); private `ComputeScheduleOrder()` (returns `std::vector<size_t>` topo order); private members `m_scheduleHadCycle` (`bool`) and `m_cycleMembers` (`std::vector<size_t>` of insertionOrders) — populated here, surfaced in Task 3.

- [ ] **Step 1: Write the failing tests.** Append to `tests/System/SystemSchedulerTest.cpp`. Add ordering systems in a new anonymous namespace near the existing traited structs. `WOrdX`/`WOrdX2` both write the SAME component (`Position`) so they always conflict → always land in separate groups, making their *order* observable in the plan:
```cpp
namespace  // Phase D ordering systems
{
    // Both write Position => always conflict => always separate groups.
    struct OrdA  : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct OrdB  : Astra::SystemTraits<Astra::Writes<Position>, Astra::After<OrdA>>  { void operator()(Astra::Registry&) {} };
    struct OrdC  : Astra::SystemTraits<Astra::Writes<Position>, Astra::Before<OrdA>> { void operator()(Astra::Registry&) {} };
}

// Compile-time: the trait aliases collect the declared ordering targets.
static_assert(std::is_same_v<Astra::SystemTraits<Astra::After<OrdA>>::AfterTypes,  std::tuple<OrdA>>);
static_assert(std::is_same_v<Astra::SystemTraits<Astra::Before<OrdA>>::BeforeTypes, std::tuple<OrdA>>);
static_assert(std::is_same_v<Astra::SystemTraits<Astra::AmbiguousWith<OrdA>>::AmbiguousWithTypes, std::tuple<OrdA>>);

// With no ordering edges at all, groups follow registration order (baseline
// unchanged). WPosOnly (Writes<Position>, defined in the Phase C tests earlier
// in this same TU, so its anonymous-namespace type is visible here) has no edges.
TEST(SystemSchedulerOrdering, NoEdgesKeepsInsertionOrder)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<OrdA>().IsOk());     // index 0, no edges
    ASSERT_TRUE(s.AddSystem<WPosOnly>().IsOk()); // index 1, no edges (both write Position => conflict => 2 groups)
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 0u);  // OrdA first (insertion order preserved)
    EXPECT_EQ(plan[1][0], 1u);  // WPosOnly second
}

// After<OrdA> on a system registered BEFORE OrdA moves it after OrdA.
TEST(SystemSchedulerOrdering, AfterEdgeReordersEarlierRegisteredSystem)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<OrdB>().IsOk());   // index 0, declares After<OrdA>
    ASSERT_TRUE(s.AddSystem<OrdA>().IsOk());   // index 1
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 1u);  // OrdA (index 1) runs first
    EXPECT_EQ(plan[1][0], 0u);  // OrdB (index 0) runs second, per After<OrdA>
}

// Before<OrdA> on a system registered AFTER OrdA moves it before OrdA.
TEST(SystemSchedulerOrdering, BeforeEdgeReordersLaterRegisteredSystem)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<OrdA>().IsOk());   // index 0
    ASSERT_TRUE(s.AddSystem<OrdC>().IsOk());   // index 1, declares Before<OrdA>
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 1u);  // OrdC (index 1) runs first, per Before<OrdA>
    EXPECT_EQ(plan[1][0], 0u);  // OrdA (index 0) runs second
}
```

- [ ] **Step 2: Run — verify RED.** Build Debug. Expected: **compile failure** — `Before`/`After`/`AmbiguousWith` and the `*Types` aliases are not declared. (Correct RED: the feature is missing; `AstraTest` will not link until Steps 3-7 land.)

- [ ] **Step 3: Add the traits (`System.hpp`).** After `struct Writes` (~:25) — and before `struct Exclusive` — add:
```cpp
    template<typename... Systems>
    struct Before { using type = std::tuple<Systems...>; };

    template<typename... Systems>
    struct After { using type = std::tuple<Systems...>; };

    template<typename... Systems>
    struct AmbiguousWith { using type = std::tuple<Systems...>; };
```
In `namespace Detail` after `TraitWrites` (~:36) add:
```cpp
        template<typename T> struct TraitBefore { using type = std::tuple<>; };
        template<typename... S> struct TraitBefore<Before<S...>> { using type = std::tuple<S...>; };
        template<typename T> struct TraitAfter  { using type = std::tuple<>; };
        template<typename... S> struct TraitAfter<After<S...>>   { using type = std::tuple<S...>; };
        template<typename T> struct TraitAmbiguousWith { using type = std::tuple<>; };
        template<typename... S> struct TraitAmbiguousWith<AmbiguousWith<S...>> { using type = std::tuple<S...>; };
```
In `struct SystemTraits` after `WritesComponents` (~:44) add:
```cpp
        using BeforeTypes        = decltype(std::tuple_cat(std::declval<typename Detail::TraitBefore<Traits>::type>()...));
        using AfterTypes         = decltype(std::tuple_cat(std::declval<typename Detail::TraitAfter<Traits>::type>()...));
        using AmbiguousWithTypes = decltype(std::tuple_cat(std::declval<typename Detail::TraitAmbiguousWith<Traits>::type>()...));
```

- [ ] **Step 4: Add the metadata fields (`SystemMetadata.hpp`).** After `bool requiresExclusive = false;` (~:36), still inside `struct SystemMetadata`, add:
```cpp
        // Explicit ordering edges (Phase D), resolved to the target systems'
        // TypeID::Hash() -- the same 64-bit key m_systemIndices uses. Filled by
        // ExtractSystemTraits; resolved to indices in BuildExecutionPlan.
        std::vector<uint64_t> beforeIds;
        std::vector<uint64_t> afterIds;
        std::vector<uint64_t> ambiguousWithIds;

        // Position of this system in the topological execution order (filled by
        // BuildExecutionPlan). Equals insertionOrder when no ordering edges
        // exist. Primary key of the deferred-command SortKey (see Task 4).
        size_t scheduleOrder = 0;
```
`SystemMetadata` stays an aggregate (default member initializers are allowed). The new fields sit at the struct end, all default-constructible/initialized, so **no** `SystemMetadata{...}` construction site (there are four, all designated-init ending at `.requiresExclusive`) needs changing — confirm by grepping `SystemMetadata` construction sites and checking each lists fields only up to `.requiresExclusive`.

- [ ] **Step 5: Add the id-list extraction helper + fill the masks (`SystemScheduler.hpp`).** In `ExtractSystemTraits` (~:433), inside `if constexpr (HasSystemTraits_v<T>)`, after the Phase C resource block, add:
```cpp
                if constexpr (requires { typename T::BeforeTypes; typename T::AfterTypes; typename T::AmbiguousWithTypes; })
                {
                    ExtractSystemIdList<typename T::BeforeTypes>(metadata.beforeIds);
                    ExtractSystemIdList<typename T::AfterTypes>(metadata.afterIds);
                    ExtractSystemIdList<typename T::AmbiguousWithTypes>(metadata.ambiguousWithIds);
                }
```
Add the helper beside `ExtractComponentMask` (~:450):
```cpp
        // Push TypeID<Each>::Hash() for each system type in the tuple into `out`
        // (the same 64-bit key m_systemIndices uses to look systems up).
        template<typename Tuple>
        void ExtractSystemIdList(std::vector<uint64_t>& out)
        {
            ExtractSystemIdListImpl<Tuple>(out, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        template<typename Tuple, size_t... Is>
        void ExtractSystemIdListImpl(std::vector<uint64_t>& out, std::index_sequence<Is...>)
        {
            ((out.push_back(TypeID<std::tuple_element_t<Is, Tuple>>::Hash())), ...);
        }
```

- [ ] **Step 6: Add `ComputeScheduleOrder` + cycle members (`SystemScheduler.hpp`).** Add the private members beside `m_systemIndices` (~:673):
```cpp
        bool m_scheduleHadCycle = false;            // set by ComputeScheduleOrder; surfaced in Task 3
        std::vector<size_t> m_cycleMembers;         // insertionOrders forced during a cycle break (Task 3 log)
```
Add the method beside `BuildExecutionPlan` (before it, ~:489):
```cpp
        // Stable topological order over the Before/After edges. Honors every
        // resolved edge; among unconstrained systems preserves insertion order
        // (m_systems is stored in registration order, so index == insertionOrder,
        // and picking the lowest ready index is the insertion-order tiebreak).
        // A cycle cannot be ordered -- it is broken deterministically by forcing
        // the lowest-index unplaced system, so this ALWAYS terminates with a
        // total order (m_scheduleHadCycle records that a break happened).
        std::vector<size_t> ComputeScheduleOrder()
        {
            m_scheduleHadCycle = false;
            m_cycleMembers.clear();
            const size_t n = m_systems.size();
            const size_t UNKNOWN = n;

            auto resolve = [&](uint64_t hash) -> size_t
            {
                auto it = m_systemIndices.Find(hash);
                return it == m_systemIndices.end() ? UNKNOWN : it->second;
            };

            // predecessor -> successor adjacency (predecessor runs first) + in-degrees.
            std::vector<std::vector<size_t>> succ(n);
            std::vector<size_t> indeg(n, 0);
            for (size_t s = 0; s < n; ++s)
            {
                const auto& md = m_systems[s].metadata;
                for (uint64_t h : md.afterIds)   // After<T> on s: T runs before s => T -> s
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s) { succ[t].push_back(s); ++indeg[s]; }
                }
                for (uint64_t h : md.beforeIds)  // Before<T> on s: s runs before T => s -> T
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s) { succ[s].push_back(t); ++indeg[t]; }
                }
            }

            std::vector<size_t> order;
            order.reserve(n);
            std::vector<bool> placed(n, false);
            while (order.size() < n)
            {
                size_t pick = UNKNOWN;
                for (size_t k = 0; k < n; ++k)
                    if (!placed[k] && indeg[k] == 0) { pick = k; break; }  // lowest ready index
                if (pick == UNKNOWN)
                {
                    // Cycle: no ready node but systems remain. Force the lowest
                    // unplaced index (deterministic), recording it for Task 3.
                    m_scheduleHadCycle = true;
                    for (size_t k = 0; k < n; ++k)
                        if (!placed[k]) { pick = k; break; }
                    m_cycleMembers.push_back(m_systems[pick].metadata.insertionOrder);
                }
                placed[pick] = true;
                order.push_back(pick);
                for (size_t nx : succ[pick])
                    if (indeg[nx] > 0) --indeg[nx];
            }
            return order;
        }
```

- [ ] **Step 7: Rewrite `BuildExecutionPlan`'s loop to iterate the topo order (`SystemScheduler.hpp` ~:509-550).** Keep the `m_executionPlan.clear()` / empty-guard / `declaresAccess` lambda as-is. Replace the `size_t i = 0; while (i < m_systems.size())` loop (currently indexing `m_systems[i]`/`m_systems[j]` directly) with a loop over `order`, storing original indices and assigning `scheduleOrder`:
```cpp
            const std::vector<size_t> order = ComputeScheduleOrder();
            for (size_t k = 0; k < order.size(); ++k)
                m_systems[order[k]].metadata.scheduleOrder = k;

            size_t p = 0;
            while (p < order.size())
            {
                const size_t iIdx = order[p];
                const auto& sysI = m_systems[iIdx].metadata;

                std::vector<size_t> group;
                group.push_back(iIdx);
                ComponentMask groupReads = sysI.reads;
                ComponentMask groupWrites = sysI.writes;
                ComponentMask groupResourceReads = sysI.resourceReads;
                ComponentMask groupResourceWrites = sysI.resourceWrites;

                const bool acceptsMore = !sysI.requiresExclusive && declaresAccess(sysI);

                size_t q = p + 1;
                for (; acceptsMore && q < order.size(); ++q)
                {
                    const size_t jIdx = order[q];
                    const auto& sysJ = m_systems[jIdx].metadata;

                    if (sysJ.requiresExclusive || !declaresAccess(sysJ))
                        break;
                    if ((sysJ.writes & groupWrites).Any() ||
                        (sysJ.writes & groupReads ).Any() ||
                        (sysJ.reads  & groupWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceReads ).Any() ||
                        (sysJ.resourceReads  & groupResourceWrites).Any())
                        break;
                    // (Task 2 inserts the edge-as-barrier break here.)

                    group.push_back(jIdx);
                    groupReads  |= sysJ.reads;
                    groupWrites |= sysJ.writes;
                    groupResourceReads  |= sysJ.resourceReads;
                    groupResourceWrites |= sysJ.resourceWrites;
                }

                m_executionPlan.push_back(std::move(group));
                p = q;
            }

            m_needsRebuild = false;
```

- [ ] **Step 8: Run — verify GREEN.** Build all 3 configs. Run `AstraTest.exe --gtest_filter=SystemSchedulerOrdering.*` (3 tests pass) each config. Confirm the pre-existing `SystemScheduler*` grouping and resource tests still pass (no-edge schedules are the identity permutation → unchanged). `git grep -n 'Commands/\|System/' include/Astra/Registry/Registry.hpp` shows nothing new. Full suite green (baseline 624/622/622 + 3 → 627/625/625).

- [ ] **Step 9: Commit.**
```bash
git add include/Astra/System/System.hpp include/Astra/System/SystemMetadata.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): Before/After ordering traits + stable topological reorder in the scheduler"
```

---

### Task 2: Explicit ordering edge as a grouping barrier

An ordering edge must serialize even two systems with disjoint masks (they'd otherwise share a parallel group and lose the declared order).

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (add `IsOrderingPredecessor` helper; add the break term inside the Task-1 grouping loop)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `beforeIds`/`afterIds`, `SystemMetadata::typeId`, the topo-ordered grouping loop.
- Produces: private `IsOrderingPredecessor(size_t predIdx, size_t succIdx) const`.

- [ ] **Step 1: Write the failing test.** Append. Two systems with **disjoint** masks (`Position` vs `Velocity`) plus an ordering edge must NOT share a group:
```cpp
namespace  // Phase D edge-barrier systems (disjoint masks)
{
    struct BarWritesPos : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct BarWritesVel : Astra::SystemTraits<Astra::Writes<Velocity>, Astra::After<BarWritesPos>> { void operator()(Astra::Registry&) {} };
    struct BarPlainVel  : Astra::SystemTraits<Astra::Writes<Velocity>> { void operator()(Astra::Registry&) {} };
}

// Disjoint masks but an After edge => serialized into separate groups.
TEST(SystemSchedulerOrdering, OrderingEdgeSplitsDisjointMaskSystems)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<BarWritesPos>().IsOk());  // index 0
    ASSERT_TRUE(s.AddSystem<BarWritesVel>().IsOk());  // index 1, After<BarWritesPos>
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);         // NOT one group of two
    EXPECT_EQ(plan[0][0], 0u);          // BarWritesPos first
    EXPECT_EQ(plan[1][0], 1u);          // BarWritesVel second
}

// Control: the SAME two disjoint-mask systems WITHOUT an edge share one group.
TEST(SystemSchedulerOrdering, DisjointMaskSystemsWithoutEdgeShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<BarWritesPos>().IsOk());
    ASSERT_TRUE(s.AddSystem<BarPlainVel>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}
```

- [ ] **Step 2: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerOrdering.OrderingEdgeSplitsDisjointMaskSystems`. Expected: **FAIL — `plan.size()` is 1, expected 2.** (With only the mask-conflict check, `BarWritesPos` and `BarWritesVel` have disjoint masks, so the run does not break and they share a group.) `DisjointMaskSystemsWithoutEdgeShareGroup` already passes.

- [ ] **Step 3: Add the predecessor helper (`SystemScheduler.hpp`).** Beside `ComputeScheduleOrder`:
```cpp
        // True if there is a DIRECT ordering edge predIdx -> succIdx, i.e. succ
        // declares After<pred> or pred declares Before<succ>. (Systems are keyed
        // by TypeID::Hash(), stored in metadata.typeId.) Direct edges suffice for
        // the grouping barrier: because the plan is grouped over the topological
        // order in contiguous runs, any transitive predecessor sits in an earlier,
        // already-closed group, so it can never be a current-group member.
        ASTRA_NODISCARD bool IsOrderingPredecessor(size_t predIdx, size_t succIdx) const
        {
            const auto& pred = m_systems[predIdx].metadata;
            const auto& succ = m_systems[succIdx].metadata;
            const uint64_t predHash = static_cast<uint64_t>(pred.typeId);
            const uint64_t succHash = static_cast<uint64_t>(succ.typeId);
            for (uint64_t h : succ.afterIds)  if (h == predHash) return true;
            for (uint64_t h : pred.beforeIds) if (h == succHash) return true;
            return false;
        }
```

- [ ] **Step 4: Add the barrier break in the grouping loop (`SystemScheduler.hpp`).** At the `// (Task 2 inserts the edge-as-barrier break here.)` marker inside the `for (; acceptsMore ...)` loop (after the mask-conflict `if`, before `group.push_back(jIdx)`), insert:
```cpp
                    // Edge-as-barrier: an explicit ordering edge into sysJ from any
                    // current-group member forces sysJ into a later group, even with
                    // disjoint masks (the edge demands serialization).
                    {
                        bool blockedByEdge = false;
                        for (size_t member : group)
                            if (IsOrderingPredecessor(member, jIdx)) { blockedByEdge = true; break; }
                        if (blockedByEdge)
                            break;
                    }
```

- [ ] **Step 5: Run — verify GREEN.** Build all 3 configs. `--gtest_filter=SystemSchedulerOrdering.*` (5 tests pass). Full suite green (→ 629/627/627).

- [ ] **Step 6: Commit.**
```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): treat an explicit ordering edge as a parallel-group barrier"
```

---

### Task 3: Cycle surfacing — `OrderingCycle` + `ValidateSchedule` + diagnostics

A `Before`/`After` cycle is reported (log + `Result`) and broken deterministically; `Execute` still runs.

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (`SystemError` enum ~:25-30; add `ValidateSchedule()`; emit the log inside `BuildExecutionPlan` after grouping; add `#include "../Core/Log.hpp"`)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `m_scheduleHadCycle`, `m_cycleMembers`, `m_needsRebuild`, `BuildExecutionPlan()`.
- Produces: `SystemError::OrderingCycle`; public `Result<void, SystemError> ValidateSchedule()`.

- [ ] **Step 1: Write the failing tests.** Append. Two systems that each declare `After` the other form a cycle. (Use disjoint masks so nothing but the edges relates them.)
```cpp
namespace  // Phase D cycle systems
{
    struct CycA;
    struct CycB;
    struct CycA : Astra::SystemTraits<Astra::Writes<Position>, Astra::After<CycB>> { void operator()(Astra::Registry&) {} };
    struct CycB : Astra::SystemTraits<Astra::Writes<Velocity>, Astra::After<CycA>> { void operator()(Astra::Registry&) {} };
    struct AcyclicPos : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
}

// A Before/After cycle is reported through ValidateSchedule(), not aborted.
TEST(SystemSchedulerOrdering, CycleReportedViaValidateSchedule)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<CycA>().IsOk());
    ASSERT_TRUE(s.AddSystem<CycB>().IsOk());
    auto r = s.ValidateSchedule();
    ASSERT_TRUE(r.IsErr());
    EXPECT_EQ(r.Error(), Astra::SystemError::OrderingCycle);
}

// Despite the cycle, the broken order is deterministic (two independently-built
// schedulers with identical registration produce the identical plan) and Execute
// still runs without aborting.
TEST(SystemSchedulerOrdering, CycleStillProducesDeterministicPlan)
{
    Astra::SystemScheduler s1;
    ASSERT_TRUE(s1.AddSystem<CycA>().IsOk());
    ASSERT_TRUE(s1.AddSystem<CycB>().IsOk());
    Astra::SystemScheduler s2;
    ASSERT_TRUE(s2.AddSystem<CycA>().IsOk());
    ASSERT_TRUE(s2.AddSystem<CycB>().IsOk());
    const auto plan1 = s1.GetExecutionPlan();    // copy
    const auto plan2 = s2.GetExecutionPlan();
    EXPECT_EQ(plan1, plan2);                      // deterministic break, independent of build instance
    EXPECT_EQ(plan1.size(), 2u);                 // both systems present after the break
    Astra::Registry reg;
    EXPECT_NO_FATAL_FAILURE(s1.Execute(reg));     // does not abort
}

// No cycle => ValidateSchedule() is Ok.
TEST(SystemSchedulerOrdering, AcyclicScheduleValidatesOk)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<AcyclicPos>().IsOk());
    ASSERT_TRUE(s.AddSystem<OrdA>().IsOk());
    EXPECT_TRUE(s.ValidateSchedule().IsOk());
}

// An edge to a system not registered here is ignored (no cycle, no reorder, Ok).
TEST(SystemSchedulerOrdering, UnknownEdgeTargetIsIgnored)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<OrdB>().IsOk());   // declares After<OrdA>, but OrdA is NOT registered here
    EXPECT_TRUE(s.ValidateSchedule().IsOk());
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0][0], 0u);
}
```
> Note on `CycleStillProducesDeterministicPlan`: `CycA` writes `Position`, `CycB` writes `Velocity` (disjoint), so after the cycle is broken they do not mask-conflict — but the residual ordering edge (the one NOT dropped) is an edge-barrier (Task 2), so they still land in two groups. `plan.size()==2` holds either way; the assertion of interest is `plan1 == plan2` (determinism) and no abort.

- [ ] **Step 2: Run — verify RED.** Build. Expected: **compile failure** — `SystemError::OrderingCycle` and `ValidateSchedule` do not exist.

- [ ] **Step 3: Add the error variant + include (`SystemScheduler.hpp`).** In `enum class SystemError` (~:25) add a member:
```cpp
        AllocationFailed,   // nothrow allocation of the system instance failed
        SchedulerExecuting, // registration attempted while Execute() is running
        OrderingCycle       // a Before/After cycle was detected at plan build
```
(Keep the existing members; `OrderingCycle` is additive.) Add near the other Core includes (~:14):
```cpp
#include "../Core/Log.hpp"
```

- [ ] **Step 4: Emit the cycle log in `BuildExecutionPlan` (`SystemScheduler.hpp`).** At the very end of `BuildExecutionPlan`, after the grouping loop and before/after `m_needsRebuild = false;`, add:
```cpp
            if (m_scheduleHadCycle)
            {
                std::string msg = "SystemScheduler: Before/After ordering cycle detected and broken "
                                  "deterministically (insertion-order fallback). Systems forced during "
                                  "the break (by insertionOrder):";
                for (size_t io : m_cycleMembers)
                    msg += ' ' + std::to_string(io);
                ASTRA_LOG_ERROR(msg);
            }
```
Add `#include <string>` if not already present (it is transitively via `<vector>`/STL, but include it explicitly at the top for clarity).

- [ ] **Step 5: Add `ValidateSchedule()` (`SystemScheduler.hpp`).** Add a public method near `GetExecutionPlan()` (~:410):
```cpp
        // Forces a plan build if needed, then reports whether the last build hit
        // a Before/After cycle. Ok() means the declared ordering is acyclic.
        // (A cycle is still handled gracefully -- Execute() runs with a
        // deterministic fallback order -- this is the explicit programmatic check.)
        ASTRA_NODISCARD Result<void, SystemError> ValidateSchedule()
        {
            if (m_needsRebuild)
                BuildExecutionPlan();
            if (m_scheduleHadCycle)
                return Result<void, SystemError>::Err(SystemError::OrderingCycle);
            return Result<void, SystemError>::Ok();
        }
```

- [ ] **Step 6: Run — verify GREEN.** Build all 3 configs. `--gtest_filter=SystemSchedulerOrdering.*` (9 tests pass). Note: `CycleStillProducesDeterministicPlan` and `CycleReportedViaValidateSchedule` will emit an `[error]` log line via the default sink (stderr) during the run — this is expected; the tests do not assert on the log here (Task 5 exercises the log-sink capture). Full suite green (→ 633/631/631).

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): recoverable Before/After cycle handling via ValidateSchedule + diagnostics log"
```

---

### Task 4: Deferred commands apply in execution (schedule) order

Stamp the deferred-command `SortKey` from `scheduleOrder`, so a reordered system's deferred structural changes apply in the order it actually ran.

**Files:**
- Modify: `include/Astra/System/SystemExecutor.hpp` (`DispatchSystem` ~:39-42)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `SystemMetadata::scheduleOrder`; the existing `SystemContext` ctor (its 3rd param is the sort-key primary), `ParallelCommandBuffer`, `TestWorkerPool`.
- Produces: no new API — a semantics change (deferred apply order follows `scheduleOrder`).

- [ ] **Step 1: Write the failing test.** Append. Two context systems that defer overlapping ops on ONE real entity; the After edge must decide the apply order. `AddSysA` adds a tag component, `AddSysB` (After A) removes it — final state distinguishes execution order (tag absent) from registration order (tag present).
```cpp
namespace  // Phase D deferred-apply-order systems
{
    struct DTag { int v = 0; };
    // Recorded-against entity is shared via a namespace-scope handle set by the test.
    inline Astra::Entity g_applyOrderEntity{};

    struct AddsTag : Astra::SystemTraits<Astra::Exclusive>
    {
        void operator()(Astra::SystemContext& ctx) { ctx.Commands().AddComponent<DTag>(g_applyOrderEntity, DTag{7}); }
    };
    // Registered BEFORE AddsTag, but After<AddsTag> => must run/apply second.
    struct RemovesTag : Astra::SystemTraits<Astra::Exclusive, Astra::After<AddsTag>>
    {
        void operator()(Astra::SystemContext& ctx) { ctx.Commands().RemoveComponent<DTag>(g_applyOrderEntity); }
    };
}

// After<AddsTag> on a system registered first => its Remove applies AFTER the Add
// => the tag ends up absent. (Registration-order apply would remove-then-add => present.)
TEST(SystemSchedulerOrdering, DeferredCommandsApplyInScheduleOrder)
{
    Astra::Registry reg;
    g_applyOrderEntity = reg.CreateEntity();
    reg.AddComponent<DTag>(g_applyOrderEntity, DTag{1});  // pre-exists so RemoveComponent is valid

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<RemovesTag>().IsOk());  // index 0, After<AddsTag>
    ASSERT_TRUE(s.AddSystem<AddsTag>().IsOk());     // index 1
    s.Execute(reg);   // schedule order: AddsTag then RemovesTag

    EXPECT_FALSE(reg.HasComponent<DTag>(g_applyOrderEntity));  // Remove applied last
}
```
> Both systems are `Exclusive` (each runs solo) purely so this test isolates apply-order from grouping; the ordering edge still reorders them. `AddComponent`/`RemoveComponent`/`HasComponent`/`CreateEntity` are the standard `Registry`/`CommandBuffer` API used throughout the existing System tests — mirror their exact spelling from a neighboring deferred-command test if unsure.

- [ ] **Step 2: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerOrdering.DeferredCommandsApplyInScheduleOrder`. Expected: **FAIL — the tag is present.** (`DispatchSystem` still stamps the sort key from `insertionOrder`: `RemovesTag` has `insertionOrder 0`, so its Remove sorts first and is skipped on the not-yet-present tag; then `AddsTag` adds it. Final: tag present, so `EXPECT_FALSE(HasComponent)` fails.)

- [ ] **Step 3: Swap the sort-key source to `scheduleOrder` (`SystemExecutor.hpp`).** In `DispatchSystem` (~:39), change the 3rd `SystemContext` ctor argument from `insertionOrder` to `scheduleOrder`:
```cpp
            SystemContext sysCtx(*context.registry,
                context.commandBuffer->GetThreadBuffer(),
                // Sort-key primary = this system's SCHEDULE order (topological
                // position), so deferred commands apply in execution order.
                // Equals insertionOrder when no Before/After edges exist.
                static_cast<uint32_t>(context.metadata[systemIdx].scheduleOrder),
                0u, context.commandBuffer);
```

- [ ] **Step 4: Run — verify GREEN.** Build all 3 configs. The new test passes (tag absent). Crucially, confirm **the entire Phase A/B determinism suite stays green unchanged** — with no ordering edges `scheduleOrder == insertionOrder`, so every existing deferred-command sort key is byte-identical. Run `--gtest_filter=*Deferred*:*Determinism*:*Parallel*:SystemSchedulerOrdering.*`. Full suite green (→ 634/632/632).

- [ ] **Step 5: Add a determinism gate for the reordered + deferred path.** Append a repeated-run test that a reordered schedule deferring structural changes is byte-identical across many runs under a real `TestWorkerPool`. Reuse the worker-pool + repeated-run harness pattern from the existing Phase B chunk-parallel determinism gate in this same file (search the file for the existing `TestWorkerPool` construction and `ParallelExecutor` usage; mirror it). The gate:
```cpp
// A reordered schedule that defers a create from each of several systems is
// byte-identical every run (schedule-order sort key is deterministic).
TEST(SystemSchedulerOrdering, ReorderedDeferredScheduleIsDeterministicAcrossRuns)
{
    auto run = []{
        Astra::Registry reg;
        auto pool = std::make_shared<Astra::Testing::TestWorkerPool>(4);  // match neighboring gates' spelling
        Astra::ParallelExecutor exec(pool);
        Astra::SystemScheduler s;
        // Register several Exclusive context systems with cross Before/After edges
        // so the schedule order is a non-trivial permutation; each defers a create
        // with a distinct component value. (Reuse the same small system set below.)
        ASSERT_TRUE(s.AddSystem<AddsTag>().IsOk());
        ASSERT_TRUE(s.AddSystem<RemovesTag>().IsOk());
        // ... (mirror the existing gate: build the world, execute, snapshot) ...
        return SnapshotWorld(reg);   // reuse the file's existing snapshot helper if present, else compare component values of a known entity set
    };
    const auto oracle = run();
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(run(), oracle);
}
```
> If the file has no reusable `SnapshotWorld`/gate helper, keep this gate minimal: assert a single deterministic observable (e.g. the final component value on a fixed entity) is equal across 20 runs. The point is to pin that the schedule-order sort key does not flake under the parallel executor. Do not introduce new fresh component types (TypeID ceiling) — reuse `Position`/`Velocity`/`DTag`.

- [ ] **Step 6: Run — verify GREEN.** Build + run all 3 configs; the gate passes 20/20. Full suite green (→ 635/633/633).

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/System/SystemExecutor.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): deferred commands apply in schedule (execution) order via scheduleOrder sort key"
```

---

### Task 5: Ambiguity detection + `AmbiguousWith` suppression

Opt-in: after a build, warn about conflicting-and-unordered system pairs through the diagnostics seam.

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (add `SetAmbiguityReporting`; `m_reportAmbiguities` member; `Conflicts` + reachability helpers + `ReportAmbiguities`; call it at the end of `BuildExecutionPlan`)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Phase C conflict masks; Task 1 edge id-lists + `m_systemIndices`; `SystemMetadata::typeId`/`insertionOrder`; `Astra::MAX_COMPONENTS`; the `Astra::Testing::ScopedLogSink` test guard.
- Produces: public `void SetAmbiguityReporting(bool)`.

- [ ] **Step 1: Write the failing tests.** Append. Use the log-sink capture pattern from `tests/Core/LogTest.cpp` (include `../Support/DiagnosticsTestGuards.hpp`). Systems that conflict (same component write) but declare no order should warn; ordering or `AmbiguousWith` silences it; off-by-default is silent.
```cpp
namespace  // Phase D ambiguity systems
{
    struct AmbW1 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct AmbW2 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct AmbOrdered1 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct AmbOrdered2 : Astra::SystemTraits<Astra::Writes<Position>, Astra::After<AmbOrdered1>> { void operator()(Astra::Registry&) {} };
    struct AmbSup2;
    struct AmbSup1 : Astra::SystemTraits<Astra::Writes<Position>, Astra::AmbiguousWith<AmbSup2>> { void operator()(Astra::Registry&) {} };
    struct AmbSup2 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };

    struct AmbCapture { int warnCount = 0; std::string last; };
    inline void AmbSink(const Astra::LogRecord& r, void* user) noexcept
    {
        if (r.level == Astra::LogLevel::Warn)
        {
            auto* c = static_cast<AmbCapture*>(user);
            c->warnCount++;
            c->last = std::string(r.message);
        }
    }
}

// Opt-in ambiguity report fires for a conflicting, unordered pair.
TEST(SystemSchedulerOrdering, AmbiguityReportedForUnorderedConflict)
{
    AmbCapture cap;
    Astra::Testing::ScopedLogSink guard(&AmbSink, &cap);
    Astra::SystemScheduler s;
    s.SetAmbiguityReporting(true);
    ASSERT_TRUE(s.AddSystem<AmbW1>().IsOk());
    ASSERT_TRUE(s.AddSystem<AmbW2>().IsOk());
    (void)s.GetExecutionPlan();   // triggers the build + report
    EXPECT_EQ(cap.warnCount, 1);
}

// Off by default: no report.
TEST(SystemSchedulerOrdering, AmbiguityNotReportedWhenDisabled)
{
    AmbCapture cap;
    Astra::Testing::ScopedLogSink guard(&AmbSink, &cap);
    Astra::SystemScheduler s;  // reporting NOT enabled
    ASSERT_TRUE(s.AddSystem<AmbW1>().IsOk());
    ASSERT_TRUE(s.AddSystem<AmbW2>().IsOk());
    (void)s.GetExecutionPlan();
    EXPECT_EQ(cap.warnCount, 0);
}

// An explicit ordering edge silences the report.
TEST(SystemSchedulerOrdering, AmbiguitySilencedByOrderingEdge)
{
    AmbCapture cap;
    Astra::Testing::ScopedLogSink guard(&AmbSink, &cap);
    Astra::SystemScheduler s;
    s.SetAmbiguityReporting(true);
    ASSERT_TRUE(s.AddSystem<AmbOrdered1>().IsOk());
    ASSERT_TRUE(s.AddSystem<AmbOrdered2>().IsOk());   // After<AmbOrdered1>
    (void)s.GetExecutionPlan();
    EXPECT_EQ(cap.warnCount, 0);
}

// AmbiguousWith suppresses the report for a genuinely order-independent pair.
TEST(SystemSchedulerOrdering, AmbiguitySilencedByAmbiguousWith)
{
    AmbCapture cap;
    Astra::Testing::ScopedLogSink guard(&AmbSink, &cap);
    Astra::SystemScheduler s;
    s.SetAmbiguityReporting(true);
    ASSERT_TRUE(s.AddSystem<AmbSup1>().IsOk());   // AmbiguousWith<AmbSup2>
    ASSERT_TRUE(s.AddSystem<AmbSup2>().IsOk());
    (void)s.GetExecutionPlan();
    EXPECT_EQ(cap.warnCount, 0);
}
```

- [ ] **Step 2: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerOrdering.Ambiguity*`. Expected: **compile failure** — `SetAmbiguityReporting` does not exist.

- [ ] **Step 3: Add the flag + accessor (`SystemScheduler.hpp`).** Add a private member beside `m_scheduleHadCycle`:
```cpp
        bool m_reportAmbiguities = false;  // opt-in ambiguity reporting (Phase D §12)
```
Add a public setter near `ValidateSchedule()`:
```cpp
        // Opt-in (default off): after each plan build, report every pair of
        // systems that mutably conflict (share a component or resource with a
        // write on either side) whose relative order is fixed only by an
        // insertion-order accident -- no Before/After edge (direct or transitive)
        // orders them and neither declares AmbiguousWith the other. Reported via
        // ASTRA_LOG_WARN; a development aid, never an error.
        void SetAmbiguityReporting(bool enabled) noexcept { m_reportAmbiguities = enabled; }
```

- [ ] **Step 4: Add the pairwise conflict + reachability + report helpers (`SystemScheduler.hpp`).** Beside `IsOrderingPredecessor`:
```cpp
        // Pairwise mutable conflict: share a component OR resource with a write on
        // either side. (Same semantics as the grouping conflict test, in pairwise
        // form.) Returns the first conflicting component bit in `outComp` (or
        // MAX_COMPONENTS if the conflict is resource-only), and likewise the first
        // resource bit in `outRes`, for the report.
        ASTRA_NODISCARD static bool Conflicts(const SystemMetadata& a, const SystemMetadata& b,
                                              size_t& outComp, size_t& outRes)
        {
            const ComponentMask comp = (a.writes & b.writes) | (a.writes & b.reads) | (a.reads & b.writes);
            const ComponentMask res  = (a.resourceWrites & b.resourceWrites)
                                     | (a.resourceWrites & b.resourceReads)
                                     | (a.resourceReads  & b.resourceWrites);
            outComp = MAX_COMPONENTS;
            outRes  = MAX_COMPONENTS;
            for (size_t bit = 0; bit < MAX_COMPONENTS; ++bit)
            {
                if (outComp == MAX_COMPONENTS && comp.Test(bit)) outComp = bit;
                if (outRes  == MAX_COMPONENTS && res.Test(bit))  outRes  = bit;
            }
            return comp.Any() || res.Any();
        }

        // Reachability over the resolved ordering DAG: can `from` reach `to` by
        // following Before/After edges (transitively)? Used to decide whether a
        // conflicting pair is already ordered. n is small (tens); a per-query DFS
        // is fine.
        ASTRA_NODISCARD bool OrderingReaches(size_t from, size_t to,
                                             const std::vector<std::vector<size_t>>& succ) const
        {
            std::vector<bool> seen(succ.size(), false);
            std::vector<size_t> stack{from};
            while (!stack.empty())
            {
                size_t cur = stack.back(); stack.pop_back();
                if (cur == to) return true;
                if (seen[cur]) continue;
                seen[cur] = true;
                for (size_t nx : succ[cur]) stack.push_back(nx);
            }
            return false;
        }

        // True if a declares AmbiguousWith b, or b declares AmbiguousWith a.
        ASTRA_NODISCARD bool SuppressedAsAmbiguous(size_t aIdx, size_t bIdx) const
        {
            const auto& a = m_systems[aIdx].metadata;
            const auto& b = m_systems[bIdx].metadata;
            const uint64_t aHash = static_cast<uint64_t>(a.typeId);
            const uint64_t bHash = static_cast<uint64_t>(b.typeId);
            for (uint64_t h : a.ambiguousWithIds) if (h == bHash) return true;
            for (uint64_t h : b.ambiguousWithIds) if (h == aHash) return true;
            return false;
        }

        void ReportAmbiguities()
        {
            const size_t n = m_systems.size();
            const size_t UNKNOWN = n;
            auto resolve = [&](uint64_t hash) -> size_t
            {
                auto it = m_systemIndices.Find(hash);
                return it == m_systemIndices.end() ? UNKNOWN : it->second;
            };
            // Rebuild the successor adjacency (same convention as ComputeScheduleOrder).
            std::vector<std::vector<size_t>> succ(n);
            for (size_t s = 0; s < n; ++s)
            {
                const auto& md = m_systems[s].metadata;
                for (uint64_t h : md.afterIds)  { size_t t = resolve(h); if (t != UNKNOWN && t != s) succ[t].push_back(s); }
                for (uint64_t h : md.beforeIds) { size_t t = resolve(h); if (t != UNKNOWN && t != s) succ[s].push_back(t); }
            }
            for (size_t a = 0; a < n; ++a)
                for (size_t b = a + 1; b < n; ++b)
                {
                    size_t comp = 0, res = 0;
                    if (!Conflicts(m_systems[a].metadata, m_systems[b].metadata, comp, res)) continue;
                    if (OrderingReaches(a, b, succ) || OrderingReaches(b, a, succ)) continue;   // ordered
                    if (SuppressedAsAmbiguous(a, b)) continue;                                  // opted out
                    std::string msg = "SystemScheduler: ambiguous system order -- systems (insertionOrder) "
                        + std::to_string(m_systems[a].metadata.insertionOrder) + " and "
                        + std::to_string(m_systems[b].metadata.insertionOrder)
                        + " mutably conflict but declare no relative order.";
                    if (comp != MAX_COMPONENTS) msg += " component id " + std::to_string(comp) + '.';
                    if (res  != MAX_COMPONENTS) msg += " resource id "  + std::to_string(res)  + '.';
                    msg += " Add Before/After, or AmbiguousWith to silence.";
                    ASTRA_LOG_WARN(msg);
                }
        }
```

- [ ] **Step 5: Call the detector at the end of `BuildExecutionPlan` (`SystemScheduler.hpp`).** After the cycle-log block (Task 3, Step 4) and before/around `m_needsRebuild = false;`, add:
```cpp
            if (m_reportAmbiguities)
                ReportAmbiguities();
```
(Placing it after `m_needsRebuild = false;` is fine — it only reads metadata.)

- [ ] **Step 6: Run — verify GREEN.** Build all 3 configs. `--gtest_filter=SystemSchedulerOrdering.Ambiguity*` (4 tests pass). Add one more assertion of the **resource** path to prove Phase C's predicate feeds ambiguity (reuse a resource type already declared in the Phase C tests, e.g. `ResA`, with two `WritesResources<ResA>` systems and no order) — append it in Step 1's namespace if not already present and re-run. Full suite green (→ 640/638/638 for the 5 ambiguity tests + 1 resource-path).

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): opt-in ambiguity detection with AmbiguousWith suppression (Bevy-style)"
```

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §11 module 1 (traits) → Task 1 Steps 3-4; module 2 (extraction) → Task 1 Step 5; reorder (topo sort + scheduleOrder) → Task 1 Steps 6-7; edge-as-barrier (decision 5) → Task 2; cycle handling (decision 4) → Task 3; deferred apply order = execution order (decision 3) → Task 4; §12 ambiguity + `AmbiguousWith` (decision 7) + opt-in flag → Task 5. Boundaries (unknown-target-ignored → Task 3 Step 1 test; advisory/opt-in → Task 5; lambda-systems-excluded → design note, no code) all placed.
- **Placeholder scan:** every code step shows complete code; the Task 4 Step 5 determinism gate is the one deliberately-schematic step (it depends on the file's existing worker-pool/snapshot harness) and is bounded with a concrete fallback (assert a single observable equal across 20 runs) — not a "TODO".
- **Type consistency:** `BeforeTypes`/`AfterTypes`/`AmbiguousWithTypes` (Task 1 Step 3) → consumed by `ExtractSystemTraits` (Step 5); `beforeIds`/`afterIds`/`ambiguousWithIds` (Step 4) → read in `ComputeScheduleOrder` (Step 6), `IsOrderingPredecessor` (Task 2), `ReportAmbiguities` (Task 5); `scheduleOrder` (Step 4) → assigned in Step 7, consumed in `DispatchSystem` (Task 4); `m_scheduleHadCycle`/`m_cycleMembers` (Step 6) → surfaced in Task 3; `SystemError::OrderingCycle` (Task 3) → returned by `ValidateSchedule`. Helper names (`ExtractSystemIdList`, `IsOrderingPredecessor`, `Conflicts`, `OrderingReaches`, `SuppressedAsAmbiguous`, `ReportAmbiguities`, `ComputeScheduleOrder`) are each defined once and referenced consistently.
- **TypeID budget:** the only fresh component type introduced is `DTag` (Task 4); everything else reuses `Position`/`Velocity` (existing) and reuses `ResA` (Phase C) for the resource ambiguity case. Ordering edges are Hash-keyed and consume no ComponentIDs. Well within the 128 ceiling.
- **Additive:** no-edge schedules are the identity permutation (`scheduleOrder == insertionOrder`), so grouping and deferred sort keys are byte-identical to Phase C; ambiguity is opt-in/off by default; `Exclusive` and no-trait behavior unchanged.
