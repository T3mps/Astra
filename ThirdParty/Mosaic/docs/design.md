# Mosaic -- the shared Starworks Core library (design note)

Date: 2026-07-13. Status: draft for review. Author: design pass with Ethan.

> Interim home: this note lives in the Aphelyon `docs/superpowers/specs/` tree
> (where the Astra/Manifold2D planning already lives). It moves into the Mosaic
> repo (`docs/`) once that repo is stood up.

## 1. Context and goal

Starworks ships as **pickable, independently-usable components** -- ECS
(**Astra**), 2D physics (**Manifold2D**), engine (**Arcane**), runtime
(**Loom**). Each of those, to stay dependency-free, has grown its **own copy of
the same low-level primitives and seams**: a threading interface, force-inline /
arch-detection macros, a SIMD layer, a non-owning callable, bit utilities, and
(in Astra) logging + assert seams. That duplication is now a maintenance tax and
a divergence risk -- the two `IWorkScheduler` interfaces have *already* drifted
apart (see D3).

**Mosaic** is the shared foundation those components sit on -- the common ground
the tiles are set into. It is a standalone, open-source, **zero-dependency**
(stdlib only) library that owns the primitives and the **pluggable seams**
(logging, asserts, threading) every Starworks library needs, so each library
consumes one canonical copy instead of hand-rolling its own.

**Metaphor discipline:** the components are the tesserae; Mosaic is what makes
them cohere into one picture. It is the *substrate*, not the assembled whole.

### Dependency arrow (one way, always)

```
        Mosaic   (stdlib only -- depends on NOTHING)
       /   |   \
   Astra  Manifold2D  Arcane      (each depends on Mosaic)
                         |
                       Loom / Game
```

Mosaic must never include anything from `Astra/`, `Manifold2D/`, `Arcane/`, or
any third party. `rg` for those in the Mosaic tree returns zero. This is the
same liftability rule Manifold2D already lives under.

### Non-goals

- Not a utility grab-bag. Only primitives + seams that are **already duplicated
  or already needed** across >=2 components go in. No speculative additions.
- Not a behavior change. Extractions are value/behavior-identical (the seam
  contracts are the *union* of what the consumers already rely on).
- Not a runtime. Mosaic creates no threads, opens no files, owns no globals it
  can avoid; the seams are interfaces the *host* injects implementations for.

## 2. Reference model: how it ships (Astra/Manifold2D-style)

Mosaic is developed as a standalone repo and **synced-copy vendored** into each
consumer, exactly like Astra and Manifold2D today:

```
D:\dev\starworks\Mosaic          source of truth  (dev happens here)
      | sync_to_github.ps1        robocopy -> GitHub clone
      v
D:\dev\github\Mosaic              github.com/<org>/Mosaic   (publish)
      | vendor.ps1                robocopy into each consumer
      v
<consumer>\ThirdParty\Mosaic     full source mirror (Astra / Manifold2D / Arcane)
```

**Header-mostly.** Platform, SIMD, Bits, FunctionRef, and the seam *interfaces*
are all headers; the only compiled units are tiny (e.g. `Version.cpp`, and any
default seam impls we choose to ship non-inline). This keeps vendoring trivial
and lets header-only consumers (Astra) stay header-only.

## 3. v0 module map (what moves, and the evidence it should)

| Module | Role | Duplicated/needed today |
|---|---|---|
| `Mosaic/Platform` | arch/compiler/capability detection: `MOSAIC_ARCH_*`, `MOSAIC_COMPILER_*`, `MOSAIC_HAS_{SSE2,SSE42,AVX,AVX2,NEON,...}`, `MOSAIC_FORCEINLINE`, `MOSAIC_NODISCARD`, `MOSAIC_HAS_BUILTIN` | Astra has a mature `Platform.hpp`; Manifold2D hand-rolls a 3-macro subset inline in `Simd.hpp` |
| `Mosaic/Jobs` | `IWorkScheduler` + `SerialWorkScheduler` (threading seam) | **both** Astra & Manifold2D ship their own -- and they've **diverged** (D3) |
| `Mosaic/Log` | logging seam: `LogSink` fn-ptr + `SetLogSink`/`SetLogLevel`, `MOSAIC_LOG_*` (compile-time floor + runtime level) | Astra has `Log.hpp` (the move origin); Manifold2D has none but needs one |
| `Mosaic/Assert` | assert seam: `AssertHandler` + `MOSAIC_ASSERT`/`VERIFY`/`ENSURE` | Astra's `Assert.hpp` is the move origin; Manifold2D/Arcane use bare `assert()` |
| `Mosaic/Simd/Wide` | numeric `f32w/i32w/b32w` lane vectors (+ Scalar/AVX2/NEON backends) | Manifold2D (determinism-critical) |
| `Mosaic/Simd/Bits` | byte-match / `Int128`/`Int256` bitmap / hash toolbox | Astra (SwissTable/bloom acceleration) |
| `Mosaic/Bits` | generic `PopCount`/`CountTrailingZeros`/`FindFirstSet`/`FindLastSet` | Astra's bit-scan helpers (generic, not really SIMD) |
| `Mosaic/BitSet` | grow-only fixed-capacity bit set (b2BitSet equivalent) | Manifold2D's per-worker narrowphase scratch; generic enough to share |
| `Mosaic/FunctionRef` | non-owning zero-alloc callable | Manifold2D has `FunctionRef`; Astra uses `std::function` in the scheduler seam (upgrade target) |

**Namespace:** flat `Mosaic::` for primitives and seams (`Mosaic::IWorkScheduler`,
`Mosaic::FunctionRef`, `Mosaic::ILogger`), `Mosaic::Simd::` for the SIMD types,
`Mosaic::Platform`/macros for detection. Mirrors Manifold2D's flat `Manifold2D::`
Core convention.

## 4. Design decisions

### D1. The threading seam is the first extraction -- and the reconciliation is the proof

Both libraries already have `IWorkScheduler`, and they have **drifted**:

```cpp
// Manifold2D::IWorkScheduler          // Astra::IWorkScheduler
ParallelFor(size_t count,              ParallelFor(size_t count,
            size_t minBatch,                       size_t minBatch,
  FunctionRef<void(size_t begin,         const std::function<void(size_t begin,
                   size_t end,                                    size_t end)>& fn)
                   uint32_t worker)>);  // <-- NO worker id, std::function
uint32_t WorkerCount();                size_t WorkerCount();
```

Differences: (a) Manifold2D's `fn` carries a **`worker` id** (solver-MT and
broadphase-MT index per-worker scratch without locking); Astra's does not.
(b) Manifold2D uses zero-alloc `FunctionRef`; Astra uses `const std::function&`.
(c) `uint32_t` vs `size_t`. And each documents constraints the other omits:
Manifold2D pins "**distinct** worker id per concurrently-running range"; Astra
pins "**bidirectional happens-before**, no-throw, no OS-thread migration
mid-`fn` (thread_local per-lane state), re-entrancy."

**Unified `Mosaic::IWorkScheduler` = Manifold2D's signature + the union of both contracts:**

```cpp
namespace Mosaic
{
    struct IWorkScheduler
    {
        // Partition [0,count) into DISJOINT contiguous sub-ranges (each >= minBatch
        // where possible) covering it exactly once, and invoke fn(begin,end,worker)
        // on each, possibly concurrently. BLOCKS until all complete. count==0 is a
        // no-op; minBatch==0 is treated as 1.
        //
        // worker in [0,WorkerCount()) names the running lane. Concurrently-running
        // sub-ranges MUST receive DISTINCT worker ids, so fn may index per-worker
        // scratch without locking (Manifold2D solver-MT/broadphase-MT rely on this).
        //
        // Memory model: happens-before in BOTH directions -- writes before
        // ParallelFor are visible inside fn; writes inside fn are visible to the
        // caller once ParallelFor returns. fn must not throw. fn must not migrate
        // OS threads mid-invocation (consumers keep thread_local per-lane state);
        // fiber schedulers must pin a task for fn's duration. Re-entrant: legal to
        // call from within a running fn (may degrade to inline).
        virtual void ParallelFor(std::size_t count, std::size_t minBatch,
                                 FunctionRef<void(std::size_t begin, std::size_t end,
                                                  std::uint32_t worker)> fn) = 0;

        // Inclusive of the calling thread; always >= 1. Batch-size denominator.
        virtual std::uint32_t WorkerCount() const noexcept = 0;

        virtual ~IWorkScheduler() = default;
    };

    // Deterministic default: runs the whole range inline as worker 0.
    class SerialWorkScheduler final : public IWorkScheduler { /* ... */ };
}
```

Migration cost is asymmetric and instructive:
- **Manifold2D**: already this exact shape -- retarget the namespace to `Mosaic::`
  and adopt `Mosaic::FunctionRef`. Near-zero.
- **Astra**: two real edits -- (1) callbacks gain a third `worker` param (call
  sites use it or ignore it), (2) `const std::function&` -> `Mosaic::FunctionRef`
  (drops a potential heap alloc, enforces no-throw). A **strict improvement**, but
  it touches Astra's `ParallelFor` sites + its scheduler adapter.

The engine keeps its own enkiTS-backed adapter (`Arcane/Jobs/*WorkScheduler.hpp`)
that now targets `Mosaic::IWorkScheduler`; the test pools (Manifold2D's
`TestWorkScheduler`, Astra's `TestWorkerPool`) likewise retarget.

### D2. Platform.hpp is the substrate under everything

Adopt Astra's mature `Platform.hpp` (arch/compiler/capability macros), renamed
`ASTRA_*` -> `MOSAIC_*`. Manifold2D's `Simd.hpp` `#if` ladder (currently raw
`__AVX2__`/`__ARM_NEON` + a 3-macro inline block) keys off `MOSAIC_HAS_AVX2` /
`MOSAIC_HAS_NEON` / `MOSAIC_FORCEINLINE` instead. This is *also* what unblocks
Manifold2D's Linux port (Part B) -- the portability the port needs lives here.

### D3. SIMD is two siblings over Platform, NOT one merged type

The two SIMD layers solve different problems and must not be fused:
- `Mosaic/Simd/Wide` -- Manifold2D's numeric lane-wide float vectors
  (`f32w/b32w/i32w`, operator overloads, `mul_add`, `gather/scatter`). **The
  determinism contract is preserved verbatim**: `/fp:strict`, fused-only FMA,
  bit-matches the scalar oracle. This is a hard requirement (byte-identical MT);
  it must not be "optimized" toward the Bits layer's speed-first ethos.
- `Mosaic/Simd/Bits` -- Astra's byte-match (`MatchByteMask`), `Int128/256`
  bitmap ops, `HashCombine`, prefetch, `BatchOps` (SwissTable/bloom).

Manifold2D includes `Wide`; Astra includes `Bits`; neither needs the other, but
both stand on `Mosaic/Platform`. Astra's generic bit-scans (`PopCount`,
`CountTrailingZeros`, ...) split out to `Mosaic/Bits` (they're not SIMD).

### D4. Log + Assert are pluggable seams (host injects the impl)

- `Mosaic::ILogger` -- a minimal sink interface (level + message + optional
  category/source-loc); generalize Astra's `Log.hpp`. Default = a no-op / stderr
  sink. Consumers/hosts inject their real logger (Aphelyon's `Logger`, Arcane's).
- `Mosaic::IAssertHandler` + `MOSAIC_ASSERT(cond, msg)` -- the pluggable assert
  Astra doesn't have yet (its assert is a bare `assert()` today). Default handler
  = `assert()`-equivalent in debug, `((void)0)` in dist. Host can install a
  handler (log + break, throw in tests, etc.). **This is the item the user is
  actively building for Astra -- Mosaic is where the injectable version lands so
  both adopt one design.**

### D5. FunctionRef is the canonical non-owning callable

`Mosaic::FunctionRef` (Manifold2D's) is the seam callable -- non-owning,
zero-alloc, no exceptions. Astra's scheduler seam moves off `std::function` onto
it (D1). **Open question:** Astra's richer `Delegate.hpp` (owning / multicast?)
is a *different* abstraction; decide whether it belongs in Mosaic or stays
Astra-specific (leaning: stays in Astra until a second consumer needs it -- the
no-speculative-additions rule).

## 5. Extraction order (strangler, one landable step at a time)

Each step is independently landable and value-exact; a consumer deletes its
local copy only when its last includer has migrated (M0-strangler discipline).

1. **Stand up Mosaic** + `Platform.hpp` + `FunctionRef` + `IWorkScheduler`/
   `SerialWorkScheduler`. (These are the tightest, most-duplicated, lowest-risk.)
   Vendor into Manifold2D first (near-zero churn -- it already matches), prove
   green, then into Astra (absorbs the signature+contract change), prove green.
2. **SIMD**: move `Simd/Wide` (Manifold2D) + `Simd/Bits` + `Bits` (Astra) over
   `Platform`. Manifold2D's determinism gate (`~[gpu]` value-exact / the
   standalone suite) is the tripwire; Astra's SwissTable/bloom tests are theirs.
   DONE. Both consumers deleted their copies; Manifold2D qualifies `Mosaic::Simd::`
   directly, Astra keeps its vocabulary through a `namespace Simd = ::Mosaic::Simd`
   re-export in `Core/Simd.hpp` (its 8 includers, several of them in-flight, did not
   move). This also discharged step 1b's deferred Platform deletion in BOTH: Astra's
   `Core/Platform.hpp` + the `Core/Base.hpp` attribute block are now one-line
   re-exports of `MOSAIC_*`, and Manifold2D's Platform copy (the `ARCANE_SIMD_INLINE`
   macro + raw `__AVX2__`/`__ARM_NEON` ladder) lived inside the Simd files that moved.
   Backend selection is byte-identical in both (`MOSAIC_HAS_AVX2` <=> `__AVX2__`;
   Astra's premake defines `__SSE2__`/`__SSE4_2__` on MSVC, which does not predefine
   them). Mosaic's own suite grew a cross-validation gate for the moved code
   (`BitsTest` + `SimdBitsTest`: every backend mask must equal the scalar reference).
3. **Log + Assert** seams -- co-designed with Astra's in-flight work; land the
   injectable interfaces, wire the hosts' real impls.
   INTERFACES LANDED (`Mosaic/Log.hpp` + `Mosaic/Assert.hpp`, ported from Astra's
   design -- fn-ptr+`void*` sink, `AssertAction Break/Continue` handler,
   `MOSAIC_ASSERT` / `VERIFY` / `ENSURE` / `ENSURE_ALWAYS`, continuable debug-break,
   ENSURE breaks only into an ATTACHED debugger). Two deliberate departures from the
   origin: asserts default to active on `!NDEBUG` (so a host gets them in its debug
   build without learning a Mosaic build flag), and the default assert handler falls
   back to stderr when no sink is installed (a fatal assert must not be silently
   swallowed). Mosaic's own `BitSet` bounds guard and `Simd/Bits` alignment
   preconditions are the seam's first consumers.
   HOST ADOPTION IS STILL OPEN: Astra's `Core/Log.hpp` + `Core/Assert.hpp` were being
   actively edited during this pass, so they were LEFT ALONE -- Astra keeps its own
   copies for now. Convergence (Astra deletes its copies / installs its handler into
   Mosaic, Arcane wires its logger) is the next step, once that work lands.
4. **Bit utilities / any remaining shared leaf** as a mop-up.
   DONE for `BitSet`: moved out of Manifold2D (its `include/Manifold2D/Core/` is now
   GONE -- every primitive it had lives in Mosaic), its unit test moved with it, and
   `Manifold2DCoreTest.cpp` became `MosaicLinkSmokeTest.cpp`. Astra's `Delegate` and
   its `Bitmap` container stay Astra-side (no speculative moves).

Arcane picks Mosaic up transitively (it already consumes Astra + Manifold2D) and
directly for anything it uses first-hand; its enki adapter retargets to
`Mosaic::IWorkScheduler`.

## 6. Gates / invariants

- **Zero deps**: `rg 'Astra/|Manifold2D/|Arcane/' include src` in Mosaic = 0.
- **No threads created** by Mosaic; `SerialWorkScheduler` is the inject-nothing default.
- **Determinism** for `Simd/Wide`: `/fp:strict` (MSVC) / `-ffp-contract=off
  -fno-fast-math` (gcc/clang), fused-only, bit-matches scalar. Manifold2D's
  existing value-exact suite is the regression gate.
- Each consumer stays green at value-exact counts across the migration (Astra's
  suite; Manifold2D's standalone `Manifold2DTests` + Aphelyon `~[gpu]`).
- UTF-8 no BOM, ASCII comments, C++23, header-mostly, static + `/MD` both buildable
  (parameterized like the ThirdParty wrappers -- Astra is header-only so this only
  bites the compiled bits).

## 7. Decisions to lock before coding

1. **Name of the org/repo path** (`github.com/T3mps/Mosaic` vs a Starworks org).
2. **`WorkerCount()` return type** -- `uint32_t` (proposed, matches the worker id)
   vs `size_t` (Astra's current). Trivial but pick once.
3. **Log/Assert in v0 or fast-follow?** (Leaning: Platform + FunctionRef +
   Jobs in v0; Log/Assert as step 3 co-designed with Astra's in-flight work.)
4. **Does Astra's `Delegate` move to Mosaic?** (Leaning: no, not yet.)
5. **Namespace shape** -- flat `Mosaic::` (proposed) vs `Mosaic::Core::`.

## 8. Risks

- **Three-repo coordination.** Mitigated by the strangler order (step 1 is the
  smallest, and Manifold2D adopts first at ~zero churn to prove the pipeline).
- **Astra's `IWorkScheduler` signature change** touches its call sites (the
  worker param + FunctionRef). Contained, but it's the one real edit in v0.
- **Determinism drift during the SIMD move** -- the value-exact suite is the
  tripwire; move `Simd/Wide` verbatim, do not refactor it in the same step.
- **Name search noise** (MosaicML). Handled by repo description + the `Mosaic::`
  C++ namespace; no functional collision.
```
