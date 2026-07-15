#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <Astra/Core/WorkScheduler.hpp>

namespace Astra::Testing
{
    class TestWorkerPool final : public IWorkScheduler
    {
    public:
        explicit TestWorkerPool(size_t threadCount = 0)
        {
            if (threadCount == 0)
            {
                const size_t hw = std::thread::hardware_concurrency();
                threadCount = hw > 1 ? hw - 1 : 1;  // caller participates too
            }
            m_threads.reserve(threadCount);
            for (size_t i = 0; i < threadCount; ++i)
                m_threads.emplace_back([this, i] { WorkerLoop(static_cast<uint32_t>(i)); });
        }

        ~TestWorkerPool() override
        {
            {
                std::lock_guard lock(m_mutex);
                m_stop = true;
            }
            m_wakeCv.notify_all();
            for (auto& t : m_threads) t.join();
        }

        TestWorkerPool(const TestWorkerPool&) = delete;
        TestWorkerPool& operator=(const TestWorkerPool&) = delete;

        void ParallelFor(size_t count, size_t minBatch,
                         Mosaic::FunctionRef<void(size_t, size_t, uint32_t)> fn) override
        {
            if (count == 0)
                return;
            if (minBatch == 0)
                minBatch = 1;
            // Inline when: trivial size, no workers, or nested call from a
            // worker thread (taking m_submitMutex there would deadlock).
            // Note: t_insideWorker is shared across all TestWorkerPool instances,
            // so cross-pool nesting also inlines — acceptable for a test pool.
            if (count <= minBatch || m_threads.empty() || t_insideWorker)
            {
                // Worker id: this thread's own lane if we are a pool worker, else
                // the caller lane (== m_threads.size(), the id reserved for the
                // participating caller in WorkerCount()'s inclusive count).
                fn(0, count, t_insideWorker ? t_workerId : static_cast<uint32_t>(m_threads.size()));
                return;
            }

            std::lock_guard submitLock(m_submitMutex);

            auto job = std::make_shared<Job>();
            job->fn = fn;                 // a non-owning view; the caller's callable outlives this blocking call
            job->count = count;
            job->batch = minBatch;

            {
                std::lock_guard lock(m_mutex);
                m_job = job;
                ++m_generation;
            }
            m_wakeCv.notify_all();

            t_workerId = static_cast<uint32_t>(m_threads.size());  // caller runs as the reserved caller lane
            t_insideWorker = true;   // nested ParallelFor from the caller's own batches must inline
            RunJob(*job);            // caller participates
            t_insideWorker = false;

            // Wait until every participant has drained out of the job.
            {
                std::unique_lock lock(m_mutex);
                m_doneCv.wait(lock, [&]
                {
                    return job->next.load(std::memory_order_acquire) >= job->count &&
                           job->active.load(std::memory_order_acquire) == 0;
                });
                m_job.reset();  // late wakers see a null job and skip
            }
        }

        // Inclusive of the participating caller (>= 1), per the reconciled Mosaic
        // contract: N pool threads + the caller lane.
        ASTRA_NODISCARD uint32_t WorkerCount() const noexcept override
        {
            return static_cast<uint32_t>(m_threads.size()) + 1u;
        }

    private:
        struct Job
        {
            Mosaic::FunctionRef<void(size_t, size_t, uint32_t)> fn{};
            size_t count = 0;
            size_t batch = 1;
            std::atomic<size_t> next{0};
            std::atomic<size_t> active{0};
        };

        void WorkerLoop(uint32_t workerId)
        {
            t_workerId = workerId;
            t_insideWorker = true;
            uint64_t seen = 0;
            for (;;)
            {
                std::shared_ptr<Job> job;
                {
                    std::unique_lock lock(m_mutex);
                    m_wakeCv.wait(lock, [&] { return m_stop || m_generation != seen; });
                    if (m_stop)
                        return;
                    seen = m_generation;
                    job = m_job;  // may be null if we woke late
                }
                if (job)
                    RunJob(*job);
            }
        }

        void RunJob(Job& job)
        {
            job.active.fetch_add(1, std::memory_order_acq_rel);
            size_t i;
            while ((i = job.next.fetch_add(job.batch, std::memory_order_relaxed)) < job.count)
            {
                const size_t end = i + job.batch < job.count ? i + job.batch : job.count;
                job.fn(i, end, t_workerId);
            }
            if (job.active.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::lock_guard lock(m_mutex);  // pairs with the waiter's predicate read
                m_doneCv.notify_all();
            }
        }

        inline static thread_local bool t_insideWorker = false;
        inline static thread_local uint32_t t_workerId = 0;   // pool threads: 0..N-1; caller lane: N

        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_wakeCv;
        std::condition_variable m_doneCv;
        std::shared_ptr<Job> m_job;
        uint64_t m_generation = 0;
        bool m_stop = false;
        std::mutex m_submitMutex;
    };
}
