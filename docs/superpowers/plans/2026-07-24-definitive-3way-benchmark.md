# Definitive 3-Way Benchmark Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. NOTE: Tasks 2 and 3 touch disjoint files and MAY be dispatched in parallel (spec §5 explicitly authorizes this — it is bench scratch, not library code).

**Goal:** Extend `bench-compare/` to the full spec op matrix (15 Tier-A 3-way ops + 5 Tier-B 2-way ops), run one interleaved campaign at scale, and produce the Definitive 3-Way Scoreboard in RESULTS.md.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-24-definitive-3way-benchmark-design.md` (user-approved). Approach A: the three monolithic exes grow in place over shared `bench_common.hpp`; the CSV gains an items-processed column that the analysis hard-fails on if libraries disagree; Tier B compiles into astra+flecs (+ EnTT's one hand-rolled parallel entry).

**Tech Stack:** MSVC full-opt recipe, vendored `entt.hpp`/`flecs.h+flecs.c`, Python analysis, PowerShell campaign runner.

## Global Constraints

- **ALL bench-compare sources are UNTRACKED SCRATCH — never `git add` anything under `bench-compare/` except `RESULTS.md`** (user decision, spec §5.3). Work happens on dev directly (no branch — only RESULTS.md gets committed, in Task 4).
- Build recipe (per exe, PowerShell tool — the Bash tool's `cmd /c` silently NO-OPS `build_one.bat`):
  `cmd /c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /GL /DNDEBUG [/DASTRA_BUILD_DIST astra only] /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /nologo [includes] <file>.cpp [libs] /link /LTCG'`
  Astra: `/I..\include /I..\vendor\Mosaic\include` + `advapi32.lib`. flecs: link `flecs.obj ws2_32.lib` (flecs.obj already built with matching flags — do NOT rebuild it). EnTT: header-only.
- CSV protocol (NEW — every op, every lib): `lib,op,N,ns_per_op,items` where `items` = total logical items processed per measured pass (spec §4). The old `Mops` column is REPLACED by `items`. Every EXISTING op's call site must be updated to pass its items count (create/add/remove/destroy/random_get: N; get_multi: 2N; iterate1/2/3/5: N; iterate2_half: N/2; iterate2_one: 1; batch ops: N; relations: nodes visited; system_tick: N×systems; parallel: N).
- Population shapes are REQUIREMENTS (Astra-suite fidelity, spec §2): iterate2_half = every entity gets the second component, every entity with `i % 2 == 1` also gets the first; iterate2_one = only entity at index `N/2` gets the first; get_multi = 2 gets (P then V) per shuffled entity.
- Component types: ONLY the five in `bench_common.hpp` (Position/Velocity/Health/Mana/Damage). No new types.
- Scales: Tier A at N=1,000,000; iterate1/2/3/5 and parallel_iterate2 ALSO at N=10,000,000 (internal reps at 10M: 3, to bound runtime); relations at 100,000 nodes; system_tick_seq at 1M entities per tick.
- Timing: reuse `bench_common.hpp`'s `median_persistent` / `median_structural` helpers and `g_sink` DCE guard — match their EXISTING signatures (read the header first; do not redesign them). Persistent ops: median-of-7 (3 at 10M). Structural ops: median-of-3 with rebuild.
- Parallel ops: `std::thread::hardware_concurrency()` threads for every lib; one unmeasured warm-up pass; EnTT's entry is CSV-named `parallel_iterate2_handrolled`.
- Machine etiquette: typeperf quiet check before any measurement; do NOT close user applications; noisy runs get labeled, never silently used.
- Smoke gate (per exe, before Task 4's campaign): run with small N (the exes take `N` fixed internally today — smoke = a temporary small-N constant or just run as-is and check the CSV shape; verify items values match the table above exactly).
- Model recipe (SDD): sonnet for all four tasks; controller reviews each diff for IDIOM FAIRNESS (the review lens: does each lib do the same logical work?).

---

### Task 1: Protocol + bench_astra full op set

**Files:**
- Modify: `bench-compare/bench_common.hpp` (report signature: `Mops` → `items`)
- Modify: `bench-compare/bench_astra.cpp` (all new ops, Tier A + Tier B)

**Interfaces:**
- Consumes: existing helpers in `bench_common.hpp` (read it first).
- Produces: THE op protocol — exact CSV op names, population shapes, items counts. Tasks 2/3 mirror bench_astra op-for-op; they will read your final bench_astra.cpp as the reference. Op names (exact): `create, create_batch, add_component, remove_component, add_batch, remove_batch, destroy, iterate1, iterate2, iterate3, iterate5, iterate2_half, iterate2_one, random_get, get_multi, relations_children, relations_descendants, relations_ancestors, system_tick_seq, parallel_iterate2`.

- [ ] **Step 1: Read `bench_common.hpp` + `bench_astra.cpp` fully; update `report(...)` to emit `lib,op,N,ns_per_op,items` (items replaces Mops); update every existing bench_astra call site with its items count per the Global Constraints table.**

- [ ] **Step 2: Add the new Tier-A ops to bench_astra** (follow the file's existing State/report/g_sink patterns exactly; exact Astra calls):

- `create_batch`: `std::vector<Astra::Entity> out(N); reg->CreateEntitiesWith(N, std::span{out}, [&](size_t i){ return std::tuple{Position{(float)i,0,0}, Velocity{1,1,1}}; });` — verify the generator's exact contract against `Registry.hpp:184` and mirror what the existing test-suite/batch callers pass; measured with `median_structural` (fresh registry per rep); items=N.
- `add_batch` / `remove_batch`: build Pos+Vel population, then `reg->AddComponents(std::span{ents}, Health{1.0f})` / `reg->RemoveComponents<Health>(std::span{ents})` (exact signatures at `Registry.hpp:327/400`); `median_structural`; items=N.
- `destroy`: build Pos+Vel population; measure `for (e : ents) reg->DestroyEntity(e);`; `median_structural`; items=N.
- `iterate5`: all N entities get all five components; `CreateView<Position,Velocity,Health,Mana,Damage>()`; ForEach sums `p.x += v.x; h.value += m.value + d.value;`; g_sink one field; items=N; run at 1M and 10M.
- `iterate2_half` / `iterate2_one`: populations per Global Constraints; view `<Position,Velocity>`; body `p.x += v.x;` + a `matched` counter the FIRST rep only (assert matched==N/2 / ==1, then stop counting — the counter must not stay in the measured loop); items = N/2 / 1; 1M only.
- `get_multi`: shuffled indices (reuse the file's `shuffled_indices`); per entity `Get<Position>` then `Get<Velocity>`, accumulate both `->x`; items=2N.
- 10M variants: same ops, `N2 = 10'000'000`, reps=3, emitted with the same op name (N column distinguishes).

- [ ] **Step 3: Add Tier B to bench_astra:**

- `relations_children/descendants/ancestors` (100K nodes): build a tree — 1 root, branching factor 10, filling 100K nodes breadth-first via `reg->SetParent(child, parent)`; every node carries Position. children: for a mid-tree parent set (all depth-1 nodes), `reg->ForEachChild(parent, ...)` summing child Position.x — items = total children visited (count on first rep, assert stable). descendants: `reg->ForEachDescendant(root, ...)` — items = 100K-1. ancestors: for every leaf, `reg->ForEachAncestor(leaf, ...)` — items = leaves × depth (count first rep). Mirror the exact ForEach* signatures at `Registry.hpp:1312+` (they take Component-typed lambdas — check whether `<Position>` needs explicit template args and match the shipped suite's usage at `benchmark/Benchmark.cpp:892-1054`).
- `system_tick_seq` (1M): 3 lambda systems via `scheduler.AddSystem(...)` mirroring the shipped suite's `BM_SystemScheduler_Sequential`/`_Lambda` registration idiom (`benchmark/Benchmark.cpp:1121-1186` — copy its setup shape, adapted to bench components: movement `p.x+=v.x`, damage `h.value-=d.value`, heal `h.value+=m.value`); measure one `scheduler.Execute(reg)` per rep, median-of-7; items=3N.
- `parallel_iterate2` (1M + 10M): `view.ParallelForEach(...)` with body `p.x += v.x;` — mirror the shipped suite's `BM_ParallelIterateTwoComponents` (`benchmark/Benchmark.cpp:672-700`) for the exact call shape incl. any scheduler/thread setup it does; one warm-up pass unmeasured; items=N.

- [ ] **Step 4: Build (full-opt recipe) + smoke: run the exe once, verify every op emits one CSV line per (op,N) with the EXACT items values from the Global Constraints table. Fix mismatches — an items bug here propagates to two more files.**

No commit (untracked scratch). Report the final op list + items table + any API-contract surprises.

---

### Task 2: bench_flecs mirror

**Files:**
- Modify: `bench-compare/bench_flecs.cpp`

**Interfaces:**
- Consumes: Task 1's final `bench_astra.cpp` (THE reference for op names, shapes, items) + `bench_common.hpp` protocol + vendored `vendor/flecs.h` (verify every idiom against it, not from memory).
- Produces: flecs side of every Tier-A op + all 5 Tier-B ops.

- [ ] **Step 1: Update existing call sites to the items protocol.**
- [ ] **Step 2: Tier A** — `create_batch`: `world.entity_bulk`/`bulk_new<Position,Velocity>(N)` (find the vendored C++ API; if only C `ecs_bulk_new` exists, use it directly — disclose which); `add_batch`/`remove_batch`: `world.defer_begin(); for(e:ents) e.add<Health>()  /  e.remove<Health>(); world.defer_end();` (the spec-disclosed deferred idiom); `destroy`: `e.destruct()`; `iterate5`, `iterate2_half`, `iterate2_one`, `get_multi`: same populations/bodies/items as bench_astra, flecs query idioms (`world.query<...>()`, `e.get<T>()`/`get_mut` — match the existing file's get usage).
- [ ] **Step 3: Tier B** — relations: `child.child_of(parent)` (ChildOf pairs); children = `parent.children([&](flecs::entity c){...})`; descendants = recursive children walk (or cascade query if the vendored version's docs make that canonical — disclose the choice in a comment + your report); ancestors = `e.target(flecs::ChildOf)` walk-up loop per leaf. system_tick_seq: 3 systems (`world.system<Position,const Velocity>().each(...)` etc.) + `world.progress()` per rep. parallel_iterate2: `world.set_threads(hw)` + the iterate system marked `.multi_threaded()` and measured via `world.progress()` — one warm-up.
- [ ] **Step 4: Build + smoke — items must EQUAL bench_astra's per op** (that is the fairness contract; a flecs cascade matching a different node set than Astra's traversal MUST be reconciled, not papered over — if a shape can't be made to visit the identical set, STOP and report).

No commit. Report idiom choices (esp. bulk/deferred/descendants) + items parity confirmation.

---

### Task 3: bench_entt mirror (Tier A + one hand-rolled parallel)

**Files:**
- Modify: `bench-compare/bench_entt.cpp`

**Interfaces:**
- Consumes: Task 1's final `bench_astra.cpp` (reference) + `bench_common.hpp` + vendored `vendor/entt.hpp`.
- Produces: EnTT side of every Tier-A op + `parallel_iterate2_handrolled`. NO relations/scheduler ops (spec: EnTT lacks them).

- [ ] **Step 1: items protocol on existing sites.**
- [ ] **Step 2: Tier A** — `create_batch`: `reg.create(ents.begin(), ents.end()); reg.insert<Position>(ents.begin(), ents.end()); reg.insert<Velocity>(...)` (+ per-element values where insert supports a value range — verify vendored signature; uniform value is acceptable, disclose); `add_batch`: `reg.insert<Health>(ents.begin(), ents.end(), Health{1.0f})`; `remove_batch`: `reg.remove<Health>(ents.begin(), ents.end())`; `destroy`: `reg.destroy(e)` per entity; `iterate5`: 5-component `view(...).each(...)`; `iterate2_half`/`iterate2_one`: same populations, `view<Position,Velocity>.each` (this measures EnTT's pool-intersection cost — that asymmetry IS the measurement, spec §2); `get_multi`: `reg.get<Position>(e)` + `reg.get<Velocity>(e)` shuffled.
- [ ] **Step 3:** `parallel_iterate2_handrolled` (1M + 10M): collect the view's entities into a vector once (setup, unmeasured), then measured `std::for_each(std::execution::par, ...)` doing `reg.get<Position>(e).x += reg.get<Velocity>(e).x`... STOP — that per-element double `get` is NOT the same work as the others' direct array walk. Instead: partition the view's entity vector into `hw` contiguous chunks and run one thread per chunk doing `each`-equivalent direct gets via `view.get<Position>(e)`/`view.get<Velocity>(e)` (view-cached pools). This is the honest "what an EnTT user hand-rolls"; keep the `_handrolled` CSV name and a comment stating the extra per-element pool lookup vs the others' column walks.
- [ ] **Step 4: Build + smoke — items parity with bench_astra on every Tier-A op.**

No commit. Report the parallel implementation shape + any insert-signature adaptations.

---

### Task 4: Campaign + analysis + Definitive Scoreboard

**Files:**
- Create: `bench-compare/analyze_definitive.py` (untracked)
- Modify: `bench-compare/RESULTS.md` (the ONLY committed file)

**Interfaces:**
- Consumes: the three smoke-validated exes; known prior baselines (RESULTS.md full-opt + Lever-2 + spike sections).
- Produces: the Definitive 3-Way Scoreboard + tuning-target verdict; commit `perf(bench): definitive 3-way scoreboard - full Astra-suite-mapped op matrix at scale`.

- [ ] **Step 1: Cross-lib smoke validation** — run all three exes once, feed CSVs to the analysis script's items check: HARD-FAIL on any 3-way op where items differ across libs (2-way ops: astra vs flecs). Fix (via the owning file) before proceeding.
- [ ] **Step 2: Campaign** — typeperf quiet gate (<~15-20% sustained; if noisy, wait/label); then 6 interleaved rounds `astra → flecs → entt`, each round's stdout appended with a `roundN,` prefix to `definitive_campaign.csv`; background; expect 45-75 min.
- [ ] **Step 3: `analyze_definitive.py`** — parse the campaign CSV; re-run the items hard-fail across ALL rounds; per (lib, op, N): median [min,max] of ns_per_op; emit (a) the full table, (b) per-op Astra-vs-flecs and Astra-vs-EnTT ratio with a NON-OVERLAPPING-BANDS flag (a gap claim requires disjoint [min,max] bands — spec §6), (c) the tuning-target list: every op where Astra is behind with non-overlapping bands.
- [ ] **Step 4: RESULTS.md "Definitive 3-way scoreboard (2026-07-24)"** — campaign conditions (load evidence, flags, rounds), Tier-A table, Tier-B table with caveats inline, per-family verdicts, the explicit tuning-target list (possibly "none"), the 19-of-36+destroy mapping statement, and the excluded-ops list with reasons. Commit RESULTS.md ONLY.

---

### Finishing

Controller reviews the scoreboard against the spec's honesty rules → present verdict + tuning-target list to the user → update `[[astra-perf-optimization]]` memory. No merge gate (dev-direct docs commit; bench sources untracked).
