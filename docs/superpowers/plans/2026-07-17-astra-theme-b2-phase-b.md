# Theme B2 — Phase B: Thread-Safe Registration + Chunk-Parallel Deferral — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make component registration worker-safe, then let a system iterate a view across worker threads by chunk and defer structural changes deterministically from each chunk via `ctx.ParallelForEach`.

**Architecture:** Additive on Phase A. Module 1 adds a lock-free-warm-path first-registration guard to `ComponentRegistry`. Module 2 stamps each chunk's deferred commands with the `iterationIndex` field reserved in Phase A (`iterationIndex = chunkIndex`), so the existing stable-sorted flush recovers a deterministic apply order; `SystemContext` gains a nullable `ParallelCommandBuffer*` read only by a new `ParallelForEach` wrapper over the existing `View::ParallelForEach`.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`; the `IWorkScheduler` seam (`Mosaic::IWorkScheduler`).

**Spec:** `docs/superpowers/specs/2026-07-17-astra-theme-b2-phase-b-design.md` (approved) — Module 1 = thread-safe registration; Module 2 = parent-spec §9 chunk-parallel. NOT in Phase B: resources §10, ordering §11, ambiguity §12, barriers §13, lambda polish §15.

## Global Constraints

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang. **Exception-free & RTTI-off in shipping** → errors are values; no `try`/`catch`; a check that must hold in shipping is a real `if`, never `ASTRA_ASSERT` (compiles out under `NDEBUG`). Mutexes are permitted (already used in `ParallelCommandBuffer`).
- **Additive, no break.** `void(Registry&)` systems, view-lambdas, the standalone eager `CommandBuffer`, and Phase A's `void(SystemContext&)` + `Commands()` path keep working unchanged. Phase A's determinism gate + error-channel tests stay green. Baseline suite **611 Debug / 609 Release+Dist** (dev @ `c2b53b2`) stays green.
- **Pay-for-what-you-use:** `include/Astra/Registry/Registry.hpp` gains no `System/` or `Commands/` include. Confirm with a grep before committing any include-touching task.
- **Determinism is a contract:** the applied order of deferred structural changes and placeholder id assignment are identical across runs, independent of thread count/scheduling and of which worker processed which chunk. Keys stay globally unique: `(insertionOrder, chunkIndex, recordSequence)`.
- Namespace `Astra`. IDE/clang-tidy diagnostics are misconfigured false positives (expects Clang 20; "no gtest"; "no std::byte"; "'Mosaic/Platform.hpp' not found"; "unknown type 'concept'") — judge only by the MSVC build.
- All file:line anchors are from the 2026-07-17 tree (dev @ `c2b53b2`); **confirm each against the live tree before editing.**

**Build (per config, whole solution — `-t:AstraTest` does NOT work):**
`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`
**New test file → regen** `D:\dev\_shared\tools\premake5 vs2022` (never `git add ide/`/`Astra.sln`/`Makefile`/`*.make`); appending to an existing test file does not need regen. Stale-PDB `mspdbsrv.exe` lock → `taskkill //F //IM mspdbsrv.exe` + rebuild.

## File Structure

- `include/Astra/Component/ComponentRegistry.hpp` — Module 1: the first-registration guard (atomic flags + mutex) around `RegisterComponent`/`ReRegisterComponent`.
- `include/Astra/System/SystemContext.hpp` — Module 2: parameterize `iterationIndex`; add the nullable `ParallelCommandBuffer* m_parallelBuffer`; add the `ParallelForEach` wrapper.
- `include/Astra/System/SystemExecutor.hpp` — pass the `ParallelCommandBuffer*` into the `SystemContext` it constructs (Phase A already builds the context here).
- `include/Astra/Registry/View.hpp` — Module 2 glue: a context-threading chunk-iteration path that exposes `chunkIndex` to a per-chunk sub-context (mirrors `ParallelForEach`, `View.hpp:115-376`).
- Tests: `tests/Component/ComponentRegistryConcurrencyTest.cpp` (**new** → premake regen), `tests/System/SystemContextTest.cpp` (append).

---

### Task 1: Thread-safe component registration (Module 1)

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (`RegisterComponent` ~:23, `ReRegisterComponent` ~:35, add members near the private section)
- Test: `tests/Component/ComponentRegistryConcurrencyTest.cpp` (**new** → premake regen this task)

**Interfaces:**
- Produces (consumed by Module 2 + the current parallel path): `RegisterComponent<T>()` is safe to call concurrently from multiple threads. First registration of a type is serialized; the warm path is a lock-free atomic load. Observable results (`GetComponentDescriptor`, `AddComponentByID`) are unchanged.

- [ ] **Step 1: Write the failing test — concurrent registration doesn't corrupt.**
Create `tests/Component/ComponentRegistryConcurrencyTest.cpp` (mirror an existing `tests/Component/*` fixture for includes; use `../Support/TestWorkerPool.hpp`). Define ~16 distinct component structs. Under the real `TestWorkerPool`, dispatch work that has many workers concurrently call `registry.GetComponentRegistry()->RegisterComponent<Ti>()` for a mix of **distinct** types and the **same** type repeatedly, looped enough (e.g. 200 iterations over fresh registries) that the unguarded version reliably crashes. After each iteration assert: every type has a non-null descriptor via `GetComponentDescriptorByHash(TypeID<Ti>::Hash())`, each descriptor's `id`/`size`/`alignment` are correct, and `Size()` equals the distinct-type count.
```cpp
// Sketch — adapt includes/fixture to the Component test dir.
TEST(ComponentRegistryConcurrency, ConcurrentRegisterDistinctAndSameTypesIsRaceFree) { /* ... */ }
```

- [ ] **Step 2: Run — verify RED** (crash / sanitizer / wrong count under contention, or — if it happens to pass by luck — treat as inconclusive and raise iteration/worker count until the unguarded version fails). Run: `AstraTest.exe --gtest_filter=ComponentRegistryConcurrency.*`

- [ ] **Step 3: Add the guard members + guarded `RegisterComponent`.** Near `ComponentRegistry`'s private members add:
```cpp
    // First-registration guard. m_registered[id] is set once per type; the
    // warm path is a lock-free acquire-load (replacing the old Contains()
    // lookup). m_registrationMutex serializes the one-time cold path so two
    // workers first-registering different types can't race the containers.
    std::mutex m_registrationMutex;
    std::atomic<bool> m_registered[MAX_COMPONENTS] = {};
```
Rewrite `RegisterComponent`:
```cpp
    template<Component T>
    void RegisterComponent()
    {
        const ComponentID id = TypeID<T>::Value();
        if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY  // guard before indexing m_registered
            return;
        if (m_registered[id].load(std::memory_order_acquire))
            return;                                // warm path: lock-free
        std::lock_guard<std::mutex> lock(m_registrationMutex);
        if (m_registered[id].load(std::memory_order_relaxed))
            return;                                // double-check under lock
        RegisterComponentImpl<T>(id);              // may refuse (over-aligned) — that's fine
        m_registered[id].store(true, std::memory_order_release);  // "attempt resolved for id"
    }
```
Make `ReRegisterComponent<T>()` take the same lock and set the flag after `RegisterComponentImpl` (hot-reload keeps its unconditional rebuild):
```cpp
    template<Component T>
    void ReRegisterComponent()
    {
        const ComponentID id = TypeID<T>::Value();
        if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY return;
        std::lock_guard<std::mutex> lock(m_registrationMutex);
        RegisterComponentImpl<T>(id);
        m_registered[id].store(true, std::memory_order_release);
    }
```
Add `#include <atomic>` and `#include <mutex>` at the top. Leave `RegisterComponentImpl` unchanged. Note the flag means "registration attempted for this id" — a refused (over-aligned / id-exhausted) type stays absent from `m_components`, so `GetComponentDescriptor` still returns nullptr, identical to today; the flag only prevents re-running `Impl`.

- [ ] **Step 4: Confirm `RegisterComponents<T...>` and copyability.**
  - `RegisterComponents<T...>` (bulk, ~:42) calls `RegisterComponent` per type (each now guarded) after `m_components.Reserve(...)`. This bulk API is a single-threaded setup path — do NOT call it from workers; add a one-line comment saying so. Do not hold `m_registrationMutex` across the fan-out (the per-type `RegisterComponent` re-locks; `std::mutex` is non-recursive → would deadlock).
  - **Copyability:** the mutex + atomic array make `ComponentRegistry` non-copyable/movable. Run `git grep -n "ComponentRegistry" include/ | grep -iE "copy|clone|= *[a-z_]*registry"` and check the `Registry` copy path. If nothing copies a `ComponentRegistry`, add a brief `// non-copyable: holds a registration mutex + atomics` note and proceed. If a copy IS required, add a hand-written copy that value-copies the `FlatMap`s + deque and default-initializes the mutex/atomics (a fresh registry's flags reflect its copied contents), and say so in the report.

- [ ] **Step 5: Run — verify GREEN** (`--gtest_filter=ComponentRegistryConcurrency.*`), then build all three configs; full suite green (baseline + the new test → 612/610/610). Because this is a race test, run it several times per config.

- [ ] **Step 6: Commit.**
```bash
git add include/Astra/Component/ComponentRegistry.hpp tests/Component/ComponentRegistryConcurrencyTest.cpp
git commit -m "fix(component): thread-safe first-registration guard for worker-side RegisterComponent"
```

---

### Task 2: SystemContext chunk sub-context support (Module 2 plumbing)

**Files:**
- Modify: `include/Astra/System/SystemContext.hpp` (ctor, `Commands()` stamping, add `m_iterationIndex` + `m_parallelBuffer`)
- Modify: `include/Astra/System/SystemExecutor.hpp` (`DispatchSystem` passes the `ParallelCommandBuffer*` when constructing the context)
- Test: `tests/System/SystemContextTest.cpp` (append)

**Interfaces:**
- Consumes: Phase A `SortKey`, `ParallelCommandBuffer`, the executor's per-worker `GetThreadBuffer()`.
- Produces (consumed by Task 3): a `SystemContext` constructible with an explicit `iterationIndex` whose `Commands()` stamps `SortKey{insertionOrder, iterationIndex, recordSequence++}`; a nullable `ParallelCommandBuffer* GetParallelBuffer() const` accessor (read only by `ParallelForEach`, Task 3). Phase A's default construction path stamps `iterationIndex = 0` exactly as before.

- [ ] **Step 1: Write the failing test — a sub-context stamps its iterationIndex; a normal context still stamps 0.**
Append to `SystemContextTest.cpp`. Construct a `SystemContext` two ways (via the existing ctor for the `iterationIndex=0` case, and via the new ctor with `iterationIndex=K`), record a command on each into a `CommandBuffer`, and assert (via `CommandKeys()`) the stamped `SortKey.iterationIndex` is `0` and `K` respectively, with `insertionOrder` and a monotonic `recordSequence` preserved. (Confirm how to read back `CommandKeys()` from a `CommandBuffer` — Phase A exposes it.)
```cpp
TEST_F(SystemContextTest, SubContextStampsIterationIndexAndNormalContextStampsZero) { /* ... */ }
```

- [ ] **Step 2: Run — verify RED** (no ctor with `iterationIndex`, no accessor, or wrong stamping).

- [ ] **Step 3: Parameterize `SystemContext`.** Add `uint32_t m_iterationIndex = 0;` and `ParallelCommandBuffer* m_parallelBuffer = nullptr;`. Keep the existing ctor (Phase A: `SystemContext(Registry&, CommandBuffer&, uint32_t insertionOrder)`) delegating with `iterationIndex = 0, parallelBuffer = nullptr`. Add a full ctor:
```cpp
    SystemContext(Registry& reg, CommandBuffer& cmds, uint32_t insertionOrder,
                  uint32_t iterationIndex, ParallelCommandBuffer* parallelBuffer) noexcept
        : m_registry(reg), m_commands(cmds), m_insertionOrder(insertionOrder),
          m_iterationIndex(iterationIndex), m_parallelBuffer(parallelBuffer) {}
```
Change `Commands()` to stamp `SortKey{m_insertionOrder, m_iterationIndex, m_recordSequence++}` (was hardcoded `0u` for the middle field). Add `[[nodiscard]] ParallelCommandBuffer* GetParallelBuffer() const noexcept { return m_parallelBuffer; }`. Forward-declare `class ParallelCommandBuffer;` if not already visible (the pointer needs no complete type here; `ParallelForEach` in Task 3 will need it complete).

- [ ] **Step 4: Wire the executor.** In `SystemExecutor.hpp` `DispatchSystem` (where Phase A constructs `SystemContext(reg, GetThreadBuffer(), insertionOrder)`), pass the `ParallelCommandBuffer*` and `iterationIndex = 0`: `SystemContext(reg, buf, insertionOrder, 0u, &parallelBuffer)`. The executor already has the `ParallelCommandBuffer` (Phase A) — confirm the exact name/reference it holds and thread it in. No behavior change for existing systems (iterationIndex stays 0; the pointer is unused until Task 3).

- [ ] **Step 5: Run — verify GREEN**; `git grep -n 'Commands/\|System/' include/Astra/Registry/Registry.hpp` returns nothing new; build 3 configs; suite green (Phase A tests unchanged + the new one).

- [ ] **Step 6: Commit** → `feat(system): SystemContext iterationIndex + parallel-buffer handle for chunk sub-contexts`

---

### Task 3: ctx.ParallelForEach — chunk-parallel deferral (Module 2)

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (add a context-threading chunk-iteration path mirroring `ParallelForEach`, `:115-376`)
- Modify: `include/Astra/System/SystemContext.hpp` (add the `ParallelForEach` wrapper)
- Test: `tests/System/SystemContextTest.cpp` (append)

**Interfaces:**
- Consumes: Task 2's `SystemContext(reg, buf, insertionOrder, iterationIndex, parallelBuffer)` ctor + `GetParallelBuffer()`; Phase A `ParallelCommandBuffer::GetThreadBuffer()`; `View::ParallelForEach` internals (`ParallelForEachChunkImpl`, `chunkIndex`).
- Produces: `void SystemContext::ParallelForEach(View&, Func&&)` where `Func` is invocable as `func(Entity, Components&..., SystemContext& sub)`; each chunk's body receives a sub-context with `iterationIndex = chunkIndex` recording into that chunk-worker's buffer.

- [ ] **Step 1: Write the failing test — chunk-parallel bodies record deferred changes that land.**
Append to `SystemContextTest.cpp`. Seed a registry with a multi-chunk set of entities carrying `Position`. Register a `void(SystemContext&)` system whose body calls `ctx.ParallelForEach(view, [](Entity e, const Position&, SystemContext& sub){ sub.Commands().AddComponent<Tag>(e, ...); })`. After `scheduler.Execute(...)` under the real `TestWorkerPool`, assert **every** seeded entity now has `Tag` (count via a `CreateView<Tag>()`), and no crash. Pre-register `Tag` on the main thread (Module 1 also covers this, but keep the pattern). (Confirm the multi-chunk seeding — enough entities to span >1 chunk; check the archetype chunk capacity used by `View::ParallelForEach`.)
```cpp
TEST_F(SystemContextTest, ParallelForEachRecordsDeferredChangesPerChunkThatApplyAtFlush) { /* ... */ }
```

- [ ] **Step 2: Run — verify RED** (`ParallelForEach` undeclared on `SystemContext`).

- [ ] **Step 3: Add the View glue.** `View::ParallelForEach` (`:115`) dispatches `chunkWork` via `m_scheduler->ParallelFor` and calls `ParallelForEachChunkImpl(archetype, chunkIndex, func, ...)` (`:167-173, :351`) per chunk — chunkIndex is known there. Add a sibling templated method that threads a per-chunk context to the body, e.g.:
```cpp
    // Like ParallelForEach, but builds a per-chunk sub-context via `factory(chunkIndex)`
    // and invokes `body(entity, components..., sub)`. factory runs once per chunk on the
    // worker executing that chunk; the sub-context records into that worker's buffer.
    template<typename Factory, typename Body>
    ASTRA_FORCEINLINE void ParallelForEachWithContext(Factory&& factory, Body&& body);
```
Mirror the existing chunk-split + `ParallelFor` dispatch exactly; the only change is: per chunk, `auto sub = factory(chunkIndex);` then iterate that chunk's entities calling `body(entity, components..., sub)` (reuse the existing per-chunk entity walk). **Confirm the exact `ParallelForEachChunkImpl`/`ParallelForEachChunkWithOptional` signatures (`:351-376`) and thread `sub` through as a trailing arg with minimal change — do not rewrite iteration.** Null-scheduler path: iterate all chunks inline in order (deterministic), building a sub-context per chunk.

- [ ] **Step 4: Add `SystemContext::ParallelForEach`.** Needs `ParallelCommandBuffer` complete here (include `../Commands/CommandBuffer.hpp` — already included by SystemContext.hpp in Phase A). Implement:
```cpp
    template<typename View, typename Func>
    void ParallelForEach(View& view, Func&& func)
    {
        Registry& reg = m_registry;
        const uint32_t insertionOrder = m_insertionOrder;
        ParallelCommandBuffer* pcb = m_parallelBuffer;
        view.ParallelForEachWithContext(
            // factory: one sub-context per chunk, on the worker running that chunk
            [&, insertionOrder, pcb](uint32_t chunkIndex) {
                CommandBuffer& buf = pcb ? pcb->GetThreadBuffer() : /* the immediate buffer, m_commands */ ;
                return SystemContext(reg, buf, insertionOrder, chunkIndex, pcb);
            },
            std::forward<Func>(func));
    }
```
Resolve the null-`pcb` fallback (a standalone context with no parallel buffer — use `m_commands`, iterationIndex still = chunkIndex; determinism holds). Confirm `GetThreadBuffer()` is called INSIDE the factory (on the chunk-worker), matching Phase A's worker-thread rule.

- [ ] **Step 5: Run — verify GREEN** (all seeded entities tagged, no crash) in all 3 configs; suite green.

- [ ] **Step 6: Commit** → `feat(system): ctx.ParallelForEach — per-chunk sub-context deferral stamped by chunkIndex`

---

### Task 4: Chunk-parallel determinism gate (acceptance)

**Files:**
- Test: `tests/System/SystemContextTest.cpp` (append)

- [ ] **Step 1: Write the crux test.** Seed a rich multi-chunk entity set. Register a `void(SystemContext&)` system that uses `ctx.ParallelForEach` over the view and, per entity, defers a MIX of structural changes (add a tag; create a placeholder related entity with a component; destroy some subset by a deterministic predicate) — some targeting overlapping entities so apply order is observable. Run `scheduler.Execute(...)` under the real multi-threaded `TestWorkerPool` **N=50 times from identical initial state**, capturing a canonical world snapshot each run (resolved entity ids + component values, sorted by id, as the Phase A gate does). Assert byte-identical across all 50 runs. Pre-register every component type on the main thread first.
```cpp
TEST_F(SystemContextTest, ChunkParallelDeferredChangesFlushIdenticallyAcross50Runs) { /* ... */ }
```

- [ ] **Step 2: Run — verify it passes** in all 3 configs (given Tasks 1–3). If it flakes, a `chunkIndex`-stamping or sub-context bug remains in Task 2/3 — fix there, do NOT weaken this test. Capture 50 runs × 3 configs.

- [ ] **Step 3: Commit** → `test(system): 50-run chunk-parallel determinism gate`

---

## Self-Review (author checklist — completed)

- **Spec coverage:** Module 1 (thread-safe registration) → Task 1; Module 2 API + sub-context stamping → Tasks 2–3; the §9 View glue → Task 3; determinism-contract extension → Task 4. Out-of-Phase-B modules (§10–§13, §15) are explicitly later plans.
- **Grounding checks (spec decision #5) each assigned:** `ComponentRegistry` copyability → Task 1 Step 4; nested-`ParallelFor` / null-scheduler behavior → Task 3 Step 3–4 (inline path) + confirm-notes; exact `View::ParallelForEach` value-mutation signature + sub-context threading → Task 3 Step 3.
- **Type consistency:** `SortKey{insertionOrder, iterationIndex, recordSequence}` (Phase A) is stamped by the new `SystemContext(..., iterationIndex, parallelBuffer)` ctor (Task 2) and consumed by `ParallelForEach` (Task 3) and the gate (Task 4). `ParallelForEachWithContext`/`GetParallelBuffer`/`GetThreadBuffer` names consistent across tasks.
- **Exception-free honored:** the guard uses a `std::mutex` (not exceptions); no `ASTRA_ASSERT` as a shipping guard; errors stay values.
- **Additive:** Phase A `Commands()` path unchanged (Task 2 keeps the old ctor delegating with iterationIndex 0, pointer null); the new `ParallelCommandBuffer*` is dormant for non-chunk systems.
- **Placeholder scan:** production changes give concrete code (the guard, the ctor, the wrapper skeleton) or a precisely-bounded design with the exact live-code anchor to confirm (the View glue). Test bodies are specified by behavior + assertion; the determinism test gives the exact N and invariant.
