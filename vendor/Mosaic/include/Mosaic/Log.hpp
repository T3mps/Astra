#pragma once

// Mosaic logging SEAM. Mosaic never picks a logging backend -- it emits records
// through an installed sink and the HOST decides what happens (spdlog, a game
// console, a test capture, nothing). No sink installed = no output, no cost
// beyond a relaxed atomic load.
//
// Two gates, so an unwanted level costs nothing:
//   compile time -- MOSAIC_ACTIVE_LEVEL strips the call site entirely
//   run time     -- SetLogLevel filters what reaches the sink
//
// Move origin: Astra Core/Log.hpp (the mature copy; Manifold2D had none and
// Arcane's is engine-flavored). Astra keeps its own for now -- the two converge
// once its in-flight diagnostics work lands.

#include <atomic>
#include <cstdio>
#include <source_location>
#include <string_view>

// Integer level constants usable by the preprocessor (for the compile-time floor).
#define MOSAIC_LEVEL_TRACE    0
#define MOSAIC_LEVEL_DEBUG    1
#define MOSAIC_LEVEL_INFO     2
#define MOSAIC_LEVEL_WARN     3
#define MOSAIC_LEVEL_ERROR    4
#define MOSAIC_LEVEL_CRITICAL 5
#define MOSAIC_LEVEL_OFF      6

#ifndef MOSAIC_ACTIVE_LEVEL
#  ifdef NDEBUG
#    define MOSAIC_ACTIVE_LEVEL MOSAIC_LEVEL_INFO
#  else
#    define MOSAIC_ACTIVE_LEVEL MOSAIC_LEVEL_TRACE
#  endif
#endif

// A consumer may define this to its own name before including, so records carry
// the library that produced them.
#ifndef MOSAIC_LOG_CATEGORY
#  define MOSAIC_LOG_CATEGORY "Mosaic"
#endif

namespace Mosaic
{
    enum class LogLevel : int
    {
        Trace = MOSAIC_LEVEL_TRACE, Debug = MOSAIC_LEVEL_DEBUG, Info = MOSAIC_LEVEL_INFO,
        Warn = MOSAIC_LEVEL_WARN, Error = MOSAIC_LEVEL_ERROR, Critical = MOSAIC_LEVEL_CRITICAL,
        Off = MOSAIC_LEVEL_OFF,
    };

    struct LogRecord
    {
        LogLevel             level;
        std::string_view     category;   // compile-time MOSAIC_LOG_CATEGORY
        std::string_view     message;    // plain string (points into a string literal)
        std::source_location location;
    };

    // Raw fn-ptr + void*: no std::function (no alloc, no vtable), and it survives a
    // DLL/.so boundary. Contract: may be called from any thread (serialize
    // internally); must NOT throw; must NOT call back into Mosaic.
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

    // Convenience sink (NOT installed by default): "[level] file:line - message" to
    // stderr. Flushes itself, so it is safe to use as the last word before an abort.
    inline void StderrSink(const LogRecord& record, void* /*user*/) noexcept
    {
        const std::string_view name = LevelName(record.level);
        std::fprintf(stderr, "[%.*s] %s:%u - %.*s\n",
                     static_cast<int>(name.size()), name.data(),
                     record.location.file_name(), record.location.line(),
                     static_cast<int>(record.message.size()), record.message.data());
        std::fflush(stderr);
    }

    namespace detail
    {
        // Two 8-byte atomics (NOT one 16-byte atomic) so there is no libatomic
        // dependency and they are always lock-free. Set-once-before-work: install the
        // sink before handing Mosaic any work.
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

    // Runtime floor for MOSAIC_LOG_*. Deliberately does NOT gate assert/ensure reports:
    // DefaultAssertHandler (Assert.hpp) delivers straight to the installed sink (or prints
    // to stderr with none installed), bypassing both detail::Emit and this level entirely,
    // so SetLogLevel(LogLevel::Off) silences MOSAIC_LOG_CRITICAL but not a failing guard.
    // That is intentional -- a fatal condition must not become suppressible by a log
    // setting -- but is easy to assume otherwise.
    inline void SetLogLevel(LogLevel minimum) noexcept
    {
        detail::g_logLevel.store(minimum, std::memory_order_relaxed);
    }

    [[nodiscard]] inline LogLevel GetLogLevel() noexcept
    {
        return detail::g_logLevel.load(std::memory_order_relaxed);
    }
}

// =============================================================================
// Logging macros. The per-macro #if strips calls below the compile-time floor
// (MOSAIC_ACTIVE_LEVEL); the runtime `if` filters against the installed
// SetLogLevel. `msg` is a plain string -- no formatting, by design (a formatting
// layer is the host's business, not a zero-dependency core's).
// =============================================================================

#define MOSAIC_DETAIL_LOG(level_, msg)                                             \
    do {                                                                           \
        if ((level_) >= ::Mosaic::GetLogLevel()) [[unlikely]] {                    \
            ::Mosaic::detail::Emit((level_), MOSAIC_LOG_CATEGORY,                  \
                                   std::source_location::current(), (msg));        \
        }                                                                          \
    } while (0)

// Disabled form: reference `msg` in an unevaluated context so there is no code
// and no unused-variable warning.
#define MOSAIC_DETAIL_LOG_DISABLED(msg) do { (void)sizeof(msg); } while (0)

#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_TRACE
#  define MOSAIC_LOG_TRACE(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Trace, msg)
#else
#  define MOSAIC_LOG_TRACE(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_DEBUG
#  define MOSAIC_LOG_DEBUG(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Debug, msg)
#else
#  define MOSAIC_LOG_DEBUG(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_INFO
#  define MOSAIC_LOG_INFO(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Info, msg)
#else
#  define MOSAIC_LOG_INFO(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_WARN
#  define MOSAIC_LOG_WARN(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Warn, msg)
#else
#  define MOSAIC_LOG_WARN(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_ERROR
#  define MOSAIC_LOG_ERROR(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Error, msg)
#else
#  define MOSAIC_LOG_ERROR(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
#if MOSAIC_ACTIVE_LEVEL <= MOSAIC_LEVEL_CRITICAL
#  define MOSAIC_LOG_CRITICAL(msg) MOSAIC_DETAIL_LOG(::Mosaic::LogLevel::Critical, msg)
#else
#  define MOSAIC_LOG_CRITICAL(msg) MOSAIC_DETAIL_LOG_DISABLED(msg)
#endif
