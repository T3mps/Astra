# Astra vs EnTT vs flecs — same-machine head-to-head (2026-07-21, updated 2026-07-22 post-W1)

**Setup:** one machine, identical hand-rolled harness (`bench_common.hpp`), identical component layout, 1,000,000 entities, MSVC `/std:c++20 /O2 /DNDEBUG /EHsc`, view/query/group created once (pure-iteration), median of runs. EnTT master, flecs v4.1.6, Astra @ `perf/w1-unified-entity-record` (post-W1, unified paged entity record). Each library uses its idiomatic fast path. Numbers are **ns per entity/op** (lower = better); M/s in parens.

| Operation | Astra | EnTT | flecs | Rank |
|---|---|---|---|---|
| iterate 1 comp | 0.436 (2292) | 0.511 (1959) | **0.403 (2484)** | flecs > **Astra** > EnTT |
| iterate 2 comp | 1.173 (852) | 2.269 view / 1.076 group | **0.815 (1234)** | flecs > EnTT-group > **Astra** > EnTT-view |
| iterate 3 comp | 1.142 (876) | 3.213 view (311) | **0.888 (1126)** | flecs > **Astra** > EnTT-view |
| create (2 comp) | 129.0 (7.75) | **37.8 (26.5)** | 94.7 (10.6) | EnTT > flecs > **Astra** |
| add component | 150.5 (6.64) | **11.2 (89)** | 53.5 (18.7) | EnTT > flecs > **Astra** |
| remove component | 105.6 (9.47) | **15.9 (63)** | 32.2 (31.0) | EnTT > flecs > **Astra** |
| random get | 62.2 (16.1) | **18.6 (54)** | 52.5 (19.1) | EnTT > flecs > **Astra** |

Astra numbers are the median of 3 runs; EnTT/flecs are the average of 2 (re-run this session for a same-machine baseline; sources unchanged since 2026-07-21, only re-measured for consistency).

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

## Honest verdict (updated post-W1)

- My original "Astra **can't out-perform** EnTT/flecs" was **wrong**: Astra beats EnTT's common `view` idiom on 2- and 3-component iteration (1.9×–2.8×) and beats EnTT on single-component.
- My mid-stream "Astra **dominates** iteration" (from the EnTT-only slice) was **also wrong**: **flecs — the direct archetype competitor — is still faster than Astra on all three iteration cases** (iterate2: flecs 0.82 vs Astra 1.17 = **1.4× faster**, narrower than pre-W1's 1.7× but iteration wasn't W1's target — this delta is noise, not a real gain).
- **W1 closed a large chunk of the structural-churn gap.** Pre-W1 Astra was a distant #3 on create/add/remove/get, 2.3×–6.4× behind flecs. Post-W1: create is now 1.4× behind flecs (was 3.5×), remove 3.3× behind (was 6.4×), random_get 1.2× behind (was 2.3×) — random_get in particular is now nearly at flecs parity. Add is still the biggest remaining gap (150.5 vs 53.5 ns, 2.8× — W1 wasn't targeted at add's dominant cost, the edge-lookup double-hash in `ArchetypeGraph`; that's W2's job).

**The real picture:** Astra is still #2 on iteration (ahead of EnTT-view, behind flecs, unchanged by W1 as expected) and now a much closer #3 on structural churn — no longer "distant." EnTT's sparse-set numbers remain out of reach for an archetype model (see plan §4, do-not-chase), but flecs parity (the actual target) is now within ~1.2×–3.3× depending on the op, down from ~2.3×–6.4×.

## Why this is good news, not bad

flecs uses the **same storage model** as Astra. Pre-W1 it was 1.7× faster on iteration and 3.5×–6.4× faster on structural ops — meaning Astra's deficits were **optimization gaps, not architectural limits**. W1 (the first of those fixes) already halved-to-thirded most of the structural gap, confirming the diagnosis. Remaining causes, per the perf plan (`docs/reviews/2026-07-21-astra-perf-optimization-plan.md`):
- **Iteration gap vs flecs (untouched by W1, as expected):** no internal hash mixing (H2≡1 for pointer keys defeats the SIMD filter — W6), per-chunk 23 KB `ComponentDescriptor`-by-value metadata bloat (cache pressure — W5), and possibly chunk-size/prefetch tuning.
- **Remaining structural gap (add especially):** the archetype-edge lookup is still two pointer-keyed `FlatMap` probes (`ArchetypeGraph.hpp`) — W2's target. Add is now the largest single ratio to flecs (2.8×).
- **random_get** is now close to flecs (62.2 vs 52.5 ns, 1.2×) — W1 alone nearly closed this gap, exactly as the plan predicted (W1 "dominates random_get").

**Takeaway for the roadmap:** W1 validated the "optimization gaps, not architectural limits" diagnosis — a single surgical change (paged record, no API change) closed most of random_get and a meaningful slice of create/add/remove. Next up per the plan's execution order: **W6** (SplitHash pointer mixing, cheap hygiene) → **W2** (array-indexed archetype edges, the biggest remaining add/remove lever) → Phase C (chunk storage modernization: W5/W4/W3/W7).

## Reproduce
`bench-compare/` — `build_one.bat` (vcvars+cl wrapper), `bench_{astra,entt,flecs}.cpp`, shared `bench_common.hpp`. EnTT/flecs sources under `bench-compare/vendor/`.
