# Meta binder scoping: multi-registry-safe TypeMeta lifetime

**Date:** 2026-09-09
**Status:** design approved in brainstorm (user, 2026-09-09); spec pending user review
**Origin:** the "per-registry meta scoping" backlog note in
`2026-08-09-astra-component-module-raii-design.md` §3.5.1 and §3.6 ("one
registry per context is the supported shape"), and Arcane's reverted
Runtime-owned engine ComponentModule (Arcane `Runtime.cpp`, comment at the
roster registration, 2026-08-10).

## 1. Problem

A `TypeMeta` lives once per `TypeContext`, in that context's single
`MetaRegistry`. Every `ComponentRegistry` that shares the context caches the
meta's address in `ComponentDescriptor::meta`, and every registry-less
consumer (`Astra::GetMeta`, `GetMetaByName`, `ForEachType`) reads it through
the context.

The 2026-08-09 program tied a meta's lifetime to a single signal: "the slot
for this hash in *this* registry cleared to empty". `ComponentModule::Reset`
and `UnregisterModuleRange` both call `MetaRegistry::EraseUnchecked` on that
transition. The signal is only correct when exactly one registry exists per
context. With several registries -- Arcane's test suite runs ~48 short-lived
`Runtime` instances, each with its own `ComponentRegistry`, against one
process-wide `TypeContext` -- the first registry to clear a slot erases a
meta that:

- another registry's live descriptor still caches (dangling `desc.meta`);
- registry-less consumers still expect (Arcane reads `GetMeta<Transform>()`
  with no `Runtime` alive at all).

That is why Arcane's engine roster had to stay on anonymous registration.
The meta's real lifetime constraints are neither per-registry nor
per-handle:

1. **Reference safety** -- nothing may erase it while any registry slot or
   any handle still points at it.
2. **Module lifetime** -- its content (closures, `typeName`, field-name
   views) points into the image of the module that built it, so it must be
   rebound or erased before that image unmaps -- and the only party that
   knows whether an image ever unmaps is that module.

## 2. Decisions (user-approved, 2026-09-09)

1. **Shared table, correct ownership.** One `MetaRegistry` per `TypeContext`
   stays. No per-registry meta tables, no registry-scoped meta view.
2. **Modules declare residency.** A module that never unmaps says so once at
   install (`SetTypeContext(ctx, ModuleResidency::Resident)`). Its metas fall
   back to its own binding instead of being erased when the last reference
   goes. Default is `Transient`, which reproduces today's plugin semantics.
3. **Astra-only program.** Acceptance is an Astra test that replays Arcane's
   shape (N registries on one context, resident engine + transient plugin,
   both teardown orders, registry-less lookups stay valid). Arcane re-vendors
   later and decides separately whether to re-open the anonymous-roster
   ratification.
4. **Approach B, binder stack.** Each meta entry keeps a small stack of
   binders `{module, build thunk, refs, pinned}`; content is bound to the
   top; erase on empty; resident binders are pinned. Same mental model as the
   descriptor owner stack in `ComponentRegistry`.

Rejected: (A) a bare hold count plus baseline thunk -- cannot answer "which
surviving module's thunk do I rebind to" when an overriding module departs
while other registries still hold; (C) image-anchored metas purged by
address range -- makes the host's range purge the only meta cleaner,
inverting the RAII-first premise, and does nothing for the rebind case.

## 3. Design

### 3.1 Module identity and residency

```cpp
namespace Astra
{
    enum class ModuleResidency : uint8_t { Transient, Resident };
    using ModuleToken = const void*;

    namespace Detail
    {
        struct ModuleIdentity { ModuleToken token; ModuleResidency residency; };
        // Per-module static (inline function-local): one copy per EXE/DLL
        // image. token defaults to the static's own address.
        inline ModuleIdentity& CurrentModuleIdentity();
        // Test seam: swaps the per-module identity for a scope, restoring on
        // exit. Lets one test binary play several modules.
        struct ScopedModuleIdentity;
    }

    inline void SetTypeContext(TypeContext* ctx,
                               ModuleResidency residency = ModuleResidency::Transient);
}
```

- `ModuleToken` is the address of a function-local static inside an inline
  function -- the same per-module-static mechanism `CurrentTypeContextSlot`
  already relies on. A reloaded DLL lands at a new base address and
  therefore gets a new token; that is what makes load-before-unload reload
  fall out without special handling.
- `SetTypeContext` stores `residency` into the module's identity **before**
  draining pending metas, so the drain sees it. Declare once, before any
  registration in that module; the pinned flag is stamped on a binder when
  it is first pushed and never revisited.
- Inherits the TypeContext slot's POSIX caveat: default-visibility DSOs may
  coalesce the inline static, merging two modules into one token. That only
  **over**-holds (a plugin's metas may outlive it until the merged partner
  releases); it never erases early.

### 3.2 MetaRegistry entry

```cpp
struct Binder
{
    ModuleToken module;
    MetaBuildFn build;     // may be null: a module registering a component
                           // it has no reflect factory for (see 3.6)
    uint32_t    refs;      // live registry slots + RegisterMeta handles
    bool        pinned;    // resident module: never removed
};

struct Entry
{
    std::unique_ptr<TypeMeta>  meta;      // address-stable for the entry's life
    SmallVector<Binder, 2>     binders;   // back() == top == whose closures
                                          // `meta` currently carries
};
FlatMap<uint64_t, Entry> m_types;
// link rows unchanged: m_typeToComponentId / m_componentIdToType
```

Invariants:

- The top binder always has a non-null `build` unless every binder is
  null-build (3.6, degraded case).
- `refs` counts exactly one per live-or-shadowed registry slot holding this
  hash, plus one per `RegisterMeta` adoption. Registry destruction does
  **not** decrement (3.6).
- Link rows are context facts (ComponentIDs are context-scoped); they are
  dropped only when the entry is erased.

### 3.3 Operations

```cpp
enum class BindOutcome    : uint8_t { Refused, Bound, BoundNeedsRebuild };
enum class ReleaseOutcome : uint8_t { Unbound, Held, Dropped, Retained, Rebound, Erased };
struct BindResult { BindOutcome outcome; TypeMeta* meta; };

// Drain / ReflectType path. Installs the entry if absent (content = fresh,
// this module's binder as the only one). If the entry exists: identity-check,
// then push a refs-0 binder AT THE BOTTOM (first-wins content) -- a no-op if
// this module already has a binder. Never swaps existing content.
BindResult InstallBaseline(uint64_t hash, Detail::ModuleIdentity who,
                           MetaBuildFn build, TypeMeta&& fresh);

// Registration path. Entry absent: install from `fresh` (Refused if fresh is
// null) with this binder as the only one. Entry present, no binder for
// `who.token` yet: push a NEW binder on top (build != null) or just below the
// top (build == null); if it landed on top, swap content to `fresh`, or --
// when `fresh` is null and build != null -- report BoundNeedsRebuild so the
// caller runs `build()` outside its locks and calls RebindInPlace. Entry
// present, binder already exists: content is swapped only if that binder is
// top AND `fresh` is given (same-image re-bind / in-binary reload); otherwise
// nothing changes -- an existing top binder already carries this module's
// closures. Identity mismatch on any supplied `fresh` -> Refused. No ref
// change on any path.
BindResult Bind(uint64_t hash, Detail::ModuleIdentity who,
                MetaBuildFn build, TypeMeta* fresh);

// refs++ on an EXISTING binder. Absent binder -> false + ASTRA_ENSURE (a
// caller must Bind before Acquire).
bool Acquire(uint64_t hash, ModuleToken module);

// refs--. At zero and unpinned the binder is removed (Dropped if it was not
// top). If it was top: rebuild from the new top's thunk OUTSIDE the mutex,
// relock, confirm the top is still that binder, identity-check, swap
// (Rebound). Zero and pinned -> Retained (nothing else changes). Empty stack
// -> erase entry + both link rows (Erased). refs still > 0 -> Held.
ReleaseOutcome Release(uint64_t hash, ModuleToken module);
```

`Register(TypeMeta&&)` and `RebindInPlace(TypeMeta&&)` stay as the content
primitives underneath. `Erase` and `EraseUnchecked` are removed from the
public surface. `Clear()` stays (tests). All read accessors are unchanged.

Lock discipline (unchanged in kind): registration mutex -> meta mutex, never
reversed; `Bind`, `Acquire`, `InstallBaseline` never run a thunk and may be
called under the registration mutex; `Release` may run a thunk and is
therefore called only after every registry lock is dropped; inside `Release`
the thunk runs with the meta mutex released and the swap re-validates the
top afterwards.

### 3.4 Registry slot accounting

`ComponentRegistry` gains `ModuleToken m_metaModule[MAX_COMPONENTS]` parallel
to `m_metaThunk`, and `ShadowEntry` gains `ModuleToken module`. Every
live-or-shadowed entry holds exactly one ref on its module's binder:

- `RegisterComponentImpl<T>` (anonymous, under the registration lock):
  `who = Detail::CurrentModuleIdentity()` (the template is instantiated in
  the caller's module, so this resolves to the caller's copy);
  `Bind(hash, who, MetaFactory<T>::fn ? &BuildMetaThunk<T> : nullptr, nullptr)`;
  store token/thunk; `LinkToComponent`; `Acquire`. `BoundNeedsRebuild` is
  recorded and honored by `RegisterComponent` after the lock drops. A
  `Refused` bind here (type reflected in some other module but never drained
  into this context, and no factory in this module) registers the component
  UNREFLECTED -- `desc.meta` null, no link, no acquire -- exactly what a null
  `Get<T>()` produces today. Only on the empty->live transition (the existing
  `m_registered` guard).
- `InstallOwned(id, owner, desc, buildMeta, module)` returns
  `InstallResult { Refused, Installed, Overrode, Replaced }`. The caller
  acquires on `Installed` and `Overrode`; `Replaced` (same owner, same slot)
  changes nothing -- the ref already exists and the image is the same.
- `RestoreOrClearSlot` no longer carries a build thunk out; it appends
  `MetaRelease{hash, module}` for the departing entry whether a survivor was
  restored or the slot cleared. Stripped shadows append one each too.
- `ReleaseModule(owner)` returns `SmallVector<MetaRelease, 4>`;
  `UnregisterModuleRange` collects the same list under its lock. Both callers
  run `Release` per entry after the lock.
- Registry destructor: no releases (see 3.6).

### 3.5 Flows

1. **Static-init drain.** `StaticTypeRegistrar<T>` builds eagerly, stores
   `MetaFactory<T>::fn`, enqueues. The drained callback runs in the enqueuing
   module (its own `SetTypeContext` or `MetaRegistry::Instance()`), reads
   `CurrentModuleIdentity()` there and calls
   `InstallBaseline(hash, who, &BuildMetaThunk<T>, std::move(meta))`.
   `ReflectType` / `ReflectEnum` do the same and additionally store their
   builder into `MetaFactory<T>::fn` so a rebuild thunk exists.
2. **Anonymous `RegisterComponent<T>`.** As in 3.4. In the common case the
   registering module is the drainer, its binder already exists at the
   bottom and is the only one, so nothing rebuilds.
3. **`ComponentModule::Register<T>`.** `Open` captures
   `m_identity = CurrentModuleIdentity()`. Phase A (no locks): `fresh =
   thunk()`; `Bind(hash, m_identity, thunk, &fresh)` -- installs when absent
   (the in-binary reload tests depend on this), swaps content when top;
   `LinkToComponent`. Phase B: `InstallOwned(..., m_identity.token)`;
   `Installed`/`Overrode` -> `Acquire`. Refusals at any step leave the type
   never-owned, never-acquired (unchanged contract).
4. **`ComponentModule::Reset`.** `ReleaseModule(owner)` under the lock, then
   `Release(hash, m_identity.token)` per entry outside all locks. If this
   module's binder was top and hit zero, `Release` rebinds to the new top --
   the survivor's thunk, wherever that survivor lives. If the module still
   holds refs from another registry, its binder stays on top and content
   stays put (that handle's image is still mapped). `RegisterMeta` entries
   are released last (`EraseOwnedMetas` becomes a release loop).
5. **`UnregisterModuleRange`.** Same as 4 with the token read from each
   stripped/cleared entry rather than a handle, so one purge that hits
   several modules' entries releases each against its own binder.
6. **`RegisterMeta<T>`.** `Bind` with `fresh` then `Acquire`, recorded only
   when the bind took; teardown `Release`s. The old guarded `Erase` and its
   component-linked refusal disappear: a component-linked hash simply has
   other refs and reports `Held`.
7. **Registry destruction.** Releases nothing (3.6).

### 3.6 Error handling and edges

- **Refusals unchanged in kind.** Identity collision in `Bind`/
  `InstallBaseline` applies the existing size/alignment/triviality/name check
  (`Refused`, `ASTRA_LOG_ERROR` + `ASTRA_ENSURE_ALWAYS`); the caller treats the
  type as refused: no descriptor, no acquire. Over-aligned refusal keeps its
  compile-time hoist above Phase A. Id exhaustion / `MAX_COMPONENTS` guards
  untouched.
- **Reload shapes.** Real DLL reload: new image = new token; the new
  generation pushes its own binder on top; the old generation's `Reset` drops
  a binder that is no longer top (`Dropped`). In-binary reload tests share a
  token: load-before-unload goes refs 1 -> 2 -> 1 (content swapped to the
  newest `Bind`), unload-before-load hits zero (`Erased`) and the next
  `Register` installs fresh. Existing tests keep their assertions.
- **Residency misuse.** A module that declares `Resident` and then unmaps
  leaves pinned binders whose content dangles -- the same class as a leaked
  handle. Undetectable; documented in the header.
- **Null-build binders.** A module that registers a reflected component
  without having its reflect factory (reflect data compiled only into another
  module) gets a null-build binder, inserted *below* the top so it can hold
  refs but never source content. If a release ever leaves only null-build
  binders on the stack, content cannot be rebuilt: the entry is kept
  (`Retained`, address stays valid) and an `ASTRA_LOG_WARN` names the hash
  (not the `typeName`, which may already point into unmapped memory). That
  module inherited the drainer's lifetime by construction; documented misuse,
  strictly better than today's outright erase.
- **Preserved as today, on purpose.** (a) A transient module that drains
  metas it never adopts leaves refs-0 unpinned binders forever: immortal,
  dangling if that module unmaps -- documented misuse. (b) A leaked handle
  never releases; registry-owned name interning remains the follow-up
  hardening for that window. (c) Registry destruction releases nothing:
  anonymous entries outlive their registry exactly as today (Arcane's
  registry-less reads depend on it); the cost is an over-hold, the safe
  direction. (d) Range purge on POSIX has no image spans; the RAII path is
  complete without it. (e) Token coalescing across default-visibility DSOs
  over-holds, never under-holds.
- **Concurrency.** Every `MetaRegistry` operation is individually
  thread-safe under its `shared_mutex`. `Release`'s unlock / thunk / relock
  window re-checks that the top binder is still the one it built for and
  skips the swap otherwise (a thunk may legally re-enter `Bind`). Registration
  remains a single-threaded setup/teardown path by contract. Distinct hashes
  never touch each other's binders, so the existing two-module concurrent
  registration test holds.

### 3.7 API changes

- `SetTypeContext(TypeContext*, ModuleResidency = Transient)` -- additive.
- `Detail::ModuleIdentity`, `Detail::CurrentModuleIdentity()`,
  `Detail::ScopedModuleIdentity` -- new (Detail).
- `MetaRegistry`: `+InstallBaseline`, `+Bind`, `+Acquire`, `+Release`,
  `+BindOutcome/BindResult/ReleaseOutcome`; `-Erase`, `-EraseUnchecked`.
  `RebindInPlace`, `Register`, `LinkToComponent`, reads, `Clear` unchanged.
- `ComponentRegistry`: `InstallOwned` returns `InstallResult` and takes a
  `ModuleToken`; `ReleaseModule` returns `SmallVector<MetaRelease,4>`;
  `MetaRestore` -> `MetaRelease{hash, module}`; `ShadowEntry` + per-slot
  arrays carry the token. `UnregisterModuleRange` signature unchanged.
- `ComponentModule`: `Open`/`Register`/`RegisterMeta`/`Reset` signatures
  unchanged (source-compatible for plugins); captures `ModuleIdentity`.
- Deleted text: the "one registry per context is the supported shape" caveat
  in `ComponentModule.hpp` (Reset), `ComponentRegistry.hpp`
  (`RestoreOrClearSlot`), and the 2026-08-09 spec §3.6 -- replaced by a
  pointer to this spec's binder rule. §3.5.1's backlog note is marked done.
- Callers verified 2026-09-09: Arcane uses none of the changed internals
  (only comments mention them); inside Astra only `ComponentModule` and three
  test files do.

### 3.8 Non-goals

- Registry-scoped meta views / per-registry meta tables.
- ComponentID recycling.
- Registry-owned name interning (leaked-handle window).
- Any Arcane change; any re-opening of the anonymous-roster ratification.
- Detecting residency misuse or leaked handles.

## 4. Testing

**Seam.** `Detail::ScopedModuleIdentity{token, residency}` swaps the
per-module identity for a scope (same pattern as the `InstalledContext`
fixture). One test binary can then play "engine, resident" and "plugin,
transient" against one context. All new tests reuse existing test component
types -- the suite is near its id ceiling even at `ASTRA_MAX_COMPONENTS=192`.

**MetaRegistry unit tests** -- new `tests/Reflection/MetaBinderTest.cpp`
(replaces the erase cases in `MetaRebindTest`):

1. `Bind` installs when absent; a second token pushes on top and swaps
   content; releasing the top rebinds to the previous thunk (`Rebound`).
   Content observed through field count, which the identity check ignores.
2. Two `Acquire`s + one `Release` -> `Held`; second -> `Erased`, both link
   rows gone.
3. Pinned binder survives every release (its own releases report
   `Retained`); when an overrider above it departs, that release reports
   `Rebound` and content returns to the pinned binder's thunk.
4. `Bind` with mismatched-identity `fresh` -> `Refused`, nothing pushed.
5. `Release` on an unknown token -> `Unbound`, nothing changes.
6. A thunk that itself calls `Bind` during a release exercises the
   top-changed recheck.
7. `InstallBaseline` on an existing entry pushes at the bottom and keeps
   content (first-wins).
8. Null-build binder never becomes top; all-null-build release -> `Retained`
   + warning.

**ComponentModule integration tests** -- additions to
`tests/Component/ComponentModuleTest.cpp`:

9. **Arcane-shaped (acceptance).** Engine identity, resident. Registries A
   and B, handles hA/hB, both register T. Registry-less `GetMeta(hash)`
   succeeds. Reset hA: B's cached `desc.meta` unchanged and valid. Reset hB:
   `Retained`, lookup still succeeds, link rows intact. A third registry
   registers T at the same address. Both teardown orders.
10. **Transient plugin across two registries (the original bug).** Same
    shape, plugin identity: first reset `Held`, second `Erased`, lookup null.
11. **Override then depart while others hold.** Engine anonymous in B,
    plugin handle overrides T in A (shadow push); content shows the plugin's
    fields; plugin resets: A restores the engine survivor, content shows the
    engine's fields, B's pointer never moved.
12. **Range purge releases per token** -- extends the existing purge test
    with a plugin-identity anonymous entry.
13. **Registry destruction releases nothing** -- destroy a registry holding
    anonymous T; lookup still works (regression guard for Arcane's
    registry-less reads).
14. Existing tests adjusted, not weakened: erase-on-pop-to-empty still holds
    for transient; `RegisterMetaAdoptsAndErasesOnDestruction` still holds;
    `InstallOwned` refusal tests compare against `Refused`;
    `EraseGuardsComponentLinkedHashes` moves to `MetaBinderTest` as
    "component-linked hash with live refs is `Held`, not erased".

**Gates.** TDD per task; three-config green (Debug/Release/Dist) with a
controller-independent verify; sanitizer lane for the concurrency smoke;
whole-branch review at opus effort (registration + concurrency diff); local
fast-forward to `dev`, branch deleted, no push.

## 5. Sequencing

Single Astra movement, one plan. Suggested task order for the plan (the
writing-plans pass owns the final breakdown): (1) identity + residency seam
and `SetTypeContext` parameter; (2) `MetaRegistry` binder stack + operations
+ unit tests; (3) `ComponentRegistry` slot accounting (`InstallResult`,
`MetaRelease`, tokens); (4) `ComponentModule` + drain / `ReflectType`
rewiring; (5) `UnregisterModuleRange`; (6) integration tests 9-13; (7)
caveat deletions, 2026-08-09 spec amendment, README/contract docs, three-
config gate. Arcane follow-up (separate, not this program): re-vendor (plugin
ABI bump), optionally `SetTypeContext(ctx, Resident)` in `Runtime::Impl` and
a Runtime-owned engine `ComponentModule` -- re-opens the 2026-08-10
ratification, which is Arcane's call.
