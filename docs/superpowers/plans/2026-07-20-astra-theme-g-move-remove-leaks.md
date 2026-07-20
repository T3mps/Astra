# Theme G — Move/Remove Destruct & Leak Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix four lifetime-management defects on the archetype/container move-and-remove paths (leaked moved-from slots, a leaked probe temporary, an uninitialized move-only component, and a false remove-count on chunk exhaustion).

**Architecture:** Each fix is a small, local edit that mirrors a proven-correct sibling already in the tree. No wire-format or public-API signature change. Every fix ships with a RED→GREEN regression test that uses a static live-instance counter (`ctor ++s_live; dtor --s_live;`) so a leak leaves the counter above the true live count.

**Tech Stack:** Header-only C++20 archetype ECS (MSVC, x64). GoogleTest. Build via `Astra.sln` (premake5-generated).

## Global Constraints

- **All three configs must build clean and green:** Debug, Release, Dist (`-p:Platform=x64`). Gate on "all configs green + intended new tests," not an absolute count.
- **Uniform-graceful misuse policy:** do NOT add an assert-and-abort on a recoverable condition. The one new `ASTRA_ENSURE` (Task 4) is diagnostic-only and non-fatal; the actual safety must hold in Release/Dist without it.
- **TypeID ceiling (~128):** the test binary is near the `MAX_COMPONENTS = 128` ceiling. Net new component IDs for this whole theme must be **+1** (`Astra::Test::Tracked`). Reuse existing `Astra::Test::*` types elsewhere. Theme-E made file-local generic-named *component* types hard-fail on collision — the new component lives in the qualified `Astra::Test::` namespace (distinct hash); the FlatSet element type is NOT a component (no TypeID).
- **No `ide/` regen:** all tests are **appended to existing test files** (`tests/Registry/ArchetypeManagerTest.cpp`, `tests/Container/FlatSetTest.cpp`) and the new component goes in the existing header `tests/TestComponents.hpp`. Appending to an existing `.cpp` and editing a header need no premake regen. Do NOT `git add ide/`.
- **Build (Debug example):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m`
- **Test exe:** `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (swap `Debug` for `Release`/`Dist`). Filter with `--gtest_filter=`.
- **IDE clang diagnostics are false positives — judge only by the MSVC build.** If a build shows a stale-link/PDB error after an interrupted build: `taskkill //F //IM mspdbsrv.exe` then rebuild.

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `include/Astra/Archetype/ArchetypeChunkPool.hpp` | Chunk-level entity storage | Fix 1: destruct moved-from source slot in `Chunk::RemoveEntity` |
| `include/Astra/Container/FlatSet.hpp` | SwissTable set | Fix 2: always destruct the `Emplace` probe temporary |
| `include/Astra/Archetype/ArchetypeManager.hpp` | Archetype/entity orchestration | Fix 3: `MoveAndAddByID` move-only fallback; Fix 4: `RemoveComponents` truthful count + partial-path hardening |
| `tests/TestComponents.hpp` | Shared test components | Add `Astra::Test::Tracked` (move-only, lifetime-counted) |
| `tests/Registry/ArchetypeManagerTest.cpp` | Manager tests | Append Fix 1, Fix 3, Fix 4 tests |
| `tests/Container/FlatSetTest.cpp` | FlatSet tests | Append Fix 2 test |

---

### Task 1: Fix 1 — `ArchetypeChunkPool::Chunk::RemoveEntity` destructs the moved-from source slot

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`Chunk::RemoveEntity`, swap branch ~`352-374`)
- Modify: `tests/TestComponents.hpp` (add `Astra::Test::Tracked`)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (append `TEST_F`)

**Interfaces:**
- Produces: `Astra::Test::Tracked` — a move-only, lifetime-counted component with `static inline int s_live` and `int value`. Reused by Task 3. Satisfies the `Astra::Component` concept (nothrow move-assign/destruct, move-construct).
- Consumes: `ArchetypeManager::AddEntityWith`, `::RemoveEntity`, `::GetComponent<T>` (existing).

- [ ] **Step 1: Add the shared `Tracked` component**

In `tests/TestComponents.hpp`, inside `namespace Astra::Test` (after the `Damage` struct, before `ComponentTraits`), add:

```cpp
    // 17. Move-only, lifetime-counted component (Theme G leak accounting).
    //     s_live == number of live instances; a skipped destructor leaves it high.
    struct Tracked
    {
        static inline int s_live = 0;
        int value = 0;

        Tracked() { ++s_live; }
        explicit Tracked(int v) : value(v) { ++s_live; }
        Tracked(Tracked&& o) noexcept : value(o.value) { ++s_live; }
        Tracked& operator=(Tracked&& o) noexcept { value = o.value; return *this; }
        ~Tracked() { --s_live; }

        Tracked(const Tracked&) = delete;
        Tracked& operator=(const Tracked&) = delete;
    };
```

Then add these lines to the `static_assert` block near the bottom of the namespace:

```cpp
    static_assert(Component<Tracked>, "Tracked must satisfy Component concept");
    static_assert(!std::is_copy_constructible_v<Tracked>, "Tracked should be move-only");
```

- [ ] **Step 2: Write the failing test**

Append to `tests/Registry/ArchetypeManagerTest.cpp` (end of file):

```cpp
// Theme G Fix 1: swap-and-pop removal must destruct the moved-from source slot.
TEST_F(ArchetypeManagerTest, RemoveEntityDestructsMovedFromSourceSlot)
{
    using Astra::Test::Tracked;
    componentRegistry->RegisterComponents<Tracked>();
    Tracked::s_live = 0;

    Astra::Entity e0(200, 1), e1(201, 1), e2(202, 1);
    manager->AddEntityWith(e0, Tracked{10});
    manager->AddEntityWith(e1, Tracked{11});
    manager->AddEntityWith(e2, Tracked{12});
    ASSERT_EQ(Tracked::s_live, 3);

    // Remove the first (non-tail) entity: swap-and-pop moves e2's Tracked into
    // slot 0, leaving the last slot as a moved-from object that must be destructed.
    manager->RemoveEntity(e0);

    EXPECT_EQ(Tracked::s_live, 2);   // BUG leaves 3 (moved-from source slot never destructed)
    EXPECT_EQ(manager->GetComponent<Tracked>(e2)->value, 12);
}
```

- [ ] **Step 3: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.RemoveEntityDestructsMovedFromSourceSlot
```
Expected: FAIL — `Tracked::s_live` is `3`, expected `2`.

- [ ] **Step 4: Apply the fix**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, in `Chunk::RemoveEntity`'s swap branch (`if (index != lastIndex)`), the component loop currently ends with:

```cpp
                        // Destruct destination, move from source
                        info.descriptor.Destruct(dstPtr);
                        info.descriptor.MoveConstruct(dstPtr, srcPtr);
```

Replace those three lines with:

```cpp
                        // Destruct destination, move from source, then destruct the
                        // moved-from source slot (mirrors Archetype::MoveEntitiesBetweenChunks).
                        info.descriptor.Destruct(dstPtr);
                        info.descriptor.MoveConstruct(dstPtr, srcPtr);
                        info.descriptor.Destruct(srcPtr);
```

- [ ] **Step 5: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.RemoveEntityDestructsMovedFromSourceSlot
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp tests/TestComponents.hpp tests/Registry/ArchetypeManagerTest.cpp
git commit -m "fix(archetype): destruct moved-from source slot in Chunk::RemoveEntity (Theme G)"
```

---

### Task 2: Fix 2 — `FlatSet::Emplace` always destructs the probe temporary

**Files:**
- Modify: `include/Astra/Container/FlatSet.hpp` (`Emplace`: `Cleanup` struct ~`430-439`, dismiss line ~`557`)
- Test: `tests/Container/FlatSetTest.cpp` (append `TEST_F` + a file-local type)

**Interfaces:**
- Consumes: `Astra::FlatSet<T, Hash, Equals>::Emplace` (existing).
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the failing test**

Append to `tests/Container/FlatSetTest.cpp` (end of file). The element type is copyable **and non-movable** (the user-declared copy ctor suppresses the implicit move ctor), so `std::move(*temp)` in `Emplace` binds to the copy ctor — the exact leak case. It is NOT an ECS component, so it consumes no ComponentID.

```cpp
namespace
{
    // Copyable, non-movable, lifetime-counted. The user-declared copy ctor
    // suppresses the implicit move ctor, so std::move(x) copies -> the leak case.
    struct CopyOnly
    {
        static inline int s_live = 0;
        int value;
        explicit CopyOnly(int v = 0) : value(v) { ++s_live; }
        CopyOnly(const CopyOnly& o) : value(o.value) { ++s_live; }
        ~CopyOnly() { --s_live; }
    };
    struct CopyOnlyHash
    {
        std::size_t operator()(const CopyOnly& c) const noexcept { return std::hash<int>{}(c.value); }
    };
    struct CopyOnlyEq
    {
        bool operator()(const CopyOnly& a, const CopyOnly& b) const noexcept { return a.value == b.value; }
    };
}

// Theme G Fix 2: Emplace must destruct its probe temporary on success.
TEST_F(FlatSetTest, EmplaceDestructsProbeTemporary)
{
    CopyOnly::s_live = 0;
    Astra::FlatSet<CopyOnly, CopyOnlyHash, CopyOnlyEq> set;

    auto [it, inserted] = set.Emplace(42);
    EXPECT_TRUE(inserted);

    // Exactly one live object: the element stored in the set. The probe
    // temporary must have been destructed (BUG leaves 2).
    EXPECT_EQ(CopyOnly::s_live, 1);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=FlatSetTest.EmplaceDestructsProbeTemporary
```
Expected: FAIL — `CopyOnly::s_live` is `2`, expected `1`.

- [ ] **Step 3: Apply the fix**

In `include/Astra/Container/FlatSet.hpp`, `Emplace` currently declares:

```cpp
            // Use RAII to ensure cleanup
            struct Cleanup {
                AllocatorType& alloc;
                T* ptr;
                bool dismissed = false;
                ~Cleanup() { 
                    if (!dismissed) {
                        std::allocator_traits<AllocatorType>::destroy(alloc, ptr);
                    }
                }
            } cleanup{m_alloc, temp};
```

Replace that block with (drop the `dismissed` flag; always destruct):

```cpp
            // Always destruct the probe temporary on scope exit. The value is
            // copied/moved into a *separate* slot object, so the temporary must
            // still be destructed on every exit path (duplicate found, reserve
            // failure, or successful insert). Skipping it leaks the temporary for
            // a copyable-non-movable T (std::move binds to the copy ctor).
            struct Cleanup {
                AllocatorType& alloc;
                T* ptr;
                ~Cleanup() {
                    std::allocator_traits<AllocatorType>::destroy(alloc, ptr);
                }
            } cleanup{m_alloc, temp};
```

Then, further down in `Emplace`, DELETE this line (it appears right after the value is moved into the slot):

```cpp
            cleanup.dismissed = true; // Prevent cleanup since we moved the value
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=FlatSetTest.*
```
Expected: PASS (the new test and all existing `FlatSetTest.*`).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Container/FlatSet.hpp tests/Container/FlatSetTest.cpp
git commit -m "fix(container): always destruct FlatSet::Emplace probe temporary (Theme G)"
```

---

### Task 3: Fix 3 — `ArchetypeManager::MoveAndAddByID` constructs a move-only new component

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`MoveAndAddByID`, new-component cascade ~`1391-1406`)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (append `TEST_F`)

**Interfaces:**
- Consumes: `Astra::Test::Tracked` (Task 1); `ArchetypeManager::AddEntityWith`, `::AddComponentByID(Entity, ComponentID, const void*, size_t) -> bool`, `::GetComponent<T>`, `::HasComponent<T>`; `Astra::TypeID<T>::Value()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/Registry/ArchetypeManagerTest.cpp` (end of file). A move-only component added through the type-erased CommandBuffer path (`AddComponentByID`) must be move-constructed into the slot with its value intact. With the bug, the slot is left unconstructed; because fresh chunk memory is zero-initialized, its `value` reads back as `0`.

```cpp
// Theme G Fix 3: MoveAndAddByID must construct a move-only new component.
TEST_F(ArchetypeManagerTest, MoveAndAddByIDMoveOnlyComponentPreservesValue)
{
    using Astra::Test::Position;
    using Astra::Test::Tracked;
    componentRegistry->RegisterComponents<Tracked>();
    Tracked::s_live = 0;

    Astra::Entity e(210, 1);
    manager->AddEntityWith(e, Position{1.0f, 2.0f, 3.0f});   // e now in {Position}

    // Add the move-only component via the type-erased path (as CommandBuffer flush does).
    Tracked src{42};
    const bool ok = manager->AddComponentByID(
        e, Astra::TypeID<Tracked>::Value(), &src, sizeof(Tracked));
    ASSERT_TRUE(ok);

    ASSERT_TRUE(manager->HasComponent<Tracked>(e));
    Tracked* t = manager->GetComponent<Tracked>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->value, 42);   // BUG leaves 0 (slot never constructed; chunk is zeroed)
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.MoveAndAddByIDMoveOnlyComponentPreservesValue
```
Expected: FAIL — `t->value` is `0`, expected `42`.

- [ ] **Step 3: Apply the fix**

In `include/Astra/Archetype/ArchetypeManager.hpp`, `MoveAndAddByID`'s new-component branch currently reads:

```cpp
                if (dstComp.id == newComponentId) ASTRA_UNLIKELY
                {
                    // Copy new component data - use memcpy for trivially copyable types
                    if (newDesc.is_trivially_copyable)
                    {
                        std::memcpy(dstPtr, componentData, newDesc.size);
                    }
                    else if (newDesc.constructWith)
                    {
                        newDesc.constructWith(dstPtr, componentData);
                    }
                    else if (newDesc.copyConstruct)
                    {
                        newDesc.copyConstruct(dstPtr, componentData);
                    }
                }
```

Append two fallbacks (keep the existing copy-first order unchanged):

```cpp
                if (dstComp.id == newComponentId) ASTRA_UNLIKELY
                {
                    // Copy new component data - use memcpy for trivially copyable types
                    if (newDesc.is_trivially_copyable)
                    {
                        std::memcpy(dstPtr, componentData, newDesc.size);
                    }
                    else if (newDesc.constructWith)
                    {
                        newDesc.constructWith(dstPtr, componentData);
                    }
                    else if (newDesc.copyConstruct)
                    {
                        newDesc.copyConstruct(dstPtr, componentData);
                    }
                    else if (newDesc.moveConstruct)
                    {
                        // Move-only component: the CommandBuffer move-constructed the
                        // value into its own storage, so move it out (the buffer's copy
                        // is destructed after flush). Without this the slot was left
                        // uninitialized.
                        newDesc.moveConstruct(dstPtr, const_cast<void*>(componentData));
                    }
                    else
                    {
                        // Never-UB floor (mirrors ComponentDescriptor::ConstructWith).
                        newDesc.DefaultConstruct(dstPtr);
                    }
                }
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.MoveAndAddByIDMoveOnlyComponentPreservesValue
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp tests/Registry/ArchetypeManagerTest.cpp
git commit -m "fix(archetype): construct move-only new component in MoveAndAddByID (Theme G)"
```

---

### Task 4: Fix 4 — `RemoveComponents<T>` truthful count + partial-move hardening

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp`
  - `RemoveComponents<T>` (~`344-369`)
  - `BatchMoveEntitiesInternal` (~`1243-1315`)
  - `BatchMoveEntitiesWithComponent` (~`1317-1324`), `BatchMoveEntitiesWithoutComponent` (~`1326-1329`), `BatchMoveEntitiesWithComponentByID` (~`1423-1439`)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (append `TEST_F`; add `#include <span>` at top if not present)

**Interfaces:**
- Changes `BatchMoveEntitiesInternal` and the three `BatchMoveEntities*` wrappers from returning `void` to returning `size_t` (entities actually moved). Only `RemoveComponents<T>` consumes the value; other callers ignore it. No public API change.
- Consumes: existing `Astra::Test::Position`, `Astra::Test::Velocity`; `ArchetypeChunkPool::Config`; `ArchetypeManager(componentRegistry, poolConfig)`, `::AddEntityWith`, `::HasComponent<T>`, `::RemoveComponents<T>(std::span<Entity>) -> size_t`.

**Why the OOM is deterministic:** the manager's root archetype eagerly allocates chunk #1 at construction (`Archetype::Initialize` creates a chunk). A pool with `maxChunks = 2` therefore has room for root (#1) plus the `{Position,Velocity}` source archetype (#2). Removing `Velocity` needs a fresh chunk for the `{Position}` target archetype (#3) which the pool refuses, so `BatchMoveEntitiesFrom` returns empty and nothing is moved.

- [ ] **Step 1: Write the failing test**

If `tests/Registry/ArchetypeManagerTest.cpp` does not already `#include <span>`, add it near the top includes. Then append (end of file):

```cpp
// Theme G Fix 4: RemoveComponents<T> must report the true moved count, and must
// not lose entities, when the destination archetype cannot allocate a chunk.
TEST_F(ArchetypeManagerTest, RemoveComponentsReportsActualCountOnChunkExhaustion)
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;

    // Pool room for root (chunk #1) + {Position,Velocity} (chunk #2) only; the
    // remove target {Position} cannot get a 3rd chunk.
    Astra::ArchetypeChunkPool::Config poolConfig;
    poolConfig.chunkSize      = 4096;   // MIN_CHUNK_SIZE
    poolConfig.chunksPerBlock = 1;
    poolConfig.maxChunks      = 2;
    poolConfig.useHugePages   = false;

    Astra::ArchetypeManager localManager(componentRegistry, poolConfig);

    std::vector<Astra::Entity> ents;
    for (int i = 0; i < 5; ++i)
    {
        Astra::Entity e(300 + i, 1);
        localManager.AddEntityWith(e, Position{1, 2, 3}, Velocity{4, 5, 6});
        ents.push_back(e);
    }
    ASSERT_TRUE(localManager.HasComponent<Velocity>(ents[0]));   // src built OK (1 chunk)

    // Removing Velocity needs a fresh chunk for {Position}; the pool is exhausted,
    // so nothing actually moves.
    std::span<Astra::Entity> span(ents.data(), ents.size());
    size_t removed = localManager.RemoveComponents<Velocity>(span);

    EXPECT_EQ(removed, 0u);   // BUG reports 5
    for (auto e : ents)
    {
        EXPECT_TRUE(localManager.HasComponent<Velocity>(e));   // entities untouched in src
    }
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.RemoveComponentsReportsActualCountOnChunkExhaustion
```
Expected: FAIL — `removed` is `5`, expected `0`. (The `HasComponent` checks pass either way — the entities do stay in src; the desync is only the returned count. Both assertions lock the fix.)

- [ ] **Step 3a: Make `BatchMoveEntitiesInternal` return the moved count and harden the partial path**

In `include/Astra/Archetype/ArchetypeManager.hpp`, change the signature of `BatchMoveEntitiesInternal` from `void` to `size_t`:

```cpp
        template<typename PostMoveOp>
        size_t BatchMoveEntitiesInternal(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, PostMoveOp&& postMoveOp)
```

In the OOM bail (the `if (newLocations.empty() && !entityBatch.empty())` block), change the bare `return;` to:

```cpp
                return 0;
```

Then replace the tail of the function — currently:

```cpp
            // Batch remove from source (defer chunk cleanup to avoid invalidating locations)
            auto movedEntities = srcArchetype->RemoveEntities(srcLocations, true);
            
            // Update locations of entities moved during removal
            for (const auto& [movedEntity, newLocation] : movedEntities)
            {
                auto it = m_entityMap.find(movedEntity);
                if (it != m_entityMap.end()) ASTRA_LIKELY
                {
                    it->second.location = newLocation;
                }
            }
        }
```

with:

```cpp
            // Normally every entity is placed (dst chunks are pre-allocated to fit
            // the whole batch). If BatchMoveEntitiesFrom ever returns a short result,
            // remove ONLY the entities actually moved to dst -- removing all of
            // srcLocations would drop the un-moved entities from src without ever
            // placing them in dst (silent entity loss).
            if (!ASTRA_ENSURE(newLocations.size() == entityBatch.size(),
                              "BatchMoveEntitiesFrom placed fewer entities than requested")) ASTRA_UNLIKELY
            {
                // Diagnostic only; the prefix-limited removal below keeps the
                // un-moved entities valid in src.
            }

            // Batch remove from source (defer chunk cleanup to avoid invalidating locations)
            std::span<const EntityLocation> movedSrcLocations(srcLocations.data(), newLocations.size());
            auto movedEntities = srcArchetype->RemoveEntities(movedSrcLocations, true);
            
            // Update locations of entities moved during removal
            for (const auto& [movedEntity, newLocation] : movedEntities)
            {
                auto it = m_entityMap.find(movedEntity);
                if (it != m_entityMap.end()) ASTRA_LIKELY
                {
                    it->second.location = newLocation;
                }
            }

            return newLocations.size();
        }
```

- [ ] **Step 3b: Make the three wrappers return the count**

`BatchMoveEntitiesWithoutComponent` — change to:

```cpp
        size_t BatchMoveEntitiesWithoutComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch)
        {
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [](Archetype*, const std::vector<EntityLocation>&) { /* No component operation needed for removal */ });
        }
```

`BatchMoveEntitiesWithComponent` — change its declaration to `size_t` and `return` the internal call:

```cpp
        template<Component T, typename... Args>
        size_t BatchMoveEntitiesWithComponent(Archetype* srcArchetype, Archetype* dstArchetype, SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch, Args&&... args)
        {
            // Create component value upfront and capture by value to avoid dangling reference
            // The lambda may be invoked after args go out of scope in optimized builds
            T component{std::forward<Args>(args)...};
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch, [component](Archetype* dst, const std::vector<EntityLocation>& locs) { dst->SetComponents<T>(locs, component); });
        }
```

`BatchMoveEntitiesWithComponentByID` — change its declaration to `size_t` and `return` the internal call:

```cpp
        size_t BatchMoveEntitiesWithComponentByID(Archetype* srcArchetype, Archetype* dstArchetype,
                                                SmallVector<std::pair<Entity, EntityLocation>, 8>& entityBatch,
                                                ComponentID componentId, const void* data, const ComponentDescriptor& desc)
        {
            return BatchMoveEntitiesInternal(srcArchetype, dstArchetype, entityBatch,
                [componentId, data, &desc](Archetype* dst, const std::vector<EntityLocation>& locs)
                {
                    // Set the new component for all entities at their new locations
                    for (const auto& location : locs)
                    {
                        auto [chunk, entityIdx] = dst->ResolveLocation(location);
                        const auto& arrayInfo = chunk->m_componentArrays[componentId];
                        void* dstPtr = static_cast<std::byte*>(arrayInfo.base) + entityIdx * arrayInfo.stride;
                        desc.ConstructWith(dstPtr, data);
                    }
                });
        }
```

- [ ] **Step 3c: Make `RemoveComponents<T>` accumulate the returned count**

In `RemoveComponents<T>`, replace:

```cpp
                Archetype* dstArchetype = GetArchetypeWithRemoved(srcArchetype, componentId);
                BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
                removedCount += entityBatch.size();
```

with:

```cpp
                Archetype* dstArchetype = GetArchetypeWithRemoved(srcArchetype, componentId);
                removedCount += BatchMoveEntitiesWithoutComponent(srcArchetype, dstArchetype, entityBatch);
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ArchetypeManagerTest.*
```
Expected: PASS (the new test and all existing `ArchetypeManagerTest.*` — the add-side `BatchMoveEntitiesWithComponent` path still works with its now-`size_t` return ignored by `AddComponents`).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeManager.hpp tests/Registry/ArchetypeManagerTest.cpp
git commit -m "fix(archetype): truthful RemoveComponents count + partial-move hardening (Theme G)"
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
Expected: all green in all three configs. The four new tests pass; the Debug/Release count delta is only the pre-existing Debug-only `EXPECT_DEATH` tests. If only `CompressionTest.PerformanceBenchmark` fails, rerun it isolated (known flake) — not a regression.

- [ ] **Step 2: Confirm no `ide/` changes are staged**

Run: `git status`
Expected: only `include/…`, `tests/…`, and `docs/…` changes. `ide/` must NOT appear. (No new test files were created, so no premake regen was needed.)

---

## Self-Review

**1. Spec coverage:**
- Spec §2 Fix 1 (RemoveEntity source-slot destruct) → Task 1. ✓
- Spec §2 Fix 2 (FlatSet probe temporary) → Task 2. ✓
- Spec §2 Fix 3 (MoveAndAddByID move-only, moveConstruct-then-DefaultConstruct) → Task 3. ✓
- Spec §2 Fix 4 (RemoveComponents count + partial-path hardening) → Task 4. ✓
- Spec §3 design decisions (all four together; move-then-default floor; count + hardening) → reflected in Tasks 3 & 4. ✓
- Spec §4 test strategy (shared move-only counter; file-local copyable-non-movable; maxChunks OOM; +1 TypeID; no regen) → Tasks 1–4 + Global Constraints. ✓
- Spec §6 acceptance (3 configs green; RED→GREEN; no assert-abort; no API change) → Task 5 + Global Constraints. ✓

**2. Placeholder scan:** No TBD/TODO; every code and test step shows complete code. ✓

**3. Type consistency:** `Astra::Test::Tracked` (Task 1) with `s_live`/`value`/move-only is used identically in Task 3. `BatchMoveEntitiesInternal`/`BatchMoveEntitiesWithoutComponent`/`BatchMoveEntitiesWithComponent`/`BatchMoveEntitiesWithComponentByID` all move `void`→`size_t` consistently, and `RemoveComponents` consumes the `size_t`. `ASTRA_ENSURE` used in the `if (!ASTRA_ENSURE(...))` form (matches existing FlatSet usage). ✓

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-20-astra-theme-g-move-remove-leaks.md`.**
