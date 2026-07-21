# Theme H — Portability Hardening + RTTI-Free Reflection — Design Spec

**Date:** 2026-07-21
**Branch:** `theme-h-portability-rtti-free-reflection` (off `dev` @ `2a1ce83`)
**Review of record:** `docs/reviews/2026-07-11-astra-full-review.md` — Theme H ("Portability fragility on non-default configs")

## Goal

Close the reachable portability defects in the 2026-07-11 review's **Theme H**, scoped to Astra's real target reality (**64-bit only, but Linux / macOS / Windows / consoles — i.e. non-MSVC compilers and RTTI-off ship builds matter**), and make Astra's reflection system work **without RTTI** (so `rtti off` can be shipped everywhere for performance).

## Background — what Theme H flagged, and what changed since the review

The review's Theme H names four sub-items (`file:line` from 2026-07-11, some now stale):

- **H1** — `hash >> 57` is UB when `size_t` is 32-bit (`Swiss.hpp:46`, `Entity.hpp:165`).
- **H2** — bare-MSVC never defines `__SSE2__/__SSE4_2__/__AVX2__`, so SIMD silently degrades to scalar unless the consumer's build injects the (reserved-identifier) feature macros (`Platform.hpp:130-152`).
- **H3** — entity version-wraparound bit math is wrong for `ASTRA_ENTITY_VERSION_BITS ∉ {8,16,32}` (`Entity.hpp:59-61`, `EntityManager.hpp:124-128`); no `static_assert(VersionBits >= 1)`.
- **H4** — `std::any` in reflection needs RTTI, but the Dist/Release/benchmark presets ship `rtti "off"` (`premake5.lua`).

Two things changed since the review that reshape this work:

1. **Platform/SIMD detection moved into `vendor/Mosaic`** (the shared Starworks core — Astra/Manifold2D/Arcane). Astra's `Core/Platform.hpp` is now a re-export shim over `<Mosaic/Platform.hpp>`. So **H2's fix belongs in Mosaic**, not Astra's shim; fixing it here would fork the ladder that was just consolidated.
2. **Theme E already added RTTI guards** (`#if defined(__cpp_rtti) || defined(_CPPRTTI)`) around the `TypeID`/`TypeContext` `type_info` discriminator. Confirmed: **`std::any` is the *sole* remaining RTTI touchpoint in `Reflection/`** (no `typeid`/`type_info` anywhere else in the subsystem).

## Scope

**In scope (this spec, one branch):**
- **H1** — enforce the 64-bit contract (compile-time).
- **H3** — entity version-bit robustness (compile-time guard + correct wraparound math).
- **H4** — RTTI-free reflection: replace `std::any` with an Astra-`TypeID`-tagged type-erased carrier.

**Out of scope (tracked follow-ups):**
- **H2 (SIMD ISA detection)** — now a **Mosaic** concern. File a Mosaic follow-up; do NOT fork Astra's `Platform.hpp` shim. (Note: on clang/gcc the `__SSE2__/__AVX2__` macros ARE compiler-defined when the ISA is targeted, so the fragility is largely MSVC-bare-build specific; robust cross-compiler detection is a Mosaic-level improvement.)
- A dedicated **`/GR-` (rtti off) compile+run smoke/CI target** — deferred (see Verification). The structural proof below is the acceptance gate for this branch.

## Design decisions (all user-approved, 2026-07-21)

1. **Decompose Theme H:** H1+H3 (cheap, compiler-independent hardening) + H4 (RTTI-free reflection, a real design change) in one spec/branch; **H2 deferred to Mosaic** (it left this repo).
2. **Reflection must work without RTTI** — motivated by performance (RTTI is slow); `rtti off` is a supported ship config where reflection still functions.
3. **H4 mechanism = replace `std::any` with an Astra-native `TypeID`-tagged carrier** (`AnyValue`), NOT "verify `std::any` survives `-fno-rtti` + guard." Deterministically RTTI-free on all toolchains, faster (a `uint64_t` hash compare vs `std::any`'s manager indirection), and consistent with the typed `FieldInfo::Get`/`Set` path that already checks `TypeID<T>::Hash()`.
4. **Verification = structural proof, no new build target:** functional correctness via the normal (rtti-on) gtest suite + a verify-task assertion that `std::any`/`typeid`/`type_info` no longer appear anywhere in `Reflection/`. Since `std::any` is fully removed, RTTI-independence holds *by construction*. No `ide/` regen.

## H1 — Enforce the 64-bit contract

Both live sites extract the Swiss-table H2 control byte as the top 7 bits of a 64-bit hash:
- `Swiss.hpp:46` — `uint8_t h2 = static_cast<uint8_t>(hash >> 57) & 0x7F;`
- `Entity.hpp:165` — `if (((hash >> 57) & 0x7F) == 0)`

On 64-bit these are correct; the failure is only a hypothetical 32-bit build. **Fix:** add a `static_assert(sizeof(std::size_t) == 8, "Astra targets 64-bit platforms only (Swiss/Entity hash math assumes a 64-bit word)");` co-located with each shift site (or one central assert in a header both include). No behavior change on 64-bit; a 32-bit build now fails **loudly at compile time** instead of silent UB. This documents the just-stated "64-bit only" contract.

**Test:** none new required (a `static_assert` is its own test; a 32-bit build is not in CI). The verify task confirms the assert exists at both sites and all 3 configs still build.

## H3 — Entity version-bit robustness

`Entity.hpp` already asserts `TotalBits ∈ {16,32,64}`, `VersionBits < TotalBits`, and `ASTRA_ENTITY_VERSION_BITS <= 32`. Two gaps remain:

1. **No lower bound.** Add `static_assert(VersionBits >= 1, "ASTRA_ENTITY_VERSION_BITS must be at least 1");` in `EntityTraits`.
2. **Wraparound math assumes a byte-width version field.** `VersionType` is selected as `uint8/16/32` by `VersionBits <= 8/16/32`, so a `VersionBits` like 12 stores in a `uint16` but must wrap at 2¹² — not at the `VersionType`'s natural width. The recycle/increment path (`EntityManager.hpp:~122-132`) must compute the next version as `(version + 1) & VERSION_MASK` (the mask already derived at `Entity.hpp:126` = `(StorageType{1} << VersionBits) - 1`), so wraparound is correct for **any** `VersionBits`, not just {8,16,32}. Audit every version increment/compare to route through `VERSION_MASK`.

**Test (`tests/Entity/...` — reuse an existing entity test file, append):** instantiate `Detail::EntityTraits<32, 12>` (a non-byte-width version field) and assert the version wraps from `VERSION_MASK` back to `INITIAL_VERSION` correctly (not at 2¹⁶). Trait structs are Hash-keyed → **zero new ComponentIDs**. If a full recycle-wrap is impractical to unit-test directly, at minimum unit-test the mask/next-version helper in isolation.

## H4 — RTTI-free reflection (core)

### Current state
Reflection's dynamic path uses `std::any` in exactly two operations (never `.type()`, the one truly RTTI-dependent member):
- `FieldInfo::getterAny` — `return std::any(obj->*FieldPtr);` (construct).
- `FieldInfo::setterAny` — `std::any_cast<DecayedType>(&value)` (pointer form → `nullptr` on mismatch).
- `FieldInfo::GetAny`/`SetAny` — pass-through wrappers.
- `TypeMeta::GetFieldValueAny`/`SetFieldValueAny` — thin by-name wrappers over `FieldInfo::GetAny/SetAny`.

The typed `FieldInfo::Get`/`Set`/`GetPtr` already type-check via `TypeID<T>::Hash() == typeHash` (RTTI-free). Only the `std::any` path is the outlier.

### New component: `Astra::AnyValue`
A focused, header-only, type-erased single-value box — one clear responsibility.

- **Storage:** a fixed inline small-buffer (SBO) with heap fallback for larger/over-aligned `T`. Mirror the **`Delegate` SBO + function-pointer-manager idiom** (and `ComponentDescriptor`'s copy/move/destruct thunks) — NOT `AlignedStorage` (that is a compile-time two-type union, unsuitable for a runtime-typed box). Honor `alignof(T)` for the heap path (reuse the aligned-`new` idiom from the just-landed `SmallVector::Allocate`).
- **Type tag:** `uint64_t m_typeHash` = `TypeID<T>::Hash()` — same identity the typed path uses. (No `type_info`, no `typeid`.)
- **Manager:** a static per-type table of `destroy`/`copyConstruct`/`moveConstruct` **function pointers** (RTTI-free), selected at construction; an empty state has a null manager.
- **API (minimum needed):**
  - `AnyValue()` — empty; `bool HasValue() const`.
  - `template<class T> static AnyValue Make(const T&)` / templated constructor — construct-from-`T`, stamping `m_typeHash`.
  - `template<class T> const T* TryCast() const noexcept` — returns the stored object iff `TypeID<T>::Hash() == m_typeHash`, else `nullptr` (replaces the pointer-form `any_cast`). Optional non-const `TryCast`.
  - `uint64_t TypeHash() const`.
  - Copy ctor/assign, move ctor/assign (via the manager thunks), destructor.
- **Contract:** value semantics; a wrong-`T` `TryCast` returns `nullptr` (never UB); RTTI-free by construction.

Before implementing, do a final reuse pass over `Core/`/`Container/`; extend an existing type-erased primitive if one fits, otherwise ship `AnyValue` as a new focused type (likely `include/Astra/Reflection/AnyValue.hpp`, or `Core/` if it reads as generally reusable).

### Migrations (contained blast radius — all in-tree)
- `FieldInfo.hpp`: `getterAny` → `std::function<AnyValue(const void*)>` returning `AnyValue::Make(obj->*FieldPtr)`; `setterAny` → `AnyValue`-accepting, using `value.TryCast<DecayedType>()` (a `TypeID` compare); `GetAny` returns `AnyValue`; `SetAny` accepts `const AnyValue&`.
- `TypeMeta.hpp`: `GetFieldValueAny`/`SetFieldValueAny` signatures `std::any` → `AnyValue` (delegation unchanged).
- `FieldVisitor.hpp`: update its `GetAny`/`SetAny` usage to `AnyValue`.
- Remove the `<any>` include from `FieldInfo.hpp`/`TypeMeta.hpp`.

### Result
No `std::any`/`typeid`/`type_info` anywhere in `Reflection/` — RTTI-free by construction, faster on the dynamic path, and consistent with the existing `TypeID`-based typed accessors.

## Testing & verification

- **Functional (normal rtti-on gtest binary):**
  - New `AnyValue` unit tests: construct→`TryCast<T>` round-trip for the matching `T`; `TryCast<WrongT>()` → `nullptr`; empty `AnyValue`; copy/move/destroy lifetime accounting via a file-local **non-component** counted type (SBO and heap-fallback sizes both exercised, mirroring the F+D D2 over-aligned test discipline).
  - Migrate the existing `ReflectionTest.cpp` / `FieldVisitorTest.cpp` `std::any` assertions to `AnyValue` (behavior preserved: get returns the value, set accepts matching type and rejects a mismatched type by returning `false`).
- **Structural RTTI-free proof (verify task):** assert `grep -rn "std::any\|any_cast\|<any>\|typeid\|type_info" include/Astra/Reflection/` returns **nothing**. This is the acceptance gate for the RTTI-free property.
- **3-config green:** Debug / Release / Dist (x64), controller-independent verify (as in F+D).

## Global constraints

- **All three configs build clean and green:** Debug, Release, Dist (`-p:Platform=x64`).
- **Uniform-graceful:** no assert-and-abort on a recoverable condition; a wrong-`T` `TryCast` returns `nullptr`, a mismatched `SetAny` returns `false` (mirrors the existing `Get`/`Set` degrade-safe guards).
- **TypeID ceiling (~128):** net new component IDs ≈ **0** — reflection tests reuse existing reflected test types; `AnyValue` lifetime tests use a file-local non-component counted type; H3 uses trait instantiations (Hash-keyed → zero ComponentIDs).
- **No `ide/` regen:** all tests append to existing files or add a header-only source picked up by the existing test globs; do NOT `git add ide/`. (If a genuinely new test source file is needed and the build uses file globs, confirm it compiles without an ide/ change.)
- **IDE clang diagnostics are false positives** — judge only by the MSVC build.

## Acceptance criteria

1. H1: a `static_assert(sizeof(std::size_t) == 8)` guards both `hash >> 57` sites; all 3 configs build.
2. H3: `static_assert(VersionBits >= 1)` present; version wraparound uses `VERSION_MASK` (correct for any `VersionBits`); the `EntityTraits<32,12>` regression test passes.
3. H4: `std::any` fully removed from `Reflection/` (structural grep clean); `AnyValue` unit tests + migrated reflection tests pass; dynamic get/set behavior preserved.
4. 3-config green (Debug/Release/Dist); no `ide/` staged; net new ComponentIDs ≈ 0.

## Deferred / tracked follow-ups

- **H2** — robust SIMD ISA detection, in **`vendor/Mosaic`** (shared core). File a Mosaic follow-up.
- **Hard `-fno-rtti` proof** — a dedicated `/GR-` compile+run smoke/CI target that includes `Reflection` and exercises `GetAny`/`SetAny`; add when console CI stands up (structural proof covers it until then).
- H1's 32-bit `hash >> 57` remains a compile error (by design) rather than a 32-bit implementation — Astra is 64-bit only.
