# Sanitizer CI Lane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an ASan+UBSan and a TSan CI lane (clang on Linux) that builds and runs the full `AstraTest` suite, catching the dynamic memory-corruption / UB / data-race bug class MSVC masks — as an ongoing regression net and a first-ever sanitizer pass over the current unpushed work.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-25-sanitizer-ci-lane-design.md`. The capability lives in premake as an option-gated `--sanitize=address|thread` flag set (reusable on any Linux box); `.github/workflows/ci.yml` gains two thin consumer jobs; a `ci/tsan-suppressions.txt` scaffold ships empty. The lane is validated by pushing the branch to `origin` and triaging the real Actions run.

**Tech Stack:** premake5 (5.0.0-beta6), GitHub Actions, clang/ASan/UBSan/TSan on Ubuntu, GoogleTest.

## Global Constraints

- Branch: `ci/sanitizer-lane` off dev HEAD (record SHA in the ledger). Finish = validation via a real Actions run (push required — user pre-approved push-to-validate), then FF-merge local. The lane is the deliverable; sanitizer FINDINGS (if the run is red) are a SEPARATE follow-on effort, NOT this task's job to fix.
- Baseline: dev @ `533da9e`. Windows build unchanged by this work: **AstraTest Debug/Release/Dist = 772 / 770 / 770** (re-verify only if a step could touch it; Task 1 must not).
- Local premake5: `/d/dev/_shared/tools/premake5` (Bash) / `D:\dev\_shared\tools\premake5.exe` (PowerShell). `.gitignore` ignores `ide/`, `Makefile`, `*.make` — generating gmake2/vs2022 locally creates ONLY ignored artifacts (no tracked churn), so premake wiring is fully validatable locally by generation + Makefile inspection.
- **Zero blast radius (binding):** the sanitize filters are `filter { "options:sanitize=..." }`-gated, so with NO `--sanitize` flag every generated project (Windows VS, plain Linux gmake) is byte-identical to today. This is proven in Task 1 by a no-option gmake2 diff.
- **Exact sanitizer flag sets (verbatim from spec §1/§2):**
  - `--sanitize=address` → buildoptions AND linkoptions `-fsanitize=address,undefined`; buildoptions also `-fno-omit-frame-pointer -fno-sanitize-recover=all`; `staticruntime "off"`.
  - `--sanitize=thread` → buildoptions AND linkoptions `-fsanitize=thread`; `staticruntime "off"`.
- **Runtime options (verbatim):** ASan job `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:abort_on_error=1`; TSan job `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=ci/tsan-suppressions.txt`.
- clang only for the sanitizer jobs; Debug config; `AstraTest` only (no benchmark). MSan / libc++ / macOS / gcc sanitizer jobs are out of scope.
- Model recipe (SDD): sonnet Task 1 and Task 2 (mechanical config, no reasoning-heavy code); controller drives the push+triage; opus not required for a whole-branch review of two config files — a single controller read of the final diff suffices (note in Finishing).

---

### Task 1: premake `--sanitize` option + gated filters

**Files:**
- Modify: `premake5.lua` (add `newoption` near the top before `workspace`; add two option-gated filters inside the `AstraTest` project block, after its existing `configurations:*` filters at ~line 129, before the `AstraBenchmark` project at line 131)

**Interfaces:**
- Consumes: nothing.
- Produces: a `sanitize` premake option with allowed values `address` / `thread`; when passed to any generator, injects the sanitizer flag sets into the `AstraTest` project only. Task 2's CI jobs invoke `premake5 gmake2 --sanitize=address` / `--sanitize=thread`.

- [ ] **Step 1: Capture the pre-change gmake2 baseline for the zero-blast-radius diff.**

Run (Bash):
```bash
PM=/d/dev/_shared/tools/premake5
"$PM" gmake2 >/dev/null && cp ide/AstraTest.make /tmp/AstraTest.make.baseline && echo "baseline captured"
```
Expected: `baseline captured`. (If `ide/AstraTest.make` isn't the path your premake emits, find it: `ls ide/*.make` — the AstraTest project has `location "ide"`, so its makefile lives under `ide/`. Use the actual path consistently below.)

- [ ] **Step 2: Add the `newoption` block** at the top of `premake5.lua`, immediately before `workspace "Astra"` (line 1):

```lua
newoption {
    trigger = "sanitize",
    value = "MODE",
    description = "Build AstraTest with a sanitizer (Linux/clang; used by the CI sanitizer lane)",
    allowed = {
        { "address", "AddressSanitizer + UndefinedBehaviorSanitizer" },
        { "thread",  "ThreadSanitizer" },
    }
}

```

- [ ] **Step 3: Add the two gated filters** inside the `AstraTest` project, immediately after the `filter "configurations:Dist"` block ends (after line 129, before the blank line preceding `project "AstraBenchmark"`). Read lines 107-130 first to place it right; the new filters must be at the same indentation as the sibling `filter "configurations:*"` blocks:

```lua
            -- Sanitizer lane (opt-in via --sanitize; Linux/clang). Option-gated so a
            -- normal generation with no --sanitize is byte-identical. staticruntime is
            -- forced off because static libstdc++ conflicts with the sanitizer runtimes'
            -- link-ordering requirement. Both compile AND link need the -fsanitize flags.
            filter { "options:sanitize=address" }
                staticruntime "off"
                buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all" }
                linkoptions  { "-fsanitize=address,undefined" }

            filter { "options:sanitize=thread" }
                staticruntime "off"
                buildoptions { "-fsanitize=thread" }
                linkoptions  { "-fsanitize=thread" }

            filter {}
```

- [ ] **Step 4: Verify the option is registered.**

Run: `/d/dev/_shared/tools/premake5 --help`
Expected: output includes a `--sanitize=MODE` line with the description, and lists `address` / `thread` as allowed values. (premake prints registered options in `--help`.)

- [ ] **Step 5: Verify zero blast radius (no-option generation unchanged).**

Run (Bash):
```bash
PM=/d/dev/_shared/tools/premake5
"$PM" gmake2 >/dev/null && diff /tmp/AstraTest.make.baseline ide/AstraTest.make && echo "IDENTICAL"
```
Expected: `IDENTICAL` (no diff). Proves that without `--sanitize`, the AstraTest makefile is unchanged — the filters contribute nothing. (The same option-gating protects the VS/`Astra.sln` path structurally.)

- [ ] **Step 6: Verify the ASan flags land on build AND link.**

Run (Bash):
```bash
PM=/d/dev/_shared/tools/premake5
"$PM" gmake2 --sanitize=address >/dev/null
grep -c -- '-fsanitize=address,undefined' ide/AstraTest.make    # expect >= 2 (build + link)
grep -- '-fno-omit-frame-pointer' ide/AstraTest.make            # expect >= 1 hit
grep -- '-fno-sanitize-recover=all' ide/AstraTest.make          # expect >= 1 hit
grep -c -- '-static-libstdc++' ide/AstraTest.make               # expect 0 (staticruntime off)
```
Expected: the `-fsanitize=address,undefined` count is ≥2 (appears in `ALL_CXXFLAGS`/build and `ALL_LDFLAGS`/link), the frame-pointer and no-recover flags are present, and there is NO `-static-libstdc++`. If `-static-libstdc++` appears, the `staticruntime "off"`-in-filter didn't take on this premake version — STOP and report (the Actions run would fail to link); the fallback is a `-shared-libasan` linkoption, but confirm the actual premake behavior first rather than guessing.

- [ ] **Step 7: Verify the TSan flags land.**

Run (Bash):
```bash
PM=/d/dev/_shared/tools/premake5
"$PM" gmake2 --sanitize=thread >/dev/null
grep -c -- '-fsanitize=thread' ide/AstraTest.make               # expect >= 2 (build + link)
grep -c -- '-fsanitize=address' ide/AstraTest.make              # expect 0 (address filter not active)
```
Expected: `-fsanitize=thread` count ≥2; zero `-fsanitize=address`.

- [ ] **Step 8: Clean up generated artifacts, restore a clean no-option generation.**

Run (Bash):
```bash
PM=/d/dev/_shared/tools/premake5
"$PM" gmake2 >/dev/null   # regenerate without any sanitizer flag
rm -f /tmp/AstraTest.make.baseline
git status --porcelain premake5.lua   # only premake5.lua should be modified
```
Expected: `git status` shows ONLY `premake5.lua` modified (the `ide/`, `Makefile`, `*.make` artifacts are git-ignored, so they don't appear). If any non-ignored file changed, investigate before committing.

- [ ] **Step 9: Commit**

```bash
git add premake5.lua
git commit -m "build(premake): add --sanitize=address|thread option for the CI sanitizer lane (option-gated, staticruntime-off, Linux/clang)"
```

---

### Task 2: CI jobs + TSan suppressions scaffold

**Files:**
- Modify: `.github/workflows/ci.yml` (append two jobs after the existing `linux` job, which ends at line 63)
- Create: `ci/tsan-suppressions.txt`

**Interfaces:**
- Consumes: Task 1's `--sanitize=address` / `--sanitize=thread` premake option; the workflow-level `env.PREMAKE_VERSION` (line 7-8, inherited by all jobs).
- Produces: two CI jobs `sanitize-asan-ubsan` and `sanitize-tsan`; the suppressions file the TSan job references.

- [ ] **Step 1: Create `ci/tsan-suppressions.txt`:**

```
# ThreadSanitizer suppressions for Astra's CI TSan lane.
#
# Add an entry ONLY for a triaged, understood-benign race (e.g. a documented
# third-party data race that cannot be fixed here). NEVER add an entry to
# silence a real Astra finding -- fix the code instead.
#
# Format: https://github.com/google/sanitizers/wiki/ThreadSanitizerSuppressions
#   race:SymbolNameSubstring
#   called_from_lib:libname
#
# (empty -- populated only from triage)
```

- [ ] **Step 2: Append the two jobs** to `.github/workflows/ci.yml` (after the `linux` job's last line, `      - name: Test (Debug)` block ending at line 63). Mirror the `linux` job's shape (checkout → install → premake → generate → build → test), clang-only, with the `--sanitize` flag and the runtime `*_OPTIONS`:

```yaml

  sanitize-asan-ubsan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install clang
        run: |
          sudo apt-get update
          sudo apt-get install -y clang
      - name: Download premake
        run: |
          curl -L "https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz" | tar xz
      - name: Generate (ASan+UBSan)
        run: ./premake5 gmake2 --sanitize=address
      - name: Build (Debug, AstraTest)
        run: make config=debug AstraTest -j"$(nproc)" CC=clang CXX=clang++
      - name: Test (ASan+UBSan)
        env:
          ASAN_OPTIONS: halt_on_error=1:detect_leaks=1:abort_on_error=1
          UBSAN_OPTIONS: halt_on_error=1:print_stacktrace=1:abort_on_error=1
        run: ./bin/Debug-linux-x86_64/AstraTest/AstraTest --gtest_brief=1

  sanitize-tsan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install clang
        run: |
          sudo apt-get update
          sudo apt-get install -y clang
      - name: Download premake
        run: |
          curl -L "https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz" | tar xz
      - name: Generate (TSan)
        run: ./premake5 gmake2 --sanitize=thread
      - name: Build (Debug, AstraTest)
        run: make config=debug AstraTest -j"$(nproc)" CC=clang CXX=clang++
      - name: Test (TSan)
        env:
          TSAN_OPTIONS: halt_on_error=1:second_deadlock_stack=1:suppressions=ci/tsan-suppressions.txt
        run: ./bin/Debug-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```

(The `Debug-linux-x86_64` output path matches the existing `linux` job's test path at line 63 — the outputdir is `%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}` = `Debug-linux-x86_64`. Do not change it.)

- [ ] **Step 3: Validate the YAML parses and the jobs are well-formed.**

Run (Bash):
```bash
python -c "import yaml,sys; d=yaml.safe_load(open('.github/workflows/ci.yml')); j=d['jobs']; assert 'sanitize-asan-ubsan' in j and 'sanitize-tsan' in j, 'jobs missing'; print('jobs:', list(j.keys()))"
```
Expected: prints the job list including `sanitize-asan-ubsan` and `sanitize-tsan`; no YAML parse error. (If `python`/`pyyaml` is unavailable, use any YAML linter; the check is that the file parses and both jobs exist. Do NOT skip — a malformed workflow silently doesn't run.)

- [ ] **Step 4: Confirm the referenced suppressions path matches the created file.**

Run (Bash):
```bash
grep -q 'suppressions=ci/tsan-suppressions.txt' .github/workflows/ci.yml && test -f ci/tsan-suppressions.txt && echo OK
```
Expected: `OK` (the TSAN_OPTIONS path and the actual file agree).

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml ci/tsan-suppressions.txt
git commit -m "ci: add ASan+UBSan and TSan clang jobs running AstraTest (consumes premake --sanitize; empty TSan suppressions scaffold)"
```

---

### Finishing — push-to-validate + triage (controller-driven)

This lane's acceptance (spec §5) is a completed Actions run with both sanitizer jobs' outcomes triaged. That requires the branch on `origin` — the controller performs these steps, not a subagent.

- [ ] **Whole-branch sanity read.** `git diff dev..HEAD` (or the recorded base) — confirm ONLY `premake5.lua`, `.github/workflows/ci.yml`, `ci/tsan-suppressions.txt` changed, and the flag sets / `*_OPTIONS` / paths match this plan verbatim. (Two config files + a scaffold — a controller read replaces the opus whole-branch review; no code paths changed.)

- [ ] **Confirm with the user before pushing** — pushing `ci/sanitizer-lane` publishes dev's 264 unpushed commits to `origin` AS A BRANCH (not a merge to `main`/`dev`; the only way Actions can build it). Surface this explicitly and get an explicit go before the push (an outward-facing action). If declined, fall back to the spec's "local recipe, no push" shape (document the `premake5 gmake2 --sanitize=... && make config=debug AstraTest CC=clang CXX=clang++ && ASAN_OPTIONS=... ./AstraTest` recipe in a short `ci/README.md` for a Linux box to run) and stop before merge.

- [ ] **Push + watch.** `git push -u origin ci/sanitizer-lane`, then watch the two sanitizer jobs (`gh run watch` / `gh run list --branch ci/sanitizer-lane`, or the Actions UI). Also confirm the pre-existing `windows-msvc` and `linux` jobs stay green (zero-blast-radius holds in CI, not just locally).

- [ ] **Triage the outcomes (record in the ledger + a short report):**
  - **Both green:** lane validated AND the entire unpushed body of work (perf levers, Commands hardening, enableable components) is certified ASan/UBSan/TSan-clean for the first time. Record it; note the user may now enable required-check branch protection for the two jobs (a repo setting, not a file).
  - **Red:** capture each finding (sanitizer type + the failing test + the stack) as a concrete, reproduced defect. These feed the follow-on correctness work (likely Serialization C5 and/or the Relations parallel-descendant UAF, or an LSan leak-report false-positive — a known first-run noise source; if a leak is genuinely benign, the remedy is an LSan suppression or `detect_leaks=0`, decided in triage, NOT silently). A red run is the lane succeeding on day one — do NOT paper over a real finding.
  - Either way the LANE is correct and mergeable: the CI files are the deliverable; findings are separate work.

- [ ] **FF-merge dev local** (`git checkout dev && git merge --ff-only ci/sanitizer-lane`), delete the branch, and update memory (astra-remediation-roadmap: the sanitizer/macOS-CI-net item — record the lane shipped + the first-run triage result; note gating is a pending repo-settings switch). Do not force-push dev. Leave `ci/sanitizer-lane` on `origin` (harmless; or delete the remote branch after merge per user preference).
