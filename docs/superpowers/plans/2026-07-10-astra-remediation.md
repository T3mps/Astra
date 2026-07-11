# Astra v3.4 Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix every confirmed defect from the 2026-07-10 full-source review (stale views, wrong default-construction, command-buffer corruption, invalid-entity leaks, empty-tag UB, dangling signal pointers, config loss, ID-space overflow, non-portable archives, missing resource persistence) so all behavior is correct in Debug, Release, and Dist.

**Architecture:** Astra is a header-only C++20 archetype ECS. All fixes are edits to headers under `include/Astra/` plus new GoogleTest files under `tests/`. Each task is TDD: write the regression test (adapted from the review's executed repros), watch it fail, apply the minimal header fix, watch it pass, run the full suite, commit.

**Tech Stack:** C++20 headers, GoogleTest, premake5 → MSBuild (VS solution), Google Benchmark (unchanged).

## Global Constraints

- C++20; header-only; **no exceptions** (test project builds with exceptions off; never add `throw`/`try`); no new dependencies.
- Must compile on MSVC 2022+, GCC 11+, Clang 13+ (CI covers gcc/clang — avoid MSVC-only constructs).
- Test files are globbed by premake (`tests/**.cpp`), but the checked-in `.vcxproj` files are static: **after adding any new file, regenerate the solution**: `D:\dev\_shared\tools\premake5 vs2022` (run from repo root; same as `scripts\generate_vs2022.bat`).
- Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m -v:minimal` (build the whole solution; `-t:AstraTest` does not work — projects are nested in solution folders).
- Test: `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`. **Gate for every task: full suite passes in BOTH Debug and Release** (Task 1 makes the Debug gate possible).
- Work on a branch: `git checkout -b remediation/3.4 dev` (first task creates it).
- Commit style (matches repo history): `fix(scope): ...`, `feat(scope): ...`, `test(scope): ...`, `chore: ...`.
- Behavior contract adopted throughout (matches Release behavior today): **recoverable caller input (invalid entities, cycles, self-links, dead handles) is rejected gracefully in ALL configs — never `ASTRA_ASSERT` on caller input.** Asserts are for internal invariants only.

---

## Phase 0 — Unblock verification & baseline the test suite

### Task 1: Make the Debug test suite run to completion

Six relationship tests abort Debug runs because `ASTRA_ASSERT` fires on inputs the API deliberately rejects gracefully (cycles, self-links, invalid entities), and one compression test has a release-only performance threshold. CI never caught it because CI tests Release only. Until this is fixed, no other task can use Debug as a gate.

**Files:**
- Modify: `include/Astra/Registry/RelationshipGraph.hpp:147-171` (SetParent), `:228-235` (AddLink)
- Modify: `tests/Serialization/Compression/CompressionTests.cpp:398-405` (approx — the `PerformanceBenchmark` EXPECTs)

**Interfaces:**
- Consumes: nothing.
- Produces: a green Debug suite — the verification gate every later task relies on. No API changes.

- [ ] **Step 1: Create the branch and reproduce the abort**

```bash
git checkout -b remediation/3.4 dev
```

Build Debug (command in Global Constraints), then run:
`bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`
Expected: process aborts with exit code 3 and prints `Assertion failed: (!IsAncestorOf(child, parent)) ... RelationshipGraph.hpp, line 158`.

- [ ] **Step 2: Remove caller-input asserts from SetParent**

In `RelationshipGraph.hpp`, replace the beginning of `SetParent`:

```cpp
        void SetParent(Entity child, Entity parent)
        {
            ASTRA_ASSERT(child != parent, "Entity cannot be its own parent");
            ASTRA_ASSERT(child.IsValid() && parent.IsValid(), "Invalid entity in relationship");

            // Silently ignore invalid operations in release builds
            if (!child.IsValid() || !parent.IsValid() || child == parent)
                return;

            // Check for circular hierarchy: if child is an ancestor of parent,
            // setting parent as child's parent would create a cycle
            ASTRA_ASSERT(!IsAncestorOf(child, parent), "Circular hierarchy detected: child is an ancestor of parent");
            if (IsAncestorOf(child, parent))
                return;
```

with:

```cpp
        void SetParent(Entity child, Entity parent)
        {
            // Caller-recoverable inputs: rejected gracefully in ALL configs.
            // (Asserting here made the Debug suite abort on tests that verify
            // the rejection contract; asserts are reserved for internal invariants.)
            if (!child.IsValid() || !parent.IsValid() || child == parent)
                return;

            // Rejecting a cycle: if child is an ancestor of parent, setting
            // parent as child's parent would create a cycle.
            if (IsAncestorOf(child, parent))
                return;
```

- [ ] **Step 3: Remove caller-input asserts from AddLink**

Replace:

```cpp
        void AddLink(Entity a, Entity b)
        {
            ASTRA_ASSERT(a != b, "Entity cannot link to itself");
            ASTRA_ASSERT(a.IsValid() && b.IsValid(), "Invalid entity in link");
            
            // Silently ignore invalid operations in release builds
            if (!a.IsValid() || !b.IsValid() || a == b)
                return;
```

with:

```cpp
        void AddLink(Entity a, Entity b)
        {
            // Caller-recoverable inputs: rejected gracefully in ALL configs.
            if (!a.IsValid() || !b.IsValid() || a == b)
                return;
```

Do NOT touch the assert inside `BuildAncestorCache` (line ~653) — that one guards an internal invariant (a cycle that slipped past `SetParent`).

- [ ] **Step 4: Gate the performance thresholds in CompressionTests.cpp**

Find the `PerformanceBenchmark` test's threshold assertions (around line 403):

```cpp
    EXPECT_GT(compressMBps, 10.0f);
```

Wrap all throughput EXPECTs in that test (there may be one for decompression too):

```cpp
#ifdef NDEBUG
    // Throughput thresholds are meaningless in unoptimized builds
    // (Debug LZ4 measures ~2 MB/s); only enforce them in optimized configs.
    EXPECT_GT(compressMBps, 10.0f);
#endif
```

Apply the same `#ifdef NDEBUG` guard to any other `EXPECT_GT(...MBps...)` in the same test. Keep the correctness assertions (round-trip equality) unguarded.

- [ ] **Step 5: Rebuild Debug and run the full suite**

Expected: `[==========] 502 tests from 38 test suites ran.` and `[  PASSED  ] 502 tests.` Exit code 0. The six former aborters (`ComplexRelationshipTest.RapidRelationshipChanges`, `RelationsTest.CircularHierarchyHandling`, `RelationshipGraphTest.{SelfLinks, CircularRelationshipPrevention, SelfLinkingPrevention, InvalidEntityOperations}`) now pass.

- [ ] **Step 6: Build Release and run the full suite**

Expected: 502/502 pass (unchanged behavior — Release asserts were already no-ops).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Registry/RelationshipGraph.hpp tests/Serialization/Compression/CompressionTests.cpp
git commit -m "fix(relationship): reject caller-recoverable inputs gracefully in all configs

Asserting on cycles/self-links/invalid entities made the Debug suite abort
on the six tests that verify the rejection contract; CompressionTest's
throughput thresholds are now NDEBUG-only. Debug suite runs green for the
first time."
```

### Task 2: Audit every existing test file (quality, structure, coverage)

The suite is substantial (37 files, 502 tests, ~13K lines) but has systemic blind spots the review exposed: every ViewTest creates a fresh view after mutations (which is why the stale-view bug survived), Debug/Release behavior divergence was never pinned, and no test exercises non-default configs through serialization. Audit every file against a fixed rubric, record findings in an audit document, apply the zero-risk mechanical fixes immediately, and route everything else to the task that covers it (or to Deferred).

**Files:**
- Create: `docs/superpowers/plans/2026-07-10-test-suite-audit.md` (the deliverable)
- Modify: `tests/TestCommandBuffer.cpp` → move to `tests/Commands/CommandBufferTest.cpp` (Category B, known)
- Modify: oversized test files confirmed by the audit (split candidates listed in Step 4; test bodies stay byte-identical)
- Modify: `premake5.lua` is NOT touched (tests are globbed) but the solution must be regenerated after any move/split.

**Interfaces:**
- Consumes: the built Debug and Release `AstraTest` binaries from Task 1 (suite must already run green).
- Produces: `docs/superpowers/plans/2026-07-10-test-suite-audit.md` with one verdict row per test file; the directory `tests/Commands/` (Task 5 creates `tests/Commands/CommandBufferAlignmentTest.cpp` and relies on this location); an unchanged test count (`--gtest_list_tests` identical before/after, modulo renamed suite prefixes recorded in the audit doc).

- [ ] **Step 1: Baseline and order-dependence check**

Record the baseline and shuffle-run both configs — order dependence found here is itself a finding:

```bash
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_list_tests | grep -c "^  " > /tmp/baseline_count.txt
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1 --gtest_shuffle --gtest_random_seed=12345
bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1 --gtest_shuffle --gtest_random_seed=12345
```

Expected: both shuffled runs pass 502/502. Any failure is an order-dependence finding — record it in the audit doc (Category C unless trivially a missing fixture reset, which is Category B).

- [ ] **Step 2: Create the audit document with the full inventory**

Create `docs/superpowers/plans/2026-07-10-test-suite-audit.md` starting from this exact skeleton — the inventory below is the complete worklist (test counts / line counts from the 2026-07-10 review); fill one verdict row per file as you review:

```markdown
# Astra Test Suite Audit — 2026-07-10

Verdicts: OK | SPLIT (needs decomposition) | FIX (test-quality defect) | GAP (missing coverage)
Dispositions: A = covered by remediation Task N | B = fixed in this task | C = new item (appended to plan Deferred section or filed as follow-up)

| File | Tests | Lines | Verdict | Findings | Disposition |
|---|---|---|---|---|---|
| tests/TestMain.cpp | - | 7 | | | |
| tests/TestComponents.hpp | - | 332 | | | |
| tests/TestCommandBuffer.cpp | 12 | 350 | | | |
| tests/Comprehensive/ComplexRelationshipTest.cpp | 9 | 561 | | | |
| tests/Comprehensive/ComponentLifecycleTest.cpp | 8 | 487 | | | |
| tests/Comprehensive/ErrorRecoveryTest.cpp | 9 | 389 | | | |
| tests/Comprehensive/MemoryCleanupTest.cpp | 7 | 353 | | | |
| tests/Comprehensive/ResourceExhaustionTest.cpp | 10 | 476 | | | |
| tests/Container/AlignedStorageTest.cpp | 8 | 195 | | | |
| tests/Container/BitmapFuzzTest.cpp | 4 | 78 | | | |
| tests/Container/BitmapTest.cpp | 10 | 317 | | | |
| tests/Container/FlatMapFuzzTest.cpp | 7 | 154 | | | |
| tests/Container/FlatMapTest.cpp | 29 | 851 | | | |
| tests/Container/FlatSetFuzzTest.cpp | 8 | 165 | | | |
| tests/Container/FlatSetTest.cpp | 15 | 459 | | | |
| tests/Container/SmallVectorFuzzTest.cpp | 2 | 75 | | | |
| tests/Container/SmallVectorTest.cpp | 23 | 688 | | | |
| tests/Core/TypeContextTest.cpp | 6 | 78 | | | |
| tests/Core/TypeIDTests.cpp | 9 | 186 | | | |
| tests/Core/WorkSchedulerTest.cpp | 7 | 101 | | | |
| tests/Entity/EntityManagerSerializationTest.cpp | 11 | 515 | | | |
| tests/Entity/EntityManagerTest.cpp | 26 | 598 | | | |
| tests/Entity/EntityTest.cpp | 21 | 449 | | | |
| tests/Component/ComponentRegistryTest.cpp | 24 | 674 | | | |
| tests/Reflection/FieldVisitorTest.cpp | 4 | 144 | | | |
| tests/Reflection/ReflectionTest.cpp | 43 | 944 | | | |
| tests/Registry/ArchetypeManagerTest.cpp | 16 | 521 | | | |
| tests/Registry/ArchetypeTest.cpp | 25 | 1090 | | | |
| tests/Registry/ParallelIterationTest.cpp | 4 | 124 | | | |
| tests/Registry/RegistryLoadConfigTest.cpp | 2 | 73 | | | |
| tests/Registry/RegistryTest.cpp | 23 | 814 | | | |
| tests/Registry/RelationsTest.cpp | 16 | 560 | | | |
| tests/Registry/RelationshipGraphSerializationTest.cpp | 10 | 536 | | | |
| tests/Registry/RelationshipGraphTest.cpp | 16 | 451 | | | |
| tests/Registry/ResourceTest.cpp | 18 | 505 | | | |
| tests/Registry/ViewIteratorTest.cpp | 8 | 290 | | | |
| tests/Registry/ViewTest.cpp | 12 | 381 | | | |
| tests/Serialization/BinarySerializationTests.cpp | 28 | 1083 | | | |
| tests/Serialization/Compression/CompressionTests.cpp | 14 | 404 | | | |
| tests/Support/TestWorkerPool.hpp | - | 153 | | | |

## Known findings seeded from the 2026-07-10 source review
(verify each during the per-file pass; do not re-derive)
- ViewTest.cpp: every test constructs a fresh view after mutation — cached-view
  contracts untested. Disposition A: Task 4 adds ViewInvalidationTest.
- CompressionTests.cpp: perf thresholds in unit tests. Disposition A: Task 1.
- RelationshipGraphTest/RelationsTest/ComplexRelationshipTest rejection tests:
  now that Task 1 removed the Debug asserts, confirm each asserts OBSERVABLE
  state after rejection (GetParent unchanged, no link added) — not just
  "did not crash". Any that only prove absence-of-crash: Verdict FIX,
  Disposition B (strengthen the assertion in place).
- TestCommandBuffer.cpp: lives at tests/ root; every other suite is foldered.
  Disposition B: move to tests/Commands/CommandBufferTest.cpp.
- No test drives a non-default chunkPoolConfig through Save/Load.
  Disposition A: Task 10.
- No coverage: NSDMI defaults / Release zeroing (Task 3), alignas payloads in
  CommandBuffer (Task 5), entity exhaustion & dead-handle batch adds (Task 6),
  tags in range-for (Task 7), ComponentRemoved pointer lifetime (Task 8),
  Delegate large functors (Task 9), ID-space overflow (Task 11), 16/64-bit
  entity widths (Task 12), archive portability (Task 13), resource
  persistence (Task 14). All Disposition A.
- Fuzz tests (Bitmap/FlatMap/FlatSet/SmallVector): confirm fixed seeds and a
  reference-container oracle (std::unordered_map / std::set / std::vector
  comparison). Missing oracle or nondeterministic seed: Verdict FIX.
- TestComponents.hpp (332 lines, shared by many suites): flag unused component
  types and any component whose semantics assume Debug-only zeroing.
- ParallelIterationTest.cpp (4 tests): confirm it covers BOTH the injected-
  scheduler path and the null-scheduler sequential fallback, and nested
  ParallelFor reentrancy. Missing legs: Verdict GAP, Disposition C.

## Public-API spot-check (record hits/misses)
While reviewing each module's file, check these known-thin areas and record
whether ANY test exercises them: Registry::Defragment + DefragmentationResult
fields, Registry::GetFragmentationLevel, CommandBuffer::MergeFrom,
ParallelCommandBuffer::MergeInto/Clear/GetThreadCount, View::Size/Empty,
EntityManager::ShrinkToFit, ArchetypeChunkPool::Defragment, SignalManager
handler Unregister, EnumInfo flag enums, JsonSchema generation for nested
types. Untested public API: Verdict GAP, Disposition C (append to the plan's
Deferred section as "test-coverage follow-ups" with the API name).
```

- [ ] **Step 3: Per-file review pass**

Read each file top to bottom (all under `tests/`) and evaluate against this rubric — every check is concrete; record violations in the Findings column:

1. **Location/naming**: file lives in `tests/<Module>/` matching the header under test; suite name matches file name.
2. **Size/cohesion**: > ~700 lines or > ~25 tests spanning more than one responsibility ⇒ Verdict SPLIT with the proposed split named in Findings.
3. **Real assertions**: every `TEST` has at least one `EXPECT_`/`ASSERT_` that can fail; no print-only or tautological tests.
4. **Config parity**: no test depends on Debug-only behavior (memset zeroing, assert side effects) or is gated to a single config without an `#ifdef NDEBUG` + explanatory comment.
5. **Determinism**: RNG uses fixed seeds; no wall-clock timing assertions outside NDEBUG guards; no sleeps as synchronization.
6. **Isolation**: fixtures reset shared state in `SetUp`/`TearDown`; no inter-test order dependence (Step-1 shuffle run is the oracle).
7. **Negative paths**: each module has at least one test of its documented failure path (invalid input rejected, error `Result` returned). Missing ⇒ GAP.
8. **Both storage variants**: serialization tests cover file AND memory targets; ResourceStorage tests cover SBO (≤64B) AND heap (>64B) sizes; SmallVector tests cover inline AND spilled. Missing leg ⇒ GAP.
9. **No dead helpers**: helper functions/fixtures actually used; unused ⇒ FIX (delete).
10. **Assertion specificity**: state-change tests assert the new state AND the non-change of adjacent state (e.g., a rejected `SetParent` leaves `GetParent` unchanged) — not merely return values.

- [ ] **Step 4: Apply Category B fixes (mechanical, zero-risk)**

(a) Move `tests/TestCommandBuffer.cpp` to `tests/Commands/CommandBufferTest.cpp` via `git mv` (creates the `tests/Commands/` directory Task 5 expects).
(b) Strengthen rejection tests flagged in Step 3 in place (add the missing observable-state assertion, e.g. after a rejected cycle: `EXPECT_EQ(registry->GetParent(child), originalParent);`).
(c) For each file the audit confirmed as SPLIT (expected candidates: `Registry/ArchetypeTest.cpp` 1090, `Serialization/BinarySerializationTests.cpp` 1083, `Reflection/ReflectionTest.cpp` 944, `Container/FlatMapTest.cpp` 851; `Registry/RegistryTest.cpp` 814 is borderline — split only with ≥2 clearly separable themes), split mechanically:
   - One new file per theme (e.g., `BinarySerializationTests.cpp` → keep core + new `BinarySerializationVersioningTests.cpp`), TEST bodies moved **byte-identical**; shared fixtures/helpers move to the file that owns them, or are duplicated only if < 20 lines.
   - Rule: no test-logic changes inside a split — splits (c) and assertion-strengthening (b) must be separable in the diff.
(d) Delete dead helpers found by rubric check 9.

- [ ] **Step 5: Regenerate and verify nothing changed behaviorally**

```bash
D:/dev/_shared/tools/premake5 vs2022
```

Rebuild Debug + Release, then:

```bash
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_list_tests | grep -c "^  "   # must equal Step 1 baseline (502)
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1 --gtest_shuffle --gtest_random_seed=12345
bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1 --gtest_shuffle --gtest_random_seed=12345
```

Expected: count identical to baseline; both shuffled runs 502/502.

- [ ] **Step 6: Route Category C findings**

Append every Disposition-C row to this plan's **Deferred** section under a new bullet `**Test-coverage follow-ups (from Task 2 audit):**` — one line each, naming the file/API and the missing check. The audit table's Disposition column must have no blanks.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/plans/2026-07-10-test-suite-audit.md tests/ ide/ docs/superpowers/plans/2026-07-10-astra-remediation.md
git commit -m "test: audit all suite files; relocate command-buffer tests; mechanical splits and assertion strengthening

Per-file audit with verdicts and dispositions in
docs/superpowers/plans/2026-07-10-test-suite-audit.md. Test count and
behavior unchanged; coverage gaps routed to their remediation tasks or the
plan's Deferred list."
```

---

## Phase 1 — State-corruption fixes

### Task 3: Correct default-construction semantics

`ComponentDescriptor::DefaultConstruct` currently skips construction for `is_trivially_copyable && is_nothrow_default_constructible` types (memset in Debug, nothing in Release). Two confirmed consequences: (a) NSDMI defaults like `Health{int current = 100;}` come out zero in every config; (b) in Release, a chunk slot vacated by swap-and-pop hands the previous entity's bytes to the next `CreateEntity<T>()`. The correct gate is `is_trivially_default_constructible` (for which value-init ≡ zero-fill), and the zero-fill must always happen.

**Files:**
- Create: `tests/Component/DefaultConstructTest.cpp`
- Modify: `include/Astra/Component/Component.hpp:60-133` (descriptor field + `DefaultConstruct` + `BatchDefaultConstruct`)
- Modify: `include/Astra/Component/ComponentRegistry.hpp:134-138` (populate new field)

**Interfaces:**
- Consumes: `Registry::CreateEntity<T>()`, `Registry::CreateEntities<T>(count, span)`, `Registry::DestroyEntity`.
- Produces: new descriptor field `bool ComponentDescriptor::is_trivially_default_constructible;` — Task 6's move paths and any future code must treat `DefaultConstruct` as "always produces a value-initialized object".

- [ ] **Step 1: Write the failing tests**

Create `tests/Component/DefaultConstructTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <vector>

namespace
{
    struct NsdmiHealth { int current = 100; int max = 100; };  // trivially copyable, NOT trivially default-constructible
    struct PodPoint { float x, y, z; };                        // trivially default-constructible
}

TEST(DefaultConstruct, NsdmiAppliedOnCreateEntity)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<NsdmiHealth>();
    auto* h = reg.GetComponent<NsdmiHealth>(e);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->current, 100);
    EXPECT_EQ(h->max, 100);
}

TEST(DefaultConstruct, NsdmiAppliedOnBatchCreate)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> out(64);
    reg.CreateEntities<NsdmiHealth>(64, out);
    for (auto e : out)
    {
        auto* h = reg.GetComponent<NsdmiHealth>(e);
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->current, 100);
    }
}

TEST(DefaultConstruct, PodZeroedAfterSlotReuse)
{
    Astra::Registry reg;
    auto e1 = reg.CreateEntity<PodPoint>();
    auto* p1 = reg.GetComponent<PodPoint>(e1);
    p1->x = 123.0f; p1->y = 456.0f; p1->z = 789.0f;
    reg.DestroyEntity(e1);

    auto e2 = reg.CreateEntity<PodPoint>();   // reuses the vacated chunk slot
    auto* p2 = reg.GetComponent<PodPoint>(e2);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->x, 0.0f);
    EXPECT_EQ(p2->y, 0.0f);
    EXPECT_EQ(p2->z, 0.0f);
}
```

- [ ] **Step 2: Regenerate the solution, build Release, verify the tests fail**

Run `D:\dev\_shared\tools\premake5 vs2022`, build Release, then:
`bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=DefaultConstruct.*`
Expected: `NsdmiAppliedOnCreateEntity` FAILS (current=0), `NsdmiAppliedOnBatchCreate` FAILS, `PodZeroedAfterSlotReuse` FAILS (x=123).

- [ ] **Step 3: Add the descriptor field and fix the two construct helpers**

In `include/Astra/Component/Component.hpp`, after the line `bool is_nothrow_default_constructible;` add:

```cpp
        bool is_trivially_default_constructible;
```

Replace `DefaultConstruct`:

```cpp
        inline void DefaultConstruct(void* ptr) const
        {
            if (size == 0)
            {
                return;  // empty (tag) component: nothing to construct
            }
            if (is_trivially_default_constructible)
            {
                // Value-initialization of a trivially-default-constructible
                // type is zero-initialization; memset is the fast equivalent.
                // This must run in ALL configs: a chunk slot vacated by
                // swap-and-pop still holds the previous entity's bytes.
                std::memset(ptr, 0, size);
            }
            else
            {
                defaultConstruct(ptr);  // applies NSDMIs / user default ctor
            }
        }
```

Replace `BatchDefaultConstruct` (also deleting the dead `if (is_empty) {}` block):

```cpp
        inline void BatchDefaultConstruct(void* ptr, size_t count) const
        {
            if (size == 0)
            {
                return;
            }
            if (is_trivially_default_constructible)
            {
                std::memset(ptr, 0, count * size);
            }
            else
            {
                std::byte* p = static_cast<std::byte*>(ptr);
                for (size_t i = 0; i < count; ++i)
                {
                    defaultConstruct(p + i * size);
                }
            }
        }
```

- [ ] **Step 4: Populate the field at registration**

In `ComponentRegistry.hpp::RegisterComponentImpl`, next to the other trait assignments:

```cpp
            desc.is_trivially_default_constructible = std::is_trivially_default_constructible_v<T>;
```

- [ ] **Step 5: Build Release, run the new tests — expect PASS. Then the full suite in Release AND Debug — expect 505/505.**

- [ ] **Step 6: Commit**

```bash
git add tests/Component/DefaultConstructTest.cpp include/Astra/Component/Component.hpp include/Astra/Component/ComponentRegistry.hpp ide/
git commit -m "fix(component): DefaultConstruct always value-initializes

Gate the memset fast path on is_trivially_default_constructible (not
is_trivially_copyable) and run it in every config. Fixes NSDMI defaults
coming out zero and stale component data on chunk-slot reuse in Release."
```

### Task 4: Fix View cache invalidation (stale views + dangling archetype pointers)

`View` refresh keys off a counter that only bumps on archetype *creation*, and views skip empty archetypes when collecting — so a view created while a matching archetype is empty never sees entities added to it (confirmed by repro). Additionally, `ArchetypeManager::CleanupEmptyArchetypes` (Defragment) deletes archetypes without bumping any counter, leaving cached views holding dangling `Archetype*`.

**Files:**
- Create: `tests/Registry/ViewInvalidationTest.cpp`
- Modify: `include/Astra/Registry/View.hpp:44-49` (ctor), `:142-155` (Size/Empty), `:195-258` (EnsureArchetypes/CollectArchetypes), `:361-366` (members)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:601-686` (CleanupEmptyArchetypes), `:1325` (new member)

**Interfaces:**
- Consumes: `ArchetypeManager::m_structuralChangeCounter` (existing), `View` is already a friend of `ArchetypeManager`.
- Produces: `std::atomic<uint32_t> ArchetypeManager::m_archetypeRemovalCounter{0}` (bumped whenever archetypes are deleted; read by View), `uint32_t View::m_lastRemovalCounter`. Contract: **empty matching archetypes stay in a view's archetype list** (iterating them is free — their chunks have count 0), and any archetype removal forces a full re-collect.

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/ViewInvalidationTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct VPos { float x, y, z; }; struct VVel { float dx, dy, dz; }; }

TEST(ViewInvalidation, SeesEntityAddedToPreexistingEmptyArchetype)
{
    Astra::Registry reg;
    auto e1 = reg.CreateEntity<VPos>();   // creates archetype [VPos]
    reg.DestroyEntity(e1);                // archetype now empty but alive

    auto view = reg.CreateView<VPos>();   // collected while archetype is empty

    reg.CreateEntity<VPos>();             // reuses existing archetype: no new-archetype event

    size_t seen = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++seen; });
    EXPECT_EQ(seen, 1u);                  // was 0 before the fix

    // Size()/Empty() must agree with iteration
    EXPECT_EQ(view.Size(), 1u);
    EXPECT_FALSE(view.Empty());
}

TEST(ViewInvalidation, SurvivesArchetypeRemoval)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<VPos>();
    auto b = reg.CreateEntity<VPos, VVel>();   // second archetype so removal has candidates
    auto view = reg.CreateView<VPos>();
    size_t first = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++first; });
    EXPECT_EQ(first, 2u);

    reg.DestroyEntity(a);
    reg.DestroyEntity(b);

    Astra::Registry::DefragmentationOptions opts;
    opts.minArchetypesToKeep = 1;              // allow removing the emptied archetypes
    reg.Defragment(opts);

    size_t after = 0;
    view.ForEach([&](Astra::Entity, VPos&) { ++after; });  // must not touch freed archetypes
    EXPECT_EQ(after, 0u);
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ViewInvalidation.*`
Expected: `SeesEntityAddedToPreexistingEmptyArchetype` FAILS with `seen == 0`. (`SurvivesArchetypeRemoval` may pass by luck — freed-memory reads are nondeterministic; keep it regardless.)

- [ ] **Step 3: Add the removal counter to ArchetypeManager**

In `ArchetypeManager.hpp`, next to `m_structuralChangeCounter`:

```cpp
        std::atomic<uint32_t> m_structuralChangeCounter{0};  // Fast path check
        std::atomic<uint32_t> m_archetypeRemovalCounter{0};  // Bumped when archetypes are deleted; views must fully re-collect
```

At the end of `CleanupEmptyArchetypes`, just before `return removed;`:

```cpp
            if (removed > 0)
            {
                // Views cache raw Archetype*: signal both "something changed"
                // and "pointers may be stale" so they rebuild from scratch.
                m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
                m_archetypeRemovalCounter.fetch_add(1, std::memory_order_release);
            }
```

- [ ] **Step 4: Rework View collection/refresh**

In `View.hpp`:

(a) Add member (next to `m_lastGeneration`): `uint32_t m_lastRemovalCounter = 0;`

(b) In the constructor, after `m_lastGeneration = m_archetypeManager->m_generation;` add:
```cpp
            m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
```

(c) Replace `CollectArchetypes` — drop the empty-archetype skip and clear first:

```cpp
        void CollectArchetypes()
        {
            m_archetypes.clear();
            if (!m_archetypeManager) ASTRA_UNLIKELY
            {
                return;  // Registry destroyed
            }

            auto archetypes = m_archetypeManager->GetArchetypes();
            const size_t queryComponentCount = QueryBuilder::GetRequiredMask().Count();

            m_archetypes.reserve(archetypes.size());

            for (Archetype* archetype : archetypes)
            {
                // NOTE: empty archetypes are deliberately KEPT — they may gain
                // entities later without any archetype-creation event, and
                // iterating an empty archetype is free (zero-count chunks).
                if (archetype->GetComponentCount() < queryComponentCount) ASTRA_UNLIKELY
                {
                    continue;
                }
                if (QueryBuilder::Matches(archetype->GetMask()))
                {
                    m_archetypes.push_back(archetype);
                }
            }

            std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
        }
```

(d) Replace `EnsureArchetypes`:

```cpp
        void EnsureArchetypes()
        {
            if (!m_archetypeManager) ASTRA_UNLIKELY
                return;  // Registry destroyed

            uint32_t currentCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            if (m_lastRefreshCounter == currentCounter)
            {
                return;
            }

            uint32_t removalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            if (m_lastGeneration == 0 || removalCounter != m_lastRemovalCounter)
            {
                // Archetypes were removed (or first refresh): cached pointers
                // may be stale — rebuild the whole list.
                CollectArchetypes();
                m_lastRemovalCounter = removalCounter;
            }
            else
            {
                auto newArchetypes = m_archetypeManager->GetArchetypesSince(m_lastGeneration);
                for (Archetype* arch : newArchetypes)
                {
                    if (QueryBuilder::Matches(arch->GetMask()))
                    {
                        m_archetypes.push_back(arch);
                    }
                }
                std::sort(m_archetypes.begin(), m_archetypes.end(), ArchetypeEntityCountComparator{});
            }

            m_lastRefreshCounter = currentCounter;
            m_lastGeneration = m_archetypeManager->m_generation;
        }
```

(e) Fix `Empty()` to agree with `Size()`:

```cpp
        ASTRA_NODISCARD bool Empty() const noexcept
        {
            return Size() == 0;
        }
```

- [ ] **Step 5: Build Debug, run `--gtest_filter=ViewInvalidation.*` — expect PASS. Run full suite in Debug and Release — expect 507/507.**

- [ ] **Step 6: Commit**

```bash
git add tests/Registry/ViewInvalidationTest.cpp include/Astra/Registry/View.hpp include/Astra/Archetype/ArchetypeManager.hpp ide/
git commit -m "fix(view): keep empty archetypes in view caches; rebuild on archetype removal

Views no longer permanently miss entities added to a pre-existing empty
archetype, and Defragment can no longer leave cached views holding dangling
Archetype pointers."
```

### Task 5: Fix CommandBuffer inline-payload alignment

Writers compute the payload data offset **relative to the command start**; readers (`GetDataPtr`) re-align the **absolute address**. Commands advance in 8-byte steps, so any preceding command whose aligned size ≡ 8 (mod 16) desynchronizes the two for `alignas(16)` components (confirmed: `{1,2,3,4}` read back as `{3,4,0,0}`). Fix: make command starts 16-aligned (then relative and absolute alignment agree for all alignments ≤ 16) and turn alignment > 16 into a compile error instead of silent corruption.

**Files:**
- Create: `tests/Commands/CommandBufferAlignmentTest.cpp`
- Modify: `include/Astra/Commands/CommandBuffer.hpp:54` (ALIGNMENT), `:255-293` (AddComponent), `:325-370` (AddComponents), `:520-557` (SetResource), `:616-660` (Execute — add base-alignment assert)

**Interfaces:**
- Consumes: `CommandByteBuffer::ALIGNMENT`, `AlignUp` from `Command.hpp`.
- Produces: contract — **command-buffer payloads support `alignof(T) <= 16`; over-aligned components fail to compile with a static_assert directing callers to direct Registry APIs.** `CommandByteBuffer::ALIGNMENT == 16`.

- [ ] **Step 1: Write the failing test**

Create `tests/Commands/CommandBufferAlignmentTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Commands/CommandBuffer.hpp>

namespace
{
    struct APos { float x, y, z; };
    struct alignas(16) Align16 { float v[4]; };
}

TEST(CommandBufferAlignment, Align16SurvivesParityShiftingPredecessor)
{
    Astra::Registry reg;
    auto victim = reg.CreateEntity<APos>();

    Astra::CommandBuffer cmd(&reg);
    // DestroyEntities of 2 entities: totalSize 8+4+8=20 -> aligned 24; 24 % 16 == 8.
    // Before the fix this shifted every following command start to ≡8 (mod 16).
    Astra::Entity junk[2] = { Astra::Entity::Invalid(), Astra::Entity::Invalid() };
    cmd.DestroyEntities(std::span<const Astra::Entity>(junk, 2));

    Align16 a{}; a.v[0]=1; a.v[1]=2; a.v[2]=3; a.v[3]=4;
    cmd.AddComponent(victim, Align16(a));

    ASSERT_TRUE(cmd.Execute().IsOk());

    auto* got = reg.GetComponent<Align16>(victim);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->v[0], 1.0f);
    EXPECT_EQ(got->v[1], 2.0f);
    EXPECT_EQ(got->v[2], 3.0f);
    EXPECT_EQ(got->v[3], 4.0f);
}
```

- [ ] **Step 2: Regenerate solution, build Release, verify failure**

`--gtest_filter=CommandBufferAlignment.*` — Expected: FAIL (values shifted, e.g. `v[0] == 3`).

- [ ] **Step 3: Bump the command alignment and add the compile-time cap**

In `CommandBuffer.hpp`:

(a) `CommandByteBuffer`:
```cpp
        static constexpr size_t DEFAULT_INITIAL_CAPACITY = 4096;
        // Every command start is aligned to 16. std::vector<std::byte>'s
        // allocation is aligned to __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 on all
        // supported targets), so for payload alignment A <= 16 the reader's
        // absolute-address alignment and the writer's command-relative offset
        // computation are guaranteed to agree.
        static constexpr size_t ALIGNMENT = 16;
```

(b) In `AddComponent`, after `using DecayedT = std::decay_t<T>;`:
```cpp
            static_assert(alignof(DecayedT) <= CommandByteBuffer::ALIGNMENT,
                "CommandBuffer supports component alignment up to 16 bytes; "
                "add over-aligned components directly via Registry::AddComponent");
```

(c) Add the identical `static_assert` (same message, s/AddComponent/SetResource/ where apt) in `AddComponents` and `SetResource`, after their `using DecayedT = ...;` lines.

(d) In `Execute`, right after `std::byte* ptr = m_buffer.Data();`:
```cpp
            ASTRA_ASSERT((reinterpret_cast<uintptr_t>(ptr) % CommandByteBuffer::ALIGNMENT) == 0,
                         "Command buffer base must be 16-aligned");
```

No changes to `Command.hpp` readers are needed: with 16-aligned command starts, `GetDataPtr`'s absolute alignment equals the writer's relative offset for every legal alignment.

- [ ] **Step 4: Build Release, run `--gtest_filter=CommandBufferAlignment.*` — expect PASS. Full suite Debug + Release — expect 508/508.**

- [ ] **Step 5: Commit**

```bash
git add tests/Commands/CommandBufferAlignmentTest.cpp include/Astra/Commands/CommandBuffer.hpp ide/
git commit -m "fix(commands): 16-align command starts; reject over-aligned payloads at compile time

Writer computed payload offsets relative to the command start while readers
aligned the absolute address; any predecessor with alignedSize % 16 == 8
desynchronized them and corrupted alignas(16) component data."
```

### Task 6: Entity validity contracts (exhaustion, batch shortfalls, dead-handle resurrection)

Three confirmed holes: (a) `ArchetypeManager::AddEntity` accepts `Entity::Invalid()` into storage (visible in views while `Size()==0`); (b) `EntityManager::CreateBatch` can allocate fewer than requested but callers add the full span (garbage entities); (c) `Registry::AddComponents` skips the validity filter `EmplaceComponents` has, and `ArchetypeManager::AddComponents` adds unknown handles to the root archetype — resurrecting destroyed entities (confirmed).

**Files:**
- Create: `tests/Registry/EntityValidityTest.cpp`
- Modify: `include/Astra/Entity/EntityManager.hpp:67-96` (CreateBatch returns count)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:61-98` (AddEntity/AddEntityWith reject invalid), `:100-133` (AddEntities filter), `:250-323` (AddComponents — remove root-add branch and printfs)
- Modify: `include/Astra/Registry/Registry.hpp:87-134` (CreateEntity/CreateEntityWith guard), `:136-191` (CreateEntities/CreateEntitiesWith partial handling), `:297-318` (AddComponents filter)
- Modify: `include/Astra/Commands/CommandBuffer.hpp:198-225` (CreateEntities partial handling)

**Interfaces:**
- Consumes: `Entity::IsValid()`, `EntityManager::IsValid(Entity)`.
- Produces: `template<typename OutputIt> std::size_t EntityManager::CreateBatch(std::size_t count, OutputIt out) noexcept` — **returns the number actually created** (source-compatible: previous return was void). Contract: invalid entities never reach archetype storage; unfulfilled batch slots are set to `Entity::Invalid()`; unknown/dead handles passed to component APIs are skipped.

- [x] **Step 1: Write the failing tests**

Create `tests/Registry/EntityValidityTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct EPos { float x, y, z; }; }

TEST(EntityValidity, ArchetypeManagerRejectsInvalidEntity)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<EPos>();

    // This is exactly what Registry::CreateEntity does after an exhausted
    // EntityManager returns Entity::Invalid().
    reg.GetArchetypeManager()->AddEntity<EPos>(Astra::Entity::Invalid());

    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(Astra::Entity::Invalid()), nullptr);

    size_t seen = 0; bool sawInvalid = false;
    reg.CreateView<EPos>().ForEach([&](Astra::Entity e, EPos&) {
        ++seen;
        if (!e.IsValid() || !reg.IsValid(e)) sawInvalid = true;
    });
    EXPECT_EQ(seen, 0u);
    EXPECT_FALSE(sawInvalid);
}

TEST(EntityValidity, BatchAddComponentsIgnoresDeadHandles)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<EPos>();
    reg.DestroyEntity(e);
    ASSERT_FALSE(reg.IsValid(e));

    Astra::Entity arr[1] = { e };
    reg.AddComponents(std::span<Astra::Entity>(arr, 1), EPos{9, 9, 9});

    size_t seen = 0;
    reg.CreateView<EPos>().ForEach([&](Astra::Entity, EPos&) { ++seen; });
    EXPECT_EQ(seen, 0u);                        // dead handle must not resurrect
    EXPECT_FALSE(reg.HasComponent<EPos>(e));
}

TEST(EntityValidity, CreateBatchReportsCount)
{
    Astra::EntityManager em;
    std::vector<Astra::Entity> out(100);
    std::size_t created = em.CreateBatch(100, out.begin());
    EXPECT_EQ(created, 100u);
    for (auto& e : out) EXPECT_TRUE(e.IsValid());
}
```

- [x] **Step 2: Regenerate solution, build Release, verify failure**

`--gtest_filter=EntityValidity.*` — Expected: `ArchetypeManagerRejectsInvalidEntity` FAILS (record non-null, seen=1), `BatchAddComponentsIgnoresDeadHandles` FAILS (seen=1), `CreateBatchReportsCount` FAILS TO COMPILE? No — void return means `created` assignment fails compilation. **Comment out that test for this step**, confirm the other two fail, then restore it.

- [x] **Step 3: Make `EntityManager::CreateBatch` return the created count**

Replace the method in `EntityManager.hpp`:

```cpp
        // Creates up to `count` entities; returns how many were actually
        // created (can be < count when the ID space is exhausted). Only the
        // first `return-value` slots of the output are written.
        template<typename OutputIt>
        std::size_t CreateBatch(std::size_t count, OutputIt out) noexcept
        {
            if (count == 0) ASTRA_UNLIKELY
                return 0;

            // For small batches, simple loop is fine
            if (count < 32) ASTRA_LIKELY
            {
                std::size_t created = 0;
                for (std::size_t i = 0; i < count; ++i)
                {
                    Entity e = Create();
                    if (!e.IsValid()) ASTRA_UNLIKELY
                        break;
                    *out++ = e;
                    ++created;
                }
                return created;
            }

            // Large batch: allocate IDs in batch
            SmallVector<EntityIDStack::VersionedID, 256> allocations;
            allocations.resize(count);

            size_t allocated = m_idStack.AllocateBatch(count, allocations.begin());

            for (size_t i = 0; i < allocated; ++i)
            {
                auto [id, version] = allocations[i];
                m_table.SetVersion(id, version);
                *out++ = Entity(id, version);
            }
            return allocated;
        }
```

- [x] **Step 4: Reject invalid entities in ArchetypeManager**

(a) `AddEntity` — insert as the first statement (graceful rejection, NO assert — callers may legitimately probe after exhaustion):
```cpp
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return;
```
(b) `AddEntityWith` — same first statement.
(c) `AddEntities` — after the `count == 0` early-out, filter:
```cpp
            // Filter invalid handles (rare: only on ID exhaustion upstream).
            SmallVector<Entity, 256> validStorage;
            bool anyInvalid = false;
            for (Entity e : entities)
            {
                if (!e.IsValid()) { anyInvalid = true; break; }
            }
            if (anyInvalid) ASTRA_UNLIKELY
            {
                validStorage.reserve(entities.size());
                for (Entity e : entities)
                    if (e.IsValid()) validStorage.push_back(e);
                if (validStorage.empty())
                    return;
                entities = std::span<const Entity>(validStorage.data(), validStorage.size());
                count = entities.size();
            }
```
(d) `AddComponents` — replace the whole method body (this also deletes the root-archetype resurrection branch and all `printf` debug spam):
```cpp
        template<Component T, typename... Args>
        void AddComponents(std::span<Entity> entities, Args&&... args)
        {
            if (entities.empty())
                return;

            auto registry = m_componentRegistry.lock();
            if (!registry) ASTRA_UNLIKELY
                return;
            registry->RegisterComponent<T>();
            ComponentID componentID = TypeID<T>::Value();

            // Handles not present in the entity map (never added, or already
            // destroyed) are skipped by GroupEntitiesByArchetype: stale
            // handles must never re-enter storage.
            auto batches = GroupEntitiesByArchetype(entities,
                [componentID](Archetype* arch)
                {
                    return !arch->GetMask().Test(componentID);
                });

            for (auto& [srcArchetype, entityBatch] : batches)
            {
                if (entityBatch.empty())
                {
                    continue;
                }

                Archetype* dstArchetype = GetArchetypeWithAdded(srcArchetype, componentID);
                BatchMoveEntitiesWithComponent<T>(srcArchetype, dstArchetype, entityBatch, std::forward<Args>(args)...);
            }
        }
```

- [x] **Step 5: Guard the Registry creation paths**

(a) `CreateEntity` — after `Entity entity = m_entityManager.Create();`:
```cpp
            if (!entity.IsValid()) ASTRA_UNLIKELY
                return Entity::Invalid();
```
(b) `CreateEntityWith` — same guard in the same position.
(c) `CreateEntities` — replace the body's first half:
```cpp
            if (count == 0 || outEntities.size() < count)
                return;

            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();  // unfulfilled slots are explicit
            }
            if (created == 0) ASTRA_UNLIKELY
                return;

            m_archetypeManager->AddEntities<Components...>(outEntities.subspan(0, created));
```
and change both signal loops from `i < count` to `i < created`.
(d) `CreateEntitiesWith` — identical pattern (`created` from CreateBatch, Invalid-fill, `subspan(0, created)`, signal loops over `created`).
(e) `AddComponents` — replace with the filtering version (mirror of `EmplaceComponents`):
```cpp
        template<Component T>
        void AddComponents(std::span<Entity> entities, const T& component)
        {
            if (entities.empty())
                return;

            SmallVector<Entity, 256> validEntities;
            validEntities.reserve(entities.size());
            for (Entity entity : entities)
            {
                if (m_entityManager.IsValid(entity))
                {
                    validEntities.push_back(entity);
                }
            }
            if (validEntities.empty())
                return;

            m_archetypeManager->AddComponents<T>(validEntities, component);

            if (m_signalManager.IsSignalEnabled(Signal::ComponentAdded))
            {
                ComponentID componentId = TypeID<T>::Value();
                for (Entity entity : validEntities)
                {
                    T* comp = m_archetypeManager->GetComponent<T>(entity);
                    if (comp)
                    {
                        m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, comp);
                    }
                }
            }
        }
```

- [x] **Step 6: Fix `CommandBuffer::CreateEntities`**

Replace the allocation part of the method:
```cpp
            auto& manager = m_registry->GetEntityManager();
            size_t created = manager.CreateBatch(count, outEntities);
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0)
                return;

            // Track for potential rollback (only what was actually created)
            for (size_t i = 0; i < created; ++i)
            {
                m_allocatedEntities.push_back(outEntities[i]);
            }

            size_t totalSize = sizeof(CommandHeader) + sizeof(CreateEntitiesPayload) + created * sizeof(Entity);
            std::byte* ptr = m_buffer.Allocate(totalSize);

            auto* header = new (ptr) CommandHeader{CommandType::CreateEntities, 0, static_cast<uint32_t>(totalSize)};
            auto* payload = new (ptr + sizeof(CommandHeader)) CreateEntitiesPayload{static_cast<uint32_t>(created)};
            (void)header;

            Entity* entityDst = reinterpret_cast<Entity*>(payload + 1);
            std::memcpy(entityDst, outEntities, created * sizeof(Entity));

            m_commandCount++;
```

- [x] **Step 7: Restore the commented test; build Release; run `--gtest_filter=EntityValidity.*` — expect PASS. Full suite Debug + Release — expect 511/511.**

- [x] **Step 8: Commit**

```bash
git add tests/Registry/EntityValidityTest.cpp include/Astra/Entity/EntityManager.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp include/Astra/Commands/CommandBuffer.hpp ide/
git commit -m "fix(entity): invalid and stale handles can no longer enter component storage

AddEntity rejects Entity::Invalid; CreateBatch reports its actual count and
callers only insert the created prefix; batch AddComponents filters dead
handles instead of resurrecting them into the root archetype (also removes
the Debug printf spam in that path)."
```

### Task 7: Remove null-pointer paths for empty (tag) components

Empty components store `base == nullptr, stride == 0, isValid == true`. `Chunk::RemoveEntity`, `MoveEntitiesBetweenChunks`, `Archetype::MoveEntityFrom`, and `ArchetypeManager::MoveAndAdd(ByID)` guard only `isValid`, so they call `Destruct(nullptr)` / `MoveConstruct(nullptr, nullptr)` on tag columns (formal UB; dangerous if an empty type ever gets a non-trivial destructor). `ViewIterator` binds tag references at address null (confirmed: `&tag == 0`), unlike `ForEach` which substitutes a static instance.

**Files:**
- Create: `tests/Registry/EmptyTagTest.cpp`
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp:344-395` (RemoveEntity), `:215-261` (BatchMoveComponentsFrom)
- Modify: `include/Astra/Archetype/Archetype.hpp:438-471` (MoveEntityFrom), `:1275-1330` (MoveEntitiesBetweenChunks)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:1003-1044` (MoveAndAdd), `:1241-1293` (MoveAndAddByID)
- Modify: `include/Astra/Registry/ViewIterator.hpp:147-169` (CacheChunkState/MakeEntityTuple)

**Interfaces:**
- Consumes: `ComponentArrayInfo{base, stride, descriptor, isValid}` — empty components have `base == nullptr`.
- Produces: contract — **no lifecycle function is ever invoked with a null object pointer**; `ViewIterator` yields a reference to a per-type static instance for empty components (same behavior as `Archetype::ForEach`).

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/EmptyTagTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct TPos { float x, y, z; }; struct Tag {}; }

TEST(EmptyTag, RangeForBindsTagAtValidAddress)
{
    Astra::Registry reg;
    reg.CreateEntity<TPos, Tag>();

    auto view = reg.CreateView<TPos, Tag>();
    size_t n = 0;
    const Tag* tagAddr = nullptr;
    for (auto [e, pos, tag] : view)
    {
        ++n;
        tagAddr = &tag;
    }
    EXPECT_EQ(n, 1u);
    EXPECT_NE(tagAddr, nullptr);   // was nullptr before the fix
}

TEST(EmptyTag, MigrationAndRemovalPathsAreSafe)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<TPos, Tag>();
    auto b = reg.CreateEntity<TPos, Tag>();
    reg.DestroyEntity(a);                       // swap-and-pop with a Tag column
    EXPECT_TRUE(reg.RemoveComponent<Tag>(b));   // migration dropping the tag
    EXPECT_TRUE(reg.HasComponent<TPos>(b));
    EXPECT_FALSE(reg.HasComponent<Tag>(b));
    auto* p = reg.GetComponent<TPos>(b);
    ASSERT_NE(p, nullptr);
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`--gtest_filter=EmptyTag.*` — Expected: `RangeForBindsTagAtValidAddress` FAILS (`tagAddr == nullptr`). (`MigrationAndRemovalPathsAreSafe` passes today by luck — it pins the contract.)

- [ ] **Step 3: Guard every lifecycle callsite with `base == nullptr`**

(a) `ArchetypeChunkPool.hpp` `Chunk::RemoveEntity` — both loops currently read:
```cpp
                        const auto& info = m_componentArrays[id];
                        if (!info.isValid)
                        {
                            continue;
                        }
```
change both to:
```cpp
                        const auto& info = m_componentArrays[id];
                        if (!info.isValid || info.base == nullptr)
                        {
                            continue;  // empty (tag) component: no storage to touch
                        }
```
(b) `Chunk::BatchMoveComponentsFrom` — change
`if (!dstInfo.isValid || !srcInfo.isValid) continue;` to
`if (!dstInfo.isValid || !srcInfo.isValid || dstInfo.base == nullptr || srcInfo.base == nullptr) continue;`
(c) `Archetype::MoveEntityFrom` — inside the loop, after `const auto& dstInfo = dstArrays[id];` add:
```cpp
                if (dstInfo.base == nullptr) ASTRA_UNLIKELY
                {
                    continue;  // empty component: presence is carried by the mask
                }
```
(d) `Archetype::MoveEntitiesBetweenChunks` — change `if (!srcInfo.isValid)` to `if (!srcInfo.isValid || srcInfo.base == nullptr)`.
(e) `ArchetypeManager::MoveAndAdd` and `MoveAndAddByID` — in both loops, after `const auto& dstArrayInfo = dstChunk->m_componentArrays[dstComp.id];` add:
```cpp
                if (dstArrayInfo.base == nullptr) ASTRA_UNLIKELY
                {
                    continue;  // empty component: nothing to construct or move
                }
```

- [ ] **Step 4: Give ViewIterator the static-instance behavior**

In `ViewIterator.hpp`, inside `class Iterator` (private section), add a helper and replace `MakeEntityTuple`:

```cpp
            // Empty components have no storage (array pointer is nullptr);
            // hand out a shared static instance instead — same contract as
            // Archetype::ForEach.
            template<typename T>
            ASTRA_FORCEINLINE T& DerefComponent(std::remove_const_t<T>* array) const noexcept
            {
                if constexpr (std::is_empty_v<std::remove_const_t<T>>)
                {
                    static std::remove_const_t<T> s_emptyInstance{};
                    return s_emptyInstance;
                }
                else
                {
                    return array[m_entityIndex];
                }
            }

            template<size_t... Is>
            ASTRA_FORCEINLINE value_type MakeEntityTuple(std::index_sequence<Is...>) const noexcept
            {
                return value_type{
                    m_entities[m_entityIndex],
                    DerefComponent<Components>(std::get<Is>(m_componentArrays))...
                };
            }
```

- [ ] **Step 5: Build Debug, run `--gtest_filter=EmptyTag.*` — expect PASS. Full suite Debug + Release — expect 513/513.**

- [ ] **Step 6: Commit**

```bash
git add tests/Registry/EmptyTagTest.cpp include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/ViewIterator.hpp ide/
git commit -m "fix(archetype): never invoke lifecycle functions on null tag-component storage

RemoveEntity/move paths now skip base==nullptr columns; ViewIterator yields
a static instance for empty components like ForEach already did."
```

---

## Phase 2 — Contract & lifetime fixes

### Task 8: Emit ComponentRemoved signals before removal

`Registry::RemoveComponent`, `RemoveComponents`, and `RemoveComponentByID` capture a component pointer, migrate the entity (destroying/moving the pointee), then emit the signal with the dangling pointer. Fix: emit **before** removal while the pointer is valid, and document the lifetime.

**Files:**
- Create: `tests/Registry/SignalLifetimeTest.cpp`
- Modify: `include/Astra/Registry/Registry.hpp:280-295` (RemoveComponent), `:358-412` (RemoveComponents), `:506-544` (RemoveComponentByID)
- Modify: `include/Astra/Core/Signal.hpp:107-113` (ComponentRemoved doc comment)

**Interfaces:**
- Consumes: `SignalManager::Emit`, `ArchetypeManager::RemoveComponent(s)/RemoveComponentByID`.
- Produces: contract — **`Events::ComponentRemoved::component` points at the live component and is valid only for the duration of the handler invocation; removal happens after all handlers return.** Note: the signal now fires for components whose removal is about to happen; if the subsequent archetype move fails (allocation failure), the signal will have fired without a removal — document this as acceptable (exception-free design; failure here is already a broken world).

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/SignalLifetimeTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct SPos { float x, y, z; }; }

TEST(SignalLifetime, ComponentRemovedSeesLiveValue)
{
    Astra::Registry reg;
    reg.EnableSignals(Astra::Signal::ComponentRemoved);

    auto e = reg.CreateEntity<SPos>();
    reg.GetComponent<SPos>(e)->x = 42.0f;

    float observed = -1.0f;
    reg.GetSignalManager()->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& ev)
        {
            observed = static_cast<const SPos*>(ev.component)->x;
        });

    ASSERT_TRUE(reg.RemoveComponent<SPos>(e));
    EXPECT_EQ(observed, 42.0f);   // pointer must be alive at emit time
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`--gtest_filter=SignalLifetime.*` — Expected: FAIL or flaky garbage read (`observed != 42`). If it passes by luck in Debug, run it in Release too; keep the test either way — it pins the new ordering.

- [ ] **Step 3: Reorder the three removal paths**

(a) `RemoveComponent`:
```cpp
        template<Component T>
        bool RemoveComponent(Entity entity)
        {
            if (!m_entityManager.IsValid(entity))
                return false;

            T* component = m_archetypeManager->GetComponent<T>(entity);
            if (!component)
                return false;

            // Emit BEFORE removal: the pointer is only valid until the entity
            // migrates. Handlers must not retain it past their invocation.
            m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);

            return m_archetypeManager->RemoveComponent<T>(entity);
        }
```
(b) `RemoveComponents` — in the signal-enabled collection loop, emit inline instead of collecting pointers; delete `componentsToRemove` and the post-removal emit loop:
```cpp
            if (m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        T* component = m_archetypeManager->GetComponent<T>(entity);
                        if (component)
                        {
                            // Emit before removal — see RemoveComponent.
                            m_signalManager.Emit<Events::ComponentRemoved>(entity, TypeID<T>::Value(), component);
                            validEntities.push_back(entity);
                        }
                    }
                }
            }
            else
            {
                for (Entity entity : entities)
                {
                    if (m_entityManager.IsValid(entity))
                    {
                        validEntities.push_back(entity);
                    }
                }
            }

            if (validEntities.empty())
                return 0;

            return m_archetypeManager->RemoveComponents<T>(validEntities);
```
(c) `RemoveComponentByID` — move the existing pointer-lookup block's `Emit` to before `m_archetypeManager->RemoveComponentByID(entity, componentId);` and delete the "points to now-invalid memory" comment (it no longer does):
```cpp
            if (componentPtr && m_signalManager.IsSignalEnabled(Signal::ComponentRemoved))
            {
                // Emit BEFORE removal so the pointer is still valid.
                m_signalManager.Emit<Events::ComponentRemoved>(entity, componentId, componentPtr);
            }

            return m_archetypeManager->RemoveComponentByID(entity, componentId);
```

- [ ] **Step 4: Document the lifetime in Signal.hpp**

Above `struct ComponentRemoved`:
```cpp
        // Emitted immediately BEFORE the component is removed. `component`
        // points at the live component and is valid ONLY for the duration of
        // the handler invocation — never retain it.
```

- [ ] **Step 5: Build Debug + Release, run `--gtest_filter=SignalLifetime.*` then full suites — expect 514/514.**

- [ ] **Step 6: Commit**

```bash
git add tests/Registry/SignalLifetimeTest.cpp include/Astra/Registry/Registry.hpp include/Astra/Core/Signal.hpp ide/
git commit -m "fix(registry): emit ComponentRemoved before removal so the pointer is alive"
```

### Task 9: Fix Delegate large-functor storage UB

`Delegate.hpp:62` assigns a `shared_ptr` into uninitialized raw storage (`*reinterpret_cast<shared_ptr<T>*>(m_storage) = ...`) — the assignment releases a garbage "previous" control block. Must be placement-new.

**Files:**
- Create: `tests/Core/DelegateLargeFunctorTest.cpp`
- Modify: `include/Astra/Core/Delegate.hpp:48-66`

**Interfaces:**
- Consumes/Produces: none beyond `Delegate<R(Args...)>` — behavior fix only.

- [ ] **Step 1: Write the failing test**

Create `tests/Core/DelegateLargeFunctorTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Core/Delegate.hpp>

namespace
{
    static int g_alive = 0;
    struct BigFunctor
    {
        char payload[64];   // > SMALL_BUFFER_SIZE (32) => heap path
        int id;
        BigFunctor(int i) : payload{}, id(i) { ++g_alive; }
        BigFunctor(const BigFunctor& o) : id(o.id) { ++g_alive; }
        BigFunctor(BigFunctor&& o) noexcept : id(o.id) { ++g_alive; }
        ~BigFunctor() { --g_alive; }
        int operator()(int x) const { return x + id; }
    };
}

TEST(DelegateLargeFunctor, ConstructInvokeCopyDestroy)
{
    g_alive = 0;
    {
        Astra::Delegate<int(int)> d(BigFunctor{7});
        EXPECT_EQ(d(10), 17);

        Astra::Delegate<int(int)> copy(d);   // shared_ptr copy — same functor
        EXPECT_EQ(copy(1), 8);

        Astra::Delegate<int(int)> moved(std::move(d));
        EXPECT_EQ(moved(2), 9);
    }
    EXPECT_EQ(g_alive, 0);   // no leaks, no double-destroy
}
```

- [ ] **Step 2: Regenerate solution, build Debug, run `--gtest_filter=DelegateLargeFunctor.*`**

Expected: FAIL (crash/heap corruption from releasing a garbage control block) — or a lucky pass on zeroed stack. Either way proceed; the fix is unambiguous UB removal.

- [ ] **Step 3: Fix the construction**

In `Delegate.hpp`, in the templated constructor's `else` branch, replace:

```cpp
                // Use shared_ptr for large functors to enable safe copying
                auto* heapFunc = new DecayedFunc(std::forward<Func>(func));
                *reinterpret_cast<std::shared_ptr<DecayedFunc>*>(m_storage) = std::shared_ptr<DecayedFunc>(heapFunc);
```

with:

```cpp
                // Use shared_ptr for large functors to enable safe copying.
                // Placement-new: m_storage is raw bytes — assignment would
                // "release" a garbage control block.
                static_assert(sizeof(std::shared_ptr<DecayedFunc>) <= SMALL_BUFFER_SIZE,
                              "shared_ptr must fit the small buffer");
                new (m_storage) std::shared_ptr<DecayedFunc>(new DecayedFunc(std::forward<Func>(func)));
```

- [ ] **Step 4: Build Debug + Release, run the filter then full suites — expect 515/515.**

- [ ] **Step 5: Commit**

```bash
git add tests/Core/DelegateLargeFunctorTest.cpp include/Astra/Core/Delegate.hpp ide/
git commit -m "fix(core): placement-new shared_ptr into Delegate storage instead of assigning over garbage"
```

### Task 10: Preserve Registry configuration across Clear() and Load()

`Registry::Clear()` recreates the `ArchetypeManager` with a **default** chunk-pool config (custom `chunkSize` silently lost), and `LoadInternal` does the same even when the caller passes a `Config`. Worse: `Archetype::Deserialize` trusts the saved `entitiesPerChunk` — a save produced with a bigger `chunkSize` overflows default 16KB chunks. Fix: store the config, use it in both places, and validate saved layout against the actual pool.

**Files:**
- Create: `tests/Registry/ConfigPreservationTest.cpp`
- Modify: `include/Astra/Registry/Registry.hpp:49-83` (ctors store config), `:986-999` (Clear), `:1510-1558` (LoadInternal), member list `:1560-1567`
- Modify: `include/Astra/Archetype/Archetype.hpp:710-780` (Deserialize validation)

**Interfaces:**
- Consumes: `ArchetypeChunkPool::GetChunkSize()`, `ArchetypeManager::GetChunkPool()`.
- Produces: `Registry::m_config` (private member, type `Registry::Config`); contract — `Clear()` and `Load(..., config)` honor `config.chunkPoolConfig`; `Archetype::Deserialize` returns `SerializationError::SizeMismatch` when the saved chunk layout cannot fit the pool's chunk size.

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/ConfigPreservationTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct CPos { float x, y, z; }; }

TEST(ConfigPreservation, ClearKeepsChunkPoolConfig)
{
    Astra::Registry::Config cfg;
    cfg.chunkPoolConfig.chunkSize = 65536;
    Astra::Registry reg(cfg);
    ASSERT_EQ(reg.GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);

    reg.Clear();
    EXPECT_EQ(reg.GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);  // was 16384
}

TEST(ConfigPreservation, LoadHonorsChunkPoolConfig)
{
    Astra::Registry::Config cfg;
    cfg.chunkPoolConfig.chunkSize = 65536;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<CPos>();
    for (int i = 0; i < 10000; ++i)
        reg.CreateEntityWith(CPos{float(i), 0, 0});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<CPos>();

    // Same config: must load and preserve pool size.
    auto ok = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, cfg);
    ASSERT_TRUE(ok.IsOk());
    EXPECT_EQ((*ok.GetValue())->GetArchetypeManager()->GetChunkPool().GetChunkSize(), 65536u);
    EXPECT_EQ((*ok.GetValue())->Size(), 10000u);

    // Default (16KB) config: saved 64KB chunk layout cannot fit — must error,
    // not overflow.
    auto bad = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    EXPECT_TRUE(bad.IsErr());
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify**

`--gtest_filter=ConfigPreservation.*` — Expected: `ClearKeepsChunkPoolConfig` FAILS (16384). `LoadHonorsChunkPoolConfig` FAILS (pool is 16384 after load; the `bad` load may "succeed" or corrupt — the assert in Debug may fire; note whatever happens).

- [ ] **Step 3: Store the config in Registry**

(a) Add member after `m_workScheduler`:
```cpp
        Config m_config;   // retained so Clear()/Load() preserve pool + storage policy
```
(b) In each of the four constructors, add `m_config` to the init list — for the three `Config`-taking ctors: `, m_config(config)`. For the legacy two-arg ctor:
```cpp
        Registry(const EntityManager::Config& entityConfig, const ArchetypeChunkPool::Config& chunkConfig) :
            m_entityManager(entityConfig),
            m_componentRegistry(std::make_shared<ComponentRegistry>()),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, chunkConfig)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry),
            m_workScheduler(nullptr),
            m_config{entityConfig, chunkConfig, {}, nullptr}
        {}
```
(c) `Clear()`:
```cpp
            m_archetypeManager = std::make_shared<ArchetypeManager>(m_componentRegistry, m_config.chunkPoolConfig);
```
(d) `LoadInternal`:
```cpp
            registry->m_archetypeManager = std::make_shared<ArchetypeManager>(componentRegistry, config.chunkPoolConfig);
```

- [ ] **Step 4: Validate saved chunk layout in Archetype::Deserialize**

In `Archetype.hpp::Deserialize`, after the descriptor-reading loop completes and before `auto archetype = std::make_unique<Archetype>(mask);`, insert:

```cpp
            // Validate that the saved per-chunk layout fits the pool we will
            // allocate from — a save produced with a larger chunkSize must
            // fail cleanly instead of overflowing chunk memory.
            {
                size_t perEntitySize = 0;
                size_t nonEmptyComponents = 0;
                for (const auto& d : descriptors)
                {
                    if (d.size == 0) continue;
                    perEntitySize += d.size;
                    ++nonEmptyComponents;
                }
                size_t alignmentOverhead = nonEmptyComponents > 1
                    ? (nonEmptyComponents - 1) * CACHE_LINE_SIZE
                    : 0;
                size_t poolChunkSize = componentPool ? componentPool->GetChunkSize()
                                                     : ArchetypeChunkPool::DEFAULT_CHUNK_SIZE;
                if (entitiesPerChunk * perEntitySize + alignmentOverhead > poolChunkSize)
                {
                    return ResultType::Err(SerializationError::SizeMismatch);
                }
            }
```

- [ ] **Step 5: Build Debug + Release, run the filter then full suites — expect 517/517.**

- [ ] **Step 6: Commit**

```bash
git add tests/Registry/ConfigPreservationTest.cpp include/Astra/Registry/Registry.hpp include/Astra/Archetype/Archetype.hpp ide/
git commit -m "fix(registry): Clear() and Load() honor the configured chunk pool; reject unfit saved layouts"
```

### Task 11: ID-space capacity guards; systems stop consuming component IDs

Components, resources, systems, and every other `TypeID<T>::Value()` caller share one dense ID counter, and `Bitmap::Set` silently ignores indices ≥ `MAX_COMPONENTS` (128) — overflow silently corrupts query masks. Fixes: (a) `SystemScheduler` keys systems by **type hash** instead of consuming ComponentIDs; (b) `ComponentRegistry` refuses to register IDs ≥ `MAX_COMPONENTS` (observable: descriptor lookup fails, `AddComponent` returns nullptr); (c) `Bitmap::Set/Reset` assert in Debug.

**Files:**
- Create: `tests/Core/IdSpaceGuardTest.cpp`
- Modify: `include/Astra/System/SystemScheduler.hpp:50-139` (AddSystem/RemoveSystem/HasSystem/AddSystemInternal key type), `:435` (map type)
- Modify: `include/Astra/Component/ComponentRegistry.hpp:98-110` (registration guard)
- Modify: `include/Astra/Container/Bitmap.hpp:31-49` (asserts)

**Interfaces:**
- Consumes: `TypeID<T>::Hash()` (stable `uint64_t`).
- Produces: `SystemScheduler::m_systemIndices` becomes `FlatMap<uint64_t, size_t>`; `SystemMetadata::typeId` now stores the hash (still `size_t` — 64-bit on all supported targets). Contract: **registering a component whose assigned ID ≥ MAX_COMPONENTS is refused** (assert in Debug; in Release `GetComponentDescriptor(id)` returns nullptr and `Registry::AddComponent<T>` returns nullptr).

- [ ] **Step 1: Write the failing test**

Create `tests/Core/IdSpaceGuardTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct IdCompA { int v; }; struct IdCompB { int v; }; }

TEST(IdSpaceGuard, SystemsDoNotConsumeComponentIds)
{
    // Force IdCompA's ID to be assigned now.
    Astra::ComponentID before = Astra::TypeID<IdCompA>::Value();

    Astra::SystemScheduler scheduler;
    scheduler.AddSystem([](Astra::Entity, IdCompA& a) { a.v++; });   // lambda wrapper type
    // A second scheduler-side type for good measure:
    scheduler.AddSystem([](Astra::Entity, const IdCompA& a) { (void)a; });

    // If systems consumed ComponentIDs, IdCompB would now be before+3 or more.
    Astra::ComponentID after = Astra::TypeID<IdCompB>::Value();
    EXPECT_EQ(after, static_cast<Astra::ComponentID>(before + 1));
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`--gtest_filter=IdSpaceGuard.*` — Expected: FAIL (`after == before + 3` — the two lambda wrapper types consumed IDs). Note: test-registration order can shift absolute values, but `after - before` is deterministic within this test.

- [ ] **Step 3: Key systems by type hash**

In `SystemScheduler.hpp`:
- Change the member: `FlatMap<size_t, size_t> m_systemIndices;` → `FlatMap<uint64_t, size_t> m_systemIndices;  // key: TypeID<T>::Hash() — systems must not consume dense ComponentIDs`
- In `AddSystem<T>`: `size_t typeId = TypeID<T>::Value();` → `uint64_t typeId = TypeID<T>::Hash();`
- In `AddSystemInternal`: same replacement.
- In `RemoveSystem<T>`: `size_t typeId = TypeID<T>::Value();` → `uint64_t typeId = TypeID<T>::Hash();`
- In `HasSystem<T>`: `return m_systemIndices.Contains(TypeID<T>::Value());` → `return m_systemIndices.Contains(TypeID<T>::Hash());`
- The two `SystemMetadata` brace-inits assign `.typeId = typeId` — `typeId` is now `uint64_t`; change nothing else (`SystemMetadata::typeId` is `size_t`, 64-bit on supported targets; add `static_cast<size_t>(typeId)` to both initializers to be explicit).

- [ ] **Step 4: Guard registration and Bitmap**

(a) `ComponentRegistry.hpp::RegisterComponentImpl` — first statements:
```cpp
            ASTRA_ASSERT(id < MAX_COMPONENTS,
                         "Component ID space exhausted (MAX_COMPONENTS); raise ASTRA_MAX_COMPONENTS");
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                // Refuse registration so failure is observable (descriptor
                // lookup returns nullptr; Registry::AddComponent returns
                // nullptr) instead of silently corrupting ComponentMask bits.
                return;
            }
```
(b) `Bitmap.hpp` `Set` and `Reset` — add before the bounds branch in each:
```cpp
            ASTRA_ASSERT(index < Bits, "Bitmap index out of range (component ID space overflow?)");
```

- [ ] **Step 5: Build Debug + Release; run `--gtest_filter=IdSpaceGuard.*` — expect PASS; full suites — expect 518/518.**

- [ ] **Step 6: Commit**

```bash
git add tests/Core/IdSpaceGuardTest.cpp include/Astra/System/SystemScheduler.hpp include/Astra/Component/ComponentRegistry.hpp include/Astra/Container/Bitmap.hpp ide/
git commit -m "fix(core): systems keyed by type hash; guard component-ID overflow loudly"
```

---

## Phase 3 — Portability & archive format v2

### Task 12: Make non-32-bit entity configurations compile

`ASTRA_ENTITY_BITS 16` fails to compile: `Entity.hpp:59` (narrowing: `version << VERSION_SHIFT` promotes to `int` inside braced init) and `Command.hpp:115` (`static_assert(sizeof(Entity) == 4)`). Also `EntityTable`/`EntityManager` default `segmentSize = 65536` doesn't fit `uint16_t`. Add compile-check targets so this can't regress.

**Files:**
- Create: `tests/Compile/Entity16Main.cpp`, `tests/Compile/Entity64Main.cpp`
- Modify: `include/Astra/Entity/Entity.hpp:59`
- Modify: `include/Astra/Commands/Command.hpp:113-118`
- Modify: `include/Astra/Entity/EntityTable.hpp:33-48`, `include/Astra/Entity/EntityManager.hpp:29-35`
- Modify: `premake5.lua` (two compile-check projects)

**Interfaces:**
- Consumes: `EntityTraits<Bits, VersionBits>`.
- Produces: two new solution projects `AstraCompile16` / `AstraCompile64` that build (not run) in CI as part of the solution.

- [ ] **Step 1: Write the failing "test" (compile check TUs)**

Create `tests/Compile/Entity16Main.cpp`:
```cpp
// Compile check: 16-bit entity configuration (8-bit ID / 8-bit version).
#define ASTRA_ENTITY_BITS 16
#define ASTRA_ENTITY_VERSION_BITS 8
#include <Astra/Astra.hpp>

int main()
{
    Astra::Registry reg;
    struct P { float x; };
    auto e = reg.CreateEntity<P>();
    return reg.IsValid(e) ? 0 : 1;
}
```
Create `tests/Compile/Entity64Main.cpp`:
```cpp
// Compile check: 64-bit entity configuration (32-bit version).
#define ASTRA_ENTITY_BITS 64
#define ASTRA_ENTITY_VERSION_BITS 32
#include <Astra/Astra.hpp>

int main()
{
    Astra::Registry reg;
    struct P { float x; };
    auto e = reg.CreateEntity<P>();
    return reg.IsValid(e) ? 0 : 1;
}
```

- [ ] **Step 2: Add the premake projects**

In `premake5.lua`, inside `group "Astra"` after the `AstraBenchmark` project block, add (once per config — they share settings):

```lua
        -- Compile checks: alternate entity widths must keep building.
        local function astraCompileCheck(name, sourceFile)
            project(name)
                kind "ConsoleApp"
                language "C++"
                cppdialect "C++20"
                staticruntime "on"
                location "ide"
                targetdir ("bin/" .. outputdir .. "/%{prj.name}")
                objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
                files { sourceFile }
                includedirs { "%{IncludeDir.Astra}" }
                filter "system:windows"
                    systemversion "latest"
                    buildoptions { "/Zc:__cplusplus", "/arch:AVX", "/bigobj" }
                    defines { "__SSE2__", "__SSE4_2__" }
                    links { "advapi32" }
                filter "system:linux"
                    links { "pthread" }
                    buildoptions { "-mavx" }
                filter "configurations:Debug"
                    runtime "Debug"
                    symbols "on"
                    defines { "ASTRA_BUILD_DEBUG" }
                filter "configurations:Release or configurations:Dist"
                    runtime "Release"
                    optimize "speed"
                    defines { "NDEBUG" }
                filter {}
        end
        astraCompileCheck("AstraCompile16", "tests/Compile/Entity16Main.cpp")
        astraCompileCheck("AstraCompile64", "tests/Compile/Entity64Main.cpp")
```

- [ ] **Step 3: Regenerate solution, build Debug — verify the compile FAILS**

Expected errors: `Entity.hpp(59): error C2397: narrowing conversion` (16-bit) and `Command.hpp(115): static assertion failed: 'Entity must be 4 bytes'`.

- [ ] **Step 4: Fix the narrowing, the static_assert, and the segment defaults**

(a) `Entity.hpp:59` — replace the two-arg constructor:
```cpp
            constexpr BasicEntity(StorageType id, VersionType version) noexcept :
                m_entity{static_cast<StorageType>((static_cast<StorageType>(version) << VERSION_SHIFT) | (id & ID_MASK))}
            {}
```
(b) `Command.hpp` — inside `AddComponentPayload`, delete these two lines (the payload layout is computed with `sizeof`; nothing depends on a 4-byte Entity):
```cpp
        static_assert(sizeof(Entity) == 4, "Entity must be 4 bytes");
        static_assert(sizeof(ComponentID) == 2, "ComponentID must be 2 bytes");
```
Keep `static_assert(sizeof(ComponentDestructorFn) == 8, ...)`.
(c) `EntityTable.hpp` `Config` — make the default segment size width-safe. Replace the NSDMIs and constructor:
```cpp
        struct Config
        {
            static constexpr IDType DEFAULT_ENTITIES_PER_SEGMENT =
                static_cast<IDType>(std::min<uint64_t>(65536ull, static_cast<uint64_t>(Entity::ID_MASK) + 1ull));

            IDType entitiesPerSegment = DEFAULT_ENTITIES_PER_SEGMENT;
            IDType entitiesPerSegmentShift = static_cast<IDType>(std::countr_zero(DEFAULT_ENTITIES_PER_SEGMENT));
            IDType entitiesPerSegmentMask = static_cast<IDType>(DEFAULT_ENTITIES_PER_SEGMENT - 1);
            float releaseThreshold = 0.1f;
            bool autoRelease = true;
            size_t maxEmptySegments = 2;
            size_t maxPooledSegments = 4;
            bool useHugePages = true;

            Config(IDType segmentSize = DEFAULT_ENTITIES_PER_SEGMENT)
            {
                entitiesPerSegment = segmentSize > 0 ? std::bit_floor(segmentSize) : IDType(1);
                entitiesPerSegment = std::max(static_cast<IDType>(std::min<uint64_t>(1024, static_cast<uint64_t>(Entity::ID_MASK) + 1ull)), entitiesPerSegment);
                entitiesPerSegmentShift = static_cast<IDType>(std::countr_zero(entitiesPerSegment));
                entitiesPerSegmentMask = static_cast<IDType>(entitiesPerSegment - 1);
            }
        };
```
(d) `EntityManager.hpp` `Config` — replace the constructor default:
```cpp
            Config(IDType segmentSize = EntityTable::Config::DEFAULT_ENTITIES_PER_SEGMENT) :
                tableConfig(segmentSize)
            {}
```
(e) Chase any remaining compile errors surfaced by `AstraCompile16`/`AstraCompile64` builds using the same pattern (explicit `static_cast<IDType>` / `static_cast<StorageType>` on arithmetic that promotes to `int`). Fix only narrowing/static_assert issues; do not change semantics.

- [ ] **Step 5: Build all three configs — the compile-check projects must build. Run full suites Debug + Release (AstraTest unchanged: 518/518). Run `bin\Debug-windows-x86_64\AstraCompile16\AstraCompile16.exe` — exit 0.**

- [ ] **Step 6: Commit**

```bash
git add tests/Compile/ premake5.lua include/Astra/Entity/Entity.hpp include/Astra/Commands/Command.hpp include/Astra/Entity/EntityTable.hpp include/Astra/Entity/EntityManager.hpp ide/ Astra.sln
git commit -m "fix(entity): 16/64-bit entity configurations compile again, with compile-check targets"
```

### Task 13: Archive format v2 — portable checksum and fixed-width sizes

`Checksum::CRC32` is actually `HashCombine` (SSE4.2 CRC32C / ARM CRC32 / Murmur fallback) — checksums differ across ISAs, so archives aren't portable. Containers/strings serialize `size_t` (pointer-width dependent). Fix: bump `BINARY_FORMAT_VERSION` to 2; v2 uses an ISA-independent checksum and explicit `uint64_t` sizes; v1 archives still load (on 64-bit targets the size bytes are identical; v1 checksums verify via the legacy function).

**Files:**
- Create: `tests/Serialization/FormatV2Test.cpp`
- Modify: `include/Astra/Core/Simd.hpp:752-769` (add PortableHashCombine)
- Modify: `include/Astra/Serialization/BinaryArchive.hpp:21-58` (Checksum::Portable, version bump)
- Modify: `include/Astra/Serialization/BinaryWriter.hpp:131-173` (checksum call), `:283-433` (uint64 sizes)
- Modify: `include/Astra/Serialization/BinaryReader.hpp:96-176` (checksum switch), `:271-529` (uint64 sizes)
- Modify: `include/Astra/Archetype/Archetype.hpp:628-707` (Serialize), `:710-879` (Deserialize)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:689-812` (entity-count fields)
- Modify: `include/Astra/Entity/EntityManager.hpp:288-392` (maxEmptySegments field)

**Interfaces:**
- Consumes: `Simd::Ops` namespace, `BinaryHeader.version`.
- Produces: `uint32_t Checksum::Portable(const void* data, size_t size, uint32_t crc = 0)`; `uint64_t Simd::Ops::PortableHashCombine(uint64_t seed, uint64_t value)`; `BINARY_FORMAT_VERSION == 2`; `bool BinaryReader::m_usePortableChecksum`. Contract: writers always produce v2; readers accept v1 and v2. **On 64-bit platforms the on-disk bytes for sizes are unchanged** (size_t was already 8 bytes), so v1 archives from 64-bit builds load bit-identically.

- [ ] **Step 1: Write the failing tests**

Create `tests/Serialization/FormatV2Test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct FPos { float x, y, z; }; }

TEST(FormatV2, HeaderVersionIs2)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    reg.CreateEntityWith(FPos{1, 2, 3});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    const auto& bytes = *saved.GetValue();
    // BinaryHeader: magic[5], then uint16 version at offset 5 (packed struct).
    uint16_t version = 0;
    std::memcpy(&version, bytes.data() + 5, sizeof(version));
    EXPECT_EQ(version, 2);
}

TEST(FormatV2, ChecksumIsPortableFunctionAndDetectsCorruption)
{
    // Determinism of the portable checksum itself:
    const char data[] = "astra-format-v2-checksum";
    uint32_t a = Astra::Checksum::Portable(data, sizeof(data));
    uint32_t b = Astra::Checksum::Portable(data, sizeof(data));
    EXPECT_EQ(a, b);
    char tampered[sizeof(data)];
    std::memcpy(tampered, data, sizeof(data));
    tampered[3] ^= 0x1;
    EXPECT_NE(a, Astra::Checksum::Portable(tampered, sizeof(tampered)));

    // End-to-end: corrupting a payload byte must fail the checksum on load.
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    for (int i = 0; i < 100; ++i) reg.CreateEntityWith(FPos{float(i), 0, 0});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto bytes = *saved.GetValue();
    bytes[bytes.size() / 2] ^= std::byte{0x1};

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<FPos>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(bytes), creg);
    EXPECT_TRUE(loaded.IsErr());
}

TEST(FormatV2, RoundTripSurvives)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    for (int i = 0; i < 1000; ++i) reg.CreateEntityWith(FPos{float(i), float(i * 2), 0});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<FPos>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ((*loaded.GetValue())->Size(), 1000u);
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`--gtest_filter=FormatV2.*` — Expected: `HeaderVersionIs2` FAILS (version==1); `ChecksumIsPortable...` FAILS TO COMPILE (`Checksum::Portable` undefined) — comment that test out, confirm the version test fails, restore after Step 3.

- [ ] **Step 3: Add the portable hash and bump the version**

(a) `Simd.hpp`, directly below `HashCombine`:
```cpp
            // ISA-independent 64-bit mix (MurmurHash3 finalizer). Identical
            // output on every platform — used by archive-format v2 checksums.
            // HashCombine above is FASTER but hardware-dependent (CRC32C).
            ASTRA_FORCEINLINE uint64_t PortableHashCombine(uint64_t seed, uint64_t value) noexcept
            {
                uint64_t h = seed ^ value;
                h ^= h >> 33;
                h *= 0xff51afd7ed558ccdULL;
                h ^= h >> 33;
                h *= 0xc4ceb9fe1a85ec53ULL;
                h ^= h >> 33;
                return h;
            }
```
(b) `BinaryArchive.hpp` — in `namespace Checksum`, below `CRC32` add `Portable` (same chunking loop, portable mix):
```cpp
        // ISA-independent checksum for archive format v2+. CRC32 above is kept
        // ONLY to verify v1 archives written by the same ISA.
        inline uint32_t Portable(const void* data, size_t size, uint32_t crc = 0)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            uint64_t result = crc;

            while (size >= 8)
            {
                uint64_t value;
                std::memcpy(&value, bytes, sizeof(uint64_t));
                result = Simd::Ops::PortableHashCombine(result, value);
                bytes += 8;
                size -= 8;
            }
            if (size > 0)
            {
                uint64_t value = 0;
                std::memcpy(&value, bytes, size);
                result = Simd::Ops::PortableHashCombine(result, value);
            }
            return static_cast<uint32_t>(result);
        }
```
(c) `BinaryArchive.hpp`:
```cpp
    /**
     * Binary format version history:
     * v1: Initial format (ISA-dependent checksum; size_t container sizes)
     * v2: Portable checksum; explicit uint64 container sizes; resource block
     */
    inline constexpr uint16_t BINARY_FORMAT_VERSION = 2;
```

- [ ] **Step 4: Switch writer to Portable; reader selects by version**

(a) `BinaryWriter.hpp::WriteBytes` — replace `m_runningChecksum = Checksum::CRC32(data, size, m_runningChecksum);` with `m_runningChecksum = Checksum::Portable(data, size, m_runningChecksum);`
(b) `BinaryReader.hpp` — add member `bool m_usePortableChecksum = true;` next to `m_runningChecksum`. In `ReadHeader`, after `m_version = header.version;` add:
```cpp
            m_usePortableChecksum = (m_version >= 2);
```
In `ReadBytes`, replace the checksum update with:
```cpp
            if (m_checksumEnabled && m_position >= m_headerSize && m_headerSize > 0)
            {
                m_runningChecksum = m_usePortableChecksum
                    ? Checksum::Portable(data, size, m_runningChecksum)
                    : Checksum::CRC32(data, size, m_runningChecksum);   // v1 archives, same-ISA only
            }
```

- [ ] **Step 5: Fixed-width sizes (identical bytes on 64-bit targets)**

Apply mechanically — every `size_t` written/read as a length becomes `uint64_t`:
- `BinaryWriter.hpp`: in `operator()(const std::string&)`: `uint64_t len = str.size();`. In the vector/map/unordered_map/set/unordered_set overloads: `uint64_t size = <container>.size();`.
- `BinaryReader.hpp`: mirror each: `uint64_t len; (*this)(len);` / `uint64_t size; (*this)(size);` — keep all existing bounds checks, casting where needed (`static_cast<size_t>(len)`).
- `Archetype.hpp::Serialize`: `writer(m_entityCount)` → `writer(static_cast<uint64_t>(m_entityCount));`, `writer(m_entitiesPerChunk)` → `writer(static_cast<uint64_t>(m_entitiesPerChunk));`, `writer(desc.size)` → `writer(static_cast<uint64_t>(desc.size));`, `writer(desc.alignment)` → `writer(static_cast<uint64_t>(desc.alignment));`
- `Archetype.hpp::Deserialize`: `size_t entityCount; size_t entitiesPerChunk;` → `uint64_t entityCount; uint64_t entitiesPerChunk;` (then `static_cast<size_t>` at use sites), and in the descriptor loop `size_t size, alignment;` → `uint64_t size, alignment;`.
- `ArchetypeManager.hpp::Serialize`: `writer(entry.archetype->GetEntityCount());` → `writer(static_cast<uint64_t>(entry.archetype->GetEntityCount()));`; `Deserialize`: `size_t entityCount; reader(entityCount);` → `uint64_t entityCount; reader(entityCount);` (the per-archetype validation count).
- `EntityManager.hpp::Serialize`: `writer(m_config.tableConfig.maxEmptySegments);` → `writer(static_cast<uint64_t>(m_config.tableConfig.maxEmptySegments));`; `Deserialize`: read into `uint64_t` then `static_cast<size_t>`.

- [ ] **Step 6: Restore the commented test. Build Debug + Release; run `--gtest_filter=FormatV2.*` — expect PASS; full suites + existing `BinarySerializationTests`/`EntityManagerSerializationTest`/`RelationshipGraphSerializationTest` must all pass — expect 521/521.**

- [ ] **Step 7: Commit**

```bash
git add tests/Serialization/FormatV2Test.cpp include/Astra/Core/Simd.hpp include/Astra/Serialization/BinaryArchive.hpp include/Astra/Serialization/BinaryWriter.hpp include/Astra/Serialization/BinaryReader.hpp include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Entity/EntityManager.hpp ide/
git commit -m "feat(serialization): archive format v2 with ISA-portable checksum and fixed-width sizes

v1 archives still load: sizes are byte-identical on 64-bit targets and v1
checksums verify through the legacy hardware-dependent function."
```

### Task 14: Serialize resources (Save/Load round-trip)

`Registry::Save` writes entities, archetypes, and relationships but silently drops all resources (confirmed: `GetResource` is nullptr after Load). Persist them as part of format v2.

**Files:**
- Create: `tests/Registry/ResourcePersistenceTest.cpp`
- Modify: `include/Astra/Component/ResourceStorage.hpp` (add Serialize/Deserialize after `RemoveByID`)
- Modify: `include/Astra/Registry/Registry.hpp:1399-1430` and `:1434-1463` (both Save overloads), `:1510-1558` (LoadInternal)

**Interfaces:**
- Consumes: `ComponentDescriptor::{hash, serializeVersioned, deserializeVersioned, DefaultConstruct}`, `ComponentRegistry::GetComponentDescriptorByHash`, format v2 gate (`BinaryReader::GetVersion()`).
- Produces: `void ResourceStorage::Serialize(BinaryWriter&) const` and `bool ResourceStorage::Deserialize(BinaryReader&)`. Contract: resources round-trip through Save/Load when their type is registered in the target ComponentRegistry; unknown resource hashes fail the load with `UnknownComponent`. v1 archives (no resource block) load with zero resources.

- [ ] **Step 1: Write the failing tests**

Create `tests/Registry/ResourcePersistenceTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct GameCfg { int seed = 0; float speed = 1.0f; }; }

TEST(ResourcePersistence, RoundTripsThroughSaveLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    reg.SetResource(GameCfg{1234, 2.5f});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());

    auto* cfg = (*loaded.GetValue())->GetResource<GameCfg>();
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->seed, 1234);
    EXPECT_EQ(cfg->speed, 2.5f);
}

TEST(ResourcePersistence, UnknownResourceHashFailsLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    reg.SetResource(GameCfg{7, 1.0f});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    // Fresh registry that has never seen GameCfg:
    auto emptyReg = std::make_shared<Astra::ComponentRegistry>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), emptyReg);
    EXPECT_TRUE(loaded.IsErr());
}
```

- [ ] **Step 2: Regenerate solution, build Debug, verify failure**

`--gtest_filter=ResourcePersistence.*` — Expected: `RoundTripsThroughSaveLoad` FAILS (`cfg == nullptr`); `UnknownResourceHashFailsLoad` FAILS (load "succeeds" because nothing is read).

- [ ] **Step 3: Implement ResourceStorage::Serialize/Deserialize**

Append to `ResourceStorage` (public section, after `RemoveByID`):

```cpp
        // ====================== Serialization (archive v2+) ======================

        void Serialize(BinaryWriter& writer) const
        {
            uint32_t count = 0;
            for (const auto& slot : m_resources)
            {
                if (slot.isValid && slot.descriptor) ++count;
            }
            writer(count);

            for (const auto& slot : m_resources)
            {
                if (!slot.isValid || !slot.descriptor) continue;
                writer(slot.descriptor->hash);
                void* data = slot.isHeap
                    ? slot.storage.heapPtr
                    : const_cast<void*>(static_cast<const void*>(slot.storage.inlineData));
                slot.descriptor->serializeVersioned(writer, data);
            }
        }

        // Restores resources previously written by Serialize. Every stored
        // type must already be registered in the ComponentRegistry (looked up
        // by stable hash); returns false on unknown types or stream errors.
        bool Deserialize(BinaryReader& reader)
        {
            uint32_t count = 0;
            reader(count);
            if (reader.HasError()) return false;

            auto registry = m_componentRegistry.lock();
            if (!registry) return false;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t hash = 0;
                reader(hash);
                if (reader.HasError()) return false;

                const ComponentDescriptor* desc = registry->GetComponentDescriptorByHash(hash);
                if (!desc || desc->id >= MAX_COMPONENTS)
                    return false;   // caller maps this to UnknownComponent

                // Allocate a slot (same layout policy as SetByID)…
                uint16_t index = m_sparse[desc->id];
                void* dst = nullptr;
                if (index == INVALID_INDEX)
                {
                    index = static_cast<uint16_t>(m_resources.size());
                    if (index >= INVALID_INDEX) return false;
                    m_sparse[desc->id] = index;
                    m_resources.emplace_back();

                    auto& slot = m_resources[index];
                    slot.id = desc->id;
                    slot.size = static_cast<uint16_t>(desc->size);
                    slot.descriptor = desc;
                    slot.isValid = true;

                    if (desc->size <= SBO_SIZE)
                    {
                        slot.isHeap = false;
                        dst = slot.storage.inlineData;
                    }
                    else
                    {
                        slot.isHeap = true;
                        AllocResult result = AllocateMemory(desc->size, desc->alignment);
                        if (!result.ptr) return false;
                        slot.storage.heapPtr = result.ptr;
                        dst = result.ptr;
                    }
                    // …default-construct, then deserialize over it.
                    desc->DefaultConstruct(dst);
                }
                else
                {
                    auto& slot = m_resources[index];
                    dst = slot.isHeap ? slot.storage.heapPtr : slot.storage.inlineData;
                }

                if (!desc->deserializeVersioned(reader, dst))
                    return false;
            }
            return !reader.HasError();
        }
```

- [ ] **Step 4: Wire into Registry Save/Load**

(a) In BOTH `Save` overloads, after `m_relationshipGraph->Serialize(writer);` add:
```cpp
            m_resourceStorage.Serialize(writer);   // format v2 resource block
```
(b) In `LoadInternal`, after the RelationshipGraph block and BEFORE `VerifyChecksum`:
```cpp
            // Resource block exists from format v2 onward.
            if (reader.GetVersion() >= 2)
            {
                if (!registry->m_resourceStorage.Deserialize(reader))
                {
                    return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::UnknownComponent);
                }
            }
```
Note: `BinaryReader::GetVersion()` exists on `BinaryArchive`. `m_resourceStorage` is a private member and `LoadInternal` is a static member of Registry — access is fine.

- [ ] **Step 5: Build Debug + Release; run `--gtest_filter=ResourcePersistence.*` — expect PASS; full suites — expect 523/523.**

- [ ] **Step 6: Commit**

```bash
git add tests/Registry/ResourcePersistenceTest.cpp include/Astra/Component/ResourceStorage.hpp include/Astra/Registry/Registry.hpp ide/
git commit -m "feat(serialization): persist resources in archive format v2"
```

---

## Phase 4 — Hygiene & guardrails

### Task 15: Hygiene sweep (small confirmed defects, one commit)

Grouped because each is a few lines with a focused test or is untestable-but-unambiguous. All were identified in the review.

**Files:**
- Modify: `include/Astra/Serialization/Compression/LZ4Decoder.hpp:193-204` (dead double-decompress)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:1118-1173` (remaining printf debug spam)
- Modify: `include/Astra/Serialization/BinaryReader.hpp:667-679` (Peek file-mode + checksum contamination)
- Modify: `include/Astra/Component/ResourceStorage.hpp:90-121` (Get bounds check), `:629-645` (ResourceSlot move semantics)
- Modify: `include/Astra/Container/SmallVector.hpp:336-359` (insert-fill correctness)
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp:970-979` (block alignment 32→64)
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (alignment cap assert, next to the Task-11 guard)
- Modify: `include/Astra/Commands/CommandBuffer.hpp:146-176` (CreateEntity threading doc)
- Modify: `tests/Container/SmallVectorTest.cpp` (append test)

**Interfaces:**
- Consumes: existing APIs only.
- Produces: contract — supported component alignment is ≤ `CACHE_LINE_SIZE` (64), asserted at registration; `ResourceSlot` relocates via descriptor move-construct (non-trivially-relocatable inline resources are now safe).

- [ ] **Step 1: Write the failing SmallVector test (append to `tests/Container/SmallVectorTest.cpp`)**

```cpp
TEST(SmallVectorTest, InsertFillWithNonTrivialType)
{
    Astra::SmallVector<std::string, 4> v;
    v.push_back("a");
    v.push_back("b");
    v.push_back("c");
    // Insert 3 copies in the middle: exercises the uninitialized-tail path.
    v.insert(v.begin() + 1, 3, std::string("X"));
    ASSERT_EQ(v.size(), 6u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "X");
    EXPECT_EQ(v[2], "X");
    EXPECT_EQ(v[3], "X");
    EXPECT_EQ(v[4], "b");
    EXPECT_EQ(v[5], "c");
}
```
Build Debug, run `--gtest_filter=SmallVectorTest.InsertFillWithNonTrivialType`. Expected: FAIL or crash (move_backward into uninitialized memory).

- [ ] **Step 2: Fix `SmallVector::insert(pos, count, value)`**

Replace the method:

```cpp
        iterator insert(const_iterator pos, size_type count, const T& value)
        {
            size_type offset = pos - cbegin();

            if (count == 0)
                return begin() + offset;

            // Copy first: `value` may alias an element about to be shifted.
            T valueCopy(value);

            if (m_size + count > capacity())
                Grow(m_size + count);

            iterator it = begin() + offset;
            size_type tail = m_size - offset;

            if (count < tail)
            {
                // Move-construct the last `count` elements into raw space,
                // shift the rest with move-assignment, then assign the gap.
                std::uninitialized_move(end() - count, end(), end());
                std::move_backward(it, end() - count, end());
                std::fill_n(it, count, valueCopy);
            }
            else
            {
                // Whole tail lands in raw space beyond the current end.
                std::uninitialized_move(it, end(), it + count);
                std::fill_n(it, tail, valueCopy);
                std::uninitialized_fill_n(it + tail, count - tail, valueCopy);
            }

            m_size += count;
            return it;
        }
```
Run the filter — expect PASS.

- [ ] **Step 3: Remove the dead LZ4 double-decompress**

In `LZ4Decoder.hpp::DecompressFrame`, delete these lines (the discarded first call decompresses every block twice):
```cpp
                    // For compressed blocks, we need to know uncompressed size
                    // This is tricky - smallz4 doesn't store it per block
                    // We'll have to decompress and see
                    
                    // Try decompressing with a reasonable max size (e.g., 4MB)
                    const size_t maxBlockSize = 4 * 1024 * 1024;
                    auto result = Decompress(src, blockSize, maxBlockSize);
                    
                    // Since we don't know exact size, we need a different approach
                    // Let's decompress to a temporary buffer and stop when input consumed
```
keeping only the `DecompressBlock(src, blockSize)` call and its use.

Also delete the three remaining `#ifdef ASTRA_BUILD_DEBUG` printf blocks in `ArchetypeManager.hpp::BatchMoveEntitiesInternal` (around lines 1121-1124, 1158-1161, and 1166-1170: "Moving %zu entities...", "BatchMoveEntitiesFrom returned...", "ERROR: Failed to allocate chunks..."). Keep the `ASTRA_ASSERT(false, "Failed to allocate chunks for batch move operation")` — only the printfs go. (Task 6 already removed the copies in `AddComponents`.)

- [ ] **Step 4: Fix `BinaryReader::Peek`**

Replace:
```cpp
        template<typename T>
        requires std::is_trivially_copyable_v<T>
        T Peek()
        {
            T value{};
            size_t savedPos = m_position;
            uint32_t savedChecksum = m_runningChecksum;   // Peek must not contaminate the checksum
            (*this)(value);
            if (m_data.empty() && m_file.is_open())
            {
                // File mode: rewind the stream cursor too.
                m_file.seekg(static_cast<std::streamoff>(savedPos), std::ios::beg);
            }
            m_position = savedPos;
            m_runningChecksum = savedChecksum;
            return value;
        }
```

- [ ] **Step 5: ResourceStorage — bounds check + safe slot relocation**

(a) In both `Get<T>()` overloads, before `uint16_t index = m_sparse[id];`:
```cpp
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
                return nullptr;
```
(b) Give `ResourceSlot` explicit move operations (replace the implicit ones — the union was byte-copied, which is unsafe for non-trivially-relocatable inline resources):
```cpp
            ResourceSlot() : descriptor(nullptr), id(0), size(0), isHeap(false), isValid(false) {}

            ResourceSlot(ResourceSlot&& other) noexcept :
                descriptor(other.descriptor), id(other.id), size(other.size),
                isHeap(other.isHeap), isValid(other.isValid)
            {
                if (isValid && !isHeap && descriptor)
                {
                    descriptor->MoveConstruct(storage.inlineData, other.storage.inlineData);
                    descriptor->Destruct(other.storage.inlineData);
                }
                else
                {
                    storage.heapPtr = other.storage.heapPtr;
                }
                other.isValid = false;
                other.storage.heapPtr = nullptr;
            }

            ResourceSlot& operator=(ResourceSlot&& other) noexcept
            {
                if (this != &other)
                {
                    if (isValid && descriptor)
                    {
                        void* mine = isHeap ? storage.heapPtr : static_cast<void*>(storage.inlineData);
                        descriptor->Destruct(mine);
                        if (isHeap) FreeMemory(storage.heapPtr, size);
                    }
                    descriptor = other.descriptor; id = other.id; size = other.size;
                    isHeap = other.isHeap; isValid = other.isValid;
                    if (isValid && !isHeap && descriptor)
                    {
                        descriptor->MoveConstruct(storage.inlineData, other.storage.inlineData);
                        descriptor->Destruct(other.storage.inlineData);
                    }
                    else
                    {
                        storage.heapPtr = other.storage.heapPtr;
                    }
                    other.isValid = false;
                    other.storage.heapPtr = nullptr;
                }
                return *this;
            }
            ResourceSlot(const ResourceSlot&) = delete;
            ResourceSlot& operator=(const ResourceSlot&) = delete;
```

- [ ] **Step 6: Chunk block alignment + component alignment cap**

(a) `ArchetypeChunkPool.hpp::AllocateBlock` — replace:
```cpp
            // Ensure proper alignment for SIMD operations (32-byte for AVX)
            constexpr size_t SIMD_ALIGNMENT = 32;
```
with:
```cpp
            // Blocks are cache-line aligned; chunk bases inherit this, so the
            // strongest alignment a component array can rely on is
            // CACHE_LINE_SIZE (64). (Windows VirtualAlloc gives 64KB anyway;
            // this matters on the POSIX posix_memalign fallback.)
            constexpr size_t BLOCK_ALIGNMENT = CACHE_LINE_SIZE;
```
and change the `AllocateMemory(blockSize, SIMD_ALIGNMENT, flags)` call to use `BLOCK_ALIGNMENT`.
(b) `ComponentRegistry.hpp::RegisterComponentImpl` — next to the Task-11 ID guard:
```cpp
            ASTRA_ASSERT(desc.alignment <= CACHE_LINE_SIZE,
                         "Component alignment above 64 bytes is not supported by chunk storage");
```
(note: `desc.alignment` is set a few lines below the guard today — place this assert immediately after `desc.alignment` is assigned).

- [ ] **Step 7: Document the CommandBuffer entity-allocation threading restriction**

Above `CommandBuffer::CreateEntity`:
```cpp
        /**
         * Create a new entity. The entity ID is allocated from the Registry's
         * EntityManager IMMEDIATELY (at record time); only the archetype
         * insertion is deferred to Execute().
         *
         * THREADING: because allocation mutates shared Registry state at
         * record time, CreateEntity/CreateEntities must only be recorded from
         * the thread that owns the Registry — including when this buffer is a
         * ParallelCommandBuffer thread buffer. All other commands are safe to
         * record from worker threads.
         */
```
Mirror one sentence onto `ParallelCommandBuffer::GetThreadBuffer`'s existing comment: `// NOTE: CreateEntity/CreateEntities must not be recorded from worker threads (they allocate from the shared EntityManager at record time).`

- [ ] **Step 8: Build Debug + Release, full suites — expect 524/524. Also build Dist (headers changed — confirm the benchmark target still compiles).**

- [ ] **Step 9: Commit**

```bash
git add tests/Container/SmallVectorTest.cpp include/Astra/Serialization/Compression/LZ4Decoder.hpp include/Astra/Serialization/BinaryReader.hpp include/Astra/Component/ResourceStorage.hpp include/Astra/Container/SmallVector.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Component/ComponentRegistry.hpp include/Astra/Commands/CommandBuffer.hpp ide/
git commit -m "fix: hygiene sweep — SmallVector insert-fill, LZ4 double-decompress, Peek file mode, ResourceStorage relocation/bounds, 64B block alignment, cmdbuf threading docs"
```

### Task 16: CI hardening, version bump, and documentation

CI only ever built and tested Release — which is how the Debug abort and Dist-only issues stayed invisible. Add Debug testing and a Dist build to CI, bump the version, and document every behavior change.

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `include/Astra/Core/Version.hpp:3-5`
- Modify: `README.md` (Building, Threading Model, new Behavioral Contracts section)

**Interfaces:**
- Consumes: everything above.
- Produces: v3.4.0.

- [ ] **Step 1: Extend CI**

Replace the `windows-msvc` job's build/test steps with a config matrix, and add Dist:

```yaml
  windows-msvc:
    runs-on: windows-latest
    strategy:
      fail-fast: false
      matrix:
        config: [Debug, Release, Dist]
    steps:
      - uses: actions/checkout@v4
      - name: Download premake
        shell: pwsh
        run: |
          Invoke-WebRequest "https://github.com/premake/premake-core/releases/download/v${env:PREMAKE_VERSION}/premake-${env:PREMAKE_VERSION}-windows.zip" -OutFile premake.zip
          Expand-Archive premake.zip -DestinationPath .
      - name: Generate
        run: .\premake5.exe vs2022
      - name: Setup MSBuild
        uses: microsoft/setup-msbuild@v2
      - name: Build (${{ matrix.config }})
        run: msbuild Astra.sln /p:Configuration=${{ matrix.config }} /m /v:minimal
      - name: Test
        run: .\bin\${{ matrix.config }}-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1
```

In the `linux` job, add a Debug leg after the Release test:

```yaml
      - name: Build (Debug, tests)
        run: make config=debug AstraTest -j"$(nproc)" CC=${{ matrix.cc }} CXX=${{ matrix.compiler }}
      - name: Test (Debug)
        run: ./bin/Debug-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```

- [ ] **Step 2: Bump the version**

`include/Astra/Core/Version.hpp`:
```cpp
#define ASTRA_VERSION_MAJOR 3
#define ASTRA_VERSION_MINOR 4
#define ASTRA_VERSION_PATCH 0
```

- [ ] **Step 3: Document behavior changes in README.md**

Add a `## Behavioral Contracts (3.4)` section after "Core Concepts" containing exactly these points (wording may be polished, content must be complete):
- Default-constructed components are **value-initialized** in every build configuration (NSDMIs apply; trivially-default-constructible types are zeroed) — single and batch creation agree.
- `Events::ComponentRemoved` fires **before** removal; the component pointer is valid only during the handler.
- CommandBuffer payloads support component alignment up to 16 bytes (compile-time enforced); component storage supports alignment up to 64 bytes. Over-aligned components must use direct Registry APIs.
- `CommandBuffer::CreateEntity/CreateEntities` allocate at record time and must be recorded only from the Registry-owning thread; all other commands may be recorded from workers via `ParallelCommandBuffer`.
- Invalid entity handles (exhaustion) and destroyed handles never enter component storage; batch creation reports how many entities were actually created and marks unfulfilled slots `Entity::Invalid()`.
- Archives are format v2: ISA-portable checksums, fixed-width sizes, and **resources are persisted**. v1 archives load (64-bit producers; checksum verified only on the producing ISA family).
- Views may be cached across frames: they observe entities added to pre-existing archetypes and survive `Defragment()`.
- Recoverable inputs (cycles, self-links, invalid entities) are rejected gracefully in all configs — Debug no longer asserts on them.

Also update the README "Batch Operations" example comment `(Position + Velocity zeroed)` to `(value-initialized: NSDMIs apply, PODs zeroed)`.

- [ ] **Step 4: Full local gate**

Build and test Debug + Release + Dist locally (all three `AstraTest` runs green: 524/524), build `AstraCompile16`/`AstraCompile64`, and run the Dist benchmark binary for a spot-check (`--benchmark_filter=BM_IterateSingleComponent` — confirm no perf collapse vs. the review baseline: ~1.2ns/entity @ 10K; the Task-3 memset on single-entity create may show a small, acceptable regression in `BM_CreateEntities`, expect < 10%).

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml include/Astra/Core/Version.hpp README.md
git commit -m "chore: bump Astra to 3.4.0; CI tests Debug+Release+Dist; document 3.4 behavioral contracts"
```

---

## Deferred (explicitly out of scope for 3.4 — tracked, not planned)

- **ParallelCommandBuffer deferred entity allocation** (true thread-safe `CreateEntity` recording) — needs an entity-reservation redesign; documented restriction shipped in Task 15 instead.
- **Per-chunk `ComponentArrayInfo` memory overhead** (~19KB of descriptor copies per 16KB chunk) — performance/memory work, not correctness.
- **`Registry::CreateEntity` chunk-pool-exhaustion half-created entities** (alive in EntityManager, no archetype record) — Task 6 closes the invalid-handle leak; full rollback-on-pool-exhaustion needs a design decision (destroy the entity vs. grow the pool).
- **UBSan/ASan CI job** — valuable follow-up once gcc/clang Debug legs are green.

**Test-coverage follow-ups (from Task 2 audit):**
- `TestComponents.hpp`: `Inventory`, `Timer`, `Hierarchy`, `Metadata` component types are registered but referenced by zero tests (and `Static`/`Damage` by only one each) — candidates for removal or documentation as intentionally-reserved fixtures.
- `tests/Commands/CommandBufferTest.cpp`: `CommandBuffer::MergeFrom` and `ParallelCommandBuffer::MergeInto`/`Clear`/`GetThreadCount` are never called; no test drives an invalid/dead entity through a `CommandBuffer`-recorded `SetParent`/`AddLink`/`AddComponent`.
- `ComponentLifecycleTest.cpp`: `SideEffectsOrder` only checks event presence, never actual ordering (name/assertion mismatch); the whole file has no negative/rejection-path test.
- `ErrorRecoveryTest.cpp`: `PartialBatchCreationFailure`/`ComponentAdditionFailureRecovery` never actually induce the failure their names claim to test; `TrackedComponent::copyConstructorCalls`/`moveConstructorCalls` are incremented but never asserted on (write-only dead state).
- `ResourceExhaustionTest.cpp`: `ArchetypeProliferation` and `SimpleEntityLifecycle` have zero `EXPECT_`/`ASSERT_` calls; `MaxComponentTypes`/`EntityIDSpaceExhaustion` don't actually approach the limits their names claim.
- `SmallVectorTest.cpp`: no boundary/misuse coverage anywhere in the file (23 tests, all happy-path for bounds).
- `TypeContextTest.cpp`: no negative tests for ID-space exhaustion, the Debug-only hash-collision assert, or double-registration; `PendingRegistrationsDrainIntoInstalledContext` restores the process-global active `TypeContext` manually instead of via RAII, fragile-by-convention against a future `ASSERT_` added mid-test.
- `TypeIDTests.cpp`: `XXHashProducesExpectedValues` falls back to `EXPECT_NE(hash, 0u)` for the `"Hello"` case instead of a hardcoded reference value like its two sibling assertions. **Higher-priority**: `TypeIDTests.cpp` and `tests/Registry/ViewIteratorTest.cpp` each declare an identically-shaped anonymous-namespace `struct Position` linked into the same test binary; since `TypeID<T>::Hash()` hashes the compiler's pretty-printed type name (which doesn't disambiguate anonymous-namespace scope across TUs), these two distinct types may silently collide to the same `ComponentID` under the shared process-wide `DefaultTypeContext()` — recommend empirical verification (dump both hashes in a throwaway build); if confirmed this is a real Astra correctness bug, not just a test gap.
- `WorkSchedulerTest.cpp`: all `TEST()` suites are named `WorkerPool` rather than `WorkScheduler` (file/header-name mismatch); cosmetic rename, zero behavior change.
- `FieldVisitorTest.cpp`: thin relative to the `FieldInfo`/`IFieldVisitor` surface — missing nested reflected-struct recursion, container-typed fields, the typed `Get<T>`/`Set<T>`/`GetPtr<T>` accessors, a single dual-mode (`IsWriting()`-branching) visitor, `SetAny` rejection (negative path), and an absent-attribute query.
- `ReflectionTest.cpp`: SPLIT-recommended (TypeMeta/Field/Attribute/Enum/Lifecycle/ContainerTraits/JsonSchema/RegistryIntegration/MetaRegistry themes) but blocked — every theme shares `ASTRA_REFLECT_TYPE`/`ASTRA_REFLECT_ENUM` static registrations into the process-wide `MetaRegistry` singleton (`MetaRegistry.hpp:360 StaticTypeRegistrar`); duplicating them across files risks double-registration, and a shared header wouldn't avoid a non-inline static registrar instantiating per-TU. Needs a design for safely sharing/gating the registration before it can be split.
- `ArchetypeManagerTest.cpp`: `StressManyArchetypes` conditionally adds `Health` but never re-checks it; `ComponentRegistrySharing` never asserts the original manager is unaffected by mutating a second manager sharing its registry; `EdgeCaching` only checks archetype count, never component values; `ArchetypeChunkPool::Defragment` (chunk-pool level) is never exercised anywhere in the suite.
- `ArchetypeTest.cpp`: SPLIT-recommended (core mechanics, 21 tests, vs. serialization, 4 tests) but blocked — the shared ~32-line fixture (`ComponentRegistry`+`ArchetypeChunkPool`+`GetDescriptors`) exceeds the 20-line safe-duplication threshold and is used identically by all 25 tests; needs a shared-header (or similar) design before it can be split safely.
- `ParallelIterationTest.cpp`: no test calls `ParallelForEach`/`ParallelFor` nested inside another `ParallelFor` callback running on a worker thread — `TestWorkerPool`'s `t_insideWorker` reentrancy guard is never actually exercised.
- `RelationshipGraphSerializationTest.cpp`: all 10 tests use only the memory-target writer/reader; no file-target leg; no test drives the documented `Result::Err` path.
- `ResourceTest.cpp`: `MixedSizeResources` only re-verifies 2 of the 4 resource types it creates; `TagResourceZeroMemory`'s name implies more than its body checks; no `static_assert` locks the SBO/heap size boundary.
- `EntityManagerSerializationTest.cpp`: all 11 tests use only the memory-target writer/reader; no file-target leg; no test drives the documented `Result::Err` path.
- `EntityManagerTest.cpp`: `ValidateMethod` calls `pool.Validate()` 4 times with zero assertions, and `EntityManager::Validate()` is itself a Debug-only no-op in Release — the test currently verifies nothing in Release builds.
- `CompressionTests.cpp`: `GenerateRandomData` seeds `std::mt19937` from `std::random_device` (nondeterministic); `PerformanceBenchmark`'s NDEBUG-gated `compressMBps > 10.0f` / `decompressMBps > 100.0f` thresholds are still measured-machine-dependent — observed flaking twice (8.5-9.3 MB/s) across 4 Release runs during this task's own verification, correlated with unrelated system CPU load, not with any change made in this task.
- Public-API spot-check misses (no test anywhere in the suite exercises): `Registry::GetFragmentationLevel`, `CommandBuffer::MergeFrom`, `ParallelCommandBuffer::MergeInto`/`Clear`/`GetThreadCount`, `EntityManager::ShrinkToFit`, `ArchetypeChunkPool::Defragment`, JSON-schema generation for a reflected type with a nested reflected-type field.

## Verification summary (run after the final task)

1. `AstraTest` green in Debug, Release, Dist (524 tests).
2. `AstraCompile16`, `AstraCompile64` build in all configs.
3. Full Dist benchmark run; compare against the 2026-07-10 baseline (notably `BM_IterateSingleComponent` ~1.2ns/entity @ 10K, `BM_CreateEntities` within 10%).
4. The three review repros (`repro.cpp`, `repro2.cpp`, `repro3.cpp` from the review session scratchpad), rebuilt against the fixed headers, must print **0 failures**.
