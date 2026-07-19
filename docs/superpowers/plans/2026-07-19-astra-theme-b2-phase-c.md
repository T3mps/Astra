# Theme B2 — Phase C: Resource Access Declaration + Conflict Analysis — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fold resource access into the scheduler's conflict analysis so two systems that share a resource with a write on either side (and, for a non-thread-safe resource, even two readers) are never placed in the same parallel group.

**Architecture:** Mirror the existing component-mask mechanism for resources. New `ReadsResources`/`WritesResources` traits compose into `SystemTraits`; `ExtractSystemTraits` fills new `resourceReads`/`resourceWrites` `ComponentMask` fields in `SystemMetadata` (resources are keyed by their `ComponentID`, same as `ResourceStorage` keys them); `BuildExecutionPlan` adds a resource dimension to its conflict test and treats a resource-only system as groupable. A per-resource `ResourceTraits<T>::ConcurrentReadSafe` (default true) folds a non-safe read into the write mask so the existing write-involved predicate serializes read/read.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-19-astra-theme-b2-phase-c-design.md` (approved).

## Global Constraints

- Header-only C++20; MSVC-primary. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a shipping check is a real `if`, never `ASTRA_ASSERT`.
- **Additive, no break.** Existing `void(Registry&)`/`void(SystemContext&)` systems, view-lambdas, and `SystemTraits<Reads<...>, Writes<...>, Exclusive>` all keep compiling and scheduling identically. A system that declares no resource traits behaves exactly as today. No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include.
- **TypeID ceiling:** the test binary sits near the `MAX_COMPONENTS = 128` process-global `TypeID` ceiling (each `ReadsResources<R>` consumes a `ComponentID` via `TypeID<R>::Value()`). Declare only a SMALL number of fresh resource/component types across these tests (this plan uses 3 fresh resource types total).
- Namespace `Astra`. IDE/clang-tidy diagnostics are misconfigured false positives (expects Clang 20; "no gtest"; "Mosaic/Platform.hpp not found") — judge only by the MSVC build.
- All file:line anchors are from the tree at `dev` @ `342f05c`; **confirm each against the live tree before editing.**

**Build (per config, whole solution):**
`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`
Appending to the existing `tests/System/SystemSchedulerTest.cpp` needs **no** premake regen. Stale-PDB `mspdbsrv.exe` lock → `taskkill //F //IM mspdbsrv.exe` + rebuild. Baseline entering Phase C: **617 Debug / 615 Release / 615 Dist**, all green.

## File Structure

- `include/Astra/System/System.hpp` — Task 1: `ReadsResources`/`WritesResources` structs + `Detail::TraitReadsResources`/`TraitWritesResources` + `SystemTraits::ReadsResourceTypes`/`WritesResourceTypes`. Task 2: the `ResourceTraits<T>` primary template.
- `include/Astra/System/SystemMetadata.hpp` — Task 1: `resourceReads`/`resourceWrites` fields.
- `include/Astra/System/SystemScheduler.hpp` — Task 1: `ExtractSystemTraits` fills the resource masks; `BuildExecutionPlan` conflict + solo predicate. Task 2: swap the read-resource extraction for the CRS-aware fold.
- `tests/System/SystemSchedulerTest.cpp` — both tasks append grouping tests (existing file → no regen).

---

### Task 1: Core resource conflict analysis

**Files:**
- Modify: `include/Astra/System/System.hpp` (`Reads`/`Writes` ~:22-25, `Detail` ~:33-36, `SystemTraits` ~:41-47)
- Modify: `include/Astra/System/SystemMetadata.hpp` (after `writes` ~:27)
- Modify: `include/Astra/System/SystemScheduler.hpp` (`ExtractSystemTraits` ~:434-441, `BuildExecutionPlan` ~:461-508)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces (consumed by Task 2): `Astra::ReadsResources<R...>` / `Astra::WritesResources<R...>` structs; `SystemTraits<...>::ReadsResourceTypes` / `WritesResourceTypes` (tuple aliases); `SystemMetadata::resourceReads` / `resourceWrites` (`ComponentMask`); `ExtractResourceReadMask` extension point in Task 2.

- [ ] **Step 1: Write the failing tests.** Append to `tests/System/SystemSchedulerTest.cpp`. Match the file's existing test macro (`TEST(...)` — the file defines traited system structs in an anonymous namespace and constructs `Astra::SystemScheduler` locally; confirm and mirror). Add resource types + traited systems in an anonymous namespace near the existing `struct WA`/`WB` definitions:
```cpp
namespace  // extend the existing anon namespace or add a new one
{
    struct ResA { int v; };
    struct ResB { int v; };

    struct WResA  : Astra::SystemTraits<Astra::WritesResources<ResA>> { void operator()(Astra::Registry&) {} };
    struct WResA2 : Astra::SystemTraits<Astra::WritesResources<ResA>> { void operator()(Astra::Registry&) {} };
    struct RResA  : Astra::SystemTraits<Astra::ReadsResources<ResA>>  { void operator()(Astra::Registry&) {} };
    struct RResA2 : Astra::SystemTraits<Astra::ReadsResources<ResA>>  { void operator()(Astra::Registry&) {} };
    struct WResB  : Astra::SystemTraits<Astra::WritesResources<ResB>> { void operator()(Astra::Registry&) {} };
    struct WPosOnly : Astra::SystemTraits<Astra::Writes<Position>>    { void operator()(Astra::Registry&) {} };
}

// Two writers of the SAME resource conflict -> separate groups.
TEST(SystemSchedulerResourceConflict, SameResourceWritersDoNotShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WResA>().IsOk());
    ASSERT_TRUE(s.AddSystem<WResA2>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    EXPECT_EQ(plan.size(), 2u);
}

// Writer + reader of the same resource conflict -> separate groups.
TEST(SystemSchedulerResourceConflict, SameResourceWriterAndReaderDoNotShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WResA>().IsOk());
    ASSERT_TRUE(s.AddSystem<RResA>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    EXPECT_EQ(plan.size(), 2u);
}

// Two readers of the same (default-safe) resource DO share a group -- proves
// read/read is not a conflict AND that a resource-only system is groupable
// (not wrongly forced solo).
TEST(SystemSchedulerResourceConflict, SameResourceReadersShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<RResA>().IsOk());
    ASSERT_TRUE(s.AddSystem<RResA2>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

// Writers of DIFFERENT resources do not conflict -> share a group (per-resource).
TEST(SystemSchedulerResourceConflict, DifferentResourceWritersShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WResA>().IsOk());
    ASSERT_TRUE(s.AddSystem<WResB>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

// A component-writer and a disjoint resource-writer compose without conflict.
TEST(SystemSchedulerResourceConflict, ComponentAndResourceAccessComposeWithoutFalseConflict)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<WPosOnly>().IsOk());
    ASSERT_TRUE(s.AddSystem<WResA>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}
```

- [ ] **Step 2: Run — verify RED.** Build Debug. Expected: **compile failure** — `ReadsResources`/`WritesResources` are not declared. (This is the correct RED: the feature is missing.) `AstraTest` will not link until Step 3-5 land.

- [ ] **Step 3: Add the traits (`System.hpp`).** After `struct Writes` (~:25) add:
```cpp
    template<typename... Resources>
    struct ReadsResources { using type = std::tuple<Resources...>; };

    template<typename... Resources>
    struct WritesResources { using type = std::tuple<Resources...>; };
```
In `namespace Detail` after `TraitWrites` (~:36) add:
```cpp
        template<typename T> struct TraitReadsResources  { using type = std::tuple<>; };
        template<typename... R> struct TraitReadsResources<ReadsResources<R...>>  { using type = std::tuple<R...>; };
        template<typename T> struct TraitWritesResources { using type = std::tuple<>; };
        template<typename... W> struct TraitWritesResources<WritesResources<W...>> { using type = std::tuple<W...>; };
```
In `struct SystemTraits` after `WritesComponents` (~:44) add:
```cpp
        using ReadsResourceTypes  = decltype(std::tuple_cat(std::declval<typename Detail::TraitReadsResources<Traits>::type>()...));
        using WritesResourceTypes = decltype(std::tuple_cat(std::declval<typename Detail::TraitWritesResources<Traits>::type>()...));
```

- [ ] **Step 4: Add the metadata fields (`SystemMetadata.hpp`).** After `ComponentMask writes;` (~:27) add:
```cpp
        // Resources this system reads / writes (singleton state; keyed by the
        // resource type's ComponentID, in masks distinct from the component
        // reads/writes so component-vs-resource can never false-conflict).
        ComponentMask resourceReads;
        ComponentMask resourceWrites;
```
Confirm every `SystemMetadata` construction uses designated-initializer aggregate init (the one in `AddSystemInternal` ~:554 does: `.reads = ..., .writes = ..., ...`); omitted designated fields value-initialize to `ComponentMask{}` (zero), so no construction site needs the new fields listed. If any site uses positional aggregate init, add `.resourceReads = ComponentMask{}, .resourceWrites = ComponentMask{}` there.

- [ ] **Step 5: Fill the masks in `ExtractSystemTraits` (`SystemScheduler.hpp` ~:434).** Inside the `if constexpr (HasSystemTraits_v<T>)` body, after the two existing `ExtractComponentMask` calls, add:
```cpp
                if constexpr (requires { typename T::ReadsResourceTypes; })
                {
                    ExtractComponentMask<typename T::ReadsResourceTypes>(metadata.resourceReads);
                    ExtractComponentMask<typename T::WritesResourceTypes>(metadata.resourceWrites);
                }
```
(The `requires` guard keeps a hypothetical hand-rolled traited type without the resource aliases compiling; all `SystemTraits`-derived systems have them.)

- [ ] **Step 6: Extend `BuildExecutionPlan` (`SystemScheduler.hpp` ~:461).** Immediately after `m_executionPlan.clear();` and the `if (m_systems.empty()) { ... return; }` early-out, add a local helper:
```cpp
            // A system participates in grouping only if it declares SOME access.
            // A system with no declared component OR resource access runs solo
            // (unchanged for component-only systems; extended to cover resources
            // so a resource-only system is groupable rather than forced solo).
            const auto declaresAccess = [](const SystemMetadata& m)
            {
                return !(m.reads.None() && m.writes.None()
                      && m.resourceReads.None() && m.resourceWrites.None());
            };
```
Add the group accumulators beside the component ones:
```cpp
                ComponentMask groupResourceReads = sysI.resourceReads;
                ComponentMask groupResourceWrites = sysI.resourceWrites;
```
Replace the opener's `acceptsMore` line:
```cpp
                const bool acceptsMore = !sysI.requiresExclusive && declaresAccess(sysI);
```
Replace the inner-loop break conditions (the `requiresExclusive || (...None()...)` check and the mask-conflict `if`) with:
```cpp
                    if (sysJ.requiresExclusive || !declaresAccess(sysJ))
                        break;
                    if ((sysJ.writes & groupWrites).Any() ||
                        (sysJ.writes & groupReads ).Any() ||
                        (sysJ.reads  & groupWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceWrites).Any() ||
                        (sysJ.resourceWrites & groupResourceReads ).Any() ||
                        (sysJ.resourceReads  & groupResourceWrites).Any())
                        break;
```
Add the resource accumulator updates beside the component ones at the end of the loop body:
```cpp
                    groupResourceReads  |= sysJ.resourceReads;
                    groupResourceWrites |= sysJ.resourceWrites;
```

- [ ] **Step 7: Run — verify GREEN.** Build all 3 configs. Run `AstraTest.exe --gtest_filter=SystemSchedulerResourceConflict.*` (5 tests pass) each config. Confirm the pre-existing `SystemScheduler` grouping tests still pass (component-only grouping unchanged). `git grep -n 'Commands/\|System/' include/Astra/Registry/Registry.hpp` shows nothing new. Full suite green (baseline 617/615/615 + 5 new → 622/620/620).

- [ ] **Step 8: Commit.**
```bash
git add include/Astra/System/System.hpp include/Astra/System/SystemMetadata.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): fold resource read/write access into scheduler conflict analysis"
```

---

### Task 2: ConcurrentReadSafe (per-resource read serialization)

**Files:**
- Modify: `include/Astra/System/System.hpp` (add `ResourceTraits<T>` primary template near the resource traits)
- Modify: `include/Astra/System/SystemScheduler.hpp` (swap the read-resource extraction for a CRS-aware fold; add the `ExtractResourceReadMask` helper)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `ReadsResourceTypes`, `resourceReads`/`resourceWrites`, `ExtractComponentMask`/`MakeComponentMask`.
- Produces: `Astra::ResourceTraits<T>` (primary template `ConcurrentReadSafe = true`; user-specializable).

- [ ] **Step 1: Add the `ResourceTraits<T>` primary template (`System.hpp`).** Near the `ReadsResources`/`WritesResources` structs add:
```cpp
    // Per-resource concurrency trait. A resource whose ConcurrentReadSafe is
    // false serializes ALL access to it -- even two readers -- for genuinely
    // non-thread-safe external state (a GPU queue, a non-thread-safe library
    // handle). Specialize to opt out; the default is safe.
    template<typename T>
    struct ResourceTraits { static constexpr bool ConcurrentReadSafe = true; };
```

- [ ] **Step 2: Write the failing test.** Append to `tests/System/SystemSchedulerTest.cpp`. Add a non-thread-safe resource in a NAMED namespace (so the `ResourceTraits` specialization can name it), the specialization at namespace scope, and two reader systems:
```cpp
namespace SchedResCrs
{
    struct ResNTS { int v; };  // non-thread-safe resource
    struct RNtsA : Astra::SystemTraits<Astra::ReadsResources<ResNTS>> { void operator()(Astra::Registry&) {} };
    struct RNtsB : Astra::SystemTraits<Astra::ReadsResources<ResNTS>> { void operator()(Astra::Registry&) {} };
}
template<> struct Astra::ResourceTraits<SchedResCrs::ResNTS> { static constexpr bool ConcurrentReadSafe = false; };

// Two READERS of a non-ConcurrentReadSafe resource must serialize -> separate groups.
TEST(SystemSchedulerResourceConflict, NonConcurrentReadSafeReadersDoNotShareGroup)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<SchedResCrs::RNtsA>().IsOk());
    ASSERT_TRUE(s.AddSystem<SchedResCrs::RNtsB>().IsOk());
    const auto& plan = s.GetExecutionPlan();
    EXPECT_EQ(plan.size(), 2u);
}
```

- [ ] **Step 3: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerResourceConflict.NonConcurrentReadSafeReadersDoNotShareGroup`. Expected: **FAIL — `plan.size()` is 1, expected 2.** (With Task 1's plain extraction, a `ReadsResources<ResNTS>` read still routes to `resourceReads`, so read/read does not conflict and the two share a group. The `ResourceTraits` specialization exists but nothing consults it yet.)

- [ ] **Step 4: Swap the read-resource extraction for the CRS fold (`SystemScheduler.hpp`).** In `ExtractSystemTraits`, replace the Task-1 line `ExtractComponentMask<typename T::ReadsResourceTypes>(metadata.resourceReads);` with:
```cpp
                    ExtractResourceReadMask<typename T::ReadsResourceTypes>(metadata.resourceReads, metadata.resourceWrites);
```
(Keep the `WritesResourceTypes` → `resourceWrites` line unchanged.) Add the helper beside `ExtractComponentMask` (~:443):
```cpp
        // For each read resource R: a ConcurrentReadSafe resource sets its bit in
        // `reads`; a non-safe one folds into `writes` so the existing write-involved
        // conflict predicate serializes even two readers.
        template<typename Tuple>
        void ExtractResourceReadMask(ComponentMask& reads, ComponentMask& writes)
        {
            ExtractResourceReadMaskImpl<Tuple>(reads, writes, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        template<typename Tuple, size_t... Is>
        void ExtractResourceReadMaskImpl(ComponentMask& reads, ComponentMask& writes, std::index_sequence<Is...>)
        {
            ([&]
            {
                using R = std::tuple_element_t<Is, Tuple>;
                if constexpr (ResourceTraits<R>::ConcurrentReadSafe)
                    reads |= MakeComponentMask<R>();
                else
                    writes |= MakeComponentMask<R>();
            }(), ...);
        }
```
(`MakeComponentMask<R>()` is the same helper `ExtractComponentMaskImpl` already uses; confirm it is in scope in this header.)

- [ ] **Step 5: Run — verify GREEN.** Build all 3 configs. The new test passes (`plan.size() == 2`), and Task 1's `SameResourceReadersShareGroup` (default-safe resource) still passes (`plan.size() == 1`) — the two together prove the fold is conditional on `ConcurrentReadSafe`. Full suite green (→ 623/621/621).

- [ ] **Step 6: Commit.**
```bash
git add include/Astra/System/System.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): ResourceTraits<T>::ConcurrentReadSafe serializes non-thread-safe resource reads"
```

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §10 module 1 (traits) → Task 1 Step 3; module 2 (metadata) → Task 1 Step 4; module 3 (extraction) → Task 1 Step 5 + Task 2 Step 4; module 4 (plan builder conflict + solo predicate) → Task 1 Step 6; `ConcurrentReadSafe` → Task 2. Advisory-masks boundary and thread-affinity deferral are design notes with no code (correctly no task).
- **Placeholder scan:** every code step shows complete code; every test step shows the assertions; RED/GREEN expectations are concrete (compile error for Task 1; `plan.size()` 1-vs-2 for Task 2).
- **Type consistency:** `ReadsResourceTypes`/`WritesResourceTypes` (Task 1 Step 3) are consumed by `ExtractSystemTraits` (Task 1 Step 5, Task 2 Step 4); `resourceReads`/`resourceWrites` (Task 1 Step 4) are filled in extraction and read in `BuildExecutionPlan` (Task 1 Step 6); `ResourceTraits<T>::ConcurrentReadSafe` (Task 2 Step 1) is consulted by `ExtractResourceReadMaskImpl` (Task 2 Step 4). `ExtractComponentMask`/`MakeComponentMask` names match the existing helpers.
- **TypeID budget:** 3 fresh resource types total (`ResA`, `ResB`, `ResNTS`) plus reuse of existing `Position` — well within the 128 ceiling.
- **Additive:** systems without resource traits get empty resource masks (the `requires`-guarded extraction + default-zero `ComponentMask`), so their grouping is unchanged; `Exclusive` still forces solo.
