#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Assert.hpp>
#include <Mosaic/Log.hpp>

#include <string>
#include <vector>

namespace
{
    struct Recorded
    {
        std::vector<std::string> expressions;
        std::vector<std::string> messages;
        std::vector<unsigned>    lines;
    };

    // A handler that RECORDS and returns Continue -- the whole reason the seam exists.
    // Break would debug-break and abort the test process; a host (here, a test) gets to
    // say "I have seen it, carry on".
    Mosaic::AssertAction ContinueHandler(const Mosaic::AssertContext& ctx, void* user) noexcept
    {
        auto* rec = static_cast<Recorded*>(user);
        rec->expressions.emplace_back(ctx.expression);
        rec->messages.emplace_back(ctx.message != nullptr ? ctx.message : "");
        rec->lines.push_back(ctx.location.line());
        return Mosaic::AssertAction::Continue;
    }

    struct HandlerGuard
    {
        explicit HandlerGuard(Recorded& rec) { Mosaic::SetAssertHandler(&ContinueHandler, &rec); }
        ~HandlerGuard() { Mosaic::SetAssertHandler(nullptr, nullptr); }
    };

    // --- For the DEFAULT-handler path (no assert handler installed) --------------
    struct SinkCapture
    {
        int              count = 0;
        Mosaic::LogLevel level = Mosaic::LogLevel::Trace;
        std::string      message;
    };

    void CapturingLogSink(const Mosaic::LogRecord& record, void* user) noexcept
    {
        auto* cap = static_cast<SinkCapture*>(user);
        ++cap->count;
        cap->level = record.level;
        cap->message.assign(record.message);
    }

    // Installs a LOG sink and clears any ASSERT handler, so a failing guard exercises
    // DefaultAssertHandler for real. Also keeps the run pristine: with no sink, the
    // default handler prints to stderr, which would litter the suite output.
    struct DefaultHandlerGuard
    {
        explicit DefaultHandlerGuard(SinkCapture& cap)
        {
            Mosaic::SetAssertHandler(nullptr, nullptr);
            Mosaic::SetLogSink(&CapturingLogSink, &cap);
        }
        ~DefaultHandlerGuard() { Mosaic::SetLogSink(nullptr, nullptr); }
    };
}

TEST_CASE("Mosaic Assert: a passing guard reports nothing and yields true", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    MOSAIC_ASSERT(1 + 1 == 2, "arithmetic still works");
    CHECK(MOSAIC_VERIFY(1 + 1 == 2, "arithmetic still works"));
    CHECK(MOSAIC_ENSURE(1 + 1 == 2, "arithmetic still works"));
    CHECK(MOSAIC_ENSURE_ALWAYS(1 + 1 == 2, "arithmetic still works"));

    CHECK(rec.expressions.empty());
}

TEST_CASE("Mosaic Assert: VERIFY reports the failure and yields false", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    const bool ok = MOSAIC_VERIFY(2 + 2 == 5, "orwell was right");
    const bool ok2 = MOSAIC_VERIFY(2 + 2 == 6, "still wrong");

    CHECK_FALSE(ok);
    CHECK_FALSE(ok2);
    REQUIRE(rec.expressions.size() == 2);
    CHECK(rec.expressions[0] == "2 + 2 == 5");   // the condition, stringized
    CHECK(rec.messages[0] == "orwell was right");

    // Location is the CALL SITE: two failures on consecutive lines report strictly
    // increasing lines. (Not compared against __LINE__ -- see LogTest for why.)
    CHECK(rec.lines[0] > 0);
    CHECK(rec.lines[1] == rec.lines[0] + 1);
}

TEST_CASE("Mosaic Assert: VERIFY evaluates its condition exactly once, in every config", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    // This is what separates VERIFY from ASSERT: side effects survive a release
    // build, because the condition is the work, not just a check on it.
    int calls = 0;
    auto bump = [&calls] { ++calls; return true; };

    CHECK(MOSAIC_VERIFY(bump(), "must run once"));
    CHECK(calls == 1);

    calls = 0;
    auto bumpAndFail = [&calls] { ++calls; return false; };
    CHECK_FALSE(MOSAIC_VERIFY(bumpAndFail(), "must still run once"));
    CHECK(calls == 1);
}

TEST_CASE("Mosaic Assert: ENSURE is non-fatal, yields false, and fires once per call site", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    // The recovery idiom: the caller keeps running and handles the bad state.
    for (int i = 0; i < 5; ++i)
    {
        if (!MOSAIC_ENSURE(false, "recoverable")) continue;
        FAIL("ENSURE must yield false on a failing condition");
    }

    CHECK(rec.expressions.size() == 1);   // deduped: one report for five failures

    // ENSURE_ALWAYS is the un-deduped form.
    rec.expressions.clear();
    for (int i = 0; i < 5; ++i)
        CHECK_FALSE(MOSAIC_ENSURE_ALWAYS(false, "noisy"));
    CHECK(rec.expressions.size() == 5);
}

TEST_CASE("Mosaic Assert: ENSURE evaluates its condition exactly once", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    int calls = 0;
    auto bumpAndFail = [&calls] { ++calls; return false; };
    CHECK_FALSE(MOSAIC_ENSURE(bumpAndFail(), "once"));
    CHECK(calls == 1);
}

TEST_CASE("Mosaic Assert: the default handler routes a failure to the installed log sink", "[mosaic][assert]")
{
    // The integration point between the two seams. With NO assert handler installed,
    // DefaultAssertHandler must still deliver the failure to whatever LOG sink the host
    // installed, at Critical -- otherwise a stock build reports a fatal condition
    // nowhere. Driven through ENSURE because it is the one guard that reports and then
    // RETURNS (Break is honored as "break if a debugger is attached", never abort), so
    // the test survives to make its assertions.
    SinkCapture cap;
    DefaultHandlerGuard guard(cap);

    const bool ok = MOSAIC_ENSURE(1 == 2, "recoverable condition");

    CHECK_FALSE(ok);                                    // recovered, did not abort
    CHECK(cap.count == 1);                              // the sink was actually called
    CHECK(cap.level == Mosaic::LogLevel::Critical);
    CHECK(cap.message == "recoverable condition");
}

TEST_CASE("Mosaic Assert: a log level cannot silence a failing guard", "[mosaic][assert]")
{
    // SetLogLevel gates MOSAIC_LOG_*, NOT the guards: the default assert handler
    // delivers straight to the sink, bypassing the runtime level. A fatal condition must
    // never become suppressible by a logging setting.
    SinkCapture cap;
    DefaultHandlerGuard guard(cap);

    const Mosaic::LogLevel prev = Mosaic::GetLogLevel();
    Mosaic::SetLogLevel(Mosaic::LogLevel::Off);

    MOSAIC_LOG_CRITICAL("this IS suppressible");
    CHECK(cap.count == 0);

    (void)MOSAIC_ENSURE_ALWAYS(false, "this is NOT");
    CHECK(cap.count == 1);
    CHECK(cap.level == Mosaic::LogLevel::Critical);

    Mosaic::SetLogLevel(prev);
}

#if MOSAIC_ASSERTS_ACTIVE
TEST_CASE("Mosaic Assert: ASSERT reports when active", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    MOSAIC_ASSERT(false, "cannot happen");

    REQUIRE(rec.expressions.size() == 1);
    CHECK(rec.expressions[0] == "false");
    CHECK(rec.messages[0] == "cannot happen");
}
#else
TEST_CASE("Mosaic Assert: ASSERT is compiled out when inactive", "[mosaic][assert]")
{
    Recorded rec;
    HandlerGuard guard(rec);

    // Not merely un-reported -- the condition is NOT EVALUATED at all.
    int calls = 0;
    auto bump = [&calls] { ++calls; return false; };
    MOSAIC_ASSERT(bump(), "never evaluated");

    CHECK(calls == 0);
    CHECK(rec.expressions.empty());
}
#endif
