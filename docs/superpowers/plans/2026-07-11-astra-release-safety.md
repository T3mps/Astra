# Astra 3.4.1 Release-Safety Remediation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the undefined behavior reachable in ordinary Release/Dist use identified by the 2026-07-11 full-codebase review — the assert-only guards that vanish in shipping builds, plus a set of ordinary-Release correctness bugs and callback-iteration use-after-frees — without touching the hot path.

**Architecture:** Every fix follows one rule (the spec's governing principle): a guard on an **invariant** (only an Astra bug can violate it) stays `ASTRA_ASSERT`-only; a guard on a **condition** that a correct caller/runtime can cause (OOM, exhaustion, expired `weak_ptr`, dynamic/wrong-type input) becomes an all-config `ASTRA_UNLIKELY` early-return of `nullptr`/`Err`/`false`/no-op. Structural bugs are fixed in place; the two common "mutate during iteration" patterns (self-unregistering event handlers, destroy-each-child) are made safe by snapshotting; View iteration is documented to defer structural changes through the existing `CommandBuffer`.

**Tech Stack:** C++20 header-only; GoogleTest; premake5 → MSBuild (VS2022 solution). Spec: `docs/superpowers/specs/2026-07-11-astra-release-safety-design.md`. Review of record: `docs/reviews/2026-07-11-astra-full-review.md`.

## Global Constraints

- C++20; header-only; **no exceptions** (the test project builds with exceptions off — never add `throw`/`try`); no new dependencies.
- Must compile on MSVC 2022+, GCC 11+, Clang 13+ (avoid MSVC-only constructs).
- **Governing rule:** never convert an *invariant* assert; only convert *condition* asserts (OOM / exhaustion / expired lifetime / dynamic-or-wrong-type input) to all-config handling, always with `ASTRA_UNLIKELY` on the failure branch. The README behavioral contract is unchanged.
- **No archive-format change.** `BINARY_FORMAT_VERSION` stays 2. This is a patch release (3.4.1).
- Build (whole solution; `-t:AstraTest` does NOT work — projects nest in solution folders):
  `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m -v:minimal`
- Test: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe --gtest_brief=1`. **Gate for every task: full suite green in Debug AND Release** (Dist for tasks that change headers used by the benchmark; always for the final task).
- After adding a NEW test file, regenerate the solution so the local build sees it: `D:/dev/_shared/tools/premake5 vs2022`. Adding a `TEST`/`TEST_F` to an EXISTING test file needs no regen.
- **Commit tracked source only.** `ide/`, `Astra.sln`, `Makefile`, `*.make` are gitignored — never `git add ide/`. Stage only `include/`, `tests/`, `docs/`, `premake5.lua`, `README.md`, `include/Astra/Core/Version.hpp`.
- **Test-count note:** current baseline on `dev`/this branch is **527** (Debug/Release/Dist). Plan tasks add tests; gate on "both configs green + exactly the intended new tests," not on an absolute number.
- End every commit message body with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01XF1hB4cickyZqBjL1acq6g`
- Existing files mixing `TEST` and `TEST_F` in one suite fail at gtest startup — match whichever the target file already uses.
- The IDE clang linter emits false positives ("expects Clang 20", "gtest not found", "no std::byte") — ignore; judge by the MSVC build.

## File structure

New test files:
- `tests/Component/ResourceStorageGuardTest.cpp` — Theme-A ResourceStorage (Task 1)
- `tests/Reflection/FieldAccessorSafetyTest.cpp` — Theme-A Reflection (Task 2)
- `tests/Component/RegistrationGuardTest.cpp` — Theme-A alignment/exhaustion (Task 3)
- `tests/Core/DelegateResultSafetyTest.cpp` — Result assert + Delegate copy (Task 5)
- `tests/Registry/RootArchetypeRoundTripTest.cpp` — B1 (Task 6)
- `tests/Registry/LargeComponentTest.cpp` — B2 (Task 7)
- `tests/Registry/ClearOrphansViewTest.cpp` — B5 (Task 9)
- `tests/Registry/IterationSafetyTest.cpp` — C1/C2 (Tasks 11–12)

Existing test files appended to: `tests/Registry/ViewInvalidationTest.cpp` (Task 8), `tests/Commands/CommandBufferTest.cpp` (Task 10), `tests/Registry/ViewTest.cpp` (Task 13).

Headers modified: `Component/ResourceStorage.hpp`, `Reflection/FieldInfo.hpp`, `Component/ComponentRegistry.hpp`, `Core/TypeContext.hpp`, `Archetype/ArchetypeManager.hpp`, `Core/Result.hpp`, `Core/Delegate.hpp`, `Archetype/Archetype.hpp`, `Registry/View.hpp`, `Registry/Registry.hpp`, `Commands/CommandBuffer.hpp`, `Registry/Relations.hpp`, `Registry/ViewIterator.hpp`, `Core/Version.hpp`, plus `README.md`.

---

## Phase A — Theme-A guard sweep

### Task 1: ResourceStorage `Set<T>`/`Emplace<T>` — all-config guards

The templated resource API validates `id < MAX_COMPONENTS`, allocation success, and registry liveness with `ASTRA_ASSERT` only (unlike its by-ID sibling, fixed in 3.4). In Release: OOB `m_sparse` write, placement-new at `nullptr`, null `shared_ptr` deref.

**Files:**
- Create: `tests/Component/ResourceStorageGuardTest.cpp`
- Modify: `include/Astra/Component/ResourceStorage.hpp` (the `Set<T>` and `Emplace<T>` new-slot branches, ~`:130-244`)

**Interfaces:**
- Produces: `ResourceStorage::Set<T>`/`Emplace<T>` return `nullptr` (no throw, no assert) on `id >= MAX_COMPONENTS`, allocation failure, or expired registry, in all configs; `slot.isValid` is set only after storage is established (matching `SetByID`/`Deserialize`).

- [ ] **Step 1: Write the failing test**

Create `tests/Component/ResourceStorageGuardTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct GuardRes { int v = 7; }; }

// The forceable arm of Theme-A/A1: an over-MAX_COMPONENTS id must not OOB-write
// m_sparse. Registering enough distinct component types to exhaust the id space
// isn't practical here, so this test pins the graceful-rejection CONTRACT via the
// public Registry resource API on the normal path (regression guard that the
// guarded returns compile and behave), and the OOM/expired arms are covered by
// code inspection per the plan.
TEST(ResourceStorageGuard, SetAndGetRoundTripsOnNormalPath)
{
    Astra::Registry reg;
    reg.SetResource(GuardRes{42});
    auto* r = reg.GetResource<GuardRes>();
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->v, 42);
}
```

- [ ] **Step 2: Regenerate + build Release, verify it builds and passes on the normal path**

Run `D:/dev/_shared/tools/premake5 vs2022`, build Release, run `--gtest_filter=ResourceStorageGuard.*`. (This test is a regression guard; the defects it protects are the un-forceable OOM/exhaustion arms.)

- [ ] **Step 3: Apply the guards**

In `ResourceStorage.hpp`, in BOTH `Set<T>` and `Emplace<T>`, at the top of the new-slot allocation branch, replace the assert-only checks with all-config guards (mirror the shipped `SetByID` fix). Specifically: (a) before indexing `m_sparse[id]`, add `if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY return nullptr;`; (b) after `auto registry = m_componentRegistry.lock();` add `if (!registry) ASTRA_UNLIKELY return nullptr;` (do not proceed on an expired weak_ptr); (c) in the heap branch, after `AllocResult result = AllocateMemory(...);` add `if (!result.ptr) ASTRA_UNLIKELY return nullptr;` BEFORE the placement-new; (d) set `slot.isValid = true;` only after the inline/heap storage is fully established and the object constructed (move it below the if/else, exactly as `Deserialize`/`SetByID` now do). Read the current `SetByID` (`:~505-560`) and mirror its structure. Keep the return type (`T*`) and existing success behavior unchanged.

- [ ] **Step 4: Build Debug + Release, run the full suite**

`--gtest_filter=ResourceStorageGuard.*` passes; full suite green both configs (527 + 1). Confirm no `ASTRA_ASSERT` remains as the *sole* guard on those three conditions in `Set<T>`/`Emplace<T>`.

- [ ] **Step 5: Commit**

```bash
git add tests/Component/ResourceStorageGuardTest.cpp include/Astra/Component/ResourceStorage.hpp
git commit -m "fix(resource): all-config guards on Set/Emplace (id overflow, OOM, expired registry)"
```

### Task 2: Reflection `FieldInfo` accessors — all-config type/const guards

`Get<T>`/`Set<T>`/`GetPtr<T>` check the field type with `ASTRA_ASSERT` only, then blindly call the type-erased getter/setter → wrong-`T` differently-sized read/write in Release; `Set<T>` on a `const` field calls an empty `std::function` → `std::terminate()` under `-fno-exceptions`; `GetPtr<T>` never checks `isConst`.

**Files:**
- Create: `tests/Reflection/FieldAccessorSafetyTest.cpp`
- Modify: `include/Astra/Reflection/FieldInfo.hpp:72-123`

**Interfaces:**
- Produces: `Get<T>` returns `T{}` on type mismatch; `Set<T>` is a no-op on type mismatch or `isConst`; non-const `GetPtr<T>` returns `nullptr` when `isConst`; both `GetPtr<T>` overloads return `nullptr` on type mismatch — all in every config.

- [ ] **Step 1: Write the failing test**

Create `tests/Reflection/FieldAccessorSafetyTest.cpp` (use the project's reflection macros; adapt registration to the codebase's actual `ASTRA_REFLECT_TYPE` form after reading `tests/Reflection/ReflectionTest.cpp` for the pattern):

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace {
    struct RFields { int a = 5; const int b = 9; float c = 1.5f; };
}
// Register RFields with reflected fields a (int), b (const int), c (float)
// following tests/Reflection/ReflectionTest.cpp's exact macro usage.

TEST(FieldAccessorSafety, WrongTypeGetReturnsDefaultNotGarbage)
{
    RFields obj;
    const auto* meta = /* get TypeMeta<RFields> */;
    const auto* fa = meta->GetField("a");           // int field
    ASSERT_NE(fa, nullptr);
    // Ask for the wrong type: must not read a differently-sized object.
    double wrong = fa->Get<double>(&obj);
    EXPECT_EQ(wrong, double{});                       // value-initialized, not garbage
}

TEST(FieldAccessorSafety, SetOnConstFieldIsNoOp)
{
    RFields obj;
    const auto* fb = /* field "b" (const int) */;
    ASSERT_NE(fb, nullptr);
    fb->Set<int>(&obj, 123);                          // must NOT terminate; must be a no-op
    EXPECT_EQ(obj.b, 9);
}

TEST(FieldAccessorSafety, GetPtrOnConstFieldReturnsNull)
{
    RFields obj;
    const auto* fb = /* field "b" (const int) */;
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->GetPtr<int>(&obj), nullptr);        // no writable pointer into a const field
}
```

- [ ] **Step 2: Regenerate, build Release, verify failure**

`--gtest_filter=FieldAccessorSafety.*` — Expected before the fix: `SetOnConstFieldIsNoOp` crashes/terminates (empty `std::function` call) or mutates; `GetPtrOnConstFieldReturnsNull` returns non-null; `WrongTypeGetReturnsDefault...` may read garbage.

- [ ] **Step 3: Apply the guards** (`FieldInfo.hpp:72-123`)

```cpp
template<typename T>
ASTRA_NODISCARD T Get(const void* instance) const
{
    if (TypeID<T>::Hash() != typeHash || !getter) ASTRA_UNLIKELY
        return T{};
    T result{};
    getter(instance, &result);
    return result;
}

template<typename T>
void Set(void* instance, const T& value) const
{
    if (TypeID<T>::Hash() != typeHash || !setter || isConst) ASTRA_UNLIKELY
        return;                                        // no-op: never call an empty std::function
    setter(instance, &value);
}

template<typename T>
ASTRA_NODISCARD T* GetPtr(void* instance) const
{
    if (TypeID<T>::Hash() != typeHash || isConst) ASTRA_UNLIKELY
        return nullptr;
    return reinterpret_cast<T*>(static_cast<std::byte*>(instance) + offset);
}

template<typename T>
ASTRA_NODISCARD const T* GetPtr(const void* instance) const
{
    if (TypeID<T>::Hash() != typeHash) ASTRA_UNLIKELY
        return nullptr;
    return reinterpret_cast<const T*>(static_cast<const std::byte*>(instance) + offset);
}
```

Keep the `ASTRA_ASSERT`s too if desired (as Debug diagnostics) but they must no longer be the *only* guard.

- [ ] **Step 4: Build Debug + Release, run the filter (expect PASS) + full suite (green both configs).**

- [ ] **Step 5: Commit**

```bash
git add tests/Reflection/FieldAccessorSafetyTest.cpp include/Astra/Reflection/FieldInfo.hpp
git commit -m "fix(reflection): all-config type/const guards on FieldInfo Get/Set/GetPtr"
```

### Task 3: Component registration guards — alignment cap + TypeContext exhaustion

`ComponentRegistry`'s over-alignment cap (`:116-117`) and `TypeContext`'s id-exhaustion boundary (`:70-86`) are `ASTRA_ASSERT`-only; Release accepts an over-aligned component (which chunk storage then misaligns) and silently wraps the `uint16` id space.

**Files:**
- Create: `tests/Component/RegistrationGuardTest.cpp`
- Modify: `include/Astra/Component/ComponentRegistry.hpp:116-117`, `include/Astra/Core/TypeContext.hpp:70-86`

**Interfaces:**
- Produces: registering a component with `alignof(T) > CACHE_LINE_SIZE` is refused in all configs (same channel as the existing `MAX_COMPONENTS` refusal — returns the "not registered"/invalid id path, does not assign a descriptor); `TypeContext` id assignment refuses past the `uint16` limit in all configs.

- [ ] **Step 1: Write the failing test**

Create `tests/Component/RegistrationGuardTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct alignas(128) OverAligned { float v[4]; }; }  // > CACHE_LINE_SIZE (64)

TEST(RegistrationGuard, OverAlignedComponentIsRefusedNotMisaligned)
{
    Astra::Registry reg;
    // Registering an over-aligned component must be refused gracefully (no
    // descriptor with an alignment the chunk storage can't honor), in Release.
    auto* creg = reg.GetComponentRegistry();
    bool refused = !creg->RegisterComponent<OverAligned>();   // adapt to the actual
                                                              // return/queryable form
    EXPECT_TRUE(refused);
}
```

Read `ComponentRegistry::RegisterComponent`/`RegisterComponentImpl` (`:~95-135`) first to see the exact refusal channel used by the `MAX_COMPONENTS` guard, and assert through the same observable (e.g. `GetComponentDescriptor` returns null / id is the invalid sentinel). Adapt the test to that channel.

- [ ] **Step 2: Regenerate, build Release, verify failure** — the over-aligned type is currently accepted (test fails).

- [ ] **Step 3: Apply the guards**

In `ComponentRegistry.hpp`, next to where `desc.alignment` is assigned and beside the existing all-config `MAX_COMPONENTS` guard, add an all-config check (mirror that guard's exact refusal structure):

```cpp
if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
{
    // Chunk storage can only honor alignments up to CACHE_LINE_SIZE; refuse
    // rather than hand back a descriptor the storage will misalign.
    return /* the same "not registered" result the MAX_COMPONENTS guard returns */;
}
```

In `TypeContext.hpp:70-86`, convert the id-exhaustion `ASTRA_ASSERT` to an all-config guard that refuses assignment past the `uint16` boundary (mirror the ComponentRegistry pattern; return the invalid-id sentinel instead of wrapping).

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green.**

- [ ] **Step 5: Commit**

```bash
git add tests/Component/RegistrationGuardTest.cpp include/Astra/Component/ComponentRegistry.hpp include/Astra/Core/TypeContext.hpp
git commit -m "fix(component): all-config guards on over-alignment and type-id exhaustion"
```

### Task 4: ArchetypeManager batch-OOM — fail gracefully, don't abort

`ArchetypeManager.hpp:1167` does `ASTRA_ASSERT(false, "Failed to allocate chunks for batch move operation")` — aborting Debug on a *recoverable* allocation failure (the inverse mistake). It must degrade gracefully in all configs.

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp:~1160-1173` (the batch-move chunk-allocation failure path)

**Interfaces:**
- Produces: on chunk-allocation failure during a batch move, the operation returns/aborts the move gracefully (no `ASTRA_ASSERT(false)`), leaving prior state consistent.

- [ ] **Step 1: (Inspection-verified — OOM is not unit-forceable.)** Read `BatchMoveEntitiesInternal` around `:1160-1173` and confirm the `ASTRA_ASSERT(false, ...)` is on the `!chunk`/allocation-failure branch.

- [ ] **Step 2: Apply the fix** — replace the `ASTRA_ASSERT(false, "Failed to allocate chunks for batch move operation");` with a graceful bail: return from the function (or break out of the batch loop) without asserting, ensuring no partially-updated bookkeeping is left inconsistent (mirror how the sibling allocation-failure paths in this file already return). Add a brief comment that chunk-pool exhaustion is recoverable and must not abort.

- [ ] **Step 3: Build Debug + Release + Dist, full suite green (no test count change).** Note in the task report that the OOM branch is verified by inspection (not unit-forceable), consistent with the 3.4 policy.

- [ ] **Step 4: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "fix(archetype): batch-move chunk-alloc failure degrades gracefully, no assert-abort"
```

### Task 5: Core — `Result::operator*` Debug assert (A7) + `Delegate` copy-of-move-only → empty (A6)

`Result::operator*` has no guard at all (not even Debug, unlike `operator->`). Per the spec it stays Release-fast (matches `std::expected`) but gains the missing Debug assert. Separately, copying a `Delegate` that wraps a small **move-only** functor no-ops in Release (assert compiled out), leaving `m_storage` uninitialized while `m_invoker` claims validity.

**Files:**
- Create: `tests/Core/DelegateResultSafetyTest.cpp`
- Modify: `include/Astra/Core/Result.hpp:144-157`, `include/Astra/Core/Delegate.hpp` (`ManageSmallFunctor<F>` Copy op + copy ctor, `:52-96`)

**Interfaces:**
- Produces: `Result::operator*` gains `ASTRA_ASSERT(m_hasValue, ...)` in all three overloads (Debug-only effect), Release unchanged. Copying a `Delegate` whose stored functor is not copy-constructible yields an **empty** delegate (`explicit operator bool() == false`) in all configs.

- [ ] **Step 1: Write the failing test**

Create `tests/Core/DelegateResultSafetyTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <memory>

TEST(DelegateSafety, CopyOfMoveOnlyFunctorIsEmptyNotUninitialized)
{
    // A small functor capturing a move-only type (unique_ptr) — not copy-constructible.
    auto up = std::make_unique<int>(5);
    Astra::Delegate<void()> d([p = std::move(up)]() { /* uses p */ });
    ASSERT_TRUE(static_cast<bool>(d));

    Astra::Delegate<void()> copy(d);      // copying a move-only functor
    // Contract: rather than "valid but uninitialized storage", the copy is empty.
    EXPECT_FALSE(static_cast<bool>(copy));
    // And it must be safe to destroy/leave-scope with no UB (no invoke of garbage).
}
```

Adapt `Astra::Delegate<void()>` to the actual delegate type name/signature after reading `Delegate.hpp`. If the delegate is not default-constructible/empty-representable, use whatever the type's documented empty state is.

- [ ] **Step 2: Regenerate, build Release, verify failure** — in Release the copy currently reports `true` (claims valid) with uninitialized storage.

- [ ] **Step 3a: `Result::operator*`** — add the Debug assert to each overload:

```cpp
ASTRA_NODISCARD T& operator*() & noexcept
{
    ASTRA_ASSERT(m_hasValue, "Dereferencing Result with no value");
    return *m_storage.template As<T>();
}
// ...same ASTRA_ASSERT line added to the const& and && overloads.
```

- [ ] **Step 3b: `Delegate` copy of move-only small functor** — read `ManageSmallFunctor<F>` and the copy constructor (`:91-96`). Make the `Copy` manager op, for a non-copy-constructible `F`, leave the destination unconstructed and signal failure so the copy constructor sets `m_invoker = nullptr; m_manager = nullptr;` in all configs (an empty delegate). Concretely, gate the copy on `if constexpr (std::is_copy_constructible_v<F>)` inside `ManageSmallFunctor`'s Copy case: copy-construct when copyable; otherwise do nothing and have the copy ctor detect the non-copy (e.g. the manager returns/sets a flag, or the copy ctor checks a `constexpr`-exposed trait) and produce the empty state. Do NOT leave `m_invoker` set with unconstructed `m_storage`.

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green.**

- [ ] **Step 5: Commit**

```bash
git add tests/Core/DelegateResultSafetyTest.cpp include/Astra/Core/Result.hpp include/Astra/Core/Delegate.hpp
git commit -m "fix(core): Result::operator* debug assert; Delegate copy of move-only functor yields empty"
```

---

## Phase B — Ordinary-Release correctness bugs

### Task 6: ArchetypeManager root-archetype round-trip (B1)

`Serialize` skips the root (zero-component) archetype and `Deserialize` never repopulates it, so an entity created with no components becomes a dangling entity-map entry after Save/Load — invisible to iteration, and UB (count underflow + OOB swap-and-pop) on a later `DestroyEntity`.

**Files:**
- Create: `tests/Registry/RootArchetypeRoundTripTest.cpp`
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`Serialize` `:~690-745`, `Deserialize` `:~765-815`)

**Interfaces:**
- Produces: zero-component entities survive Save/Load and can be safely destroyed afterward.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/RootArchetypeRoundTripTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

TEST(RootArchetypeRoundTrip, ZeroComponentEntitySurvivesSaveLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    Astra::Entity e = reg.CreateEntity();          // no components -> root archetype
    ASSERT_TRUE(reg.IsValid(e));

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg2 = **loaded.GetValue();

    EXPECT_TRUE(reg2.IsValid(e));                  // was dangling before the fix
    reg2.DestroyEntity(e);                          // must not underflow / OOB
    EXPECT_FALSE(reg2.IsValid(e));
}
```

Adapt `reg.CreateEntity()`/`IsValid`/`DestroyEntity`/`Load` deref to the actual public API (read `Registry.hpp` + an existing serialization test such as `tests/Registry/RegistrySerializationTest.cpp`).

- [ ] **Step 2: Regenerate, build Debug, verify failure** — `reg2.IsValid(e)` false, or the `DestroyEntity` asserts/corrupts.

- [ ] **Step 3: Fix Serialize + Deserialize** — read the current `Serialize`/`Deserialize`. In `Serialize`, stop skipping the root archetype: include it in the archetype set that is written (with its entity count and entity→archetype mappings), like any other archetype. In `Deserialize`, repopulate the root archetype's entities and their map entries symmetrically. Ensure the root archetype's descriptor set (empty) round-trips and the entity-count/mapping bookkeeping matches the non-root path. Preserve all existing bounds checks.

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green (527 + 1).**

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/RootArchetypeRoundTripTest.cpp include/Astra/Archetype/ArchetypeManager.hpp
git commit -m "fix(archetype): serialize/deserialize the root archetype so zero-component entities round-trip"
```

### Task 7: Archetype large-component chunk fit-check (B2)

`Archetype::Initialize` (`Archetype.hpp:157-161`) clamps `entitiesPerChunk` to 1 when a single entity's footprint exceeds the usable chunk space, with no verification that one entity fits — then writing that entity's components overflows into the neighboring chunk.

**Files:**
- Create: `tests/Registry/LargeComponentTest.cpp`
- Modify: `include/Astra/Archetype/Archetype.hpp:150-174` (Initialize), and the Deserialize fit-check (`:~800-838`)

**Interfaces:**
- Produces: a component whose single-entity footprint exceeds the usable chunk space causes graceful failure (the archetype does not initialize / entity-adds to it fail; Deserialize returns `Result::Err`) rather than a heap overflow.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/LargeComponentTest.cpp`. First read `ArchetypeChunkPool::DEFAULT_CHUNK_SIZE` to size the component above one chunk:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <array>

namespace {
    // Larger than a single chunk (DEFAULT_CHUNK_SIZE is 16 KiB) so one entity
    // cannot possibly fit — must fail gracefully, never overflow.
    struct HugeComp { std::array<std::byte, 32 * 1024> bytes{}; };
}

TEST(LargeComponent, OversizedComponentFailsGracefullyNoOverflow)
{
    Astra::Registry reg;
    // Creating an entity with a component too big for a chunk must NOT corrupt
    // memory. Acceptable outcomes: the create fails / the entity has no such
    // component / a Result::Err is surfaced — but never a heap overflow.
    Astra::Entity e = reg.CreateEntity<HugeComp>();
    // If creation "succeeds", the component must be safely accessible or absent,
    // and neighboring memory must be intact (the suite running clean under this
    // registration is the primary signal; add an explicit expectation for the
    // chosen graceful outcome after deciding it in Step 3).
    SUCCEED();
}
```

- [ ] **Step 2: Regenerate, build Release, run under scrutiny** — before the fix this can corrupt the heap (may crash intermittently). Document the observed pre-fix behavior.

- [ ] **Step 3: Add the fit-check** — in `Archetype::Initialize`, after computing `perEntitySize`/`remainingSpace`, if `perEntitySize > remainingSpace` (one entity cannot fit), do not clamp-and-proceed: leave `m_initialized = false` (and do not create a chunk), so the archetype cannot accept entities. Surface this so callers handle it (the Registry create path should treat an un-initializable archetype as a graceful failure — read the caller to wire the outcome; at minimum, no chunk is created and no component bytes are written). In `Archetype::Deserialize`, add the symmetric check and return `Result::Err(SerializationError::SizeMismatch)` when a stored archetype's `perEntitySize` exceeds the load-time chunk capacity. Decide the exact caller-visible outcome and tighten the test's expectation accordingly.

- [ ] **Step 4: Build Debug + Release + Dist, filter PASS + full suite green.**

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/LargeComponentTest.cpp include/Astra/Archetype/Archetype.hpp
git commit -m "fix(archetype): reject components too large for a chunk instead of overflowing"
```

### Task 8: View::Size()/Empty() refresh (B3)

`View::Size()`/`Empty()` (`View.hpp:143-156`) iterate `m_archetypes` without the `EnsureArchetypes()` refresh that `ForEach`/`begin()` run — so after `Defragment()` frees a cached archetype they dereference freed memory.

**Files:**
- Modify: `include/Astra/Registry/View.hpp:143-156`
- Test: append to `tests/Registry/ViewInvalidationTest.cpp`

**Interfaces:**
- Consumes: existing `View::EnsureArchetypes()`.
- Produces: `Size()`/`Empty()` refresh before reading `m_archetypes`; both are safe and consistent with iteration after structural changes.

- [ ] **Step 1: Write the failing test** — append to `tests/Registry/ViewInvalidationTest.cpp` (match its existing `TEST`/fixture style and its `VPos`/`VVel` component pattern):

```cpp
TEST(ViewInvalidation, SizeIsSafeAndCorrectAfterDefragment)
{
    Astra::Registry reg;
    auto a = reg.CreateEntity<VPos>();
    auto b = reg.CreateEntity<VPos, VVel>();
    auto view = reg.CreateView<VPos>();
    EXPECT_EQ(view.Size(), 2u);

    reg.DestroyEntity(a);
    reg.DestroyEntity(b);
    Astra::Registry::DefragmentationOptions opts;
    opts.minArchetypesToKeep = 1;
    reg.Defragment(opts);

    // Before the fix: Size() dereferences a freed Archetype* (UAF).
    EXPECT_EQ(view.Size(), 0u);
    EXPECT_TRUE(view.Empty());
}
```

Adapt `DefragmentationOptions`/`Defragment` to the exact API used by the existing `SurvivesArchetypeRemoval` test in the same file.

- [ ] **Step 2: Regenerate, build Debug, verify failure/UAF** (may pass by luck since it's freed-memory; keep it regardless — run Release too).

- [ ] **Step 3: Fix** — make `Size()` (and therefore `Empty()`, which the 3.4 fix already routes through `Size()`) call the refresh before iterating. Since `Size()`/`Empty()` are `const`, mark the refresh path appropriately (the members it updates — `m_archetypes`, refresh counters — are already mutated from other const-ish contexts; mirror how `ForEach`/`begin()` invoke `EnsureArchetypes()`; if those are non-const, make `Size()`/`Empty()` non-const *only if the codebase allows*, otherwise add `mutable`/a `const`-callable refresh consistent with existing patterns). Read `View::ForEach`/`begin()` to match the exact refresh call.

```cpp
ASTRA_NODISCARD size_t Size() /* const? match ForEach's contract */ noexcept
{
    EnsureArchetypes();                 // refresh cached Archetype* before reading
    size_t total = 0;
    for (Archetype* a : m_archetypes) total += a->GetEntityCount();
    return total;
}
ASTRA_NODISCARD bool Empty() noexcept { return Size() == 0; }
```

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green (527 + 1).**

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/ViewInvalidationTest.cpp include/Astra/Registry/View.hpp
git commit -m "fix(view): Size()/Empty() refresh cached archetypes; no UAF after Defragment"
```

### Task 9: Registry::Clear() in-place + ArchetypeManager::Clear() (B5)

`Registry::Clear()` (`Registry.hpp:1004-1017`) replaces the `ArchetypeManager` with a fresh `shared_ptr`, orphaning every pre-existing `View`/`Relations` (which keep pointing at the old manager and freeze on stale data). `ArchetypeManager` has no in-place `Clear()`.

**Files:**
- Create: `tests/Registry/ClearOrphansViewTest.cpp`
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (add `Clear()`), `include/Astra/Registry/Registry.hpp:1004-1017`

**Interfaces:**
- Produces: `void ArchetypeManager::Clear()` — removes all archetypes/entities, re-establishes the root archetype (as the constructor does), and bumps `m_structuralChangeCounter` + `m_archetypeRemovalCounter`. `Registry::Clear()` calls it instead of replacing the manager, so cached views refresh to empty.

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/ClearOrphansViewTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct CPos { float x, y, z; }; }

TEST(ClearOrphansView, CachedViewRefreshesToEmptyAfterClear)
{
    Astra::Registry reg;
    reg.CreateEntity<CPos>();
    reg.CreateEntity<CPos>();
    auto view = reg.CreateView<CPos>();
    EXPECT_EQ(view.Size(), 2u);

    reg.Clear();

    // Before the fix: the view points at the discarded manager and still reports 2.
    size_t seen = 0;
    view.ForEach([&](Astra::Entity, CPos&) { ++seen; });
    EXPECT_EQ(seen, 0u);
    EXPECT_EQ(view.Size(), 0u);
}
```

- [ ] **Step 2: Regenerate, build Debug, verify failure** — the view still reports 2 / iterates stale data.

- [ ] **Step 3: Add `ArchetypeManager::Clear()`** — read the ArchetypeManager constructor to see exactly how it initializes (`m_archetypes`, the entity map, the root archetype, `m_generation`, chunk pool). Add:

```cpp
void Clear()
{
    // Reset to the freshly-constructed state, keeping the same component
    // registry and chunk-pool config, then signal views that everything
    // changed and pointers may be stale so they fully re-collect.
    /* clear m_entityMap / archetype containers; destroy archetypes;
       re-create the root archetype exactly as the constructor does;
       reset m_generation to its initial value */
    m_structuralChangeCounter.fetch_add(1, std::memory_order_release);
    m_archetypeRemovalCounter.fetch_add(1, std::memory_order_release);
}
```

Fill the body by mirroring the constructor's initialization (the implementer has the ctor in front of them). Then change `Registry::Clear()`:

```cpp
void Clear()
{
    m_archetypeManager->Clear();     // in place — keeps cached Views/Relations valid
    m_relationshipGraph->Clear();
    m_entityManager.Clear();
}
```

(Remove the `m_archetypeManager = std::make_shared<...>()` replacement and the dead `IsSignalEnabled` TODO block, or keep the block if signals are wanted — but do not replace the manager object.)

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green (527 + 1).** Pay attention to `tests/Registry/RegistryLoadConfigTest.cpp` and any Clear/Load config test (the 3.4 "Clear() honors the configured chunk pool" behavior must be preserved — the in-place Clear must keep using `m_config.chunkPoolConfig`).

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/ClearOrphansViewTest.cpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp
git commit -m "fix(registry): Clear() resets the ArchetypeManager in place so cached views stay valid"
```

### Task 10: CommandBuffer partial rollback (B4)

`RollbackAllocatedEntities` (`CommandBuffer.hpp:773-784`) destroys every entity in `m_allocatedEntities`, including ones already committed to an archetype earlier in the same `Execute()` — orphaning live archetype rows. It must roll back only the *uncommitted* remainder.

**Files:**
- Modify: `include/Astra/Commands/CommandBuffer.hpp` (`Execute` commit tracking + `RollbackAllocatedEntities` `:773-784`)
- Test: append to `tests/Commands/CommandBufferTest.cpp`

**Interfaces:**
- Produces: `RollbackAllocatedEntities` destroys only entities allocated-but-not-yet-committed to an archetype; committed entities survive an aborted `Execute()`.

- [ ] **Step 1: Read `Execute()`** to find where allocated entities get committed into archetypes and where a mid-Execute failure can trigger `RollbackAllocatedEntities`. Determine how to induce a partial failure in a test (e.g. a command that fails after an earlier create+add succeeded). If a partial failure is not inducible from the public API, record that and fix by inspection (like the OOM cases), still adding the tracking.

- [ ] **Step 2: Write the test** (append to `tests/Commands/CommandBufferTest.cpp`, matching its style) — record a CommandBuffer that creates entity X and adds a component (committing X), then a later command that fails Execute; assert X remains valid after the failed Execute. Adapt to whatever failure the buffer can actually surface; if none is inducible, replace with a white-box test of the tracking (e.g. that entities marked committed are excluded from the rollback set).

- [ ] **Step 3: Fix** — track committed entities during `Execute()`: as each recorded-created entity is placed into an archetype, mark it committed (e.g. advance a `m_committedCount` over `m_allocatedEntities`, or move committed ids out of the rollback set). Change `RollbackAllocatedEntities` to destroy only the uncommitted remainder:

```cpp
void RollbackAllocatedEntities()
{
    if (m_registry)
    {
        auto& manager = m_registry->GetEntityManager();
        // Only entities allocated but NOT yet committed to an archetype are
        // rolled back; committed entities are live rows and must survive.
        for (size_t i = m_committedCount; i < m_allocatedEntities.size(); ++i)
            manager.Destroy(m_allocatedEntities[i]);
    }
    m_allocatedEntities.clear();
    m_committedCount = 0;
}
```

Add the `m_committedCount` member (or equivalent) and update it in `Execute()` at the commit point. Ensure the ordering of `m_allocatedEntities` matches commit order.

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green.** Record test approach + result (or the inspection note) in the task report.

- [ ] **Step 5: Commit**

```bash
git add tests/Commands/CommandBufferTest.cpp include/Astra/Commands/CommandBuffer.hpp
git commit -m "fix(commands): Execute rollback destroys only uncommitted entities"
```

---

## Phase C — Iteration safety

### Task 11: MulticastDelegate::Invoke snapshot (C1)

`MulticastDelegate::Invoke` (`Delegate.hpp:391-412`) iterates the member `m_handlers` with a range-for; a handler that registers/unregisters (a Signal listener removing itself) during dispatch invalidates the iteration → UAF / null call.

**Files:**
- Create: `tests/Registry/IterationSafetyTest.cpp` (shared with Task 12)
- Modify: `include/Astra/Core/Delegate.hpp:391-412`

**Interfaces:**
- Produces: `MulticastDelegate::Invoke` dispatches over a stable snapshot; a handler may register/unregister during dispatch without UB. Newly-registered handlers are not invoked in the same dispatch; unregistered handlers already snapshotted still run for this dispatch (documented).

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/IterationSafetyTest.cpp`. Drive this through the public Signal API if a self-unregistering handler is expressible there (read `Core/Signal.hpp` + `tests/Registry/SignalLifetimeTest.cpp`); otherwise test `MulticastDelegate` directly:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

TEST(IterationSafety, HandlerUnregisteringItselfDuringDispatchIsSafe)
{
    Astra::MulticastDelegate<void()> mc;      // adapt type/API to the real one
    int calls = 0;
    // A handler that removes a handler from mc during Invoke must not UAF.
    auto h = mc.Register([&]{ ++calls; mc.Clear(); });   // adapt Register/Clear/Unregister
    (void)h;
    mc.Register([&]{ ++calls; });
    mc.Invoke();                               // must not crash / read freed handlers
    SUCCEED();
}
```

- [ ] **Step 2: Regenerate, build Debug + Release, verify failure** (crash / ASan-style UAF, or missed/duplicated calls).

- [ ] **Step 3: Fix both `Invoke` overloads** (`:391-412`) to iterate a snapshot:

```cpp
template<typename U = R>
std::enable_if_t<std::is_void_v<U>> Invoke(Args... args) const
{
    auto handlers = m_handlers;             // snapshot: handlers may (un)register during dispatch
    for (const auto& handler : handlers)
        handler.delegate(std::forward<Args>(args)...);
}

template<typename U = R>
std::enable_if_t<!std::is_void_v<U>, SmallVector<R, 4>> Invoke(Args... args) const
{
    auto handlers = m_handlers;
    SmallVector<R, 4> results;
    results.reserve(handlers.size());
    for (const auto& handler : handlers)
        results.push_back(handler.delegate(std::forward<Args>(args)...));
    return results;
}
```

(Confirm `m_handlers`'s element type is copyable — it holds `Delegate`s + ids; with Task 5's fix, `Delegate` copy is well-defined.)

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green (527 + 1).**

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/IterationSafetyTest.cpp include/Astra/Core/Delegate.hpp
git commit -m "fix(core): MulticastDelegate::Invoke dispatches over a snapshot (safe self-unregister)"
```

### Task 12: Relations::ForEach* snapshot (C2)

`Relations::ForEachChild`/`ForEachLink` pass a live reference into the graph's `m_children`/`m_links`, and `ForEachDescendant`/`ForEachAncestor` hold a live `const TraversalCache&`, across the user callback — so "destroy each child" (swap-and-pop / rehash mid-iteration) is a UAF.

**Files:**
- Modify: `include/Astra/Registry/Relations.hpp:132-195`
- Test: append to `tests/Registry/IterationSafetyTest.cpp`

**Interfaces:**
- Produces: `ForEachChild`/`ForEachDescendant`/`ForEachAncestor`/`ForEachLink` iterate a local snapshot; a callback that destroys the iterated entities (or otherwise mutates the graph) is safe.

- [ ] **Step 1: Write the failing test** — append to `tests/Registry/IterationSafetyTest.cpp`:

```cpp
TEST(IterationSafety, DestroyEachChildDuringForEachIsSafe)
{
    Astra::Registry reg;
    auto parent = reg.CreateEntity();
    for (int i = 0; i < 8; ++i) {                 // > inline capacity to force heap promotion
        auto c = reg.CreateEntity();
        reg.SetParent(c, parent);                  // adapt to the real relationship API
    }
    size_t destroyed = 0;
    reg.GetRelations(parent).ForEachChild([&](Astra::Entity child /*, comps... */) {
        reg.DestroyEntity(child);                  // mutates m_children mid-iteration
        ++destroyed;
    });
    EXPECT_EQ(destroyed, 8u);                      // all visited, no UAF, none skipped
}
```

Adapt `SetParent`/`GetRelations(parent).ForEachChild`/the callback signature to the real API (read `Relations.hpp` + `tests/Registry/RelationsTest.cpp`).

- [ ] **Step 2: Regenerate, build Debug + Release, verify failure** (skipped children and/or heap-UAF once children spill past inline capacity).

- [ ] **Step 3: Fix** — snapshot before the callback loop. In `ForEachChild`/`ForEachLink`, copy the container into a local before passing to the Impl:

```cpp
template<typename Func>
ASTRA_FORCEINLINE void ForEachChild(Func&& func)
{
    if (!m_relationsGraph) ASTRA_UNLIKELY return;
    auto children = m_relationsGraph->GetChildren(m_rootEntity);   // snapshot (value copy)
    if (children.empty()) ASTRA_UNLIKELY return;
    ForEachChildImpl(children, std::forward<Func>(func), RequiredTuple{});
}
```

Apply the same `auto x = ...;` snapshot to `ForEachLink` (`GetLinks`), and to `ForEachDescendant`/`ForEachAncestor` copy the cache (`auto cache = m_relationsGraph->GetDescendantsCached(m_rootEntity);` — a value copy of the `TraversalCache`) before reading `cache.entries`. Leave `ParallelForEachDescendant` as-is for now (its concurrency story is deferred to a later phase; note it in the report).

- [ ] **Step 4: Build Debug + Release, filter PASS + full suite green (527 + 1).**

- [ ] **Step 5: Commit**

```bash
git add tests/Registry/IterationSafetyTest.cpp include/Astra/Registry/Relations.hpp
git commit -m "fix(relations): snapshot children/links/cache before ForEach callbacks (safe destroy-each-child)"
```

### Task 13: View iteration — document + Debug reentrancy assert (C3)

View iteration can't be cheaply snapshotted (hot path). Per the spec: document that structural mutation during `ForEach` is unsupported and must be deferred through a `CommandBuffer`, and add a Debug-only reentrancy assert.

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (doc comment on `ForEach` + a Debug reentrancy guard), `include/Astra/Registry/ViewIterator.hpp` if the guard lives there
- Test: append a Debug-only reentrancy test to `tests/Registry/ViewTest.cpp`
- Modify: `README.md` (one line in the CommandBuffer / iteration section pointing to the deferred-mutation pattern)

**Interfaces:**
- Produces: a documented contract ("no structural mutation during `ForEach`; defer via `CommandBuffer::Execute()` after the loop") + a Debug-only assert if a structural change is detected mid-iteration (via the existing structural-change counter).

- [ ] **Step 1: Add the doc comment** above `View::ForEach` stating the contract and the CommandBuffer-deferred remedy.

- [ ] **Step 2: Add a Debug-only reentrancy/mutation assert** — capture `m_archetypeManager->m_structuralChangeCounter` at the start of `ForEach` and `ASTRA_ASSERT` (Debug-only) it is unchanged when the loop finishes, with a message directing callers to the CommandBuffer. This adds zero Release cost. (Read `EnsureArchetypes` for how the counter is already accessed.)

- [ ] **Step 3: Write a Debug-only test** (append to `tests/Registry/ViewTest.cpp`), guarded so it only runs where `ASTRA_ASSERT` is active, asserting that a normal (non-mutating) `ForEach` does not trip the guard. (Death-testing the positive trip is optional and platform-sensitive; a non-trip test plus the doc is sufficient.)

- [ ] **Step 4: README** — add one line to the iteration/CommandBuffer section: "To change entity structure while iterating a View, record the changes into a `CommandBuffer` and call `Execute()` after the loop."

- [ ] **Step 5: Build Debug + Release + Dist, full suite green.**

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/View.hpp include/Astra/Registry/ViewIterator.hpp tests/Registry/ViewTest.cpp README.md
git commit -m "docs(view): document no-structural-mutation-during-ForEach + CommandBuffer remedy; debug reentrancy assert"
```

---

## Phase D — Release

### Task 14: Version bump 3.4.1 + full verification

**Files:**
- Modify: `include/Astra/Core/Version.hpp:3-5`

- [ ] **Step 1: Bump the patch version**

```cpp
#define ASTRA_VERSION_MAJOR 3
#define ASTRA_VERSION_MINOR 4
#define ASTRA_VERSION_PATCH 1
```

- [ ] **Step 2: Full local gate** — regenerate; build **Debug, Release, AND Dist** (whole solution); run `AstraTest` in all three (all green); confirm `AstraCompile16`/`AstraCompile64` build in all three. Re-run the Phase-A/B/C filters once more together. Confirm `BINARY_FORMAT_VERSION` is still 2 (no format change).

- [ ] **Step 3: Commit**

```bash
git add include/Astra/Core/Version.hpp
git commit -m "chore: bump Astra to 3.4.1 (release-safety remediation)"
```

---

## After all tasks

Run the FINAL whole-branch review on the most capable model (`review-package $(git merge-base dev HEAD) HEAD`, requesting-code-review template), pointed at this plan and the review of record. Triage any findings (one consolidated fix subagent for Critical/Important). Then `superpowers:finishing-a-development-branch` — merge target is `dev`.

## Deferred (explicitly out of scope for 3.4.1 — see the spec's Out of Scope)

Theme B (concurrency/thread-safety), Theme C (`Registry::Load` trust boundary + fuzz harness), Theme D (alignment plumbing), Theme E (TypeID anonymous-namespace collision), Theme H (32-bit/`hash>>57`, MSVC SIMD detection, non-standard entity widths), Theme J (Registry API footguns), Theme G (remove/move leaks), and performance items. Each is a later round with its own spec → plan → execution cycle.
