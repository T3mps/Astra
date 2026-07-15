# Mosaic

Mosaic is the shared foundation the Starworks libraries are built on -- the
common ground the component "tiles" are set into. It is a standalone,
**zero-dependency** (C++ standard library only) library that owns the low-level
primitives and the **pluggable seams** every Starworks component needs, so each
consumer takes one canonical copy instead of hand-rolling its own.

Starworks ships as pickable, independently-usable components -- **Astra** (ECS),
**Manifold2D** (2D physics), **Arcane** (engine), **Loom** (runtime). Each
depends on Mosaic; Mosaic depends on nothing but the standard library. The
dependency arrow runs one way.

## What's in it

- **`Mosaic/Platform`** -- architecture / compiler / SIMD-capability detection
  (`MOSAIC_HAS_AVX2/NEON/...`, `MOSAIC_FORCEINLINE`, ...).
- **`Mosaic/Jobs`** -- the threading seam: `IWorkScheduler` + `SerialWorkScheduler`.
  Mosaic creates no threads; the host injects a scheduler (e.g. an enkiTS adapter).
- **`Mosaic/Log`** -- the logging seam (`ILogger` sink); the host injects the impl.
- **`Mosaic/Assert`** -- the assert seam (`IAssertHandler` + `MOSAIC_ASSERT`).
- **`Mosaic/Simd/Wide`** -- deterministic numeric lane-wide float vectors
  (`f32w/i32w/b32w`) for solver-style math (`/fp:strict`, bit-matches scalar).
- **`Mosaic/Simd/Bits`** -- byte-match / 128/256-bit bitmap / hash toolbox
  (SwissTable / bloom acceleration).
- **`Mosaic/Bits`**, **`Mosaic/FunctionRef`** -- generic bit intrinsics and a
  non-owning zero-alloc callable.

Everything lives under `namespace Mosaic`; headers are included as
`#include <Mosaic/Jobs/WorkScheduler.hpp>`.

See [`docs/design.md`](docs/design.md) for the full design + extraction plan.

## Invariants

- **Zero external dependencies** -- standard library only. No `Astra/`,
  `Manifold2D/`, `Arcane/`, or third-party includes ever enter `include/`/`src/`.
- **The library creates no threads.** Parallelism goes through the injected
  `IWorkScheduler`; `SerialWorkScheduler` is the deterministic default.
- **Determinism** for `Simd/Wide`: `/fp:strict` (MSVC) / `-ffp-contract=off
  -fno-fast-math` (gcc/clang). No `/fp:fast`.
- Header-mostly, C++23, UTF-8 without BOM, ASCII comments.

## Build + test (Windows)

```bat
scripts\generate_vs2022.bat
msbuild Mosaic.sln /p:Configuration=Release /m
bin\Release-windows-x86_64\MosaicTests\MosaicTests.exe
```

Configurations: Debug / Release / Dist. Self-contained -- the vendored
`premake5.exe` + Catch2 mean no other repo is required. Linux
(`scripts/generate_linux.sh` -> `gmake2`) is scaffolded, not yet green.

## License

MIT -- see [LICENSE](LICENSE).
