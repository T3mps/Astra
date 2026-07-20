#pragma once

// Mosaic::IWorkScheduler -- the injected data-parallel seam every Starworks
// library keys off. The library creates NO threads; SerialWorkScheduler is the
// deterministic default when the host injects none. The host adapts its own task
// executor to this interface (a small forwarding copy), so the seam has no
// dependency back on any host type -- the arrow runs host -> Mosaic.
//
// Reconciled from the two prior copies into ONE contract: Manifold2D's signature
// (a per-lane `worker` id + zero-alloc FunctionRef) UNION Astra's memory-model
// guarantees. See the ParallelFor contract below (design: docs/design.md, D1).

#include <Mosaic/FunctionRef.hpp>

#include <cstddef>
#include <cstdint>

namespace Mosaic
{
    struct IWorkScheduler
    {
        // Partition [0,count) into DISJOINT contiguous sub-ranges (each >= minBatch
        // where possible) covering it exactly once, and invoke fn(begin,end,worker)
        // on each, possibly concurrently. BLOCKS until all complete. count==0 is a
        // no-op; minBatch==0 is treated as 1.
        //
        // worker in [0,WorkerCount()) names the running lane. Concurrently-running
        // sub-ranges MUST receive DISTINCT worker ids, so fn may index per-worker
        // scratch without locking (Manifold2D solver-MT / broadphase-MT rely on this).
        //
        // Memory model: happens-before in BOTH directions -- writes made before
        // ParallelFor are visible inside fn; writes made inside fn are visible to the
        // caller once ParallelFor returns. fn must not throw. fn must not migrate OS
        // threads mid-invocation (consumers keep thread_local per-lane state); fiber
        // schedulers must pin a task for fn's duration. Re-entrant: it is legal to
        // call ParallelFor from within a running fn (an impl may degrade to inline).
        virtual void ParallelFor(std::size_t count, std::size_t minBatch,
                                 FunctionRef<void(std::size_t begin, std::size_t end,
                                                  std::uint32_t worker)> fn) = 0;

        // Inclusive of the calling thread; always >= 1. The batch-size denominator.
        [[nodiscard]] virtual std::uint32_t WorkerCount() const noexcept = 0;

        virtual ~IWorkScheduler() = default;
    };

    // Single-threaded reference: runs the whole range inline as worker 0. The
    // inject-nothing default -- deterministic, allocation-free, creates no threads.
    class SerialWorkScheduler final : public IWorkScheduler
    {
    public:
        void ParallelFor(std::size_t count, std::size_t /*minBatch*/,
                         FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
        {
            if (count == 0) return;
            fn(0, count, 0);
        }

        [[nodiscard]] std::uint32_t WorkerCount() const noexcept override { return 1; }
    };
}
