# Phase 2 — Dynamic Chunk Sizing (TLSF + Grow-as-Populate + Compaction)

**Date:** 2026-07-23
**Status:** Design approved (brainstorm), pending spec review → writing-plans
**Branch base:** `dev @ cb68bf3` (clean, 202 ahead of origin/dev, unpushed)
**Author:** brainstorm session (superpowers:brainstorming)

---

## 1. Context & Goal

Astra's north star: out-perform flecs **and** EnTT while keeping Bevy-class ergonomics. The
performance program (see `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` and the
`astra-perf-optimization` memory) has landed W1, W6, W2, and Phase C. Current standing vs
flecs (dev `cb68bf3`, median-of-5, settled machine):

| op | Astra | flecs | EnTT | note |
|---|---|---|---|---|
| create | **51.4** | 95.9 | 40.6 | Astra ~1.87× ahead of flecs |
| add | 57.1 | 54.4 | 12.1 | ~parity flecs |
| remove | 40.6 | 34.1 | 16.1 | 1.19× behind flecs |
| random_get | 65.4 | 57.0 | 25.3 | 1.15× behind flecs |
| iterate1 | 0.509 | 0.390 | 0.492 | 1.31× behind flecs |
| iterate2 | 0.993 | 0.812 | 1.04 (group) | 1.22× behind flecs |
| iterate3 | 1.050 | 0.889 | 3.21 (view) | 1.18× behind flecs |

The remaining flecs gaps (iterate, random_get, remove) trace substantially to Astra's
**fixed 16KB chunk size**: at 1M entities the hot archetype spans ~1470 chunks, so iteration
crosses a chunk boundary every ~680 entities (prefetcher/TLB restart + per-chunk column/tuple
setup), versus flecs's large contiguous per-table arrays.

**Phase-1 study (done, `bench-compare/sweep_analysis.md`)** swept `chunkSize × N × perEntitySize`
and fitted the optimal chunk size to the archetype's total data footprint:

> **`chunk_bytes ≈ clamp( N · perEntitySize / 2 , 4KB, 512KB )`**  (K = 2; cap 512KB, not 1MB —
> same speed, half the worst-case memory)

Improvement over fixed-16KB at N=1M (pe=64): **iterate1 +51%, random_get +16%, iterate2 +15%,
iterate3 +7%, create +3%**; add/remove ~flat. The study concluded adaptive sizing puts Astra
**ahead-of/at flecs** on create + random_get + iterate1/2, ~parity iterate3, with only remove
behind. This spec designs the mechanism to realize that.

**Key obstacle the formula creates:** it needs `N` (final population), which is unknown when an
archetype is created empty. The answer is **grow-as-populate**: size each new chunk from how big
the archetype already is, so chunks ramp geometrically from small to the cap as the archetype
fills. Variable-size chunks require a variable-size allocator (TLSF) and a small addressing rework.

---

## 2. Design Decisions (all user-approved)

1. **Scope = all-in-one:** TLSF allocator **+** grow-as-populate sizing **+** variable-capacity
   addressing **+** compaction-on-defrag, in one branch. (The pure speed win needs only the first
   three; TLSF's coalescing + compaction is the memory-reclamation half, included here.)
2. **Allocator = TLSF (Two-Level Segregated Fit), not buddy.** O(1) alloc/free on a cold path;
   boundary-tag coalescing merges *any* adjacent free blocks (buddy only merges its specific
   buddy) → better for the compaction/coalescing goal; arbitrary sizes → exact chunk sizing.
3. **TLSF sourcing = modern-C++20 reimplementation guided by the proven source.** Use Matt
   Conte's public-domain `tlsf.c` (https://github.com/mattconte/tlsf) as the correctness
   reference for structure and invariants, but rewrite it as a header-only, exception-free,
   RTTI-free C++20 module in Astra's idioms (`Mosaic::Bits`, `ASTRA_ASSERT`/`ASTRA_LOG_*`,
   `std::span`, `constexpr`). Not a mechanical port; not clean-room. Preserves Astra's
   header-only property (a stated competitive selling point).
4. **Grow-as-populate:** `nextChunkBytes = clamp(currentArchetypeBytes / GROW_DIVISOR, 4KB, 512KB)`,
   `GROW_DIVISOR = 2` (matches the study's `/2`), re-swept to confirm.
5. **Compaction-on-defrag:** per-archetype fill-ratio trigger (`< COMPACT_THRESHOLD`, start 0.5)
   inside the existing `Defragment` pass; repack live entities into fresh target-size chunks; free
   the old ones → TLSF coalesces → release fully-free arenas to the OS.
6. **Objective:** iteration-weighted with create + random_get co-primary; optimize speed subject to
   a memory-overhead ceiling (no over-allocation for small archetypes).
7. **Drop pow2 capacity rounding.** Today `m_entitiesPerChunk = std::bit_floor(maxEntities)` rounds
   capacity down to a power of two (wastes up to ~50% of a chunk). Once addressing no longer needs
   pow2 shift/mask, use the exact fitted capacity — a packing win unlocked for free.

---

## 3. Findings that shape the design (verified in source)

- **The addressing landmine is contained.** `EntityLocation` (`include/Astra/Archetype/EntityLocation.hpp`)
  already stores `(chunkIndex, entityIndex)` directly; **nothing in the codebase decodes a flat
  global index**. `m_entitiesPerChunk` + `m_entitiesPerChunkShift`/`Mask` are used *only* for
  capacity arithmetic. Therefore **`EntityLocation`, `EntityRecord`, and the on-disk serialization
  format do not change.** The `Chunk` already stores its own `m_capacity`.
- **The pool is a single shared instance.** `ArchetypeManager` owns one `ArchetypeChunkPool m_chunkPool`
  (`ArchetypeManager.hpp:1523`); every `Archetype` holds a raw `ArchetypeChunkPool*`. So TLSF is one
  shared allocator over huge-page arenas serving all archetypes → coalescing is cross-archetype.
- **Arena primitives already exist.** The pool allocates huge-page-backed regions via
  `AllocateMemory(bytes, 64 /*CACHE_LINE*/, HugePages|ZeroMem) -> AllocResult{ptr,size,usedHugePages}`
  and frees via `FreeMemory(ptr, size, usedHugePages)` (`ArchetypeChunkPool::AllocateBlock`). TLSF
  reuses these to obtain/release arenas; it replaces the block/free-list/`m_memoryToNode` machinery
  that carves them.
- **Phase-C move machinery is reusable for compaction.** `MoveEntitiesBetweenChunks` /
  `MoveEntityFrom` already do trivial-`memcpy` (gated on `is_trivially_copyable`) / per-element move
  with move-only correctness guards.

---

## 4. Architecture — four units

Ordered by dependency. Each is independently testable before the next.

### Unit A — TLSF allocator (`include/Astra/Core/Tlsf.hpp`, new)

Header-only, exception-free, RTTI-free C++20. One `Tlsf` instance managed by the pool. Knows
nothing about chunks/archetypes/components — a byte allocator with a small surface.

**Constants (adapted from the reference for our case):**

| constant | stock tlsf.c (64-bit) | Astra Tlsf | rationale |
|---|---|---|---|
| `ALIGN_SIZE_LOG2` | 3 (8B) | **6 (64B)** | chunk bases must be cache-line aligned — achieved via a **size-congruence rule**, not the reference's `memalign` gap-trim path (see below) |
| `SL_INDEX_COUNT_LOG2` | 5 | 5 (32 sub-lists) | keep proven granularity |
| `FL_INDEX_SHIFT` | 8 | `5 + 6 = 11` | derived; `SMALL_BLOCK_SIZE = 1<<11 = 2048`. The small-block mapping branch **stays** (split remainders smaller than 2KB are real free blocks) |
| `FL_INDEX_MAX` | 32 (4GB) | 26 (64MB max arena) | shrinks the control tables to a few KB |
| `FL_INDEX_COUNT` | 25 | `FL_INDEX_MAX - FL_INDEX_SHIFT + 1` = 16 | `blocks[FL][32]` pointer table |

**64B-payload alignment via size congruence** (the one deliberate deviation from the reference,
replacing its `memalign` gap-trim path): with the boundary-tag layout, consecutive payloads obey
`payload_{n+1} = payload_n + size_n + overhead(8)`. Keeping **every block size ≡ 56 (mod 64)**
— request rounding `adjusted = align_up(bytes + 8, 64) - 8`, minimum block 56 — and placing each
arena's first payload on a 64B boundary makes every payload permanently 64B-aligned. The rule is
closed under split (`remainder = size - request - 8 ≡ 56`) and merge (`size + other + 8 ≡ 56`),
and is enforced by the heap-walk `Validate()` invariant check in the unit suite.

**Ported faithfully** (reference correctness inherited): `mapping_insert` / `mapping_search`
(fli/sli from size, with the round-up in search), the `block_header_t` boundary-tag layout
(`prev_phys_block`, `size` with free / prev-free bits packed in the low bits, `next_free`/`prev_free`;
`block_header_overhead = sizeof(size_t)`, payload overlaps the next header's `prev_phys_block`
slot), `block_split`, `block_merge_prev`/`block_merge_next`/`block_absorb`, `control_t`
(`fl_bitmap`, `sl_bitmap[FL]`, `blocks[FL][SL]`). Bit-scans via `Mosaic::Bits` (fls/ffs).

**Interface (allocator concerns only):**
```cpp
void* Allocate(size_t bytes);              // 64B-aligned payload, nullptr on OOM (never throws)
void  Free(void* ptr);                     // O(1); coalesces phys-adjacent free neighbors
void  AddArena(void* base, size_t size);   // register a huge-page region as one free block + sentinel
// arena lifecycle for defrag/reclamation:
template<class F> void ForEachFullyFreeArena(F&& cb) const;  // arenas whose entire span is one free block
void  RemoveArena(void* base);             // detach a fully-free arena so caller can FreeMemory it
size_t SuggestArenaBytes(size_t request) const;  // huge-page multiple ≥ request + pool overhead
```

**Alignment contract:** `Allocate` returns 64B-aligned payloads by construction (TLSF positions
headers so payloads land on `ALIGN_SIZE` boundaries; the boundary-tag trick keeps overhead at one
`size_t`). Arenas are huge-page-backed; the huge-page/TLB benefit comes from the arena backing, not
per-chunk alignment (unchanged from today's contract). **Invariant test:** every `Allocate` result
is 64B-aligned and lies within a registered arena.

**Concurrency:** matches the *existing* pool contract — structural mutation is single-threaded
(the manager mutates archetypes on one thread; parallelism lives in iteration). No new locking is
introduced. (Verify against the scheduler's access model during planning.)

**Arena sizing/growth:** when `Allocate` can't be satisfied, the pool requests a new arena of
`SuggestArenaBytes(request)` (a config'd base, e.g. 2–4MB, rounded to a huge-page multiple, ≥
request + overhead) via `AllocateMemory` and calls `AddArena`. Fully-free arenas are returned to
the OS during defrag.

**Unit tests (independent of the ECS):** alloc/free round-trips; split leaves a valid remainder;
free coalesces prev/next/both; alignment invariant; exact-size and worst-case-fragmentation
patterns; OOM returns null (no throw); multi-arena; fully-free-arena detection + removal.

### Unit B — Pool rewire to TLSF (`ArchetypeChunkPool`, behavior-preserving first)

Replace `BlockInfo`/`ChunkNode`/`m_freeList`/`m_memoryToNode`/`AllocateBlock`/`AcquireMemory`/
`ReturnChunk`/block-`Defragment` with a `Tlsf m_tlsf` + a list of owned arenas.

- `CreateChunk(size_t chunkBytes, const ArchetypeColumnMeta* meta)` — **new signature** (was
  `CreateChunk(entitiesPerChunk, meta)` using `m_config.chunkSize`). `chunkBytes` comes from the
  archetype's grow-as-populate policy (Unit C); the `Chunk` derives its capacity from `chunkBytes`
  + `meta` (exact `InitializeColumns` layout). Memory = `m_tlsf.Allocate(chunkBytes)`; on null,
  request a new arena and retry once.
- `ReturnChunk(void* memory)` → `m_tlsf.Free(memory)`.
- `Defragment()` → iterate fully-free arenas, `RemoveArena` + `FreeMemory` each (keep ≥1 reserve,
  matching today's "keep one block" heuristic). Archetype-level entity compaction is Unit D.
- **Rewire step lands first with a fixed chunk size** (pass a constant `chunkBytes` from the
  archetype so behavior == today), proving TLSF parity under the full existing suite **before**
  variable sizing turns on. `Config.chunkSize` is retained as the fixed value / cap default.

### Unit C — Variable-capacity addressing + grow-as-populate (`Archetype`)

**Addressing swap** (uniform → per-chunk); `EntityLocation`/`EntityRecord`/serialization unchanged:

| today (uniform) | becomes (per-chunk) |
|---|---|
| `available = m_entitiesPerChunk - chunk->GetCount()` | `chunk->GetCapacity() - chunk->GetCount()` |
| `newChunksNeeded = (need + epc - 1) >> shift` | iterative grow loop (add one grown chunk at a time until `need` satisfied) |
| `currentCapacity = m_chunks.size() * m_entitiesPerChunk` | running `m_totalCapacity` (or `Σ chunk->GetCapacity()`) |
| `utilization = count / m_entitiesPerChunk` | `count / chunk->GetCapacity()` |
| members `m_entitiesPerChunk`, `m_entitiesPerChunkShift`, `m_entitiesPerChunkMask` | **removed** |

**Touch sites** (`Archetype.hpp`): ~150–154 (Initialize), 184–251 (`AddEntities`), 253–299
(`AddEntitiesWith`), 605–642 (Reserve/utilization), 985–1057 (defrag helpers), 1176–1218,
1360–1389 (`GetOrCreateChunk`), 656 + 730–857 (serialization write/read of `m_entitiesPerChunk` —
becomes per-chunk or recomputed on load; **on-disk bytes unchanged**, since layout is id-keyed and
capacity is derivable). Drop `std::bit_floor` → exact capacity.

**Grow-as-populate policy** (`Archetype::NextChunkBytes()`):
```
currentArchetypeBytes = m_totalCapacity * perEntitySize        // data footprint of existing chunks
nextChunkBytes        = clamp(currentArchetypeBytes / GROW_DIVISOR, MIN_CHUNK=4KB, MAX_CHUNK=512KB)
```
First chunk → 0/2 → clamps to 4KB. Geometric ramp (~1.5× total per chunk at divisor 2) toward the
512KB cap. `GROW_DIVISOR` is a named constant, default 2, confirmed by the validation re-sweep.
`perEntitySize` = the archetype's summed column strides (with cache-line padding), already known
from `ArchetypeColumnMeta`.

### Unit D — Compaction-on-defrag (`ArchetypeManager` + `Archetype`)

- **Trigger:** during the periodic/on-demand `Defragment`, flag any archetype with
  `liveEntities / totalCapacity < COMPACT_THRESHOLD` (start 0.5) **and** ≥2 chunks.
- **Action:** at compaction time `N` (= live count) is *known*, so apply the study formula
  directly rather than the grow-as-populate approximation:
  `targetBytes = clamp(liveBytes / 2, 4KB, 512KB)` where `liveBytes = liveEntities · perEntitySize`;
  allocate `⌈liveBytes / targetBytes⌉` fresh target-size chunks; repack live entities via the existing
  `MoveEntitiesBetweenChunks` (trivial-`memcpy` / move, move-only-safe); update each moved entity's
  `EntityRecord.location`; free the old chunks. Then Unit B's arena reclamation runs.
- **Invalidation:** reuse exactly what `Defragment` already invalidates — view caches + archetype
  edges + per-entity records. No new invalidation surface. **Verify the edge/UAF ordering** against
  the W2 defrag-invalidation code (`ArchetypeManager.hpp:733-746`, `ClearEdgesTo` before
  `unique_ptr.reset()`), since this is a known-sharp area.
- **Risk posture:** highest-risk unit (bulk location mutation + move-only lifetimes). Opus + a
  deterministic move-only guard (`Astra::Test::Tracked::s_live`) with RED→GREEN tests, mirroring
  Phase C.

---

## 5. Build order

Each step is green (3-config) before the next:

1. **Unit A — `Tlsf`** in isolation with its full unit suite.
2. **Unit B — pool rewire to TLSF, fixed chunk size** (behavior-preserving; prove parity under the
   existing suite + a 3-way bench sanity check that nothing regressed).
3. **Unit C — variable-capacity addressing + drop-pow2 + grow-as-populate** (turn on dynamic sizing).
4. **Unit D — compaction-on-defrag.**
5. **Validation** — re-sweep (lock `GROW_DIVISOR`), 3-way bench, memory-waste check.

---

## 6. Validation & Done Criteria

Done = **all** of:

1. **3-config test suite green** (Debug/Release/Dist). Baseline dev `cb68bf3` = 697/695/695.
2. **`Tlsf` unit suite** green (alloc/free/coalesce/alignment/OOM/arena-release/fragmentation).
3. **Re-run `bench_sweep`** (`bench-compare/bench_sweep.cpp` → `sweep_results.csv`) — confirm the
   fitted formula holds under real variable-capacity chunks; lock `GROW_DIVISOR` (2 vs 1 vs 4).
4. **3-way bench** (`bench_astra`/`bench_entt`/`bench_flecs`) — confirm the predicted end-state:
   **ahead-of/at flecs** on create + random_get + iterate1/2, **~parity** iterate3, add/remove no
   regression. Rebuild `bench_astra` against the branch head.
5. **Memory-waste check** — small archetypes (N≈10–1000) stay near-minimal footprint (grow-as-
   populate's whole point); assert via pool stats.

Reproduce/build recipes: `bench-compare/` (`build_one.bat`, `bench_common.hpp`); Astra build needs
`/I..\include /I..\vendor\Mosaic\include` + `advapi32.lib`.

---

## 7. Risks, Confounds, Out-of-Scope

**Risks & mitigations:**
- *New allocator in the hot core* → reference-guided (proven algorithm), isolated with its own unit
  suite, landed behind a behavior-preserving fixed-size rewire before dynamic sizing; opus on Units
  A/B/D.
- *Compaction location mutation / move-only lifetimes* → reuse Phase-C move machinery + `s_live`
  deterministic guard + RED→GREEN tests; opus; verify defrag edge/UAF ordering.
- *64B `ALIGN_SIZE` adaptation* → alignment-invariant test on every `Allocate`.
- *Serialization* → on-disk format is id-keyed and layout-order-neutral; `m_entitiesPerChunk` write
  becomes per-chunk/derived. Round-trip tests (incl. the legacy v2 path) must stay green.

**Confound (from the study, noted not fixed):** Phase-C's fixed `Column[MAX_COMPONENTS]` (~2KB/chunk)
inflates per-chunk overhead and biases the sizing optimum bigger. Shrinking it to a packed `[N]`
heap array is a *complementary* deferred lever that would shift the formula constants — out of scope
here.

**Out of scope / deferred:** packed-`[N]` chunk `Column` metadata; ASan CI lane (durable UAF/
move-only net, tracked separately); `FlatSet.hpp:819` unmixed `SplitHash`; O(n²) defrag
edge-invalidation flatten; consuming `ArchetypeColumnMeta::isComplex` in a whole-archetype fast path.

**Tuning knobs exposed as named constants (not magic numbers):** `GROW_DIVISOR` (2),
`COMPACT_THRESHOLD` (0.5), `MIN_CHUNK` (4KB), `MAX_CHUNK` (512KB), TLSF `FL_INDEX_MAX` /
`SL_INDEX_COUNT_LOG2`, arena base size.

---

## Results (2026-07-23, Task 7 validation, branch `perf/phase-2-dynamic-chunk-sizing` @ `ae40c9b`)

Full data, tables, and the machine-state caveat are in `bench-compare/RESULTS.md` ("Phase 2 — dynamic
chunk sizing"). Summary against this spec's predictions:

- **3-config suite:** Debug 722/722, Release 720 (1 pre-existing `CompressionTest` timing flake,
  confirmed via isolated reruns), Dist 720 (same flake, same confirmation). No regressions.
- **Memory-waste goal (§2.7, §6.5): CONFIRMED directly.** 100 `Position` entities occupy exactly one
  4096-byte chunk (`ArchetypeTest.SmallArchetypeStaysAtMinimumChunk`, corroborated by a
  `GetArchetypeMemoryUsage` scratch probe) vs the pre-Phase-2 fixed 16KB first chunk — a 4× reduction,
  exactly the grow-as-populate goal.
- **create/add/remove (§6.4): met or exceeded, robustly.** create is decisively ahead of flecs (7/7
  interleaved rounds, non-overlapping spreads); add beats flecs 7/7 rounds (no regression, better than
  the "no regression" bar); remove shows no regression (its ~1.2–1.5× gap to flecs matches the
  already-known post-Phase-C state, not something Phase 2 made worse).
- **iterate1/iterate2/random_get vs flecs (§6.4, the study's headline prediction of +51%/+15%/+16%):
  NOT confirmed by this session's data** — the benchmark ran under sustained ~70–84% background CPU
  load from unrelated foreground applications (not started or stopped by this task). Under that load,
  iterate1/iterate2 paired win-rates against flecs were near coin-flips (4/7 rounds) and random_get
  leaned the other way (flecs faster 6/7 rounds), with fully-overlapping [min,max] spreads across all
  three. This is reported as **inconclusive, not as a negative result** — the noise floor was too high
  this session to see the predicted win (or rule it out). A clean-machine re-run is the recommended next
  step before treating this part of the study's prediction as settled.
- **Grow-divisor/max-chunk-bytes re-sweep (§6.3): inconclusive under the same noise**, no cell beat the
  shipped `growDivisor=2, maxChunkBytes=512KB` NSDMIs consistently and meaningfully (>5%) across the
  swept N values, so they were left unchanged — but the noise level means this isn't strong positive
  confirmation either, just an absence of contrary evidence.

| Operation | Astra (this session, median of 7) | flecs (this session, median of 7) | Historical Astra (clean machine) | Historical flecs (clean machine) |
|---|---|---|---|---|
| create | 73.76 | 162.63 | 51.4 | 95.9 |
| add | 67.73 | 94.73 | 57.1 | 54.4 |
| remove | 59.57 | 47.74 | 40.6 | 34.1 |
| random_get | 194.79 | 174.28 | 65.4 | 57.0 |
| iterate1 | 1.193 | 1.651 | 0.509 | 0.390 |
| iterate2 | 2.474 | 2.793 | 0.993 | 0.812 |
| iterate3 | 2.651 | 3.355 | 1.050 | 0.889 |

**Net assessment:** the mechanism (TLSF + grow-as-populate + per-chunk addressing + compaction) is
implemented, tested, and demonstrably delivers its memory-waste and structural-op goals under real
measurement. The iteration/random_get speed goals — the study's original headline motivation — remain
plausible but unproven pending a quiet-machine re-run; nothing in this session's data suggests they
regressed, only that this session couldn't isolate the signal from the noise.
