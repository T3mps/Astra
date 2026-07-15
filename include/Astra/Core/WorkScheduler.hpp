#pragma once

// Astra::IWorkScheduler -- the shared Mosaic data-parallel seam.
//
// The threading interface Astra iterates through was reconciled with the sibling
// Starworks libraries (Manifold2D, the Arcane engine) into ONE canonical
// contract that now lives in Mosaic, the zero-dependency shared-core library.
// Astra consumes it here as Astra::IWorkScheduler -- a using-alias, NOT a second
// copy: one definition, no drift. Astra deliberately creates NO threads; when no
// scheduler is provided (Registry::Config::workScheduler is null) every
// Parallel* API executes sequentially inline. Hook up the job system of your
// choice in the host application (e.g. an enkiTS-backed adapter).
//
// The reconciled ParallelFor differs from Astra's prior local seam in two ways,
// both strict upgrades: (1) the callback gained a per-lane `worker` id -- in
// [0, WorkerCount()), distinct for each concurrently-running sub-range, so a
// callback may index per-worker scratch without locking; and (2) it takes the
// callback by zero-alloc Mosaic::FunctionRef instead of const std::function&.
// See Mosaic/Jobs/WorkScheduler.hpp for the full memory-model + worker-id
// contract (happens-before in both directions, no-throw, no OS-thread migration
// mid-fn, re-entrancy). WorkerCount() is now inclusive of the calling thread
// (always >= 1) -- the batch-size denominator.

#include <Mosaic/Jobs/WorkScheduler.hpp>

#include "Base.hpp"

namespace Astra
{
    using Mosaic::IWorkScheduler;
}
