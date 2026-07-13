# Theme B — Concurrency B1 (Honest Scheduler) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Astra's built-in `SystemScheduler`/`ParallelExecutor` honest and memory-safe with no breaking API change, and shape every API choice so the future B2 (command-buffer-backed parallel structural changes) is purely additive.

**Architecture:** Component masks a system declares are a *promise of purity*; the scheduler trusts the promise and ships a Debug tripwire. Systems that mutate structure or reach out of band declare `Astra::Exclusive` (→ run solo). Fix the execution guard (depth counter, not a bool/mutex), make the plan insertion-order-stable, tighten the `IWorkScheduler` memory contract, and make `AddSystem` failable.

**Tech Stack:** Header-only C++20, MSVC (primary), GoogleTest, premake5-generated `Astra.sln`.

**Design spec:** `docs/superpowers/specs/2026-07-13-theme-b-concurrency-b1-design.md` (read it first).

## Global Constraints

- **No public API break.** Existing `AddSystem<T>()` / `SystemTraits<Reads…, Writes…>` / `void operator()(Registry&)` systems must still compile and behave.
- **All three configs green:** Debug, Release, Dist. The C1 tripwire and reentrancy assert are Debug-only; behavioral contracts hold in all configs.
- **Zero Release cost for the tripwire:** guard the counter snapshot *reads* (not just the `ASTRA_ASSERT`) under `ASTRA_BUILD_DEBUG`.
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` (also run `Release` and `Dist` at the gate). `-t:AstraTest` does NOT work — build the whole solution.
- **Test exe:** `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (GoogleTest; filter with `--gtest_filter=SystemScheduler*`).
- **New test file → regenerate premake:** run `D:\dev\_shared\tools\premake5 vs2022` from the repo root once after creating `tests/System/SystemSchedulerTest.cpp`. Appending to it later needs no regen. **Never `git add ide/`, `Astra.sln`, `Makefile`, or `*.make`** (all gitignored). Keep one gtest macro style per file (`TEST`, not `TEST_F`).
- **Baseline:** 551 tests on `dev`. Gate on "all configs green + intended new tests," not an absolute count.
- **Branch:** `theme-b-concurrency-b1` (already created, off `dev`). Do not push.
- **Deferred (do NOT implement here):** anything B2; the lambda view-cache; the `IsReadOnly`/optional-param minor; the custom assert seam; `BuildExecutionPlan` is rewritten (which resolves the O(n³) minor) but no further complexity work.

---

## File Structure

- `include/Astra/Archetype/ArchetypeManager.hpp` — add public `GetStructuralChangeCounter()` (Task 1).
- `include/Astra/System/System.hpp` — `Exclusive` tag; rewrite `SystemTraits` to a pack-scan surfacing `RequiresExclusive`; `LambdaSystemWrapper` gains `RequiresExclusive=false` (Task 2).
- `include/Astra/System/SystemMetadata.hpp` — add `bool requiresExclusive` to `SystemMetadata` (Task 2).
- `include/Astra/System/SystemScheduler.hpp` — `SystemError` enum; extract `requiresExclusive` at registration; rewrite `BuildExecutionPlan` (insertion-order-stable + Exclusive-solo, removes `HasConflict`/`scheduled`); depth-counter `ExecutionGuard` + reentrancy assert; `AddSystem`→`Result`; hash-collision comment; `insertionOrder` resync (Tasks 3–5).
- `include/Astra/Core/WorkScheduler.hpp` — forward happens-before comment (Task 4).
- `include/Astra/System/SystemExecutor.hpp` — Debug structural-change tripwire in `ParallelExecutor` (Task 6).
- `tests/System/SystemSchedulerTest.cpp` — **new**, the whole scheduler suite (Tasks 1–6).
- `README.md` — threading/ordering contract (Task 7).

---

## Task 1: `ArchetypeManager::GetStructuralChangeCounter()` accessor

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (add public method near other public accessors)
- Test: `tests/System/SystemSchedulerTest.cpp` (**create**)

**Interfaces:**
- Produces: `uint32_t ArchetypeManager::GetStructuralChangeCounter() const noexcept` — monotonic count of structural changes (archetype create, entity add/remove-driven transitions). Reached from the scheduler via the already-public `Registry::GetArchetypeManager()` (`Registry.hpp:1026`).

- [ ] **Step 1: Create the test file with the failing test**

Create `tests/System/SystemSchedulerTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
}

// ---- Task 1: structural-change counter accessor -----------------------------

TEST(SystemScheduler, StructuralChangeCounterIncrementsOnCreate)
{
    Astra::Registry reg;
    auto* am = reg.GetArchetypeManager();
    const uint32_t before = am->GetStructuralChangeCounter();
    (void)reg.CreateEntity<Position>();  // creates the {Position} archetype
    EXPECT_GT(am->GetStructuralChangeCounter(), before);
}
```

- [ ] **Step 2: Regenerate premake and confirm the test fails to build**

Run (repo root): `D:\dev\_shared\tools\premake5 vs2022`
Then build Debug (see Global Constraints).
Expected: compile error — `GetStructuralChangeCounter` is not a member of `ArchetypeManager`.

- [ ] **Step 3: Add the accessor**

In `include/Astra/Archetype/ArchetypeManager.hpp`, in the `public:` section (near other `Get*` accessors), add:

```cpp
// Monotonic count of structural changes (archetype creation, entity
// add/remove transitions). Used by the built-in ParallelExecutor's Debug
// tripwire and by View cache invalidation. Read-only; single-writer.
ASTRA_NODISCARD uint32_t GetStructuralChangeCounter() const noexcept
{
    return m_structuralChangeCounter.load(std::memory_order_acquire);
}
```

- [ ] **Step 4: Build Debug and run the test**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.StructuralChangeCounterIncrementsOnCreate`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(archetype): public GetStructuralChangeCounter() for scheduler tripwire"
```

---

## Task 2: `Exclusive` tag + `SystemTraits` pack-scan + `SystemMetadata.requiresExclusive`

**Files:**
- Modify: `include/Astra/System/System.hpp:19-60` (`Reads`/`Writes`/`SystemTraits`), `:68-138` (`LambdaSystemWrapper`)
- Modify: `include/Astra/System/SystemMetadata.hpp:19-32` (`SystemMetadata`)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces: `struct Astra::Exclusive {}` (tag). `SystemTraits<Traits...>` for any mix/order of `Reads<…>`, `Writes<…>`, `Exclusive`, surfacing `ReadsComponents`, `WritesComponents`, `static constexpr bool HasTraits = true`, `static constexpr bool RequiresExclusive`. `SystemMetadata::requiresExclusive` (`bool`, default `false`).

- [ ] **Step 1: Write the failing compile-time test (append to the test file)**

```cpp
// ---- Task 2: SystemTraits pack-scan + Exclusive tag -------------------------

namespace
{
    using RW   = Astra::SystemTraits<Astra::Reads<Velocity>, Astra::Writes<Position>>;
    using WOnly = Astra::SystemTraits<Astra::Writes<Position>>;
    using WEx   = Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>;
    using ExOnly= Astra::SystemTraits<Astra::Exclusive>;

    static_assert(RW::HasTraits && !RW::RequiresExclusive);
    static_assert(std::tuple_size_v<RW::ReadsComponents>  == 1);
    static_assert(std::tuple_size_v<RW::WritesComponents> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::ReadsComponents>,  Velocity>);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::WritesComponents>, Position>);

    static_assert(!WOnly::RequiresExclusive);
    static_assert(std::tuple_size_v<WOnly::ReadsComponents> == 0);

    static_assert(WEx::RequiresExclusive);
    static_assert(std::tuple_size_v<WEx::WritesComponents> == 1);
    static_assert(std::tuple_size_v<WEx::ReadsComponents>  == 0);

    static_assert(ExOnly::RequiresExclusive && ExOnly::HasTraits);
    static_assert(std::tuple_size_v<ExOnly::ReadsComponents>  == 0);
    static_assert(std::tuple_size_v<ExOnly::WritesComponents> == 0);
}

TEST(SystemScheduler, SystemTraitsPackScanCompiles) { SUCCEED(); }
```

- [ ] **Step 2: Build Debug and confirm it fails**

Expected: compile errors — `Exclusive` is undefined; `RequiresExclusive` is not a member; `SystemTraits<Writes<Position>, Exclusive>` doesn't match any specialization.

- [ ] **Step 3: Rewrite `SystemTraits` as a pack-scan in `System.hpp`**

Replace the three `SystemTraits` specializations (`System.hpp:25-50`) with a tag + per-trait extractor + single primary template:

```cpp
template<typename... Components>
struct Reads { using type = std::tuple<Components...>; };

template<typename... Components>
struct Writes { using type = std::tuple<Components...>; };

// Marker: a system that mutates entity structure (create/destroy/add/remove)
// or accesses state outside its declared masks. Forces a solo execution group.
struct Exclusive {};

namespace Detail
{
    template<typename T> struct TraitReads  { using type = std::tuple<>; };
    template<typename... R> struct TraitReads<Reads<R...>>  { using type = std::tuple<R...>; };
    template<typename T> struct TraitWrites { using type = std::tuple<>; };
    template<typename... W> struct TraitWrites<Writes<W...>> { using type = std::tuple<W...>; };
}

// Accepts Reads<...>, Writes<...>, and Exclusive in any order/combination.
template<typename... Traits>
struct SystemTraits
{
    using ReadsComponents  = decltype(std::tuple_cat(std::declval<typename Detail::TraitReads<Traits>::type>()...));
    using WritesComponents = decltype(std::tuple_cat(std::declval<typename Detail::TraitWrites<Traits>::type>()...));
    static constexpr bool HasTraits = true;
    static constexpr bool RequiresExclusive = (std::is_same_v<Traits, Exclusive> || ...);
};
```

(`<tuple>`, `<type_traits>`, `<utility>` are already included in `System.hpp`. `std::declval`/`std::tuple_cat` need `<utility>`/`<tuple>` — add `#include <utility>` if the build complains.)

- [ ] **Step 4: Give `LambdaSystemWrapper` a uniform `RequiresExclusive`**

In `System.hpp`, in `LambdaSystemWrapper`'s `public:` block (next to `static constexpr bool HasTraits = true;`, ~`:109`), add:

```cpp
static constexpr bool RequiresExclusive = false;  // lambda systems operate on a view; never exclusive
```

- [ ] **Step 5: Add the metadata field**

In `include/Astra/System/SystemMetadata.hpp`, add to `SystemMetadata` (after `insertionOrder`):

```cpp
    // True if the system declared Astra::Exclusive (runs in its own solo group).
    bool requiresExclusive = false;
```

- [ ] **Step 6: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.SystemTraitsPackScanCompiles`
Expected: PASS (the static_asserts compiled).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/System/System.hpp include/Astra/System/SystemMetadata.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): Exclusive tag + SystemTraits pack-scan, RequiresExclusive/requiresExclusive"
```

---

## Task 3: `BuildExecutionPlan` — insertion-order-stable + Exclusive-solo

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` — set `metadata.requiresExclusive` in both `AddSystem` (`:73-101`) and `AddSystemInternal` (`:406-429`); rewrite `BuildExecutionPlan` (`:247-344`); delete `HasConflict` (`:346-365`, now unused).
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: `SystemMetadata::requiresExclusive` (Task 2), `SystemTraits::RequiresExclusive` (Task 2).
- Produces: `SystemScheduler::GetExecutionPlan()` returns groups that are contiguous insertion-order runs; an exclusive/no-trait system is always alone in its group.

- [ ] **Step 1: Write failing tests (append)**

```cpp
// ---- Task 3: plan construction ---------------------------------------------

namespace
{
    // A=Position, B=Velocity, C=Health. Distinct types => distinct registrations.
    struct WA  : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct WA2 : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct WB  : Astra::SystemTraits<Astra::Writes<Velocity>> { void operator()(Astra::Registry&) {} };
    struct WC  : Astra::SystemTraits<Astra::Writes<Health>>   { void operator()(Astra::Registry&) {} };
    struct RA  : Astra::SystemTraits<Astra::Reads<Position>>  { void operator()(Astra::Registry&) {} };
    struct ExA : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive> { void operator()(Astra::Registry&) {} };
    struct NoTraits { void operator()(Astra::Registry&) {} };
}

TEST(SystemScheduler, NonConflictingSystemsShareAGroup)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();  // A
    s.AddSystem<WB>();  // B (disjoint)
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

TEST(SystemScheduler, ConflictingSystemsSplitIntoSeparateGroups)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();
    s.AddSystem<WA2>();  // both write A => conflict
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0][0], 0u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, PlanIsInsertionOrderStableNoLeapfrog)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();   // 0: writes A
    s.AddSystem<WA2>();  // 1: writes A (conflicts with 0)
    s.AddSystem<WB>();   // 2: writes B (independent)
    // Stable plan: [[0],[1,2]] — 2 never leapfrogs ahead of 1 into group 0.
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);
    ASSERT_EQ(plan[0].size(), 1u);
    EXPECT_EQ(plan[0][0], 0u);
    ASSERT_EQ(plan[1].size(), 2u);
    EXPECT_EQ(plan[1][0], 1u);
    EXPECT_EQ(plan[1][1], 2u);
}

TEST(SystemScheduler, ExclusiveSystemGetsSoloGroup)
{
    Astra::SystemScheduler s;
    s.AddSystem<WB>();   // 0: writes B
    s.AddSystem<ExA>();  // 1: exclusive (even though A is disjoint from B)
    s.AddSystem<WC>();   // 2: writes C
    const auto& plan = s.GetExecutionPlan();
    // 1 must be alone; nothing shares its group.
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);
    EXPECT_EQ(plan[1][0], 1u);
}

TEST(SystemScheduler, NoTraitSystemForcesSerialization)
{
    Astra::SystemScheduler s;
    s.AddSystem<WA>();       // 0
    s.AddSystem<NoTraits>(); // 1: no hints => solo
    s.AddSystem<WB>();       // 2
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[1].size(), 1u);  // the no-trait system is alone
}
```

- [ ] **Step 2: Build Debug and confirm failures**

Expected: the exclusive/stable tests FAIL (current greedy yields `[[0,2],[1]]` for the leapfrog case; `ExA` is grouped with disjoint systems because exclusivity isn't threaded yet).

- [ ] **Step 3: Set `requiresExclusive` at registration**

In `SystemScheduler.hpp` `AddSystem` (after the `if constexpr (HasSystemTraits_v<T>) { ExtractSystemTraits<T>(metadata); }` block, ~`:82-91`) add:

```cpp
            if constexpr (requires { T::RequiresExclusive; })
                metadata.requiresExclusive = T::RequiresExclusive;
```

Add the identical block in `AddSystemInternal` after its `ExtractSystemTraits` block (~`:415-418`), using `SystemType` instead of `T`:

```cpp
            if constexpr (requires { SystemType::RequiresExclusive; })
                metadata.requiresExclusive = SystemType::RequiresExclusive;
```

Also set the field in both metadata initializers so it is never read uninitialized — add `.requiresExclusive = false` as the last designated initializer in the `SystemMetadata metadata{ … }` braces in **both** `AddSystem` (`:73-79`) and `AddSystemInternal` (`:406-412`).

- [ ] **Step 4: Rewrite `BuildExecutionPlan` and delete `HasConflict`**

Replace `BuildExecutionPlan` (`:247-344`) entirely with:

```cpp
        // Partition systems into sequential groups of concurrently-runnable
        // systems. The plan is a set of CONTIGUOUS insertion-order runs: a run
        // grows from its opener until the first system that conflicts (mask
        // overlap), is Exclusive, or declares no traits. This keeps Sequential
        // and Parallel executors in identical observable order and never lets a
        // later system's effects appear before an earlier system's (I3). O(n).
        void BuildExecutionPlan()
        {
            m_executionPlan.clear();
            if (m_systems.empty())
            {
                m_needsRebuild = false;
                return;
            }

            size_t i = 0;
            while (i < m_systems.size())
            {
                const auto& sysI = m_systems[i].metadata;

                std::vector<size_t> group;
                group.push_back(i);
                ComponentMask groupReads = sysI.reads;
                ComponentMask groupWrites = sysI.writes;

                // A solo opener (Exclusive, or no declared hints) accepts nobody.
                const bool acceptsMore = !sysI.requiresExclusive
                                      && !(sysI.reads.None() && sysI.writes.None());

                size_t j = i + 1;
                for (; acceptsMore && j < m_systems.size(); ++j)
                {
                    const auto& sysJ = m_systems[j].metadata;

                    // Exclusive / no-trait systems never join an existing group,
                    // and any conflict ends the contiguous run (order preserved).
                    if (sysJ.requiresExclusive || (sysJ.reads.None() && sysJ.writes.None()))
                        break;
                    if ((sysJ.writes & groupWrites).Any() ||
                        (sysJ.writes & groupReads ).Any() ||
                        (sysJ.reads  & groupWrites).Any())
                        break;

                    group.push_back(j);
                    groupReads  |= sysJ.reads;
                    groupWrites |= sysJ.writes;
                }

                m_executionPlan.push_back(std::move(group));
                i = j;  // next group starts right after this contiguous run
            }

            m_needsRebuild = false;
        }
```

Delete the now-unused `HasConflict` method (`:346-365`). (This also resolves the O(n³) minor — the new builder is O(n).)

- [ ] **Step 5: Build Debug and run the Task 3 tests**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.*Group*:SystemScheduler.*Plan*:SystemScheduler.*Serial*:SystemScheduler.*Exclusive*`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): insertion-order-stable plan + Exclusive-solo grouping (I3, C1 scheduling)"
```

---

## Task 4: `ExecutionGuard` depth counter (I1/I2) + `IWorkScheduler` forward-edge contract (I4)

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp:27-48` (`ExecutionGuard`/`IsExecuting`), `:153-184` (`Execute`), `:438` (member).
- Modify: `include/Astra/Core/WorkScheduler.hpp:25-32` (contract comment).
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces: `IsExecuting()` truthful under nesting; `Execute` reentrancy-safe; a `std::atomic<int> m_executionDepth` whose return-to-zero is B2's designated sync point.

- [ ] **Step 1: Write failing tests (append)**

```cpp
// ---- Task 4: execution guard -----------------------------------------------

namespace
{
    // A system that mutates the scheduler mid-Execute (the practical misuse).
    struct SelfRemovingSystem
    {
        Astra::SystemScheduler* sched = nullptr;
        bool* sawExecuting = nullptr;
        void operator()(Astra::Registry&)
        {
            *sawExecuting = sched->IsExecuting();     // must be true inside Execute
            sched->RemoveSystem<SelfRemovingSystem>(); // must no-op (guarded)
        }
    };
}

TEST(SystemScheduler, IsExecutingTrueInsideExecuteAndMutationNoOps)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    bool sawExecuting = false;
    (void)s.AddSystem<SelfRemovingSystem>(&s, &sawExecuting);
    EXPECT_FALSE(s.IsExecuting());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);

    EXPECT_TRUE(sawExecuting);          // flag was set during Execute
    EXPECT_FALSE(s.IsExecuting());      // cleared after Execute
    EXPECT_EQ(s.Size(), 1u);           // RemoveSystem no-oped during execution
}
```

(`(void)`-cast on `AddSystem` because Task 5 makes it `[[nodiscard]]`; casting now avoids revisiting this line. `RemoveSystem` stays `void`.)

- [ ] **Step 2: Build Debug and confirm behavior**

This is a **characterization lock** around the guard refactor, not a RED→GREEN case: with the current bool flag it already passes (`IsExecuting()` is true during `Execute`, `RemoveSystem` no-ops). Run it and confirm PASS *before* Step 3, then confirm it still passes *after* the depth-counter swap — that is the point (the refactor must not regress the one behavior that matters). The reentrancy assert added in Step 3 is Debug-only and verified by inspection (no death test, per project convention). The true RED→GREEN for the registration error path lives in Task 5.

- [ ] **Step 3: Convert the guard to a depth counter**

In `SystemScheduler.hpp`, replace the `ExecutionGuard` class (`:27-42`) with:

```cpp
        // RAII depth counter for execution. NOT a lock: it does not provide
        // mutual exclusion against external threads. Registration (Add/Remove/
        // Clear) follows the same single-writer contract as the Registry — it
        // must not race Execute. The counter exists to (a) make the one
        // practical mistake safe (a system, on a worker, calling Remove/Add
        // mid-frame no-ops) and (b) stay truthful under reentrant Execute.
        // Depth returning to zero is the designated B2 command-buffer sync point.
        class ExecutionGuard
        {
        public:
            explicit ExecutionGuard(std::atomic<int>& depth) : m_depth(depth)
            {
                m_depth.fetch_add(1, std::memory_order_acq_rel);
            }
            ~ExecutionGuard()
            {
                m_depth.fetch_sub(1, std::memory_order_acq_rel);
            }
            ExecutionGuard(const ExecutionGuard&) = delete;
            ExecutionGuard& operator=(const ExecutionGuard&) = delete;
        private:
            std::atomic<int>& m_depth;
        };
```

Change `IsExecuting` (`:45-48`) to:

```cpp
        ASTRA_NODISCARD bool IsExecuting() const noexcept
        {
            return m_executionDepth.load(std::memory_order_acquire) > 0;
        }
```

Change the member (`:438`) from `mutable std::atomic<bool> m_isExecuting{false};` to:

```cpp
        mutable std::atomic<int> m_executionDepth{0};  // reentrancy-safe; ==0 is the B2 sync point
```

In `Execute(Registry&, ISystemExecutor*)` (`:153-162`), before constructing the guard, add a reentrancy tripwire, then construct with the new member:

```cpp
            ASTRA_ASSERT(m_executionDepth.load(std::memory_order_acquire) == 0,
                "Reentrant SystemScheduler::Execute is unsupported; if a system must "
                "re-run systems, do it from an Astra::Exclusive system. (The depth "
                "counter keeps this safe, but nesting is almost always a design error.)");
            ExecutionGuard guard(m_executionDepth);
```

**Also (uniform-graceful misuse policy, decision 2026-07-13):** remove the `ASTRA_ASSERT(!IsExecuting(), …)` line from **`RemoveSystem`** (`:117`) and **`Clear`** (`:189`), keeping each method's existing graceful `if (IsExecuting()) return;` guard. These two `void` methods have no error channel; a system calling them mid-`Execute` is the "one practical mistake" the guard makes safe (a no-op), so aborting in Debug contradicts that and would crash the Task-4 test. (`AddSystem`'s two misuse-asserts are removed as part of Task 5, which rewrites `AddSystem` wholesale.) The reentrancy assert added just above is the **only** execution-guard assert that remains.

- [ ] **Step 4: Fix the `IWorkScheduler` contract comment (I4)**

In `include/Astra/Core/WorkScheduler.hpp`, extend the memory-model clause (`:25-28`) to require both edges:

```cpp
        // Memory model: implementations MUST establish a happens-before edge in
        // BOTH directions: (1) from the ParallelFor call site to the start of
        // every fn invocation (writes made before ParallelFor are visible inside
        // fn), and (2) from the completion of every fn invocation to the return
        // of ParallelFor (writes made inside fn are visible to the caller after).
        // Every conforming scheduler (std::execution::par, TBB parallel_for, a
        // task pool) already provides both; Astra's correctness depends on it.
```

- [ ] **Step 5: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.IsExecuting*`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/System/SystemScheduler.hpp include/Astra/Core/WorkScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "fix(system): depth-counter ExecutionGuard (I1/I2) + both-edge IWorkScheduler contract (I4)"
```

---

## Task 5: Failable `AddSystem` → `Result<void, SystemError>` (I5) + housekeeping

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` — `SystemError` enum (near top); `AddSystem<T>` (`:50-104`), lambda `AddSystem` (`:106-111`), `AddLambdaSystemImpl` (`:368-381`), `AddSystemInternal` (`:383-432`) return `Result`; nothrow `new`; hash-collision comment; `insertionOrder` resync in `RemoveSystem` (`:126-136`).
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces: `enum class SystemError { AlreadyRegistered, AllocationFailed, SchedulerExecuting };` and `[[nodiscard]] Result<void, SystemError> AddSystem(...)` on all `AddSystem` overloads.

- [ ] **Step 1: Write failing tests (append)**

```cpp
// ---- Task 5: failable registration -----------------------------------------

TEST(SystemScheduler, AddSystemReportsDuplicateAndSuccess)
{
    Astra::SystemScheduler s;
    auto first = s.AddSystem<WA>();
    EXPECT_TRUE(first.IsOk());
    auto dup = s.AddSystem<WA>();
    ASSERT_TRUE(dup.IsErr());
    EXPECT_EQ(*dup.GetError(), Astra::SystemError::AlreadyRegistered);
    EXPECT_EQ(s.Size(), 1u);
}

TEST(SystemScheduler, AddSystemDuringExecuteReturnsExecutingError)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    struct Probe {
        Astra::SystemScheduler* sched = nullptr;
        Astra::SystemError* out = nullptr;
        void operator()(Astra::Registry&)
        {
            auto r = sched->AddSystem<WB>();
            if (r.IsErr()) *out = *r.GetError();
        }
    };
    Astra::SystemError captured = Astra::SystemError::AlreadyRegistered;  // sentinel
    s.AddSystem<Probe>(&s, &captured);
    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(captured, Astra::SystemError::SchedulerExecuting);
    EXPECT_EQ(s.Size(), 1u);
}

TEST(SystemScheduler, RemoveSystemKeepsSurvivorsValid)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    std::atomic<int> ran{0};
    struct Counter0 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(1); } };
    struct Counter1 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(10); } };
    struct Counter2 { std::atomic<int>* c; void operator()(Astra::Registry&){ c->fetch_add(100); } };
    s.AddSystem<Counter0>(&ran);
    s.AddSystem<Counter1>(&ran);
    s.AddSystem<Counter2>(&ran);
    s.RemoveSystem<Counter1>();          // remove the middle one
    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(ran.load(), 101);          // 0 and 2 ran, 1 did not; delegates valid
}
```

- [ ] **Step 2: Build Debug and confirm failures**

Expected: compile error — `AddSystem` returns `void` (can't call `.IsOk()`); `SystemError` undefined.

- [ ] **Step 3: Add the `SystemError` enum**

In `SystemScheduler.hpp`, inside `namespace Astra` above `class SystemScheduler` (~`:21`), add:

```cpp
    enum class SystemError
    {
        AlreadyRegistered,  // a system of this type is already registered
        AllocationFailed,   // nothrow allocation of the system instance failed
        SchedulerExecuting  // registration attempted while Execute() is running
    };
```

Ensure `#include "../Core/Result.hpp"` is present in the includes (add it if missing).

- [ ] **Step 4: Make `AddSystem<T>` failable**

Replace the signature/body of `AddSystem<System T, Args...>` (`:50-104`). Key changes: return type, nothrow `new`, `Result` returns, hash-collision comment:

```cpp
        template<System T, typename... Args>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Args&&... args)
        {
            // Uniform-graceful misuse policy (decision 2026-07-13): NO
            // ASTRA_ASSERT here — the Result channel below IS the contract, so
            // asserting-and-aborting on the same condition would make the error
            // unreachable/untestable in Debug. Return the typed error instead.
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            // Systems are keyed by TypeID::Hash() (64-bit). A hash collision
            // would make a DISTINCT type look already-registered and be dropped;
            // astronomically unlikely, but it is a hash, not a dense unique id.
            const uint64_t typeId = TypeID<T>::Hash();
            if (m_systemIndices.Contains(typeId))
            {
                // No ASTRA_ASSERT — duplicate registration is a handleable
                // runtime error (uniform-graceful policy, decision 2026-07-13).
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);
            }

            T* instance = new (std::nothrow) T(std::forward<Args>(args)...);
            if (!instance)
                return Result<void, SystemError>::Err(SystemError::AllocationFailed);

            const size_t index = m_systems.size();
            m_systemIndices[typeId] = index;

            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = static_cast<size_t>(typeId),
                .insertionOrder = index,
                .requiresExclusive = false
            };
            if constexpr (HasSystemTraits_v<T>)
                ExtractSystemTraits<T>(metadata);
            if constexpr (requires { T::RequiresExclusive; })
                metadata.requiresExclusive = T::RequiresExclusive;

            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<T*>(ptr); }),
                .execute = [instance](Registry& reg) { (*instance)(reg); },
                .metadata = metadata
            });

            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }
```

- [ ] **Step 5: Make the lambda overloads failable**

Change the lambda `AddSystem` (`:106-111`) to return and forward the result:

```cpp
        template<typename Lambda>
        requires LambdaLike<Lambda>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Lambda&& lambda)
        {
            return AddLambdaSystemImpl(std::forward<Lambda>(lambda), &std::decay_t<Lambda>::operator());
        }
```

Change both `AddLambdaSystemImpl` overloads (`:368-381`) to `ASTRA_NODISCARD Result<void, SystemError>` and `return AddSystemInternal<Wrapper>(...);`. Change `AddSystemInternal` (`:383-432`) to `ASTRA_NODISCARD Result<void, SystemError>`, mirroring Step 4's body (nothrow `new`, the three `Err`/`Ok` returns, `.requiresExclusive = false` initializer, the `if constexpr (requires { SystemType::RequiresExclusive; })` block, and — per the uniform-graceful policy — **NO misuse-asserts**: strip the existing `ASTRA_ASSERT(!IsExecuting(), …)` and `ASTRA_ASSERT(false, "System type already registered")` here too).

- [ ] **Step 6: Resync `insertionOrder` in `RemoveSystem`**

In `RemoveSystem` (`:126-138`), after the index-fixup loop, keep `metadata.insertionOrder` consistent with the new vector positions (it is public `SystemExecutionContext.metadata` surface):

```cpp
            // Keep insertionOrder consistent with vector position after erase
            // (it is exposed via SystemExecutionContext.metadata).
            for (size_t idx = 0; idx < m_systems.size(); ++idx)
                m_systems[idx].metadata.insertionOrder = idx;
```

- [ ] **Step 7: `(void)`-cast every discarding `AddSystem` call site**

`AddSystem` is now `[[nodiscard]]`. Any call that discards the result warns (and errors if the build uses warnings-as-errors). Grep the whole tree for **both** dot and arrow calls:

Grep pattern: `AddSystem[<(]` across `tests/` and `benchmark/`.

Cast every site that discards the result to `(void)`. Known sites:
- `tests/Registry/ParallelIterationTest.cpp:111-112`:
  ```cpp
        (void)scheduler.AddSystem<CountingSystem<0>>(ran);
        (void)scheduler.AddSystem<CountingSystem<1>>(ran);
  ```
- Every `s.AddSystem<…>();` in the **Task 3 tests** you already committed in this file (they check the plan, not the return value) — cast each: `(void)s.AddSystem<WA>();` etc.
- Do **not** cast the sites that inspect the result (Task 5's `AddSystemReportsDuplicateAndSuccess`, and the `Probe` struct's `auto r = sched->AddSystem<WB>();` in `AddSystemDuringExecuteReturnsExecutingError`) — they use it.
- Check `benchmark/Benchmark.cpp` — if it calls `AddSystem`, cast there too.

- [ ] **Step 8: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.AddSystem*:SystemScheduler.RemoveSystem*`
Expected: PASS. Also run the full suite once to catch any missed `[[nodiscard]]` call site:
Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe`
Expected: all green.

- [ ] **Step 9: Commit**

```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp tests/Registry/ParallelIterationTest.cpp
git commit -m "feat(system): failable AddSystem -> Result<void, SystemError> (I5) + insertionOrder resync"
```

---

## Task 6: Debug structural-change tripwire in `ParallelExecutor` (C1 enforcement)

**Files:**
- Modify: `include/Astra/System/SystemExecutor.hpp` (top include + `ParallelExecutor::Execute` `:39-60`)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: `Registry::GetArchetypeManager()`, `ArchetypeManager::GetStructuralChangeCounter()` (Task 1); `SystemMetadata::requiresExclusive` grouping (Task 3).
- Produces: Debug-only assert if a multi-member parallel group performs a structural change.

- [ ] **Step 1: Write the safety test (append)**

```cpp
// ---- Task 6: C1 safety — Exclusive spawner is solo and safe under threads ---

namespace
{
    struct SpawnSystem : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>
    {
        void operator()(Astra::Registry& r)
        {
            for (int k = 0; k < 10; ++k) (void)r.CreateEntity<Position>();  // structural
        }
    };
    struct TouchVelocity : Astra::SystemTraits<Astra::Writes<Velocity>>
    {
        void operator()(Astra::Registry& r)
        {
            auto v = r.CreateView<Velocity>();
            v.ForEach([](Astra::Entity, Velocity& vel) { vel.dx += 1.0f; });
        }
    };
    struct TouchHealth : Astra::SystemTraits<Astra::Writes<Health>>
    {
        void operator()(Astra::Registry& r)
        {
            auto v = r.CreateView<Health>();
            v.ForEach([](Astra::Entity, Health& h) { h.current += 1; });
        }
    };

    size_t CountPositions(Astra::Registry& r)
    {
        auto v = r.CreateView<Position>();
        size_t n = 0;
        v.ForEach([&](Astra::Entity, Position&) { ++n; });
        return n;
    }
}

TEST(SystemScheduler, ExclusiveSpawnerRunsSoloWhilePureGroupRunsOnThreads)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    (void)s.AddSystem<SpawnSystem>();     // 0: exclusive => solo group
    (void)s.AddSystem<TouchVelocity>();   // 1: pure (writes B)
    (void)s.AddSystem<TouchHealth>();     // 2: pure (writes C, disjoint from B)
    // Plan: [[0]], [[1,2]] — 1 and 2 form a real multi-member group dispatched
    // concurrently to the pool; 0 (structural) is solo, so it never races them.
    ASSERT_EQ(s.GetExecutionPlan().size(), 2u);
    ASSERT_EQ(s.GetExecutionPlan()[1].size(), 2u);

    Astra::ParallelExecutor exec(std::make_shared<Astra::Testing::TestWorkerPool>());

    constexpr int kFrames = 50;
    for (int f = 0; f < kFrames; ++f)
        s.Execute(reg, &exec);

    // 10 new Position entities per frame, no corruption/loss. The Debug tripwire
    // sees no structural change across the pure [1,2] group, so it never fires.
    EXPECT_EQ(CountPositions(reg), static_cast<size_t>(10 * kFrames));
}
```

- [ ] **Step 2: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemScheduler.ExclusiveSpawnerRunsSoloWhilePureGroupRunsOnThreads`
Expected: PASS already (Task 3 made `SpawnSystem` solo; the pure `[1,2]` group runs on the pool). This test *locks* that safety; Step 3 adds the tripwire that catches the *un*-declared structural-change case in Debug.

- [ ] **Step 3: Add the tripwire to `ParallelExecutor`**

At the top of `include/Astra/System/SystemExecutor.hpp` (after the existing includes), add a Debug-only include so the executor can read the counter through the (forward-declared) `Registry*`:

```cpp
#ifdef ASTRA_BUILD_DEBUG
    #include "../Registry/Registry.hpp"          // GetArchetypeManager() (Debug tripwire only)
#endif
```

Replace the `else` branch of `ParallelExecutor::Execute` (`:49-58`, the multi-member dispatch) with the snapshot-guarded version:

```cpp
                else
                {
#ifdef ASTRA_BUILD_DEBUG
                    const uint32_t structuralBefore =
                        context.registry->GetArchetypeManager()->GetStructuralChangeCounter();
#endif
                    // Dispatch each system in the group as its own unit of work.
                    m_scheduler->ParallelFor(group.size(), 1, [&](size_t begin, size_t end)
                    {
                        for (size_t i = begin; i < end; ++i)
                            context.systems[group[i]](*context.registry);
                    });
#ifdef ASTRA_BUILD_DEBUG
                    const uint32_t structuralAfter =
                        context.registry->GetArchetypeManager()->GetStructuralChangeCounter();
                    ASTRA_ASSERT(structuralBefore == structuralAfter,
                        "A system in a multi-member parallel group performed a structural "
                        "change (create/destroy entity, add/remove component) without "
                        "declaring Astra::Exclusive. Mark it Exclusive, or defer the change "
                        "via a CommandBuffer. (Structural mutation races the archetype "
                        "storage against the other systems in the group.)");
#endif
                }
```

(`ASTRA_ASSERT` and `ASTRA_BUILD_DEBUG` come from `Core/Base.hpp`, already transitively included via `WorkScheduler.hpp`. Add `#include "../Core/Base.hpp"` explicitly if the build complains.)

- [ ] **Step 4: Build all three configs and run the full suite**

Build Debug, Release, Dist (see Global Constraints). For each:
Run: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`
Expected: all green in every config. The safety test passes; the tripwire is inert in Release/Dist (no counter reads compiled). Verify the tripwire logic by inspection (per project convention — no death tests).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/System/SystemExecutor.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): Debug structural-change tripwire in ParallelExecutor (C1 enforcement)"
```

---

## Task 7: Document the threading & ordering contract

**Files:**
- Modify: `README.md` (the "Threading Model" section, ~`:306-327`)
- Test: build only (docs)

- [ ] **Step 1: Update the Threading Model section**

In `README.md`, after the existing "Threading Model" prose (`:308-316`), add a subsection documenting the B1 contract verbatim in spirit:

````markdown
#### System scheduling contract (built-in `SystemScheduler`)

The built-in scheduler is an **opt-in convenience**, not a guarantee. When you
inject an `IWorkScheduler` and use `ParallelExecutor`, it groups systems that
declare **disjoint** component masks and runs them concurrently. The rules:

- **Declared masks are a promise of purity.** A system in a multi-member
  parallel group must touch only the components in its `Reads`/`Writes` and must
  perform **no** structural changes (create/destroy entity, add/remove
  component). In Debug, an undeclared structural change trips an assert.
- **`Astra::Exclusive`** — mark a system that does structural changes or reaches
  outside its declared masks. It runs in its own solo group (nothing concurrent):

  ```cpp
  struct SpawnSystem : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>
  {
      void operator()(Astra::Registry& r) { r.CreateEntity<Position>(); /* safe: solo */ }
  };
  ```

- **Ordering.** Only component-mask dependencies are honored. Within a parallel
  group, order is concurrent and unspecified. Independent systems are **not**
  reordered across insertion order (the plan is insertion-order-stable, and
  `SequentialExecutor` and `ParallelExecutor` produce identical observable
  order). A hidden dependency between two mask-independent systems in the same
  group (through a resource, event, or side effect) is a *system-order
  ambiguity* and is not honored — express it via masks.
- **Registration** (`AddSystem`) returns `Result<void, SystemError>` and must
  not race `Execute` (single-writer, like the `Registry` itself).
````

- [ ] **Step 2: Build once (sanity) and commit**

```bash
git add README.md
git commit -m "docs: document the B1 system-scheduling & ordering contract"
```

---

## Final gate

- [ ] Build **Debug**, **Release**, **Dist** (x64) — whole solution — all succeed.
- [ ] `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe` green in all three configs (a lone `CompressionTest.PerformanceBenchmark` failure is a known flake — rerun it isolated).
- [ ] New `SystemScheduler.*` tests all present and passing; test count ≥ 551 + new tests.
- [ ] No public API break: existing systems / `SystemTraits` / the benchmark still compile.
- [ ] Only tracked source committed (`include/`, `tests/`, `README.md`, `docs/`); `ide/`, `Astra.sln`, `Makefile`, `*.make` untouched in git.

---

## Self-review notes (spec coverage)

- C1 → Tasks 2 (marker), 3 (solo grouping), 6 (tripwire). I1/I2 → Task 4. I3 → Task 3. I4 → Task 4. I5 (registration half) → Task 5. Minors: hash-collision + insertionOrder → Task 5; O(n³) resolved by the Task 3 rewrite. Accessor prerequisite → Task 1. Contract docs → Task 7. Deferred items (lambda view-cache, IsReadOnly/optional-param, assert seam, B2) intentionally absent.
