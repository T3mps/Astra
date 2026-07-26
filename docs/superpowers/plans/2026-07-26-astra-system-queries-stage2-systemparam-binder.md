# Astra System Queries — Stage 2: SystemParam Binder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user register a plain function whose parameters are `View<…>&` / `Res<T>` / `ResMut<T>` / `Commands`, with the scheduler's read/write/resource access **derived from the parameter list** (declared access == used access, automatically).

**Architecture:** A new header `SystemParam.hpp` adds three handle types (`Res`, `ResMut`, `Commands`), a per-parameter access classifier, and a `SystemParamBinder` base plus two leaf wrappers (`FunctionSystemWrapper` for lambdas, `FreeFunctionSystemWrapper` for free functions via NTTP). Each wrapper is a **context system** (`void operator()(SystemContext&)`) that exposes the same `ReadsComponents`/`WritesComponents`/`ReadsResourceTypes`/`WritesResourceTypes` + `HasTraits` typedefs the existing `ExtractSystemTraits` already harvests — so the scheduler's grouping/conflict machinery is consumed **unchanged**. Registration adds two `AddSystem` overloads, one `LambdaLike` exclusion clause, and one private internal.

**Tech Stack:** C++20 header-only; MSVC/MSBuild; GoogleTest; premake5 (vs2022). Heavy template metaprogramming (fold expressions, `if constexpr`, `std::tuple`, `std::index_sequence`, NTTP `auto`).

## Global Constraints

- **Additive only.** Do NOT modify `ViewAccess` (`View.hpp`), `SystemContext.hpp`, `SystemExecutor.hpp`, `SystemMetadata.hpp`, or `ExtractSystemTraits`/`BuildExecutionPlan`/`Conflicts` in `SystemScheduler.hpp`. The only edits to existing files are: `System.hpp` (+1 `LambdaLike` clause, +1 include), `SystemScheduler.hpp` (+2 `AddSystem` overloads, +1 deduction helper, +1 internal), `Astra.hpp` (+1 include).
- **TypeID ceiling (128).** The test binary sits near the ceiling. Tests MUST reuse `Astra::Test::*` types from `tests/TestComponents.hpp` — `Position`/`Velocity` as components, `Health`/`Physics` as resource payloads. Do NOT mint new component/resource types. A type used as a resource consumes the SAME `TypeID<T>::Value()` it uses as a component, so reusing already-registered test types costs zero new IDs.
- **Systems keyed by `TypeID<Wrapper>::Hash()`.** Free functions must be registered as a non-type template argument (`AddSystem<Fn>()`), never by value, so two same-signature free functions get distinct wrapper types.
- **`Commands`'s ctor is `explicit`.** This keeps `ContextSystem` and `ParamFunctor` disjoint (a `(Commands)` lambda must not be implicitly convertible from `SystemContext&`).
- **Judge only by MSBuild.** IDE/clangd diagnostics (STL1000, missing Mosaic/gtest headers) are false positives.
- **Build & run commands** (used verbatim throughout — `<Cfg>` ∈ {Debug, Release, Dist}):
  - Regen (only when a `.cpp` is added): `premake5 vs2022`
  - Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Cfg> -p:Platform=x64 -m`
  - Run all Stage-2 tests: `bin/<Cfg>-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemParam.*`
  - `-t:AstraTest` is NOT usable (MSB4057) — build the whole solution.

---

## File Structure

**New**
- `include/Astra/System/SystemParam.hpp` — the entire Stage-2 surface: `Res`/`ResMut`/`Commands`; `Detail` classifiers (`ParamAccess`, `IsSystemParam`, `IsView`, `IsResourceParam`, `ResourceType`, `ViewOf`, `ViewSlot`, `FnSignature`, `AllParams`, `IsParamFunctor`, `IsParamFreeFunction`); `ParamFunctor` concept; `SystemParamBinder`; `FunctionSystemWrapper`; `FreeFunctionSystemWrapper`. Built incrementally across Tasks 1–5.
- `tests/System/SystemParamTest.cpp` — all Stage-2 tests. Grown across Tasks 1–6.

**Modified**
- `include/Astra/System/System.hpp` — Task 5: include `SystemParam.hpp`, add `!ParamFunctor<T>` to `LambdaLike`.
- `include/Astra/System/SystemScheduler.hpp` — Task 5: two `AddSystem` overloads + `AddParamSystemImpl` + `AddParamSystemInternal`.
- `include/Astra/Astra.hpp` — Task 1: include `SystemParam.hpp`.

---

## Task 1: `Res`/`ResMut`/`Commands` handle types + header + test scaffold

**Files:**
- Create: `include/Astra/System/SystemParam.hpp`
- Modify: `include/Astra/Astra.hpp` (add include near the other `System/` includes)
- Test: `tests/System/SystemParamTest.cpp` (create)

**Interfaces:**
- Produces: `Astra::Res<T>` (ctor `explicit Res(const T*)`; `operator*`→`const T&`, `operator->`→`const T*`, `Get()`→`const T&`); `Astra::ResMut<T>` (ctor `explicit ResMut(T*)`; `operator*`→`T&`, `operator->`→`T*`, `Get()`→`T&`); `Astra::Commands` (ctor `explicit Commands(SystemContext&)`; `operator->`→`CommandBuffer*`, `Get()`→`CommandBuffer&`).

- [ ] **Step 1: Verify include graph has no cycle**

Confirm none of `View.hpp`, `SystemContext.hpp`, `Registry.hpp`, `Commands/CommandBuffer.hpp` include `System/System.hpp` (grep). Expected: no matches — `SystemParam.hpp` can safely include all four without a cycle.

Run: `grep -rn "System/System.hpp" include/Astra/Registry include/Astra/Commands include/Astra/System/SystemContext.hpp`
Expected: no output.

- [ ] **Step 2: Create `SystemParam.hpp` with the three handle types**

Create `include/Astra/System/SystemParam.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "../Commands/CommandBuffer.hpp"
#include "../Core/Base.hpp"
#include "../Core/Log.hpp"
#include "../Registry/Registry.hpp"
#include "../Registry/View.hpp"
#include "SystemContext.hpp"

namespace Astra
{
    // ---- SystemParam handle types (design §3.1) --------------------------------

    // Read-only resource handle. Contributes T to a system's resourceReads.
    // Never null inside a system body: SystemParamBinder's resource-presence
    // gate (Task 4) skips-and-logs before invoking if the resource is absent.
    template<typename T>
    class Res
    {
        const T* m_ptr;
    public:
        explicit Res(const T* p) noexcept : m_ptr(p) {}
        ASTRA_NODISCARD const T& operator*()  const noexcept { return *m_ptr; }
        ASTRA_NODISCARD const T* operator->() const noexcept { return  m_ptr; }
        ASTRA_NODISCARD const T& Get()        const noexcept { return *m_ptr; }
    };

    // Mutable resource handle. Contributes T to a system's resourceWrites.
    template<typename T>
    class ResMut
    {
        T* m_ptr;
    public:
        explicit ResMut(T* p) noexcept : m_ptr(p) {}
        ASTRA_NODISCARD T& operator*()  const noexcept { return *m_ptr; }
        ASTRA_NODISCARD T* operator->() const noexcept { return  m_ptr; }
        ASTRA_NODISCARD T& Get()        const noexcept { return *m_ptr; }
    };

    // Deferred structural-change handle. Contributes NO scheduling access
    // (per-worker buffer, deterministic flush at sync points). Each operator->
    // re-stamps the sort key via SystemContext::Commands(), so N statements
    // record N deterministically-ordered commands. Do NOT cache the returned
    // pointer across statements. Ctor is explicit so a `(Commands)` lambda is
    // NOT implicitly convertible from SystemContext& (keeps ContextSystem and
    // ParamFunctor disjoint).
    class Commands
    {
        SystemContext* m_ctx;
    public:
        explicit Commands(SystemContext& ctx) noexcept : m_ctx(&ctx) {}
        ASTRA_NODISCARD CommandBuffer* operator->() const { return &m_ctx->Commands(); }
        ASTRA_NODISCARD CommandBuffer& Get()        const { return  m_ctx->Commands(); }
    };
}
```

(Note: `Commands` stores a `SystemContext*` rather than a reference so it is trivially copyable/assignable as a by-value parameter; behavior is identical.)

- [ ] **Step 3: Wire the include into `Astra.hpp`**

In `include/Astra/Astra.hpp`, add alongside the other `System/` includes (e.g. right after the `System/SystemScheduler.hpp` / `System/SystemContext.hpp` includes):

```cpp
#include "System/SystemParam.hpp"
```

- [ ] **Step 4: Create the test file with the RED test**

Create `tests/System/SystemParamTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
    using Astra::Test::Physics;
}

// ---- Task 1: the Res/ResMut handle types wrap a pointer and deref it. -------

TEST(SystemParam, ResAndResMutWrapAndDereferencePointer)
{
    Health h{7, 42};
    Astra::Res<Health>    r{&h};
    Astra::ResMut<Health> m{&h};

    EXPECT_EQ(r->current, 7);
    EXPECT_EQ((*r).max,   42);

    m->current = 9;            // ResMut is mutable
    EXPECT_EQ(h.current, 9);
    EXPECT_EQ(r.Get().current, 9);
}
```

- [ ] **Step 5: Regen, build, and verify the test PASSES**

Run: `premake5 vs2022`
Then build Debug, then run.
Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemParam.*`
Expected: `[ PASSED ] 1 test` (`SystemParam.ResAndResMutWrapAndDereferencePointer`). If the header has a compile error, fix it before proceeding — the whole binary must build. (This task's "RED" is implicit: before Step 2 the symbols don't exist, so the file wouldn't compile; the deliverable is a green build with the passing test.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/System/SystemParam.hpp include/Astra/Astra.hpp tests/System/SystemParamTest.cpp
git commit -m "feat(system-param): Res/ResMut/Commands handle types + test scaffold (Stage 2 task 1)"
```

---

## Task 2: Per-parameter classifiers (`ParamAccess`, `IsSystemParam`, helpers)

**Files:**
- Modify: `include/Astra/System/SystemParam.hpp` (add a `Detail` block after the handle types)
- Test: `tests/System/SystemParamTest.cpp`

**Interfaces:**
- Consumes: `Astra::Res<T>`, `Astra::ResMut<T>`, `Astra::Commands` (Task 1); `Astra::View<Args...>`, `Astra::ViewAccess<View<Args...>>` (existing).
- Produces (all in `Astra::Detail`): `ParamAccess<P>` with `::Reads`/`::Writes`/`::ResReads`/`::ResWrites` (each a `std::tuple<…>`); `IsSystemParam<P>` / `IsSystemParam_v<P>`; `IsView<P>`; `IsResourceParam<P>`; `ResourceType<P>::type`; `ViewOf<P>::type`; `ViewSlot<P>::type`.

- [ ] **Step 1: Write the failing test**

Append to `tests/System/SystemParamTest.cpp`:

```cpp
// ---- Task 2: per-parameter access classification (component reads/writes -----
// ---- from View const-ness; Res->resReads, ResMut->resWrites). ----------------

TEST(SystemParam, ParamAccessClassifiesComponentsAndResources)
{
    using VRef = Astra::View<Position, const Velocity>&;
    using PA_V = Astra::Detail::ParamAccess<VRef>;
    EXPECT_TRUE((std::is_same_v<PA_V::Writes, std::tuple<Position>>));   // Position (non-const) = write
    EXPECT_TRUE((std::is_same_v<PA_V::Reads,  std::tuple<Velocity>>));   // const Velocity = read
    EXPECT_TRUE((std::is_same_v<PA_V::ResReads,  std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_V::ResWrites, std::tuple<>>));

    using PA_R = Astra::Detail::ParamAccess<Astra::Res<Health>>;
    EXPECT_TRUE((std::is_same_v<PA_R::ResReads,  std::tuple<Health>>));
    EXPECT_TRUE((std::is_same_v<PA_R::ResWrites, std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_R::Reads,     std::tuple<>>));

    using PA_M = Astra::Detail::ParamAccess<Astra::ResMut<Physics>>;
    EXPECT_TRUE((std::is_same_v<PA_M::ResWrites, std::tuple<Physics>>));
    EXPECT_TRUE((std::is_same_v<PA_M::ResReads,  std::tuple<>>));

    using PA_C = Astra::Detail::ParamAccess<Astra::Commands>;
    EXPECT_TRUE((std::is_same_v<PA_C::Reads,     std::tuple<>>));
    EXPECT_TRUE((std::is_same_v<PA_C::ResWrites,  std::tuple<>>));

    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<VRef>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::Res<Health>>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::ResMut<Physics>>));
    EXPECT_TRUE((Astra::Detail::IsSystemParam_v<Astra::Commands>));
    EXPECT_FALSE((Astra::Detail::IsSystemParam_v<Position>));            // bare component: NOT a param
    EXPECT_FALSE((Astra::Detail::IsSystemParam_v<Astra::View<Position>>)); // by-value View: NOT a param (must be &)
}
```

- [ ] **Step 2: Run to verify it fails**

Build Debug. Expected: compile error — `Astra::Detail::ParamAccess` / `IsSystemParam_v` do not exist yet (the binary fails to build). That is the RED signal.

- [ ] **Step 3: Add the `Detail` classifiers**

In `SystemParam.hpp`, after the handle types (still inside `namespace Astra`), add:

```cpp
    namespace Detail
    {
        // ---- Per-parameter access classification (design §4.1) --------------
        // Default (Commands and anything else): contributes nothing.
        template<typename P> struct ParamAccess
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<>;
        };
        template<typename... A> struct ParamAccess<View<A...>&>
        {
            using Reads    = typename ViewAccess<View<A...>>::Reads;   // consumes Stage 1
            using Writes   = typename ViewAccess<View<A...>>::Writes;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<>;
        };
        template<typename T> struct ParamAccess<Res<T>>
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<T>; using ResWrites = std::tuple<>;
        };
        template<typename T> struct ParamAccess<ResMut<T>>
        {
            using Reads    = std::tuple<>;  using Writes    = std::tuple<>;
            using ResReads = std::tuple<>;  using ResWrites = std::tuple<T>;
        };

        // ---- IsSystemParam: the closed param set (design §5.1) --------------
        template<typename P> struct IsSystemParam : std::false_type {};
        template<typename... A> struct IsSystemParam<View<A...>&> : std::true_type {};
        template<typename T>    struct IsSystemParam<Res<T>>      : std::true_type {};
        template<typename T>    struct IsSystemParam<ResMut<T>>   : std::true_type {};
        template<>              struct IsSystemParam<Commands>    : std::true_type {};
        template<typename P> inline constexpr bool IsSystemParam_v = IsSystemParam<P>::value;

        // ---- Shape helpers used by the binder (Task 3/4) --------------------
        template<typename P> struct IsView : std::false_type {};
        template<typename... A> struct IsView<View<A...>&> : std::true_type {};

        template<typename P> struct IsResourceParam : std::false_type {};
        template<typename T> struct IsResourceParam<Res<T>>    : std::true_type {};
        template<typename T> struct IsResourceParam<ResMut<T>> : std::true_type {};

        template<typename P> struct ResourceType;                 // Res<T>/ResMut<T> -> T
        template<typename T> struct ResourceType<Res<T>>    { using type = T; };
        template<typename T> struct ResourceType<ResMut<T>> { using type = T; };

        template<typename P> struct ViewOf { using type = void; };            // View<A...>& -> View<A...>
        template<typename... A> struct ViewOf<View<A...>&> { using type = View<A...>; };

        template<typename P> struct ViewSlot { using type = std::monostate; };   // per-param cache slot
        template<typename... A> struct ViewSlot<View<A...>&> { using type = std::optional<View<A...>>; };
    }
```

- [ ] **Step 4: Run to verify it passes**

Build Debug, run `--gtest_filter=SystemParam.*`. Expected: 2 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/System/SystemParam.hpp tests/System/SystemParamTest.cpp
git commit -m "feat(system-param): per-parameter access classifiers + shape helpers (Stage 2 task 2)"
```

---

## Task 3: `SystemParamBinder` access-harvest typedefs

**Files:**
- Modify: `include/Astra/System/SystemParam.hpp` (add the `SystemParamBinder` class template after the `Detail` block)
- Test: `tests/System/SystemParamTest.cpp`

**Interfaces:**
- Consumes: `Detail::ParamAccess<P>` (Task 2).
- Produces: `Astra::SystemParamBinder<Params...>` exposing `ReadsComponents`, `WritesComponents`, `ReadsResourceTypes`, `WritesResourceTypes` (each a `std::tuple<…>` = concat of per-param access), `static constexpr bool HasTraits = true`, `static constexpr bool RequiresExclusive = false`. These are exactly the typedefs `HasSystemTraits_v` and `ExtractSystemTraits` read.

- [ ] **Step 1: Write the failing test**

Append to `tests/System/SystemParamTest.cpp`:

```cpp
// ---- Task 3: SystemParamBinder unions each parameter's access into the -------
// ---- typedefs ExtractSystemTraits harvests. ----------------------------------

TEST(SystemParam, BinderHarvestsUnionedAccess)
{
    using Binder = Astra::SystemParamBinder<
        Astra::View<Position, const Velocity>&,   // write Position, read Velocity
        Astra::Res<Health>,                        // read resource Health
        Astra::ResMut<Physics>,                    // write resource Physics
        Astra::Commands>;                          // nothing

    EXPECT_TRUE((std::is_same_v<Binder::WritesComponents,    std::tuple<Position>>));
    EXPECT_TRUE((std::is_same_v<Binder::ReadsComponents,     std::tuple<Velocity>>));
    EXPECT_TRUE((std::is_same_v<Binder::ReadsResourceTypes,  std::tuple<Health>>));
    EXPECT_TRUE((std::is_same_v<Binder::WritesResourceTypes, std::tuple<Physics>>));
    EXPECT_TRUE(Binder::HasTraits);
    EXPECT_FALSE(Binder::RequiresExclusive);

    // The binder must satisfy the trait-detection gate the scheduler uses.
    EXPECT_TRUE((Astra::HasSystemTraits_v<Binder>));
}
```

- [ ] **Step 2: Run to verify it fails**

Build Debug. Expected: compile error — `Astra::SystemParamBinder` does not exist. RED.

- [ ] **Step 3: Add `SystemParamBinder` (typedefs only for now)**

In `SystemParam.hpp`, after the `Detail` block (inside `namespace Astra`), add:

```cpp
    // Shared machinery for a parameter-function system (design §4.2). This task
    // adds only the harvested-access typedefs; Task 4 adds Run/BuildParam and
    // the per-View cache. The typedefs make HasSystemTraits_v<Wrapper> true so
    // the existing ExtractSystemTraits fills the scheduler's masks unchanged.
    template<typename... Params>
    class SystemParamBinder
    {
    public:
        using ReadsComponents     = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Reads>()...));
        using WritesComponents    = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Writes>()...));
        using ReadsResourceTypes  = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResReads>()...));
        using WritesResourceTypes = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResWrites>()...));
        static constexpr bool HasTraits = true;
        static constexpr bool RequiresExclusive = false;
    };
```

- [ ] **Step 4: Run to verify it passes**

Build Debug, run `--gtest_filter=SystemParam.*`. Expected: 3 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/System/SystemParam.hpp tests/System/SystemParamTest.cpp
git commit -m "feat(system-param): SystemParamBinder access-harvest typedefs (Stage 2 task 3)"
```

---

## Task 4: Binder execution (`Run`/`BuildParam`/view-cache/skip-and-log) + leaf wrappers

**Files:**
- Modify: `include/Astra/System/SystemParam.hpp` (extend `SystemParamBinder`; add the two wrappers)
- Test: `tests/System/SystemParamTest.cpp`

**Interfaces:**
- Consumes: `Detail::IsView`/`IsResourceParam`/`ResourceType`/`ViewOf`/`ViewSlot` (Task 2); `SystemContext` (`GetRegistry()`, `Commands()`); `Registry::CreateView<…>()`, `Registry::GetResource<T>()`.
- Produces: `SystemParamBinder<Params...>::Run(SystemContext&, Invoke)` (protected) — rebinds the view cache on registry change, runs the resource-presence gate (skip-and-log if absent), then calls `invoke(BuildParam<Params>()...)`. `Astra::FunctionSystemWrapper<Fn, Params...>` (ctor `explicit FunctionSystemWrapper(Fn)`; `void operator()(SystemContext&)`). `Astra::FreeFunctionSystemWrapper<auto FnPtr, Params...>` (default-constructible; `void operator()(SystemContext&)`).

- [ ] **Step 1: Write the failing test**

Append to `tests/System/SystemParamTest.cpp`. These tests build a `SystemContext` by hand (Registry + CommandBuffer), invoke the wrapper directly, and thereby exercise param construction, view iteration, `Commands`, view caching, and skip-and-log **without** the scheduler.

```cpp
// ---- Task 4: a wrapper builds each parameter from a SystemContext, iterates ---
// ---- the cached view, records Commands, and skips-and-logs a missing resource.

namespace
{
    // A free function param-system used by several tests (Task 4 & Task 6).
    inline void AddOneToPositions(Astra::View<Position>& v)
    {
        v.ForEach([](Position& p) { p.x += 1.0f; });
    }
}

TEST(SystemParam, WrapperRunsViewParamAndCachesView)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 10.0f;

    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    Astra::FunctionSystemWrapper<decltype(AddOneToPositions)*, Astra::View<Position>&>
        wrapper{&AddOneToPositions};

    wrapper(ctx);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 11.0f);
    wrapper(ctx);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 12.0f);   // second run reuses cached view
}

TEST(SystemParam, WrapperRunsResourceAndCommandsParams)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.SetResource(Health{5, 5});

    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    int seen = -1;
    auto body = [&](Astra::Res<Health> h, Astra::Commands c)
    {
        seen = h->current;              // reads the resource
        c->DestroyEntity(e);            // records a deferred command
    };
    Astra::FunctionSystemWrapper<decltype(body), Astra::Res<Health>, Astra::Commands>
        wrapper{body};

    wrapper(ctx);
    EXPECT_EQ(seen, 5);
    EXPECT_EQ(cmds.GetCommandCount(), 1u);    // Commands recorded into the buffer
}

TEST(SystemParam, WrapperSkipsWhenResourceAbsent)
{
    Astra::Registry reg;                 // no Health resource set
    Astra::CommandBuffer cmds{&reg};
    Astra::SystemContext ctx{reg, cmds, 0u};

    bool ran = false;
    auto body = [&](Astra::ResMut<Health> h) { (void)h; ran = true; };
    Astra::FunctionSystemWrapper<decltype(body), Astra::ResMut<Health>> wrapper{body};

    wrapper(ctx);
    EXPECT_FALSE(ran);                   // body never entered (skip-and-log)

    reg.SetResource(Health{1, 1});       // now present
    wrapper(ctx);
    EXPECT_TRUE(ran);                    // resumes once the resource exists
}
```

- [ ] **Step 2: Run to verify it fails**

Build Debug. Expected: compile error — `FunctionSystemWrapper` does not exist, and `SystemParamBinder` has no `Run`. RED.

- [ ] **Step 3: Extend `SystemParamBinder` with the execution machinery**

Replace the `SystemParamBinder` body from Task 3 with (typedefs unchanged, new `protected`/`private` members added):

```cpp
    template<typename... Params>
    class SystemParamBinder
    {
    public:
        using ReadsComponents     = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Reads>()...));
        using WritesComponents    = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::Writes>()...));
        using ReadsResourceTypes  = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResReads>()...));
        using WritesResourceTypes = decltype(std::tuple_cat(std::declval<typename Detail::ParamAccess<Params>::ResWrites>()...));
        static constexpr bool HasTraits = true;
        static constexpr bool RequiresExclusive = false;

    protected:
        // Rebind caches if the registry changed; run the resource-presence gate;
        // then invoke `invoke(BuildParam<Params>(ctx, reg)...)`. `invoke` is the
        // wrapper's thunk that forwards to the user fn / free fn.
        template<typename Invoke>
        void Run(SystemContext& ctx, Invoke invoke)
        {
            Registry& reg = ctx.GetRegistry();
            if (m_viewRegistry != &reg)   // IM-23 parity: rebuild views on registry switch
            {
                ResetViews(std::index_sequence_for<Params...>{});
                m_viewRegistry = &reg;
            }
            if (!ResourcesPresent(reg))   // skip-and-log (design §6)
            {
                if (!m_loggedMissing)
                {
                    ASTRA_LOG_ERROR("Astra param-system skipped this frame: a declared "
                                    "Res/ResMut resource is absent from the Registry.");
                    m_loggedMissing = true;
                }
                return;
            }
            m_loggedMissing = false;      // a later disappearance logs again
            InvokeImpl(ctx, reg, invoke, std::index_sequence_for<Params...>{});
        }

    private:
        template<typename Invoke, size_t... Is>
        void InvokeImpl(SystemContext& ctx, Registry& reg, Invoke& invoke, std::index_sequence<Is...>)
        {
            invoke(BuildParam<Params, Is>(ctx, reg)...);
        }

        // Construct one parameter. View params return an lvalue reference into
        // the persistent cache; Res/ResMut/Commands return a fresh handle.
        template<typename P, size_t I>
        decltype(auto) BuildParam(SystemContext& ctx, Registry& reg)
        {
            if constexpr (Detail::IsView<P>::value)
            {
                auto& slot = std::get<I>(m_views);       // std::optional<View<A...>>
                if (!slot.has_value())
                    slot.emplace(CreateViewFor(reg, static_cast<typename Detail::ViewOf<P>::type*>(nullptr)));
                return static_cast<P>(*slot);            // P == View<A...>& -> lvalue ref
            }
            else if constexpr (std::is_same_v<P, Commands>)
            {
                return Commands{ctx};
            }
            else if constexpr (Detail::IsResourceParam<P>::value)
            {
                using RT = typename Detail::ResourceType<P>::type;
                if constexpr (std::is_same_v<P, Res<RT>>)
                    return Res<RT>{reg.GetResource<RT>()};
                else
                    return ResMut<RT>{reg.GetResource<RT>()};
            }
        }

        template<typename... A>
        static View<A...> CreateViewFor(Registry& reg, View<A...>*) { return reg.CreateView<A...>(); }

        bool ResourcesPresent(Registry& reg) const
        {
            return (ParamPresent<Params>(reg) && ...);
        }
        template<typename P>
        static bool ParamPresent(Registry& reg)
        {
            if constexpr (Detail::IsResourceParam<P>::value)
                return reg.GetResource<typename Detail::ResourceType<P>::type>() != nullptr;
            else
                return true;
        }

        template<size_t... Is>
        void ResetViews(std::index_sequence<Is...>) { (ResetSlot(std::get<Is>(m_views)), ...); }
        template<typename Slot>
        static void ResetSlot(Slot& slot)
        {
            if constexpr (!std::is_same_v<Slot, std::monostate>)
                slot.reset();
        }

        // One cache slot per parameter, index-aligned with Params (monostate for
        // non-View params). IM-23 view caching, generalized to N views.
        std::tuple<typename Detail::ViewSlot<Params>::type...> m_views{};
        Registry* m_viewRegistry = nullptr;
        bool m_loggedMissing = false;
    };
```

- [ ] **Step 4: Add the two leaf wrappers**

In `SystemParam.hpp`, after `SystemParamBinder` (inside `namespace Astra`), add:

```cpp
    // Lambda / functor param-system (registered by value). Fn is the closure
    // type (unique per lambda -> unique wrapper type -> unique TypeID hash).
    template<typename Fn, typename... Params>
    class FunctionSystemWrapper : public SystemParamBinder<Params...>
    {
        Fn m_fn;
    public:
        explicit FunctionSystemWrapper(Fn fn) : m_fn(std::move(fn)) {}
        void operator()(SystemContext& ctx)
        {
            this->Run(ctx, [this](auto&&... p) { m_fn(static_cast<decltype(p)>(p)...); });
        }
    };

    // Free-function param-system (registered as a non-type template argument, so
    // each function is its own wrapper type -> no same-signature hash collision).
    // No stored callable: FnPtr is the template argument.
    template<auto FnPtr, typename... Params>
    class FreeFunctionSystemWrapper : public SystemParamBinder<Params...>
    {
    public:
        void operator()(SystemContext& ctx)
        {
            this->Run(ctx, [](auto&&... p) { FnPtr(static_cast<decltype(p)>(p)...); });
        }
    };
```

- [ ] **Step 5: Run to verify it passes**

Build Debug, run `--gtest_filter=SystemParam.*`. Expected: 6 tests PASS. (The skip-and-log test emits one `ASTRA_LOG_ERROR` line to stderr — that is expected, not a failure.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/System/SystemParam.hpp tests/System/SystemParamTest.cpp
git commit -m "feat(system-param): binder Run/BuildParam/view-cache/skip-and-log + leaf wrappers (Stage 2 task 4)"
```

---

## Task 5: Registration wiring (concepts, `LambdaLike` clause, `AddSystem` overloads)

**Files:**
- Modify: `include/Astra/System/SystemParam.hpp` (add the concept machinery)
- Modify: `include/Astra/System/System.hpp` (include `SystemParam.hpp`; amend `LambdaLike`)
- Modify: `include/Astra/System/SystemScheduler.hpp` (two `AddSystem` overloads + helpers)
- Test: `tests/System/SystemParamTest.cpp`

**Interfaces:**
- Consumes: `FunctionSystemWrapper`/`FreeFunctionSystemWrapper` (Task 4); `Detail::IsSystemParam_v` (Task 2); `SystemScheduler::ExtractSystemTraits`/`AddContextSystemInternal` pattern (existing).
- Produces (in `SystemParam.hpp`): `Astra::Detail::FnSignature<…>::Params`, `Detail::AllParams<Tuple>`, `Detail::IsParamFunctor_v<Fn>`, `Detail::IsParamFreeFunction_v<FnType>`, and the `Astra::ParamFunctor<T>` concept. Produces (in `SystemScheduler.hpp`): `template<typename Fn> requires ParamFunctor<Fn> AddSystem(Fn&&)` and `template<auto FnPtr> requires Detail::IsParamFreeFunction_v<decltype(FnPtr)> AddSystem()`, both returning `Result<void, SystemError>`.

- [ ] **Step 1: Write the failing test**

Append to `tests/System/SystemParamTest.cpp`:

```cpp
// ---- Task 5: register param-systems on the scheduler; masks flow through ------
// ---- ExtractSystemTraits; both spellings run under Execute(). -----------------

TEST(SystemParam, LambdaParamSystemRegistersRunsAndHarvestsMasks)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;
    reg.SetResource(Health{3, 3});

    Astra::SystemScheduler s;
    auto added = s.AddSystem([](Astra::View<Position>& v, Astra::Res<Health> h)
    {
        v.ForEach([&](Position& p) { p.x += static_cast<float>(h->current); });
    });
    ASSERT_TRUE(added.IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 3.0f);   // body ran with the resource
}

TEST(SystemParam, FreeFunctionParamSystemRegistersViaNTTPAndRuns)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    auto added = s.AddSystem<AddOneToPositions>();       // free fn as template arg
    ASSERT_TRUE(added.IsOk());
    EXPECT_TRUE(s.HasSystem<AddOneToPositions>());       // symmetric Has<>

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 1.0f);
}
```

- [ ] **Step 2: Run to verify it fails**

Build Debug. Expected: compile error — `AddSystem` has no `ParamFunctor` / NTTP overload; `AddSystem<AddOneToPositions>()` does not resolve. RED.

- [ ] **Step 3: Add the concept machinery to `SystemParam.hpp`**

In `SystemParam.hpp`, inside the `Detail` namespace (append after the shape helpers from Task 2), add:

```cpp
        // ---- Callable-signature deduction for the registration concepts -----
        template<typename> struct FnSignature;                                   // undefined base
        template<typename R, typename C, typename... A>
        struct FnSignature<R(C::*)(A...)>       { using Params = std::tuple<A...>; };
        template<typename R, typename C, typename... A>
        struct FnSignature<R(C::*)(A...) const> { using Params = std::tuple<A...>; };
        template<typename R, typename... A>
        struct FnSignature<R(*)(A...)>          { using Params = std::tuple<A...>; };

        // Non-empty and every element is a SystemParam.
        template<typename Tuple> struct AllParams : std::false_type {};
        template<typename... A> struct AllParams<std::tuple<A...>>
            : std::bool_constant<(sizeof...(A) > 0) && (IsSystemParam_v<A> && ...)> {};

        // Functor whose operator() params are all SystemParams.
        template<typename Fn, typename = void>
        struct IsParamFunctor : std::false_type {};
        template<typename Fn>
        struct IsParamFunctor<Fn, std::void_t<decltype(&Fn::operator())>>
            : AllParams<typename FnSignature<decltype(&Fn::operator())>::Params> {};
        template<typename Fn> inline constexpr bool IsParamFunctor_v = IsParamFunctor<Fn>::value;

        // Free-function pointer type whose params are all SystemParams. Only the
        // R(*)(A...) partial specialization is truthy; anything else is false
        // (no hard error on non-function types).
        template<typename FnType> struct IsParamFreeFunction : std::false_type {};
        template<typename R, typename... A>
        struct IsParamFreeFunction<R(*)(A...)> : AllParams<std::tuple<A...>> {};
        template<typename FnType> inline constexpr bool IsParamFreeFunction_v = IsParamFreeFunction<FnType>::value;
```

Then, after the `Detail` namespace closes but still inside `namespace Astra` (place it AFTER the wrappers, or anywhere at namespace scope — it only needs `Detail::IsParamFunctor_v`), add the concept:

```cpp
    // A callable whose operator() parameters are all SystemParams (View&/Res/
    // ResMut/Commands), non-empty. Disjoint from ContextSystem (whose lone
    // SystemContext& arg is not a SystemParam) and from view-lambdas.
    template<typename T>
    concept ParamFunctor = Detail::IsParamFunctor_v<std::decay_t<T>>;
```

- [ ] **Step 4: Amend `LambdaLike` in `System.hpp`**

In `include/Astra/System/System.hpp`, add the include near the top (after the existing includes, before the concepts):

```cpp
#include "SystemParam.hpp"
```

Then extend the `LambdaLike` concept (System.hpp ~line 107) — add the final clause:

```cpp
    template<typename T>
    concept LambdaLike = requires
    {
        &T::operator();  // Has operator()
    } && !std::invocable<T, Registry&>    // But not a traditional system
      && !ContextSystem<T>                // ...and not a void(SystemContext&) context system
      && !ParamFunctor<T>;                // ...and not a View/Res/ResMut/Commands param-function
```

- [ ] **Step 5: Add the two `AddSystem` overloads + helpers to `SystemScheduler.hpp`**

In `include/Astra/System/SystemScheduler.hpp`, add the two public overloads next to the existing lambda `AddSystem` overloads (after the `ContextSystem<Lambda>` overload, ~line 195):

```cpp
        // Param-function system: a lambda/functor whose params are
        // View<...>&/Res<T>/ResMut<T>/Commands. Access is derived from the
        // params (design §5). Deduces the param pack off operator().
        template<typename Fn>
        requires ParamFunctor<Fn>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Fn&& fn)
        {
            return AddParamSystemImpl(std::forward<Fn>(fn), &std::decay_t<Fn>::operator());
        }

        // Param-function system registered as a free function (non-type template
        // argument), so two same-signature free functions get distinct wrapper
        // types. Deduces the param pack off decltype(FnPtr).
        template<auto FnPtr>
        requires Detail::IsParamFreeFunction_v<decltype(FnPtr)>
        ASTRA_NODISCARD Result<void, SystemError> AddSystem()
        {
            return AddFreeFnParamSystemImpl<FnPtr>(FnPtr);
        }
```

Add the private helpers next to `AddLambdaSystemImpl` (~line 984):

```cpp
        // Deduce Params from a const operator() (the common lambda case).
        template<typename Fn, typename Ret, typename Class, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddParamSystemImpl(Fn&& fn, Ret(Class::*)(Params...) const)
        {
            using Wrapper = FunctionSystemWrapper<std::decay_t<Fn>, Params...>;
            return AddParamSystemInternal<Wrapper>(Wrapper{std::forward<Fn>(fn)});
        }
        // Deduce Params from a non-const (mutable) operator().
        template<typename Fn, typename Ret, typename Class, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddParamSystemImpl(Fn&& fn, Ret(Class::*)(Params...))
        {
            using Wrapper = FunctionSystemWrapper<std::decay_t<Fn>, Params...>;
            return AddParamSystemInternal<Wrapper>(Wrapper{std::forward<Fn>(fn)});
        }
        // Deduce Params from the free-function pointer type; build the NTTP wrapper.
        template<auto FnPtr, typename Ret, typename... Params>
        ASTRA_NODISCARD Result<void, SystemError> AddFreeFnParamSystemImpl(Ret(*)(Params...))
        {
            using Wrapper = FreeFunctionSystemWrapper<FnPtr, Params...>;
            return AddParamSystemInternal<Wrapper>(Wrapper{});
        }
```

Add the internal next to `AddContextSystemInternal` (~line 1060). It is `AddContextSystemInternal` **plus** the `ExtractSystemTraits` line:

```cpp
        // Registers a param-system wrapper as a context system (it needs the
        // per-worker CommandBuffer for its Commands param), harvesting its
        // derived access via ExtractSystemTraits -- the one line the plain
        // context-lambda internal omits (design §5.4).
        template<typename Wrapper>
        ASTRA_NODISCARD Result<void, SystemError> AddParamSystemInternal(Wrapper wrapper)
        {
            if (IsExecuting())
                return Result<void, SystemError>::Err(SystemError::SchedulerExecuting);

            const uint64_t typeId = TypeID<Wrapper>::Hash();
            if (m_systemIndices.Contains(typeId))
                return Result<void, SystemError>::Err(SystemError::AlreadyRegistered);

            Wrapper* instance = new (std::nothrow) Wrapper(std::move(wrapper));
            if (!instance)
                return Result<void, SystemError>::Err(SystemError::AllocationFailed);

            const size_t index = m_systems.size();
            m_systemIndices[typeId] = index;

            SystemMetadata metadata
            {
                .reads = ComponentMask{},
                .writes = ComponentMask{},
                .typeId = static_cast<size_t>(typeId),
                .insertionOrder = index,
                .requiresExclusive = false,
                .segmentIndex = m_currentSegment
            };
            if constexpr (HasSystemTraits_v<Wrapper>)
                ExtractSystemTraits<Wrapper>(metadata);

            m_systems.emplace_back(SystemEntry
            {
                .instance = std::unique_ptr<void, void(*)(void*)>(instance,
                    [](void* ptr) { delete static_cast<Wrapper*>(ptr); }),
                .metadata = metadata,
                .executeContext = [instance](SystemContext& ctx) { (*instance)(ctx); },
            });

            m_needsRebuild = true;
            return Result<void, SystemError>::Ok();
        }
```

- [ ] **Step 6: Run to verify it passes**

Build Debug, run `--gtest_filter=SystemParam.*`. Expected: 8 tests PASS.

- [ ] **Step 7: Commit**

```bash
git add include/Astra/System/SystemParam.hpp include/Astra/System/System.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemParamTest.cpp
git commit -m "feat(system-param): AddSystem overloads + LambdaLike exclusion + trait harvest (Stage 2 task 5)"
```

---

## Task 6: Scheduling, disambiguation, collision & edge-case regressions + 3-config gate

**Files:**
- Test: `tests/System/SystemParamTest.cpp` (final tests)
- No production changes expected. If a test reveals a defect, fix the relevant `SystemParam.hpp`/`SystemScheduler.hpp` code and note it in the commit.

**Interfaces:**
- Consumes: everything from Tasks 1–5, plus `SystemScheduler::GetExecutionPlan()`, `RemoveSystem<Fn>()`, `HasSystem<Fn>()`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/System/SystemParamTest.cpp`:

```cpp
// ---- Task 6: derived access drives grouping; disambiguation intact; free-fn ---
// ---- NTTP collision-free; multi-view / no-view / skip-under-scheduler. --------

namespace
{
    inline void MovePlayers(Astra::View<Position>& v) { v.ForEach([](Position& p){ p.x += 1.0f; }); }
    inline void MoveEnemies(Astra::View<Position>& v) { v.ForEach([](Position& p){ p.x += 1.0f; }); }  // same signature as MovePlayers
    inline void ReadsHealthRes(Astra::Res<Health>)        {}   // resource reader
    inline void WritesHealthRes(Astra::ResMut<Health>)    {}   // resource writer (conflicts with reader)
}

TEST(SystemParam, DisjointParamSystemsShareAGroupConflictingOnesDoNot)
{
    // Two systems touching different resources -> same parallel group.
    Astra::SystemScheduler s1;
    ASSERT_TRUE(s1.AddSystem<ReadsHealthRes>().IsOk());
    ASSERT_TRUE(s1.AddSystem([](Astra::Res<Physics>){}).IsOk());   // different resource
    const auto& plan1 = s1.GetExecutionPlan();
    ASSERT_EQ(plan1.size(), 1u);
    EXPECT_EQ(plan1[0].size(), 2u);                                 // grouped together

    // A resource reader + writer of the SAME resource -> serialized (2 groups).
    Astra::SystemScheduler s2;
    ASSERT_TRUE(s2.AddSystem<ReadsHealthRes>().IsOk());
    ASSERT_TRUE(s2.AddSystem<WritesHealthRes>().IsOk());
    const auto& plan2 = s2.GetExecutionPlan();
    EXPECT_EQ(plan2.size(), 2u);                                    // separate groups
}

TEST(SystemParam, TwoSameSignatureFreeFunctionsBothRegisterAndRun)
{
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<MovePlayers>().IsOk());
    ASSERT_TRUE(s.AddSystem<MoveEnemies>().IsOk());   // MUST NOT be AlreadyRegistered
    EXPECT_EQ(s.Size(), 2u);

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 2.0f);   // both ran

    s.RemoveSystem<MovePlayers>();
    EXPECT_FALSE(s.HasSystem<MovePlayers>());
    EXPECT_TRUE(s.HasSystem<MoveEnemies>());
}

TEST(SystemParam, DisambiguationViewLambdaAndContextLambdaStillRoute)
{
    // A view-lambda, a context lambda, and a param-system coexist and each runs.
    Astra::Registry reg;
    Astra::Entity e = reg.CreateEntity<Position>();
    reg.GetComponent<Position>(e)->x = 0.0f;

    Astra::SystemScheduler s;
    int viewRuns = 0, ctxRuns = 0;
    ASSERT_TRUE(s.AddSystem([&](Astra::Entity, Position& p){ p.x += 1.0f; ++viewRuns; }).IsOk());   // view-lambda
    ASSERT_TRUE(s.AddSystem([&](Astra::SystemContext&){ ++ctxRuns; }).IsOk());                       // context lambda
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& v){ v.ForEach([](Position& p){ p.x += 10.0f; }); }).IsOk()); // param-system

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(viewRuns, 1);
    EXPECT_EQ(ctxRuns, 1);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(e)->x, 11.0f);   // view-lambda +1, param-system +10
}

TEST(SystemParam, MultiViewAndNoViewSystemsCompileAndRun)
{
    Astra::Registry reg;
    Astra::Entity a = reg.CreateEntity<Position>();
    Astra::Entity b = reg.CreateEntity<Velocity>();
    reg.SetResource(Health{0, 0});

    Astra::SystemScheduler s;
    // Two views in one system.
    ASSERT_TRUE(s.AddSystem([](Astra::View<Position>& vp, Astra::View<Velocity>& vv)
    {
        vp.ForEach([](Position& p){ p.x += 1.0f; });
        vv.ForEach([](Velocity& v){ v.dx += 1.0f; });
    }).IsOk());
    // No view at all: pure resource + commands.
    ASSERT_TRUE(s.AddSystem([](Astra::ResMut<Health> h, Astra::Commands){ h->current += 5; }).IsOk());

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_FLOAT_EQ(reg.GetComponent<Position>(a)->x, 1.0f);
    EXPECT_FLOAT_EQ(reg.GetComponent<Velocity>(b)->dx, 1.0f);
    EXPECT_EQ(reg.GetResource<Health>()->current, 5);
}
```

- [ ] **Step 2: Run to verify they fail (or reveal defects)**

Build Debug. Expected: the new tests fail to compile only if a symbol is missing; otherwise they run. Any assertion failure is a real defect to fix in `SystemParam.hpp`/`SystemScheduler.hpp` before proceeding. Iterate until the four new tests pass.

- [ ] **Step 3: Full Debug suite green**

Build Debug, run the WHOLE suite (no filter):
Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe`
Expected: all tests pass (Stage-2 count = 12 `SystemParam.*` plus the pre-existing suite). No regressions in `SystemScheduler.*` / `SystemContext.*` / `ViewEnrichment.*`.

- [ ] **Step 4: Release + Dist gate**

Build and run Release, then Dist:
Run: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Release -p:Platform=x64 -m`
Run: `bin/Release-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemParam.*:SystemScheduler.*:SystemContext.*:ViewEnrichment.*`
Run: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Dist -p:Platform=x64 -m`
Run: `bin/Dist-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=SystemParam.*:SystemScheduler.*:SystemContext.*:ViewEnrichment.*`
Expected: green in both configs. (Release/Dist may compile out Debug asserts; behavior must be identical.)

- [ ] **Step 5: Commit**

```bash
git add tests/System/SystemParamTest.cpp
git commit -m "test(system-param): scheduling/disambiguation/collision/edge regressions + 3-config gate (Stage 2 task 6)"
```

---

## Self-Review (completed during planning)

**Spec coverage:**
- §3.1 `Res`/`ResMut`/`Commands` → Task 1. §3.2 two spellings → Tasks 4 (wrappers) + 5 (overloads). §3.3 strict param contract → enforced structurally (`IsSystemParam` only matches `View<…>&`; a by-value `View` is not a param — Task 2 test asserts `IsSystemParam_v<View<Position>> == false`, so such a system fails the concept and gets a clean "no matching AddSystem" error rather than a mis-bind; a plain `static_assert` message is not added because the concept already rejects it cleanly). §4.1 `ParamAccess` → Task 2. §4.2 binder Run/BuildParam/cache → Task 4. §4.3 wrappers → Task 4. §5 disambiguation/registration → Task 5. §6 skip-and-log → Task 4 (unit) + Task 6 (under scheduler). §7 edge cases → Task 6. §8 tests → Tasks 1–6. §9 files → File Structure. §10 build → Global Constraints. §11 success criteria → Task 6 gate.
- **Note on §3.3 `static_assert`:** the spec calls for a `static_assert` on a mis-spelled param (by-value View, `const Res&`). The concept-based rejection produces an overload-resolution failure instead of a targeted message. This is a deliberate, equivalent-safety simplification (no silent mis-bind). If a friendlier diagnostic is wanted, a future task can add a `static_assert` in a fallback `AddSystem` overload; not required for correctness.

**Placeholder scan:** none — every step has complete code or an exact command.

**Type consistency:** `SystemParamBinder<Params...>`, `FunctionSystemWrapper<Fn, Params...>`, `FreeFunctionSystemWrapper<auto FnPtr, Params...>`, `ParamAccess::{Reads,Writes,ResReads,ResWrites}`, `IsSystemParam_v`, `ParamFunctor`, `IsParamFreeFunction_v`, `AddParamSystemImpl`, `AddFreeFnParamSystemImpl`, `AddParamSystemInternal` — names are identical across all tasks and match the spec.

**Known risk (flagged in spec §9):** the `System.hpp` → `SystemParam.hpp` include order. Task 1 Step 1 verifies no cycle before any code is written; Task 5 Step 4 adds the include. If a cycle surfaces, the fallback is to hoist just the `ParamFunctor` concept (and its `Detail` deps) into a tiny `SystemParamFwd.hpp` that `System.hpp` includes, leaving the wrappers in `SystemParam.hpp`.
