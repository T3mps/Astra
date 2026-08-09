# ComponentModule: RAII module-owned component registration

**Date:** 2026-08-09
**Status:** Approved (design review with user, section-by-section)
**Scope:** Astra library change + Arcane migration (one program, two movements)
**Origin:** 2026-08-09 Astra-in-Arcane integration audit (artifact:
https://claude.ai/code/artifact/b601daa3-fb36-48ec-a341-2ee23a2f2fea)

## 1. Problem

Astra has no object representing "the set of component types this module owns."
Ownership of a `ComponentDescriptor` (12 raw function pointers into whichever
module registered the type) is a side effect of who called
`RegisterComponent<T>` / `ReRegisterComponent<T>` last, and can only be undone
by address-range probing (`UnregisterModuleRange`). The same gap exists twice
over for reflection: `MetaRegistry` has **no unregister API at all**, and
`Register(TypeMeta&&)` is idempotent-on-hash-hit, so a hot-reloaded module's
static-init meta drain **silently discards the new module's field closures** —
they never rebind to the new image.

Measured consequences in Arcane (first production consumer, audit 2026-08-09):

- 4 shipped bugs 2026-07-26 → 2026-07-31, all in this class (silent roster
  drop, TypeID aliasing, project-switch AV, tag-component display bug adjacent).
- 17 hand-transcribed `ReRegisterComponent` lines across 3 plugins, with
  confirmed drift (`Camera` added to the engine roster 2026-07-29; no plugin
  updated).
- A 7-step hand-ordered teardown ritual in `PluginHost::TeardownImage`
  (PluginHost.cpp:227-285).
- **Live bug:** secondary plugins are never disowned — `plugins.clear()`
  (PluginHost.cpp:575, :312) unmaps without a purge; HotReloadPlugin registers
  `Pulse` as a secondary → permanently dangling descriptor.
- Both Arcane hosts deliberately heap-leak the shared `TypeContext` forever
  (EditorApp.cpp:169-179, RuntimeApp.cpp:46-59) because destroying the
  MetaRegistry would run `~std::function()` over closures compiled into
  unloaded plugin code.

## 2. Decisions (user-approved)

| Decision | Choice |
|---|---|
| V1 ownership scope | **Descriptors + meta** (full fix, including rebind-on-reload) |
| Back-compat | `RegisterComponent` stays (simple path, anonymous owner 0); **`ReRegisterComponent` is deleted**; `ComponentModule` is the one complex path; `UnregisterModuleRange` kept as fallback net |
| Ownership model | **Approach A: per-slot owner stack** (live array + sparse shadow store), not single-owner transfer |
| Program reach | **Astra + Arcane migration together** — `ReRegisterComponent` removal makes vendor sync and Arcane migration inseparable |

## 3. Design

### 3.1 The handle

```cpp
// include/Astra/Component/ComponentModule.hpp (new)
class ComponentModule
{
public:
    static ComponentModule Open(std::shared_ptr<ComponentRegistry> registry,
                                std::string_view name);

    template<Component... Ts> void Register();      // descriptors + component metas
    template<typename... Ts>  void RegisterMeta();  // non-component reflected types

    ~ComponentModule();                             // removes this handle's entries wherever
                                                    // they sit; restores newest shadow + meta
    ComponentModule(ComponentModule&&) noexcept;    // moved-from = empty; dtor no-ops
    ComponentModule(const ComponentModule&) = delete;
};
```

- `Open` takes the `shared_ptr` **explicitly** (no `enable_shared_from_this`):
  no footgun with stack-constructed registries, and the handle keeps the
  registry alive by construction (handle-outlives-registry is impossible).
- Internally: `{shared_ptr<ComponentRegistry>, uint32_t moduleId}`. The
  registry issues monotonic module ids and keeps names for diagnostics.
- `Register<Ts...>` is a template instantiated **in the calling module** — that
  is what binds descriptors (and meta thunks) to the caller's image. This is
  the load-bearing property; document it prominently.

### 3.2 Slot storage: live array + sparse shadow store

`m_components[MAX_COMPONENTS]` stays exactly as-is: the pointer-stable,
directly-indexed **live** descriptor array. Every existing
`GetComponentDescriptor` caller and cached pointer is untouched.

Added:

- `uint32_t m_owner[MAX_COMPONENTS]` — owner of the live entry (0 = anonymous
  `RegisterComponent`). ~512 B.
- Sparse shadow store: `FlatMap<ComponentID, SmallVector<ShadowEntry, 1>>`,
  empty in the common case; a slot only occupies it while genuinely overridden
  (realistically depth 1, only during a hot-reload window).
- `ShadowEntry = { uint32_t owner; ComponentDescriptor desc; MetaRebindFn metaRebind; }`.

Operations (all under `m_registrationMutex`):

- **Push** (module registers a type whose live entry belongs to someone else):
  move live entry (+ its owner + meta thunk) into the shadow list, install the
  new live entry.
- **Same-module re-register:** rebind own live entry in place; no shadow push,
  no growth.
- **Remove own entry** (handle destruction / explicit reset): if mine is live —
  restore the newest shadow entry into the live array (or clear the slot to a
  clean `nullptr` miss if none) and invoke its meta rebind; if mine is
  shadowed — drop it from the list.

ComponentIDs are **never freed** — the TypeContext hash→id mapping persists
(serialization identity depends on it). A cleared slot keeps its id reserved.

### 3.3 Meta rebind thunks

- `using MetaRebindFn = void(*)();` — each registration path captures
  `&RebindMetaThunk<T>`, a per-T-per-module function that rebuilds T's
  `TypeMeta` from the calling module's reflect data and installs it **in place
  at the stable `TypeMeta*`** (descriptors cache that pointer; contents swap,
  address doesn't).
- `Detail::StaticTypeRegistrar<T>` additionally retains its built meta as a
  per-module factory (today the builder output is consumed once and enqueued;
  it must remain reachable per-type so the thunk can rebuild).
- `MetaRegistry` gains `RebindInPlace(TypeMeta&&)`: swaps contents under the
  meta mutex, preserves the `componentId` backref and address, applies the same
  identity-collision refusal as `Register`.
- Slot transitions drive meta transitions: push rebinds T's meta to the
  pusher's closures; pop rebinds to the restored owner's (legal — a live shadow
  entry's module is by definition still mapped). Unreflected types carry a null
  thunk and skip the meta step (matches today's nullable `desc.meta`).
- **Non-component reflected types** (plugin-private enums/structs — reachable
  via `GetByName`/`GetMeta`/`ForEachType` even after their component is gone):
  explicit `RegisterMeta<Ts...>()` adopts/rebinds the entry the static-init
  drain already installed (or installs it fresh if the drain has not run /
  the type was never enqueued) and tags it with this owner; destruction
  **erases** it. Engine-shared reflected types stay anonymous first-wins (accurate:
  Arcane.dll never unloads). Erasure invalidates cached `TypeMeta*` for those
  types only — consumers use meta pointers transiently; documented.

### 3.4 Changed / removed API

- **`ReRegisterComponent<T>` — deleted.** Module handles are the only rebind
  mechanism.
- `RegisterComponent<T>` — unchanged signature/semantics (idempotent
  first-touch); now records owner 0 and captures the meta thunk.
- `UnregisterModuleRange` — kept as the fallback net for modules that never
  adopted the RAII path; **extended** to also strip matching shadow entries
  and, when it strips a live entry, re-materialize the newest surviving shadow
  (so the two mechanisms cannot disagree).
- `ResourceStorage::Set<T>` auto-registration stays anonymous owner-0.
- `m_componentNames`: entries reused by hash on re-registration instead of
  appended forever (kills the hot-reload deque growth).

### 3.5 Flows (Arcane as the concrete script)

1. **Engine boot** — `Runtime::Impl` gains
   `ComponentModule engineModule` (declared **after** `components` so
   destruction order is automatic), opened as `"Arcane.Engine"`, registering
   the 11-type roster.
2. **Plugin Init** — plugin opens its own handle (plugin-side static),
   registers **only its own types**. The 16 engine-type `ReRegisterComponent`
   lines across Sandbox/PlaygroundGame are deleted, not migrated.
   PlaygroundGame registers nothing (owns no types).
3. **Plugin Shutdown** — `g_module.reset()` (runs before unmap by the existing
   vtable contract). `TeardownImage` keeps registry-reset-while-mapped (live
   component *instances* still need destructors) and the range-purge net, and
   **drops step 7** (engine roster re-registration) — engine entries were never
   displaced.
4. **Hot reload, load-before-unload** (real PluginHost order): gen N+1 pushes
   over gen N's live entry; gen N's destruction finds its entry shadowed and
   drops it. No unregistered window.
5. **Unload-before-load:** gen N's destruction pops to a clean `nullptr` miss
   (`SkippedUnregistered`, not a call into freed code); gen N+1 registers
   fresh. Both orders correct with no coordination.
6. **Secondary plugins** — same pattern per plugin; the disown hole disappears
   because cleanup is the handle destructor, not a primary-only host ritual.

Ownership collision (module registers a type whose live entry belongs to
another **live** module): allowed — it is the reload window — but logged at
info level with both module names, so needless overrides are visible.

### 3.6 Error handling & edges

- All existing refusals unchanged: identity collision → `INVALID_COMPONENT`
  (Theme E), `id >= MAX_COMPONENTS`, over-aligned. Module `Register` routes
  through the same `RegisterComponentImpl` core; a refused type is never owned.
- **Lock order:** `m_registrationMutex` → meta mutex, never the reverse; no
  user callback under either (C3 lesson).
- Moved-from handle: empty, dtor no-ops. Double reset: idempotent. Two handles
  from one DLL: allowed, distinct owners.
- Plugin forgets `Shutdown` cleanup: handle stays alive (holds the registry
  `shared_ptr`); descriptors dangle at unmap exactly like today; the range
  purge catches the descriptor half. `RegisterMeta`-owned metas cannot be
  address-probed — documented as the one thing only the RAII path cleans.
- POSIX: RAII path needs no image spans; the platform gap only affects the
  fallback net (unchanged from today).

### 3.7 Non-goals

- No freeing/recycling of ComponentIDs.
- No archive/serialization format change.
- No automatic ownership of drain-registered metas (explicit `RegisterMeta`
  only).
- Arcane's TypeContext heap-leak removal: becomes *safe* after this program but
  is scheduled separately as Arcane hygiene.

## 4. Testing

**Astra** (TDD; new `tests/Component/ComponentModuleTest.cpp`; **reuse existing
`Astra::Test::*` component types — test binary is near the 128-TypeID
ceiling**):

1. Open/Register/lookup; destruction clears owned slots to clean `nullptr`.
2. Shadow mechanics: push/restore; both hot-reload orders (two handles = two
   owners; DLL boundaries not required to exercise owner semantics); mid-stack
   removal; same-module re-register (no shadow growth).
3. Meta: rebind-in-place keeps `TypeMeta*` stable, swaps contents, preserves
   `componentId` backref; `RegisterMeta` adopt-then-erase; null-thunk path.
4. `UnregisterModuleRange` unification: strips shadow entries; re-materializes
   newest survivor when stripping a live entry.
5. Collision refusal through the module path; concurrency smoke under the
   registration mutex; moved-from/double-reset.
6. Migrate any Astra-internal `ReRegisterComponent` uses.
7. Full 3-config verify (Debug/Release/Dist), independent controller run.

**Arcane** (acceptance, after vendor sync): `[hotreload]` suite,
`PluginHostTest`, `EditorComponentCatalogTest`, plus a **new regression test
for the secondary-plugin disown hole**. ABI bump to **v10**.

## 5. Sequencing

1. **Astra movement:** branch → SDD (spec → plan → TDD tasks) → whole-branch
   OPUS review → 3-config → local FF-merge to dev, delete branch, do not push.
2. **Arcane movement (inseparable pair):** `scripts\sync-astra.ps1` vendor bump
   + migration (Runtime engineModule, PluginHost step-7 deletion + net
   retention, plugin rosters deleted/replaced, ABI v10) + Arcane test suite.
   The vendor sync must not run before the migration is ready —
   `ReRegisterComponent` no longer exists after this lands.
