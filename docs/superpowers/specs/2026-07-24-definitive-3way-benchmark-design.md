# Definitive 3-Way Benchmark Suite (Astra / flecs / EnTT)

**Date:** 2026-07-24
**Baseline:** dev @ `8878112` (post-Lever-2, post-growDivisor-closure)
**Status:** Design approved by user (op matrix + methodology + process, with bench sources
remaining untracked scratch). Supersedes the 7-op head-to-head as the scoreboard of record
once the campaign lands in RESULTS.md.

## 1. Goal

Mirror Astra's shipped benchmark surface (`benchmark/Benchmark.cpp`, 36 cases) into the
3-way `bench-compare/` harness wherever a fair cross-library idiom exists, run one
campaign at large scale on this machine under the proven methodology, and produce a
definitive scoreboard identifying any remaining comparative tuning targets.

Context that motivated it: the per-lever program closed every stable structural gap
(create 1.79x ahead, add ahead, remove 23-28% ahead, random_get ~parity) and the
iteration-lever investigation ended with both candidate mechanisms disproven (vectorization
— NEITHER lib vectorizes its iterate loop; entity-argument stream — twice disproven, spike
2026-07-24: traffic-ratio prediction contradicted, iterate1 worsened while only noisy
iterate2/3 medians improved) and iterate2/3 measuring at parity-to-noise. The 7-op window
is exhausted; the question is whether the FULL surface hides anything.

## 2. Op matrix

### Tier A — 3-way (N = 1,000,000; iteration family additionally at N = 10,000,000)

| # | op (CSV name) | Astra idiom | flecs idiom | EnTT idiom | fairness caveat |
|---|---|---|---|---|---|
| 1 | create | per-entity `CreateEntityWith(P,V)` (existing) | `e.set(P).set(V)` (existing) | `create`+`emplace` (existing) | — |
| 2 | create_batch | batch creation API (`CreateEntitiesWith`-family; exact call verified at impl) | `bulk_new<P,V>(N)` | `create(begin,end)` + `insert` per component | — |
| 3 | add_component | existing | existing | existing | — |
| 4 | remove_component | existing | existing | existing | — |
| 5 | add_batch | `AddComponents(span, T)` | `defer_begin(); loop e.add; defer_end()` | `insert(begin,end)` | flecs has no batch API — deferred loop is its native bulk idiom (disclosed) |
| 6 | remove_batch | `RemoveComponents(span)` | deferred loop | `remove(begin,end)` | same flecs caveat |
| 7 | destroy | per-entity `DestroyEntity` | `e.destruct()` | `registry.destroy(e)` | new op (absent from Astra's own suite; obvious gap) |
| 8 | iterate1 / iterate2 / iterate3 | existing | existing | existing (view; + group for 2) | — |
| 9 | iterate5 | `ForEach<P,V,H,Mana,Damage>` | 5-term query `each` | 5-component view `each` | uses the 5 existing bench_common types; no new types |
| 10 | iterate2_half | population: every entity gets V, every SECOND gets P (`i % 2`) — Astra-suite shape; iterate `<P,V>`; items = N/2 | same population/query | same | archetype models table-skip; EnTT pool-intersects — that difference IS the measurement |
| 11 | iterate2_one | every entity V, ONLY entity N/2 gets P; iterate `<P,V>`; items = 1 | same | same | query/dispatch overhead floor |
| 12 | random_get | shuffled `GetComponent<P>` (existing) | existing | existing | — |
| 13 | get_multi | shuffled per entity `Get<P>` + `Get<V>` (2 gets, Astra-suite shape); items = 2N | `get<P>`/`get<V>` | `get<P>`/`get<V>` | — |

### Tier B — 2-way Astra/flecs (EnTT only where an honest hand-rolled entry exists)

| op (CSV name) | Astra idiom | flecs idiom | scale | caveat |
|---|---|---|---|---|
| relations_children | build parent set + `ForEachChild` | `ChildOf` pairs + children iteration | 100K nodes | both first-class |
| relations_descendants | `ForEachDescendant` (cached traversal) | recursive children walk (or cascade query — whichever is flecs's canonical deep-traversal idiom, chosen at impl and disclosed) | 100K nodes, branching tree | traversal strategies differ by design — disclosed, that difference IS the measurement |
| relations_ancestors | `ForEachAncestor` | `target(ChildOf)` walk-up loop | 100K | — |
| system_tick_seq | 3 registered systems (movement P+=V; damage H-=D; heal H+=Mana shapes), sequential `SystemScheduler` tick | 3 systems + pipeline `progress()` | 1M entities/tick | closest-idiom pairing; measures a full tick |
| parallel_iterate2 | `ParallelForEach<P,V>` | multithreaded system, `set_threads(hw_concurrency)` | 1M and 10M | EnTT entry = hand-rolled `std::for_each(std::execution::par)` over its view, CSV-named `parallel_iterate2_handrolled` |

Excluded, with reasons recorded: range-for family (Astra-internal API comparison, its
cross-lib equivalents are already the iterate ops); `ForEachLink` (no flecs analog chosen —
custom relation pairs exist but a fair shape needs its own design); SystemScheduler
Parallel/ManyIndependent/WithDependencies/CustomExecutor variants (scheduler-internal
shapes with no flecs mapping beyond system_tick_seq + parallel_iterate2);
ParallelForEachDescendant (compound of two Tier-B caveats).

## 3. Methodology

- **Flags:** the full-opt Dist-parity recipe (RESULTS.md 2026-07-24 baseline:
  `/std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus
  /EHsc /link /LTCG`; + `/DASTRA_BUILD_DIST` for Astra; flecs.c `/c /O2 /GL /W0 /DNDEBUG
  /arch:AVX /fp:fast`), all three libraries identical. PowerShell tool for `build_one.bat`
  (Bash `cmd /c` silently no-ops it).
- **Harness shape:** extend the three existing monolithic exes in place (approach A);
  Tier B ops compile into bench_astra/bench_flecs (EnTT's single hand-rolled entry into
  bench_entt). One exe per library preserves exe-level interleaving.
- **Campaign:** typeperf quiet gate (~<15-20% sustained), then 6 interleaved rounds
  (astra → flecs → entt per round), background. Per-op internal reps reuse
  `bench_common.hpp`'s `median_persistent` / `median_structural`. Report cross-round
  medians [min,max]; conclusions from paired same-session numbers ONLY.
- **Scale:** Tier A at 1M; iterate1/2/3/5 + parallel_iterate2 additionally at 10M
  (~450MB peak per exe, sequential — fine); relations at 100K nodes; system_tick at 1M.
  Estimated campaign wall-clock 45-75 min.
- **Threads:** parallel ops use `hardware_concurrency` for every lib; one warm-up tick
  excluded from measurement.

## 4. Fairness validation (load-bearing)

- **Items-processed cross-check:** every op emits its processed-items count as a CSV
  column; the analysis script HARD-FAILS if the three libraries' counts disagree for any
  3-way op (2-way ops compare Astra vs flecs). A wrong population or mis-matched query
  cannot silently masquerade as a perf delta.
- `volatile g_sink` DCE guards on every op (existing pattern).
- **Smoke run** of each exe (small N) before the campaign: counts + sane magnitudes.
- Per-op idiom disclosure lives in the CSV op name where it matters
  (`*_handrolled`, deferred-batch note in RESULTS.md).

## 5. Process & deliverables

1. This spec, committed.
2. Implementation plan (writing-plans), then dispatched subagents: `bench_common` +
   `bench_astra` first (defines the op protocol: CSV columns `lib,op,N,ns_per_op,items`),
   then bench_flecs and bench_entt mirrored against it (parallel dispatch — different
   files), each reviewed by the controller for idiom fairness. Bench code is scratch:
   no SDD ceremony, but every idiom gets eyeballed against the lib's docs/headers.
3. **Bench sources remain UNTRACKED scratch (user decision)** — the spec + RESULTS.md
   carry the durable methodology; the reproduce section continues to describe the files.
4. Campaign → analysis script (items validation + medians tables + scoreboard) →
   RESULTS.md **"Definitive 3-way scoreboard"** section: full tables, per-family verdicts,
   and an explicit "remaining comparative tuning targets" list (possibly empty) →
   [[astra-perf-optimization]] memory update.

## 6. Risks / honesty notes

- Tier B idiom pairings are closest-available, not identical machinery — every such op
  ships with its caveat attached to the number; per-family verdicts, not a single blended
  score.
- flecs deferred-batch and EnTT hand-rolled-parallel entries measure "what a user of that
  library would write", which is the honest cross-library question, but they are NOT the
  same code shape — disclosed inline.
- The iterate ops' session instability on this machine (±20-48% pair swings documented
  2026-07-24) means iteration verdicts require the [min,max] bands, not medians alone;
  a gap claim needs non-overlapping bands, same standard as prior phases.
- Astra's `benchmark/Benchmark.cpp` cases not mirrored here (range-for, scheduler
  variants, ForEachLink, ParallelForEachDescendant) are LISTED as excluded with reasons —
  the "how many can we implement" question gets an explicit answer: **19 of the 36 shipped
  cases mapped** (6 creation/structural + 6 iteration shapes + 1 of 6 parallel + 2 gets +
  3 of 5 relations + 1 of 6 scheduler), **plus 1 new op (destroy)** the shipped suite
  itself lacks; 17 excluded as Astra-internal or unpairable.
