# CI

Astra's CI follows the shared Starworks CI standard (Arcane, Astra,
Manifold2D): one workflow, one OS x configuration matrix on pinned images and
compilers, one build entry point and one test entry point per shell, the same
section order in this document in every repo, and every departure from the
standard listed under [Deviations](#deviations) with its reason.

## Overview

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every push and
pull request. Job `build-test` is the matrix; job `extra` carries the RTTI-off
and sanitizer lanes. A newer push to the same ref cancels the older run
(`concurrency: ci-<ref>`).

## Matrix

| Leg | Runner image | Compiler | Configs |
|---|---|---|---|
| `windows-msvc` | `windows-2025` | MSVC from the newest Visual Studio on the image (VS 2022 17.14 today), C++20 | Debug, Release |
| `linux-gcc-14` | `ubuntu-24.04` | GCC 14 (`g++-14`), libstdc++ 14 | Debug, Release |
| `linux-clang-19` | `ubuntu-24.04` | Clang 19 (`clang++-19`) on libstdc++ 14 | Debug, Release |
| `macos-apple-clang` | `macos-15` (arm64) | Apple Clang from Xcode 16.4, libc++ | Debug, Release |

Each leg builds AstraTest and the AstraCompile16 / AstraCompile64 entity-width
checks (Windows builds the whole solution), compiles every public header on
its own on POSIX legs ([`ci/check-headers.sh`](../ci/check-headers.sh)), and runs
AstraTest twice: seed `1` and a seed derived from the run number. The JUnit XML
of both runs is uploaded as `test-results-<leg>-<config>`.

## Toolchains

- **Images** are pinned by name (`windows-2025`, `ubuntu-24.04`, `macos-15`),
  never `*-latest`, so an image-label move cannot change what is tested.
- **Windows:** `scripts/build.ps1` asks `vswhere` for the newest Visual Studio
  with MSBuild and generates for it (`vs2022` for 17.x, `vs2026` for 18.x).
- **Linux:** `g++-14` or `clang-19` + `libstdc++-14-dev` from the Ubuntu 24.04
  archive; the leg exports `CC` / `CXX`.
- **macOS:** `sudo xcode-select -s /Applications/Xcode_16.4.app`, then
  `CC` / `CXX` = `xcrun -f clang` / `clang++`. Builds are native arm64
  (`bin/<Config>-macosx-AARCH64/`); `-mavx` is x86-64-only and Apple silicon
  takes Mosaic's NEON paths.

## Build driver

premake on every OS, pinned in [`scripts/premake.lock`](../scripts/premake.lock)
(5.0.0-beta8: version plus one SHA-256 per OS). One script per shell, same
arguments:

```
scripts/build.sh  <Debug|Release|Dist> [premake options...]   # Linux, macOS
scripts/build.ps1 <Debug|Release|Dist> [premake options...]   # Windows
```

Both fetch premake into `.tools/premake/<version>/` (gitignored), refuse it
unless the SHA-256 matches the lock, generate (`gmake` / `vs20xx`), and build.

## Tests

```
scripts/run-tests.sh  <Debug|Release|Dist> [--rng-seed N]
scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]
```

Without `--rng-seed` AstraTest runs in declaration order. With it, GoogleTest
shuffles with seed `N` (1..99999), so a CI failure reproduces locally with the
seed shown in the failing step's name. Reports go to
`test-results/AstraTest-<os>-<Config>[-seed<N>].xml` (GoogleTest's JUnit XML).

`TypeNameCanonical.NamesAndHashesArePinnedOnEveryToolchain`
([`tests/Core/TypeNameCanonicalTest.cpp`](../tests/Core/TypeNameCanonicalTest.cpp))
pins canonical type names and their XXHash64 values as literals, so every leg
proves MSVC, GCC, Clang and Apple Clang agree on the identities that binary
archives persist.

## Extra lanes

All on `ubuntu-24.04` with Clang 19, through the same scripts with a premake
option selecting the lane, two seeds each:

| Lane | Config | premake option | What it proves |
|---|---|---|---|
| `linux-no-rtti` | Release, Debug | `--no-rtti` | the suite builds and passes with RTTI off (TypeIdentity's RTTI-off path) |
| `sanitize-asan-ubsan` | Debug | `--sanitize=address` | ASan + UBSan clean |
| `sanitize-tsan` | Debug | `--sanitize=thread` | TSan clean ([`ci/tsan-suppressions.txt`](../ci/tsan-suppressions.txt) ships empty) |

Sanitizer details and triage rules: [`ci/README.md`](../ci/README.md).

## Supply chain

- Actions are pinned by full commit SHA (`actions/checkout` v4.4.0,
  `actions/upload-artifact` v4.6.2), with the tag in a comment.
- The workflow token is read-only: `permissions: contents: read`.
- The only download is premake, pinned by version and SHA-256 in
  `scripts/premake.lock`; a mismatch fails the build. Compilers come from the
  pinned images (apt archive, Xcode bundle, Visual Studio install).

## Reproducing locally

```bash
# Ubuntu 24.04
sudo apt-get install -y g++-14 clang-19 libstdc++-14-dev
CC=gcc-14 CXX=g++-14 scripts/build.sh Debug
scripts/run-tests.sh Debug --rng-seed 1

# macOS (Apple silicon)
sudo xcode-select -s /Applications/Xcode_16.4.app
scripts/build.sh Release
scripts/run-tests.sh Release --rng-seed 1

# a sanitizer lane
CC=clang-19 CXX=clang++-19 scripts/build.sh Debug --sanitize=address
scripts/run-tests.sh Debug --rng-seed 1
```

```powershell
# Windows
./scripts/build.ps1 Debug
./scripts/run-tests.ps1 Debug --rng-seed 1
```

Switching between `--sanitize` modes (or `--no-rtti`) needs a clean `bin-int/`:
the modes produce incompatible objects.

## Deviations

| Deviation | Reason |
|---|---|
| C++20, not C++23, on every leg | Astra's language standard is C++20 (`cppdialect "C++20"`); the standard says "C++23 or the repo's standard". |
| Windows builds the whole solution; POSIX legs build AstraTest + AstraCompile16/64 | Unchanged from before: AstraStudio needs GLFW/OpenGL and AstraBenchmark uses `-march=native`, neither of which belongs on a portable CI leg. |
| `Dist` is not in the matrix | The standard matrix is Debug + Release. `Dist` differs from Release only in `symbols "off"` / `optimize "full"`; the scripts still build it locally. |
| Clang 19 runs on libstdc++ 14 rather than libc++ | It matches what Linux consumers link; libc++ is covered by the macOS leg. |
| Extra lanes use Clang 19 (formerly the image's default `clang`, 18) | Pins the compiler like the main matrix; `libclang-rt-19-dev` provides the sanitizer runtimes. |
| No `docs/ci.md` existed in Arcane or Manifold2D at the time of writing | This file sets the section order (Overview, Matrix, Toolchains, Build driver, Tests, Extra lanes, Supply chain, Reproducing locally, Deviations) for the other two to follow. |
