#include <gtest/gtest.h>
#include <atomic>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using namespace Astra::Test;

    // ---------------------------------------------------------------------------
    // Helpers for ParallelExecutorRunsAllSystems
    // ---------------------------------------------------------------------------

    // A typed System struct that increments a shared counter.
    // Must be distinct types so SystemScheduler accepts both registrations.
    template<int N>
    struct CountingSystem
    {
        std::atomic<int>* counter = nullptr;
        explicit CountingSystem(std::atomic<int>& c) : counter(&c) {}
        void operator()(Astra::Registry&) { counter->fetch_add(1); }
    };

    // No scheduler injected => ParallelForEach degrades to sequential inline
    // execution (Astra creates no threads). Correctness must be identical.
    TEST(ParallelIteration, NoSchedulerFallsBackSequentially)
    {
        Astra::Registry registry;
        constexpr size_t kCount = 50'000;
        std::vector<Astra::Entity> entities(kCount);
        registry.CreateEntities<Position>(kCount, entities);

        auto view = registry.CreateView<Position>();
        size_t visits = 0;
        view.ParallelForEach([&](Astra::Entity, Position& p)
        {
            p.x += 1.0f;
            ++visits;
        });
        EXPECT_EQ(visits, kCount);

        size_t mutated = 0;
        view.ForEach([&](Astra::Entity, Position& p)
        {
            if (p.x == 1.0f) ++mutated;
        });
        EXPECT_EQ(mutated, kCount);
    }

    TEST(ParallelIteration, InjectedSchedulerIsUsed)
    {
        struct CountingScheduler final : Astra::IWorkScheduler
        {
            std::shared_ptr<Astra::Testing::TestWorkerPool> inner =
                std::make_shared<Astra::Testing::TestWorkerPool>();
            std::atomic<int> calls{0};
            void ParallelFor(size_t count, size_t minBatch,
                             const std::function<void(size_t, size_t)>& fn) override
            {
                calls.fetch_add(1);
                inner->ParallelFor(count, minBatch, fn);
            }
            size_t WorkerCount() const noexcept override { return inner->WorkerCount(); }
        };

        auto sched = std::make_shared<CountingScheduler>();
        Astra::Registry::Config config;
        config.workScheduler = sched;
        Astra::Registry registry(config);

        constexpr size_t kCount = 50'000;  // big enough to clear parallel thresholds
        std::vector<Astra::Entity> entities(kCount);
        registry.CreateEntities<Position>(kCount, entities);

        auto view = registry.CreateView<Position>();
        std::atomic<size_t> visits{0};
        view.ParallelForEach([&](Astra::Entity, Position&) { visits.fetch_add(1, std::memory_order_relaxed); });
        EXPECT_GE(sched->calls.load(), 1);
        EXPECT_EQ(visits.load(), kCount);  // parallel execution still visits every entity exactly once
    }

    TEST(ParallelIteration, ParallelForEachDescendantVisitsAll)
    {
        Astra::Registry::Config config;
        config.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
        Astra::Registry registry(config);
        auto root = registry.CreateEntity<Position>();
        constexpr size_t kChildren = 5'000;
        std::vector<Astra::Entity> kids(kChildren);
        registry.CreateEntities<Position>(kChildren, kids);
        for (auto e : kids) registry.SetParent(e, root);

        auto relations = registry.GetRelations(root);
        std::atomic<size_t> visits{0};
        relations.ParallelForEachDescendant([&](Astra::Entity, size_t)
        {
            visits.fetch_add(1, std::memory_order_relaxed);
        });
        EXPECT_EQ(visits.load(), kChildren);
    }

    TEST(ParallelIteration, ParallelExecutorRunsAllSystems)
    {
        Astra::Registry registry;
        Astra::SystemScheduler scheduler;
        std::atomic<int> ran{0};

        // Two distinct typed systems (lambda taking Registry& does not satisfy
        // LambdaLike in Astra — use explicit System structs instead).
        scheduler.AddSystem<CountingSystem<0>>(ran);
        scheduler.AddSystem<CountingSystem<1>>(ran);

        Astra::ParallelExecutor executor(std::make_shared<Astra::Testing::TestWorkerPool>());
        scheduler.Execute(registry, &executor);
        EXPECT_EQ(ran.load(), 2);

        // No scheduler => degrades to sequential, still runs everything.
        ran = 0;
        Astra::ParallelExecutor sequentialFallback;
        scheduler.Execute(registry, &sequentialFallback);
        EXPECT_EQ(ran.load(), 2);
    }
}
