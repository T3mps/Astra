#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
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
                m_threads.emplace_back([this] { WorkerLoop(); });
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
                         const std::function<void(size_t, size_t)>& fn) override
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
                fn(0, count);
                return;
            }

            std::lock_guard submitLock(m_submitMutex);

            auto job = std::make_shared<Job>();
            job->fn = &fn;
            job->count = count;
            job->batch = minBatch;

            {
                std::lock_guard lock(m_mutex);
                m_job = job;
                ++m_generation;
            }
            m_wakeCv.notify_all();

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

        ASTRA_NODISCARD size_t WorkerCount() const noexcept override
        {
            return m_threads.size();
        }

    private:
        struct Job
        {
            const std::function<void(size_t, size_t)>* fn = nullptr;
            size_t count = 0;
            size_t batch = 1;
            std::atomic<size_t> next{0};
            std::atomic<size_t> active{0};
        };

        void WorkerLoop()
        {
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
                (*job.fn)(i, end);
            }
            if (job.active.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::lock_guard lock(m_mutex);  // pairs with the waiter's predicate read
                m_doneCv.notify_all();
            }
        }

        inline static thread_local bool t_insideWorker = false;

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
