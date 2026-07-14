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

// Regression: a failing ENSURE on the DEFAULT handler (which returns Break) must not
// halt a process with no debugger attached. Before the debugger-gated break, this
// executed a bare __debugbreak() and killed the test process (Release/Dist exit 3).
TEST(Assert, DefaultEnsureFailureRecoversWithoutDebugger)
{
    Astra::SetAssertHandler(nullptr);   // default handler: reports, returns Break

    const bool ok = ASTRA_ENSURE(1 == 2, "recoverable condition");

    EXPECT_FALSE(ok);   // yields the condition, so the caller can recover
    SUCCEED();          // reaching this line at all proves the process was not halted
}
