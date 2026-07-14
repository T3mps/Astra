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
