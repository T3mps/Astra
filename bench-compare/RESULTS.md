# Astra vs EnTT vs flecs — same-machine head-to-head (2026-07-21, updated 2026-07-23 post-Phase-C)

**Setup:** one machine, identical hand-rolled harness (`bench_common.hpp`), identical component layout, 1,000,000 entities, MSVC `/std:c++20 /O2 /DNDEBUG /EHsc`, view/query/group created once (pure-iteration), median of runs. EnTT master, flecs v4.1.6, Astra @ `perf/phase-c-chunk-storage` (post-W5+W7+W4+W3 chunk storage modernization — see the Phase C section below). Each library uses its idiomatic fast path. Numbers are **ns per entity/op** (lower = better); M/s in parens.

| Operation | Astra | EnTT | flecs | Rank |
|---|---|---|---|---|
| iterate 1 comp | 0.451 (2218) | 0.512 (1955) | **0.389 (2574)** | flecs > **Astra** > EnTT |
| iterate 2 comp | 1.069 (936) | 2.303 view / 1.046 group | **0.773 (1294)** | flecs > EnTT-group ≈ **Astra** > EnTT-view |
| iterate 3 comp | 1.176 (850) | 3.249 view (308) | **0.980 (1020)** | flecs > **Astra** > EnTT-view |
| create (2 comp) | **49.2 (20.3)** | 38.2 (26.2) | 94.6 (10.6) | EnTT > **Astra** > flecs |
| add component | 55.6 (18.0) | **11.3 (88.2)** | 53.6 (18.6) | EnTT > **Astra** ≈ flecs |
| remove component | 40.8 (24.5) | **16.0 (62.4)** | 32.8 (30.5) | EnTT > flecs > **Astra** |
| random get | 58.4 (17.1) | **19.0 (52.6)** | 54.2 (18.5) | EnTT > flecs > **Astra** |

Astra numbers are the median of 8 interleaved same-session runs against a pre-Phase-C worktree build (see
"Phase C" below for the full A/B methodology). EnTT/flecs are the median of 4 fresh same-session runs
(unmodified code — re-measured, not reused from 2026-07-22, to keep the table internally consistent; this
machine's absolute numbers drift a few percent session-to-session from background load, same caveat as the
W6+W2 session). **create and add both changed rank this update** — Astra now beats flecs on create and is
within measurement noise of flecs on add (see "Residual gap to flecs" under Phase C for the honest
before/after and the caveat that create/add rank swaps rest on cross-library comparisons across sessions,
not the tightly-controlled pre/post-C A/B, which only compares Astra against itself).

## W1 (unified paged entity record) — landed 2026-07-22

W1 replaced `ArchetypeManager`'s `std::unordered_map<Entity,EntityRecord>` with a paged, id-indexed
`EntityRecord{archetype, location, version}` table shared with the liveness-version storage (`Entity/EntityTable.hpp`).
Validate (version check) and locate (archetype+location) now read the **same cache line** with **no hashing**
and **no per-create heap allocation**. Public `Registry` API is unchanged.

| Operation | Before (d4f8042) | After (W1) | Delta |
|---|---|---|---|
| create (2 comp) | 366.8 ns | 129.0 ns | **2.84× faster** |
| add component | 278.6 ns | 150.5 ns | **1.85× faster** |
| remove component | 220.4 ns | 105.6 ns | **2.09× faster** |
| random get | 147.0 ns | 62.2 ns | **2.36× faster** (beat the ~80 ns estimate) |
| iterate 1/2/3 comp | 0.456 / 1.538 / 1.457 ns | 0.436 / 1.173 / 1.142 ns | flat, within run-to-run noise (record table is off the iteration hot path, as expected — no regression) |

`GetComponent`/`HasComponent` were verified to route through the new `m_records->GetRecord(id)` path
(`ArchetypeManager.hpp:512-529`) — both the version check and the location read come off the single fetched
`EntityRecord`, confirming the win is real (not a leftover/mixed path).

## W6+W2 (avalanche hash mix + array-indexed archetype edges) — landed 2026-07-22

W6 replaced `SplitHash`'s raw `std::hash` forwarding with an `fmix64`-style avalanche mix before splitting
H1/H2, so pointer-keyed (and other low-entropy) `FlatMap` keys no longer degenerate the SIMD H2 filter
(H2 was constant for pointer keys pre-mix). W2 replaced the two-level `FlatMap<Archetype*, FlatMap<ComponentID,
Archetype*>>` in `ArchetypeGraph` with a lazily-allocated, per-archetype `Archetype* addEdges[MAX_COMPONENTS]`
/ `removeEdges[MAX_COMPONENTS]` array embedded directly on `Archetype` (`Archetype.hpp:1061-1087`); warm add/remove
is now a direct array index (`from->GetAddEdge(id)` / `from->GetRemoveEdge(id)`), not a hashed double-probe.
`ArchetypeGraph.hpp` was deleted. Verified `GetArchetypeWithAdded`/`GetArchetypeWithRemoved`
(`ArchetypeManager.hpp:1092-1106`) route through `GetAddEdge`/`SetAddEdge`/`GetRemoveEdge`/`SetRemoveEdge` —
not a leftover path.

### Measurement methodology

The machine was materially noisier during this benchmarking session than the 2026-07-21/W1 session — a
sanity check running the **unmodified** `bench_entt`/`bench_flecs` binaries showed their numbers drifting
20-100% from the recorded baseline run-to-run (e.g. EnTT random_get swung 18.6 → 36-37 ns on early runs before
settling back to ~22 ns), which is impossible if their code hadn't changed. So absolute ns numbers are **not**
directly comparable run-to-run against the 2026-07-21 baseline on this session alone.

To get an honest, noise-controlled delta, a second `bench_astra.exe` was built from the pre-W6+W2 commit
(`13ee47b`, the last commit before W6/W2) using the identical harness and flags, and the two binaries were
run **interleaved, back-to-back**, 10 rounds total. The first 4 rounds landed during a noisy window (machine
settling); the last 6 rounds were internally consistent and closely reproduced the original W1-session
absolute numbers (Astra-old create/add/remove/random_get ≈ 130/152/106/65 ns vs the recorded W1 baseline of
129/150.5/105.6/62.2 ns), confirming those 6 are a valid apples-to-apples comparison. The table below uses
the median of that settled 6-round window for both binaries.

| Operation | Before (pre-W2, `13ee47b`, this session) | After (W6+W2) | Delta |
|---|---|---|---|
| add component | 152.5 ns | 131.5 ns | **13.7% faster** |
| remove component | 106.0 ns | 86.4 ns | **18.4% faster** |
| create (2 comp) | 130.0 ns | 133.2 ns | flat (±2%, within noise — create doesn't hit the edge cache on the hot path, as expected) |
| random get | 65.3 ns | 63.8 ns | flat (±2%, within noise — unaffected, as expected) |
| iterate 1/2/3 comp | 0.510 / 1.147 / 1.280 ns | 0.432 / 1.222 / 1.192 ns | flat within run-to-run noise, no regression |

The add/remove win is real and consistent, not a fluke: across all 6 settled rounds, every single "after" add
sample (130-136 ns) was below every single "before" sample (150-155 ns), and likewise remove (85-88 ns after
vs 105-107 ns before) — the two distributions don't overlap.

### Honest sanity-check against the plan's prediction

The perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`, Phase B) originally estimated add
279→~90 ns and remove 220→~60 ns against the **pre-W1** baseline that was current when that line was written.
Rebased onto the actual pre-W2 (i.e. post-W1) starting point — the baseline this session's A/B actually
measured from — the predicted target was **add ~150→~100-110 ns and remove ~106→~70 ns** (i.e. ≈27-34%
reductions: add ~27-33%, remove ~34%), still on the theory that the edge lookup was the dominant remaining
cost. The measured reduction is smaller than that rebased prediction — **13.7%/18.4%**, below the ≈27-34%
range. The wiring was independently verified correct in source (above), so this is not a leftover-path bug;
it means the archetype-edge `FlatMap` double-probe was a real but smaller share of add/remove's cost than
the plan estimated. The remaining cost is dominated by the transition **move** itself (per-element
`MoveConstruct`/`Destruct` via function pointer over the chunk's present components, `Archetype.hpp:1433` /
`ArchetypeChunkPool.hpp:365-396`) — unchanged by W2, and exactly what **W3** (trivial memcpy move) targets next.

Residual gap to flecs, post-W6+W2: add 131.5 vs flecs 54.6 (2.41×, down from 2.79× pre-W2: 152.5/54.6, this
session's own fresh pre-W2/flecs medians), remove 86.4 vs flecs 32.7 (2.64×, down from 3.24× pre-W2:
106.0/32.7). Both gaps shrank but flecs is still meaningfully ahead — consistent with the move-cost diagnosis
above (W3/W4/W5, Phase C, is where that residual gap should close further).

## Phase C (chunk storage modernization: W5+W7+W4+W3) — landed 2026-07-23

Phase C bundled four work items sharing one layout change:
- **W5 — metadata once per archetype.** `ArchetypeColumnMeta` now holds a shared `const ComponentDescriptor*`
  per column instead of a 176 B by-value copy, and the chunk's present-component storage is a packed
  `columns[0..columnCount)` array (ascending id) instead of a `MAX_COMPONENTS`(128)-slot absolute-id-indexed
  array — killing the ~23 KB/chunk metadata bloat and letting every hot loop iterate `0..N` instead of `0..128`.
- **W7 — compact `idToColumn` get.** Record→component resolution now indexes a small per-archetype
  `idToColumn[id]` array instead of walking the old by-value `ComponentArrayInfo` array; this necessarily
  rides on W5's layout (the old 128-slot direct array is gone).
- **W4 — fast-append.** Confirmed a **genuine no-op**: git archaeology (commit `3b77b30`) shows
  `AddEntityWithComponents` already avoided double-constructing caller-supplied components *before* Phase C
  touched anything (it predates the W5 metadata scaffold). The commit has **zero production-code diff** — it
  only adds a regression-guard test (`FastAppendPreservesAllProvidedValues`) pinning the property. `AddEntity`
  (no-value-provided path) is deliberately left doing a full `DefaultConstruct` per column, since chunk slots
  reused after swap-and-pop hold stale bytes, not zeros — skipping it would leak stale data.
- **W3 — trivial memcpy transition move**, gated on `is_trivially_copyable` as a **correctness gate, not an
  optimization** (a blanket memcpy would skip a move-only/lifetime-counting type's move ctor and corrupt it).
  Verified in source on **both** transition directions: the remove path `MoveEntityFrom`
  (`Archetype.hpp:439,462,482`) and the add path `MoveAndAdd`/`MoveAndAddByID`
  (`ArchetypeManager.hpp:1178,1208-1213,1433,1491-1496`) — both merge-join over the two archetypes' packed
  `idToColumn`-indexed column lists (no per-id `isValid` scan) and `memcpy` any trivially-copyable column,
  falling back to `desc.MoveConstruct` otherwise.

### Measurement methodology

Following the W6+W2 session's finding that this machine can be noisy, a second `bench_astra.exe` was built
from the pre-Phase-C commit (`a8712d6`, the last commit before the W5 metadata scaffold) in a temporary git
worktree (`git worktree add <path> a8712d6`). The bench harness files (`bench_astra.cpp`, `bench_common.hpp`,
`build_one.bat`) aren't tracked by git, so they were copied by hand into the worktree's `bench-compare/`
(with `build_one.bat`'s `cd` path adjusted to the worktree) and built with the identical flags. The two
binaries were then run **interleaved, back-to-back, 8 rounds total** (pre-C, post-C, pre-C, post-C, ...).
Unlike the W6+W2 session, all 8 rounds were internally consistent — no noisy leading window needed to be
discarded. The worktree was removed after data collection (`git worktree remove`).

| Operation | Before (pre-C, `a8712d6`, this session, n=8) | After (Phase C, n=8) | Delta |
|---|---|---|---|
| create (2 comp) | 129.4 ns | 49.2 ns | **62.0% faster (2.63×)** |
| add component | 131.2 ns | 55.6 ns | **57.6% faster (2.36×)** |
| remove component | 85.6 ns | 40.8 ns | **52.4% faster (2.10×)** |
| random get | 58.8 ns | 58.4 ns | flat (±1%, within noise — W7's headroom was small post-W1, as expected) |
| iterate 1/2/3 comp | 0.435 / 1.074 / 1.188 ns | 0.451 / 1.069 / 1.176 ns | flat within run-to-run noise, no regression |

The win is real and consistent, not a fluke: across all 8 paired rounds, every post-C sample was strictly
below every pre-C sample for all three structural ops — create pre-C range [128.6, 130.6] vs post-C
[48.5, 50.6]; add pre-C [128.7, 134.5] vs post-C [54.9, 57.8]; remove pre-C [85.0, 90.5] vs post-C
[39.4, 42.5] — non-overlapping distributions.

### Honest attribution — the create win is NOT from W4

W4 was independently confirmed a no-op (no source diff — see above), exactly as this task's brief predicted.
Yet create moved 129.4 → 49.2 ns (2.63×), a bigger win than add or remove got. The actual mechanism is **W5**:
pre-Phase-C, `AddEntity`/`AddEntityWithComponents` scanned the full `0..MAX_COMPONENTS` (128) slot range with
an `isValid`-gated `DefaultConstruct` per slot (root causes R4/R5 in the plan); post-W5, those same functions
iterate the packed `m_meta->columnCount` present-column array (`ArchetypeChunkPool.hpp:137,144,190,199`) — for
the benchmark's 2-component archetype, that's a 2-iteration loop, not a 128-iteration scan with 126 wasted
`isValid` checks. The plan's fix-matrix rated W5's create impact as "●" (merely contributory) and gave W4 the
dominant "●●●" — in practice, since W4's own construct-avoidance logic had already shipped before Phase C,
killing the 0..128 scan (which is what W5 actually did) turned out to be the dominant create lever. The win
is genuine and verified in source; it was just attributed to the wrong work item in the original matrix.

### Add/remove: bigger than predicted, too

The brief's directional guidance suggested add moving toward ~90-100 ns and remove toward ~55-65 ns from the
pre-C baseline (~131/86). Measured: add 55.6 ns, remove 40.8 ns — both **better than the optimistic end of
that range**. W3's memcpy gate (verified correct and present on both transition directions — see above)
compounds with W5: the merge-join over present columns now walks a packed `0..N` array of shared descriptor
pointers (cache-friendly) instead of scanning 128 by-value 176 B `ComponentDescriptor`s, so both the
"which columns move" bookkeeping and the per-column move itself got cheaper together.

### Residual gap to flecs, post-Phase-C

- **create — Astra is now AHEAD of flecs:** 49.2 vs 94.6 ns (Astra **1.92× faster**). This is a genuine
  result: flecs's own numbers are stable and its code is unmodified, and Astra's A/B against its own pre-C
  build is a clean, non-overlapping 2.63× win. The plan's original 3.5× flecs-behind gap on create is now
  inverted.
- **add — near parity:** 55.6 vs 53.6 ns (1.04×). This comparison is cross-library and cross-session (not
  the tightly-controlled pre/post-C worktree A/B), so treat "near parity" rather than a precise ranking as
  the honest takeaway; the plan's Phase-B residual of 2.41× is closed to essentially zero either way.
- **remove — gap much smaller, still real:** 40.8 vs 32.8 ns (1.24×, down from 2.64× post-W6+W2 and 6.4×
  pre-W1).
- **random_get — unchanged, near parity:** 58.4 vs 54.2 ns (1.08×), as expected — get was never a Phase C
  target and W7's remaining headroom post-W1 was small.
- **iteration — flecs still ahead, unchanged:** iterate1 1.16×, iterate2 1.38×, iterate3 1.20× flecs-ahead.
  W5's cache-density argument (per-chunk metadata shrink) was expected to possibly help iteration, but this
  session's data does not show a measurable win there — reported honestly as flat/unchanged rather than
  reading a win into session-level noise. Chunk-size/prefetch tuning remains open for a future pass.

## Honest verdict (updated post-W1)

- My original "Astra **can't out-perform** EnTT/flecs" was **wrong**: Astra beats EnTT's common `view` idiom on 2- and 3-component iteration (1.9×–2.8×) and beats EnTT on single-component.
- My mid-stream "Astra **dominates** iteration" (from the EnTT-only slice) was **also wrong**: **flecs — the direct archetype competitor — is still faster than Astra on all three iteration cases** (iterate2: flecs 0.82 vs Astra 1.17 = **1.4× faster**, narrower than pre-W1's 1.7× but iteration wasn't W1's target — this delta is noise, not a real gain).
- **W1 closed a large chunk of the structural-churn gap.** Pre-W1 Astra was a distant #3 on create/add/remove/get, 2.3×–6.4× behind flecs. Post-W1: create is now 1.4× behind flecs (was 3.5×), remove 3.3× behind (was 6.4×), random_get 1.2× behind (was 2.3×) — random_get in particular is now nearly at flecs parity. Add is still the biggest remaining gap (150.5 vs 53.5 ns, 2.8× — W1 wasn't targeted at add's dominant cost, the edge-lookup double-hash in `ArchetypeGraph`; that's W2's job).

**The real picture:** Astra is still #2 on iteration (ahead of EnTT-view, behind flecs, unchanged by W1 as expected) and now a much closer #3 on structural churn — no longer "distant." EnTT's sparse-set numbers remain out of reach for an archetype model (see plan §4, do-not-chase), but flecs parity (the actual target) is now within ~1.2×–3.3× depending on the op, down from ~2.3×–6.4×.

## Why this is good news, not bad

flecs uses the **same storage model** as Astra. Pre-W1 it was 1.7× faster on iteration and 3.5×–6.4× faster on structural ops — meaning Astra's deficits were **optimization gaps, not architectural limits**. W1 (the first of those fixes) already halved-to-thirded most of the structural gap, confirming the diagnosis. Remaining causes, per the perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`):
- **Iteration gap vs flecs:** W6 (hash mixing) landed but iteration was never W6's target and shows no change, as predicted; the remaining cause is per-chunk 23 KB `ComponentDescriptor`-by-value metadata bloat (cache pressure — W5) and possibly chunk-size/prefetch tuning — both Phase C.
- **Remaining structural gap (add especially):** W2 replaced the two pointer-keyed `FlatMap` probes with array-indexed edges and measurably helped (add 2.79×→2.41× behind flecs, remove 3.24×→2.64×), but the transition **move** cost (per-element ctor/dtor via fn-ptr, not memcpy) now dominates what's left — that's **W3**'s target.
- **random_get** is close to flecs (63.8 vs 57.4 ns, 1.11×) — W1 alone nearly closed this gap, exactly as the plan predicted (W1 "dominates random_get"); W2 doesn't touch this path, as expected (flat).

**Takeaway for the roadmap (as of the W6+W2 landing):** W1 validated the "optimization gaps, not architectural limits" diagnosis — a single surgical change (paged record, no API change) closed most of random_get and a meaningful slice of create/add/remove. **W6+W2 landed 2026-07-22** (avalanche hash mix + array-indexed archetype edges): a real but smaller-than-predicted add/remove win (13.7%/18.4%, below the rebased ≈27-34% prediction — see the W6+W2 section above), confirming the edge lookup was a genuine but not dominant cost. Next up per the plan's execution order: **Phase C** (chunk storage modernization: W5 ⇒ W4 ⇒ W3 ⇒ W7), beginning with W5 — the shared metadata-layout change W4/W3/W7 build on; W3 (trivial memcpy move) is the clearest lever on add/remove once that groundwork lands.

## Honest verdict (updated post-Phase C, 2026-07-23)

**Phase C landed 2026-07-22/23** (W5 metadata-once + packed columns, W7 compact `idToColumn` get, W4
confirmed no-op, W3 memcpy move on both transition directions) and **exceeded the plan's Phase C
expectations on every structural op**, not just met them:

- **create: 129.4 → 49.2 ns (2.63×), Astra now beats flecs (94.6 ns) outright** — the single biggest
  surprise of this round. The plan attributed create's dominant lever to W4; W4 turned out to be a no-op,
  and the real driver was W5 killing the `0..128` scan in `AddEntity`/`AddEntityWithComponents` (see "Honest
  attribution" above). The plan's original 3.5×-behind-flecs create gap is now inverted to Astra being ahead.
- **add: 131.2 → 55.6 ns (2.36×), essentially at flecs parity (53.6 ns, 1.04×)** — beyond the brief's
  optimistic ~90-100 ns guidance, and closing what had been the single biggest remaining structural gap
  (2.8× behind flecs pre-W2, 2.41× post-W6+W2) to near-zero.
- **remove: 85.6 → 40.8 ns (2.10×)**, gap to flecs now 1.24× (down from 6.4× pre-W1, 2.64× post-W6+W2) —
  smaller than add's closure but still a large, real win.
- **random_get and iteration are honestly flat**, as predicted (W7's headroom was small post-W1; iteration
  was never Phase C's primary target and this session's data shows no measurable movement there, reported
  as such rather than reading a win into noise).

**Updated overall picture:** what started as "Astra is a distant #3 behind flecs on every structural op"
(pre-W1: 2.3×-6.4× behind) is now "Astra leads flecs on create, ties it on add, and trails by only ~1.1×-1.2×
on remove/random_get" — with iteration (flecs's own strength, ~1.2×-1.4× ahead of Astra) the only place flecs
still holds a clear edge. This confirms the plan's central diagnosis end-to-end: the gaps were optimization
gaps in Astra's implementation of the same archetype model flecs uses, not model-inherent limits — three
surgical phases (W1, then W6+W2, then W5+W7+W4+W3) closed nearly all of them.

**Next per the plan's execution order (§3/§7): Phase D** — wiring `CommandBuffer::Execute` through the
existing `GroupEntitiesByArchetype`/batch-move machinery, and per-chunk change versions for change detection.
Both are **roadmap features, not benchmark movers** (the immediate single-entity API this benchmark exercises
is already close to fully optimized); Phase D's value is real-world batched-workload throughput and the
change-detection feature itself, not this microbenchmark's numbers.

## Phase 2 — dynamic chunk sizing (2026-07-23, branch `perf/phase-2-dynamic-chunk-sizing` @ `ae40c9b`)

Phase 2 replaced the fixed 16KB chunk with a **TLSF (Two-Level Segregated Fit) allocator** +
**grow-as-populate sizing** (`chunkBytes = clamp(archetypeBytes / growDivisor, minChunkBytes,
maxChunkBytes)`, shipped NSDMIs `growDivisor=2`, `minChunkBytes=4KB`, `maxChunkBytes=512KB`) +
**per-chunk-capacity addressing** (dropped the uniform pow2 `m_entitiesPerChunk` shift/mask —
`EntityLocation`/`EntityRecord`/on-disk format unchanged) + **compaction-on-defrag** (rebuild-style
repack of sparse archetypes into fresh target-size chunks, freed chunks TLSF-coalesced). Full design:
`docs/superpowers/specs/2026-07-23-phase-2-dynamic-chunk-sizing-design.md`.

### 3-config test suite (Step 1)

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 722 | 722 | clean |
| Release | 720 | 719 direct / 720 after retry | `CompressionTest.PerformanceBenchmark` failed once (9.71 MB/s vs the >10.0 MB/s throughput gate), passed on 3 isolated reruns (11.99, 10.92, 11.12 MB/s) — the known pre-existing timing-threshold flake, not a regression |
| Dist | 720 | 719 direct / 720 after retry | same flake, isolated reruns 2/2 passed (11.47, 11.36 MB/s) |

No other failures in any config. This matches the expected post-Task-6 counts (722/720/720).

### Machine state — RESOLVED, quiet-machine re-run performed (2026-07-23, later same day)

The first pass at Steps 2/3 (below, in the "Superseded" subsections) ran under **sustained ~70–84%
background CPU load** from the user's own foreground applications (a running game process, Discord, a
browser, Steam/Roblox helpers). The user then closed the game and heavy apps. Before trusting the
machine, load was re-sampled (`Get-CimInstance Win32_Processor LoadPercentage`, repeated windows):
a brief transient spike (avg ~27–29%, one process — `clangd`, the IDE's background indexer — visibly
churning) settled within ~15–20s to a clean **avg 10.7% and 12.6%** across two consecutive windows,
confirmed well under the 15% bar with a per-process CPU-delta snapshot showing only small, ordinary
desktop background consumers (browser tabs, Steam helper) — no heavy foreground app. Steps 2 and 3 were
then re-run in full. **The results in this section are the authoritative, quiet-machine numbers**; the
original noisy-session tables are retained further down, clearly marked **superseded**, for transparency
about what changed and why.

### 3-way benchmark (Step 2) — quiet machine, 7 interleaved rounds (astra→flecs→entt ×7)

`bench_astra.cpp` needed a harness fix before it would even build against this branch's headers (not a
library defect, and unchanged from the first pass): its three `View::ForEach` lambdas were missing the
mandatory leading `Astra::Entity` parameter (every other call site in the repo, e.g.
`benchmark/Benchmark.cpp`, passes `(Astra::Entity, Components&...)`) — fixed in the untracked scratch
file, not a library change. HEAD was confirmed unchanged (`ae40c9b` under the results commit) before
reusing the existing `bench_astra.exe`; no rebuild was needed.

Median of 7 same-session interleaved rounds, N=1,000,000, ns/op (lower = better), with [min, max] spread:

| Operation | Astra | flecs | EnTT |
|---|---|---|---|
| create | **48.56** [46.81, 51.46] | 97.92 [96.73, 103.96] | 41.29 [39.59, 46.77] |
| add | **52.29** [51.40, 52.79] | 55.82 [54.75, 58.16] | 11.73 [11.46, 12.34] |
| remove | 38.25 [37.82, 39.73] | **34.18** [33.83, 36.06] | 16.46 [16.29, 17.30] |
| random_get | 69.70 [67.76, 75.41] | **62.76** [60.03, 83.81]¹ | 32.45 [28.82, 47.52] |
| iterate1 | 0.451 [0.392, 0.652] | **0.396** [0.387, 0.557] | 0.641 [0.565, 0.794] |
| iterate2 | 1.237 [1.049, 1.873] | **1.107** [0.886, 1.464] | 2.420 view / 1.330 group |
| iterate3 | 1.359 [0.975, 1.888] | **1.123** [1.042, 1.652] | 3.277 view |

¹ flecs random_get had one anomalous round (83.807 ns, round 6 of 7) — Astra's and EnTT's own round-6
numbers were unremarkable that round, so the spike looks isolated to flecs, not shared background
noise; excluding it, flecs's clean-round range is [60.03, 65.45].

For context, the historical clean-machine baseline (dev `cb68bf3`, pre-Phase-2, a different session —
included for trend, not as a same-session comparison):

| Operation | Astra (historical) | flecs (historical) | EnTT (historical) |
|---|---|---|---|
| create | 51.4 | 95.9 | 40.6 |
| add | 57.1 | 54.4 | 12.1 |
| remove | 40.6 | 34.1 | 16.1 |
| random_get | 65.4 | 57.0 | 25.3 |
| iterate1 | 0.509 | 0.390 | 0.492 |
| iterate2 | 0.993 | 0.812 | 1.04 (group) |
| iterate3 | 1.050 | 0.889 | 3.21 (view) |

This quiet-machine session's absolute numbers track the historical baseline closely (create/add/remove
within a few percent; flecs create 97.9 vs 95.9 historical, astra create 48.6 vs 51.4 historical) —
consistent with a genuinely quiet machine, unlike the first pass's 1.2×–1.8× inflation.

**Per-round paired comparison** (same round, both libraries) — win counts out of 7, spreads now mostly
non-overlapping (tight and trustworthy):

| Operation | Astra-faster-than-flecs, rounds | Spread overlap? | Read |
|---|---|---|---|
| create | **7/7** | none — Astra max (51.46) < flecs min (96.73) | **robust, decisive** |
| add | **7/7** | none — Astra max (52.79) < flecs min (54.75) | **robust — a clean win, not just "no regression"** |
| remove | 0/7 (flecs faster 7/7) | none — Astra min (37.82) > flecs max (36.06) | robust; Astra ~11.9% slower, close to (slightly better than) the historical ~19% gap — no regression |
| iterate2 | 1/7 (flecs faster 6/7) | full | flecs robustly faster but the median gap shrank a lot (~11.8% vs ~22% historical) |
| iterate3 | 2/7 (flecs faster 5/7) | full | flecs faster; gap (~21%) essentially matches historical (~18%) — no regression, not the hoped "~parity" either |
| iterate1 | 2.5/7 (flecs faster 4/7, one near-tie) | full | close to a coin-flip, but flecs's median edge (~14%) is real; a big improvement over the historical ~31% gap |
| random_get | 1/7 (flecs faster 6/7, excl. the flecs outlier round) | none excluding the outlier | robust; Astra ~11.1% slower — essentially the *same* gap as historical (~15%), i.e. **no improvement materialized here** |

**Verdict against the Step 2 success criteria (quiet-machine, authoritative):**
- **create stays ahead of flecs:** ✅ **confirmed, decisively** — non-overlapping, 7/7 rounds, and the
  gap is even wider than the historical baseline (2.02× vs 1.87× historically).
- **add/remove no regression beyond noise:** ✅ **confirmed, and add is better than required** — add is
  a clean, non-overlapping win over flecs this session (not just parity); remove's gap is essentially
  unchanged from (slightly better than) history.
- **iterate1, iterate2, random_get at-or-ahead of flecs:** ❌ **not met, now confirmed with confidence
  (not noise).** flecs remains ahead on all three. The honest, more nuanced picture: **iterate1 and
  iterate2 both improved substantially** — their gaps to flecs roughly halved from the historical
  pre-Phase-2 baseline (iterate1 ~31%→~14%, iterate2 ~22%→~12%) — real, partial progress toward the
  study's prediction, just not enough to flip the ranking. **random_get did not improve at all** — its
  gap to flecs (~11%) is essentially the same as the pre-Phase-2 historical gap (~15%), contradicting
  the study's predicted +16% Astra-side swing for this specific op. This is now a clean, decisive,
  non-noise finding, not an open question.

### Grow-divisor / max-chunk-bytes mini-sweep (Step 3) — quiet machine re-run

Same `bench-compare/bench_sweep2.cpp` harness, re-run on the quiet machine. Numbers are much tighter
and internally consistent than the first pass (e.g. all six cells' create times now cluster in
47.2–49.0 ns at N=1e6, vs the noisy pass's 67.9–87.6 ns spread).

N=1,000,000, ns/op (shipped NSDMIs `growDivisor=2, maxChunkBytes=512KB` marked `*`):

| growDivisor, cap | create | add | remove | random_get | iterate1 | iterate2 | iterate3 |
|---|---|---|---|---|---|---|---|
| 1, 256KB | 47.34 | 52.25 | 39.04 | 66.78 | 0.391 | 0.907 | 1.338 |
| 1, 512KB | 49.00 | 51.98 | 39.13 | 63.04 | 0.390 | 0.910 | 0.894 |
| 2, 256KB | 48.17 | 52.74 | 38.51 | 64.59 | 0.343 | 0.843 | 1.029 |
| **2, 512KB\*** | **47.17** | 53.80 | 39.57 | 71.21 | 0.400 | 1.029 | **0.849** |
| 4, 256KB | 48.25 | 54.33 | 38.93 | 69.32 | 0.409 | 0.895 | 1.426 |
| 4, 512KB | 48.09 | 51.19 | 37.33 | 64.97 | 0.385 | 0.956 | 0.889 |

(N=100,000 and N=1,000 tables in the untracked `sweep2_results.csv` show the same qualitative picture.)

**Reading:** the shipped `(2, 512KB)` cell wins outright on **create** (47.17, the best of the six) and
**iterate3** (0.849, the best of the six), and is mid-pack on **remove** (39.57) and **iterate1**
(0.400). It is the *worst* of the six on **add** (53.80, ~5% above the best) and **random_get** (71.21,
~11% above the best, `1,512KB`) and **iterate2** (1.029, ~22% above the best, `2,256KB`). No single
alternative cell dominates across all primary ops either: `4, 512KB` is modestly better than shipped on
add/remove/random_get/iterate2 (roughly 2–10%) at N=1e6, and similarly at N=1e5/1e3, but is not
uniformly ahead (create is a rounding error apart, iterate3 favors shipped by ~4.5%) — and each cell was
measured as a single MIN-of-3 sample, not a repeated-trial confidence interval, so a ~5–10% edge on 2 of
7 ops isn't yet strong enough to call a real, reproducible win.

**Verdict: the shipped NSDMIs hold, but not by a landslide.** `growDivisor=2, maxChunkBytes=512KB`
remains a reasonable, defensible default — it wins the two ops the design most cares about
(create, and iterate3 is a bonus) and is never the *worst* choice by a wide margin. `growDivisor=4,
maxChunkBytes=512KB` shows a modest, worth-a-look edge on add/remove/random_get/iterate2 (~2–10%,
fairly consistent across N) that a future pass could investigate with repeated trials, but this single
sweep pass is not a data-backed case to change the NSDMIs now, per the brief's own bar (>5%,
*consistently*, on the primary ops — met on some ops, not all). **NSDMIs left unchanged.**

### Superseded — first pass (noisy machine, ~70–84% background CPU load), kept for transparency

<details>
<summary>Original noisy-session Step 2/3 tables and verdict (click to expand) — superseded by the
quiet-machine re-run above; retained only so the before/after and the reasoning for the re-run are
auditable.</summary>

Median of 7 same-session interleaved rounds under heavy background load, N=1,000,000, ns/op, [min, max]:

| Operation | Astra (noisy session) | flecs (noisy session) | EnTT (noisy session) |
|---|---|---|---|
| create | 73.76 [59.15, 85.40] | 162.63 [125.25, 197.29] | 59.62 [50.30, 75.10] |
| add | 67.73 [64.51, 88.47] | 94.73 [68.52, 104.51] | 15.42 [14.20, 20.61] |
| remove | 59.57 [42.91, 76.73] | 47.74 [43.01, 59.61] | 18.49 [18.01, 23.06] |
| random_get | 194.79 [128.27, 226.27] | 174.28 [113.65, 208.20] | 83.20 [55.53, 146.40] |
| iterate1 | 1.193 [0.639, 2.851] | 1.651 [0.787, 2.544] | 1.941 [1.042, 3.004] |
| iterate2 | 2.474 [2.044, 4.403] | 2.793 [2.128, 5.076] | 4.233 view / 2.785 group |
| iterate3 | 2.651 [2.338, 5.382] | 3.355 [2.216, 5.382] | 4.803 view |

Per-round paired win counts (of 7): create 7/7 Astra, add 7/7 Astra, remove 1/7 Astra (flecs 6/7),
iterate3 5/7 Astra, iterate2 4/7 Astra, iterate1 4/7 Astra, random_get 1/7 Astra (flecs 6/7). Verdict at
the time: create/add/remove robust, iterate1/iterate2/random_get inconclusive (spreads fully
overlapped) — recommended a clean-machine re-run, which is the quiet-machine section above. In
hindsight, the paired win-rate direction for every op (including the inconclusive ones) matches the
quiet-machine result — the noise widened the spreads enough to erase confidence, but didn't flip the
underlying signal.

Grow-divisor sweep, noisy session, N=1,000,000, ns/op:

| growDivisor, cap | create | add | remove | random_get | iterate1 | iterate2 | iterate3 |
|---|---|---|---|---|---|---|---|
| 1, 256KB | 67.86 | 84.60 | 66.77 | 231.90 | 1.536 | 3.440 | 3.904 |
| 1, 512KB | 78.23 | 85.33 | 57.26 | 209.32 | 1.901 | 3.854 | 2.928 |
| 2, 256KB | 77.01 | 85.36 | 75.43 | 263.19 | 0.356 | 2.135 | 4.176 |
| 2, 512KB* | 87.60 | 74.76 | 64.44 | 235.43 | 1.084 | 2.820 | 3.416 |
| 4, 256KB | 87.58 | 85.96 | 61.83 | 212.19 | 0.833 | 2.746 | 3.448 |
| 4, 512KB | 81.06 | 86.78 | 71.29 | 136.18 | 1.943 | 3.229 | 4.031 |

Verdict at the time: inconclusive (non-monotonic, metric-flipping pattern under noise); the quiet-machine
re-sweep above is the authoritative one.

</details>

### Memory-waste check (Step 4)

`ArchetypeTest.SmallArchetypeStaysAtMinimumChunk` (added in Task 5, `tests/Registry/ArchetypeTest.cpp:1382`)
already pins the exact invariant and passes in isolation: 100 `Position` entities (100 × 12B ≈ 1.2KB)
stay in exactly one chunk, and that chunk is exactly 4096 bytes.

A scratch probe (`Registry::GetArchetypeMemoryUsage`/`GetArchetypeCount`, isolated via baseline
subtraction — a fresh `Registry` already carries an empty root archetype with its own fixed
bookkeeping overhead and a legacy-fixed-size chunk, so a second `Registry` with the 100 entities was
diffed against it to isolate the `{Position}` archetype's own contribution) corroborates this at the
public-API level:

```
baseline (root archetype only): archetypeCount=1, ArchetypeMemoryUsage=20992 bytes
with 100 Position entities:     archetypeCount=2, ArchetypeMemoryUsage=29696 bytes (29.00 KB)
{Position} archetype's OWN contribution = 8704 bytes = 4096 (its one 4KB data chunk)
                                           + 4608 (fixed per-archetype bookkeeping, unrelated to Phase 2)
```

4096 bytes for the data chunk itself, exactly the 4KB `minChunkBytes` floor, vs the pre-Phase-2 fixed
`ArchetypeChunkPool::DEFAULT_CHUNK_SIZE = 16384` (16KB) every archetype's first chunk used to cost
regardless of how little data it held — a genuine 4× reduction for small archetypes, confirming Phase
2's memory-waste goal. No new test was added; Task 5's existing test already covers this exactly.

### Overall Phase-2 verdict (quiet-machine, authoritative)

- **Robust, decisive wins:** create (2.02× ahead of flecs, 7/7 non-overlapping rounds — even better than
  the historical 1.87×), add (a clean, non-overlapping win over flecs, 7/7 rounds — better than the
  "no regression" bar), and the memory-waste invariant (4× smaller footprint for small archetypes,
  directly measured).
- **No regression:** remove (~11.9% behind flecs, essentially the historical gap, slightly improved).
- **Real, partial progress — gap roughly halved but ranking unchanged:** iterate1 (~31%→~14% behind
  flecs) and iterate2 (~22%→~12% behind flecs) both improved substantially versus the historical
  pre-Phase-2 baseline, confirming Phase 2 helped iteration meaningfully — just not enough to reach
  "at-or-ahead of flecs."
- **Did not improve:** random_get's gap to flecs (~11%) is essentially unchanged from the historical
  pre-Phase-2 gap (~15%) — the study's predicted +16% swing for this specific op did not materialize.
  This is now a clean, decisive, non-noise finding (non-overlapping spreads, 6/7 paired rounds), not an
  open question.
- **Divisor/cap sweep:** the shipped `growDivisor=2, maxChunkBytes=512KB` NSDMIs hold — best on
  create and iterate3, mid-pack on remove/iterate1, worst (by 5–22%) on add/random_get/iterate2 versus
  the best alternative cell in each case, with no single alternative dominating across all primary ops.
  `growDivisor=4` (either cap) is a modest, worth-a-future-look candidate on add/remove/random_get/
  iterate2, but this single-sample-per-cell sweep isn't a strong enough basis to change the defaults now.
- **No library defect found.** All test-suite failures were the pre-existing `CompressionTest` timing
  flake, confirmed via isolated reruns. This task changed no library code (only the untracked benchmark
  scratch harness).
- **Net read for the merge decision:** Phase 2 delivers on its memory-waste goal outright, delivers a
  decisive structural-op win (create, add), holds steady on remove, and makes real but incomplete
  progress on iteration (iterate1/iterate2) — while random_get's predicted improvement did not
  materialize on this machine. None of this is a regression; the shortfall is against the study's
  optimistic prediction, not against Astra's own pre-Phase-2 baseline.

## Lever 1 — random_get record→direct-chunk-pointer (2026-07-24, branch `perf/random-get-chunk-pointer` @ `f5ea6c3`)

Design: `docs/superpowers/specs/2026-07-24-random-get-chunk-pointer-design.md`. `EntityRecord` now caches
the `ArchetypeChunk*` for its location (behind a write funnel — all 16 write sites routed through
`EntityTable::SetRecord`/`ArchetypeManager`'s `SetRecordLocation`/`ClearRecordLocation`), so
`Archetype::GetComponent` and `Registry`'s ByID/hash `GetComponent` paths read `rec->chunk` directly
instead of hopping through `archetype->GetChunks()[location.GetChunkIndex()]` + a mask test — shortening
the dependent-load chain from 8 hops to ~5. The trade-off, called out in the spec up front: `EntityRecord`
grew from 24B to a hard `alignas(32)` 32B (guaranteeing no record ever straddles a cache line — 1/3 of the
old 24B records did) at the cost of a fatter per-entity record, i.e. lower record density per cache line
for the random-order lookups this lever targets.

### 3-config test suite (Step 1)

| Config | Total | Passed | Notes |
|---|---|---|---|
| Debug | 732 | 732 | clean, no flake |
| Release | 730 | 730 | clean, no flake |
| Dist | 730 | 730 | clean, no flake |

Matches the expected post-branch counts (732/730/730 — the Release/Dist delta from Debug is the 2
`EXPECT_DEATH`-only tests, same spread pattern as dev's 724/722/722). `CompressionTest.PerformanceBenchmark`
did not flake this run; no reruns were needed. `bench_astra.exe` was rebuilt clean against this branch's
headers (Step 2) with no source changes needed this time (the `View::ForEach` leading-`Entity`-param fix
from the Phase-2 session was already present in the untracked harness).

### Machine load (Step 3 pre-check)

`typeperf "\Processor(_Total)\% Processor Time"`, two windows of 6–8 samples each, taken immediately
before benching: **avg ~9.9% and ~11.7%** (samples 8–15%, one 14.8%/14.5% pair, otherwise high-single-digits)
— comfortably under the 15–20% quiet-machine bar (`Get-Process` showed the usual desktop background set —
Discord, Steam helpers, a Sublime/editor process, browsers, no heavy foreground app — consistent with
ordinary idle-desktop load, not a spike). Machine judged quiet; results below are **not** noise-flagged.

### 6 interleaved rounds (astra→flecs→entt ×6), N=1,000,000, ns/op, median [min, max]

| Operation | Astra (this branch) | flecs | EnTT |
|---|---|---|---|
| create | 50.45 [49.59, 61.73]¹ | 100.93 [98.17, 104.28] | 41.83 [40.00, 48.16] |
| add | 54.14 [52.04, 56.19] | 57.34 [56.52, 62.04] | 11.90 [11.59, 12.51] |
| remove | 37.65 [37.22, 38.69] | 34.58 [33.80, 35.34] | 16.47 [16.29, 16.87] |
| **random_get** | **74.55 [72.42, 88.32]¹** | 67.26 [61.99, 71.45] | 37.51 [32.07, 41.62] |
| iterate1 | 0.468 [0.401, 0.539] | 0.476 [0.393, 0.553] | 0.754 [0.690, 1.361] |
| iterate2 | 1.107 [0.958, 1.285] | 1.122 [0.937, 1.511] | 2.608 view / 1.524 group |
| iterate3 | 1.471 [1.091, 2.087]¹ | 1.507 [1.256, 2.742] | 3.516 view |

¹ Round 3 had a latency blip localized to Astra's turn (create 61.73, random_get 88.32, iterate3 2.09 —
all well above the other 5 rounds), while flecs's and EnTT's own round-3 numbers were unremarkable that
round — looks like a scheduler/cache hiccup isolated to Astra's process, not shared background noise.
Excluding round 3: create median 50.05 [49.59, 51.70], random_get median 72.71 [72.42, 76.46], iterate3
median 1.40 [1.09, 1.67] — the random_get exclusion narrows the number but does not change the verdict
below (still well above the ≤63ns target, still robustly slower than flecs every round).

### Before/after vs the 2026-07-23 pre-lever baseline

| Operation | Astra before (dev, `ec1d5ce`) | Astra after (this branch) | Δ% | Target |
|---|---|---|---|---|
| create | 48.56 | 50.45 | +3.9% | within noise |
| add | 52.29 | 54.14 | +3.5% | within noise |
| remove | 38.25 | 37.65 | -1.6% | within noise |
| **random_get** | **69.70** | **74.55** | **+7.0%** | **≤63 (missed)** |
| iterate1 | 0.451 | 0.468 | +3.8% | within noise |
| iterate2 | 1.237 | 1.107 | -10.5% | within noise |

**Per-round paired comparison, random_get, Astra vs flecs:** flecs faster in **6/6** rounds, no spread
overlap (Astra min 72.42 > flecs max 71.45) — identical direction and a near-identical relative gap to the
pre-lever baseline (this session: (74.55−67.26)/67.26 = **10.8%** Astra-slower; pre-lever baseline session:
(69.70−62.76)/62.76 = **11.1%** Astra-slower). create/add/remove keep their pre-lever win/loss pattern too
(create 6/6 Astra-faster, add 6/6 Astra-faster, remove 6/6 flecs-faster — all matching the historical 7/7
patterns), confirming the lever changed nothing structural about any op except the one it targeted.

**Interpretation.** The lever missed its target: random_get's median (74.55ns) is not just short of the
≤63ns goal, it's numerically *worse* than the 69.70ns pre-lever baseline. Two pieces of evidence say this
is not primarily a regression introduced by the branch, though: (1) flecs's and EnTT's own random_get
numbers inflated by similarly large amounts in this same session versus their 2026-07-23 numbers (flecs
62.76→67.26, +7.2%; EnTT 32.45→37.51, +15.6%) — a session-to-session shift affecting all three libraries in
the same direction, most plausibly machine/thermal/turbo variance rather than anything in Astra's code path;
and (2) the *same-session, paired* Astra-vs-flecs gap (10.8%) is essentially unchanged from the pre-lever
historical gap (11.1%) — the fair, noise-controlled comparison says the gap didn't move, not that it grew.
Taking both together, the honest read is **no measurable improvement**, not a proven regression: the
predicted win (dependent-load chain 8→~5) did not show up in the gap to flecs, and the most likely
structural explanation is the trade-off the spec called out up front — growing `EntityRecord` from 24B to a
hard-aligned 32B lowers record density for exactly the random-order access pattern this lever targets, and
that cache-pressure cost plausibly ate the shortened-chain benefit. All other primary ops (create, add,
remove, iterate1, iterate2) stayed within session-level noise of their pre-lever baselines (−10.5%..+3.9%)
and kept the same win/loss pattern against flecs — **no regression found anywhere except the missed
random_get target.** This is a real, decisive, non-noise finding (not an open question) and should inform
the merge decision as such: the lever is safe to merge (no regressions), but should not be described as
having closed the random_get gap.

### Full-opt flag re-run — NEW BENCH FLAG BASELINE (2026-07-24, dev @ `5590ca8`, post-Lever-1 merge)

**Why.** Every bench run above built with minimal flags (`/O2 /DNDEBUG /EHsc`). The solution's own Dist
config compiles with substantially more: `/arch:AVX`, manual `__SSE2__`/`__SSE4_2__` defines (MSVC never
auto-defines these — without them Mosaic's `Platform.hpp:172-191` compiles OUT the hardware-CRC32 and AVX
SIMD tiers, so every prior bench ran Astra at its SSE2 floor), `/fp:fast`, `/GL`+`/LTCG`.
(`ASTRA_BUILD_DIST` was checked and is cosmetic — a build-type string; `NDEBUG` governs asserts.)
All THREE benches were rebuilt with the identical full flag set for fairness:
`/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /link /LTCG`
(+ `/DASTRA_BUILD_DIST` for Astra; flecs.c compiled `/c /O2 /GL /W0 /DNDEBUG /arch:AVX /fp:fast`).
**These flags are the bench recipe from now on** — they match what a real optimized consumer ships with.

**Setup.** Quiet machine (typeperf ~3-11% before the run). 6 interleaved rounds (astra→flecs→entt), N=1M,
medians [min,max]. Raw: `ab2_rounds.csv` (untracked).

| Operation | Astra | flecs | EnTT | Astra vs flecs |
|---|---|---|---|---|
| create | 49.90 [48.69, 51.17] | 89.54 [88.35, 92.52] | 38.91 [37.29, 44.71] | **1.79× ahead** |
| add | 48.63 [47.34, 50.11] | 50.99 [49.18, 52.84] | 11.65 [11.21, 12.93] | **~5% ahead** |
| remove | 37.36 [36.95, 38.24] | 33.84 [31.51, 35.98] | 16.59 [16.44, 17.05] | ~10% behind |
| random_get | 60.03 [54.09, 70.05] | 56.21 [54.77, 67.93] | 24.40 [22.26, 45.98] | ~4% behind (paired median; **2/6 rounds ahead**) |
| iterate1 | 0.484 [0.385, 0.602] | 0.434 [0.382, 0.741] | 0.576 [0.515, 0.732] | ~11% behind (spreads overlap heavily) |
| iterate2 | 1.046 [0.836, 1.148] | 0.885 [0.846, 1.036] | 2.80 view / 1.34 group | ~18% behind — now the biggest gap |
| iterate3 | 0.997 [0.915, 1.216] | 1.010 [0.892, 1.129] | 3.23 view | **~parity (slightly ahead)** |

Per-round paired random_get gaps (Astra vs flecs): −1.1%, +4.4%, −1.3%, +17.8%, +3.1%, +8.1%.

**Interpretation.** Under Dist-parity flags the random_get gap to flecs narrows from ~11% (both prior
sessions) to **~4% median, with Astra winning 2 of 6 rounds — effectively near-parity**. IMPORTANT CAVEAT:
this run changes BOTH the flags and (vs the 07-23 baseline) includes the Lever-1 merge, and no pre-lever
build was measured under the new flags — so the narrowing CANNOT be attributed to the lever vs the SIMD
tiers (hardware CRC32, AVX) vs LTCG; they are confounded. What can be said cleanly: **on the flags a real
user would ship with, current dev is 1.79× ahead of flecs on create, ahead on add, near-parity on
random_get and iterate3, ~10% behind on remove, and ~18% behind on iterate2** — iterate2 replaces
random_get as the biggest flecs gap (flecs's 2-component loop benefits strongly from AVX+`/fp:fast`
auto-vectorization: its iterate2 dropped 1.11→0.885 across the two same-day sessions while Astra's dropped
only 1.107→1.046). EnTT's sparse-set random_get also leaps ahead under full opts (37.5→24.4) — model-inherent,
not a target. Net for the lever program: lever 2 (per-chunk iteration-setup flattening, iterate1/2) is now
clearly the highest-value target; remove (~10%) second; random_get is no longer the headline gap.

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`. **Build with the full-opt flag set above (2026-07-24 baseline), not bare `/O2`.**
