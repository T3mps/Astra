# Theme B2 — Phase E+F: Sync-Point Barriers + Lambda Polish — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user insert an explicit `SyncPoint` fence (`AddSyncPoint()` / `AddSyncPoint(label)`) that partitions systems into ordered segments; deferred structural commands flush at each fence mid-run so a system after the fence sees, the same frame, entities a system before it deferred-created. Plus remove the vestigial pointer/optional lambda-parameter path via a `static_assert`.

**Architecture:** A `SyncPoint` is a **fence**: each system carries a `segmentIndex` (count of `SyncPoint`s registered before it). `ComputeScheduleOrder` excludes cross-segment ordering edges — and because `segmentIndex` is monotonic with registration index and the Kahn's ready-pick already selects the lowest index, the resulting order is automatically **segment-major** (all of segment k before any of segment k+1). The grouper breaks a run at a segment boundary so a group never spans a fence. `Execute` runs the plan **segment-by-segment**, flushing (`ExecuteSorted` + clear) between segments; the executor is unchanged. With no `SyncPoint` there is one segment = today's behavior, byte-identical. Phase F adds a `static_assert` in `LambdaSystemWrapper` rejecting non-reference component params.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-20-astra-theme-b2-phase-ef-design.md` (approved).

## Global Constraints

- Header-only C++20; MSVC-primary. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a shipping check is a real `if`, never `ASTRA_ASSERT`.
- **Additive, no break.** Existing `void(Registry&)` / `void(SystemContext&)` systems, view-lambdas, and all `SystemTraits<...>` compositions keep compiling and scheduling identically. **With no `SyncPoint`, behavior is byte-identical to `dev` @ `d7c9970`** (one segment = the whole schedule = one flush at end; `segmentIndex == 0` for every system). No wire-format or serialization change.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no new `System/` include.
- **TypeID ceiling:** the test binary sits near the `MAX_COMPONENTS = 128` ceiling. Reuse existing component types (`Position`/`Velocity`/`DTag`) in tests; do not register fresh distinct types. (See `[[astra-flatmap-pointer-stability]]`.)
- Namespace `Astra`. IDE/clang-tidy diagnostics are misconfigured false positives (Clang 20; "no gtest"; "Mosaic/Platform.hpp not found") — **judge only by the MSVC build.**
- All file:line anchors are from the tree at `dev` @ `d7c9970`; **confirm each against the live tree before editing** (Phase D shifted the file; find edit points by the named symbol/marker, not by absolute line).

**Build (per config, whole solution):**
`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`
Appending to the existing `tests/System/SystemSchedulerTest.cpp` needs **no** premake regen. Stale-PDB `mspdbsrv.exe` lock → `taskkill //F //IM mspdbsrv.exe` + rebuild. `CompressionTest.PerformanceBenchmark` is a known Release/Dist timing flake (a lone failure of only that test is not a regression; rerun isolated). Baseline entering Phase E+F: **640 Debug / 638 Release / 638 Dist**, all green.

## File Structure

- `include/Astra/System/SystemMetadata.hpp` — Task 1: `size_t segmentIndex` field.
- `include/Astra/System/SystemScheduler.hpp` — Task 1: `m_currentSegment`/`m_fenceLabels` members, `AddSyncPoint()`/`AddSyncPoint(label)`, `GetSegmentCount()`, `segmentIndex` stamping in the AddSystem paths, `Clear` reset, cross-segment edge exclusion in `ComputeScheduleOrder`, segment-boundary break + `m_planGroupSegment` in `BuildExecutionPlan`. Task 2: per-segment flush loop in `Execute`. Task 3: label diagnostic log.
- `include/Astra/System/System.hpp` — Task 4: `static_assert` in `LambdaSystemWrapper`.
- `tests/System/SystemSchedulerTest.cpp` — Tasks 1-3 append (existing file → no regen). Task 4: a positive regression test + a documented manual compile-fail check.

---

### Task 1: Segment model + partitioned plan

`AddSyncPoint()` partitions systems into ordered segments; a group never spans a fence; cross-segment ordering edges are ignored. Observable via `GetExecutionPlan()` grouping + `GetSegmentCount()`. (Mid-run flush is Task 2 — this task changes only the PLAN, not `Execute`.)

**Files:**
- Modify: `include/Astra/System/SystemMetadata.hpp` (after `scheduleOrder`)
- Modify: `include/Astra/System/SystemScheduler.hpp` (members ~:916-932; `AddSystem` metadata sites; `Clear` ~:375; `RemoveSystem` ~:194; `ComputeScheduleOrder` ~:553-566; `BuildExecutionPlan` grouping ~:744-780; add `AddSyncPoint`/`GetSegmentCount` near `AddSystem`/`GetExecutionPlan`)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Produces (consumed by Task 2/3): `SystemMetadata::segmentIndex` (`size_t`); `AddSyncPoint()` / `AddSyncPoint(std::string_view)`; `GetSegmentCount()` (`size_t`, = fences + 1); private `m_currentSegment` (`size_t`), `m_fenceLabels` (`std::vector<std::string>`), `m_planGroupSegment` (`mutable std::vector<size_t>`, parallel to `m_executionPlan`: segment index of each group).

- [ ] **Step 1: Write the failing tests.** Append to `tests/System/SystemSchedulerTest.cpp`. Reuse existing `Writes<Position>`/`Writes<Velocity>` system structs where possible; the point is that a fence forces a group split that masks alone would not:
```cpp
namespace  // Phase E segment systems (disjoint masks, no edges)
{
    struct SegPos : Astra::SystemTraits<Astra::Writes<Position>> { void operator()(Astra::Registry&) {} };
    struct SegVel : Astra::SystemTraits<Astra::Writes<Velocity>> { void operator()(Astra::Registry&) {} };
}

// A SyncPoint between two disjoint-mask systems forces them into separate
// segments/groups (masks alone would share one group).
TEST(SystemSchedulerSyncPoint, FenceSplitsDisjointSystemsIntoSegments)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<SegPos>().IsOk());   // segment 0
    ASSERT_TRUE(s.AddSyncPoint().IsOk());
    ASSERT_TRUE(s.AddSystem<SegVel>().IsOk());   // segment 1
    EXPECT_EQ(s.GetSegmentCount(), 2u);
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);                  // NOT one group of two
    EXPECT_EQ(plan[0][0], 0u);                   // SegPos first
    EXPECT_EQ(plan[1][0], 1u);                   // SegVel second
}

// Control: without the fence the same disjoint systems share one group and one segment.
TEST(SystemSchedulerSyncPoint, NoFenceKeepsOneSegment)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<SegPos>().IsOk());
    ASSERT_TRUE(s.AddSystem<SegVel>().IsOk());
    EXPECT_EQ(s.GetSegmentCount(), 1u);
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].size(), 2u);
}

// An empty segment (SyncPoint before any system, or two in a row) is harmless.
TEST(SystemSchedulerSyncPoint, EmptySegmentsAreHarmless)
{
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSyncPoint().IsOk());        // empty segment 0
    ASSERT_TRUE(s.AddSystem<SegPos>().IsOk());   // segment 1
    ASSERT_TRUE(s.AddSyncPoint().IsOk());
    ASSERT_TRUE(s.AddSyncPoint().IsOk());        // empty segment 2
    ASSERT_TRUE(s.AddSystem<SegVel>().IsOk());   // segment 3
    EXPECT_EQ(s.GetSegmentCount(), 4u);
    const auto& plan = s.GetExecutionPlan();
    ASSERT_EQ(plan.size(), 2u);                  // only the two non-empty segments contribute groups
    EXPECT_EQ(plan[0][0], 0u);
    EXPECT_EQ(plan[1][0], 1u);
}
```

- [ ] **Step 2: Run — verify RED.** Build Debug. Expected: **compile failure** — `AddSyncPoint` and `GetSegmentCount` do not exist.

- [ ] **Step 3: Add the `segmentIndex` field (`SystemMetadata.hpp`).** After `size_t scheduleOrder = 0;` (the last field, added by Phase D), still inside `struct SystemMetadata`, add:
```cpp
        // Sync-point segment this system belongs to (Phase E): the number of
        // AddSyncPoint() fences registered before it. Systems never reorder or
        // group across a segment boundary; deferred commands flush at each fence.
        // 0 for every system when no SyncPoint is used (byte-identical to Phase D).
        size_t segmentIndex = 0;
```

- [ ] **Step 4: Add the scheduler members (`SystemScheduler.hpp`).** Beside `m_executionPlan`/`m_needsRebuild` (~:921-922) add:
```cpp
        size_t m_currentSegment = 0;                 // segment stamped onto systems registered now (Phase E)
        std::vector<std::string> m_fenceLabels;      // label of each fence i (between segment i and i+1); "" if unlabeled
        mutable std::vector<size_t> m_planGroupSegment;  // segment index of each group in m_executionPlan (parallel)
```
Add `#include <string>` near the top if not already present (Phase D added it for the cycle log — confirm).

- [ ] **Step 5: Add `AddSyncPoint` overloads + `GetSegmentCount` (`SystemScheduler.hpp`).** Near the `AddSystem` overloads (public), add:
```cpp
        // Insert a sync-point fence at the current end of the registration
        // sequence (Phase E). Systems registered after it are in a later segment
        // and never reorder/group across it; all deferred structural commands
        // recorded before the fence are applied before the next segment runs.
        ASTRA_NODISCARD Result<void, SystemError> AddSyncPoint()
        {
            return AddSyncPoint(std::string_view{});
        }

        ASTRA_NODISCARD Result<void, SystemError> AddSyncPoint(std::string_view label)
        {
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);
            ++m_currentSegment;
            m_fenceLabels.emplace_back(label);   // owns a copy; empty for the no-arg form
            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }

        // Number of segments = number of SyncPoint fences + 1.
        ASTRA_NODISCARD size_t GetSegmentCount() const noexcept { return m_currentSegment + 1; }
```

- [ ] **Step 6: Stamp `segmentIndex` at registration (`SystemScheduler.hpp`).** Every `SystemMetadata{...}` construction site in the registration paths must set `segmentIndex = m_currentSegment`. There are FOUR sites (the `System T` `AddSystem`, the `ContextSystem T` `AddSystem`, and the two inside `AddSystemInternal`/`AddContextSystemInternal` used by the lambda/context paths — grep `SystemMetadata metadata` / `.insertionOrder = index`). To each designated-initializer, append `, .segmentIndex = m_currentSegment` as the LAST designated field (it is the last struct field, after `scheduleOrder`; the Phase C/D fields between `requiresExclusive` and it stay defaulted/skipped, which is legal):
```cpp
            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = static_cast<size_t>(typeId),
                .insertionOrder = index,
                .requiresExclusive = false,
                .segmentIndex = m_currentSegment
            };
```
(Confirm each of the 4 sites now lists `.segmentIndex = m_currentSegment`.)

- [ ] **Step 7: Reset fence state in `Clear` (`SystemScheduler.hpp` ~:375).** In `Clear()`, alongside `m_systems.clear()` etc., add:
```cpp
            m_currentSegment = 0;
            m_fenceLabels.clear();
            m_planGroupSegment.clear();
```
`RemoveSystem` needs **no** `segmentIndex` change: removing a system does not remove a fence, and each surviving system keeps its `segmentIndex`. (Confirm `RemoveSystem`'s existing `insertionOrder = idx` renumber loop does NOT touch `segmentIndex`.)

- [ ] **Step 8: Exclude cross-segment edges in `ComputeScheduleOrder` (`SystemScheduler.hpp` ~:553-566).** In BOTH edge loops, skip an edge whose endpoints are in different segments (the fence already fixes cross-segment order; only intra-segment edges reorder). Change the two `if (t != UNKNOWN && t != s)` guards to also require same-segment:
```cpp
                for (uint64_t h : md.afterIds)   // After<T> on s: T runs before s => T -> s
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s
                        && m_systems[t].metadata.segmentIndex == md.segmentIndex)
                    { succ[t].push_back(s); ++indeg[s]; }
                }
                for (uint64_t h : md.beforeIds)  // Before<T> on s: s runs before T => s -> T
                {
                    size_t t = resolve(h);
                    if (t != UNKNOWN && t != s
                        && m_systems[t].metadata.segmentIndex == md.segmentIndex)
                    { succ[s].push_back(t); ++indeg[t]; }
                }
```
No other change to `ComputeScheduleOrder` is needed: the ready-pick (`for k: lowest !placed && indeg==0`) and the cycle-break (`lowest !placed`) already select the lowest index, and `segmentIndex` is monotonic with index, so the emitted order is automatically segment-major (all of segment k before any of k+1). `scheduleOrder = position in order` is therefore segment-major too.

- [ ] **Step 9: Segment-boundary break + record group segments in `BuildExecutionPlan` (`SystemScheduler.hpp` ~:707-780).** At the top of `BuildExecutionPlan`, clear the new parallel vector next to `m_executionPlan.clear();`:
```cpp
            m_executionPlan.clear();
            m_planGroupSegment.clear();
```
In the inner grouping `for (; acceptsMore && q < order.size(); ++q)` loop, add a segment-boundary break as the FIRST break condition (before the Exclusive/no-trait check), so a group never spans a fence:
```cpp
                    const size_t jIdx = order[q];
                    const auto& sysJ = m_systems[jIdx].metadata;

                    if (sysJ.segmentIndex != sysI.segmentIndex)  // never group across a fence
                        break;
                    // (existing Exclusive/no-trait, mask-conflict, edge-barrier checks follow unchanged)
```
Where each group is pushed (`m_executionPlan.push_back(std::move(group)); p = q;`), record its segment first (the opener's, which every member shares):
```cpp
                m_planGroupSegment.push_back(sysI.segmentIndex);
                m_executionPlan.push_back(std::move(group));
                p = q;
```
(Leave the cycle-log block and the `if (m_reportAmbiguities) ReportAmbiguities();` call untouched.)

- [ ] **Step 10: Run — verify GREEN.** Build all 3 configs. Run `AstraTest.exe --gtest_filter=SystemSchedulerSyncPoint.*` (3 tests pass) each config. Confirm the pre-existing `SystemScheduler*`/`SystemSchedulerResourceConflict*`/`SystemSchedulerOrdering*` tests still pass (no-syncpoint schedules are `segmentIndex==0` everywhere → the `!= segmentIndex` break never fires, the edge filter never excludes anything → grouping + order byte-identical to Phase D). `git grep -n 'Commands/\|System/' include/Astra/Registry/Registry.hpp` shows nothing new. Full suite green (640/638/638 + 3 → 643/641/641).

- [ ] **Step 11: Commit.**
```bash
git add include/Astra/System/SystemMetadata.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): AddSyncPoint fence partitions systems into ordered scheduling segments"
```

---

### Task 2: Mid-run flush + spawn-then-process

`Execute` runs the plan segment-by-segment, flushing deferred commands between segments, so a segment-N+1 system sees entities segment-N deferred-created the same frame.

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (`Execute` ~:280-338)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `m_planGroupSegment`, `m_executionPlan`, `GetSegmentCount()`; the existing `m_commandBuffer->ExecuteSorted()` / `GetDeferredErrors()` / `Clear()`; `SystemExecutionContext`.
- Produces: no new API — the semantics change (deferred commands flush at each segment boundary, in schedule order within a segment).

- [ ] **Step 1: Write the failing test.** Append. A segment-0 context system defers creating entities (with `Position`); a segment-1 `void(Registry&)` system counts `Position` entities into a namespace-scope counter — it sees the created entities ONLY if the mid-run flush applied them at the fence:
```cpp
namespace  // Phase E spawn-then-process
{
    inline int g_syncSeenCount = 0;

    struct SpawnThree : Astra::SystemTraits<Astra::Exclusive>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            for (int i = 0; i < 3; ++i)
            {
                auto e = ctx.Commands().CreateEntity();       // deferred create (placeholder)
                ctx.Commands().AddComponent<Position>(e, Position{});
            }
        }
    };
    struct CountPositions : Astra::SystemTraits<Astra::Exclusive>
    {
        void operator()(Astra::Registry& reg)
        {
            g_syncSeenCount = 0;
            auto view = reg.CreateView<Position>();           // mirror the file's existing view API spelling
            view.ForEach([](Astra::Entity, Position&) { ++g_syncSeenCount; });
        }
    };
}

// With a SyncPoint between them, the spawner's deferred creates apply at the
// fence, so the counter (in segment 1) sees all 3 the SAME frame.
TEST(SystemSchedulerSyncPoint, DeferredSpawnsVisibleAfterFenceSameFrame)
{
    Astra::Registry reg;
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<SpawnThree>().IsOk());     // segment 0
    ASSERT_TRUE(s.AddSyncPoint("post-spawn").IsOk());
    ASSERT_TRUE(s.AddSystem<CountPositions>().IsOk()); // segment 1
    g_syncSeenCount = -1;
    s.Execute(reg);
    EXPECT_EQ(g_syncSeenCount, 3);                     // counter saw the 3 spawned entities
}
```
> `CreateEntity`/`AddComponent`/`CreateView`/`ForEach` are the standard `CommandBuffer`/`Registry`/`View` APIs used throughout the existing System tests — mirror their exact spelling from a neighboring deferred-command/view test if unsure. Both systems are `Exclusive` to keep the test focused on flush timing (each runs solo); the fence is what makes the creates visible mid-frame, not the grouping.

- [ ] **Step 2: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerSyncPoint.DeferredSpawnsVisibleAfterFenceSameFrame`. Expected: **FAIL — `g_syncSeenCount` is 0.** (Task 1 partitions the plan into segments, but `Execute` still runs all groups in one pass and flushes once at the end, so `CountPositions` runs BEFORE the deferred creates are applied → counts 0.)

- [ ] **Step 3: Rewrite `Execute`'s run+flush to loop over segments (`SystemScheduler.hpp` ~:281-338).** Replace the single `executor->Execute(context)` + single end-flush with a per-segment loop. Build the context ONCE (systems/contextSystems/metadata/registry/commandBuffer as today), then loop: for each contiguous run of groups sharing a segment, set `context.parallelGroups` to that segment's group slice, run the executor, then flush + accumulate errors + clear. **The `ExecutionGuard` wraps the whole loop** (it prevents reentrant `Execute` throughout; each segment's flush happens after that segment's `executor->Execute` returns, i.e. after all its systems completed — no worker is recording, so flushing at `depth==1` is safe). Concretely:
```cpp
            {
                ExecutionGuard guard(m_executionDepth);

                // Build the shared context once (systems/metadata do not change
                // between segments; only which groups run does).
                SystemExecutionContext context;
                context.registry = &registry;
                context.commandBuffer = m_commandBuffer.get();
                context.systems.reserve(m_systems.size());
                context.contextSystems.reserve(m_systems.size());
                context.metadata.reserve(m_systems.size());
                for (const auto& entry : m_systems)
                {
                    context.systems.push_back(entry.execute);
                    context.contextSystems.push_back(entry.executeContext);
                    context.metadata.push_back(entry.metadata);
                }

                // Run the plan segment-by-segment. Groups are emitted in
                // segment-major order (Task 1), so groups sharing a segment are
                // contiguous in m_executionPlan; m_planGroupSegment[g] is group
                // g's segment. Flush all deferred structural commands at each
                // segment boundary so the next segment sees them this frame.
                size_t g = 0;
                while (g < m_executionPlan.size())
                {
                    const size_t seg = m_planGroupSegment[g];
                    size_t gEnd = g;
                    while (gEnd < m_executionPlan.size() && m_planGroupSegment[gEnd] == seg)
                        ++gEnd;

                    context.parallelGroups.assign(m_executionPlan.begin() + g,
                                                  m_executionPlan.begin() + gEnd);
                    executor->Execute(context);

                    // Segment boundary = a sync point: flush this segment's
                    // deferred commands (schedule-order within the segment),
                    // surface errors, and start the next segment from empty.
                    auto flushResult = m_commandBuffer->ExecuteSorted();
                    (void)flushResult;
                    const auto& segErrors = m_commandBuffer->GetDeferredErrors();
                    m_lastDeferredErrors.insert(m_lastDeferredErrors.end(),
                                                segErrors.begin(), segErrors.end());
                    m_commandBuffer->Clear();

                    g = gEnd;
                }

                // If the plan is empty (no systems, or only empty segments),
                // there is nothing to run or flush -- matches the old
                // early-out behavior for an empty schedule.
            }
```
Remove the old post-guard single flush block (the `ExecuteSorted()` / `m_lastDeferredErrors = GetDeferredErrors()` / `m_commandBuffer->Clear()` at ~:320-338) — it is now inside the loop. `m_lastDeferredErrors.clear()` at the START of `Execute` (~:245) stays; the loop ACCUMULATES per-segment errors into it (append, not assign).
> **Also update the now-stale comment** (~:282-287) that says the `ExecutionGuard` is scoped so `m_executionDepth` returns to 0 *before* the flush: the flushes now happen INSIDE the guard scope (at `depth==1`), which is safe because each segment's `executor->Execute` has returned (all that segment's systems completed, no worker recording) before its flush. Reword it to describe the segment-loop flush (each segment boundary is a sync point; the final segment's flush is still the last one before `Execute` returns). Do not "recover" the old depth==0 wording.
> **Grounding check (do before writing):** confirm `ParallelCommandBuffer`'s per-flush semantics — that after `ExecuteSorted()`, `GetDeferredErrors()` returns THIS flush's errors, and `Clear()` resets them so the next segment's `GetDeferredErrors()` does not re-return prior-segment errors (else the accumulation double-counts). If `Clear()` does not reset the error list, grab `GetDeferredErrors()` into a local copy before `Clear()` and rely on `ExecuteSorted()` starting each flush fresh. State what you found in the report.

- [ ] **Step 4: Run — verify GREEN.** Build all 3 configs. The spawn-then-process test passes (`g_syncSeenCount == 3`). **Critically, confirm the entire Phase A–D suite stays green** — with no `SyncPoint` there is one segment, so the loop runs once and flushes once, exactly as before. Run `--gtest_filter=*Deferred*:*Determinism*:*Parallel*:*SystemSchedulerOrdering*:SystemSchedulerSyncPoint.*`. Full suite green (→ 644/642/642).

- [ ] **Step 5: Commit.**
```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): flush deferred commands at each SyncPoint segment boundary (spawn-then-process)"
```

---

### Task 3: Label diagnostic + determinism/backward-compat gate

Surface a labeled fence's name in a Debug-level flush log (zero production cost), and pin determinism of a barriered deferred schedule.

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (`Execute` segment loop — emit the label log)
- Test: `tests/System/SystemSchedulerTest.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `m_fenceLabels`; Task 2's segment loop; `ASTRA_LOG_DEBUG` / the `Astra::Testing::ScopedLogSink` test guard.

- [ ] **Step 1: Write the failing tests.** Append. Use the log-sink capture pattern from `tests/Core/LogTest.cpp` (include `../Support/DiagnosticsTestGuards.hpp`); the label line is Debug level, so the test raises the log level to Debug/Trace and restores it:
```cpp
namespace  // Phase E label + determinism
{
    struct LblSpawn : Astra::SystemTraits<Astra::Exclusive>
    {
        void operator()(Astra::SystemContext& ctx)
        {
            auto e = ctx.Commands().CreateEntity();
            ctx.Commands().AddComponent<Position>(e, Position{});
        }
    };
    struct LblNoop : Astra::SystemTraits<Astra::Exclusive> { void operator()(Astra::Registry&) {} };

    struct LblCapture { int debugWithLabel = 0; };
    inline void LblSink(const Astra::LogRecord& r, void* user) noexcept
    {
        if (r.level == Astra::LogLevel::Debug &&
            std::string(r.message).find("post-spawn") != std::string::npos)
            static_cast<LblCapture*>(user)->debugWithLabel++;
    }
}

// A labeled SyncPoint emits its label in a Debug-level flush log line.
TEST(SystemSchedulerSyncPoint, LabeledFenceEmitsDebugLogLine)
{
    LblCapture cap;
    Astra::Testing::ScopedLogSink guard(&LblSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Debug);   // Debug lines are off by default
    Astra::Registry reg;
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<LblSpawn>().IsOk());
    ASSERT_TRUE(s.AddSyncPoint("post-spawn").IsOk());
    ASSERT_TRUE(s.AddSystem<LblNoop>().IsOk());
    s.Execute(reg);
    Astra::SetLogLevel(Astra::LogLevel::Info);     // restore documented default
    EXPECT_GE(cap.debugWithLabel, 1);
}

// A barriered schedule that defers structural changes is deterministic across runs.
TEST(SystemSchedulerSyncPoint, BarrieredDeferredScheduleIsDeterministic)
{
    auto run = []{
        Astra::Registry reg;
        Astra::SystemScheduler s;
        (void)s.AddSystem<LblSpawn>();       // segment 0 defers a create
        (void)s.AddSyncPoint();
        (void)s.AddSystem<LblNoop>();        // segment 1
        s.Execute(reg);
        // observable: final Position-entity count is stable
        int n = 0;
        auto v = reg.CreateView<Position>();
        v.ForEach([&](Astra::Entity, Position&){ ++n; });
        return n;
    };
    const int oracle = run();
    EXPECT_EQ(oracle, 1);
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(run(), oracle);
}
```

- [ ] **Step 2: Run — verify RED.** Build + run `--gtest_filter=SystemSchedulerSyncPoint.LabeledFenceEmitsDebugLogLine`. Expected: **FAIL — `cap.debugWithLabel` is 0** (no label log is emitted yet). `BarrieredDeterministic` already passes (Task 2 made spawns deterministic); it stays as a regression guard.

- [ ] **Step 3: Emit the label log in the segment loop (`SystemScheduler.hpp`, Task 2's loop).** After the per-segment flush (`m_commandBuffer->Clear();`), before `g = gEnd;`, emit a Debug line when this segment's *following* fence has a non-empty label. Fence `seg` sits after segment `seg` (valid for `seg < m_currentSegment`; the last segment has no following fence):
```cpp
                    if (seg < m_fenceLabels.size() && !m_fenceLabels[seg].empty())
                    {
                        std::string msg = "SystemScheduler: sync point '" + m_fenceLabels[seg]
                                        + "' flushed segment " + std::to_string(seg);
                        ASTRA_LOG_DEBUG(msg);
                    }
```
(`m_fenceLabels.size() == m_currentSegment`; fence index `seg` is the fence between segment `seg` and `seg+1`. `ASTRA_LOG_DEBUG` is off at the default Info level → zero production cost.)
> Confirm `ASTRA_LOG_DEBUG` exists in `Core/Log.hpp` (Phase D used `ASTRA_LOG_ERROR`/`ASTRA_LOG_WARN`; the `Debug` level macro is `ASTRA_LOG_DEBUG` per `LogTest.cpp`'s `ASTRA_LOG_INFO` sibling — verify the exact macro name and use it).

- [ ] **Step 4: Run — verify GREEN.** Build all 3 configs. Both tests pass. Full suite green (→ 646/644/644).

- [ ] **Step 5: Commit.**
```bash
git add include/Astra/System/SystemScheduler.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): emit a Debug-level log line at each labeled SyncPoint flush"
```

---

### Task 4: Lambda polish — reject non-reference component params

Remove the vestigial pointer/optional lambda-parameter path by `static_assert`ing that every component parameter is an lvalue reference.

**Files:**
- Modify: `include/Astra/System/System.hpp` (`LambdaSystemWrapper` ~:74-145)
- Test: `tests/System/SystemSchedulerTest.cpp` (append a positive regression test) + a documented manual compile-fail check.

**Interfaces:**
- Consumes: `LambdaSystemWrapper<Lambda, Args...>::ComponentArgs` (`= SkipEntityArg<Args...>::Components`, a `std::tuple` of the post-`Entity` parameter types).
- Produces: a compile-time guard (no runtime API).

- [ ] **Step 1: Positive regression test (must already pass).** Append a test confirming a `const T&` / `T&` view-lambda still registers and runs (read/write inference unchanged). (If an equivalent already exists in the file, note it in the report and skip adding a duplicate.)
```cpp
// A view-lambda with reference component params still schedules and runs.
TEST(SystemSchedulerLambdaPolish, ReferenceParamLambdaStillWorks)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity();
    reg.AddComponent<Position>(e, Position{});
    Astra::SystemScheduler s;
    int ran = 0;
    ASSERT_TRUE(s.AddSystem([&](Astra::Entity, const Position&) { ++ran; }).IsOk());
    s.Execute(reg);
    EXPECT_EQ(ran, 1);   // the lambda iterated the one Position entity
}
```

- [ ] **Step 2: Demonstrate RED (manual compile-fail, documented — not committed).** Temporarily add, in a scratch spot, a registration with a POINTER component param and build Debug:
```cpp
// TEMPORARY — remove before committing.
// s.AddSystem([](Astra::Entity, const Position*) {});
```
Before the `static_assert` (Step 3), record what the compiler does today (either it compiles into a malformed `CreateView<const Position*>` and fails deep inside `CreateView` with a cryptic error, or it silently mis-infers) — capture the message. This is the RED evidence for F: the pointer path is not cleanly rejected today. **Remove this line before committing.**

- [ ] **Step 3: Add the `static_assert` (`System.hpp` `LambdaSystemWrapper`).** After `using ComponentArgs = typename SkipEntityArg<Args...>::Components;` (~:112), add a trait + assert that every element of `ComponentArgs` is an lvalue reference:
```cpp
        template<typename Tuple> struct AllLvalueRefs;
        template<typename... Ts> struct AllLvalueRefs<std::tuple<Ts...>>
            : std::bool_constant<(std::is_lvalue_reference_v<Ts> && ...)> {};

        static_assert(AllLvalueRefs<ComponentArgs>::value,
            "Astra system lambda component parameters must be 'T&' or 'const T&'. "
            "Pointer, by-value, and optional/nullable component parameters are not "
            "supported (a 'const T*' would be mis-inferred as a write and build a "
            "malformed view). Use 'const T&' for read access or 'T&' for write access.");
```
(An empty `ComponentArgs` — a lambda with only `Entity` — makes the fold `(... && )` vacuously `true`, which is correct: no component params, nothing to reject.)

- [ ] **Step 4: Verify.** Re-add the temporary pointer-param registration from Step 2, build Debug, confirm it now fails with the CLEAR `static_assert` message (GREEN for the guard), then **remove it**. Build all 3 configs clean (positive test + full suite). Run `--gtest_filter=SystemSchedulerLambdaPolish.*` and the existing view-lambda tests. Full suite green (→ 647/645/645). Document the before/after compiler messages in the report.

- [ ] **Step 5: Commit.**
```bash
git add include/Astra/System/System.hpp tests/System/SystemSchedulerTest.cpp
git commit -m "feat(system): static_assert rejects non-reference lambda component params (removes vestigial const T* path)"
```

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §13 module 1 (API) → Task 1 Steps 4-5; module 2 (segment model) → Task 1 Steps 3,6,8,9; module 3 (cross-segment edges ignored) → Task 1 Step 8; module 4 (mid-run flush) → Task 2 + the label surface → Task 3; module 5 (determinism/backward-compat) → Task 1 Step 10 + Task 2 Step 4 + Task 3's determinism gate. §15 (lambda polish) → Task 4. Deferred items (auto barrier insertion, `Optional<T>`, View-rebuild perf) correctly have no task.
- **Placeholder scan:** every code step shows complete code; the two deliberately-manual steps (Task 2 Step 3 grounding check on `ParallelCommandBuffer` error semantics; Task 4 Step 2/4 manual compile-fail demonstration) are bounded with concrete instructions and a report requirement, not "TODO".
- **Type consistency:** `segmentIndex` (Task 1 Step 3) is stamped in Step 6, read in Step 8 (edge filter), Step 9 (grouping break + `m_planGroupSegment`), and Task 2's flush loop; `m_currentSegment`/`m_fenceLabels` (Step 4) are set in Step 5, reset in Step 7, read in Task 2/3; `m_planGroupSegment` (Step 4) is filled in Step 9, consumed in Task 2 Step 3; `GetSegmentCount()` returns `m_currentSegment + 1`. `AddSyncPoint`/`AddSyncPoint(label)` names match across tasks. `ComponentArgs`/`AllLvalueRefs` (Task 4) are self-contained.
- **TypeID budget:** no fresh component types — reuses `Position`/`Velocity` (existing). Well within the 128 ceiling.
- **Additive:** no-`SyncPoint` ⇒ `segmentIndex==0` everywhere ⇒ the segment-break never fires, the edge filter excludes nothing, the flush loop runs once ⇒ grouping + deferred apply order byte-identical to Phase D; `Exclusive`/resource/ordering behavior unchanged; reference-param lambdas unchanged (only pointer/value/optional now rejected, and none existed).
