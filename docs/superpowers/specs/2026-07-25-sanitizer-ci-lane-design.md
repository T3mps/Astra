# Sanitizer CI Lane — Design (2026-07-25)

**Status:** user-approved design (this session).

**Goal:** Add an ASan+UBSan and a TSan CI lane that builds and runs the full `AstraTest`
suite on a non-MSVC toolchain (clang on Linux) so the dynamic memory-corruption / UB /
data-race bug class that MSVC masks is actually caught — both as a regression net for all
future work and as an immediate first-time sanitizer pass over the current (unpushed) body
of work. The review-of-record (`docs/reviews/2026-07-21-astra-current-state-full-review.md`)
named this lane "urgent, not optional" because it would have caught Commands C1 (fixed) and
Serialization C5 (open).

## Context (feasibility — already in place)

- **Portable build exists.** `premake5.lua` has complete `system:linux` / `system:macosx`
  filters (AVX, warnings, pthread, LTO). `premake5 gmake2` generates working Makefiles.
- **Non-MSVC CI already runs.** `.github/workflows/ci.yml` has a `linux` job building +
  running `AstraTest` under BOTH gcc and clang, Debug + Release, today. This lane is an
  ADDITIVE change to a working non-MSVC path, not a build-authoring project.
- **The suite exercises real threads** (`tests/System/SystemContextTest.cpp` launches
  `std::thread`s), so TSan has genuine concurrency to analyze — targeting the two open UAF
  Importants (Relations `ParallelForEachDescendant` cache-by-ref; View iteration guard gap).

## Scope (user decisions, this session)

| Decision | Choice |
|---|---|
| Sanitizers | **ASan+UBSan (one job) + TSan (separate job)** — MSan declined (needs instrumented libc++, floods false positives) |
| First-run policy | **Triage first, then decide gating** — land the config, run it, see what lights up; don't pre-commit to gating vs advisory |
| Structure + validation | **Capability in premake (`--sanitize` option, reusable on any Linux box) + CI as thin consumer; validate by pushing THIS branch to `origin` so Actions runs it** |

## 1. premake capability — `--sanitize` option

A new premake option makes sanitizer builds a first-class, discoverable, reusable capability
(usable locally on any Linux/macOS box AND by CI):

- `newoption { trigger = "sanitize", value = "MODE", allowed = { {"address","ASan + UBSan"}, {"thread","ThreadSanitizer"} } }`.
- Applied to the `AstraTest` project (and the reusable core for any target that wants it) via
  option-gated filters:
  - `filter { "options:sanitize=address" }` → buildoptions + linkoptions
    `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`.
    (ASan and UBSan compose; `-fno-sanitize-recover=all` makes UBSan ABORT on first finding
    so CI actually fails; `-fno-omit-frame-pointer` for readable ASan stacks.)
  - `filter { "options:sanitize=thread" }` → buildoptions + linkoptions `-fsanitize=thread`.
  - Both filters also set `staticruntime "off"` and (equivalently) avoid `-static-libstdc++`,
    because static libstdc++ conflicts with the sanitizer runtimes' "must link first"
    ordering on Linux. **This interaction is the primary implementation risk and is
    confirmed by the push-to-validate CI run (§4).**
- **Zero blast radius when unused:** the filters are option-gated, so `premake5 vs2022`,
  `premake5 gmake2`, and every existing CI build with no `--sanitize` flag generate
  byte-identical projects to today. The Windows/MSVC path is never touched (sanitizers are a
  clang/gcc-on-Linux capability here).
- **Config:** sanitizer builds use the **Debug** config (symbols on, `optimize off`) — no
  optimization hiding faults; matches the existing Debug linux job.

## 2. CI jobs — thin consumers in `ci.yml`

Two new jobs, each mirroring the existing `linux` job's shape (checkout, install clang,
download premake, generate, build, test), differing only in the `--sanitize` flag and the
runtime `*_OPTIONS`:

- **`sanitize-asan-ubsan`** (runs-on ubuntu-latest, clang):
  `./premake5 gmake2 --sanitize=address` → `make config=debug AstraTest -j"$(nproc)" CC=clang CXX=clang++`
  → run `AstraTest` with
  `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:abort_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:abort_on_error=1`.
- **`sanitize-tsan`** (runs-on ubuntu-latest, clang):
  `./premake5 gmake2 --sanitize=thread` → `make config=debug AstraTest ...` → run with
  `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=ci/tsan-suppressions.txt`.

Details:
- **clang only** for the sanitizer jobs (clang's sanitizers are the reference implementation;
  the existing `linux` job already covers gcc+clang correctness). clang on Ubuntu uses
  libstdc++ by default, which is where the C1/SSO-relocation class manifests — so libstdc++
  coverage is achieved without extra setup. A libc++ variant (`-stdlib=libc++`) is a possible
  later add, explicitly out of scope for v1.
- **Benchmarks excluded** (the premake benchmark filters use `-march=native`, already noted
  as CI-cross-compiler-unsafe; only `AstraTest` is built).
- **Suppressions scaffold:** `ci/tsan-suppressions.txt` ships EMPTY (a header comment
  explaining its purpose). Entries are added ONLY from triaged, understood-benign findings
  (e.g. a documented third-party race) — never to silence a real finding. An analogous
  `ASAN_OPTIONS=suppressions=` path is available if a real need appears; not created empty.

## 3. Gating (decoupled from the YAML)

"Required check" is a GitHub branch-protection SETTING (repo config), not a file in this repo.
So this task adds the jobs; whether they block merges is a separate switch the user controls:
- First run green → the user may flip both jobs to required checks.
- First run red → leave them advisory; the findings become concrete inputs to the correctness
  work (C4/C5 + the UAFs), and gating is flipped once the suite is sanitizer-clean.
This cleanly realizes the "triage first" decision without the YAML having to encode a policy.

## 4. Validation strategy — push to run

- The lane's value only materializes when Actions runs it, which requires the branch on
  `origin` (GitHub — where the workflows live). `origin/dev` already exists (dev is 264 ahead).
- **Pushing this branch carries dev's 264 unpushed commits up AS A BRANCH** (they are the
  branch's ancestors; git cannot push a branch without its history). This is NOT a merge to
  `main`/`dev` — it is feature-branch history landing on the remote so CI can build it, which
  is the normal and only way to validate a CI change. The user accepted push-to-validate.
- **Triage step (part of execution, not a code change):** after the branch is pushed, read
  the two sanitizer jobs' results (`gh run` / the Actions UI). Outcomes:
  - **Green:** the lane is validated AND the entire unpushed body of work (perf levers,
    Commands hardening, enableable components) is certified ASan/UBSan/TSan-clean for the
    first time. Record this; the user may enable required checks.
  - **Red:** capture each finding (sanitizer + stack) as a concrete, reproduced defect. These
    feed the follow-on correctness work (likely C5 and/or the Relations parallel-descendant
    UAF). The lane has earned its keep on day one. Do NOT paper over a real finding with a
    suppression.
- I cannot run clang/libc++ sanitizers on this Windows/MSVC box, so local pre-validation of
  the sanitizer build is not possible; the Actions run IS the validation. The premake option
  and YAML are validated by inspection + the real run.

## 5. Testing / acceptance

- **Non-sanitizer builds unaffected:** `premake5 vs2022` and `premake5 gmake2` (no option)
  produce identical output to pre-change; the existing Windows + linux CI jobs stay green.
  (Verified by inspection — the option filters are the only additions and are gated.)
- **Sanitizer build compiles + links** under clang on Linux with the `--sanitize` flag (the
  staticruntime/link-ordering interaction resolved) — confirmed by the Actions run.
- **Sanitizer jobs run the full suite** and either pass clean or fail with actionable
  sanitizer output (halt_on_error ensures a real finding fails the job rather than printing
  and passing).
- **Acceptance = a completed Actions run on the pushed branch with both sanitizer jobs'
  outcomes triaged and recorded.** A green pair is the success state; a red pair is a
  successful lane with captured findings (the follow-on fixes are a SEPARATE effort, not this
  task's deliverable).

## 6. Out of scope (recorded)

- Fixing whatever the sanitizers find (C5, UAFs) — separate correctness work; this task
  delivers the lane + the triaged first-run result, not the fixes.
- MSan; libc++ variant; macOS sanitizer job; gcc sanitizer job (clang is the v1 reference).
- Coverage instrumentation, fuzzing harness (adjacent but distinct future lanes).
- Configuring the GitHub branch-protection "required check" setting (a repo setting the user
  owns, not a file change).

## 7. Files touched

- Modify: `premake5.lua` (the `newoption` + option-gated sanitize filters on `AstraTest`).
- Modify: `.github/workflows/ci.yml` (two new jobs).
- Create: `ci/tsan-suppressions.txt` (empty scaffold + header comment).
- (No `include/` or `tests/` changes — this is build/CI infrastructure. If the first-run
  triage requires a code fix to reach green, that is surfaced as a finding for separate work,
  not folded in silently.)
