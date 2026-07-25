# Enableable Components — Design (2026-07-25)

**Status:** user-approved design (this session).

**Goal:** DOTS-style enable/disable for components — O(1) toggle with zero structural
change, enabled-only default query filtering with per-view opt-out, deterministic
iteration preserved regardless of toggle history, and **zero cost on every path that
does not use the feature** (the flecs-parity iteration and create/batch wins are not
negotiable).

**Evaluated alternative (recorded):** user-selectable sparse-set component storage
(EnTT/Bevy hybrid) was considered and declined for this need: it forks every
archetype-model invariant (unified EntityRecord, chunk iteration, serialization,
commands, reflection), its packed iteration order is mutation-history-dependent
(conflicts with the deterministic-ECS pillar), and the archetype-native industry answer
to toggle churn is bits (DOTS `IEnableableComponent`, flecs `flecs::Toggle`). Sparse
storage remains a possible future lever for the one niche bits don't serve (rarely
present FAT components); nothing here forecloses it.

## 1. Scope (user decisions, this session)

| Decision | Choice |
|---|---|
| Which components | **Opt-in per type** (trait); non-enableable types pay nothing |
| Entity-level `Disabled` | **Out of scope** (composable later as enableable tag + default filter) |
| Signals | **Gated `ComponentEnabled`/`ComponentDisabled` pair ships NOW** (off by default) |
| Threading | **Owner-thread immediate + deferred `SetEnabled` via CommandBuffer** (no atomic bits; deterministic flush order) |
| Storage approach | **A: disabled-bit words in chunk-owned memory + per-column disabledCount** (B archetype-level side BitSets rejected: no stable flat index under variable-capacity chunks; C sparse disabled-set rejected: per-entity hash probe in iteration) |
| Query default | **Enabled-only** for enableable required components; `IncludeDisabled<T>` per-view opt-out; disabled-only filtering out of scope |

## 2. Opt-in surface

- Detection (one detection point, two spellings): a component is enableable iff
  `T::AstraEnableable` is `true` (member `static constexpr bool`) OR
  `Astra::EnableableTraits<T>::value` is true (specialization escape hatch for types the
  user cannot edit). Evaluated once per type.
- `ComponentDescriptor` gains `bool isEnableable`, snapshotted at registration by
  `ComponentRegistry` from the trait. Per-archetype `ArchetypeColumnMeta` already holds
  the shared descriptor pointer (Phase C), so archetypes and chunks learn enableability
  with no new per-archetype state beyond what §3 adds.
- `SetEnabled<T>`/`IsEnabled<T>` on a non-enableable `T` is a compile error
  (`static_assert` with an actionable message naming the trait).
- **Compatibility caveat:** toggling a type's `AstraEnableable` opt-in between builds
  (adding or removing it) is a BREAKING on-disk format change that `BINARY_FORMAT_VERSION`
  does NOT capture -- the format doesn't self-describe per-column enableability, the
  reader infers it from its own registry's current trait snapshot, same class of hazard
  as changing a component's size.

## 3. Storage — disabled-bit words in chunk memory

- Per enableable column per chunk: `ceil(capacity/64)` × `uint64_t` disabled-bit words,
  allocated INSIDE the chunk's single allocation (locality, lifetime, serialization all
  ride the chunk), referenced from the packed per-chunk `Column` as
  `uint64_t* disabledWords` (nullptr for non-enableable columns). 8-byte alignment
  suffices (words); placement within the chunk layout is an implementation choice the
  plan pins.
- Per enableable column per chunk: `uint32_t disabledCount` (chunk capacity can exceed
  65535 at the 512KB ramp cap — uint16 would truncate).
- **Polarity: a SET bit means DISABLED.** Rationale (load-bearing): chunk memory is
  zeroed at chunk construction, so zero-init = all-enabled — new entities are born
  enabled with ZERO added writes on the create paths (the Lever-3 chunk-run batch create
  gains no instructions), and `disabledCount` starts at 0 to match.
- **Bit invariants:**
  1. `disabledCount == popcount(disabledWords[0..words))` at all times.
  2. Bits at slots `>= chunk count` are always 0 (swap-remove clears the vacated tail
     slot's bit; slot-claim paths never need to touch words).
  3. A component's disabled bit is state OF THAT ENTITY's component instance: preserved
     across swap-remove, archetype transition, compaction, defragment (§8).
- Existing primitives reused where they are shaped right: `Astra::Bitmap`
  (`Container/Bitmap.hpp`) for query-level ComponentMask work (§5); Mosaic `BitSet`'s
  `countr_zero` scan idiom (`vendor/Mosaic/include/Mosaic/BitSet.hpp`) generalized to
  run extraction for mixed-chunk iteration (§5). Mosaic `BitSet` instances themselves
  are NOT used for storage (heap-backed, no stable flat index across variable-capacity
  chunks).

## 4. API + behavior table

- `Registry::SetEnabled<T>(Entity, bool) -> bool` (true iff the entity is valid, has T,
  and the call was applied — including the idempotent same-state case);
  `Registry::IsEnabled<T>(Entity) const -> bool`;
  `SetEnabledByID(Entity, ComponentID, bool)` / `IsEnabledByID(Entity, ComponentID)` for
  type-erased consumers (commands, reflection).
- Behavior table (governs implementation and review):

| State | SetEnabled | IsEnabled |
|---|---|---|
| valid entity, has T, state differs | flips bit, updates count, fires gated signal, returns true | — |
| valid entity, has T, state same | no-op (no signal), returns true | current state |
| valid entity, no T | no-op, returns false | false |
| stale/invalid entity | no-op, returns false | false |
| T not enableable | compile error (typed API); ByID: no-op, returns false (nothing to toggle) | compile error (typed API); ByID: true if present (present + not disabled ⇒ enabled is the correct answer for a type-erased caller — only `SetEnabledByID` has "nothing to toggle" to refuse), false if absent |

- **Existence never lies:** `Has<T>` remains true and `GetComponent<T>` returns the data
  pointer while disabled (DOTS semantics; disabled ≠ removed). Only query filtering and
  `IsEnabled` observe the bit.
- Toggle cost: O(1) — EntityRecord already carries the direct chunk pointer + index
  (W1 + Lever 1); toggle = one validated record fetch, one word RMW, one count update.

## 5. Query semantics

- Default: **enabled-only** for every enableable component in the view's REQUIRED set.
  Per-view opt-out: `IncludeDisabled<T>` modifier in the view's type list disables
  filtering for T only. Disabled-only filtering: out of scope.
- Per matched archetype, the view computes ONCE (at view construction / archetype-match
  time, alongside existing per-archetype setup): the set of its filtered components that
  are enableable in this archetype (Bitmap/ComponentMask intersection minus
  `IncludeDisabled` types). **Empty set ⇒ the existing tight loop runs UNCHANGED — the
  filtering machinery is never entered.** This is the zero-cost path covering all
  current users and all benchmarks.
- Non-empty set, per chunk:
  - all relevant columns have `disabledCount == 0` → existing tight loop unchanged;
  - any relevant column fully disabled (`disabledCount == count`) → skip chunk;
  - mixed → combine the relevant columns' disabled words (`OR`), invert, mask the tail
    beyond `count`, and extract enabled runs via `countr_zero` scanning; feed each
    `[begin, end)` run to the existing loop body. Iteration order remains chunk order —
    **deterministic regardless of toggle history**.
- `Optional<T>` where T is enableable and not `IncludeDisabled`: reports
  null/absent while disabled (single rule: disabled is invisible to default queries).
- Enabled-aware `View::Size()` (and any default query count surface): exact — sums
  counts with the word-popcount walk in mixed chunks only.
- `ParallelForEach` chunking: unchanged work partitioning; each worker applies the same
  per-chunk filter locally (bits are read-only during iteration — toggling during
  iteration falls under the existing structural/mutation-during-iteration rules).

## 6. Signals

- New gated pair `Events::ComponentEnabled { Entity, ComponentID }` /
  `Events::ComponentDisabled { Entity, ComponentID }` + matching `Signal` enum entries.
  Off by default (house model). Fired ONLY on genuine state change (idempotent
  `SetEnabled` is silent), on both the immediate path and at deferred command apply
  (the executor calls the same Registry entry point — one signal site).
- Explicitly NOT reusing `ComponentAdded`/`ComponentRemoved` — existence does not change.

## 7. Commands (deferred toggling)

- `CommandType::SetEnabled`; POD payload `{ Entity entity; ComponentID componentId;
  uint8_t enable; }` (no inline data, no destructor thunk); recorded via
  `CommandBuffer::SetEnabled<T>(Entity, bool)` (static_asserts the trait) — worker-safe
  like every recording API, placeholder-entity resolution included.
- Executor calls `Registry::SetEnabledByID`; returns its bool. Failure (stale entity /
  missing component) surfaces exactly like other single-entity ops: whole-buffer
  failure semantics on eager `Execute()`, one attributed `DeferredCommandError`
  (`InvalidTargetEntity`) on the sorted flush.

## 8. Structural-op preservation invariants

Enabled state is per-instance and survives every entity move:
1. **Swap-remove** (`Archetype::RemoveEntity`): the moved (last) entity's bit is copied
   into the vacated slot, the tail slot's bit is cleared, both per §3 invariant 2;
   `disabledCount` adjusted iff the removed entity was disabled.
2. **Archetype transitions** (the three move sites: `Archetype::MoveEntityFrom`,
   `ArchetypeManager::MoveAndAdd`, `MoveAndAddByID`): for each enableable column present
   on BOTH sides, the bit carries; a freshly added component is born enabled; a removed
   component's bit disappears with the column. Counts updated on both chunks.
3. **Batch create** (chunk-run `AddEntitiesWith`): no bit writes (born enabled by
   zero-init) — the Lever-3 hot path is untouched.
4. **CompactChunks / Defragment**: bits and counts move with the entities they describe.
These are the same funnel/move sites Levers 2-3 hardened; the existing invariant-test
pattern (deterministic `Tracked`-style guards) extends to bits.

## 9. Scheduler / access model

- `SetEnabled` (immediate or deferred apply) = **write access on T** for scheduling
  purposes; `IsEnabled` and default filtering = read access on T. Conservative — a
  distinct "enabled-bits access unit" (togglers not serializing against T-data readers)
  is deferred to the auto-access-derivation work (Tier 2), where it can be derived
  rather than declared.

## 10. Serialization

- Chunk sections gain, per enableable column: the disabled words (`ceil(chunkEntityCount/64)`
  words, derived from the persisted entity count) and `disabledCount`. Binary format
  version bump. Per-chunk capacity is NEVER persisted, and a reload's
  `ChunkBytesToHold` may re-derive a load capacity `>=` the saved entity count (dynamic
  chunk sizing, Phase 2) -- entity count is the only basis both sides agree on, and the
  bits at slots `>= chunkEntityCount` are provably zero (§14 invariant 2), so writing
  more than `ceil(chunkEntityCount/64)` words would only ever store zeros.
- Legacy (pre-bump) saves load as all-enabled (absent section ⇒ words zero, count zero —
  the polarity pays again).
- Load validation (safety-first lane): refuse a chunk section where
  `disabledCount != popcount(words)` or any bit ≥ the chunk's entity count is set
  (same refuse-not-trust posture as the Theme-E / untrusted-load direction).

## 11. Performance stance + gate

- Invariant: paths not using the feature are byte-identical in behavior and flat in
  measurement. Create/batch-create gain zero instructions (polarity); queries with no
  enableable components in their filter never enter the machinery (§5 empty-set path);
  non-enableable columns allocate no words.
- **Bench gate:** one checkpoint at the end — definitive-harness flat-watch set
  (create, create_batch, add, remove, destroy, random_get, iterate1/2/3), 6 interleaved
  rounds, quiet gate, vs the Lever-3 scoreboard baselines. All must be flat
  (non-overlapping-bands rule for any claimed regression/win). Bench components remain
  non-enableable — this measures the zero-cost claim, which is the claim that matters.
- Informational (non-gating, optional): a toggle micro-timing and a mixed-chunk
  iteration probe may be recorded in the report but set no pass/fail bar this arc.

## 12. Testing plan

**TypeID budget: ZERO new component types planned.** Enableability is added to one or
two existing `Astra::Test` components — invisible to their other tests (everything
defaults enabled; only bit storage appears). A new type is added ONLY if implementation
review demonstrates cross-test contamination, disclosed as a deviation.

1. Trait detection: member spelling, specialization spelling, negative case
   (non-enableable → no words allocated; typed API static_asserts verified by
   compile-time `requires`-based checks where the suite's idiom allows).
2. Toggle behavior table (§4) — every row, including idempotent-no-signal and the gated
   signal pair firing exactly once per genuine change (both signal-enabled and default
   silent configurations).
3. Query filtering: default excludes disabled; `IncludeDisabled<T>` includes them;
   multi-enableable AND intersection in one query; `Optional<T>` null-while-disabled;
   enabled-aware `Size()` exactness.
4. Word-boundary run-scan: disabled patterns at slots 0, 63, 64, 65, full-word runs,
   alternating bits, all-disabled chunk skip, tail-mask correctness at
   `count % 64 != 0`.
5. Preservation: swap-remove bit carry + tail clear; archetype-transition carry
   (add/remove an unrelated component to a disabled-component holder); batch-create
   born-enabled; CompactChunks/Defragment preservation. Counts re-derivable
   (`disabledCount == popcount`) asserted via a debug/test seam after each operation
   family.
6. Determinism: identical query visit order under different toggle histories.
7. Serialization: round-trip with mixed bits; legacy-format load = all-enabled;
   corrupt count / out-of-range bit refused.
8. Commands: deferred `SetEnabled` applies in SortKey order; stale-target surfaces
   through the deferred-error channel; eager failure path.
9. Full 3-config suite green; bench gate per §11.

## 13. Out of scope (recorded)

Entity-level `Disabled` flag; disabled-only query filters; change-detection integration
(shares the per-chunk-metadata shape — deliberate synergy, separate arc);
sparse-set component storage; atomic/concurrent immediate toggles.

## 14. Binding invariants (for implementers and reviewers)

1. Zero cost when unused: no words for non-enableable columns; no filter machinery
   entered for queries whose enableable-intersection is empty; no added instructions on
   create/batch-create paths.
2. SET bit == DISABLED; zero-init == enabled; `disabledCount == popcount(words)`; bits
   beyond chunk count are zero. (Serialized word count is `ceil(chunkEntityCount/64)`,
   derived from the persisted entity count -- per-chunk capacity is never persisted, see
   §10.)
3. Enabled state preserved across swap-remove, transitions, compaction, defragment;
   fresh components born enabled.
4. `Has`/`GetComponent` ignore bits; only queries (default), `IsEnabled`, and `Size()`
   observe them.
5. Iteration visit order is independent of toggle history (chunk order always).
6. Signals fire only on genuine state change, through one site, gated off by default.
7. Toggling follows existing mutation threading rules; deferred toggles flush in
   deterministic SortKey order.
8. Serialization refuses count/popcount mismatches and out-of-range bits; legacy loads
   are all-enabled.
9. TypeID budget: zero new test component types (deviation requires disclosure).
