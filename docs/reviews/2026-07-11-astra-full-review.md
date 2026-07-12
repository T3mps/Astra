# Astra ECS — Full Codebase Review

**Date:** 2026-07-11 · **Scope:** all 58 headers under `include/Astra/` (the entire library) plus the test suite · **HEAD:** `2b08bd3` (dev, post-3.4).

**Method.** The codebase was partitioned into 13 cohesive subsystems and each was audited independently, from scratch ("assume nothing"), by a dedicated reviewer against eight dimensions: correctness/UB, memory safety, concurrency, API design, performance, error handling, portability, and test coverage. Reviews were read-only. The five subtlest subsystems (archetype storage, archetype manager, serialization, concurrency/systems, hash containers) were reviewed at higher rigor. Several headline findings were independently reproduced by compiling probes against the real MSVC/Clang toolchains and by direct source inspection during synthesis. The 13 detailed per-subsystem chapters follow this summary.

---

## 1. Verdict

**Astra is architecturally strong but not yet safe in its shipping (Release/Dist) configuration, under concurrency, or on untrusted input.** The core data structures and hot paths are well-designed and, where they matter most, correct: the SwissTable hash containers are correct and well-fuzzed; the SOA archetype/chunk/pool design is sound; the type-erasure machinery generates correctly-paired construct/move/destruct thunks; the live-API relationship rejection contracts (cycles, self-links, invalid entities) are solid and tested. That foundation is real and worth preserving.

But the review surfaced a small number of **systemic patterns**, each repeated across many subsystems, that together undermine the library's own stated guarantees:

1. **Recoverable conditions are validated with `ASTRA_ASSERT` only — so the checks vanish in Release/Dist and become memory-unsafety.** This directly contradicts the project's own contract ("caller/recoverable errors handled gracefully in ALL configs; never assert on caller input"). It is the single most important finding, and it recurs in at least five subsystems.
2. **Thread-safety is largely illusory.** `std::atomic` counters and locking on *some* members imply a concurrency safety that the surrounding code does not actually provide; the true guarantee is single-threaded-mutation, but that is neither documented nor enforced, and the library ships multithreaded entry points (`ParallelForEach`, parallel systems) that are unsafe.
3. **The `Registry::Load` path trusts the byte stream's semantic validity.** The checksum detects accidental corruption, not a crafted-but-valid-checksum payload; every layer (serialization, entities, archetypes, relationships) reads stream-supplied counts/indices/IDs and uses them without bounds/consistency validation.
4. **Alignment is handled incompletely** across the hand-rolled storage/allocation paths.
5. **Iteration holds live references into internal containers across user callbacks**, so natural patterns (e.g. "destroy each child") cause use-after-free.

These are patterns, not one-off bugs. The recently-merged 3.4 remediation fixed *specific instances* of several of them (e.g. one `ResourceStorage` OOM path, one typed-`AddComponent` overflow, the `View` invalidation counters); this review shows the underlying **classes** are still pervasive. The highest-leverage next step is to fix the *patterns* library-wide, not more instances.

### Severity accounting (calibrated)

Raw reviewer labels totalled **27 Critical / 66 Important / ~80 Minor**. Many "Criticals" share one root cause (e.g. the assert-only-guard pattern accounts for ~8 of them) or are latent UB that only triggers under a specific — but entirely plausible — condition. De-duplicated into distinct root issues and ranked by *impact × reachability*, the picture is:

- **Reachable in ordinary Release use (no adversary, no threads):** ~8 distinct Critical-class issues (see the Must-Fix list, items 1–8). These are the ones to fix first.
- **Reachable only under concurrency:** the entire thread-safety theme (Theme B) — Critical *if* the multithreaded paths are used, otherwise a documentation defect.
- **Reachable only on untrusted/crafted input:** the Load-hardening theme (Theme C) — Critical *if* saves are ever loaded from an untrusted source; several of its members are also plain round-trip bugs on *trusted* data.
- **Config/portability-specific:** 32-bit `size_t`, non-standard entity widths, bare-MSVC SIMD — real but gated on non-default build configs.

Counts below are per-subsystem raw tallies; the thematic and Must-Fix sections are the calibrated view.

| # | Subsystem | C | I | Chapter | One-line verdict |
|---|-----------|---|---|---------|------------------|
| 1 | Archetype storage (Archetype/ChunkPool/Graph) | 2 | 6 | `01-archetype-core.md` | Solid SOA/pool design; real memory-safety defects, none caught by tests. |
| 2 | ArchetypeManager | 1 | 6 | `02-archetype-manager.md` | Core add/remove/move coherent; serialization drops zero-component entities; remove/deserialize paths lack the add paths' guards. |
| 3 | Serialization & Compression | 1 | 4 | `03-serialization.md` | Round-trip & checksum sound; "parse untrusted bytes safely" contract is broken. |
| 4 | Concurrency & Systems | 1 | 5 | `04-concurrency-systems.md` | Conflict analysis conservatively sound; mask-only model permits corruption; guard promises more than it delivers. No shipped thread-pool. |
| 5 | Hash containers (FlatMap/FlatSet/Swiss) | 0 | 4 | `05-hash-containers.md` | **Core probe/rehash/erase correct & well-fuzzed.** Defects: one leak, a 32-bit hash UB, MSVC SIMD fragility. |
| 6 | Core foundation (Result/Memory/Simd/TypeID/…) | 2 | 7 | `06-core-foundation.md` | Solid mechanics; a real type-identity collision and a silently-disabled SIMD path; Result/Memory/Simd untested. |
| 7 | Small containers (SmallVector/Bitmap/AlignedStorage) | 2 | 3 | `07-small-containers.md` | Growth/SBO careful; single-element mutators lack self-aliasing defense the count-insert has. |
| 8 | Entity (Entity/Manager/Table/IDStack) | 2 | 3 | `08-entity.md` | Solid recycling design; deserialize + non-standard version-width bit math have handle-aliasing bugs; unsynchronized. |
| 9 | Component & Resources | 4 | 6 | `09-component-resources.md` | Type-erasure sound; `ResourceStorage` templated API systematically drops the guards its by-ID sibling has. |
| 10 | Registry, Views & Query | 1 | 10 | `10-registry-views.md` | View refresh protocol sound but `Size()`/`Empty()` bypass it (UAF); many public-API footguns. |
| 11 | Relationships (RelationshipGraph/Relations) | 4 | 3 | `11-relationships.md` | Live-API rejection solid; fast traversal holds live refs across callbacks (UAF); deserialize/cycle unguarded. |
| 12 | Commands & Events (CommandBuffer/Delegate/Signal) | 4 | 3 | `12-commands-events.md` | Clever byte-encoding; four independently-triggerable Criticals in Delegate SBO, multicast reentrancy, and rollback. |
| 13 | Reflection | 3 | 6 | `13-reflection.md` | Static-init design solid; type-erased field accessors unguarded in shipping builds; several advertised features don't work. |

---

## 2. Cross-cutting themes

The themes are the actionable structure: fixing each *pattern* resolves many findings at once.

### Theme A — Assert-only validation of recoverable conditions → Release/Dist UB  ★ highest priority

`ASTRA_ASSERT` compiles to nothing outside Debug. The codebase repeatedly uses it as the *only* guard on conditions that are recoverable and caller-reachable, so in the shipping configs the guard disappears and the code proceeds into UB. This is a direct violation of the project's stated all-configs contract and is the root cause of the largest share of Critical findings.

Instances (each cited in its chapter):
- **Reflection** `FieldInfo::Get<T>`/`Set<T>`/`GetPtr<T>` (`FieldInfo.hpp:75,92-94,108,121`): type match is assert-only → a wrong `T` does a differently-sized blind read/write (stack/heap corruption) in Release; `Set<T>` on a `const` field calls an empty `std::function` → `std::terminate()` under `-fno-exceptions`; `GetPtr<T>` never checks `isConst` at all → writable pointer to a const field. *(Confirmed by inspection.)*
- **ResourceStorage** `Set<T>`/`Emplace<T>` (`ResourceStorage.hpp:130-244`): `id < MAX_COMPONENTS`, `AllocateMemory` success, and `weak_ptr` liveness are all assert-only → OOB `m_sparse` write, placement-new at `nullptr`, and null `shared_ptr` deref in Release; plus `slot.isValid=true` with a null descriptor permanently skips teardown.
- **ComponentRegistry** alignment cap (`ComponentRegistry.hpp:116-117`): assert-only (unlike the paired `MAX_COMPONENTS` guard) → Release silently accepts over-aligned components.
- **Core** `TypeContext` ID-exhaustion (`TypeContext.hpp:70-86`): assert-only → Release wraps `uint16_t` and collides IDs. `Result::operator*` (`Result.hpp:144-157`): **no guard at all**, not even Debug → type-confusion read of `T` from an `E`-holding union. *(Confirmed by inspection.)*
- **Delegate** copy of a small move-only functor (`Delegate.hpp:52-274`): assert-only → in Release leaves storage uninitialized while `m_invoker` claims validity.
- **ArchetypeManager** batch OOM (`ArchetypeManager.hpp:1167`): `ASTRA_ASSERT(false)` → aborts Debug on a recoverable allocation failure (the inverse mistake — asserting where it should gracefully fail).

**Systemic fix:** audit every `ASTRA_ASSERT` whose predicate can be caused by caller input, allocation failure, or exhaustion. Convert those to real, all-configs checks that return `Result::Err`/`nullptr`/`false`/no-op. Reserve `ASTRA_ASSERT` for genuine internal invariants that a correct caller cannot violate. A grep for `ASTRA_ASSERT` triaged into "invariant" vs "recoverable" is a concrete first task.

### Theme B — Thread-safety is illusory (atomics/comments imply guarantees the code lacks)

Several subsystems decorate a few members with `std::atomic` or take a mutex on *derived* state while leaving the primary structural containers completely unsynchronized, and comments describe a safety that does not exist.

- **ArchetypeManager** (`:1327-1329`) + **View** (`View.hpp`): atomic change/removal counters imply safe concurrent iterate+mutate; the archetype map and `m_generation` are read racily.
- **ArchetypeChunkPool**: `m_freeList`/`m_blocks`/`m_memoryToNode` mutated unlocked despite `atomic` decorations; true guarantee is single-writer.
- **EntityManager/Table/IDStack** (`08`): no locks → two threads calling `Destroy(e)` both pass `IsValid` then double-recycle the ID (Critical).
- **ComponentRegistry**: `RegisterComponent<T>` runs on runtime hot paths (first resource use) with no sync → concurrent first-registration data race.
- **RelationshipGraph**: only the cache maps get `m_cacheMutex`; `m_parents`/`m_children`/`m_links` are unsynchronized, yet `ParallelForEachDescendant` is a first-party multithreaded entry point.
- **Core** `TypeContext` context pointer (`:104-142`) and `Memory` huge-page double-checked-locking (`:94-138`): unsynchronized plain-variable races.
- **SystemScheduler** `ExecutionGuard` (`:30-56`): a check-then-act TOCTOU boolean, not exclusion, and reentrancy clears it early.

**Systemic fix:** pick and document *one* threading model. The pragmatic choice for an ECS is "structural mutation is single-threaded; parallel iteration is read-only / value-mutation-only." Then (a) delete the misleading atomics/comments that suggest more, (b) make the read-only parallel paths actually read-only-safe, and (c) either remove the "parallel systems that do structural changes" capability or route it through a command buffer. If true concurrent mutation is a goal, it needs a real design, not incremental atomics.

### Theme C — `Registry::Load` trusts untrusted input (semantic validation missing)

The checksum guarantees the bytes are the bytes that were written; it does **not** guarantee those bytes describe a valid registry. Every deserialization layer reads counts/indices/IDs and uses them without validation, and several of the resulting bugs also fire on *trusted* saves.

- **BinaryReader**: overflowable `ptr+len`/`pos+size` bounds checks (`:100`, LZ4 `:76,187,253`) → OOB read on 32-bit and UB on 64-bit; multi-GB allocations from raw `uint32` length fields (`:206,219`) → crash-DoS with exceptions off; `size*sizeof(T)` overflow defeats the one vector guard (`:304`); LZ4 frame decode ignores `originalSize` → decompression bomb (`:141,216`) and silently returns Ok on truncated frames (`:172,208`).
- **ArchetypeManager** `Deserialize` (`:805-811`): validates `archetypeIndex` but not `chunkIndex`/`entityIndex` → OOB; and it **skips the root archetype** (`:698,738-741`), so zero-component entities become dangling map entries — a plain round-trip bug on trusted data (Critical, Theme-C-adjacent but not adversarial).
- **Archetype** `Deserialize` (`:814-838`): no clamp of per-chunk count to `entitiesPerChunk` → OOB write.
- **EntityManager** `Deserialize` (`:332-403`): trusts IDs with no bounds/duplicate check → a later `Create()` reissues an "alive" ID, aliasing two entities onto one slot.
- **RelationshipGraph** `Deserialize` (`:449-565`): zero validation → a crafted save plants a cycle that feeds the unguarded `IsAncestorOf` (below) into an infinite loop, or trips `ASTRA_ASSERT(false)` in `BuildAncestorCache`.

**Systemic fix:** treat `Load` as a trust boundary. Validate every stream-supplied count/index/ID/offset against the actual extents *before* use; use the non-overflowing `len > remaining` / `size > remaining/sizeof(T)` idiom (already used correctly for strings at `BinaryReader.hpp:279`); cap allocations and decompression output; and add a fuzz harness over `Registry::Load(span, creg)` — one such harness would surface most of these.

### Theme D — Incomplete alignment handling in hand-rolled storage/allocation

- **ArchetypeChunkPool** ignores `ComponentDescriptor::alignment` (`:530-541,977`) — arrays/blocks are aligned only to `CACHE_LINE_SIZE` (64), so a component with `alignof > 64` (permitted in Release, Theme A) gets misaligned bases.
- **SmallVector** `Grow`/`shrink_to_fit` use plain `::operator new` (`:576,310`) — not alignment-aware once `T` spills past the (correctly-aligned) inline buffer.
- **ResourceStorage** SBO buffer hardcodes `alignas(64)` while `SBO_SIZE`/`CACHE_LINE_SIZE` can be 128 on ARM64 (`:40,732`) — size and alignment guarantees of the same buffer disagree cross-platform.
- **Delegate** SBO is only 8-byte aligned on MSVC x64 with no functor-alignment check (`Delegate.hpp:52,305`) → placement-new of a 16-aligned functor is UB.
- **CommandBuffer** alignment agreement depends on `std::vector<std::byte>` allocation alignment (8 on 32-bit), asserted only in a comment (`:53-58`).

**Systemic fix:** thread `alignof(T)`/`descriptor.alignment` through every raw-storage and heap-allocation path; use aligned allocation; and reject (in all configs) or statically-assert alignments the storage cannot honor.

### Theme E — Type identity collides for identically-named anonymous-namespace types  *(confirmed on MSVC & Clang)*

`TypeID<T>::Hash()`/`Value()` derive identity from the compiler pretty-name (`TypeID.hpp:148-196`). Two distinct types named `Position` in anonymous namespaces in different TUs render identically, so they collide to the same `ComponentID`/hash — and the Debug collision guard (`ComponentRegistry.hpp:121-135`) is *defeated* because the names match (it reads as a re-registration). Impact: archetype identity and serialization identity conflation. This is pre-existing (also flagged and deferred during the 3.4 remediation) and is not harmfully triggered by the current suite, but it is a real production-correctness limitation.

**Fix:** mix a non-name discriminator into type identity (e.g. the address of a per-type `static` sentinel, or `__COUNTER__`-based registration token), or at minimum document the constraint ("component/reflected types must not live in anonymous namespaces / must have a unique unqualified name") and add a Release-safe collision guard.

### Theme F — Empty / tag (size==0) component asymmetries

`Component::DefaultConstruct` skips `size==0` unconditionally but `Destruct` does not (`Component.hpp:84-140` vs `154-157`) → an empty type with non-trivial ctor/dtor gets skipped construction and UB destruction. Registry by-ID paths drop `ComponentAdded`/`ComponentRemoved` signals for tag components and `Get…ByHash/Name` returns `nullptr` for a tag the entity actually has (`Registry.hpp:470-616`), contradicting `Has…`.

### Theme G — Missing destruct / leaks on move & remove paths

`ArchetypeChunkPool::RemoveEntity` swap-and-pop never `Destruct`s the moved-from source slot (`:353-375`) → leaks non-trivial components (its sibling `MoveEntitiesBetweenChunks` does it right). `FlatSet::Emplace` skips `~T()` on its moved-from probe temporary (`FlatSet.hpp:558`) → a leak every insert for copyable-non-movable `T`. `ArchetypeManager::MoveAndAddByID` has no final `else`, leaving a move-only added component uninitialized (`:1268-1283`).

### Theme H — Portability fragility on non-default configs

`hash >> 57` is UB when `size_t` is 32-bit (`Swiss.hpp:46`, `Entity.hpp:165`). Bare-MSVC never defines `__SSE2__/__SSE4_2__/__AVX2__`, so SIMD silently degrades to scalar unless the consumer's build injects the (reserved-identifier) defines that this repo's `premake5.lua` does (`Platform.hpp:130-152`). Entity version-wraparound bit math is wrong for `ASTRA_ENTITY_VERSION_BITS` not in {8,16,32} (`Entity.hpp:59-61`, `EntityManager.hpp:124-128`), and there is no `static_assert(VersionBits >= 1)`. `std::any` in reflection needs RTTI, but the Dist/Release benchmark preset ships `rtti "off"` (`premake5.lua`).

### Theme I — Iteration holds live references into containers across user callbacks → UAF

- **Relations** `ForEachChild`/`ForEachLink` iterate a live reference into `m_children`/`m_links` (`Relations.hpp:138,188`); a "destroy each child" callback triggers swap-and-pop on the same container mid-iteration → skipped entities and, once heap-promoted, a freed buffer being iterated (heap-UAF).
- **Relations** `ForEach{Descendant,Ancestor}` hold a `const TraversalCache&` across the callback loop (`Relations.hpp:155,172,203`); a nested traversal (FlatMap rehash) or destroying the root mid-callback frees the cache's backing vector while it's read.
- **MulticastDelegate::Invoke** uses a plain range-for (`Delegate.hpp:391-412`); a Signal handler that registers/unregisters during dispatch invalidates the iteration → UAF / null call.
- **View/ViewIterator** (`View.hpp:263-308`): structural mutation mid-iteration silently skips entities via swap-and-pop reordering — undocumented.

**Systemic fix:** for any `ForEach` that invokes a user callback, either snapshot/index by value, or explicitly document and enforce "no structural mutation of the iterated set during iteration." Signal/multicast dispatch should iterate a stable copy or a generation-guarded index.

### Theme J — Public-API footguns (Registry surface)

Lower-severity than the UB themes but high user-impact: `Clear()` swaps in a new `ArchetypeManager`, silently freezing every pre-existing `View`/`Relations` on stale data forever (`Registry.hpp:1004-1017`); `CreateEntities`/`CreateEntitiesWith` return `void` and silently no-op when the output span is too small (`:144-212`); `Registry(const Registry&, Config)` is *not* a copy despite its shape and the project's own `CopyConstructor` test (`:79-87`); batch creation always emits `ComponentAdded` with a `nullptr` component even when real values exist; by-ID batch APIs skip the dead-handle filtering their siblings perform; `View<T>(nullptr)` crashes in the constructor despite null being a supported state elsewhere.

---

## 3. Top must-fix list (calibrated, de-duplicated, ordered)

Ranked by impact × reachability. Items 1–8 are reachable in ordinary Release use.

1. **Convert assert-only guards on recoverable conditions to all-configs checks (Theme A).** Start with `Reflection::FieldInfo` accessors, `ResourceStorage::Set/Emplace`, `Result::operator*`, `Delegate` copy-of-move-only, `ComponentRegistry`/`TypeContext` caps. *(Root of ~8 Criticals.)*
2. **ArchetypeManager root-archetype round-trip (`:698,738-741,805-811`)** — zero-component entities are lost on Save and become dangling map entries → count underflow + OOB on later destroy. Plain data-loss/UB on trusted saves.
3. **Archetype large-component chunk fit-check (`Archetype.hpp:157-161`)** — clamps `entitiesPerChunk` to 1 without verifying one entity fits → Release heap overflow on ordinary large-component registration.
4. **Iteration-across-callback UAFs (Theme I)** — `Relations::ForEach*`, `MulticastDelegate::Invoke`. Triggered by the natural "destroy each child" / self-unregistering-handler patterns.
5. **`View::Size()`/`Empty()` skip the refresh protocol (`View.hpp:143-156`)** → dangling `Archetype*` deref after `Defragment()`. (This was *under-rated* as a "stale-count caveat" during the 3.4 remediation; it is a UAF.)
6. **`CommandBuffer::Execute` rollback destroys already-committed entities (`CommandBuffer.hpp:773-784`)** → orphaned live archetype rows on a mid-Execute failure.
7. **SmallVector self-aliasing insert/push_back (`:372-448`)** — corruption / heap-UAF when the inserted value aliases an element (calibrate against real call sites; std::vector guarantees this works, so it's a latent contract gap).
8. **`Registry::Clear()` orphans live Views/Relations (`Registry.hpp:1004-1017`)** — silent, undetectable stale iteration.
9. **Thread-safety model (Theme B)** — decide, document, and either enforce or de-advertise. Critical for anyone using the parallel paths.
10. **`Registry::Load` hardening + fuzz harness (Theme C)** — Critical if loads are ever untrusted.
11. **Alignment plumbing (Theme D)** and **type-identity collision (Theme E)** — correctness under over-aligned components / anonymous-namespace types.

---

## 4. Strengths (what's genuinely good)

An accurate review credits the parts that hold up under scrutiny:

- **Hash containers are correct and well-fuzzed.** The unusual aligned-group + full-group-scan + linear-group-probe SwissTable is correct because capacity is always a power-of-two multiple of 16; find/insert/erase agree on the probe sequence; the deferred-insert tombstone guard is correct and has a dedicated regression test (`05`). These underpin the whole ECS, so their soundness matters enormously.
- **Type-erasure generation is correct.** Construct/destruct/move/copy thunks are properly paired, and the trivial-vs-nontrivial value-init split is exactly right (`09`).
- **SOA/archetype/chunk/pool architecture is sound**, with careful SBO handling in SmallVector and a genuinely clever self-correcting byte-encoding in CommandBuffer (`01`, `07`, `12`).
- **The live-API relationship rejection contract** (cycles/self-links/invalid entities rejected gracefully) is solid and well-tested (`11`).
- **`Result`'s placement-new storage, Simd's scalar fallbacks, and TypeContext's mutex model** are mechanically sound designs (`06`).
- **The View refresh protocol** (structural/removal/generation counters) is correctly wired into `ForEach`/`ParallelForEach`/`begin()` — the gap is only that `Size()`/`Empty()` bypass it (`10`).
- **Several tests are genuinely discriminating** — the destructor-sentinel signal-lifetime test, the layout-mismatch config test, the tombstone regression test.

---

## 5. Test coverage — aggregate gaps

Happy-path coverage is broad, but the review found near-zero coverage for exactly the failure modes above:

- **No adversarial serialization tests** (oversized lengths, `size*sizeof` overflow, decompression bombs, truncated/garbage LZ4, crafted indices/IDs, cyclic relationship saves). No fuzz harness over `Registry::Load`.
- **No Release-behavior tests for the assert-only paths** — every Theme-A finding is invisible to a Debug-only assert and untested in Release.
- **No concurrency tests** beyond the reference worker pool; the system-scheduling layer (`BuildExecutionPlan`/`HasConflict`/ordering/guard) has *no* dedicated tests.
- **No destructor/leak accounting** for `FlatSet::Emplace`, chunk swap-and-pop, or the delegate/resource move paths (would immediately expose the leaks).
- **No mid-iteration structural-mutation tests** for Views or Relations (would expose the callback UAFs).
- **Result / Memory / Simd have no dedicated unit tests at all.**
- **Reflection**: no coverage for container-typed fields, const-field set, wrong-type accessor, or namespaced-type macros — all of which are broken or unguarded.

A single fuzz target over `Registry::Load`, plus a handful of Release-mode negative tests and destructor-accounting tests, would convert most of these findings from "latent" to "caught."

---

## 6. Relationship to the just-merged 3.4 remediation

The 3.4 branch (merged into `dev` as this review began) correctly fixed a set of *specific* defects, several of which belong to the themes above: one `ResourceStorage::SetByID` OOM path (Theme A), the typed `AddComponent<T>` overflow (Theme A), the `View` cache-invalidation counters (Theme I/B-adjacent), the empty-tag batch UB (Theme F), and default-construction semantics. This review's contribution is to show those were **instances of pervasive patterns** — the same classes recur in `Set<T>`/`Emplace<T>`, the reflection accessors, the delegate copy path, the relationship traversals, and the whole load path. The efficient path forward is pattern-level fixes (Section 3, item 1 especially) rather than another round of instance-by-instance patching.

---

## 7. Suggested phasing

- **Phase 1 (Release safety, no new features):** Theme A conversion sweep + must-fix items 2–8. All are localized, mostly small, and directly close UB reachable in ordinary Release use. Add Release-mode negative tests as you go.
- **Phase 2 (trust boundary):** Theme C — harden `Load`, add the fuzz harness.
- **Phase 3 (concurrency):** Theme B — commit to a threading model, document it, enforce/de-advertise accordingly.
- **Phase 4 (correctness breadth):** Themes D/E/F/G/H and the Theme-J API footguns, prioritized by your actual target platforms and public-API surface.

---

*The 13 detailed per-subsystem chapters follow, in order. Each contains full Design assessment, Strengths, Critical/Important/Minor findings with `file:line` and suggested fixes, and per-subsystem test-coverage gaps.*

---
# Archetype storage (Archetype / ChunkPool / Graph) — Review

## Overview

Scope: `include/Astra/Archetype/Archetype.hpp`, `ArchetypeChunkPool.hpp`, `ArchetypeGraph.hpp` — the chunk memory layout, the block/chunk pool allocator, and the archetype transition graph. This is the memory-layout heart of the ECS.

**Verdict: solid architecture with several genuine memory-safety defects.** The SOA chunk layout, power-of-two capacity, non-intrusive free list, tag/empty-component handling, and the recently-added deserialize fit-guard are well done. But there is a **release-mode heap overflow when a per-entity component footprint exceeds the chunk size** (no fit check; `entitiesPerChunk` is silently clamped to 1), a **swap-and-pop that never destructs the vacated slot** (leak/invariant break), a **coalesce path that returns stale chunk indices** after erasing chunks, and a pool whose atomic decorations imply a thread-safety it does not provide. Deserialize has two robustness gaps (unclamped per-chunk count; capacity desync on config-mismatched load). None of the three critical/near-critical issues are caught by the current tests.

## Design assessment

- **Chunk = pool-owned raw memory holding component SOA arrays only.** The per-chunk entity list (`m_entities`) is a *separate* `std::vector`, not part of the chunk block. This is a reasonable simplification but means "cache-friendly chunk" applies to components, not the entity id column, and it makes capacity bookkeeping split between the chunk (`m_capacity`) and the archetype (`m_entitiesPerChunk`) — the source of finding I3.
- **Capacity math** rounds down to a power of two and derives shift/mask for cheap division/modulo — good. The alignment-overhead estimate is a safe over-estimate of real padding (see Strengths), so *normal* archetypes never overflow; the failure is only at the "doesn't even fit one entity" boundary (C1).
- **Pool** uses a block allocator with a non-intrusive free list of heap-allocated `ChunkNode`s (stable addresses), lazy clearing, and huge-page support. Clean, but single-threaded only despite the atomics.
- **Graph** is a thin double `FlatMap` of non-owning `Archetype*`; correctness depends entirely on the manager invalidating edges on archetype destruction.

## Strengths (file:line)

- `Archetype.hpp:148-164` — the alignment-overhead estimate `(nonEmpty-1)*CACHE_LINE_SIZE` is a strict over-estimate of the true worst-case padding `(nonEmpty-1)*(CACHE_LINE_SIZE-1)`, so for archetypes that fit, `entitiesPerChunk*perEntitySize + realPadding <= chunkSize` always holds; the `offset <= m_chunkSize` assert at `ArchetypeChunkPool.hpp:544` confirms it. The math is sound for the common case.
- `Archetype.hpp:776-797` — Deserialize validates the saved per-chunk layout fits the target pool's chunk size before allocating, returning `SizeMismatch` instead of overflowing. Good defensive addition (and exercised by ConfigPreservationTest).
- `ArchetypeChunkPool.hpp:886,995-1005` — `ChunkNode`s are heap-allocated via `unique_ptr<ChunkNode[]>`, so `m_memoryToNode`'s stored `ChunkNode*` stay valid even if `m_blocks` (a `SmallVector`) reallocates; only indices into `m_blocks` are stored in the node, and those are stable. The reasoning is correct and the `BlockInfo` explicit move ops (889-923) correctly carry the non-movable `atomic`.
- `ArchetypeChunkPool.hpp:868-878` — non-intrusive free list avoids scribbling over live chunk memory; `needsClear` supports lazy zeroing (though defeated — see M1).
- Tag/empty-component handling is consistent and correct: `base=nullptr, stride=0, isValid=true` (510-524), `DefaultConstruct`/`BatchDefaultConstruct` early-return on `size==0` (Component.hpp:86,124), `GetComponentArray` returns `nullptr` for empty types, and `BatchConstructComponent` explicitly refuses to form a pointer from a null base (`ArchetypeChunkPool.hpp:289`). This matches the EmptyTagTest regression fixes.
- `Archetype.hpp:1344-1346` — `MoveEntitiesBetweenChunks` correctly `MoveConstruct`s then `Destruct`s the source, the exact construct/destruct pairing that `RemoveEntity` omits (finding I1) — evidence the omission is a bug, not a convention.

## Findings

### Critical

- **C1 — `Archetype.hpp:157-168` (Initialize): per-entity footprint larger than the chunk overflows chunk memory in release.**
  When `perEntitySize` (or the sum of component sizes) exceeds `remainingSpace`, `maxEntities` becomes `0`, and line 160-161 force `m_entitiesPerChunk = 1` **without verifying that even one entity fits**. `InitializeComponentArrays` (`ArchetypeChunkPool.hpp:510-545`) then lays out `size*1` bytes past `m_chunkSize`; the guard at line 544 is `ASTRA_ASSERT`, i.e. a no-op in release (`Base.hpp:51`). Concretely: register one component with `sizeof(T) = 20 KB` (or two 10 KB components) under the default 16 KB chunk → `entitiesPerChunk=1` → the component array's offset runs to 20 KB inside a 16 KB chunk. Because chunks are packed contiguously in a block (`ArchetypeChunkPool.hpp:1001`), the first `AddEntity`'s `DefaultConstruct` writes into the *next chunk's* memory → cross-archetype heap corruption. Any archetype with per-entity footprint > `MAX_CHUNK_SIZE` (1 MB) overflows regardless of configuration. **Why it matters:** silent memory corruption reachable through ordinary component registration, no malformed input required. **Fix:** compute the real layout size (or reuse the Deserialize check at 793) in `Initialize`; if it exceeds `chunkSize`, leave `m_initialized=false` and return so callers fail gracefully instead of allocating an overflowing chunk.

- **C2 — `Archetype.hpp:925-1018` + `1303-1358` (CoalesceChunks / MoveEntitiesBetweenChunks): returned entity locations become stale after empty-chunk erase.**
  `MoveEntitiesBetweenChunks` records each moved entity as `EntityLocation::Create(destChunkIndex, destEntityIndex)` (line 1326) using indices valid *before* cleanup. `CoalesceChunks` then erases emptied source chunks with `m_chunks.erase(m_chunks.begin()+i)` (1012). Erasing a chunk at index `i` shifts every chunk after it down by one, so any `destChunkIndex > i` recorded in `allMovedEntities` now points at the wrong chunk (or past the end). This is reachable: when chunk 0 is full and a middle sparse chunk's entities are packed into a *higher-indexed* dest chunk (dest loop starts at 0 and takes the first with room — `979-997`), then the drained middle chunk is erased and the dest shifts. The manager consumes these `(Entity, EntityLocation)` pairs as "entity is now here" and writes them into its entity→location map → later component access resolves to the wrong chunk/slot → out-of-bounds or wrong-entity reads. **Why it matters:** memory-unsafe location corruption from a public maintenance API. **Fix:** either erase before recording (record final indices), remap recorded indices through the erase, or compact without `erase` (swap emptied chunks to the back). Note the ChunkCoalescing test (ArchetypeTest.cpp:537) never validates the returned locations, so this is uncaught.

### Important

- **I1 — `ArchetypeChunkPool.hpp:353-375` (Chunk::RemoveEntity): swap-and-pop never destructs the moved-from source slot.**
  In the `index != lastIndex` branch, each component is `Destruct(dstPtr)` then `MoveConstruct(dstPtr, srcPtr)`, but `srcPtr` (the old last slot) is left moved-from and **not destructed**; `--m_count` then puts it outside `[0,m_count)`, so `~Chunk` (92-109, which only destructs `[0,m_count)`) never reclaims it either. For trivially-copyable components this is harmless (memcpy leaves identical bytes). For non-trivial components whose moved-from state still owns a resource — including any type where "move" resolves to a copy — this leaks on every middle removal and permanently at chunk destruction. The sibling `MoveEntitiesBetweenChunks` (1344-1346) does it correctly, confirming the omission. **Fix:** add `info.descriptor.Destruct(srcPtr);` after the `MoveConstruct` in the `index != lastIndex` branch.

- **I2 — `Archetype.hpp:814-838` (Deserialize): per-chunk `chunkEntityCount` is never clamped to `entitiesPerChunk`.**
  `chunkEntityCount` is read from the stream (817) and drives an unbounded `AddEntity` loop (833-838). `Chunk::AddEntity` only guards capacity with `ASTRA_ASSERT` (`ArchetypeChunkPool.hpp:113`), so in release a `chunkEntityCount` larger than the chunk's capacity writes component data past the chunk (via `DefaultConstruct` at index `>= capacity`) → heap overflow. The neighboring code deliberately added the `entitiesPerChunk`-fits guard (776-797) "instead of overflowing chunk memory," so the missing count clamp is an inconsistency in the same function. The Registry layer's checksum (FormatV2Test) catches *random* corruption, but `Archetype::Deserialize` is a public static that tests call directly with a raw `BinaryReader` and no checksum, and a crafted-but-checksum-valid stream defeats the higher layer. **Why it matters:** OOB write on malformed/untrusted input on a path meant to fail cleanly. **Fix:** `if (chunkEntityCount > entitiesPerChunk) return Err(SizeMismatch);` before the read loop.

- **I3 — `Archetype.hpp:800-825` (Deserialize): `m_entitiesPerChunk` (from Initialize) can diverge from the chunks' real capacity (from stream).**
  Line 802 calls `Initialize`, which sets `m_entitiesPerChunk` from the *current* pool's chunk size; lines 810/825 then discard the pre-made chunk and create chunks with the *stream's* `entitiesPerChunk`. When the load pool's chunk size is **larger** than the save's (e.g. save 16 KB, load 64 KB), the stream value fits the 793 guard but is smaller than what Initialize computes, so `m_entitiesPerChunk` (large) > deserialized chunk capacity (small). Afterward, `GetRemainingCapacity`/`AddEntities`/`BatchMoveEntitiesFrom` compute `available = m_entitiesPerChunk - chunk->GetCount()` (e.g. 227, 1101) and over-report room in the smaller chunks; `BatchAddEntities` (`ArchetypeChunkPool.hpp:196`) then writes past the chunk in release. Also newly-created chunks use `m_entitiesPerChunk` (large), so the archetype ends with mixed capacities. ConfigPreservationTest covers same-config and smaller-config(reject) but not larger-config. **Fix:** after Initialize, set `m_entitiesPerChunk`/shift/mask from the stream value (and recompute), or require the two to match.

- **I4 — `ArchetypeChunkPool.hpp` (whole pool): not thread-safe despite atomic decorations implying otherwise.**
  `usedChunks`, `m_totalChunks`, and the stats are `atomic`, and comments say "atomic increment for thread safety" (679, 941), but the actual allocation state — `m_freeList` (raw pointer, mutated at 685-686, 930-931), `m_blocks` (push_back/erase), and `m_memoryToNode` (Insert/Erase) — is touched with no synchronization in `AcquireMemory`/`ReturnChunk`/`AllocateBlock`/`Defragment`. Concurrent `CreateChunk` and chunk destruction from two threads race on `m_freeList` → UB (lost nodes, double hand-out). The `Defragment` "block became non-empty after our check" fallback (811-815) is only meaningful under concurrency it cannot actually support. **Why it matters:** the project advertises "optional multithreading"; the atomics are a footgun that reads as safe. **State the real guarantee:** the pool requires external synchronization; it is single-threaded/one-writer only. Either drop the misleading atomics or add a real lock.

- **I5 — `ArchetypeChunkPool.hpp:530-541, 977` (InitializeComponentArrays / block alignment): over-aligned components are laid out under their required alignment.**
  Component arrays are placed only at `CACHE_LINE_SIZE`-aligned offsets, and blocks are allocated with `BLOCK_ALIGNMENT = CACHE_LINE_SIZE` (64 on x64). `ComponentDescriptor::alignment` (Component.hpp:53) is **never consulted** for layout. A component declared `alignas(128)` (or any alignment > `CACHE_LINE_SIZE`; note `CACHE_LINE_SIZE` is 64 on x64 but the code's own comment flags the POSIX `posix_memalign` cap) gets a base aligned to only 64 → every access is misaligned → UB, and hard faults for aligned SIMD loads. The comment at 973-976 acknowledges the 64-byte cap but nothing rejects or honors larger alignments. **Fix:** align each array offset to `max(CACHE_LINE_SIZE, desc.alignment)`, align the block to the max component alignment across the archetype, and/or reject archetypes whose max component alignment exceeds the block alignment.

- **I6 — `Archetype.hpp:155,168,715` (Initialize / Deserialize): null `m_chunkPool` is treated as valid at 155 but dereferenced unconditionally at 168 → crash.**
  Line 155 defensively falls back to `DEFAULT_CHUNK_SIZE` when `m_chunkPool` is null, but line 168 then calls `m_chunkPool->CreateChunk(...)` with no null check → null dereference. `Deserialize`'s signature defaults `componentPool = nullptr` (715); calling it without a pool passes the 793 guard (which also handles null) and then crashes inside `Initialize` at 168. `Initialize` is `void` and cannot report the error. **Why it matters:** a public API with a `nullptr` default argument crashes instead of returning `Result::Err`, violating the graceful-error contract. **Fix:** guard the pool in `Initialize` (fail `m_initialized=false` if null), and have `Deserialize` return an error for a null pool rather than proceeding.

### Minor

- **M1 — `ArchetypeChunkPool.hpp:506` vs `934-938`:** the `Chunk` constructor unconditionally `memset`s the whole `m_chunkSize`, which fully defeats the pool's `needsClear` lazy-clear optimization (AcquireMemory already zeroed fresh/returned memory). Either drop the ctor memset (rely on the pool's zeroing + per-slot `DefaultConstruct`) or drop the lazy-clear machinery; keeping both is redundant work on the hot allocation path.
- **M2 — `Archetype.hpp:157`:** a component-less archetype gets `entitiesPerChunk=256` and allocates a full `chunkSize` block that stores nothing (all data lives in `m_entities`). Wasted memory per empty-archetype chunk; consider not acquiring pool memory when `perEntitySize == 0`.
- **M3 — `Archetype.hpp:1007-1015` (CoalesceChunks):** never resets `m_firstNonFullChunkIndex` after erasing chunks; if it ends `>= m_chunks.size()`, the next `GetOrCreateChunk` (1254) skips the scan loop and allocates a new chunk even though space exists. Minor perf/space.
- **M4 — `Archetype.hpp:627-630` (GetFragmentationLevel):** `m_chunks.size() - optimalChunkCount` is unsigned; if `m_entityCount` ever exceeds `chunks*perChunk` (e.g. after an I3 desync) this underflows to a huge value cast to float. Guard with `optimalChunkCount <= m_chunks.size()`.
- **M5 — `Archetype.hpp:655-657,642` (Serialize):** writes `chunkCount = m_chunks.size()` but the loop `continue`s past any null chunk, so a null chunk would desync the stream vs. what Deserialize reads. Chunks are never null in practice; still, count and emitted records should match (count non-null first, or assert).
- **M6 — `Archetype.hpp:700-710` vs `879-898` (Serialize/Deserialize):** a non-trivially-copyable component with **no** serialize hook makes Serialize's `else` branch write nothing (only a debug-only assert) and Deserialize's chain skip it entirely — a symmetric skip that keeps the stream aligned but silently drops the component's data (values load as default). Depends on the registry always providing hooks for non-trivial types; if it can't, this is silent data loss with no release-mode signal.
- **M7 — `ArchetypeGraph.hpp:23-39,127-128`:** the graph holds non-owning `Archetype*` and relies on the manager calling `RemoveEdgesTo`/`RemoveEdgesFrom` on archetype destruction — a missed call leaves a dangling edge that `GetAddEdge` hands back (UAF). Also `edges.Insert(make_pair(...))` uses `FlatMap::Emplace`, which does not overwrite an existing key (FlatMap.hpp:600-608), so `SetAddEdge` cannot *update* an edge that already exists for a `(from, componentId)`. Document the ownership/lifetime contract; use `operator[]`/erase-then-insert if updates are ever needed. (`RemoveEdgesToInternal`'s `it = Erase(it)` loop is correct — `FlatMap::Erase(iterator)` tombstones and returns `++pos`, FlatMap.hpp:630-642.)
- **M8 — `ArchetypeChunkPool.hpp:57-74,601-638` (ChunkDeleter / pool move):** moving an `ArchetypeChunkPool` while chunks are outstanding leaves those chunks' `ChunkDeleter::pool` pointing at the moved-from (now-empty) pool; on destruction `ReturnChunk` finds nothing in `m_memoryToNode` (asserts in debug, no-ops in release), so the memory is not returned to the moved-to pool and `usedChunks`/`freeChunks` stats drift. No UAF/double-free (the moved-to pool still owns and frees the block), but the chunk is leaked from the pool's reuse accounting until pool destruction. Consider deleting the pool move ops or documenting "don't move a pool with live chunks."

## Test coverage

**Covered well:** basic create/init, single/multi/batch add, single & batch remove (swap-and-pop), get/set, `ForEach` (full and partial), multi-chunk allocation and chunk boundaries, `EnsureCapacity`, `GetRemainingCapacity`, `MoveEntityFrom` transition, empty (no-component) archetype, tag/empty components (EmptyTagTest: view binding, migration/removal, batch-add tag), serialization round-trips (empty, entities+data, multiple chunks, non-trivial `Name`/string), and the same-config / smaller-config-reject load paths (ConfigPreservationTest), plus format v2 header/checksum/round-trip.

**Concrete gaps (map to findings):**
- **No test that a component (or component set) larger than the chunk fails gracefully** — C1 is entirely untested; a `sizeof(T) > chunkSize` archetype would corrupt memory in release.
- **`CoalesceChunks` is called but its returned locations are never validated** (ArchetypeTest.cpp:577-589 only checks `chunksFreed>0` and a `ForEach` count) — C2's stale indices slip through. Add a test that, after coalescing, every `(Entity, EntityLocation)` returned resolves via `GetEntity(location)` back to that entity.
- **No larger-chunk-on-load test** (save 16 KB → load 64 KB) — I3's capacity desync is unexercised; add a round-trip that loads into a bigger pool and then batch-adds enough entities to force `BatchAddEntities` on a deserialized chunk.
- **No malformed-stream test at the `Archetype::Deserialize` level** (oversized `chunkEntityCount`, bogus descriptor sizes) — I2 is unexercised; the direct static API has no checksum shield.
- **No leak/destruction-accounting test for non-trivial components under swap-remove of a middle element** — I1's missed `Destruct` needs a component with an instrumented dtor (count ctor/dtor calls; assert balanced after middle removals and chunk teardown).
- **No pool exhaustion test** (`maxChunks` reached → `CreateChunk` returns `nullptr` → `AddEntity`/`AddEntities` return invalid/partial) — the exhaustion and partial-batch paths (Archetype.hpp:213-216, 1080-1083) are untested.
- **No `Defragment` test** — block release, free-list rebuild, `blockIndex` reindexing (ArchetypeChunkPool.hpp:714-865) are entirely uncovered.
- **No over-aligned component test** — I5's alignment gap is invisible without an `alignas(128)` component and an alignment assertion on the array base.
- **Thread-safety is untested and unsupported** — consistent with I4; if concurrency is ever intended for the pool, it needs a stress test (and a lock first).
# ArchetypeManager — Review

Scope: `include/Astra/Archetype/ArchetypeManager.hpp` (entity↔archetype map, add/remove/move,
batch ops, archetype create/cleanup, Serialize/Deserialize). Traced against `Archetype.hpp`,
`ArchetypeGraph.hpp`, `Component(Registry).hpp`, `Bitmap.hpp`, `Entity.hpp`, `View.hpp`,
`Registry.hpp`, and the four named tests. Read-only audit; nothing modified.

## Overview

`ArchetypeManager` owns the authoritative map `m_entityMap : unordered_map<Entity, {Archetype*, EntityLocation}>`,
the archetype vector `m_archetypes` (root at index 0), the mask→archetype `FlatMap` `m_archetypeMap`,
and the transition edge cache `m_edgeGraph`. Archetype transitions are done by allocating a slot in
the destination archetype, move-constructing shared components, then swap-and-pop removing from the
source (single-entity `MoveEntityInternal`; batch `BatchMoveEntitiesInternal`). Two atomics
(`m_structuralChangeCounter`, `m_archetypeRemovalCounter`) plus a non-atomic `m_generation` drive
`View` cache invalidation.

## Design assessment

The swap-and-pop bookkeeping is the core risk area and is mostly handled correctly: descending-order
removal (Archetype::RemoveEntities) plus "update moved entity's map location" keeps the map coherent,
and `std::unordered_map`'s node stability means the `EntityRecord&` held across `m_entityMap[...]`
inserts in `MoveEntityInternal` is NOT invalidated (correct, though subtle and undocumented). Empty/tag
components are handled by the `base == nullptr → continue` guard in every move loop, matching
`EmptyTagTest`. The big structural weakness is **serialization**: the root archetype is skipped on
write and never repopulated on read, yet it holds real entities. Secondary weaknesses are
inconsistent null/again guards on the remove paths, an assert on a recoverable OOM, and the usual
"header-only, zero synchronization" story that the atomic counters can make look safer than it is.

## Strengths

- `MoveEntityInternal` (964–986): destination slot allocated *before* source removal; on allocation
  failure it returns early leaving the entity fully intact in the source archetype — no half-moved state.
- Descending-order swap-and-pop in `RemoveEntities` (Archetype 367–428) + the moved-entity relink loops
  (198–199, 228–235, 1183–1191) keep `m_entityMap` consistent even when a non-batch entity is swapped
  into a vacated slot. Traced several interleavings; final positions are correct.
- Component-ID overflow is guarded on the add paths (268–269, 295–296) and registration refuses
  `id >= MAX_COMPONENTS` (ComponentRegistry 103–109); `Bitmap::Test` is bounds-checked (Bitmap 53–60),
  so even the unguarded remove paths can't OOB the mask.
- `CleanupEmptyArchetypes` correctly tears down all dangling references (archetypeMap, both edge
  directions) *before* freeing, resets `unique_ptr`s in place (indices stay valid), then compacts —
  and bumps both counters so Views re-collect (658–685). Matches `ViewInvalidationTest.SurvivesArchetypeRemoval`.
- Invalid/dead handles are rejected early (64, 92, 117–137) — `EntityValidityTest` passes.

## Findings

### Critical

**C1 — Zero-component (root) entities are silently dropped by Serialize/Deserialize, leaving dangling
map entries → lost from iteration and UB on later destroy. (`Serialize` 698; `Deserialize` 738–741, 805–811)**

`Serialize` writes archetypes with `for (size_t i = 1; i < m_archetypes.size(); ++i)` — index 0 (root)
is skipped, so root's chunk contents (its Entity array) are never written. `Deserialize` keeps root
(`while (m_archetypes.size() > 1) pop_back()`) but never repopulates it. The entity-map section, however,
*does* iterate the whole `m_entityMap`, so every root entity is written with `archetypeIndex == 0`
(719–727) and on load is restored as `m_entityMap[E] = {root, {chunkIndex, entityIndex}}` (805–811)
pointing into a root chunk that has `GetCount() == 0`.

Reachability is first-class: `Registry::CreateEntity()` / `CreateEntity<>()` with no components routes
to the root archetype (`AddEntity<>` → `m_rootArchetype`, 68–71; Registry 92–99). Concrete failure:
```
auto e = reg.CreateEntity();      // zero components → root slot {0,0}
auto bytes = reg.Save();          // root skipped; map entry for e written w/ archIndex 0
auto r2 = Registry::Load(bytes,...);
// e now maps to root {0,0} but root has 0 entities:
//   - View<>/Size(): root count 0 → e is invisible (silent data loss)
//   - reg.DestroyEntity(e) → root->RemoveEntity({0,0}) → chunk RemoveEntity(0) on empty chunk
//     (OOB swap-and-pop) and --m_entityCount underflows size_t → memory corruption / UB
```
(Note: entities that reach an *empty* mask via `RemoveComponent` land in a *separate* empty-mask
archetype created by `GetArchetypeWithRemoved` — that one is at index ≥ 1 and DOES round-trip. Only the
true root, populated by `AddEntity<>`, is affected. The two coexisting empty-mask archetypes are
themselves a design smell.)
Fix: serialize/deserialize the root archetype like any other (include index 0), or explicitly persist
root entities in the map section and re-`AddEntity` them into root on load.

### Important

**I1 — Remove paths never null-check the target archetype → null-deref when the registry weak_ptr is
expired. (`RemoveComponent` 333–334; `RemoveComponents` 363–364; `RemoveComponentByID` 465–466)**

`GetArchetypeWithModified` returns `nullptr` when `m_componentRegistry.lock()` fails while it must
*create* the target archetype (888–892). The add paths are safe because they lock the registry at the
top and hold it (`AddComponent` 258–260, `AddComponents` 290–292), guaranteeing creation succeeds. The
remove paths do neither: `RemoveComponent` calls `MoveEntity(entity, oldLoc, newArchetype)` with an
unchecked `newArchetype`; `MoveEntityInternal` immediately does `newArchetype->AllocateEntitySlot(...)`
→ null dereference. `RemoveComponents` → `BatchMoveEntitiesWithoutComponent` →
`dstArchetype->BatchMoveEntitiesFrom(...)` on null, same result. Reachable in standalone manager use
(this is a public class; `EntityValidityTest` drives it directly):
```
auto creg = std::make_shared<ComponentRegistry>();
ArchetypeManager mgr(creg);
// add entity with {A,B}; target {A} archetype not yet created
creg.reset();                 // weak_ptr now expired
mgr.RemoveComponent<B>(e);    // creates {A} → lock() fails → nullptr → deref
```
Fix: null-check the archetype before moving (return `false`/invalid), mirroring `AddComponentByID`'s
guard at 408–409.

**I2 — Assert on recoverable OOM in batch move aborts debug builds. (`BatchMoveEntitiesInternal` 1163–1169)**

`if (newLocations.empty() && !entityBatch.empty()) { ASTRA_ASSERT(false, "Failed to allocate chunks..."); return; }`
Chunk-allocation failure is a caller-recoverable/out-of-memory condition, not an internal invariant, yet
`ASTRA_ASSERT` compiles to `assert` in debug (Base.hpp 49) and will `abort()`. This violates the project
rule "never assert on recoverable errors." (Behavior is otherwise graceful — the early `return` leaves
entities in the source, map untouched — so just drop the assert.) Same pattern, lower reach:
`ASTRA_ASSERT(false, "Failed to allocate chunks for batch move operation")` is the only assert here that
fires on runtime OOM.

**I3 — `MoveAndAddByID` leaves a move-only / non-copyable added component *uninitialized*. (1268–1283)**

The new-component branch is `if (is_trivially_copyable) memcpy; else if (constructWith) …; else if
(copyConstruct) …;` with **no final `else`**. For a component that is neither trivially copyable nor
copy-constructible (move-only type), `ComponentRegistry` sets `constructWith == copyConstruct == nullptr`
(158–169), so none of the branches run and the freshly allocated slot is left with indeterminate bytes.
That slot is later read and, on the entity's eventual removal, `Destruct`ed → UB. The safe wrapper
`ComponentDescriptor::ConstructWith` (Component.hpp 104–120) falls back to `defaultConstruct`; the
dead `BatchMoveEntitiesWithComponentByID` (1313) even uses it — but the live single-entity
`AddComponentByID`→`MoveAndAddByID` path open-codes the chain and omits the fallback. Reachability is
narrow (move-only component pushed through the CommandBuffer type-erased path), hence Important not
Critical. Fix: add `else newDesc.DefaultConstruct(dstPtr);` (or call `newDesc.ConstructWith`).

**I4 — `Deserialize` accepts entity locations without validating them against the archetype's real
extents → OOB on untrusted/mismatched saves. (805–811)**

The only check is `archetypeIndex < m_archetypes.size()`. `chunkIndex`/`entityIndex` are copied verbatim
into the map with no comparison to the target archetype's chunk count or per-chunk entity count. `Load`
is a public entry point for arbitrary bytes; the format checksum (FormatV2Test) only detects *random*
corruption, not a crafted-but-consistent payload (an attacker controls the checksum too). A save with a
valid `archetypeIndex` but out-of-range `entityIndex` yields a map entry whose `GetComponent`/
`RemoveEntity` reads past the chunk. Fix: validate `chunkIndex < archetype->GetChunkCount()` and
`entityIndex < archetype->GetChunkEntityCount(chunkIndex)`; reject (return false) otherwise.

**I5 — `AddEntity`/`AddEntityWith` on an already-present entity orphans its old slot and duplicates it.
(84, 96–103)**

Neither method checks whether `entity` is already in `m_entityMap`; both do
`m_entityMap[entity] = {archetype, location}` after unconditionally allocating a *new* chunk slot. Adding
the same entity twice leaves two chunk slots carrying that Entity value while the map tracks only the
second: the first is an orphan that a `View` iterates as a ghost (entity appears twice / with stale
component data) and that leaks until archetype teardown. Registry::CreateEntity always draws fresh
handles so it isn't hit there, but the method is public and unguarded. Fix: at minimum
`ASTRA_ASSERT(!m_entityMap.contains(entity))`, ideally early-return on duplicate.

**I6 — Concurrency: no synchronization; the atomic counters imply a safety that does not exist, and
`m_generation` is read racily by `View`. (1327–1329; View.hpp 201–229)**

Actual guarantee: `ArchetypeManager` is single-writer only. Every mutator races on `m_entityMap`
(unordered_map), `m_archetypes` (vector realloc), `m_archetypeMap`/`m_edgeGraph` (FlatMap), and chunk
storage. `m_structuralChangeCounter`/`m_archetypeRemovalCounter` are atomic *only* to let a single
consumer thread detect "did the structure change" for View cache invalidation — they do not guard any of
the containers, so concurrent `View::ForEach` during any structural change is UB (dangling `Archetype*`
after vector realloc/cleanup, torn chunk reads). Additionally `m_generation` (plain `uint32_t`, 1329) is
written by creators (`++m_generation`) and read non-atomically by `View::EnsureArchetypes` (View.hpp
229/217) — a data race even for the "one mutator + one iterator" pattern the atomics seem to bless.
Fix: document "structural mutation must be externally serialized against all iteration"; make
`m_generation` atomic for consistency with the other two counters (or route the whole decision through a
single atomic epoch).

### Minor

- **Dead code, pokes friend internals:** `MoveEntitiesWithComponent` (1054–1101, directly mutates
  `dstArchetype->m_firstNonFullChunkIndex`/`m_chunks`) and `BatchMoveEntitiesWithComponentByID`
  (1300–1316) are never called (grep-confirmed). Untested, and the first duplicates the swap-and-pop
  logic — a latent divergence/maintenance hazard. Remove or cover.
- **Misleading comment:** `RemoveComponent` line 337 says "component destroyed but entity couldn't be
  moved" — in `MoveEntityInternal` allocation failure returns *before* any destruction; nothing is
  destroyed and the entity keeps the component. Comment overstates severity.
- **`Serialize` is O(archetypes × entities):** the per-entity linear scan to find `archetypeIndex`
  (719–727) is quadratic for large worlds. Precompute an `Archetype*→index` map once.
- **Silent drop on OOM:** `AddEntities`/`AddEntitiesWith` map only `locations.size()` entries; if the
  archetype's chunk allocation fails partway (Archetype 213–217, 286–288) the surplus entities are added
  to neither chunk nor map — silently lost with no error surfaced (160–164, 177–181). Acceptable
  degradation but undetectable by the caller.
- **Duplicate entities within a single batch are unguarded:** `AddComponents`/`RemoveComponents`/
  `RemoveEntities` group by looking up each entity independently (206–215, 1104–1124); a repeated entity
  yields two slots / double-processed component data / stale-location removal. Caller misuse, but no
  assert.
- **`SetEntityLocation` (250–253)** is public and overwrites the map unconditionally with no validation —
  a footgun if called outside the coalesce path that needs it (Registry 1146).
- **`RemoveComponent` lacks the `id >= MAX_COMPONENTS` guard** that the add paths have (322 vs 268);
  benign only because `Bitmap::Test` bounds-checks. Inconsistent with the "guard overflow loudly" intent.
- **`GetArchetypeMemoryUsage` (543–554)** adds `sizeof(size_t) * MAX_COMPONENTS * 2` per archetype as a
  rough placeholder — over/underestimates real overhead; document as an estimate.

## Test coverage

Present: single/batch add & remove, archetype transitions and edge caching, component-data preservation
across transitions, move-only components (single path, `MoveOnlyComponents`), empty/tag components
(`EmptyTagTest`), cleanup/defragment, invalid-handle rejection and dead-handle batch skip
(`EntityValidityTest`), and View invalidation on add-to-empty-archetype and archetype removal
(`ViewInvalidationTest`).

Concrete gaps:
- **No serialization test with zero-component (root) entities** — exactly the shape that triggers C1.
  `FormatV2Test` only ever creates entities *with* a component. Add: `CreateEntity()` ×N, save/load,
  assert count survives and `DestroyEntity` post-load doesn't corrupt.
- **No round-trip test that walks entity locations after load** (only `Size()` is checked); an
  out-of-range `entityIndex` (I4) would pass current tests.
- **No expired-registry test** for the remove paths (I1) — would surface the null-deref.
- **No move-only component via `AddComponentByID`/CommandBuffer** (I3) — the typed `MoveOnlyComponents`
  test does not exercise the type-erased branch that skips initialization.
- **No OOM / chunk-allocation-failure test** — I2's assert-abort and the silent-drop behavior are
  unexercised.
- **No duplicate-entity-in-batch or double-`AddEntity` test** (I5, batch-duplicate minor).
- **No concurrency/TSan test** asserting the documented single-writer contract (I6).
# Serialization & Compression — Review

Scope reviewed (fully traced):
- include/Astra/Serialization/BinaryArchive.hpp
- include/Astra/Serialization/BinaryReader.hpp
- include/Astra/Serialization/BinaryWriter.hpp
- include/Astra/Serialization/SerializationError.hpp
- include/Astra/Serialization/Compression/Compression.hpp
- include/Astra/Serialization/Compression/LZ4Decoder.hpp
- include/Astra/Serialization/Compression/Internal/smallz4.hpp

Cross-referenced (context, not in scope): Core/Simd.hpp (HashCombine / PortableHashCombine),
Core/Result.hpp, Core/TypeID.hpp, Registry/Registry.hpp (Save/Load), Archetype/Archetype.hpp
(the only real caller of WriteCompressedBlock/ReadCompressedBlock), and the serialization tests.

## Overview

The subsystem is a header-only binary archive: a 32-byte packed `BinaryHeader` (magic, version,
endianness byte, counts, a 32-bit `dataChecksum`, compression mode) followed by a stream of
type-dispatched writes/reads via `operator()`. Integrity is a running 32-bit hash over
everything after the header, verified at the end of load. Optional per-block LZ4 compression is
provided by a vendored `smallz4` encoder and a hand-written `LZ4Decoder`. Format version is 2;
v2 switched to an ISA-feature-independent ("Portable") checksum and explicit `uint64` container
sizes.

The real data path is: `Registry::Save` → `WriteHeader` → managers serialize → `FinalizeHeader`
(back-patches the checksum). `Registry::Load` → `ReadHeader` → managers deserialize → `VerifyChecksum`.
Compression is only exercised for POD component arrays in `Archetype::Serialize/Deserialize` via
`Write/ReadCompressedBlock`.

## Design assessment

Reasonable shape, and several things are done right: every `ReadBytes` is length-checked against
the buffer, `std::string` reads pre-validate the length against the remaining bytes, non-POD
containers have element-count caps, the LZ4 back-reference offset is validated against the current
output size, the checksum genuinely gates load acceptance, and the whole thing is `Result`-based
with zero `throw`/`try`/`catch` (matches the exceptions-off constraint).

But for a component whose stated job is "maximally paranoid parsing of untrusted/malicious bytes,"
the defensive posture is inconsistent. Three recurring anti-patterns undercut it:

1. **Overflowable bounds checks** of the form `ptr + attacker_len > end` and `pos + size > size_`.
   These are the textbook way a bounds check gets silently defeated (pointer/size_t wrap). Benign on
   LP64 for the uint32-sourced lengths, but memory-unsafe on ILP32/32-bit — a supported config.
2. **Allocations sized directly from stream values** *before* any sanity check
   (`vector(originalSize)`, `vector(compressedSize)`, `vec.resize(size)`), which with exceptions
   disabled means a malformed length field is a guaranteed `std::terminate` (crash-DoS).
3. **The one guard that tries to prevent (2)** — `size * sizeof(T) > remaining` — is itself an
   unchecked multiply that overflows, so it is defeated by exactly the input it exists to stop.

The LZ4 decode path additionally has no decompression-bomb ceiling even though the exact output
size is known and passed around. Endianness is handled by *rejection* (the format is same-endian
only), which is a legitimate but under-advertised limitation, and the "portable" checksum is
actually endian-dependent — only safe because same-endian is enforced.

## Strengths (file:line)

- BinaryReader.hpp:100 — every `ReadBytes` checks `m_position + size > m_size` before copying
  (sound on 64-bit).
- BinaryReader.hpp:279 — `std::string` read validates `len > m_size - m_position` *before*
  `resize`, i.e. it does not trust the length. This is the correct pattern the other readers omit.
- BinaryReader.hpp:314-319, 428-432, 487-491, 515-519 — non-POD vectors/maps/sets are capped at
  1M/10M elements, limiting one-by-one read loops.
- LZ4Decoder.hpp:98-99, 271-272 — match offset validated (`offset == 0 || offset > output.size()`),
  preventing backward OOB reads; the overlapping RLE copy (121-125, 290-294) indexes fresh each
  iteration so it stays in-bounds and reallocation-safe.
- LZ4Decoder.hpp:79-81, 117-118 — the *bounded* `Decompress` caps output against
  `uncompressedSize` on both literal and match copies (this is the safe routine — but see I3, it is
  not the one actually wired into the frame path).
- BinaryReader.hpp:153-169 — header rejects bad magic, unsupported version, and endianness mismatch
  before trusting any field; checksum verification (718-733) is actually invoked and propagated by
  `Registry::LoadInternal` (Registry.hpp:1580-1584), confirmed end-to-end by FormatV2Test.
- Simd.hpp:774-783 — `PortableHashCombine` is a fixed MurmurHash3 finalizer, feature-independent
  (unlike `HashCombine`, which is hardware CRC32C when available), which is the right call for a
  cross-machine archive checksum.
- No `throw`/`try`/`catch` anywhere in the subsystem (verified) — conforms to exceptions-off.

## Findings

### Critical

**C1 — Bounds checks use overflowable pointer/size arithmetic with stream-controlled lengths;
defeated on 32-bit → OOB read (memory unsafety).**
`BinaryReader::ReadBytes` (BinaryReader.hpp:100) guards with `m_position + size > m_size`, and the
LZ4 decoders guard with `src + literalLength > srcEnd` (LZ4Decoder.hpp:76, 253),
`src + 2 > srcEnd` (92, 265), and `src + blockSize > srcEnd` (187). All of these add an
attacker-controlled length to a pointer/`size_t` and compare against the end.
- Why it fails: on an ILP32/32-bit build (nothing in the project constraints excludes 32-bit —
  MSVC/GCC/Clang all target it), `size`/`literalLength`/`blockSize` can be driven near or past
  2^32 (e.g. `literalLength` accumulates 255 per `0xFF` byte, so ~16 MB of `0xFF` in a block yields
  >2^32; `blockSize` is a raw 31-bit field; `originalSize`/`compressedSize` are full uint32). The
  addition wraps, the comparison yields "in bounds," and the subsequent
  `memcpy`/`output.insert(src, src+literalLength)` reads far out of bounds. Pointer overflow is also
  UB in the abstract machine, so even on 64-bit a sufficiently aggressive compiler is entitled to
  mis-optimize these comparisons (UBSan flags all of them).
- Fix: compare against the *remaining* count without moving the pointer, e.g.
  `if (size > m_size - m_position)` (as the string reader already does at line 279), and for LZ4
  `if (literalLength > size_t(srcEnd - src))`, `if (blockSize > size_t(srcEnd - src))`, etc. Never
  form `ptr + untrusted` before validating.

### Important

**I1 — Heap allocations sized directly from untrusted length fields, before any bounds check →
guaranteed `std::terminate` (crash-DoS) with exceptions off.**
`ReadCompressedBlock` does `std::vector<uint8_t> data(originalSize);` (BinaryReader.hpp:206) and
`std::vector<uint8_t> compressedData(compressedSize);` (219) where both are raw `uint32` read from
the stream — up to ~4 GB — and are allocated *before* the `ReadBytes` length check runs. A 40-byte
crafted archive claiming `originalSize = 0xFFFFFFFF` forces a 4 GB allocation + zero-fill; with
exceptions disabled the resulting `std::bad_alloc` becomes `std::terminate` (hard process kill).
Same shape in the POD-vector path (`vec.reserve`/`vec.resize(size)` at 323/328 once the guard I2 is
bypassed) and in `unordered_map`/`unordered_set` (`reserve(size)` at 465/523 with `size` up to 10M
→ hundreds of MB of buckets from a tiny truncated file). This is the single most impactful
robustness gap for the "parse malicious input" mandate.
- Fix: before allocating, clamp every stream length against remaining bytes (compressed data can
  never exceed `GetRemaining()`; uncompressed `originalSize` against a caller-supplied ceiling or
  remaining bytes). Prefer incremental growth over one-shot `reserve(streamValue)`.

**I2 — `size * sizeof(T)` integer overflow defeats the only vector bounds guard.**
BinaryReader.hpp:304: `if (size * sizeof(T) > (m_size - m_position))`. `size` is `uint64` from the
stream; `size * sizeof(T)` overflows `uint64` (e.g. `size = 2^61`, `sizeof(T) = 8` → product
wraps to 0), so the guard passes for an absurd `size`. Control then reaches
`vec.resize(size)`/`ReadBytes(vec.data(), size*sizeof(T))` (328-329) — the same product overflows
again — and `resize(2^61)` terminates (feeds directly into I1). The guard exists precisely to stop
this and is nullified by the exact class of input the prompt says to hunt for.
- Fix: check `size > (m_size - m_position) / sizeof(T)` (division can't overflow), and reject before
  `reserve`/`resize`.

**I3 — LZ4 decompression bomb: the frame decoder ignores the known output size and imposes no
output ceiling.**
`ReadCompressedBlock` already knows `originalSize` (BinaryReader.hpp:193) but hands the compressed
bytes to `Compression::DecompressBlock` → `DecompressLZ4` → `LZ4Decoder::DecompressFrame`
(LZ4Decoder.hpp:141), whose private `DecompressBlock` (216) grows `output` with **no cap**
(`reserve(compressedSize * 3)` then unbounded `push_back`). Match lengths are attacker-controlled
(15 + 255·k), so a small compressed block can expand to hundreds of MB/GB before the
`decompressed.size() != originalSize` check (BinaryReader.hpp:238) ever runs — memory-exhaustion
DoS → terminate. Note the *safe* bounded routine `LZ4Decoder::Decompress(compressed, size,
uncompressedSize)` (35-133) already caps against `uncompressedSize` but is dead code — nothing
calls it.
- Fix: thread `originalSize` into the frame decoder and abort as soon as `output.size()` would
  exceed it (reuse the bounded `Decompress` logic). Also cap total output across blocks.

**I4 — Truncated / malformed LZ4 frame silently returns partial success.**
`DecompressFrame` (LZ4Decoder.hpp:172) loops `while (src + 4 <= srcEnd)` and simply *returns Ok*
with whatever was decoded if the 0-length end marker is missing or the stream is truncated
mid-block (208). The private `DecompressBlock` likewise `break`s on end-of-input rather than
erroring in several places (231, 261). As a public entry point (`Compression::DecompressLZ4`) this
reports success on corrupt/truncated input. In the archive path it is only saved by the downstream
`size() != originalSize` comparison; any caller using `DecompressLZ4` directly gets silent data
loss.
- Fix: require the end marker (or full input consumption) and return `CorruptedData` otherwise;
  don't treat "ran out of bytes" as normal completion.

### Minor

- **Checksum is a 32-bit truncation of a non-crypto hash, and "Portable" is actually
  endian-dependent.** `Checksum::Portable` (BinaryArchive.hpp:55-75) `memcpy`s raw bytes into a
  `uint64` (63, 71) and truncates the 64-bit result to `uint32` (74). The byte→`uint64` packing is
  native-endian, so BE and LE machines compute different checksums for identical bytes — the
  "ISA-independent" comment (53-54) is only true within one endianness. It's harmless *because* the
  format rejects cross-endian loads (137-140), but the naming oversells it and the 32-bit width
  gives only ~2^-32 corruption-miss odds. Determinism holds on a fixed platform (as the test
  checks) — just not across endianness. Document as same-endian, or hash byte-at-a-time for true
  portability.

- **`Skip`/`SkipPadding` do not update the running checksum, but `WritePadding` does →
  latent checksum mismatch.** BinaryReader.hpp:644-667 advances position without feeding the
  checksum, whereas BinaryWriter `WritePadding`→`WriteBytes` (490-505, 144) *includes* padding in
  the checksum. If alignment padding were ever used inside a headered archive, every load would
  fail `VerifyChecksum`. Currently unreached (the real format uses no padding; the AlignmentPadding
  test uses no header so the checksum is inert), so it is a landmine rather than an active bug.
  Make `Skip` update the checksum, or forbid skipping within checksummed regions.

- **`Peek` restores position and checksum but not `m_error`.** BinaryReader.hpp:672-688: a peek at
  or near EOF sets `m_error = CorruptedData` inside the internal read and never clears it, so a
  peek permanently poisons the reader even though it "didn't consume anything." Save/restore
  `m_error` (and the file `good()` state) too.

- **Deserializing `bool`/enum/optional-flag from an arbitrary stream byte is UB.**
  `operator()(std::optional<T>&)` reads `bool hasValue` via `ReadBytes(&value,1)`
  (BinaryReader.hpp:403-404, through the trivially-copyable path at 255); a stream byte like `0x37`
  produces a `bool` with an invalid object representation, and the following `if (hasValue)` is UB.
  Same for any enum whose byte isn't a valid enumerator. Normalize on read (`value = raw != 0`) for
  `bool`, and range-check enums.

- **Header count fields are neither checksummed nor validated.** `entityCount`/`archetypeCount`
  (BinaryArchive.hpp:108-110) live in the un-checksummed header and, on load, are not
  cross-checked against the actually-deserialized data (Registry.hpp:1466-1467 writes them;
  LoadInternal never validates them). Corrupting them is silently ignored — fine today, but they're
  a false sense of metadata integrity.

- **smallz4 does unaligned 32-bit reads through reinterpret casts** (`match4`, smallz4.hpp:160;
  `*(uint32_t*)(dataBlock + i)`, 646; `*(uint32_t*)(&data[...])`, 684). Strict-aliasing + alignment
  UB; works on x86-64 and ARMv8 in practice but is UBSan-positive and unsafe on strict-alignment
  targets. Vendored code, and only used on *our own* (trusted) data during compression, so
  portability-only — but worth a `memcpy`-based patch if ARM32/UBSan CI is in play.

- **Ambiguous `operator()` overloads for a type that is both trivially copyable and has
  `Serialize()`.** Reader 251-268 / writer 261-278 provide one overload constrained on
  `is_trivially_copyable_v<T>` and one on `HasSerializeMethod<T,...>`; a trivially-copyable struct
  that also defines `Serialize` matches both → hard ambiguity compile error. Add mutual exclusion
  (`requires (... && !HasSerializeMethod<...>)`) to the POD overload.

- **`IsVersionSupported()` has no lower bound** (BinaryArchive.hpp:132-135): version 0 is accepted
  and drives the reader onto the legacy `CRC32` (feature-dependent) checksum path
  (BinaryReader.hpp:172, 134). No writer emits v0/v1, so this is only a theoretical attacker lever
  (forces a feature-dependent checksum), but a `version == 0` reject would be cheap.

- **v1 back-compat is "same CPU-feature," not merely "same ISA."** The `CRC32` fallback with no
  SSE4.2/ARM-CRC uses the *same* Murmur finalizer as `Portable` (Simd.hpp:759-768), so two same-
  endian x86 hosts that differ only in SSE4.2 availability produce different v1 checksums. The
  comment at BinaryArchive.hpp:53-54 ("same ISA") understates the constraint.

## Test coverage

Present and solid: POD/string/vector/array/pair/tuple/optional/map/set round trips
(BinarySerializationTests), header invalid-magic and truncated-vector error paths (ErrorHandling
346-374), checksum success + tamper detection (unit 618/677 and end-to-end FormatV2
`ChecksumIsPortableFunctionAndDetectsCorruption`), portable-checksum determinism + version==2 wire
check (FormatV2Test), versioned component write/read, migration, too-old, and hash-mismatch
(BinarySerializationVersioningTests), and broad compression round trips across sizes/levels/
repetitive/random/text plus block-header validation and format detection (CompressionTests).

Gaps — all adversarial, which is exactly this subsystem's threat model:
- No test drives a large `originalSize`/`compressedSize`/vector-`size` field to prove I1 allocation
  behavior (should assert a graceful error, which today it can't give).
- No `size * sizeof(T)` overflow test (I2).
- No decompression-bomb test (I3) and no truncated/garbage LZ4 frame fed to
  `DecompressFrame`/`DecompressLZ4` (I4) — CompressionTests only ever decode self-produced valid
  frames; `InvalidBlockDecompression` only covers the outer `BlockHeader`.
- No endianness-mismatch load test (the `IsEndianCompatible` reject path is unexercised).
- No 32-bit/ILP32 build in the matrix to surface C1; no UBSan/ASan run over `BinaryReader` +
  `Registry::Load`.
- Padding-with-header-present (the Skip vs WritePadding checksum asymmetry) is never tested because
  AlignmentPadding writes no header.
- `Peek` at EOF, and the streamed file path (>10 MB, `m_data` empty → file-backed `ReadBytes`/
  `Skip`) are essentially unexercised.
- No fuzz target. A single libFuzzer/AFL harness over `Registry::Load(span, creg)` would likely
  surface I1–I4 and C1 immediately and is the highest-value coverage add.
# Concurrency & Systems — Review

## Overview

Scope: the work-scheduling seam and the system scheduling/execution layer.

- `include/Astra/Core/WorkScheduler.hpp` — `IWorkScheduler`, a pure interface (parallel-for seam). Astra ships **no** thread pool by design; the host wires one in.
- `include/Astra/System/System.hpp` — `System` concept, `Reads`/`Writes`/`SystemTraits` metadata, `LambdaSystemWrapper` (auto-derives read/write sets from a lambda's parameter const-ness).
- `include/Astra/System/SystemScheduler.hpp` — registration, conflict analysis, `BuildExecutionPlan` (greedy parallel grouping), `ExecutionGuard`.
- `include/Astra/System/SystemExecutor.hpp` — `SequentialExecutor` and `ParallelExecutor` (dispatches a group's systems via `IWorkScheduler::ParallelFor`).
- `include/Astra/System/SystemMetadata.hpp` — `SystemMetadata` and `SystemExecutionContext` PODs.

Because Astra creates no threads, the only concrete thread pool in the tree is `tests/Support/TestWorkerPool.hpp` (test support, technically out of scope). I reviewed it as the reference implementation since the prompt emphasises pool/CV correctness; it is **correct** (details in Strengths + Test coverage).

The subsystem's correctness rests on one load-bearing premise: *systems the scheduler places in the same parallel group are safe to execute concurrently against a single shared `Registry`.* Tracing that premise end-to-end is where the significant findings are.

## Design assessment

The layering is clean: a scheduler-agnostic `ParallelFor` seam, a conflict analyser that emits `parallelGroups` (outer = sequential barriers, inner = concurrent), and pluggable executors. The conflict analyser itself is **sound in the conservative direction** — I traced write-write, write-read, read-write and the group-aggregate union check across several orderings and could not construct a case where two genuinely-conflicting systems land in the same group (proof sketch in Strengths). Systems with no traits fall back to solo groups (forced serialization). That part is defensible.

The problem is the *model's expressive ceiling*. Parallelism is decided purely from `ComponentMask reads/writes`. That mask cannot represent (a) **structural changes** (create/destroy entity, add/remove component), or (b) **out-of-band access** (resources/singletons, or `registry.Get<Other>()` on a component not in the declared traits). Nothing at this layer forces such systems to serialize, provides a deferred command buffer, or even documents the restriction. The executor hands every grouped system the same live `Registry&` and runs them concurrently. So the core promise is only true for the narrow class of "pure, disjoint component read/write, no structural change" systems — and that narrowness is invisible to the user. That is the central finding (Critical #1).

Secondary theme: the `ExecutionGuard`/`m_isExecuting` mechanism advertises thread-safety ("prevents use-after-free during parallel execution", atomic flag) but is a check-then-act with a TOCTOU window and a non-reentrant boolean. It correctly handles the one case that actually matters in practice (a system on a worker calling `RemoveSystem`, which no-ops thanks to the dispatch happens-before), but gives a false sense of safety for the cases it does not cover.

## Strengths (file:line)

- `WorkScheduler.hpp:25-32` — the memory-model clause is explicitly stated ("establish a happens-before edge between the completion of every fn invocation and the return of ParallelFor"), plus the thread-identity/no-throw/reentrancy requirements. Rare to see a seam document its ordering contract at all. (Gap noted in Important #4.)
- `SystemScheduler.hpp:293-311, 346-365` — the conflict test is conservative and correct. The group-aggregate check uses the *union* of the group's reads/writes (`groupReads`/`groupWrites`), which is a superset of every member, so any system conflicting with *any* member also conflicts with the union and is excluded. Read-read is (correctly) not treated as a conflict. No-trait systems are treated as conflicting-with-everything (`HasConflict` line 353; group line 300-302), forcing safe serialization.
- `SystemScheduler.hpp:318-328` — the `dependsOnEarlier` scan preserves ordering against *unscheduled* earlier systems; systems already pulled into the current group are covered by the aggregate mask check instead. I traced A(wX)/B(rX,wY)/C(wY) and A(wX)/B(wX)/C(rX) chains and ordering held.
- `SystemScheduler.hpp:30-37` — instances are heap-allocated and captured by raw pointer into the `Delegate`, while ownership stays in a `unique_ptr`. Vector reallocation / mid-vector `erase` moves the entry but the heap address is stable, so delegates never dangle across `AddSystem`/`RemoveSystem`.
- `SystemScheduler.hpp:162, 176-183` — `Execute` snapshots the plan and delegates into a local `SystemExecutionContext` under the guard, so the executor iterates a stable copy.
- `SystemExecutor.hpp:43-47` — single-system groups and the no-scheduler case short-circuit to inline execution, avoiding dispatch overhead and correctly degrading to sequential when Astra is used thread-free.
- Reference pool `tests/Support/TestWorkerPool.hpp` — genuinely correct: `m_submitMutex` serialises external submissions so the single-`m_job`/generation design is safe; the caller *participates* in `RunJob` then waits on `next>=count && active==0`; `shared_ptr<Job>` keeps the job alive for late-waking workers (no UAF); the done-notify is issued under `m_mutex` after the atomic state is published, and `wait(pred)` re-checks so an early notify can't be lost. Nested calls inline via `t_insideWorker` (no self-deadlock).

## Findings

### Critical

**C1 — Systems declared "parallel" are NOT safe against the archetype storage if they do structural changes or out-of-band access; nothing prevents or documents it.**
`SystemExecutor.hpp:39-60` (concurrent dispatch of a group onto one `Registry&`) + `SystemScheduler.hpp:82-91, 293-311` (parallelism decided solely from component read/write masks).
*What:* `ParallelExecutor::Execute` runs every system in a `parallelGroup` concurrently, each receiving the same live `*context.registry`. The scheduler admits two systems to a group iff their declared `reads`/`writes` masks don't collide. But a system's mask cannot express a **structural change**. Consider two systems the scheduler considers non-conflicting: `SpawnSystem` declares `Writes<Position>` and internally calls `registry.CreateEntity<Position>()`; `MoveSystem` declares `Writes<Velocity>`. Interleaving: worker W1 runs `SpawnSystem`, which creates an entity → `ArchetypeManager` may `push_back` into `m_archetypes` (vector realloc / iterator invalidation), move entities between chunks, and does a **non-atomic** `++m_generation` (`ArchetypeManager.hpp:861,919,1329`). Concurrently worker W2 runs `MoveSystem`, whose `View::EnsureArchetypes` (`View.hpp:201,229`) reads `m_structuralChangeCounter` and **non-atomically reads `m_generation`**, then iterates the lazy `GetArchetypes()` range (`ArchetypeManager.hpp:533-536`) that references the very vector W1 is mutating, and reads chunk component arrays W1 is relocating. Result: torn reads of `m_generation`, iterator invalidation of the archetype range, and reads of half-moved entity data → data race, corruption, and dangling access. The same applies to add/remove-component and destroy-entity. *Why it's Critical:* structural mutation inside systems is extremely common in real game loops, the only safety signal the user provides is the read/write traits, and the layer offers **no** enforcement, no deferred command buffer wired into the executor, and not even a comment warning. The subsystem's headline promise ("systems declared parallel are safe to run concurrently against the archetype storage") is simply false for this common case.
*Fix (pick one, ideally more):* (1) Add a `structural`/`exclusive` flag to `SystemMetadata`; any system with it set gets its own solo group (forced serialization) — and derive it automatically where possible. (2) Provide each parallel system a per-thread `ParallelCommandBuffer` and forbid direct structural mutation during parallel execution, flushing between groups. (3) At minimum, document loudly in `SystemScheduler`/`SystemMetadata`/`ParallelExecutor` that a system in a multi-member group must perform **no** structural changes and must access only the components in its declared traits, and assert in debug builds if a structural change is observed mid-parallel-group (e.g., compare `m_structuralChangeCounter` snapshots around the group).

### Important

**I1 — `ExecutionGuard`/`m_isExecuting` is a check-then-act (TOCTOU), not mutual exclusion.**
`SystemScheduler.hpp:30-42, 45-48, 54-56, 117-118, 160-162, 189-190, 387-388`.
*What:* `AddSystem`/`RemoveSystem`/`Clear` do `if (IsExecuting()) return;` then mutate `m_systems`/`m_systemIndices`. `Execute` sets the flag via the guard. An atomic flag read followed by a separate non-atomic vector mutation is not exclusion: if an external thread T2 calls `RemoveSystem` while thread T1 is inside `Execute`, and T2's acquire-load of `m_isExecuting` is ordered before T1's release-store of `true` (or simply races it), T2 proceeds to `m_systems.erase(...)` while T1/its workers iterate `m_systems`/read the copied delegates → data race and use-after-free of a `SystemEntry`. The guard *does* correctly cover the one case that matters in practice — a system running on a worker calling `RemoveSystem`, which observes `true` via the dispatch happens-before and no-ops — but the atomic + the "prevents use-after-free" comment invite the broader, unsafe usage.
*Why:* false sense of safety; genuine UB under concurrent external mutation.
*Fix:* either document that mutation must not race `Execute` (single-writer model) and downgrade the atomic to a plain assert-only flag, or take a real mutex around registration and the `m_systems` read in `Execute`.

**I2 — `ExecutionGuard` is a boolean, not a counter: reentrant `Execute()` clears the flag early.**
`SystemScheduler.hpp:27-42, 162`.
*What:* if a system (directly or transitively) calls `scheduler.Execute()` again, the inner guard's destructor stores `false` on return (`~ExecutionGuard`, line 34-37) while the outer `Execute` is still running. From that point `IsExecuting()` returns `false`, so a concurrent worker-thread `AddSystem`/`RemoveSystem` is no longer suppressed → the exact use-after-free the guard exists to prevent (I1's safe case becomes unsafe). Nested `Execute` also silently re-enters with a live-but-unlocked scheduler.
*Why:* the guard's one reliable protection is defeated by reentrancy.
*Fix:* make it a counter (`std::atomic<int>` / depth), set `IsExecuting` = depth>0; or explicitly detect and reject reentrant `Execute`.

**I3 — Independent systems are reordered relative to insertion order, observable even under `SequentialExecutor`.**
`SystemScheduler.hpp:247-344` + `SystemExecutor.hpp:20-30`.
*What:* the greedy grouping can pull a later, independent system into an earlier group (e.g. S0:wA, S1:wA, S2:wB → plan `[[S0,S2],[S1]]`), so S2 runs before S1. `SequentialExecutor` executes the plan order, so this reordering is visible even with zero threads. Any inter-system dependency not expressed as a component conflict — ordering through a resource, an event queue, an external side effect, or a component touched via `registry.Get` outside the declared traits — is silently violated. This compounds C1: the mask model is the *only* thing preserving order.
*Why:* breaks determinism and implicit ordering that users of most ECS schedulers assume; hidden dependencies produce wrong results with no diagnostic.
*Fix:* document that only component-mask dependencies are honoured and independent systems may be reordered; optionally offer an "ordered/barrier" attribute or a stable-order mode that keeps insertion order among independent systems.

**I4 — `IWorkScheduler` contract specifies only the backward happens-before edge; the whole design relies on the unspecified forward edge.**
`WorkScheduler.hpp:25-32`.
*What:* the memory-model clause guarantees fn-completion → ParallelFor-return (backward). It does **not** state that the caller's writes *before* `ParallelFor` are visible *inside* fn (forward). Yet every parallel path depends on the forward edge: the archetype list, chunk pointers, `SystemExecutionContext`, and `m_isExecuting=true` are all published before dispatch and read inside fn. A strictly-conforming-but-adversarial scheduler could leave those invisible to a worker → races (including I1's `IsExecuting` visibility).
*Why:* under-specified contract that correctness silently depends on.
*Fix:* extend the clause to require a happens-before from the `ParallelFor` call site to the start of every fn invocation (both edges), which every real scheduler already provides.

**I5 — No exception-free / error channel for systems, and `new T(...)` on registration is unbudgeted under `-fno-exceptions`.**
`SystemScheduler.hpp:70, 403` (raw `new`), `SystemExecutor.hpp:47,53-57` (no result path).
*What:* `AddSystem` does `new T(...)`; with exceptions disabled a `bad_alloc` path either terminates or yields a null this-store — no `Result`/bool is returned, so callers can't detect a failed registration. Systems return `void`, so a system that hits an unrecoverable condition has no way to signal it and (per the seam contract) must not throw; a throwing system under `-fno-exceptions` calls `std::terminate`. This is partly inherent to the `System` concept, but the registration allocation at least should be failable.
*Fix:* return `bool`/`Result` from `AddSystem`; consider a nothrow allocation and a documented failure mode.

### Minor

- `System.hpp:78` — `IsReadOnly<T> = is_const_v<remove_reference_t<T>>` misclassifies pointer-to-const optional params: `const T*` has a non-const *pointer*, so `is_const_v` is `false` → treated as a **write**. Over-conservative (safe, but needlessly serializes read-only optionals). Use `is_const_v<remove_pointer_t<remove_reference_t<T>>>` for pointer params.
- `SystemScheduler.hpp:57, 435` — systems keyed by `TypeID<T>::Hash()` (64-bit). A hash collision makes the second `AddSystem` believe the type is already registered and silently drop it (`line 60-64`). Astronomically unlikely, but it's a hash, not a unique id; worth an assert-noting comment (the existing comment only explains the ID-space rationale).
- `SystemScheduler.hpp:126-136` — after `RemoveSystem`, `m_systemIndices` values are fixed up but each surviving `metadata.insertionOrder` keeps its original value, so it no longer matches the vector position. Only used for debugging/stable-sort intent, so harmless today, but stale.
- `System.hpp:114-135` — `LambdaSystemWrapper::operator()` creates a fresh `View` every invocation, so `EnsureArchetypes` starts from `m_lastRefreshCounter==0` and re-`CollectArchetypes()` on every `Execute`. The incremental view cache is defeated for lambda systems → per-frame archetype re-scan. Perf only.
- `SystemScheduler.hpp:320-328` — `BuildExecutionPlan` is O(n³) worst case (the `dependsOnEarlier` inner `HasConflict` scan). Fine for tens of systems; note it if system counts grow. Only rebuilt on `m_needsRebuild`.
- `SystemScheduler.hpp:179` — `context.metadata` is copied on every `Execute` but neither provided executor reads it. Minor per-frame cost.
- `SystemScheduler.hpp:208-215` — `GetExecutionPlan() const` mutates through `const_cast` (`BuildExecutionPlan`); not thread-safe if called during `Execute`. Consistent with the single-writer assumption but worth documenting.

## Test coverage

- **The system scheduling layer has essentially no dedicated tests.** The only coverage is `tests/Registry/ParallelIterationTest.cpp::ParallelExecutorRunsAllSystems` (asserts both systems *ran*, count==2 — it does **not** check grouping, conflict detection, or ordering) and `tests/Core/IdSpaceGuardTest.cpp` (only that systems don't consume `ComponentID`s). There is **no** test that: two conflicting systems are placed in separate groups; two non-conflicting systems are grouped together; no-trait systems force serialization; the `dependsOnEarlier` ordering is preserved; `RemoveSystem` index fixup keeps delegates valid; or the `ExecutionGuard` blocks mutation. Given this is the concurrency-critical subsystem, that is a significant gap. `BuildExecutionPlan`/`HasConflict` should have direct unit tests over hand-built mask scenarios (they're deterministic and easy to assert).
- **No test exercises C1's hazard**: a "parallel" system that does a structural change concurrently with another — precisely the case that corrupts storage. A TSan/repeated-run test with an injected multi-thread `TestWorkerPool` and two mask-disjoint systems (one spawning entities) would surface it.
- `tests/Core/WorkSchedulerTest.cpp` covers the *reference pool* well: exactly-once dispatch, reuse across 200 calls, inline for small counts, zero-count no-op, **nested reentrancy without deadlock**, concurrent external callers, and explicit thread count. Good. Not covered: an explicit assertion of the happens-before/visibility guarantee (I4), and behaviour when fn mutates shared ECS state (only atomics are tested).
- `tests/Registry/ParallelIterationTest.cpp` covers `View::ParallelForEach` with an injected scheduler and the no-scheduler sequential fallback, plus `ParallelForEachDescendant`. Solid for the iteration seam; none of it touches the scheduler's conflict/ordering logic.
# Hash Containers (FlatMap / FlatSet / Swiss) — Review

## Overview

Scope:
- `include/Astra/Container/FlatMap.hpp`
- `include/Astra/Container/FlatSet.hpp`
- `include/Astra/Container/Swiss.hpp`
- Supporting: `include/Astra/Core/Simd.hpp` (group scan), `include/Astra/Entity/Entity.hpp` (`EntityHash`).

These are the SwissTable-style open-addressing containers that back the whole ECS
(entity→archetype maps, component sets, etc.). Both use 16-byte metadata groups
(`GROUP_SIZE = 16`), 7-bit `H2` control bytes, an 87.5% max load factor, and a
128-bit SIMD group scan with SSE2 / NEON / scalar fallbacks.

I traced the probe/insert/erase/rehash logic against every review dimension and
cross-checked against the (quite strong) differential fuzz suite. **Bottom line:
the core probe scheme is correct** — find/insert/erase agree on the probe order,
tombstones do not truncate chains, and the deferred-insert logic correctly avoids
the classic tombstone double-insert. The findings are one real lifetime bug in
`FlatSet::Emplace`, a 64-bit-only hashing assumption that is UB on 32-bit targets,
an MSVC SIMD-detection fragility, and several performance/portability footguns.

## Design assessment

The probe scheme is unconventional but internally consistent. `H1 = hash & (cap-1)`
selects a *slot* index; the home group is `index / 16` and probing starts at
`startSlot = index % 16`, scanning that group in rotated order `[startSlot..15, 0..startSlot-1]`,
then advancing linearly `(g+1) % numGroups`. Two invariants make it correct:

1. **Capacity is always a power of two ≥ 16**, so it is always an exact multiple of
   `GROUP_SIZE`. There are no partial trailing groups, no boundary/sentinel slots,
   and no mirror bytes to maintain (`SENTINEL` is unused). This eliminates an entire
   class of group-boundary bugs.
2. **Insert advances past a group only when that group has no EMPTY slot**
   (`MatchEmpty() != 0` terminates — FlatMap.hpp:556, FlatSet.hpp:522), and empties
   never reappear except via full `Clear`/`Rehash`. Therefore `FindImpl`'s
   "any EMPTY in group ⇒ stop" test (FlatMap.hpp:898–908) is exactly the correct
   termination: a full-group `Match(h2)` scan finds the key if it lives in this
   group, and if the group has any empty the key cannot live in a later group.

The **deferred insertion slot** (record first empty-or-deleted, but keep scanning
for a duplicate until an EMPTY terminates — FlatMap.hpp:530–559 / FlatSet.hpp:496–525)
is the right fix for the tombstone double-insert hazard, and there is a dedicated
regression test for it. Linear (not triangular/quadratic) group probing is the main
design compromise: correct, but more prone to primary clustering under high load.

## Strengths (file:line)

- **Clean control-byte algebra.** `H2 ∈ [1,127]` (folds 0→1, Swiss.hpp:40–48),
  `EMPTY=0x80`, `DELETED=0xFE`, `SENTINEL=0xFF` all have the high bit set, so
  `Match(h2)` with `h2 < 128` can never alias a control byte and `IsFull(m)=m<0x80`
  is a single compare (Swiss.hpp:56–78). `ASTRA_ASSUME(h2>0 && h2<128)` documents it.
- **Correct tombstone semantics.** Erase writes `DELETED` not `EMPTY` (FlatMap.hpp:637,
  FlatSet.hpp:583), preserving probe chains; tombstone reuse decrements the count on
  insert (FlatMap.hpp:580–583).
- **Deferred-insert duplicate guard** (FlatMap.hpp:530–559) — genuinely subtle and
  correct; matched by `ReinsertAfterTombstoneDoesNotDuplicate` regression tests.
- **Portable SIMD with real scalar fallback** (Simd.hpp:161–173, 218–230, 357, 394):
  `MatchByteMask<Width128>` degrades SSE2 → NEON → scalar, and the NEON bit-mask
  reduction (Simd.hpp:137–159) is correct.
- **RAII cleanup on the duplicate path** in `FlatSet::Emplace` (FlatSet.hpp:430–439)
  correctly destroys the probe temporary when a duplicate is found.
- **Exact-multiple-of-group capacity invariant** removes partial-group hazards.
- **Strong differential fuzzing**: churn vs `std::unordered_map/set`, adversarial
  `k&0xF` hash, single-bucket `GroupColliderHash`, colliding-low-bits churn.

## Findings

### Critical

None confirmed. The probe/rehash/erase logic is correct and the fuzz suite
(including adversarial single-group collisions and tombstone-reinsert regressions)
corroborates it. The most serious defect is the `FlatSet` lifetime bug below, which
is a resource leak rather than corruption/UB, so it is filed as Important.

### Important

1. **`FlatSet::Emplace` skips the destructor of the moved-from probe temporary —
   resource leak on every successful insert** — `FlatSet.hpp:558`
   (`cleanup.dismissed = true;`).
   The value is constructed into a local `temp` (FlatSet.hpp:425–427), then
   move-constructed into the slot (`std::move(*temp)`, line 552–556), then cleanup is
   *dismissed*. Moving does **not** end `temp`'s lifetime; `temp` is still a live
   object that must be destroyed exactly once. Dismissing cleanup skips
   `~T()` on `temp`.
   Failure scenarios:
   - `T` is copyable but **not movable** and owns heap memory (e.g. a type with a
     user copy ctor and no move ctor). `std::move(*temp)` binds to the *copy* ctor;
     the slot gets a deep copy while `temp` still owns its original buffer. Skipping
     `~T()` **leaks that buffer on every insert.**
   - `T` performs unconditional work in its destructor (scope guard, live-instance
     counter `~T(){--count;}`). The counter/side effect is never balanced → observable
     corruption of program state.
   For the current ECS this is masked (FlatSet is used with `Entity`, which is
   trivially destructible), and moved-from `std::string`/`std::vector` happen to hold
   nothing so the tests don't catch it — but it is a real bug for the general-purpose
   container. **Fix:** delete line 558 entirely; let `cleanup` destroy the moved-from
   `temp` on scope exit (no double-free risk — `temp` and the slot are distinct
   objects). `FlatMap::Emplace` is unaffected (it constructs in place via
   `piecewise_construct`, FlatMap.hpp:586–592).

2. **`H2` / hash splitting assumes 64-bit `size_t`; `hash >> 57` is UB on 32-bit
   targets** — `Swiss.hpp:46` (and `Entity.hpp:165`).
   `SwissTable::H2(size_t hash)` computes `hash >> 57`. On x86-32 / ARM32 / wasm32
   (all detected and treated as supported in `Platform.hpp:68–89`, `ASTRA_POINTER_SIZE 4`),
   `size_t` is 32 bits and a shift count ≥ width is **undefined behavior**; in
   practice it yields 0/garbage, so `H2` collapses to a constant and the SIMD filter
   stops filtering. `EntityHash` (Entity.hpp:156–171) also computes a `uint64_t` and
   returns it as `size_t`, truncating away exactly the high bits `H2` wants. This is
   Critical-severity (UB) *if* a 32-bit build is produced; downgraded to Important
   because the project builds x64 in practice. **Fix:** derive `H2` from a width-aware
   position (e.g. `(hash >> (sizeof(size_t)*8 - 7)) & 0x7F`) and mix the hash to full
   width before splitting.

3. **No hash mixing → degenerate `H2` (and clustering) for identity hashers** —
   `FlatMap.hpp:919–925` / `FlatSet.hpp:821–827` (`SplitHash` passes the raw hash
   straight to `H1`/`H2`).
   `std::hash<int>` and friends are identity on every major stdlib. For small integer
   keys, `H2 = (key >> 57) & 0x7F == 0 → folded to 1` for *all* keys, so every
   occupied slot with `h2==1` matches `Match(1)` and each group scan forces a full
   equality sweep — `Find`/`Emplace` degrade toward O(n) per group. `H1` also uses the
   raw low bits, inviting collisions for strided keys. Correctness is preserved (the
   fuzz `AwfulHash`/`GroupColliderHash` tests pass), but this is a real hot-path
   footgun for any non-`Entity` key type — and these containers underpin the ECS.
   Abseil-style tables deliberately mix the user hash for exactly this reason.
   **Fix:** apply a fixed integer finalizer (e.g. `Simd::Ops::PortableHashCombine`)
   to the hash inside `SplitHash` before extracting `H1`/`H2`.

4. **Header-only SIMD detection silently disables SSE2 under a bare MSVC build** —
   `Platform.hpp:130–136` gates `ASTRA_HAS_SSE2` on `#ifdef __SSE2__`, but MSVC
   **never defines `__SSE2__`** (it uses `_M_X64` / `_M_IX86_FP`). The in-tree build
   works only because `premake5.lua:82,176,272` force-`define`s `__SSE2__` — and
   defining a double-underscore reserved identifier is itself technically UB. Any
   consumer who includes these headers from their own MSVC build (CMake/vcpkg/manual)
   without that define gets the **scalar** `MatchByteMask` path for the entire Swiss
   table — a large, silent performance regression in a header-only library.
   **Fix:** in `Platform.hpp`, also set `ASTRA_HAS_SSE2` for MSVC via
   `defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)` (and `ASTRA_HAS_SSE42`
   analogously) so detection is self-contained.

### Minor

5. **`relevantEmpty` ternary is a no-op** — `FlatMap.hpp:901–903`,
   `FlatSet.hpp:803–805`. Both branches evaluate to the full-group `emptyMatches`
   (`emptyMatches >> 0 == emptyMatches`). The result is *correct* (full-group empty
   check is what the scheme needs) but the code reads as if the two cases differ,
   which will mislead the next maintainer. Simplify to `if (emptyMatches) return m_capacity;`.

6. **Probe loop bound is 16× looser than necessary** — `FlatMap.hpp:485,813`,
   `FlatSet.hpp:451,742`. `while (probes++ < m_capacity)` counts *group* iterations
   but bounds by *slot* count. Harmless given the load-factor guarantee (always an
   empty slot) but should be `m_numGroups`.

7. **Rehash copies keys instead of moving; runs a redundant duplicate scan** —
   `FlatMap.hpp:765`. `std::move(oldPair->first)` is `const Key&&` and binds to the
   copy ctor, so keys are copied during every grow (comment "avoids the const issue"
   is misleading — it silently copies). Each reinsert also runs the full `Emplace`
   duplicate scan though the source is known unique. Perf only; keys like `Entity`
   are trivial so impact is small.

8. **Tombstone-heavy churn doubles capacity instead of cleaning in place** —
   `ReserveForInsert` (FlatMap.hpp:726–735). The grow branch (`size+tomb+1 > 0.875·cap`)
   is checked before the tombstone-cleanup branch, so a workload with small live size
   but many tombstones grows (memory doubles) rather than same-size rehashing.
   Memory bloat under insert/erase churn.

9. **`m_capacity * 2` can overflow to 0** — `FlatMap.hpp:728`, `FlatSet.hpp:659`.
   Only at `cap = 2^63` (astronomically unreachable), but `Rehash(0)` would then
   allocate nothing and corrupt state. `NextPowerOfTwo` saturates at `SIZE_MAX`;
   the doubling path does not.

10. **Allocator traits not honored** — copy ctor copies allocators directly
    (FlatMap.hpp:248–253) rather than via `select_on_container_copy_construction`, and
    `Swap`/move-assign ignore `propagate_on_container_*`. Fine for `std::allocator`
    (the only use), wrong for stateful allocators. `FlatMap`'s `CustomAllocator` test
    only exercises a stateless wrapper.

11. **Rehash re-entrancy is only safe by load-factor accident** — `Rehash` reinserts
    via `Emplace`, which calls `ReserveForInsert` per element (FlatMap.hpp:738–779).
    A nested rehash would reallocate `m_groups` mid-loop while the outer loop still
    reads its own `oldGroups` locals. It cannot currently trigger (post-alloc load is
    always < threshold), but the invariant is undocumented and fragile.

12. **Not thread-safe (by design), undocumented.** No atomics/locks; `Emplace`/`Reserve`
    rehash invalidates all iterators, pointers, and references. Standard for
    open-addressing but worth a one-line contract note given how widely these are used.

## Test coverage

Coverage is strong on correctness and weak on lifetime/perf edge cases.

- **Excellent:** differential fuzzing vs `std::unordered_map/set` across small/medium/large
  key spaces and 16 seeds; adversarial `AwfulHash` (`k&0xF`) and single-bucket
  `GroupColliderHash`; the `ReinsertAfterTombstoneDoesNotDuplicate` regression (both
  map and set) directly locks in the tombstone/deferred-insert fix; group-boundary,
  wraparound, and colliding-low-bits churn tests; copy/move/swap/const/heterogeneous
  lookup; load-factor boundary.
- **Gaps:**
  - **No destructor/leak accounting for `FlatSet::Emplace`** — a live-instance-counter or
    copyable-non-movable resource type would immediately expose finding #1. This is the
    single most valuable test to add.
  - No test for a **copy-only (non-movable) value type** in either container (would also
    surface the rehash key-copy path).
  - No **stateful-allocator** propagation test (finding #10).
  - No **32-bit build** in CI, so finding #2 (`>> 57` UB) is invisible.
  - No coverage asserting the **scalar SIMD fallback** matches SSE2 output (portability
    of the group scan is untested off-x64).
  - Perf footgun #3 (identity-hash `H2` degeneration) is correctness-covered but not
    guarded by any load/probe-length assertion.
# Core Foundation (Base/Platform/Memory/Result/Simd/TypeID/TypeContext) — Review

## Overview

Scope: `include/Astra/Core/{Base,Platform,Memory,Result,Version,Simd,TypeID,TypeContext}.hpp`, `include/Astra/Astra.hpp`.

This is the portability/identity/memory foundation the rest of Astra sits on. Two findings below were **empirically reproduced** with the locally installed toolchains (MSVC "VS2026" `cl.exe` and LLVM `clang++ 18.1.8`, both targeting `x86_64-pc-windows-msvc`) rather than inferred from reading:

1. `TypeNameInternal<T>()` collides for distinct types that share a name inside an anonymous namespace across translation units, on **both** MSVC and Clang.
2. MSVC never defines the GCC/Clang-style `__SSE2__`/`__SSE4_2__`/`__AVX2__` macros that `Platform.hpp` keys its SIMD feature flags on, so `Simd.hpp`'s fast paths are compiled out on MSVC unless something outside the header (this repo's `premake5.lua`) injects those defines manually.

Repro code and commands are described inline with each finding so they can be re-run.

## Design assessment

- `Result<T,E>` is a hand-rolled tagged union (`AlignedStorage<T,E>` + `bool`) implementing the usual `Ok`/`Err`/`Map`/`AndThen`/`ValueOr` vocabulary. The mechanics (placement-new, `std::launder`, explicit dtor calls) are correct for the common case, but the accessor family is inconsistently guarded (see Critical #2) and the assignment operators are not exception-safe if `T`/`E` can throw (see Important). Since this is a header-only library, "Astra itself builds with exceptions off" does not guarantee the *consumer's* translation unit does too — `Result<T,E>` can end up instantiated for throwing `T`/`E` in an exception-enabled downstream build.
- `Memory.hpp`'s `AllocateMemory`/`FreeMemory` pair is a reasonable OS-allocation abstraction (VirtualAlloc/mmap/posix_memalign/malloc, huge-page opportunistic path, matching free). The main gaps are a missing power-of-two/overflow precondition on `alignment`/`size`, a genuine UB spot in the Windows huge-page detector, and a plain-bool double-checked-locking race in `IsHugePagesAvailable()`.
- `Simd.hpp`'s design (explicit `Width128`/`Width256` tags, `Ops::*` free functions, scalar fallback for every intrinsic) is sound and every intrinsic path *does* have a scalar fallback, as required. The defect is one level up, in `Platform.hpp`'s feature-detection macros, which simply don't fire on MSVC.
- `TypeID`/`TypeContext` implement a "stable name hash → dense per-context ID" scheme with a sensible process/host-install model (`SetTypeContext`/`GetTypeContext`, pending-registration draining for plugins that load before the host installs a shared context). The mutex-guarded `GetOrAssignComponentID` is correctly synchronized. The fatal flaw is upstream of the mutex: the *input* to hashing (compiler-pretty-printed type name) is not unique per type when anonymous namespaces are involved, and the existing debug-mode collision assert can't catch it because the colliding names are literally byte-identical, not just hash-identical.
- Layering: `Core/TypeID.hpp` and `Core/TypeContext.hpp` both `#include "../Component/Component.hpp"` (for `ComponentID`) and `TypeContext.hpp` also pulls `Container/FlatMap.hpp`. That makes "Core" depend on higher layers, which is a smell for a foundation layer but not a functional bug.

## Strengths

- `include/Astra/Container/AlignedStorage.hpp:19-31` — `As<U>()` correctly uses `std::launder` after reinterpret_cast, which is the right pattern for type-punned placement-new storage; `Result`'s use of it is sound.
- `include/Astra/Core/Simd.hpp` — every SIMD entry point (`MatchByteMask`, `MatchEitherByteMask`, `Load128/And128/Or128/CompareEqual128/TestSubset128`, and their 256-bit counterparts) has a correct scalar (or narrower-width) fallback; `Detail256::*_Fallback` even documents *why* it interleaves loads/broadcasts for scheduling. This is careful, well-commented work.
- `include/Astra/Core/Simd.hpp:753-783` — `HashCombine` (hardware CRC32) vs `PortableHashCombine` (MurmurHash3 finalizer, ISA-independent) is a deliberate, well-documented split so that on-disk checksums stay portable while the hot path can still use hardware CRC32.
- `include/Astra/Core/TypeContext.hpp:56` (comment block) and the `Detail::PendingMetaRegistration` queue — the plugin/host bootstrap ordering problem (static registrars running before `SetTypeContext` is called) is handled with a real mutex-guarded queue and a swap-and-run drain (`DrainPendingMeta`, `TypeContext.hpp:146-168`) that avoids deadlocking re-entrant registrations. Good, deliberate design for a genuinely hard multi-module problem.
- `include/Astra/Core/Memory.hpp:274-292` — `FreeMemory` correctly dispatches `munmap` vs `free` based on the `usedHugePages` flag returned from the matching `AllocateMemory` call, and null-checks first.
- `include/Astra/Reflection/MetaRegistry.hpp:281-289` / `include/Astra/Core/TypeContext.hpp:88-90` — the forward-declare-here/define-there split for `TypeContext::Meta()` is correctly marked `inline` at the definition site, avoiding an ODR trap.

## Findings

### Critical

**1. `TypeID<T>` collides for distinct types sharing a name in anonymous namespaces across translation units — confirmed on MSVC and Clang.**
`include/Astra/Core/TypeID.hpp:148-196` (`TypeNameInternal<T>`), consumed by `TypeHash<T>()` (line 199-203) and `TypeContext::GetOrAssignComponentID` (`include/Astra/Core/TypeContext.hpp:70-86`).

Repro (verified locally): two translation units each declare `namespace { struct Foo { /* different members */ }; }` and print the compiler's pretty-printed function signature for a template instantiated on `Foo`:
- MSVC (`cl.exe`, `/std:c++20`): both TUs print
  `` const char *__cdecl Name<struct `anonymous-namespace'::Foo>(void) noexcept ``
  — byte-for-byte identical.
- Clang 18.1.8 (`-std=c++20`): both TUs print
  `const char *Name() [T = (anonymous namespace)::Foo]`
  — byte-for-byte identical.

Neither compiler embeds any per-TU disambiguator (file hash, `__COUNTER__`-like token, etc.) in the *pretty-printed* name, even though their *mangled* symbol names do disambiguate anonymous namespaces internally. Since `TypeNameInternal<T>()` is exactly this pretty-printed string, `XXHash::XXHash64` of it is identical for the two unrelated types, so `TypeContext::GetOrAssignComponentID(hash, name)` finds the existing entry and returns the **same `ComponentID`** for two types that are not the same type.

This is worse than a plain hash collision because the debug-mode safety net can't catch it: `TypeContext.hpp:75-78`
```cpp
#ifdef ASTRA_BUILD_DEBUG
    ASTRA_ASSERT(m_names[it->second] == name,
                 "TypeContext hash collision: two distinct type names share a hash");
#endif
```
compares the *name string*, not type identity — and the two colliding types produce the exact same name string, so the assert passes.

Impact: this is a realistic pattern (many codebases put "private to this .cpp file" marker/tag components in anonymous namespaces across multiple systems files). Two such components silently alias to one `ComponentID`. The archetype/component storage will apply one type's `ComponentDescriptor` (size, alignment, ctor/dtor/copy function pointers) to both types' data, producing genuine memory corruption/type-confusion when the two types differ in size or have non-trivial special members — not just a logical mislabeling.

Fix directions: document the limitation loudly (this is a known, accepted limitation in comparable libraries such as EnTT); and/or additionally salt with something that *is* unique per-TU without breaking the "stable across platforms/recompiles" contract documented on `TypeID::Hash()` is hard — the two goals are in tension. At minimum, add a build-time or first-run warning path, and consider using `typeid(T).hash_code()`/RTTI identity (which *is* disambiguated per anonymous namespace at the ABI level on both compilers) as an additional cross-check in debug builds instead of comparing name strings, since name-string comparison is provably unable to detect this class of collision.

**2. `Result<T,E>::operator*()` has no guard at all (not even debug-only) against dereferencing an `Err` result — inconsistent with `operator->()`, and violates the "wrong alternative" invariant.**
`include/Astra/Core/Result.hpp:132-157`.

```cpp
ASTRA_NODISCARD T* operator->()
{
    ASTRA_ASSERT(m_hasValue, "Dereferencing Result with no value");   // guarded (debug only)
    return m_storage.template As<T>();
}
...
ASTRA_NODISCARD T& operator*() & noexcept                              // NOT guarded, in any config
{
    return *m_storage.template As<T>();
}
ASTRA_NODISCARD const T& operator*() const& noexcept                   // NOT guarded
{ ... }
ASTRA_NODISCARD T&& operator*() && noexcept                            // NOT guarded
{
    return std::move(*m_storage.template As<T>());
}
```
If a caller writes `auto r = MightFail(); Use(*r);` without checking `r.IsOk()` first (an easy, natural mistake — and the sibling accessor `operator->()` does have a debug check, so the omission on `operator*()` reads as an oversight rather than a deliberate contract), the code reinterprets the live `E` object's storage bytes as a `T` via `AlignedStorage::As<T>()` (`std::launder(reinterpret_cast<T*>(&data))`). This is executed in **every** build configuration, including Debug — there is no assertion net whatsoever on this path, unlike `operator->()`. If `T` and `E` differ in size, alignment, or have non-trivial special members, this is textbook type-confusion UB: reading uninitialized/foreign bytes as a `T`, or later calling `~T()` on memory that was never a `T` (the destructor at `Result.hpp:44-51` runs `~T()` or `~E()` based on `m_hasValue`, which was never touched by this misuse, so the mismatch is silent until the wrong destructor runs on the wrong bytes).

Fix: add the same `ASTRA_ASSERT(m_hasValue, ...)` to all three `operator*()` overloads that `operator->()` already has.

### Important

**3. `Platform.hpp`'s SIMD capability macros never fire on MSVC without external help — the primary listed compiler silently gets zero SIMD acceleration.**
`include/Astra/Core/Platform.hpp:130-152`.

```cpp
#if defined(ASTRA_ARCH_X64) || defined(ASTRA_ARCH_X86)
    #ifdef __SSE2__
        #define ASTRA_HAS_SSE2 1
    #endif
    #ifdef __SSE4_2__
        #define ASTRA_HAS_SSE42 1
    #endif
    #ifdef __AVX2__
        #define ASTRA_HAS_AVX2 1
    #endif
    ...
```

Verified locally with the installed MSVC (`cl.exe`, VS2026 toolset) compiling a trivial TU:
- No `/arch` flag: `__SSE2__` **not defined**, `__SSE4_2__` **not defined**, `__AVX2__` **not defined** — even though every x64 target has SSE2 as an ISA baseline.
- `/arch:AVX`: `__AVX2__` still **not defined** (needs `/arch:AVX2` specifically); `__SSE2__`/`__SSE4_2__` still not defined by the compiler itself.
- `__SSE4_2__` is **never** defined by MSVC under any `/arch` flag tested (MSVC has no predefined macro for SSE4.2 at all).

`Platform.hpp` has no MSVC-specific branch (e.g. `defined(_M_X64) || defined(_M_AMD64) || (_M_IX86_FP >= 2)` for the always-true SSE2 baseline) — it relies entirely on GCC/Clang-style predefined macros that plain MSVC never sets. The result: `ASTRA_HAS_SSE2`/`ASTRA_HAS_SSE42`/`ASTRA_HAS_AVX2` are unconditionally undefined on MSVC unless something *outside this header* defines `__SSE2__`/`__SSE4_2__` manually.

This repo's own `premake5.lua:81-84,175-178` does exactly that (`defines { "__SSE2__", "__SSE4_2__" }` with a comment "matching benchmark"), which is why the in-tree tests/benchmarks work — but that workaround lives in the build script, not in `Platform.hpp`/`Simd.hpp`. Since Astra is header-only (`kind "None" -- Header-only library`) and `Astra.hpp` is meant to be the single consumer-facing include, **any downstream consumer who includes these headers from a different build system (CMake, a plain VS project, another premake script that doesn't copy this exact `defines` block) silently gets the fully-scalar fallback path on MSVC**, with zero warning/error — `Bitmap<N>` operations, archetype component matching, and everything else built on `Simd::Ops` quietly run 10s of times slower than intended, with no diagnostic that this happened.

Fix: detect the SSE2 baseline natively for MSVC x64 (`defined(_M_X64)`) and x86 (`_M_IX86_FP >= 2`) instead of depending on `__SSE2__`; for AVX/AVX2/SSE4.2 (which MSVC genuinely cannot express via `_M_*` alone), either document the requirement to define `__SSE2__`/`__SSE4_2__`/`__AVX2__` prominently in the public header (not just a build script comment), or switch to runtime `IsProcessorFeaturePresent`/`__cpuid` dispatch for MSVC.

**4. `TypeContext::GetOrAssignComponentID`'s ID-space-exhaustion guard is debug-assert-only; Release/Dist builds silently wrap and collide.**
`include/Astra/Core/TypeContext.hpp:70-86`.

```cpp
ASTRA_ASSERT(m_next != INVALID_COMPONENT, "TypeContext ID space exhausted");
const ComponentID id = m_next++;
```
`ComponentID` is `uint16_t` and `INVALID_COMPONENT = 65535` (`Component.hpp:11-13`). `ASTRA_ASSERT` is compiled to `((void)0)` outside `ASTRA_BUILD_DEBUG` (`Base.hpp:47-52`). So in Release/Dist, once `m_next` reaches `65535`, the next registration assigns `id = 65535` — which **is** the sentinel `INVALID_COMPONENT` value — and then `m_next++` wraps `uint16_t` back to `0`, so the *following* registration reuses `ComponentID 0`, colliding with whatever type was first registered. Any code that treats `id == INVALID_COMPONENT` as "not found/invalid" will misinterpret a legitimately-assigned ID.

This is inconsistent with the sibling fix already applied one layer up in `include/Astra/Component/ComponentRegistry.hpp:101-109`, which — per the immediately preceding commit ("guard component-ID overflow loudly") — refuses registration gracefully in *all* configs when `id >= MAX_COMPONENTS`, specifically to avoid silent corruption. `TypeContext` (the lower-level, process-global ID allocator that the comment block at `TypeContext.hpp:59-65` says is now shared by components, resources, and systems keyed by type hash) has no equivalent graceful-refusal path for its own boundary (65535), only the debug assert. Practically hard to hit (65535 distinct hashes registered through one context) but it is the exact class of bug the prior commit was written to eliminate elsewhere, and `TypeContext` is the one place that doesn't have it.

Fix: mirror the `ComponentRegistry` pattern — check `m_next != INVALID_COMPONENT` unconditionally (not just in the assert), and return `INVALID_COMPONENT` (or otherwise refuse) instead of wrapping, in every build configuration.

**5. `SetTypeContext`/`GetTypeContext` read/write a plain (non-atomic) pointer with no synchronization — a data race if a host installs a context concurrently with any thread already resolving `TypeID<T>::Value()`.**
`include/Astra/Core/TypeContext.hpp:104-142`.

```cpp
inline TypeContext*& CurrentTypeContextSlot() noexcept
{
    static TypeContext* s_ctx = nullptr;  // per-module slot, by design
    return s_ctx;
}
...
inline void SetTypeContext(TypeContext* ctx)
{
    Detail::CurrentTypeContextSlot() = ctx;   // plain, unsynchronized write
    ...
}
inline TypeContext* GetTypeContext()
{
    TypeContext* ctx = Detail::CurrentTypeContextSlot();  // plain, unsynchronized read
    ...
}
```
The doc comment says this "must run before the module's first `TypeID<T>::Value()`/Registry use," which is a reasonable contract for the intended startup-time host/plugin handshake, but nothing enforces it and the slot itself is not `std::atomic<TypeContext*>`. If a host ever installs/swaps a context after other threads are already active (e.g. late plugin load, hot-reload, or a test harness restoring a previous context concurrently with another test's threads, similar to the pattern in `TypeContextTest.cpp`'s `PendingRegistrationsDrainIntoInstalledContext`), this is a formal data race (UB) with no assertion or detection to catch misuse.

Fix: make `CurrentTypeContextSlot()` an `std::atomic<TypeContext*>` (relaxed ordering is enough given the documented "before first use" contract) so at minimum the read/write pair is well-defined, and consider an assert-on-first-use-after-install guard in debug builds.

**6. `IsHugePagesAvailable()` uses an unsynchronized manual double-checked-locking pattern on plain `bool`s — a data race under concurrent first calls.**
`include/Astra/Core/Memory.hpp:94-138`.

```cpp
ASTRA_FORCEINLINE bool IsHugePagesAvailable() noexcept
{
    static bool checked = false;
    static bool available = false;
    if (!checked)
    {
        ... // OS calls, no locking
        checked = true;
    }
    return available;
}
```
Function-local `static` *initialization* is thread-safe per C++11 magic statics, but `checked`/`available` here are trivially/constant-initialized to `false` — no runtime-guarded initialization is generated for the declarations themselves. The subsequent `if (!checked) { ...; checked = true; }` is ordinary, unsynchronized code. If `AllocateMemory(..., AllocFlags::HugePages)` is called from two threads concurrently before the first call completes (plausible: chunk-pool/entity-table allocation can happen from worker threads), both threads can see `checked == false` and run the OS privilege-check logic concurrently, then both write `checked`/`available` concurrently — a formal data race (UB), flaggable by TSan, even though the practical consequence is usually just redundant work.

Fix: use `std::atomic<bool>` with acquire/release ordering, or `std::call_once`/a function-local `static` whose initializer itself performs the check (so the compiler-generated magic-static guard protects it).

**7. `AllocateMemory`'s size-rounding has no power-of-two precondition check and no overflow guard.**
`include/Astra/Core/Memory.hpp:140-145`.

```cpp
ASTRA_FORCEINLINE AllocResult AllocateMemory(size_t size, size_t alignment = 64, AllocFlags flags = AllocFlags::None) noexcept
{
    AllocResult result;
    result.size = size;
    size = (size + alignment - 1) & ~(alignment - 1);
```
The bitmask round-up only works for power-of-two `alignment`; there's no `ASTRA_ASSERT`/static documentation of that precondition, so a caller passing a non-power-of-two alignment silently gets a wrong (non-conservative) rounded size instead of a clear error. Separately, `size + alignment - 1` is unchecked for overflow: a caller-supplied `size` near `SIZE_MAX` wraps to a small value, and `AllocateMemory` then silently **succeeds**, returning a pointer to a far smaller buffer than the caller believes it requested — a classic integer-overflow-to-buffer-overflow setup. All current in-tree call sites (`alignof(T)`, fixed `64`, `BLOCK_ALIGNMENT`) are safe (power-of-two, bounded sizes), so this isn't currently reachable, but `AllocateMemory`/`FreeMemory` are public, header-exposed utilities with no documented precondition guarding either property.

Fix: `ASTRA_ASSERT((alignment & (alignment - 1)) == 0, ...)` and an overflow check (`size > SIZE_MAX - alignment` → fail) before rounding.

**8. Windows `IsHugePagesAvailable()` reads an uninitialized `BOOL` if `PrivilegeCheck` fails, and never enables the privilege it's checking for — the huge-page path is effectively permanently dead on Windows.**
`include/Astra/Core/Memory.hpp:101-119`.

```cpp
HANDLE token;
if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
{
    LUID luid;
    if (LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid))
    {
        PRIVILEGE_SET privSet = {};
        ...
        BOOL result;                              // uninitialized
        PrivilegeCheck(token, &privSet, &result);  // return value (success/failure) not checked
        available = (result == TRUE);
    }
    CloseHandle(token);
}
```
Two separate issues: (a) `PrivilegeCheck`'s own `BOOL` return (whether the check itself succeeded) is discarded; if it fails, `result` was never written and `available = (result == TRUE)` reads an indeterminate value — real UB, however narrow. (b) Even when `PrivilegeCheck` succeeds, it reports whether `SeLockMemoryPrivilege` is currently *enabled* in this process's token — and Windows process tokens start with assignable privileges **present but disabled** even when the account has been granted "Lock pages in memory" via Local Security Policy. Nothing in this file (or elsewhere in the huge-page path) ever calls `AdjustTokenPrivileges` to enable it. In practice this means `IsHugePagesAvailable()` returns `false` for essentially every real process on Windows regardless of whether the OS-level right was granted, silently disabling the entire `MEM_LARGE_PAGES` fast path documented at the top of the file.

Fix: initialize `result = FALSE;`, check `PrivilegeCheck`'s own return value, and call `AdjustTokenPrivileges` to actually enable the privilege (with its own success check) before relying on it — or drop the check and document that huge pages require the caller's process to enable the privilege itself.

**9. `Result::operator=` (copy and move, both `Result<T,E>` and `Result<void,E>`) is not exception-safe in the "switching active alternative" branch.**
`include/Astra/Core/Result.hpp:54-95`, `263-295`.

```cpp
else
{
    this->~Result();
    new (this) Result(other);   // or Result(std::move(other))
}
```
If constructing the new alternative throws (only possible if `T`/`E`'s copy/move constructor can throw — Astra's own project builds with exceptions off, but this is a header-only template that a downstream consumer can instantiate in an exception-enabled TU), `this->~Result()` has already destroyed the old contents, and the placement-`new` constructor's member-initializer-list has already written the *new* `m_hasValue` before the body's nested placement-new throws. The result is `*this` left with `m_hasValue` reflecting the new state but `m_storage` holding no valid object of either type. When `*this`'s destructor later runs (stack unwind or normal scope exit), it calls `~T()`/`~E()` on memory that was never validly constructed — UB.

Fix: either document that `T`/`E` must have non-throwing copy/move constructors (and consider a `static_assert`), or wrap the placement-new in a way that leaves `*this` in a well-defined (e.g. previously-cleared/`INVALID_COMPONENT`-style sentinel) state on failure.

### Minor

- **`include/Astra/Core/Base.hpp:30-43`** — `Platform.hpp` detects and names `ASTRA_COMPILER_INTEL`, but `Base.hpp`'s `ASTRA_FORCEINLINE`/`ASTRA_NOINLINE`/`ASTRA_ASSUME`/`ASTRA_PACK_BEGIN`/`ASTRA_PACK_END` only branch on MSVC or GCC/Clang, with a hard `#error` otherwise (`Base.hpp:13`). Classic Intel C++ Compiler (icc, EOL since 2023) would fail to compile. Low priority since Intel isn't in the officially supported compiler list, but the dangling detection branch in `Platform.hpp` implies support that doesn't exist — either implement it (icc is largely GCC-attribute-compatible) or remove the detection to avoid the false impression.
- **`include/Astra/Core/Base.hpp:38-42`** — `ASTRA_ASSUME(x)` is documented nowhere as requiring `x` to be side-effect-free, yet its evaluation guarantee differs by backend: the GCC-fallback form (`if (!(x)) __builtin_unreachable();`) always evaluates `x`; MSVC's `__assume(x)` and Clang's `__builtin_assume(x)` are not guaranteed to. Not currently misused in-tree (both call sites pass pure comparisons), but a latent footgun for future use with a side-effecting expression, where behavior would diverge across compilers.
- **`include/Astra/Core/Version.hpp:7`** — `ASTRA_VERSION` packs MAJOR/MINOR/PATCH into 8 bits each via shifts with no bound-check; MINOR or PATCH ≥ 256 silently corrupts the adjacent field. Currently safe (3.4.0) but unguarded.
- **`include/Astra/Core/Memory.hpp:19-27`** — `<sys/memfd.h>`, `<linux/memfd.h>`, `<sys/shm.h>` are conditionally included but never used anywhere in the file (no `memfd_create`/`shm*` calls) — dead/vestigial includes.
- **`include/Astra/Core/Memory.hpp:140-269`** — the Windows huge-page success path (early `return result;` around line 162) skips the `zeroMemory` memset that the POSIX huge-page path performs; harmless in practice since both `VirtualAlloc`-committed and `mmap(MAP_ANONYMOUS)` pages are OS-zeroed on first fault regardless, but the asymmetry reads as an oversight and is worth making consistent for clarity/defensiveness.
- **`include/Astra/Core/Memory.hpp:87-91`** — on total allocation failure (`ptr` stays null), `AllocResult.size` still holds the original, pre-rounding requested size (set at the top of the function) rather than `0`; harmless as long as callers check `ptr` first (they do, via `FreeMemory`'s `if (!ptr) return;`), but slightly misleading if a caller inspects `size` without checking `ptr`.
- **`include/Astra/Core/Memory.hpp:82-85`** — `operator&(AllocFlags, AllocFlags)` returns `bool` rather than `AllocFlags`, breaking the conventional bitmask-operator chaining idiom (works fine for the current single-flag-test call sites, but is non-idiomatic and would surprise anyone trying to compose flag subsets).
- **`include/Astra/Core/TypeID.hpp:8`, `include/Astra/Core/TypeContext.hpp:12-13`** — layering inversion: "Core" headers depend on `Component/Component.hpp` (for `ComponentID`) and `Container/FlatMap.hpp`, coupling the generic type-identity utility to ECS-specific concepts and reducing its reusability as a standalone facility.
- **`include/Astra/Core/Result.hpp:160-201, 321-333`** — `Map`/`MapError`/`AndThen`'s trailing-return-type `decltype(func(std::declval<T>()))` uses an rvalue (`declval<T>()`) to deduce the return type, but the function bodies actually invoke `func` with an lvalue (`*m_storage.template As<T>()`). A callable that only accepts `T&` (mutable lvalue reference) will fail to compile at the trailing-return-type even though the actual call would be well-formed — an avoidable inconsistency.
- **`include/Astra/Core/Result.hpp:30, 75-76, 247, 280`** — the move constructor/assignment `noexcept` specifications require *both* `T` and `E` to be nothrow-movable, even though only one is ever touched per instance at runtime; overly conservative and can defeat noexcept-gated optimizations (e.g. `std::vector` preferring move over copy) when only one side is throwing-movable.
- **`include/Astra/Core/TypeContext.hpp:75-77`** — the `#ifdef ASTRA_BUILD_DEBUG` guard around the `ASTRA_ASSERT` hash-collision check is redundant; `ASTRA_ASSERT` itself already expands to `((void)0)` outside debug builds (`Base.hpp:47-52`).
- **`include/Astra/Reflection/MetaRegistry.hpp:281-289`** — `TypeContext::Meta()` re-acquires `m_metaMutex` on every call, even long after `m_meta` has been lazily constructed; correct but leaves a small amount of avoidable lock contention on a presumably hot accessor.
- **`include/Astra/Astra.hpp:7-21`** — several headers (e.g. `Core/Simd.hpp`, `Core/TypeID.hpp`) already `#include` their own transitive dependencies (`Base.hpp`, `Platform.hpp`, etc.), so the umbrella header's explicit ordering is documentation/harmless redundancy rather than load-bearing; not a bug (each header is independently self-contained, which is the right property to have), just worth noting the file-level comment ("includes all headers in the correct dependency order") slightly overstates how much the order actually matters given `#pragma once` + self-contained includes.

## Test coverage

- **`Result<T,E>` / `Result<void,E>`: zero coverage.** `grep -r "Result<" tests/` returns nothing. No test exercises `Ok`/`Err` construction, copy/move for non-trivial `T`/`E`, `Map`/`MapError`/`AndThen`/`ValueOr`, `GetValue`/`GetError` null-on-wrong-variant behavior, or (critically) `operator*`/`operator->` on a wrong-variant `Result` — which would have caught Critical finding #2 immediately.
- **`Memory.hpp`: zero direct coverage.** `grep -r "AllocateMemory\|FreeMemory\|IsHugePagesAvailable" tests/` returns nothing. `AllocateMemory`/`FreeMemory` are exercised only *indirectly* through consumers (`EntityTable`'s huge-page block, `ResourceStorage`, `ArchetypeChunkPool`) via those subsystems' own tests — nothing directly validates alignment guarantees, the `ZeroMem` flag, or the huge-page/regular-allocation fallback boundary.
- **`Simd.hpp`: no dedicated test file.** The 128-bit path (`Load128`/`And128`/`Or128`/`CompareEqual128`/`TestSubset128`, and by extension `MatchByteMask<Width128>`) gets solid *incidental* coverage via `tests/Container/BitmapTest.cpp` and `BitmapFuzzTest.cpp`, since `Bitmap<128>` (`MAX_COMPONENTS`) routes through it. However, there is no coverage — direct or indirect — for: `PopCount`, `FindFirstSet`, `FindLastSet`, `CountTrailingZeros`, `HashCombine`, `PortableHashCombine`, `PrefetchRead`/`PrefetchT0..NTA`, `BatchOps`, or **the entire 256-bit (`Width256`/`Int256`) path**, which no in-tree bitmap size currently instantiates.
- **`TypeContextTest.cpp`/`TypeIDTests.cpp`: good happy-path coverage, missing the collision case.** Dense sequential ID assignment, same-hash-same-id, independent-context isolation, meta-registry isolation, and pending-registration draining are all tested. Missing: the anonymous-namespace name collision scenario (Critical #1 — this is exactly the kind of test that should exist given the task description explicitly calls it out as a risk), any concurrent/multithreaded exercise of `GetOrAssignComponentID` or `SetTypeContext` (despite the class documenting itself as mutex-guarded and the module-static slot being a documented multi-module concern), and ID-space-exhaustion behavior at the 65535 boundary.
# Small Containers (SmallVector / Bitmap / AlignedStorage) — Review

## Overview

Scope: `include/Astra/Container/SmallVector.hpp`, `include/Astra/Container/Bitmap.hpp`,
`include/Astra/Container/AlignedStorage.hpp`. These are the foundational hand-rolled
containers underneath archetype storage: a small-buffer-optimized vector, a fixed-width
component-mask bitmap with SIMD fast paths, and a raw type-erased aligned buffer used by
`Result<T,E>`.

Supporting files read for context (not separately reviewed, cited only where they explain
a finding): `include/Astra/Core/Simd.hpp`, `include/Astra/Core/Base.hpp`,
`include/Astra/Core/Memory.hpp`, and the current call sites of `SmallVector`/`Bitmap` across
`Archetype*.hpp`, `RelationshipGraph.hpp`, `EntityIDStack.hpp`, `CommandBuffer.hpp`.

## Design assessment

`SmallVector<T,N>` follows the classic "N inline, else heap" SBO shape (`m_data` either
points at `m_buffer` or at a separately `::operator new`'d block; `IsSmall()` disambiguates
by pointer identity). The uninitialized-vs-constructed bookkeeping is mostly done correctly
and carefully — `Grow`, `shrink_to_fit`, the multi-element `insert(pos,count,value)`, `erase`,
move ctor/assignment and the three `swap` cases all get the
construct/uninitialized-move/destroy/deallocate ordering right, including the tricky
count-vs-tail split in `insert(pos,count,value)` (verified by hand for both branches).

The one systemic gap is **aliasing discipline for single-element mutators**
(`push_back`/`emplace_back`/`insert(pos,single)`/`emplace(pos,...)`/`resize(count,value)`):
unlike `insert(pos,count,value)`, which explicitly makes a defensive copy of `value` before
touching storage (with a comment acknowledging exactly why), none of the single-element
paths do this. That asymmetry is strong evidence the single-element gap is an oversight, not
a documented limitation — see Critical #1 and #2.

`Bitmap<Bits>` is small, tight, and mostly correct: bounds-checked `Set`/`Reset`/`Test` (safe
no-op in Release, loud assert in Debug — nice, deliberate design per the recent "Task 11"
comment), correct SIMD/scalar split gated on `WORD_COUNT >= SIMD_WORDS`, and word-for-word
scalar fallback that's been fuzzed against `std::bitset`. The main soft spot is that it hands
out a mutable raw-word pointer (`Data()`) with no enforcement of the "bits beyond `Bits` stay
zero" invariant that its own SIMD equality/hash implicitly relies on.

`AlignedStorage<T,E>` is a minimal, correct raw-storage primitive: size/alignment are the max
of the two candidate types, `As<U>()` uses `std::launder` correctly for the placement-new/
type-pun-reuse pattern, and the `<void,E>` specialization is a sensible degenerate case. It
deliberately does not manage construct/destroy (that's the caller's job, e.g. `Result<T,E>`,
out of scope) — for implicit-lifetime types (scalars, PODs) that's even safe without an
explicit placement-new under C++20's implicit-object-creation rules; for non-implicit-lifetime
T/E the caller must placement-new/destroy explicitly, same as `std::aligned_storage` always
required. No bugs found here.

## Strengths (file:line)

- `include/Astra/Container/SmallVector.hpp:596` — `alignas(T) std::byte m_buffer[N * sizeof(T)];` correctly propagates `alignof(T)` to the inline buffer (and hence to `alignof(SmallVector<T,N>)`), including for over-aligned `T`. The SBO fast path is alignment-safe.
- `include/Astra/Container/SmallVector.hpp:336-370` — `insert(pos, count, value)` correctly copies `value` into a local (`T valueCopy(value)`, line 344) *before* any growth/shift, defending against both the growth-invalidation and shift-corruption hazards described below. Both branches (`count < tail` and `count >= tail`) were hand-traced and are correct: raw-space elements are `uninitialized_move`d, previously-constructed destinations are `move`d/`fill_n`-assigned, and only genuinely raw memory receives `uninitialized_fill_n`.
- `include/Astra/Container/SmallVector.hpp:291-318` — `shrink_to_fit()` orders move → destroy-source → free-old correctly in both the heap→small and heap→heap(exact) cases.
- `include/Astra/Container/SmallVector.hpp:485-519` — `swap()` correctly special-cases small/small, heap/heap, and falls back to a verified-correct 3-move sequence for the mixed case.
- `include/Astra/Container/Bitmap.hpp:31-51` — `Set`/`Reset` pair an `ASTRA_ASSERT` (loud in Debug) with a real runtime bounds guard (`if (index < Bits)`), so Release builds stay memory-safe (silent no-op) even though the assert compiles out. Confirmed by `tests/Container/BitmapTest.cpp:63-75` and `tests/Container/BitmapFuzzTest.cpp:70-86`, which test both configurations.
- `include/Astra/Container/Bitmap.hpp:62-94` — SIMD `HasAll` correctly restricts the 128-bit fast path to `WORD_COUNT >= SIMD_WORDS` and falls back to an equivalent scalar loop otherwise; verified against `std::bitset` in `BitmapFuzzTest.cpp`.
- `include/Astra/Container/AlignedStorage.hpp:19-31` — correct `std::launder` usage for the reused-storage type pun; `static_assert` on `As<U>()` prevents accessing storage as an unrelated third type at compile time.

## Findings

### Critical

**1. `emplace`/`insert(pos, single value)` corrupts the inserted value when it aliases any element at or after the insertion point (shift-before-read).**
`include/Astra/Container/SmallVector.hpp:372-402` (esp. 390-397), reached via `insert` at `:326-334`.

```cpp
std::construct_at(end(), std::move(back()));   // 390
std::move_backward(it, end() - 1, end());       // 393
std::destroy_at(it);                            // 396
std::construct_at(it, std::forward<Args>(args)...); // 397 — reads `args` AFTER the shift
```
`args`/`value` is read *after* `move_backward` has overwritten every slot from `it` to `end()-1`
(each slot receives its left neighbor's value). If the caller's reference aliases any element
at index `>= offset` (i.e. at or after the insertion point — including the exact position
being inserted at), that memory has already been overwritten by the time it's finally read.

Concrete repro: `SmallVector<int,4> v{1,2,3}; v.insert(v.begin()+1, v[2]);` — expected result
`{1,3,2,3}` (inserting a copy of the old `v[2]==3`), but trace: `construct_at(end(), move(back()))`
writes `3` to index 3; `move_backward(it=+1, end()-1=+2, end()=+3)` moves index2→index3 (now
`v[3]` holds old `v[2]`, a duplicate) then index1→index2 (so `v[2]` — the very memory `args`
is bound to — is overwritten with the old `v[1]==2`); the final `construct_at(it, args)` then
reads `v[2]` and gets `2`, not the intended `3`. Result is `{1,2,2,3}` — silently wrong data,
not even a crash to reveal the bug.

Contrast with `insert(pos,count,value)` (Strengths, above), which defends against exactly this
by copying `value` before touching storage. Nothing analogous exists for the single-element
overloads.

**Fix**: copy the argument into a local before the shift (mirroring the `count` overload), e.g. construct the incoming element from a local copy, or read `args` into a temporary at function entry before any `move_backward`/`construct_at(end(), move(back()))`.

**2. `push_back`/`emplace_back`/`emplace`/`insert`/`resize(count,value)` read a freed/destroyed source when the argument aliases an existing element and the call triggers reallocation (growth-invalidation).**
`include/Astra/Container/SmallVector.hpp:439-448` (`emplace_back`), `:372-402` (`emplace`), `:471-483` (`resize(count,value)`), all funneling into `Grow()` at `:572-593`.

`Grow()` destroys the source range (`:582 std::destroy(begin(), end())`) and, once the vector
is already heap-backed, frees the old block (`:587 ::operator delete(m_data)`) — *before*
control returns to the caller, which then reads `args`/`value` to construct the new element.
If that reference aliases an element of the same vector, the read at `:445`
(`std::construct_at(end(), std::forward<Args>(args)...)`) or `:480`
(`std::uninitialized_fill(end(), begin() + count, value)`) targets memory that has already
been passed to `::operator delete` — a genuine heap-use-after-free, not merely stale-but-alive
inline storage.

Concrete repro (ASan-catchable):
```cpp
Astra::SmallVector<int, 1> v;
v.push_back(1); v.push_back(2); v.push_back(3); v.push_back(4); // now heap-backed, size==capacity==4
v.push_back(v[0]); // Grow() frees the 4-int heap block at line 587, then emplace_back
                    // reads `value` (a reference into that just-freed block) at line 445
```
For a trivially-copyable `T` in the *small→heap* transition specifically the bug is latent
(the inline buffer isn't deallocated, only "destroyed", and destroying a trivial `int` is a
no-op, so the read happens to still see the old bit pattern) — which is precisely why this is
easy to miss in testing. It becomes a real UAF as soon as an already-heap-backed vector grows
again, or whenever `T`'s destructor actually does something (any non-trivial `T`).

**Fix**: same remedy as #1 — snapshot/copy the argument before calling `Grow`, or have `Grow`
construct the new element into the new buffer from the (still-valid) old location before
destroying/freeing the source, the way `libstdc++`'s `_M_realloc_insert` does for `push_back`.

Note: a search of current internal call sites (`Archetype*.hpp`, `RelationshipGraph.hpp`,
`EntityIDStack.hpp`, `CommandBuffer.hpp`) found no existing self-referential
`push_back`/`insert`/`emplace` calls on `SmallVector` today (the one lookalike,
`LZ4Decoder.hpp:124/293`, operates on `std::vector<uint8_t>`, which real-world STL
implementations do handle safely for this exact pattern) — so this is not presently
triggered internally, but `SmallVector` is a general-purpose, header-only, publicly-usable
container and this is a straightforward, easy-to-hit footgun for any future caller
(the ECS "copy this entity's relationship list into itself" style of code is exactly where
it'd surface).

### Important

**3. Heap growth path is not alignment-aware — breaks for over-aligned `T`.**
`include/Astra/Container/SmallVector.hpp:576` (`Grow`) and `:310` (`shrink_to_fit`):
```cpp
T* newData = static_cast<T*>(::operator new(newCap * sizeof(T)));   // Grow, :576
T* newData = static_cast<T*>(::operator new(m_size * sizeof(T)));   // shrink_to_fit, :310
```
Both call the plain, non-allocating-aligned `::operator new(std::size_t)` overload directly
(not a `new`-expression, so the compiler cannot silently upgrade to the aligned overload the
way it does for e.g. `new SomeOveralignedType`). For any `T` with `alignof(T)` greater than
`__STDCPP_DEFAULT_NEW_ALIGNMENT__` (commonly 16 on 64-bit MSVC/GCC/Clang — so anything
`alignas(32)`/`alignas(64)`, e.g. AVX/AVX-512-friendly component types or manually
cache-line-aligned structs), the returned block is not guaranteed to satisfy `T`'s alignment.
Placement-constructing/accessing `T` there is undefined behavior and can fault outright on
platforms/instructions that require aligned addresses (e.g. aligned SIMD loads/stores). This
is a direct contrast with the inline buffer (`:596`), which — per Strengths — *does* get this
right; the bug only appears once the vector spills past `N` elements.
No current internal instantiation (`Entity`, `EntityLocation`, `size_t`,
`std::pair<Entity,EntityLocation>`, `BlockInfo`, `RecycledEntry`, …) is over-aligned, so this
doesn't bite today's callers, but it's a real correctness gap in a general-purpose container.

**Fix**: use `::operator new(newCap * sizeof(T), std::align_val_t{alignof(T)})` /
matching sized-and-aligned `::operator delete` when `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__` (can be `if constexpr`-gated to avoid the extra call for common types).

**4. `reserve(n)` can silently allocate far more than `n`.**
`include/Astra/Container/SmallVector.hpp:283-289` delegates straight to `Grow(newCap)`
(`:572-593`), which unconditionally re-applies the 2x growth heuristic:
```cpp
newCap = std::max(newCap, std::max<size_type>(capacity() * 2, 4));  // :574
```
So `v.reserve(5)` on a vector with `capacity()==4` allocates `max(5, max(8,4)) == 8`, not 5.
Callers using `reserve()` for precise, one-time capacity control (a very common reason to call
`reserve` at all — e.g. "I know I'm about to bulk-insert exactly N items") get up to ~2x the
memory they asked for with no way to opt out. This is standard-legal (the contract is only
`capacity() >= n`) but is a surprising deviation from typical `reserve()` semantics and from
what `tests/Container/SmallVectorTest.cpp:256-279` implicitly documents via `EXPECT_GE`.

**Fix**: have `reserve()` allocate exactly `newCap` (bypassing the growth-factor heuristic, which belongs only to the amortized-growth paths like `Grow` from `push_back`/`insert`).

**5. `Bitmap::Data()` is a mutable raw escape hatch with no protection of the "padding bits stay zero" invariant it relies on internally.**
`include/Astra/Container/Bitmap.hpp:195` (`Word* Data() noexcept { return m_words.data(); }`).
Whenever `Bits` is not an exact multiple of `BITS_PER_WORD` (64), the last word contains
"padding" bits (indices `[Bits, WORD_COUNT*64)`) that `Set()`/`Reset()` (bounds-checked
against `Bits`, not `WORD_COUNT*64`) can never touch — every whole-word comparison
(`operator==` at `:96-121`, `HasAll` at `:62-94`, `GetHash()` at `:182-191`) implicitly assumes
those bits are always zero. `Data()` hands out a non-const `Word*` with no such enforcement,
so any writer through it can desync that invariant. This is exactly what
`include/Astra/Archetype/Archetype.hpp:723` does during deserialization
(`reader(mask.Data()[i]);`, reading raw 64-bit words straight from a file). With the default
`ASTRA_MAX_COMPONENTS == 128` (an exact multiple of 64) there's currently no padding to
corrupt, but any build configured with a non-multiple-of-64 `ASTRA_MAX_COMPONENTS` combined
with a corrupted/foreign save file could set stray high bits that silently break archetype
mask equality/hashing/`HasAll` (used for the entire archetype lookup and system-matching
machinery) without any assertion firing.

**Fix**: either keep `Data()` const-only and add a bounds-checked bulk-load API for
deserialization that masks off bits `>= Bits` in the last word, or add a cheap
`Normalize()`/invariant-restoring helper that callers of the mutable `Data()` are required to
invoke.

### Minor

**6. `SmallVector::at()` provides no more protection than `operator[]` in Release builds, despite the STL convention that `.at()` is the checked accessor.**
`include/Astra/Container/SmallVector.hpp:200-210`. Both `at()` and `operator[]` guard solely
with `ASTRA_ASSERT`, which is `((void)0)` when `ASTRA_BUILD_DEBUG` isn't defined
(`include/Astra/Core/Base.hpp:47-52`). That's a reasonable project-wide convention (exceptions
are off, `ASTRA_ASSERT` is documented as "internal invariants only"), but naming the method
`at()` — the one STL method whose entire purpose is to be bounds-checked in all
configurations — invites callers migrating `std::vector`-based code to assume Release-mode
safety they don't get. Consider either renaming (drop `at()`, keep only `operator[]`) or
documenting the divergence prominently next to the declaration.

**7. Mixed small/heap `swap()` is correct but always pays for a temporary + two extra full moves.**
`include/Astra/Container/SmallVector.hpp:512-518`. Functionally fine (verified above), just
notably more expensive than a `SmallVector`'s "cheap swap" reputation might suggest for the
asymmetric case; worth a comment if intentional.

**8. `Bitmap` has `operator|=` but no `operator&=`.** `include/Astra/Container/Bitmap.hpp:146-153` vs. the read-only `operator&` at `:124-132`. Minor API asymmetry; not a bug.

**9. `alignas(CACHE_LINE_SIZE)` on `Bitmap<Bits>` (`:17`) pads every instance up to a full cache line (typically 64B) regardless of `Bits`.** For `Bitmap<32>`/`Bitmap<64>` (8B of real data) that's an 8x memory blow-up per instance. Fine for the one-per-`Archetype` `ComponentMask` use case, but a footgun if `Bitmap` is ever reused for a per-entity or high-cardinality small bitmask.

## Test coverage

`tests/Container/SmallVectorTest.cpp` and `SmallVectorFuzzTest.cpp` are solid for the
"well-behaved, non-aliasing" surface: SBO↔heap transition, all insert/erase/resize paths
(including both branches of `insert(pos,count,value)`, explicitly called out in comments at
`SmallVectorTest.cpp:691-729`), copy/move/swap across all small/heap combinations, and a
40k-op differential fuzz test against `std::vector` (`SmallVectorFuzzTest.cpp:70-74`). What's
missing:
- No test exercises a self-referential `push_back`/`insert`/`emplace` (would have caught
  Critical #1 and #2 immediately — this is the single highest-value test to add).
- No test instantiates `SmallVector` with an over-aligned `T` (`alignas(32)`+) and forces the
  small→heap transition, which would have caught Important #3.
- No test asserts an upper bound on what `reserve(n)` actually allocates (Important #4).
- No death-test for `at()`/`operator[]` OOB under `ASTRA_BUILD_DEBUG`, unlike the analogous
  pattern already used for `Bitmap` (see below) — would make Minor #6's Debug/Release
  divergence explicit and regression-tested.

`tests/Container/BitmapTest.cpp` and `BitmapFuzzTest.cpp` are strong: differential fuzzing
against `std::bitset` for `Set`/`Reset`/`Test`/`Count`/`Any`/`None` (50k ops,
`BitmapFuzzTest.cpp:9-27`), `HasAll` fuzzed against a manual bitset computation, bitwise ops
fuzzed, and explicit `EXPECT_DEATH`-gated out-of-range behavior for both Debug and Release
(`BitmapTest.cpp:63-75`, `BitmapFuzzTest.cpp:70-86`) — this Debug/Release dual-mode pattern is
exactly what `SmallVectorTest.cpp` is missing for `at()`. Gaps: no test touches `Data()` at
all (mutable or const), so Important #5's padding-bit invariant is entirely unverified;
`operator|=` isn't directly tested (only `operator|` via the fuzz test).

`tests/Container/AlignedStorageTest.cpp` covers size/alignment computation, const access,
byte-preservation type punning, and the `<void,E>` specialization — adequate for what is a
small, low-risk primitive; no gaps worth flagging.
# Entity Subsystem (Entity/EntityManager/EntityTable/EntityIDStack) — Review

## Overview

Scope: `include/Astra/Entity/Entity.hpp`, `include/Astra/Entity/EntityManager.hpp`,
`include/Astra/Entity/EntityTable.hpp`, `include/Astra/Entity/EntityIDStack.hpp`.

This is the bit-packed entity handle plus its three supporting structures: a segmented
sparse-version table (`EntityTable`), a LIFO free-id stack with recycled
`(id, nextVersion)` pairs (`EntityIDStack`), and the orchestrator (`EntityManager`) that
ties creation/destruction/serialization together. The design is a fairly standard
generational-index ECS entity scheme (comparable to EnTT), configurable at compile time
via `ASTRA_ENTITY_BITS` / `ASTRA_ENTITY_VERSION_BITS` for 16/32/64-bit handles with an
arbitrary version-bit split.

The core algorithmic structure is sound (LIFO recycling for version-churn locality,
segment pooling + huge-page-backed allocation for the version table, dual small/large
batch paths). However, tracing the bit-packing math across non-default version-bit
widths surfaced a real handle-corruption bug in the version-wraparound path, and tracing
`Deserialize` and the (complete) absence of synchronization surfaced two genuine
handle-aliasing scenarios.

## Design assessment

- The packed-handle layout (`id | version << VERSION_SHIFT`) with a dedicated
  `INVALID` sentinel and one reserved id (`MAX_SAFE_ID = ID_MASK - 1`) to avoid
  colliding with that sentinel is the correct, standard approach, and is applied
  consistently between `Entity.hpp` and `EntityIDStack::INVALID_ID`.
- `EntityManager` reimplements the version-increment/wraparound logic inline
  (`EntityManager.hpp:124-128`, `:173-177`) instead of calling
  `Entity::NextVersion()` (`Entity.hpp:85-96`), which already encodes the correct
  boundary check (`currentVersion >= VERSION_MASK`). The duplication is where the
  Important-severity wraparound bug below was introduced — the two implementations
  disagree, and only one of them is exercised by the recycle path.
- `EntityTable`'s segment/version storage is decoupled from `EntityIDStack`'s
  id-allocation bookkeeping; `EntityManager` is the only place that keeps them in
  sync. That single choke point is good for auditability but means every caller of
  `EntityTable::SetVersion`/`Destroy` (including `Deserialize`) must independently
  uphold invariants the table itself does not check (see Critical #1).
- No synchronization primitives exist anywhere in the four files, and no comment
  states a threading contract. For a library that otherwise documents error-handling
  conventions carefully (`Result<T,E>`, no exceptions), the silence on concurrency is
  a real documentation gap given `Create`/`Destroy` are asked about explicitly.

## Strengths

- `Entity.hpp:130` (`MAX_SAFE_ID`) + `EntityIDStack.hpp:55` (`if (m_nextID >= Entity::ID_MASK)`)
  correctly reserve the top id so a fully-allocated id space can never collide with
  `Entity::Invalid()`'s sentinel value. Verified by trace: `m_nextID` never reaches
  `ID_MASK`, so `INVALID_ID` (`= ID_MASK`) is never handed out as a real id.
- `Entity.hpp:60` defensively masks `id` (`id & ID_MASK`) in the packing constructor,
  and this is specifically unit-tested (`tests/Entity/EntityTest.cpp:91-100`,
  `IDMasking`). (The asymmetry with the *version* side not being masked is the root
  cause of Important #1 below.)
- `EntityTable::GetOrCreateSegment` (`EntityTable.hpp:460-506`) gracefully falls back
  from huge-page-backed allocation to a pooled segment to plain heap allocation with
  no crash path if huge pages are unavailable (`AllocateHugePage`,
  `EntityTable.hpp:553-566`, tolerates `result.ptr == nullptr`).
- `EntityIDStack::Allocate`/`Recycle` (`EntityIDStack.hpp:44-61`, `:96-100`) use a
  LIFO stack (`back()`/`push_back`/`pop_back`), which maximizes locality (most
  recently freed id reused first) and keeps version churn concentrated rather than
  spread across the whole id space.
- `CreateBatch`/`DestroyBatch` (`EntityManager.hpp:70-105`, `:139-192`) use a
  small/large dual path with `SmallVector<_, 256>` staging so typical batch sizes
  never touch the heap (`EntityIDStack.hpp:179`).
- ID-space exhaustion is handled gracefully everywhere it's checked: `Create()`
  returns `Entity::Invalid()` (`EntityManager.hpp:58-61`), `Allocate()`/`AllocateBatch()`
  return `INVALID_ID`/a partial count rather than asserting or looping forever
  (`EntityIDStack.hpp:55-58`, `:78-93`), consistent with the project's no-throw
  contract.
- Serialize/Deserialize consistently check `reader.HasError()` after each logical
  block and propagate `SerializationError` via `Result` (`EntityManager.hpp:346-349`,
  `:374-377`, `:394-397`) — good adherence to the structural error-handling
  convention, even though *content* validation is missing (Critical #1).

## Findings

### Critical

**C1. `Deserialize` performs no bounds/duplicate validation on restored ids — a corrupted or crafted save file produces silent entity-id aliasing.**
`EntityManager.hpp:332-403`, specifically the id reads at `:352-368` (recycled
entries) and `:387-400` (alive entities).

`Deserialize` reads `nextFreshID`, a list of `(id, nextVersion)` recycled entries,
and a list of `(id, version)` alive entities straight off the wire and trusts them
verbatim:

```cpp
IDType nextFreshID; reader(nextFreshID);
...
for (uint32_t i = 0; i < recycledCount; ++i) { reader(id); reader(nextVersion); recycledEntries.push_back({id, nextVersion}); }
...
manager->m_idStack.SetNextID(nextFreshID);
manager->m_idStack.RestoreRecycledEntries(recycledEntries);
for (uint32_t i = 0; i < aliveCount; ++i) { reader(id); reader(version); ...; manager->m_table.SetVersion(id, version); }
```

Nowhere is it checked that `id <= Entity::ID_MASK`, that `id < nextFreshID`, or that
an id doesn't appear in *both* the alive list and the recycled list. Concrete failure
scenario:

1. A save file (corrupted, hand-edited, or produced by a future/foreign build) marks
   id `X` "alive" via the alive-entity loop, but sets `nextFreshID <= X` (or simply
   omits `X` from ever being consumed by `m_idStack`).
2. `Deserialize` returns `Result::Ok` — no error is ever raised for this.
3. The restored manager keeps handing out fresh ids via `EntityIDStack::Allocate()`
   (`EntityIDStack.hpp:44-61`, the `m_nextID++` path). When the counter reaches `X`,
   `Create()` calls `m_table.SetVersion(X, INITIAL_VERSION)`
   (`EntityManager.hpp:63`), silently overwriting whatever version the alive-loop had
   set for `X` in step 1 and incrementing `EntityTable::aliveCount`
   incorrectly-or-not-at-all depending on the old/new version transition
   (`EntityTable.hpp:128-133`).
4. Two logically distinct "entities" — the one implied by the deserialized alive-list
   entry, and the one just handed out by `Create()` — now share id `X`. Any holder of
   the first (deserialized) handle and any holder of the second (freshly created)
   handle can each pass `EntityManager::IsValid` at different times for the same slot,
   which is a textbook use-after-free / handle-aliasing setup one level removed from
   raw memory (whatever component storage keys off this id will alias too).
   The same aliasing happens more directly if the same id is present in both the
   recycled list and the alive list — the very next `Create()` call resurrects it.
5. Separately, an out-of-range `id` (e.g. near `IDType`'s max for a corrupted file)
   drives `EntityTable::GetOrCreateSegment`'s `m_segmentIndex.resize(segIdx + 1, ...)`
   (`EntityTable.hpp:465-468`) with an attacker-influenced size — an uncontrolled
   allocation from untrusted input, with no `SerializationError` raised even if it
   somehow succeeds.

Fix: validate every restored id (`id <= Entity::ID_MASK`, alive/recycled ids `<
nextFreshID`), reject duplicates across the alive and recycled sets, and return
`SerializationError::CorruptedData` on violation instead of silently accepting the
data.

**C2. No synchronization anywhere in `EntityManager`/`EntityTable`/`EntityIDStack` — concurrent `Destroy()` calls can double-recycle the same id, which is then issued to two different callers as two "live" entities.**
Whole-file: `EntityManager.hpp` (`Create` `:54-65`, `Destroy` `:107-137`), `EntityTable.hpp`
(`SetVersion` `:119-146`, `Destroy` `:164-186`), `EntityIDStack.hpp`
(`Allocate`/`Recycle` `:44-61`, `:96-105`) — none contain a mutex, atomic, or any
other synchronization primitive, and no comment states a "single-writer" contract.

Concrete race: two threads T1 and T2 both call `EntityManager::Destroy(e)` for the
same live entity `e` at (nearly) the same time.

```cpp
bool Destroy(Entity entity) noexcept
{
    if (!IsValid(entity)) return false;      // (EntityManager.hpp:109) both T1 and T2 can pass this
    ...
    m_table.Destroy(id);                      // (EntityManager.hpp:131) both threads execute this
    m_idStack.Recycle(id, nextVersion, true);// (EntityManager.hpp:134) id pushed onto m_recycledIDs TWICE
    return true;
}
```

Both threads observe `IsValid(entity) == true` before either mutates state (classic
TOCTOU), so `id` is pushed onto `EntityIDStack::m_recycledIDs`
(`EntityIDStack.hpp:179`) twice. `EntityTable::aliveCount`/`m_totalAlive` are also
decremented twice for one logical destroy (`EntityTable.hpp:175-177`), silently
under-counting `AliveCount()`. The next two `Create()` calls both pop an entry for
`id` (with the same recycled version, since both entries were pushed with identical
`nextVersion`) and hand the *identical* `Entity` value out to two different callers —
each believes it uniquely owns that entity. This is direct handle-aliasing/ABA:
either caller destroying "their" copy invalidates the other's live handle
unexpectedly, and both callers can simultaneously read/write whatever component data
is keyed by that id.

Even without the exact double-recycle race, plain data races on `m_totalAlive`,
`segment->aliveCount`, and the `m_segments`/`m_segmentIndex` vectors
(`EntityTable.hpp:577-586`) under concurrent `Create`/`Destroy` are undefined
behavior per the C++ memory model regardless of outcome.

Fix at minimum: document the threading contract explicitly at class level (e.g.
"`EntityManager` is not thread-safe; `Create`/`Destroy`/`CreateBatch`/`DestroyBatch`
require external synchronization for any concurrent use"). If concurrent access from
multiple threads is meant to be supported directly, this subsystem needs real
synchronization (mutex/spinlock around the id-stack and per-segment version writes,
or a lock-free design), not just internal `noexcept` markers.

### Important

**I1. Version-wraparound handling truncates the packed version field instead of wrapping it — a documented, supported entity configuration silently corrupts and leaks entities.**
Root cause: `Entity.hpp:59-61` (packing constructor doesn't mask `version` by
`VERSION_MASK`). Manifests via: `EntityManager.hpp:124-128` (`Destroy`) and
`EntityManager.hpp:173-177` (`DestroyBatch`).

The packing constructor masks `id` but not `version`:

```cpp
constexpr BasicEntity(StorageType id, VersionType version) noexcept :
    m_entity{static_cast<StorageType>((static_cast<StorageType>(version) << VERSION_SHIFT) | (id & ID_MASK))}
{}
```

`EntityManager::Destroy()`'s wraparound test only catches the case where incrementing
the version overflows the *underlying `VersionType`* back to exactly 0:

```cpp
VersionType nextVersion = currentVersion + 1;
if (nextVersion == NULL_VERSION)      // == 0
{
    nextVersion = INITIAL_VERSION;    // = 1
}
```

This is correct **only** when `VersionBits` equals the full bit-width of the derived
`VersionType` (i.e. exactly 8, 16, or 32 — see `EntityTraits::VersionType` selection
at `Entity.hpp:120-121`), because only then does `currentVersion + 1` naturally wrap
to 0 in `VersionType` arithmetic at the same point the packed field would overflow.
For any other `VersionBits` (which the static_asserts at `Entity.hpp:115-116` freely
permit, and which the file's own header comment advertises as a supported
configuration — *"Small: 16-bit total, 4-bit version = 4K entities, 16 versions"*,
`Entity.hpp:20`), this check never fires when the packed field actually overflows.

Concrete repro with `ASTRA_ENTITY_BITS=16`, `ASTRA_ENTITY_VERSION_BITS=4`
(`ID_BITS=12`, `VERSION_SHIFT=12`, `VERSION_MASK=0xF`, `VersionType=uint8_t`):

1. Repeatedly `Destroy()`/`Create()` the same id, driving its version 1 → 15. Each
   step matches the table because `version <= VERSION_MASK` so no truncation occurs
   yet.
2. On the 16th cycle, `currentVersion == 15`, `nextVersion = 16`. `16 != NULL_VERSION
   (0)`, so the wraparound branch does **not** trigger — `nextVersion` stays `16`.
3. `EntityTable::SetVersion(id, 16)` stores the raw value `16` in the table's
   `uint8_t` version array (no masking there either — `EntityTable.hpp:119-146`).
4. `Entity(id, 16)` is packed: `static_cast<StorageType>(16) << 12` promotes to `int`
   and evaluates to `65536`, which is then truncated by the outer
   `static_cast<StorageType>(...)` (`StorageType = uint16_t`) down to `0`. The
   resulting handle's embedded version field is **0 = `NULL_VERSION`**, not 16 mod
   16 = 0 as might be "intended" — the effect is the same either way: the version
   field is now `NULL_VERSION`.
5. `Create()` returns this handle without validating it
   (`EntityManager.hpp:54-65`). `EntityManager::IsValid()` special-cases
   `version == NULL_VERSION` as always-false (`EntityManager.hpp:199`), so this
   freshly "created" entity is **permanently invalid** from the caller's point of
   view, yet `EntityTable::aliveCount`/`m_totalAlive` were incremented as if it were
   alive (`EntityTable.hpp:128-131`).
6. Because `Destroy()` requires `IsValid()` to pass first
   (`EntityManager.hpp:109`), this id can never be destroyed/recycled again through
   the public API — it is permanently leaked from the usable id space, and
   `EntityManager::Size()` overcounts an entity nobody can ever reach or free again.
7. This repeats on every subsequent multiple of 16 (versions 16, 32, 48, ... up to
   the point the *raw* `uint8_t` counter itself naturally overflows at 256, at which
   point the `nextVersion == NULL_VERSION` check finally fires correctly by
   coincidence) — i.e. roughly 15 of every 16 reuses of an id under this
   configuration silently leak it.

The bug is invisible under the library's default configuration
(`ASTRA_ENTITY_VERSION_BITS=8`, matching `uint8_t` exactly) and under both existing
compile-check configurations (`tests/Compile/Entity16Main.cpp` uses
`VERSION_BITS=8`; `tests/Compile/Entity64Main.cpp` uses `VERSION_BITS=32`) — both
happen to be full-width-safe. It is not caught by
`EntityManagerTest.VersionWraparound` (10 cycles, default config) or
`EntityManagerSerializationTest.VersionWraparound` (300 cycles, but still default
config) — see Test coverage below.

Fix: mask `version` by `VERSION_MASK` in the packing constructor
(`Entity.hpp:59-61`) so out-of-range versions can never silently alias into the id
field or another version value, **and** fix the wraparound test in
`EntityManager::Destroy`/`DestroyBatch` to compare against `Entity::VERSION_MASK`
(`currentVersion >= Entity::VERSION_MASK`) rather than the raw-type `NULL_VERSION`
sentinel — i.e. make `EntityManager` call `Entity::NextVersion()` (which already
implements the correct check at `Entity.hpp:90`) instead of duplicating
(incorrectly) the logic inline.

**I2. Missing `static_assert` allows `VersionBits == 0`, which silently makes every entity permanently invalid.**
`Entity.hpp:112-131` (`EntityTraits`), `Entity.hpp:140-142` (global static_asserts).

The static_asserts only require `VersionBits < TotalBits`
(`Entity.hpp:116`,`:141`) — `VersionBits == 0` is not rejected. For
`ASTRA_ENTITY_BITS=16`, `ASTRA_ENTITY_VERSION_BITS=0` this compiles (the
`StorageType{1} << ID_BITS` shift in `ID_MASK`'s computation happens to be safe only
because `uint16_t` promotes to a wider `int` before the shift), producing
`VERSION_MASK == 0`. `GetVersion()` (`Entity.hpp:99`) then always returns `0`
regardless of what's packed, so `EntityManager::IsValid()`
(`EntityManager.hpp:199`, `version == NULL_VERSION`) treats **every entity, always,**
as invalid — total, silent breakage of the whole entity system for a configuration
that isn't statically rejected. (For `TotalBits` of 32/64 the same misconfiguration
happens to be caught, but only incidentally as a constexpr-evaluated-UB compile
error rather than a clear diagnostic.) Fix: add
`static_assert(ASTRA_ENTITY_VERSION_BITS >= 1, "...")` (and the equivalent bound in
`EntityTraits`) to fail the build with an actionable message.

**I3. `CreateBatch`'s large-batch path allocates a staging buffer sized to the full requested count before checking availability, with no upper bound on `count`.**
`EntityManager.hpp:91-105`.

```cpp
SmallVector<EntityIDStack::VersionedID, 256> allocations;
allocations.resize(count);                       // sized to the full request, not what's available
size_t allocated = m_idStack.AllocateBatch(count, allocations.begin());
```

`count` is caller-supplied with no sanity bound (e.g. against `Entity::ID_MASK` or
the number of ids actually still available). Since the project builds with
exceptions off, a `resize()` that would need to throw `std::bad_alloc` /
`std::length_error` on an unreasonable `count` has no `Result`-based recovery path
here — it will abort/terminate rather than gracefully reporting "0 created" the way
`CreateBatch`'s own contract promises ("returns how many were actually created").
Recommend clamping `count` against `Entity::ID_MASK` (or the sum of
`RecycledCount()` + remaining fresh ids) before allocating the staging buffer.

### Minor

**M1. `Entity::NextVersion()` and `EntityManager`'s inline recycle logic implement two different, disagreeing wraparound policies, and the former is dead code on the actual recycle path.**
`Entity.hpp:85-96` vs. `EntityManager.hpp:124-128`, `:173-177`.
`NextVersion()` returns `Invalid()` once `currentVersion >= VERSION_MASK`;
`EntityManager::Destroy`/`DestroyBatch` instead wrap back to `INITIAL_VERSION` (and,
per I1, do so incorrectly for non-full-width configs). `NextVersion()` is exercised
only directly by unit tests (`tests/Entity/EntityTest.cpp:103-122`, `:391-410`) and
is never called from `EntityManager.hpp`/`EntityTable.hpp`. Two incompatible
policies for the same concept is a maintenance hazard — consolidate on one (after
fixing I1) and have `EntityManager` call it.

**M2. Serialize/Deserialize silently drop two `EntityTable::Config` fields, reverting them to compiled-in defaults on every load.**
`EntityManager.hpp:300-305` (write) vs. `:337-344` (read) vs.
`EntityTable.hpp:36-43` (full field list). `entitiesPerSegment`,
`entitiesPerSegmentShift`, `entitiesPerSegmentMask`, `releaseThreshold`,
`autoRelease`, and `maxEmptySegments` are round-tripped, but `maxPooledSegments`
and `useHugePages` are not — they silently reset to `4` and `true` respectively on
every deserialize, regardless of what the original manager was configured with. A
user who disabled huge pages for a specific deployment target would find them
silently re-enabled after any save/load cycle.

**M3. `MaybeReleaseSegments` does a full linear scan of all segments on every call, on the `Destroy`/`SetVersion` hot path.**
`EntityTable.hpp:230-279`. Called whenever any segment's `aliveCount` drops to 0
(`EntityTable.hpp:139-141`, `:179-181`), it iterates the *entire* `m_segments`
vector every time (`EntityTable.hpp:237`) rather than tracking empty-segment
state incrementally. For workloads with many segments and frequent near-empty
churn, this is effectively O(n) per destroy that happens to empty a segment,
i.e. up to O(n²) over a churny sequence. Additionally, which segments actually get
released depends on vector iteration order, not recency of emptying.

**M4. `EntityIDStack::Recycle(id, nextVersion, bool preferLocal)` silently ignores its third parameter; the call site's comment claims a behavior that doesn't exist.**
`EntityIDStack.hpp:102-105`:
```cpp
void Recycle(IDType id, VersionType nextVersion, bool) noexcept
{
    Recycle(id, nextVersion);  // Ignore preferLocal in simple design
}
```
called from `EntityManager.hpp:134` with the comment
`// preferLocal = true for segment locality`. There is no segment-locality logic
anywhere in `EntityIDStack` — this is a dead parameter and a misleading API
surface/comment that should either be implemented or removed.

**M5. Hand-written comparison operators are fully redundant with the defaulted `operator<=>`.**
`Entity.hpp:67-76`. `operator==`, `operator<`, `operator>`, `operator<=`,
`operator>=` are all manually defined, and then `operator<=>` is *also* defaulted
on the next line. The defaulted three-way comparison alone generates all of the
above via C++20 rewritten-candidate rules, so the hand-written versions are dead
code that could silently diverge from the defaulted one in a future edit.

**M6. `BasicEntity::IsValid()` and `EntityManager::IsValid()` share a name but very different guarantees.**
`Entity.hpp:102-103` vs. `EntityManager.hpp:194-201`. The former only checks the raw
sentinel (`m_entity != Traits::INVALID`); the latter performs the real liveness
check (version + table lookup). It's easy to call the cheap bitwise one and believe
you've validated liveness — this exact confusion would mask the corrupted handles
produced by I1 (they pass `entity.IsValid()` but fail `manager.IsValid(entity)`
forever).

**M7. `EntityManager::Capacity()` name is misleading.**
`EntityManager.hpp:225-228`. Returns `m_idStack.GetNextID()` — the "next fresh id"
high-water mark, not a count of reserved/allocated memory or a usable-capacity
figure (which `Reserve()` affects independently and which this getter doesn't
reflect at all). Consider renaming to something like `NextID()` or documenting the
semantics explicitly.

## Test coverage

- `EntityManagerTest.VersionWraparound` (`tests/Entity/EntityManagerTest.cpp:176-192`)
  only runs 10 destroy/recreate cycles against the **default** configuration
  (8-bit version = 256 values) — nowhere near the `VERSION_MASK` boundary (255), so
  it cannot exercise the actual wraparound path.
- `EntityManagerSerializationTest.VersionWraparound`
  (`tests/Entity/EntityManagerSerializationTest.cpp:306-340`) does run 300 cycles
  and does cross the default config's 255→256 boundary — but the default
  configuration (`VersionBits=8`, matching `uint8_t`'s full width) is exactly the
  "safe" case per I1, so this test passes today and would continue to pass even
  with the bug present or fixed. It provides no signal either way for I1.
- No test anywhere exercises an `ASTRA_ENTITY_VERSION_BITS` value other than the two
  full-width-safe cases used by the compile-check mains
  (`tests/Compile/Entity16Main.cpp` uses 8, `tests/Compile/Entity64Main.cpp` uses
  32). The "4-bit version" example explicitly documented in `Entity.hpp:20`'s own
  header comment is never compiled or tested anywhere in the suite — this is the
  gap that let I1 ship.
- `Entity::NextVersion()`'s overflow-to-`Invalid()` contract is unit-tested
  (`tests/Entity/EntityTest.cpp:113-122`, `VersionOverflow`) but there is no
  equivalent test pinning down `EntityManager::Destroy()`/`DestroyBatch()`'s
  behavior at the real `VERSION_MASK` boundary for a non-default config — the two
  code paths implement different policies (M1) and neither is cross-checked
  against the other.
- `EntityManagerSerializationTest.cpp` (11 tests) only ever exercises
  round-trips of *valid* data produced by the same library build. No test feeds
  `Deserialize` malformed/adversarial input (out-of-range ids, ids duplicated
  between the alive and recycled lists, `nextFreshID` beyond `ID_MASK`, truncated
  streams beyond what `BinaryReader`'s generic size checks already catch) — this
  is the gap behind C1.
- No concurrency test exists for `Create`/`Destroy` (consistent with there being no
  synchronization to test), but there is also no test or static assertion pinning
  down the intended single-threaded/external-synchronization contract for future
  contributors (C2).
- `EntityTable`'s segment pooling/huge-page fallback and `maxEmptySegments` behavior
  are not directly covered by the reviewed test files with targeted assertions on
  segment release timing (only indirectly, via `EntityManagerTest`/
  `EntityManagerSerializationTest` construction configs like `PreserveConfiguration`,
  `tests/Entity/EntityManagerSerializationTest.cpp:268-304`).
# Component & Resources — Review

Scope:
- include/Astra/Component/Component.hpp
- include/Astra/Component/ComponentRegistry.hpp
- include/Astra/Component/ResourceStorage.hpp

## Overview

This subsystem implements Astra's type erasure: `ComponentDescriptor` carries a set of function-pointer
"thunks" generated per-type in `ComponentRegistry::RegisterComponentImpl`, plus size/alignment/trait metadata.
`ComponentRegistry` assigns `ComponentID`s (via the process-wide `TypeContext`, `Core/TypeID.hpp`,
`Core/TypeContext.hpp`) and owns the descriptor table, keyed both by ID (dense `FlatMap`) and by stable
XXHash64 name-hash (for cross-run/cross-module lookups such as save-file loading). `ResourceStorage` is a
sparse-set of singleton "resource" instances, each stored either inline (SBO, ≤ `CACHE_LINE_SIZE` bytes) or on
the heap, addressed by the same `ComponentID` space via a fixed-size `std::array<uint16_t, MAX_COMPONENTS>`
sparse index.

Two facts drive most of the findings below and are worth stating up front:

1. **`ASTRA_ASSERT` is a hard no-op outside `ASTRA_BUILD_DEBUG`** (`Core/Base.hpp:47-52`): `#define
   ASTRA_ASSERT(condition, message) ((void)0)`. Any code path that relies on an assert alone to prevent an
   unsafe operation is a Release-build vulnerability, not a debug-time nicety.
2. **`ComponentID` is a single, process-wide, shared counter** (`TypeContext::GetOrAssignComponentID`,
   `Core/TypeContext.hpp:70-86`), handed out to *every* `TypeID<T>::Value()` caller — components, resources,
   and (per the current branch's "systems keyed by type hash" work) systems — with its own ceiling at 65535,
   not at `MAX_COMPONENTS` (128 by default). So IDs `>= MAX_COMPONENTS` are an entirely ordinary, expected
   output of `TypeID<T>::Value()` once more than 128 distinct types have touched `TypeID` anywhere in the
   process — this is not a contrived edge case, and every array indexed directly by `ComponentID` in this
   subsystem must guard against it explicitly (not via assert).

## Design assessment

The type-erasure generation itself (`ComponentRegistry.hpp:193-275`) is competently done: construct/destruct/
move/copy/assign/serialize thunks are all generated from the same `T` in the same function, so there's no
possibility of e.g. a construct thunk for one type paired with a destruct thunk for another. The
copy-constructible branch (`:158-169`) correctly leaves `copyConstruct`/`copyAssign`/`constructWith` null as a
group for move-only types, matching the `Component` concept's deliberate choice not to require copy semantics.

`ComponentDescriptor::DefaultConstruct` (`Component.hpp:84-102`) is the strongest piece of reasoning in the
file: it correctly distinguishes *value-initialization* from *default-initialization* for
trivially-default-constructible types (memset-zero is the standard-mandated result of `T{}` for such types,
even when `T` has no explicit initializers) from types with NSDMIs or user-provided constructors (which must
run through the real constructor). This is exercised correctly by
`tests/Component/DefaultConstructTest.cpp`.

Where the subsystem is weaker is in *consistency*: the same guard pattern (e.g. "id may exceed
`MAX_COMPONENTS`", "allocation may fail", "registry may have expired") is implemented correctly in some
methods of `ResourceStorage` and only assert-guarded (i.e., absent in Release) in others — specifically, the
type-erased ID-based API (`SetByID`, `Deserialize`, `GetByID`, `HasResourceByHash`) is careful and the
templated, primary user-facing API (`Set<T>`, `Emplace<T>`) is not. This is the single biggest risk area in
the subsystem; see Critical findings 1-3.

## Strengths (file:line)

- `Component.hpp:90-97` — `DefaultConstruct`'s memset-vs-real-ctor split correctly implements C++
  value-initialization semantics for trivially-default-constructible types (verified against
  `DefaultConstructTest.cpp`'s NSDMI vs. POD cases).
- `ComponentRegistry.hpp:101-109` — the `MAX_COMPONENTS` overflow guard pairs its assert with an explicit
  runtime `if` + early return and a comment explaining why (avoids silently corrupting `ComponentMask` bits).
  This is the correct pattern; it just isn't applied everywhere it needs to be (see Critical 1).
  This double-guard pattern is also mirrored correctly in `TypeContext::GetOrAssignComponentID` (assert +
  a real mutex, `Core/TypeContext.hpp:70-86`).
- `ComponentRegistry.hpp:279-281` — using `std::deque<std::string>` for `m_componentNames` (rather than
  `std::vector`) is the right call and is explicitly commented: avoids invalidating `ComponentDescriptor::name`
  `c_str()` pointers on reallocation.
- `ResourceStorage.hpp:501-580` (`SetByID`) and `:661-725` (`Deserialize`) both correctly check `lock()` for
  an expired registry and `AllocResult::ptr` for allocation failure before touching the slot, and only flip
  `slot.isValid = true` once storage is fully established — comments even flag this ordering ("must leave the
  slot invalid so teardown never frees a garbage pointer"). This is good code; see Critical 1-3 for where the
  same rigor is missing.
- `ComponentRegistry.hpp:121-135` — a Debug-time hash-collision guard exists at all, showing awareness of the
  risk of a 64-bit hash lookup table; see Important 6 for its blind spot.

## Findings

### Critical

**1. `ResourceStorage::Set<T>`/`Emplace<T>` index a fixed-size array with an assert-only bounds check — real OOB read+write in Release when `ComponentID >= MAX_COMPONENTS`.**
`ResourceStorage.hpp:130-141` (`Set`) and `:205-216` (`Emplace`):
```cpp
ComponentID id = TypeID<T>::Value();
ASTRA_ASSERT(id < MAX_COMPONENTS, "Component ID out of range");   // no-op in Release
uint16_t index = m_sparse[id];                                    // OOB read if id >= 128
...
m_sparse[id] = index;                                             // OOB write if id >= 128
```
Compare to `Get<T>`/`Has<T>` (`:90-106`, `:190-199`) and `SetByID`/`Deserialize` (`:503-505`, `:677-678`),
which all have a real `if (id >= MAX_COMPONENTS) return ...;`. `m_sparse` is
`std::array<uint16_t, MAX_COMPONENTS>` (`:796`), immediately followed in the class by `Config m_config;`
(`:797`) — an out-of-bounds write at `id == MAX_COMPONENTS` corrupts `m_config`; larger overshoots (easily
reached — see Overview, point 2) corrupt whatever follows `ResourceStorage` in its owner (e.g. `Registry`'s
other members). **Repro:** in a Release build, register (via components, resources, or any other `TypeID<T>`
consumer sharing the process context) more than `MAX_COMPONENTS` distinct types, then call
`registry.SetResource<Type129>(...)` — `Registry::SetResource` (`Registry.hpp:843-848`) forwards directly to
`ResourceStorage::Set` with no additional guard.

**2. `ResourceStorage::Set<T>`/`Emplace<T>` mishandle allocation failure — placement-new at `nullptr` in Release.**
`ResourceStorage.hpp:166-169` (`Set`, heap branch) and `:241-244` (`Emplace`, heap branch):
```cpp
AllocResult result = AllocateMemory(sizeof(T), alignof(T));
ASTRA_ASSERT(result.ptr, "Failed to allocate memory for resource");  // no-op in Release
slot.storage.heapPtr = result.ptr;                                    // nullptr on OOM
new (slot.storage.heapPtr) T(std::forward<T>(resource));              // UB: placement-new at 0
```
`AllocateMemory` (`Core/Memory.hpp:140-269`) is `noexcept` and returns `ptr == nullptr` on failure by design
(no throw, matches the project's no-exceptions constraint) — but the caller here does not check it. Contrast
with `SetByID` (`:544-546`) and `Deserialize` (`:703-704`), which both do `if (!result.ptr) ... return
false;`. This violates the stated project constraint that "caller/recoverable errors [are] handled
gracefully in ALL configs" — OOM on a large resource type crashes instead of returning an error signal.

**3. `ResourceStorage::Set<T>`/`Emplace<T>` dereference a possibly-expired `weak_ptr` registry.**
`ResourceStorage.hpp:148-150` (`Set`) and `:223-225` (`Emplace`):
```cpp
auto registry = m_componentRegistry.lock();
ASTRA_ASSERT(registry, "Component registry expired");   // no-op in Release
registry->RegisterComponent<T>();                        // null shared_ptr deref if expired
```
`ResourceStorage` intentionally stores only a `weak_ptr<ComponentRegistry>` (not a `shared_ptr`) — the whole
point of that design is to let the registry be owned/destroyed elsewhere while `ResourceStorage` survives
briefly (as documented use in `GetResourceByHash`/`HasResourceByHash`/`GetAllResources`, which all correctly
handle `lock()` returning null). `Registry`'s own member layout happens to make this unreachable through
`Registry`'s owned `ResourceStorage` (its `m_resourceStorage` is declared after `m_componentRegistry` at
`Registry.hpp:1590/1594`, so it's destroyed first), but `ResourceStorage` is a public, independently
constructible class (`ResourceStorage(std::weak_ptr<ComponentRegistry>, const Config&)`), and nothing prevents
a caller from holding one past the registry's lifetime — which is precisely the scenario the `weak_ptr`
choice exists to support.

**4. When `slot.descriptor` ends up null while `slot.isValid == true`, the destructor and heap free are silently skipped forever (wrong-lifetime-op / leak), and this state is reachable from Critical 1.**
If `RegisterComponent<T>()` is refused (e.g. via the `MAX_COMPONENTS` guard,
`ComponentRegistry.hpp:101-109`, hit through the OOB path of Critical 1) `GetComponentDescriptor(id)` returns
`nullptr`, yet `Set`/`Emplace` still do `slot.isValid = true;` unconditionally
(`ResourceStorage.hpp:153`/`:228`) *before* checking the descriptor, and the object is still placement-new'd
directly (bypassing the descriptor entirely, so `Get<T>()` keeps working — masking the problem). Every
teardown path — `Remove<T>` (`:296`), `RemoveByID` (`:606`), `Clear()` (`:328`), and both `ResourceSlot`
move-ctor/move-assign (`:749`, `:766`) — only destructs/frees `if (slot.descriptor)`. So the resource's
destructor never runs and, for heap-backed resources, `FreeMemory` never runs either: a guaranteed
skipped-lifetime-op / leak with no diagnostic, for the lifetime of the `ResourceStorage`.

### Important

**5. `Component.hpp` — `DefaultConstruct`/`Destruct` are not symmetrically guarded for empty (`size == 0`) components with non-trivial special members.**
`DefaultConstruct`/`BatchDefaultConstruct` (`Component.hpp:84-140`) unconditionally `return` when `size == 0`
— no constructor call at all. `Destruct` (`:154-157`) has no matching guard and unconditionally invokes the
destructor thunk. `ComponentRegistry` sets `size = 0` from `std::is_empty_v<T>`
(`ComponentRegistry.hpp:114`), which is purely a *layout* property (no non-static data members, no virtual
functions) — it says nothing about whether `T`'s default constructor or destructor is trivial or
side-effecting. A type like
```cpp
struct EventMarker { static inline int liveCount = 0;
    EventMarker() { ++liveCount; }
    ~EventMarker() { --liveCount; } };
```
is `std::is_empty_v` (no data members) but has real construction/destruction side effects a user would
reasonably expect to fire (this is a very plausible "tag with a lifecycle hook" pattern). With the current
code: every `DefaultConstruct` call for this type is a silent no-op (ctor never runs, `liveCount` never
incremented), while `Destruct` still runs the real, non-trivial destructor over memory whose lifetime, per the
object model, never began — UB, and concretely wrong (`liveCount` goes negative). No test in this repo
registers an empty type with non-trivial special members (`EmptyComponent`/`Player`/`DebugModeResource` in
the test suites are all plain empty structs) — this asymmetry is untested.

**6. `Component.hpp:104-120` — `ConstructWith` silently discards the source value for non-copy-constructible types, producing a wrong result with no error signal.**
```cpp
inline void ConstructWith(void* ptr, const void* value) const
{
    if (constructWith) { constructWith(ptr, value); }
    else if (copyConstruct) { copyConstruct(ptr, value); }
    else { defaultConstruct(ptr); }   // <-- 'value' is silently ignored
}
```
The `Component` concept (`Component.hpp:24-28`) does not require copy-constructibility, and
`ComponentRegistry` correctly leaves `constructWith`/`copyConstruct` both null for such types
(`ComponentRegistry.hpp:158-169`, exercised by `ComponentRegistryTest.MoveOnlyComponent`). But
`ResourceStorage::SetByID`'s new-resource branch calls `desc->ConstructWith(slot.storage.*, data)`
unconditionally on first-time resource creation (`ResourceStorage.hpp:539`, `:548`) and then returns `true`
("resource was set successfully", per the doc comment at `:492-500`) even though, for a move-only resource
type, the caller-supplied `data` was never used — the resource is default-constructed instead. This is a
public, documented entry point ("Type-erased resource setting for use by CommandBuffer") that can silently do
something other than what it claims, with a success return.

**7. `ComponentRegistry.hpp:116-117` — the over-alignment guard is assert-only, unlike the `MAX_COMPONENTS` guard two lines above it.**
```cpp
ASTRA_ASSERT(desc.alignment <= CACHE_LINE_SIZE,
             "Component alignment above 64 bytes is not supported by chunk storage");
```
No runtime `if`/early-return backs this up (contrast with `:101-109`, which pairs its assert with a real
guard). In Release, a type with `alignof(T) > CACHE_LINE_SIZE` (legal per the `Component` concept — nothing
constrains alignment) registers successfully with a `desc.alignment` that downstream storage cannot honor.
`ResourceStorage`'s SBO-vs-heap decisions check only *size* (`if (desc->size <= SBO_SIZE)` /
`if constexpr (sizeof(T) <= SBO_SIZE)` at `:156`, `:231`, `:536`, `:695`), never `desc->alignment`, before
placement-new'ing into `ResourceSlot::Storage::inlineData`. A type with size ≤ 64 but alignment > 64 (e.g.
`alignas(128) struct { char c[32]; };`) is placed at a potentially misaligned address — real UB, not merely
theoretical, for SIMD-typed resources.

**8. `ResourceStorage.hpp:732` — the SBO buffer's alignment is hardcoded to 64, but `SBO_SIZE` (and the bound `ComponentRegistry` checks against) tracks the platform's `CACHE_LINE_SIZE`, which is 128 on ARM64.**
```cpp
static constexpr size_t SBO_SIZE = CACHE_LINE_SIZE;         // :40 — up to 128 on ARM64 (Memory.hpp:52-53)
...
alignas(64) std::byte inlineData[SBO_SIZE];                 // :732 — always 64, regardless of platform
```
On a platform where `CACHE_LINE_SIZE == 128` (ARM64, via `std::hardware_destructive_interference_size`,
`Core/Memory.hpp:52-53,61`), a component whose alignment is, say, 128 and whose size is ≤ 128 passes the
size-based SBO test and (once finding 7 is fixed, or even today since that check is assert-only) would appear
to satisfy `alignment <= CACHE_LINE_SIZE`, yet the inline buffer backing it only guarantees 64-byte alignment
— an internal inconsistency between the two constants that are supposed to describe the same buffer.

**9. `ComponentRegistry.hpp` has no internal synchronization, but `RegisterComponent<T>` is reachable from ordinary runtime code, not just startup.**
`m_components`, `m_hashToID`, `m_componentNames` (`:277-281`) are plain, unsynchronized containers.
`RegisterComponent<T>` (`:23-30`) does an unsynchronized `Contains` check followed by an unsynchronized
`operator[]` insert (`:189-190`) on the shared `FlatMap`s. This is called not only at setup time but from
ordinary hot-path code: `ResourceStorage::Set`/`Emplace`/`SetByID` all call `registry->RegisterComponent<T>()`
on first use of a resource type (`ResourceStorage.hpp:150`, `:225`; `SetByID`'s equivalent path relies on the
type having been pre-registered but other call sites in the wider codebase register lazily). If two threads
race to register even two *different* types for the first time (e.g. two systems on separate worker threads
each touching a not-yet-seen component/resource type), the concurrent mutation of the shared hash maps is a
data race — unlike the ID-assignment layer directly underneath it (`TypeContext::GetOrAssignComponentID`,
`Core/TypeContext.hpp:70-86`), which is explicitly mutex-guarded. Nothing in `ComponentRegistry.hpp` documents
a single-threaded-registration requirement for callers to honor.

**10. `ComponentRegistry.hpp:121-135` — the Debug-only hash-collision guard's oracle (type-name string equality) is not reliable for anonymous-namespace or local types, and is compiled out entirely in Release.**
Two distinct types defined in anonymous namespaces in different translation units can stringify to the
*identical* text under `__FUNCSIG__`/`__PRETTY_FUNCTION__` (e.g. MSVC's `` `anonymous namespace'::Foo ``,
Clang/GCC's `(anonymous namespace)::Foo` — neither embeds a per-TU disambiguator). For such a pair, the
`XXHash64` of that text is identical (not a probabilistic collision — guaranteed), and the guard's own
oracle — `currentName == existingName` — also passes, so it does *not* fire; the two distinct types silently
share one `ComponentID`/hash slot, with descriptor and function pointers from whichever type registered
first. This is a real, deterministic footgun for a fairly common C++ idiom (helper types kept anonymous in
.cpp files), not a statistical edge case — and even where it would fire, the guard only exists under
`ASTRA_BUILD_DEBUG`, so Release builds have no collision detection at all.

### Minor

- `ComponentRegistry.hpp:74-77` — `GetAllComponentIDs()` returns the ID→descriptor map, not a collection of
  IDs; the name reads as if it returns `ComponentID`s only. Minor naming mismatch against `GetAllDescriptors`
  (`:84-95`), which does what its name says.
- `ComponentRegistry.hpp:137-141` (comment acknowledges this) — `m_componentNames` grows by one entry per
  call to `ReRegisterComponent`, with old entries for the same ID simply abandoned. For the hot-reload use
  case this deque is meant to support, a long-running process that reloads the same module repeatedly
  accumulates unbounded (if individually small) leaked string entries. Already flagged as an accepted
  trade-off in the comment, but worth confirming that's still the intended trade-off given how long-lived
  hot-reload sessions can get.
- `Component.hpp:122-140` (`BatchDefaultConstruct`) and the `MoveConstruct`/`memcpy` fast paths compute
  `count * size` / rely on `size` with no overflow guard. Unlikely to be reached in practice given bounded
  chunk sizes elsewhere in the archetype system, but worth a defensive comment noting the assumption.
- `Component.hpp` — `is_nothrow_default_constructible` and `is_nothrow_move_constructible` are recorded on
  `ComponentDescriptor` but not consulted by any method in this file (informational only from this
  subsystem's point of view; presumably consumed by archetype/chunk code outside scope).

## Test coverage

`ComponentRegistryTest.cpp` and `DefaultConstructTest.cpp` cover the "happy path" of descriptor generation
well: trivially-copyable vs. non-trivial (`Name`, has `std::string`) vs. move-only (`Resource`) vs. empty
(`Player`) types, all exercised through direct thunk calls, plus NSDMI value-init semantics via
`Registry::CreateEntity`. `ResourceTest.cpp`/`ResourcePersistenceTest.cpp` cover SBO-vs-heap sizing, alignment
up to 32 bytes, add/update/remove/clear, and save/load round-trips including an unknown-hash rejection test.

Gaps directly corresponding to the findings above — none of the following are exercised anywhere in the
suite:
- Registering more than `MAX_COMPONENTS` distinct types (Critical 1) — no test approaches the 128-type
  ceiling from any of components, resources, or systems combined.
- Simulated allocation failure for a heap-backed resource (Critical 2) — no OOM-injection test exists for
  `ResourceStorage::Set`/`Emplace`/`SetByID`/`Deserialize`.
- Constructing a `ResourceStorage` whose registry `weak_ptr` has expired before a `Set`/`Emplace`/`Get` call
  (Critical 3) — `ResourceTest` always keeps the owning `Registry` (and thus its `shared_ptr`) alive for the
  storage's whole lifetime.
- An empty (`is_empty_v`) type with a non-trivial, side-effecting constructor/destructor (Important 5) — every
  empty test type (`Player`, `EmptyComponent`, `DebugModeResource`) is a plain empty struct with implicit
  trivial special members.
- `SetByID`/CommandBuffer-style first-time resource creation for a move-only resource type (Important 6) —
  `MoveOnlyComponent`/`Resource` are exercised only through the templated component/descriptor APIs, never
  through `ResourceStorage::SetByID`.
- A component/resource type with alignment > 64 bytes (Important 7/8) — `AlignedResource` in `ResourceTest`
  uses 32-byte alignment, comfortably under `CACHE_LINE_SIZE`; nothing tests the boundary or beyond.
- Concurrent first-time registration of distinct types from multiple threads (Important 9) — `ResourceTest`'s
  only threaded test (`ConcurrentReads`) pre-registers all resources single-threaded before spawning readers.
- Two distinct anonymous-namespace types colliding on name+hash (Important 10) — not tested (would need a
  multi-TU test fixture to demonstrate).
# Registry, Views & Query — Review

## Overview

Scope: `include/Astra/Registry/Registry.hpp`, `include/Astra/Registry/View.hpp`,
`include/Astra/Registry/ViewIterator.hpp`, `include/Astra/Registry/Query.hpp`.

Read in full, plus enough of `Archetype/Archetype.hpp`, `Archetype/ArchetypeManager.hpp`,
`Archetype/ArchetypeChunkPool.hpp`, `Entity/EntityManager.hpp`, `Component/ComponentRegistry.hpp`
and `Core/Signal.hpp` to trace lifetimes, the view-refresh protocol and the empty-component
contract end to end. Cross-checked findings against
`tests/Registry/ViewInvalidationTest.cpp`, `ViewTest.cpp`, `ViewIteratorTest.cpp` and
`RegistryTest.cpp` to confirm whether each defect is actually exercised.

The headline result: the View cache-refresh protocol (structural-change counter +
removal counter + generation) is *fundamentally sound* and is applied consistently in
`ForEach`/`ParallelForEach`/`begin()` — but `Size()`/`Empty()` were never wired into it,
which reproduces exactly the dangling-Archetype* scenario the review brief called out,
and is provably not caught by the existing invalidation tests. Several more footguns
sit in the type-erased (`*ByID`) CommandBuffer-facing surface of `Registry`, where
empty/tag components are handled inconsistently with the typed API.

## Design assessment

- The `m_structuralChangeCounter` / `m_archetypeRemovalCounter` / `m_generation`
  scheme (View.hpp:196-230, ArchetypeManager.hpp:1327-1329) is a reasonable
  single-writer/multi-reader "generation stamp" design: archetypes are never moved
  once created (owned via `unique_ptr` inside a `std::vector<ArchetypeEntry>`, so
  vector growth never invalidates `Archetype*`), and removal is the only operation
  that can dangle a cached pointer — which is exactly what `m_archetypeRemovalCounter`
  forces a full rebuild for. This part is correct and well thought out.
- Ownership is via `shared_ptr<ArchetypeManager>` copied into every `View`/`Relations`,
  so a `View` outliving its `Registry` is safe (no dangling `ArchetypeManager`); the
  `ComponentRegistry` is only held by a `weak_ptr` inside `ArchetypeManager`, so
  post-Registry-destruction structural mutation attempts fail gracefully (nullptr
  returns) rather than crashing. Good defensive design.
- The chunk-capacity-is-pre-reserved invariant (`m_entities.reserve(m_capacity)` in
  `ArchetypeChunkPool.hpp`) means `ViewIterator`'s cached `Entity*`/component-array
  pointers are stable for the chunk's lifetime — no reallocation hazard from
  `push_back` within a chunk. Good.
- The weak spot is that this careful cache-invalidation discipline was applied to
  three of the View's four archetype-consuming entry points (`ForEach`,
  `ParallelForEach`, `begin()`) but not the fourth (`Size`/`Empty`), and the
  type-erased `Registry` methods added for `CommandBuffer` support don't reuse the
  typed accessors' empty-component handling, so they silently diverge in behavior.

## Strengths (file:line)

- `View.hpp:232-258` — `CollectArchetypes()` deliberately keeps empty archetypes in
  the cached list ("empty archetypes are deliberately KEPT"), which is what makes
  requirement (a) — a view sees entities later added to a pre-existing empty
  archetype — hold. Confirmed correct and positively covered by
  `ViewInvalidationTest.SeesEntityAddedToPreexistingEmptyArchetype`.
- `ArchetypeManager.hpp:596-688` — archetype removal cleans up `m_archetypeMap` and
  `m_edgeGraph` edges *before* releasing the `unique_ptr`, and bumps both counters
  atomically with release semantics only when something was actually removed —
  correctly gates the "must fully rebuild" signal for Views.
- `ViewIterator.hpp:161-173` / `Archetype.hpp:1216-1230` / `ArchetypeChunkPool.hpp:399-417`
  — the empty/tag component dereference convention (shared function-local `static`
  instance instead of nullptr-deref) is applied consistently across `ForEach`,
  `ViewIterator`, and `Chunk::GetComponent<T>`, and is ODR-safe because it's a
  template-function local static.
- `View.hpp:188-194` / `260` — sorting cached archetypes by descending entity count
  is a sound, cheap heuristic for iterating the highest-yield archetypes first.
- `Query.hpp:313-345` — `QueryBuilder::Matches` correctly implements required/
  excluded/Any/OneOf semantics, including the subtlety that multiple `Any<...>`
  modifiers are independent AND-ed constraints and `OneOf` requires *exactly* one.

## Findings

### Critical

1. **`View::Size()`/`View::Empty()` skip the refresh protocol → use-after-free of a
   freed `Archetype*` after `Defragment()`.**
   `View.hpp:143-156`:
   ```cpp
   ASTRA_NODISCARD size_t Size() const noexcept
   {
       size_t total = 0;
       for (const auto* archetype : m_archetypes)
           total += archetype->GetEntityCount();
       return total;
   }
   ASTRA_NODISCARD bool Empty() const noexcept { return Size() == 0; }
   ```
   Every other archetype-consuming entry point (`ForEach` at `View.hpp:68`,
   `ParallelForEach` at `View.hpp:85`, `begin()` at `View.hpp:175`) calls
   `EnsureArchetypes()` first. `Size()`/`Empty()` do not, and iterate the View's
   *stale* cached `m_archetypes` vector directly.
   `Registry::Defragment()` → `ArchetypeManager::Defragment()` →
   `CleanupEmptyArchetypes()` (`ArchetypeManager.hpp:596-688`) actually frees empty
   archetypes via `m_archetypes[index].archetype.reset()` (line 669) once the
   archetype count exceeds `minArchetypesToKeep`. It correctly bumps
   `m_structuralChangeCounter`/`m_archetypeRemovalCounter` (lines 683-684) — but
   nothing forces a View to observe that unless it calls `EnsureArchetypes()`.
   Repro:
   ```cpp
   Astra::Registry reg;
   auto a = reg.CreateEntity<VPos>();
   auto b = reg.CreateEntity<VPos, VVel>();
   auto view = reg.CreateView<VPos>();
   view.ForEach([](Astra::Entity, VPos&) {});     // caches both archetypes

   reg.DestroyEntity(a); reg.DestroyEntity(b);
   Astra::Registry::DefragmentationOptions opts;
   opts.minArchetypesToKeep = 1;
   reg.Defragment(opts);                          // frees both cached Archetype*

   view.Size();                                   // UAF: dereferences freed memory
   ```
   `tests/Registry/ViewInvalidationTest.cpp::SurvivesArchetypeRemoval` (lines 25-45)
   exercises this exact removal scenario but only calls `view.ForEach(...)`
   afterwards (which correctly refreshes) — it never calls `Size()`/`Empty()` post-
   Defragment, so the suite does not catch this. The companion test
   `SeesEntityAddedToPreexistingEmptyArchetype` *does* assert on `Size()`/`Empty()`
   (lines 20-22), but only after a `ForEach()` call already refreshed the cache, so
   it doesn't exercise the no-prior-refresh path either.
   Even without a removal, the same missing call means `Size()`/`Empty()` silently
   *undercount* relative to what `ForEach`/iteration would show whenever new
   matching archetypes were created since the View's last refresh (violates
   review requirement (c), "Size()/Empty() agree with iteration") — this is the
   non-crashing sibling of the same bug.
   **Fix:** call `EnsureArchetypes()` (non-const, so `Size()`/`Empty()` can no
   longer be `const`/`noexcept` — or make `EnsureArchetypes` mutate through a
   `mutable` cache) at the top of both methods, exactly like the other three entry
   points.

### Important

2. **`View`'s public constructor null-derefs `m_archetypeManager` before any of the
   class's own null-guards apply.**
   `View.hpp:40-51`:
   ```cpp
   explicit View(std::shared_ptr<ArchetypeManager> manager,
                 std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
       m_archetypeManager(manager), ...
   {
       CollectArchetypes();                 // internally null-checks, OK
       m_lastRefreshCounter = m_archetypeManager->m_structuralChangeCounter.load(...); // CRASH if manager == nullptr
       m_lastGeneration = m_archetypeManager->m_generation;
       m_lastRemovalCounter = m_archetypeManager->m_archetypeRemovalCounter.load(...);
   }
   ```
   Every other member (`IsValid()`, `ForEach`, `ParallelForEach`, `EnsureArchetypes`,
   `CollectArchetypes`) explicitly tolerates `m_archetypeManager == nullptr` ("//
   Registry destroyed" comments throughout), which signals the class is designed to
   be null-safe. The constructor itself is not: `Astra::View<Position> v(nullptr);`
   is valid, compiling code that immediately dereferences a null shared_ptr. Not
   reachable through `Registry::CreateView()` today (Registry never hands out a null
   `m_archetypeManager`), but it's a public, `explicit`-but-otherwise-unguarded
   constructor and the class's own documentation/behavior elsewhere implies this
   should be tolerated. **Fix:** guard the three post-`CollectArchetypes()` reads
   with the same `if (!m_archetypeManager)` check used everywhere else.

3. **Iterator invalidation contract for structural mutation mid-iteration is
   undocumented and untested; it silently skips entities.**
   Both `View::ForEachImpl`/`ForEachWithOptional` (`View.hpp:263-308`) and
   `ViewIterable::Iterator` (`ViewIterator.hpp:24-222`) read `chunk->GetCount()`,
   the entities array, and component arrays *live* each time a chunk is visited/
   cached, with no snapshot of "which entities existed when iteration started."
   Combined with `Archetype`'s swap-and-pop removal (`Archetype.hpp:336-365`,
   `RemoveEntities` at 367-428): if the callback passed to `view.ForEach(...)` (or
   a range-`for` over the view) destroys an entity, or adds/removes a component on
   an entity, whose new location lands *earlier* in the same chunk than the
   iterator's current position, the entity that gets swapped into the vacated slot
   is silently **never visited** for that pass (the iterator has already advanced
   past that index). No crash, no assertion — just an entity quietly missed. This
   is a classic, well-known archetype-ECS hazard, but nothing in `View.hpp` or
   `ViewIterator.hpp` documents the contract ("do not mutate archetype membership
   of entities matched by this view while iterating it"), and the only existing
   coverage (`ViewIteratorTest.ModifyDuringIteration`,
   `ViewIteratorTest.hpp:206-237`; `ViewTest.ModificationDuringIteration`,
   `ViewTest.hpp:357-382`) only mutates component *values* in place, never
   triggers an archetype move (no `RemoveComponent`/`AddComponent`/`DestroyEntity`
   inside the loop body). **Fix:** add a doc comment on `View::ForEach`/`begin()`
   stating the contract explicitly (defer structural mutations via a command
   buffer, or document that entities may be skipped/duplicated), and add a test
   that removes a component/destroys an entity mid-loop to make the behavior
   explicit and regression-proof either way.

4. **`Registry::Clear()` silently orphans any `View`/`Relations` created before the
   call — they keep iterating frozen stale data forever, and there is no way to
   detect it.**
   `Registry.hpp:1004-1017`:
   ```cpp
   void Clear()
   {
       ...
       m_archetypeManager = std::make_shared<ArchetypeManager>(m_componentRegistry, m_config.chunkPoolConfig);
       m_relationshipGraph->Clear();
       m_entityManager.Clear();
   }
   ```
   This *replaces* `m_archetypeManager` with a brand-new instance rather than
   clearing the existing one in place. Any `View` created earlier via
   `CreateView()` holds its own `shared_ptr<ArchetypeManager>` copy (`View.hpp:365`)
   pointing at the *old* (now orphaned but kept alive) manager — it is not freed
   (no UAF), but it is permanently disconnected from the "live" registry: it will
   never see new entities created after `Clear()`, and `View::IsValid()`
   (`View.hpp:57-60`, doc: "Check if the View is still valid (Registry not
   destroyed)") returns `true` forever regardless, since it only checks for a null
   pointer, not identity/freshness against the owning Registry. `RegistryTest.
   ClearRegistry` (`RegistryTest.cpp:228-250`) does not create a View before
   calling `Clear()`, so this gap is untested. **Fix:** either document this
   loudly (Views must be re-created after `Clear()`), or give `View::IsValid()`
   a way to detect it (e.g. compare against a generation/epoch on `Registry`
   rather than just null-checking `m_archetypeManager`).

5. **Type-erased `AddComponentByID`/`RemoveComponentByID` silently drop the
   `ComponentAdded`/`ComponentRemoved` signal for empty (tag) components, unlike
   their typed counterparts.**
   `ArchetypeChunkPool.hpp:510-524` sets `base = nullptr` for every zero-size
   component array (by design — tags carry no data). `Registry::AddComponentByID`
   (`Registry.hpp:470-502`) and `RemoveComponentByID` (`Registry.hpp:527-562`) both
   gate signal emission on that pointer being non-null:
   ```cpp
   void* compPtr = chunks[...]->GetComponentArrayByID(componentId);
   if (compPtr)   // always nullptr for a tag component — signal silently skipped
   {
       ...
       m_signalManager.Emit<Events::ComponentAdded>(entity, componentId, actualPtr);
   }
   ```
   even though `AddComponentByID` (and `RemoveComponentByID`) *did* successfully
   add/remove the component (`result`/return value is `true`). Compare with the
   typed path: `Registry::AddComponent<T>` → `ArchetypeManager::AddComponent<T>` →
   `Archetype::GetComponent<T>` → `Chunk::GetComponent<T>` (`ArchetypeChunkPool.hpp:
   399-417`), which returns a pointer to a shared `static T emptyInstance{}` for
   empty `T` — never nullptr — so the typed API *does* emit the signal for tags.
   Since these `*ByID` entry points are explicitly documented as "for use by
   CommandBuffer," any CommandBuffer-driven system that reacts to `ComponentAdded`/
   `ComponentRemoved` for a tag component (an extremely common ECS pattern —
   "Dirty", "Active", "Visible" markers) will silently never fire when the tag is
   applied via a command buffer, while the exact same operation performed via the
   typed API fires correctly. No test in `tests/Registry` exercises signal
   emission for these `*ByID` methods at all. **Fix:** special-case zero-size
   components the same way the typed path does (or the way `InspectEntity` already
   does at `Registry.hpp:729-738`, which correctly treats `compArray == nullptr &&
   desc.size == 0` as "empty component, still emit/represent it" rather than "no
   component").

6. **`GetComponentByHash`/`GetComponentByName` return `nullptr` for a tag component
   the entity actually has**, for the identical reason as #5.
   `Registry.hpp:588-616`:
   ```cpp
   void* compArray = chunks[...]->GetComponentArrayByID(componentId);
   if (!compArray) return nullptr;   // also true for present-but-empty components
   ```
   `HasComponentByHash` (`Registry.hpp:637-652`) correctly returns `true` for the
   same entity/component (it tests the mask, not the array pointer), so
   `HasComponentByHash(e, h) == true && GetComponentByHash(e, h) == nullptr` is a
   directly observable contradiction for any tag component. These methods are
   explicitly documented "for reflection/runtime access" and used for
   editor/scripting integration (`GetComponentByName` forwards to
   `GetComponentByHash`) — a binding that uses "non-null == present" (the natural
   reading of a `void*`-returning accessor) will report the entity as missing the
   tag. `InspectEntity` (`Registry.hpp:703-745`) already gets this right by
   explicitly branching on `desc.size > 0` and treating `data == nullptr` as a
   deliberate "empty component" sentinel documented at the call site — the hash/
   name accessors should follow the same convention (or document that `nullptr`
   is ambiguous between "absent" and "present but empty" and tell callers to
   check `HasComponentByHash` first).

7. **`AddComponentsByID`/`RemoveComponentsByID` skip `EntityManager::IsValid()`
   dead-handle filtering that every other batch mutator performs.**
   `Registry.hpp:515-518` and `573-576`:
   ```cpp
   size_t AddComponentsByID(std::span<Entity> entities, ComponentID componentId, const void* data, size_t dataSize)
   {
       return m_archetypeManager->AddComponentsByID(entities, componentId, data, dataSize);
   }
   size_t RemoveComponentsByID(std::span<Entity> entities, ComponentID componentId)
   {
       return m_archetypeManager->RemoveComponentsByID(entities, componentId);
   }
   ```
   Contrast with `AddComponents<T>` (`Registry.hpp:318-351`), `EmplaceComponents<T>`
   (`353-389`), `RemoveComponents<T>` (`391-433`) and `DestroyEntities` (`226-266`),
   which all build a `validEntities` list via `m_entityManager.IsValid(entity)`
   before touching the archetype layer, and even the *singular* `AddComponentByID`/
   `RemoveComponentByID` (`470-502`, `527-562`) call `m_entityManager.IsValid(entity)`
   first. The batch `*ByID` overloads instead rely entirely on
   `ArchetypeManager::m_entityMap.find(entity)` (inside the per-entity loops in
   `ArchetypeManager.hpp:427-444`, `478-494`) succeeding or failing to reject dead
   handles. In practice a destroyed entity is removed from `m_entityMap` by
   `RemoveEntity`, so this is *probably* safe today — but it's an inconsistent
   trust boundary in the exact API surface (CommandBuffer type-erasure) that most
   needs consistent guarding, and if `m_entityMap` and `EntityManager`'s alive-table
   were ever to diverge (e.g. a partial-failure code path), this is the one entry
   point that wouldn't catch it. **Fix:** filter through `m_entityManager.IsValid()`
   here too, for consistency with every sibling method.

8. **`CreateEntities`/`CreateEntitiesWith` silently no-op, indistinguishably from
   `count == 0`, when the caller's output buffer is too small — and return no
   count at all.**
   `Registry.hpp:144-149`:
   ```cpp
   template<Component... Components>
   void CreateEntities(size_t count, std::span<Entity> outEntities)
   {
       if (count == 0 || outEntities.size() < count)
           return;
       ...
   }
   ```
   (identical guard in `CreateEntitiesWith`, `Registry.hpp:182-186`). Both methods
   return `void`. On the *entity-ID-space-exhausted* partial-failure path, the
   function is careful to mark unfilled slots `Entity::Invalid()` (lines 151-154)
   so the caller can detect how many were created by scanning for invalid
   entries — but on the *buffer-too-small* caller-error path, nothing is written
   to `outEntities` at all, and there is no return value to distinguish "you
   passed `count == 0`" from "your span was too small" from "everything
   succeeded." `EntityManager::CreateBatch` (which both wrap) already computes and
   returns the actual `created` count (`EntityManager.hpp:70-105`) — that
   information is simply discarded at the `Registry` layer. No test in
   `RegistryTest.cpp` exercises the `outEntities.size() < count` path. **Fix:**
   return the actual creation count (`size_t`) from both methods, mirroring
   `AddComponentsByID`/`RemoveComponentsByID`'s existing count-return convention.

9. **`ComponentAdded` signal payload is `nullptr` for every component created via
   `CreateEntities`/`CreateEntitiesWith`, even though `CreateEntitiesWith` was
   given real per-entity values.**
   `Registry.hpp:169-178` (`CreateEntities`) and `205-211`
   (`CreateEntitiesWith`):
   ```cpp
   m_signalManager.Emit<Events::ComponentAdded>(outEntities[i], TypeID<Components>::Value(), nullptr);
   ```
   versus `CreateEntity`/`CreateEntityWith` (`Registry.hpp:103-113`, `131-139`),
   which look up the real component pointer via `record->archetype->GetComponent<Components>(...)`
   and pass it. `Events::ComponentAdded::component` (`Signal.hpp:99-105`) has no
   doc comment indicating it may be null (unlike `ComponentRemoved`, which is
   explicitly documented as valid-only-during-the-handler at `Signal.hpp:107-109`).
   A handler written and tested against the single-entity API (which always gets a
   live pointer) will null-deref the moment the same component type is populated
   through a batch-creation path instead. This is a correctness-relevant asymmetry,
   not just a missing optimization: for `CreateEntitiesWith` specifically, the
   caller-supplied initial values are fully available at the call site (the
   generator already produced them) and are simply not threaded through to the
   signal. **Fix:** either resolve and pass the real pointer (entities from a
   single `AddEntities`/`AddEntitiesWith` batch land in one archetype, contiguous
   chunks — the pointer is cheap to compute per entity, same as the emplace/add
   `T` batch paths already do at `Registry.hpp:344-349`/`382-387`), or document
   the payload as nullable and update `Events::ComponentAdded`'s comment to match.

10. **The `Registry(const Registry&, const Config&)` "copy" constructor does not
    copy anything — it shares only the `ComponentRegistry` and builds an
    otherwise-empty Registry.**
    `Registry.hpp:79-87`:
    ```cpp
    explicit Registry(const Registry& other, const Config& config = {}) :
        m_entityManager(config.entityManagerConfig),           // fresh, NOT copied from other
        m_componentRegistry(other.m_componentRegistry),        // shared
        m_archetypeManager(std::make_shared<ArchetypeManager>(m_componentRegistry, config.chunkPoolConfig)), // fresh, empty
        m_relationshipGraph(std::make_shared<RelationshipGraph>()),  // fresh, empty
        m_resourceStorage(m_componentRegistry, config.resourceStorageConfig),  // fresh, empty
        ...
    {}
    ```
    No entity, archetype, resource, or relationship data from `other` is copied.
    The signature and name (`Registry(const Registry&, ...)`, directly callable as
    `Registry copy(*registry);` thanks to the defaulted second parameter) strongly
    read as copy-construction. The project's own test for this constructor is
    literally named `RegistryTest.CopyConstructor` (`RegistryTest.cpp:526-547`)
    and only asserts that the component registry is shared and that a *newly
    created* entity works — it never asserts that `other`'s pre-existing entities
    appear in the "copy" (they don't), which suggests the test author's own mental
    model matched the misleading name rather than the actual behavior. There is no
    doc comment on the constructor clarifying this. **Fix:** rename (e.g. a named
    factory `Registry::WithSharedComponentRegistry(other.GetComponentRegistrySharedPtr(), config)`)
    or add a prominent doc comment; at minimum, rename the test so its title
    doesn't reinforce the wrong mental model.

11. **Batch APIs taking `std::span<Entity>` don't document (or, as far as can be
    traced from the `Registry` facade, guard against) duplicate `Entity` values.**
    `AddComponents<T>`/`EmplaceComponents<T>`/`RemoveComponents<T>`
    (`Registry.hpp:318-433`), `DestroyEntities` (`226-266`),
    `AddComponentsByID`/`RemoveComponentsByID` (`515-576`) all accept a span and
    forward it (after only an *entity-alive* filter, not a *uniqueness* filter) into
    `ArchetypeManager`'s batching/grouping helpers (`GroupEntitiesByArchetype`,
    `BatchMoveEntitiesInternal`, `RemoveEntities`), which build per-archetype lists
    of `(Entity, EntityLocation)` and process them assuming each `EntityLocation`
    is visited once. A duplicate entity in the input span produces two entries
    with what was, at snapshot time, the *same* location; after the first is
    processed the location is stale, so the second reference — depending on the
    downstream operation — can mis-index into whatever entity now occupies that
    swapped-to slot. This mechanism lives inside `ArchetypeManager.hpp` (outside
    this file's scope) so it hasn't been traced to a specific crash here, but the
    contract gap is squarely in the `Registry` public API surface: nothing in the
    method docs says "the span must not contain duplicates," and there's no
    dedup/guard at the facade. Flagging for cross-reference with the
    Archetype/ArchetypeManager reviewer, and recommending the `Registry`-level
    docs state the constraint explicitly regardless of where it's ultimately
    fixed.

### Minor

- `View.hpp:57-60` — `IsValid()`'s doc comment ("Check if the View is still valid
  (Registry not destroyed)") is inaccurate: it only reflects a moved-from `View`
  (default-constructed-via-move leaves `m_archetypeManager` null); it never
  reflects actual Registry destruction (the `ArchetypeManager` is kept alive by the
  View's own `shared_ptr`) and, per finding #4, doesn't reflect `Clear()` either.
  Fix the comment or the semantics.
- `Query.hpp:246-263` — `QueryBuilder::GetRequiredMask()`/`GetOptionalMask()`/
  `GetExcludedMask()` rebuild a `ComponentMask` from scratch on every call
  (including every call to `Matches()`, once per archetype in
  `CollectArchetypes`/`EnsureArchetypes`) even though the result is a compile-time-
  deterministic function of `QueryArgs...`. Cheap to memoize as a function-local
  `static const ComponentMask` per template instantiation.
- `View.hpp:323-330` (`ParallelForEachChunkWithOptional`) recomputes the
  `hasOptional[]` array (an `archetype->HasComponent<T>()` mask test per optional
  type) once *per chunk* rather than once per archetype, even though presence is
  an archetype-level property. Minor redundant work on the parallel hot path.
- `Query.hpp:139-141` (`Detail::AllComponents`) is defined but has zero references
  anywhere in `include/` — dead code.
- Nothing prevents nonsensical/degenerate query packs from compiling silently:
  duplicate component types (`View<Position, Position>` — `GetRequired` doesn't
  dedupe, unlike the `UniqueTypes` machinery used for `AllComponents`) or
  self-contradictory modifiers (`Not<T>, Optional<T>` for the same `T`). Both
  "work" (harmlessly) rather than failing to compile; a `static_assert` catching
  duplicates would be cheap and would catch a plausible copy-paste mistake.
- `Registry.hpp:1004-1009` — `Clear()`'s `if (m_signalManager.IsSignalEnabled(...))
  { // TODO: Consider if we want to emit signals during Clear() }` is dead code
  (the branch body is only a comment). `EntityDestroyed` is silently *not* emitted
  during `Clear()`, unlike `DestroyEntity`/`DestroyEntities` — an acknowledged,
  shipped inconsistency in the signal contract.
- `Registry.hpp:1220-1223` / `ArchetypeManager.hpp:533-536` — `GetAllArchetypes()`/
  `GetArchetypes()` return a lazy `std::ranges::transform_view` bound by reference
  to the live `ArchetypeManager`'s internal vector. Storing this range across a
  `Clear()` call (which swaps in a brand-new `ArchetypeManager`, see finding #4) or
  across Registry destruction dangles. Not documented; low risk since the natural
  usage (`for (auto* a : registry.GetAllArchetypes())`) doesn't hit it.
- `View.hpp:80-141` (`ParallelForEach`) has no doc comment stating that `Func` may
  be invoked concurrently from multiple worker threads when a real
  `IWorkScheduler` is injected, and therefore must be safe for concurrent/
  reentrant invocation (captures aside — the entity/component split is safe by
  construction, but shared mutable state captured by the callback is the caller's
  problem). Worth a one-line doc comment given how easy it is to write a
  `ParallelForEach` callback that closes over a plain (non-atomic) accumulator.

## Test coverage

Existing coverage is solid for the "happy path" refresh protocol
(`ViewInvalidationTest.cpp` explicitly targets requirement (a) — seeing entities
added to a pre-existing empty archetype — and archetype-removal survival via
`ForEach`), and `ViewIteratorTest.cpp`/`ViewTest.cpp` cover multi-archetype,
multi-chunk, Optional/Not/Any query shapes reasonably well. Gaps found while
reviewing, all directly tied to findings above:

- **No test calls `Size()`/`Empty()` without a prior `ForEach()`/`begin()` in the
  same test**, which is precisely the condition that exposes finding #1 (both the
  UAF-after-Defragment case and the plain staleness case). This is the single
  highest-value test to add.
- **No test mutates archetype membership (destroys an entity, adds/removes a
  component) from inside a live `ForEach`/range-`for` loop body** — only in-place
  component-value mutation is tested (finding #3). Worth a test that asserts
  *either* the documented skip behavior *or* forces deferral via a command buffer.
- **No test constructs a `View` with a null `ArchetypeManager`** (finding #2) —
  trivial to add (`ASSERT_DEATH`/documented-UB style, or fix the code and assert
  `IsValid() == false`).
- **No test creates a `View` before calling `Registry::Clear()`** and checks its
  post-Clear behavior (finding #4) — `RegistryTest.ClearRegistry` only checks the
  registry itself.
- **Zero direct tests for `AddComponentByID`, `RemoveComponentByID`,
  `AddComponentsByID`, `RemoveComponentsByID`** in `tests/Registry/` (confirmed via
  search — only incidental unrelated matches elsewhere) — no coverage at all for
  their signal-emission behavior (findings #5, #7) or empty-component handling.
  Given these exist specifically to support `CommandBuffer`, and `CommandBuffer` is
  presumably tested elsewhere, at minimum a focused `Registry`-level test for the
  tag-component signal gap (#5) and the hash-accessor gap (#6) would be valuable.
- **No test exercises `CreateEntities`/`CreateEntitiesWith` with
  `outEntities.size() < count`** (finding #8); `BatchEntityCreation` only tests
  the success path.
- `RegistryTest.CopyConstructor` (finding #10) tests the *actual* (empty, shared-
  registry) behavior but under a name that implies something stronger — worth
  renaming regardless of whether the constructor itself changes, so the test
  doesn't keep reinforcing the misleading mental model for future readers.
# Relationships (RelationshipGraph / Relations) — Review

## Overview

`RelationshipGraph` (`include/Astra/Registry/RelationshipGraph.hpp`) stores a forest (single-parent hierarchy: `FlatMap<Entity,Entity>` child→parent plus `FlatMap<Entity, SmallVector<Entity,4>>` parent→children) and a separate symmetric peer-link adjacency (`FlatMap<Entity, SmallVector<Entity,8>>`). It memoizes BFS/upward-walk traversals in two more `FlatMap`s (`m_descendantCaches`, `m_ancestorCaches`) keyed by root/entity, invalidated wholesale via a single `std::atomic<uint32_t> m_structureVersion` bumped on every structural mutation. `Relations<QueryArgs...>` (`include/Astra/Registry/Relations.hpp`) is a thin, optionally-component-filtered iteration façade over a `shared_ptr<const RelationshipGraph>` plus `shared_ptr<ArchetypeManager>`, used by `Registry::GetRelations()`.

Both files were read in full. `Registry.hpp` (out of scope) was consulted only to confirm the caller-side contract: `Registry::SetParent/AddLink/RemoveParent/DestroyEntity` all gate on `m_entityManager.IsValid(entity)` before touching the graph, and `DestroyEntity` reliably calls `OnEntityDestroyed` before recycling the id. So live-entity validity is enforced one layer up; `RelationshipGraph` itself has no way to distinguish a dead-but-bit-pattern-valid `Entity` from a live one (`Entity::IsValid()` only checks against the `INVALID` sentinel, see `Entity.hpp:102`).

## Design assessment

- **Forest, not DAG.** Single parent per child by construction, so `IsAncestorOf` cycle-checking a straight-line walk is the right complexity model *if* the stored data is guaranteed acyclic — but that guarantee is enforced only at the `SetParent` call site, not structurally, and `Deserialize` bypasses it entirely (see Critical #3/#4).
- **Locking is asymmetric and misleading.** `m_cacheMutex` (a `shared_mutex`) protects only `m_descendantCaches`/`m_ancestorCaches`. The actual graph topology — `m_parents`, `m_children`, `m_links` — has *zero* synchronization anywhere in the class. The class comment ("Mutex for thread-safe cache access") reads as if the type is meaningfully thread-safe; it is only thread-safe for repeated reads of an otherwise-quiescent graph. `Relations<>` holding `shared_ptr<const RelationshipGraph>` plus shipping a first-party `ParallelForEachDescendant` (worker-thread fan-out over `IWorkScheduler`) invites exactly the concurrent-mutation-during-read scenario the class doesn't defend against.
- **Fast-path vs. safe-path inconsistency already visible in the codebase.** `Registry::GetChildren()` / `Registry::RemoveAllChildren()` (`Registry.hpp:1277-1288`, out of scope but instructive) explicitly copy the children list (`auto children = GetChildren(parent);`, itself returning `std::vector<Entity>` by value) *before* iterating and calling `RemoveParent` on each — i.e., the authors already know iterating a live relationship container while mutating it is unsafe, and defend against it there. `Relations<>::ForEachChild`/`ForEachLink` (in scope) take the opposite, unguarded path — a `const auto&` reference straight into `RelationshipGraph`'s owned storage — specifically to avoid that copy. That performance choice removes the safety net the rest of the codebase relies on (see Critical #1/#2).
- **Cache invalidation is coarse (whole-graph epoch), not scoped.** Any `SetParent`/`RemoveParent`/`OnEntityDestroyed`-with-structural-change anywhere invalidates *every* entity's cached descendant/ancestor list, not just the affected subtree/path. Correctness-safe, but a real perf cliff for churny hierarchies (e.g., per-frame reparenting in a game) — every cached traversal elsewhere gets rebuilt from scratch on next touch.
- **Orphaning-on-destroy (not cascading) is a deliberate, reasonable design choice**, consistent with the tests (`ComplexRelationshipTest.OrphaningBehavior`, `RelationsTest`/`RelationshipGraphTest` `EntityDestruction`) — children of a destroyed parent survive as orphans rather than being recursively destroyed. Not a defect, just worth naming since the task flags "orphan/dangling" as a category: these orphans are *intentional* and cleanly detached (no dangling parent pointer left behind), which is correct behavior.

## Strengths (file:line)

- `RelationshipGraph.hpp:152,157-158` — `SetParent` gracefully rejects invalid/self/cyclic input via early `return` with **no** `ASTRA_ASSERT` on caller-recoverable input, matching the stated contract (and per the code comment, this was previously an assert — the fix is verified present).
- `RelationshipGraph.hpp:229-230` — `AddLink` likewise rejects invalid/self links without asserting.
- `RelationshipGraph.hpp:589-623` / `635-650` — `BuildDescendantCache`/`BuildAncestorCache` both use a `FlatSet<Entity> visited` guard while walking, so even if the underlying `m_children`/`m_parents` data were malformed (e.g. via a corrupted load — see Critical #4), the *cache-building* BFS/walk itself cannot infinite-loop; it terminates via the visited-set check. (Ancestor cache then has a separate problem handling that case — see Critical #4.)
- `RelationshipGraph.hpp:179-190` — swap-and-pop `RemoveParent` and `RelationshipGraph.hpp:295-348` `OnEntityDestroyed` correctly touch every place an entity can appear (own parent entry, parent's children list w/ empty-list cleanup, both sides of every link, both cache maps) — no residual dangling entries for the *destroyed entity itself* after cleanup (the correctness problem is elsewhere: containers mutated *while another caller is mid-iteration over them*, see Critical #1/#2).
- `tests/Comprehensive/ComplexRelationshipTest.cpp:407-493` (`RapidRelationshipChanges`) — strong fuzz-style regression test that explicitly walks every entity's ancestor chain post-chaos to prove no cycle ever slipped through the live `SetParent` cycle guard, not just "didn't crash."
- `tests/Registry/RelationshipGraphTest.cpp:370-420` — thorough 2/3/4-level cycle-rejection coverage via the live API.

## Findings

### Critical

**1. `ForEachChild`/`ForEachLink` hold a live reference into `RelationshipGraph`'s storage across the user callback; a callback that removes/destroys one of the entities being iterated corrupts the in-progress iteration.**
`Relations.hpp:138` (`const auto& children = m_relationsGraph->GetChildren(m_rootEntity);`) and `Relations.hpp:188` (same pattern for links) bind directly to the `SmallVector` stored inside `RelationshipGraph::m_children`/`m_links` (`RelationshipGraph.hpp:208-212`, `272-276` return `const ChildrenContainer&`/`const LinksContainer&`, not copies). The loop body in `ForEachEntityList` (`Relations.hpp:268-293`) is a plain range-`for` over that reference while invoking arbitrary user `Func`.
Failure scenario — the natural "destroy every child" idiom:
```cpp
registry.GetRelations(parent).ForEachChild([&](Entity child){
    registry.DestroyEntity(child);
});
```
`DestroyEntity(child)` → `RelationshipGraph::OnEntityDestroyed` → `RemoveParent(child)` (`RelationshipGraph.hpp:171-201`) performs a **swap-and-pop** on `m_children[parent]` — the *very container the outer range-`for` is iterating*: `*it = std::move(children.back()); children.pop_back();` (`RelationshipGraph.hpp:185-189`). This moves a not-yet-visited element into an already-visited slot, so that element is silently skipped — some children never get their callback invoked (in a "destroy all children" pattern, some children survive un-destroyed). If the removed child was the *last* one, `m_children.Erase(parent)` (`RelationshipGraph.hpp:195`) runs, which destructs the `SmallVector` object in place (`FlatMap::Erase` → `FlatMap.hpp:636`, `std::allocator_traits<...>::destroy(...)` → `SmallVector::~SmallVector()`). If that vector had been heap-promoted (>4 children), the destructor frees its heap buffer (`SmallVector.hpp:122-125`) *while the outer range-`for`'s `begin()/end()` (raw `T*`) still point into it* — the remainder of the outer loop then dereferences freed memory (heap-use-after-free). Even in the small-buffer case (≤4 children) where nothing is actually deallocated, the skip-a-child correctness bug is 100% reproducible and requires no unlucky timing.
Contrast with `Registry::RemoveAllChildren` (`Registry.hpp:1282`, out of scope but confirms the authors know this pattern is unsafe): it explicitly copies (`auto children = GetChildren(parent);`, itself `std::vector<Entity>` by value) before iterating+removing. `Relations<>::ForEachChild`/`ForEachLink` do not.
*Fix*: snapshot the container (copy into a local `SmallVector`/`vector`) before invoking the callback loop, or otherwise document/enforce that structural mutation of `parent`'s own children/links is illegal from within the callback and detect it (e.g. a generation counter checked after each callback invocation).

**2. `ForEachDescendant`/`ForEachAncestor`/`ParallelForEachDescendant` hold a `const TraversalCache&` across the callback loop; any reentrant cache-map mutation invalidates it → heap-use-after-free.**
`GetDescendantsCached`/`GetAncestorsCached` (`RelationshipGraph.hpp:668-693`, `696-721`) return `const TraversalCache&` — a reference into a `FlatMap` slot — after releasing `m_cacheMutex` (the lock is scoped to the function body only). `Relations.hpp:155` / `172` / `203` bind that reference into a local `const auto&` held for the *entire* iteration loop (`Relations.hpp:157-162`, `174-179`, `203-240`) while calling arbitrary user `Func`.
`FlatMap::Erase` (`FlatMap.hpp:630-642`) destroys the value in place — for a `TraversalCache` this runs `~TraversalCache()`, which destroys its `std::vector<TraversalEntry> entries`, freeing that vector's heap buffer. `FlatMap::operator[]`/`Emplace` (`FlatMap.hpp:610-613`, `472-598`) can trigger `ReserveForInsert()` → `Rehash()` (`FlatMap.hpp:720-736`, `738-779`), which allocates an entirely new backing array, move-constructs every occupied entry into it, and **deallocates the old array** (`FlatMap.hpp:778`, `m_slotAlloc.deallocate(oldSlots, oldCapacity)`).
Two independent, realistic triggers while a `ForEachDescendant`/`ForEachAncestor` loop is in flight, holding `cache`:
- *Nested/recursive traversal for a not-yet-cached entity.* `MIN_CAPACITY = 16`, `MAX_LOAD_FACTOR = 0.875` (`Swiss.hpp:34,37`), so the ~15th distinct entity ever passed to `GetDescendantsCached`/`GetAncestorsCached` triggers a rehash of `m_descendantCaches`/`m_ancestorCaches`. Any callback that calls `otherEntity.Relations().ForEachDescendant(...)` / `ForEachAncestor(...)` for a fresh entity — a very natural nested-query pattern ("for each descendant, look at its own ancestors/descendants") — can trip this. The outer `cache` reference, obtained before the rehash, now points at freed memory; continuing the outer loop (`Relations.hpp:157-162` reading `cache.entries[i]`) is a use-after-free.
- *Destroying the root (or otherwise triggering `OnEntityDestroyed`/`Clear`/`ClearCaches` on the same map slot) from within the callback.* `OnEntityDestroyed` (`RelationshipGraph.hpp:346-347`) calls `m_descendantCaches.Erase(entity)` / `m_ancestorCaches.Erase(entity)` for the destroyed entity — if that entity is the very root the outer loop is iterating (e.g. a "destroy self and descendants" pattern that destroys the root mid-traversal), the destructor of the vector backing `cache.entries` runs underneath the still-executing outer loop.
*Fix*: either return traversal caches by value/shared-ownership (e.g. `shared_ptr<const TraversalCache>` captured under the lock) so a reference outlives the map mutation, or snapshot `cache.entries` into a local copy before the callback loop begins, or add a "traversal in progress" guard that defers/rejects reentrant cache mutation.

**3. `IsAncestorOf` has no cycle guard — if the parent chain ever contains a cycle, it infinite-loops.**
```cpp
bool IsAncestorOf(Entity ancestor, Entity entity) const {
    Entity current = GetParent(entity);
    while (current.IsValid()) {           // never becomes Invalid() inside a cycle
        if (current == ancestor) return true;
        current = GetParent(current);
    }
    return false;
}
```
(`RelationshipGraph.hpp:135-145`). `SetParent` (`RelationshipGraph.hpp:147-169`) uses this as its *sole* cycle-rejection mechanism (line 157: `if (IsAncestorOf(child, parent)) return;`), and it correctly prevents cycles from being introduced *through the live API*. But it assumes the pre-existing graph is acyclic in order to terminate — it has no `visited` set and no depth bound (unlike `BuildAncestorCache`, which does — see Critical #4). If a cycle is ever present in `m_parents` (today, achievable only via `Deserialize` — see Critical #4, but this function itself has no independent defense), *every subsequent* `SetParent` call whose parent-chain walk touches that cycle spins forever: a full CPU-core hang with no crash, no timeout, and no way to recover except killing the process. This is strictly worse than the assert-on-corrupt-data problem in `BuildAncestorCache`, because it can't even fail loudly.
*Fix*: bound the walk with a `visited` set (or a depth cap derived from `m_parents.Size()`), mirroring what `BuildAncestorCache` already does, and treat a detected cycle as "not an ancestor" (safe default) rather than looping.

**4. `Deserialize` performs no integrity validation at all — a corrupted or adversarial save file can plant a cycle, a self-parent, an invalid/sentinel entity, or a `m_parents`/`m_children` inconsistency, and nothing downstream rejects it gracefully.**
`RelationshipGraph::Deserialize` (`RelationshipGraph.hpp:449-565`) reads `child`/`parent` pairs and inserts them directly into `graph.m_parents[child] = parent;` (line 477) with **no** check that `child != parent`, that `child`/`parent` are not `Entity::Invalid()`'s sentinel bit pattern, or that following the resulting chain doesn't cycle. It separately reads the `parent→children` map (lines 491-520) and the `links` map (lines 533-562) with **zero cross-validation** against the parent map (a corrupted file can have `A`'s parent recorded as `B` while `B`'s children list omits `A` or lists something else entirely — `GetParent(A)` and `GetChildren(B)` then permanently disagree) and no self-link rejection for the links map either. Every one of these checks *is* enforced by the live `SetParent`/`AddLink` API (Strengths above) — `Deserialize` is a second, unguarded entry point into the same data structures that bypasses all of them.
Concretely, a save file (or an in-memory buffer built by anything other than this library, or corrupted on disk) containing `child(A)=B, child(B)=A` reconstructs a 2-cycle. The very next `SetParent` call anywhere that walks through `A`/`B` hangs forever (Critical #3). Independently, the very next call that touches `A` or `B` via `ForEachAncestor`/`GetAncestorsCached` reaches `BuildAncestorCache`'s cycle check (`RelationshipGraph.hpp:645-650`):
```cpp
if (!visited.Insert(current).second) {
    ASTRA_ASSERT(false, "Cycle detected in parent-child relationships");
    break;
}
```
`ASTRA_ASSERT` is `assert(...)` in debug builds (`Core/Base.hpp:49`) — this **aborts the process** on what is, by the task's own definition, caller-recoverable input (a bad/corrupted/adversarial file), exactly the failure mode the codebase's recent history (per git log: "guard component-ID overflow loudly", "16/64-bit entity configurations compile again") has been actively hunting down and eliminating elsewhere. In release builds the assert is a no-op and the loop merely `break`s (no crash, but the returned ancestor list is silently truncated/wrong — a correctness bug in the opposite direction of "graceful rejection with a clear signal").
*Fix*: `Deserialize` should validate as it loads (reject/ignore self-parent pairs, reject `Entity::Invalid()` sentinel values, and either detect-and-drop cycles or run a post-load consistency pass — e.g. rebuild `m_children` from `m_parents` rather than trusting the serialized copy, which also fixes the cross-consistency problem for free) and surface a `SerializationError` (the `Result<RelationshipGraph, SerializationError>` return type already supports this) instead of silently or fatally admitting bad topology.

### Important

**5. No synchronization at all guards `m_parents`/`m_children`/`m_links`; only the *derived* cache maps get a `shared_mutex`, and this file ships a first-party multithreaded entry point (`ParallelForEachDescendant`) that can trip it directly.**
`RelationshipGraph.hpp:729-742` — `m_cacheMutex` covers `m_descendantCaches`/`m_ancestorCaches` only. `SetParent`, `RemoveParent`, `AddLink`, `RemoveLink`, `OnEntityDestroyed`, `Clear` all mutate `m_parents`/`m_children`/`m_links` with no locking whatsoever. `Relations<>` is built around `shared_ptr<const ArchetypeManager/RelationshipGraph>` and explicitly supports `ParallelForEachDescendant` (`Relations.hpp:197-241`), which fans a per-entity `func` out across worker threads via `IWorkScheduler::ParallelFor`. If `func` performs any mutating call back into the registry (`SetParent`, `DestroyEntity`, `AddLink`, ...) — a plausible pattern for, say, "cull descendants beyond a radius" — multiple worker threads can concurrently mutate `m_parents`/`m_children` with zero synchronization: a genuine data race/UB, distinct from (and in addition to) the single-threaded reentrancy bugs in Critical #1/#2. Even without `ParallelForEachDescendant`, the class's own docs/comments ("Mutex for thread-safe cache access (shared_mutex allows concurrent reads)") overstate what's actually protected, which is a footgun for any caller who assumes the whole object is thread-safe for concurrent read+write because *some* of it visibly is.
*Fix*: either document explicitly that `RelationshipGraph` requires external single-writer synchronization for all structural mutation (and that `ParallelForEachDescendant`'s callback must not mutate the graph), or extend the locking discipline to the structural maps too.

**6. Move-assignment mutates the cache maps without taking `m_cacheMutex`, inconsistent with the rest of the class's own locking discipline.**
`RelationshipGraph.hpp:110-123` — `operator=(RelationshipGraph&&)` reassigns `m_descendantCaches`/`m_ancestorCaches` directly, with no `m_cacheMutex` acquisition, unlike `GetDescendantsCached`/`GetAncestorsCached` which always go through the mutex to touch those same maps. A concurrent reader mid-`GetDescendantsCached` (holding only a shared lock, per the design) racing a move-assignment on the same object is a data race on the cache `FlatMap`s. (The copy-assignment operator, `RelationshipGraph.hpp:95-107`, has the same gap for `m_descendantCaches.Clear()`/`m_ancestorCaches.Clear()`.)

**7. Cache invalidation is a single global epoch — any structural mutation anywhere invalidates every entity's cached descendant/ancestor traversal, not just the affected subtree/path.**
`IncrementVersion()` (`RelationshipGraph.hpp:723-727`) is one atomic counter shared by the entire graph; `TraversalCache::IsValid` (`RelationshipGraph.hpp:46-49`) compares against it wholesale. This is correctness-safe (never returns stale data) but means a single `SetParent` call for one small, unrelated corner of a large forest forces every other previously-cached descendant/ancestor list in the whole graph to be rebuilt from scratch (`O(subtree size)` each) on next access. For workloads that reparent frequently while also querying hierarchies elsewhere every frame (a common game-engine pattern), this is a latent performance cliff worth calling out even though it's not incorrect.

### Minor

**8. `RemoveParent` shadows the outer `it` with a differently-typed local `it`.**
`RelationshipGraph.hpp:173` declares `auto it = m_parents.Find(child);` (a `FlatMap` iterator); `RelationshipGraph.hpp:181` re-declares `auto it = std::find(children.begin(), children.end(), child);` (a raw `Entity*`) in the same function without qualifying/renaming. Harmless today (the outer `it` isn't used after the inner declaration) but will warn under `-Wshadow`/`/W4` and is a maintenance trap if someone edits the function later expecting `it` to still refer to the `FlatMap` iterator. Rename the inner one (e.g. `childIt`).

**9. `Relations::GetChildren()`/`GetLinks()` comment claims "return direct reference" but actually copies.**
`Relations.hpp:87` / `113` comment "No filtering - return direct reference" above `return m_relationsGraph->GetChildren(m_rootEntity);` inside a function declared `auto GetChildren() const` (not `auto&`/`decltype(auto)`). Because the always-compiled early-return path (`Relations.hpp:82-83`/`108-109`, `if (!m_relationsGraph) return RelationshipGraph::ChildrenContainer{};`) forces the deduced `auto` return type to be a value type, this path actually **copies** the `SmallVector` on every call — safe (no dangling reference, unlike the `ForEach*` methods in Finding 1), but the comment is wrong and the copy is presumably not what was intended given the effort spent elsewhere avoiding allocations. Either fix the comment or restructure to actually return a reference where safe (careful: the `HAS_FILTERING` branch legitimately needs to return a local by value, so a bare reference return isn't simply available).

**10. `GetParentCount()` name is misleading.**
`RelationshipGraph.hpp:351` — `size_t GetParentCount() const { return m_children.Size(); }` returns the number of *distinct entities that have at least one child* (i.e., "parent count" in the sense of "how many parents exist"), not e.g. "how many parent-child links exist" (that's `GetParentChildCount()`, `RelationshipGraph.hpp:350`, returning `m_parents.Size()`). The two names read as near-synonyms for very different quantities; a reader has to check the implementation to be sure which is which.

**11. `Serialize`/`Deserialize` truncate `size_t` counts to `uint32_t` with no overflow guard.**
`RelationshipGraph.hpp:405,417,433` etc. cast `m_parents.Size()`/`children.size()`/`links.size()` to `uint32_t` unchecked. Given entity id space is bounded by `Entity::StorageType` (≤64-bit, and in practice far smaller ID ranges), this is not realistically reachable today, but it's a silent-truncation footgun if entity/relationship counts ever grow into the billions on a 64-bit configuration.

## Test coverage

Strong: `tests/Registry/RelationshipGraphTest.cpp` covers live-API self/cycle/invalid-entity rejection (2/3/4-level cycles), parent reassignment, entity destruction cleanup (parent/children/links), `Clear()`, large (1000-node) trees, highly-connected link graphs. `tests/Registry/RelationsTest.cpp` covers the full filtering matrix (`Not`/`Any`/`OneOf`/required) across `GetParent`/`GetChildren`/`GetLinks`/`ForEachDescendant`/`ForEachAncestor`, plus a 100-level deep chain for traversal correctness. `tests/Registry/RelationshipGraphSerializationTest.cpp` round-trips empty/single/multi-child/nested/star-link/mixed/large (625-node) graphs, including entity-version preservation. `tests/Comprehensive/ComplexRelationshipTest.cpp` adds very-deep (1500) and very-wide (1500) hierarchies, a diamond-reparent case, a link-only "multiple cycles" graph (links, not parent/child, so `IsAncestorOf` is never exercised there), a 1000-iteration random fuzz test with an explicit post-hoc "walk every ancestor chain and assert it terminates" check (`ComplexRelationshipTest.cpp:482-493` — good, targeted regression for the cycle-rejection contract), orphaning behavior, and a memory-cleanup loop over `DestroyEntities`.

Gaps directly behind the Critical findings above, all currently untested:
- No test destroys/reparents/unlinks an entity **from within** a `ForEachChild`/`ForEachLink`/`ForEachDescendant`/`ForEachAncestor` callback (Finding 1 & 2's trigger). The closest test (`RelationshipMemoryCleanup`) destroys entities via a separate batch call *after* iteration completes, not reentrantly.
- No test performs a nested/recursive `Relations<>` traversal call from within another traversal's callback (Finding 2's other trigger), nor exercises enough distinct cached roots (>~14) to force a `FlatMap` rehash of `m_descendantCaches`/`m_ancestorCaches` while a reference to an earlier entry is still live.
- No test calls `RelationshipGraph::Deserialize` on anything other than data this library itself just serialized — no malformed/adversarial payload (self-parent pair, cyclic pair, sentinel/invalid entity value, or `m_parents`/`m_children` cross-inconsistency) is ever fed in (Finding 4). `MultipleCircularReferences` in `ComplexRelationshipTest.cpp` builds cycles only via `AddLink` (which has no cycle concept), never via `SetParent`/`Deserialize`, so `IsAncestorOf`'s missing cycle guard (Finding 3) is never actually exercised against real cyclic parent data.
- No concurrency test exists for `RelationshipGraph` at all (no test links to `<thread>`/spawns threads against a shared graph), so Finding 5's data-race surface (including via `ParallelForEachDescendant`, which itself has no dedicated test file/case beyond compiling) is entirely unverified.
# Commands & Events (CommandBuffer / Delegate / Signal) — Review

## Overview

Scope: `include/Astra/Commands/Command.hpp`, `include/Astra/Commands/CommandBuffer.hpp`,
`include/Astra/Core/Delegate.hpp`, `include/Astra/Core/Signal.hpp`.

This subsystem has two independent halves that share the same reasoning tools (type erasure,
manual placement-new, SBO):

1. **CommandBuffer** — a raw byte-buffer encoding of deferred ECS mutations, with a
   `ParallelCommandBuffer` wrapper that gives each OS thread its own `CommandBuffer`.
2. **Delegate / MulticastDelegate / Signal** — a small-buffer-optimized type-erased callable
   (`Delegate`), a vector-of-delegates multicast wrapper (`MulticastDelegate`), and the ECS event
   dispatcher (`SignalManager`) built on top of it.

Verification performed beyond static reading: I compiled small probes with the repo's actual
toolchains (real `cl.exe` from the installed VS instance, and `clang++` targeting
`x86_64-pc-windows-msvc` / `x86_64-pc-linux-gnu` / 32-bit variants) to pin down two
compiler-dependent alignment facts that the source code's correctness depends on:

- `__STDCPP_DEFAULT_NEW_ALIGNMENT__` = 16 on 64-bit MSVC/GCC/Clang targets, but **8 on 32-bit
  targets** (confirmed via clang `-E` with `i686-pc-windows-msvc` / `i686-pc-linux-gnu`).
- `alignof(std::max_align_t)` = **8 on real MSVC x64** (confirmed by compiling and running a
  probe with the installed `cl.exe`), vs. 16 on Linux x86-64 (where `long double` is 16-byte
  aligned). This directly undermines a `Delegate` alignment guarantee — see Critical #1.

## Design assessment

The CommandBuffer's core idea — record commands into a flat byte buffer instead of
type-erased lambdas/`std::function` — is sound and the "dynamic re-derivation" trick in
`AddComponentPayload::GetDataPtr()` (recomputing alignment padding from the *actual* runtime
address of `this + 1` rather than trusting a value baked in at record time) is a genuinely good
design choice: it makes the payload self-correcting under buffer relocation (`std::vector`
growth) and buffer merging (`MergeFrom`), as long as one global invariant holds — every command
start is a multiple of `CommandByteBuffer::ALIGNMENT` (16) from the buffer's base address, and
the buffer's base address is itself 16-aligned. That invariant is documented in a comment but
never asserted (Important #1).

The `CommandBuffer::Execute()` contract ("not fully transactional; only pre-allocated entities
that haven't been processed yet are destroyed") is a reasonable design for a deferred command
system, but the implementation does not actually track "processed vs. not yet processed," so the
documented guarantee is false in practice (Critical #4).

`Delegate`'s SBO is a fairly standard "manager function pointer" pattern (à la small `std::any`)
and the large-functor `shared_ptr` fallback is correctly implemented (this repo evidently already
fixed a related "assignment over garbage control block" bug per git history — the current code
correctly *placement-news* the `shared_ptr` in both paths). What's missing is that the
small-vs-large branch selection only checks `sizeof` and `is_nothrow_move_constructible`; it does
not check alignment or copy-constructibility, and both omissions are independently exploitable
into real UB (Critical #1, #2).

`MulticastDelegate::Invoke()` is a plain range-based-for over `std::vector<Handler>` with no
reentrancy protection — the classic "modify container while iterating it" trap, directly reachable
from user Signal handlers (Critical #3).

## Strengths (file:line)

- `include/Astra/Commands/Command.hpp:47-56` — `CommandHeader` layout is pinned down with
  `static_assert`s on size and every member's `offsetof`, guarding against silent ABI drift across
  compilers/flags.
- `include/Astra/Commands/Command.hpp:117-132` (and the `AddComponentBatchPayload`/
  `SetResourcePayload` equivalents) — `GetDataPtr()` recomputes alignment padding from the live
  runtime address rather than trusting a record-time-computed offset, making the payload
  self-correcting across buffer relocation/merge.
- `include/Astra/Commands/CommandBuffer.hpp:153-163` and `:1104-1115` — the
  `CreateEntity`/`ParallelCommandBuffer::GetThreadBuffer` threading caveats (record-time
  allocation must happen on the registry-owning thread; fiber pinning requirement for the
  thread-local cache) are unusually honest and precise for this kind of API.
- `include/Astra/Commands/CommandBuffer.hpp:1220-1254` — `InitializeThreadBuffer()` correctly
  minimizes the locked critical section (only vector mutation is under `m_mutex`; the thread-local
  cache write happens after `lock.unlock()`), and correctly performs the "allocate index once per
  thread via `fetch_add`, cache thereafter" pattern without a lock-free race on `m_buffers`.
- `include/Astra/Core/Delegate.hpp:58-68` — the large-functor path's comment about avoiding
  "assignment over garbage control block" and its use of placement-new for the `shared_ptr` is
  correct and matches recent history (`f63d642`); `ManageLargeFunctor::Copy` copies the
  `shared_ptr` handle itself, so it stays correct even for a `DecayedFunc` that is not
  copy-constructible (see Critical #2 for why the *small*-path doesn't get this benefit).
- `include/Astra/Registry/Registry.hpp:311-313, 410-411, 555-558` — every `ComponentRemoved`
  emission I found honors the "emit before removal, pointer valid only for the handler" contract
  documented at `include/Astra/Core/Signal.hpp:107-116`; `tests/Registry/SignalLifetimeTest.cpp`
  actively regression-tests this ordering with a destructor-poisoning sentinel.
- `include/Astra/Commands/CommandBuffer.hpp:643-654` — `Execute()` at least asserts the buffer
  base is 16-aligned before trusting the invariant (though see Important #1 for why this
  assertion alone is insufficient in Release).

## Findings

### Critical

**C1. `Delegate`'s small-buffer storage is only 8-byte aligned on real MSVC x64, but the SBO
selection never checks the functor's alignment — placement-new of a 16-byte-aligned functor into
it is UB.**
`include/Astra/Core/Delegate.hpp:52` (selection condition), `:305` (storage declaration).

```cpp
alignas(std::max_align_t) mutable std::byte m_storage[SMALL_BUFFER_SIZE];   // line 305
...
if constexpr (sizeof(DecayedFunc) <= SMALL_BUFFER_SIZE &&
              std::is_nothrow_move_constructible_v<DecayedFunc>)            // line 52 — no alignof check
{
    new (m_storage) DecayedFunc(std::forward<Func>(func));
```

I compiled and ran a probe against the repo's installed MSVC (`cl /std:c++20`, VS "18" /
toolset 14.44/14.51, i.e. the VS2022+ family this project targets):
```
alignof(max_align_t)=8
STDCPP_DEFAULT_NEW_ALIGNMENT=16
```
So on MSVC x64, `alignas(std::max_align_t)` only buys 8-byte alignment for `m_storage`. Any
functor with `alignof > 8` and `sizeof <= 32` — e.g. a lambda capturing an `alignas(16)` SIMD-ish
value by copy (the project itself uses `alignas(16)` component types, see
`tests/Commands/CommandBufferAlignmentTest.cpp:8`) — takes the small path and gets
placement-new'd at an address the language does not guarantee is 16-aligned. This is UB per the
object model regardless of observed behavior, and in practice risks a fault if the compiler emits
aligned SSE loads/stores for the captured member, or silent corruption otherwise. On Linux x86-64
(GCC/Clang) `max_align_t` happens to be 16-aligned (because `long double` is), so this specific
failure is MSVC-only among the three supported compiler families — but MSVC 2022+ is explicitly a
first-class target.
*Fix*: bump storage alignment to a fixed value that actually covers what the small path claims to
support (e.g. `alignas(16)` or a configurable `SMALL_BUFFER_ALIGN`), and add
`alignof(DecayedFunc) <= alignof(m_storage)` to the `if constexpr` gate so over-aligned functors
correctly fall through to the heap/`shared_ptr` path (which is alignment-safe via C++17's
automatic aligned-`operator new` dispatch).

**C2. Copying a `Delegate` that holds a move-only-but-small functor silently corrupts the copy in
Release builds — `ASTRA_ASSERT` is a no-op there, so `m_invoker`/`m_manager` end up valid while
the storage is never constructed.**
`include/Astra/Core/Delegate.hpp:52-56` (small-path selection doesn't check copy-constructibility),
`:265-274` (`ManageSmallFunctor::Copy`), reachable via `include/Astra/Core/Delegate.hpp:326-335`
(`MulticastDelegate::Register(const DelegateType&)`).

```cpp
case ManagerOp::Copy:
    if constexpr (std::is_copy_constructible_v<Func>)
    {
        new (dst) Func(*reinterpret_cast<const Func*>(src));
    }
    else
    {
        ASTRA_ASSERT(false, "Functor is not copy constructible");   // no-op in Release (Base.hpp:51)
    }
    break;
```
Repro: a lambda that captures a `std::unique_ptr<T>` by value is move-constructible (so it
qualifies for the small path at line 52 — that check is `is_nothrow_move_constructible_v` only)
but **not** copy-constructible. Build a `Delegate` from it, then copy that `Delegate` — e.g. via
the public `MulticastDelegate::Register(const DelegateType& delegate)` overload, which does
`m_handlers.push_back({id, delegate})`, a genuine copy-construction of the stored `Delegate`. In a
Debug build the `ASTRA_ASSERT(false, ...)` fires and aborts — annoying but safe. In a **Release
build** (`ASTRA_ASSERT` compiles to `((void)0)`, `include/Astra/Core/Base.hpp:51`) the `Copy`
branch does nothing: `dst` (the new `Delegate`'s storage) is left uninitialized, yet the caller
(`Delegate`'s copy ctor, line 91-102) has already copied `m_invoker`/`m_manager` from the source as
if the copy succeeded. Invoking the new delegate calls `InvokeSmallFunctor<Closure>` on
uninitialized memory — reads garbage as a live closure object, plausibly dereferencing/destroying
a garbage "pointer" on the next use. This is exactly the class of bug the project's own
constraints call out as unacceptable ("caller/recoverable errors handled gracefully in ALL
configs"; `ASTRA_ASSERT` is supposed to be for internal invariants only) — copying a non-copyable
user functor through a public API is a caller-observable, statically-detectable condition, not an
internal invariant.
*Fix*: the large (`shared_ptr`) path is unconditionally copy-safe regardless of `DecayedFunc`'s own
copyability (copying a `shared_ptr` never touches the pointee). Add
`std::is_copy_constructible_v<DecayedFunc>` to the small-path `if constexpr` gate at line 52 so
move-only functors are routed to the `shared_ptr` path even when they'd otherwise fit inline.

**C3. `MulticastDelegate::Invoke()` iterates `m_handlers` with a plain range-based `for`; a handler
that registers/unregisters/clears the *same* signal from inside its own callback invalidates the
iteration — use-after-free / null-function-pointer call.**
`include/Astra/Core/Delegate.hpp:391-398` (void `Invoke`), `:400-412` (non-void `Invoke`);
triggerable from any `SignalManager::Emit` call site, e.g. `include/Astra/Core/Signal.hpp:201-210`.

```cpp
for (const auto& handler : m_handlers)      // __end captured once, before the loop body runs
{
    handler.delegate(std::forward<Args>(args)...);   // may call Register/Unregister/Clear on *this*
}
```
Repro: register two handlers on `Events::ComponentRemoved`; have the first handler call
`registry.GetSignalManager()->On<Events::ComponentRemoved>().Unregister(selfId)` (a natural
"handle once, then unsubscribe" pattern) or `.Register(...)`. `Unregister` does
`m_handlers.erase(it)` (`Delegate.hpp:364-374`), which destroys the last element in place and
shifts others — the range-for's cached `end()` iterator, computed before the loop started, is now
stale and one element short of the container's real size; the loop dereferences a `Handler` whose
`Delegate` has already run its destructor (`m_invoker == nullptr` post-`Reset()`), and
`Delegate::operator()` calls `m_invoker(m_storage, ...)` unconditionally once past the
`ASTRA_ASSERT` (also compiled out in Release) — i.e. a null/garbage function-pointer call. A
`Register` reentrant call is worse: `push_back` can reallocate the vector's backing storage,
invalidating every iterator/reference held by the in-progress loop, including the one currently
being dereferenced for `handler.delegate`.
*Fix*: don't iterate the live container directly — snapshot handler IDs/pointers before dispatch
(or dispatch over an index with a stable, re-read `size()` each iteration and tombstone-based
removal instead of `erase`), or explicitly disallow/defer mutation during dispatch (a
"dispatching" flag + deferred apply queue is the usual fix for this exact shape of bug).

**C4. `CommandBuffer::RollbackAllocatedEntities()` destroys entities via `EntityManager::Destroy`
only, even for entities whose `CreateEntity` command already executed successfully earlier in the
*same* `Execute()` call — orphaning a live archetype row and contradicting the documented
"not fully transactional" contract.**
Contract: `include/Astra/Commands/CommandBuffer.hpp:121-126` (esp. line 125: "Only pre-allocated
entities that haven't been processed yet are destroyed"). Implementation:
`:773-784` (`RollbackAllocatedEntities`), invoked from the failure branch of `Execute()` at
`:664-673`.

```cpp
void RollbackAllocatedEntities()
{
    if (m_registry)
    {
        auto& manager = m_registry->GetEntityManager();
        for (Entity e : m_allocatedEntities)     // contains EVERY entity CreateEntity()/CreateEntities()
            manager.Destroy(e);                  // ever recorded, processed or not
    }
    m_allocatedEntities.clear();
}
```
`m_allocatedEntities` is appended to at *record* time (`CreateEntity()` at `:174`,
`CreateEntities()` at `:225-228`) and is never pruned as commands successfully execute — there is
no tracking of "committed vs. still-pending" anywhere in the class. Minimal repro:
```cpp
CommandBuffer cmd(&registry);
Entity e = cmd.CreateEntity();                       // will be committed to an archetype at Execute()
cmd.RemoveComponent<Position>(Entity::Invalid());     // deliberately fails at Execute time
auto result = cmd.Execute();                          // Err(ExecutionFailed)
```
At `Execute()` time: `ExecuteCreateEntity` runs first and calls
`m_registry->GetArchetypeManager()->AddEntity(e)` — `e` is now a live row in an archetype. The next
command (`RemoveComponent` on `Entity::Invalid()`) fails
(`ExecuteRemoveComponent` returns `false`, `CommandBuffer.hpp:913-920`), so `Execute()` takes the
failure branch and calls `RollbackAllocatedEntities()`, which calls
`m_registry->GetEntityManager().Destroy(e)` directly — **bypassing**
`Registry::DestroyEntity()`, which is the only code path that also calls
`m_archetypeManager->RemoveEntity(entity)` and `m_relationshipGraph->OnEntityDestroyed(entity)`
(`include/Astra/Registry/Registry.hpp:214-224`). The result: `e`'s entity ID/generation is freed
in `EntityManager` (so `registry.IsValid(e)` correctly reports `false`), but the archetype's
`m_entityMap` and chunk storage still hold a live row for `e` — a leaked, unreachable-by-ID "ghost"
entity that a `View`/`ForEach` iteration over that archetype will still enumerate (yielding an
`Entity` value that fails `IsValid()`), and any relationship-graph edges on `e` are never torn
down. If the freed index is later recycled by `EntityManager::Create()` for a brand-new entity,
that new entity gets a fresh (different-generation) key, so it won't directly collide with the
ghost row in the entity map, but the ghost row itself is never reclaimed for the lifetime of the
registry. This is squarely the "commands that capture entity/component data at record time — any
dangling by Execute time" class of bug the review brief calls out, and it is trivially reachable
by any two-command buffer where the second command fails for an unrelated reason after the first
successfully creates an entity.
*Fix*: track a "high-water mark" of committed `CreateEntity`/`CreateEntities` commands (e.g. an
index into `m_allocatedEntities` advanced only as those specific commands execute successfully),
and route only the *not-yet-committed* tail through `EntityManager::Destroy`; route anything
already committed through `Registry::DestroyEntity()` (or an equivalent full teardown) instead —
or, simpler, stop rolling back already-committed entities at all and clearly document that
`Execute()` failure leaves committed entities exactly as `RollbackAllocatedEntities()` documents
they should be treated (only *unprocessed* ones destroyed), then actually implement that.

### Important

**I1. The whole alignment scheme (writer's command-relative offset ≡ reader's absolute-address
alignment) depends on `std::vector<std::byte>`'s allocation being ≥16-byte aligned, which is
asserted only in a comment and holds only on 64-bit targets.**
`include/Astra/Commands/CommandBuffer.hpp:53-58`.
```cpp
// Every command start is aligned to 16. std::vector<std::byte>'s
// allocation is aligned to __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 on all
// supported targets), so for payload alignment A <= 16 the reader's
// absolute-address alignment and the writer's command-relative offset
// computation are guaranteed to agree.
static constexpr size_t ALIGNMENT = 16;
```
I verified via `clang++ -E` targeting `i686-pc-windows-msvc` and `i686-pc-linux-gnu` that
`__STDCPP_DEFAULT_NEW_ALIGNMENT__` is **8**, not 16, on 32-bit targets (it is 16 on the 64-bit
equivalents). The comment's "16 on all supported targets" claim is true only if the project is
implicitly 64-bit-only — nothing in the stated project constraints (`MSVC 2022+/GCC 11+/Clang
13+`) says so explicitly. If this header is ever compiled for a 32-bit target, every
`AddComponent`/`AddComponentBatch`/`SetResource` payload whose component type has `alignof == 16`
(the maximum the API allows, and exactly what
`tests/Commands/CommandBufferAlignmentTest.cpp` exercises) would silently compute the wrong
data offset at the writer (`CommandBuffer.hpp:290`, `:367`, `:560`, all relative-offset
`AlignUp` calls that assume the command start is itself 16-aligned) versus what the *reader*
(`GetDataPtr()`) independently derives from the (now differently-offset) runtime address — the
two would still agree with each other in this specific case since both derive from the same
formula, but the **actual placement-new address** the writer uses for `dataPtr` is the
*writer's* (unverified) `dataOffset`, not `GetDataPtr()`'s recomputed value, so if the command
start is not itself a multiple of `dataAlignment`, the writer's construction site and reader's
retrieval site genuinely diverge — see Design assessment for the derivation. This is silent
(no compile error, no runtime assert failure on the base-alignment check at `Execute():652-653`
since that check only verifies 16-byte alignment of the *whole buffer*, which is still satisfied
at 8-byte granularity in the 32-bit case for many allocations by chance, masking the bug in
smoke testing).
*Fix*: `static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= CommandByteBuffer::ALIGNMENT, "...")`
next to the `ALIGNMENT` constant, so an unsupported target fails to compile instead of silently
misencoding.

**I2. `CreateEntity`/`CreateEntities` mutate shared `EntityManager` state at record time and are
documented as unsafe from `ParallelCommandBuffer` worker threads, but nothing enforces the
contract — not even a debug-only check.**
`include/Astra/Commands/CommandBuffer.hpp:153-163` (doc), `:164-188` (impl, no thread check),
`:1104-1115` (`ParallelCommandBuffer::GetThreadBuffer` doc reiterating the same caveat).
The entire *point* of `ParallelCommandBuffer` is "here's your own private buffer, do what you
want with it" — a caller reasonably assumes any `CommandBuffer` method is safe to call on their
own thread's buffer. `CreateEntity()` breaks that assumption silently: it calls
`m_registry->GetEntityManager().Create()` directly, with no synchronization, so two worker
threads each calling `GetThreadBuffer().CreateEntity()` concurrently race on the shared
`EntityManager` allocator (free-list / generation table) — genuine UB, and exactly the kind of
mistake the API's own ergonomics invite. There is no `ASTRA_ASSERT` comparing the calling
thread's ID against a registry-owner thread ID, even as a debug-only tripwire.
*Fix*: at minimum, add a debug-only thread-id check (store the constructing/owning thread's ID in
`Registry` or `CommandBuffer`, assert against it in `CreateEntity`/`CreateEntities`) so
violations fail loudly in Debug instead of racing silently in Release.

**I3. `AddComponentPayload`/`AddComponentBatchPayload`/`SetResourcePayload` encode `dataSize` as
`uint16_t` with no compile-time cap on `sizeof(T)`; components/resources over 64KB silently
truncate the recorded size and fail at `Execute()` time (compounding with C4's rollback bug).**
`include/Astra/Commands/Command.hpp:110-111` (`AddComponentPayload::dataSize`), `:151-152`
(`AddComponentBatchPayload`), `:275-276` (`SetResourcePayload`);
write sites `include/Astra/Commands/CommandBuffer.hpp:303` (`static_cast<uint16_t>(dataSize)`),
similarly at `:379`, `:572`.
The buffer allocation itself uses the correct, untruncated `constexpr size_t dataSize`, so the
bytes are written and sized correctly in the buffer — only the *metadata* field that's later read
back at `Execute()` time (`cmd->dataSize`, passed to `Registry::AddComponentByID`/`SetResourceByID`
for size validation) is silently wrong for `sizeof(T) > 65535`. `Registry::AddComponentByID`
correctly *rejects* the mismatched size rather than corrupting memory (`ArchetypeManager.hpp:392-394`),
so the practical effect is a spurious `ExecutionError::ExecutionFailed` for any oversized
component/resource type, not memory corruption — but it fails with a confusing generic error
rather than a clear compile-time diagnostic, and (per C4) can drag an unrelated,
already-successfully-created entity down with it.
*Fix*: `static_assert(sizeof(DecayedT) <= std::numeric_limits<uint16_t>::max(), ...)` alongside
the existing alignment `static_assert`s at `CommandBuffer.hpp:277`, `:353`, `:548`.

### Minor

**M1. `Delegate`'s large-functor path performs two heap allocations instead of one.**
`include/Astra/Core/Delegate.hpp:65`: `new (m_storage) std::shared_ptr<DecayedFunc>(new DecayedFunc(...))`
allocates the object and the `shared_ptr` control block separately. `std::allocate_shared`/an
equivalent single-allocation helper would halve the allocation count and improve locality for
every "large" (>32 byte or throwing-move) handler — likely to be common for Signal handlers that
capture several fields.

**M2. `Delegate::operator==` always returns `false` for any two functor-based delegates, even
exact duplicates.** `include/Astra/Core/Delegate.hpp:193-207`. Equality is only implemented for
the raw-function-pointer case (`m_manager == nullptr` on both sides); any delegate built from
`Delegate(Func&& func)` (small or large path) has `m_manager != nullptr` and therefore always
compares unequal to everything, including a copy of itself. Not currently exploited internally
(`MulticastDelegate::Unregister` keys off `HandlerID`, not equality), but a caller relying on
`Delegate::operator==` for deduplication will silently get "always different" — worth a doc note
or a real implementation (e.g. compare `m_invoker` + a type-erased "same underlying object"
check) if equality is meant to be meaningful for functors.

**M3. `CommandBuffer::MergeFrom`/`ParallelCommandBuffer::MergeInto` are not self-merge-safe.**
`include/Astra/Commands/CommandBuffer.hpp:721-744`, `:1158-1167`. `buf.MergeFrom(std::move(buf))`
computes `dst` via `m_buffer.Allocate(otherSize)` (mutating/possibly reallocating `m_buffer`),
then re-reads `other.m_buffer.Data()` (== the same, now-relocated buffer) and memcpy's the
original bytes into the newly appended region — that part is technically well-defined (adjacent,
non-overlapping ranges) — but the very next lines (`other.m_buffer.Clear(); ...`) then clear the
*same* buffer object, discarding the just-duplicated data. `MergeInto(CommandBuffer& target)`
doesn't guard against `target` aliasing one of `m_buffers[i]` (e.g. a caller passing their own
`ParallelCommandBuffer::GetThreadBuffer()` result back into `MergeInto` on the same instance),
which would hit exactly this path. Low likelihood, easy defensive fix
(`ASTRA_ASSERT(this != &other, ...)` in `MergeFrom`).

**M4. Batch command executors swallow per-entity failures; single-entity executors don't.**
`include/Astra/Commands/CommandBuffer.hpp:922-937` (`ExecuteAddComponentBatch`) and `:939-953`
(`ExecuteRemoveComponentBatch`) always `return true` regardless of how many
`AddComponentByID`/`RemoveComponentByID` calls inside the loop actually failed, while
`ExecuteAddComponent`/`ExecuteRemoveComponent` (single-entity) propagate `false` on failure and
thereby trigger the whole-buffer failure/rollback path. This is an inconsistency in the error
model between batched and non-batched variants of the same operation — a caller has no way to
detect that, say, 2 of 50 entities in an `AddComponents<T>` batch silently failed to receive the
component.

**M5. `ParallelCommandBuffer`'s thread-local buffer cache is a single global slot shared across
*all* `ParallelCommandBuffer` instances on a thread**, causing buffer-slot fragmentation (not
unsafe, just wasteful) when a thread interleaves usage of two different instances.
`include/Astra/Commands/CommandBuffer.hpp:1262-1269` (`t_cache`), `:1116-1126`
(`GetThreadBuffer`). `t_cache.context == this` correctly discriminates between instances, but
because there's only one cached `(context, buffer, index)` triple per thread, switching from
instance A to instance B and back to A on the same thread causes a second "slow path" run for A,
which allocates a *new* index via `m_nextIndex.fetch_add(1)` — the thread ends up with two
separate buffer slots in A's `m_buffers` instead of reusing the first one. No data is lost
(`Execute()`/`MergeInto()`/`GetCommandCount()` iterate the whole vector), but it's a surprising,
easily-triggered inefficiency worth a doc note.

## Test coverage

- `tests/Commands/CommandBufferTest.cpp` covers the happy path thoroughly (entity
  create/destroy, single & batch component add/remove, relationships, resources,
  `ParallelCommandBuffer` basic single-thread-buffer-reuse) but **never exercises `Execute()`'s
  failure path** — no test triggers `ExecutionError::ExecutionFailed`, so C4
  (`RollbackAllocatedEntities` orphaning committed entities) has zero coverage. A test as simple
  as "`CreateEntity()` + a deliberately-failing second command in the same buffer, then assert
  the created entity either doesn't exist in any archetype view or that `Execute()`'s documented
  contract holds" would have caught this immediately.
- `tests/Commands/CommandBufferAlignmentTest.cpp` has exactly one regression test
  (`Align16SurvivesParityShiftingPredecessor`) for the "predecessor command shifts the following
  command's start off a 16-byte boundary" class of bug — good, and its comment indicates this
  fixes a previously-real bug. It does **not** cover `AddComponentBatch`/`SetResource` alignment,
  `MergeFrom` alignment preservation, or (naturally, since it can't be caught by a runtime test on
  this machine) the 32-bit-target alignment assumption in I1.
- `tests/Core/DelegateLargeFunctorTest.cpp` covers construct/invoke/copy/move/destroy for one
  >32-byte, *copyable* functor and checks for leaks via a liveness counter — solid for that one
  shape. It has **no coverage** for: an over-aligned small functor (C1), a move-only small functor
  being copied (C2), or SBO alignment at all (no `alignas` types appear in the test file).
- `tests/Registry/SignalLifetimeTest.cpp` has exactly one test
  (`ComponentRemovedSeesLiveValue`) verifying emit-before-removal ordering with a single handler —
  a good, precise regression test for that specific ordering bug, but there is **no test for
  reentrancy** (a handler registering/unregistering during dispatch, C3), and no test with more
  than one handler registered at all.
- No test anywhere exercises `ParallelCommandBuffer::MergeInto`/`MergeFrom` self-aliasing (M3), the
  `CreateEntity`-off-worker-thread contract violation (I2 — inherently hard to test
  deterministically, but a thread-sanitizer-driven stress test would be valuable), or an
  oversized (>64KB) component/resource type (I3).
# Reflection — Review

## Overview

Scope: `include/Astra/Reflection/{Reflection,TypeMeta,MetaRegistry,FieldInfo,FieldVisitor,Attribute,EnumInfo,ContainerTraits,JsonSchema,Macros}.hpp`, plus `tests/Reflection/{ReflectionTest,FieldVisitorTest}.cpp`. `include/Astra/Core/TypeContext.hpp` and `TypeID.hpp` are out of primary scope but are read directly by `MetaRegistry.hpp`/`TypeID`-based type-safety and are cited only where they directly explain in-scope behavior (the static-registration queue, and the hash-based type check used throughout `FieldInfo`).

The subsystem is a classic C++ compile-time-registration / runtime-introspection reflection layer: macros build a `TypeMeta` (field list, lifecycle functions, enum info, attributes) via `TypeMetaBuilder<T>`, register it into a process/`TypeContext`-scoped `MetaRegistry`, and expose type-erased field access (`FieldInfo::Get/Set/GetPtr/GetAny/SetAny`) consumed either directly (`IFieldVisitor`) or through checked `TypeMeta::GetFieldValue/SetFieldValue` wrappers.

## Design assessment

The static-registration path is genuinely well thought out: `ASTRA_REFLECT_TYPE`/`ASTRA_REFLECT_ENUM` static objects never touch `MetaRegistry`/`TypeContext` at static-init time — they only build a `TypeMeta` and push it onto a trivially-constructible pending queue (`Detail::PendingMetaQueue()`, a Meyer's singleton of PODs/std::function), deferring the actual `Register()` call to first explicit use (`MetaRegistry::Instance()`) or host install (`SetTypeContext`). This sidesteps the classic static-initialization-order fiasco that naive "register-yourself-into-a-global-map" reflection systems fall into. Ownership of `TypeMeta` across its several `std::move`s (builder → `shared_ptr` → `Register(TypeMeta&&)` → `unique_ptr` in the registry) is also handled correctly: `FieldInfo::attributes` holds raw `Attribute*` into `TypeMeta::fieldAttributeStorage`, and moving the containing vectors never relocates the pointed-to heap objects, so those pointers stay valid.

Where the subsystem is weaker is at the actual field-value boundary. There are two parallel access APIs: a *checked* one (`TypeMeta::GetFieldValue<T>/SetFieldValue<T>`, which compares `field->typeHash` against `TypeID<T>::Hash()` with a real `if`), and an *unchecked* one (`FieldInfo::Get<T>/Set<T>/GetPtr<T>`, gated only by `ASTRA_ASSERT`, which is compiled to `((void)0)` outside `ASTRA_BUILD_DEBUG`). That two-tier design (checked wrapper + trusted fast path) is a legitimate pattern in isolation, but `FieldVisitor.hpp`'s own docstring explicitly tells consumers implementing serializers to use the *unchecked* tier directly (`field.Get<T>/Set<T>`, `field.GetPtr<T>(instance)`), with no caveat that this is a "you must pre-validate the hash yourself" contract. Combined with the project's stated policy that `ASTRA_ASSERT` is for *internal invariants only* (not user-input validation) and that CI only ever builds/tests Release, the unchecked tier ships with effectively zero protection in the configurations that matter. See Critical findings below.

`ContainerTraits.hpp` is a complete, nicely factored trait system, but it is dead code from the Reflection subsystem's point of view: nothing in `FieldInfo.hpp` ever consults it, so `FieldInfo::isStdArray`/`isVector` (declared for exactly this purpose) are permanently `false`, and `JsonSchema.hpp`'s array-type detection silently never fires for `std::vector`/`std::array` fields.

## Strengths (file:line)

- `include/Astra/Reflection/MetaRegistry.hpp:359-375` + `include/Astra/Core/TypeContext.hpp:27-56` — deferred pending-registration queue avoids static-initialization-order fiasco entirely; registrars never dereference `MetaRegistry`/`TypeContext` during static init.
- `include/Astra/Reflection/TypeMeta.hpp:472-503` — `TypeMetaBuilder`'s type-level lifecycle functions (`defaultConstruct`/`copyConstruct`/`moveConstruct`/`copyAssign`/`moveAssign`) are correctly `if constexpr`-gated on the matching type trait, so `TypeMeta::Construct/CopyConstruct/...` (`TypeMeta.hpp:297-381`) never risk invoking an empty `std::function`. Contrast with the field-level bug in Important-2 below, which shows this pattern was known but not applied one layer down.
- `include/Astra/Reflection/TypeMeta.hpp:170-199` — `GetFieldValue<T>`/`SetFieldValue<T>` perform a real, always-on `typeHash` comparison before dispatch, correctly following the project's Result/bool error-handling policy.
- `include/Astra/Reflection/FieldInfo.hpp:145-152` + `370-383` — `SetAny` uses the pointer overload of `std::any_cast<DecayedType>(&value)`, which is exception-free and gives genuine, independent type-safety (not reliant on the 64-bit hash scheme at all).
- `include/Astra/Reflection/FieldInfo.hpp:350-383` — field getter/setter/getterAny/setterAny lambdas capture nothing (the member pointer is a compile-time non-type template parameter), so the four `std::function`s per field are realistically small-buffer-optimizable, avoiding a heap allocation per field per accessor.
- `include/Astra/Reflection/Attribute.hpp:15-61` — attribute type identification uses an explicit virtual `GetTypeHash()` instead of `dynamic_cast`/RTTI, which is friendly to `-fno-rtti`/`/GR-` builds (for this piece specifically — see Important-6 for the counter-example elsewhere in the subsystem).

## Findings

### Critical

**C1. `FieldInfo::Get<T>`/`Set<T>`/`GetPtr<T>` perform zero type checking outside debug builds — a wrong `T` is a blind, differently-sized memory write.**
`include/Astra/Reflection/FieldInfo.hpp:72-123`. The only guard is `ASTRA_ASSERT(TypeID<T>::Hash() == typeHash, ...)`, which `include/Astra/Core/Base.hpp:47-52` compiles to `((void)0)` unless `ASTRA_BUILD_DEBUG` is defined; premake5.lua only defines that for the `Debug` config (`Release`/`Dist` define `ASTRA_BUILD_RELEASE`/`ASTRA_BUILD_DIST` + `NDEBUG` instead, lines 111/119/127). Concretely: a `Position` component reflects `float x` (`MakeFieldInfo` captures `DecayedType = float` in its getter closure). In a Release build, `field->Get<double>(&pos)` compiles fine (nothing catches the mismatch), then at runtime: `T result{};` allocates a 4-or-8-byte-mismatched local, and the getter lambda does `*static_cast<float*>(outValue) = obj->*FieldPtr;` — wait, the *closure* was built for `float`, so it writes exactly `sizeof(float)` through `outValue`; the danger direction is the other way: reflect a `double` field and call `Get<float>()` — `outValue` points at a 4-byte stack slot but the getter lambda (closed over `DecayedType = double`) executes `*static_cast<double*>(outValue) = obj->*FieldPtr;`, an 8-byte write into a 4-byte stack local — a stack buffer overflow, 100% silent, on the very first mismatched call. `Set<T>`/`GetPtr<T>` have the same defect in the opposite direction (out-of-bounds read from the caller's storage, or a bogus differently-typed pointer handed back for direct dereference). This is exactly the scenario the review brief calls out ("type confusion → UB"), it is reachable through the officially documented `IFieldVisitor` usage pattern (`FieldVisitor.hpp:19`), and per the project's own stated policy `ASTRA_ASSERT` is for internal invariants only — using it as the sole gate on an externally-supplied template type parameter contradicts that policy. The guarded check is a single `uint64_t` compare, negligible next to the `std::function` call it wraps, so there is no performance rationale for the debug-only gating.
Fix: make the hash comparison an always-on branch (independent of `ASTRA_BUILD_DEBUG`); on mismatch, fail closed (e.g., `Get<T>` returns `T{}`/`std::nullopt` via a `TryGet<T>`, `Set<T>`/`GetPtr<T>` no-op and report via a `bool`/`Result` return), keeping the current assert only as an additional loud debug diagnostic.

**C2. `Set<T>` on a genuinely `const`-qualified reflected field invokes an empty `std::function` in Release — with exceptions off, this aborts the process.**
`include/Astra/Reflection/FieldInfo.hpp:89-97` calls `setter(instance, &value)` unconditionally after only `ASTRA_ASSERT`-checking `setter` and `!isConst` — both no-ops in Release. `MakeFieldInfo` (`FieldInfo.hpp:356-362`) only assigns `info.setter` `if constexpr (!std::is_const_v<FieldType>)`; for a field declared `const int id;`, `setter` is left default-constructed (empty). Invoking an empty `std::function::operator()` is specified to throw `std::bad_function_call` — but `premake5.lua` builds Astra with `exceptionhandling "off"` in every configuration (lines 109/117/125/220/228/244), and the project's own constraints state "NO throw/try". With exceptions disabled, the standard library's internal throw site typically routes to `std::terminate()`, i.e. an unconditional abort, reachable via the documented direct-field-access pattern, with none of the graceful `Result<T,E>`/bool/nullptr handling the project otherwise commits to. Unlike C1, pre-checking `typeHash` does not help here — the type is correct, the field is simply const.
Fix: check `isConst` (or `bool(setter)`) as an always-on branch before invoking `setter`, and give `Set` a way to report failure (return `bool`, or route through `SetAny`'s existing `isConst` check at `FieldInfo.hpp:145-152`, which is already correct and unconditional).

**C3. `GetPtr<T>` never checks `isConst` at all — not even in debug builds — letting a caller obtain and write through a pointer to a field the source type declared `const`.**
`include/Astra/Reflection/FieldInfo.hpp:105-123`. Both overloads only assert the type-hash match; there is no analog of `Set`'s (admittedly also-broken) `!isConst` assert. `IFieldVisitor.hpp:19` explicitly recommends `field.GetPtr<T>(instance)` as one of three sanctioned ways to read *or write* a field. A visitor implementation that writes through `GetPtr<T>` for every non-hidden serializable field (a very natural implementation, especially since nothing in `FieldInfo` surfaces "you must check `isConst`/`IsReadOnly()` before writing via `GetPtr`") will silently write through a pointer to a `const`-qualified subobject for any reflected read-only/id-style field — modifying a `const` object is undefined behavior in the C++ object model regardless of build configuration; optimizers are entitled to assume such values never change.
Fix: add the same const-guard `Set` should have, and/or split the API into `GetPtr<T>`/`GetMutablePtr<T>` where the latter is only enabled (via `if constexpr`/SFINAE or a runtime check) for non-const fields.

### Important

**I1. `ContainerTraits.hpp` is never wired into `FieldInfo` construction — `isStdArray`/`isVector` are permanently `false`, producing wrong JSON-schema types for container fields.**
`include/Astra/Reflection/FieldInfo.hpp:346-347` hardcodes `info.isStdArray = false;` / `info.isVector = false;` with the comment "Will be set by ContainerTraits" — nothing in `MakeFieldInfo`, `TypeMetaBuilder::Field`, or anywhere else ever consults `ContainerTraits<T>` to flip these. `JsonSchema.hpp:269` and `:343` gate `"type": "array"` / items-schema generation on `field.isArray || field.isVector || field.isStdArray`; since `isVector`/`isStdArray` never become `true` (and `isArray`, the C-array trait, is unreachable in practice — see I2), a reflected `std::vector<int> items;` or `std::array<float,3> axis;` field is *always* classified with the fallback `"type": "object"` and gets no `items` schema at all — a concrete JSON-schema type mismatch for what is presumably the most common non-scalar field type in a component.
Fix: in `MakeFieldInfo`, set `info.isVector = ContainerTraits<DecayedType>::IsSequence && /* is std::vector specifically, or add a dedicated IsVector trait */;` and `info.isStdArray = ContainerTraits<DecayedType>::HasFixedSize;` (or similar), actually using the already-built `ContainerTraits.hpp`.

**I2. `MakeFieldInfo`'s generated getter/setter/getterAny/setterAny are not `if constexpr`-gated on copy-assignability, so two plausible field categories fail to compile.**
`include/Astra/Reflection/FieldInfo.hpp:350-383`. (a) *C-style arrays*: `decltype(Type::arr)` for `int arr[4];` is `int[4]`; `DecayedType = std::decay_t<int[4]> = int*` (array-to-pointer decay). The generated setter (`FieldInfo.hpp:358-361`) does `obj->*FieldPtr = *static_cast<const DecayedType*>(inValue);`, i.e. `arrayLvalue = somePointer;` — arrays are not assignable in C++, so `ASTRA_REFLECT_FIELD(Type, arr)` fails to compile for any raw array member, despite `isArray` being tracked as a first-class trait (`FieldInfo.hpp:42`) and consumed by `JsonSchema.hpp:269,343,282`. (b) *Non-copy-assignable types* (`std::unique_ptr<T>`, `std::atomic<T>`, any move-only handle): the getter (`FieldInfo.hpp:350-353`) does `*static_cast<DecayedType*>(outValue) = obj->*FieldPtr;`, a *copy* assignment; `getterAny` (`FieldInfo.hpp:365-368`) constructs `std::any(obj->*FieldPtr)`, which requires `DecayedType` be copy-constructible. Neither is gated by `if constexpr`, unlike `TypeMetaBuilder`'s own type-level lifecycle functions one file over (`TypeMeta.hpp:472-499`), which correctly check `is_copy_constructible_v<T>`/`is_move_constructible_v<T>` before generating those closures. Reflecting any component field of a common move-only type produces a wall of `unique_ptr` deleted-function errors with nothing pointing at the actual cause (the macro).
Fix: either `static_assert` inside `MakeFieldInfo` with an actionable message when `FieldType` is an array or non-copy-assignable, or gate the affected members behind `if constexpr` and fall back to a smaller feature set (e.g. `GetPtr`-only access) for those field types.

**I3. `ASTRA_REFLECT_TYPE`/`ASTRA_REFLECT_ENUM` fail to compile for namespace-qualified type names, with a confusing, unrelated-looking error.**
`include/Astra/Reflection/Macros.hpp:67-70` (and `:121-124` for the enum variant) generate the static variable's name via `ASTRA_UNIQUE_NAME(_astra_reflect_##Type##_)`. When `Type` is a multi-token sequence (e.g. `MyNamespace::Foo`), `##` only pastes at the *boundary* tokens of the substituted argument: `_astra_reflect_` pastes with the first token (`MyNamespace`) and the trailing `_` (later concatenated with `__LINE__`) pastes with the last token (`Foo`), while the middle `::` survives untouched. The macro literally expands to a declarator like `_astra_reflect_MyNamespace::Foo_68`, i.e. an attempt to declare a static member of a non-existent class/namespace `_astra_reflect_MyNamespace` — a hard, "undeclared identifier" compile error nowhere near the real cause. (Multi-parameter template type arguments, e.g. `Foo<int,int>`, hit a different, more familiar break: the un-parenthesized top-level comma splits the macro call into "too many arguments" — the codebase already demonstrates awareness of this exact pitfall for `ContainerTraits` types in `tests/Reflection/ReflectionTest.cpp:519,532` ("Use typedef to avoid comma-in-macro issues"), but that lesson isn't documented for `ASTRA_REFLECT_TYPE`.) The trivial workaround — invoke the macro *inside* the namespace block with the unqualified name — works today (no `::` token appears) but is undocumented, and ECS components declared inside a project namespace is close to universal practice, so this is a real first-contact trap. No test exercises a namespaced type.
Fix: derive the unique suffix purely from `__LINE__`/`__COUNTER__` without folding the type name into the identifier (e.g. `ASTRA_CONCAT(_astra_reflect_registrar_, __COUNTER__)`), and document the namespace/template-argument caveat either way.

**I4. `MetaRegistry::Instance()` has a narrow cross-thread TOCTOU race during pending-registration drain.**
`include/Astra/Core/TypeContext.hpp:146-168` (`DrainPendingMeta`) swaps the entire pending queue out under `PendingMetaMutex()` and then processes the swapped-out batch *without* holding the lock. If thread A has just swapped out a non-empty batch and started calling `ctx.Meta().Register(...)` for each entry, a concurrent thread B calling `MetaRegistry::Instance()` (`MetaRegistry.hpp:32-37`) re-enters `DrainPendingMeta`, observes the now-*empty* queue (A already claimed it), and returns immediately — `Instance()` hands B a valid `MetaRegistry&` before A has finished registering everything in its batch. If B immediately calls `Get<T>()` for a type that's still mid-registration in A's batch, it gets a false-negative "not registered" `nullptr`, even though registration was already "in flight" by the time B's `Instance()` call returned. The window is narrow (requires concurrent first-touch across threads while static registrations are still pending, and self-heals on the next call), but it's real and undocumented, and the review brief specifically asks about registration thread-safety.
Fix: document the caveat (recommend a single-threaded "touch reflection once at startup" barrier before any multi-threaded use), or have `DrainPendingMeta` track an in-flight count and have late-arriving callers wait for in-flight batches to finish rather than just observing an empty queue.

**I5. `Attribute`-derived types store raw `std::string_view` fields with no defensive copy; non-literal input dangles for the process lifetime.**
`include/Astra/Reflection/Attribute.hpp` — `DisplayName`, `Tooltip`, `Category`, `FilePath`, `AliasName`, `Deprecated` all store `std::string_view` constructed directly from the constructor argument. `ASTRA_REFLECT_ATTR(Tooltip, "text")` is safe (string literal, static storage), but nothing stops (or warns against) `ASTRA_REFLECT_ATTR(Tooltip, someLocalString.c_str())`/a runtime `std::string` temporary; attributes are heap-allocated once at static-registration time and live for the rest of the process (owned by `TypeMeta::fieldAttributeStorage`), so a dangling view here is a permanent use-after-free on every subsequent `GetTooltip()`/`GetDisplayName()` call. Not currently triggered by any test or by Astra's own examples (which all use literals), but wholly unguarded.
Fix: document the static/program-lifetime requirement prominently in `Attribute.hpp`, or have the string-bearing attributes own a `std::string` instead of a view (small, one-time cost, paid only at registration).

**I6. Reflection's `std::any`-based `GetAny`/`SetAny` requires RTTI, but the project's own "ship" preset builds with RTTI off.**
`std::any`/`std::any_cast` (used throughout `FieldInfo.hpp:130-152,365-383` and required by any `IFieldVisitor` consumer that follows the documented `GetAny`/`SetAny` pattern) generally depend on `typeid`/RTTI machinery in mainstream standard library implementations. `premake5.lua:220-245` configures the `AstraBenchmark` project's `Debug`/`Release`/`Dist` builds with `rtti "off"` (only the `AstraTest` project keeps `rtti "on"`, explicitly "because GoogleTest requires RTTI" — an incidental reason unrelated to Reflection's own needs). Nothing in `Reflection.hpp`/`FieldInfo.hpp` documents an RTTI requirement, and there is no `static_assert`/`#error` to catch a consumer who copies the Dist-style `rtti off` preset (a very plausible thing to do for a shipping game binary) while also using `<Astra/Reflection/...>`. Currently latent — no benchmark exercises Reflection today — but a real trap for the next person who wires reflection into a size/perf-optimized build.
Fix: document the RTTI dependency of `GetAny`/`SetAny` next to `IFieldVisitor`'s docstring, and/or add a `static_assert(__cpp_rtti, ...)`-style guard where feasible.

### Minor

- **`FieldInfo.hpp:37,340`** — `isReference` is tracked but practically unreachable: taking `&Type::FieldName` for a reference-typed member (`int& ref;`) is itself ill-formed in standard C++ ("cannot take the address of a reference member"), so the macro fails before `MakeFieldInfo` even runs. Dead trait, no test.
- **`TypeMeta.hpp:530-541`** (`TypeMetaBuilder::Attr`) — silently no-ops if called before any field has been registered (`m_meta.fields.empty()`); a misordered `ASTRA_REFLECT_ATTR`/`ASTRA_REFLECT_FIELD` pair loses the attribute with zero diagnostic.
- **`TypeMeta.hpp:513-521`** (`TypeMetaBuilder::Field`) — duplicate field registration (same name reflected twice, e.g. copy-paste error) silently overwrites the `fieldsByHash` mapping to the *later* entry while `fields`/`ForEachField` still visits *both* — `GetField("x")` and `ForEachField` can disagree about how many "x" fields exist.
- **`EnumInfo.hpp`** (`EnumInfoBuilder::Value`) — no duplicate-name/duplicate-value guard; `ToString`/`FromString`/`GetValue` all resolve to the first match, silently ignoring later reflected values with the same name or numeric value.
- **`JsonSchema.hpp:84-107`** — the `"required"` array includes every non-hidden field unconditionally; the accompanying comment ("all non-optional fields") implies real optionality tracking that doesn't exist. Compounding this, `ContainerTraits.hpp` has no `std::optional<T>` specialization at all, so an `std::optional<T>` field is both marked `"required": true` *and* mistyped as `"object"` by `GetJsonType` (`JsonSchema.hpp:294-350`, falls through every case to the default).
- **`JsonSchema.hpp:247-266`** — an enum-typed field only gets its `"enum": [...]` value list if the enum *type itself* was separately registered via `ASTRA_REFLECT_ENUM`; if it wasn't, the schema still emits `"type": "string"` with no whitelist, silently under-constraining validation. Undocumented requirement.
- **`FieldInfo.hpp:163-178` / `TypeMeta.hpp:243-258`** (`GetAttribute<A>`) — attribute type identity, like field/type identity throughout the subsystem, is decided purely by 64-bit `TypeID<A>::Hash()` equality with no collision detection at registration, in contrast to `TypeContext::GetOrAssignComponentID` (`Core/TypeContext.hpp:70-86`), which *does* assert on a hash collision in debug builds. Astronomically unlikely in practice, but asymmetric with the sibling system's own defensive check, and a collision would silently defeat every typed accessor's "safety" check in the same stroke.
- **`MetaRegistry.hpp:249-258`** (`ForEachType`) — holds a `shared_lock` for the duration of the callback; a callback that re-enters any `MetaRegistry` method on the same thread risks deadlock against `std::shared_mutex`'s non-recursive semantics (particularly under MSVC's writer-preferring SRWLOCK-backed implementation). Same pattern in `TypeMeta::ForEachField`/`ForEachAttribute`, though those don't hold a lock.
- **`TypeMeta.hpp:513`** (`TypeMetaBuilder::Field`) — reflecting a `static` data member produces a confusing compile error (`&Type::staticMember` has type `FieldType*`, not the expected `FieldType Class::*` non-type template parameter), with no documentation that only non-static members are supported.
- **Perf note**: each `FieldInfo` carries four `std::function` members (`getter`/`setter`/`getterAny`/`setterAny`, `FieldInfo.hpp:48-57`); bounded and one-time (built once at registration), but worth noting for very field-heavy components/hot reflective loops.

## Test coverage

`tests/Reflection/ReflectionTest.cpp` and `FieldVisitorTest.cpp` cover the happy paths well (registration, field lookup, `Get`/`Set`/`GetPtr`/`GetAny`/`SetAny` with matching types, attributes, enums incl. flags, lifecycle functions, container-trait type traits in isolation, JSON schema smoke tests, and ECS/`Registry` integration). Gaps directly relevant to the findings above:

- No test calls `Get<T>`/`Set<T>`/`GetPtr<T>` with a **mismatched** `T` — the exact scenario unguarded in Release (C1). At minimum, a `ASTRA_BUILD_DEBUG`-only death test asserting the mismatch is caught would document current (weak) behavior; a regression test should follow any fix that makes the check always-on.
- No test calls `Set<T>`/`GetPtr<T>` on a field declared `const` in the source struct (C2/C3) — `Player::experience`'s `ReadOnly` *attribute* is tested (`ReflectionTest.cpp:341-357`) but that's a metadata flag, not a real C++ `const` member; the actual const-member code path is entirely uncovered.
- No test reflects a `std::vector<T>` or `std::array<T,N>` **field** (as opposed to testing `ContainerTraits<T>` in isolation) — would immediately expose the always-`false` `isVector`/`isStdArray` bug (I1) via `GenerateJsonSchema`.
- No test reflects a C-style array field or a move-only (`std::unique_ptr<T>`) field — both currently fail to compile (I2); a `static_assert`-based negative/compile-fail test doesn't exist either.
- No test reflects a namespace-qualified type (I3).
- No concurrency/stress test for `MetaRegistry::Instance()`/`DrainPendingMeta` under concurrent first-touch from multiple threads (I4).
