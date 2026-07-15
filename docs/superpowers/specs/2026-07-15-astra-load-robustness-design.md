# Astra `Registry::Load` robustness floor — design

**Date:** 2026-07-15
**Status:** Design — awaiting review
**Branch:** `load-robustness` (off `dev`)
**Origin:** 2026-07-11 full-codebase review, must-fix item 10 ("Registry::Load trust boundary"), **deliberately descoped** to a robustness floor (see §1).

---

## 1. Motivation, and why this is scoped small

The review flagged `Registry::Load` as an unhardened trust boundary and asked for
full untrusted-input hardening plus a fuzz harness. Research into how the ECS
field actually handles this (EnTT, flecs, Bevy, Unity DOTS, Unreal) found a clear
consensus: **ECS save data is treated as trusted.** EnTT — the reference C++ ECS —
reads counts straight off the archive and loops on them with zero validation;
Unity bakes scenes at build time; Unreal's default `USaveGame` is unvalidated and
integrity is left to the developer. The crafted-input CVEs in that space live on
the *network/package* surface, not the save file.

So full untrusted-save hardening is a **positioning choice** Astra is explicitly
*not* making right now. What we ARE fixing is narrower and threat-model-independent:
today a **non-adversarial** bad file — one that was truncated by an interrupted
write, bit-rotted on disk, or written by a skewed version — can drive `Load` into
**heap corruption, process termination, or an infinite hang** rather than a clean
error. That is a robustness bug in a library regardless of whether an attacker
exists, and it is what this work fixes.

## 2. Posture / guarantee (and non-goals)

**Guarantee (the "B" posture):** On any byte sequence — truncated, bit-rotted,
version-skewed, or internally inconsistent — `Registry::Load` returns a valid
`Registry` or `Err(SerializationError)`. It never:
- reads or writes out of bounds (no heap corruption),
- allocates unboundedly (no `bad_alloc` → `std::terminate`; Astra ships
  exception-free, so a throwing allocation is not catchable — see §3),
- hangs or infinite-loops,
- or otherwise terminates the process.

**Explicit non-goals** (this is NOT a security boundary):
- We do **not** promise to reject every semantically-inconsistent-but-safe save.
  Data that is odd but cannot cause the four failures above (e.g. a relationship
  referencing an entity not in the loaded set) may load through.
- We do **not** add a decompression-bomb cap, a cryptographic integrity check, or
  a fuzz harness. Those belong to a future "untrusted saves" effort ("C", §6) if
  Astra ever chooses to advertise that capability.
- The `Load` doc comment (§9) states this boundary plainly so a downstream user
  does not mistake "fails cleanly on corruption" for "safe to load hostile input."

## 3. Design principle: check-before-allocate

Astra is built exception-free (RTTI-off, `GTEST_HAS_EXCEPTIONS=0`; Release/Dist
define `NDEBUG`). A `reserve`/`resize`/`new` that throws `std::bad_alloc` therefore
calls `std::terminate` — it cannot be turned into an `Err`. **Every fix in this
design validates a value BEFORE it is used to allocate, index, or loop** — never
"try it and catch the failure." This is not a stylistic choice; try/catch is
unavailable in the shipping configuration.

## 4. Scope — three groups, by the failure each prevents

All file:line anchors are as of branch point `dev`; the implementer must re-confirm
them against the live tree.

### Group 1 — heap corruption (the must-fix)

**1a. `chunkEntityCount` unbounded by chunk capacity.**
`Archetype::Deserialize` reads a per-chunk `chunkEntityCount` (`Archetype.hpp` ~:847)
and uses it to (i) loop `AddEntity` and (ii) compute
`arraySize = chunkEntityCount * desc.size` for a `std::memcpy` into a
chunk-sized component array (`Archetype.hpp` ~:881, ~:927) and per-element
placement-new on the custom-serializer path (~:889-901). `entitiesPerChunk` is
already validated against the pool chunk size (~:809-827); `chunkEntityCount` is
not validated against `entitiesPerChunk`. A value greater than capacity writes
past the end of the heap chunk.
**Fix:** immediately after reading `chunkEntityCount`, require
`chunkEntityCount <= entitiesPerChunk`; else set the reader error / return
`Err(CorruptedData)` before any allocation, loop, or copy.

**1b. `chunkIndex` / `entityIndex` stored unvalidated.**
`ArchetypeManager::Deserialize` reads `(entity, archetypeIndex, chunkIndex,
entityIndex)` (`ArchetypeManager.hpp` ~:878). `archetypeIndex` IS bounds-checked
(~:883); `chunkIndex`/`entityIndex` are stored raw into the `EntityRecord` (~:887).
Any later access through that record indexes `m_chunks[chunkIndex]` and within-chunk
`[entityIndex]` → OOB. By the time the entity map is rebuilt, the archetypes and
their chunks already exist (archetype loop precedes the entity-map loop), so the
valid ranges are known.
**Fix:** validate `chunkIndex < archetype->m_chunks.size()` and `entityIndex`
against that chunk's entity count before storing the record; on failure return
`Err(CorruptedData)`. Also change the existing `archetypeIndex`-out-of-range branch
from **silently dropping** the mapping (which leaves a live entity with no
location — a latent bug) to the same `Err(CorruptedData)`.

### Group 2 — process termination from a garbage count

On a truncated file, a count field reads as garbage and drives an allocation far
larger than the file could justify → `bad_alloc` → `terminate`. The fix is the
industry-standard "bound the length by the bytes that remain" pattern (Cap'n Proto
read limiter, FlatBuffers offset checks, serde `cautious` cap): a count that
exceeds `remainingBytes / minBytesPerElement` cannot be real and is rejected. In
this posture the bound doubles as a **truncation detector** ("claims 1M entities,
20 bytes remain").

**2a. New bounded-count reader primitive.** Add to `BinaryReader`:
- `size_t Remaining() const` — bytes left in the buffer (`m_size - m_position`).
- A checked count read, e.g.
  `uint64_t ReadBoundedCount(size_t minBytesPerElement) noexcept` — reads the
  count, and if `count > Remaining() / minBytesPerElement` (computed to avoid
  division by zero and without overflow) sets `m_error = CorruptedData` and
  returns 0. (Exact name/return-type to be finalized in the plan; behavior is
  fixed.)

**2b. Apply it at every manual count site** that drives a `reserve`/`resize`/loop:
- `ArchetypeManager.hpp` ~:806 `archetypeCount`, ~:807 `entityCount`
- `Archetype.hpp` ~:777 `descriptorCount`, chunk-count loop
- `EntityManager.hpp` ~:358 `recycledCount`, alive-count loop
- `RelationshipGraph.hpp` ~:462/:489/:506/:531/:548 (parent/child/link counts)
Each uses the minimum on-disk size of one element (the sum of its fixed-size
prefix fields; a conservative lower bound where variable, never below 1). The
existing `reserve`/loop then operates on an already-bounded count.

**2c. Integer-overflow in the vector reader.**
`BinaryReader::ReadVector` for trivially-copyable `T` checks
`size * sizeof(T) > remaining` (`BinaryReader.hpp` ~:304) — a `uint64 * size_t`
product that wraps, letting a crafted `size` pass the check and then
`vec.resize(size)` attempt an exabyte allocation. This is the exact overflow class
Cap'n Proto / FlatBuffers ship advisories for.
**Fix:** overflow-safe form — `size > (remaining / sizeof(T))` — so the resize is
correctly bounded by remaining bytes. (This alone makes the reader's POD-vector
path safe; the non-POD 1M cap and map/set 10M caps already bound their paths.)

**2d. Uncompressed-block pre-allocation.**
`ReadCompressedBlock` uncompressed path does `std::vector<uint8_t> data(originalSize)`
from a `uint32` (`BinaryReader.hpp` ~:206) before `ReadBytes` verifies the bytes
exist — a 4 GB alloc from a 4-byte field.
**Fix:** require `originalSize <= Remaining()` before allocating.

### Group 3 — infinite hang

**3a. `IsAncestorOf` has no cycle guard.**
A crafted/corrupt parent map (A→B→A, or A→A) is accepted verbatim at load
(`RelationshipGraph::Deserialize` writes `m_parents[child] = parent` directly, no
`SetParent`). `IsAncestorOf` (`RelationshipGraph.hpp` ~:135-145) walks parents with
a plain `while (current.IsValid()) current = GetParent(current)` — no visited
guard — so it loops forever on a cyclic map. It is reached by the next runtime
`SetParent`, making a corrupt save a latent hang.
**Fix (traversal-safe, not load-time rejection):** make the walk terminate on a
cycle — a visited guard, or a step cap bounded by the parent-map size. Consistent
with the posture: we do **not** reject the cyclic data at load (that would be
semantic validation, out of scope); we ensure no traversal hangs on it. The
implementer audits sibling traversals (`GetParent` walks) for the same unguarded
pattern and guards any found; `IsAncestorOf` is the known one.

## 5. Failure mode & API impact

- **No public API change.** No new `Load`/`Save` signatures, no new configuration.
- **No new error variants required** — reuse `SerializationError::CorruptedData`
  (and `SizeMismatch` where a size specifically mismatches). The plan may add one
  variant only if it materially improves a caller's ability to react; default is
  reuse.
- **Zero behavior change on valid saves** — every check is a reject path for input
  that is already malformed; a well-formed save takes an identical path and
  produces an identical `Registry`. This is verified by the existing
  round-trip/serialization tests continuing to pass unchanged.

## 6. Explicitly deferred (a future "C", not in this work)

Recorded so the boundary is unambiguous:
- Decompression-output cap / bomb defense (adversarial; accidental corruption of a
  compressed block already fails via the decoder's error path and the post-decode
  size cross-check).
- Dangling relationship references, declared-vs-restored `entityCount`
  cross-checks, `version == 0` lower-bound rejection, stored-vs-registry descriptor
  size comparison — semantic/integrity, none memory-unsafe.
- Cryptographic / tamper-evident checksum.
- A fuzz harness (libFuzzer or mutational).

## 7. Testing strategy

Targeted, hand-crafted corruption cases — **not** a fuzz harness:
- Each fix gets a unit test that starts from a valid `Save` output, mutates the
  specific field it addresses (e.g. overwrite a `chunkEntityCount` to
  `entitiesPerChunk + 1`; a count to `0xFFFFFFFF`; install a cyclic parent map),
  and asserts `Load` returns `Err(...)` (or, for the hang, that the traversal
  terminates) — with no crash, OOB, or hang.
- A truncation sweep: take a valid save and truncate it at a range of offsets;
  every prefix must yield `Err`, never a crash. (Cheap loop; the closest thing to
  fuzzing we include, and it directly exercises the buffer-derived bounds.)
- Live under `tests/Registry/` and/or `tests/Serialization/` alongside the
  existing serialization tests; match each file's `TEST`/`TEST_F` style.
- Gate: the standard 3-config build (Debug/Release/Dist) + full suite. The
  memory-corruption fixes (Group 1, 2c/2d) are the ones whose absence is most
  visible under an address sanitizer; running the new tests under ASan on the
  Linux CI leg is encouraged but not required for this work.

## 8. Where the changes land

- `include/Astra/Serialization/BinaryReader.hpp` — `Remaining()`,
  `ReadBoundedCount`, the `size*sizeof` overflow fix (2c), the uncompressed-block
  pre-alloc guard (2d).
- `include/Astra/Archetype/Archetype.hpp` — `chunkEntityCount` bound (1a),
  `descriptorCount`/chunk-count via bounded read (2b).
- `include/Astra/Archetype/ArchetypeManager.hpp` — `chunkIndex`/`entityIndex`
  validation + no-silent-drop (1b), `archetypeCount`/`entityCount` bounded (2b).
- `include/Astra/Entity/EntityManager.hpp` — `recycledCount`/alive-count bounded (2b).
- `include/Astra/Registry/RelationshipGraph.hpp` — relationship counts bounded (2b),
  `IsAncestorOf` cycle guard (3a).
- `include/Astra/Registry/Registry.hpp` — the `Load` trust-boundary doc comment (§9).
- Tests under `tests/Registry/` and/or `tests/Serialization/`.

## 9. Trust-boundary documentation

A doc comment on `Registry::Load` (and a short note wherever `Save`/`Load` are
documented) stating the posture in plain terms:

> `Load` validates structural integrity enough to fail cleanly — returning
> `Err(SerializationError)` — on truncated, corrupt, or version-skewed input,
> without out-of-bounds access, unbounded allocation, or hangs. It is **not** a
> security boundary: it does not guarantee rejection of every maliciously-crafted
> archive, and callers must not load saves from untrusted sources without their
> own validation.

This is the one universal recommendation across the research: EnTT's real defect
is not that it trusts the archive but that it does so *silently*. Say the boundary
out loud.

## 10. Global constraints (bind the implementation)

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang.
- **Exception-free & RTTI-off in shipping** → check-before-allocate (§3); no
  try/catch on allocation.
- No public API break; no new dependency.
- Namespace `Astra`; the diagnostics seam (`ASTRA_ASSERT`/`ASTRA_ENSURE`) is
  available but note asserts compile out in Release/Dist — a robustness check that
  must hold in shipping is a real `if (…) return Err(…)`, never an assert.
- Build via MSBuild on the whole solution (`-t:AstraTest` does not work); new test
  files require `premake5 vs2022` regen; appending to an existing test file does
  not.
- Baseline suite: 585 Debug / 583 Release+Dist (dev @ `e56d8fd`).

## 11. References (research sources)

- EnTT snapshot loader (no validation; trusted archive):
  `github.com/skypjack/entt` `src/entt/entity/snapshot.hpp`, `docs/md/entity.md`
- serde `size_hint::cautious` (cap preallocation at 1 MiB-equiv):
  `github.com/serde-rs/serde` `serde_core/src/private/size_hint.rs`; issue #1087
- Cap'n Proto read/traversal limit (default 64 MiB) + integer-overflow advisories:
  `capnproto.org/encoding.html`, security advisories 2015-03-02 / 2015-03-05
- FlatBuffers `Verifier` (max_depth 64, max_tables 1e6, overflow-checked sizes):
  `github.com/google/flatbuffers` `include/flatbuffers/verifier.h`
- Protobuf recursion + total-bytes limits: `protobuf.dev` CodedInputStream docs
- Decompression-bomb guidance (absolute cap + ratio tripwire): bamsoftware zipbomb
