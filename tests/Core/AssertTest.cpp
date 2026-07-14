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
