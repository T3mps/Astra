#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <Astra/Core/WorkScheduler.hpp>
#include "../Support/TestWorkerPool.hpp"

using Astra::Testing::TestWorkerPool;

namespace
{
    TEST(WorkerPool, ProcessesEveryIndexExactlyOnce)
    {
        TestWorkerPool pool;
        constexpr size_t kCount = 100'000;
        std::vector<std::atomic<int>> hits(kCount);
        pool.ParallelFor(kCount, 64, [&](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
                hits[i].fetch_add(1, std::memory_order_relaxed);
        });
        for (size_t i = 0; i < kCount; ++i)
            ASSERT_EQ(hits[i].load(), 1) << "index " << i;
    }

    TEST(WorkerPool, ReusableAcrossManyCalls)
    {
        TestWorkerPool pool;
        for (int iter = 0; iter < 200; ++iter)
        {
            std::atomic<size_t> sum{0};
            pool.ParallelFor(1000, 16, [&](size_t b, size_t e)
            {
                sum.fetch_add(e - b, std::memory_order_relaxed);
            });
            ASSERT_EQ(sum.load(), 1000u);
        }
    }

    TEST(WorkerPool, SmallCountRunsInline)
    {
        TestWorkerPool pool;
        const auto caller = std::this_thread::get_id();
        std::atomic<bool> sameThread{true};
        pool.ParallelFor(8, 64, [&](size_t, size_t)  // count <= minBatch
        {
            if (std::this_thread::get_id() != caller) sameThread = false;
        });
        EXPECT_TRUE(sameThread.load());
    }

    TEST(WorkerPool, ZeroCountIsNoop)
    {
        TestWorkerPool pool;
        bool called = false;
        pool.ParallelFor(0, 16, [&](size_t, size_t) { called = true; });
        EXPECT_FALSE(called);
    }

    TEST(WorkerPool, NestedCallRunsInline)
    {
        TestWorkerPool pool;
        std::atomic<size_t> inner{0};
        pool.ParallelFor(4 * pool.WorkerCount() + 4, 1, [&](size_t b, size_t e)
        {
            for (size_t i = b; i < e; ++i)
                pool.ParallelFor(10, 1, [&](size_t b2, size_t e2)  // must not deadlock
                {
                    inner.fetch_add(e2 - b2, std::memory_order_relaxed);
                });
        });
        EXPECT_EQ(inner.load(), (4 * pool.WorkerCount() + 4) * 10);
    }

    TEST(WorkerPool, ConcurrentExternalCallersAreSafe)
    {
        TestWorkerPool pool;
        std::vector<std::thread> callers;
        std::atomic<size_t> total{0};
        for (int t = 0; t < 4; ++t)
            callers.emplace_back([&]
            {
                for (int i = 0; i < 50; ++i)
                    pool.ParallelFor(500, 8, [&](size_t b, size_t e)
                    {
                        total.fetch_add(e - b, std::memory_order_relaxed);
                    });
            });
        for (auto& th : callers) th.join();
        EXPECT_EQ(total.load(), 4u * 50u * 500u);
    }

    TEST(WorkerPool, ExplicitThreadCount)
    {
        TestWorkerPool pool(2);
        EXPECT_EQ(pool.WorkerCount(), 2u);
        std::atomic<size_t> sum{0};
        pool.ParallelFor(10'000, 64, [&](size_t b, size_t e) { sum += e - b; });
        EXPECT_EQ(sum.load(), 10'000u);
    }
}
