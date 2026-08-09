# ComponentModule (RAII module-owned registration) — Astra Movement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `Astra::ComponentModule` — an RAII handle that owns a module's component descriptor AND TypeMeta registrations, with per-slot owner stacks — and delete `ReRegisterComponent`.

**Architecture:** The live descriptor array `m_components` stays untouched (pointer stability). Ownership is a parallel `m_owner[]` array plus a sparse shadow store that only holds displaced entries. Meta rebinding uses per-module build thunks retained by `StaticTypeRegistrar<T>`, executed outside all locks, installed in place at the stable `TypeMeta*` via a new `MetaRegistry::RebindInPlace`. Spec: `docs/superpowers/specs/2026-08-09-astra-component-module-raii-design.md` — read it before starting.

**Tech Stack:** Header-only C++20, MSVC (Astra.sln via premake5), GoogleTest.

## Global Constraints

- Branch: `feature/component-module` off `dev`. Finish = local FF-merge to dev, delete branch, do NOT push.
- Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` (also Release/Dist for the final gate). Build the whole solution — `-t:AstraTest` does not work.
- Test run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe` (GoogleTest). Filter with `--gtest_filter=ComponentModule*`.
- If `Astra.sln`/`ide/` are absent (fresh worktree) or a new test file is added, regenerate: `D:\dev\_shared\tools\premake5 vs2022` in the repo root.
- **TypeID ceiling:** the test binary is near the 128-ComponentID ceiling. Every new probe type must be namespace-scoped with a unique prefixed namespace (`Astra_Test_Mod*`), and this plan adds **at most 7 new component-id-consuming types total**. Do not add more; reuse the ones defined here across sections of the same file.
- `ASTRA_ENSURE_ALWAYS(cond, msg)` is a non-fatal log-and-continue **expression** (see `tests/Core/AssertTest.cpp:208`). Refusal paths must ALSO be observable (empty handle / nullptr / no-op) — never rely on ENSURE to stop execution.
- Lock order: `m_registrationMutex` → meta mutex, never reversed. **No user code (reflect builder bodies) under either lock.** Descriptor building (traits + fn-pointer addresses) is not user code and may run under lock.
- `CompressionTest.PerformanceBenchmark` is a known flake — a lone failure of only that test is not a regression; rerun it isolated.
- Commit messages: repo style — `feat(component): …`, `test(component): …`, `docs: …`.

---

### Task 1: Meta factory retention + `MetaRegistry::RebindInPlace`

**Files:**
- Modify: `include/Astra/Reflection/MetaRegistry.hpp` (StaticTypeRegistrar at :411-427; add `RebindInPlace` next to `Register(TypeMeta&&)` at :95)
- Test: `tests/Reflection/MetaRebindTest.cpp` (new)

**Interfaces:**
- Consumes: `Detail::TypeMetaBuilder<T>` (TypeMeta.hpp:454), `MetaRegistry::Register(TypeMeta&&)` / `Get(hash)` / `LinkToComponent` / `GetByComponentId` (MetaRegistry.hpp).
- Produces (later tasks rely on these exact names):
  - `template<typename T> struct Astra::Detail::MetaFactory { static inline std::function<TypeMeta()> fn; };`
  - `TypeMeta* MetaRegistry::RebindInPlace(TypeMeta&& fresh);` — install if absent; identity-collision refuse (nullptr) exactly like `Register(TypeMeta&&)`; else move-assign through the existing pointer (address stable) and return it.
  - `bool MetaRegistry::Erase(uint64_t hash);` — public, GUARDED: refuses (`ASTRA_ENSURE_ALWAYS` + false) when `m_typeToComponentId` contains the hash (a live component's meta must never be pulled out from under cached `ComponentDescriptor::meta` pointers). Removes the entry only.
  - `bool MetaRegistry::EraseUnchecked(uint64_t hash);` — internal clear-path form: removes the entry AND both link-map rows (`m_typeToComponentId` by hash; `m_componentIdToType` by the linked id, if any). Used by ComponentRegistry when a slot clears to empty (spec §3.6 meta-lifecycle adjudication).

- [ ] **Step 1: Write the failing test**

```cpp
// tests/Reflection/MetaRebindTest.cpp
#include <gtest/gtest.h>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Reflection/Macros.hpp>
#include <Astra/Core/TypeID.hpp>

namespace Astra_Test_ModMeta
{
    struct MetaProbe { int a = 0; float b = 0.0f; };
    ASTRA_REFLECT_TYPE(MetaProbe)
        ASTRA_REFLECT_FIELD(MetaProbe, a)
        ASTRA_REFLECT_FIELD(MetaProbe, b)
    ASTRA_REFLECT_TYPE_END()
}

TEST(MetaRebind, FactoryIsRetainedPerType)
{
    // The reflect block above must have stored a rebuildable factory.
    ASSERT_TRUE(static_cast<bool>(
        Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn));
    Astra::TypeMeta rebuilt = Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn();
    EXPECT_EQ(rebuilt.typeHash, Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash());
    EXPECT_EQ(rebuilt.fields.size(), 2u);
}

TEST(MetaRebind, RebindInPlaceKeepsAddressAndLink)
{
    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash();
    const Astra::TypeMeta* before = reg.Get(hash);
    ASSERT_NE(before, nullptr);   // drained by Instance() from the reflect block

    // 9999: far above any real ComponentID -- must not stomp a live suite's
    // GetByComponentId row in the shared default-context registry.
    reg.LinkToComponent(hash, static_cast<Astra::ComponentID>(9999));

    Astra::TypeMeta fresh = Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn();
    Astra::TypeMeta* after = reg.RebindInPlace(std::move(fresh));

    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after, before);                      // address stable: descriptors cache this pointer
    EXPECT_EQ(after->fields.size(), 2u);           // contents are the fresh build
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(9999)), before); // link survives (side maps keyed by hash)
}

TEST(MetaRebind, RebindInPlaceInstallsWhenAbsent)
{
    // NOTE: this test (and the 9999 link above) permanently mutates the
    // shared default-context registry -- harmless: synthetic hashes/ids no
    // real suite reads (plan-review finding 6).
    auto& reg = Astra::MetaRegistry::Instance();
    Astra::TypeMeta synthetic;
    synthetic.typeHash = 0xA110C8ED00000001ull;    // no reflect block uses this hash
    synthetic.typeName = "Astra_Test_ModMeta::SyntheticNeverReflected";
    Astra::TypeMeta* installed = reg.RebindInPlace(std::move(synthetic));
    ASSERT_NE(installed, nullptr);
    EXPECT_EQ(reg.Get(0xA110C8ED00000001ull), installed);
}

TEST(MetaRebind, EraseGuardsComponentLinkedHashes)
{
    auto& reg = Astra::MetaRegistry::Instance();
    // Synthetic non-component entry: public Erase succeeds.
    Astra::TypeMeta loose;
    loose.typeHash = 0xA110C8ED00000002ull;
    loose.typeName = "Astra_Test_ModMeta::SyntheticLoose";
    ASSERT_NE(reg.RebindInPlace(std::move(loose)), nullptr);
    EXPECT_TRUE(reg.Erase(0xA110C8ED00000002ull));
    EXPECT_EQ(reg.Get(0xA110C8ED00000002ull), nullptr);

    // Component-linked entry (MetaProbe was linked to 9999 above): public
    // Erase REFUSES; EraseUnchecked removes entry + link rows.
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash();
    EXPECT_FALSE(reg.Erase(hash));
    ASSERT_NE(reg.Get(hash), nullptr);
    EXPECT_TRUE(reg.EraseUnchecked(hash));
    EXPECT_EQ(reg.Get(hash), nullptr);
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(9999)), nullptr);
}
```

(Ordering note: `EraseGuardsComponentLinkedHashes` depends on `RebindInPlaceKeepsAddressAndLink` having linked 9999 — GoogleTest runs same-file tests in declaration order; keep the order as written.)

- [ ] **Step 2: Regenerate the solution and run the test to verify it fails**

Run: `D:\dev\_shared\tools\premake5 vs2022`, build Debug, then
`bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=MetaRebind*`
Expected: compile FAILURE — `MetaFactory` and `RebindInPlace` do not exist.

- [ ] **Step 3: Implement `MetaFactory` retention in `StaticTypeRegistrar`**

In `include/Astra/Reflection/MetaRegistry.hpp`, inside `namespace Detail`, ABOVE `StaticTypeRegistrar` (:411):

```cpp
        // Per-T-per-module meta factory, retained so a module handle can
        // REBUILD this module's TypeMeta later (hot-reload rebind). Inline
        // template static: each DLL/EXE gets its own copy, which is exactly
        // the ownership boundary the rebuild must respect.
        template<typename T>
        struct MetaFactory
        {
            static inline std::function<TypeMeta()> fn;
        };
```

In `StaticTypeRegistrar<T>`'s constructor (:414-426), before the existing `TypeMetaBuilder` lines, add:

```cpp
                MetaFactory<T>::fn = [f = builderFunc]() {
                    TypeMetaBuilder<T> b;
                    f(b);
                    return b.Build();
                };
```

(The macro-generated builder lambdas are captureless, hence copyable; keep the existing eager-build + enqueue below it unchanged.)

- [ ] **Step 4: Implement `MetaRegistry::RebindInPlace`**

Add directly below `Register(TypeMeta&&)` (:95-133), mirroring its identity checks:

```cpp
        // Hot-reload rebind: installs `fresh` if the hash is unknown; on a
        // hash hit with MATCHING identity (size/alignment/triviality/name)
        // move-assigns the contents through the existing pointer -- the
        // address every ComponentDescriptor::meta caches stays valid, only
        // the closures/fields swap. Identity mismatch refuses (nullptr),
        // exactly like Register(TypeMeta&&). The componentId link maps are
        // keyed by hash/id and are deliberately untouched.
        TypeMeta* RebindInPlace(TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(fresh.typeHash);
            if (it == m_types.end())
            {
                uint64_t hash = fresh.typeHash;
                auto metaPtr = std::make_unique<TypeMeta>(std::move(fresh));
                TypeMeta* ptr = metaPtr.get();
                m_types[hash] = std::move(metaPtr);
                return ptr;
            }
            TypeMeta& existing = *it->second;
            if (existing.size != fresh.size
                || existing.alignment != fresh.alignment
                || existing.isTrivial != fresh.isTrivial
                || existing.typeName != fresh.typeName)
            {
                ASTRA_LOG_ERROR("MetaRegistry: RebindInPlace identity mismatch -- refused");
                ASTRA_ENSURE_ALWAYS(false, "MetaRegistry rebind identity collision");
                return nullptr;
            }
            existing = std::move(fresh);   // move-assign: unique_ptr target address unchanged
            return &existing;
        }

        // Public erase: for module-owned NON-component metas (RegisterMeta
        // teardown). Refuses a component-linked hash -- erasing one would
        // dangle every cached ComponentDescriptor::meta for a live component.
        bool Erase(uint64_t hash)
        {
            std::unique_lock lock(m_mutex);
            if (m_typeToComponentId.Find(hash) != m_typeToComponentId.end())
            {
                ASTRA_ENSURE_ALWAYS(false,
                    "MetaRegistry::Erase refused: hash is component-linked (use the "
                    "component clear path, not a manual erase)");
                return false;
            }
            return m_types.Erase(hash) != 0;
        }

        // Internal clear-path erase (spec 2026-08-09 meta-lifecycle rule):
        // called when a component slot clears TO EMPTY, so the descriptor is
        // already gone and nothing legitimately holds this meta. Removes the
        // entry AND both link rows.
        bool EraseUnchecked(uint64_t hash)
        {
            std::unique_lock lock(m_mutex);
            if (auto it = m_typeToComponentId.Find(hash); it != m_typeToComponentId.end())
            {
                m_componentIdToType.Erase(it->second);
                m_typeToComponentId.Erase(it);
            }
            return m_types.Erase(hash) != 0;
        }
```

(Adjust the two `Erase(it)`-style calls to FlatMap's actual iterator/key erase overloads — FlatMap.hpp:620/:629.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: build Debug, `AstraTest.exe --gtest_filter=MetaRebind*`
Expected: 3 PASS. Also run the full suite once (`AstraTest.exe`) — no regressions.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Reflection/MetaRegistry.hpp tests/Reflection/MetaRebindTest.cpp
git commit -m "feat(reflection): retained meta factories + MetaRegistry::RebindInPlace"
```

---

### Task 2: `ComponentModule::Open` + tripwire + owned `Register<T>`

**Files:**
- Create: `include/Astra/Component/ComponentModule.hpp`
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (owner bookkeeping; descriptor-build refactor)
- Test: `tests/Component/ComponentModuleTest.cpp` (new)

**Interfaces:**
- Consumes: `TypeContext::GetOrAssignComponentID(uint64_t hash, std::string_view name, TypeIdentity)` (TypeContext.hpp:102), `Detail::CurrentTypeContextSlot()` (TypeContext.hpp:180), `MakeTypeIdentity<T>()` (TypeID.hpp:279), `Detail::MetaFactory<T>` (Task 1).
- Produces (Tasks 3–7 rely on these exact names):
  - `class Astra::ComponentModule` with: `static ComponentModule Open(std::shared_ptr<ComponentRegistry>, std::string_view name)`, `template<Component... Ts> void Register()`, `void Reset()`, `explicit operator bool() const noexcept`, move ctor/assign, dtor.
  - `using Astra::MetaBuildFn = TypeMeta (*)();`
  - On ComponentRegistry (public, documented as ComponentModule plumbing):
    - `uint32_t OpenModuleId(std::string_view name);` (issues ids starting at 1; 0 = anonymous)
    - `bool InstallOwned(ComponentID id, uint32_t owner, ComponentDescriptor desc, MetaBuildFn buildMeta);` — by-value: InstallOwned sets `desc.name` via `StoreComponentName` before writing the slot (Task 2 scope: same-owner replace + install-into-empty only; Task 3 adds shadow push)
    - `ASTRA_NODISCARD uint32_t GetOwner(ComponentID id) const;`
    - `template<Component T> static ComponentDescriptor MakeDescriptor(ComponentID id, const TypeMeta* meta);` — the pure build extracted from `RegisterComponentImpl`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/Component/ComponentModuleTest.cpp
#include <gtest/gtest.h>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>

namespace Astra_Test_Mod
{
    struct OwnedA { int v = 0; };
    struct OwnedB { float v = 0.0f; };
}

namespace
{
    // Open() requires an EXPLICITLY installed TypeContext slot (spec §3.1).
    // Installing the module-default context satisfies the tripwire while
    // keeping every id identical to what TypeID<T>::Value() mints for the
    // rest of this test binary -- no cross-suite id divergence.
    struct InstalledContext
    {
        InstalledContext()  { Astra::SetTypeContext(&Astra::DefaultTypeContext()); }
        ~InstalledContext() { Astra::SetTypeContext(nullptr); }
    };
}

TEST(ComponentModule, OpenRefusesWithoutInstalledContext)
{
    ASSERT_EQ(Astra::Detail::CurrentTypeContextSlot(), nullptr);  // precondition
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "NoContext");
    EXPECT_FALSE(static_cast<bool>(mod));            // observable refusal (ENSURE logs, continues)
    mod.Register<Astra_Test_Mod::OwnedA>();          // must be a safe no-op
    EXPECT_EQ(creg->Size(), 0u);
}

TEST(ComponentModule, OpenAndRegisterOwnsDescriptor)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "TestModule");
    ASSERT_TRUE(static_cast<bool>(mod));

    mod.Register<Astra_Test_Mod::OwnedA, Astra_Test_Mod::OwnedB>();

    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();
    const auto* desc = creg->GetComponentDescriptor(idA);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->id, idA);
    EXPECT_NE(desc->defaultConstruct, nullptr);
    EXPECT_NE(creg->GetOwner(idA), 0u);              // owned, not anonymous
    EXPECT_EQ(creg->Size(), 2u);
}

TEST(ComponentModule, AnonymousRegisterComponentIsOwnerZero)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Astra_Test_Mod::OwnedA>();
    EXPECT_EQ(creg->GetOwner(Astra::TypeID<Astra_Test_Mod::OwnedA>::Value()), 0u);
}

TEST(ComponentModule, OpenRefusesNullRegistry)
{
    InstalledContext ctx;
    Astra::ComponentModule mod = Astra::ComponentModule::Open(nullptr, "Null");
    EXPECT_FALSE(static_cast<bool>(mod));
}
```

- [ ] **Step 2: Regenerate solution, build, verify FAIL**

Run: premake + build Debug.
Expected: compile FAILURE — `ComponentModule.hpp` does not exist.

- [ ] **Step 3: Refactor `RegisterComponentImpl` into `MakeDescriptor` + install**

In `ComponentRegistry.hpp`: extract everything in `RegisterComponentImpl<T>(id)` (:209-308) that BUILDS the descriptor (size/alignment/triviality/hash/version/fn-pointers/enableable static_assert) into:

```cpp
        template<Component T>
        ASTRA_NODISCARD static ComponentDescriptor MakeDescriptor(ComponentID id, const TypeMeta* meta)
        {
            ComponentDescriptor desc;
            // ... body moved VERBATIM from RegisterComponentImpl, with two changes:
            //   (1) `desc.meta = meta;` instead of MetaRegistry::Instance().Get<T>()
            //   (2) no m_componentNames / m_components / m_present / m_hashToID access here
            //       (desc.name is set by the caller from the name-storage helper below)
            return desc;
        }

        // Name storage split out so both registration paths share it (called under lock).
        const char* StoreComponentName(std::string_view name);
        // Reuse-by-hash comes in Task 7; for now StoreComponentName keeps today's
        // emplace_back behavior verbatim.
```

`RegisterComponentImpl<T>(id)` becomes: `MakeDescriptor<T>(id, MetaRegistry::Instance().Get<T>())` + the existing install lines (`m_componentNames`/`desc.name`, `LinkToComponent` when meta, `m_components[id] = desc; m_present.Set(id); m_hashToID[...]`). Behavior identical; run the full existing suite after this step before proceeding.

- [ ] **Step 4: Add owner bookkeeping to `ComponentRegistry`**

Members (next to `m_registered`, :414):

```cpp
        uint32_t m_owner[MAX_COMPONENTS] = {};       // owner of the LIVE entry; 0 = anonymous
        uint32_t m_nextModuleId = 1;                 // module ids; issued under m_registrationMutex
        std::deque<std::string> m_moduleNames;       // index = moduleId - 1; diagnostics
```

Public methods:

```cpp
        uint32_t OpenModuleId(std::string_view name)
        {
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            m_moduleNames.emplace_back(name);
            return m_nextModuleId++;
        }

        ASTRA_NODISCARD uint32_t GetOwner(ComponentID id) const
        {
            return (id < MAX_COMPONENTS) ? m_owner[id] : 0u;
        }

        // ComponentModule plumbing (Task 2 scope: empty slot or same-owner
        // replace; Task 3 adds the shadow push for different-owner slots).
        // By-value desc: InstallOwned sets desc.name from StoreComponentName
        // under the lock before writing the slot.
        // Returns false when the id is invalid/refused. buildMeta may be null.
        bool InstallOwned(ComponentID id, uint32_t owner,
                          ComponentDescriptor desc, MetaBuildFn buildMeta);
```

`InstallOwned` (under `m_registrationMutex`): guard `id >= MAX_COMPONENTS` → false; set `desc.name` via `StoreComponentName`, write `m_components[id]`, `m_present.Set(id)`, `m_hashToID[desc.hash] = id`, `m_owner[id] = owner`, `m_registered[id].store(true, release)`, remember `buildMeta` for the slot (add `MetaBuildFn m_metaThunk[MAX_COMPONENTS] = {};` member), return true. `RegisterComponent<T>`'s cold path also records `m_owner[id] = 0` and its thunk (`Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr` — add `template<typename T> TypeMeta Detail::BuildMetaThunk() { return MetaFactory<T>::fn(); }` in MetaRegistry.hpp's Detail namespace, and `using MetaBuildFn = TypeMeta (*)();` in Component.hpp near the TypeMeta fwd-decl at :34).

- [ ] **Step 5: Write `ComponentModule.hpp`**

```cpp
#pragma once

#include <memory>
#include <string_view>

#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "ComponentRegistry.hpp"

namespace Astra
{
    // RAII owner of a module's component registrations (spec 2026-08-09).
    // Open() captures the EXPLICITLY INSTALLED TypeContext -- all ids and
    // metas route through the captured context, never ambient per-module
    // state. Register<Ts...>() must be instantiated in the module whose
    // code the descriptors should point into (that is the whole point).
    class ComponentModule
    {
    public:
        ComponentModule() = default;

        ASTRA_NODISCARD static ComponentModule Open(std::shared_ptr<ComponentRegistry> registry,
                                                    std::string_view name)
        {
            ComponentModule mod;
            TypeContext* installed = Detail::CurrentTypeContextSlot();
            if (!registry || !ASTRA_ENSURE_ALWAYS(installed != nullptr,
                    "ComponentModule::Open requires SetTypeContext before use "
                    "(the module-local default context would silently mint private ids)"))
            {
                return mod;   // empty handle: observable refusal
            }
            mod.m_registry = std::move(registry);
            mod.m_context  = installed;
            mod.m_moduleId = mod.m_registry->OpenModuleId(name);
            return mod;
        }

        template<Component... Ts>
        void Register()
        {
            if (!*this) return;
            (RegisterOne<Ts>(), ...);
        }

        void Reset();                      // Task 3 fills the removal sweep; Task 2: just release
        ~ComponentModule() { Reset(); }

        ComponentModule(ComponentModule&& other) noexcept { *this = std::move(other); }
        ComponentModule& operator=(ComponentModule&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_registry = std::move(other.m_registry);
                m_context  = other.m_context;
                m_moduleId = other.m_moduleId;
                other.m_context  = nullptr;
                other.m_moduleId = 0;
            }
            return *this;
        }
        ComponentModule(const ComponentModule&) = delete;
        ComponentModule& operator=(const ComponentModule&) = delete;

        ASTRA_NODISCARD explicit operator bool() const noexcept
        {
            return m_registry != nullptr && m_moduleId != 0;
        }

    private:
        template<Component T>
        void RegisterOne()
        {
            // Explicit-context id mint: TypeID<T>::Hash()/Name() are pure
            // compile-time values; only the id assignment touches context state.
            const ComponentID id = m_context->GetOrAssignComponentID(
                TypeID<T>::Hash(), TypeID<T>::Name(), MakeTypeIdentity<T>());
            if (id == INVALID_COMPONENT || id >= MAX_COMPONENTS)
                return;                                    // refused type: never owned

            MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;

            // Phase 1 (locked, inside registry): slot + owner bookkeeping.
            const TypeMeta* meta = m_context->Meta().Get(TypeID<T>::Hash());
            ComponentDescriptor desc = ComponentRegistry::MakeDescriptor<T>(id, meta);
            if (!m_registry->InstallOwned(id, m_moduleId, desc, thunk))
                return;

            // Phase 2 (NO locks held here): rebuild + install this module's
            // meta so field closures point into THIS image (spec §3.3).
            if (thunk)
            {
                TypeMeta fresh = thunk();
                m_context->Meta().RebindInPlace(std::move(fresh));
                m_context->Meta().LinkToComponent(TypeID<T>::Hash(), id);
            }
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        TypeContext* m_context = nullptr;
        uint32_t m_moduleId = 0;
    };
}
```

Task-2 `Reset()`: `m_registry.reset(); m_context = nullptr; m_moduleId = 0;` (the removal sweep is Task 3). Note: `MetaRegistry::Get(hash)` and `Meta()` calls require including `../Reflection/MetaRegistry.hpp` — ComponentRegistry.hpp already includes it.

- [ ] **Step 6: Build, run, verify PASS**

Run: build Debug, `AstraTest.exe --gtest_filter=ComponentModule*` → 4 PASS, then full suite → no regressions (the `MakeDescriptor` refactor must be behavior-identical).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Component/ComponentModule.hpp include/Astra/Component/ComponentRegistry.hpp include/Astra/Component/Component.hpp include/Astra/Reflection/MetaRegistry.hpp tests/Component/ComponentModuleTest.cpp
git commit -m "feat(component): ComponentModule::Open with context tripwire + owned registration"
```

---

### Task 3: Shadow push/restore + handle destruction sweep

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (shadow store; `InstallOwned` push path; new `ReleaseModule`)
- Modify: `include/Astra/Component/ComponentModule.hpp` (`Reset()` full semantics)
- Test: `tests/Component/ComponentModuleTest.cpp` (extend)

**Interfaces:**
- Consumes: Task 2 surface.
- Produces:
  - `struct ComponentRegistry::ShadowEntry { uint32_t owner; ComponentDescriptor desc; MetaBuildFn buildMeta; };`
  - `SmallVector<ComponentRegistry::MetaRestore, 4> ComponentRegistry::ReleaseModule(uint32_t owner);` where `struct MetaRestore { MetaBuildFn buildMeta; uint64_t hash; ComponentID id; };` — performs the locked sweep and RETURNS the meta work for the caller to run outside all locks. **`buildMeta != nullptr` ⇒ rebuild + `RebindInPlace` + `LinkToComponent`; `buildMeta == nullptr` ⇒ the slot cleared TO EMPTY: `EraseUnchecked(hash)`** (spec §3.6 meta-lifecycle rule — no stale meta stays reachable after its module's type vanishes).
  - Push semantics inside `InstallOwned`: different-live-owner slot → current live entry (owner, desc, thunk) moves into `m_shadow[id]`, new entry installed, `ASTRA_LOG_INFO` naming both modules.

- [ ] **Step 1: Write the failing tests** (append to ComponentModuleTest.cpp; REUSE `Astra_Test_Mod::OwnedA/OwnedB` — no new types)

```cpp
TEST(ComponentModule, DestructionClearsOwnedSlotToCleanMiss)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();
    {
        auto mod = Astra::ComponentModule::Open(creg, "Ephemeral");
        mod.Register<Astra_Test_Mod::OwnedA>();
        ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);
    }
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);   // clean miss, id stays reserved
    EXPECT_EQ(creg->GetOwner(idA), 0u);
}

TEST(ComponentModule, ReloadLoadBeforeUnload)
{
    // Gen N+1 registers while gen N is still alive (real PluginHost order):
    // push shadows gen N; destroying gen N later must drop its SHADOWED
    // entry and leave gen N+1's live entry untouched.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();

    auto genN = Astra::ComponentModule::Open(creg, "GenN");
    genN.Register<Astra_Test_Mod::OwnedA>();
    const uint32_t ownerN = creg->GetOwner(idA);

    auto genN1 = Astra::ComponentModule::Open(creg, "GenN1");
    genN1.Register<Astra_Test_Mod::OwnedA>();
    const uint32_t ownerN1 = creg->GetOwner(idA);
    EXPECT_NE(ownerN, ownerN1);

    genN.Reset();                                            // old image goes away
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);   // no unregistered window
    EXPECT_EQ(creg->GetOwner(idA), ownerN1);
}

TEST(ComponentModule, ReloadUnloadBeforeLoad)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();

    auto genN = Astra::ComponentModule::Open(creg, "GenN");
    genN.Register<Astra_Test_Mod::OwnedA>();
    genN.Reset();
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);   // clean miss window

    auto genN1 = Astra::ComponentModule::Open(creg, "GenN1");
    genN1.Register<Astra_Test_Mod::OwnedA>();
    EXPECT_NE(creg->GetComponentDescriptor(idA), nullptr);
}

TEST(ComponentModule, OverrideRestoresPreviousOwnerOnUnload)
{
    // Engine registers anonymously; a module overrides; module death must
    // RESTORE the anonymous base entry -- the "engine roster survives" case.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idB = Astra::TypeID<Astra_Test_Mod::OwnedB>::Value();

    creg->RegisterComponent<Astra_Test_Mod::OwnedB>();       // anonymous base (owner 0)
    const auto* base = creg->GetComponentDescriptor(idB);
    ASSERT_NE(base, nullptr);

    {
        auto mod = Astra::ComponentModule::Open(creg, "Override");
        mod.Register<Astra_Test_Mod::OwnedB>();
        EXPECT_NE(creg->GetOwner(idB), 0u);
    }
    const auto* restored = creg->GetComponentDescriptor(idB);
    ASSERT_NE(restored, nullptr);                            // base came back
    EXPECT_EQ(restored, base);                               // SAME address: m_components[id] is stable
    EXPECT_EQ(creg->GetOwner(idB), 0u);
    EXPECT_NE(restored->defaultConstruct, nullptr);
}

TEST(ComponentModule, SameModuleReRegisterDoesNotGrowShadow)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "Twice");
    mod.Register<Astra_Test_Mod::OwnedA>();
    mod.Register<Astra_Test_Mod::OwnedA>();                  // in-place rebind of own entry
    mod.Reset();
    EXPECT_EQ(creg->GetComponentDescriptor(Astra::TypeID<Astra_Test_Mod::OwnedA>::Value()), nullptr);
    // (no shadow left behind: OwnedA pops to empty, not to a stale copy of itself)
}

TEST(ComponentModule, MidStackRemovalAtDepthTwo)
{
    // Plan-review finding 2: the nontrivial ReleaseModule branch. Base
    // (anonymous) -> modA overrides -> modB overrides; destroying modA
    // (MID-stack) must not disturb modB's live entry; destroying modB then
    // restores the BASE (modA's entry is gone from the middle).
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();

    creg->RegisterComponent<Astra_Test_Mod::OwnedA>();       // base, owner 0
    auto modA = Astra::ComponentModule::Open(creg, "DepthA");
    modA.Register<Astra_Test_Mod::OwnedA>();
    auto modB = Astra::ComponentModule::Open(creg, "DepthB");
    modB.Register<Astra_Test_Mod::OwnedA>();
    const uint32_t ownerB = creg->GetOwner(idA);

    modA.Reset();                                            // mid-stack removal
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);
    EXPECT_EQ(creg->GetOwner(idA), ownerB);                  // live entry untouched

    modB.Reset();                                            // pops PAST the removed middle
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);   // base restored
    EXPECT_EQ(creg->GetOwner(idA), 0u);
    EXPECT_NE(creg->GetComponentDescriptor(idA)->defaultConstruct, nullptr);
}

TEST(ComponentModule, MoveAndDoubleResetAreIdempotent)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto a = Astra::ComponentModule::Open(creg, "Mover");
    a.Register<Astra_Test_Mod::OwnedA>();
    Astra::ComponentModule b = std::move(a);
    EXPECT_FALSE(static_cast<bool>(a));
    a.Reset();                                               // moved-from reset: no-op
    EXPECT_TRUE(static_cast<bool>(b));
    b.Reset();
    b.Reset();                                               // double reset: no-op
    EXPECT_EQ(creg->GetComponentDescriptor(Astra::TypeID<Astra_Test_Mod::OwnedA>::Value()), nullptr);
}
```

- [ ] **Step 2: Build, run, verify FAIL** — `ReloadLoadBeforeUnload` and `OverrideRestoresPreviousOwnerOnUnload` fail (no shadow store; Task-2 `Reset` doesn't sweep).

- [ ] **Step 3: Implement the shadow store + push + `ReleaseModule`**

In `ComponentRegistry.hpp`:

```cpp
        struct ShadowEntry { uint32_t owner; ComponentDescriptor desc; MetaBuildFn buildMeta; };
        struct MetaRestore { MetaBuildFn buildMeta; uint64_t hash; ComponentID id; };
    private:
        FlatMap<ComponentID, SmallVector<ShadowEntry, 1>> m_shadow;  // sparse: only overridden slots
```

`InstallOwned` full semantics (replacing Task 2's simplified body; still under `m_registrationMutex`):

1. `id >= MAX_COMPONENTS` → false.
2. Slot empty (`!m_present.Test(id)`) → plain install (Task 2 path).
3. Live owner == this owner → in-place replace of live entry + thunk (no shadow).
4. Live owner != this owner → append `{m_owner[id], m_components[id], m_metaThunk[id]}` to `m_shadow[id]`, `ASTRA_LOG_INFO` "module '<new>' overrides '<prev>' for component '<name>'" (names from `m_moduleNames`, "(anonymous)" for 0), then install the new entry.

`ReleaseModule(uint32_t owner)` (under `m_registrationMutex`; returns the out-of-lock meta work):

```cpp
        SmallVector<MetaRestore, 4> ReleaseModule(uint32_t owner)
        {
            SmallVector<MetaRestore, 4> metaWork;
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
            {
                // Drop this owner's SHADOWED entries wherever they sit.
                if (auto it = m_shadow.Find(static_cast<ComponentID>(id)); it != m_shadow.end())
                {
                    auto& list = it->second;
                    for (size_t i = list.size(); i-- > 0;)
                        if (list[i].owner == owner)
                            list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                    if (list.empty()) m_shadow.Erase(it);
                }
                if (!m_present.Test(id) || m_owner[id] != owner)
                    continue;
                // This owner's entry is LIVE: restore newest shadow, or clear.
                if (auto it = m_shadow.Find(static_cast<ComponentID>(id));
                    it != m_shadow.end() && !it->second.empty())
                {
                    ShadowEntry restored = std::move(it->second.back());
                    it->second.pop_back();
                    if (it->second.empty()) m_shadow.Erase(it);
                    m_components[id] = restored.desc;        // same address, new contents
                    m_owner[id]      = restored.owner;
                    m_metaThunk[id]  = restored.buildMeta;
                    if (restored.buildMeta)
                        metaWork.push_back({restored.buildMeta, restored.desc.hash,
                                            static_cast<ComponentID>(id)});
                }
                else
                {
                    const uint64_t clearedHash = m_components[id].hash;
                    m_components[id] = ComponentDescriptor{};
                    m_present.Reset(id);
                    m_owner[id] = 0;
                    m_metaThunk[id] = nullptr;
                    m_registered[id].store(false, std::memory_order_release);
                    // buildMeta == nullptr ⇒ caller runs EraseUnchecked(hash)
                    // outside this lock (spec §3.6: cleared-to-empty types
                    // take their meta with them).
                    metaWork.push_back({nullptr, clearedHash, static_cast<ComponentID>(id)});
                }
            }
            return metaWork;
        }
```

(Note: when restoring, `m_registered[id]` stays true and `m_present` stays set — the slot is still occupied. Adjust `SmallVector` erase syntax to the project's `SmallVector` API — check its header for the erase/pop_back names before writing.)

`ComponentModule::Reset()` full version:

```cpp
        void Reset()
        {
            if (*this)
            {
                auto metaWork = m_registry->ReleaseModule(m_moduleId);
                for (auto& w : metaWork)          // outside all locks: user reflect code
                {
                    if (w.buildMeta)              // survivor restored: rebind its meta
                    {
                        TypeMeta fresh = w.buildMeta();
                        m_context->Meta().RebindInPlace(std::move(fresh));
                        m_context->Meta().LinkToComponent(w.hash, w.id);
                    }
                    else                          // cleared to empty: meta goes too
                    {
                        m_context->Meta().EraseUnchecked(w.hash);
                    }
                }
                EraseOwnedMetas();                // Task 5 adds this; Task 3: omit the call
            }
            m_registry.reset();
            m_context = nullptr;
            m_moduleId = 0;
        }
```

- [ ] **Step 4: Build, run, verify PASS** — `--gtest_filter=ComponentModule*` all green, then full suite.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Component/ComponentModule.hpp include/Astra/Component/ComponentRegistry.hpp tests/Component/ComponentModuleTest.cpp
git commit -m "feat(component): shadow owner stack -- push on override, restore on module release"
```

---

### Task 4: Meta transitions across slot push/pop (end-to-end)

**Files:**
- Test: `tests/Component/ComponentModuleTest.cpp` (extend)
- Modify: only if the test exposes gaps in Tasks 1–3 wiring (expected: none — this task PROVES the wiring)

**Interfaces:**
- Consumes: Tasks 1–3. Uses `Astra_Test_ModMeta::MetaProbe`? NO — that type lives in MetaRebindTest.cpp. Define ONE new reflected component probe here (type budget: 3rd of 7).

- [ ] **Step 1: Write the test**

```cpp
namespace Astra_Test_ModMeta2
{
    struct ReflectedOwned { int x = 0; };
    ASTRA_REFLECT_TYPE(ReflectedOwned)
        ASTRA_REFLECT_FIELD(ReflectedOwned, x)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, MetaRebindsAcrossPushAndPop)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta2::ReflectedOwned>::Hash();

    creg->RegisterComponent<Astra_Test_ModMeta2::ReflectedOwned>();   // anonymous base
    const Astra::TypeMeta* meta = Astra::MetaRegistry::Instance().Get(hash);
    ASSERT_NE(meta, nullptr);
    const auto idR = Astra::TypeID<Astra_Test_ModMeta2::ReflectedOwned>::Value();
    ASSERT_EQ(creg->GetComponentDescriptor(idR)->meta, meta);

    {
        auto mod = Astra::ComponentModule::Open(creg, "MetaOwner");
        mod.Register<Astra_Test_ModMeta2::ReflectedOwned>();          // push: meta rebound (same address)
        EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), meta);   // address stable through push
        EXPECT_EQ(meta->fields.size(), 1u);
    }
    // Pop back to the anonymous base: its thunk re-ran, address still stable,
    // contents rebuilt, component link intact.
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), meta);
    EXPECT_EQ(meta->fields.size(), 1u);
    EXPECT_EQ(Astra::MetaRegistry::Instance().GetByComponentId(idR), meta);
    EXPECT_NE(creg->GetComponentDescriptor(idR), nullptr);            // base descriptor restored
}

namespace Astra_Test_ModMeta2
{
    struct ReflectedEphemeral { int y = 0; };   // type budget: 4th id-consuming type
    ASTRA_REFLECT_TYPE(ReflectedEphemeral)
        ASTRA_REFLECT_FIELD(ReflectedEphemeral, y)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, MetaErasedWhenSlotPopsToEmpty)
{
    // Spec §3.6 meta-lifecycle rule (plan-review finding 1): a module-owned
    // reflected component with NO shadow survivor takes its meta with it --
    // unload-before-load then reinstalls FRESH, never comparing against a
    // stale entry whose typeName views unmapped storage.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta2::ReflectedEphemeral>::Hash();

    {
        auto genN = Astra::ComponentModule::Open(creg, "EphemeralGenN");
        genN.Register<Astra_Test_ModMeta2::ReflectedEphemeral>();
        ASSERT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    }
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), nullptr);    // meta erased with the slot

    auto genN1 = Astra::ComponentModule::Open(creg, "EphemeralGenN1");
    genN1.Register<Astra_Test_ModMeta2::ReflectedEphemeral>();        // absent path: fresh install
    EXPECT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash)->fields.size(), 1u);
}
```

- [ ] **Step 2: Build, run.** Expected: PASS if Tasks 1–3 wired the thunks correctly; if it fails, the gap is in `RegisterComponent`'s cold path not capturing the thunk (Task 2 Step 4) or `Reset()` not running `metaWork` (Task 3 Step 3) — fix there, not here.

- [ ] **Step 3: Run full suite; commit**

```bash
git add tests/Component/ComponentModuleTest.cpp
git commit -m "test(component): meta rebind end-to-end across owner push/pop"
```

---

### Task 5: `RegisterMeta<Ts...>` — non-component reflected types

**Files:**
- Modify: `include/Astra/Component/ComponentModule.hpp` (add `RegisterMeta`, `EraseOwnedMetas`)
- Test: `tests/Component/ComponentModuleTest.cpp` (extend)

**Interfaces:**
- Consumes: Task 1's guarded `MetaRegistry::Erase(hash)` (refuses component-linked hashes — `RegisterMeta` types are non-components, so the legit path always passes the guard).
- Produces:
  - `template<typename... Ts> void ComponentModule::RegisterMeta();` — for each T: requires `Detail::MetaFactory<T>::fn` (reflected in this module; `ASTRA_ENSURE` + skip otherwise); builds outside locks; `RebindInPlace`; records `TypeID<T>::Hash()` in the handle's owned-meta list (`SmallVector<uint64_t, 4> m_ownedMetas`).
  - `ComponentModule::Reset()` calls `EraseOwnedMetas()` (uncomment the Task-3 placeholder): for each recorded hash, `m_context->Meta().Erase(hash)` — the PUBLIC guarded form.

- [ ] **Step 1: Write the failing tests** (types budget: 4th of 7 — a reflected NON-component struct; it consumes no ComponentID since it is never registered as a component)

```cpp
namespace Astra_Test_ModMeta3
{
    struct PluginPrivate { double d = 0.0; };   // reflected, never a component
    ASTRA_REFLECT_TYPE(PluginPrivate)
        ASTRA_REFLECT_FIELD(PluginPrivate, d)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, RegisterMetaAdoptsAndErasesOnDestruction)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta3::PluginPrivate>::Hash();

    // The static-init drain installed it anonymously the first time any
    // MetaRegistry::Instance() ran in this binary.
    ASSERT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);

    {
        auto mod = Astra::ComponentModule::Open(creg, "PrivateMetaOwner");
        mod.RegisterMeta<Astra_Test_ModMeta3::PluginPrivate>();
        EXPECT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    }
    // Owned meta erased with the handle: GetMeta/GetByName can no longer
    // reach closures that would have died with the module.
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), nullptr);
}
```

- [ ] **Step 2: Build, verify FAIL** (`RegisterMeta`/`Erase` missing).

- [ ] **Step 3: Implement** — `ComponentModule::RegisterMeta` mirrors `RegisterOne`'s phase-2 only (no descriptor phase), then `m_ownedMetas.push_back(TypeID<T>::Hash())`. `EraseOwnedMetas()` iterates the list calling `m_context->Meta().Erase(hash)` (Task 1's guarded public form). Wire the call into `Reset()` before the registry release.

- [ ] **Step 4: Build, run `ComponentModule*` + `MetaRebind*` + full suite. Commit**

```bash
git add include/Astra/Component/ComponentModule.hpp include/Astra/Reflection/MetaRegistry.hpp tests/Component/ComponentModuleTest.cpp
git commit -m "feat(component): RegisterMeta -- module-owned non-component reflection lifecycle"
```

---

### Task 6: Delete `ReRegisterComponent`; migrate Astra tests + README

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (delete :59-68; rewrite the :83-111 comment block, which references it)
- Modify: `tests/Component/ComponentRegistryTest.cpp` (:632-677 — rewrite the three `ComponentRegistryReRegister` tests)
- Modify: `README.md` (:403-404 hot-reload flow)

**Interfaces:** consumes Tasks 2–3 (`ComponentModule::Open`/`Register`).

- [ ] **Step 1: Delete the API and fix the comment**

Remove `ReRegisterComponent` (ComponentRegistry.hpp:59-68). In the `UnregisterModuleRange` doc comment (:83-111), replace the sentence referencing it ("a host's plugin deliberately re-points even engine-owned types at itself, which is exactly what ReRegisterComponent above is for.") with: "a module may re-point a type at itself via ComponentModule::Register — the RAII path cleans that up itself; this range purge is the fallback net for modules that never adopted it."

- [ ] **Step 2: Rewrite the three tests** (keep the existing `Astra_Test_ReReg` probe types — zero new ids):

```cpp
namespace
{
    // Same RAII guard as ComponentModuleTest.cpp (plan-review finding 3):
    // a failed ASSERT must not leak an installed slot into later tests --
    // OpenRefusesWithoutInstalledContext asserts a null slot as its
    // precondition. Duplicated 6 lines; anonymous namespace, no ODR issue.
    struct InstalledContext
    {
        InstalledContext()  { Astra::SetTypeContext(&Astra::DefaultTypeContext()); }
        ~InstalledContext() { Astra::SetTypeContext(nullptr); }
    };
}

TEST(ComponentRegistryReRegister, ModuleOverrideRebuildsDescriptor)
{
    InstalledContext ctx;
    auto registry = std::make_shared<Astra::ComponentRegistry>();
    registry->RegisterComponent<Astra_Test_ReReg::ReRegProbe>();
    const auto* before = registry->GetComponentDescriptor(Astra::TypeID<Astra_Test_ReReg::ReRegProbe>::Value());
    ASSERT_NE(before, nullptr);
    const auto id = before->id; const auto hash = before->hash;

    auto mod = Astra::ComponentModule::Open(registry, "ReRegTest");
    mod.Register<Astra_Test_ReReg::ReRegProbe>();     // the hot-reload rebind, RAII form
    const auto* after = registry->GetComponentDescriptor(id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->id, id);
    EXPECT_EQ(after->hash, hash);
    EXPECT_NE(after->defaultConstruct, nullptr);
}

TEST(ComponentRegistryReRegister, RegisterRemainsIdempotent)
{
    Astra::ComponentRegistry registry;                 // unchanged from today
    registry.RegisterComponent<Astra_Test_ReReg::IdemProbe>();
    const size_t count = registry.Size();
    registry.RegisterComponent<Astra_Test_ReReg::IdemProbe>();
    EXPECT_EQ(registry.Size(), count);
}

TEST(ComponentRegistryReRegister, ModuleRegisterOnFreshTypeActsAsRegister)
{
    InstalledContext ctx;
    auto registry = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(registry, "FreshTest");
    mod.Register<Astra_Test_ReReg::FreshProbe>();
    const auto* desc = registry->GetComponentDescriptor(Astra::TypeID<Astra_Test_ReReg::FreshProbe>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(registry->Size(), 1u);
}
```

Add `#include <Astra/Component/ComponentModule.hpp>` and `#include <Astra/Core/TypeContext.hpp>` to the test file.

- [ ] **Step 3: Update README.md:403-404** — replace the `SetTypeContext -> componentRegistry->ReRegisterComponent<T>() for each type -> deserialize` flow with the ComponentModule form: `SetTypeContext` → `auto mod = ComponentModule::Open(creg, "MyPlugin"); mod.Register<MyTypes...>();` in the new image's Init → deserialize; note the old image's handle destruction (in its Shutdown) restores or clears its entries automatically, and that the handle must be heap-held and reset in Shutdown (never a DLL static — the destructor would run under the loader lock).

- [ ] **Step 4: Grep gate, build, full suite, commit**

Run: `grep -rn "ReRegisterComponent" include/ tests/` → expected: **zero hits**. (docs/ history may keep mentions.) Build Debug + full suite green.

```bash
git add include/Astra/Component/ComponentRegistry.hpp tests/Component/ComponentRegistryTest.cpp README.md
git commit -m "feat(component)!: delete ReRegisterComponent -- ComponentModule is the rebind path"
```

---

### Task 7: `UnregisterModuleRange` unification + name-storage reuse

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (`UnregisterModuleRange` :112-157; `StoreComponentName`)
- Test: `tests/Component/ComponentModuleTest.cpp` (extend)

**Interfaces:** consumes Task 3's shadow store.

- [ ] **Step 1: Write the failing tests** (reuse `Astra_Test_Mod::OwnedA`; no new types)

```cpp
TEST(ComponentModule, RangePurgeStripsShadowsAndRestoresSurvivor)
{
    // A non-RAII "module" (simulated by an anonymous base) is shadowed by an
    // owned override; purging the OVERRIDE's address range must restore the
    // base -- and purging a range covering a SHADOWED entry must strip it
    // without touching the live one.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra_Test_Mod::OwnedA>::Value();

    creg->RegisterComponent<Astra_Test_Mod::OwnedA>();            // base
    auto mod = Astra::ComponentModule::Open(creg, "PurgeVictim");
    mod.Register<Astra_Test_Mod::OwnedA>();                       // override (live)

    // Purge the whole image (every fn pointer in this test binary lies in
    // range): strips BOTH entries -- live override AND shadowed base -- so
    // the slot must end EMPTY, not restored-to-a-purged-entry.
    const auto* liveDesc = creg->GetComponentDescriptor(idA);
    ASSERT_NE(liveDesc, nullptr);
    const void* base = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(liveDesc->defaultConstruct) & ~uintptr_t(0xFFFF));
    const size_t dropped = creg->UnregisterModuleRange(base, size_t(1) << 30);
    EXPECT_GE(dropped, 1u);
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);        // no dangling restore
    mod.Reset();                                                  // idempotent after purge: no crash
}

TEST(ComponentModule, ComponentNamesDoNotGrowOnRebind)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "NameReuse");
    mod.Register<Astra_Test_Mod::OwnedA>();
    const size_t after1 = creg->ComponentNameCount();
    mod.Register<Astra_Test_Mod::OwnedA>();
    mod.Register<Astra_Test_Mod::OwnedA>();
    EXPECT_EQ(creg->ComponentNameCount(), after1);                // reused by content, not appended
}
```

Add `ASTRA_NODISCARD size_t ComponentRegistry::ComponentNameCount() const { return m_componentNames.size(); }` (public; also useful for AstraStudio later).

- [ ] **Step 2: Build, verify FAIL** (purge ignores shadows; names grow).

- [ ] **Step 3: Implement**

`UnregisterModuleRange` extension, inside the existing per-id loop (after the current live-entry `owned` check), with the same `inRange` lambda:

1. First strip in-range SHADOW entries for this id (same fn-pointer probe against each `ShadowEntry::desc`), regardless of whether the live entry is owned.
2. Then, if the live entry is in-range: instead of always blanking, restore the newest REMAINING shadow entry (same code path as `ReleaseModule` — extract a private `RestoreOrClearSlot(ComponentID id)` helper used by both) and count it dropped. Meta work from inside the purge is collected the same way — `MetaRestore` items (restore ⇒ rebuild+rebind+relink; **clear-to-empty ⇒ null buildMeta + the purged descriptor's hash, captured BEFORE blanking ⇒ `EraseUnchecked`**, spec §3.6) and run after releasing the registration lock. The purge has no captured context — it runs the meta work through `MetaRegistry::Instance()`, which is correct here because the caller is the HOST (the module that installed the shared context), not the dying plugin.
3. Also clear `m_owner[id]`/`m_metaThunk[id]` when in-range (a purged thunk must never be invoked).

`StoreComponentName` reuse: before `emplace_back`, linear-scan `m_componentNames` for an equal string and return its `c_str()` if found (the deque is tiny — bounded by distinct type names; a hot-reload loop re-registers the SAME names, which is exactly the case this makes O(existing) instead of unbounded growth).

- [ ] **Step 4: Build, run `ComponentModule*` + the existing `UnregisterModuleRange` coverage + full suite. Commit**

```bash
git add include/Astra/Component/ComponentRegistry.hpp tests/Component/ComponentModuleTest.cpp
git commit -m "feat(component): range purge strips shadow entries; component-name storage reuse"
```

---

### Task 8: Concurrency smoke, 3-config gate, docs

**Files:**
- Test: `tests/Component/ComponentModuleTest.cpp` (one concurrency test)
- Modify: `include/Astra/Component/ComponentModule.hpp` (final doc comment pass: heap-held-handle contract, loader-lock warning, "register only what you own")

**Steps:**

- [ ] **Step 1: Concurrency smoke test** (two threads, two handles, disjoint types — reuse `OwnedA`/`OwnedB`; asserts no crash/torn state under the registration mutex):

```cpp
TEST(ComponentModule, ConcurrentRegisterFromTwoModules)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto m1 = Astra::ComponentModule::Open(creg, "T1");
    auto m2 = Astra::ComponentModule::Open(creg, "T2");
    std::thread t1([&] { for (int i = 0; i < 100; ++i) m1.Register<Astra_Test_Mod::OwnedA>(); });
    std::thread t2([&] { for (int i = 0; i < 100; ++i) m2.Register<Astra_Test_Mod::OwnedB>(); });
    t1.join(); t2.join();
    EXPECT_NE(creg->GetComponentDescriptor(Astra::TypeID<Astra_Test_Mod::OwnedA>::Value()), nullptr);
    EXPECT_NE(creg->GetComponentDescriptor(Astra::TypeID<Astra_Test_Mod::OwnedB>::Value()), nullptr);
}
```

- [ ] **Step 1b: Refusal-path unit test** (spec §4 item 5 — the module path must never own a refused id; `INVALID_COMPONENT` cannot be fabricated through a public collision in one TU, so exercise the guard directly):

```cpp
TEST(ComponentModule, InstallOwnedRefusesInvalidId)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentDescriptor desc{};
    EXPECT_FALSE(creg->InstallOwned(Astra::INVALID_COMPONENT, 1u, desc, nullptr));
    EXPECT_FALSE(creg->InstallOwned(static_cast<Astra::ComponentID>(Astra::MAX_COMPONENTS), 1u, desc, nullptr));
    EXPECT_EQ(creg->Size(), 0u);
}
```

(`ComponentModule::RegisterOne`'s own early-return on `INVALID_COMPONENT` is the same branch, exercised process-wide by the Theme E collision suites.)

- [ ] **Step 2: Final header doc pass** on `ComponentModule.hpp` and the new registry members. The class comment must state (verbatim requirements from the spec) — heap-held in plugins, reset in Shutdown, NEVER a DLL static (destructor under loader lock), "register only what you own" posture, `Register<T>` instantiation-module significance, and that `UnregisterModuleRange` remains the fallback net. Plus these four one-liners (plan-review findings 5 & 6):
  - In `RegisterOne`, above the `MakeDescriptor` call: `// desc.meta is captured BEFORE the phase-2 rebind -- valid only because RebindInPlace is address-stable.`
  - In `RegisterOne`, above the `Meta().Get` call: `// Get() never drains the pending queue (TypeContext.hpp:210-213); it cannot miss here only because Open's tripwire proved SetTypeContext ran, which drained.`
  - On the phase-1/phase-2 pair: `// Not atomic across two modules racing the same type -- benign: reload registration is host-serialized by contract.`
  - On the purge restore path (Task 7's helper): `// A restored shadow entry's module is still mapped only under RAII discipline; a host that unmapped a non-RAII module without purging it first double-faults here -- that ordering is the documented contract.`
  - `ComponentNameCount()` doc: `// Introduced for registration-lifecycle tests + AstraStudio's registry panel; counts distinct stored name strings, not registered components.`

- [ ] **Step 3: 3-config gate**

Run all three:
```
MSBuild.exe Astra.sln -p:Configuration=Debug   -p:Platform=x64 -m   && bin/Debug-windows-x86_64/AstraTest/AstraTest.exe
MSBuild.exe Astra.sln -p:Configuration=Release -p:Platform=x64 -m   && bin/Release-windows-x86_64/AstraTest/AstraTest.exe
MSBuild.exe Astra.sln -p:Configuration=Dist    -p:Platform=x64 -m   && bin/Dist-windows-x86_64/AstraTest/AstraTest.exe
```
Expected: all green (baseline 811/809/809 + the ~14 new tests; gate on "all configs green + intended new tests", not absolute counts). `CompressionTest.PerformanceBenchmark` alone failing = rerun isolated.

- [ ] **Step 4: Commit**

```bash
git add include/Astra/Component/ComponentModule.hpp tests/Component/ComponentModuleTest.cpp
git commit -m "test(component): concurrency smoke + ComponentModule contract docs; 3-config green"
```

---

## After the plan

Whole-branch review (opus effort — core/registration diff), independent controller 3-config verify, then local FF-merge `feature/component-module` → dev, delete the branch, do not push. The **Arcane movement** (vendor sync + migration + ABI v10 + zero-legacy sweep) and **Aphelyon movement** get their own plan once this lands — spec §5.
