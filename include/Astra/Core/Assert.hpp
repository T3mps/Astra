#pragma once

// Re-export shim. The assert seam MOVED TO MOSAIC, the shared Starworks core, where it
// is the single canonical copy (this file was its move origin). Astra keeps its
// ASTRA_* / Astra:: vocabulary via the aliases below, so every call site is unchanged:
// the ~134 ASTRA_ASSERT / ASTRA_VERIFY / ASTRA_ENSURE sites in production, and
// Astra::AssertAction / Astra::SetAssertHandler / Astra::detail::ReportAssertFailure /
// Astra::detail::FailEnsure in the tests.
//
// The "checked release" knob is forwarded BEFORE Mosaic's header is parsed:
//   ASTRA_ENABLE_ASSERTS -> MOSAIC_ENABLE_ASSERTS
// so a Release/Dist TU that opts back in keeps ASTRA_ASSERT/VERIFY active. In the
// standard configs the two gates already coincide: Astra keys "active" off
// ASTRA_BUILD_DEBUG, Mosaic off !NDEBUG, and an Astra Debug build defines the former
// while leaving NDEBUG undefined (Release/Dist is the reverse), so ASTRA_ASSERT is
// active in exactly the same builds it always was.
//
// One accepted behavior change (Log/Assert host-adoption decision): with NEITHER an
// assert handler NOR a log sink installed, the stderr fallback tags category "Mosaic",
// not "Astra". It only surfaces in that degenerate no-sink case; an installed handler
// or sink never sees it (and the Astra tests check level/message/condition, not that
// category).

#if defined(ASTRA_ENABLE_ASSERTS) && !defined(MOSAIC_ENABLE_ASSERTS)
#  define MOSAIC_ENABLE_ASSERTS
#endif

#include "Log.hpp"           // Astra:: log aliases (Mosaic's Assert uses the log seam)
#include <Mosaic/Assert.hpp>

// A continuable debugger break, for callers that want it directly.
#define ASTRA_DEBUG_BREAK() MOSAIC_DEBUG_BREAK()

namespace Astra
{
    using Mosaic::AssertAction;
    using Mosaic::AssertContext;
    using Mosaic::AssertHandler;

    using Mosaic::SetAssertHandler;

    namespace detail
    {
        using Mosaic::detail::ReportAssertFailure;
        using Mosaic::detail::FailFatal;
        using Mosaic::detail::FailEnsure;
        using Mosaic::detail::DefaultAssertHandler;
        using Mosaic::detail::IsDebuggerAttached;
    }
}

// Guard macros -- Astra's names for Mosaic's. ASTRA_ASSERT is statement-form and
// compiles out in Release/Dist (condition not evaluated) unless ASTRA_ENABLE_ASSERTS;
// ASTRA_VERIFY always evaluates its condition and yields it; ASTRA_ENSURE is the
// recoverable, all-configs, never-fatal guard. The active-config gate was forwarded
// above.
#define ASTRA_ASSERT(cond, message)        MOSAIC_ASSERT(cond, message)
#define ASTRA_VERIFY(cond, message)        MOSAIC_VERIFY(cond, message)
#define ASTRA_ENSURE(cond, message)        MOSAIC_ENSURE(cond, message)
#define ASTRA_ENSURE_ALWAYS(cond, message) MOSAIC_ENSURE_ALWAYS(cond, message)
