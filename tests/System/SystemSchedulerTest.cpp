#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Health;
}

// ---- Task 1: structural-change counter accessor -----------------------------

TEST(SystemScheduler, StructuralChangeCounterIncrementsOnCreate)
{
    Astra::Registry reg;
    auto* am = reg.GetArchetypeManager();
    const uint32_t before = am->GetStructuralChangeCounter();
    (void)reg.CreateEntity<Position>();  // creates the {Position} archetype
    EXPECT_GT(am->GetStructuralChangeCounter(), before);
}

// ---- Task 2: SystemTraits pack-scan + Exclusive tag -------------------------

namespace
{
    using RW   = Astra::SystemTraits<Astra::Reads<Velocity>, Astra::Writes<Position>>;
    using WOnly = Astra::SystemTraits<Astra::Writes<Position>>;
    using WEx   = Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>;
    using ExOnly= Astra::SystemTraits<Astra::Exclusive>;

    static_assert(RW::HasTraits && !RW::RequiresExclusive);
    static_assert(std::tuple_size_v<RW::ReadsComponents>  == 1);
    static_assert(std::tuple_size_v<RW::WritesComponents> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::ReadsComponents>,  Velocity>);
    static_assert(std::is_same_v<std::tuple_element_t<0, RW::WritesComponents>, Position>);

    static_assert(!WOnly::RequiresExclusive);
    static_assert(std::tuple_size_v<WOnly::ReadsComponents> == 0);

    static_assert(WEx::RequiresExclusive);
    static_assert(std::tuple_size_v<WEx::WritesComponents> == 1);
    static_assert(std::tuple_size_v<WEx::ReadsComponents>  == 0);

    static_assert(ExOnly::RequiresExclusive && ExOnly::HasTraits);
    static_assert(std::tuple_size_v<ExOnly::ReadsComponents>  == 0);
    static_assert(std::tuple_size_v<ExOnly::WritesComponents> == 0);
}

TEST(SystemScheduler, SystemTraitsPackScanCompiles) { SUCCEED(); }
