#pragma once

#include <cstddef>
#include <functional>

#include "Base.hpp"

namespace Astra
{
    // Astra deliberately creates NO threads. Every Parallel* API accepts an
    // implementation of this seam (see Registry::Config::workScheduler);
    // when none is provided, the API executes sequentially inline. Hook up
    // the job system of your choice in the host application (e.g. an
    // enkiTS-backed adapter) -- Astra itself stays scheduler-agnostic.
    class IWorkScheduler
    {
    public:
        virtual ~IWorkScheduler() = default;

        // Partition [0, count) into batches of at least minBatch and invoke
        // fn(begin, end) for each batch, possibly concurrently. Blocks until
        // all batches complete. Must be safe to call from multiple threads
        // and re-entrantly from inside fn (implementations may degrade to
        // inline execution in either case).
        // Memory model: implementations MUST establish a happens-before edge
        // between the completion of every fn invocation and the return of
        // ParallelFor -- all writes made inside fn must be visible to the
        // caller afterwards.
        // fn must not throw (Astra is exception-free), and must not suspend
        // or migrate OS threads mid-invocation: Astra uses thread identity
        // (thread_local) for per-thread state such as ParallelCommandBuffer.
        // Fiber-based schedulers must pin tasks for the duration of fn.
        virtual void ParallelFor(size_t count, size_t minBatch,
                                 const std::function<void(size_t, size_t)>& fn) = 0;

        // Number of scheduler-owned threads; the calling thread may additionally participate in ParallelFor.
        ASTRA_NODISCARD virtual size_t WorkerCount() const noexcept = 0;
    };
}
