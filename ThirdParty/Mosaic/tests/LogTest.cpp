#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Log.hpp>

#include <string>
#include <vector>

namespace
{
    // A capture sink -- the shape a host installs, exercised as a test double.
    struct Capture
    {
        std::vector<Mosaic::LogLevel> levels;
        std::vector<std::string>      messages;
        std::vector<std::string>      categories;
        std::vector<unsigned>         lines;
    };

    void CaptureSink(const Mosaic::LogRecord& record, void* user) noexcept
    {
        auto* cap = static_cast<Capture*>(user);
        cap->levels.push_back(record.level);
        cap->messages.emplace_back(record.message);
        cap->categories.emplace_back(record.category);
        cap->lines.push_back(record.location.line());
    }

    // RAII: install a sink for one test, then put the globals back. The seam is
    // process-global set-once-before-work state, so a test must not leak it.
    struct SinkGuard
    {
        explicit SinkGuard(Capture& cap, Mosaic::LogLevel level = Mosaic::LogLevel::Trace)
            : m_prevLevel(Mosaic::GetLogLevel())
        {
            Mosaic::SetLogSink(&CaptureSink, &cap);
            Mosaic::SetLogLevel(level);
        }
        ~SinkGuard()
        {
            Mosaic::SetLogSink(nullptr, nullptr);
            Mosaic::SetLogLevel(m_prevLevel);
        }
        Mosaic::LogLevel m_prevLevel;
    };
}

TEST_CASE("Mosaic Log: no sink installed = no output, no crash", "[mosaic][log]")
{
    Mosaic::SetLogSink(nullptr, nullptr);
    MOSAIC_LOG_ERROR("nobody is listening");   // must be a silent no-op, not a null deref
    SUCCEED();
}

TEST_CASE("Mosaic Log: an installed sink receives level, category, message, location", "[mosaic][log]")
{
    Capture cap;
    SinkGuard guard(cap);

    MOSAIC_LOG_WARN("disk is on fire");
    MOSAIC_LOG_ERROR("and now the backup");

    REQUIRE(cap.messages.size() == 2);
    CHECK(cap.messages[0] == "disk is on fire");
    CHECK(cap.levels[0] == Mosaic::LogLevel::Warn);
    CHECK(cap.levels[1] == Mosaic::LogLevel::Error);
    CHECK(cap.categories[0] == "Mosaic");

    // The location is the CALL SITE, not somewhere inside the logging machinery:
    // two calls on consecutive lines must report strictly increasing line numbers.
    // (Deliberately NOT compared against __LINE__ -- MSVC's __LINE__ is unreliable
    // under Edit-and-Continue debug info, which would make this a test of the
    // compiler rather than of the seam.)
    CHECK(cap.lines[0] > 0);
    CHECK(cap.lines[1] == cap.lines[0] + 1);
}

TEST_CASE("Mosaic Log: the runtime level filters what reaches the sink", "[mosaic][log]")
{
    Capture cap;
    SinkGuard guard(cap, Mosaic::LogLevel::Error);

    MOSAIC_LOG_INFO("chatter");        // below the floor -> dropped
    MOSAIC_LOG_WARN("still chatter");  // below the floor -> dropped
    MOSAIC_LOG_ERROR("this matters");
    MOSAIC_LOG_CRITICAL("this too");

    REQUIRE(cap.messages.size() == 2);
    CHECK(cap.messages[0] == "this matters");
    CHECK(cap.messages[1] == "this too");
    CHECK(cap.levels[0] == Mosaic::LogLevel::Error);
    CHECK(cap.levels[1] == Mosaic::LogLevel::Critical);

    // Off silences everything, including Critical.
    Mosaic::SetLogLevel(Mosaic::LogLevel::Off);
    MOSAIC_LOG_CRITICAL("swallowed");
    CHECK(cap.messages.size() == 2);
}

TEST_CASE("Mosaic Log: LevelName covers every level", "[mosaic][log]")
{
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Trace) == "trace");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Debug) == "debug");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Info) == "info");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Warn) == "warn");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Error) == "error");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Critical) == "critical");
    CHECK(Mosaic::LevelName(Mosaic::LogLevel::Off) == "off");
}
