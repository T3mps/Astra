#pragma once

// RAII install/restore for the diagnostics seam's two global seams (log sink, assert
// handler). The tests that exercise these seams point them at a STACK capture object
// for the duration of one TEST() and restore nullptr manually afterward. That is safe
// today because every such test only uses non-fatal EXPECT_*, so an early return never
// happens -- but the first ASSERT_*/FAIL() added between install and the manual restore
// would skip the restore, leaving the global pointing at a now-dead stack frame for
// every later test in the binary (a dangling-pointer write on the next failure). These
// guards restore in their destructor unconditionally, so that failure mode cannot occur
// regardless of how the test function exits.

#include <Astra/Core/Assert.hpp>
#include <Astra/Core/Log.hpp>

namespace Astra::Testing
{
    // Installs a log sink for the guard's scope; restores the no-op default (nullptr) on
    // destruction, including on early return / ASSERT_* failure inside the scope.
    class ScopedLogSink
    {
    public:
        explicit ScopedLogSink(Astra::LogSink sink, void* user = nullptr) noexcept
        {
            Astra::SetLogSink(sink, user);
        }
        ~ScopedLogSink() noexcept { Astra::SetLogSink(nullptr); }

        ScopedLogSink(const ScopedLogSink&) = delete;
        ScopedLogSink& operator=(const ScopedLogSink&) = delete;
    };

    // Installs an assert handler for the guard's scope; restores the default handler
    // (nullptr) on destruction, including on early return / ASSERT_* failure inside scope.
    class ScopedAssertHandler
    {
    public:
        explicit ScopedAssertHandler(Astra::AssertHandler handler, void* user = nullptr) noexcept
        {
            Astra::SetAssertHandler(handler, user);
        }
        ~ScopedAssertHandler() noexcept { Astra::SetAssertHandler(nullptr); }

        ScopedAssertHandler(const ScopedAssertHandler&) = delete;
        ScopedAssertHandler& operator=(const ScopedAssertHandler&) = delete;
    };
}
