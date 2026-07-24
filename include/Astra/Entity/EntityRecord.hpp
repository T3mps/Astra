#pragma once

#include <type_traits>

#include "../Archetype/EntityLocation.hpp"
#include "Entity.hpp"

namespace Astra
{
    class Archetype;        // forward declarations — EntityRecord only stores pointers
    class ArchetypeChunk;

    // Unified entity slot: liveness (version) + storage location (archetype + location),
    // co-located so a single paged lookup can both validate a handle and locate it.
    // version == 0 (EntityTable::NULL_VERSION) marks a dead/empty slot; a slot is
    // "located" iff version matches the handle AND archetype != nullptr.
    //
    // `chunk` caches archetype->GetChunks()[location.GetChunkIndex()].get() so the
    // get hot path skips the chunks-vector hop. INVARIANT: whenever archetype != nullptr
    // and location.IsValid(), chunk points at that exact chunk. ALL writes to
    // archetype/chunk/location must go through EntityTable::SetRecord or
    // ArchetypeManager's SetRecordLocation/ClearRecordLocation helpers — never
    // assign the fields directly, or the cached pointer goes stale (UAF).
    // alignas(32): a 32B record must never straddle a cache line.
    struct alignas(32) EntityRecord
    {
        Archetype*           archetype = nullptr;  // null ⇒ no location assigned
        ArchetypeChunk*      chunk     = nullptr;  // cached chunk for the location
        EntityLocation       location;             // {chunkIndex, entityIndex}
        Entity::VersionType  version   = 0;        // 0 ⇒ dead/empty (NULL_VERSION)
    };

    static_assert(std::is_trivially_copyable_v<EntityRecord>,
        "EntityRecord must be trivially copyable so the paged table can memcpy/relocate slots");
    static_assert(std::is_trivially_destructible_v<EntityRecord>,
        "EntityRecord must be trivially destructible for cheap segment teardown");
}
