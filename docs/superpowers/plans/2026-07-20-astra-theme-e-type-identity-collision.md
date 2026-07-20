# Theme E — Type-Identity Collision Detection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect when two distinct types share a compiler pretty-name (the anonymous-namespace-across-TUs case) or collide on the name-hash, at ID-assignment time, and refuse the second + report loudly in all configs — instead of silently aliasing it onto the first's `ComponentID` (memory corruption). Keep the stable name-hash identity and the wire format unchanged.

**Architecture:** A per-type discriminator (`TypeIdentity`: structural `sizeof`/`alignof`/triviality as the RTTI-free all-config floor, plus a `type_info*` RTTI cross-check where RTTI is enabled) is threaded through the single `TypeContext::GetOrAssignComponentID` chokepoint. On a name-hash hit, the chokepoint refuses (`return INVALID_COMPONENT`) and reports (`ASTRA_LOG_ERROR` + `ASTRA_ENSURE_ALWAYS`) if the incoming name differs from the stored one (real hash collision) OR the discriminator proves a different type (same-name anon collision). The discriminator never enters the hash, the id, or the wire format. Two Debug-only name-comparison guards that could never detect this class are retired.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-20-astra-theme-e-type-identity-collision-design.md` (approved).

## Global Constraints

- Header-only C++20; MSVC-primary. **Exception-free & RTTI-off in the shipping library posture** → errors are values; a shipping check is a real `if`, never a Debug-only assert. Any `typeid`/`std::type_info` use MUST be guarded by `#if defined(__cpp_rtti) || defined(_CPPRTTI)` — the structural discriminator is the load-bearing all-config path (there is no existing RTTI use in `include/`).
- **Additive, no break.** Existing type registration, component storage, and serialization behave identically for all non-colliding types. The only new observable behavior: a genuine collision is now refused (`INVALID_COMPONENT`) + reported instead of silently aliased. **No `BINARY_FORMAT_VERSION` bump; saves byte-identical.** `TypeID::Hash()` and the `hash → id` map are untouched.
- **Failure mode = refuse + loud report, all configs:** on collision, `ASTRA_LOG_ERROR` (detailed message) + `ASTRA_ENSURE_ALWAYS(false, …)` (host-escalatable) + `return INVALID_COMPONENT` uncached — mirroring the id-exhaustion guard already in the same function.
- **TypeID ceiling:** none of these tests register fresh types into the global context. Task 1 uses a **local** `TypeContext`; Task 2's factory test only computes `sizeof`/`alignof`/`typeid` (no id assignment); the wiring smoke test reuses built-in `int`/`double`. No `MAX_COMPONENTS=128` pressure.
- IDE/clang-tidy diagnostics are misconfigured false positives (expects Clang 20; "Mosaic/*.hpp not found"; "no gtest") — **judge only by the MSVC build.** Mosaic now lives at `vendor/Mosaic`.
- All file:line anchors are from `dev` @ `529706b`; **confirm each against the live tree before editing** (find edit points by named symbol/marker, not absolute line).

**Build (per config, whole solution):**
`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m -v:minimal -nologo`
**Run:** `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Filter>`
Appending to existing `tests/Core/TypeContextTest.cpp` / `tests/Core/TypeIDTests.cpp` needs **no** premake regen. Stale-PDB `mspdbsrv.exe` lock → `taskkill //F //IM mspdbsrv.exe` + rebuild. `CompressionTest.PerformanceBenchmark` is a known Release/Dist timing flake (a lone failure of only that test is not a regression; rerun isolated). Baseline entering this work: **650 Debug / 648 Release / 648 Dist**, all green.

## File Structure

- `include/Astra/Core/TypeContext.hpp` — Task 1: `TypeIdentity` struct + flags; `#include "Log.hpp"`; `m_identities` member; `IsTypeIdentityCollision` helper; the 3-arg `GetOrAssignComponentID` with name-mismatch||identity detection + refuse (replacing the Debug name-assert).
- `include/Astra/Core/TypeID.hpp` — Task 2: `MakeTypeIdentity<T>()` factory (guarded RTTI); `TypeIDStorage<T>::Value()` passes it; Task 3: doc comment on `TypeID`/`Hash()`.
- `include/Astra/Component/ComponentRegistry.hpp` — Task 3: remove the superseded Debug name-comparison guard; doc comment on `RegisterComponent`.
- `tests/Core/TypeContextTest.cpp` — Task 1: append collision-refuse tests.
- `tests/Core/TypeIDTests.cpp` — Task 2: append the factory + wiring tests.

---

### Task 1: Detection at the `GetOrAssignComponentID` chokepoint

Add the `TypeIdentity` discriminator and make the chokepoint refuse + report a collision (both classes), replacing the Debug-only name-assert. Tested directly against a local `TypeContext` with hand-built identities — no wiring or real types needed yet.

**Files:**
- Modify: `include/Astra/Core/TypeContext.hpp` (add includes/struct/helper/member; edit `GetOrAssignComponentID` ~:70-97)
- Test: `tests/Core/TypeContextTest.cpp` (append)

**Interfaces:**
- Produces (consumed by Task 2): `struct Astra::TypeIdentity { uint32_t size; uint32_t align; uint8_t flags; [const std::type_info* rtti under RTTI]; }` with flag bits `Astra::TIF_TriviallyCopyable | TIF_TriviallyDestructible | TIF_Empty`; the 3-arg `ComponentID TypeContext::GetOrAssignComponentID(uint64_t hash, std::string_view name, TypeIdentity identity = {})`.

- [ ] **Step 1: Write the failing tests.** Append to `tests/Core/TypeContextTest.cpp`. (Ensure the file includes `../Support/DiagnosticsTestGuards.hpp`; if not, add it near the existing includes — the diagnostics guards are header-only, no regen.) These use a **local** `TypeContext` and hand-built identities:
```cpp
#include "../Support/DiagnosticsTestGuards.hpp"   // add if not already present

namespace  // Theme E collision-detection capture
{
    struct ECapture { int errors = 0; };
    inline void ECaptureSink(const Astra::LogRecord& r, void* user) noexcept
    {
        if (r.level == Astra::LogLevel::Error) static_cast<ECapture*>(user)->errors++;
    }
    Astra::TypeIdentity MakeStructId(uint32_t size, uint32_t align, uint8_t flags = 0)
    {
        Astra::TypeIdentity id; id.size = size; id.align = align; id.flags = flags; return id;
    }
}

// Class 1: same name-hash, same name, but a provably different type (differing
// structural identity, as two distinct anonymous-namespace types would) -> refused.
TEST(TypeContextCollision, SameNameDistinctIdentityIsRefused)
{
    ECapture cap;
    Astra::Testing::ScopedLogSink sink(&ECaptureSink, &cap);
    Astra::Testing::ScopedAssertHandler handler(nullptr);  // default handler routes through the sink
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    ASSERT_NE(id0, Astra::INVALID_COMPONENT);
    const auto id1 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(16, 8));  // different size => different type
    EXPECT_EQ(id1, Astra::INVALID_COMPONENT);
    EXPECT_GE(cap.errors, 1);
}

// Class 2: same name-hash but a DIFFERENT name (a real XXHash collision of two
// differently-named types) -> refused, in all configs (upgrades the old
// Debug-only name-assert to a graceful all-config refuse).
TEST(TypeContextCollision, DifferentNameSameHashIsRefused)
{
    ECapture cap;
    Astra::Testing::ScopedLogSink sink(&ECaptureSink, &cap);
    Astra::Testing::ScopedAssertHandler handler(nullptr);
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(4321, "A");
    ASSERT_NE(id0, Astra::INVALID_COMPONENT);
    const auto id1 = ctx.GetOrAssignComponentID(4321, "B");  // same hash, different name
    EXPECT_EQ(id1, Astra::INVALID_COMPONENT);
    EXPECT_GE(cap.errors, 1);
}

// Non-collision: identical name + identity re-registration is idempotent.
TEST(TypeContextCollision, SameNameSameIdentityIsIdempotent)
{
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    const auto id1 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    EXPECT_EQ(id0, id1);
    EXPECT_NE(id0, Astra::INVALID_COMPONENT);
}
```
> The `ScopedAssertHandler handler(nullptr)` explicitly clears any assert handler so the `ASTRA_ENSURE_ALWAYS` on the refuse path routes through the installed log sink (Assert + Log are one seam — see `tests/Core/AssertTest.cpp`) rather than to stderr, keeping test output pristine. Confirm `Astra::LogRecord` / `Astra::LogLevel::Error` / `Astra::SetLogSink` spellings against `tests/Core/LogTest.cpp`.

- [ ] **Step 2: Run — verify RED.** Build Debug. Expected: **compile failure** — the 3-arg `GetOrAssignComponentID` and `TypeIdentity` do not exist yet.

- [ ] **Step 3: Add includes + the `TypeIdentity` struct (`TypeContext.hpp`).** Near the top includes, add `#include "Log.hpp"` (for `ASTRA_LOG_ERROR`) and, guarded, `#include <typeinfo>`. (`ASTRA_ASSERT`/`ASTRA_ENSURE_ALWAYS` are already reachable — `ASTRA_ASSERT` is used in this file today; `<string>`/`<vector>` are already included.) Above the `TypeContext` class, add:
```cpp
    // Triviality bits carried by TypeIdentity.
    enum TypeIdentityFlags : uint8_t
    {
        TIF_TriviallyCopyable     = 1u << 0,
        TIF_TriviallyDestructible = 1u << 1,
        TIF_Empty                 = 1u << 2,
    };

    // Distinguishes two types that share a compiler pretty-name (and thus the
    // same stable name-hash) -- e.g. distinct same-named types in anonymous
    // namespaces across TUs. A pure side-channel used ONLY to detect a collision
    // at id assignment; it never enters the hash, the assigned id, or the wire
    // format. `size == 0` means "unspecified" (raw call with no type). Built by
    // MakeTypeIdentity<T>() (TypeID.hpp).
    struct TypeIdentity
    {
        uint32_t size  = 0;   // sizeof(T); always >= 1 for a real type, so 0 == unspecified
        uint32_t align = 0;   // alignof(T)
        uint8_t  flags = 0;   // TypeIdentityFlags bits
#if defined(__cpp_rtti) || defined(_CPPRTTI)
        const std::type_info* rtti = nullptr;  // &typeid(T) where RTTI is enabled
#endif
    };
```

- [ ] **Step 4: Add the collision predicate + storage (`TypeContext.hpp`).** Inside `class TypeContext`, add a private helper and a parallel storage vector:
```cpp
    private:
        // True iff a and b provably denote DIFFERENT types. Each dimension is
        // compared only when present on both sides. Absence of any comparable
        // dimension => not treated as a collision (raw test-helper calls).
        ASTRA_NODISCARD static bool IsTypeIdentityCollision(const TypeIdentity& a, const TypeIdentity& b) noexcept
        {
#if defined(__cpp_rtti) || defined(_CPPRTTI)
            if (a.rtti != nullptr && b.rtti != nullptr)
                return *a.rtti != *b.rtti;  // ABI-correct, cross-module-safe; disambiguates anon namespaces
#endif
            if (a.size != 0 && b.size != 0)
                return a.size != b.size || a.align != b.align || a.flags != b.flags;
            return false;
        }
```
And beside `std::deque<std::string> m_names;` add:
```cpp
        std::vector<TypeIdentity> m_identities;  // parallel to m_names, index == id
```

- [ ] **Step 5: Extend `GetOrAssignComponentID` with detection + refuse (`TypeContext.hpp` ~:70-97).** Change the signature and the hit/assign bodies:
```cpp
        ASTRA_NODISCARD ComponentID GetOrAssignComponentID(uint64_t hash, std::string_view name,
                                                           TypeIdentity identity = {})
        {
            std::lock_guard lock(m_mutex);
            if (auto it = m_hashToId.Find(hash); it != m_hashToId.end())
            {
                const ComponentID existingId = it->second;
                // Two collision classes: (1) same name-hash + DIFFERENT name = a
                // real XXHash collision of two differently-named types; (2) same
                // name-hash + same name but a DIFFERENT type = identical
                // anonymous-namespace names across TUs (the discriminator catches
                // what the byte-identical name cannot). Either is refused loudly.
                if (m_names[existingId] != name
                    || IsTypeIdentityCollision(m_identities[existingId], identity))
                {
                    std::string msg = "TypeContext: type-identity collision -- a distinct type shares "
                                      "the name-hash of '";
                    msg.append(m_names[existingId]);
                    msg += "'. The second type is refused (its ComponentID is INVALID). Give types a "
                           "unique unqualified name; do not place two same-named types in anonymous "
                           "namespaces across translation units.";
                    ASTRA_LOG_ERROR(msg);
                    ASTRA_ENSURE_ALWAYS(false, "TypeContext type-identity collision (see log)");
                    return INVALID_COMPONENT;  // refuse, uncached -- mirrors the id-exhaustion guard below
                }
                return existingId;
            }
            if (m_next == INVALID_COMPONENT) ASTRA_UNLIKELY
            {
                return INVALID_COMPONENT;
            }
            ASTRA_ASSERT(m_next != INVALID_COMPONENT, "TypeContext ID space exhausted");
            const ComponentID id = m_next++;
            m_hashToId[hash] = id;
            m_names.emplace_back(name);
            m_identities.push_back(identity);
            return id;
        }
```
This **removes** the old `#ifdef ASTRA_BUILD_DEBUG ASTRA_ASSERT(m_names[it->second] == name, ...)` block — the new all-config name-mismatch check supersedes it (and is graceful, not a Debug abort).

- [ ] **Step 6: Run — verify GREEN.** Build all 3 configs. Run `AstraTest.exe --gtest_filter=TypeContextCollision.*:TypeContext.*` each config. The 3 new tests pass; the pre-existing `TypeContext.*` tests (dense assignment, same-hash-same-id, independent-context isolation) still pass unchanged (they use consistent name-per-hash, so the name check never trips). Full suite green (650/648/648 + 3 → 653/651/651).

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Core/TypeContext.hpp tests/Core/TypeContextTest.cpp
git commit -m "feat(core): detect type-identity collisions at the GetOrAssignComponentID chokepoint"
```

---

### Task 2: Wire real types through the discriminator

Build a `TypeIdentity` from `T` and pass it from `TypeIDStorage<T>::Value()`, so every real type registration carries a discriminator end-to-end. Prove the factory captures the right fields and that normal (non-colliding) registration is unaffected.

**Files:**
- Modify: `include/Astra/Core/TypeID.hpp` (add `MakeTypeIdentity<T>()`; edit `TypeIDStorage<T>::Value()` ~:214-219)
- Test: `tests/Core/TypeIDTests.cpp` (append)

**Interfaces:**
- Consumes (from Task 1): `Astra::TypeIdentity`, `TypeIdentityFlags`, the 3-arg `GetOrAssignComponentID`.
- Produces: `template<typename T> Astra::TypeIdentity Astra::MakeTypeIdentity() noexcept`.

- [ ] **Step 1: Write the failing tests.** Append to `tests/Core/TypeIDTests.cpp`:
```cpp
namespace  // Theme E factory checks (local types; MakeTypeIdentity does NOT assign an id)
{
    struct Sz16 { char data[16]; };
    struct EmptyTag {};
}

// The factory captures structural fields from T.
TEST(TypeIdentityFactory, CapturesStructuralFields)
{
    const auto a = Astra::MakeTypeIdentity<Sz16>();
    EXPECT_EQ(a.size, 16u);
    EXPECT_EQ(a.align, static_cast<uint32_t>(alignof(Sz16)));
    EXPECT_NE(a.flags & Astra::TIF_TriviallyCopyable, 0);

    const auto e = Astra::MakeTypeIdentity<EmptyTag>();
    EXPECT_NE(e.flags & Astra::TIF_Empty, 0);
    EXPECT_NE(e.size, 0u);  // sizeof is always >= 1, so "present" is detectable
}

// Wiring smoke test: distinct real types still get distinct ids (built-in types,
// no fresh component registration -> no TypeID-ceiling pressure).
TEST(TypeIdentityFactory, DistinctRealTypesStillGetDistinctIds)
{
    EXPECT_NE(Astra::TypeID<int>::Value(), Astra::TypeID<double>::Value());
}
```

- [ ] **Step 2: Run — verify RED.** Build Debug. Expected: **compile failure** — `MakeTypeIdentity` does not exist.

- [ ] **Step 3: Add `MakeTypeIdentity<T>()` (`TypeID.hpp`).** In `namespace Astra` (after `struct TypeID`, so `TypeIdentity` from the included `TypeContext.hpp` is visible), add:
```cpp
    // Build the per-type discriminator used by TypeContext to DETECT identity
    // collisions (see TypeContext::GetOrAssignComponentID). Structural fields are
    // always present (all configs); the RTTI type_info is added only where RTTI
    // is enabled. Never enters the hash/id/wire format.
    template<typename T>
    ASTRA_NODISCARD inline TypeIdentity MakeTypeIdentity() noexcept
    {
        TypeIdentity id;
        id.size  = static_cast<uint32_t>(sizeof(T));
        id.align = static_cast<uint32_t>(alignof(T));
        id.flags = static_cast<uint8_t>(
            (std::is_trivially_copyable_v<T>    ? TIF_TriviallyCopyable     : 0) |
            (std::is_trivially_destructible_v<T> ? TIF_TriviallyDestructible : 0) |
            (std::is_empty_v<T>                 ? TIF_Empty                 : 0));
#if defined(__cpp_rtti) || defined(_CPPRTTI)
        id.rtti = &typeid(T);
#endif
        return id;
    }
```

- [ ] **Step 4: Pass the discriminator from `TypeIDStorage<T>::Value()` (`TypeID.hpp` ~:214-219).** Change the magic-static initializer to pass the identity:
```cpp
            ASTRA_NODISCARD static ComponentID Value()
            {
                static const ComponentID s_id =
                    GetTypeContext()->GetOrAssignComponentID(
                        TypeHash<T>(), TypeNameInternal<T>(), MakeTypeIdentity<T>());
                return s_id;
            }
```

- [ ] **Step 5: Run — verify GREEN.** Build all 3 configs. Run `--gtest_filter=TypeIdentityFactory.*:TypeID*.*` each config. **Then run the FULL suite on all 3 configs.** With real types now carrying discriminators, a *pre-existing latent* collision in the test binary (two distinct types with the same pretty-name, or a real hash collision) would now be refused and could surface as a failing test. **If that happens, it is a genuine pre-existing bug — investigate the two colliding types and rename one; do NOT weaken the guard.** (The 2026-07-11 review states the current suite does not harmfully trigger this, so no surfacing is expected.) Full suite green (653/651/651 + 2 → 655/653/653).

- [ ] **Step 6: Commit.**
```bash
git add include/Astra/Core/TypeID.hpp tests/Core/TypeIDTests.cpp
git commit -m "feat(core): TypeIDStorage passes a per-type discriminator into id assignment"
```

---

### Task 3: Retire the superseded name-guard + document the constraint

The chokepoint now detects both collision classes in all configs, so the Debug-only name-comparison guard in `ComponentRegistry` is dead. Remove it and document the constraint and its residual limitation. (The `TypeContext` Debug name-assert was already removed in Task 1.)

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp` (remove the Debug guard ~:162-176; doc comment on `RegisterComponent`)
- Modify: `include/Astra/Core/TypeID.hpp` (doc comment on `TypeID`/`Hash()`)
- Modify: `include/Astra/Core/TypeContext.hpp` (doc comment on `GetOrAssignComponentID`)

**Interfaces:** none produced/consumed (cleanup + docs).

- [ ] **Step 1: Confirm the refuse path is safe downstream (read-only check).** Read `RegisterComponentImpl` (`ComponentRegistry.hpp` ~:129-139). Confirm it already early-returns on `id >= MAX_COMPONENTS` (it does at ~:133-139), so a colliding type whose `TypeID<T>::Value()` is now `INVALID_COMPONENT (65535)` is refused there without OOB (descriptor lookup → nullptr; `AddComponent` → nullptr) — identical to the pre-existing id-exhaustion path. **No code change needed here; note the confirmation in the report.**

- [ ] **Step 2: Remove the superseded Debug name-comparison guard (`ComponentRegistry.hpp` ~:162-176).** Delete the entire `#ifdef ASTRA_BUILD_DEBUG … #endif` block that compares `TypeID<T>::Name()` to `existing->name` (the "Hash collision detected!" `ASTRA_ASSERT`). It can never fire for the anonymous-namespace class (byte-identical names) and the different-name class is now caught all-config + gracefully upstream at the chokepoint. Leave the surrounding descriptor-build code intact.

- [ ] **Step 3: Add the doc comments.** Concise, factual:
  - On `TypeID` (or its `Hash()`), `TypeID.hpp`:
```cpp
    // NOTE: type identity derives from the compiler pretty-name, so every type
    // must have a UNIQUE unqualified name. Two distinct types that share a
    // pretty-name (e.g. same-named types in anonymous namespaces in different
    // translation units) collide; the collision is DETECTED at id assignment and
    // the second type is refused with a logged error (see
    // TypeContext::GetOrAssignComponentID). Detection is complete wherever RTTI is
    // enabled; in RTTI-off builds it catches every collision whose types differ in
    // size/alignment/triviality (the memory-corrupting cases), but two distinct
    // types with identical layout evade detection (logical mislabel, not corruption).
```
  - On `GetOrAssignComponentID`, `TypeContext.hpp`: one line stating it refuses (`INVALID_COMPONENT`) + reports on a name-hash collision (differing name, or a differing TypeIdentity).
  - On `RegisterComponent`, `ComponentRegistry.hpp`: one line that a type whose identity collided resolves to `INVALID_COMPONENT` and is refused (no descriptor written).

- [ ] **Step 4: Run — verify GREEN.** Build all 3 configs, full suite. Nothing should change behaviorally (the removed guard had no correct effect). Run `--gtest_filter=TypeContextCollision.*:TypeIdentityFactory.*:ComponentRegistry*.*:TypeContext.*:TypeID*.*` each config. Full suite green (655/653/653, unchanged count).

- [ ] **Step 5: Commit.**
```bash
git add include/Astra/Component/ComponentRegistry.hpp include/Astra/Core/TypeID.hpp include/Astra/Core/TypeContext.hpp
git commit -m "refactor(core): retire the superseded Debug name-collision guard; document the unique-name constraint"
```

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §1 discriminator → Task 1 Step 3 (struct) + Task 2 Step 3 (factory, incl. structural + guarded RTTI); §2 detection at chokepoint + refuse → Task 1 Steps 4-5 (name-mismatch || `IsTypeIdentityCollision`, `INVALID_COMPONENT` + `ASTRA_LOG_ERROR` + `ASTRA_ENSURE_ALWAYS`); §3 retire both broken guards → Task 1 Step 5 (TypeContext assert, replaced) + Task 3 Step 2 (ComponentRegistry guard) + Task 3 Step 1 (confirm graceful `INVALID` downstream); §4 serialization/back-compat → Global Constraints + no hash/wire change anywhere; §5 residual limitation → Task 3 Step 3 doc comment; §6 testing → Task 1 (chokepoint, both classes) + Task 2 (factory + wiring), with the optional two-TU integration test **deliberately deferred** (documented follow-up: fragile global-context/magic-static interaction; the chokepoint + factory tests fully cover the detection logic and wiring). Out-of-scope items (identity redesign, wire-format change, other TypeContext coverage) correctly have no task.
- **Placeholder scan:** every code step shows complete code; the one read-only step (Task 3 Step 1) is a bounded confirmation with a concrete anchor and a report requirement, not a TODO.
- **Type consistency:** `TypeIdentity` (fields `size`/`align`/`flags`/guarded `rtti`) is defined in Task 1 Step 3, built in Task 2 Step 3, compared in Task 1 Step 4, and passed in Task 2 Step 4; `TIF_TriviallyCopyable`/`TIF_TriviallyDestructible`/`TIF_Empty` names match across Task 1 Step 3, Task 2 Steps 1/3; the 3-arg `GetOrAssignComponentID(hash, name, identity = {})` signature matches across Task 1 Steps 1/5 and Task 2 Steps 1/4; `MakeTypeIdentity<T>()` matches Task 2 Steps 1/3/4.
- **Additive:** no-collision path is byte-identical (name check trips only on a name mismatch, which never happens for consistent name-per-hash registration; the discriminator trips only on a provably different type); `TypeID::Hash()`, the `hash → id` map, and the wire format are untouched; no `BINARY_FORMAT_VERSION` bump.
