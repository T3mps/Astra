# Lever 2 — Remove-Path Redundancy Elimination (validate-once seam + move-path micro-work)

**Date:** 2026-07-24
**Baseline:** dev @ `390fb10` (post-Lever-1 merge, post full-opt-flag re-baseline)
**Branch:** `perf/lever2-remove-path` off dev @ `390fb10`. Local only — never push.
**Status:** Design approved by user (4 sections, 2026-07-24). All source anchors verified in-session against `390fb10`.

## 1. Context and evidence

Under the full-opt bench baseline (Dist-parity flags, `bench-compare/RESULTS.md` 2026-07-24 section),
Astra vs flecs at N=1M: create 49.9 vs 89.5 (ahead 1.79x), add 48.6 vs 51.0 (ahead), remove **37.4 vs
33.8 (~10% behind, ~3.5 ns/op absolute)**, random_get near-parity, iterate3 parity, iterate2 ~18%
behind. Remove is the last structural-op gap.

### 1.1 Why not iteration (investigated first, pivoted by user decision)

Lever 2 was originally scoped at iterate1/2. The evidence-gathering pass killed both candidate causes:

- **Auto-vectorization: neither library vectorizes its iteration inner loop.** `/Qvec-report:2` under
  the shipping flags including `/GL /LTCG`: Astra's `Archetype::ForEachImpl` loop
  (`Archetype.hpp:1487`) fails with reasons 1501 (possible aliasing on arrays-of-structs), 1200
  (unprovable loop-carried dependence), 1300 (too little computation). flecs's `each` invoker loop
  (`flecs.h:31720`) fails with 1305 (not enough type information). flecs's iterate2 lead is
  scalar-vs-scalar. `__restrict` on Astra's component arrays is REJECTED as a fix: a user lambda
  calling `GetComponent` obtains pointers into the same arrays — restrict would miscompile that legal
  pattern. An explicit vectorizable span/chunk API could leapfrog both libraries later (ergonomics-pass
  territory, out of scope here).
- **Per-chunk setup flattening does not survive arithmetic.** At N=1M with 512KB chunks, iterate2's
  archetype is ~62 chunks of ~16K entities; per-chunk tuple setup + prefetch amortizes to noise, and
  16K-iteration runs are far past prefetcher warm-up.
- Remaining iterate2 hypothesis (NOT disproven, NOT pursued now): the mandatory `Entity` argument
  stream — `ForEach` requires `(Entity, Comps&...)`, so the loop reads `entities[i]` every element
  (iterate2: 32B/entity moved vs flecs 24B = 1.33x traffic; observed gap 1.18x). The 2026-07-23
  entity-optional prototype measured ~noise, but on a noisy machine under the old SSE2-floor flags.
  Revisit when iteration is next scoped (doubles as the deferred Bevy-ergonomics feature).

User decision: pivot Lever 2 to the remove path.

### 1.2 Remove-path redundancy (all verified in source @ 390fb10)

Per `Registry::RemoveComponent<T>` call today (`Registry.hpp:309-324`):

1. `m_entityManager.IsValid(entity)` (`Registry.hpp:312`) — record read #1
   (`EntityManager.hpp:187-194` → `m_table.IsAlive`).
2. `GetComponent<T>(entity)` (`Registry.hpp:315`) — record read #2. Dual purpose: presence guard
   (`if (!component) return false`) + signal payload. The payload is needed only if the
   ComponentRemoved signal is enabled; the presence guard duplicates AM's mask test.
3. `SignalManager::Emit` (`Registry.hpp:321`) — already flag-gated internally
   (`Signal.hpp:202-210`: `if (IsEnabled<E>())`), so the emit itself is ~free when disabled — but
   step 2 already paid for its payload unconditionally.
4. `ArchetypeManager::RemoveComponent<T>` (`ArchetypeManager.hpp:339-360`) — record read #3
   (`GetRecord` + version + archetype at 343-345) + mask test (349) + edge (352) + move (353).
5. Inside `MoveEntityInternal` (`ArchetypeManager.hpp:1157-1186`): swap-moved entity's `GetRecord`
   (#4, unavoidable — different record) + two `SetRecordLocation` writes.

W1 made liveness and location one unified `EntityRecord`, so reads #1-#3 are three walks of the SAME
slot. Add (`Registry.hpp:281-293`) wastes only #1 (AM::AddComponent validates and returns the payload
pointer) — consistent with add leading flecs while remove lags.

Move internals (the Phase B targets):

- `ComponentDescriptor::Destruct` (`Component.hpp:155-159`) has NO triviality gate — unconditional
  indirect call through the `destruct` fn-ptr (a no-op stub for trivially-destructible types). Its
  siblings are gated: `MoveConstruct` → memcpy (`Component.hpp:143-153`), `DefaultConstruct` → memset
  (`Component.hpp:85-97`). The swap-remove backfill loop (`ArchetypeChunkPool.hpp:333-347`) calls
  `Destruct` twice per column (hole at :344, moved-from source at :346) ⇒ the bench's 3-column remove
  performs 6 wasted indirect calls per op.
- `Archetype::MoveEntityFrom` (`Archetype.hpp:514-567`) merge-join resolves
  `GetComponentPointer(id, idx)` per column via `idToColumn` although the join already holds the
  column ordinals (`a`/`b`), and Phase C guarantees chunk columns are packed in meta (ascending-id)
  order ⇒ 4 redundant id-resolutions per op.
- A transition walks src storage twice: merge-join copy-out (`MoveEntityFrom`), then
  `Chunk::RemoveEntity` (`ArchetypeChunkPool.hpp:319-364`) re-derives every column for the backfill.
- `Archetype::AllocateEntitySlot` (`Archetype.hpp:1531-1556`): `GetEntities().resize(count+1)`
  (:1542) value-initializes a slot that :1546 immediately overwrites, plus double bookkeeping with
  `SetCount`.

## 2. Goal and success criteria

Close the remove gap by eliminating the redundancy above, in two phases on one branch, each measured.

- **Target:** remove at or within noise of flecs (~34 ns) on the standard protocol (full-opt flags,
  quiet-machine check, 6 interleaved rounds astra→flecs→entt, medians). A prediction, not a promise —
  misses are reported honestly.
- **No regression elsewhere:** add keeps/widens its lead; create / random_get / iterate / destroy
  within noise. Shared code alert: create uses `AllocateEntitySlot` (B4), destroy + batch removes +
  CompactChunks use `Chunk::RemoveEntity` / `Destruct` (B1, B2) — the bench covers create/add/remove
  explicitly; destroy has no bench op, so its correctness rides on the suite and its perf on B1 being
  strictly-less-work.
- **3-config green** (Debug/Release/Dist) at every checkpoint; final authoritative run on the last
  commit. Baseline counts: Debug 732 / Release 730 / Dist 730.
- Rider: Lever 1's deferred Minor — typed `ArchetypeManager::GetComponent` null-checks `rec->chunk`
  (matches the Registry ByID sites' guard; see Lever 1 final review Minor #1).

**Non-goals:** batch APIs (`AddComponents`/`RemoveComponents` spans) beyond incidental fallout;
iteration work (see §1.1); any public API change; any signal-CONTRACT change (what is emitted, when,
with what payload — unchanged; only when the payload work happens changes); on-disk format (untouched).

## 3. Phase A — validate-once + signal-gated payloads

Principle: `ArchetypeManager` entry points are the single validation authority (they already do
`GetRecord` + version + archetype + mask). Registry stops pre-validating; signal payload work hides
behind a hoisted `IsSignalEnabled` check.

Sites (all in `include/Astra/Registry/Registry.hpp`):

| Site | Today | After |
|---|---|---|
| `RemoveComponent<T>` (:309-324) | IsValid + GetComponent guard/payload + Emit + AM call | Two-armed on `IsSignalEnabled(ComponentRemoved)`. **Disabled arm:** `return m_archetypeManager->RemoveComponent<T>(entity);` — one lookup total. **Enabled arm:** guarded `GetComponent` fetch (null ⇒ false; its validated-record path subsumes IsValid, which is dropped on BOTH arms), emit before migration, then the AM call — observable behavior identical to today per §3.1. |
| `AddComponent<T>` (:281-293) | IsValid + AM call + `if (ptr) Emit` | Drop IsValid; rest unchanged (AM validates; nullptr ⇒ no emit ⇒ identical void return). |
| `EmplaceComponent<T>` (:295-307) | same as AddComponent | same transformation |
| `AddComponentByID` (:478-~510) | AM call, then signal block recomputes record + chunk payload unconditionally (record->chunk at ~:495) | Hoist `IsSignalEnabled(ComponentAdded)` around the whole payload block. The post-AM record re-fetch inside the block STAYS (entity migrated; payload legitimately needs the new location) — it just becomes conditional. |
| `RemoveComponentByID` (:540-578) | pre-AM signal block (record->chunk at ~:555) unconditional | Hoist `IsSignalEnabled(ComponentRemoved)` around the block; emit-before-removal order preserved on the enabled arm. |

**Deliberate YAGNI decision:** the enabled arms keep their plain (second) lookup — no new
record-passing AM overload. Signals-enabled is the cold path; a narrower seam beats a faster cold path.

### 3.1 Behavior table (must hold exactly — the Phase A test targets)

| Case | Today | After |
|---|---|---|
| remove, invalid entity, signal off | false (IsValid) | false (AM version check) |
| remove, invalid entity, signal on | false (IsValid) | false (guarded fetch fails → false) |
| remove, absent component, off | false (GetComponent null) | false (AM mask test) |
| remove, absent component, on | false, no emit | false, no emit |
| remove, present, off | true, no emit | true, no emit (arm skipped) |
| remove, present, on | true, emit BEFORE removal, pointer valid at emit time | identical |
| add, invalid entity | void return, no add, no emit | identical (AM returns nullptr) |
| add, success, off | added, Emit's internal gate declines | added, no emit (same observable) |
| add, success, on | added, emit with new pointer | identical |
| ByID variants | as above per direction | as above; payload block only runs when enabled |

### 3.2 Phase A verify-items (confirm at plan time, before coding)

- `AM::AddComponent<T>`'s guard shape: returns nullptr for invalid entity AND for already-present
  component the same way the current Registry pre-checks would (audit its early-outs).
- `IsSignalEnabled` naming/flags: `Signal.hpp:262-265` (`IsSignalEnabled(Signal)`) vs templated
  `IsEnabled<E>()` (:236-240) — pick the form matching Events::ComponentAdded/Removed flags.
- `SetComponent` / any other per-entity structural Registry path: audit for the same pattern; if
  found, list-and-decide in the plan (scope stays the 5 sites unless trivially identical).
- Batch paths (`AddComponents` :326+, `RemoveComponents` :400-441): NOT in scope; confirm the sweep
  does not accidentally change them.

## 4. Phase B — move-path micro-work

Order: B1 → B3 → B4 (safe tranche, one checkpoint), then B2 (invasive, own checkpoint, user-gated).

### B1 — `Destruct` triviality gate (`Component.hpp:155-159`)

Add the missing gate, mirroring `MoveConstruct`/`DefaultConstruct`:
trivially-destructible (and size>0) ⇒ return without the indirect call.

- Verify-item: does `ComponentDescriptor` already carry `is_trivially_destructible`? If not, add the
  field and populate it in the descriptor factory alongside its siblings; audit hand-built descriptors
  (reflection/serialization construct some) for the new flag's correctness — the contract stays "flag
  true ⇒ destruct ptr may be null; flag false ⇒ destruct ptr must be valid" (same class of contract
  as `is_trivially_default_constructible`, see the Theme-E-era memory note).
- Beneficiaries: swap-remove backfill (2 calls/column), tail-remove destructs
  (`ArchetypeChunkPool.hpp:352-356`), entity destroy, batch removes, CompactChunks.
- Risk: near-zero. Non-trivial components take the fn-ptr path exactly as today (`Tracked` lifetime
  tests unaffected and load-bearing).

### B3 — merge-join direct column addressing (`Archetype.hpp:541-566`)

Inside `MoveEntityFrom`'s merge-join, replace `GetComponentPointer(dId, idx)` (id → `idToColumn` →
column) with direct ordinal addressing: `chunk.columns[ordinal].base + idx * stride`, using the join
ordinals `a` (dst) and `b` (src). Phase C invariant relied on: chunk columns are packed in the same
ascending-id order as `ArchetypeColumnMeta` (single creation path; verified during Phase C review).

- Verify-item: existing chunk accessor for column-by-ordinal (add a private/internal one if missing).
- Contained inside `MoveEntityFrom`; other `GetComponentPointer` callers untouched.

### B4 — `AllocateEntitySlot` append fix (`Archetype.hpp:1531-1556`)

Replace `GetEntities().resize(count+1)` + index write + `SetCount` with a direct append (single size
bump, no value-init of a slot that is immediately overwritten).

- Verify-item: `GetEntities()` container type and its API (push_back semantics, capacity guarantee —
  the chunk's entity array capacity equals chunk capacity, pre-reserved? confirm).
- Shared with create + every transition destination: free co-benefit; watch create in the bench.

### B2 — fused move-out + backfill (invasive; last; checkpoint-gated)

Today a transition remove walks src storage twice: (1) `MoveEntityFrom` merge-join copies the moving
entity's surviving columns src→dst; (2) `Chunk::RemoveEntity` walks ALL src columns deriving
base/stride again to backfill the hole from the last entity. Fuse into a single-pass primitive over
src columns (merge-joined against dst meta): per src column — copy-out to dst (matched) or destruct
the hole slot (dropped column); then backfill hole ← last (gated memcpy / MoveConstruct+Destruct);
entities-array fixup + count bump once.

- The primitive must preserve exactly: moved-entity reporting (`std::optional<Entity>` semantics —
  none when removing the tail), `m_firstNonFullChunkIndex` / `PopBackChunk` behavior
  (`Archetype.hpp:424-438`), dst-only default-construct branch, and every lifetime rule the Phase C
  `s_live` guards pin. `MoveEntityFrom` remains for its other callers.
- Signature/placement decided at plan time (likely `Archetype`-level, mirroring `MoveEntityFrom`'s
  two-chunk merge-join shape, returning the moved entity).
- **Gate:** lands only after the post-B1/B3/B4 bench checkpoint. If remove is already at/below flecs,
  the controller presents the evidence and the USER decides whether B2 still lands.

## 5. Testing

Existing nets (load-bearing, no action): Lever 1 chunk-invariant tests + Debug desync assert
(suite-wide record/chunk coherence sweep); Phase C move-only `Tracked::s_live` guards at the three
memcpy-gated sites; the 732-test Debug suite as the behavior net.

New tests:

- **Phase A — signal-contract tests** (audit existing signal tests first; add what's missing):
  enabled: ComponentRemoved fires before removal with value-asserted payload; ComponentAdded fires
  after with the new pointer; disabled: no emission; invalid-entity / absent-component return values
  per the §3.1 table, both arms, typed + ByID. Reuse `Astra::Test::*` — **register NO new component
  types (TypeID ceiling).**
- **B1 — lifetime test:** non-trivially-destructible component (`Tracked`) destructed on all three
  paths (hole slot, moved-from source, tail path) — likely partially covered; verify RED-ability by
  flag-flipping in a scratch build if a new test is added. Plus a descriptor-flag correctness check
  (static_assert or unit) for a known non-trivial type.
- **B2 — fused-primitive tests:** backfill value integrity (mirror existing swap tests), exact
  moved-entity reporting (tail vs non-tail), dropped-column destruct via `s_live`, dst-only
  default-construct branch.
- TDD where a red is honestly constructible; characterization-first where the change is
  behavior-preserving (disclosed in the plan, house style).

## 6. Process and measurement

- SDD (spec → plan → tasks): opus implements + reviews B2 and the final whole-branch review; sonnet
  elsewhere; reviewers scaled to diff.
- Checkpoints: 1. after Phase A, 2. after B1+B3+B4, 3. after B2 (if it lands). Every checkpoint =
  full Debug suite + bench; checkpoints 2 and 3 also run full 3-config; final authoritative 3-config
  on the last commit regardless.
- Bench protocol: full-opt flags (2026-07-24 baseline in RESULTS.md — `/arch:AVX /D__SSE2__
  /D__SSE4_2__ /fp:fast /GL /LTCG` etc.), typeperf quiet check, 6 interleaved rounds, medians +
  spreads, paired same-session comparison only. Append results to RESULTS.md.
- Finish: confirm with user → FF-merge dev local → delete branch → do NOT push → update
  [[astra-perf-optimization]] memory.

## 7. Risks

- **Phase A seam:** dropping Registry pre-checks changes which layer rejects an op. §3.1 is the
  contract; the desync assert + signal tests are the net. No concurrency dimension (single-writer
  mutation contract, unchanged).
- **B1:** wrong `is_trivially_destructible` on a hand-built descriptor would skip a real destructor —
  bounded by the verify-item audit + flag test; same contract class the codebase already carries for
  default-construct.
- **B2:** the highest-risk diff; mitigated by ordering (last), its own checkpoint, opus
  implementation/review, and the s_live/value-integrity test set. Its gate means it can be dropped
  with zero sunk cost in the rest of the lever.
- **Perf honesty:** ~3.5 ns/op is a thin target; session drift is documented — all conclusions from
  paired same-session medians only.
