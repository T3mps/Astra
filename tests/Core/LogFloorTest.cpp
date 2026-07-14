// Raise the compile-time floor BEFORE including Log.hpp. Use the raw integer 4
// (== ASTRA_LEVEL_ERROR, which is only #defined inside Log.hpp) so Log.hpp's
// `#ifndef ASTRA_ACTIVE_LEVEL` guard adopts it. This forces macros below ERROR
// (Trace/Debug/Info/Warn) to expand to the ASTRA_DETAIL_LOG_DISABLED form.
#define ASTRA_ACTIVE_LEVEL 4
#include <gtest/gtest.h>
#include <Astra/Core/Log.hpp>
#include <string>

#include "../Support/DiagnosticsTestGuards.hpp"

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

// Proves the compile-time floor (ASTRA_ACTIVE_LEVEL == ASTRA_LEVEL_ERROR here)
// strips macro calls below the floor regardless of how permissive the runtime
// level is. Also proves the ASTRA_DETAIL_LOG_DISABLED branch compiles cleanly
// (no code, no unused-variable/unused-value warning) since this file's mere
// compilation exercises it for Trace/Debug/Info/Warn.
TEST(LogFloor, CompileTimeFloorStripsBelowFloorCallsRegardlessOfRuntimeLevel)
{
    Capture cap;
    Astra::Testing::ScopedLogSink sinkGuard(&CapturingSink, &cap);
    Astra::SetLogLevel(Astra::LogLevel::Trace);  // most permissive runtime level

    ASTRA_LOG_INFO("stripped");        // Info(2) < floor(4): disabled at compile time
    EXPECT_EQ(cap.count, 0);

    ASTRA_LOG_WARN("also stripped");   // Warn(3) < floor(4): disabled at compile time
    EXPECT_EQ(cap.count, 0);

    ASTRA_LOG_ERROR("delivered");      // Error(4) == floor(4): live; 4 >= Trace runtime
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(cap.message, "delivered");

    Astra::SetLogLevel(Astra::LogLevel::Info);  // restore documented default for other tests
}
