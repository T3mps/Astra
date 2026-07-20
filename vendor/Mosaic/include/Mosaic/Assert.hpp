#pragma once

// Mosaic assert SEAM. Three guards, distinguished by what they do about a failure
// -- not by how loud they are:
//
//   MOSAIC_ASSERT(cond, msg)  statement. A bug: this cannot happen. Compiled OUT
//                             (condition NOT evaluated) in a release build.
//   MOSAIC_VERIFY(cond, msg)  expression yielding cond. ALWAYS evaluates the
//                             condition (side effects preserved); handles a failure
//                             fatally when active. For a check you must keep running
//                             but that must never fail.
//   MOSAIC_ENSURE(cond, msg)  expression yielding cond. ALL configs, NEVER fatal.
//                             A recoverable condition: report once per call site,
//                             break only into an ATTACHED debugger, and hand the
//                             caller `false` so it runs its recovery path.
//
// A failure is reported through an installed handler (fn-ptr + void*, same shape as
// the log sink) so the HOST decides -- break, continue, throw from its own frame,
// record it in a test. With no handler installed, the default routes the failure
// through the log seam at Critical (falling back to stderr if no sink is installed,
// so a fatal assert can never be silently swallowed) and asks to Break.
//
// Move origin: Astra Core/Assert.hpp (the mature copy). Astra keeps its own for
// now -- the two converge once its in-flight diagnostics work lands.

#include <atomic>
#include <cstdio>           // std::fprintf, std::fflush (no-sink stderr fallback)
#include <cstdlib>          // std::abort
#include <source_location>
#include <string_view>

#include <Mosaic/Log.hpp>       // LogLevel, LogRecord, LogSink, g_logSink, g_logUser
#include <Mosaic/Platform.hpp>  // MOSAIC_COMPILER_*, MOSAIC_HAS_BUILTIN, MOSAIC_PLATFORM_*

// --- Portable, CONTINUABLE debugger break ------------------------------------
// Never __builtin_trap(): that is SIGILL / non-continuable, and GCC has no
// __builtin_debugtrap (GCC bug #99299). We want a breakpoint you can step past.
#if defined(MOSAIC_COMPILER_MSVC)
    #include <intrin.h>
    #define MOSAIC_DEBUG_BREAK() __debugbreak()
#elif MOSAIC_HAS_BUILTIN(__builtin_debugtrap)
    #define MOSAIC_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)
    #define MOSAIC_DEBUG_BREAK() __asm__ __volatile__("int3")
#elif defined(__aarch64__)
    #define MOSAIC_DEBUG_BREAK() __asm__ __volatile__("brk #0xF000")
#elif defined(__arm__)
    #define MOSAIC_DEBUG_BREAK() __asm__ __volatile__("bkpt #0")
#else
    #include <csignal>
    #define MOSAIC_DEBUG_BREAK() std::raise(SIGTRAP)
#endif

// A recoverable guard must break ONLY into an attached debugger (mirroring Unreal's
// UE_DEBUG_BREAK / IsDebuggerPresent): an unattended process must never execute a
// bare int3, which would raise an unhandled EXCEPTION_BREAKPOINT and kill it. We
// forward-declare the Win32 entry point rather than including <windows.h>, which
// would otherwise leak into every TU that includes this header. The signature
// matches <debugapi.h> exactly (WINBASEAPI BOOL WINAPI IsDebuggerPresent(VOID)), so
// a TU that also includes <windows.h> sees a compatible redeclaration.
#if defined(MOSAIC_PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
#endif

namespace Mosaic
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

        // A fatal condition must never die silently -- plain assert() always printed
        // before aborting, and every MOSAIC_ASSERT site inherits that. If the host
        // installed a sink, route through it (it may forward to a crash reporter).
        // Otherwise print the failure ourselves, because there is no sink to delegate to.
        //
        // Note this delivers STRAIGHT to the sink, bypassing detail::Emit and the runtime
        // log level: SetLogLevel(Off) silences MOSAIC_LOG_CRITICAL but must NOT be able to
        // suppress a failing guard.
        //
        // The category is hard-coded rather than MOSAIC_LOG_CATEGORY: that macro is
        // redefinable per-TU, and baking a per-TU token into this inline function's body
        // would be an ODR violation.
        inline AssertAction DefaultAssertHandler(const AssertContext& ctx, void* /*user*/) noexcept
        {
            const std::string_view text = ctx.message != nullptr ? ctx.message : ctx.expression;
            const LogSink sink = g_logSink.load(std::memory_order_acquire);
            if (sink != nullptr)
            {
                sink(LogRecord{LogLevel::Critical, "Mosaic", text, ctx.location},
                     g_logUser.load(std::memory_order_acquire));
                return AssertAction::Break;
            }

            // No sink: print the failure ourselves, INCLUDING the stringized condition.
            // LogRecord has no expression field (a log line has no business with one), so
            // routing this through StderrSink would silently drop the condition -- exactly
            // what plain assert() always printed. A host that wants the expression too
            // installs an AssertHandler and gets the whole AssertContext.
            std::fprintf(stderr, "[critical] %s:%u - assertion failed: %s (%s)\n",
                         ctx.location.file_name(),
                         static_cast<unsigned>(ctx.location.line()),
                         ctx.expression != nullptr ? ctx.expression : "<expression>",
                         ctx.message != nullptr ? ctx.message : "");
            std::fflush(stderr);
            return AssertAction::Break;
        }

        inline AssertAction ReportAssertFailure(const AssertContext& ctx) noexcept
        {
            const AssertHandler h = g_assertHandler.load(std::memory_order_acquire);
            if (h != nullptr)
                return h(ctx, g_assertUser.load(std::memory_order_acquire));
            return DefaultAssertHandler(ctx, nullptr);
        }

        // Best-effort: on platforms with no cheap query we report "no debugger",
        // which errs toward never halting an unattended process.
        [[nodiscard]] inline bool IsDebuggerAttached() noexcept
        {
        #if defined(MOSAIC_PLATFORM_WINDOWS)
            return ::IsDebuggerPresent() != 0;
        #else
            // Windows-only for now. Returning false elsewhere is the safe default: it can
            // only ever cost us a missed break, never an unattended crash -- which is the
            // property this whole gate exists to guarantee. Linux CAN be supported (read
            // TracerPid: from /proc/self/status); it is deferred only to keep POSIX headers
            // out of a header included this widely.
            return false;
        #endif
        }

        // Fatal failure path (ASSERT/VERIFY): report; break ONLY into an attached
        // debugger, then ALWAYS abort. The break is gated because an unattended process
        // would otherwise die at an unhandled EXCEPTION_BREAKPOINT and never reach
        // abort() -- no CRT abort report, no SIGABRT, a confusing exit code.
        inline bool FailFatal(const char* expr, const char* msg,
                              const std::source_location& loc) noexcept
        {
            if (ReportAssertFailure(AssertContext{expr, msg, loc}) == AssertAction::Break)
            {
                if (IsDebuggerAttached())
                {
                    MOSAIC_DEBUG_BREAK();
                }
                std::abort();
            }
            return false;
        }

        // ENSURE failure path (recoverable): report; break ONLY into an attached
        // debugger; NEVER abort; always yield false so the caller runs its recovery.
        inline bool FailEnsure(const char* expr, const char* msg,
                               const std::source_location& loc) noexcept
        {
            if (ReportAssertFailure(AssertContext{expr, msg, loc}) == AssertAction::Break
                && IsDebuggerAttached())
            {
                MOSAIC_DEBUG_BREAK();
            }
            return false;
        }
    }

    inline void SetAssertHandler(AssertHandler handler, void* user = nullptr) noexcept
    {
        detail::g_assertUser.store(user, std::memory_order_release);
        detail::g_assertHandler.store(handler, std::memory_order_release);
    }
}

// =============================================================================
// Guard macros.
//
// Active by default in any build that has NOT defined NDEBUG -- i.e. the same
// on/off as <cassert>, so a host gets Mosaic's checks in its debug build without
// having to learn a Mosaic build flag. Force either way with MOSAIC_ENABLE_ASSERTS
// / MOSAIC_DISABLE_ASSERTS.
// =============================================================================

#if defined(MOSAIC_DISABLE_ASSERTS)
#  define MOSAIC_ASSERTS_ACTIVE 0
#elif defined(MOSAIC_ENABLE_ASSERTS) || defined(MOSAIC_DEBUG) || !defined(NDEBUG)
#  define MOSAIC_ASSERTS_ACTIVE 1
#else
#  define MOSAIC_ASSERTS_ACTIVE 0
#endif

#if MOSAIC_ASSERTS_ACTIVE

    #define MOSAIC_ASSERT(cond, message)                                                   \
        do {                                                                               \
            if (!(cond)) [[unlikely]] {                                                    \
                (void)::Mosaic::detail::FailFatal(#cond, (message),                        \
                                                  std::source_location::current());        \
            }                                                                              \
        } while (0)

    #define MOSAIC_VERIFY(cond, message)                                                   \
        ( (cond) ||                                                                        \
          ::Mosaic::detail::FailFatal(#cond, (message), std::source_location::current()) )

#else

    // Compiled out: keep `cond` compiler-checked at zero runtime cost. Do NOT insert
    // an unreachable-hint here -- that would mask a recoverable condition.
    #define MOSAIC_ASSERT(cond, message) do { (void)sizeof(bool(cond)); (void)sizeof(message); } while (0)

    // VERIFY still EVALUATES cond when inactive (side effects preserved); it just
    // discards the result instead of handling it.
    #define MOSAIC_VERIFY(cond, message) ( (cond) ? true : (((void)(message)), false) )

#endif

// ENSURE -- recoverable guard, ALL configs, non-fatal, returns cond. Fires once per
// call site via a call-site-local static (each macro expansion is a distinct lambda
// type, hence its own `static`); the flag is an atomic exchange, so concurrent
// failures at one site report exactly once rather than racing. The immediately-invoked
// lambda takes cond/message/location as ARGUMENTS, so cond is evaluated exactly once
// and the source_location is captured at the call site rather than inside the lambda.
#define MOSAIC_ENSURE(cond, message)                                                       \
    ([](bool mosaic_ok, const char* mosaic_msg,                                            \
        const std::source_location& mosaic_loc) noexcept -> bool {                         \
        if (mosaic_ok) [[likely]] return true;                                             \
        static std::atomic<bool> mosaic_ensure_fired{false};                               \
        if (!mosaic_ensure_fired.exchange(true, std::memory_order_relaxed)) {              \
            (void)::Mosaic::detail::FailEnsure(#cond, mosaic_msg, mosaic_loc);             \
        }                                                                                  \
        return false;                                                                      \
    }(static_cast<bool>(cond), (message), std::source_location::current()))

// ENSURE_ALWAYS -- reports on every failure (no per-site dedup). Plain expression.
#define MOSAIC_ENSURE_ALWAYS(cond, message)                                                \
    ( (cond) ||                                                                            \
      ::Mosaic::detail::FailEnsure(#cond, (message), std::source_location::current()) )
