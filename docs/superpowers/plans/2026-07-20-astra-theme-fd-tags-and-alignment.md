# Themes F + D — Tag Correctness + SmallVector Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three empty/tag (size==0) component inconsistencies (asymmetric `Destruct`, dropped by-ID signals, `Get`/`Has` contradiction) via a shared non-null sentinel, and make `SmallVector` heap allocation alignment-aware.

**Architecture:** A canonical `EmptyComponentSentinel()` unifies the type-erased tag paths so `Get != nullptr` agrees with `Has` and by-ID signals fire for tags. `SmallVector` centralizes its 7 raw alloc/dealloc sites into aligned-aware helpers. Each fix ships a RED→GREEN regression test appended to an existing file.

**Tech Stack:** Header-only C++20 archetype ECS (MSVC, x64). GoogleTest. Build via `Astra.sln`.

## Global Constraints

- **All three configs must build clean and green:** Debug, Release, Dist (`-p:Platform=x64`).
- **Uniform-graceful misuse policy:** no assert-and-abort on a recoverable condition.
- **TypeID ceiling (~128):** net new component IDs must be **+1** (`ThemeFCountedTag` for F1). F2/F3 reuse `Astra::Test::Player`; D2 uses a file-local non-component type. The test binary is near `MAX_COMPONENTS = 128`; give any new *component* type a UNIQUE name (Theme-E makes generic-named file-local component types hard-fail on collision).
- **No `ide/` regen:** all tests append to existing files. Do NOT `git add ide/`.
- **The sentinel is one shared address:** all tags return the same `EmptyComponentSentinel()`; the `ComponentID` carried alongside identifies the type. Callers must not read through it (a tag has `desc.size == 0`).
- **Build (Debug example):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m`
- **Test exe:** `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (swap `Debug` for `Release`/`Dist`). Filter with `--gtest_filter=`.
- **IDE clang diagnostics are false positives — judge only by the MSVC build.**

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `include/Astra/Component/Component.hpp` | Component descriptor | Sentinel + F1 (`Destruct` skips size==0) |
| `include/Astra/Registry/Registry.hpp` | Registry public API | F2 (by-ID tag signals), F3 (`GetComponentByHash` sentinel) |
| `include/Astra/Container/SmallVector.hpp` | Small-buffer vector | D2 (aligned allocation) |
| `tests/Component/ComponentRegistryTest.cpp` | Descriptor tests | F1 test |
| `tests/Registry/RegistryTest.cpp` | Registry tests | F2, F3 tests |
| `tests/Container/SmallVectorTest.cpp` | SmallVector tests | D2 test |

---

### Task 1: Sentinel + F1 — `Component::Destruct` skips `size==0`

**Files:**
- Modify: `include/Astra/Component/Component.hpp` (add sentinel; `Destruct` ~`154-157`)
- Test: `tests/Component/ComponentRegistryTest.cpp` (append)

**Interfaces:**
- Produces: `void* Astra::EmptyComponentSentinel() noexcept` — a stable non-null pointer for present zero-size components (used by Tasks 2 & 3). `Component::Destruct` becomes a no-op for `size == 0`.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Component/ComponentRegistryTest.cpp`:

```cpp
namespace
{
    // Empty (is_empty) tag with a lifetime-counting destructor. Unique name (Theme-E:
    // generic-named file-local component types hard-fail on collision). Empty Serialize
    // is required because RegisterComponentImpl odr-uses Serialize<T> for a
    // non-trivially-copyable type (a user dtor makes it non-trivially-copyable).
    struct ThemeFCountedTag
    {
        static inline int s_live = 0;
        ThemeFCountedTag() { ++s_live; }
        ~ThemeFCountedTag() { --s_live; }
        template<typename Archive> void Serialize(Archive&) {}
    };
    static_assert(std::is_empty_v<ThemeFCountedTag>, "ThemeFCountedTag must be empty (size 0 descriptor)");
}

// Theme F1: ComponentDescriptor::Destruct must be a no-op for a size==0 (tag) component,
// symmetric with DefaultConstruct.
TEST(ComponentDescriptorTagTest, DestructSkipsEmptyComponent)
{
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<ThemeFCountedTag>();
    const Astra::ComponentDescriptor* desc =
        registry.GetComponentDescriptor(Astra::TypeID<ThemeFCountedTag>::Value());
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(desc->size, 0u);

    ThemeFCountedTag::s_live = 0;
    std::byte dummy{};
    desc->Destruct(&dummy);   // must NOT invoke ~ThemeFCountedTag() for a tag

    EXPECT_EQ(ThemeFCountedTag::s_live, 0);   // BUG: -1 (the dtor ran on non-object storage)
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ComponentDescriptorTagTest.DestructSkipsEmptyComponent
```
Expected: FAIL — `s_live` is `-1` (the descriptor's `destruct` thunk ran `~ThemeFCountedTag()`).

- [ ] **Step 3: Add the sentinel and apply the F1 fix**

In `include/Astra/Component/Component.hpp`: ensure `#include <cstddef>` is present near the top (for `std::byte`); add it if missing. Then, inside `namespace Astra` immediately AFTER the closing `};` of the `ComponentDescriptor` struct, add:

```cpp
    // Stable, non-null pointer used as the "component pointer" for a present zero-size (tag)
    // component: by-hash/name Get returns it and by-ID signals fire with it, so Get(...) !=
    // nullptr agrees with Has(...). It points at a real static byte; callers must NOT read
    // through it (a tag has no data -- desc.size == 0). All tags share this one address; the
    // ComponentID carried alongside identifies the type.
    inline void* EmptyComponentSentinel() noexcept
    {
        static std::byte sentinel{};
        return &sentinel;
    }
```

Then change `Component`... i.e. the `ComponentDescriptor::Destruct` method (currently):
```cpp
        inline void Destruct(void* ptr) const
        {
            destruct(ptr);
        }
```
to:
```cpp
        inline void Destruct(void* ptr) const
        {
            if (size == 0) return;  // empty (tag) component: nothing to destruct (mirrors DefaultConstruct)
            destruct(ptr);
        }
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ComponentDescriptorTagTest.DestructSkipsEmptyComponent
```
Expected: PASS.

- [ ] **Step 5: Run the archetype/registry suites to confirm no regression**

Run:
```
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ComponentRegistryTest.*:ArchetypeManagerTest.*:EmptyTag.*
```
Expected: all pass (the chunk paths already guarded `base==nullptr`, so making `Destruct` skip size==0 changes nothing for them).

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Component/Component.hpp tests/Component/ComponentRegistryTest.cpp
git commit -m "fix(component): Destruct skips size==0 + add EmptyComponentSentinel (Theme F)"
```

---

### Task 2: F2 — by-ID paths emit tag signals

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (`AddComponentByID` signal block ~`494-504`, `RemoveComponentByID` signal block ~`550-558`)
- Test: `tests/Registry/RegistryTest.cpp` (append)

**Interfaces:**
- Consumes: `Astra::EmptyComponentSentinel()` (Task 1); `Astra::Test::Player` (registered in the `RegistryTest` fixture); the signal API (`EnableSignals`, `GetSignalManager`, `On<Events::ComponentAdded/Removed>().Register`).

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Registry/RegistryTest.cpp`:

```cpp
// Theme F2: AddComponentByID/RemoveComponentByID must emit signals for a tag (size==0)
// component, with a non-null (sentinel) pointer.
TEST_F(RegistryTest, ByIdTagEmitsAddAndRemoveSignals)
{
    using namespace Astra::Test;

    registry->EnableSignals(Astra::Signal::ComponentAdded | Astra::Signal::ComponentRemoved);
    auto* signals = registry->GetSignalManager();

    void* addedPtr = nullptr;
    bool removedFired = false;
    void* removedPtr = nullptr;
    auto ha = signals->On<Astra::Events::ComponentAdded>().Register(
        [&](const Astra::Events::ComponentAdded& e) { addedPtr = e.component; });
    auto hr = signals->On<Astra::Events::ComponentRemoved>().Register(
        [&](const Astra::Events::ComponentRemoved& e) { removedFired = true; removedPtr = e.component; });

    Astra::Entity e = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});

    // Add the tag through the type-erased path (data=nullptr, dataSize=0 for a tag).
    ASSERT_TRUE(registry->AddComponentByID(e, Astra::TypeID<Player>::Value(), nullptr, 0));
    EXPECT_NE(addedPtr, nullptr);   // BUG: stays null (signal dropped for the tag)

    ASSERT_TRUE(registry->RemoveComponentByID(e, Astra::TypeID<Player>::Value()));
    EXPECT_TRUE(removedFired);       // BUG: never fires
    EXPECT_NE(removedPtr, nullptr);

    signals->On<Astra::Events::ComponentAdded>().Unregister(ha);
    signals->On<Astra::Events::ComponentRemoved>().Unregister(hr);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.ByIdTagEmitsAddAndRemoveSignals
```
Expected: FAIL — `addedPtr` stays `nullptr` and `removedFired` is false (signals dropped for the tag).

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/Registry.hpp`, `AddComponentByID`'s signal block currently reads:
```cpp
                        auto& chunks = record->archetype->GetChunks();
                        if (record->location.GetChunkIndex() < chunks.size())
                        {
                            void* compPtr = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
                            if (compPtr)
                            {
                                void* actualPtr = static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size;
                                m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
                            }
                        }
```
Replace that inner block with a size-aware branch:
```cpp
                        auto& chunks = record->archetype->GetChunks();
                        if (record->location.GetChunkIndex() < chunks.size())
                        {
                            void* actualPtr;
                            if (desc->size == 0)
                            {
                                actualPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compPtr = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
                                actualPtr = compPtr ? static_cast<std::byte*>(compPtr) + record->location.GetEntityIndex() * desc->size : nullptr;
                            }
                            if (actualPtr)
                                m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
                        }
```

In `RemoveComponentByID`, the block that computes `componentPtr` currently reads:
```cpp
                        auto& chunks = record->archetype->GetChunks();
                        if (record->location.GetChunkIndex() < chunks.size())
                        {
                            void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
                            if (compArray)
                            {
                                componentPtr = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
                            }
                        }
```
Replace it with:
```cpp
                        auto& chunks = record->archetype->GetChunks();
                        if (record->location.GetChunkIndex() < chunks.size())
                        {
                            if (desc->size == 0)
                            {
                                componentPtr = EmptyComponentSentinel();  // present tag: no data
                            }
                            else
                            {
                                void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
                                if (compArray)
                                    componentPtr = static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
                            }
                        }
```
The existing `if (componentPtr && IsSignalEnabled(...)) Emit<ComponentRemoved>(...)` below now fires for a tag (componentPtr is the sentinel).

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.ByIdTagEmitsAddAndRemoveSignals
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "fix(registry): by-ID AddComponentByID/RemoveComponentByID emit tag signals (Theme F)"
```

---

### Task 3: F3 — `GetComponentByHash`/`Name` returns the sentinel for a present tag

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (`GetComponentByHash` ~`615-623`)
- Test: `tests/Registry/RegistryTest.cpp` (append)

**Interfaces:**
- Consumes: `Astra::EmptyComponentSentinel()` (Task 1); `record->archetype->GetMask().Test(componentId)`; `Astra::Test::Player`. `GetComponentByName` delegates to `GetComponentByHash` and is fixed for free.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Registry/RegistryTest.cpp`:

```cpp
// Theme F3: GetComponentByHash for a present tag must agree with HasComponentByHash
// (non-null), not return nullptr.
TEST_F(RegistryTest, GetComponentByHashTagAgreesWithHas)
{
    using namespace Astra::Test;

    Astra::Entity e = registry->CreateEntityWith(Position{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(registry->AddComponentByID(e, Astra::TypeID<Player>::Value(), nullptr, 0));

    uint64_t playerHash = Astra::TypeID<Player>::Hash();
    EXPECT_TRUE(registry->HasComponentByHash(e, playerHash));
    EXPECT_NE(registry->GetComponentByHash(e, playerHash), nullptr);   // BUG: nullptr contradicts Has

    // A component the entity does NOT have still returns nullptr.
    EXPECT_EQ(registry->GetComponentByHash(e, Astra::TypeID<Enemy>::Hash()), nullptr);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.GetComponentByHashTagAgreesWithHas
```
Expected: FAIL — `GetComponentByHash` returns `nullptr` for the present `Player` tag while `HasComponentByHash` is true.

- [ ] **Step 3: Apply the fix**

In `include/Astra/Registry/Registry.hpp`, `GetComponentByHash`'s tail currently reads:
```cpp
            auto& chunks = record->archetype->GetChunks();
            if (record->location.GetChunkIndex() >= chunks.size())
                return nullptr;

            void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
            if (!compArray)
                return nullptr;

            return static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
```
Replace it with a mask-based presence check (mirroring `HasComponentByHash`) that returns the sentinel for a present tag:
```cpp
            // Presence is the archetype mask, not the chunk array (which is null for a
            // size==0 tag). This makes Get(...) != nullptr agree with Has(...).
            if (!record->archetype->GetMask().Test(componentId))
                return nullptr;                     // entity does not have the component

            if (desc->size == 0)
                return EmptyComponentSentinel();     // present tag: no data

            auto& chunks = record->archetype->GetChunks();
            if (record->location.GetChunkIndex() >= chunks.size())
                return nullptr;

            void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
            if (!compArray)
                return nullptr;                      // defensive (should not happen for size > 0)

            return static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=RegistryTest.GetComponentByHashTagAgreesWithHas:ReflectionTest.*
```
Expected: PASS (the new test and the existing `ReflectionTest` `GetComponentByHash` tests, which use a non-empty component and are unaffected).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistryTest.cpp
git commit -m "fix(registry): GetComponentByHash returns sentinel for a present tag (Theme F)"
```

---

### Task 4: D2 — SmallVector alignment-aware allocation

**Files:**
- Modify: `include/Astra/Container/SmallVector.hpp` (add `<new>`; add `Allocate`/`Deallocate` helpers; route 7 sites)
- Test: `tests/Container/SmallVectorTest.cpp` (append)

**Interfaces:**
- Produces: nothing consumed by later tasks. Private static `Allocate(size_type)` / `Deallocate(T*)` centralize all heap alloc/dealloc.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Container/SmallVectorTest.cpp`:

```cpp
namespace
{
    struct alignas(64) Over64 { std::byte pad[64]; };
}

// Theme D2: SmallVector heap storage must honor alignof(T) for over-aligned T.
// The bug (plain ::operator new) yields only __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16),
// so a spilled buffer is usually mis-aligned; 32 independent spilled vectors make a
// coincidental all-64-aligned RED astronomically unlikely.
TEST_F(SmallVectorTest, OverAlignedHeapStorageIsAligned)
{
    constexpr int kInstances = 32;
    std::vector<std::unique_ptr<Astra::SmallVector<Over64, 2>>> keep;
    for (int i = 0; i < kInstances; ++i)
    {
        auto v = std::make_unique<Astra::SmallVector<Over64, 2>>();
        for (int j = 0; j < 8; ++j)   // > inline capacity (2) -> spills to heap
            v->push_back(Over64{});
        ASSERT_GT(v->capacity(), 2u); // confirm it is on the heap
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(v->data()) % alignof(Over64), 0u);
        keep.push_back(std::move(v)); // keep alive so allocations don't get reused
    }
}
```
(Ensure `SmallVectorTest.cpp` includes `<cstdint>`, `<memory>`, and `<cstddef>` — add any that are missing to the top includes.)

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SmallVectorTest.OverAlignedHeapStorageIsAligned
```
Expected: FAIL — at least one spilled vector's `data()` is not 64-aligned (plain `::operator new` gives 16-byte alignment).

- [ ] **Step 3: Apply the fix**

In `include/Astra/Container/SmallVector.hpp`: ensure `#include <new>` is present near the top (for `std::align_val_t`); add it if missing. In the private section (near `IsSmall()` / `GetBuffer()`), add two static helpers:
```cpp
        static T* Allocate(size_type count)
        {
            if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{alignof(T)}));
            else
                return static_cast<T*>(::operator new(count * sizeof(T)));
        }

        static void Deallocate(T* ptr) noexcept
        {
            if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                ::operator delete(ptr, std::align_val_t{alignof(T)});
            else
                ::operator delete(ptr);
        }
```
Then route every raw site through them (the `if constexpr` guarantees `new`/`delete` forms are paired):
- `~SmallVector` (`:124`): `::operator delete(m_data);` → `Deallocate(m_data);`
- move-assign (`:145`): `::operator delete(m_data);` → `Deallocate(m_data);`
- `shrink_to_fit` (`:302`): `::operator delete(heapData);` → `Deallocate(heapData);`
- `shrink_to_fit` (`:310`): `T* newData = static_cast<T*>(::operator new(m_size * sizeof(T)));` → `T* newData = Allocate(m_size);`
- `shrink_to_fit` (`:313`): `::operator delete(m_data);` → `Deallocate(m_data);`
- `Grow` (`:584`): `T* newData = static_cast<T*>(::operator new(newCap * sizeof(T)));` → `T* newData = Allocate(newCap);`
- `Grow` (`:595`): `::operator delete(m_data);` → `Deallocate(m_data);`

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SmallVectorTest.*
```
Expected: PASS (the new test and all existing `SmallVectorTest.*`).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Container/SmallVector.hpp tests/Container/SmallVectorTest.cpp
git commit -m "fix(container): SmallVector heap storage honors alignof(T) (Theme D)"
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
Expected: all green in all three configs. Four new tests pass (F1 in `ComponentDescriptorTagTest`, F2+F3 in `RegistryTest`, D2 in `SmallVectorTest`); the Debug/Release count delta is the pre-existing Debug-only `EXPECT_DEATH` tests. If only `CompressionTest.PerformanceBenchmark` fails, rerun it isolated (known flake).

- [ ] **Step 2: Confirm no `ide/` changes are staged**

Run: `git status`
Expected: only `include/…`, `tests/…`, `docs/…` changes. `ide/` must NOT appear.

---

## Self-Review

**1. Spec coverage:**
- Spec §2 sentinel → Task 1. ✓
- Spec §2 F1 (`Destruct` skips size==0) → Task 1. ✓
- Spec §2 F2 (by-ID tag signals) → Task 2. ✓
- Spec §2 F3 (`GetComponentByHash` sentinel + mask presence) → Task 3. ✓
- Spec §2 D2 (SmallVector aligned alloc) → Task 4. ✓
- Spec §5 deferred D items (D1-honor/D3/D4') → correctly absent. ✓
- Spec §6 acceptance (3 configs green; RED→GREEN; Get==Has for tags; aligned heap) → Task 5 + Global Constraints. ✓

**2. Placeholder scan:** No TBD/TODO; every code and test step shows complete code. ✓

**3. Type consistency:** `EmptyComponentSentinel()` (Task 1) is used in Tasks 2 and 3. `Allocate`/`Deallocate` (Task 4) route all 7 sites. `Astra::Events::ComponentAdded/Removed::component` (`void*`) is read in Task 2; `HasComponentByHash`/`GetComponentByHash` return-consistency is the Task 3 assertion. ✓

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-20-astra-theme-fd-tags-and-alignment.md`.**
