# Meta Binder Scoping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `TypeMeta` lifetime correct when several `ComponentRegistry` instances share one `TypeContext`, so a module-owned reflected component in one registry can never erase a meta another registry or a registry-less consumer still uses, and a module that never unmaps can say so.

**Architecture:** One `MetaRegistry` per `TypeContext` stays. Each meta entry gains a small stack of *binders* `{module token, build thunk, refs, pinned}`; the entry's content is always bound to the top binder, refs are owned by registry slots (one per live-or-shadowed entry) and `RegisterMeta` adoptions, a binder leaves the stack only at zero refs and unpinned, and an entry is erased only when its stack is empty. A module declares residency once at `SetTypeContext`, which pins every binder it pushes. `ComponentRegistry` and `ComponentModule` stop deciding meta lifetime themselves and reduce to `Bind` / `Acquire` / `Release` calls.

**Tech Stack:** header-only C++20 (MSVC via `Astra.sln`, premake5-generated), GoogleTest (`AstraTest`), three configs Debug/Release/Dist.

**Spec:** `docs/superpowers/specs/2026-09-09-astra-meta-binder-scoping-design.md` — read it first; every task below cites the section it implements.

## Global Constraints

- Repo `D:\dev\starworks\Astra`, branch `feature/meta-binder` off `dev` HEAD (`f5f6ea5`, the commit that adds this plan; `20cd3ee` is `07b9240` plus the spec commit only, so the 852-test baseline cited below is the same tree). Work in a `git worktree` (spec §5 / house rule: a dirty main tree once produced a green build that validated nothing). `Astra.sln`/`ide/` are gitignored: after creating the worktree, and again after **adding any new test file** (the test project globs `tests/**.cpp` at generation time), run `D:\dev\_shared\tools\premake5.exe vs2022` inside the worktree.
- Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m -v:m -nologo` (whole solution; `-t:AstraTest` does not work). Test binary: `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe`. Filter with `--gtest_filter=Suite.Name`.
- Header-only library: **every task must leave the whole tree compiling and 852+ tests green** (`AstraTest.exe --gtest_brief=1`). There is no partial build; a task that removes an API must rewire its callers in the same task.
- Baseline before Task 1: Debug 852/852 (verified 2026-09-09 on `07b9240`). `CompressionTest.PerformanceBenchmark` is a known flake under CPU load; a lone failure of only that test is not a regression (rerun isolated).
- TypeID ceiling: the test binary builds with `ASTRA_MAX_COMPONENTS=192` (premake, test project only) and is near it. **Mint no new component types.** New reflected types are allowed only if they are never registered as components and never have `TypeID<T>::Value()` called (reflection-only types cost zero ids). Reuse `Astra_Test_ModMeta2::ReflectedOwned` / `ReflectedEphemeral` (ComponentModuleTest.cpp) and `Astra_Test_ModMeta::MetaProbe` (MetaRebindTest.cpp).
- Lock discipline (spec §3.3): registration mutex → meta mutex, never reversed; **no user reflection code (`MetaBuildFn` thunks) under either mutex**. `Bind`/`Acquire`/`InstallBaseline` never run a thunk; `Release` and `Rebind` may and are called only after every registry lock is dropped.
- Style: match surrounding code (4-space indent, Allman braces, `--` in comments, ASCII only, `ASTRA_NODISCARD`, `ASTRA_ENSURE_ALWAYS` for all-config refusals, `ASTRA_LOG_*` for diagnostics). Comments explain *why*; cite `spec 2026-09-09 §x.y` where a rule comes from the spec.
- Commit style: `feat(reflection): ...`, `feat(component): ...`, `test(...)`, `docs(...)`, one logical change per commit, explicit `git add <paths>`; end every commit message with:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01CcMKuqRThS9DA3z4aorh7t
  ```
  That trailer is the attribution of the session that WROTE this plan. The executing session substitutes its own model name and session link.
- Concurrency scope: beyond the reentrant-thunk unit test (Task 2) and the two-module smoke, the cases "two concurrent Releases on one hash", "Release racing Acquire", and "erase-then-reinstall during a thunk window" were walked at plan review and none corrupts state; all are out-of-contract under spec §3.6's single-threaded registration rule and are deliberately not tested.
- Finish (after Task 6): whole-branch review at opus effort (registration + concurrency diff), controller-independent three-config verify, local fast-forward merge to `dev`, delete the branch, **do not push**.

---

## File Structure

| File | Responsibility after this plan |
|---|---|
| `include/Astra/Core/ModuleIdentity.hpp` (new) | `ModuleResidency`, `ModuleToken`, `Detail::ModuleIdentity`, `Detail::CurrentModuleIdentity()`, `Detail::ScopedModuleIdentity` — the per-module identity seam. Nothing else. |
| `include/Astra/Core/TypeContext.hpp` | includes the seam; `SetTypeContext` gains the residency parameter and stamps it before the drain. |
| `include/Astra/Reflection/MetaRegistry.hpp` | `MetaBinder`, `BindOutcome`/`BindResult`/`ReleaseOutcome`, the binder stack per entry, `InstallBaseline`/`Bind`/`Acquire`/`Release`/`Rebind`, diagnostics; `Erase`/`EraseUnchecked` gone; drain installs baselines. |
| `include/Astra/Reflection/Macros.hpp` | `ReflectType`/`ReflectEnum` store a factory and install a baseline. |
| `include/Astra/Component/ComponentRegistry.hpp` | slot-level ref accounting: `InstallResult`, `MetaRelease`, per-slot module token, no per-slot thunk; `UnregisterModuleRange` releases per entry. |
| `include/Astra/Component/ComponentModule.hpp` | captures `ModuleIdentity` at `Open`; `Register` = Bind + install + Acquire; `Reset` = releases. |
| `tests/Core/ModuleIdentityTest.cpp` (new) | seam tests. |
| `tests/Reflection/MetaBinderTest.cpp` (new) | `MetaRegistry` binder-stack unit tests on a local registry. |
| `tests/Reflection/MetaRebindTest.cpp` | erase-guard test removed (moved to MetaBinderTest); drain-baseline + `ReflectType` factory tests added. |
| `tests/Component/ComponentModuleTest.cpp` | `InstallOwned` tests updated; Arcane-shaped integration tests added. |
| `docs/superpowers/specs/2026-08-09-astra-component-module-raii-design.md` | §3.6 caveat replaced by a pointer to the new spec; §3.5.1 backlog note marked done. |
| `docs/superpowers/specs/2026-09-09-astra-meta-binder-scoping-design.md` | §3.3/§3.7 amended for the two additive ops this plan introduces (`Rebind`, diagnostics) and the dropped `InstallOwned` thunk parameter. |

---

### Task 1: Module identity + residency seam

**Spec:** §3.1.

**Files:**
- Create: `include/Astra/Core/ModuleIdentity.hpp`
- Modify: `include/Astra/Core/TypeContext.hpp:12-15` (includes), `:199-206` (`SetTypeContext`)
- Create: `tests/Core/ModuleIdentityTest.cpp`

**Interfaces:**
- Produces: `Astra::ModuleResidency { Transient, Resident }`, `Astra::ModuleToken = const void*`, `Astra::Detail::ModuleIdentity { ModuleToken token; ModuleResidency residency; }`, `Astra::Detail::ModuleIdentity& Detail::CurrentModuleIdentity() noexcept`, `Astra::Detail::ScopedModuleIdentity(ModuleToken, ModuleResidency)`, `Astra::SetTypeContext(TypeContext*, ModuleResidency = ModuleResidency::Transient)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/Core/ModuleIdentityTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Core/ModuleIdentity.hpp>
#include <Astra/Core/TypeContext.hpp>

// Spec 2026-09-09 §3.1: one identity per EXE/DLL image -- a per-module
// inline-function-local static whose own address is the token.

TEST(ModuleIdentity, TokenIsStableAndNonNull)
{
    const Astra::ModuleToken a = Astra::Detail::CurrentModuleIdentity().token;
    const Astra::ModuleToken b = Astra::Detail::CurrentModuleIdentity().token;
    EXPECT_NE(a, nullptr);
    EXPECT_EQ(a, b);
}

TEST(ModuleIdentity, DefaultResidencyIsTransient)
{
    // Default = today's plugin semantics; nothing changes for code that never
    // declares residency.
    Astra::SetTypeContext(nullptr);   // reset any residency a prior test declared
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Transient);
}

TEST(ModuleIdentity, ScopedOverrideRestoresOnExit)
{
    static int s_fakeImage = 0;   // any distinct address stands in for another image
    const Astra::Detail::ModuleIdentity before = Astra::Detail::CurrentModuleIdentity();
    {
        Astra::Detail::ScopedModuleIdentity scope(&s_fakeImage, Astra::ModuleResidency::Resident);
        EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().token, &s_fakeImage);
        EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Resident);
    }
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().token, before.token);
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, before.residency);
}

TEST(ModuleIdentity, SetTypeContextStampsResidency)
{
    // The residency argument is stored on the module identity BEFORE the
    // pending-meta drain runs, so drained baselines see it (§3.1). Exercised
    // through the nullptr (uninstall, no-drain) path ON PURPOSE: if this test
    // happened to be the first drain in the process, a Resident install would
    // pin every ASTRA_REFLECT_TYPE'd meta in the binary and break the
    // transient-erase tests elsewhere in the suite.
    Astra::SetTypeContext(nullptr, Astra::ModuleResidency::Resident);
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Resident);
    Astra::SetTypeContext(nullptr);   // defaulted argument resets to Transient
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Transient);
}
```

- [ ] **Step 2: Regenerate projects and run the tests to verify they fail**

Run (inside the worktree):
```
D:\dev\_shared\tools\premake5.exe vs2022
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m -v:m -nologo
```
Expected: build FAILS — `Astra/Core/ModuleIdentity.hpp: No such file or directory`.

- [ ] **Step 3: Create the seam header**

Create `include/Astra/Core/ModuleIdentity.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace Astra
{
    // Whether the calling module's image can ever be unmapped (spec
    // 2026-09-09 §3.1). Declared once, at SetTypeContext, before any
    // registration in that module. Resident => every meta binder this module
    // pushes is PINNED (never removed at zero refs), so registry-less
    // GetMeta/GetByName keep resolving after the module's last handle is
    // gone. Transient (default) => a binder leaves at zero refs, which is
    // exactly today's plugin behaviour. A module that declares Resident and
    // then unmaps leaves pinned binders whose content dangles -- the same
    // misuse class as a leaked handle; Astra cannot detect either.
    enum class ModuleResidency : uint8_t { Transient, Resident };

    // Identity of the calling EXE/DLL image: the address of a per-module
    // inline-function-local static (the same mechanism as the per-module
    // TypeContext slot). A reloaded DLL lands at a new base address and so
    // gets a NEW token -- that is what makes load-before-unload reload fall
    // out with no special handling. POSIX caveat (shared with the context
    // slot): default-visibility DSOs may coalesce the static, merging two
    // modules into one token. That only over-holds; it never erases early.
    using ModuleToken = const void*;

    namespace Detail
    {
        struct ModuleIdentity
        {
            ModuleToken     token     = nullptr;
            ModuleResidency residency = ModuleResidency::Transient;
        };

        // Per-module slot. The token is the slot's own address: unique per
        // image, stable for the image's life, never null.
        inline ModuleIdentity& CurrentModuleIdentity() noexcept
        {
            static ModuleIdentity s_identity{ &s_identity, ModuleResidency::Transient };
            return s_identity;
        }

        // Test seam: swaps the per-module identity for a scope so one test
        // binary can play several modules (an "engine" and a "plugin" with
        // distinct tokens and residencies). Restores the previous identity
        // on exit. Not for production use; production identity comes from
        // the image itself.
        struct ScopedModuleIdentity
        {
            ScopedModuleIdentity(ModuleToken token, ModuleResidency residency)
                : m_saved(CurrentModuleIdentity())
            {
                CurrentModuleIdentity() = ModuleIdentity{ token, residency };
            }
            ~ScopedModuleIdentity() { CurrentModuleIdentity() = m_saved; }
            ScopedModuleIdentity(const ScopedModuleIdentity&) = delete;
            ScopedModuleIdentity& operator=(const ScopedModuleIdentity&) = delete;

        private:
            ModuleIdentity m_saved;
        };
    }
}
```

- [ ] **Step 4: Wire `SetTypeContext`**

In `include/Astra/Core/TypeContext.hpp`, add after `#include "Log.hpp"`:

```cpp
#include "ModuleIdentity.hpp"
```

Replace the `SetTypeContext` definition (currently at :199-206) with:

```cpp
    // Install the process-shared context for THIS module, draining any
    // pending static meta registrations into it. Must run before the
    // module's first TypeID<T>::Value() / Registry use. Passing nullptr
    // uninstalls (reverts to DefaultTypeContext) and performs no drain.
    // `residency` is stamped on this module's identity FIRST, so the drained
    // baselines are pinned iff this module never unmaps (spec 2026-09-09
    // §3.1). Declare it once, before any registration in this module.
    // Not noexcept: drained registration callbacks may allocate.
    inline void SetTypeContext(TypeContext* ctx, ModuleResidency residency = ModuleResidency::Transient)
    {
        Detail::CurrentModuleIdentity().residency = residency;
        Detail::CurrentTypeContextSlot() = ctx;
        if (ctx)
        {
            Detail::DrainPendingMeta(*ctx);
        }
    }
```

- [ ] **Step 5: Build and run the new tests**

Run: build (Step 2 command), then `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=ModuleIdentity.*`
Expected: 4 PASSED.

- [ ] **Step 6: Run the whole suite**

Run: `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`
Expected: 856 tests, all PASSED (852 + 4).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Core/ModuleIdentity.hpp include/Astra/Core/TypeContext.hpp tests/Core/ModuleIdentityTest.cpp
git commit -m "feat(core): module identity + residency seam -- SetTypeContext declares whether this image ever unmaps"
```

---

### Task 2: MetaRegistry binder stack + operations + unit tests

**Spec:** §3.2, §3.3 (plus two additive ops: `Rebind` for the deferred-rebuild path and read-only diagnostics for tests/tools; Task 6 records them in the spec).

**Files:**
- Modify: `include/Astra/Reflection/MetaRegistry.hpp` (whole class body :23-393; `Erase`/`EraseUnchecked` are **kept in this task** so `ComponentModule`/`ComponentRegistry` still compile — Task 3 deletes them)
- Create: `tests/Reflection/MetaBinderTest.cpp`

**Interfaces:**
- Consumes: `Detail::ModuleIdentity`, `ModuleToken`, `ModuleResidency` (Task 1); `MetaBuildFn = TypeMeta (*)()` (`Component/Component.hpp:41`); `SmallVector<T,N>` (`insert(const_iterator, T&&)`, `erase(const_iterator)`, `back()`, iterators are `T*`).
- Produces (all on `Astra::MetaRegistry` unless noted):
  - `struct Astra::MetaBinder { ModuleToken module; MetaBuildFn build; uint32_t refs; bool pinned; }`
  - `enum class Astra::BindOutcome : uint8_t { Refused, Bound, BoundNeedsRebuild }`
  - `enum class Astra::ReleaseOutcome : uint8_t { Unbound, Held, Dropped, Retained, Rebound, Erased }`
  - `struct Astra::BindResult { BindOutcome outcome; TypeMeta* meta; }`
  - `BindResult InstallBaseline(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta&& fresh)`
  - `BindResult Bind(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta* fresh)`
  - `bool Acquire(uint64_t hash, ModuleToken module)`
  - `ReleaseOutcome Release(uint64_t hash, ModuleToken module)`
  - `bool Rebind(uint64_t hash, ModuleToken module, TypeMeta&& fresh)` — swap content iff `module`'s binder is top
  - diagnostics: `size_t BinderCount(uint64_t hash) const`, `uint32_t Refs(uint64_t hash, ModuleToken module) const`, `bool IsPinned(uint64_t hash, ModuleToken module) const`, `ModuleToken TopBinder(uint64_t hash) const`
  - unchanged: `Instance()`, `Register(uint64_t, string_view)`, `Register(TypeMeta&&)`, `RebindInPlace`, `Get`, `GetByName`, `IsRegistered`, `LinkToComponent`, `GetComponentId`, `GetTypeHash`, `GetByComponentId`, `GetRegisteredCount`, `ForEachType`, `Clear`.

- [ ] **Step 1: Write the failing unit tests**

Create `tests/Reflection/MetaBinderTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Core/ModuleIdentity.hpp>

// Spec 2026-09-09 §3.2/§3.3: the binder stack. Every test uses a LOCAL
// MetaRegistry (not Instance()) and a synthetic hash, so nothing here touches
// the shared default-context registry or consumes a ComponentID. "Content"
// is observed through fields.size(), which the identity check ignores.

namespace
{
    constexpr uint64_t kHash = 0xB1BDE50000000001ull;

    // Distinct addresses stand in for distinct module images.
    int s_imageA = 0;
    int s_imageB = 0;
    int s_imageC = 0;

    Astra::TypeMeta Make(size_t fieldCount)
    {
        Astra::TypeMeta m{};   // value-init: TypeMeta has no member initializers
        m.typeHash  = kHash;
        m.typeName  = "Astra_Test_Binder::Probe";
        m.size      = 8;
        m.alignment = 4;
        m.isTrivial = true;
        m.fields.resize(fieldCount);
        return m;
    }
    Astra::TypeMeta BuildOne()   { return Make(1); }
    Astra::TypeMeta BuildTwo()   { return Make(2); }
    Astra::TypeMeta BuildThree() { return Make(3); }

    Astra::Detail::ModuleIdentity Who(const void* token,
                                      Astra::ModuleResidency r = Astra::ModuleResidency::Transient)
    {
        return Astra::Detail::ModuleIdentity{ token, r };
    }

    // For the re-entrancy test: a build thunk that pushes a THIRD module on
    // top while Release is rebuilding for the module it thought was top.
    Astra::MetaRegistry* g_reentrantTarget = nullptr;
    Astra::TypeMeta ReentrantBuild()
    {
        g_reentrantTarget->Bind(kHash, Who(&s_imageC), &BuildTwo, nullptr);
        return Make(1);
    }
}

TEST(MetaBinder, BindInstallsThenSecondModuleTopsAndReleaseRebinds)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    const Astra::BindResult a = reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one);
    ASSERT_EQ(a.outcome, Astra::BindOutcome::Bound);
    ASSERT_NE(a.meta, nullptr);
    EXPECT_EQ(a.meta->fields.size(), 1u);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);

    Astra::TypeMeta two = BuildTwo();
    const Astra::BindResult b = reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two);
    ASSERT_EQ(b.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(b.meta, a.meta);                       // address stable across binders
    EXPECT_EQ(a.meta->fields.size(), 2u);            // content = top binder (B)
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(reg.BinderCount(kHash), 2u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.Get(kHash), a.meta);               // same address
    EXPECT_EQ(a.meta->fields.size(), 1u);            // content rebuilt from A's thunk
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Get(kHash), nullptr);
}

TEST(MetaBinder, RefsHoldThenLastReleaseErasesWithLinkRows)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));      // two registry slots
    reg.LinkToComponent(kHash, static_cast<Astra::ComponentID>(77));
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 2u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Held);
    EXPECT_NE(reg.GetByComponentId(static_cast<Astra::ComponentID>(77)), nullptr);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 1u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Get(kHash), nullptr);
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(77)), nullptr);
    EXPECT_EQ(reg.GetComponentId(kHash), Astra::INVALID_COMPONENT);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 0u);
}

TEST(MetaBinder, PinnedBinderIsRetainedAndOverriderRebindsBackToIt)
{
    Astra::MetaRegistry reg;
    // Resident "engine" drains a baseline: pinned, refs 0.
    ASSERT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA, Astra::ModuleResidency::Resident), &BuildOne, BuildOne()).outcome,
              Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.IsPinned(kHash, &s_imageA));
    const Astra::TypeMeta* address = reg.Get(kHash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->fields.size(), 1u);

    // Transient "plugin" overrides.
    Astra::TypeMeta two = BuildTwo();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(address->fields.size(), 2u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    // Plugin departs: content returns to the pinned engine thunk.
    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(address->fields.size(), 1u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);

    // Engine's own refs come and go; at zero it is RETAINED, never erased.
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Retained);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 0u);
}

TEST(MetaBinder, BindRefusesIdentityMismatchAndPushesNothing)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);

    Astra::TypeMeta impostor = BuildOne();
    impostor.size = 16;                               // same hash, different identity
    const Astra::BindResult r = reg.Bind(kHash, Who(&s_imageB), &BuildOne, &impostor);   // ENSURE logs, continues
    EXPECT_EQ(r.outcome, Astra::BindOutcome::Refused);
    EXPECT_EQ(r.meta, nullptr);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);
    EXPECT_EQ(reg.Get(kHash)->size, 8u);              // incumbent content untouched

    // Same refusal on the baseline path.
    Astra::TypeMeta impostor2 = BuildOne();
    impostor2.alignment = 8;
    EXPECT_EQ(reg.InstallBaseline(kHash, Who(&s_imageC), &BuildOne, std::move(impostor2)).outcome,
              Astra::BindOutcome::Refused);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
}

TEST(MetaBinder, BindAbsentEntryWithoutFreshIsRefused)
{
    Astra::MetaRegistry reg;
    const Astra::BindResult r = reg.Bind(kHash, Who(&s_imageA), &BuildOne, nullptr);
    EXPECT_EQ(r.outcome, Astra::BindOutcome::Refused);
    EXPECT_EQ(r.meta, nullptr);
    EXPECT_EQ(reg.Get(kHash), nullptr);
    EXPECT_EQ(reg.BinderCount(kHash), 0u);
}

TEST(MetaBinder, ReleaseOnUnknownTokenOrHashIsUnbound)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Unbound);        // no binder for B
    EXPECT_EQ(reg.Release(0xDEADull, &s_imageA), Astra::ReleaseOutcome::Unbound);    // no such entry
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 1u);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);

    // A binder at zero refs is not "held": releasing it again is a caller
    // bug, reported as Unbound (ENSURE logs, continues) and changes nothing.
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Unbound);
}

TEST(MetaBinder, AcquireWithoutBindRefuses)
{
    Astra::MetaRegistry reg;
    EXPECT_FALSE(reg.Acquire(kHash, &s_imageA));      // no entry (ENSURE logs, continues)
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_FALSE(reg.Acquire(kHash, &s_imageB));      // entry, but no binder for B
    EXPECT_EQ(reg.Refs(kHash, &s_imageB), 0u);
}

TEST(MetaBinder, ReentrantBindDuringReleaseSkipsTheStaleSwap)
{
    // Release(B) pops B, sees A as the new top, and runs A's thunk with the
    // mutex released. That thunk pushes C on top. On relock the top is no
    // longer A, so the swap MUST be skipped: content stays as it was. The
    // outcome is still Rebound (the release completed and a rebind was
    // attempted; the swap itself was superseded -- see Release's contract).
    Astra::MetaRegistry reg;
    g_reentrantTarget = &reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &ReentrantBuild, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    Astra::TypeMeta two = BuildTwo();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    ASSERT_EQ(reg.Get(kHash)->fields.size(), 2u);

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageC);       // C landed on top during the rebuild window
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 2u);     // A's stale build was NOT swapped in
    EXPECT_EQ(reg.BinderCount(kHash), 2u);            // A, C
    g_reentrantTarget = nullptr;
}

TEST(MetaBinder, InstallBaselineOnExistingEntryPushesBottomAndKeepsContent)
{
    Astra::MetaRegistry reg;
    ASSERT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA), &BuildOne, BuildOne()).outcome, Astra::BindOutcome::Bound);
    const Astra::TypeMeta* address = reg.Get(kHash);
    ASSERT_NE(address, nullptr);

    const Astra::BindResult second = reg.InstallBaseline(kHash, Who(&s_imageB), &BuildTwo, BuildTwo());
    EXPECT_EQ(second.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(second.meta, address);
    EXPECT_EQ(address->fields.size(), 1u);            // first-wins content
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);       // B sits at the BOTTOM
    EXPECT_EQ(reg.BinderCount(kHash), 2u);

    EXPECT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA), &BuildOne, BuildOne()).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.BinderCount(kHash), 2u);            // idempotent per module
}

TEST(MetaBinder, NullBuildBinderNeverTopsAndAllNullBuildReleaseRetains)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));

    // A module with no factory for this type registers it as a component:
    // it holds a ref but can never source content.
    const Astra::BindResult d = reg.Bind(kHash, Who(&s_imageB), nullptr, nullptr);
    EXPECT_EQ(d.outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);       // inserted BELOW the top
    EXPECT_EQ(reg.BinderCount(kHash), 2u);

    // A departs: only a null-build binder remains. Nothing can rebuild the
    // content, so the entry is kept (address valid) and a warning is logged.
    const Astra::TypeMeta* address = reg.Get(kHash);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Retained);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
}

TEST(MetaBinder, BindWithoutFreshOnNewTopReportsNeedsRebuildAndRebindHonorsTopOnly)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);

    // B binds under a lock elsewhere (no thunk allowed): it lands on top and
    // is told to rebuild later.
    const Astra::BindResult b = reg.Bind(kHash, Who(&s_imageB), &BuildTwo, nullptr);
    EXPECT_EQ(b.outcome, Astra::BindOutcome::BoundNeedsRebuild);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 1u);     // content not yet B's
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    EXPECT_FALSE(reg.Rebind(kHash, &s_imageA, BuildThree()));   // A is not top: refused, no change
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 1u);
    EXPECT_TRUE(reg.Rebind(kHash, &s_imageB, BuildTwo()));      // B is top: swapped
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 2u);

    // Same-module re-bind with fresh while top: swaps (in-binary reload).
    Astra::TypeMeta three = BuildThree();
    EXPECT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &three).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 3u);
    // Existing binder NOT top + fresh: no swap.
    Astra::TypeMeta again = BuildOne();
    EXPECT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &again).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 3u);
    EXPECT_EQ(reg.BinderCount(kHash), 2u);
}

TEST(MetaBinder, ComponentLinkedHashWithLiveRefsIsHeldNotErased)
{
    // Replaces MetaRebindTest.EraseGuardsComponentLinkedHashes: there is no
    // guarded Erase any more -- a component-linked hash simply has refs.
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));                       // the component slot
    reg.LinkToComponent(kHash, static_cast<Astra::ComponentID>(5));
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));                       // a RegisterMeta adoption

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Held);   // RegisterMeta teardown
    EXPECT_NE(reg.GetByComponentId(static_cast<Astra::ComponentID>(5)), nullptr);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased); // slot clears
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(5)), nullptr);
}
```

- [ ] **Step 2: Regenerate projects, build, verify the tests fail to compile**

Run: `D:\dev\_shared\tools\premake5.exe vs2022`, then the Debug build.
Expected: build FAILS on `MetaBinderTest.cpp` — `'Bind': is not a member of 'Astra::MetaRegistry'` (and `BindOutcome`, `Acquire`, ...).

- [ ] **Step 3: Implement the binder stack in `MetaRegistry.hpp`**

Replace the file's contents from the `#include` block through the end of the `MetaRegistry` class (up to and including the closing `};` before `inline MetaRegistry& TypeContext::Meta()`) with the following. Everything below the class (`TypeContext::Meta()`, the convenience functions, `Detail::MetaFactory`, `BuildMetaThunk`, `StaticTypeRegistrar`) stays as it is in this task.

```cpp
#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "../Container/SmallVector.hpp"
#include "../Core/ModuleIdentity.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "TypeMeta.hpp"

namespace Astra
{
    // Who is bound to a meta entry (spec 2026-09-09 §3.2). One per module
    // that has registered, adopted, or drained this type. The TOP binder is
    // the one whose closures the entry's TypeMeta content currently carries.
    struct MetaBinder
    {
        ModuleToken module = nullptr;
        MetaBuildFn build  = nullptr;   // null: this module has no reflect factory for the type
                                        // (registered a component it did not reflect) -- it can
                                        // hold refs but never source content (§3.6)
        uint32_t    refs   = 0;         // live-or-shadowed registry slots + RegisterMeta adoptions
        bool        pinned = false;     // resident module: never removed at zero refs
    };

    enum class BindOutcome    : uint8_t { Refused, Bound, BoundNeedsRebuild };
    enum class ReleaseOutcome : uint8_t { Unbound, Held, Dropped, Retained, Rebound, Erased };
    struct BindResult { BindOutcome outcome; TypeMeta* meta; };

    /**
     * Registry for type metadata, owned by a TypeContext.
     * Provides thread-safe registration and lookup of TypeMeta instances.
     * Instance() resolves through the active TypeContext, so all modules
     * sharing one context see the same metadata.
     *
     * Lifetime (spec 2026-09-09 §3.3): an entry's TypeMeta address is stable
     * for the entry's whole life. Registrations bind and acquire refs; a
     * binder leaves the stack only at zero refs and unpinned; the entry is
     * erased only when its stack is empty. Bind/Acquire/InstallBaseline never
     * run user code and may be called under a registry lock (lock order:
     * registration mutex -> meta mutex). Release and Rebind may run a
     * MetaBuildFn thunk -- user reflection code -- and must be called with
     * no registry lock held; the thunk itself runs with THIS mutex released.
     */
    class MetaRegistry
    {
    public:
        MetaRegistry() = default;
        ~MetaRegistry() = default;

        /**
         * Gets the registry of the active TypeContext.
         * Drains any pending static registrations first, so reflection
         * registered during static initialization is visible here.
         */
        static MetaRegistry& Instance()
        {
            TypeContext* ctx = GetTypeContext();
            Detail::DrainPendingMeta(*ctx);
            return ctx->Meta();
        }

        // Deleted copy/move
        MetaRegistry(const MetaRegistry&) = delete;
        MetaRegistry& operator=(const MetaRegistry&) = delete;
        MetaRegistry(MetaRegistry&&) = delete;
        MetaRegistry& operator=(MetaRegistry&&) = delete;

        /**
         * Registers a new type or returns the existing registration.
         * Thread-safe. Installs a binder-less entry: nothing holds it, and it
         * is erased only if a later Bind attaches a binder that then releases
         * to an empty stack -- the raw manual path, mostly for tests.
         * @param hash Type hash
         * @param name Type name
         * @return Reference to the TypeMeta (new or existing)
         */
        TypeMeta& Register(uint64_t hash, std::string_view name)
        {
            std::unique_lock lock(m_mutex);

            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                // Hash hit: only the name is available to disambiguate here.
                // A DIFFERENT name on the same hash is a real XXHash64
                // collision of two distinct types -- refuse loudly rather than
                // alias the second type onto the first's metadata. Same name =>
                // idempotent re-registration, return the existing entry.
                // (Reference return type: cannot signal refusal via null, so
                // the loud log/ensure IS the refusal; mirrors TypeContext.)
                if (it->second.meta->typeName != name)
                {
                    std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
                    msg.append(name);
                    msg += "' shares the type-hash of already-registered '";
                    msg.append(it->second.meta->typeName);
                    msg += "'. The second type is refused. Give types a unique unqualified name.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
                }
                return *it->second.meta;
            }

            TypeMeta fresh;
            fresh.typeHash = hash;
            fresh.typeName = name;
            return *InstallEntryLocked(hash, std::move(fresh)).meta;
        }

        /**
         * Registers a TypeMeta instance directly (binder-less, see above).
         * Thread-safe.
         * @param meta The TypeMeta to register (moved)
         * @return Pointer to the registered TypeMeta, nullptr on identity collision
         */
        TypeMeta* Register(TypeMeta&& meta)
        {
            std::unique_lock lock(m_mutex);

            uint64_t hash = meta.typeHash;
            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                // Hash hit: compare the identity fields the TypeMeta already
                // carries. Matching size/alignment/triviality/name => the same
                // type re-registered (idempotent), return the existing entry.
                // Any difference is a real type-hash collision of two distinct
                // types -- refuse loudly and return nullptr rather than alias
                // one onto the other (aliasing mismatched size/alignment onto
                // an existing entry corrupts memory downstream).
                if (!SameIdentity(*it->second.meta, meta))
                {
                    RefuseCollision(*it->second.meta, meta);
                    return nullptr;
                }
                return it->second.meta.get();
            }
            return InstallEntryLocked(hash, std::move(meta)).meta.get();
        }

        // Raw content swap: installs `fresh` if the hash is unknown (binder-
        // less); on a hash hit with MATCHING identity move-assigns the
        // contents through the existing pointer -- the address every
        // ComponentDescriptor::meta caches stays valid, only the closures/
        // fields swap. Identity mismatch refuses (nullptr). Ignores the
        // binder stack entirely: prefer Bind/Rebind, which respect it.
        TypeMeta* RebindInPlace(TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(fresh.typeHash);
            if (it == m_types.end())
            {
                uint64_t hash = fresh.typeHash;
                return InstallEntryLocked(hash, std::move(fresh)).meta.get();
            }
            TypeMeta& existing = *it->second.meta;
            if (!SameIdentity(existing, fresh))
            {
                ASTRA_LOG_ERROR("MetaRegistry: RebindInPlace identity mismatch -- refused");
                ASTRA_ENSURE_ALWAYS(false, "MetaRegistry rebind identity collision");
                return nullptr;
            }
            existing = std::move(fresh);   // move-assign: unique_ptr target address unchanged
            return &existing;
        }

        // ==== Binder stack (spec 2026-09-09 §3.3) =============================

        // Drain / ReflectType path. Entry absent: install from `fresh` with
        // `who`'s binder (refs 0, pinned iff Resident) as the only one. Entry
        // present: identity-check `fresh`, then push a refs-0 binder AT THE
        // BOTTOM (first-wins content) -- a no-op if `who` already has one.
        // Never swaps existing content. Never runs a thunk.
        BindResult InstallBaseline(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                Entry& e = InstallEntryLocked(hash, std::move(fresh));
                e.binders.push_back(MakeBinder(who, build));
                return { BindOutcome::Bound, e.meta.get() };
            }
            Entry& e = it->second;
            if (!SameIdentity(*e.meta, fresh))
            {
                RefuseCollision(*e.meta, fresh);
                return { BindOutcome::Refused, nullptr };
            }
            if (FindBinder(e, who.token) == e.binders.size())
            {
                e.binders.insert(e.binders.begin(), MakeBinder(who, build));
            }
            return { BindOutcome::Bound, e.meta.get() };
        }

        // Registration path. Entry absent: install from `fresh` (Refused when
        // fresh is null) with `who`'s binder as the only one. Entry present,
        // no binder for `who` yet: push a NEW binder on top (build != null)
        // or just below the top (build == null -- it can hold refs but never
        // source content); if it landed on top, swap content to `fresh`, or,
        // when fresh is null and build != null, report BoundNeedsRebuild so
        // the caller runs build() OUTSIDE its locks and calls Rebind. Entry
        // present, binder exists: content is swapped only if that binder is
        // top AND fresh is given (same-image re-bind / in-binary reload);
        // otherwise nothing changes -- an existing top binder already carries
        // this module's closures. Identity mismatch on any supplied fresh ->
        // Refused. No ref change on any path. Never runs a thunk.
        BindResult Bind(uint64_t hash, Detail::ModuleIdentity who, MetaBuildFn build, TypeMeta* fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                if (!fresh)
                {
                    return { BindOutcome::Refused, nullptr };   // nothing to install from
                }
                Entry& e = InstallEntryLocked(hash, std::move(*fresh));
                e.binders.push_back(MakeBinder(who, build));
                return { BindOutcome::Bound, e.meta.get() };
            }
            Entry& e = it->second;
            if (fresh && !SameIdentity(*e.meta, *fresh))
            {
                RefuseCollision(*e.meta, *fresh);
                return { BindOutcome::Refused, nullptr };
            }
            const size_t idx = FindBinder(e, who.token);
            if (idx == e.binders.size())
            {
                const bool onTop = (build != nullptr) || e.binders.empty();
                if (!onTop)
                {
                    e.binders.insert(e.binders.end() - 1, MakeBinder(who, build));
                    return { BindOutcome::Bound, e.meta.get() };
                }
                e.binders.push_back(MakeBinder(who, build));
                if (fresh)
                {
                    *e.meta = std::move(*fresh);
                    return { BindOutcome::Bound, e.meta.get() };
                }
                return { build ? BindOutcome::BoundNeedsRebuild : BindOutcome::Bound, e.meta.get() };
            }
            if (fresh && idx == e.binders.size() - 1)
            {
                *e.meta = std::move(*fresh);
            }
            return { BindOutcome::Bound, e.meta.get() };
        }

        // refs++ on an EXISTING binder. A caller must Bind before Acquire;
        // an absent binder is refused (ENSURE) and returns false.
        bool Acquire(uint64_t hash, ModuleToken module)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            const size_t idx = (it != m_types.end()) ? FindBinder(it->second, module) : 0;
            const bool bound = (it != m_types.end()) && idx < it->second.binders.size();
            if (!ASTRA_ENSURE_ALWAYS(bound, "MetaRegistry::Acquire without a prior Bind for this module"))
            {
                return false;
            }
            ++it->second.binders[idx].refs;
            return true;
        }

        // refs--. Held while refs remain. At zero: pinned -> Retained (binder
        // stays, nothing else changes); unpinned -> the binder is removed --
        // Dropped if it was not top; if it WAS top and other binders remain,
        // content is rebuilt from the new top's thunk OUTSIDE this mutex,
        // then swapped under it only if that binder is still top (Rebound).
        // Rebound means the rebind was ATTEMPTED from the new top: if the top
        // changed during the thunk window (a thunk may re-enter Bind), that
        // newer binder owns the content and the attempted build is dropped --
        // still reported Rebound, because the departing binder's release
        // completed. A new top with no build thunk cannot rebuild -> Retained
        // + warning (§3.6). An empty stack erases the entry and both link rows
        // (Erased). Unknown hash/module, or a binder already at zero refs,
        // -> Unbound (the latter ENSUREs: release without acquire).
        ReleaseOutcome Release(uint64_t hash, ModuleToken module)
        {
            MetaBuildFn rebuild = nullptr;
            ModuleToken newTop  = nullptr;
            {
                std::unique_lock lock(m_mutex);
                auto it = m_types.Find(hash);
                if (it == m_types.end())
                {
                    return ReleaseOutcome::Unbound;
                }
                Entry& e = it->second;
                const size_t idx = FindBinder(e, module);
                if (idx == e.binders.size())
                {
                    return ReleaseOutcome::Unbound;
                }
                MetaBinder& b = e.binders[idx];
                if (!ASTRA_ENSURE_ALWAYS(b.refs > 0, "MetaRegistry::Release on a binder with no refs (release without acquire)"))
                {
                    return ReleaseOutcome::Unbound;
                }
                --b.refs;
                if (b.refs > 0)
                {
                    return ReleaseOutcome::Held;
                }
                if (b.pinned)
                {
                    return ReleaseOutcome::Retained;
                }
                const bool wasTop = (idx == e.binders.size() - 1);
                e.binders.erase(e.binders.begin() + idx);
                if (e.binders.empty())
                {
                    EraseEntryLocked(it);
                    return ReleaseOutcome::Erased;
                }
                if (!wasTop)
                {
                    return ReleaseOutcome::Dropped;
                }
                const MetaBinder& top = e.binders.back();
                if (top.build == nullptr)
                {
                    // Only binders without a factory remain: the content still
                    // points at the departed module's closures and nothing can
                    // rebuild it. Keep the entry (its address stays valid for
                    // cached descriptors); name the hash, NOT typeName, which
                    // may already view unmapped memory (§3.6).
                    ASTRA_LOG_WARN("MetaRegistry: meta content for hash " + std::to_string(hash)
                                   + " is bound to a departed module and no remaining binder can rebuild it");
                    return ReleaseOutcome::Retained;
                }
                rebuild = top.build;
                newTop  = top.module;
            }

            // Outside the mutex: user reflection code.
            TypeMeta fresh = rebuild();

            {
                std::unique_lock lock(m_mutex);
                auto it = m_types.Find(hash);
                if (it != m_types.end() && !it->second.binders.empty()
                    && it->second.binders.back().module == newTop
                    && SameIdentity(*it->second.meta, fresh))
                {
                    *it->second.meta = std::move(fresh);
                }
                // else: the top changed while the thunk ran (a re-entrant
                // Bind) -- that binder owns the content now; drop ours. The
                // token compare is NOT ABA-proof against an erase-and-reinstall
                // at this hash during the window, and need not be: `fresh`
                // came from that module's own thunk and is identity-checked,
                // so swapping it into a reinstalled entry of the same identity
                // is still correct.
            }
            return ReleaseOutcome::Rebound;
        }

        // Content refresh for a module whose binder is (still) top: the
        // deferred half of BoundNeedsRebuild, run by the caller after it has
        // dropped its own locks. Identity-checked. Returns false (no change)
        // when the module's binder is not top or the entry is gone.
        bool Rebind(uint64_t hash, ModuleToken module, TypeMeta&& fresh)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end() || it->second.binders.empty())
            {
                return false;
            }
            if (it->second.binders.back().module != module)
            {
                return false;
            }
            if (!SameIdentity(*it->second.meta, fresh))
            {
                RefuseCollision(*it->second.meta, fresh);
                return false;
            }
            *it->second.meta = std::move(fresh);
            return true;
        }

        // ---- Diagnostics (tests, tools) ----
        ASTRA_NODISCARD size_t BinderCount(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            return it != m_types.end() ? it->second.binders.size() : 0;
        }

        ASTRA_NODISCARD uint32_t Refs(uint64_t hash, ModuleToken module) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end()) return 0;
            const size_t idx = FindBinder(it->second, module);
            return idx < it->second.binders.size() ? it->second.binders[idx].refs : 0;
        }

        ASTRA_NODISCARD bool IsPinned(uint64_t hash, ModuleToken module) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end()) return false;
            const size_t idx = FindBinder(it->second, module);
            return idx < it->second.binders.size() && it->second.binders[idx].pinned;
        }

        ASTRA_NODISCARD ModuleToken TopBinder(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            return (it != m_types.end() && !it->second.binders.empty()) ? it->second.binders.back().module : nullptr;
        }

        // Public erase: for module-owned NON-component metas (RegisterMeta
        // teardown). Refuses a component-linked hash -- erasing one would
        // dangle every cached ComponentDescriptor::meta for a live component.
        // TRANSITIONAL: deleted in the next task once ComponentModule releases
        // through the binder stack instead.
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

        // TRANSITIONAL clear-path erase (deleted in the next task with its
        // last caller): removes the entry AND both link rows.
        bool EraseUnchecked(uint64_t hash)
        {
            std::unique_lock lock(m_mutex);
            auto it = m_types.Find(hash);
            if (it == m_types.end())
            {
                return false;
            }
            EraseEntryLocked(it);
            return true;
        }

        /**
         * Gets type metadata by hash.
         * Thread-safe.
         * @param hash Type hash
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        ASTRA_NODISCARD const TypeMeta* Get(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_types.Find(hash);
            if (it != m_types.end())
            {
                return it->second.meta.get();
            }
            return nullptr;
        }

        /**
         * Gets type metadata by type.
         * Thread-safe.
         * @tparam T The type to look up
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        template<typename T>
        ASTRA_NODISCARD const TypeMeta* Get() const
        {
            return Get(TypeID<T>::Hash());
        }

        /**
         * Gets type metadata by name.
         * Thread-safe but slower than hash lookup.
         * @param name Type name
         * @return Pointer to TypeMeta, or nullptr if not registered
         */
        ASTRA_NODISCARD const TypeMeta* GetByName(std::string_view name) const
        {
            std::shared_lock lock(m_mutex);

            for (const auto& [hash, entry] : m_types)
            {
                if (entry.meta->typeName == name)
                {
                    return entry.meta.get();
                }
            }
            return nullptr;
        }

        /**
         * Checks if a type is registered.
         * Thread-safe.
         * @param hash Type hash
         * @return true if registered
         */
        ASTRA_NODISCARD bool IsRegistered(uint64_t hash) const
        {
            std::shared_lock lock(m_mutex);
            return m_types.Find(hash) != m_types.end();
        }

        /**
         * Checks if a type is registered.
         * Thread-safe.
         * @tparam T The type to check
         * @return true if registered
         */
        template<typename T>
        ASTRA_NODISCARD bool IsRegistered() const
        {
            return IsRegistered(TypeID<T>::Hash());
        }

        /**
         * Links a type hash to a ComponentID for ECS integration.
         * Thread-safe. Link rows are context facts (ComponentIDs are
         * context-scoped) and are dropped only when the entry is erased.
         * @param typeHash Type hash
         * @param componentId ComponentID from ComponentRegistry
         */
        void LinkToComponent(uint64_t typeHash, ComponentID componentId)
        {
            std::unique_lock lock(m_mutex);
            m_typeToComponentId[typeHash] = componentId;
            m_componentIdToType[componentId] = typeHash;
        }

        /**
         * Gets the ComponentID for a type hash.
         * Thread-safe.
         * @param typeHash Type hash
         * @return ComponentID, or INVALID_COMPONENT if not linked
         */
        ASTRA_NODISCARD ComponentID GetComponentId(uint64_t typeHash) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_typeToComponentId.Find(typeHash);
            if (it != m_typeToComponentId.end())
            {
                return it->second;
            }
            return INVALID_COMPONENT;
        }

        /**
         * Gets the type hash for a ComponentID.
         * Thread-safe.
         * @param componentId ComponentID
         * @return Type hash, or 0 if not linked
         */
        ASTRA_NODISCARD uint64_t GetTypeHash(ComponentID componentId) const
        {
            std::shared_lock lock(m_mutex);

            auto it = m_componentIdToType.Find(componentId);
            if (it != m_componentIdToType.end())
            {
                return it->second;
            }
            return 0;
        }

        /**
         * Gets type metadata by ComponentID.
         * Thread-safe.
         * @param componentId ComponentID from ComponentRegistry
         * @return Pointer to TypeMeta, or nullptr if not linked
         */
        ASTRA_NODISCARD const TypeMeta* GetByComponentId(ComponentID componentId) const
        {
            uint64_t hash = GetTypeHash(componentId);
            if (hash == 0)
            {
                return nullptr;
            }
            return Get(hash);
        }

        /**
         * Gets the number of registered types.
         * Thread-safe.
         */
        ASTRA_NODISCARD size_t GetRegisteredCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_types.Size();
        }

        /**
         * Iterates over all registered types.
         * Thread-safe. Snapshots the entries under the lock, then releases it
         * before invoking the callback: std::shared_mutex is NOT recursive, so
         * a callback that re-enters any read accessor (Get/GetByName/...) while
         * the shared lock was still held would be UB/deadlock.
         * @tparam Func Callback type
         * @param func Callback invoked for each TypeMeta
         */
        template<typename Func>
        void ForEachType(Func&& func) const
        {
            std::vector<TypeMeta*> snapshot;
            {
                std::shared_lock lock(m_mutex);
                snapshot.reserve(m_types.Size());
                for (const auto& [hash, entry] : m_types)
                {
                    snapshot.push_back(entry.meta.get());
                }
            }

            for (TypeMeta* meta : snapshot)
            {
                func(*meta);
            }
        }

        /**
         * Clears all registrations (entries, binders, link rows).
         * Thread-safe. Use with caution - mainly for testing.
         */
        void Clear()
        {
            std::unique_lock lock(m_mutex);
            m_types.Clear();
            m_typeToComponentId.Clear();
            m_componentIdToType.Clear();
        }

    private:
        struct Entry
        {
            std::unique_ptr<TypeMeta>  meta;      // address-stable for the entry's life
            SmallVector<MetaBinder, 2> binders;   // back() == top == whose closures `meta` carries
        };

        // All helpers below require m_mutex to be held (unique for mutators).

        Entry& InstallEntryLocked(uint64_t hash, TypeMeta&& fresh)
        {
            Entry& e = m_types[hash];
            e.meta = std::make_unique<TypeMeta>(std::move(fresh));
            return e;
        }

        void EraseEntryLocked(typename FlatMap<uint64_t, Entry>::iterator it)
        {
            const uint64_t hash = it->first;
            if (auto link = m_typeToComponentId.Find(hash); link != m_typeToComponentId.end())
            {
                m_componentIdToType.Erase(link->second);
                m_typeToComponentId.Erase(link);
            }
            m_types.Erase(it);
        }

        static MetaBinder MakeBinder(Detail::ModuleIdentity who, MetaBuildFn build) noexcept
        {
            return MetaBinder{ who.token, build, 0u, who.residency == ModuleResidency::Resident };
        }

        // Index of `module`'s binder in e.binders, or e.binders.size() if none.
        static size_t FindBinder(const Entry& e, ModuleToken module) noexcept
        {
            for (size_t i = 0; i < e.binders.size(); ++i)
            {
                if (e.binders[i].module == module) return i;
            }
            return e.binders.size();
        }

        // The identity fields a TypeMeta carries: matching => the same type
        // (re-registered / rebuilt), anything else => a real type-hash
        // collision of two distinct types.
        static bool SameIdentity(const TypeMeta& a, const TypeMeta& b) noexcept
        {
            return a.size == b.size
                && a.alignment == b.alignment
                && a.isTrivial == b.isTrivial
                && a.typeName == b.typeName;
        }

        static void RefuseCollision(const TypeMeta& existing, const TypeMeta& incoming)
        {
            std::string msg = "MetaRegistry: type-identity collision -- incoming type '";
            msg.append(incoming.typeName);
            msg += "' shares the type-hash of already-registered '";
            msg.append(existing.typeName);
            msg += "'. The second type is refused (nullptr). Give types a unique unqualified name.";
            ASTRA_LOG_ERROR(msg);
            ASTRA_ENSURE_ALWAYS(false, "MetaRegistry type-identity collision");
        }

        mutable std::shared_mutex m_mutex;
        FlatMap<uint64_t, Entry> m_types;
        FlatMap<uint64_t, ComponentID> m_typeToComponentId;
        FlatMap<ComponentID, uint64_t> m_componentIdToType;
    };
```

Notes for the implementer:
- `FlatMap::Erase(iterator)` exists (`FlatMap.hpp:629`); `Erase(key)` returns a count.
- `SmallVector` iterators are plain `T*`, so `e.binders.begin() + idx` and `e.binders.end() - 1` are valid `const_iterator` arguments for `erase`/`insert`.
- `std::to_string` needs `<string>` (already included).
- The class previously had no `<SmallVector.hpp>` include; keep the include list alphabetical as shown.

- [ ] **Step 4: Build and run the new tests**

Run: Debug build, then `AstraTest.exe --gtest_filter=MetaBinder.*`
Expected: 12 PASSED. (Expected `[critical]` ENSURE lines in the output for the refusal tests are normal — they are the observable refusal, not failures.)

- [ ] **Step 5: Run the whole suite**

Run: `AstraTest.exe --gtest_brief=1`
Expected: 868 tests, all PASSED (856 + 12). `MetaRebind.*`, `ComponentModule.*`, `Reflection*` all still pass — `Erase`/`EraseUnchecked` are still present and `RebindInPlace` is unchanged in behaviour.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Reflection/MetaRegistry.hpp tests/Reflection/MetaBinderTest.cpp
git commit -m "feat(reflection): MetaRegistry binder stack -- Bind/Acquire/Release/Rebind, content bound to the top binder"
```

---

### Task 3: Registry slot accounting + ComponentModule rewiring (delete Erase paths)

**Spec:** §3.4, §3.5 flows 2–7, §3.6 "reload shapes", §3.7 (`InstallOwned` also drops its now-unused `MetaBuildFn` parameter — the stack owns thunks; Task 6 records this in the spec).

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` — `RegisterComponent` (:42-65), `UnregisterModuleRange` (:136-214), module plumbing (`ShadowEntry`/`MetaRestore`/`InstallOwned`/`ReleaseModule`/`RestoreOrClearSlot`, :386-547), `RegisterComponentImpl` (:570-606), members (:740-754)
- Modify: `include/Astra/Component/ComponentModule.hpp` (whole file)
- Modify: `include/Astra/Reflection/MetaRegistry.hpp` — delete `Erase` and `EraseUnchecked`
- Modify: `tests/Component/ComponentModuleTest.cpp` — `InstallOwnedRefusesOverAlignedDescriptor` (:386-399), `InstallOwnedRefusesInvalidId` (:503-516)
- Modify: `tests/Reflection/MetaRebindTest.cpp` — delete `EraseGuardsComponentLinkedHashes` (:59-78)

**Interfaces:**
- Consumes: everything Task 2 produces; `Detail::CurrentModuleIdentity()`.
- Produces (on `Astra::ComponentRegistry`):
  - `enum class Astra::InstallResult : uint8_t { Refused, Installed, Overrode, Replaced }`
  - `struct Astra::ComponentRegistry::MetaRelease { uint64_t hash; ModuleToken module; }`
  - `InstallResult InstallOwned(ComponentID id, uint32_t owner, ComponentDescriptor desc, ModuleToken module)`
  - `SmallVector<MetaRelease, 4> ReleaseModule(uint32_t owner)`
  - `size_t UnregisterModuleRange(const void* base, size_t size)` (signature unchanged)
  - `ShadowEntry { uint32_t owner; ComponentDescriptor desc; ModuleToken module; }`
  - `ComponentModule` public surface unchanged (`Open`, `Register`, `RegisterMeta`, `Reset`, move ops, `operator bool`).

- [ ] **Step 1: Update the two `InstallOwned` tests to the new return type (they fail to compile until Step 3)**

In `tests/Component/ComponentModuleTest.cpp`, in `InstallOwnedRefusesOverAlignedDescriptor` replace

```cpp
    EXPECT_FALSE(creg->InstallOwned(0, 1u, desc, nullptr));
```
with
```cpp
    EXPECT_EQ(creg->InstallOwned(0, 1u, desc, Astra::Detail::CurrentModuleIdentity().token), Astra::InstallResult::Refused);
```

and in `InstallOwnedRefusesInvalidId` replace

```cpp
    EXPECT_FALSE(creg->InstallOwned(Astra::INVALID_COMPONENT, 1u, desc, nullptr));
    EXPECT_FALSE(creg->InstallOwned(static_cast<Astra::ComponentID>(Astra::MAX_COMPONENTS), 1u, desc, nullptr));
```
with
```cpp
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    EXPECT_EQ(creg->InstallOwned(Astra::INVALID_COMPONENT, 1u, desc, me), Astra::InstallResult::Refused);
    EXPECT_EQ(creg->InstallOwned(static_cast<Astra::ComponentID>(Astra::MAX_COMPONENTS), 1u, desc, me), Astra::InstallResult::Refused);
```

Delete `TEST(MetaRebind, EraseGuardsComponentLinkedHashes)` (lines 59-78 of `tests/Reflection/MetaRebindTest.cpp`) entirely; its replacement is `MetaBinder.ComponentLinkedHashWithLiveRefsIsHeldNotErased` from Task 2.

- [ ] **Step 2: Build to confirm the compile failure**

Run: Debug build.
Expected: FAILS — `'Astra::InstallResult': is not a class or namespace name` (ComponentModuleTest.cpp).

- [ ] **Step 3: Rewrite the registry's module plumbing in `ComponentRegistry.hpp`**

3a. Replace `RegisterComponent<T>` (:42-65) with:

```cpp
        // A type whose identity collided with an already-registered type (see
        // TypeContext::GetOrAssignComponentID) resolves to INVALID_COMPONENT and
        // is refused here -- no descriptor is written for it.
        template<Component T>
        void RegisterComponent()
        {
            // Birth-context affinity (all-config): a caller whose ambient context
            // differs from the one this registry was constructed under would mint
            // ids from the WRONG counter -- the silent cross-module aliasing
            // class. Checked BEFORE TypeID<T>::Value() so the mismatched module's
            // per-type id cache is never seeded from the wrong context.
            if (!ASTRA_ENSURE_ALWAYS(GetTypeContext() == m_birthContext,
                    "ComponentRegistry: RegisterComponent from a module whose TypeContext "
                    "differs from this registry's birth context -- call Astra::SetTypeContext "
                    "in the calling module first"))
                return;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY  // guard before indexing m_registered
                return;
            if (m_registered[id].load(std::memory_order_acquire))
                return;                                // warm path: lock-free
            ModuleToken rebuildAs = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                if (m_registered[id].load(std::memory_order_relaxed))
                    return;                            // double-check under lock
                rebuildAs = RegisterComponentImpl<T>(id);   // may refuse (over-aligned) -- that's fine
                m_registered[id].store(true, std::memory_order_release);  // "attempt resolved for id"
            }
            if (rebuildAs != nullptr)
            {
                // This module's binder just became the TOP binder for a type
                // some other module drained: the content must carry this
                // module's closures. Built here, OUTSIDE the registration lock
                // (user reflection code), and swapped only if still top
                // (spec 2026-09-09 §3.4). The token is the one the bind was
                // made under, threaded out rather than re-read.
                TypeMeta fresh = Detail::BuildMetaThunk<T>();
                MetaRegistry::Instance().Rebind(TypeID<T>::Hash(), rebuildAs, std::move(fresh));
            }
        }
```

3b. In `UnregisterModuleRange` (:136-214): replace the doc paragraph that begins "This function has no captured TypeContext the way ComponentModule does" (:126-133) with:

```cpp
        // Meta work runs through the ambient MetaRegistry::Instance() AFTER the
        // registration lock is released. That is correct here specifically
        // because the caller of a range purge is, by construction, the HOST --
        // the module that installed the shared TypeContext -- and never the
        // plugin being unloaded. Each dropped entry is released against ITS
        // OWN module's binder (the token recorded at registration), so one
        // purge that hits several modules' entries releases each correctly
        // (spec 2026-09-09 §3.5 flow 5).
```

and replace the body from `SmallVector<MetaRestore, 4> metaWork;` through the closing `return dropped;` with:

```cpp
            SmallVector<MetaRelease, 4> metaWork;
            size_t dropped = 0;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                for (size_t id = 0; id < MAX_COMPONENTS; ++id)
                {
                    // Strip in-range SHADOW entries for this id first, regardless
                    // of whether the live entry is owned by this range.
                    if (auto it = m_shadow.Find(static_cast<ComponentID>(id)); it != m_shadow.end())
                    {
                        auto& list = it->second;
                        for (size_t i = list.size(); i-- > 0;)
                        {
                            if (isOwned(list[i].desc))
                            {
                                if (list[i].desc.meta)
                                    metaWork.push_back(MetaRelease{list[i].desc.hash, list[i].module});
                                list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                                ++dropped;
                            }
                        }
                        if (list.empty()) m_shadow.Erase(it);
                    }

                    if (!m_present.Test(id) || !isOwned(m_components[id]))
                        continue;

                    // Live entry is in range: replace it via the same
                    // restore-newest-shadow-or-clear path ReleaseModule uses
                    // (rather than unconditionally blanking). m_owner[id] and
                    // m_metaModule[id] are updated inside RestoreOrClearSlot.
                    RestoreOrClearSlot(static_cast<ComponentID>(id), metaWork);
                    ++dropped;
                }
            }

            for (const auto& w : metaWork)         // outside the lock: may run user reflection code
            {
                MetaRegistry::Instance().Release(w.hash, w.module);
            }

            return dropped;
```

3c. Replace the block from `// Snapshot of a slot's previously-live entry` through the end of `RestoreOrClearSlot` (:386-547) with:

```cpp
        // Snapshot of a slot's previously-live entry, taken when a different
        // owner overrides it (InstallOwned) so ReleaseModule can restore it.
        // `module` is the token the entry's meta ref was acquired under.
        struct ShadowEntry { uint32_t owner; ComponentDescriptor desc; ModuleToken module; };
        // Post-lock meta work ReleaseModule / UnregisterModuleRange hand back
        // to their caller: one Release per departing live-or-shadowed entry
        // that held a meta ref (spec 2026-09-09 §3.4). The meta stack decides
        // what a release means (held / rebound / erased); the registry only
        // reports the departure.
        struct MetaRelease { uint64_t hash; ModuleToken module; };

        // ComponentModule plumbing. Entry contract: desc.name must be a live
        // NUL-terminated const char* for the duration of this call -- it is
        // re-pointed to registry-owned storage under the lock before this
        // function returns (see the by-value-desc paragraph below), so the
        // caller's storage need not outlive the call, only span it.
        // Full semantics:
        //   1. id >= MAX_COMPONENTS -> Refused.
        //   2. Slot empty            -> Installed (plain install).
        //   3. Live owner == owner   -> Replaced: in-place replace of the live
        //                               entry (no shadow: this module is just
        //                               re-registering/rebinding its own type).
        //   4. Live owner != owner   -> Overrode: the CURRENT live entry
        //                               (owner, desc, module) is pushed onto
        //                               this id's shadow stack before the new
        //                               entry overwrites the slot, so a later
        //                               ReleaseModule can restore it.
        // The caller acquires a meta ref on Installed and Overrode only; on
        // Replaced the ref already exists and the image is the same.
        // By-value desc: desc.name is caller-owned temporary storage (RegisterOne's
        // stack std::string's c_str()) and is copied into registry-owned storage
        // here, under the lock, before it is written into any slot. A pushed
        // SHADOW copy needs no such re-storage: it is a copy of the CURRENT live
        // entry, whose desc.name already points at registry-owned m_componentNames
        // storage from that entry's own prior InstallOwned call.
        // Returns Refused when the id is invalid, or when the descriptor is
        // over-aligned.
        InstallResult InstallOwned(ComponentID id, uint32_t owner, ComponentDescriptor desc, ModuleToken module)
        {
            // Birth-context affinity (all-config) -- see RegisterComponent.
            if (!ASTRA_ENSURE_ALWAYS(GetTypeContext() == m_birthContext,
                    "ComponentRegistry: InstallOwned from a foreign-context module"))
                return InstallResult::Refused;
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                return InstallResult::Refused;
            }
            // Defense in depth: this is a PUBLIC entry point, so it repeats
            // MakeDescriptor's over-alignment refusal instead of trusting every
            // caller to re-test it (RegisterComponentImpl and
            // ComponentModule::RegisterOne both do, but a hand-built descriptor
            // need not have come through MakeDescriptor at all). Chunk storage
            // can only honor alignments up to CACHE_LINE_SIZE; installing a
            // descriptor above it would hand back misaligned component memory.
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
            {
                return InstallResult::Refused;
            }

            // The override notice is BUILT under the lock (DescribeOverride
            // reads m_moduleNames and the live slot's name) but EMITTED after
            // the lock releases: ASTRA_LOG_INFO reaches a user-installed
            // LogSink, and user code must never run under m_registrationMutex.
            std::string overrideNotice;
            InstallResult result = InstallResult::Installed;
            {
                std::lock_guard<std::mutex> lock(m_registrationMutex);
                // std::string_view from a null pointer is UB, and a hand-built
                // descriptor may legitimately arrive with desc.name unset.
                desc.name = StoreComponentName(desc.name ? desc.name : "");

                if (m_present.Test(id))
                {
                    if (m_owner[id] != owner)
                    {
                        overrideNotice = DescribeOverride(owner, m_owner[id], m_components[id].name);
                        m_shadow[id].push_back(ShadowEntry{m_owner[id], m_components[id], m_metaModule[id]});
                        result = InstallResult::Overrode;
                    }
                    else
                    {
                        result = InstallResult::Replaced;
                    }
                }

                m_components[id] = desc;
                m_present.Set(id);
                m_hashToID[desc.hash] = id;
                m_owner[id] = owner;
                m_metaModule[id] = module;
                m_registered[id].store(true, std::memory_order_release);
            }
            if (!overrideNotice.empty())
            {
                ASTRA_LOG_INFO(overrideNotice);
            }
            return result;
        }

        // Per-owner cleanup for module unload (ComponentModule::Reset). Under
        // m_registrationMutex: strips this owner's SHADOWED entries wherever
        // they sit in the stack, then -- for every id where this owner's entry
        // is LIVE -- either restores the newest remaining shadow entry or clears
        // the slot to empty. One MetaRelease per departing entry that held a
        // meta ref is returned rather than performed here so the caller can
        // run it OUTSIDE all locks (MetaRegistry::Release may run a MetaBuildFn
        // thunk -- user reflection code).
        SmallVector<MetaRelease, 4> ReleaseModule(uint32_t owner)
        {
            SmallVector<MetaRelease, 4> metaWork;
            std::lock_guard<std::mutex> lock(m_registrationMutex);
            for (size_t id = 0; id < MAX_COMPONENTS; ++id)
            {
                // Drop this owner's SHADOWED entries wherever they sit.
                if (auto it = m_shadow.Find(static_cast<ComponentID>(id)); it != m_shadow.end())
                {
                    auto& list = it->second;
                    for (size_t i = list.size(); i-- > 0;)
                    {
                        if (list[i].owner == owner)
                        {
                            if (list[i].desc.meta)
                                metaWork.push_back(MetaRelease{list[i].desc.hash, list[i].module});
                            list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                        }
                    }
                    if (list.empty()) m_shadow.Erase(it);
                }
                if (!m_present.Test(id) || m_owner[id] != owner)
                    continue;
                // This owner's entry is LIVE: restore newest shadow, or clear.
                RestoreOrClearSlot(static_cast<ComponentID>(id), metaWork);
            }
            return metaWork;
        }

    private:
        // Replaces the LIVE entry at `id` with the newest remaining shadow
        // entry, or clears the slot to empty if none remain -- recording the
        // DEPARTING entry's meta release in `metaWork` rather than performing
        // it here, so callers can run it OUTSIDE the registration lock. Must
        // be called while m_registrationMutex is already held. Shared by
        // ReleaseModule (owner-scoped release) and UnregisterModuleRange
        // (address-range purge). No meta rebind happens here: the survivor's
        // module is already a binder on the meta stack, and Release rebinds
        // to it if the departing binder was on top (spec 2026-09-09 §3.4).
        // See UnregisterModuleRange's doc comment above for the RAII-discipline
        // / double-fault contract a restored shadow entry depends on.
        void RestoreOrClearSlot(ComponentID id, SmallVector<MetaRelease, 4>& metaWork)
        {
            if (m_components[id].meta)
                metaWork.push_back(MetaRelease{m_components[id].hash, m_metaModule[id]});

            if (auto it = m_shadow.Find(id); it != m_shadow.end() && !it->second.empty())
            {
                ShadowEntry restored = std::move(it->second.back());
                it->second.pop_back();
                if (it->second.empty()) m_shadow.Erase(it);
                m_components[id]  = restored.desc;        // same address, new contents
                m_owner[id]       = restored.owner;
                m_metaModule[id]  = restored.module;
            }
            else
            {
                m_components[id] = ComponentDescriptor{};
                m_present.Reset(id);
                m_owner[id] = 0;
                m_metaModule[id] = nullptr;
                m_registered[id].store(false, std::memory_order_release);
            }
        }
```

3d. Replace `RegisterComponentImpl<T>` (:570-606) with:

```cpp
        // Returns the module token the caller must Rebind under after
        // dropping the registration lock (BoundNeedsRebuild -- this module's
        // binder became the top binder for a type some other module drained),
        // or nullptr when no rebuild is needed or the type was refused.
        template<Component T>
        ModuleToken RegisterComponentImpl(ComponentID id)
        {
            ASTRA_ASSERT(id < MAX_COMPONENTS,
                         "Component ID space exhausted (MAX_COMPONENTS); raise ASTRA_MAX_COMPONENTS");
            if (id >= MAX_COMPONENTS) ASTRA_UNLIKELY
            {
                // Refuse registration so failure is observable (descriptor
                // lookup returns nullptr; Registry::AddComponent returns
                // nullptr) instead of silently corrupting ComponentMask bits.
                return nullptr;
            }

            // Over-aligned refusal BEFORE any meta side effect (mirrors
            // ComponentModule's hoisted gate): a refused type must push no
            // binder. MakeDescriptor repeats the test as defense in depth.
            constexpr size_t descriptorAlignment = std::is_empty_v<T> ? size_t(1) : alignof(T);
            if constexpr (descriptorAlignment > CACHE_LINE_SIZE)
            {
                return nullptr;
            }
            else
            {
                // Anonymous registration binds under THIS module's identity
                // (the template is instantiated in the caller's module, so
                // CurrentModuleIdentity() is the caller's copy) and takes one
                // ref for this slot (spec 2026-09-09 §3.4). No thunk runs here:
                // we are under m_registrationMutex.
                const Detail::ModuleIdentity who = Detail::CurrentModuleIdentity();
                const MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;
                const TypeMeta* meta = nullptr;
                bool needsRebuild = false;
                {
                    // A Refused bind (type reflected in some other module but
                    // never drained into this context, and no factory here)
                    // registers the component UNREFLECTED -- meta null, no link,
                    // no ref -- exactly what a null Get<T>() produced before.
                    const BindResult bound = MetaRegistry::Instance().Bind(TypeID<T>::Hash(), who, thunk, nullptr);
                    if (bound.outcome != BindOutcome::Refused)
                    {
                        meta = bound.meta;
                        needsRebuild = (bound.outcome == BindOutcome::BoundNeedsRebuild);
                    }
                }

                ComponentDescriptor desc = MakeDescriptor<T>(id, meta);
                if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY  // MakeDescriptor refused (unreachable: gated above)
                {
                    return nullptr;
                }

                // Pointer-stable c_str() storage, reused by content -- see
                // StoreComponentName (a rebuilt/reloaded type's name string is
                // identical to what's already stored, so this does not grow).
                desc.name = StoreComponentName(TypeID<T>::Name());

                // Link MetaRegistry to ComponentID for reverse lookup and take
                // this slot's ref on the binder Bind just ensured.
                if (desc.meta)
                {
                    MetaRegistry::Instance().LinkToComponent(desc.hash, id);
                    MetaRegistry::Instance().Acquire(desc.hash, who.token);
                }

                // Directly indexed; the array slot is pointer-stable for life.
                m_components[id] = desc;
                m_present.Set(id);
                m_hashToID[desc.hash] = id;
                m_owner[id] = 0;  // anonymous: this path never goes through a ComponentModule
                m_metaModule[id] = who.token;
                return needsRebuild ? who.token : nullptr;
            }
        }
```

3e. In the members block (:740-754) replace

```cpp
        // ComponentModule plumbing. m_owner tracks who owns the LIVE entry at
        // each id (0 = anonymous, i.e. registered via RegisterComponent, never
        // through a module); m_metaThunk retains a per-slot rebuild callback
        // (null when the type isn't reflected) so a later hot-reload can
        // regenerate that slot's TypeMeta. Both are written only under
        // m_registrationMutex (RegisterComponentImpl / InstallOwned).
        uint32_t m_owner[MAX_COMPONENTS] = {};
        MetaBuildFn m_metaThunk[MAX_COMPONENTS] = {};
```
with
```cpp
        // ComponentModule plumbing. m_owner tracks who owns the LIVE entry at
        // each id (0 = anonymous, i.e. registered via RegisterComponent, never
        // through a module); m_metaModule records the module token the LIVE
        // entry's meta ref was acquired under, so the departing entry can be
        // released against the right binder (spec 2026-09-09 §3.4). Both are
        // written only under m_registrationMutex (RegisterComponentImpl /
        // InstallOwned / RestoreOrClearSlot). NOTE: the registry destructor
        // releases NOTHING -- anonymous entries outlive their registry exactly
        // as before (registry-less GetMeta readers depend on it); over-holding
        // is the safe direction (spec §3.6 c).
        uint32_t m_owner[MAX_COMPONENTS] = {};
        ModuleToken m_metaModule[MAX_COMPONENTS] = {};
```

3f. Add, immediately before `class ComponentRegistry` (:25), the result enum:

```cpp
    // Outcome of ComponentRegistry::InstallOwned (spec 2026-09-09 §3.4).
    enum class InstallResult : uint8_t { Refused, Installed, Overrode, Replaced };

```

3g. Remove every remaining reference to `m_metaThunk` and `MetaRestore` in the file (grep: `grep -n "m_metaThunk\|MetaRestore\|buildMeta" include/Astra/Component/ComponentRegistry.hpp` must return nothing).

- [ ] **Step 4: Rewire `ComponentModule.hpp`**

Replace the whole file with:

```cpp
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "../Container/SmallVector.hpp"
#include "../Core/ModuleIdentity.hpp"
#include "../Core/TypeContext.hpp"
#include "../Core/TypeID.hpp"
#include "ComponentRegistry.hpp"

namespace Astra
{
    // RAII owner of a module's component registrations (spec 2026-08-09;
    // meta lifetime per spec 2026-09-09). Open() captures the EXPLICITLY
    // INSTALLED TypeContext and this module's identity -- all ids and metas
    // route through the captured context, never ambient per-module state.
    // Register<Ts...>() must be instantiated in the module whose code the
    // descriptors should point into -- the compiler stamps each descriptor's
    // function pointers with THIS translation unit's code, so the
    // instantiating module is the module the resulting registration is tied
    // to (that is the whole point).
    //
    // Ownership contract (mandatory, not advisory):
    //   - HEAP-HELD in every plugin/module that uses it (e.g. a file-scope
    //     std::optional<ComponentModule>), and reset EXPLICITLY from that
    //     module's own Shutdown entry point.
    //   - NEVER a plugin-side static/global object. A DLL static's
    //     destructor runs *during* FreeLibrary at DLL_PROCESS_DETACH, under
    //     the loader lock -- but ~ComponentModule() takes the registry's
    //     registration mutex, may invoke a MetaBuildFn thunk (arbitrary user
    //     reflection code), and may drop the last shared_ptr to the
    //     registry. None of that is safe under the loader lock. Heap-held +
    //     explicit Shutdown-time Reset() keeps all of it off the loader lock.
    //   - "Register only what you own": Register<Ts...>() should list only
    //     the types this module's code actually implements -- registering a
    //     type you don't own just to keep it alive defeats RAII ownership
    //     (Reset() would release it out from under its real owner on unload).
    //   - UnregisterModuleRange remains the fallback net for modules that
    //     never adopted this RAII path (or that forgot Shutdown cleanup) --
    //     it is a safety valve, not a substitute for the contract above.
    //
    // Several registries per context are supported: each registration holds
    // its own ref on the type's meta binder, and a meta is erased only when
    // no registry, no handle, and no resident module holds it any more
    // (spec 2026-09-09 §3.3).
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
            // Birth-context affinity: the installed slot must be the context
            // the target registry's ids are minted under, or every id this
            // module registers would come from the wrong counter.
            if (!ASTRA_ENSURE_ALWAYS(installed == registry->GetBirthContext(),
                    "ComponentModule::Open: this module's installed TypeContext is not the "
                    "one the target registry was constructed under -- ids would alias"))
            {
                return mod;   // empty handle: observable refusal
            }
            mod.m_registry = std::move(registry);
            mod.m_context  = installed;
            mod.m_identity = Detail::CurrentModuleIdentity();   // token + residency of THIS image
            mod.m_moduleId = mod.m_registry->OpenModuleId(name);
            return mod;
        }

        template<Component... Ts>
        void Register()
        {
            if (!*this) return;
            (RegisterOne<Ts>(), ...);
        }

        // Module-owned NON-component reflected types. T is never registered as
        // a component here -- no ComponentID is consumed, no descriptor phase,
        // no LinkToComponent. The module still owns the meta's lifetime: the
        // bind points the closures into THIS image and takes one ref; Reset()
        // releases it. Register<T> and RegisterMeta<T> for the SAME T on one
        // handle is fine: both are refs on the same binder and both are
        // released at teardown (the slot's by ReleaseModule, this one by
        // ReleaseOwnedMetas).
        template<typename... Ts>
        void RegisterMeta()
        {
            if (!*this) return;
            (RegisterOneMeta<Ts>(), ...);
        }

        void Reset();
        ~ComponentModule() { Reset(); }

        ComponentModule(ComponentModule&& other) noexcept { *this = std::move(other); }
        ComponentModule& operator=(ComponentModule&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_registry   = std::move(other.m_registry);
                m_context    = other.m_context;
                m_identity   = other.m_identity;
                m_moduleId   = other.m_moduleId;
                m_ownedMetas = std::move(other.m_ownedMetas);
                other.m_context  = nullptr;
                other.m_moduleId = 0;
                other.m_ownedMetas.clear();
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
        // The descriptor alignment MakeDescriptor will compute for T. Mirrors
        // its expression EXACTLY, empty-type case included, so the module path
        // refuses precisely the set of types the anonymous path refuses --
        // no more, no less (an over-aligned EMPTY tag is storage-free and is
        // accepted by both).
        template<Component T>
        static constexpr size_t DescriptorAlignmentV = std::is_empty_v<T> ? size_t(1) : alignof(T);

        // Over-aligned refusal, HOISTED above EVERY side effect. Alignment is a
        // purely compile-time property -- it needs neither a ComponentID nor a
        // TypeMeta -- so this MUST precede Phase A. A type that is both
        // ASTRA_REFLECT_TYPE'd AND over-aligned would otherwise push a binder
        // and take a ref it could never release (the slot is never installed,
        // so ReleaseModule never reaches it). Refusing up here means a refused
        // type mints no id, binds no meta, and installs no descriptor -- it is
        // invisible to the entire registry.
        template<Component T>
        void RegisterOne()
        {
            if constexpr (DescriptorAlignmentV<T> <= CACHE_LINE_SIZE)
            {
                RegisterOneChecked<T>();
            }
            // else: over-aligned -> refused with zero side effects.
        }

        template<Component T>
        void RegisterOneChecked()
        {
            // Explicit-context id mint: TypeID<T>::Hash()/Name() are pure
            // compile-time values; only the id assignment touches context state.
            const ComponentID id = m_context->GetOrAssignComponentID(
                TypeID<T>::Hash(), TypeID<T>::Name(), MakeTypeIdentity<T>());
            if (id == INVALID_COMPONENT || id >= MAX_COMPONENTS)
                return;                                    // refused type: never owned

            MetaBuildFn thunk = Detail::MetaFactory<T>::fn ? &Detail::BuildMetaThunk<T> : nullptr;

            // Not atomic across two modules racing the same type -- benign:
            // reload registration is host-serialized by contract.
            //
            // LOCK DISCIPLINE: no lock NESTING occurs here. Phase A takes only
            // the MetaRegistry mutex (inside Bind/LinkToComponent); phase B
            // takes only the registry's registration mutex (inside
            // InstallOwned); phase C takes only the MetaRegistry mutex again.
            // They are held strictly sequentially, never simultaneously. The
            // thunk itself (arbitrary user reflection code) runs with NO lock
            // held.

            // ---- Phase A: meta bind (no registry lock) ----------------------
            const TypeMeta* meta = nullptr;
            if (thunk)
            {
                TypeMeta fresh = thunk();
                // Bind installs when the entry is absent (unload-before-load
                // reload: the previous generation's last release erased it),
                // swaps content to `fresh` when this module's binder is top,
                // and leaves content alone when another module is above us.
                // Either way the returned pointer is the address the descriptor
                // must cache (spec 2026-09-09 §3.5 flow 3).
                const BindResult bound = m_context->Meta().Bind(TypeID<T>::Hash(), m_identity, thunk, &fresh);
                if (bound.outcome == BindOutcome::Refused)
                {
                    // Identity-collision refusal (Bind already logged + ENSUREd).
                    // The type's identity is contested, so do NOT install a
                    // descriptor claiming a meta we could not bind: treat it
                    // exactly like a refused id -- never owned.
                    return;
                }
                meta = bound.meta;
                m_context->Meta().LinkToComponent(TypeID<T>::Hash(), id);
            }
            // An UNREFLECTED type (null thunk) still registers normally with
            // meta == nullptr; only a non-null thunk whose bind REFUSED aborts.

            // ---- Phase B: descriptor + slot install --------------------------
            ComponentDescriptor desc = ComponentRegistry::MakeDescriptor<T>(id, meta);
            // MakeDescriptor refuses an over-aligned type by early-returning a
            // zeroed descriptor whose only valid field is `alignment`; callers
            // MUST re-test it. UNREACHABLE via RegisterOne (the hoisted
            // compile-time guard already refused such a T before Phase A) --
            // kept as defense in depth so this function stays correct on its
            // own terms.
            if (desc.alignment > CACHE_LINE_SIZE) ASTRA_UNLIKELY
                return;                                    // over-aligned: never owned

            // MakeDescriptor is static (no ComponentRegistry instance), so it
            // cannot reach m_componentNames and leaves desc.name unset. This
            // module owns TypeID<T>::Name() -- a slice of the compiler's
            // __FUNCSIG__/__PRETTY_FUNCTION__ literal, not independently
            // NUL-terminated -- so copy it into a NUL-terminated buffer that
            // outlives the synchronous InstallOwned call below; InstallOwned
            // re-copies the bytes into the registry's own storage before
            // returning.
            const std::string nameStorage(TypeID<T>::Name());
            desc.name = nameStorage.c_str();

            const InstallResult installed = m_registry->InstallOwned(id, m_moduleId, desc, m_identity.token);

            // ---- Phase C: ref accounting ------------------------------------
            // A new slot or an override push holds one ref on this module's
            // binder; a same-owner in-place replace already holds it.
            if (meta && (installed == InstallResult::Installed || installed == InstallResult::Overrode))
            {
                m_context->Meta().Acquire(TypeID<T>::Hash(), m_identity.token);
            }
        }

        // Mirrors RegisterOne's Phase A ONLY -- T is not a component, so there
        // is no id mint, no descriptor, no InstallOwned/LinkToComponent.
        // Requires T reflected in THIS module (Detail::MetaFactory<T>::fn);
        // otherwise refuses observably and skips, matching RegisterOne's
        // treat-refusal-as-safe-no-op contract.
        template<typename T>
        void RegisterOneMeta()
        {
            if (!*this) return;
            if (!Detail::MetaFactory<T>::fn)
            {
                ASTRA_ENSURE_ALWAYS(false, "RegisterMeta<T> requires T reflected in this module");
                return;
            }

            MetaBuildFn thunk = &Detail::BuildMetaThunk<T>;   // built outside locks

            TypeMeta fresh = thunk();
            // Record ownership ONLY if the bind and the acquire both took. A
            // refused bind (identity collision) left the INCUMBENT meta in
            // place; recording the hash anyway would make teardown release a
            // ref this handle never held.
            if (m_context->Meta().Bind(TypeID<T>::Hash(), m_identity, thunk, &fresh).outcome != BindOutcome::Refused
                && m_context->Meta().Acquire(TypeID<T>::Hash(), m_identity.token))
            {
                m_ownedMetas.push_back(TypeID<T>::Hash());
            }
        }

        // Releases every ref this handle took via RegisterMeta. Called from
        // Reset() BEFORE the registry/context members are released.
        void ReleaseOwnedMetas()
        {
            for (uint64_t hash : m_ownedMetas)
            {
                m_context->Meta().Release(hash, m_identity.token);
            }
            m_ownedMetas.clear();
        }

        std::shared_ptr<ComponentRegistry> m_registry;
        TypeContext* m_context = nullptr;
        Detail::ModuleIdentity m_identity{};
        uint32_t m_moduleId = 0;
        SmallVector<uint64_t, 4> m_ownedMetas;
    };

    // Full unload semantics. A no-op on a moved-from or already-reset handle
    // (operator bool guards the sweep). ReleaseModule does the locked slot
    // bookkeeping and hands back one release per departing entry; those
    // releases run here, OUTSIDE any registry lock, because MetaRegistry::
    // Release may run a MetaBuildFn thunk (user reflection code) to rebind
    // the meta to whichever module survives on top. If this module still
    // holds refs from another registry, its binder stays and nothing
    // rebinds; if its last ref goes, the stack rebinds to the survivor or --
    // when nothing else holds the type -- erases the meta (spec 2026-09-09
    // §3.5 flow 4).
    inline void ComponentModule::Reset()
    {
        if (*this)
        {
            auto work = m_registry->ReleaseModule(m_moduleId);
            for (const auto& w : work)        // outside all locks: may run user reflect code
            {
                m_context->Meta().Release(w.hash, w.module);
            }
            // Module-owned non-component metas: release BEFORE dropping
            // m_context -- ReleaseOwnedMetas needs it valid.
            ReleaseOwnedMetas();
        }
        m_registry.reset();
        m_context = nullptr;
        m_moduleId = 0;
    }
}
```

- [ ] **Step 5: Delete the transitional erase APIs from `MetaRegistry.hpp`**

Remove the `Erase(uint64_t)` and `EraseUnchecked(uint64_t)` member functions (the two blocks marked TRANSITIONAL in Task 2). Then confirm no caller remains:

Run: `grep -rn "EraseUnchecked\|\.Erase(hash)\|Meta().Erase(" include tests`
Expected: no output.

- [ ] **Step 6: Build and run the affected suites**

Run: Debug build, then `AstraTest.exe --gtest_filter=ComponentModule*:MetaRebind.*:MetaBinder.*:Reflection*:ComponentRegistry*:TypeContext*`
Expected: all PASSED. In particular these existing tests must pass unchanged, and here is why under the new rule (the implementer should check each if any fails):
- `MetaRebindsAcrossPushAndPop`: anonymous register binds this module (top) and acquires; the override is the SAME token in-binary, so `InstallOwned` reports `Overrode`, refs go to 2; handle reset releases to 1 -> `Held`; meta stays, address stable, link intact.
- `MetaErasedWhenSlotPopsToEmpty`: refs 1 -> 0 -> binder removed -> stack empty -> `Erased`; the next `Register` hits the absent path and `Bind` installs from `fresh`; the reloaded descriptor caches the new address.
- `RegisterMetaAdoptsAndErasesOnDestruction`: bind + acquire (1) -> release -> `Erased`.
- `RangePurgeStripsShadowsAndRestoresSurvivor`: both entries released; `Position` is unreflected so no meta work at all.
- `ReloadLoadBeforeUnload` / `ReloadUnloadBeforeLoad` / `OverrideRestoresPreviousOwnerOnUnload` / `MidStackRemovalAtDepthTwo`: `Position` is unreflected; pure slot behaviour, untouched.

- [ ] **Step 7: Run the whole suite**

Run: `AstraTest.exe --gtest_brief=1`
Expected: 867 tests, all PASSED (868 - 1 deleted `MetaRebind` test).

- [ ] **Step 8: Commit**

```bash
git add include/Astra/Component/ComponentRegistry.hpp include/Astra/Component/ComponentModule.hpp include/Astra/Reflection/MetaRegistry.hpp tests/Component/ComponentModuleTest.cpp tests/Reflection/MetaRebindTest.cpp
git commit -m "feat(component)!: registry slots hold meta binder refs; ComponentModule releases instead of erasing -- multi-registry safe"
```

---

### Task 4: Static-init drain and `ReflectType` install baselines

**Spec:** §3.5 flow 1.

**Files:**
- Modify: `include/Astra/Reflection/MetaRegistry.hpp` — `Detail::StaticTypeRegistrar` (the enqueue lambda near the end of the file)
- Modify: `include/Astra/Reflection/Macros.hpp:207-230` — `ReflectType`, `ReflectEnum`
- Modify: `tests/Reflection/MetaRebindTest.cpp` — add two tests

**Interfaces:**
- Consumes: `InstallBaseline`, `Detail::CurrentModuleIdentity()`, `Detail::MetaFactory<T>::fn`, `Detail::BuildMetaThunk<T>`.
- Produces: no new names. Behavioural contract: after a module's drain, every `ASTRA_REFLECT_TYPE`d type has a refs-0 binder under that module's token, pinned iff the module declared `Resident` at `SetTypeContext`; `ReflectType<T>`/`ReflectEnum<T>` leave `MetaFactory<T>::fn` set.

- [ ] **Step 1: Write the failing tests**

Append to `tests/Reflection/MetaRebindTest.cpp` (`ModuleIdentity.hpp` already arrives through `MetaRegistry.hpp`; add no include):

```cpp
TEST(MetaRebind, DrainInstallsBaselineBinderForThisModule)
{
    // Spec 2026-09-09 §3.5 flow 1: the static-init drain ran in THIS module,
    // so MetaProbe's entry carries a refs-0, unpinned (this binary never
    // declared Resident) binder under this module's token.
    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash();
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    ASSERT_NE(reg.Get(hash), nullptr);
    EXPECT_GE(reg.BinderCount(hash), 1u);
    EXPECT_EQ(reg.TopBinder(hash), me);
    EXPECT_FALSE(reg.IsPinned(hash, me));
}

namespace Astra_Test_ModMeta
{
    struct ManualProbe { int m = 0; };   // reflected by hand below; never a component (zero ids)
}

TEST(MetaRebind, ReflectTypeStoresFactoryAndInstallsBaseline)
{
    ASSERT_FALSE(static_cast<bool>(Astra::Detail::MetaFactory<Astra_Test_ModMeta::ManualProbe>::fn));
    using Probe = Astra_Test_ModMeta::ManualProbe;
    Astra::TypeMeta* meta = Astra::ReflectType<Probe>(
        [](Astra::Detail::TypeMetaBuilder<Probe>& b)
        {
            b.Field<decltype(Probe::m), &Probe::m>("m");   // what ASTRA_REFLECT_FIELD expands to
        });
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->fields.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(Astra::Detail::MetaFactory<Astra_Test_ModMeta::ManualProbe>::fn));

    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::ManualProbe>::Hash();
    EXPECT_EQ(reg.Get(hash), meta);
    EXPECT_EQ(reg.TopBinder(hash), Astra::Detail::CurrentModuleIdentity().token);
    EXPECT_EQ(reg.Refs(hash, Astra::Detail::CurrentModuleIdentity().token), 0u);

    // Idempotent: a second manual registration returns the same entry.
    Astra::TypeMeta* again = Astra::ReflectType<Probe>(
        [](Astra::Detail::TypeMetaBuilder<Probe>& b)
        {
            b.Field<decltype(Probe::m), &Probe::m>("m");
        });
    EXPECT_EQ(again, meta);
    EXPECT_EQ(reg.BinderCount(hash), 1u);
}

namespace Astra_Test_ModMeta
{
    enum class ManualMode : uint8_t { Alpha = 0, Beta = 1 };   // reflected by hand below; never a component
}

TEST(MetaRebind, ReflectEnumStoresFactoryAndInstallsBaseline)
{
    using Mode = Astra_Test_ModMeta::ManualMode;
    ASSERT_FALSE(static_cast<bool>(Astra::Detail::MetaFactory<Mode>::fn));
    Astra::TypeMeta* meta = Astra::ReflectEnum<Mode>(
        [](Astra::Detail::EnumInfoBuilder<Mode>& eb)
        {
            eb.Value("Alpha", Mode::Alpha);   // what ASTRA_REFLECT_ENUM_VALUE expands to
            eb.Value("Beta",  Mode::Beta);
        });
    ASSERT_NE(meta, nullptr);
    EXPECT_TRUE(meta->isEnum);
    ASSERT_NE(meta->enumInfo, nullptr);

    // The retained factory must rebuild the SAME enum meta -- this is the
    // b.Enum(eb.Build()) path a hot-reload rebind would run, and it has no
    // other in-tree caller.
    ASSERT_TRUE(static_cast<bool>(Astra::Detail::MetaFactory<Mode>::fn));
    Astra::TypeMeta rebuilt = Astra::Detail::MetaFactory<Mode>::fn();
    EXPECT_TRUE(rebuilt.isEnum);
    ASSERT_NE(rebuilt.enumInfo, nullptr);
    EXPECT_EQ(rebuilt.typeHash, meta->typeHash);

    auto& reg = Astra::MetaRegistry::Instance();
    EXPECT_EQ(reg.TopBinder(meta->typeHash), Astra::Detail::CurrentModuleIdentity().token);
    EXPECT_EQ(reg.Refs(meta->typeHash, Astra::Detail::CurrentModuleIdentity().token), 0u);
}
```

- [ ] **Step 2: Build and run to verify they fail**

Run: Debug build, `AstraTest.exe --gtest_filter=MetaRebind.*`
Expected: `DrainInstallsBaselineBinderForThisModule` FAILS (`BinderCount == 0`: the drain still calls binder-less `Register`); `ReflectTypeStoresFactoryAndInstallsBaseline` and `ReflectEnumStoresFactoryAndInstallsBaseline` FAIL at their factory assertions.

- [ ] **Step 3: Rewire the drain**

In `MetaRegistry.hpp`, in `Detail::StaticTypeRegistrar<T>::StaticTypeRegistrar`, replace

```cpp
                // TypeMeta is move-only; hold it via shared_ptr so the
                // deferred registration stays copyable for std::function.
                auto meta = std::make_shared<TypeMeta>(builder.Build());
                EnqueuePendingMeta([meta](TypeContext& ctx)
                {
                    ctx.Meta().Register(std::move(*meta));
                });
```
with
```cpp
                // TypeMeta is move-only; hold it via shared_ptr so the
                // deferred registration stays copyable for std::function.
                auto meta = std::make_shared<TypeMeta>(builder.Build());
                EnqueuePendingMeta([meta](TypeContext& ctx)
                {
                    // Runs in the ENQUEUING module: the queue is module-local
                    // and is drained by that module's own SetTypeContext /
                    // Instance(), so the identity read here is this module's
                    // -- token AND residency (spec 2026-09-09 §3.5 flow 1).
                    // Installs a refs-0 baseline binder; first-wins content.
                    ctx.Meta().InstallBaseline(meta->typeHash, CurrentModuleIdentity(),
                                               &BuildMetaThunk<T>, std::move(*meta));
                });
```

- [ ] **Step 4: Rewire `ReflectType` / `ReflectEnum`**

In `include/Astra/Reflection/Macros.hpp` replace the two functions (:207-230) with:

```cpp
    /**
     * Manually registers a type with the reflection system.
     * Use this for types where macros are inconvenient (e.g., templates).
     * Retains the builder as this module's rebuild factory and installs a
     * baseline binder under this module's identity, exactly like the
     * ASTRA_REFLECT_TYPE static path (spec 2026-09-09 §3.5 flow 1).
     *
     * @tparam T The type to register
     * @param builderFunc Function that configures the TypeMetaBuilder
     * @return Pointer to the registered TypeMeta (existing entry on an
     *         idempotent re-registration; nullptr on identity collision)
     */
    template<typename T, typename BuilderFunc>
    inline TypeMeta* ReflectType(BuilderFunc&& builderFunc)
    {
        Detail::MetaFactory<T>::fn = [f = builderFunc]() {
            Detail::TypeMetaBuilder<T> b;
            f(b);
            return b.Build();
        };
        Detail::TypeMetaBuilder<T> builder;
        builderFunc(builder);
        TypeMeta fresh = builder.Build();
        const uint64_t hash = fresh.typeHash;
        return MetaRegistry::Instance().InstallBaseline(hash, Detail::CurrentModuleIdentity(),
                                                        &Detail::BuildMetaThunk<T>, std::move(fresh)).meta;
    }

    /**
     * Manually registers an enum with the reflection system.
     *
     * @tparam EnumType The enum type to register
     * @param enumBuilderFunc Function that configures the EnumInfoBuilder
     * @return Pointer to the registered TypeMeta (see ReflectType)
     */
    template<typename EnumType, typename EnumBuilderFunc>
    inline TypeMeta* ReflectEnum(EnumBuilderFunc&& enumBuilderFunc)
    {
        Detail::MetaFactory<EnumType>::fn = [f = enumBuilderFunc]() {
            Detail::TypeMetaBuilder<EnumType> b;
            Detail::EnumInfoBuilder<EnumType> eb;
            f(eb);
            b.Enum(eb.Build());
            return b.Build();
        };
        Detail::TypeMetaBuilder<EnumType> builder;
        Detail::EnumInfoBuilder<EnumType> enumBuilder;
        enumBuilderFunc(enumBuilder);
        builder.Enum(enumBuilder.Build());
        TypeMeta fresh = builder.Build();
        const uint64_t hash = fresh.typeHash;
        return MetaRegistry::Instance().InstallBaseline(hash, Detail::CurrentModuleIdentity(),
                                                        &Detail::BuildMetaThunk<EnumType>, std::move(fresh)).meta;
    }
```

`BuilderFunc` is copied into the factory (`[f = builderFunc]`), so callers passing a lambda with move-only captures will fail to compile — no such caller exists (grep `ReflectType<\|ReflectEnum<` in `tests/` to confirm before building).

- [ ] **Step 5: Build and run**

Run: Debug build, `AstraTest.exe --gtest_filter=MetaRebind.*:Reflection*:ComponentModule*:MetaBinder.*`
Expected: all PASSED. The `ComponentModule` suite must still pass with the drain now installing baselines — the flows in Task 3 Step 6 hold; only the starting stack differs (a refs-0 baseline under this binary's token instead of an empty stack), and in-binary that binder IS the handle's binder.

- [ ] **Step 6: Run the whole suite**

Run: `AstraTest.exe --gtest_brief=1`
Expected: 870 tests, all PASSED (867 + 3).

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Reflection/MetaRegistry.hpp include/Astra/Reflection/Macros.hpp tests/Reflection/MetaRebindTest.cpp
git commit -m "feat(reflection): static-init drain and ReflectType install baseline binders under the draining module's identity"
```

---

### Task 5: Arcane-shaped integration tests

**Spec:** §4 tests 9–13.

**Files:**
- Modify: `tests/Component/ComponentModuleTest.cpp` — append after the last existing test

**Interfaces:**
- Consumes: `Detail::ScopedModuleIdentity`, `MetaRegistry::{Refs, IsPinned, TopBinder, BinderCount}`, `Astra::GetMeta(uint64_t)`, existing reflected types `Astra_Test_ModMeta2::ReflectedOwned` / `ReflectedEphemeral`, the file's `InstalledContext` fixture.
- Produces: nothing new. These are the acceptance tests for decision 3 of the spec.

Ordering note for the implementer: gtest runs a suite's tests in declaration order. Two of these tests state a precondition on `ReflectedEphemeral` having **no entry** (left that way by `MetaErasedWhenSlotPopsToEmpty`, which runs earlier in this file); they `ASSERT` it up front so a reordering fails loudly instead of silently.

- [ ] **Step 1: Write the failing tests**

Append to `tests/Component/ComponentModuleTest.cpp`:

```cpp
// ============================================================================
// Meta binder scoping (spec 2026-09-09 §4 tests 9-13): several registries on
// ONE context. ScopedModuleIdentity lets this single binary play a resident
// "engine" image and a transient "plugin" image -- distinct addresses are
// distinct module tokens. Reuses the two reflected component types declared
// above (zero new ids).
// ============================================================================

namespace
{
    int s_engineImage = 0;   // stand-in image anchors
    int s_pluginImage = 0;
}

TEST(ComponentModule, ResidentEngineRosterSurvivesEveryRegistryTeardown)
{
    // Arcane shape: ~N short-lived Runtimes, each with its own registry and
    // its own engine handle, against one process-wide context. After the
    // LAST handle goes, registry-less GetMeta must still resolve. Both
    // teardown orders.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    Astra::Detail::ScopedModuleIdentity engine(&s_engineImage, Astra::ModuleResidency::Resident);
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();

    for (int order = 0; order < 2; ++order)
    {
        auto regA = std::make_shared<Astra::ComponentRegistry>();
        auto regB = std::make_shared<Astra::ComponentRegistry>();
        auto hA = Astra::ComponentModule::Open(regA, "EngineA");
        auto hB = Astra::ComponentModule::Open(regB, "EngineB");
        ASSERT_TRUE(static_cast<bool>(hA));
        ASSERT_TRUE(static_cast<bool>(hB));
        hA.Register<T>();
        hB.Register<T>();
        const Astra::TypeMeta* address = meta.Get(hash);
        ASSERT_NE(address, nullptr);
        EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
        EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);
        EXPECT_EQ(meta.Refs(hash, &s_engineImage), 2u);
        EXPECT_TRUE(meta.IsPinned(hash, &s_engineImage));

        Astra::ComponentModule& first  = (order == 0) ? hA : hB;
        Astra::ComponentModule& second = (order == 0) ? hB : hA;
        const std::shared_ptr<Astra::ComponentRegistry>& survivor = (order == 0) ? regB : regA;

        first.Reset();
        EXPECT_EQ(meta.Get(hash), address);                               // Held: still there, same address
        EXPECT_EQ(survivor->GetComponentDescriptor(id)->meta, address);   // the other registry is unharmed
        EXPECT_EQ(meta.Refs(hash, &s_engineImage), 1u);

        second.Reset();
        EXPECT_EQ(meta.Get(hash), address);                               // Retained: pinned at zero refs
        EXPECT_EQ(meta.Refs(hash, &s_engineImage), 0u);
        EXPECT_EQ(Astra::GetMeta(hash), address);                         // registry-less lookup works
        EXPECT_EQ(meta.GetByComponentId(id), address);                    // link rows intact

        auto regC = std::make_shared<Astra::ComponentRegistry>();
        auto hC = Astra::ComponentModule::Open(regC, "EngineC");
        hC.Register<T>();
        EXPECT_EQ(regC->GetComponentDescriptor(id)->meta, address);       // a third registry: same address
        EXPECT_EQ(meta.Refs(hash, &s_engineImage), 1u);
    }                                                                     // hC/regC die here: refs back to 0
}

TEST(ComponentModule, TransientPluginMetaOutlivesFirstRegistryOnly)
{
    // The original bug: a plugin-owned type in TWO registries. The first
    // teardown must not erase what the second still caches; the second
    // teardown erases.
    using T = Astra_Test_ModMeta2::ReflectedEphemeral;
    InstalledContext ctx;
    Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();
    ASSERT_EQ(meta.Get(hash), nullptr) << "precondition: MetaErasedWhenSlotPopsToEmpty runs earlier and leaves no entry";

    auto regA = std::make_shared<Astra::ComponentRegistry>();
    auto regB = std::make_shared<Astra::ComponentRegistry>();
    auto hA = Astra::ComponentModule::Open(regA, "PluginA");
    auto hB = Astra::ComponentModule::Open(regB, "PluginB");
    hA.Register<T>();
    hB.Register<T>();
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 2u);
    EXPECT_FALSE(meta.IsPinned(hash, &s_pluginImage));

    hA.Reset();
    EXPECT_EQ(meta.Get(hash), address);                                   // Held: B still caches it
    EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 1u);

    hB.Reset();
    EXPECT_EQ(meta.Get(hash), nullptr);                                   // Erased with the last ref
    EXPECT_EQ(meta.GetByComponentId(id), nullptr);
    EXPECT_EQ(meta.BinderCount(hash), 0u);
}

TEST(ComponentModule, OverriderDepartsWhileAnotherRegistryHolds)
{
    // Engine (resident) registers T anonymously in A and B; a transient
    // plugin overrides T in A only. When the plugin departs, A restores the
    // engine survivor, the meta rebinds to the engine's thunk, and B's cached
    // pointer never moved.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();

    auto regA = std::make_shared<Astra::ComponentRegistry>();
    auto regB = std::make_shared<Astra::ComponentRegistry>();
    {
        Astra::Detail::ScopedModuleIdentity engine(&s_engineImage, Astra::ModuleResidency::Resident);
        regA->RegisterComponent<T>();
        regB->RegisterComponent<T>();
    }
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(meta.TopBinder(hash), &s_engineImage);
    const uint32_t engineRefs = meta.Refs(hash, &s_engineImage);
    ASSERT_GE(engineRefs, 2u);

    {
        Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
        auto hP = Astra::ComponentModule::Open(regA, "PluginOverride");
        hP.Register<T>();                                                 // shadow push over the engine's slot in A
        EXPECT_EQ(meta.TopBinder(hash), &s_pluginImage);
        EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 1u);
        EXPECT_NE(regA->GetOwner(id), 0u);
        EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
    }                                                                     // plugin departs
    EXPECT_EQ(meta.TopBinder(hash), &s_engineImage);                      // Rebound to the engine's thunk
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 0u);
    EXPECT_EQ(meta.Get(hash), address);
    EXPECT_EQ(regA->GetOwner(id), 0u);                                    // engine survivor restored in A
    EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
    EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);           // B never moved
    EXPECT_EQ(meta.Refs(hash, &s_engineImage), engineRefs);               // engine refs untouched
}

TEST(ComponentModule, RangePurgeReleasesEachEntryAgainstItsOwnModule)
{
    // A non-RAII plugin registered anonymously under ITS token; the host's
    // range purge must release against the PLUGIN's binder, and the meta
    // survives because other binders still hold the type.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();
    const uint32_t before = meta.Refs(hash, &s_pluginImage);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    {
        Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
        creg->RegisterComponent<T>();
    }
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), before + 1);
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    const auto* liveDesc = creg->GetComponentDescriptor(id);
    ASSERT_NE(liveDesc, nullptr);
    const void* base = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(liveDesc->defaultConstruct) & ~uintptr_t(0xFFFF));
    const size_t dropped = creg->UnregisterModuleRange(base, size_t(1) << 30);
    EXPECT_GE(dropped, 1u);
    EXPECT_EQ(creg->GetComponentDescriptor(id), nullptr);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), before);                   // released against the plugin's binder
    EXPECT_EQ(meta.Get(hash), address);                                   // other binders still hold: not erased
}

TEST(ComponentModule, RegistryDestructionReleasesNothing)
{
    // Spec 2026-09-09 §3.6 (c): anonymous entries outlive their registry
    // exactly as before -- Arcane's registry-less GetMeta readers depend on
    // it. Over-holding is the safe direction.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    auto& meta = Astra::MetaRegistry::Instance();
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    const uint32_t before = meta.Refs(hash, me);
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponent<T>();
        EXPECT_EQ(meta.Refs(hash, me), before + 1);
    }                                                                     // registry dies WITHOUT releasing
    EXPECT_EQ(meta.Refs(hash, me), before + 1);
    EXPECT_NE(Astra::GetMeta(hash), nullptr);
}
```

- [ ] **Step 2: Build and run the new tests**

Run: Debug build, `AstraTest.exe --gtest_filter=ComponentModule.*`
Expected: all five new tests PASS on the first run — the implementation is complete after Task 4; these tests are the acceptance gate for the whole program. If any fails, the implementation is wrong, not the test: trace the flow in the spec (§3.5) for the failing assertion before touching either.

- [ ] **Step 3: Run the whole suite in Debug**

Run: `AstraTest.exe --gtest_brief=1`
Expected: 875 tests, all PASSED (870 + 5).

- [ ] **Step 4: Commit**

```bash
git add tests/Component/ComponentModuleTest.cpp
git commit -m "test(component): Arcane-shaped multi-registry meta lifetime -- resident engine, transient plugin, override, purge, registry death"
```

---

### Task 6: Documentation, spec amendments, three-config gate

**Spec:** §3.7 "Deleted text", §5.

**Files:**
- Modify: `docs/superpowers/specs/2026-08-09-astra-component-module-raii-design.md` — §3.5.1 backlog note, §3.6 meta-lifecycle bullet
- Modify: `docs/superpowers/specs/2026-09-09-astra-meta-binder-scoping-design.md` — §3.3, §3.7 (record `Rebind`, the diagnostics, and the dropped `InstallOwned` thunk parameter), status line
- Modify: `README.md` — the multi-module / ComponentModule contract paragraph, if it states the single-registry precondition (grep first)

- [ ] **Step 1: Confirm no stale caveat text survives in code**

Run: `grep -rn "one registry per context\|One registry per context\|single-registry\|EraseUnchecked\|m_metaThunk\|MetaRestore" include tests README.md`
Expected: no output. (Tasks 3–4 rewrote the two header comments; if anything prints, fix that comment now.)

- [ ] **Step 2: Amend the 2026-08-09 spec**

In `docs/superpowers/specs/2026-08-09-astra-component-module-raii-design.md`:

(a) In §3.5 flow 1 (the AMENDED 2026-08-10 paragraph), after the sentence ending "...if a consumer ever needs module-owned engine rosters." append:

```
   *(DONE 2026-09-10: per-registry meta scoping shipped as the meta binder
   stack -- see `2026-09-09-astra-meta-binder-scoping-design.md`. A resident
   module now declares `SetTypeContext(ctx, ModuleResidency::Resident)` and
   its metas survive the last handle; the single-registry precondition is
   gone.)*
```

(b) In §3.6, replace the bullet that begins "**Meta lifecycle mirrors descriptor lifecycle (plan-review finding 1, adjudicated):**" and ends "...registry-owned name storage is the follow-up hardening for that window." with:

```
- **Meta lifecycle (SUPERSEDED 2026-09-10):** the original rule -- erase the
  meta when a slot clears to empty -- was correct only with one registry per
  context. It is replaced by the binder stack in
  `2026-09-09-astra-meta-binder-scoping-design.md` §3.3: every live-or-
  shadowed slot in every registry holds a ref on the type's meta binder for
  its module; a binder leaves at zero refs unless its module declared
  residency; the meta is erased only when its stack is empty. The dangling-
  `typeName` concern that motivated erase-on-clear is preserved: a transient
  module's last release still erases. Residual: a LEAKED handle on an
  unmapped module keeps its stale meta -- pre-existing exposure, documented
  misuse; interned registry-owned name storage is the follow-up hardening for
  that window.
```

(c) In §3.6, delete the bullet "**Public `Erase` guard (plan-review finding 4):** ..." entirely (the guarded `Erase` no longer exists; a component-linked hash simply has refs).

- [ ] **Step 3: Amend the 2026-09-09 spec for what this plan added**

In `docs/superpowers/specs/2026-09-09-astra-meta-binder-scoping-design.md`:

(a) Change the `**Status:**` line to `**Status:** implemented 2026-09-10 (plan `docs/superpowers/plans/2026-09-10-astra-meta-binder-scoping.md`)`.

(b) In §3.3, after the `Release` declaration inside the code block, add:

```cpp
// Deferred half of BoundNeedsRebuild: swap content to `fresh` iff `module`'s
// binder is (still) top. Identity-checked. Called by RegisterComponent after
// it drops the registration lock.
bool Rebind(uint64_t hash, ModuleToken module, TypeMeta&& fresh);

// Read-only diagnostics (tests, tools): binder count, a module's refs,
// whether a module's binder is pinned, and the top binder's token.
size_t      BinderCount(uint64_t hash) const;
uint32_t    Refs(uint64_t hash, ModuleToken module) const;
bool        IsPinned(uint64_t hash, ModuleToken module) const;
ModuleToken TopBinder(uint64_t hash) const;
```

(c) In §3.7, change the `MetaRegistry` line to `+InstallBaseline, +Bind, +Acquire, +Release, +Rebind, +BinderCount/Refs/IsPinned/TopBinder, +MetaBinder/BindOutcome/BindResult/ReleaseOutcome; -Erase, -EraseUnchecked.` and the `ComponentRegistry` line to say `InstallOwned` returns `InstallResult`, takes a `ModuleToken`, and **drops its `MetaBuildFn` parameter** (the stack owns thunks; per-slot `m_metaThunk` and `ShadowEntry::buildMeta` are gone, replaced by the module token).

- [ ] **Step 4: README**

Run: `grep -n "ComponentModule\|SetTypeContext\|hot-reload\|hot reload" README.md | head`
If the README documents the `ComponentModule` contract or `SetTypeContext`, add one sentence where `SetTypeContext` is introduced:

```
A module that never unmaps (the engine EXE/DLL) should install with
`Astra::SetTypeContext(ctx, Astra::ModuleResidency::Resident)`: its reflected
metas then survive the last `ComponentModule` handle, so registry-less
`Astra::GetMeta` keeps resolving. Plugins keep the default `Transient`.
```
If the README has no such section, skip this step and say so in the commit message.

- [ ] **Step 5: Three-config gate**

Run each, in the worktree:
```
MSBuild.exe Astra.sln -p:Configuration=Debug   -p:Platform=x64 -m -v:m -nologo && bin\Debug-windows-x86_64\AstraTest\AstraTest.exe   --gtest_brief=1
MSBuild.exe Astra.sln -p:Configuration=Release -p:Platform=x64 -m -v:m -nologo && bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1
MSBuild.exe Astra.sln -p:Configuration=Dist    -p:Platform=x64 -m -v:m -nologo && bin\Dist-windows-x86_64\AstraTest\AstraTest.exe    --gtest_brief=1
```
Expected: 0 build errors in each; Debug 875/875; Release and Dist = Debug minus the Debug-only tests. There are exactly 3 Debug-only tests at `07b9240` (852 Debug; the count was independently re-derived at plan review), and this branch adds none, so expect 872/872. Record the three counts in the commit message.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-08-09-astra-component-module-raii-design.md docs/superpowers/specs/2026-09-09-astra-meta-binder-scoping-design.md README.md
git commit -m "docs: meta binder scoping shipped -- supersede the single-registry caveat, record Rebind/diagnostics; 3-config green"
```

---

## After the plan (Finish)

1. Whole-branch review at opus effort (this diff touches registration, the meta mutex, and the reload paths); fix wave if needed.
2. Controller-independent three-config verify in a clean worktree.
3. Local fast-forward merge `feature/meta-binder` → `dev`; delete the branch; **do not push**.
4. **OWED, cannot run here -- spec §4 sanitizer gate.** The lane (ASan/UBSan/TSan) is clang-on-Linux only (`premake5.lua` `--sanitize`, gmake `-fsanitize` flags; no MSVC path), and Finish forbids the push that triggers CI. Trigger: the next push of `dev`. What it guards: the `Release` unlock → thunk → relock window and the two-module registration smoke. Until it runs, that window is covered only by `MetaBinder.ReentrantBindDuringReleaseSkipsTheStaleSwap` and `ComponentModule.ConcurrentRegisterFromTwoModules`. Deferral acknowledged by the user at plan review, 2026-09-10.
5. **Arcane vendored-copy sync -- executed from the Arcane side (approved by the user at plan review, 2026-09-10: the sync is mechanical and is part of Finish; only the ratification stays a separate Arcane decision).** From `D:\dev\starworks\Arcane`: run `Scripts\sync-astra.ps1` after step 3; bump the plugin ABI to the next number with a history line in `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (reason: `ComponentRegistry` layout changed -- `m_metaThunk` → `m_metaModule`, `MetaRegistry` entries carry a binder stack -- so a plugin's inlined template code manipulates a registry whose layout changed; same class as the v10 entry); expect ~63 CRLF-only `.hpp` diffs and verify content identity with `git diff --ignore-cr-at-eol`; regenerate, build, and run Arcane's suite. No Arcane logic changes: Arcane calls none of the changed internals (verified 2026-09-09). Whether Arcane then declares `SetTypeContext(ctx, Resident)` in `Runtime::Impl` and moves to a Runtime-owned engine `ComponentModule` re-opens the 2026-08-10 ratification and is a separate decision.
