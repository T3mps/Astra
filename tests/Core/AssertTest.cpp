#include <gtest/gtest.h>
#include <Astra/Core/Assert.hpp>
#include <Astra/Core/Log.hpp>
#include <string>

#include "../Support/DiagnosticsTestGuards.hpp"

namespace
{
    struct AssertCapture { int count = 0; std::string expr; std::string message; };

    // Records and CONTINUES (so the process does not abort during tests).
    Astra::AssertAction RecordingHandler(const Astra::AssertContext& ctx, void* user) noexcept
    {
        auto* c = static_cast<AssertCapture*>(user);
        c->count++;
        c->expr = ctx.expression ? ctx.expression : "";
        c->message = ctx.message ? ctx.message : "";
        return Astra::AssertAction::Continue;
    }

    // Captures LogRecords delivered by the DEFAULT assert handler (Assert + Log are one
    // seam: with no assert handler installed, a failure routes through the log sink).
    struct LogCapture { int count = 0; Astra::LogLevel level{}; std::string message; unsigned line = 0; };

    void CapturingLogSink(const Astra::LogRecord& r, void* user) noexcept
    {
        auto* c = static_cast<LogCapture*>(user);
        c->count++;
        c->level = r.level;
        c->message = std::string(r.message);
        c->line = r.location.line();
    }
}

TEST(Assert, HandlerReceivesContextAndItsDecisionIsReturned)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);

    const auto action = Astra::detail::ReportAssertFailure(
        Astra::AssertContext{"x < y", "bad bounds", std::source_location::current()});

    EXPECT_EQ(action, Astra::AssertAction::Continue);
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.expr, "x < y");
    EXPECT_EQ(cap.message, "bad bounds");
}

TEST(Assert, FailEnsureReportsAndReturnsFalseWithoutAborting)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);
    const bool r = Astra::detail::FailEnsure("p != nullptr", "null", std::source_location::current());
    EXPECT_FALSE(r);
    EXPECT_EQ(cap.count, 1);
}

// ---- Final-review fixes: the handler->sink integration point ---------------
// These close the exact coverage hole that let the headline bug ship: with no
// assert handler installed, DefaultAssertHandler used to report via detail::Emit,
// which is a no-op with no log sink installed either -- so a stock build aborted
// on a failing guard while printing nothing at all. None of the above tests
// exercise the DEFAULT handler; they all install RecordingHandler.

// The reason Log and Assert are ONE seam: a failure with no assert handler
// installed must still reach an installed LOG sink, at Critical, with the
// message and a plausible source location. This does NOT catch the Critical --
// detail::Emit already does exactly this (load g_logSink, call it), so this
// test passes against the buggy handler too. What it pins is that the DEFAULT
// handler routes through an installed sink rather than always hitting stderr;
// see DefaultHandlerFallsBackToStderrWithNoSinkInstalled below for the test
// that actually catches the silent-abort regression.
TEST(Assert, DefaultHandlerRoutesFailureToInstalledLogSink)
{
    LogCapture cap;
    Astra::Testing::ScopedLogSink sinkGuard(&CapturingLogSink, &cap);
    Astra::Testing::ScopedAssertHandler handlerGuard(nullptr);  // exercise the DEFAULT handler

    const unsigned expectedLine = __LINE__ + 1;
    const auto action = Astra::detail::ReportAssertFailure(Astra::AssertContext{"x < y", "bad bounds", std::source_location::current()});

    EXPECT_EQ(action, Astra::AssertAction::Break);
    EXPECT_EQ(cap.count, 1);                                  // the sink was actually called
    EXPECT_EQ(cap.level, Astra::LogLevel::Critical);           // fatal reports are always Critical
    EXPECT_EQ(cap.message, "bad bounds");
    EXPECT_EQ(cap.line, expectedLine);                         // plausible source location
}

// The shipped default policy (Break) was never asserted anywhere.
TEST(Assert, BreakIsTheDefaultDecision)
{
    // Install a sink only to keep the run pristine (DefaultAssertHandler falls back to
    // stderr with none installed) -- this test is about the returned decision, not the
    // sink; see DefaultHandlerRoutesFailureToInstalledLogSink above for that.
    LogCapture cap;
    Astra::Testing::ScopedLogSink sinkGuard(&CapturingLogSink, &cap);
    Astra::Testing::ScopedAssertHandler handlerGuard(nullptr);

    const auto action = Astra::detail::ReportAssertFailure(
        Astra::AssertContext{"cond", "message", std::source_location::current()});

    EXPECT_EQ(action, Astra::AssertAction::Break);
}

// Guards this Critical from regressing: with NEITHER a log sink NOR an assert handler
// installed, the default handler must still print the failure -- including the
// stringized condition, which LogRecord (and therefore StderrSink) has no field for,
// so the no-sink path formats and writes to stderr directly.
TEST(Assert, DefaultHandlerFallsBackToStderrWithNoSinkInstalled)
{
    Astra::Testing::ScopedLogSink sinkGuard(nullptr);
    Astra::Testing::ScopedAssertHandler handlerGuard(nullptr);

    testing::internal::CaptureStderr();
    // Do NOT use ASTRA_ASSERT here -- with no debugger attached it would abort() the
    // test process. Call the reporting path directly, same as the tests above.
    (void)Astra::detail::ReportAssertFailure(
        Astra::AssertContext{"p != nullptr", "stderr fallback message", std::source_location::current()});
    const std::string output = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("stderr fallback message"), std::string::npos) << output;
    EXPECT_NE(output.find("p != nullptr"), std::string::npos) << output;   // the condition itself
}

// ---- Task 4: ASSERT + VERIFY ------------------------------------------------

TEST(Assert, AssertInvokesHandlerOnlyWhenActive)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);
    ASTRA_ASSERT(1 == 2, "never equal");
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
    EXPECT_EQ(cap.count, 1);          // active config → fired (handler returned Continue)
    EXPECT_EQ(cap.message, "never equal");
#else
    EXPECT_EQ(cap.count, 0);          // compiled out → not evaluated
#endif
}

TEST(Assert, VerifyEvaluatesConditionInEveryConfigAndYieldsIt)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);
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
}

// FailFatal's abort() was never directly exercised by this file in any config -- only
// incidentally, Debug-only, by the pre-existing Bitmap death tests. ASSERT now delegates
// to FailFatal (see Assert.hpp) instead of hand-inlining the same fatal path, so this
// covers that shared path directly. Only applies where the guards are active: with no
// installed handler the DEFAULT handler runs and, with no debugger attached, always
// aborts on Break.
#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)

TEST(Assert, AssertAbortsOnFailureInActiveConfig)
{
    EXPECT_DEATH({ ASTRA_ASSERT(false, "boom"); }, "boom");
}

TEST(Assert, VerifyAbortsOnFailureInActiveConfig)
{
    EXPECT_DEATH({ (void)ASTRA_VERIFY(false, "boom"); }, "boom");
}

#endif

// ---- Task 5: ENSURE ---------------------------------------------------------

TEST(Assert, EnsureContinuesReturnsConditionAndFiresOncePerSite)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);

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
}

// (Each ASTRA_ENSURE expansion owns its own call-site-local `static` fire-once flag.)

TEST(Assert, EnsureAlwaysReportsEveryTime)
{
    AssertCapture cap;
    Astra::Testing::ScopedAssertHandler guard(&RecordingHandler, &cap);
    for (int i = 0; i < 3; ++i) (void)ASTRA_ENSURE_ALWAYS(false, "each time");
    EXPECT_EQ(cap.count, 3);
}

// Regression: a failing ENSURE on the DEFAULT handler (which returns Break) must not
// halt a process with no debugger attached. Before the debugger-gated break, this
// executed a bare __debugbreak() and killed the test process (Release/Dist exit 3).
//
// Under an attached debugger this test WILL break once (by design -- it drives the
// default Break decision). Hit continue. CI has no debugger, so it never fires there.
//
// Installs a capturing LOG sink (not an assert handler override) so this drives the
// real DEFAULT handler end to end: since DefaultAssertHandler now falls back to stderr
// with no sink installed, leaving no sink here would print a stray [critical] line on
// every suite run. The sink also lets us assert the default handler actually reported.
TEST(Assert, DefaultEnsureFailureRecoversWithoutDebugger)
{
    LogCapture cap;
    Astra::Testing::ScopedLogSink sinkGuard(&CapturingLogSink, &cap);
    Astra::Testing::ScopedAssertHandler handlerGuard(nullptr);  // default handler: reports, returns Break

    const bool ok = ASTRA_ENSURE(1 == 2, "recoverable condition");

    EXPECT_FALSE(ok);                                  // yields the condition, so the caller can recover
    EXPECT_EQ(cap.count, 1);                            // default handler routed the failure to the sink
    EXPECT_EQ(cap.level, Astra::LogLevel::Critical);
    EXPECT_EQ(cap.message, "recoverable condition");

    SUCCEED();          // reaching this line at all proves the process was not halted
}
