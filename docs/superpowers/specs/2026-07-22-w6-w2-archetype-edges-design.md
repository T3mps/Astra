# W6 + W2 — Hash Hygiene + Array-Indexed Archetype Edges

**Date:** 2026-07-22
**Status:** Approved design → ready for implementation planning
**Source:** Work items **W6** and **W2** of `docs/reviews/2026-07-21-astra-perf-optimization-plan.md`
**Follows:** W1 (unified paged entity record, landed 2026-07-22 @ dev `13ee47b`)
**Related:** `bench-compare/RESULTS.md`

---

## 1. Context & goal

Post-W1, Astra's remaining structural-churn gaps to flecs (same archetype model) are **add 150.5 ns vs
53.5 (2.8×)** and **remove 105.6 vs 32.2 (3.3×)** — the largest remaining ratios. Two independent causes
sit on the add/remove path:

- **The archetype-edge lookup** (finding "which archetype do I transition to when adding/removing
  component C?") is **two hashed `FlatMap` probes**, the outer one keyed by `Archetype*` and hitting a
  SwissTable degeneracy that turns the SIMD group filter into a linear probe.
- **The transition *move*** (copying components into the destination chunk) — a separate cost, addressed
  by **W3**, out of scope here.

This work removes the first cause. It has two parts:

- **W6** (general hygiene): fix the SwissTable hash degeneracy at its root so *every* `FlatMap` is robust to
  any hasher — including pointer/identity keys — by avalanche-mixing the hash before the H1/H2 split.
- **W2** (the structural win): replace the pointer-keyed edge map entirely with **per-archetype edge
  arrays indexed directly by `ComponentID`**, so an edge lookup is a single array index, no hashing.

W6 is done first as cheap defense-in-depth; W2 then *eliminates* the pointer-keyed edge map that was W6's
primary beneficiary — so W6's standalone value is container robustness, not this benchmark. Realistic target:
add 150→~100-110, remove 106→~70 (the edge-lookup slice); flecs parity awaits W3's memcpy move.

---

## 2. Current state (verified against source)

### 2.1 The hash degeneracy (W6)
`FlatMap::SplitHash` forwards the raw hash unchanged:
```cpp
// include/Astra/Container/FlatMap.hpp:918
static std::pair<std::size_t, std::uint8_t> SplitHash(std::size_t hash) noexcept {
    std::uint8_t h2 = SwissTable::H2(hash);   // top 7 bits
    return {hash, h2};                         // h1 = full hash, masked later by H1()
}
```
`SwissTable` (`include/Astra/Container/Swiss.hpp`): `H2(hash) = ((hash>>57)&0x7F)`, folding 0→1
(`:45-53`); `H1(hash,cap) = hash & (cap-1)` (`:55-58`). `size_t` is statically asserted 64-bit (`:44`).

For a pointer key hashed by `std::hash<T*>` (identity): the top 7 bits of an x64 user-space heap pointer are
constant ⇒ **H2 ≡ 1 for every such key** ⇒ `Group::Match(1)` matches all occupied slots ⇒ the SIMD filter
degenerates to a full linear probe. The low bits also cluster (16-byte-aligned pointers zero the low 4
bits, which `H1` masks). Well-distributed hashers (`EntityHash` mixes via `HashCombine`; `BitmapHash` for
`ComponentMask`) are unaffected — this bites identity/pointer hashers specifically.

### 2.2 The archetype-edge map (W2)
`ArchetypeGraph` (`include/Astra/Archetype/ArchetypeGraph.hpp`) stores:
```cpp
FlatMap<Archetype*, FlatMap<ComponentID, Archetype*>> m_addEdges;      // :127
FlatMap<Archetype*, FlatMap<ComponentID, Archetype*>> m_removeEdges;   // :128
```
`GetEdgeInternal` (`:41-53`) does **two** probes per lookup: `m_addEdges.Find(from)` (pointer-keyed →
the degenerate case) then `it->second.Find(componentId)`. Consumed by `GetArchetypeWithModified`
(`ArchetypeManager.hpp:1020`), which checks the edge cache first, else computes the new mask, looks up
`m_archetypeMap` (ComponentMask-keyed, fine), creates the archetype if absent, and caches the edge via
`setEdge`. Wrappers `GetArchetypeWithAdded/Removed` (`:1084/:1092`) pass lambdas
`graph.GetAddEdge`/`SetAddEdge` etc.

Edge lifecycle: `SetAddEdge/SetRemoveEdge` (`ArchetypeGraph.hpp:23-39`) populate on demand;
`RemoveEdgesTo(target)` (`:88`) scans all edges and erases those pointing to a deleted archetype;
`RemoveEdgesFrom(from)` (`:97`) erases the deleted archetype's own outgoing edges; both are called on
archetype deletion (`ArchetypeManager.hpp:730-731`). `Clear()` (`ArchetypeManager.hpp:613`) wipes the
graph. `m_edgeGraph` member at `ArchetypeManager.hpp:1517`; `m_archetypes` is a
`std::vector<ArchetypeEntry>` where each entry owns a `std::unique_ptr<Archetype>`.

`MAX_COMPONENTS = 128` (`Component.hpp:20`, `ASTRA_MAX_COMPONENTS`) — it is the `ComponentID` ceiling, so
**every valid id fits in a `MAX_COMPONENTS`-sized array**; no high-id fallback is needed (unlike flecs's
two-tier `lo`/`hi`).

Edges are a **runtime cache only** — never serialized (rebuilt lazily), so neither part touches the on-disk
format.

---

## 3. Part 1 — W6: avalanche mix in `SplitHash`

Apply a murmur3 `fmix64` finalizer to the hash before splitting. Because `H1` uses the *low* bits and `H2`
the *top 7* bits, the mix must avalanche **both** ends; `fmix64` does.

```cpp
// include/Astra/Container/FlatMap.hpp — SplitHash
static std::pair<std::size_t, std::uint8_t> SplitHash(std::size_t hash) noexcept
{
    // Avalanche so both H1 (low bits, position) and H2 (top 7 bits, tag) get full
    // entropy even for identity hashers (raw pointers, small ints) — otherwise
    // e.g. heap-pointer keys share their top bits, H2 is constant, and the SIMD
    // group filter degenerates to a linear probe. (murmur3 fmix64)
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    std::uint8_t h2 = SwissTable::H2(hash);
    return {hash, h2};
}
```

- Cost: ~6 integer ops per hash split, only on lookup/insert paths (iteration never hashes). Negligible
  against a ~100 ns structural op; well-distributed hashers pay it redundantly but harmlessly.
- `EntityHash`'s manual H2-fixup (`Entity.hpp:179-183`) becomes redundant but harmless — **leave it** to
  minimize churn (a separate cleanup if desired).
- Applies centrally, so no per-key-type hasher changes are needed.

**Risk — iteration order changes.** `FlatMap` iteration order is hash-dependent and will change. Nothing in
Astra's persistence should depend on it (archetype/entity serialization is data-driven, not map-order-
driven), but the full serialization + container suites are the net.

**Test (W6).** In `tests/Container/FlatMapInternalsTest.cpp`: assert `SplitHash` over aligned-pointer-like
inputs (`i * 16` for a range of `i`) produces a well-spread set of `H2` values (not a single constant), and
that a `FlatMap<void*, int>` populated with many 16-byte-aligned keys inserts/finds all correctly (a
functional guard that the previously-degenerate case now probes sanely). Reuse existing patterns/types in
that test file.

---

## 4. Part 2 — W2: per-archetype lazy edge arrays

### 4.1 `Archetype` (edge storage moves here)
Add to `include/Astra/Archetype/Archetype.hpp`:
```cpp
// Add/remove transition edges, indexed directly by ComponentID (all ids < MAX_COMPONENTS).
// Lazily allocated on first edge; nullptr slot = no cached edge. Freed with the archetype.
std::unique_ptr<Archetype*[]> m_addEdges;      // nullptr until first SetAddEdge
std::unique_ptr<Archetype*[]> m_removeEdges;   // nullptr until first SetRemoveEdge

ASTRA_NODISCARD Archetype* GetAddEdge(ComponentID id) const noexcept
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    return m_addEdges ? m_addEdges[id] : nullptr;
}
ASTRA_NODISCARD Archetype* GetRemoveEdge(ComponentID id) const noexcept
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    return m_removeEdges ? m_removeEdges[id] : nullptr;
}
void SetAddEdge(ComponentID id, Archetype* to)
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    if (!m_addEdges) m_addEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);  // value-inits to nullptr
    m_addEdges[id] = to;
}
void SetRemoveEdge(ComponentID id, Archetype* to)
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "component id out of range");
    if (!m_removeEdges) m_removeEdges = std::make_unique<Archetype*[]>(MAX_COMPONENTS);
    m_removeEdges[id] = to;
}
// Null every edge (add or remove) pointing to `target` — for invalidation when `target` is deleted.
void ClearEdgesTo(Archetype* target) noexcept
{
    if (m_addEdges)    for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_addEdges[i]    == target) m_addEdges[i]    = nullptr;
    if (m_removeEdges) for (ComponentID i = 0; i < MAX_COMPONENTS; ++i) if (m_removeEdges[i] == target) m_removeEdges[i] = nullptr;
}
```
`std::make_unique<Archetype*[]>(MAX_COMPONENTS)` value-initializes all slots to `nullptr`. The arrays are
owned by the `Archetype` and freed on its destruction. `Archetype` only stores the pointers (forward
declaration of `Archetype` within its own class is implicit).

### 4.2 `ArchetypeManager` (rewire + own invalidation)
- `GetArchetypeWithModified` (`:1020`): replace `getEdge(m_edgeGraph, from, id)` with `from->GetAddEdge(id)`
  / `from->GetRemoveEdge(id)`, and `setEdge(m_edgeGraph, from, id, to)` with `from->SetAddEdge(id, to)` /
  `from->SetRemoveEdge(id, to)`. Simplify the `GetArchetypeWithAdded/Removed` lambdas to operate on `from`
  directly (drop the `graph` parameter): e.g.
  `[](Archetype* f, ComponentID id){ return f->GetAddEdge(id); }`,
  `[](Archetype* f, ComponentID id, Archetype* to){ f->SetAddEdge(id, to); }`.
- Delete the `ArchetypeGraph m_edgeGraph;` member (`:1517`) and its include.
- **Invalidation on archetype deletion** (`:730-731`): replace
  `m_edgeGraph.RemoveEdgesTo(archetype); m_edgeGraph.RemoveEdgesFrom(archetype);` with a scan over the
  archetype list that nulls incoming edges, before the archetype is freed:
  ```cpp
  for (auto& entry : m_archetypes)
      if (entry.archetype && entry.archetype.get() != archetype)
          entry.archetype->ClearEdgesTo(archetype);
  // (edges FROM `archetype` — its own arrays — are freed when the unique_ptr is destroyed)
  ```
  Confirm during planning whether the deleted archetype is actually freed vs pooled/reused; if reused, its
  edge arrays must be reset on reuse (reset the `unique_ptr`s or null them).
- Remove `m_edgeGraph.Clear()` (`:613`) — clearing the archetypes disposes their edge arrays.

### 4.3 Delete `ArchetypeGraph`
With storage in `Archetype` and invalidation in `ArchetypeManager`, `ArchetypeGraph` has no remaining
responsibility. **Delete `include/Astra/Archetype/ArchetypeGraph.hpp`** and its `#include`. First grep the
tree (src + tests) for `ArchetypeGraph` / `GetEdgeCount` / `m_edgeGraph`; adapt or remove any consumer (e.g.
a test asserting `GetEdgeCount()` → replace with a sum over `m_archetypes` of populated edge slots, or drop
the assertion if it tested the old internal structure).

---

## 5. Correctness — the one real hazard

**Dangling edges on archetype deletion (UAF).** When an archetype `T` is deleted, every edge (in any
archetype) pointing to `T` must be nulled **before** `T`'s memory is freed; otherwise a later
`GetAddEdge/GetRemoveEdge` returns a dangling `Archetype*` → use-after-free. The scan-all invalidation
(§4.2) must be complete and ordered before the free. This exactly reproduces the guarantee the old
`RemoveEdgesTo` provided (it scanned the edge maps); W2 scans the archetype arrays instead. Deletions are
rare (empty-archetype cleanup), so O(N × MAX_COMPONENTS) per deletion is an acceptable trade for O(1)
hot-path lookups.

**Test:** create a chain of archetypes with cached add/remove edges, delete a middle archetype, and assert
that no surviving archetype's `GetAddEdge/GetRemoveEdge` resolves to the deleted one (all such slots read
`nullptr`), and that subsequent add/remove operations that would have used those edges recompute correctly.

---

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| W6 changes `FlatMap` iteration order → could break an order-dependent consumer/format | Serialization + container suites are the net; archetype/entity persistence is data-driven, not map-order-driven. Verify green 3-config. |
| W2 dangling edge after archetype deletion (UAF) | Scan-all invalidation before free (§5) + a dedicated deletion test. |
| Archetypes are pooled/reused rather than freed | Planning step: confirm the deletion path; reset edge arrays on reuse if pooled. |
| `ComponentID` ≥ `MAX_COMPONENTS` indexing the arrays | Callers already guard `id < MAX_COMPONENTS` (typed + ByID add/remove paths); asserts added in the accessors. |
| Memory: lazy arrays are `MAX_COMPONENTS × 8 B × 2` per transitioning archetype | Lazy allocation (chosen) keeps it proportional to actual transitions; freed with the archetype. |
| A stale hidden consumer of `ArchetypeGraph` | Grep src + tests before deleting the header; adapt/remove consumers. |

---

## 7. Out of scope
- **W3** (the transition *move*: memcpy/merge-join over N present columns vs per-element ctor/dtor) — the
  remaining add/remove gap to flecs after W2. Separate work item / Phase C.
- Precomputing an edge `diff` (added/removed id list) on the edge (flecs #7) — a further micro-opt.
- The `EntityHash` manual H2-fixup cleanup (now redundant after W6) — leave in place.
- Reverse edge-index for O(1) invalidation — unnecessary given rare deletions.

---

## 8. Testing & validation
1. **W6 unit test** (`FlatMapInternalsTest`): `SplitHash` H2 distribution over aligned inputs; `FlatMap<void*,int>` aligned-key functional guard.
2. **W2 unit tests**: edge get/set round-trip on `Archetype` (lazy alloc, nullptr-until-set); invalidation/no-dangling on archetype deletion; add-then-remove recompute after invalidation.
3. **Existing suites green** in **Debug / Release / Dist** — especially `ArchetypeTest`, `ArchetypeManagerTest`, `RegistryTest`, `ViewInvalidationTest`, `RootArchetypeRoundTripTest`, all serialization tests (W6 order-change net), and any `ArchetypeGraph` test (adapted/removed). Watch the TypeID ceiling — reuse existing component types.
4. **Benchmark**: re-run `bench-compare/`, update `RESULTS.md`. Expect add ~150→~100-110, remove ~106→~70, no iteration regression; note the residual gap is W3's move.

## 9. Success criteria
- All three configs green; new W6 + W2 tests pass.
- `ArchetypeGraph` gone; the add/remove edge lookup is a single array index off `Archetype` (no hashing);
  `SplitHash` avalanches every `FlatMap` hash.
- No dangling-edge UAF on archetype deletion (test-proven).
- Measured add/remove improvement toward the ~100-110 / ~70 targets with no iteration regression;
  `RESULTS.md` updated. Public API + serialization format unchanged.

---

## Appendix — key source references
- `include/Astra/Container/FlatMap.hpp:918` (`SplitHash`); `include/Astra/Container/Swiss.hpp:44-58` (H1/H2).
- `include/Astra/Archetype/ArchetypeGraph.hpp` (to delete).
- `include/Astra/Archetype/Archetype.hpp` (edge arrays + accessors); `include/Astra/Component/Component.hpp:20` (`MAX_COMPONENTS`).
- `include/Astra/Archetype/ArchetypeManager.hpp:1020` (`GetArchetypeWithModified`), `:1084/:1092` (wrappers), `:730-731` (invalidation), `:613` (Clear), `:1517` (`m_edgeGraph`).
- Perf plan: `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` (W2, W6).
