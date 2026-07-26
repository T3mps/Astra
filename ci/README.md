# CI — Sanitizer Lane

Astra's primary CI builds under MSVC (Windows) and gcc/clang (Linux). MSVC masks a
whole class of dynamic faults — memory corruption, undefined behavior, data races.
The **sanitizer lane** builds and runs the full `AstraTest` suite under clang's
AddressSanitizer + UndefinedBehaviorSanitizer and, separately, ThreadSanitizer, so
that class is actually caught.

The lane is defined two ways, from one source of truth (the premake `--sanitize`
option):

- **In CI:** jobs `sanitize-asan-ubsan` and `sanitize-tsan` in
  [`.github/workflows/ci.yml`](../.github/workflows/ci.yml). These run automatically
  on GitHub Actions once the branch is pushed to `origin` (the workflows only execute
  on GitHub).
- **Locally:** the recipes below, on any Linux (or macOS) box with clang — no CI
  required. This is the way to validate the lane without pushing.

## Prerequisites

- Linux or macOS (the sanitizers here are a clang/gcc capability; the Windows/MSVC
  path is intentionally untouched by `--sanitize`).
- `clang` / `clang++`.
- `premake5` (5.0.0-beta6) and `make`.

## The `--sanitize` premake option

`premake5.lua` defines an option-gated capability:

```
premake5 gmake2 --sanitize=address   # AddressSanitizer + UndefinedBehaviorSanitizer
premake5 gmake2 --sanitize=thread    # ThreadSanitizer
```

Applied to the `AstraTest` project only, via option-gated filters. **With no
`--sanitize` flag every generated project is byte-identical to a normal generation**
(zero blast radius), so this option is safe to leave in the build authoring.

- `--sanitize=address` → `-fsanitize=address,undefined -fno-omit-frame-pointer
  -fno-sanitize-recover=all` on compile, `-fsanitize=address,undefined` on link,
  with `staticruntime "off"` (static libstdc++ conflicts with the sanitizer
  runtimes' link-ordering requirement). `-fno-sanitize-recover=all` makes UBSan
  **abort** on the first finding so a real fault fails the run rather than printing
  and passing.
- `--sanitize=thread` → `-fsanitize=thread` on compile and link, `staticruntime "off"`.

Builds use the **Debug** config (symbols on, `optimize off`) — no optimization
hiding faults.

## Run it locally

### ASan + UBSan

```bash
premake5 gmake2 --sanitize=address
make config=debug AstraTest -j"$(nproc)" CC=clang CXX=clang++
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:abort_on_error=1 \
  ./bin/Debug-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```

### ThreadSanitizer

```bash
premake5 gmake2 --sanitize=thread
make config=debug AstraTest -j"$(nproc)" CC=clang CXX=clang++
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=ci/tsan-suppressions.txt \
  ./bin/Debug-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```

> The two `--sanitize` modes produce incompatible binaries — regenerate + rebuild
> when switching between `address` and `thread` (a clean `make config=debug
> AstraTest ... clean` or a fresh checkout avoids stale objects).

## Reading the result

- **Clean exit, all tests pass:** the suite is sanitizer-clean for that sanitizer.
- **Non-zero exit with a sanitizer report:** a real, reproduced defect (the
  `halt_on_error=1` / `abort_on_error=1` options make the run fail rather than
  print-and-continue). Capture the sanitizer type, the failing test, and the stack —
  that is the actionable finding.

## `tsan-suppressions.txt`

[`tsan-suppressions.txt`](tsan-suppressions.txt) ships **empty** (header comment
only). Add an entry **only** for a triaged, understood-benign race (e.g. a documented
third-party data race that cannot be fixed here) — **never** to silence a real Astra
finding. Fix the code instead.

A leak report from LSan (bundled with ASan) on a first run can be a benign
false-positive; if a leak is genuinely benign, the remedy is a narrow LSan
suppression or `detect_leaks=0` — decided in triage, never applied silently to hide a
real leak.
