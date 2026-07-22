#pragma once

#include <type_traits>

#include "../Archetype/EntityLocation.hpp"
#include "Entity.hpp"

namespace Astra
{
    class Archetype;  // forward declaration — EntityRecord only stores the pointer

    // Unified entity slot: liveness (version) + storage location (archetype + location),
    // co-located so a single paged lookup can both validate a handle and locate it.
    // version == 0 (EntityTable::NULL_VERSION) marks a dead/empty slot; a slot is
    // "located" iff version matches the handle AND archetype != nullptr.
    struct EntityRecord
    {
        Archetype*           archetype = nullptr;  // null ⇒ no location assigned
        EntityLocation       location;             // {chunkIndex, entityIndex}
        Entity::VersionType  version   = 0;        // 0 ⇒ dead/empty (NULL_VERSION)
    };

    static_assert(std::is_trivially_copyable_v<EntityRecord>,
        "EntityRecord must be trivially copyable so the paged table can memcpy/relocate slots");
    static_assert(std::is_trivially_destructible_v<EntityRecord>,
        "EntityRecord must be trivially destructible for cheap segment teardown");
}
