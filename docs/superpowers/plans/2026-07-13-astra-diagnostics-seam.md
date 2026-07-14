# Astra Diagnostics Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dependency-free diagnostics seam to Astra — a settable log sink and a settable, decision-returning assert handler — plus an `ASSERT`/`VERIFY`/`ENSURE` guard family, with zero release cost by default and no break to the 134 existing `ASTRA_ASSERT` sites.

**Architecture:** Two new header-only layers. `Core/Log.hpp` owns the output sink (raw `fn-ptr + void*`, `LogRecord`, 7 levels, compile-time floor + runtime double-gate, no-op default). `Core/Assert.hpp` owns the guard macros + a settable handler that returns `Break`/`Continue` (so asserts are testable), routing its default output through the log sink. `Core/Base.hpp` includes both so existing includers keep resolving `ASTRA_ASSERT`.

**Tech Stack:** Header-only C++20, MSVC (primary) + GCC/Clang, GoogleTest, premake5-generated `Astra.sln`.

**Design spec:** `docs/superpowers/specs/2026-07-13-astra-diagnostics-seam-design.md` (read it first).

## Global Constraints

- **Exception-free & RTTI-off in shipping.** No `try`/`catch`, nothing may throw. (`premake5.lua` sets `exceptionhandling "off"`.)
- **Zero third-party dependencies; no `libatomic` link dependency** — each seam's `{fn, user}` is stored as **two separate 8-byte atomics**, never one 16-byte `std::atomic`.
- **Zero release cost by default.** `ASTRA_ASSERT` compiles out in Release/Dist (condition not evaluated); log calls below `ASTRA_ACTIVE_LEVEL` compile out. Only `ASTRA_ENABLE_ASSERTS` (opt-in build define, default off) re-enables `ASSERT`/`VERIFY` handling in Release/Dist.
- **No public API break.** All 134 existing `ASTRA_ASSERT(cond, "msg")` sites compile and behave unchanged.
- **Namespace `Astra`** (PascalCase), not `ASTRA`. Messages are **plain strings** (`const char*`/`string_view`) — no formatting.
- **Debug-break must be *continuable*** — never `__builtin_trap()`.
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` (also `Release` and `Dist` at the gate). `-t:AstraTest` does NOT work — build the whole solution.
- **Test exe:** `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe` (GoogleTest).
- **New test file → regenerate premake once:** `D:\dev\_shared\tools\premake5 vs2022` from repo root. **Never `git add ide/`, `Astra.sln`, `Makefile`, `*.make`** (gitignored). One gtest macro style per file (`TEST`, not `TEST_F`). New `.hpp` files need no regen (header-only).
- **Baseline:** 563 tests on `dev`. IDE clang diagnostics for this project are misconfigured false positives (expects Clang 20, "no gtest", "unknown type name 'concept'") — IGNORE; judge only by the MSVC build.
- **Branch:** `diagnostics-seam` (already created, off `dev`). Do not push.

---

## File Structure

- `include/Astra/Core/Log.hpp` — **new.** Log sink seam: `LogLevel`, `LogRecord`, `LogSink`, `SetLogSink`/`SetLogLevel`/`GetLogLevel`, `LevelName`, `StderrSink`, `detail::Emit`, the `ASTRA_LOG_*` macros + double-gate (Tasks 1–2).
- `include/Astra/Core/Assert.hpp` — **new.** `ASTRA_DEBUG_BREAK`; `AssertAction`/`AssertContext`/`AssertHandler`/`SetAssertHandler`; `detail::ReportAssertFailure`/`DefaultAssertHandler`/`FailFatal`/`FailEnsure`; `ASTRA_ASSERT`/`ASTRA_VERIFY`/`ASTRA_ENSURE`/`ASTRA_ENSURE_ALWAYS` (Tasks 3–5).
- `include/Astra/Core/Base.hpp` — **modify.** Remove the old `ASTRA_ASSERT` (`:45-52`); add `#include "Log.hpp"` + `#include "Assert.hpp"` at the **bottom** (Task 4).
- `include/Astra/Archetype/Archetype.hpp`, `ArchetypeChunkPool.hpp`, `Container/FlatSet.hpp`, `Container/FlatMap.hpp`, `Container/Bitmap.hpp`, `System/SystemScheduler.hpp`, `Registry/Registry.hpp` — **modify.** ENSURE conversions + `<iostream>` removals (Task 6).
- `tests/Core/LogTest.cpp` — **new** (Tasks 1–2).
- `tests/Core/AssertTest.cpp` — **new** (Tasks 3–5).

---

## Task 1: Log seam core — types, sink storage, API, `StderrSink`

**Files:**
- Create: `include/Astra/Core/Log.hpp`
- Test: `tests/Core/LogTest.cpp` (**create**)

**Interfaces:**
- Produces: `Astra::LogLevel` (`Trace=0..Off=6`); `Astra::LogRecord {LogLevel level; std::string_view category; std::string_view message; std::source_location location;}`; `using LogSink = void(*)(const LogRecord&, void*) noexcept;`; `void SetLogSink(LogSink, void* = nullptr) noexcept`; `void SetLogLevel(LogLevel) noexcept`; `LogLevel GetLogLevel() noexcept`; `std::string_view LevelName(LogLevel) noexcept`; `void StderrSink(const LogRecord&, void*) noexcept`; `void Astra::detail::Emit(LogLevel, std::string_view category, const std::source_location&, std::string_view message) noexcept`.

- [ ] **Step 1: Create the failing test**

Create `tests/Core/LogTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Core/Log.hpp>
#include <string>

namespace
{
    struct Capture { int count = 0; Astra::LogLevel level{}; std::string message; std::string category; unsigned line = 0; };
    Capture g_cap;

    void CapturingSink(const Astra::LogRecord& r, void* user) noexcept
    {
        auto* c = static_cast<Capture*>(user);
        c->count++;
        c->level = r.level;
        c->message = std::string(r.message);
        c->category = std::string(r.category);
        c->line = r.location.line();
    }
}

TEST(Log, EmitDeliversRecordToInstalledSink)
{
    Capture cap;
    Astra::SetLogSink(&CapturingSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Trace);
    Astra::detail::Emit(Astra::LogLevel::Warn, "TestCat", std::source_location::current(), "hello");
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.level, Astra::LogLevel::Warn);
    EXPECT_EQ(cap.message, "hello");
    EXPECT_EQ(cap.category, "TestCat");
    Astra::SetLogSink(nullptr);  // restore no-op for other tests
}

TEST(Log, NullSinkIsSilent)
{
    Astra::SetLogSink(nullptr);
    // Must not crash / must be a no-op:
    Astra::detail::Emit(Astra::LogLevel::Error, "X", std::source_location::current(), "ignored");
    SUCCEED();
}

TEST(Log, LevelNameMapsAllLevels)
{
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Trace), "trace");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Critical), "critical");
}
```

- [ ] **Step 2: Regenerate premake, build Debug, confirm it fails**

Run (repo root): `D:\dev\_shared\tools\premake5 vs2022`
Then build Debug. Expected: compile error — `Astra/Core/Log.hpp` not found.

- [ ] **Step 3: Create `include/Astra/Core/Log.hpp`**

```cpp
#pragma once

#include <atomic>
#include <cstdio>
#include <source_location>
#include <string_view>

// Integer level constants usable by the preprocessor (for the compile-time floor).
#define ASTRA_LEVEL_TRACE    0
#define ASTRA_LEVEL_DEBUG    1
#define ASTRA_LEVEL_INFO     2
#define ASTRA_LEVEL_WARN     3
#define ASTRA_LEVEL_ERROR    4
#define ASTRA_LEVEL_CRITICAL 5
#define ASTRA_LEVEL_OFF      6

#ifndef ASTRA_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define ASTRA_ACTIVE_LEVEL ASTRA_LEVEL_INFO
#  else
#    define ASTRA_ACTIVE_LEVEL ASTRA_LEVEL_TRACE
#  endif
#endif

#ifndef ASTRA_LOG_CATEGORY
#  define ASTRA_LOG_CATEGORY "Astra"
#endif

namespace Astra
{
    enum class LogLevel : int
    {
        Trace = ASTRA_LEVEL_TRACE, Debug = ASTRA_LEVEL_DEBUG, Info = ASTRA_LEVEL_INFO,
        Warn = ASTRA_LEVEL_WARN, Error = ASTRA_LEVEL_ERROR, Critical = ASTRA_LEVEL_CRITICAL,
        Off = ASTRA_LEVEL_OFF,
    };

    struct LogRecord
    {
        LogLevel             level;
        std::string_view     category;   // compile-time ASTRA_LOG_CATEGORY
        std::string_view     message;    // plain string (points into a string literal)
        std::source_location location;
    };

    // Raw fn-ptr + void*: no std::function (no alloc/vtable), survives a DLL/.so boundary.
    // Contract: may be called from any thread (serialize internally); must NOT throw; must
    // NOT call back into Astra.
    using LogSink = void (*)(const LogRecord& record, void* user) noexcept;

    [[nodiscard]] constexpr std::string_view LevelName(LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Trace:    return "trace";
            case LogLevel::Debug:    return "debug";
            case LogLevel::Info:     return "info";
            case LogLevel::Warn:     return "warn";
            case LogLevel::Error:    return "error";
            case LogLevel::Critical: return "critical";
            case LogLevel::Off:      return "off";
        }
        return "unknown";
    }

    namespace detail
    {
        // Two 8-byte atomics (NOT one 16-byte atomic) so no libatomic dependency and always
        // lock-free. Set-once-before-work: install the sink before handing Astra work.
        inline std::atomic<LogSink>  g_logSink{nullptr};
        inline std::atomic<void*>    g_logUser{nullptr};
        inline std::atomic<LogLevel> g_logLevel{LogLevel::Info};

        inline void Emit(LogLevel level, std::string_view category,
                         const std::source_location& location, std::string_view message) noexcept
        {
            const LogSink sink = g_logSink.load(std::memory_order_acquire);
            if (sink == nullptr) return;
            void* user = g_logUser.load(std::memory_order_acquire);
            sink(LogRecord{level, category, message, location}, user);
        }
    }

    inline void SetLogSink(LogSink sink, void* user = nullptr) noexcept
    {
        detail::g_logUser.store(user, std::memory_order_release);
        detail::g_logSink.store(sink, std::memory_order_release);
    }

    inline void SetLogLevel(LogLevel minimum) noexcept
    {
        detail::g_logLevel.store(minimum, std::memory_order_relaxed);
    }

    [[nodiscard]] inline LogLevel GetLogLevel() noexcept
    {
        return detail::g_logLevel.load(std::memory_order_relaxed);
    }

    // Convenience sink (NOT installed by default): "[level] file:line - message" to stderr.
    inline void StderrSink(const LogRecord& record, void* /*user*/) noexcept
    {
        const std::string_view name = LevelName(record.level);
        std::fprintf(stderr, "[%.*s] %s:%u - %.*s\n",
                     static_cast<int>(name.size()), name.data(),
                     record.location.file_name(), record.location.line(),
                     static_cast<int>(record.message.size()), record.message.data());
        std::fflush(stderr);
    }
}
```

- [ ] **Step 4: Build Debug and run the Task 1 tests**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Log.*`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Core/Log.hpp tests/Core/LogTest.cpp
git commit -m "feat(core): log sink seam — LogRecord, settable sink, levels (Log.hpp)"
```

---

## Task 2: Log macros + double-gate

**Files:**
- Modify: `include/Astra/Core/Log.hpp` (append the macros at the bottom, after `namespace Astra`)
- Test: `tests/Core/LogTest.cpp` (append)

**Interfaces:**
- Consumes: `Astra::detail::Emit`, `Astra::GetLogLevel` (Task 1); `ASTRA_ACTIVE_LEVEL`, `ASTRA_LOG_CATEGORY`, `ASTRA_LEVEL_*` (Task 1).
- Produces: `ASTRA_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL(msg)` — route `msg` (a plain string) to the sink when its level passes both the compile-time floor and the runtime level.

- [ ] **Step 1: Write the failing tests (append)**

```cpp
// ---- Task 2: log macros + gating -------------------------------------------

TEST(Log, MacroRoutesToSinkAtOrAboveRuntimeLevel)
{
    Capture cap;
    Astra::SetLogSink(&CapturingSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Warn);

    ASTRA_LOG_INFO("below threshold");   // Info < Warn: filtered at runtime
    EXPECT_EQ(cap.count, 0);

    ASTRA_LOG_ERROR("at or above");      // Error >= Warn: delivered
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.level, Astra::LogLevel::Error);
    EXPECT_EQ(cap.message, "at or above");
    EXPECT_EQ(cap.category, "Astra");    // default ASTRA_LOG_CATEGORY

    Astra::SetLogSink(nullptr);
}

TEST(Log, MacroCapturesCallSiteLine)
{
    Capture cap;
    Astra::SetLogSink(&CapturingSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Trace);
    const unsigned expected = __LINE__ + 1;
    ASTRA_LOG_WARN("here");
    EXPECT_EQ(cap.line, expected);
    Astra::SetLogSink(nullptr);
}
```

- [ ] **Step 2: Build Debug and confirm failure**

Expected: compile error — `ASTRA_LOG_INFO`/`ASTRA_LOG_ERROR`/`ASTRA_LOG_WARN` undefined.

- [ ] **Step 3: Append the macros to `Log.hpp`** (after the closing `}` of `namespace Astra`)

```cpp
// =============================================================================
// Logging macros. Two gates: the per-macro #if strips calls below the
// compile-time floor (ASTRA_ACTIVE_LEVEL); the runtime `if` filters against the
// installed SetLogLevel. `msg` is a plain string (no formatting).
// =============================================================================

#define ASTRA_DETAIL_LOG(level_, msg)                                              \
    do {                                                                           \
        if ((level_) >= ::Astra::GetLogLevel()) [[unlikely]] {                     \
            ::Astra::detail::Emit((level_), ASTRA_LOG_CATEGORY,                     \
                                  std::source_location::current(), (msg));         \
        }                                                                          \
    } while (0)

// Disabled form: reference `msg` in an unevaluated context so there is no code
// and no unused-variable warning.
#define ASTRA_DETAIL_LOG_DISABLED(msg) do { (void)sizeof(msg); } while (0)

#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_TRACE
#  define ASTRA_LOG_TRACE(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Trace, msg)
#else
#  define ASTRA_LOG_TRACE(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_DEBUG
#  define ASTRA_LOG_DEBUG(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Debug, msg)
#else
#  define ASTRA_LOG_DEBUG(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_INFO
#  define ASTRA_LOG_INFO(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Info, msg)
#else
#  define ASTRA_LOG_INFO(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_WARN
#  define ASTRA_LOG_WARN(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Warn, msg)
#else
#  define ASTRA_LOG_WARN(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_ERROR
#  define ASTRA_LOG_ERROR(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Error, msg)
#else
#  define ASTRA_LOG_ERROR(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
#if ASTRA_ACTIVE_LEVEL <= ASTRA_LEVEL_CRITICAL
#  define ASTRA_LOG_CRITICAL(msg) ASTRA_DETAIL_LOG(::Astra::LogLevel::Critical, msg)
#else
#  define ASTRA_LOG_CRITICAL(msg) ASTRA_DETAIL_LOG_DISABLED(msg)
#endif
```

(`LogLevel`'s `>=` compares two same-type scoped-enum values — well-formed in C++20.)

- [ ] **Step 4: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Log.*`
Expected: PASS (all 5 Log tests).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Core/Log.hpp tests/Core/LogTest.cpp
git commit -m "feat(core): ASTRA_LOG_* macros with compile-time + runtime double-gate"
```

---

## Task 3: Assert seam core — debug-break + handler seam + dispatch helpers

**Files:**
- Create: `include/Astra/Core/Assert.hpp`
- Test: `tests/Core/AssertTest.cpp` (**create**)

**Interfaces:**
- Consumes: `Astra::detail::Emit`, `Astra::LogLevel` (Task 1); `ASTRA_HAS_BUILTIN`, `ASTRA_COMPILER_MSVC` (`Base.hpp`/`Platform.hpp`).
- Produces: `ASTRA_DEBUG_BREAK()`; `Astra::AssertAction {Break, Continue}`; `Astra::AssertContext {const char* expression; const char* message; std::source_location location;}`; `using AssertHandler = AssertAction(*)(const AssertContext&, void*) noexcept;`; `void SetAssertHandler(AssertHandler, void* = nullptr) noexcept`; `AssertAction Astra::detail::ReportAssertFailure(const AssertContext&) noexcept`; `bool Astra::detail::FailFatal(const char*, const char*, const std::source_location&) noexcept`; `bool Astra::detail::FailEnsure(const char*, const char*, const std::source_location&) noexcept`.

- [ ] **Step 1: Create the failing test**

Create `tests/Core/AssertTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Astra/Core/Assert.hpp>
#include <string>

namespace
{
    struct AssertCapture { int count = 0; std::string expr; std::string message; };
    AssertCapture g_ac;

    // Records and CONTINUES (so the process does not abort during tests).
    Astra::AssertAction RecordingHandler(const Astra::AssertContext& ctx, void* user) noexcept
    {
        auto* c = static_cast<AssertCapture*>(user);
        c->count++;
        c->expr = ctx.expression ? ctx.expression : "";
        c->message = ctx.message ? ctx.message : "";
        return Astra::AssertAction::Continue;
    }
}

TEST(Assert, HandlerReceivesContextAndItsDecisionIsReturned)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);

    const auto action = Astra::detail::ReportAssertFailure(
        Astra::AssertContext{"x < y", "bad bounds", std::source_location::current()});

    EXPECT_EQ(action, Astra::AssertAction::Continue);
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.expr, "x < y");
    EXPECT_EQ(cap.message, "bad bounds");

    Astra::SetAssertHandler(nullptr);  // restore default for other tests
}

TEST(Assert, FailEnsureReportsAndReturnsFalseWithoutAborting)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);
    const bool r = Astra::detail::FailEnsure("p != nullptr", "null", std::source_location::current());
    EXPECT_FALSE(r);
    EXPECT_EQ(cap.count, 1);
    Astra::SetAssertHandler(nullptr);
}
```

- [ ] **Step 2: Regenerate premake, build Debug, confirm it fails**

Run: `D:\dev\_shared\tools\premake5 vs2022`, then build Debug.
Expected: compile error — `Astra/Core/Assert.hpp` not found.

- [ ] **Step 3: Create `include/Astra/Core/Assert.hpp`**

```cpp
#pragma once

#include <cstdlib>          // std::abort
#include <atomic>
#include <source_location>

#include "Base.hpp"         // ASTRA_HAS_BUILTIN, ASTRA_COMPILER_* (see note below)
#include "Log.hpp"          // detail::Emit, LogLevel

// --- Portable, CONTINUABLE debugger break ------------------------------------
// Never __builtin_trap(): that is SIGILL/non-continuable and GCC has no
// __builtin_debugtrap (GCC bug #99299). We want a breakpoint you can step past.
#if defined(ASTRA_COMPILER_MSVC)
    #include <intrin.h>
    #define ASTRA_DEBUG_BREAK() __debugbreak()
#elif ASTRA_HAS_BUILTIN(__builtin_debugtrap)
    #define ASTRA_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("int3")
#elif defined(__aarch64__)
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("brk #0xF000")
#elif defined(__arm__)
    #define ASTRA_DEBUG_BREAK() __asm__ __volatile__("bkpt #0")
#else
    #include <csignal>
    #define ASTRA_DEBUG_BREAK() std::raise(SIGTRAP)
#endif

namespace Astra
{
    enum class AssertAction { Break, Continue };

    struct AssertContext
    {
        const char*          expression;  // stringized condition
        const char*          message;     // plain string
        std::source_location location;
    };

    // Same fn-ptr + void* shape as the log sink. Set-once-before-work.
    using AssertHandler = AssertAction (*)(const AssertContext& ctx, void* user) noexcept;

    namespace detail
    {
        inline std::atomic<AssertHandler> g_assertHandler{nullptr};
        inline std::atomic<void*>         g_assertUser{nullptr};

        // Default handler: route the failure through the log sink at Critical, then
        // ask to Break. (Astra cannot flush a user's sink; StderrSink flushes itself.)
        inline AssertAction DefaultAssertHandler(const AssertContext& ctx, void* /*user*/) noexcept
        {
            Emit(LogLevel::Critical, ASTRA_LOG_CATEGORY, ctx.location,
                 ctx.message != nullptr ? ctx.message : ctx.expression);
            return AssertAction::Break;
        }

        inline AssertAction ReportAssertFailure(const AssertContext& ctx) noexcept
        {
            const AssertHandler h = g_assertHandler.load(std::memory_order_acquire);
            if (h != nullptr)
                return h(ctx, g_assertUser.load(std::memory_order_acquire));
            return DefaultAssertHandler(ctx, nullptr);
        }

        // VERIFY failure path: report; on Break → break + abort; always yields false.
        inline bool FailFatal(const char* expr, const char* msg,
                              const std::source_location& loc) noexcept
        {
            if (ReportAssertFailure(AssertContext{expr, msg, loc}) == AssertAction::Break)
            {
                ASTRA_DEBUG_BREAK();
                std::abort();
            }
            return false;
        }

        // ENSURE failure path: report; on Break → break (NEVER abort); always yields false.
        inline bool FailEnsure(const char* expr, const char* msg,
                               const std::source_location& loc) noexcept
        {
            if (ReportAssertFailure(AssertContext{expr, msg, loc}) == AssertAction::Break)
                ASTRA_DEBUG_BREAK();
            return false;
        }
    }

    inline void SetAssertHandler(AssertHandler handler, void* user = nullptr) noexcept
    {
        detail::g_assertUser.store(user, std::memory_order_release);
        detail::g_assertHandler.store(handler, std::memory_order_release);
    }
}
```

> **Include-order note for the implementer:** `Assert.hpp` includes `Base.hpp` for `ASTRA_HAS_BUILTIN`/`ASTRA_COMPILER_*`. In Task 4 `Base.hpp` will include `Assert.hpp` at its bottom; `#pragma once` makes the cycle safe **provided** `Base.hpp` includes `Assert.hpp` only AFTER defining those macros. This task does not yet touch `Base.hpp`, so `AssertTest.cpp` including `Assert.hpp` directly pulls `Base.hpp` first (defining the macros), which is correct.

- [ ] **Step 4: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Assert.*`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Core/Assert.hpp tests/Core/AssertTest.cpp
git commit -m "feat(core): assert handler seam + continuable debug-break (Assert.hpp)"
```

---

## Task 4: `ASTRA_ASSERT` + `ASTRA_VERIFY` macros + `Base.hpp` integration

**Files:**
- Modify: `include/Astra/Core/Assert.hpp` (append macros)
- Modify: `include/Astra/Core/Base.hpp` (`:45-52` remove old `ASTRA_ASSERT`; add includes at bottom)
- Test: `tests/Core/AssertTest.cpp` (append)

**Interfaces:**
- Consumes: `detail::ReportAssertFailure`, `detail::FailFatal`, `AssertAction` (Task 3); `ASTRA_DEBUG_BREAK` (Task 3).
- Produces: `ASTRA_ASSERT(cond, msg)` (statement; active in Debug or with `ASTRA_ENABLE_ASSERTS`; else compiled out); `ASTRA_VERIFY(cond, msg)` (expression yielding `bool`; evaluates `cond` in all configs; handles failure only when active).

- [ ] **Step 1: Write the failing tests (append to `AssertTest.cpp`)**

```cpp
// ---- Task 4: ASSERT + VERIFY ------------------------------------------------

TEST(Assert, AssertInvokesHandlerOnlyWhenActive)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);
    ASTRA_ASSERT(1 == 2, "never equal");
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_EQ(cap.count, 1);          // active config → fired (handler returned Continue)
    EXPECT_EQ(cap.message, "never equal");
#else
    EXPECT_EQ(cap.count, 0);          // compiled out → not evaluated
#endif
    Astra::SetAssertHandler(nullptr);
}

TEST(Assert, VerifyEvaluatesConditionInEveryConfigAndYieldsIt)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);
    int sideEffects = 0;
    const bool ok = ASTRA_VERIFY([&]{ ++sideEffects; return true; }(), "should pass");
    EXPECT_TRUE(ok);
    EXPECT_EQ(sideEffects, 1);        // condition ran in EVERY config

    const bool bad = ASTRA_VERIFY([&]{ ++sideEffects; return false; }(), "should fail");
    EXPECT_FALSE(bad);
    EXPECT_EQ(sideEffects, 2);        // still evaluated even on the failing path
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_EQ(cap.count, 1);          // failure handled (Continue) in active configs
#else
    EXPECT_EQ(cap.count, 0);          // failure not handled in Release/Dist
#endif
    Astra::SetAssertHandler(nullptr);
}
```

- [ ] **Step 2: Build Debug and confirm failure**

Expected: compile error — `ASTRA_ASSERT`/`ASTRA_VERIFY` unknown (they are still the old `Base.hpp` form which lacks the handler, and `ASTRA_VERIFY` does not exist).

- [ ] **Step 3: Append the macros to `Assert.hpp`** (after the closing `}` of `namespace Astra`)

```cpp
// =============================================================================
// Guard macros. ASSERT is statement-form and compiles out in Release/Dist
// (condition NOT evaluated) unless ASTRA_ENABLE_ASSERTS. VERIFY is an
// expression that ALWAYS evaluates its condition and yields it; it only HANDLES
// a failure when active. (cond, message) — message is a plain string.
// =============================================================================

#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)

    #define ASTRA_ASSERT(cond, message)                                                    \
        do {                                                                               \
            if (!(cond)) [[unlikely]] {                                                     \
                if (::Astra::detail::ReportAssertFailure(                                   \
                        ::Astra::AssertContext{#cond, (message),                            \
                                               std::source_location::current()})            \
                    == ::Astra::AssertAction::Break) [[unlikely]] {                         \
                    ASTRA_DEBUG_BREAK();                                                    \
                    std::abort();                                                          \
                }                                                                          \
            }                                                                              \
        } while (0)

    #define ASTRA_VERIFY(cond, message)                                                    \
        ( (cond) ||                                                                         \
          ::Astra::detail::FailFatal(#cond, (message), std::source_location::current()) )

#else

    // Compiled out: keep `cond` compiler-checked at zero runtime cost. Do NOT insert an
    // unreachable-hint here (that would mask a recoverable Condition — see release-safety spec).
    #define ASTRA_ASSERT(cond, message) do { (void)sizeof(bool(cond)); (void)sizeof(message); } while (0)

    // VERIFY still EVALUATES cond in Release/Dist (side effects preserved), discards the result.
    #define ASTRA_VERIFY(cond, message) ( (cond) ? true : (((void)(message)), false) )

#endif
```

- [ ] **Step 4: Integrate into `Base.hpp`**

In `include/Astra/Core/Base.hpp`, DELETE the old assert block (`:45-52`):

```cpp
// Runtime assertion macro
// Note: Define ASTRA_BUILD_DEBUG in your build system (premake5) for debug builds
#ifdef ASTRA_BUILD_DEBUG
    #include <cassert>
    #define ASTRA_ASSERT(condition, message) assert((condition) && (message))
#else
    #define ASTRA_ASSERT(condition, message) ((void)0)
#endif
```

Then add, as the **last two lines of the file** (after every `ASTRA_HAS_BUILTIN`/`ASTRA_NODISCARD`/attribute macro is defined):

```cpp
// Diagnostics seam — included last so Log/Assert see the macros defined above.
#include "Log.hpp"
#include "Assert.hpp"
```

- [ ] **Step 5: Build all three configs and run the FULL suite**

Build Debug, Release, Dist (whole solution). For each: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`
Expected: all green in every config — this proves the 134 existing `ASTRA_ASSERT` sites still compile and behave, and the config-gated Task 4 tests pass in each config. (`CompressionTest.PerformanceBenchmark` is a known Release/Dist flake — rerun alone if it is the lone failure.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Core/Assert.hpp include/Astra/Core/Base.hpp tests/Core/AssertTest.cpp
git commit -m "feat(core): ASTRA_ASSERT/ASTRA_VERIFY on the handler seam; wire Base.hpp"
```

---

## Task 5: `ASTRA_ENSURE` + `ASTRA_ENSURE_ALWAYS`

**Files:**
- Modify: `include/Astra/Core/Assert.hpp` (append)
- Test: `tests/Core/AssertTest.cpp` (append)

**Interfaces:**
- Consumes: `detail::FailEnsure` (Task 3).
- Produces: `ASTRA_ENSURE(cond, msg)` — expression yielding `bool`, evaluates + reports in **all** configs, **non-fatal** (continues), returns `cond`, fires **once per call-site**. `ASTRA_ENSURE_ALWAYS(cond, msg)` — same but reports every time.

- [ ] **Step 1: Write the failing tests (append)**

```cpp
// ---- Task 5: ENSURE ---------------------------------------------------------

TEST(Assert, EnsureContinuesReturnsConditionAndFiresOncePerSite)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);

    int recovered = 0;
    for (int i = 0; i < 5; ++i)
    {
        // Same call-site hit 5x: fires ONCE, always returns false, always lets us recover.
        if (!ASTRA_ENSURE(i > 100, "i too small")) { ++recovered; }
    }
    EXPECT_EQ(recovered, 5);          // non-fatal: recovery branch taken every iteration
    EXPECT_EQ(cap.count, 1);          // reported once for this call-site

    const bool ok = ASTRA_ENSURE(1 + 1 == 2, "math");
    EXPECT_TRUE(ok);                  // passing ensure yields true, no report
    EXPECT_EQ(cap.count, 1);

    Astra::SetAssertHandler(nullptr);
}

// (Each ASTRA_ENSURE expansion owns its own call-site-local `static` fire-once flag.)

TEST(Assert, EnsureAlwaysReportsEveryTime)
{
    AssertCapture cap;
    Astra::SetAssertHandler(&RecordingHandler, &cap);
    for (int i = 0; i < 3; ++i) (void)ASTRA_ENSURE_ALWAYS(false, "each time");
    EXPECT_EQ(cap.count, 3);
    Astra::SetAssertHandler(nullptr);
}
```

(Delete the stray `EnsureOnce` forward-declaration line if your linter flags it — it is not needed; it is only here to remind you each `ASTRA_ENSURE` expansion owns its own `static` flag.)

- [ ] **Step 2: Build Debug and confirm failure**

Expected: compile error — `ASTRA_ENSURE`/`ASTRA_ENSURE_ALWAYS` undefined.

- [ ] **Step 3: Append the ENSURE macros to `Assert.hpp`**

```cpp
// ENSURE — recoverable guard, ALL configs, non-fatal, returns cond. Fires once
// per call-site via a call-site-local static (each macro expansion is a distinct
// lambda type → its own `static`). Best-effort under threads (documented). The
// immediately-invoked lambda takes cond/message/location as ARGUMENTS so cond is
// evaluated exactly once and source_location is captured at the call site.
#define ASTRA_ENSURE(cond, message)                                                        \
    ([](bool astra_ok, const char* astra_msg,                                              \
        const std::source_location& astra_loc) noexcept -> bool {                          \
        if (astra_ok) [[likely]] return true;                                              \
        static bool astra_ensure_fired = false;                                            \
        if (!astra_ensure_fired) {                                                         \
            astra_ensure_fired = true;                                                     \
            (void)::Astra::detail::FailEnsure(#cond, astra_msg, astra_loc);                \
        }                                                                                  \
        return false;                                                                      \
    }(static_cast<bool>(cond), (message), std::source_location::current()))

// ENSURE_ALWAYS — reports on every failure (no per-site dedup). Plain expression.
#define ASTRA_ENSURE_ALWAYS(cond, message)                                                 \
    ( (cond) ||                                                                             \
      ::Astra::detail::FailEnsure(#cond, (message), std::source_location::current()) )
```

- [ ] **Step 4: Build Debug and run**

Run: `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Assert.*`
Expected: PASS (all Assert tests).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Core/Assert.hpp tests/Core/AssertTest.cpp
git commit -m "feat(core): ASTRA_ENSURE / ASTRA_ENSURE_ALWAYS recoverable guards"
```

---

## Task 6: Convert hand-rolled ensure sites + drop dead includes

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`:565` ensure; `:12` drop `<iostream>`)
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`:810`)
- Modify: `include/Astra/Container/FlatSet.hpp` (`:536`)
- Modify: `include/Astra/Container/FlatMap.hpp` (`:570`)
- Modify: `include/Astra/Container/Bitmap.hpp` (`:33`, `:44`)
- Modify: `include/Astra/System/SystemScheduler.hpp` (`:173`)
- Modify: `include/Astra/Registry/Registry.hpp` (`:5` drop `<iostream>`)
- Test: existing suites (no new tests — behavior is unchanged; the ENSURE macro is covered by Task 5)

**Interfaces:**
- Consumes: `ASTRA_ENSURE` (Task 5).

Each conversion replaces a hand-rolled `ASTRA_ASSERT(cond, "…"); if (<negation>) { recover }` with a single `if (!ASTRA_ENSURE(cond, "…")) { recover }`, deleting the now-redundant separate assert. Verify each `cond`/recovery by reading the surrounding function first (line numbers are pre-Task-6 anchors — grep the message string to locate the exact site).

- [ ] **Step 1: Convert `Archetype.hpp:565`**

Current:
```cpp
                ASTRA_ASSERT(chunkIndex < m_chunks.size(), "Chunk index out of bounds");
                if (chunkIndex >= m_chunks.size()) ASTRA_UNLIKELY
                    continue;  // Skip invalid chunk indices in Release builds
```
Replace with:
```cpp
                if (!ASTRA_ENSURE(chunkIndex < m_chunks.size(), "Chunk index out of bounds")) ASTRA_UNLIKELY
                    continue;  // Skip invalid chunk indices
```

- [ ] **Step 2: Convert `ArchetypeChunkPool.hpp:810`**

> **Superseded 2026-07-14** (final-review fix, Important 4) — this site was reclassified as
> control flow, not a guard, and reverted to a plain `if (blockUsed != 0) { continue; }` with
> no diagnostic at all: the check immediately below it documents that a chunk being acquired
> after the initial check is an *expected* outcome of the concurrent lock-free acquire/release
> path, not a violated invariant. See the design spec's "Amendments (2026-07-14)" §3.

Current:
```cpp
                ASTRA_ASSERT(blockUsed == 0, "Attempting to release non-empty block");
                if (blockUsed != 0) ASTRA_UNLIKELY
                {
```
Replace the two lines with:
```cpp
                if (!ASTRA_ENSURE(blockUsed == 0, "Attempting to release non-empty block")) ASTRA_UNLIKELY
                {
```

- [ ] **Step 3: Convert `FlatSet.hpp:536` and `FlatMap.hpp:570`**

`FlatSet.hpp` — current:
```cpp
            ASTRA_ASSERT(insertIdx < m_capacity, "FlatSet::Emplace failed to find insertion slot");
            if (insertIdx >= m_capacity) ASTRA_UNLIKELY
            {
                return {end(), false};
```
Replace the first two lines with:
```cpp
            if (!ASTRA_ENSURE(insertIdx < m_capacity, "FlatSet::Emplace failed to find insertion slot")) ASTRA_UNLIKELY
            {
                return {end(), false};
```
Do the identical transform in `FlatMap.hpp:570` (message `"FlatMap::Emplace failed to find insertion slot"`).

- [ ] **Step 4: Convert `Bitmap.hpp:33` and `:44`**

> **Superseded 2026-07-14** (final-review fix, Important 7) — `Bitmap` was excluded from this
> conversion and stays `ASTRA_ASSERT` (fatal): "recovering" here drops a bit from an archetype
> mask, so a query for that component would silently return nothing — a wrong answer with no
> crash, which is worse than halting. Both `Bitmap::Set` and `Bitmap::Reset` carry an inline
> comment recording this. See the design spec's "Amendments (2026-07-14)" §2.

Both sites are:
```cpp
            ASTRA_ASSERT(index < Bits, "Bitmap index out of range (component ID space overflow?)");
            if (index < Bits) ASTRA_LIKELY
            {
                ...work...
            }
```
Convert each to:
```cpp
            if (ASTRA_ENSURE(index < Bits, "Bitmap index out of range (component ID space overflow?)")) ASTRA_LIKELY
            {
                ...work...
            }
```
(Note the polarity: the guard keeps the *positive* `index < Bits` branch — `ASTRA_ENSURE` returns the condition, so `if (ASTRA_ENSURE(index < Bits, …))` preserves the existing `if (index < Bits)` shape while adding the debug report on the false path.)

- [ ] **Step 5: Upgrade `SystemScheduler.hpp:173` (assert-only → ensure with recovery)**

Current:
```cpp
            ASTRA_ASSERT(executor != nullptr, "Executor cannot be null");

            if (m_systems.empty())
                return;
```
Replace with:
```cpp
            if (!ASTRA_ENSURE(executor != nullptr, "Executor cannot be null"))
                return;

            if (m_systems.empty())
                return;
```
(This removes the latent null-deref in Release: `executor` was asserted then dereferenced later with no guard.)

- [ ] **Step 6: Drop the two dead `<iostream>` includes**

In `include/Astra/Archetype/Archetype.hpp` delete `#include <iostream>` (`:12`).
In `include/Astra/Registry/Registry.hpp` delete `#include <iostream>` (`:5`).
(Confirm neither file uses `std::cout`/`std::cerr` first — grep both; they do not.)

- [ ] **Step 7: Build all three configs and run the FULL suite**

Build Debug, Release, Dist. For each: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`
Expected: all green in every config (563 baseline + the new Log/Assert tests). The converted sites are exercised by the existing archetype/container/scheduler suites; behavior is unchanged (Debug still reports; Release still recovers).

- [ ] **Step 8: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Container/FlatSet.hpp include/Astra/Container/FlatMap.hpp include/Astra/Container/Bitmap.hpp include/Astra/System/SystemScheduler.hpp include/Astra/Registry/Registry.hpp
git commit -m "refactor(core): convert hand-rolled assert-then-recover sites to ASTRA_ENSURE; drop dead <iostream>"
```

---

## Final gate

- [ ] Build **Debug**, **Release**, **Dist** (x64) — whole solution — all succeed.
- [ ] `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe` green in all three (lone `CompressionTest.PerformanceBenchmark` failure is a known flake — rerun isolated).
- [ ] New `Log.*` and `Assert.*` tests present and passing; test count ≥ 563 + new tests.
- [ ] No public API break: the 134 existing `ASTRA_ASSERT` sites compile and behave; the benchmark still builds.
- [ ] Only tracked source committed (`include/`, `tests/`, `docs/`); `ide/`, `Astra.sln`, `Makefile`, `*.make` untouched in git.

---

## Self-review notes (spec coverage)

- Log seam §4 → Tasks 1–2 (types/sink/API/StderrSink; macros + double-gate). Assert seam §5 → Tasks 3–5 (debug-break + handler §5.1–5.2; ASSERT/VERIFY matrix §5.3; ENSURE §5.3; hygiene §5.4). Base.hpp wiring §3 → Task 4. Code changes §6 → Task 6 (6 ensure conversions + executor upgrade + 2 iostream drops). Testing §7 → Tasks 1–5 test files. Plain-string messages §4.4 / two-atomics §4.3 / continuable break §5.1 / `(void)sizeof(bool(cond))` disabled path §5.4 / no-auto-unreachable §5.4 → embedded in the respective tasks. Non-goals §9 (no formatting, no framework, no full sweep) respected. Forward-compat §10 (ENSURE for Theme C) — macro delivered, no code owed here.
