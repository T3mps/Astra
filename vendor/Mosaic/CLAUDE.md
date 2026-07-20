# CLAUDE.md

Guidance for Claude Code when working in the Mosaic repository.

## What this is

Mosaic is the shared, dependency-free foundation for the Starworks libraries
(Astra, Manifold2D, Arcane, Loom). It owns the low-level primitives and the
pluggable seams (logging, asserts, threading) each component needs. Developed
here, then synced to GitHub and vendored into each consumer (Astra-style; no
submodule). Design + extraction plan: `docs/design.md`.

## Layout + namespaces

- `include/Mosaic/**` -- public headers, flat `namespace Mosaic`
  (`Mosaic::IWorkScheduler`, `Mosaic::FunctionRef`, `Mosaic::ILogger`),
  `Mosaic::Simd::` for SIMD types, `Mosaic::Platform` / `MOSAIC_*` macros for
  detection.
- `src/**` -- implementation `.cpp` (header-mostly; keep it thin), NOT on the
  public include path; reach headers via `<Mosaic/...>`.
- `tests/**` -- Catch2. `tests/Support/` holds test-only helpers.
- `vendor/{Catch2,premake5}` -- self-contained build; no external repo.

## Invariants

- **Zero external dependencies** -- standard library only.
  `rg 'Astra/|Manifold2D/|Arcane/' include src` must be empty.
- **The library creates no threads.** All parallelism goes through the injected
  `Mosaic::IWorkScheduler`; `SerialWorkScheduler` is the default.
- **Determinism** for `Simd/Wide`: `/fp:strict` / `-ffp-contract=off
  -fno-fast-math`. No `/fp:fast`. Move `Simd/Wide` verbatim from Manifold2D --
  do NOT refactor it during the extraction.
- C++23, UTF-8 without BOM, ASCII comments.

## Build + test (Windows)

```bat
scripts\generate_vs2022.bat
msbuild Mosaic.sln /p:Configuration=Debug /m
bin\Debug-windows-x86_64\MosaicTests\MosaicTests.exe
```

Configurations: Debug / Release / Dist. Linux (`gmake2`) is scaffolded, not green.
