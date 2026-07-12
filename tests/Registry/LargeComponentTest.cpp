#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <array>

namespace
{
    // ArchetypeChunkPool::DEFAULT_CHUNK_SIZE is 16 KiB (see ArchetypeChunkPool.hpp).
    // 32 KiB is twice that, so a single entity's footprint can never fit in the
    // usable chunk space no matter how alignment overhead is estimated - this
    // must fail gracefully, never overflow into a neighboring chunk.
    struct HugeComp { std::array<std::byte, 32 * 1024> bytes{}; };
}

TEST(LargeComponent, OversizedComponentFailsGracefullyNoOverflow)
{
    Astra::Registry reg;

    // Creating an entity with a component too big for a chunk must NOT corrupt
    // memory. The entity ID itself is still allocated (CreateEntity always
    // returns a live Entity), but it must never be placed into an archetype
    // whose chunk layout would overflow: the archetype refuses to accept the
    // entity, so the entity ends up with no HugeComp attached.
    Astra::Entity e = reg.CreateEntity<HugeComp>();

    EXPECT_TRUE(e.IsValid());
    EXPECT_TRUE(reg.IsValid(e));

    // Graceful outcome: the oversized component was never attached because the
    // owning archetype could never initialize a valid chunk layout for it.
    EXPECT_FALSE(reg.HasComponent<HugeComp>(e));
    EXPECT_EQ(reg.GetComponent<HugeComp>(e), nullptr);

    // A second entity with the same oversized component must hit the same
    // (now-cached) archetype and fail exactly the same way - not just on the
    // archetype's first creation attempt.
    Astra::Entity e2 = reg.CreateEntity<HugeComp>();
    EXPECT_TRUE(reg.IsValid(e2));
    EXPECT_FALSE(reg.HasComponent<HugeComp>(e2));

    // The registry must remain otherwise usable: an ordinary small-component
    // entity must still work fine after the oversized-component archetype was
    // created (proves no corruption of shared pool/archetype-manager state).
    struct Small { int x; };
    Astra::Entity ok = reg.CreateEntityWith(Small{42});
    ASSERT_TRUE(reg.HasComponent<Small>(ok));
    EXPECT_EQ(reg.GetComponent<Small>(ok)->x, 42);
}
