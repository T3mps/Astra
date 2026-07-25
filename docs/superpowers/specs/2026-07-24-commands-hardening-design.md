# Commands Subsystem Hardening — Design (2026-07-24)

**Status:** user-approved design (this session). First Tier 0 Critical off the fresh
review-of-record (`docs/reviews/2026-07-21-astra-current-state-full-review.md`, Commands C1);
detailed findings in `docs/reviews/2026-07-21-fresh-review/rev-commands.md` (the punch list
this spec executes).

**Genesis note (honest history):** this began as a "reducer-model formalization" of the
Commands layer (the user likes the pattern). Scoping concluded the formalization itself was
ceremony — `CommandBuffer` already *is* the pattern (typed action stream, deterministic fold
at a sync point) — and the branch was reframed as what remained after removing the ceremony:
the full Commands hardening punch list. The reducer lens survives in two engineering
decisions it sharpened: the storage becomes physically append-only (§2), and the
deterministic `ExecuteSorted` becomes the *only* parallel fold (§3.2). A user-facing
reducer/store layer remains a candidate for the future ergonomics pass ("Tier B"), not this
branch.

## 1. Scope (user decisions, this session)

| Decision | Choice |
|---|---|
| Breadth | **Full subsystem hardening**: C1 (Critical) + C2 + C3 + size/alignment guards + dead code + review test gaps |
| Reducer API deliverables (naming, docs-as-feature, introspection iterator) | **Dropped** (ceremony; no consumer) |
| C1 fix shape | **Approach 1: stable segmented arena** (relocation structurally impossible), over relocate-thunks and over restrict-to-trivially-copyable |
| Block source | **Existing `Astra::Tlsf`** (`include/Astra/Core/Tlsf.hpp`), dedicated Registry-owned arena — buddy allocator evaluated and declined (§8) |
| `ParallelCommandBuffer::Execute()` (unsorted flush) | **Delete** (sub-decision A) |
| RelocationCanary test component (~1 TypeID slot) | **Approved** (sub-decision B) |

Baseline: dev @ `6a4f45b`, tests Debug 742 / Release 740 / Dist 740.

## 2. Storage redesign — `CommandByteBuffer` becomes a stable segmented arena

### 2.1 The defect being removed (C1, Critical, controller-verified)

`CommandByteBuffer` today is one `std::vector<std::byte>` (`CommandBuffer.hpp:52-…`);
`Allocate()` grows it via `resize` (`:73-85`). Non-trivial components are placement-new'd
into that storage (`AddComponent`, `:411-453`), so vector reallocation **bit-copies live
C++ objects** — UB per the object model everywhere, a live crash on libstdc++/libc++ SSO
layouts, masked on MSVC by its offset-based SSO. `MergeFrom` (`:962-989`) memcpy's whole
buffers with the same defect, unconditionally. Payloads carry only a destructor thunk — no
relocate thunk exists (`Command.hpp:148-175`, `:190-225`, `:314-338`).

### 2.2 New shape

```
CommandBlockArena (one per Registry, owned by Registry)
  - std::mutex          (block acquire/release only -- never on the record path)
  - Astra::Tlsf         (dedicated instance; NEVER the chunk pool's)
  - OS regions          (same acquisition pattern as ArchetypeChunkPool's arena regions,
                         ordinary pages -- this arena is single-digit MB total)

CommandByteBuffer (per CommandBuffer, as today)
  - std::vector<Block>  where Block = { std::byte* base; size_t capacity; size_t used; }
  - bump-pointer Allocate() in the tail block
```

Rules:
- **No byte ever moves after it is written.** Growth = acquire a fresh block; existing
  blocks are untouched. C1's mechanism (relocation) is structurally impossible.
- **Commands never straddle blocks.** A command that does not fit the tail block's
  remainder opens a new block; the old tail keeps its dead tail bytes. Walks skip them
  because commands are self-delimiting (`CommandHeader::totalSize`, `Command.hpp:47-57`)
  and each block knows its `used` extent.
- **Ramp:** first block = the existing ctor `initialCapacity` param (default
  `DEFAULT_INITIAL_CAPACITY` = 4096), doubling per acquisition, capped at 64 KB. An
  oversized command gets a block sized to fit it exactly (payloads are ≤ 65535 B by the
  new static_assert, §5, so the worst case is bounded).
- **Retention:** `Clear()` resets every block's `used` to 0 and keeps all blocks. Steady
  state (per-frame record → flush → clear) makes **zero** arena calls; the arena mutex is
  touched only during warm-up ramps, workload spikes, and buffer destruction (blocks
  returned to the TLSF).
- Alignment: `Astra::Tlsf` guarantees every payload is 64-byte aligned by construction
  (its size-congruence law, `Tlsf.hpp:19-28`) — exceeding `CommandByteBuffer::ALIGNMENT`
  (16) with headroom. Note the congruence law adjusts requested sizes to ≡56 (mod 64):
  a "4096-byte" block's underlying TLSF payload comes back slightly larger, but
  `CommandBlockArena::Acquire` returns (and `Block::capacity` records) the
  REQUESTED size, not TLSF's adjusted size — the congruence remainder is
  intentionally left unused (a safe under-use of the allocation, not a bug).
  Intra-block layout keeps the existing header/payload/data alignment math
  unchanged.

Lifetime: a `CommandBuffer` already requires a live `Registry*` at construction; blocks
must be returned before the Registry (and its arena) dies. This is the existing implicit
contract, now stated: **command buffers must not outlive their Registry.**

### 2.3 What deliberately does not change

The recording hot path: one `Allocate()` (bump-pointer, amortized-O(1)) + placement-new,
same signatures, same `RegisterComponent` warm path, same `SortKey` recording, same
placeholder-entity design, same rollback/`m_committedCount` semantics. Flush semantics
(§14/Task-4 error channel, deterministic sort order) are unchanged.

## 3. API consequences

### 3.1 Deletions

- **`CommandBuffer::MergeFrom` and `ParallelCommandBuffer::MergeInto`: deleted.** Zero
  callers anywhere (engine or tests — verified by grep this session); the doc comment
  itself says the path predates `ExecuteSorted()` and nothing routes through it. Deleting
  it erases **C2** (self-merge silent data loss + leak) rather than guarding it.
- **`ParallelCommandBuffer::Execute()` (unsorted, physical-registration-order flush):
  deleted** (user decision A). It is non-deterministic by construction, used by nothing
  but one same-thread test (`CommandBufferTest.cpp:192-215`, which converts to
  `ExecuteSorted`). `ExecuteSorted()` becomes the only parallel fold.
  `CommandBuffer::Execute()` (the single-buffer eager path) is untouched.

### 3.2 Signature changes (internal-facing plumbing)

With stable storage, the offset indirection loses its reason to exist (the offset scheme
is self-documented at `CommandBuffer.hpp:1041-1053` as a workaround for vector
reallocation):

- `m_commandKeys`: `std::vector<std::pair<SortKey, size_t>>` →
  `std::vector<std::pair<SortKey, const std::byte*>>` (direct stable command pointers).
- `ApplyCommandAt(size_t offset)` → takes the command pointer.
- `ResolveAndApplyCommandAt(size_t offset, PlaceholderMap&)` → same.
- `ParallelCommandBuffer::ExecuteSorted()` plumbing updates mechanically (it consumes
  `CommandKeys()` across worker buffers; the tuple's offset member becomes the pointer).

`Execute()` / `CleanupPendingCommands()` iterate blocks-then-commands (two-level loop over
self-delimiting commands) instead of one flat byte range. Identical visit order (blocks
are recorded in order; commands in a block are in record order).

## 4. C3 — batch executors stop swallowing per-entity failures

Today `ExecuteAddComponentBatch`/`ExecuteRemoveComponentBatch` (`CommandBuffer.hpp:
1401-1416`, `:1418-1432`) discard every per-entity `bool` and return `true`
unconditionally, while the single-entity executors propagate failures. New semantics
(match the single-entity contract exactly, scaled to N):

- **Attempt all entities** in the batch (no abort-at-first — consistent with the sorted
  path's skip-and-report philosophy).
- Per failed entity, on the sorted path: emit one `DeferredCommandError` through the
  existing Task-4 channel, with the **same `Reason` the equivalent single-entity op would
  produce** for that failure (no new Reason values unless implementation finds a gap —
  if it does, that is a spec deviation to disclose).
- Executor returns `false` iff ≥1 entity failed, feeding eager `Execute()`'s existing
  whole-buffer failure semantics exactly as a failed single-entity command does.
- The hand-rolled per-entity loops stay. Routing through `Registry::AddComponentsByID`/
  `RemoveComponentsByID` was considered (rev-commands C3 "fix direction") and declined:
  they return only a success *count*, which cannot attribute *which* entity failed, and
  they are themselves per-entity loops today — per-entity checking in place is strictly
  more informative at identical cost. (Chunk-grouped batch transitions at the
  ArchetypeManager layer are a separate future perf lever, §7.)

## 5. Guards and dead code

- `static_assert(sizeof(DecayedT) <= std::numeric_limits<uint16_t>::max())` in
  `AddComponent`, `AddComponents`, and `SetResource`, beside the existing alignment
  static_asserts (`CommandBuffer.hpp:415,493,696`). Oversize components become a compile
  error; the silent `uint16_t` truncation path (prior-review I3) ceases to exist.
- `static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= CommandByteBuffer::ALIGNMENT)`
  (or the equivalent guard on the TLSF block alignment) — makes the comment-only claim at
  `CommandBuffer.hpp:56-61` real (prior-review I1).
- Delete dead code: `ExecutionResult` struct, the never-called `CalculateAddComponentSize`/
  `CalculateAddComponentBatchSize`/`CalculateSetResourceSize` trio; fix the stale
  "Align the size to 8 bytes" comment (code uses 16).

## 6. Testing

Baseline counts: Debug 742 / Release 740 / Dist 740. All tests reuse `Astra::Test::*`
types except the one budgeted addition below. House rule applies: characterization first
where behavior is preserved; RED→GREEN where behavior changes.

1. **C1 regression — `RelocationCanary` (the one budgeted new component type, user
   decision B).** `std::string` cannot RED deterministically on MSVC (offset-based SSO —
   exactly why C1 evaded detection). Canary design: a component holding
   `char buf[N]; char* self;` with the invariant `self == buf` maintained by its
   (properly written) ctors/move-ops; every move/copy ctor increments a static
   `s_violations` when the *source* invariant is broken (`src.self != src.buf` — pointer
   comparison only, **no dereference, no UB in the test**). RED (pre-fix): record enough
   canary commands to force buffer growth past the initial capacity, `Execute()`, assert
   `s_violations == 0` — fails on every platform because vector reallocation bit-copied
   the sources. GREEN (post-fix): same test passes, **and** asserts growth actually
   happened (>1 block acquired) so it cannot pass vacuously. Also assert the canary's
   destructor balance (destructor thunk runs exactly once per recorded payload).
2. **C3 RED:** a 50-entity `AddComponents`/`RemoveComponents` batch where a subset was
   destroyed earlier in the same flush: sorted path — per-entity `DeferredCommandError`s
   appear with correct attribution; eager path — `Execute()` reports failure. Both
   currently pass silently, which is the bug.
3. **Non-trivial components through `ParallelCommandBuffer::ExecuteSorted` under real
   threads** (`Name`/`Metadata` + canary; closes test-gap #1's parallel half).
4. **Retention:** record → `Execute(clear=true)` → record again; assert block count did
   not grow on the second cycle (steady-state zero-arena-traffic, observable via a
   test-visible block-count accessor or friend seam — implementation picks, disclose).
5. **Rollback across block boundaries:** force >1 block, fail mid-flush, assert the
   existing committed-prefix semantics hold.
6. Moot by deletion: MergeFrom/MergeInto coverage, PCB unsorted-`Execute` coverage
   (the `:192-215` test converts to `ExecuteSorted`), oversize runtime behavior (now a
   compile error).

## 7. Explicitly deferred (tracked, not this branch)

- `t_cache` cross-instance slot fragmentation (rev-commands M5) — hot thread-local path;
  zero impact under today's single-scheduler usage.
- Placeholder-entity type-level ambiguity (rev-commands §2) — a design feature (DOTS-style
  debug traps), not hardening.
- Chunk-grouped batch structural transitions at ArchetypeManager (rev-commands §6
  runner-up) — future perf lever.
- User-facing reducer/store layer ("Tier B") — ergonomics final pass.

## 8. Allocator evaluation record (buddy vs TLSF)

Buddy was re-evaluated for THIS workload rather than inheriting Phase 2's rejection, and
the Phase-2 rationale indeed does not transfer: chunk-pool requests are deliberately exact
(≡56 mod 64 congruence), which buddy's pow2 rounding fights; command blocks have no such
constraint — with pow2 block sizes, buddy's internal-fragmentation weakness vanishes and
its split/coalesce model fits a 4KB–128KB pow2 mix well. **On workload fit, buddy ≈ TLSF
here. It was declined on cost of ownership alone:** a new allocator implementation (new
correctness surface, tests, review burden) for a path whose steady-state traffic is zero
by construction, when an in-tree, chunk-pool-proven TLSF serves the same requests with
~zero marginal code. If a pow2-block consumer ever appears somewhere TLSF doesn't already
serve, buddy is the natural first candidate.

## 9. Performance stance

This is a correctness branch; Commands ops are not in the definitive 3-way bench matrix
and **no bench checkpoint is required**. Perf-neutrality is by construction: the record
path is byte-identical in shape (bump-pointer + placement-new); growth gets strictly
cheaper (block acquisition replaces O(n) reallocate-and-copy); flush adds one outer block
loop over the same command walk (negligible; block count stays small — the ramp reaches
the 64 KB cap within ~5 blocks, then one block per further 64 KB of recorded commands). Any
implementation step that would touch a measured hot path (none is expected) must say so
in its report.

## 10. Invariants (binding on implementation and review)

1. A recorded command's bytes are **immutable and address-stable** from record until
   `Clear()`/destruction/consumption.
2. Every non-trivial payload's destructor thunk runs **exactly once** (on execute-consume
   or cleanup — never zero times, never twice), including on rollback and mid-flush
   failure paths.
3. Commands never straddle blocks; every walk uses `CommandHeader::totalSize` +
   per-block `used`.
4. `Clear()` retains blocks (steady state performs no arena calls).
5. `ExecuteSorted()` is the only parallel flush; its determinism contract (SortKey total
   order) is unchanged.
6. The Registry-owned arena is the only block source, its TLSF is never shared with the
   chunk pool, and its mutex is never taken on the record fast path.
7. TypeID budget: exactly **one** new test component type (`RelocationCanary`); all other
   tests reuse `Astra::Test::*`.
