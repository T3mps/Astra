# Theme B2 — Phase A: Deferred-Command Core — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a system defer structural changes during scheduler execution via a `SystemContext`, and flush them **deterministically** at the `depth==0` sync point — with worker-safe placeholder-entity creation and a per-system error channel.

**Architecture:** Additive. A new `SystemContext` handle wraps `Registry&` + the system's per-worker command-recording surface + its plan `insertionOrder` + an error sink. Each recorded command gets a composite **sort key** stored in a parallel per-command descriptor (not in the byte buffer). At `depth→0` the executor calls a new `ExecuteSorted()` that stable-sorts every command across every worker buffer by key and applies them single-threaded, resolving placeholder entities to real ids in that order. `void(Registry&)` systems are untouched.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`; the `IWorkScheduler` seam (`Mosaic::IWorkScheduler`).

**Spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-concurrency-design.md` (approved) — Phase A covers spec modules §5 (SystemContext), §6 (deferred + sort key), §7 (placeholder entities), §8 (error channel), the §13 *default single flush at depth==0*, and the §14 determinism contract. NOT in Phase A: chunk-parallel (§9), resources (§10), ordering (§11), ambiguity (§12), explicit barriers (§13), lambda polish (§15) — each is a later plan off the same spec.

## Global Constraints

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values (the error channel / `Result`), never exceptions; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT` (compiles out in Release/Dist).
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` must gain **no** include of `System/` or `Commands/`. All new code lives in `System/` and `Commands/` (both already sit above Registry). Confirm with a grep before committing any task that touches includes.
- **Additive, no break:** `void(Registry&)` systems keep working unchanged. `void(SystemContext&)` is a new overload. Baseline suite **602 Debug / 600 Release+Dist** stays green.
- Everything opt-in via `IWorkScheduler`; a null scheduler ⇒ sequential inline fallback (and then the sorted flush is trivially in insertion order). Astra spawns no threads.
- Namespace `Astra`. Determinism is a contract: the applied order of deferred structural changes, and the ids assigned to placeholder entities, must be identical across runs independent of thread count/scheduling.
- Build via MSBuild **whole solution** — `-t:AstraTest` does NOT work. New test file → `D:\dev\_shared\tools\premake5 vs2022` regen; appending to an existing test file does not. Never `git add` `ide/`/`Astra.sln`/`Makefile`/`*.make`. IDE/clang-tidy diagnostics are misconfigured false positives — judge only by MSVC.
- All file:line anchors are from the 2026-07-16 code map; **confirm each against the live tree before editing.**

**Build (per config):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`

## File Structure

- `include/Astra/Commands/CommandBuffer.hpp` — add the sort-key descriptor + sorted recording + `ParallelCommandBuffer::ExecuteSorted()`. Existing `Execute()` (arrival order) stays for standalone use.
- `include/Astra/Commands/Command.hpp` — the `SortKey` struct (shared vocabulary).
- `include/Astra/System/SystemContext.hpp` — **new file.** The `SystemContext` handle + the `ContextSystem` concept.
- `include/Astra/System/System.hpp` — the `ContextSystem` concept may live here beside `System`; the wrapper detection.
- `include/Astra/System/SystemScheduler.hpp` — registration detects `void(SystemContext&)`; `Execute()` owns a `ParallelCommandBuffer`, hands each system a `SystemContext`, and calls `ExecuteSorted()` at `depth→0`; accumulates the error channel.
- `include/Astra/System/SystemExecutor.hpp` — the executor invokes the context thunk.
- Tests: `tests/System/SystemContextTest.cpp` (**new**), extend `tests/Commands/CommandBufferTest.cpp`.

---

### Task 1: Sort key + deterministic sorted flush (command layer)

**Files:**
- Modify: `include/Astra/Commands/Command.hpp` (add `SortKey` near `CommandHeader` ~:47)
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (`CommandBuffer` recording ~:134+; `ParallelCommandBuffer` ~:1118)
- Test: `tests/Commands/CommandBufferTest.cpp` (append)

**Interfaces:**
- Produces (consumed by Tasks 2–5):
  - `struct Astra::SortKey { uint32_t insertionOrder; uint32_t iterationIndex; uint32_t recordSequence; };` with `operator<` (lexicographic).
  - `CommandBuffer` records, per command, a parallel descriptor `{SortKey key; size_t offset;}` in a side `std::vector`. A new overload path tags the next-recorded command with a caller-supplied `SortKey` (default `{0,0,seq}` where `seq` auto-increments per buffer). Exact API: `void CommandBuffer::SetNextSortKey(SortKey)` applied to the next recorded command, OR a `recordSeq` member the buffer stamps automatically — the implementer picks the cleaner of the two; the REQUIREMENT is that every recorded command has an associated `SortKey`.
  - `Result<void, CommandBuffer::ExecutionError> ParallelCommandBuffer::ExecuteSorted()` — gather every (command, key) across all worker buffers, **stable-sort by key**, apply single-threaded in that order. Existing `Execute()` unchanged.

- [ ] **Step 1: Write the failing test — sorted flush applies in key order, not record/arrival order.**

Append to `tests/Commands/CommandBufferTest.cpp` (match its fixture/registry setup). Record two commands whose keys invert their record order, assert `ExecuteSorted` applies them in key order. Simplest observable: two `CreateEntity`/`AddComponent` ops whose apply order is detectable, or record into two worker buffers with descending keys and assert the resulting entities/components reflect ascending-key order.
```cpp
// Sketch — adapt to the file's Registry/component setup. The assertion must distinguish
// key-order from record-order (e.g. two adds of a component whose value encodes the key,
// last-write-wins → the higher key wins deterministically).
TEST_F(CommandBufferTest, ExecuteSortedAppliesInKeyOrderNotRecordOrder) { /* ... */ }
```

- [ ] **Step 2: Run — verify RED** (`ExecuteSorted`/`SortKey` undeclared, or wrong order).
Run: `AstraTest.exe --gtest_filter=CommandBufferTest.ExecuteSortedAppliesInKeyOrderNotRecordOrder`

- [ ] **Step 3: Add `SortKey`** to `Command.hpp`:
```cpp
    // Composite key that makes deferred-command flush order deterministic (spec §6/§14).
    struct SortKey
    {
        uint32_t insertionOrder = 0;  // recording system's plan order (high bits)
        uint32_t iterationIndex = 0;  // 0 for ordinary deferral; chunk index under ParallelForEach (Phase B)
        uint32_t recordSequence = 0;  // per-(system,iterationIndex) monotonic tiebreak = exact record order
        friend bool operator<(const SortKey& a, const SortKey& b) noexcept
        {
            if (a.insertionOrder != b.insertionOrder) return a.insertionOrder < b.insertionOrder;
            if (a.iterationIndex != b.iterationIndex) return a.iterationIndex < b.iterationIndex;
            return a.recordSequence < b.recordSequence;
        }
    };
```

- [ ] **Step 4: Store a per-command descriptor in `CommandBuffer`.** Add `std::vector<std::pair<SortKey, size_t>> m_commandKeys;` populated at each command append (the append site records the byte offset already used by `Execute`'s walk — reuse it). Provide the sort-key entry point (`SetNextSortKey` or an auto-stamped sequence). Keep the byte buffer and existing `Execute()` untouched.

- [ ] **Step 5: Add `ParallelCommandBuffer::ExecuteSorted()`.** Gather `{key, worker-buffer-ptr, offset}` triples from every `m_buffers[i]`'s `m_commandKeys`, `std::stable_sort` by `key`, then dispatch each command by (buffer, offset) through the existing per-command execute path (`ExecuteCommand`/the `Execute` switch — factor out a single-command apply if needed). Rollback semantics: keep the existing partial-failure rollback shape (Task 5 refines error handling).
```cpp
        Result<void, CommandBuffer::ExecutionError> ExecuteSorted()
        {
            struct Item { SortKey key; CommandBuffer* buf; size_t offset; };
            std::vector<Item> items;
            for (auto& b : m_buffers)
                if (b) for (auto& [k, off] : b->CommandKeys()) items.push_back({k, b.get(), off});
            std::stable_sort(items.begin(), items.end(),
                             [](const Item& a, const Item& b){ return a.key < b.key; });
            for (auto& it : items) { /* apply one command at (it.buf, it.offset); on Err, roll back + return */ }
            return Result<void, CommandBuffer::ExecutionError>::Ok();
        }
```
(Confirm the real per-command apply entry point; `CommandKeys()` is a new accessor for `m_commandKeys`.)

- [ ] **Step 6: Run — verify GREEN**, then build all three configs; full suite green (+1 test → 603/601).

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Commands/Command.hpp include/Astra/Commands/CommandBuffer.hpp tests/Commands/CommandBufferTest.cpp
git commit -m "feat(commands): composite sort key + deterministic ExecuteSorted flush"
```

---

### Task 2: SystemContext + the void(SystemContext&) signature

**Files:**
- Create: `include/Astra/System/SystemContext.hpp`
- Modify: `include/Astra/System/System.hpp` (add the `ContextSystem` concept + lambda detection)
- Modify: `include/Astra/System/SystemScheduler.hpp` (`AddSystem` detects the new signature, stores a `Delegate<void(SystemContext&)>` thunk)
- Modify: `include/Astra/System/SystemExecutor.hpp` (invoke the context thunk)
- Test: `tests/System/SystemContextTest.cpp` (**new** → premake regen this task)

**Interfaces:**
- Consumes: `SortKey` (Task 1); the per-worker `CommandBuffer` from a `ParallelCommandBuffer` (Task 1/3).
- Produces:
  - `class Astra::SystemContext` with: `Registry& GetRegistry() const;` (reads/queries), `CommandBuffer& Commands();` (the system's per-worker recorder, sort-key-stamped with this system's `insertionOrder`), `void ReportError(SystemError);` (Task 4 error sink), and internal `insertionOrder`/`recordSequence` state.
  - `concept ContextSystem = requires(T s, SystemContext& ctx) { { s(ctx) } -> std::same_as<void>; };`

- [ ] **Step 1: Write the failing test — a context system runs, reads the registry, records a deferred command.**

Create `tests/System/SystemContextTest.cpp` (mirror `SystemSchedulerTest.cpp`'s includes/`TestWorkerPool` usage). Register a `void(SystemContext&)` system that reads an entity and calls `ctx.Commands().DestroyEntity(e)`; after `scheduler.Execute()`, assert the entity is destroyed. (This exercises Tasks 2+3 together; keep it minimal here and let Task 3 wire the flush — until Task 3 the deferred command won't apply, so gate the "is destroyed" assertion into Task 3's test and here assert only that the context system was invoked and recorded a command.)

- [ ] **Step 2: Run — verify RED** (no `SystemContext`, no `void(SystemContext&)` registration).

- [ ] **Step 3: Create `SystemContext.hpp`** — the lightweight handle:
```cpp
#pragma once
#include "../Registry/Registry.hpp"
#include "../Commands/CommandBuffer.hpp"
namespace Astra
{
    class SystemContext
    {
    public:
        SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder) noexcept
            : m_registry(reg), m_commands(cmds), m_insertionOrder(insertionOrder) {}
        [[nodiscard]] Registry& GetRegistry() const noexcept { return m_registry; }
        CommandBuffer& Commands() noexcept
        {
            m_commands.SetNextSortKey(SortKey{m_insertionOrder, 0u, m_recordSequence++});
            return m_commands;
        }
        // ReportError added in Task 4.
    private:
        Registry& m_registry;
        CommandBuffer& m_commands;
        uint32_t m_insertionOrder;
        uint32_t m_recordSequence = 0;
    };
}
```
(Confirm the `SetNextSortKey` spelling matches Task 1. Note: `Commands()` stamping the key per-call means each op the system records gets the next sequence — confirm the recorder applies the pending key to exactly the next command.)

- [ ] **Step 4: Add the `ContextSystem` concept + registration.** In `System.hpp` add the concept. In `SystemScheduler::AddSystem`/`AddLambdaSystemImpl`, detect `void(SystemContext&)` (concept dispatch) and store a `Delegate<void(SystemContext&)>` thunk alongside the existing `Delegate<void(Registry&)>` (a variant/second delegate on `SystemEntry`; trait extraction unchanged). In `SystemExecutor`, when a system has the context thunk, construct a `SystemContext` (registry + this system's per-worker `CommandBuffer` + its `insertionOrder`) and invoke it; otherwise invoke the `Registry&` thunk as today.

- [ ] **Step 5: Run — verify GREEN** (context system invoked, records a command). Build 3 configs; suite green (+the new test).

- [ ] **Step 6: Confirm pay-for-what-you-use** — `git grep -n 'Commands/\|System/' include/Astra/Registry/Registry.hpp` returns nothing new.

- [ ] **Step 7: Commit** (stage `SystemContext.hpp`, `System.hpp`, `SystemScheduler.hpp`, `SystemExecutor.hpp`, `SystemContextTest.cpp`).
```
feat(system): SystemContext + additive void(SystemContext&) system signature
```

---

### Task 3: Wire the deterministic flush at the depth==0 sync point

**Files:**
- Modify: `include/Astra/System/SystemScheduler.hpp` (own a `ParallelCommandBuffer`; flush at `depth→0` in `Execute`)
- Modify: `include/Astra/System/SystemExecutor.hpp` (hand each context system its per-worker buffer)
- Test: `tests/System/SystemContextTest.cpp` (append)

**Interfaces:**
- Consumes: `ParallelCommandBuffer::ExecuteSorted()` (Task 1); `SystemContext` (Task 2).
- Produces: after `SystemScheduler::Execute()` returns, all deferred structural changes have been applied in sort-key order.

- [ ] **Step 1: Write the failing test — deferred changes apply after Execute, in deterministic order.** Two context systems (distinct insertion orders) each defer a structural change to the same entity (e.g. both add a component; last-in-key-order wins). After `Execute()`, assert the world reflects **insertion-order** resolution, and assert it's identical across 20 repeated runs under `TestWorkerPool`.

- [ ] **Step 2: Run — verify RED** (deferred commands never applied, or nondeterministic).

- [ ] **Step 3: Own + flush the buffer.** `SystemScheduler` holds a `ParallelCommandBuffer` (constructed from the Registry). `SystemExecutor` gives each context system a `CommandBuffer&` = `parallelBuffer.GetThreadBuffer()` on the worker running it. In `SystemScheduler::Execute`, after all groups run and the `ExecutionGuard` returns depth to 0 (the spec's sync point), call `parallelBuffer.ExecuteSorted()` then `Clear()`. Sequential/null-scheduler path flushes identically (trivially in insertion order).

- [ ] **Step 4: Run — verify GREEN** (applied + deterministic across 20 runs). Build 3 configs; suite green.

- [ ] **Step 5: Commit** → `feat(system): flush deferred commands deterministically at the depth==0 sync point`

---

### Task 4: Per-system error channel

**Files:**
- Modify: `include/Astra/System/SystemContext.hpp` (`ReportError`), `include/Astra/System/SystemScheduler.hpp` (accumulate + expose), `include/Astra/Commands/CommandBuffer.hpp` (a flush op that fails logically → report, not abort)
- Test: `tests/System/SystemContextTest.cpp` (append)

**Interfaces:**
- Produces: a `SystemError`-style value (reuse B1's `SystemError` if present; else a small enum + context) attributed to the recording system's `insertionOrder`; surfaced after `Execute()` (e.g. `Execute()` returns/accumulates `std::vector<SystemError>` the caller can read, or a scheduler accessor).

- [ ] **Step 1: Write the failing test** — system A defers `DestroyEntity(e)`; system B (later insertion order) defers `AddComponent<T>(e)`. At flush, B's op targets a destroyed entity → it is **skipped and reported**, the flush completes, and the reported error names system B. Assert the world is consistent (e destroyed, no crash) and the error is surfaced.

- [ ] **Step 2: Run — verify RED.**

- [ ] **Step 3: Implement.** `ctx.ReportError(...)` pushes to a scheduler-owned sink. In the sorted flush, a logical failure (op on a non-existent/destroyed entity, later: unresolved placeholder) is **skipped + recorded** (attributed via the command's `SortKey.insertionOrder`), NOT aborted — distinct from an allocation failure, which keeps the existing rollback. Surface accumulated errors after `Execute`.

- [ ] **Step 4: Run — verify GREEN**; build 3 configs; suite green.

- [ ] **Step 5: Commit** → `feat(system): per-system error channel for deferred-command flush`

---

### Task 5: Placeholder entities (worker-safe deferred creation)

**Files:**
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (`CreateEntity` returns a placeholder; resolve at flush), possibly `include/Astra/Entity/Entity.hpp` (placeholder id domain)
- Test: `tests/System/SystemContextTest.cpp` + `tests/Commands/CommandBufferTest.cpp` (append)

**Interfaces:**
- Produces: `ctx.Commands().CreateEntity(...)` returns a **placeholder `Entity`** (a reserved handle distinguishable from a real entity). Placeholders are usable in subsequent deferred ops in the same buffer. At `ExecuteSorted`, placeholders resolve to real ids **in sort-key order** (deterministic), and referencing ops are rewritten. Passing a placeholder to the immediate Registry API or across buffers → error channel.

- [ ] **Step 1: Write the failing test — defer create + add-to-created in one context, deterministic id.** A context system does `Entity e = ctx.Commands().CreateEntity(); ctx.Commands().AddComponent<Position>(e, ...);`. After flush, assert a real entity exists with `Position`, and that its id is identical across 20 repeated runs (deterministic resolution).

- [ ] **Step 2: Run — verify RED** (today `CreateEntity` allocates eagerly / not worker-safe / nondeterministic).

- [ ] **Step 3: Implement placeholders.** Confirm the `Entity` id layout (`Entity/Entity.hpp`) for a spare high bit or a separate placeholder domain (spec §19). Record `CreateEntity` as a command that carries a **local placeholder id** (per-buffer counter, tagged as placeholder) instead of allocating from `EntityManager`. At `ExecuteSorted`, walk commands in sorted order; when a `CreateEntity` command applies, allocate the real id then (single-threaded, so deterministic in sorted order) and record placeholder→real in a map; every later command referencing a placeholder id is translated through the map before applying. Remove the "must be recorded from the owning thread" restriction for creation.

- [ ] **Step 4: Run — verify GREEN** (exists + deterministic id). Build 3 configs; suite green.

- [ ] **Step 5: Commit** → `feat(commands): placeholder entities resolved deterministically at flush`

---

### Task 6: Determinism integration test (the acceptance gate)

**Files:**
- Test: `tests/System/SystemContextTest.cpp` (append)

- [ ] **Step 1: Write the crux test.** Build a Registry with a rich entity set; register several `void(SystemContext&)` systems (non-conflicting component masks so they share a parallel group) that each defer a mix of structural changes — creates (with placeholders), destroys, add/remove — some targeting overlapping entities so apply order is observable. Run `scheduler.Execute()` under the real `TestWorkerPool` (multiple worker threads) **N=50 times from identical initial state**, and assert the resulting world is **byte-identical every run**: same entity set, same component values, same ids. This is the determinism contract (spec §14); a sort-key or placeholder-resolution regression makes it flake.

- [ ] **Step 2: Run — verify it passes** in all three configs (it should, given Tasks 1–5). If it flakes, a determinism bug remains in an earlier task — fix there, do not weaken this test. Capture the pass across 50 iterations × 3 configs.

- [ ] **Step 3: Commit** → `test(system): 50-run determinism gate for deferred structural changes`

---

## Self-Review (author checklist — completed)

- **Spec coverage (Phase A scope):** §5 SystemContext → Task 2; §6 sort key + deferred flush → Tasks 1,3; §7 placeholder entities → Task 5; §8 error channel → Task 4; §13 default single flush at depth==0 → Task 3; §14 determinism contract → Task 6. Out-of-Phase-A modules (§9–§12, §13 barriers, §15) are explicitly later plans — not gaps.
- **Placeholder scan:** production changes give concrete code or a precisely-bounded design (the from-scratch pieces — SortKey storage spelling, placeholder id domain — name the exact decision left to the implementer with a stated default and a §19 confirm-note, not a vague TODO). Test bodies are specified by behavior + assertion; the determinism tests give the exact N and invariant.
- **Type consistency:** `SortKey{insertionOrder,iterationIndex,recordSequence}` (Task 1) is used unchanged in `SystemContext` (Task 2), the flush (Task 3), and resolution ordering (Task 5). `ExecuteSorted`/`SetNextSortKey`/`CommandKeys()` names are consistent across Tasks 1–5.
- **Exception-free honored:** flush failures route to the error channel (Task 4), never throw; no `ASTRA_ASSERT` used as a shipping guard.
- **Confirm-against-live-code notes** (from spec §19) are attached to the two genuinely uncertain internals (command-key storage spelling; placeholder id domain) — the implementer resolves them against the real files, with the behavior fixed regardless.
