// ASTRA_ENABLE_ASSERTS is the "checked release" knob: it keeps ASTRA_ASSERT / ASTRA_VERIFY
// active in Release and Dist builds, where Assert.hpp's
// `#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)` guard otherwise compiles
// them out entirely. It is documented and shipped, but no build configuration and no other
// test defines it -- so the "active in a non-Debug config" branch of every guard macro was
// compiled by nothing, free to rot silently until someone shipped a checked-release build
// and found the guards were dead weight. This TU is that coverage: it defines the knob and
// pins the enabled semantics (condition evaluated, handler consulted, failures reported) so
// they are built and run in Debug, Release, and Dist alike, on every CI leg.
//
// This file includes ONLY <Astra/Core/Assert.hpp> from the library. Do not "helpfully" add
// Registry.hpp, Astra.hpp, or any other Astra header here -- ASTRA_ENABLE_ASSERTS is a
// per-TU macro, so it only changes how ASTRA_ASSERT/ASTRA_VERIFY expand in THIS translation
// unit. Any Astra inline-or-template function that contains an assert (e.g. Registry::Get<T>)
// would therefore get a genuinely different function body here than in every other TU that
// includes it without the knob defined -- two definitions of the same inline function, which
// the One Definition Rule requires to be identical. The linker does not diagnose the
// mismatch; it silently keeps one of the two bodies and every caller gets whichever won,
// checked or not. Assert.hpp is safe to include this way only because it -- and everything
// it pulls in (Base.hpp, Platform.hpp, Log.hpp) -- contains zero assert sites of its own, so
// there is no inline function here for the knob to fork. Keep it that way: if Assert.hpp ever
// grows one, this file's safety argument breaks with it.
#define ASTRA_ENABLE_ASSERTS
#include <Astra/Core/Assert.hpp>

#include <gtest/gtest.h>

#include "../Support/DiagnosticsTestGuards.hpp"

namespace
{
    struct Counters
    {
        int handlerCalls = 0;   // times the installed AssertHandler was invoked
        int evalCount = 0;      // times a guard's condition genuinely executed
    };

    // Records and CONTINUES: returning Continue is what keeps a FATAL guard (ASSERT/VERIFY)
    // testable at all -- it suppresses the debug-break/abort path in FailFatal so the test
    // process survives a deliberately failing guard.
    Astra::AssertAction CountingHandler(const Astra::AssertContext& /*ctx*/, void* user) noexcept
    {
        auto* c = static_cast<Counters*>(user);
        c->handlerCalls++;
        return Astra::AssertAction::Continue;
    }

    // Used as the guard's condition itself -- deliberately NOT a lambda written inline in the
    // macro argument. The compiled-out ASSERT form is `(void)sizeof(bool(cond))`, which is
    // UNEVALUATED, so a side effect that only happens when the condition genuinely runs is
    // the sharpest available proof that the macro is actually active rather than stripped.
    bool Bump(Counters& c)
    {
        ++c.evalCount;
        return true;
    }
}

// Teeth: without the knob, Release/Dist compile ASTRA_ASSERT out entirely (see the #else
// branch in Assert.hpp) and the handler is never invoked -- handlerCalls stays 0 and this
// fails. With the knob (any config), a failing ASSERT must invoke the handler exactly once.
TEST(CheckedAsserts, AssertIsActiveInEveryConfig)
{
    Counters c;
    Astra::Testing::ScopedAssertHandler guard(&CountingHandler, &c);

    ASTRA_ASSERT(1 == 2, "checked-release guard must fire");

    EXPECT_EQ(c.handlerCalls, 1);
}

// Teeth: the compiled-out ASSERT form never evaluates `cond` at all ((void)sizeof(...) is an
// unevaluated operand), so if the knob were not active, Bump() would never run and evalCount
// would stay 0. This is a stronger check than "the handler fired" alone -- it proves the
// condition itself, not just the failure path, is genuinely live.
TEST(CheckedAsserts, AssertEvaluatesConditionWhenEnabled)
{
    Counters c;
    Astra::Testing::ScopedAssertHandler guard(&CountingHandler, &c);

    ASTRA_ASSERT(Bump(c), "condition must be evaluated, not compiled out");

    EXPECT_EQ(c.evalCount, 1);      // Bump() genuinely ran
    EXPECT_EQ(c.handlerCalls, 0);   // ...and the condition passed, so nothing was reported
}

// Teeth: ASTRA_VERIFY always evaluates its condition and yields it, in every config -- so
// EXPECT_FALSE below would pass even with the knob removed. What only happens when the knob
// is active is that the FAILURE gets HANDLED: without it, VERIFY's compiled-out form yields
// false directly and never consults the handler, so handlerCalls stays 0 and this fails.
TEST(CheckedAsserts, VerifyHandlesFailureWhenEnabled)
{
    Counters c;
    Astra::Testing::ScopedAssertHandler guard(&CountingHandler, &c);

    EXPECT_FALSE(ASTRA_VERIFY(1 == 2, "checked-release guard must be handled"));

    EXPECT_EQ(c.handlerCalls, 1);
}

// A passing ASSERT and a passing VERIFY must both leave the handler untouched -- guards
// against a broken condition check that reports regardless of outcome.
TEST(CheckedAsserts, PassingGuardsReportNothing)
{
    Counters c;
    Astra::Testing::ScopedAssertHandler guard(&CountingHandler, &c);

    ASTRA_ASSERT(1 == 1, "should not fire");
    EXPECT_TRUE(ASTRA_VERIFY(2 == 2, "should not fire"));

    EXPECT_EQ(c.handlerCalls, 0);
}
