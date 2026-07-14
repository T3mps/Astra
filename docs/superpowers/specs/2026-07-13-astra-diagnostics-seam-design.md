# Astra Diagnostics Seam — Logging + Assert/Verify/Ensure (Design)

> Status: design approved 2026-07-13. Next: implementation plan (`superpowers:writing-plans`).
> Branch: `diagnostics-seam` (off `dev` @ `8d8b9c3`).

## 1. Context & goal

Astra today has exactly one diagnostic primitive: `ASTRA_ASSERT(condition, message)` in
`include/Astra/Core/Base.hpp`, which expands to `assert((condition) && (message))` in Debug
and `((void)0)` otherwise (134 call sites across 23 headers, all the 2-arg `(cond, "literal")`
shape). It has three problems:

1. **No output seam.** On failure it calls `std::assert` → stderr + `abort()`. A host application
   cannot route that anywhere (its own logger, a crash reporter, telemetry). Astra emits no other
   diagnostic output at all (two stray, unused `#include <iostream>` in `Archetype.hpp` and
   `Registry.hpp` are the only traces, plus one line inside a doc comment).
2. **Not testable.** Because failure is a hard `abort()` in Debug, a test cannot verify that a guard
   *fires* without killing the test process. This directly forced the "uniform-graceful" compromise in
   Theme B B1 (see `2026-07-13-theme-b-concurrency-b1-design.md` §2): misuse-asserts had to be dropped
   because an assert-that-aborts and a testable graceful `Result` path are mutually exclusive today.
3. **No recoverable-guard primitive.** Several sites already hand-roll "assert in Debug, recover in
   Release" (`ASTRA_ASSERT(cond); if (!cond) { recover }`) with `ASTRA_UNLIKELY` hints and explicit
   "in Release builds" comments — an open-coded `ensure` with no first-class form and no Release signal.

**Goal:** give Astra a proper, dependency-free **diagnostics seam** — a settable **log sink** and a
settable **assert handler** — plus a three-macro guard family (`ASSERT`/`VERIFY`/`ENSURE`) with correct
per-configuration semantics and a portable debugger break. Users own the policy (hook spdlog / a crash
reporter / telemetry); Astra ships zero third-party dependencies and pays nothing in shipping by default.
This is the "custom assert/verify seam" queued after B1 in the remediation roadmap.

## 2. Constraints (hard)

- **Header-only, C++20.** MSVC (primary) + GCC/Clang (CI: `ubuntu-latest`, GCC 13 / Clang).
- **Exception-free.** The build sets `exceptionhandling "off"` (`premake5.lua`) and `rtti "off"` in
  shipping. No `try`/`catch` anywhere in the seam; nothing may throw. (This is why `std::format`
  and a `try`/`catch` guard are out — see §5.)
- **Zero third-party dependencies.** Users plug their own logger/handler. No `libatomic` link
  dependency either (rules out a 16-byte `std::atomic` for the sink slot — see §4.3).
- **Zero release cost by default.** `ASSERT` compiles out entirely in Release/Dist; log calls below the
  compile-time floor compile out; no atomic loads or `source_location` capture on a compiled-out path.
- **No public API break.** All 134 existing `ASTRA_ASSERT(cond, "msg")` sites must compile and behave
  unchanged. The macro signature is preserved exactly.

## 3. Architecture — two layers, two headers

| Layer | Header | Responsibility |
|---|---|---|
| 1 · Log seam | `include/Astra/Core/Log.hpp` (new) | Settable output sink; `LogRecord`; levels; `ASTRA_LOG_*` macros; double-gate. |
| 2 · Assert seam | `include/Astra/Core/Assert.hpp` (new) | `ASSERT`/`VERIFY`/`ENSURE`; portable debug-break; settable assert handler. Uses layer 1 for its default output. |

`include/Astra/Core/Base.hpp` `#include`s both, so every current includer keeps resolving
`ASTRA_ASSERT` transparently. The existing `ASTRA_ASSERT` definition moves from `Base.hpp` into
`Assert.hpp` (re-exported via the include).

**Why two seams, not one:** logging is "emit a message" (no control flow); an assert failure is a
*policy decision* (break / abort / continue). They compose — the assert handler's *default* routes its
message through the log sink — but they are distinct concerns with distinct signatures.

## 4. Layer 1 — Log seam (`Core/Log.hpp`)

Namespace `Astra` (PascalCase, matching the rest of the library — **not** `ASTRA`).

### 4.1 Types & API

```cpp
namespace Astra
{
    enum class LogLevel : int { Trace=0, Debug=1, Info=2, Warn=3, Error=4, Critical=5, Off=6 };

    struct LogRecord
    {
        LogLevel             level;
        std::string_view     category;   // compile-time ASTRA_LOG_CATEGORY (default "Astra")
        std::string_view     message;    // PLAIN string (see §5) — points into a string literal
        std::source_location location;
    };

    // Raw fn-ptr + void*: no std::function (no alloc/vtable), survives a DLL/.so boundary.
    // Contract: may be called from any thread (serialize internally); must NOT throw
    // (noexcept); must NOT call back into Astra.
    using LogSink = void (*)(const LogRecord& record, void* user) noexcept;

    void     SetLogSink(LogSink sink, void* user = nullptr) noexcept; // nullptr silences
    void     SetLogLevel(LogLevel minimum) noexcept;                  // runtime floor
    LogLevel GetLogLevel() noexcept;

    std::string_view LevelName(LogLevel) noexcept;                    // "trace".."critical"

    // Provided but NOT installed by default. Writes "[level] file:line - message" to stderr.
    void StderrSink(const LogRecord& record, void* user) noexcept;
}
```

Default sink is **none** (no-op): with no sink installed, every log call and the default assert
output are silent. Zero dependency, zero output until the host opts in.

### 4.2 Macros & the double-gate

```
ASTRA_LOG_TRACE("msg")   ASTRA_LOG_DEBUG(...)   ASTRA_LOG_INFO(...)
ASTRA_LOG_WARN(...)      ASTRA_LOG_ERROR(...)   ASTRA_LOG_CRITICAL(...)
```

Two independent gates (validated against **both** spdlog `SPDLOG_ACTIVE_LEVEL` and Abseil
`ABSL_MIN_LOG_LEVEL`):

1. **Compile-time floor** `ASTRA_ACTIVE_LEVEL` (default: `Trace` in debug, `Info` when `NDEBUG`).
   A macro below the floor expands to a no-op that still token-references its argument
   (`(void)sizeof(...)`) so there are no unused-variable warnings and no code.
2. **Runtime gate** `if (level >= GetLogLevel())` (relaxed atomic load), marked `[[unlikely]]`.

Message is emitted only when *both* gates pass and a sink is installed.

`ASTRA_LOG_CATEGORY` is a compile-time string macro (default `"Astra"`), redefinable per-TU. This is
the *only* category mechanism — no runtime category system (YAGNI).

### 4.3 Threading model for the sink

The sink is stored as **two 8-byte atomics** — `std::atomic<LogSink>` (function pointer) and
`std::atomic<void*>` (user data) — both lock-free on every target. This deliberately avoids a single
16-byte `std::atomic<{fn,user}>`, which on GCC/Clang can require linking `libatomic` (a dependency)
and may not be lock-free. The sink is a **set-once-before-work** seam: the documented contract is
"install the sink before handing Astra any work / spawning threads," so the theoretical window where a
concurrent log reads a new function pointer with a stale (opaque) `user` is a non-issue in practice.
Loads use acquire, the store in `SetLogSink` uses release.

### 4.4 Message policy — plain strings only (no formatting)

Astra renders **no** formatted messages. `LogRecord::message` and every macro argument is a plain
`const char*` / `std::string_view` (a string literal, static storage). Rationale:

- All 134 existing messages are already static strings; Astra has never needed runtime values in them.
- Under `-fno-exceptions`, `std::format` (which can throw `bad_alloc`, and heap-allocates a
  `std::string` per call) is unsafe/undesirable, and a `try`/`catch` guard won't even compile.
- The logging library the host plugs in (spdlog, etc.) owns *line* formatting (timestamps, level tags,
  colours, routing) — it receives the finished `LogRecord`. Astra never needs to build a message from
  typed values, because those values never cross the sink boundary.

If a future call site genuinely needs `expected 16, got 24`, formatting is added **then**, scoped to
that need (a printf-style `vsnprintf`-into-stack-buffer overload is the exception-free option) — not
speculatively now.

## 5. Layer 2 — Assert / Verify / Ensure (`Core/Assert.hpp`)

### 5.1 Portable debugger break

```cpp
#if defined(ASTRA_COMPILER_MSVC)
    #define ASTRA_DEBUG_BREAK() __debugbreak()
#elif ASTRA_HAS_BUILTIN(__builtin_debugtrap)     // Clang
    #define ASTRA_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)   // GCC x86
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("int3")
#elif defined(__aarch64__)
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("brk #0xF000")
#elif defined(__arm__)
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("bkpt #0")
#else
    #include <csignal>
    #define ASTRA_DEBUG_BREAK() std::raise(SIGTRAP)
#endif
```

**Not `__builtin_trap()`** — that is SIGILL/`ud2`, non-continuable, and the compiler treats code after
it as unreachable; GCC has no `__builtin_debugtrap` equivalent (open GCC bug #99299). We want a
*continuable* breakpoint (inspect, then continue in the debugger). Reuses `ASTRA_HAS_BUILTIN` (already
in `Base.hpp`).

### 5.2 Settable assert handler (decision-returning)

```cpp
namespace Astra
{
    enum class AssertAction { Break, Continue };

    struct AssertContext
    {
        const char*          expression;  // stringized condition
        const char*          message;     // plain string
        std::source_location location;
    };

    using AssertHandler = AssertAction (*)(const AssertContext& ctx, void* user) noexcept;

    void SetAssertHandler(AssertHandler handler, void* user = nullptr) noexcept;
}
```

- The handler **returns a decision** (`Break` / `Continue`). This is what makes assert-firing
  **testable**: a test installs a handler that records the failure and returns `Continue`, so the
  process does not abort. (Abseil's real-world `RegisterAbortHook` is *void*-returning and cannot
  suppress the abort — deliberately insufficient for this use case; we go further on purpose.)
- **Default handler:** emit the record through the log sink at `Critical`, then return `Break`. (Astra
  cannot flush a *user's* sink — that is the sink's job; the provided `StderrSink` `fflush`es stderr,
  and a buffering user sink should flush on `Critical` since a following `abort()` won't.)
- **How the macro layer acts on the returned decision** (this is where fatal vs. non-fatal lives, *not*
  in the handler):
  > **Superseded 2026-07-14** — the break below is described as ungated. It shipped
  > **debugger-gated** instead (both paths break only under `IsDebuggerAttached()`, and the
  > fatal path then always `abort()`s regardless). See "Amendments (2026-07-14)" §1.
  - `ASSERT` / `VERIFY` (fatal): `Break` → `ASTRA_DEBUG_BREAK()` then `std::abort()`; `Continue` → proceed.
  - `ENSURE` (non-fatal): `Break` → `ASTRA_DEBUG_BREAK()` then **continue** (never `abort()`); `Continue`
    → continue. Either way `ENSURE` then returns `cond`. So a `Break` decision drops you into the
    debugger on an ensure failure while debugging, without ever terminating.
- The handler type is **never** `[[noreturn]]` — `ENSURE` and test overrides must be able to return.
  Only the fatal `Break`→`abort` path is effectively no-return.
- Same fn-ptr + `void*` shape as the log sink; set-once-before-work; two 8-byte atomics.

### 5.3 The three macros & the configuration matrix

| Macro | Debug | Release / Dist (default) | + `ASTRA_ENABLE_ASSERTS` | Fatal? | Yields `cond`? |
|---|---|---|---|---|---|
| `ASTRA_ASSERT(c,"m")` | eval `c`; fail → handler | **compiled out** (`c` not evaluated) | eval `c`; fail → handler | yes (`Break`→abort) | no (statement) |
| `ASTRA_VERIFY(c,"m")` | eval `c`; fail → handler | eval `c`, **discard result** (no handler) | eval `c`; fail → handler | yes | **yes** |
| `ASTRA_ENSURE(c,"m")` | eval `c`; fail → handler (**non-fatal, continue**) | **same** — eval + handler + continue | same | **no** | **yes** |

- **`ASSERT`** = invariant / programmer error. Zero shipping cost; identical to today's behavior, so all
  134 sites keep their exact semantics. Statement form (can stay `do{}while(0)`-shaped or the
  expression form; it does not need to yield a value).
- **`VERIFY`** = a check whose condition has a **side effect that must run in shipping**. In
  Release/Dist the condition is still evaluated; only the failure *handling* is dropped. Canonical
  Unreal `verify` semantics (confirmed against UE docs).
- **`ENSURE`** = a **recoverable** guard for the *Condition* class (a check a *correct* caller can still
  trip — bad input, capacity limit, a race that lost). Evaluates and reports in **all** configurations,
  is **non-fatal** (a `Break` decision breaks into the debugger but then continues; it never `abort()`s),
  and **returns `cond`** so the caller branches: `if (!ASTRA_ENSURE(ptr, "null node")) return;`. Fires
  **once per call-site**
  (function-local `static bool`, documented best-effort under threads); `ASTRA_ENSURE_ALWAYS(c,"m")`
  is the no-dedup variant.
- **`ASTRA_ENABLE_ASSERTS`** — opt-in build define (default off; polarity matches UE
  `USE_CHECKS_IN_SHIPPING`) that forces `ASSERT`/`VERIFY` handling on in Release/Dist for a
  "checked release" or shipping-crash-handling configuration.

### 5.4 Macro hygiene

- `VERIFY`/`ENSURE` are **single-expression** macros (`(cond) || (Astra::detail::OnAssertFail(...),
  false)` shape) so they yield the bool, single-evaluate `cond`, and are safe in `if`/comma contexts.
  `ASSERT` is statement form.
- The 2-explicit-parameter `(condition, message)` form avoids the classic comma-operator "message
  smuggling" footgun (WG21 P2264).
- **Compiled-out `ASSERT` uses `(void)sizeof(bool(condition))`**, not bare `((void)0)`, so the
  condition expression stays compiler-checked in every configuration at zero runtime/codegen cost.
- **No automatic `__builtin_unreachable()` / `__assume(0)`** in the compiled-out branch. Auto-inserting
  an unreachable hint would recreate the exact "assert masked a recoverable condition" bug class the
  Theme-A release-safety sweep fixed (Invariant vs Condition). Unreachable hints remain a manual,
  per-call-site technique reserved for guards explicitly classified as invariants.

## 6. Code changes to existing headers

1. **No sweep of the 134 asserts** — they keep compiling and behaving identically.
2. **Convert the ~6 hand-rolled "assert-then-recover" sites to `ASTRA_ENSURE`** (they already recover in
   Release; `ENSURE` makes it first-class *and* adds the missing Release signal):
   - `Archetype.hpp:565` — `chunkIndex < m_chunks.size()` ("Skip invalid chunk indices in Release builds").
   - `ArchetypeChunkPool.hpp:810` — `blockUsed == 0` before releasing a block (race-recoverable).
     > **Superseded 2026-07-14** — this site was reclassified as control flow, not a guard: the
     > re-check is an *expected* outcome of the lock-free path, not a violated invariant. It is a
     > plain branch now, with no diagnostic. See "Amendments (2026-07-14)" §3.
   - `FlatSet.hpp:536` — `insertIdx < m_capacity` (Emplace slot → `{end(), false}`).
   - `FlatMap.hpp:570` — same.
   - `Bitmap.hpp:33`, `Bitmap.hpp:44` — `index < Bits` (component-ID overflow).
     > **Superseded 2026-07-14** — Bitmap stays `ASTRA_ASSERT` (fatal) and was excluded from this
     > conversion: "recovering" here silently drops a bit from an archetype mask, so a query for
     > that component would silently return nothing. See "Amendments (2026-07-14)" §2.
   Each becomes `if (!ASTRA_ENSURE(cond, "…")) { existing recovery }`, deleting the now-redundant
   separate `ASTRA_ASSERT`.
3. **Upgrade the assert-only `SystemScheduler.hpp:173`** `executor != nullptr` (currently asserts then
   dereferences — UB on a null executor in Release) to `if (!ASTRA_ENSURE(executor != nullptr, "…"))
   return;`.
4. **Delete the two unused `#include <iostream>`** in `Archetype.hpp` and `Registry.hpp` (dead includes,
   header-only compile-time cost).

The `ENSURE` conversions are validation + immediate value, **not** a forced migration; the remaining
~128 pure-invariant asserts stay `ASTRA_ASSERT`.

## 7. Testing

First coverage of Astra's diagnostics layer. New `tests/Core/LogTest.cpp` and `tests/Core/AssertTest.cpp`
(GoogleTest), driven through the seams (no death tests — the handler override *is* the mechanism):

- **Log:** sink receives the expected `LogRecord` (level, category, message, file/line); runtime-level
  gating filters correctly; `SetLogSink(nullptr)` silences; `StderrSink` smoke test.
- **Assert handler override:** install a handler that records `AssertContext` and returns `Continue`;
  verify `ASTRA_ASSERT`/`ASTRA_VERIFY` invoke it with the right expression/message/location and do **not**
  abort; verify `Break` is the default decision.
- **Matrix behaviors:** `VERIFY` evaluates its condition and yields it (side-effect-preserving);
  `ENSURE` continues after failure, returns `cond`, and fires **once** per call-site (`ENSURE_ALWAYS`
  every time).
- Tests run in all three configs; where a behavior differs by config (`ASSERT` compiled out), assert on
  the config-appropriate expectation.

Test files are new → regenerate premake once (`D:\dev\_shared\tools\premake5 vs2022`); never `git add
ide/`, `Astra.sln`, `Makefile`, `*.make`. Baseline on `dev` is 563 tests.

## 8. Resolved decisions (were open; recorded here)

- **`std::source_location`** (not `__FILE__`/`__LINE__`) for the clean API. `function_name()` can bloat
  template-heavy headers, but the heavy paths compile out in Dist; revisit only if Dist size measures
  badly. (Could later make `function_name` capture opt-in.)
- **Two 8-byte atomics** for each seam's `{fn, user}` (not one 16-byte atomic) — avoids `libatomic`.
- **`Log.hpp` + `Assert.hpp` split**, both included by `Base.hpp`.
- **Convert ~6 `ENSURE` sites now**, not a full sweep.
- **Plain-string messages**, no formatting mechanism in core.

## 9. Non-goals (explicitly out of scope)

- Message **formatting** (`std::format`/printf) — plain strings only (§4.4).
- A logging **framework** — no categories beyond the compile-time `ASTRA_LOG_CATEGORY`, no async, no
  sink chains / multi-subscriber, no built-in file/rotating sinks (host's job).
- Converting the other ~128 invariant asserts.
- `std::breakpoint()` (C++26) — not uniformly available yet; keep the macro.

## 10. Forward-compat

- **Theme C (`Registry::Load` trust boundary)** is the big downstream consumer: it will add many
  "validate crafted input → recover gracefully" guards, for which `ASTRA_ENSURE` is the right primitive
  (`Archetype.hpp:565`'s chunk-index check is already one, hand-rolled).
- Permanently retires the B1 "assert-abort-in-Debug vs. testable graceful path" tension — `ENSURE` *is*
  that pattern done right, and the handler override makes any guard testable.

## Appendix — research citations

- Unreal `check`/`verify`/`ensure` semantics — dev.epicgames.com "Asserts in Unreal Engine".
- Assert handler precedent (void, cannot suppress abort) — Abseil `raw_logging.h` `RegisterAbortHook`;
  heavier virtual `LogSink`/`AddLogSink` for multi-subscriber.
- Debug-break — MSVC `__debugbreak` docs; GCC "Other Builtins" (`__builtin_trap`); scottt/debugbreak;
  GCC bug #99299 (no recoverable trap).
- Fn-ptr + `void*` sink pattern — miniaudio (`ma_log_callback_proc`), sokol (`slog_func`).
- Double-gate — spdlog `SPDLOG_ACTIVE_LEVEL`, Abseil `ABSL_MIN_LOG_LEVEL` (+ stripping caveat).
- Macro/message footgun — WG21 P2264 "Make `assert()` macro user friendly".
- Namespace-collision precedent — EnTT #63 (`ensure`→`assure` due to UE's global `ensure`).

## Amendments (2026-07-14)

Recorded during the whole-branch final review (1 Critical, 7 Important) after implementation.
This section documents where the shipped code diverged from this spec, and why — the sections
above are left in place, marked superseded in place, because the history is the point: Theme C
reads this document, and "here's what we tried and why it changed" is more useful than a silently
rewritten spec.

1. **Both the fatal *and* recoverable breaks are gated on `IsDebuggerAttached()`; the fatal path
   then always `abort()`s.** §5.2 as written describes an *ungated* `ASTRA_DEBUG_BREAK()` on
   `Break`. That is unsafe for an unattended process: a bare `int3`/`__debugbreak()` with no
   debugger attached raises an unhandled `EXCEPTION_BREAKPOINT` and kills the process right there
   — it never reaches the `std::abort()` the matrix promises, so there is no CRT abort report, no
   `SIGABRT`, and a confusing exit code for anything watching the process (a test runner, a CI
   job, a crash-reporting wrapper). Gating the break on an attached debugger (mirroring Unreal's
   `UE_DEBUG_BREAK` / `IsDebuggerPresent`) means: under a debugger you land in it and can inspect;
   without one, `ASSERT`/`VERIFY` always still reach `abort()`, and `ENSURE` never halts at all.
   Implemented in `Assert.hpp`'s `FailFatal`/`FailEnsure` via `detail::IsDebuggerAttached()`.

2. **`Bitmap` stays fatal (`ASTRA_ASSERT`) and is excluded from the `ENSURE` sweep.** §6 item 2
   listed `Bitmap.hpp:33`/`:44` as ENSURE conversions. `ENSURE`'s whole value proposition is
   "recover and keep going" — but here, recovering means silently dropping a bit from an
   archetype's component mask, which means every future query for that component on that
   archetype silently returns nothing. A wrong answer with no crash is strictly worse than halting
   immediately at the corruption site. This is the exact shape (`ASSERT(cond); if (cond) { work
   }`) the `ENSURE` primitive exists to remove elsewhere, which is precisely why it needed calling
   out here instead of converting on autopilot — both `Bitmap::Set` and `Bitmap::Reset` now carry
   an inline comment saying so, pointing back here.

3. **`ArchetypeChunkPool`'s block-release re-check (`:810`) is a plain branch, not a guard.** §6
   item 2 also listed this site as an ENSURE conversion (it started life as an `ASTRA_ASSERT`
   before that). Neither classification was right: the code immediately below the check already
   documented *"a chunk was acquired after our initial check"* — an **anticipated, by-design**
   branch of the lock-free acquire/release algorithm under concurrent access, not a violated
   invariant. As an `ASSERT` it aborted Debug builds on a perfectly legal race. As an `ENSURE` it
   reported healthy, expected operation to every host sink at `LogLevel::Critical` on every such
   race — the one place in the sweep where the seam would have cried wolf, teaching exactly the
   wrong lesson about what `ENSURE` means right as Theme C starts adopting it. It is now a plain
   `if (blockUsed != 0) { continue; }` with no diagnostic at all, which is what it always should
   have been.

4. **The default assert handler falls back to `StderrSink` when no sink is installed.** §5.2's
   "Default handler: emit the record through the log sink at `Critical`" undersold what "emit"
   meant: the as-implemented first pass routed only through `detail::Emit`, which is a **no-op**
   with no sink installed — and no sink is installed by default. The practical effect: a stock
   build that never calls `SetLogSink` got total silence on every failing guard, immediately
   followed by `abort()`. That is a regression against `plain assert()`'s behavior (file, line,
   expression, message to stderr before dying) and against this branch's own "134 sites behave
   unchanged" constraint. `DefaultAssertHandler` now checks whether a sink is installed: if so, it
   routes through it (a host may forward to a crash reporter); if not, it calls the provided
   `StderrSink` directly, so a fatal condition is never silent regardless of host configuration.
