#include <catch2/catch_test_macros.hpp>

#include <Mosaic/Jobs/WorkScheduler.hpp>

#include "Support/TestWorkScheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using Mosaic::IWorkScheduler;

namespace
{
    // Sum [0,n) via the scheduler: each lane accumulates into its OWN slot
    // (indexed by worker id -> no locking), then reduce. Exercises the
    // distinct-worker-id + bidirectional-happens-before contract.
    std::uint64_t ParallelSum(IWorkScheduler& sched, std::size_t n)
    {
        std::vector<std::uint64_t> perWorker(sched.WorkerCount(), 0);
        sched.ParallelFor(n, 1, [&](std::size_t b, std::size_t e, std::uint32_t w) {
            std::uint64_t local = 0;
            for (std::size_t i = b; i < e; ++i) local += i;
            perWorker[w] += local;
        });
        return std::accumulate(perWorker.begin(), perWorker.end(), std::uint64_t{ 0 });
    }
}

TEST_CASE("SerialWorkScheduler runs the whole range inline as worker 0", "[mosaic][jobs]")
{
    Mosaic::SerialWorkScheduler serial;
    CHECK(serial.WorkerCount() == 1u);

    std::uint32_t seenWorker = 999;
    std::size_t begin = 999, end = 999;
    serial.ParallelFor(50, 1, [&](std::size_t b, std::size_t e, std::uint32_t w) {
        seenWorker = w; begin = b; end = e;
    });
    CHECK(seenWorker == 0u);
    CHECK(begin == 0u);
    CHECK(end == 50u);

    bool called = false;   // count == 0 is a no-op.
    serial.ParallelFor(0, 1, [&](std::size_t, std::size_t, std::uint32_t) { called = true; });
    CHECK_FALSE(called);
}

TEST_CASE("ParallelFor covers every index exactly once, worker ids in range", "[mosaic][jobs]")
{
    const std::uint32_t hw = std::thread::hardware_concurrency();
    Mosaic::Testing::TestWorkScheduler sched(hw > 1u ? hw : 2u);

    const std::size_t n = 10000;
    std::vector<std::atomic<int>> visits(n);
    for (auto& v : visits) v.store(0);
    std::atomic<int> badWorker{ 0 };

    sched.ParallelFor(n, 1, [&](std::size_t b, std::size_t e, std::uint32_t w) {
        if (w >= sched.WorkerCount()) badWorker.fetch_add(1);
        for (std::size_t i = b; i < e; ++i) visits[i].fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(badWorker.load() == 0);
    int minV = 2, maxV = 0;
    for (auto& v : visits) { int x = v.load(); minV = std::min(minV, x); maxV = std::max(maxV, x); }
    CHECK(minV == 1);
    CHECK(maxV == 1);
}

TEST_CASE("MT invariance: serial == 1 worker == N workers", "[mosaic][jobs][determinism]")
{
    const std::size_t n = 100000;
    const std::uint32_t hw = std::thread::hardware_concurrency();

    Mosaic::SerialWorkScheduler serial;
    Mosaic::Testing::TestWorkScheduler one(1);
    Mosaic::Testing::TestWorkScheduler many(hw > 1u ? hw : 2u);

    const std::uint64_t expected = static_cast<std::uint64_t>(n) * (n - 1) / 2;  // sum 0..n-1
    CHECK(ParallelSum(serial, n) == expected);
    CHECK(ParallelSum(one, n) == expected);
    CHECK(ParallelSum(many, n) == expected);
}
