# Theme E — Type-Identity Collision Detection — Design

> Remediation of the 2026-07-11 Astra full-codebase review, **Theme E** (§Theme E; Critical #1 in `06-core-foundation.md`). Confirmed on MSVC & Clang.

## Problem

Type identity flows: `Detail::TypeNameInternal<T>()` (compiler pretty-name) → `Detail::TypeHash<T>()` (`XXHash64` of the name) → `TypeContext::GetOrAssignComponentID(hash, name)` → a dense `ComponentID` (`ComponentMask` bit index).

Two **distinct** types with the same unqualified name declared in **anonymous namespaces** in different translation units render **byte-identically** in the compiler pretty-name on both MSVC (`` `anonymous namespace'::Foo ``) and Clang/GCC (`(anonymous namespace)::Foo`) — neither embeds a per-TU disambiguator in the *pretty-printed* name (only the *mangled* symbol name does). So they produce the identical `XXHash64`, and `GetOrAssignComponentID` returns the **same `ComponentID`** for two types that are not the same type.

Impact: the second type silently inherits the first type's `ComponentDescriptor` (size, alignment, ctor/dtor/copy function pointers). When the two types differ in size or have non-trivial special members, applying one type's descriptor to the other's storage is **genuine memory corruption / type confusion**, not merely a logical mislabel. This is a realistic C++ idiom (per-`.cpp` "private" tag/marker components kept anonymous).

Both existing collision guards are **defeated** because they compare *name strings*, which are byte-identical:
- `Core/TypeContext.hpp:75-78` — a Debug-only `ASTRA_ASSERT(m_names[id] == name, ...)`; the names match, so it reads as a legitimate re-registration and never fires. (Also compiled out in Release/Dist.)
- `Component/ComponentRegistry.hpp:168-173` — a Debug-only `ASTRA_ASSERT(currentName == existingName, ...)`; same flaw.

## The hard constraint (why this is detection, not a redesign)

`TypeID<T>::Hash()` is contractually **stable across platforms and recompiles** (`TypeID.hpp:241-246`) and is the **serialization identity**. Every discriminator that is unique-per-anonymous-type — the mangled name, a per-type `static` sentinel address, a `__COUNTER__` token, RTTI `type_info` — is *either* not stable across runs/platforms, *or* not consistent across modules (which would break the plugin/shared-`TypeContext` model: `SetTypeContext` + pending-registration draining), *or* unavailable in RTTI-off shipping builds. So two same-named anonymous types **cannot** be given distinct *stable* IDs without breaking serialization stability or the shared-context contract.

Therefore a "make them just work" identity redesign is **infeasible**. The realistic fix — and what comparable libraries (EnTT) do — is to **detect the collision and fail loudly + gracefully instead of silently corrupting**, and document the constraint. This is a bounded, single-subsystem change with **no wire-format change**.

## Enabling facts (verified against the live tree)

- The `AstraTest` project builds with **`rtti "on"` in all three configs** (GoogleTest requires it — `premake5.lua:112/120/128`), while the library-consumer project builds **`rtti "off"`** (`premake5.lua:224/232/…`) — the real shipping posture. So `typeid(T)` is usable and *testable* in the test binary, but cannot be relied on for the shipping guarantee.
- A **structural discriminator** (`sizeof`/`alignof`/triviality) is RTTI-free, all-config, and cross-module-consistent, and distinguishes exactly the *memory-corrupting* collisions.
- `GetOrAssignComponentID` has **one** real library caller — `Detail::TypeIDStorage<T>::Value()` (`TypeID.hpp:214-219`) — plus direct 2-arg calls in `tests/Core/TypeContextTest.cpp`.
- `ASTRA_ENSURE_ALWAYS(cond, message)` exists (`Core/Assert.hpp:60`) — an all-config, recoverable "report and continue" guard (host-escalatable via the assert handler).
- There is **no existing RTTI usage** in `include/`, so any `typeid` must be strictly `#if defined(__cpp_rtti) || defined(_CPPRTTI)`-guarded; the structural check is the load-bearing all-config path.

## Decisions (user-approved, 2026-07-20)

1. **Scope = real all-config detection** (over documentation-only): thread a per-type discriminator through the single `GetOrAssignComponentID` chokepoint so a genuine collision is detected and fails in *all* configs. Structural discriminator as the RTTI-free floor; RTTI `type_info` cross-check where available.
2. **Failure mode = refuse + loud report, all configs** (over hard-fatal abort): return `INVALID_COMPONENT` for the colliding type and report via `ASTRA_ENSURE_ALWAYS` + `ASTRA_LOG_ERROR`. Mirrors the id-exhaustion guard directly above it in the same function; matches Astra's uniform-graceful misuse policy (Theme B B1); host can escalate to fatal via the assert handler; directly unit-testable.
3. **Keep the stable name-hash identity and the wire format unchanged.** No `BINARY_FORMAT_VERSION` bump.

## Design

### 1. The discriminator (`TypeIdentity`)

A small POD capturing what the pretty-name cannot, computed from `T`:

- **Structural (always present, all configs, RTTI-free):** `uint32_t size` (`sizeof(T)`), `uint32_t align` (`alignof(T)`), and triviality bits (`std::is_trivially_copyable_v<T>`, `std::is_trivially_destructible_v<T>`, `std::is_empty_v<T>`). Cross-module-consistent.
- **RTTI (only under `#if defined(__cpp_rtti) || defined(_CPPRTTI)`):** `const std::type_info* rtti = &typeid(T)`. Compared via `type_info::operator==` (the ABI-correct comparison that disambiguates anonymous namespaces *and* stays consistent for the same real type across modules) — **never** by pointer identity or `hash_code`, which are not cross-module-safe.

Computed in `Detail::TypeIDStorage<T>::Value()` (which has `T`) and passed into `GetOrAssignComponentID`.

### 2. Detection at the chokepoint

Extend `TypeContext::GetOrAssignComponentID`:

```
ComponentID GetOrAssignComponentID(uint64_t hash, std::string_view name, TypeIdentity identity = {});
```

The default `identity = {}` (an "unspecified" state, e.g. `size == 0 && rtti == nullptr`) keeps the existing raw `(hash, name)` test calls compiling and behaving as today.

Store the identity per-id alongside `m_names` (a new parallel container, index == id). On a **hash-hit**, compare dimension by dimension — a dimension is comparable only when present on *both* sides:
- **RTTI:** if both stored and incoming carry a `type_info`, they must satisfy `operator==`; a mismatch is a collision.
- **Structural:** if both carry structural info, `size` / `align` / triviality bits must all match; any difference is a collision.
- A **real** type registration (via `TypeIDStorage<T>::Value()`) always supplies structural info (`size`/`align` are always computable) and also RTTI where enabled, so at least one dimension is always comparable for real types. The "unspecified" identity (`{}`) arises **only** from direct test-helper calls. If no dimension is comparable on a given hit, treat it as the same type (today's behavior).

On **collision**: emit `ASTRA_LOG_ERROR` naming both type names + the constraint, evaluate `ASTRA_ENSURE_ALWAYS(false, "type-identity collision: distinct types share a name")`, and `return INVALID_COMPONENT` **without** caching (mirrors the id-exhaustion guard directly above at `TypeContext.hpp:88-91`). All configs.

Non-colliding hits (matching identity) return the existing id unchanged; fresh hashes assign a new dense id as today, now also recording the identity.

### 3. Retire the two broken guards

- `TypeContext.hpp:75-78` — the Debug name-assert is superseded by the all-config discriminator check above; remove it (the discriminator comparison replaces it).
- `ComponentRegistry.hpp:168-173` — the Debug name-comparison guard is now redundant: the real detection is upstream at the chokepoint, which returns `INVALID_COMPONENT` for a colliding type. Simplify/remove it. Confirm `RegisterComponentImpl` handles the resulting `INVALID_COMPONENT` the same graceful way it already handles an over-aligned refusal (a `RegisterComponent<T>` for a collided type resolves `TypeID<T>::Value()` → `INVALID_COMPONENT` → refuse to register, no descriptor written).

### 4. Serialization & backward compatibility

The stable `TypeID::Hash()` and the `hash → id` mapping are **untouched**. The discriminator is a pure side-channel *detection* input; it never enters the hash, the assigned id, or the wire format. Saves are byte-identical and cross-version stable — **no `BINARY_FORMAT_VERSION` bump**. The signature change is source-compatible for the one real caller (updated) and the direct test calls (defaulted param). The guarded RTTI usage is additive; the structural floor guarantees detection regardless of RTTI availability.

### 5. The guarantee (with its honest residual limitation)

- The structural floor catches **every memory-corrupting collision** (differing size/alignment/special-members) in **all** configs.
- Two genuinely distinct types with **identical** structural layout (e.g. two empty tag structs) are caught **only where RTTI is enabled** (all test configs; an RTTI-off consumer build will not catch this sub-case). That sub-case is logical mislabeling, not memory corruption.

Net guarantee: **no silent memory corruption in any config; no silent logical aliasing where RTTI is available.** This residual is documented, not hidden.

### 6. Testing

The review flagged the collision case as untested; it is now testable because the test binary has RTTI on.

- **Core unit test (no new TU)** — construct a fresh `TypeContext`; call `GetOrAssignComponentID(H, "Foo", identityA)` → `id0`; call `GetOrAssignComponentID(H, "Foo", identityB)` with a **different structural identity** → assert it returns `INVALID_COMPONENT` and a diagnostic was emitted (captured via the diagnostics test guard / `ScopedLogSink`). Assert same-identity re-call is idempotent (`id0`); assert distinct hashes still assign distinct ids (regression). Uses a local `TypeContext`, so it does not touch the global 128-`MAX_COMPONENTS` budget.
- **RTTI-path test (guarded on `__cpp_rtti`)** — two real distinct types with the **same** structural layout but different `typeid` → assert the RTTI cross-check refuses the second. Skipped where RTTI is unavailable.
- **Optional stronger integration test** — two real anonymous-namespace `Foo` types with different layouts in two separate `.cpp` TUs → assert the second's registration/`ComponentID` is refused. Requires a **new test file** (premake regen). **Decide during planning** whether to include; the core unit test already locks the detection logic.
- **Documentation** — a doc comment on `TypeID` / `GetOrAssignComponentID` / component registration stating the constraint ("types must have a unique unqualified name; distinct types sharing a compiler pretty-name — e.g. same-named anonymous-namespace types across TUs — are detected and the second is refused with a logged error") and the residual limitation from §5.

## Out of scope (YAGNI)

- No identity-scheme redesign (infeasible under the stability + RTTI-off-shipping constraints).
- No wire-format / `BINARY_FORMAT_VERSION` change.
- Not adding the other review-flagged `TypeContext` tests (concurrent `GetOrAssignComponentID`, id-space-exhaustion boundary) — those are separate coverage items; this work stays focused on the collision.

## Global constraints (carried into the plan)

- Header-only C++20; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`; exception-free & RTTI-off in the shipping library posture (errors are values; a shipping check is a real `if`, never a Debug-only assert).
- Additive, no break: existing type registration, component storage, and serialization behave identically for all non-colliding types; the only new observable behavior is that a genuine collision is now refused + reported instead of silently aliased.
- IDE/clang-tidy diagnostics are misconfigured false positives (expects Clang 20; "Mosaic/*.hpp not found"; "no gtest") — judge only by the MSVC build. Mosaic now lives at `vendor/Mosaic`.
- Baseline entering this work: `dev` @ `529706b`, tests **650 Debug / 648 Release / 648 Dist**, all green.
