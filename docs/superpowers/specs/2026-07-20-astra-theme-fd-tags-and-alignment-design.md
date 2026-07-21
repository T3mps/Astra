# Themes F + D — Empty/tag component correctness + SmallVector alignment

**Date:** 2026-07-20
**Review of record:** `docs/reviews/2026-07-11-astra-full-review.md` §Theme F (line 112-114), §Theme D (line 96-104)
**Status:** design approved (user), ready for `writing-plans`
**Branch:** off `dev` (currently `dc0af4b`); finish = merge-to-`dev` local FF, delete branch, do not push.

## 1. Problem

A bundle of two Phase-4 themes, scoped down after verifying current state:

- **Theme F (empty/tag asymmetries)** — three real, reachable bugs where a zero-size
  (tag) component is treated inconsistently: an asymmetric `Destruct`, dropped by-ID
  signals, and a `Get`/`Has` contradiction.
- **Theme D (alignment)** — most of the review's Theme D is **already handled** (see §5);
  the one clearly-reachable bug is **SmallVector** using non-alignment-aware allocation.

None is a security issue. All are correctness/consistency floors. All fixes must build clean
in all three configs (Debug/Release/Dist) and respect the uniform-graceful misuse policy.

## 2. The fixes

### Shared piece — the empty-component sentinel
A tag component is **present but has no data**. The typed paths already return a non-null
static instance for tags (`ArchetypeChunkPool::GetComponent` returns `&emptyInstance`;
`EmptyTagTest.RangeForBindsTagAtValidAddress` pins a non-null tag address). The **type-erased**
paths (by-hash/name `Get`, by-ID signals) still use the chunk-array base, which is `nullptr`
for tags — hence the inconsistency. Introduce one canonical non-null sentinel so the
type-erased paths agree with `Has` and with the typed paths.

Add to `include/Astra/Component/Component.hpp` (namespace `Astra`):
```cpp
// Stable, non-null pointer used as the "component pointer" for a present zero-size (tag)
// component: by-hash/name Get returns it and by-ID signals fire with it, so Get(...) !=
// nullptr agrees with Has(...). It points at a real static byte; callers must NOT read
// through it (a tag has no data -- desc.size == 0). All tags share this one address; the
// ComponentID carried alongside identifies the type.
inline void* EmptyComponentSentinel() noexcept
{
    static std::byte sentinel{};
    return &sentinel;
}
```

### Fix F1 — `Component::Destruct` skips `size==0`
**Site:** `include/Astra/Component/Component.hpp:154-157`.
```cpp
inline void Destruct(void* ptr) const { destruct(ptr); }   // no size guard
```
`DefaultConstruct` (`:84-102`) skips `size==0`, but `Destruct` does not — so the descriptor
is self-inconsistent. The chunk paths all guard `base==nullptr` before calling `Destruct`, so
this is largely latent, but the fix is a cheap, correct symmetry that prevents UB if any
current/future path calls `Destruct` on a tag.
```cpp
inline void Destruct(void* ptr) const
{
    if (size == 0) return;  // empty (tag) component: nothing to destruct (mirrors DefaultConstruct)
    destruct(ptr);
}
```

### Fix F2 — by-ID paths emit tag signals
**Sites:** `Registry::AddComponentByID` (`Registry.hpp:485-501`), `RemoveComponentByID`
(`:533-557`).

Both compute the signal's component pointer from the chunk array and emit only `if (compPtr)`.
For a tag the chunk-array base is `nullptr`, so **no signal fires** — yet typed
`AddComponent<Tag>` does fire. Fix: fold in the sentinel so a tag's pointer is non-null.
```cpp
// AddComponentByID signal block (inside the existing IsSignalEnabled + record + desc guards):
void* actualPtr = (desc->size == 0)
    ? EmptyComponentSentinel()
    : /* existing chunk-array lookup: compPtr ? compPtr + idx*desc->size : nullptr */;
if (actualPtr)
    m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
```
Same shape for `RemoveComponentByID`'s `ComponentRemoved` emission (emit-before-remove is
preserved). A tag add/remove via the by-ID path now emits with the sentinel.

### Fix F3 — `GetComponentByHash`/`Name` returns the sentinel for a present tag
**Sites:** `Registry::GetComponentByHash` (`Registry.hpp:596-624`); `GetComponentByName`
(`:815-819`) delegates to it, so it is fixed for free.

Currently presence is inferred from the chunk-array base (`if (!compArray) return nullptr;`),
which is `nullptr` for a tag → returns `nullptr` even though `HasComponentByHash` (which tests
`record->archetype->GetMask().Test(componentId)`, `:659`) returns `true`. Fix: use the same
mask-based presence check.
```cpp
// after record + desc are resolved, replace the chunk-array-null branch with:
if (!record->archetype->GetMask().Test(componentId))
    return nullptr;                       // entity does not have the component
if (desc->size == 0)
    return EmptyComponentSentinel();      // present tag: no data
void* compArray = chunks[record->location.GetChunkIndex()]->GetComponentArrayByID(componentId);
if (!compArray)
    return nullptr;                       // defensive (should not happen for size > 0)
return static_cast<std::byte*>(compArray) + record->location.GetEntityIndex() * desc->size;
```
Now `Get != nullptr` agrees with `Has` for tags. Non-empty components are unchanged
(mask has the id, `size > 0`, real pointer). The existing `GetComponentByHash` reflection
tests use a non-empty component and are unaffected.

### Fix D2 — SmallVector alignment-aware allocation
**Sites:** `include/Astra/Container/SmallVector.hpp` — 7 raw allocation sites: `::operator new`
at `:310` (`shrink_to_fit`), `:584` (`Grow`); `::operator delete` at `:124`, `:145`, `:302`,
`:313`, `:595`.

Once `T` spills past the (correctly aligned) inline buffer, `Grow`/`shrink_to_fit` use plain
`::operator new`, which only guarantees `__STDCPP_DEFAULT_NEW_ALIGNMENT__` (16 on x64). A public
`SmallVector<OverAlignedT>` (e.g. `alignas(64)`) that spills to heap gets under-aligned storage
→ UB on access. Fix: centralize allocation into two private static helpers that use aligned
new/delete when the type demands it, and route all 7 sites through them.
```cpp
static T* Allocate(size_t count)
{
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{alignof(T)}));
    else
        return static_cast<T*>(::operator new(count * sizeof(T)));
}
static void Deallocate(T* ptr) noexcept
{
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        ::operator delete(ptr, std::align_val_t{alignof(T)});
    else
        ::operator delete(ptr);
}
```
Aligned `new`/`delete` must be paired (mismatched forms are UB), which the `if constexpr` on
`alignof(T)` guarantees since both helpers branch identically. Requires `<new>` for
`std::align_val_t`.

## 3. Design decisions (user-approved)

1. **Theme D scope = D2 only.** The chunk-pool over-alignment (D1) is already **all-config
   refused** at registration; `Delegate` SBO is already `alignas(std::max_align_t)` (D4);
   `CommandBuffer` already refuses over-aligned components (D5). Only SmallVector (D2) is a
   reachable bug. D1 (honor-alignment feature), D3 (ARM64 SBO), and a `Delegate`
   functor-alignof>16 static_assert are deferred as tracked follow-ups (§5).
2. **Tag semantics = a non-null empty-component sentinel** (chosen over null-but-document), so
   `Get`/`Has` agree for tags and by-ID signals fire, all via one shared address.
3. **F1 included** — the cheap `Destruct` symmetry fix.

## 4. Test strategy

Reuse existing `Astra::Test::*` tags (`Player`) where possible → **+1 TypeID** total (a counting
tag for F1). All tests append to existing files (no new `.cpp`, no `ide/` regen).

- **F1** — register a lifetime-counting tag (`struct CountedTag { static int s_live; CountedTag(){++s_live;} ~CountedTag(){--s_live;} };`, `is_empty` so `desc.size==0`); get its descriptor; construct one live instance; call `desc.Destruct(&live)`; assert `s_live` unchanged (RED: decremented — the dtor ran).
- **F2** — reuse `Astra::Test::Player`; enable signals + register a handler capturing the received pointer; `AddComponentByID(e, TypeID<Player>::Value(), nullptr, 0)` then `RemoveComponentByID`; assert each handler fired with a **non-null** pointer (RED: no fire).
- **F3** — entity with `Player`; assert `GetComponentByHash(e, TypeID<Player>::Hash()) != nullptr` **and** equals `HasComponentByHash(...)` (RED: nullptr while Has is true).
- **D2** — a file-local `struct alignas(64) Over { std::byte b[64]; };` (not a component → 0 TypeIDs); push past the inline capacity so it spills to heap; assert `reinterpret_cast<uintptr_t>(v.data()) % 64 == 0` (RED: only 16-aligned).

## 5. Scope / non-goals

- **In scope:** F1, F2, F3, D2 + their regression tests + the sentinel.
- **Verified already-handled (do NOT re-fix):**
  - D1 chunk-pool over-alignment — refused all-config at `ComponentRegistry.hpp:156-159`
    (returns in every config; misalignment is unreachable, not a bug — a documented limitation).
  - D4 `Delegate` SBO — already `alignas(std::max_align_t)` (`Delegate.hpp:330`).
  - D5 `CommandBuffer` — already refuses over-aligned components (`:417,495,698`).
- **Deferred as tracked follow-ups (non-blocking):**
  - D1' **honor** `alignof > 64` in the chunk pool (a feature: aligned block allocation +
    per-array alignment + dropping the refusal) — only if Astra ever needs over-aligned
    (128+) components.
  - D3 `ResourceStorage` `alignas(64)` vs `CACHE_LINE_SIZE == 128` on ARM64 — non-x64 edge.
  - D4' a `Delegate` static_assert rejecting functors with `alignof > alignof(std::max_align_t)`
    (currently silent-UB for exotic over-aligned functors).
  - F1' auditing non-chunk `Destruct` callers (e.g. `ResourceStorage`) for an empty-resource
    path that reaches `Destruct` — the F1 fix already makes it safe regardless.

## 6. Acceptance criteria

- All three configs (Debug/Release/Dist) build clean and green; controller-verified
  independently on the merge candidate.
- New regression tests fail RED before each fix and pass GREEN after.
- For tags: `Get…ByHash/Name != nullptr` iff `Has…` is true; by-ID `ComponentAdded`/`Removed`
  fire for tags with the sentinel; the descriptor's `Destruct` is a no-op for `size==0`.
- `SmallVector<OverAlignedT>` heap storage is aligned to `alignof(T)`.
- No public API signature change; no new assert-and-abort on a recoverable condition.

## 7. Execution model

Reuse the proven SDD model: `writing-plans` → `subagent-driven-development`; fresh sonnet
implementer per task with a task-brief; opus review on every diff (core component/registry +
a container-alignment diff); opus final whole-branch review; controller independent 3-config
build+test; finish via `finishing-a-development-branch` = merge to `dev` local FF, delete
branch, do not push.

## 8. Files touched (anticipated)

- `include/Astra/Component/Component.hpp` — sentinel + F1
- `include/Astra/Registry/Registry.hpp` — F2 + F3
- `include/Astra/Container/SmallVector.hpp` — D2
- `tests/Component/…` (F1), `tests/Registry/…` (F2, F3), `tests/Container/SmallVectorTest.cpp` (D2)
  — exact files pinned in the plan; all appends, no `ide/` regen
