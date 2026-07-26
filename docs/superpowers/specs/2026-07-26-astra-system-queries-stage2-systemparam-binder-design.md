# Astra System Queries — Stage 2: SystemParam Binder (design)

**Date:** 2026-07-26
**Status:** Design approved; ready for implementation planning
**Program:** Bevy-style system queries — 3-stage ergonomics program (Stage 1 View enrichment SHIPPED @ `638aa12`; **this is Stage 2**; Stage 3 = change detection).
**North star:** out-perform flecs and EnTT *and* match Bevy's ergonomics. This stage delivers the ergonomics-core pillar: register a plain function whose parameters *are* the query/resource/command surface, and have the scheduler's access declaration be *derived from the body's parameters* rather than hand-declared (a correctness property, not just sugar — declared access can no longer drift from used access).

---

## 1. Goal & value

Astra's common single-unfiltered-query system is already a clean one-liner via the shipped view-lambda form:

```cpp
scheduler.AddSystem([](Entity e, Position& p, const Velocity& v) { p += v; });
```

The value of Stage 2 is removing the **ergonomic cliff** that appears the moment a system needs a filter, a resource, `Commands`, or a second query — today that forces a hand-written struct with **hand-declared** `Reads<…>`/`Writes<…>`/`ReadsResources<…>` traits that can silently **drift** from the body (declare `Reads<Velocity>` but write it → the scheduler parallelizes a data race, uncaught). Stage 2 lets you write:

```cpp
scheduler.AddSystem([](View<Position, const Velocity>& v, Res<Config> cfg, Commands cmds) {
    v.ForEach([&](Position& p, const Velocity& vel) { p += vel * cfg->dt; });
    cmds->DestroyEntity(deadEntity);
});
```

and the scheduler's read/write/resource masks are **derived from the parameter list** — declared access == used access, automatically.

### In scope
- New `SystemParam` vocabulary: `Res<T>`, `ResMut<T>`, `Commands`.
- Two registration spellings: lambda/functor (by value) and free function (as a non-type template argument).
- Auto-derived access: union of each parameter's access, consumed by the **existing** `ExtractSystemTraits` → `SystemMetadata` → scheduler grouping.
- Per-frame parameter construction with a persistent per-`View` cache (IM-23 parity, generalized to N views).
- Missing-resource policy: **skip the system for the frame and log once** (see §6).

### Out of scope (deferred)
- **Change detection** — `Changed<T>`/`Added<T>`/`Ref<T>`/`Mut<T>`, per-component change ticks. This is Stage 3 and touches chunk storage + serialization.
- **Class/struct param-systems with explicit ordering** — a `struct S { using Before = …; void operator()(View<…>&, Commands); };` form that can carry `Before`/`After`/`AmbiguousWith`/`Exclusive`. Not needed for the ergonomic-cliff value case; the existing class-typed `SystemTraits` registration path remains available for ordering. May be a Stage-2 follow-up.
- **Relations `With<T>`** — still a hard `static_assert` reject inherited from Stage 1 (no new surface).

---

## 2. Existing machinery this stage consumes (no changes needed)

- **`ViewAccess<View<Args...>>`** (`View.hpp`, shipped Stage 1, currently inert): `::Reads`/`::Writes` type-lists and `ReadMask()`/`WriteMask()`. Classifies View args exactly as needed: `const T` → read, `T` → write, `Optional<T>` by pointee const-ness, `IncludeDisabled<T>` → write, `With`/`Not`/`Any`/`OneOf` → nothing.
- **`ExtractSystemTraits<T>`** (`SystemScheduler.hpp`): harvests `T::ReadsComponents` / `T::WritesComponents` / `T::ReadsResourceTypes` / `T::WritesResourceTypes` into `SystemMetadata.{reads,writes,resourceReads,resourceWrites}` — gated on `HasSystemTraits_v<T>`. **The binder exposes exactly these typedefs, so this path fills the masks unchanged.**
- **`SystemContext`** (`SystemContext.hpp`): bundles `GetRegistry()` (for views + resources) and `Commands()` (per-worker deferred `CommandBuffer`, re-stamping the sort key each call).
- **`DispatchSystem` / `SystemExecutor`**: already parallel-dispatches `void(SystemContext&)` context systems, handing each worker its own per-thread buffer via `commandBuffer->GetThreadBuffer()`. **A param-system is registered as a context system, so `Commands` works for free and the system parallelizes on its harvested masks.**
- **Resources**: `Registry::GetResource<T>() → T*`, keyed by `TypeID<T>::Value()` (dense `ComponentID`, same space the resource masks use).
- **`View` construction**: `Registry::CreateView<Args...>() → View<Args...>(m_archetypeManager, m_workScheduler)`.

---

## 3. User-facing API (`include/Astra/System/SystemParam.hpp`, new)

### 3.1 SystemParam types

```cpp
namespace Astra {

// Read-only resource handle. Contributes T to resourceReads.
template<typename T>
class Res {
    const T* m_ptr;
public:
    explicit Res(const T* p) noexcept : m_ptr(p) {}
    const T& operator*()  const noexcept { return *m_ptr; }
    const T* operator->() const noexcept { return  m_ptr; }
    const T& Get()        const noexcept { return *m_ptr; }
};

// Mutable resource handle. Contributes T to resourceWrites.
template<typename T>
class ResMut {
    T* m_ptr;
public:
    explicit ResMut(T* p) noexcept : m_ptr(p) {}
    T& operator*()  const noexcept { return *m_ptr; }
    T* operator->() const noexcept { return  m_ptr; }
    T& Get()        const noexcept { return *m_ptr; }
};

// Deferred structural-change handle. Contributes NO scheduling access
// (per-worker buffer, deterministic flush at sync points → parallel-safe).
class Commands {
    SystemContext& m_ctx;
public:
    explicit Commands(SystemContext& ctx) noexcept : m_ctx(ctx) {}
    // Each call re-stamps the sort key (via SystemContext::Commands()), so N
    // statements record N deterministically-ordered commands. Do NOT cache the
    // returned reference across statements.
    CommandBuffer* operator->() const { return &m_ctx.Commands(); }
    CommandBuffer& Get()        const { return  m_ctx.Commands(); }
};

}
```

Resource handles are **never null in the body**: the binder's skip-and-log gate (§6) returns before invoking if any declared resource is absent, so `operator*`/`operator->` are always valid inside the function.

### 3.2 Registration spellings

```cpp
// (1) lambda / functor — headline form, registered by value.
scheduler.AddSystem([](View<Position, const Velocity>& v, Res<Config> cfg, Commands cmds) { … });

// (2) free function — registered as a non-type template argument.
void MovePlayers(View<Position, const Velocity>& v, ResMut<Score> s) { … }
scheduler.AddSystem<MovePlayers>();
```

**Why free functions are NTTP, not by-value.** Systems are keyed by `TypeID<Wrapper>::Hash()`. Two by-value free functions with the *identical* signature (`void(View<Position>&, Commands)`) would produce the *same* wrapper type and the second registration would be falsely rejected as `AlreadyRegistered`. Carrying the function pointer as a non-type template parameter makes each function its own wrapper type → collision-free, and symmetric with `RemoveSystem<MovePlayers>()` / `HasSystem<MovePlayers>()`. Lambdas keep the natural by-value spelling because each closure is already a distinct type.

### 3.3 Parameter-spelling contract (strict)

Mirrors `LambdaSystemWrapper`'s existing "exact spelling or a clear compile error" philosophy (it already rejects pointer/by-value component params that would mis-infer as writes):

| Parameter | Meaning | Access harvested |
|---|---|---|
| `View<…>&` (non-const lvalue ref) | the persistent cached view is handed back | components via `ViewAccess` (`const T`→read, `T`→write) |
| `Res<T>` (by value) | read-only resource | `T` → resourceReads |
| `ResMut<T>` (by value) | mutable resource | `T` → resourceWrites |
| `Commands` (by value) | deferred structural changes | none |

- A by-value `View`, a `const Res<T>&`, an rvalue-ref param, or any unrecognized parameter type → **`static_assert`**, never a silent mis-bind.
- Multiple `View` params are allowed (iterate A while random-accessing B).
- A system with no `View` at all (pure `Res`/`ResMut`/`Commands`) is valid.
- `Res<T>` and `ResMut<T>` over the same `T` on one system, or a bare data-conflict, are the user's responsibility; the scheduler will serialize appropriately but no special diagnostic is added here.

---

## 4. The binder (`SystemParam.hpp`, new)

### 4.1 Per-parameter access classification

```cpp
namespace Astra::Detail {

template<typename P> struct ParamAccess {                        // default (Commands): nothing
    using Reads = std::tuple<>;    using Writes = std::tuple<>;
    using ResReads = std::tuple<>; using ResWrites = std::tuple<>;
};
template<typename... A> struct ParamAccess<View<A...>&> {
    using Reads  = typename ViewAccess<View<A...>>::Reads;       // consumes Stage 1
    using Writes = typename ViewAccess<View<A...>>::Writes;
    using ResReads = std::tuple<>; using ResWrites = std::tuple<>;
};
template<typename T> struct ParamAccess<Res<T>> {
    using Reads = std::tuple<>; using Writes = std::tuple<>;
    using ResReads = std::tuple<T>; using ResWrites = std::tuple<>;
};
template<typename T> struct ParamAccess<ResMut<T>> {
    using Reads = std::tuple<>; using Writes = std::tuple<>;
    using ResReads = std::tuple<>; using ResWrites = std::tuple<T>;
};

} // namespace Astra::Detail
```

This mirrors `Query.hpp`'s `ArgAccess`/`AccessReads`/`AccessWrites` shape one-to-one.

### 4.2 Shared binder base

```cpp
template<typename... Params>
class SystemParamBinder {
public:
    // Harvested access — makes HasSystemTraits_v<Wrapper> true, so the existing
    // ExtractSystemTraits fills the metadata masks with no scheduler change.
    using ReadsComponents     = /* tuple_cat ParamAccess<Params>::Reads...     */;
    using WritesComponents    = /* tuple_cat ParamAccess<Params>::Writes...    */;
    using ReadsResourceTypes  = /* tuple_cat ParamAccess<Params>::ResReads...  */;
    using WritesResourceTypes = /* tuple_cat ParamAccess<Params>::ResWrites... */;
    static constexpr bool HasTraits = true;
    static constexpr bool RequiresExclusive = false;

protected:
    // Rebind caches if the registry changed, run the resource-presence gate,
    // then invoke `invoke(BuildParam<Params>(ctx, reg)...)`.
    template<typename Invoke>
    void Run(SystemContext& ctx, Invoke&& invoke);

private:
    // One std::optional<View<A...>> per View param; std::monostate for non-View
    // params (so the tuple index lines up with Params). IM-23, generalized to N views.
    std::tuple</* ViewSlot<Params>... */> m_views;
    Registry* m_viewRegistry = nullptr;
    bool m_loggedMissing = false;   // skip-and-log latch (see §6)
};
```

- **`BuildParam<P, I>(ctx, reg, resPtrs)`**:
  - `View<A...>&`: if `std::get<I>(m_views)` is empty (reset on registry change), `emplace(reg.CreateView<A...>())`; return `*std::get<I>(m_views)` (lvalue ref → the persistent cache).
  - `Res<T>`: `Res<T>{ static_cast<const T*>(resPtrs[…]) }`.
  - `ResMut<T>`: `ResMut<T>{ static_cast<T*>(resPtrs[…]) }`.
  - `Commands`: `Commands{ ctx }`.
- **Registry rebind** — identical to `LambdaSystemWrapper::ExtractAndExecute`: if `m_viewRegistry != &reg`, reset every view slot and set `m_viewRegistry = &reg`.
- **Resource presence gate** — before invoking, a `constexpr` fold calls `reg.GetResource<T>()` for each `Res`/`ResMut` param to confirm non-null (see §6). `BuildParam` re-calls `GetResource<T>()` when constructing each handle; `GetResource` is an O(1) sparse lookup, so the extra call per resource per frame is negligible and no pointer caching is attempted (this keeps reference-preservation for the `View<…>&` param straightforward — the alternative of building all params into a tuple before the null-check would decay or dangle the view reference).

### 4.3 Leaf wrappers

```cpp
template<typename Fn, typename... Params>                 // lambda / functor
class FunctionSystemWrapper : public SystemParamBinder<Params...> {
    Fn m_fn;
public:
    explicit FunctionSystemWrapper(Fn fn) : m_fn(std::move(fn)) {}
    void operator()(SystemContext& ctx) {
        this->Run(ctx, [&](auto&&... p) { m_fn(static_cast<decltype(p)>(p)...); });
    }
};

template<auto FnPtr, typename... Params>                  // free function (NTTP → unique type)
class FreeFunctionSystemWrapper : public SystemParamBinder<Params...> {
public:
    void operator()(SystemContext& ctx) {
        this->Run(ctx, [&](auto&&... p) { FnPtr(static_cast<decltype(p)>(p)...); });
    }
};
```

Both are context systems (`void operator()(SystemContext&)`), so they populate `SystemEntry.executeContext`, are dispatched through `DispatchSystem` with a per-worker buffer, and are grouped/parallelized purely by the harvested masks.

---

## 5. Disambiguation & registration wiring (all additive)

### 5.1 Classification & concept

```cpp
namespace Astra::Detail {
    template<typename P> struct IsSystemParam : std::false_type {};
    template<typename... A> struct IsSystemParam<View<A...>&> : std::true_type {};
    template<typename T>    struct IsSystemParam<Res<T>>      : std::true_type {};
    template<typename T>    struct IsSystemParam<ResMut<T>>   : std::true_type {};
    template<>              struct IsSystemParam<Commands>    : std::true_type {};

    // Deduce Params from &Fn::operator() (const and non-const), require the
    // list to be non-empty and every element to satisfy IsSystemParam.
    template<typename Fn> /* … */ inline constexpr bool IsParamFunctor_v = /* … */;

    // Same predicate for a free-function type: deduce Params from
    // decltype(FnPtr) (i.e. Ret(*)(Args...)), require non-empty and all
    // IsSystemParam. Used to constrain the NTTP AddSystem overload (§5.3).
    template<typename FnType> /* … */ inline constexpr bool IsParamFreeFunction_v = /* … */;
}

template<typename T>
concept ParamFunctor = Detail::IsParamFunctor_v<std::decay_t<T>>;
```

`ParamFunctor` and `ContextSystem` are naturally disjoint: a param-functor's arguments are all `SystemParam`s (never a lone `SystemContext&`), and a context system's single argument (`SystemContext&`) is not a `SystemParam`. A view-lambda `(Entity, Position&, …)` is not a `ParamFunctor` (its args are not `SystemParam`s), so it stays on the `LambdaLike` path.

### 5.2 `LambdaLike` amendment (`System.hpp`)

```cpp
template<typename T>
concept LambdaLike = requires { &T::operator(); }
    && !std::invocable<T, Registry&>
    && !ContextSystem<T>
    && !ParamFunctor<T>;     // NEW — keeps a param-functor out of the view-lambda path
```

Free functions have no `operator()`, so they already fail `LambdaLike`; only the functor form needs excluding. This mirrors exactly the existing `!ContextSystem<T>` clause and its rationale.

### 5.3 New `AddSystem` overloads (`SystemScheduler.hpp`)

```cpp
// lambda / functor
template<typename Fn>
requires ParamFunctor<Fn>
ASTRA_NODISCARD Result<void, SystemError> AddSystem(Fn&& fn) {
    return AddParamSystemImpl(std::forward<Fn>(fn), &std::decay_t<Fn>::operator());
}

// free function as NTTP; require all deduced params to be SystemParams so a
// non-param free function does not match this overload.
template<auto FnPtr>
requires Detail::IsParamFreeFunction_v<decltype(FnPtr)>
ASTRA_NODISCARD Result<void, SystemError> AddSystem() {
    using Wrapper = /* FreeFunctionSystemWrapper<FnPtr, Params…> deduced from decltype(FnPtr) */;
    return AddParamSystemInternal<Wrapper>(Wrapper{});
}
```

`AddParamSystemImpl(fn, &operator())` pattern-matches `Ret(Class::*)(Args...) [const]` to recover `Args…`, builds `FunctionSystemWrapper<std::decay_t<Fn>, Args…>`, and forwards to the internal — exactly parallel to the existing `AddLambdaSystemImpl`.

### 5.4 New private internal (`SystemScheduler.hpp`)

`AddParamSystemInternal<Wrapper>(Wrapper)` is a copy of the existing `AddContextSystemInternal` with **one added line**:

```cpp
if constexpr (HasSystemTraits_v<Wrapper>)
    ExtractSystemTraits<Wrapper>(metadata);   // ← the context-lambda internal omits this
```

then it stores `.executeContext = [instance](SystemContext& ctx){ (*instance)(ctx); }`. This single call is the entire reason the harvested masks reach the scheduler; `ExtractSystemTraits` itself is unchanged.

### 5.5 Overload-resolution safety

- `AddSystem<MovePlayers>()` (NTTP, no function args) cannot match the by-value `AddSystem(Fn&&)` / view-lambda / context-lambda overloads (they need a function argument) nor the `AddSystem<System T>(Args&&…)` overload (its `T` must satisfy `System`, and a free function is not invocable with `Registry&`).
- A random free function `void f(int)` given as `AddSystem<f>()` fails `IsParamFreeFunction_v` (params not all `SystemParam`) → no matching overload → clean compile error, not a malformed wrapper.

**Net change to existing files:** `System.hpp` +1 concept clause; `SystemScheduler.hpp` +2 overloads +1 internal (+ the `AddParamSystemImpl` signature-deduction helper). `ViewAccess`, `SystemContext`, `SystemExecutor`, `BuildExecutionPlan`, `SystemMetadata`, `ExtractSystemTraits` — **untouched**.

---

## 6. Missing-resource policy: skip-and-log

**Decision:** when a param-system declares `Res<T>`/`ResMut<T>` but the resource is absent from the Registry at `Execute()` time, the binder **skips invoking that system for the frame and logs once**. Rationale: matches Astra's uniform-graceful-misuse policy (log, never abort); the common case (resources set at startup) never hits it; and skipping means the body never observes a null resource, so `Res`/`ResMut` deref is always valid.

Mechanics in `SystemParamBinder::Run`:
1. `constexpr` fold over `Params`: for each `Res<T>`/`ResMut<T>`, `allPresent = (reg.GetResource<T>() != nullptr && …)`. Non-resource params contribute `true`.
2. If `!allPresent`: if `!m_loggedMissing`, `ASTRA_LOG_ERROR("param-system skipped: resource … absent")` and set `m_loggedMissing = true`; **return without invoking**.
3. If `allPresent`: clear `m_loggedMissing` (so a later disappearance logs again), build all params (`BuildParam` re-calls `GetResource<T>()` — O(1)), invoke the target.

The latch keeps a persistently-missing resource from spamming the log every frame while still reporting the first occurrence and any recurrence after recovery. The system's declared access is still registered, so its scheduling slot/grouping is unaffected by a skipped frame.

---

## 7. Edge cases

- **Empty match set** — `View::ForEach` no-ops; nothing special.
- **Registry switch between `Execute()` calls** — every view slot reset and `m_viewRegistry` rebound (IM-23 parity with `LambdaSystemWrapper`).
- **Resource read/write across systems** — a `ResMut<X>` writer and a `Res<X>` reader serialize via the existing `resourceReads`/`resourceWrites` masks and `BuildExecutionPlan`'s resource-conflict test; two `Res<X>` readers run concurrently (subject to `ResourceTraits<X>::ConcurrentReadSafe`, honored by `ExtractResourceReadMask`).
- **Duplicate registration** — two distinct lambdas are distinct closure types → both register (view-lambda parity). The same free function via `AddSystem<F>()` twice → same wrapper type → second returns `AlreadyRegistered` (correct: it *is* the same system).
- **Relations** — a `View<…>&` param over a relations query inherits Stage 1's hard `static_assert` reject of `With<T>` on relations; no new behavior.
- **`Commands` without any structural change** — harmless; `operator->` is only invoked if the body records something.

---

## 8. Testing (`tests/System/SystemParamTest.cpp`, new)

Reuse `Astra::Test::*` component types (`tests/TestComponents.hpp`) to stay under the 128-TypeID ceiling; do not mint fresh generic-named component types.

1. **Access harvest** — a param-system's `SystemMetadata` masks equal the hand-declared equivalent: component reads/writes from View const-ness; `Res`→`resourceReads`; `ResMut`→`resourceWrites`; `Commands`→no bits.
2. **Scheduling** — two disjoint param-systems share a parallel group; a `ResMut<X>` writer + `Res<X>` reader land in different groups; a component write/read pair serializes. (Assert via `GetExecutionPlan()`.)
3. **Execution** — body runs, iterates the cached view mutating components, and a `Commands` deferral applies after the segment flush.
4. **Skip-and-log** — absent resource → system skipped (a side-effect flag proves the body was not entered), error logged once (latch verified across ≥2 frames), then the body resumes once the resource is set.
5. **View caching** — the same `View` instance address is reused across `Execute()` calls; rebuilt after a registry switch.
6. **Free-fn NTTP form** — two same-signature free functions both register (no false `AlreadyRegistered`) and both run; `RemoveSystem<F>()` / `HasSystem<F>()` work.
7. **Disambiguation** — a view-lambda `(Entity, Position&)`, a context lambda `(SystemContext&)`, and a param-system coexist and each routes to its correct overload (compiles and behaves).
8. **Multiple views / no view** — a two-`View` system and a pure-`Res`+`Commands` system both compile, harvest correct access, and run.

Run the full suite in all three configs (Debug/Release/Dist).

---

## 9. Files

**New**
- `include/Astra/System/SystemParam.hpp` — `Res`/`ResMut`/`Commands`; `Detail::ParamAccess`, `Detail::IsSystemParam`, `Detail::IsParamFunctor_v`, `Detail::IsParamFreeFunction_v`; `SystemParamBinder`; `FunctionSystemWrapper`; `FreeFunctionSystemWrapper`.
- `tests/System/SystemParamTest.cpp`.

**Modified**
- `include/Astra/System/System.hpp` — add `!ParamFunctor<T>` to `LambdaLike`. (Needs `SystemParam.hpp`'s `ParamFunctor` visible; arrange include order so `LambdaLike` sees it — likely `System.hpp` includes `SystemParam.hpp`, which forward-declares/uses `View`.)
- `include/Astra/System/SystemScheduler.hpp` — 2 `AddSystem` overloads, `AddParamSystemImpl` deduction helper, `AddParamSystemInternal` internal.
- `include/Astra/Astra.hpp` — include `SystemParam.hpp`.
- premake regen (`premake5 vs2022`) for the new test `.cpp`.

**Untouched (consumed as-is):** `View.hpp` (`ViewAccess`), `SystemContext.hpp`, `SystemExecutor.hpp`, `SystemMetadata.hpp`, and `ExtractSystemTraits`/`BuildExecutionPlan` in `SystemScheduler.hpp`.

### Include-order note (implementation risk to resolve first)
`LambdaLike` lives in `System.hpp`; `ParamFunctor` needs `Res`/`ResMut`/`Commands`/`View`. `Commands` needs `SystemContext`. Today `SystemContext.hpp` includes `Registry.hpp`; `System.hpp` includes `Registry.hpp` and `SystemContext.hpp`. Plan: `SystemParam.hpp` includes `View.hpp` (for `View`/`ViewAccess`) and `SystemContext.hpp` (for `Commands`), and `System.hpp` includes `SystemParam.hpp` before defining `LambdaLike`. Confirm no cycle (`SystemParam.hpp` must not include `System.hpp`). The wrappers reference `SystemContext` and `Registry::CreateView`/`GetResource`, all available transitively. Resolve/verify this graph as task 1 of the plan.

---

## 10. Build & verify

Build is MSBuild, not make:
```
premake5 vs2022            # only when adding the new .cpp
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" \
    Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m
bin/<Cfg>-windows-x86_64/AstraTest/AstraTest.exe
```
IDE/clangd diagnostics (STL1000, missing Mosaic/gtest headers) are false positives — judge only by MSBuild. Green bar in all three configs is the gate.

---

## 11. Success criteria

- A lambda param-system and a free-function (`AddSystem<F>()`) param-system both register, harvest access identical to the hand-declared struct, run their bodies, iterate cached views, and apply `Commands` deferrals at the flush.
- The scheduler groups/serializes param-systems purely from derived access (verified against `GetExecutionPlan()`), with **no** change to `ExtractSystemTraits`, `BuildExecutionPlan`, `SystemExecutor`, or `SystemMetadata`.
- A missing declared resource skips the system and logs once, then recovers.
- Existing view-lambda, context-lambda, `System<T>`, and class-typed `ContextSystem<T>` registrations are unaffected (disambiguation intact).
- Full test suite green in Debug/Release/Dist.
