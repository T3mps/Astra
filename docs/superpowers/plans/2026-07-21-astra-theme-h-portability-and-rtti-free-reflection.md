# Theme H — Portability Hardening + RTTI-Free Reflection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the reachable Theme H portability defects (enforce 64-bit; robust entity version-bit math) and make Astra's reflection dynamic path RTTI-free by replacing `std::any` with an Astra-`TypeID`-tagged `AnyValue` carrier.

**Architecture:** H1 adds a compile-time `static_assert` that enforces the 64-bit-only contract at the one site that shifts a `size_t`. H3 extracts a mask-correct `NextEntityVersion` helper (correct for any `VersionBits`, not just {8,16,32}) and routes both `EntityManager` recycle sites through it, plus a missing `static_assert(VersionBits >= 1)`. H4 introduces `Astra::AnyValue` — an SBO + aligned-heap, `TypeID::Hash()`-tagged type-erased value box (mirroring the `Delegate` op-manager idiom) — and migrates `FieldInfo`/`TypeMeta` and the reflection tests off `std::any`, deleting the `<any>` include.

**Tech Stack:** Header-only C++20 archetype ECS (MSVC, x64). GoogleTest. Build via `Astra.sln`.

## Global Constraints

- **All three configs must build clean and green:** Debug, Release, Dist (`-p:Platform=x64`).
- **Uniform-graceful:** no assert-and-abort on a recoverable condition. A wrong-`T` `AnyValue::TryCast` returns `nullptr`; a mismatched `SetAny` returns `false`; never throw.
- **RTTI-free by construction:** after H4, `std::any`, `any_cast`, `<any>`, `typeid`, and `type_info` must NOT appear anywhere under `include/Astra/Reflection/`. This is the H4 acceptance gate.
- **TypeID ceiling (~128):** net new **component** IDs must be **0**. `AnyValue` uses `TypeID<T>::Hash()` (the stable hash), NOT `TypeID<T>::Value()` (the dense ComponentID assigner) — so no `AnyValue` test type consumes a ComponentID. H3 uses a config-independent free helper (no types registered). Give any genuinely new type a UNIQUE name.
- **No `ide/` regen:** `AnyValue.hpp` is a header (included, never compiled standalone) and all tests APPEND to existing `.cpp` files — so no new build target. Do NOT `git add ide/`.
- **Build (Debug example):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m`
- **Test exe:** `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (swap `Debug` for `Release`/`Dist`). Filter with `--gtest_filter=`.
- **IDE clang diagnostics are false positives — judge only by the MSVC build.**

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `include/Astra/Container/Swiss.hpp` | Swiss-table hashing | H1 (`static_assert(sizeof(size_t)==8)`) |
| `include/Astra/Entity/Entity.hpp` | Entity traits + hashing | H3 (`static_assert(VersionBits>=1)` + `NextEntityVersion` helper) |
| `include/Astra/Entity/EntityManager.hpp` | Entity lifecycle | H3 (route Destroy/DestroyBatch through `NextEntityVersion`) |
| `include/Astra/Reflection/AnyValue.hpp` | RTTI-free value carrier | **NEW** (H4) |
| `include/Astra/Reflection/FieldInfo.hpp` | Field descriptor | H4 (`AnyValue` accessors; drop `<any>`) |
| `include/Astra/Reflection/TypeMeta.hpp` | Type metadata | H4 (`AnyValue` by-name wrappers) |
| `tests/Entity/EntityTest.cpp` | Entity tests | H3 test (append) |
| `tests/Reflection/ReflectionTest.cpp` | Reflection tests | AnyValue unit tests + FieldGetSetAny migration (append/edit) |
| `tests/Reflection/FieldVisitorTest.cpp` | Visitor tests | migrate map blob off `std::any` (edit) |

---

### Task 1: H1 — enforce the 64-bit contract

**Files:**
- Modify: `include/Astra/Container/Swiss.hpp` (near `H2`, ~`40-48`)

**Interfaces:**
- Produces: nothing consumed by later tasks. A compile-time guarantee that `size_t` is 64-bit.

- [ ] **Step 1: Confirm the site and the already-safe sibling**

`Swiss.hpp:46` shifts a `size_t`: `uint8_t h2 = static_cast<uint8_t>(hash >> 57) & 0x7F;` — UB if `size_t` were 32-bit. (`Entity.hpp:165` performs the same `>> 57` but on a `uint64_t hash`, so it is already safe on any width and needs NO change — do not edit it.)

- [ ] **Step 2: Add the enforcing static_assert**

In `include/Astra/Container/Swiss.hpp`, ensure `#include <cstddef>` is present (for `std::size_t`; add if missing). Then, at namespace scope immediately BEFORE the `inline uint8_t H2(size_t hash) noexcept` function, add:

```cpp
    // Astra targets 64-bit platforms only. The Swiss-table H2 control byte is the
    // top 7 bits of a 64-bit hash word (`hash >> 57`); on a 32-bit size_t that
    // shift is UB. Fail loudly at compile time rather than degrade silently.
    static_assert(sizeof(std::size_t) == 8, "Astra targets 64-bit platforms only (Swiss H2 assumes a 64-bit hash word).");
```

- [ ] **Step 3: Build Debug and confirm it still compiles green**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SwissMapTest.*:FlatMapTest.*
```
Expected: builds clean (the `static_assert` holds on x64); the map suites pass. (There is no runtime test — a `static_assert` is its own test; a 32-bit build is not in CI.)

- [ ] **Step 4: Commit**

```bash
git add include/Astra/Container/Swiss.hpp
git commit -m "fix(container): static_assert 64-bit size_t at the Swiss H2 shift (Theme H)"
```

---

### Task 2: H3 — entity version-bit robustness

**Files:**
- Modify: `include/Astra/Entity/Entity.hpp` (`EntityTraits` ~`112-131`; add a `Detail::NextEntityVersion` free helper)
- Modify: `include/Astra/Entity/EntityManager.hpp` (`Destroy` ~`122-127`, `DestroyBatch` ~`171-176`)
- Test: `tests/Entity/EntityTest.cpp` (append)

**Interfaces:**
- Produces: `template<typename V> constexpr V Astra::Detail::NextEntityVersion(V current, V versionMask, V nullVersion, V initialVersion) noexcept` — next entity version with wraparound at `versionMask`, skipping `nullVersion`. Correct for any version-bit width.

- [ ] **Step 1: Write the failing test**

Append to the end of `tests/Entity/EntityTest.cpp`:

```cpp
// Theme H3: version wraparound must key off the version MASK, not the VersionType's
// natural width, so it is correct for a non-byte-width version field (e.g. 12 bits).
TEST(EntityVersionWrap, WrapsAtMaskForNonByteWidth)
{
    using Astra::Detail::NextEntityVersion;

    // 12-bit version field: VersionType would be uint16_t, but the field wraps at
    // 4095 (VERSION_MASK), NOT at uint16 max. Old (v+1)==NULL logic gives 4096 here
    // (not 0), which then packs to 0 = invalid -> the recycled entity looks dead.
    EXPECT_EQ(NextEntityVersion<uint16_t>(5,    4095, 0, 1), 6);
    EXPECT_EQ(NextEntityVersion<uint16_t>(4094, 4095, 0, 1), 4095);
    EXPECT_EQ(NextEntityVersion<uint16_t>(4095, 4095, 0, 1), 1);   // wrap 4095 -> skip 0 -> 1

    // 8-bit version field still wraps correctly (regression guard).
    EXPECT_EQ(NextEntityVersion<uint8_t>(255, 255, 0, 1), 1);
    EXPECT_EQ(NextEntityVersion<uint8_t>(7,   255, 0, 1), 8);
}
```

- [ ] **Step 2: Build Debug and run the test to verify it FAILS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: FAIL — compile error `NextEntityVersion is not a member of Astra::Detail` (the helper does not exist yet).

- [ ] **Step 3: Add the `static_assert` and the helper**

In `include/Astra/Entity/Entity.hpp`, inside `struct EntityTraits`, after the existing `static_assert(VersionBits < TotalBits, ...)` (line ~116), add:
```cpp
        static_assert(VersionBits >= 1, "Version bits must be at least 1");
```

Then, inside `namespace Astra::Detail` (co-located with `BasicEntity`, e.g. immediately after the `BasicEntity` class's closing `};` and before the `Detail` namespace closes), add:
```cpp
        // Next entity version with wraparound at `versionMask`, skipping the reserved
        // `nullVersion` (recycles to `initialVersion`). Masking to the version field
        // width makes this correct for ANY VersionBits, not just {8,16,32} -- a field
        // narrower than its VersionType (e.g. 12 bits in a uint16) wraps at the mask,
        // never at the type's natural width.
        template<typename V>
        ASTRA_NODISCARD constexpr V NextEntityVersion(V current, V versionMask, V nullVersion, V initialVersion) noexcept
        {
            const V next = static_cast<V>((current + 1) & versionMask);
            return (next == nullVersion) ? initialVersion : next;
        }
```

- [ ] **Step 4: Rebuild Debug and run the test to verify it PASSES**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=EntityVersionWrap.*
```
Expected: PASS.

- [ ] **Step 5: Route both EntityManager recycle sites through the helper (DRY)**

In `include/Astra/Entity/EntityManager.hpp`, `Destroy` currently reads (~`122-127`):
```cpp
            // Calculate next version with wraparound
            VersionType nextVersion = currentVersion + 1;
            if (nextVersion == NULL_VERSION) ASTRA_UNLIKELY  // Wrap from 255 to 1
            {
                nextVersion = INITIAL_VERSION;
            }
```
Replace with:
```cpp
            // Calculate next version with wraparound (mask-correct for any VersionBits)
            const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);
```

In `DestroyBatch` (~`171-176`), replace the identical block:
```cpp
                // Calculate next version
                VersionType nextVersion = currentVersion + 1;
                if (nextVersion == NULL_VERSION) ASTRA_UNLIKELY
                {
                    nextVersion = INITIAL_VERSION;
                }
```
with:
```cpp
                // Calculate next version (mask-correct for any VersionBits)
                const VersionType nextVersion = Detail::NextEntityVersion<VersionType>(
                    currentVersion, static_cast<VersionType>(Entity::VERSION_MASK), NULL_VERSION, INITIAL_VERSION);
```

- [ ] **Step 6: Rebuild Debug and run the entity suites to confirm no regression**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=EntityVersionWrap.*:EntityManagerTest.*:EntityTest.*
```
Expected: all pass (the default 8-bit config is byte-for-byte unchanged; `NextEntityVersion<uint8_t>(255,255,0,1)==1` matches the old `255+1==0 -> 1`).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Entity/Entity.hpp include/Astra/Entity/EntityManager.hpp tests/Entity/EntityTest.cpp
git commit -m "fix(entity): mask-correct version wraparound for any VersionBits + static_assert bits>=1 (Theme H)"
```

---

### Task 3: `AnyValue` — the RTTI-free value carrier

**Files:**
- Create: `include/Astra/Reflection/AnyValue.hpp`
- Test: `tests/Reflection/ReflectionTest.cpp` (append; add the include)

**Interfaces:**
- Produces: `class Astra::AnyValue` — value-semantic, RTTI-free, `TypeID::Hash()`-tagged type-erased box. Key surface consumed by Task 4:
  - `AnyValue()` (empty); `template<class T> explicit AnyValue(T&& value)`
  - `template<class T> const std::decay_t<T>* TryCast() const noexcept` (+ non-const)
  - `bool HasValue() const noexcept`; `uint64_t TypeHash() const noexcept`
  - copy/move ctor + assign; destructor.

- [ ] **Step 1: Write the failing tests**

At the top of `tests/Reflection/ReflectionTest.cpp`, add the include alongside the existing reflection includes (match the file's include style):
```cpp
#include <Astra/Reflection/AnyValue.hpp>
```
Then append to the end of `tests/Reflection/ReflectionTest.cpp`:
```cpp
namespace
{
    // File-local lifetime counter (NOT a component: AnyValue uses TypeID<T>::Hash(),
    // which does not assign a dense ComponentID, so this consumes zero ceiling).
    struct AnyLifetime
    {
        static inline int s_live = 0;
        int tag = 0;
        AnyLifetime() { ++s_live; }
        explicit AnyLifetime(int t) : tag(t) { ++s_live; }
        AnyLifetime(const AnyLifetime& o) : tag(o.tag) { ++s_live; }
        AnyLifetime(AnyLifetime&& o) noexcept : tag(o.tag) { ++s_live; }
        AnyLifetime& operator=(const AnyLifetime&) = default;
        AnyLifetime& operator=(AnyLifetime&&) = default;
        ~AnyLifetime() { --s_live; }
    };

    // Larger than AnyValue's 16-byte inline buffer -> forces the heap path.
    struct AnyBig { double a, b, c, d; };   // 32 bytes

    // Over-aligned -> forces the aligned-heap path.
    struct alignas(64) AnyOver64 { std::byte pad[64]; };
}

TEST(AnyValueTest, ConstructAndTryCastMatch)
{
    Astra::AnyValue v(42);
    ASSERT_TRUE(v.HasValue());
    ASSERT_NE(v.TryCast<int>(), nullptr);
    EXPECT_EQ(*v.TryCast<int>(), 42);
    EXPECT_EQ(v.TypeHash(), Astra::TypeID<int>::Hash());
}

TEST(AnyValueTest, TryCastWrongTypeReturnsNull)
{
    Astra::AnyValue v(3.5f);
    EXPECT_NE(v.TryCast<float>(), nullptr);
    EXPECT_EQ(v.TryCast<int>(), nullptr);       // wrong T -> null, never UB
    EXPECT_EQ(v.TryCast<double>(), nullptr);
}

TEST(AnyValueTest, EmptyHasNoValue)
{
    Astra::AnyValue v;
    EXPECT_FALSE(v.HasValue());
    EXPECT_EQ(v.TryCast<int>(), nullptr);
}

TEST(AnyValueTest, HeapPathForLargeType)
{
    Astra::AnyValue v(AnyBig{1, 2, 3, 4});
    ASSERT_NE(v.TryCast<AnyBig>(), nullptr);
    EXPECT_EQ(v.TryCast<AnyBig>()->c, 3.0);
}

TEST(AnyValueTest, OverAlignedHeapPathIsAligned)
{
    Astra::AnyValue v{AnyOver64{}};
    ASSERT_NE(v.TryCast<AnyOver64>(), nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(v.TryCast<AnyOver64>()) % alignof(AnyOver64), 0u);
}

TEST(AnyValueTest, CopyIsIndependentAndBalancesLifetime)
{
    AnyLifetime::s_live = 0;
    {
        Astra::AnyValue a(AnyLifetime{7});
        Astra::AnyValue b = a;                  // copy
        ASSERT_NE(a.TryCast<AnyLifetime>(), nullptr);
        ASSERT_NE(b.TryCast<AnyLifetime>(), nullptr);
        EXPECT_EQ(b.TryCast<AnyLifetime>()->tag, 7);
        EXPECT_NE(a.TryCast<AnyLifetime>(), b.TryCast<AnyLifetime>());  // distinct storage
    }
    EXPECT_EQ(AnyLifetime::s_live, 0);          // no leak, no double-free
}

TEST(AnyValueTest, MoveTransfersAndLeavesSourceEmpty)
{
    AnyLifetime::s_live = 0;
    {
        Astra::AnyValue a(AnyLifetime{9});
        Astra::AnyValue b = std::move(a);
        EXPECT_FALSE(a.HasValue());
        ASSERT_NE(b.TryCast<AnyLifetime>(), nullptr);
        EXPECT_EQ(b.TryCast<AnyLifetime>()->tag, 9);
    }
    EXPECT_EQ(AnyLifetime::s_live, 0);
}
```
(Ensure `ReflectionTest.cpp` includes `<cstdint>` for `std::uintptr_t` and `<utility>` for `std::move` — add any missing to the top includes.)

- [ ] **Step 2: Build Debug and run the tests to verify they FAIL**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: FAIL — compile error, `AnyValue.hpp` file not found / `Astra::AnyValue` undefined.

- [ ] **Step 3: Create `AnyValue.hpp`**

Create `include/Astra/Reflection/AnyValue.hpp` with exactly:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "../Core/Base.hpp"
#include "../Core/TypeID.hpp"

namespace Astra
{
    // Type-erased, RTTI-free single-value box tagged by Astra TypeID::Hash().
    // Replaces std::any on the reflection dynamic path so reflection builds and runs
    // with RTTI disabled -- deterministically, and correctly across DSO/DLL boundaries
    // (a stable name-hash tag, unlike std::any's manager-pointer / typeid identity that
    // -fno-rtti compiles out). Value semantics; a wrong-T TryCast returns nullptr (never
    // UB, never throws). SBO keeps common reflected field types (int/float/Vec3/Vec4/
    // pointer) allocation-free; larger/over-aligned types spill to an aligned heap block.
    // The manager is a per-type function-pointer vtable (destroy/copy/move) -- no
    // typeid, no std::type_info. Astra is exception-free, so constructors are assumed
    // non-throwing (a throwing ctor / bad_alloc terminates, as everywhere in Astra).
    class AnyValue
    {
    public:
        static constexpr std::size_t kInlineSize  = 16;                       // two doubles (EnTT/Folly default)
        static constexpr std::size_t kInlineAlign = alignof(std::max_align_t);

        AnyValue() noexcept = default;

        template<typename T, typename D = std::decay_t<T>,
                 typename = std::enable_if_t<!std::is_same_v<D, AnyValue>>>
        explicit AnyValue(T&& value)
        {
            EmplaceImpl<D>(std::forward<T>(value));
        }

        AnyValue(const AnyValue& other) { CopyFrom(other); }
        AnyValue(AnyValue&& other) noexcept { MoveFrom(other); }

        AnyValue& operator=(const AnyValue& other)
        {
            if (this != &other) { Reset(); CopyFrom(other); }
            return *this;
        }
        AnyValue& operator=(AnyValue&& other) noexcept
        {
            if (this != &other) { Reset(); MoveFrom(other); }
            return *this;
        }
        ~AnyValue() { Reset(); }

        ASTRA_NODISCARD bool HasValue() const noexcept { return m_vtable != nullptr; }
        ASTRA_NODISCARD uint64_t TypeHash() const noexcept { return m_typeHash; }

        template<typename T, typename D = std::decay_t<T>>
        ASTRA_NODISCARD const D* TryCast() const noexcept
        {
            if (!m_vtable || m_typeHash != TypeID<D>::Hash()) return nullptr;
            return static_cast<const D*>(Data());
        }
        template<typename T, typename D = std::decay_t<T>>
        ASTRA_NODISCARD D* TryCast() noexcept
        {
            if (!m_vtable || m_typeHash != TypeID<D>::Hash()) return nullptr;
            return static_cast<D*>(Data());
        }

    private:
        struct VTable
        {
            void (*destroy)(void* obj);
            void (*copyConstruct)(void* dst, const void* src);
            void (*moveConstruct)(void* dst, void* src);
            std::size_t size;
            std::size_t align;
        };

        template<typename T>
        static const VTable* VTableFor() noexcept
        {
            static const VTable vt{
                [](void* obj) { static_cast<T*>(obj)->~T(); },
                [](void* dst, const void* src) { ::new (dst) T(*static_cast<const T*>(src)); },
                [](void* dst, void* src) { ::new (dst) T(std::move(*static_cast<T*>(src))); },
                sizeof(T),
                alignof(T)
            };
            return &vt;
        }

        template<typename T>
        static constexpr bool FitsInline() noexcept
        {
            return sizeof(T) <= kInlineSize
                && alignof(T) <= kInlineAlign
                && std::is_nothrow_move_constructible_v<T>;
        }

        static void* Allocate(std::size_t size, std::size_t align)
        {
            if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                return ::operator new(size, std::align_val_t{align});
            return ::operator new(size);
        }
        static void Deallocate(void* ptr, std::size_t align) noexcept
        {
            if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                ::operator delete(ptr, std::align_val_t{align});
            else
                ::operator delete(ptr);
        }

        void* Data() noexcept { return m_heap ? m_heap : static_cast<void*>(m_inline); }
        const void* Data() const noexcept { return m_heap ? m_heap : static_cast<const void*>(m_inline); }

        template<typename T, typename Arg>
        void EmplaceImpl(Arg&& arg)
        {
            void* storage;
            if constexpr (FitsInline<T>())
            {
                m_heap = nullptr;
                storage = m_inline;
            }
            else
            {
                m_heap = Allocate(sizeof(T), alignof(T));
                storage = m_heap;
            }
            ::new (storage) T(std::forward<Arg>(arg));
            m_typeHash = TypeID<T>::Hash();
            m_vtable = VTableFor<T>();      // set last: a hypothetical throw leaves us empty-safe
        }

        void CopyFrom(const AnyValue& other)
        {
            if (!other.m_vtable) return;    // stay empty
            void* storage;
            if (other.m_heap)               // same T => same inline/heap decision
            {
                m_heap = Allocate(other.m_vtable->size, other.m_vtable->align);
                storage = m_heap;
            }
            else
            {
                m_heap = nullptr;
                storage = m_inline;
            }
            other.m_vtable->copyConstruct(storage, other.Data());
            m_typeHash = other.m_typeHash;
            m_vtable = other.m_vtable;
        }

        void MoveFrom(AnyValue& other) noexcept
        {
            if (!other.m_vtable) return;    // stay empty
            if (other.m_heap)               // steal the heap block, no element move
            {
                m_heap = other.m_heap;
                m_typeHash = other.m_typeHash;
                m_vtable = other.m_vtable;
                other.m_heap = nullptr;
                other.m_vtable = nullptr;
                other.m_typeHash = 0;
            }
            else                            // inline: move-construct (nothrow by FitsInline)
            {
                m_heap = nullptr;
                other.m_vtable->moveConstruct(m_inline, other.Data());
                m_typeHash = other.m_typeHash;
                m_vtable = other.m_vtable;
                other.Reset();
            }
        }

        void Reset() noexcept
        {
            if (m_vtable)
            {
                m_vtable->destroy(Data());
                if (m_heap) Deallocate(m_heap, m_vtable->align);
            }
            m_vtable = nullptr;
            m_heap = nullptr;
            m_typeHash = 0;
        }

        alignas(kInlineAlign) std::byte m_inline[kInlineSize];
        void* m_heap = nullptr;
        const VTable* m_vtable = nullptr;
        uint64_t m_typeHash = 0;
    };
}
```

- [ ] **Step 4: Rebuild Debug and run the tests to verify they PASS**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=AnyValueTest.*
```
Expected: all 7 `AnyValueTest.*` pass (inline int/float, wrong-type null, empty, heap `AnyBig`, aligned `AnyOver64`, copy-independence + lifetime balance, move-empties-source).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Reflection/AnyValue.hpp tests/Reflection/ReflectionTest.cpp
git commit -m "feat(reflection): add RTTI-free AnyValue type-erased carrier (Theme H)"
```

---

### Task 4: Migrate reflection off `std::any`

**Files:**
- Modify: `include/Astra/Reflection/FieldInfo.hpp` (drop `<any>`; `getterAny`/`setterAny` fields + builder ~`53-57,385-403`; `GetAny`/`SetAny` ~`151-173`)
- Modify: `include/Astra/Reflection/TypeMeta.hpp` (`GetFieldValueAny`/`SetFieldValueAny` ~`207-232`; ensure `AnyValue` is included)
- Test: `tests/Reflection/ReflectionTest.cpp` (edit `FieldGetSetAny` ~`223`, and the `GetAny` use ~`940`)
- Test: `tests/Reflection/FieldVisitorTest.cpp` (migrate the `std::any` map blob)

**Interfaces:**
- Consumes: `Astra::AnyValue` (Task 3) — `AnyValue(value)`, `TryCast<T>()`, value/copy semantics.
- Produces: `FieldInfo::GetAny(...) -> AnyValue`, `FieldInfo::SetAny(const AnyValue&) -> bool`, `TypeMeta::GetFieldValueAny(...) -> AnyValue`, `TypeMeta::SetFieldValueAny(..., const AnyValue&) -> bool`.

- [ ] **Step 1: Migrate the tests first (they define the target API — RED)**

In `tests/Reflection/ReflectionTest.cpp`, `FieldGetSetAny` (~`223`) currently reads:
```cpp
    std::any zAny = zField->GetAny(&pos);
    EXPECT_FLOAT_EQ(std::any_cast<float>(zAny), 3.0f);
    ...
    bool success = zField->SetAny(&pos, std::any(300.0f));
```
Change those lines to:
```cpp
    Astra::AnyValue zAny = zField->GetAny(&pos);
    ASSERT_NE(zAny.TryCast<float>(), nullptr);
    EXPECT_FLOAT_EQ(*zAny.TryCast<float>(), 3.0f);
    ...
    bool success = zField->SetAny(&pos, Astra::AnyValue(300.0f));
```
(Keep the rest of the test — the `EXPECT_TRUE(success)` and the follow-up field read — as-is.)

At ~`940`, the block (inside a `ForEachField` lambda) reads:
```cpp
            std::any value = field.GetAny(info.data);
            EXPECT_TRUE(value.has_value());
```
Change it to:
```cpp
            Astra::AnyValue value = field.GetAny(info.data);
            EXPECT_TRUE(value.HasValue());
```

In `tests/Reflection/FieldVisitorTest.cpp`: remove `#include <any>`; replace every `std::any` with `Astra::AnyValue` (the map type `std::map<std::string, std::any>` → `std::map<std::string, Astra::AnyValue>`, the `MapWriteVisitor`/`MapReadVisitor` members, and the local `blob`). The write path `out[...] = field.GetAny(instance);` and read path `field.SetAny(instance, it->second);` need no change beyond the type swap (`AnyValue` is copyable and the map stores it by value). Ensure `#include <Astra/Reflection/AnyValue.hpp>` is present (it arrives transitively via `FieldInfo.hpp` after this task, but add it explicitly for clarity).

- [ ] **Step 2: Build Debug and confirm the tests FAIL to compile**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
Expected: FAIL — `GetAny` still returns `std::any` / `SetAny` takes `std::any`, so `TryCast` on the result and passing an `AnyValue` do not compile.

- [ ] **Step 3: Migrate `FieldInfo.hpp`**

In `include/Astra/Reflection/FieldInfo.hpp`:

1. Delete `#include <any>` (line 3). Add `#include "AnyValue.hpp"` in the local-include block (near `#include "Attribute.hpp"`).

2. Change the two field declarations (~`53-57`):
```cpp
        // Getter returning std::any for dynamic type handling
        std::function<std::any(const void* instance)> getterAny;
        // Setter accepting std::any for dynamic type handling
        std::function<bool(void* instance, const std::any& value)> setterAny;
```
to:
```cpp
        // Getter returning AnyValue for dynamic (type-erased, RTTI-free) handling
        std::function<AnyValue(const void* instance)> getterAny;
        // Setter accepting AnyValue for dynamic (type-erased, RTTI-free) handling
        std::function<bool(void* instance, const AnyValue& value)> setterAny;
```

3. Change `GetAny`/`SetAny` (~`151-173`):
```cpp
        ASTRA_NODISCARD std::any GetAny(const void* instance) const
        {
            if (getterAny)
            {
                return getterAny(instance);
            }
            return std::any{};
        }
        ...
        bool SetAny(void* instance, const std::any& value) const
        {
            if (isConst || !setterAny)
            {
                return false;
            }
            return setterAny(instance, value);
        }
```
to:
```cpp
        ASTRA_NODISCARD AnyValue GetAny(const void* instance) const
        {
            if (getterAny)
            {
                return getterAny(instance);
            }
            return AnyValue{};
        }
        ...
        bool SetAny(void* instance, const AnyValue& value) const
        {
            if (isConst || !setterAny)
            {
                return false;
            }
            return setterAny(instance, value);
        }
```

4. Change the builder lambdas (~`385-403`):
```cpp
            // std::any getter
            info.getterAny = [](const void* instance) -> std::any {
                const Class* obj = static_cast<const Class*>(instance);
                return std::any(obj->*FieldPtr);
            };

            // std::any setter (uses pointer overload to avoid exceptions)
            if constexpr (!std::is_const_v<FieldType>)
            {
                info.setterAny = [](void* instance, const std::any& value) -> bool {
                    const DecayedType* ptr = std::any_cast<DecayedType>(&value);
                    if (!ptr)
                    {
                        return false;
                    }
                    Class* obj = static_cast<Class*>(instance);
                    obj->*FieldPtr = *ptr;
                    return true;
                };
            }
```
to:
```cpp
            // AnyValue getter (RTTI-free)
            info.getterAny = [](const void* instance) -> AnyValue {
                const Class* obj = static_cast<const Class*>(instance);
                return AnyValue(obj->*FieldPtr);
            };

            // AnyValue setter (TryCast returns null on type mismatch -- never throws)
            if constexpr (!std::is_const_v<FieldType>)
            {
                info.setterAny = [](void* instance, const AnyValue& value) -> bool {
                    const DecayedType* ptr = value.TryCast<DecayedType>();
                    if (!ptr)
                    {
                        return false;
                    }
                    Class* obj = static_cast<Class*>(instance);
                    obj->*FieldPtr = *ptr;
                    return true;
                };
            }
```

- [ ] **Step 4: Migrate `TypeMeta.hpp`**

In `include/Astra/Reflection/TypeMeta.hpp`, add `#include "AnyValue.hpp"` (near the existing local includes) so `AnyValue` is visible regardless of include order. Then change `GetFieldValueAny`/`SetFieldValueAny` (~`207-232`):
```cpp
        ASTRA_NODISCARD std::any GetFieldValueAny(const void* instance, std::string_view fieldName) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field)
            {
                return std::any{};
            }
            return field->GetAny(instance);
        }
        ...
        bool SetFieldValueAny(void* instance, std::string_view fieldName, const std::any& value) const
        {
```
to:
```cpp
        ASTRA_NODISCARD AnyValue GetFieldValueAny(const void* instance, std::string_view fieldName) const
        {
            const FieldInfo* field = GetField(fieldName);
            if (!field)
            {
                return AnyValue{};
            }
            return field->GetAny(instance);
        }
        ...
        bool SetFieldValueAny(void* instance, std::string_view fieldName, const AnyValue& value) const
        {
```
(the `SetFieldValueAny` body — `GetField` guard then `return field->SetAny(instance, value);` — is unchanged.)

- [ ] **Step 5: Rebuild Debug and run the reflection suites to verify GREEN**

Run:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=ReflectionTest.*:FieldVisitorTest.*:AnyValueTest.*
```
Expected: all pass (the migrated `FieldGetSetAny`, the visitor round-trip through the `AnyValue` map, and the Task-3 `AnyValueTest.*`).

- [ ] **Step 6: Verify the structural RTTI-free property**

Run (Git Bash):
```
grep -rn "std::any\|any_cast\|<any>\|typeid\|type_info" include/Astra/Reflection/
```
Expected: **no output** (empty). If anything prints, migrate that site before committing.

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Reflection/FieldInfo.hpp include/Astra/Reflection/TypeMeta.hpp tests/Reflection/ReflectionTest.cpp tests/Reflection/FieldVisitorTest.cpp
git commit -m "refactor(reflection): replace std::any with RTTI-free AnyValue on the dynamic path (Theme H)"
```

---

### Task 5: Full 3-config verification + structural proof

**Files:** none (verification only).

- [ ] **Step 1: Build and test all three configs**

For each of `Debug`, `Release`, `Dist`:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Config> -p:Platform=x64 -m
bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe
```
Expected: all green in all three configs. New tests: `EntityVersionWrap.WrapsAtMaskForNonByteWidth` and 7 `AnyValueTest.*`; migrated `ReflectionTest.FieldGetSetAny` and `FieldVisitorTest.*` still pass. The Debug/Release count delta is the pre-existing Debug-only death tests. If only `CompressionTest.PerformanceBenchmark` fails, rerun it isolated (known flake).

- [ ] **Step 2: Confirm the RTTI-free structural gate and no `ide/` changes**

Run:
```
grep -rn "std::any\|any_cast\|<any>\|typeid\|type_info" include/Astra/Reflection/
git status
```
Expected: the grep prints nothing; `git status` shows only `include/…`, `tests/…`, `docs/…` changes — `ide/` must NOT appear.

---

## Self-Review

**1. Spec coverage:**
- Spec H1 (enforce 64-bit) → Task 1. ✓
- Spec H3 (`static_assert(VersionBits>=1)` + `VERSION_MASK`-correct wraparound) → Task 2. ✓
- Spec H4 sentinel/carrier (`AnyValue` SBO+heap, `TypeID::Hash()` tag, manager thunks, `TryCast`) → Task 3. ✓
- Spec H4 migration (FieldInfo/TypeMeta/tests off `std::any`; drop `<any>`) → Task 4. ✓
- Spec H4 structural RTTI-free proof + 3-config green → Task 4 Step 6 + Task 5. ✓
- Spec "keep GetPtr<T> fast path" → nothing removed; typed accessors untouched. ✓
- Spec H2 (SIMD) correctly ABSENT (deferred to Mosaic). ✓
- Spec deferred non-owning view correctly ABSENT (value semantics only). ✓

**2. Placeholder scan:** No TBD/TODO; every code and test step shows complete code. The one judgement call (ReflectionTest ~940 follow-up) names the exact transformation (`std::any_cast<...>` → `*value.TryCast<...>()` with a null guard, or `HasValue()`). ✓

**3. Type consistency:** `AnyValue` surface used in Task 4 (`AnyValue(value)`, `TryCast<T>()`, `GetAny`→`AnyValue`, `SetAny(const AnyValue&)`) matches Task 3's definitions exactly. `Detail::NextEntityVersion<V>(current, mask, null, initial)` used in Task 2 Step 5 matches the Step 3 signature. `TypeID<T>::Hash()` (not `Value()`) is used throughout AnyValue, honoring the zero-ComponentID constraint. ✓

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-21-astra-theme-h-portability-and-rtti-free-reflection.md`.**
