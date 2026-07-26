# Astra — Consolidated Tier-0 Correctness Register

**Date:** 2026-07-25
**Purpose:** The single source of truth for Astra's outstanding correctness work. Merges and de-duplicates two independent reviews plus three targeted spot-checks, so nothing is double-counted, re-opened, or lost.

> **✅ RESOLVED 2026-07-25 — batch fix committed `66277af` on branch `fix/tier0-correctness-batch` (off `dev`, NOT merged).** All 4 Criticals (CR-1..CR-4), all 25 Importants (IM-1..IM-25), and the 3 LZ4 codec fixes (C3/I11/IM-18) are fixed; **AstraTest Debug 771 / Release 769 / Dist 769, all pass.** Applied in a single pass without per-step verification (per user experiment), then verified once at the end; two mechanical test-contract reconciliations were needed (ChunkPool move test → non-movable contract test for IM-3; IM-24 recast from a Debug-abort to a graceful `bool` return to honor the tested graceful-misuse contract). **STILL OPEN: only the LZ4 *dispatch* wire-up (§3, Approach 1 vs 2 — a design decision). Caveat: the batch traded away per-task code review for speed — recommend a focused review of the non-localized changes (CR-2, CR-4, IM-4, IM-9, IM-22/23) before merging to `dev`.** Notable behavior changes: on-disk **v4 format changed** (per-descriptor enableable flag added — pre-fix v4 saves won't load; v4 is unreleased); range-`for` over an enableable-filtered View is now a **compile error** (IM-7); `AddComponent`/`EmplaceComponent` now return `bool` (IM-24).
>
> **✅ PRE-MERGE REVIEW PASS DONE 2026-07-25 (commit `15a71f8`):** 5 focused opus reviewers over the non-localized diffs (CR-2, CR-4/IM-9, IM-4, IM-22/23, LZ4 codec). **No Critical/UAF.** CR-4/IM-9 and IM-22/23 verified clean. Fixed 2 Important + 2 Minor: (1) CR-2 was inert on the scheduler flush path (`(void)flushResult`) → now logged in all configs; (2) LZ4 `output.reserve(uncompressedSize)` trusted an unbounded wire uint32 → clamped to `compressedSize*256`; (3) LZ4 match-copy realloc-UB → read-into-local; (4) IM-22 reentrant `Execute` Release-unsafe → graceful no-op guard. Debug 771 / Release 769 / Dist 769 green after fixes. **Deferred Minor follow-ups (OOM/misuse-only, non-blocking):** IM-4 new-slot rollback on alloc failure (permanent zombie slot after a transient OOM); CommandBuffer `IsEmpty()`/`GetCommandCount()` should reflect `m_recordFailed`; a defensive `~ResourceSlot`; registry-pointer ABA on persisted lambda-system Views.

**Sources merged:**
- **[OLD]** `docs/reviews/2026-07-21-astra-current-state-full-review.md` — 9-agent review-of-record (6 Criticals + ~13 Importants).
- **[NEW]** `docs/reviews/2026-07-25-astra-full-codebase-review.md` — 147-agent blind full review (assume-nothing; 3 Criticals + 25 Importants; every C/I adversarially verified at ≥2/3). Was NOT shown the OLD review, so agreement = independent corroboration.
- **[SPOT]** Three targeted opus code-reads (2026-07-25) settling the items the two reviews disagreed on (OLD-C2 / OLD-C4 / OLD-C6).

**Status legend:**
- `CONFIRMED` — real, reproduced/verified; needs a fix.
- `STILL-OPEN` — a prior finding re-verified against current code as still present.
- `PARTIAL` — half fixed, half open (scope narrowed; see note).
- `REFUTED` — adversarially killed or spot-checked as a non-issue; **do not re-open**.
- `FIXED` — resolved since the OLD review; listed only so it isn't re-flagged.
- `GATED` — validity depends on a design decision recorded below.

> **Reachability convention.** "Memory-unsafe" = corruption/UAF/OOB reachable in Release. "Silent" = wrong result with no crash/checksum failure (a determinism/data-integrity defect — still Tier-0 given Astra's stated bar). "Latent/public-API" = a real code defect not reachable through the normal `Registry` API.

---

## 1. Critical — fix before feature work

| ID | Title | Source | Status | Location | Fix |
|----|-------|--------|--------|----------|-----|
| **CR-1** | `ParallelForEachDescendant` holds a descendant-cache reference across the parallel region (cross-thread UAF) | OLD-Imp → **NEW-C1** (upgraded); found by 4 blind reviewers, 3/3 | **CONFIRMED** (memory-unsafe) | `Registry/Relations.hpp:213` (cache in `RelationshipGraph.hpp:754`) | Snapshot the cache **by value** (as every sequential sibling already does); longer-term have `GetDescendantsCached`/`GetAncestorsCached` return a value/`shared_ptr` snapshot, never a reference that escapes the lock. |
| **CR-2** | Every command-record method placement-news into an unchecked, possibly-null `Allocate()` result (null-page write in Release) | **NEW-C2**; 3/3 | **CONFIRMED** (memory-unsafe) | `Commands/CommandBuffer.hpp` (~20 sites: 333/372/403/433/462/512/553/577/623/675) | Make `Allocate()` failure a checkable outcome: one header+payload helper returning `false` on null, a sticky `m_recordFailed` surfaced by `Execute()` as `AllocationFailed`; guard every placement-new/`memcpy` so null never reaches `new`. |
| **CR-3** | `MetaRegistry::Register` silently aliases distinct types on a hash collision (wrong-size `Construct` → buffer overflow) | **OLD-C2** — *missed by NEW*; **[SPOT] STILL-OPEN** | **CONFIRMED** (memory-unsafe) | `Reflection/MetaRegistry.hpp:76-92` (and `52-68`) | On a `typeHash` hit, compare identity fields the `TypeMeta` already carries (`size`/`alignment`/`isTrivial`/`typeName`); same identity → idempotent return, different → refuse loudly (`ASTRA_LOG_ERROR` + `ASTRA_ENSURE_ALWAYS`, return null), mirroring `TypeContext::GetOrAssignComponentID` (`Core/TypeContext.hpp:102-147`). The component path is guarded; reflection never got the Theme-E `TypeIdentity` guard. |
| **CR-4** | Archetype `ComponentMask` serialized as raw per-run `ComponentID` bit positions → cross-process `Has<T>`/query/archetype-map desync | **OLD-C4**; **[SPOT] PARTIAL** | **CONFIRMED, silent** (data-integrity) | write `Archetype.hpp:873-876`; read `1001-1005`; never rebuilt (`Initialize` `249-258`); map key `ArchetypeManager.hpp:989/997` | Read-side only, format-preserving: after descriptors resolve by hash, rebuild `localMask` from each resolved `desc.id` and construct from it (not the raw disk mask). **Note:** the descriptor block itself is already fixed — it writes stable `Hash()` (`Archetype.hpp:900`) and resolves by hash on load (`1057-1072`), so column *data* lands correctly; only the mask half remains. |

---

## 2. Important — high priority

Grouped by theme. Full mechanism + concrete failure scenario for each NEW-I# is in `2026-07-25-astra-full-codebase-review.md`; this table is the actionable index.

### Memory safety / lifetime
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-1 | Batch `DestroyEntities`/component ops corrupt state on **duplicate** entities in the span | NEW-I1 (3/3) | CONFIRMED | `Registry/Registry.hpp:263` (+353–461) |
| IM-2 | `EntityTable::SetVersion` writes into a segment **after** it may be released (UAF); sibling `Destroy()` orders it correctly | NEW-I2 (3/3) | CONFIRMED | `Entity/EntityTable.hpp:141` |
| IM-3 | `ArchetypeChunkPool` move-assignment over a live pool frees all live chunk memory (public-API-reachable UAF) | NEW-I3 (2/3) | CONFIRMED | `Archetype/ArchetypeChunkPool.hpp:772` (move-ctor 759) |
| IM-4 | Small (SBO) resource pointers dangle after any resource add or unrelated `Remove` (vector relocation) | NEW-I7 (3/3) | CONFIRMED | `Component/ResourceStorage.hpp:334` |
| IM-5 | `RelationshipGraph` cache `shared_mutex` gives false safety: references escape the lock, mutators bypass it (systemic form of CR-1) | NEW-I6 (3/3) | CONFIRMED | `Registry/RelationshipGraph.hpp:760`, mutators 360/386/424 |

### Enableable-component query holes
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-6 | Required empty/tag component **null deref** on Optional and enableable-filtered iteration paths (UBSan-unclean, unconditional) | NEW-I4 (3/3) | CONFIRMED | `Registry/View.hpp:596` (+516–527, 659–721) |
| IM-7 | Range-based `for` over a View **bypasses** enableable disabled-bit filtering (`for(x:v)` disagrees with `v.ForEach()`/`Size()`) | NEW-I5 (3/3) | CONFIRMED | `Registry/View.hpp:375` (iterator `ViewIterator.hpp`) |

### Serialization / persistence (non-compression)
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-8 | Custom `Serialize()` hooks **bypassed** for `std::vector`/`std::array` elements (silent raw-`memcpy`) | **OLD-C5** → NEW-I8 (3/3) | CONFIRMED | `Serialization/BinaryWriter.hpp:300` (array 322; reader 345/371) |
| IM-9 | Enableable-ness gates a variable-length v4 section but is **not recorded** in the stream → schema-evolution desync | NEW-I9 (3/3) | CONFIRMED | `Archetype/Archetype.hpp:1271` (write 970–991) |
| IM-10 | `unordered_map`/`unordered_set` serialized in bucket order → **non-deterministic bytes + checksum** (violates design goal #1 on trusted data) | NEW-I10 (3/3) | CONFIRMED | `Serialization/BinaryWriter.hpp:423` (map 393) |
| IM-11 | `Skip`/`SkipPadding` don't update the running checksum but `WritePadding` does → paired use always fails checksum | NEW-I14 (3/3) | CONFIRMED | `Serialization/BinaryReader.hpp:679` |

### Untrusted / corrupt-input robustness (not a claimed security boundary, but a robustness bar)
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-12 | Container deserializers `reserve()` on an **unvalidated count** before reading elements (~20–30M× amplification → `terminate` under `-fno-exceptions`) | NEW-I12 (3/3) | CONFIRMED | `Serialization/BinaryReader.hpp:485` (543; non-POD vec 343) |

### Reflection
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-13 | `MetaRegistry::ForEachType` holds a `shared_lock` across the user callback → recursive-lock UB / deadlock | **OLD-C3** → NEW-I21 (2/3) | CONFIRMED | `Reflection/MetaRegistry.hpp:249` |
| IM-14 | `isVector`/`isStdArray` never populated (`ContainerTraits` unwired) → vector/array fields permanently misclassified; JsonSchema mistypes as `object` | **OLD-Imp** → NEW-I19 (3/3) | CONFIRMED | `Reflection/FieldInfo.hpp:367` |
| IM-15 | `copyAssign`/`moveAssign` lambdas gated on *constructible* (not *assignable*) traits → hard compile error for const/reference-member types | NEW-I20 (3/3) | CONFIRMED | `Reflection/TypeMeta.hpp:486` (guards 358/374) |

### Concurrency / portability (sanitizer-lane blockers — land before flipping the lane to required)
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-16 | `MAP_HUGETLB` referenced unguarded → **macOS build break** | NEW-I16 (3/3) | CONFIRMED | `Core/Memory.hpp:211` |
| IM-17 | `IsHugePagesAvailable` caches through non-atomic statics → data race (TSan) | NEW-I17 (3/3) | CONFIRMED | `Core/Memory.hpp:96` |
| IM-18 | `smallz4` unaligned `uint32` reads via pointer cast → UBSan alignment + strict-aliasing UB | NEW-I13 (3/3) | CONFIRMED but **GATED** (see §3 — only live if LZ4 is wired up) | `Serialization/Compression/Internal/smallz4.hpp:160` (646/684) |

### Core primitives / containers
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-19 | `MulticastDelegate` forwards the same args into every handler (double-move → moved-from value to 2nd+ handler) | NEW-I18 (3/3) | CONFIRMED | `Core/Delegate.hpp:439` (453) |
| IM-20 | `Delegate` SBO path checks size but **not alignment** → over-aligned functor placement-new'd misaligned (UB) | **OLD-Imp** → NEW-Minor | CONFIRMED (downgraded to Minor by NEW; sanitizer-relevant) | `Core/Delegate.hpp:330` |
| IM-21 | `FlatSet::SplitHash` omits the avalanche `Mix` that `FlatMap` applies (hash-quality cliff for low-entropy/pointer keys) | **OLD-Imp** (broader H2≡1) → NEW-I15 (3/3) | CONFIRMED | `Container/FlatSet.hpp:819` (cf. `FlatMap.hpp:918`) |

### Scheduler performance (perf-credibility, not memory-safety)
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-22 | `SystemExecutionContext` fully rebuilt (metadata + delegate copies) every `Execute()` | **OLD-Imp** → NEW-I23 (3/3) | CONFIRMED | `System/SystemScheduler.hpp:326` |
| IM-23 | View-lambda systems reconstruct + re-collect a full View every `Execute()` (the per-frame view-rebuild cost the north-star flags) | **OLD-Imp** → NEW-I24 (3/3) | CONFIRMED | `System/System.hpp:196` |

### API ergonomics / consistency
| ID | Title | Source | Status | Location |
|----|-------|--------|--------|----------|
| IM-24 | `AddComponent`/`EmplaceComponent` silently no-op on a stale handle while `RemoveComponent`/`SetEnabled` report `bool` (mixed void/bool/nullptr/Result vocabulary) | **OLD-Imp** (batch swallow) → NEW-I25 (2/3) | CONFIRMED | `Registry/Registry.hpp:300` (313) |
| IM-25 | Context systems can be added but never removed/queried (`RemoveSystem`/`HasSystem` reject `ContextSystem`) | NEW-I22 (3/3) | CONFIRMED | `System/SystemScheduler.hpp:219` (254) |

---

## 3. The LZ4 compression cluster — one decision governs four findings  ✅ SHIPPED

> **✅ SHIPPED 2026-07-26 — LZ4 dispatch wired up (branch `fix/tier0-correctness-batch` @ `2ceb13d`, FF-merged from `feat/lz4-per-column-compression`, 6 commits).** Chosen approach: **per-column block compression** (columnar standard, Parquet/ORC/Arrow-style), orthogonal to per-element versioning (covers all component types), on-disk format bumped **v4→v5**. Delivered via full SDD (spec `2026-07-25-lz4-per-column-compression-design.md`, plan `…/plans/2026-07-25-lz4-per-column-compression.md`; opus whole-branch review = ready to merge, zero Critical/Important). **OLD-C6 CLOSED** (compression now engages: measured 5,000,279 → 19,967 bytes, ~250×). **NEW-C3 CLOSED** — the >4MB multi-block round-trip is fixed and tested (previously saved-but-never-loaded). **NEW-I11 + IM-18 CLOSED** — the codec fixes (multi-block shared-buffer decode, output cap + clamped reserve, `memcpy` reads) landed in the tier0 batch + review and are now on the live path. `None` mode is byte-identical; v≤4 files load as legacy-raw (version-gated). 3-config green: Debug 776 / Release 774 / Dist 774. Deferred follow-up: reuse `colBuf` across columns (cold-path allocation nit; spec §3 envisioned it). *The GATED analysis below is retained as the historical record of why this was needed.*

**[SPOT] verdict: the LZ4 path is entirely DEAD on `Registry::Save`/`Load`** (OLD-C6 confirmed). `ComponentDescriptor::serializeVersioned` is unconditionally non-null for every component (`ComponentRegistry.hpp:212`), so `Archetype::Serialize`'s compressed branch (`Archetype.hpp:952/956 WriteCompressedBlock`) and its read mirror (`1241 ReadCompressedBlock`) are **unreachable**. A default save writes a header *claiming* `CompressionMode::LZ4` (`Registry.hpp:1530`) with **zero compressed bytes** — a cosmetic header/body mismatch.

**Consequence for the NEW review's compression findings:** NEW-C3 (multi-block frames >4MB save-but-never-load), NEW-I11 (~255× decode amplification), and NEW-I13/IM-18 (smallz4 unaligned reads) all claimed reachability via the **default save/load path**. That reachability is **REFUTED** — it repeats the exact error OLD-C6 identified. The underlying decoder/compressor defects are **real** but only in the public `BinaryWriter::WriteCompressedBlock` / `BinaryReader::ReadCompressedBlock` / `LZ4Decoder::DecompressFrame` API (which the tests call directly), not on the `Registry` path.

**DECISION (user, 2026-07-25): KEEP LZ4 as a supported option — wire it up, do NOT delete.** The encode/decode codec is fully implemented and correct at the call boundary (`WriteCompressedBlock` → `Compression::CompressBlock`); the gap is **dispatch**: `desc.serializeVersioned` is set unconditionally (`ComponentRegistry.hpp:212`), so the `else if (desc.is_trivially_copyable) → WriteCompressedBlock` branch (`Archetype.hpp:952/956`) is dead. A default save stamps an LZ4 header but writes uncompressed bytes. This is an unintended regression (universal `serializeVersioned` shadowed the POD compression branch that line 933's comment shows was the original intent).

Because we're wiring it up, **NEW-C3, NEW-I11, and IM-18 are now LIVE (no longer gated-out) and MUST be fixed** as part of making LZ4 real:
- **NEW-C3 (mandatory):** `DecompressFrame` decodes each 4MB block into a fresh buffer, so cross-block back-references fail → any payload >4MB compresses but never decompresses (unrecoverable data loss). LZ4 is not shippable as an option until this is fixed. `Serialization/Compression/Compression.hpp:162` / `LZ4Decoder.hpp:141`.
- **NEW-I11:** thread the known `originalSize` into the decode loop and cap output → kills the ~255× amplification / OOM on corrupt input.
- **IM-18:** replace smallz4's pointer-cast `uint32` reads with `memcpy`/`bit_cast` → UBSan-clean + strict-aliasing-safe on the compress path.

**Open design fork for the dispatch (needs a brainstorm before implementing):**
- **Approach 1 — block/stream-level compression, orthogonal to versioning:** keep the per-element `serializeVersioned` writes (schema evolution preserved) and compress each column's serialized bytes as a block (buffer → compress → emit with a size prefix). Keeps both versioning and compression; needs a small format change (compressed-block wrapper per column/section).
- **Approach 2 — POD bulk-compressed fast path:** for trivially-copyable components, skip the per-element versioned path and bulk-compress the whole array (restores the original intent; requires not setting `serializeVersioned` for POD types, or checking it after `is_trivially_copyable`). Per-element version headers are dropped for POD types — but the descriptor block already carries a schema-level version, so POD schema evolution isn't necessarily lost. Smaller change; aligns with the existing dead branch.

Recommend deciding this in a brainstorm; Approach 2 is closer to the existing structure, Approach 1 is the more general architecture. Either way the three codec fixes above are required.

---

## 4. Refuted / verified non-issues — do NOT re-open

Each was raised by a reviewer and killed by ≥2/3 adversarial verifiers or by a targeted spot-check. Kept here so they aren't rediscovered and re-worked.

- **`Archetype::ForEach` caches count/chunk-ref across the callback (iterator invalidation).** (OLD View-guard Important.) REFUTED 0/3 — internal primitive; the only public entry (`View::ForEach`) exposes no mutation handle, documents defer-to-CommandBuffer, and enforces it with a Debug structural-change assert. The standard footgun EnTT/flecs also ship. *(`Archetype.hpp:783`)*
- **`View` keeps `ArchetypeManager` alive past the Registry → dangling `m_records`.** REFUTED 0/3 — the pointer dangles but is never dereferenced; inert, not a reachable UAF. *(`View.hpp:83`)*
- **System declared-access masks unverified → silent within-archetype race.** (OLD Important.) REFUTED 0/3 — the scheduler faithfully honors declarations; the "race" requires the user to *lie* in the declaration. Reframed as a roadmapped **auto-derived-access feature gap** + a Minor (`SystemExecutor.hpp:96`), not a defect in existing code.
- **`RelationshipGraph` structural mutators race under normal use.** REFUTED 0/3 — immediate structural mutation is single-threaded by the owner-thread contract; parallel systems mutate via deferred commands flushed single-threaded. *(The escaping-reference half survives as IM-5.)*
- **`ComponentRegistry::RegisterComponent` races / first-registration rehash race.** REFUTED 0/3 & 1/3 — correct double-checked locking under `m_registrationMutex`; the map is pre-reserved past its permanent ceiling and never rehashes/erases.
- **`RelationshipGraph::Deserialize` no cross-consistency check.** REFUTED 0/3 — traversals hardened (visited-sets, cycle detection, depth caps); inconsistent state only via a crafted binary — the documented "tolerate, don't validate" untrusted-load posture.
- **`FieldInfo` null-pointer-to-member offset → UBSan.** REFUTED 0/3 — empirically clean on clang 18 (constant-folded; zero `__ubsan_handle` calls under `-fsanitize=null,alignment`). *(`FieldInfo.hpp:353`)*
- **Reentrant `Execute()` silently corrupts the command stream.** REFUTED 0/3 — framework-unreachable (`SystemContext` exposes no scheduler handle), Debug-asserted; worst real outcome is a loud stack overflow, and the inner flush *applies* outer commands. *(`SystemScheduler.hpp:281`)*
- **NEW-C3/I11/IM-18 "reachable via default `Registry::Save`/`Load`."** REFUTED by [SPOT] — see §3 (LZ4 is dead on that path). The defects themselves are real in the public codec API and gated on the §3 decision.

---

## 5. Fixed since 2026-07-21 — for the record, don't re-flag

- **OLD-C1 — Commands byte-buffer bit-relocates non-trivial components.** FIXED (segmented TLSF arena, dev `5fb88f9`). *(CR-2 is a **different** command-buffer bug — allocation-failure handling.)*
- **Per-chunk metadata bloat (~23 KB/chunk).** FIXED (Phase C `ArchetypeColumnMeta`).
- **Hot add/remove paths scan 0..128 instead of N.** FIXED (Phase C 0..N loops).

---

## 6. Coverage gaps / thin areas — schedule a focused pass

From the NEW review's completeness critic (no file was entirely uncovered, but these got thin depth):
1. **TLSF allocator internals** (`Core/Tlsf.hpp`) — single-reviewer; boundary-tag coalescing, the ≡56-mod-64 alignment trick, split/merge under the segmented-arena free-order contract unexamined. Recommend a dedicated allocator pass + an ASan fuzz harness (it underpins every chunk/command allocation).
2. **Non-x86 SIMD fallback** for Swiss-table probing (scalar/NEON control-byte match, tombstone handling) — not probed.
3. **Auto-parallel access-derivation enforcement** — the crux of the concurrency model (parallel-group safety rests entirely on declared access being a superset of actual). Tied to the refuted System-race item and the roadmapped auto-derive feature.
4. **Cross-worker deferred-command flush determinism** — gathered worker-buffer order noted non-deterministic (`SystemScheduler.hpp:~469`); nested `ParallelForEach` an unhandled limitation (`SystemContext.hpp:~134`). Determinism is design goal #1; warrants a determinism-focused pass.

---

## 7. Suggested execution order

1. **Memory-safety Criticals first:** CR-1, CR-2, CR-3 (all Release-reachable corruption/UAF). CR-3 was invisible to the blind pass — do not skip it.
2. **Cross-process integrity:** CR-4 (mask rebuild) + IM-9 (enableable section flag) — both silent, both small, both format-adjacent; fix together.
3. **Sanitizer-lane blockers:** IM-16, IM-17 (+ IM-20 alignment) so the ASan/UBSan/TSan lane (`ci/sanitizer-lane`, built and parked) goes green on first run. Decide §3 (LZ4) here — option (B) also removes IM-18 from this list.
4. **Remaining memory-safety Importants:** IM-1..IM-7.
5. **Determinism + serialization fidelity:** IM-8, IM-10, IM-11, IM-12.
6. **Reflection correctness:** IM-13, IM-14, IM-15.
7. **Perf + ergonomics:** IM-19, IM-21, IM-22, IM-23, IM-24, IM-25 — several align with the north-star's per-frame-view-rebuild and Bevy-parity goals.

Each memory-safety item is a verify → spec → plan → SDD unit; most fixes are localized (snapshot-by-value, a null check, an identity check, a mask rebuild, an `#ifdef`, one `Mix()` call).
