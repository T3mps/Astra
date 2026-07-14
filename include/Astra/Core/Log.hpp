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
