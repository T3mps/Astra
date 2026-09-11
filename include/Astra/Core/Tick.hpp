#pragma once

#include <concepts>
#include <cstdint>

#include "Base.hpp"   // ASTRA_NODISCARD

namespace Astra
{
    /**
     * Change-detection time (spec 2026-09-10 §3.1). A Tick is a process-relative,
     * monotonically increasing counter owned by the Registry's ArchetypeManager
     * and advanced once per system group by the scheduler (or by hand via
     * Registry::AdvanceTick for unscheduled use). Tick 0 means "never": a fresh
     * chunk or entity has never been stamped. Nothing here is serialized.
     */
    using Tick = uint32_t;

    /**
     * Two ticks compared by SIGNED difference so the counter may wrap: `a` is
     * newer than `b` iff a - b, read as int32_t, is positive. Ages beyond 2^31
     * system runs misread (documented bound; a periodic clamp is a follow-up).
     * Stamped(>=1) vs never(0) is always newer; never vs never is not.
     */
    ASTRA_NODISCARD constexpr bool IsNewer(Tick a, Tick b) noexcept
    {
        return static_cast<int32_t>(a - b) > 0;
    }

    /**
     * Per-entity ticks for a change-tracked column (spec §3.2, opt-in tier):
     * `added` = the tick the component was added to this entity, `changed` = the
     * tick of the last mark. 8 bytes per entity per tracked column, carved into
     * the chunk arena beside the enableable disabled words.
     */
    struct EntityTicks
    {
        Tick added   = 0;
        Tick changed = 0;
    };

    /**
     * Anything a tick-aware view can take its "since" from: SystemContext, or a
     * test stand-in. A concept (not the concrete SystemContext) so View.hpp does
     * not have to include System/SystemContext.hpp (which includes Registry.hpp,
     * which includes View.hpp).
     */
    template<typename C>
    concept TickContext = requires(const C& c)
    {
        { c.LastRun() } -> std::convertible_to<Tick>;
    };
}
