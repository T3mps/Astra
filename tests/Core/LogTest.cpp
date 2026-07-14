#include <gtest/gtest.h>
#include <Astra/Core/Log.hpp>
#include <string>

namespace
{
    struct Capture { int count = 0; Astra::LogLevel level{}; std::string message; std::string category; unsigned line = 0; };

    void CapturingSink(const Astra::LogRecord& r, void* user) noexcept
    {
        auto* c = static_cast<Capture*>(user);
        c->count++;
        c->level = r.level;
        c->message = std::string(r.message);
        c->category = std::string(r.category);
        c->line = r.location.line();
    }
}

TEST(Log, EmitDeliversRecordToInstalledSink)
{
    Capture cap;
    Astra::SetLogSink(&CapturingSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Trace);
    Astra::detail::Emit(Astra::LogLevel::Warn, "TestCat", std::source_location::current(), "hello");
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.level, Astra::LogLevel::Warn);
    EXPECT_EQ(cap.message, "hello");
    EXPECT_EQ(cap.category, "TestCat");
    Astra::SetLogSink(nullptr);  // restore no-op for other tests
    Astra::SetLogLevel(Astra::LogLevel::Info);  // restore documented default for other tests
}

TEST(Log, NullSinkIsSilent)
{
    Astra::SetLogSink(nullptr);
    // Must not crash / must be a no-op:
    Astra::detail::Emit(Astra::LogLevel::Error, "X", std::source_location::current(), "ignored");
    SUCCEED();
}

TEST(Log, LevelNameMapsAllLevels)
{
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Trace), "trace");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Debug), "debug");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Info), "info");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Warn), "warn");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Error), "error");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Critical), "critical");
    EXPECT_EQ(Astra::LevelName(Astra::LogLevel::Off), "off");
    EXPECT_EQ(Astra::LevelName(static_cast<Astra::LogLevel>(999)), "unknown");
}
