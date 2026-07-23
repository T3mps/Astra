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

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`.
