#pragma once

// Re-export shim. The logging seam MOVED TO MOSAIC, the shared Starworks core, where
// it is the single canonical copy (this file was its move origin). Astra keeps its
// ASTRA_* / Astra:: vocabulary via the aliases below, so every call site -- the
// ASTRA_LOG_* macros in production, and Astra::LogLevel / Astra::SetLogSink /
// Astra::detail::Emit in the tests -- is unchanged.
//
// Two knobs are forwarded to Mosaic BEFORE its header is parsed, so their existing
// semantics survive:
//   ASTRA_ACTIVE_LEVEL  -- the compile-time floor (a TU may raise it before including)
//   ASTRA_LOG_CATEGORY  -- the per-TU category tag; DEFAULTS TO "Astra" (not Mosaic's
//                          "Mosaic"), so Astra log records stay tagged "Astra".
//
// Ordering: this shim is the ONLY path by which Astra reaches <Mosaic/Log.hpp>, and it
// forwards the knobs above the include, so a TU that sets ASTRA_ACTIVE_LEVEL /
// ASTRA_LOG_CATEGORY before including any Astra header still gets the intended value.

// --- Forward the compile-time floor (only if a TU explicitly set it) ----------
#if defined(ASTRA_ACTIVE_LEVEL) && !defined(MOSAIC_ACTIVE_LEVEL)
#  define MOSAIC_ACTIVE_LEVEL ASTRA_ACTIVE_LEVEL
#endif

// --- Forward the category, defaulting to "Astra" ------------------------------
#ifndef ASTRA_LOG_CATEGORY
#  define ASTRA_LOG_CATEGORY "Astra"
#endif
#ifndef MOSAIC_LOG_CATEGORY
#  define MOSAIC_LOG_CATEGORY ASTRA_LOG_CATEGORY
#endif

#include <Mosaic/Log.hpp>

// Integer level constants (part of Astra's published macro surface).
#define ASTRA_LEVEL_TRACE    MOSAIC_LEVEL_TRACE
#define ASTRA_LEVEL_DEBUG    MOSAIC_LEVEL_DEBUG
#define ASTRA_LEVEL_INFO     MOSAIC_LEVEL_INFO
#define ASTRA_LEVEL_WARN     MOSAIC_LEVEL_WARN
#define ASTRA_LEVEL_ERROR    MOSAIC_LEVEL_ERROR
#define ASTRA_LEVEL_CRITICAL MOSAIC_LEVEL_CRITICAL
#define ASTRA_LEVEL_OFF      MOSAIC_LEVEL_OFF

namespace Astra
{
    using Mosaic::LogLevel;
    using Mosaic::LogRecord;
    using Mosaic::LogSink;

    using Mosaic::LevelName;
    using Mosaic::SetLogSink;
    using Mosaic::SetLogLevel;
    using Mosaic::GetLogLevel;
    using Mosaic::StderrSink;

    namespace detail
    {
        using Mosaic::detail::Emit;
    }
}

// Logging macros -- Astra's names for Mosaic's. The compile-time floor and the
// category were forwarded above, so these behave exactly as Astra's own did.
#define ASTRA_LOG_TRACE(msg)    MOSAIC_LOG_TRACE(msg)
#define ASTRA_LOG_DEBUG(msg)    MOSAIC_LOG_DEBUG(msg)
#define ASTRA_LOG_INFO(msg)     MOSAIC_LOG_INFO(msg)
#define ASTRA_LOG_WARN(msg)     MOSAIC_LOG_WARN(msg)
#define ASTRA_LOG_ERROR(msg)    MOSAIC_LOG_ERROR(msg)
#define ASTRA_LOG_CRITICAL(msg) MOSAIC_LOG_CRITICAL(msg)
