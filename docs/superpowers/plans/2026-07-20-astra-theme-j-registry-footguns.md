# Theme J — Registry/View Public-API Footgun Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix four Registry/View public-API footguns (silent create no-op, a copy-shaped ctor that yields an empty registry, batch `ComponentAdded` with a null payload, and a `View(nullptr)` crash).

**Architecture:** Four small, local edits to `Registry.hpp`/`View.hpp`, each with a RED→GREEN regression test appended to an existing test file. Two are deliberate API changes (create-batch return type; `Registry` becomes non-copyable); two are mechanical. No new component types.

**Tech Stack:** Header-only C++20 archetype ECS (MSVC, x64). GoogleTest. Build via `Astra.sln` (premake5-generated).

## Global Constraints

- **All three configs must build clean and green:** Debug, Release, Dist (`-p:Platform=x64`). Gate on "all configs green + intended new tests," not an absolute count.
- **Uniform-graceful misuse policy:** no assert-and-abort on a recoverable condition.
- **No new component types / TypeIDs:** reuse `Astra::Test::Position`/`Velocity`. The test binary is near the `MAX_COMPONENTS = 128` ceiling.
- **No `ide/` regen:** all tests are **appended to existing test files** (`tests/Registry/RegistryTest.cpp`, `tests/Registry/ViewTest.cpp`). Appending to an existing `.cpp` needs no premake regen. Do NOT `git add ide/`.
- **Intended API changes only:** `CreateEntities`/`CreateEntitiesWith` return `size_t`; `Registry` becomes non-copyable with a new `ShareComponentRegistry()` accessor. No other public signature changes.
- **Build (Debug example):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m`
- **Test exe:** `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (swap `Debug` for `Release`/`Dist`). Filter with `--gtest_filter=`.
- **IDE clang diagnostics are false positives — judge only by the MSVC build.** On a stale-link/PDB error after an interrupted build: `taskkill //F //IM mspdbsrv.exe` then rebuild.

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `include/Astra/Registry/Registry.hpp` | Registry public API | Fix 1 (create-batch return), Fix 2 (non-copyable + share accessor), Fix 3 (batch signal pointer) |
| `include/Astra/Registry/View.hpp` | Query view | Fix 4 (null-manager ctor guard) |
| `tests/Registry/RegistryTest.cpp` | Registry tests | Fix 1, 2, 3 tests |
| `tests/Registry/ViewTest.cpp` | View tests | Fix 4 test |

---

### Task 1: Fix 1 — `CreateEntities` / `CreateEntitiesWith` return the created count

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (`CreateEntities` ~`143-178`, `CreateEntitiesWith` ~`181-211`)
- Test: `tests/Registry/RegistryTest.cpp` (append `TEST_F`)

**Interfaces:**
- Produces: `size_t Registry::CreateEntities<Components...>(size_t count, std::span<Entity>)` and `size_t Registry::CreateEntitiesWith<Components..., Generator>(size_t count, std::span<Entity>, Generator&&)` — return the number of entities actually created (`0` on too-small span, `≤count` on id-exhaustion).

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Registry/RegistryTest.cpp`:

```cpp
// Theme J Fix 1: CreateEntities/CreateEntitiesWith report the number actually created.
TEST_F(RegistryTest, CreateEntitiesReturnsCreatedCount)
{
    using namespace Astra::Test;

    // Adequate span -> returns count, creates them.
    std::vector<Astra::Entity> ents(5);
    size_t n = registry->CreateEntities<Position>(5, ents);
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(registry->Size(), 5u);

    // Too-small span -> returns 0, creates nothing.
    std::vector<Astra::Entity> tooSmall(2);
    size_t before = registry->Size();
    size_t n2 = registry->CreateEntities<Position>(5, tooSmall);
    EXPECT_EQ(n2, 0u);
    EXPECT_EQ(registry->Size(), before);

    // CreateEntitiesWith too-small span -> returns 0.
    size_t n3 = registry->CreateEntitiesWith<Position>(5, tooSmall,
        [](size_t i) { return std::make_tuple(Position{float(i), 0.0f, 0.0f}); });
    EXPECT_EQ(n3, 0u);
}
```

- [ ] **Step 2: Build Debug and confirm it FAILS (compile error — old signature is `void`)**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: BUILD FAILS — cannot initialize `size_t` from a `void`-returning `CreateEntities`/`CreateEntitiesWith`. (This is the RED signal for a return-type change.)

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/Registry.hpp`, change `CreateEntities`'s signature and return statements. The function currently starts:
```cpp
        template<Component... Components>
        void CreateEntities(size_t count, std::span<Entity> outEntities)
        {
            if (count == 0 || outEntities.size() < count)
                return;
            
            size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
            for (size_t i = created; i < count; ++i)
            {
                outEntities[i] = Entity::Invalid();
            }
            if (created == 0) ASTRA_UNLIKELY
                return;
```
Change the signature to `size_t CreateEntities(...)`, and the two bare `return;` above to `return 0;`. Then, at the very end of the function body (after the `ComponentAdded` block, before the closing `}`), add:
```cpp
            return created;
```

Apply the identical transformation to `CreateEntitiesWith`: signature `void`→`size_t`; the `if (count == 0 || outEntities.size() < count) return;` → `return 0;`; the `if (created == 0) ... return;` → `return 0;`; and add `return created;` as the last statement of the body.

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.CreateEntitiesReturnsCreatedCount
```
Expected: PASS.

- [ ] **Step 5: Run the existing batch tests to confirm no regression**

Run:
```
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.*
```
Expected: all pass (the pre-existing `BatchOperationsPerformance` etc. call these functions and ignore the new return — still compiles and passes).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "feat(registry): CreateEntities/CreateEntitiesWith return created count (Theme J)"
```

---

### Task 2: Fix 2 — `Registry` non-copyable + explicit `ShareComponentRegistry()`

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (remove copy-shaped ctor ~`78-86`; add deleted copy ops; add accessor near `GetComponentRegistry` ~`1021`)
- Test: `tests/Registry/RegistryTest.cpp` (replace the misleading `CopyConstructor` test ~`526-547`)

**Interfaces:**
- Produces: `Registry` is non-copyable (`Registry(const Registry&) = delete; Registry& operator=(const Registry&) = delete;`) and exposes `std::shared_ptr<ComponentRegistry> Registry::ShareComponentRegistry() const noexcept`. The existing `Registry(std::shared_ptr<ComponentRegistry>, Config)` ctor is the supported "share component registrations" path.

- [ ] **Step 1: Write the failing test (replace the old `CopyConstructor` test)**

In `tests/Registry/RegistryTest.cpp`, DELETE the existing test (it uses the ctor being removed):
```cpp
// Test copy constructor
TEST_F(RegistryTest, CopyConstructor)
{
    using namespace Astra::Test;
    registry->GetComponentRegistry()->RegisterComponents<Position, Velocity>();
    Astra::Registry::Config config;
    Astra::Registry copy(*registry, config);
    EXPECT_EQ(copy.GetComponentRegistry(), registry->GetComponentRegistry());
    Astra::Entity entity = copy.CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    EXPECT_TRUE(copy.IsValid(entity));
    Position* pos = copy.GetComponent<Position>(entity);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
}
```
and replace it with:
```cpp
// Theme J Fix 2: Registry is non-copyable; component registrations are shared explicitly.
TEST_F(RegistryTest, SharesComponentRegistryNotState)
{
    using namespace Astra::Test;

    static_assert(!std::is_copy_constructible_v<Astra::Registry>, "Registry must be non-copyable");
    static_assert(!std::is_copy_assignable_v<Astra::Registry>, "Registry must be non-copy-assignable");

    // Original has an entity; a shared-registry world must NOT inherit its state.
    Astra::Entity original = registry->CreateEntityWith(Position{9.0f, 0.0f, 0.0f});
    ASSERT_TRUE(registry->IsValid(original));

    Astra::Registry::Config config;
    Astra::Registry world2(registry->ShareComponentRegistry(), config);

    // Shares the component registry (same ComponentID space)...
    EXPECT_EQ(world2.GetComponentRegistry(), registry->GetComponentRegistry());
    // ...but has independent entity state.
    EXPECT_FALSE(world2.IsValid(original));
    EXPECT_EQ(world2.Size(), 0u);

    // world2 can create entities with the shared registrations.
    Astra::Entity e = world2.CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(world2.IsValid(e));
    Position* pos = world2.GetComponent<Position>(e);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->x, 1.0f);
}
```

- [ ] **Step 2: Build Debug and confirm it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: BUILD FAILS — the `static_assert(!std::is_copy_constructible_v<Astra::Registry>)` fails (the current `(const Registry&, Config=)` ctor is a copy constructor), and `ShareComponentRegistry` does not yet exist. (RED for this API change.)

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/Registry.hpp`, REMOVE the copy-shaped constructor:
```cpp
        explicit Registry(const Registry& other, const Config& config = {}) :
            m_entityManager(config.entityManagerConfig),
            m_componentRegistry(other.m_componentRegistry),
            m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig)),
            m_relationshipGraph(std::make_shared<RelationshipGraph>()),
            m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),
            m_workScheduler(config.workScheduler),
            m_config(config)
        {}
```
and replace it (same location, just before `~Registry() = default;`) with explicit deleted copy operations:
```cpp
        // A Registry is a heavy, stateful container: copying is not supported (the old
        // (const Registry&, Config) ctor looked like a copy but silently produced an empty
        // registry). To create a second world that shares this one's component-type
        // registrations, use ShareComponentRegistry() with the shared_ptr ctor.
        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
```

Then add the accessor next to `GetComponentRegistry()` (search for `ASTRA_NODISCARD ComponentRegistry* GetComponentRegistry() noexcept`) — add immediately after that pair:
```cpp
        ASTRA_NODISCARD std::shared_ptr<ComponentRegistry> ShareComponentRegistry() const noexcept { return m_componentRegistry; }
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.SharesComponentRegistryNotState
```
Expected: PASS.

- [ ] **Step 5: Run the full RegistryTest suite to confirm nothing else used the removed ctor**

Run:
```
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.*
```
Expected: all pass. (If the build fails elsewhere, some code used the removed ctor — report it; per the spec, only the test used it.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "feat(registry): make Registry non-copyable + add ShareComponentRegistry (Theme J)"
```

---

### Task 3: Fix 3 — batch create emits `ComponentAdded` with the real component pointer

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (`CreateEntities` and `CreateEntitiesWith` `ComponentAdded` emission)
- Test: `tests/Registry/RegistryTest.cpp` (append `TEST_F`)

**Interfaces:**
- Consumes: `m_archetypeManager->GetEntityRecord(Entity)` and `record->archetype->GetComponent<Components>(record->location)` (the same pattern single-entity `CreateEntity` already uses); the `Astra::Events::ComponentAdded` event has fields `{ Entity entity; ComponentID componentId; void* component; }`.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Registry/RegistryTest.cpp`:

```cpp
// Theme J Fix 3: batch create emits ComponentAdded with the real component pointer, not null.
TEST_F(RegistryTest, BatchCreateEmitsRealComponentPointer)
{
    using namespace Astra::Test;

    registry->EnableSignals(Astra::Signal::ComponentAdded);
    auto* signals = registry->GetSignalManager();

    void* captured = nullptr;
    float capturedX = -1.0f;
    auto handler = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& e)
        {
            captured = e.component;
            if (e.component)
                capturedX = static_cast<Position*>(e.component)->x;
        });

    std::vector<Astra::Entity> ents(3);
    registry->CreateEntitiesWith<Position>(3, ents,
        [](size_t i) { return std::make_tuple(Position{float(i) + 10.0f, 0.0f, 0.0f}); });

    EXPECT_NE(captured, nullptr);   // BUG passes nullptr
    EXPECT_GE(capturedX, 10.0f);    // a real, generated value was readable through the pointer

    signals->On<Astra::Events::ComponentAdded>().Unregister(handler);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.BatchCreateEmitsRealComponentPointer
```
Expected: FAIL — `captured` is `nullptr` (and `capturedX` stays `-1`).

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/Registry.hpp`, `CreateEntities` currently emits:
```cpp
                    for (size_t i = 0; i < created; ++i)
                    {
                        ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
                    }
```
Replace that loop body with a per-entity record lookup (mirroring single-entity `CreateEntity`):
```cpp
                    for (size_t i = 0; i < created; ++i)
                    {
                        auto* record = m_archetypeManager->GetEntityRecord(outEntities[i]);
                        if (record)
                        {
                            ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), record->archetype->GetComponent<Components>(record->location))), ...);
                        }
                    }
```
Apply the identical replacement to the `CreateEntitiesWith` `ComponentAdded` loop (same `nullptr` pattern).

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.BatchCreateEmitsRealComponentPointer
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "fix(registry): batch create emits real ComponentAdded pointer, not null (Theme J)"
```

---

### Task 4: Fix 4 — `View(nullptr)` constructor guard

**Files:**
- Modify: `include/Astra/Registry/View.hpp` (ctor body ~`47-52`)
- Test: `tests/Registry/ViewTest.cpp` (append `TEST_F`)

**Interfaces:**
- Consumes: `View<Position>(std::shared_ptr<ArchetypeManager>)`, `View::IsValid()`, `View::ForEach`. All three `m_last*` counters have default member initializers `= 0` (`View.hpp:535-537`), so a guarded ctor leaves them zeroed.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Registry/ViewTest.cpp`:

```cpp
// Theme J Fix 4: constructing a View over a null manager must not crash.
TEST_F(ViewTest, NullManagerViewIsEmptyNotCrash)
{
    using namespace Astra::Test;

    Astra::View<Position> nullView(nullptr);

    EXPECT_FALSE(nullView.IsValid());

    int n = 0;
    nullView.ForEach([&](Astra::Entity, Position&) { ++n; });
    EXPECT_EQ(n, 0);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it CRASHES (RED)**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewTest.NullManagerViewIsEmptyNotCrash
```
Expected: the test CRASHES / aborts (null-pointer dereference constructing the view — `CollectArchetypes()` / the counter reads deref the null manager). That crash is the RED signal.

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/View.hpp`, the constructor body is:
```cpp
        {
            CollectArchetypes();
            m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
            m_lastGeneration = m_archetypeManager->m_generation;
            m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
        }
```
Guard it against a null manager (consistent with `ForEach`/`IsValid`, which already treat a null manager as an empty/invalid view):
```cpp
        {
            if (manager)
            {
                CollectArchetypes();
                m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(std::memory_order_acquire);
                m_lastGeneration = m_archetypeManager->m_generation;
                m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(std::memory_order_acquire);
            }
            // else: null manager -> empty/invalid view; all m_last* counters keep their
            // default-initialized 0 (View.hpp member initializers).
        }
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ViewTest.*
```
Expected: PASS (the new test and all existing `ViewTest.*`).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/View.hpp tests/Registry/ViewTest.cpp
git commit -m "fix(view): guard View constructor against a null manager (Theme J)"
```

---

### Task 5: Full 3-config verification

**Files:** none (verification only).

- [ ] **Step 1: Build and test all three configs**

For each of `Debug`, `Release`, `Dist`:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Config> -p:Platform=x64 -m
bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe
```
Expected: all green in all three configs. The four new tests pass (three in `RegistryTest`, one in `ViewTest`); the Debug/Release count delta is only the pre-existing Debug-only `EXPECT_DEATH` tests. If only `CompressionTest.PerformanceBenchmark` fails, rerun it isolated (known flake) — not a regression.

- [ ] **Step 2: Confirm no `ide/` changes are staged**

Run: `git status`
Expected: only `include/…`, `tests/…`, and `docs/…` changes. `ide/` must NOT appear (no new test files were created).

---

## Self-Review

**1. Spec coverage:**
- Spec §2 Fix 1 (CreateEntities/CreateEntitiesWith → size_t) → Task 1. ✓
- Spec §2 Fix 2 (Registry non-copyable + ShareComponentRegistry) → Task 2. ✓
- Spec §2 Fix 3 (batch ComponentAdded real pointer) → Task 3. ✓
- Spec §2 Fix 4 (View null-manager ctor guard) → Task 4. ✓
- Spec §5 (by-ID batch filter dropped as verified-benign) → correctly absent from this plan. ✓
- Spec §6 acceptance (3 configs green; RED→GREEN; intended API changes only) → Task 5 + Global Constraints. ✓

**2. Placeholder scan:** No TBD/TODO; every code and test step shows complete code. ✓

**3. Type consistency:** `ShareComponentRegistry()` returns `std::shared_ptr<ComponentRegistry>` and is consumed by the `Registry(std::shared_ptr<ComponentRegistry>, Config)` ctor in Task 2's test. `CreateEntities`/`CreateEntitiesWith` are `size_t` in Task 1 and used (return ignored) by Task 3's test. `Astra::Events::ComponentAdded::component` (`void*`) is the field read in Task 3. ✓

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-20-astra-theme-j-registry-footguns.md`.**
