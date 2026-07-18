#include <gtest/gtest.h>
#include "Astra/Component/ComponentRegistry.hpp"
#include "../Support/TestWorkerPool.hpp"
#include <atomic>
#include <cstdint>
#include <thread>

// Race test: many worker threads call ComponentRegistry::RegisterComponent<T>()
// concurrently for a mix of distinct types and the same type repeatedly. Before
// the first-registration guard (Theme B2 Phase B, Task 1), RegisterComponent did
// an unsynchronized `m_components.Contains(id)` check-then-write into three
// shared containers (m_components/m_hashToID FlatMaps + m_componentNames deque)
// -- a classic TOCTOU race across worker threads. This mirrors the real call
// site: CommandBuffer::AddComponent<T> calls RegisterComponent<T>() at RECORD
// time, and Phase B makes that recording happen concurrently across chunk-
// parallel workers.

// Probe types at namespace scope to avoid MSVC concept/name issues with local
// structs (mirrors the Astra_Test_ReReg pattern in ComponentRegistryTest.cpp).
namespace Astra_Test_RegConcurrency
{
    struct CT00 { int v; };
    struct CT01 { float v; };
    struct CT02 { double v; };
    struct CT03 { char v[8]; };
    struct CT04 { long v; };
    struct CT05 { short v; };
    struct CT06 { bool v; };
    struct CT07 { int v[4]; };
    struct CT08 { float v[3]; };
    struct CT09 { double v[2]; };
    struct CT10 { unsigned v; };
    struct CT11 { long long v; };
    struct CT12 { char v; };
    struct CT13 { int v[2]; };
    struct CT14 { float v; float w; };
    struct CT15 { double v; int w; };
}

namespace
{
    using namespace Astra_Test_RegConcurrency;

    using RegisterFn = void(*)(Astra::ComponentRegistry&);

    template<typename T>
    void RegisterOne(Astra::ComponentRegistry& registry)
    {
        registry.RegisterComponent<T>();
    }

    constexpr RegisterFn kRegisterFns[] =
    {
        &RegisterOne<CT00>, &RegisterOne<CT01>, &RegisterOne<CT02>, &RegisterOne<CT03>,
        &RegisterOne<CT04>, &RegisterOne<CT05>, &RegisterOne<CT06>, &RegisterOne<CT07>,
        &RegisterOne<CT08>, &RegisterOne<CT09>, &RegisterOne<CT10>, &RegisterOne<CT11>,
        &RegisterOne<CT12>, &RegisterOne<CT13>, &RegisterOne<CT14>, &RegisterOne<CT15>,
    };

    constexpr size_t kTypeCount = sizeof(kRegisterFns) / sizeof(kRegisterFns[0]);

    // Checks a single type's post-registration invariants against a freshly
    // (concurrently) populated registry. Returns via gtest EXPECT_* so a single
    // corrupted type doesn't abort the whole iteration's checks.
    template<typename T>
    void ExpectRegistered(const Astra::ComponentRegistry& registry)
    {
        const Astra::ComponentID id = Astra::TypeID<T>::Value();
        const Astra::ComponentDescriptor* desc = registry.GetComponentDescriptorByHash(Astra::TypeID<T>::Hash());
        ASSERT_NE(desc, nullptr) << "type " << Astra::TypeID<T>::Name();
        EXPECT_EQ(desc->id, id);
        EXPECT_EQ(desc->size, std::is_empty_v<T> ? 0 : sizeof(T));
        EXPECT_EQ(desc->alignment, std::is_empty_v<T> ? 1 : alignof(T));
    }

    void ExpectAllRegistered(const Astra::ComponentRegistry& registry)
    {
        ExpectRegistered<CT00>(registry); ExpectRegistered<CT01>(registry);
        ExpectRegistered<CT02>(registry); ExpectRegistered<CT03>(registry);
        ExpectRegistered<CT04>(registry); ExpectRegistered<CT05>(registry);
        ExpectRegistered<CT06>(registry); ExpectRegistered<CT07>(registry);
        ExpectRegistered<CT08>(registry); ExpectRegistered<CT09>(registry);
        ExpectRegistered<CT10>(registry); ExpectRegistered<CT11>(registry);
        ExpectRegistered<CT12>(registry); ExpectRegistered<CT13>(registry);
        ExpectRegistered<CT14>(registry); ExpectRegistered<CT15>(registry);
        EXPECT_EQ(registry.Size(), kTypeCount);
    }
}

// Under contention, many workers race to first-register a mix of distinct
// types and repeat-register the same type. Unguarded, this corrupts the
// shared FlatMaps (concurrent rehash / insert) and either crashes or leaves
// the registry in an inconsistent state (wrong Size(), missing descriptors,
// wrong id/size/alignment). Guarded, every worker either wins the one-time
// cold path for its type or observes the warm atomic flag and returns.
//
// The only writes that race the shared containers are the ~16 FIRST
// registrations, and they all happen in a tiny window at job start. Without a
// barrier the participating-caller lane starts RunJob immediately while pool
// workers must first wake from a CV wait, so the caller usually finishes all
// 16 cold-path inserts before any worker engages -- after which every other
// item is a warm-path no-op and the race almost never reproduces. To make the
// RED deterministic we (a) dispatch exactly one work item per lane
// (WorkerCount() items, minBatch 1) so every lane grabs exactly one item, and
// (b) hold an arrival barrier at the top of fn so no lane starts registering
// until ALL lanes have arrived -- then every lane hits the first
// RegisterComponent of all kTypeCount types simultaneously. Because each lane
// blocks in the barrier holding its single item, no lane drains extra items
// early, so all WorkerCount() lanes deterministically arrive (no deadlock, no
// over-drain).
TEST(ComponentRegistryConcurrency, ConcurrentRegisterDistinctAndSameTypesIsRaceFree)
{
    Astra::Testing::TestWorkerPool pool;

    const uint32_t laneCount = pool.WorkerCount();   // pool threads + participating caller
    constexpr int kIterations = 1000;

    for (int iter = 0; iter < kIterations; ++iter)
    {
        Astra::ComponentRegistry registry;   // fresh registry per iteration; non-movable, so a plain stack local
        std::atomic<uint32_t> arrived{0};

        pool.ParallelFor(laneCount, 1, [&](size_t begin, size_t end, uint32_t)
        {
            // Arrival barrier: every lane parks here until all lanes have
            // arrived, so they all hit the first RegisterComponent together.
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < laneCount)
                std::this_thread::yield();

            for (size_t i = begin; i < end; ++i)
            {
                // Each lane races to first-register EVERY type, so all lanes
                // contend on all kTypeCount cold paths at once.
                for (size_t t = 0; t < kTypeCount; ++t)
                    kRegisterFns[t](registry);
            }
        });

        ExpectAllRegistered(registry);

        if (::testing::Test::HasFatalFailure())
            break;
    }
}
