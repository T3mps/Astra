# Theme J — Public-API footguns (Registry / View surface)

**Date:** 2026-07-20
**Review of record:** `docs/reviews/2026-07-11-astra-full-review.md` §Theme J (lines 133-135)
**Status:** design approved (user), ready for `writing-plans`
**Branch:** off `dev` (currently `38f9579`); finish = merge-to-`dev` local FF, delete branch, do not push.

## 1. Problem

Four public-API footguns on the `Registry`/`View` surface. Lower severity than the UB
themes but high user-impact: each silently does the wrong thing (no-op, empty result, null
signal payload, or a crash) where the caller reasonably expects otherwise. None is a
memory-safety bug; all are correctness/ergonomics traps. (A fifth review item — the by-ID
batch APIs' "missing dead-handle filter" — was investigated during planning and found
benign; see §5.)

`Registry::Clear()`'s orphan-views footgun (must-fix item 8) is **already fixed** — it now
clears the `ArchetypeManager` in place (`Registry.hpp:1001-1006`), pinned by
`ClearOrphansViewTest`. It is out of scope here.

All fixes must build clean in all three configs (Debug/Release/Dist) and respect the
uniform-graceful misuse policy (no assert-and-abort on a recoverable condition).

## 2. The four fixes

### Fix 1 — `CreateEntities` / `CreateEntitiesWith` silently no-op
**Sites:** `Registry.hpp:143-178` (`CreateEntities`), `181-211` (`CreateEntitiesWith`).

Both return `void` and bail invisibly on a too-small output span:
```cpp
void CreateEntities(size_t count, std::span<Entity> outEntities)
{
    if (count == 0 || outEntities.size() < count)
        return;   // caller cannot tell nothing happened
    size_t created = m_entityManager.CreateBatch(count, outEntities.begin());
    ...
}
```
`CreateBatch` can also create fewer than `count` (id-exhaustion), which the code handles by
filling the tail with `Entity::Invalid()` — but the caller still cannot learn how many
succeeded.

**Fix:** change both return types `void` → `size_t`, returning the number of entities
actually created: `0` on the too-small-span bail, `created` (which may be `< count` on
id-exhaustion) otherwise. Source-compatible for callers that ignore the return; makes both
silent-failure modes observable. The too-small-span guard stays (create nothing, return 0) —
we do not clamp-and-create, which would hide a caller sizing bug.

### Fix 2 — `Registry(const Registry&, Config)` is a copy-shaped trap
**Site:** `Registry.hpp:78-86`.

```cpp
explicit Registry(const Registry& other, const Config& config = {}) :
    m_entityManager(config.entityManagerConfig),                 // fresh, empty
    m_componentRegistry(other.m_componentRegistry),              // shared (the only thing kept)
    m_archetypeManager(std::make_shared<ArchetypeManager>(...)), // fresh, empty
    m_relationshipGraph(std::make_shared<RelationshipGraph>()),  // fresh, empty
    ...
{}
```
Because the trailing `Config` parameter is defaulted, this **is** a copy constructor per
`[class.copy.ctor]/1`, so it suppresses the implicit copy constructor. `Registry b(a);`
therefore compiles and silently yields an **empty** registry that merely shares `a`'s
component-type registrations. The project's own `RegistryTest.CopyConstructor`
(`RegistryTest.cpp:526`) is named "copy" but only asserts the component registry is shared
and never checks entity state is copied — so it passes against the empty result, masking the
trap. (There is also a latent implicit shallow copy-assignment.)

Verified: no code anywhere constructs, moves, or copies a `Registry` **by value** — the only
`std::move` involving a registry is on a `std::unique_ptr<Registry>` in `Load`
(`Registry.hpp:~1580`); every construction site uses `make_unique`/`make_shared`/a stack
local. Registry is already effectively non-movable (the user-declared copy ctor suppresses
the implicit move). The `(const Registry&, Config)` ctor is used only by the misleading test.

**Fix (user-approved: non-copyable + explicit share accessor):**
- Delete the copy constructor and copy-assignment: `Registry(const Registry&) = delete;` and
  `Registry& operator=(const Registry&) = delete;` (also removes the latent shallow
  copy-assign). Registry becomes explicitly non-copyable (and stays non-movable, as today).
- Remove the `(const Registry&, Config)` ctor.
- Add an explicit accessor for the real use case (a second registry that shares another's
  component-type registrations, so `ComponentID`s line up):
  ```cpp
  ASTRA_NODISCARD std::shared_ptr<ComponentRegistry> ShareComponentRegistry() const noexcept
  { return m_componentRegistry; }
  ```
  The existing `Registry(std::shared_ptr<ComponentRegistry>, Config)` ctor (`:68`) then serves
  the case explicitly: `Registry world2(world1.ShareComponentRegistry(), config);`.
- Update + rename the test to reflect reality (shares the component registry, does **not**
  copy state) and add `static_assert(!std::is_copy_constructible_v<Registry>)`.

### Fix 3 — batch create emits `ComponentAdded` with `nullptr`
**Sites:** `Registry.hpp:174` (`CreateEntities`), `208` (`CreateEntitiesWith`).

```cpp
((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr)), ...);
```
The component exists post-creation (default-constructed for `CreateEntities`, generated for
`CreateEntitiesWith`), yet handlers receive `nullptr` and cannot inspect it — inconsistent
with single-entity `CreateEntity` (`:110`), which passes the real pointer via the entity
record.

**Fix:** inside the already-`IsSignalEnabled`-gated per-entity loop, look up the real pointer
(mirroring `CreateEntity`):
```cpp
auto* record = m_archetypeManager->GetEntityRecord(outEntities[i]);
if (record)
    ((m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(),
        record->archetype->GetComponent<Components>(record->location))), ...);
```
Cost is incurred only when the signal is enabled (already gated).

### Fix 4 — `View(nullptr)` crashes in the constructor
**Site:** `View.hpp:41-52`.

```cpp
explicit View(std::shared_ptr<ArchetypeManager> manager, ...) :
    m_archetypeManager(manager), ...
{
    CollectArchetypes();                                    // derefs manager
    m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(...);  // null deref
    ...
}
```
A null `manager` crashes, yet null is a supported state everywhere else — `IsValid()`
(`:58`, returns false) and `ForEach` (`:87`, returns early) both treat a null manager as an
empty/invalid view.

**Fix:** guard the body: `if (manager) { CollectArchetypes(); m_lastRefreshCounter = …; m_lastGeneration = …; m_lastRemovalCounter = …; }`. A null-constructed view is then empty/invalid and consistent with the rest of the class.

## 3. Design decisions (user-approved)

1. **Scope:** all four footguns ship as one Theme J work item (one brainstorm → spec → plan →
   SDD cycle). Cohesive (Registry/View public-API footguns), small, shared test infra. (The
   fifth candidate — a by-ID batch dead-handle filter — was dropped during planning as
   verified-benign; see §5.)
2. **Fix 1:** return `size_t` created count (chosen over clamp-and-create, which would hide a
   caller sizing bug).
3. **Fix 2:** make `Registry` non-copyable + add an explicit `ShareComponentRegistry()`
   accessor (chosen over a real deep copy — a large feature, YAGNI for a footgun fix — and
   over merely de-defaulting the trap ctor, which still reads copy-ish).

## 4. Test strategy

All tests append to existing files (`tests/Registry/RegistryTest.cpp`,
`tests/Registry/ViewTest.cpp`) — no new `.cpp`, no `ide/` regen. Reuse
`Astra::Test::Position`/`Velocity` → **zero new ComponentIDs** (respects the ~128 ceiling).

- **Fix 1:** `CreateEntities`/`CreateEntitiesWith` into an under-sized span → assert return
  `== 0` and nothing created; into an adequate span → assert return `== count`.
- **Fix 2:** compile-time `static_assert(!std::is_copy_constructible_v<Registry>)`; rework the
  existing `CopyConstructor` test to use `ShareComponentRegistry()` + the shared_ptr ctor and
  assert the two registries share the component registry but have independent entity state.
- **Fix 3:** register a `ComponentAdded` handler that captures the received component pointer;
  batch-create entities with a component and assert the handler saw a **non-null** pointer to
  the correct value (RED: null).
- **Fix 4:** `View<Position>(nullptr)` (a view over a null manager) → assert construction
  does not crash and the view reports empty/invalid, and `ForEach` over it is a no-op (RED:
  crash on construction).

New `.cpp` test files would need an `ide/` regen — but there are none here (all appends).
Match each file's `TEST`/`TEST_F` macro.

## 5. Scope / non-goals

- **In scope:** the five fixes above + their regression tests.
- **Out of scope:** `Clear()` orphan-views (already fixed); the other review themes (D/F/H);
  any deep-copy/clone feature for `Registry`; broad signal-emission redesign. Do not add move
  semantics to `Registry` unless a build break proves something moves it (none does today).
- **Verified-benign, dropped:** the review's by-ID batch "missing dead-handle filter"
  (`AddComponentsByID`/`RemoveComponentsByID`, `Registry.hpp:514,572`). `Entity` equality is
  `= default` over its packed id+version (`Entity.hpp:67`) and `ArchetypeManager::m_entityMap`
  is keyed by full `Entity`, so those functions' per-entity `m_entityMap.find` already skips
  every dead / recycled / garbage handle (a stale `(id, v1)` misses the live `(id, v2)` entry)
  and the returned count is already correct. The template siblings' up-front `IsValid` filter
  is a different route to the same outcome, not a correctness gain. Adding the filter would
  only add a `validEntities` allocation + `IsValid` pass to a path explicitly documented "does
  NOT emit signals for performance" — a regression for zero benefit. Intentionally not done.

## 6. Acceptance criteria

- All three configs (Debug/Release/Dist) build clean and green; controller-verified
  independently on the merge candidate.
- New regression tests fail RED before each fix and pass GREEN after; each would fail if the
  fix regressed.
- API changes are the intended ones only: `CreateEntities`/`CreateEntitiesWith` return
  `size_t`; `Registry` is non-copyable with a new `ShareComponentRegistry()` accessor. No
  other public signature changes. No new assert-and-abort on a recoverable condition.

## 7. Execution model

Reuse the proven SDD model: `writing-plans` → `subagent-driven-development`; fresh sonnet
implementer per task with a task-brief; opus review on every diff (Registry is core public
API); opus final whole-branch review; controller independent 3-config build+test; finish via
`finishing-a-development-branch` = merge to `dev` local FF, delete branch, do not push.

## 8. Files touched (anticipated)

- `include/Astra/Registry/Registry.hpp` — Fixes 1, 2, 3
- `include/Astra/Registry/View.hpp` — Fix 4
- `tests/Registry/RegistryTest.cpp` — Fixes 1, 2, 3 tests
- `tests/Registry/ViewTest.cpp` — Fix 4 test
