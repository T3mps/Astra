# Phase C — Chunk Storage Modernization (W5 + W7 + W4 + W3) — Design

**Date:** 2026-07-22
**Branch:** `perf/phase-c-chunk-storage` (off dev `a8712d6`)
**Perf program:** bundles W5/W7/W4/W3 of `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` §3 Phase C.
**Predecessors (landed):** W1 (unified paged entity record), W6 (fmix64 hash mix), W2 (array-indexed archetype edges).

## Goal

Replace the per-chunk 128-slot `ComponentArrayInfo` array — which embeds a full ~176 B `ComponentDescriptor` **by value** in every slot (~25 KB per 16 KB chunk) plus a duplicate `m_componentDescriptors` vector — with **metadata stored once per archetype** and a **packed `[N]` present-columns array per chunk**. This kills ~23 KB/chunk, lets every hot loop iterate `0..N` instead of `0..128`, and enables a trivial-`memcpy` transition-move fast path.

## Why (measured gaps to flecs, post-W1+W6+W2)

| op | Astra now | flecs | gap | primary lever |
|----|-----------|-------|-----|---------------|
| add | 131.5 | 54.6 | **2.4×** | **W3** memcpy move |
| remove | 86.4 | 32.7 | **2.6×** | **W3** |
| create | ~129 | 105 | 1.2× | W4 fast-append (small headroom) |
| random_get | ~62 | ~52 | 1.2× | W7 compact get (small headroom) |
| iterate2 | 1.17 | 0.89 | 1.3× | W5 cache density + W7 |

**Honest scoping note:** W1 already pulled create and random_get to ~1.2× flecs, so W4 and W7 have **less headroom than the perf plan (written pre-W1) implies**. They are bundled here because they share the same layout change (touching the same hot chunk methods once rather than four times); their wins are to be **measured, not assumed**. The dominant remaining gap is add/remove → W3, which depends on the W5 layout.

## Global constraints

- Header-only C++20; exception-free (no `throw`); RTTI-off (no `<any>`/`typeid`/`dynamic_cast`). Match `ASTRA_*` / `noexcept` / `ASTRA_ASSERT` / `ASTRA_NODISCARD` style.
- **No on-disk serialization format change.** Chunk metadata is purely runtime; the format iterates the archetype's `m_componentDescriptors` and stores data by ComponentID. The full serialization suite is the regression net.
- **No public `Registry` API change.**
- `MAX_COMPONENTS` (128) is the `ComponentID` ceiling.
- `ArchetypeManager` must never write `EntityRecord::version` (unchanged from W1).
- **Trivial fast paths must never apply to non-trivially-relocatable components.** The `isComplex` / per-column `is_trivially_copyable` gate is a correctness boundary, not just an optimization — a memcpy of a move-only or self-referential type is a bug.
- TypeID ceiling: tests reuse `tests/TestComponents.hpp` (`Astra::Test::Position/Velocity/Health`) + existing move-only test types (`Tracked`); register no new component types. `Archetype`/chunk unit tests take a bare `ComponentMask` + registered descriptors for existing types.
- 3-config green (MSVC Debug, Release, Dist); whole-solution build (`-t:AstraTest` does not work).

## Architecture (Option A — hybrid: per-archetype meta + packed chunk columns)

### Per-archetype shared metadata — built once, address-stable

Owned by `Archetype`, constructed in `Archetype::Initialize()` immediately after `m_componentDescriptors` (which is built once and never mutated, so its element addresses are stable). Shared by every chunk of that archetype via a `const` pointer.

```
struct ArchetypeColumnMeta
{
    uint16                     columnCount;                 // N = number of STORAGE-BEARING components
    const ComponentDescriptor* descriptors;                 // [N] -> into m_componentDescriptors (canonical, shared)
    ComponentID                columnIds[N];                // sorted ascending (merge-join + stable iterate order)
    uint32                     strides[N];                  // per-column element size (== descriptor.size)
    int16                      idToColumn[MAX_COMPONENTS];  // ComponentID -> column index, or -1 (absent OR tag)
    bool                       isComplex;                   // true if ANY column is not trivially relocatable
};
```

(Exact storage form — inline arrays vs `unique_ptr`/`SmallVector` — is an implementation choice for the plan; the contract is: one instance per archetype, stable for the archetype's lifetime, `descriptors` points into the archetype's own canonical descriptor list.)

The heavy `ComponentDescriptor` now lives **once per archetype**, not once per chunk slot.

### Per-chunk storage — packed columns + a back-pointer

Replaces `std::array<ComponentArrayInfo, MAX_COMPONENTS> m_componentArrays` **and** the duplicate `std::vector<ComponentDescriptor> m_componentDescriptors`.

```
const ArchetypeColumnMeta* m_meta;         // shared, set at chunk creation
struct Column { void* base; uint32 stride; };
Column m_columns[N];                        // packed; base varies per chunk; stride copied from meta
```

- `stride` is duplicated into the per-chunk `Column` deliberately: it keeps `base` + `stride` cache-local on the iterate hot path, avoiding an archetype-meta dereference per element. (Design call (a), approved.)
- Chunk metadata drops from ~25 KB to `N×16 B + 8 B` (e.g. ~56 B for a 3-component archetype).

### Random access — `Get(id)` (W7)

```
int col = m_meta->idToColumn[id];
if (col < 0) return nullptr;                 // absent, or a tag (no storage)
return static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;
```

O(1) via a 256 B hot map, replacing the walk over the 176 B `ComponentArrayInfo`.

### Tags / empty (zero-size) components

Excluded from data columns (`columnCount` counts storage-bearing components only); `idToColumn[tagId] = -1`. Presence stays carried by the `ComponentMask`. `Get<Tag>` short-circuits at compile time (`std::is_empty_v<T>`) returning the static empty instance, exactly as today — so mapping a tag to `-1` (storage-wise identical to absent) is correct: neither has storage, and presence is the mask's job. This **removes the `base==nullptr` skips from every hot loop**. (Design call (b), approved.)

Runtime-id paths (`GetComponentPointer`/`GetComponentArrayByID`, used by serialization/reflection) return `nullptr` for a tag id — correct, there is no data; the mask distinguishes present-tag from absent where that distinction is needed.

## The four work items on this layout

- **W5 — Metadata once + packed columns.** The structures above. Rewire every hot loop to iterate `0..N` over `m_columns` + `m_meta->descriptors[c]`. Delete `ComponentArrayInfo[128]` and the per-chunk `m_componentDescriptors` copy.
- **W7 — Compact `idToColumn` get.** `Get` resolves id → column via the shared `int16 idToColumn[128]` + base/stride arithmetic (above).
- **W4 — Fast-append.** In `AddEntity` / `AddEntityWithComponents`, iterate the `N` present columns; skip `DefaultConstruct` for components the caller emplaces; for trivially-constructible types, no per-element fn-ptr default-construct (the value overwrite suffices; the chunk memory is already zeroed at creation).
- **W3 — Trivial memcpy move.** Cross-archetype move (add/remove component) does a **merge-join** over the source and destination archetypes' sorted `columnIds` (no `0..128` scan, no per-id `isValid` test). For matched columns whose component is trivially copyable, `memcpy`; a whole-archetype fast path is gated on both archetypes' `isComplex == false`. Non-trivial components keep the per-element `MoveConstruct`/`Destruct` path. Contiguous-run `memcpy` (already present in `BatchMoveComponentsFrom`) is preserved.

## Data flow / touch points

- `Archetype::Initialize()` (`Archetype.hpp:68`) — build `ArchetypeColumnMeta` after `m_componentDescriptors`.
- `ArchetypeChunkPool::CreateChunk` — signature changes from `const std::vector<ComponentDescriptor>&` to `const ArchetypeColumnMeta*` (all call sites in `Archetype.hpp`: 128, 179, 248, 852, 1149, 1342, and the deserialize path at 852).
- `Chunk` ctor + `InitializeComponentArrays` — build `m_columns[N]` (offset walk for bases; strides from meta); store `m_meta`.
- Rewritten to `0..N` + `m_meta`: `~Chunk`, `AddEntity`, `AddEntityWithComponents`, `BatchAddEntities`, `RemoveEntity`, `BatchMoveComponentsFrom`, `GetComponent`/`GetComponentArray`/`GetComponentPointer`/`GetComponentArrayByID`, the `Chunk` move ctor, and `Archetype::MoveEntitiesBetweenChunks` (`Archetype.hpp:1382`) + the cross-archetype move helper (`Archetype.hpp:427`).
- `Archetype::Serialize`/`Deserialize` (`Archetype.hpp:618`/`688`) — mechanical `CreateChunk` signature update only; the format and the descriptor iteration are unchanged.

## Error handling / invariants

- `idToColumn` lookups are bounds-checked in debug (`ASTRA_ASSERT(id < MAX_COMPONENTS)`); a `-1` column is the sole "no storage" signal.
- `isComplex` and per-column `is_trivially_copyable` are the correctness gate for every memcpy path — a component that is not trivially relocatable MUST take the `MoveConstruct`/`Destruct` path.
- `ArchetypeColumnMeta` lifetime ≥ every chunk that references it (guaranteed: the archetype owns both and outlives its chunks).
- Chunk memory is zeroed at creation (unchanged), which W4 relies on for trivial types.

## Testing strategy

- **Meta construction unit tests:** `columnIds` sorted ascending; `idToColumn` correct (present-storage → column idx, tag → -1, absent → -1); `columnCount` excludes tags; `isComplex` true iff any column non-trivial.
- **Packed-column access:** `Get<T>` by id round-trips to the right column/offset; a tag-bearing archetype gets/iterates correctly; runtime-id `GetComponentPointer` returns nullptr for tag/absent.
- **Correctness-boundary guards (the load-bearing tests):** a move-only / non-trivially-relocatable component (`Tracked`) survives create, add, remove, and cross-archetype transition — the `isComplex` path must NOT memcpy it (assert move/destruct counts or sentinel integrity); a mixed trivial+complex archetype moves correctly.
- **Regression net (3-config):** full suite, especially `RegistrySerializationTest`, `FormatV2Test`, `LoadRobustnessTest`, `RootArchetypeRoundTripTest`, iteration tests, and existing move-semantics tests.
- **Benchmark — the acceptance signal:** rebuild `bench_astra`, run head-to-head vs EnTT/flecs, update `RESULTS.md`. Expect add/remove to move materially (W3); create/iterate2/random_get measured honestly (may be modest given post-W1 headroom). Same-session A/B methodology as W6+W2 if the machine is noisy.

## Scope / YAGNI boundaries

- No on-disk format change; no public API change.
- Keep the existing per-element complex-type move/construct path — W3/W4 only *add* trivial fast paths beside it.
- No SIMD/vectorization beyond `memcpy`.
- `int16 idToColumn` is the extent of W7; no further get-path gold-plating (random_get is already ~1.2× flecs — we measure whether even this helps).
- No change to chunk sizing, the pool/free-list, huge-page logic, or the archetype-edge cache (W2).

## Likely task sequencing (for the implementation plan)

1. `ArchetypeColumnMeta` built in `Archetype` (additive, not yet consumed) + meta unit tests.
2. Convert `Chunk` to `m_meta` + packed `m_columns` (W5): rewire every accessor/hot loop, delete `ComponentArrayInfo[128]` + duplicate vector, update `CreateChunk` signature + all call sites. Full suite green.
3. `Get` via `idToColumn` (W7).
4. Fast-append (W4).
5. Trivial memcpy move + merge-join + `isComplex` gate (W3) — the correctness-critical task (opus); move-only guard tests.
6. Benchmark + `RESULTS.md` + perf-plan update; controller updates the perf memory.

Steps 2 and 5 are the invasive/risky ones (opus implementer + opus review); 1/3/4/6 are lighter. 3-config verification after step 2 and step 5 at minimum.
