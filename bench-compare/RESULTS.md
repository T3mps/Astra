# Astra vs EnTT vs flecs — same-machine head-to-head (2026-07-21, updated 2026-07-22 post-W6+W2)

**Setup:** one machine, identical hand-rolled harness (`bench_common.hpp`), identical component layout, 1,000,000 entities, MSVC `/std:c++20 /O2 /DNDEBUG /EHsc`, view/query/group created once (pure-iteration), median of runs. EnTT master, flecs v4.1.6, Astra @ `perf/w6-w2-archetype-edges` (post-W6 avalanche hash mix + W2 array-indexed archetype edges). Each library uses its idiomatic fast path. Numbers are **ns per entity/op** (lower = better); M/s in parens.

| Operation | Astra | EnTT | flecs | Rank |
|---|---|---|---|---|
| iterate 1 comp | 0.432 (2315) | 0.542 (1845) | **0.377 (2653)** | flecs > **Astra** > EnTT |
| iterate 2 comp | 1.222 (819) | 2.299 view / 1.083 group | **0.844 (1185)** | flecs > EnTT-group > **Astra** > EnTT-view |
| iterate 3 comp | 1.192 (839) | 3.192 view (313) | **1.159 (863)** | flecs > **Astra** > EnTT-view |
| create (2 comp) | 133.2 (7.51) | **39.4 (25.4)** | 95.1 (10.5) | EnTT > flecs > **Astra** |
| add component | 131.5 (7.60) | **11.5 (86.6)** | 54.6 (18.3) | EnTT > flecs > **Astra** |
| remove component | 86.4 (11.57) | **16.3 (61.3)** | 32.7 (30.6) | EnTT > flecs > **Astra** |
| random get | 63.8 (15.68) | **22.2 (45.0)** | 57.4 (17.4) | EnTT > flecs > **Astra** |

Astra numbers are the median of 6 same-session runs (see "Measurement methodology" in the W6+W2 section
below — this machine was noticeably noisier than the 2026-07-21 session, so EnTT/flecs were re-measured
here too, not reused, to keep the table internally consistent). All three libraries' absolute numbers moved
a few percent vs the 2026-07-21 session (background load, not code changes — EnTT/flecs are unmodified);
rankings are unchanged.

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

The perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`, Phase B) predicted **add
150→~90 ns and remove 220→~60 ns** (i.e. ~35-40% reductions) on the theory that the edge lookup was the
dominant cost. The measured reduction is smaller — **13.7%/18.4%**, not ~35%. The wiring was independently
verified correct in source (above), so this is not a leftover-path bug; it means the archetype-edge
`FlatMap` double-probe was a real but smaller share of add/remove's cost than the plan estimated. The
remaining cost is dominated by the transition **move** itself (per-element `MoveConstruct`/`Destruct` via
function pointer over the chunk's present components, `Archetype.hpp:1433` / `ArchetypeChunkPool.hpp:365-396`)
— unchanged by W2, and exactly what **W3** (trivial memcpy move) targets next.

Residual gap to flecs, post-W6+W2: add 131.5 vs flecs 54.6 (2.41×, down from 2.81× pre-W2), remove 86.4 vs
flecs 32.7 (2.64×, down from 3.28× pre-W2). Both gaps shrank but flecs is still meaningfully ahead — consistent
with the move-cost diagnosis above (W3/W4/W5, Phase C, is where that residual gap should close further).

## Honest verdict (updated post-W1)

- My original "Astra **can't out-perform** EnTT/flecs" was **wrong**: Astra beats EnTT's common `view` idiom on 2- and 3-component iteration (1.9×–2.8×) and beats EnTT on single-component.
- My mid-stream "Astra **dominates** iteration" (from the EnTT-only slice) was **also wrong**: **flecs — the direct archetype competitor — is still faster than Astra on all three iteration cases** (iterate2: flecs 0.82 vs Astra 1.17 = **1.4× faster**, narrower than pre-W1's 1.7× but iteration wasn't W1's target — this delta is noise, not a real gain).
- **W1 closed a large chunk of the structural-churn gap.** Pre-W1 Astra was a distant #3 on create/add/remove/get, 2.3×–6.4× behind flecs. Post-W1: create is now 1.4× behind flecs (was 3.5×), remove 3.3× behind (was 6.4×), random_get 1.2× behind (was 2.3×) — random_get in particular is now nearly at flecs parity. Add is still the biggest remaining gap (150.5 vs 53.5 ns, 2.8× — W1 wasn't targeted at add's dominant cost, the edge-lookup double-hash in `ArchetypeGraph`; that's W2's job).

**The real picture:** Astra is still #2 on iteration (ahead of EnTT-view, behind flecs, unchanged by W1 as expected) and now a much closer #3 on structural churn — no longer "distant." EnTT's sparse-set numbers remain out of reach for an archetype model (see plan §4, do-not-chase), but flecs parity (the actual target) is now within ~1.2×–3.3× depending on the op, down from ~2.3×–6.4×.

## Why this is good news, not bad

flecs uses the **same storage model** as Astra. Pre-W1 it was 1.7× faster on iteration and 3.5×–6.4× faster on structural ops — meaning Astra's deficits were **optimization gaps, not architectural limits**. W1 (the first of those fixes) already halved-to-thirded most of the structural gap, confirming the diagnosis. Remaining causes, per the perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`):
- **Iteration gap vs flecs:** W6 (hash mixing) landed but iteration was never W6's target and shows no change, as predicted; the remaining cause is per-chunk 23 KB `ComponentDescriptor`-by-value metadata bloat (cache pressure — W5) and possibly chunk-size/prefetch tuning — both Phase C.
- **Remaining structural gap (add especially):** W2 replaced the two pointer-keyed `FlatMap` probes with array-indexed edges and measurably helped (add 2.81×→2.41× behind flecs, remove 3.28×→2.64×), but the transition **move** cost (per-element ctor/dtor via fn-ptr, not memcpy) now dominates what's left — that's **W3**'s target.
- **random_get** is close to flecs (63.8 vs 57.4 ns, 1.11×) — W1 alone nearly closed this gap, exactly as the plan predicted (W1 "dominates random_get"); W2 doesn't touch this path, as expected (flat).

**Takeaway for the roadmap:** W1 validated the "optimization gaps, not architectural limits" diagnosis — a single surgical change (paged record, no API change) closed most of random_get and a meaningful slice of create/add/remove. **W6+W2 landed 2026-07-22** (avalanche hash mix + array-indexed archetype edges): a real but smaller-than-predicted add/remove win (13.7%/18.4%, see the W6+W2 section above), confirming the edge lookup was a genuine but not dominant cost. Next up per the plan's execution order: **Phase C** (chunk storage modernization: W5 → W4 → W3 → W7), starting with W3 (trivial memcpy move) since it's now the clearest remaining lever on add/remove.

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`.
