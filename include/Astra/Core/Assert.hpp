#pragma once

#include <cstdlib>          // std::abort
#include <atomic>
#include <source_location>
#include <string_view>

#include "Base.hpp"         // ASTRA_HAS_BUILTIN, ASTRA_COMPILER_* (see note below)
#include "Log.hpp"          // detail::Emit, LogLevel, LogRecord, LogSink, g_logSink, g_logUser, StderrSink
#include "Platform.hpp"     // ASTRA_PLATFORM_WINDOWS (do not rely on Base.hpp's include order)

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

// --- Debugger detection (gates the recoverable ENSURE break) ------------------
// A recoverable guard must break ONLY into an attached debugger (this mirrors
// Unreal's UE_DEBUG_BREAK / IsDebuggerPresent): an unattended process must never
// execute a bare int3, which would raise an unhandled EXCEPTION_BREAKPOINT and
// kill it. We forward-declare the Win32 entry point instead of including
// <windows.h>: Base.hpp includes this header, so <windows.h> here would leak into
// every single Astra TU. The signature matches <debugapi.h> exactly
// (WINBASEAPI BOOL WINAPI IsDebuggerPresent(VOID)), so a TU that also includes
// <windows.h> — e.g. anything pulling Memory.hpp — sees a compatible redeclaration.
#if defined(ASTRA_PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
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

        // A fatal condition must never die silently -- plain assert() always printed before
        // aborting, and the 134 ASTRA_ASSERT sites inherited that. If the host installed a
        // sink, route through it (they may forward to a crash reporter). Otherwise fall back
        // to stderr directly, because detail::Emit is a no-op with no sink installed.
        // The category is hard-coded: ASTRA_LOG_CATEGORY is redefinable per-TU, and baking a
        // per-TU token into this inline function's body would be an ODR violation.
        inline AssertAction DefaultAssertHandler(const AssertContext& ctx, void* /*user*/) noexcept
        {
            const std::string_view text = ctx.message != nullptr ? ctx.message : ctx.expression;
            const LogRecord record{LogLevel::Critical, "Astra", text, ctx.location};

            const LogSink sink = g_logSink.load(std::memory_order_acquire);
            if (sink != nullptr)
            {
                sink(record, g_logUser.load(std::memory_order_acquire));
            }
            else
            {
                StderrSink(record, nullptr);
            }
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
        #if defined(ASTRA_PLATFORM_WINDOWS)
            return ::IsDebuggerPresent() != 0;
        #else
            // Windows-only for now. Returning false elsewhere is the safe default: it can
            // only ever cost us a missed break, never an unattended crash -- which is the
            // property this whole gate exists to guarantee. Linux CAN be supported (read
            // TracerPid: from /proc/self/status, as UE's FLinuxPlatformMisc does); it is
            // deferred only to keep POSIX headers out of a header included by every TU.
            return false;
        #endif
        }

        // Fatal failure path (ASSERT/VERIFY): report; break ONLY into an attached
        // debugger, then ALWAYS abort. The break is gated because an unattended
        // process would otherwise die at an unhandled EXCEPTION_BREAKPOINT and never
        // reach abort() -- no CRT abort report, no SIGABRT, a confusing exit code.
        inline bool FailFatal(const char* expr, const char* msg,
                              const std::source_location& loc) noexcept
        {
            if (ReportAssertFailure(AssertContext{expr, msg, loc}) == AssertAction::Break)
            {
                if (IsDebuggerAttached())
                {
                    ASTRA_DEBUG_BREAK();
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
                ASTRA_DEBUG_BREAK();
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
                    if (::Astra::detail::IsDebuggerAttached()) [[unlikely]] {              \
                        ASTRA_DEBUG_BREAK();                                               \
                    }                                                                      \
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
