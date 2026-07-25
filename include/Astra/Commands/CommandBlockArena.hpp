#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

#include "../Core/Base.hpp"
#include "../Core/Memory.hpp"
#include "../Core/Tlsf.hpp"

namespace Astra
{
    /**
     * Block source for CommandByteBuffer's stable segmented storage (one per
     * Registry). Hands out raw byte blocks that NEVER move until released --
     * the property the Commands C1 fix rests on.
     *
     * Threading: Acquire/Release serialize on m_mutex (per-worker
     * CommandBuffers may grow concurrently during parallel recording). The
     * mutex is never taken on the recording fast path: buffers retain their
     * blocks across Clear(), so steady-state frames make no arena calls.
     *
     * The Tlsf instance is dedicated to command blocks -- NEVER the archetype
     * chunk pool's (recording growth must not contend with structural churn).
     * Tlsf payloads are 64-byte aligned by its size-congruence law.
     */
    class CommandBlockArena
    {
    public:
        struct BlockAlloc
        {
            std::byte* ptr = nullptr;
            size_t bytes = 0;   // usable capacity (== the requested size)
        };

        CommandBlockArena() = default;
        CommandBlockArena(const CommandBlockArena&) = delete;
        CommandBlockArena& operator=(const CommandBlockArena&) = delete;

        ~CommandBlockArena()
        {
            // Command buffers must not outlive their Registry, so every block
            // is back by now; regions return to the OS wholesale.
            for (const auto& region : m_regions)
            {
                FreeMemory(region.base, region.bytes, /*usedHugePages=*/false);
            }
        }

        BlockAlloc Acquire(size_t minBytes)
        {
            std::lock_guard lock(m_mutex);
            void* p = m_tlsf.Allocate(minBytes);
            if (!p) ASTRA_UNLIKELY
            {
                if (!GrowArena(minBytes)) ASTRA_UNLIKELY
                    return {};
                p = m_tlsf.Allocate(minBytes);
                if (!p) ASTRA_UNLIKELY
                    return {};
            }
            return {static_cast<std::byte*>(p), minBytes};
        }

        void Release(void* ptr)
        {
            if (!ptr)
                return;
            std::lock_guard lock(m_mutex);
            m_tlsf.Free(ptr);
        }

    private:
        // 256KB per OS region: several worker buffers' full block ramps fit in
        // one region; tiny beside the chunk pool's arenas. Ordinary pages.
        static constexpr size_t kArenaBytes = 256 * 1024;

        struct Region
        {
            void* base;
            size_t bytes;
        };

        // Mirrors ArchetypeChunkPool::GrowArena (ArchetypeChunkPool.hpp:814),
        // minus huge pages. Caller holds m_mutex.
        bool GrowArena(size_t minBytes)
        {
            constexpr size_t kArenaOverhead = 256;   // TLSF front pad + sentinel + rounding
            ASTRA_ASSERT(minBytes + kArenaOverhead <= Tlsf::MAX_REQUEST_BYTES,
                         "command block larger than the largest request TLSF can service");
            size_t want = std::max(kArenaBytes, minBytes + kArenaOverhead);
            want = std::clamp(want, Tlsf::MIN_ARENA_BYTES, Tlsf::MAX_ARENA_BYTES);

            AllocResult r = AllocateMemory(want, CACHE_LINE_SIZE, AllocFlags::None);
            if (!r.ptr) ASTRA_UNLIKELY
                return false;
            if (!m_tlsf.AddArena(r.ptr, r.size)) ASTRA_UNLIKELY
            {
                FreeMemory(r.ptr, r.size, r.usedHugePages);
                return false;
            }
            m_regions.push_back(Region{r.ptr, r.size});
            return true;
        }

        std::mutex m_mutex;
        Tlsf m_tlsf;
        std::vector<Region> m_regions;
    };

} // namespace Astra
