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

### ⚠️ Machine state during Step 2/3 benchmarking — NOT a clean machine

Unlike the W1/W6+W2/Phase-C sessions recorded above, this session's benchmarks ran with **sustained
~70–84% background CPU load** from the user's own foreground applications (a running game process,
Discord, a browser, Steam/Roblox helpers — sampled via `Get-CimInstance Win32_Processor
LoadPercentage` repeatedly through the session, and independently via `Get-Process | Sort CPU`). This
was not started by the benchmark run and was not closed (killing a user's running game/apps is outside
this task's authority) — it is disclosed here so the numbers below are read with the correct amount of
skepticism. **The top-of-file summary table above is deliberately left unchanged**; the numbers in this
section are NOT proposed as a replacement headline, only as this session's evidence for the merge
decision.

### 3-way benchmark (Step 2) — 7 interleaved rounds (astra→flecs→entt ×7)

`bench_astra.cpp` needed a harness fix before it would even build against this branch's headers (not a
library defect): its three `View::ForEach` lambdas were missing the mandatory leading `Astra::Entity`
parameter (every other call site in the repo, e.g. `benchmark/Benchmark.cpp`, passes `(Astra::Entity,
Components&...)`) — fixed in the untracked scratch file, not a library change.

Median of 7 same-session interleaved rounds, N=1,000,000, ns/op (lower = better), with [min, max] spread:

| Operation | Astra (this session) | flecs (this session) | EnTT (this session) |
|---|---|---|---|
| create | **73.76** [59.15, 85.40] | 162.63 [125.25, 197.29] | 59.62 [50.30, 75.10] |
| add | **67.73** [64.51, 88.47] | 94.73 [68.52, 104.51] | 15.42 [14.20, 20.61] |
| remove | 59.57 [42.91, 76.73] | **47.74** [43.01, 59.61] | 18.49 [18.01, 23.06] |
| random_get | 194.79 [128.27, 226.27] | **174.28** [113.65, 208.20] | 83.20 [55.53, 146.40] |
| iterate1 | 1.193 [0.639, 2.851] | 1.651 [0.787, 2.544] | 1.941 [1.042, 3.004] |
| iterate2 | 2.474 [2.044, 4.403] | 2.793 [2.128, 5.076] | 4.233 view / 2.785 group |
| iterate3 | 2.651 [2.338, 5.382] | 3.355 [2.216, 5.382] | 4.803 view |

For context only (NOT directly comparable — different machine state), the historical clean-machine
baseline (dev `cb68bf3`, pre-Phase-2):

| Operation | Astra (historical) | flecs (historical) | EnTT (historical) |
|---|---|---|---|
| create | 51.4 | 95.9 | 40.6 |
| add | 57.1 | 54.4 | 12.1 |
| remove | 40.6 | 34.1 | 16.1 |
| random_get | 65.4 | 57.0 | 25.3 |
| iterate1 | 0.509 | 0.390 | 0.492 |
| iterate2 | 0.993 | 0.812 | 1.04 (group) |
| iterate3 | 1.050 | 0.889 | 3.21 (view) |

All three libraries' this-session absolute numbers are inflated 1.2×–1.8× over their own historical
baseline (e.g. flecs create 162.6 vs 95.9 historical, entt create 59.6 vs 40.6 historical, astra create
73.8 vs 51.4 historical) — consistent with shared background contention, not a code regression in any
of the three (their sources are unmodified for flecs/entt; Astra's own 3-config suite above is green).

**Because raw medians of independently-noisy series can mislead, the more robust read is a per-round
paired comparison** (same round, same background-load instant, both libraries) — win counts out of 7:

| Operation | Astra-faster-than-flecs, rounds | Spread overlap? | Read |
|---|---|---|---|
| create | **7/7** | none — Astra max (85.40) < flecs min (125.25) | **robust, decisive** |
| add | **7/7** | partial (64.5–88.5 vs 68.5–104.5) | **robust** |
| remove | 1/7 (flecs faster 6/7) | heavy | consistent with the known ~1.2× post-Phase-C gap (40.8 vs 32.8 historical) — no regression |
| iterate3 | 5/7 | full | leans Astra-favorable, inconclusive |
| iterate2 | 4/7 | full | inconclusive (near coin-flip) |
| iterate1 | 4/7 | full | inconclusive (near coin-flip) |
| random_get | 1/7 (flecs faster 6/7) | full | **does not confirm** the study's predicted +16%/at-or-ahead-of-flecs outcome this session |

**Verdict against the Step 2 success criteria:**
- **create stays ahead of flecs:** ✅ confirmed, decisively and robustly (non-overlapping across all 7 rounds).
- **add/remove no regression beyond noise:** ✅ add is a clean win every round; remove's ~1.2–1.5×
  gap to flecs matches the already-known post-Phase-C gap, not a new regression.
- **iterate1, iterate2, random_get at-or-ahead of flecs:** ⚠️ **not confirmed this session.**
  iterate1/iterate2 are genuine coin-flips (4/7) under this load, and random_get leans the other way
  (flecs faster 6/7 rounds) — the opposite of the study's predicted +16% Astra-side improvement. Given
  the spreads fully overlap for all three, this session's data cannot distinguish a real regression from
  simply not being able to see the predicted win through the noise floor. **This should be re-measured
  on an idle machine before being treated as a settled result either way** — it is the one open question
  this task did not resolve with confidence.

### Grow-divisor / max-chunk-bytes mini-sweep (Step 3)

`bench-compare/bench_sweep2.cpp` (untracked scratch, not `bench_sweep.cpp` — a fresh minimal harness
was cleaner than adapting the Phase-1 file's payload-sweep structure) sweeps `growDivisor ∈ {1,2,4} ×
maxChunkBytes ∈ {256KB,512KB}` at `N ∈ {1e3,1e5,1e6}` over the 7 standard ops, MIN-of-3 reps per cell
(`minChunkBytes` left at its 4KB default throughout). Shipped NSDMIs are `growDivisor=2,
maxChunkBytes=512KB` (marked `*` below).

N=1,000,000, ns/op:

| growDivisor, cap | create | add | remove | random_get | iterate1 | iterate2 | iterate3 |
|---|---|---|---|---|---|---|---|
| 1, 256KB | 67.86 | 84.60 | 66.77 | 231.90 | 1.536 | 3.440 | 3.904 |
| 1, 512KB | 78.23 | 85.33 | 57.26 | 209.32 | 1.901 | 3.854 | 2.928 |
| 2, 256KB | 77.01 | 85.36 | 75.43 | 263.19 | 0.356 | 2.135 | 4.176 |
| **2, 512KB\*** | 87.60 | 74.76 | 64.44 | 235.43 | 1.084 | 2.820 | 3.416 |
| 4, 256KB | 87.58 | 85.96 | 61.83 | 212.19 | 0.833 | 2.746 | 3.448 |
| 4, 512KB | 81.06 | 86.78 | 71.29 | 136.18 | 1.943 | 3.229 | 4.031 |

N=100,000 and N=1,000 tables show the same pattern (full CSV in the untracked `sweep2_results.csv`):
no cell is a consistent winner across N values or across the primary ops (create/iterate1/iterate2/
random_get) — e.g. at N=1e6 the shipped `(2, 512KB)` cell has the *worst* create time of the six but a
mid-pack random_get; at N=1e5 it has the worst create (112.8) of the six; `(2, 256KB)` shows the best
iterate1 at N=1e6 (0.356) but the *worst* random_get (263.19) and iterate3 (4.18) at the same N. This
non-monotonic, metric-flipping pattern under the same ~70–84% background load documented above is the
signature of measurement noise, not a structural effect — a genuine sizing advantage should show up
consistently across N and across related ops, and none does here.

**Verdict: inconclusive, not a data-backed case to change the NSDMIs.** No cell beat the shipped
`growDivisor=2, maxChunkBytes=512KB` defaults consistently and meaningfully (>5% on the primary ops)
across all three N values — but given the noise level, this also isn't strong *positive* confirmation
that divisor=2/512KB is uniquely optimal, just that nothing in this dataset contradicts it. **NSDMIs
left unchanged, as instructed.** A re-sweep on an idle machine is recommended for a high-confidence
lock-in before this question is considered closed.

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

### Overall Phase-2 verdict

- **Robust, noise-resistant wins:** create (decisively ahead of flecs, 7/7 non-overlapping rounds),
  add (7/7 wins vs flecs, no regression), remove (no regression — matches the known post-Phase-C gap),
  and the memory-waste invariant (4× smaller footprint for small archetypes, directly measured).
- **Open question, not resolved by this session:** iterate1/iterate2/random_get could not be confidently
  shown at-or-ahead of flecs under this session's ~70–84% background load — spreads fully overlap and
  paired win-rates are near coin-flip (iterate1/2) or lean the wrong way (random_get). This is the one
  piece of the study's prediction (+51%/+15%/+16%) that needs a clean-machine re-run to settle either way.
- **Divisor/cap sweep:** inconclusive under the same noise; no evidence to change the shipped
  `growDivisor=2, maxChunkBytes=512KB` NSDMIs, and no evidence contradicting them either.
- **No library defect found.** All test-suite failures were the pre-existing `CompressionTest` timing
  flake, confirmed via isolated reruns. This task changed no library code (only the untracked benchmark
  scratch harness).

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`.
