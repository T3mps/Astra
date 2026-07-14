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
