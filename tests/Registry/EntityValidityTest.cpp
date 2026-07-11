#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace
{
    struct EPos
    {
        float x, y, z;
    };
}

TEST(EntityValidity, ArchetypeManagerRejectsInvalidEntity)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<EPos>();

    // This is exactly what Registry::CreateEntity does after an exhausted
    // EntityManager returns Entity::Invalid().
    reg.GetArchetypeManager()->AddEntity<EPos>(Astra::Entity::Invalid());

    EXPECT_EQ(reg.GetArchetypeManager()->GetEntityRecord(Astra::Entity::Invalid()), nullptr);

    size_t seen = 0;
    bool sawInvalid = false;
    reg.CreateView<EPos>().ForEach([&](Astra::Entity e, EPos&) {
        ++seen;
        if (!e.IsValid() || !reg.IsValid(e)) sawInvalid = true;
    });
    EXPECT_EQ(seen, 0u);
    EXPECT_FALSE(sawInvalid);
}

TEST(EntityValidity, BatchAddComponentsIgnoresDeadHandles)
{
    Astra::Registry reg;
    auto e = reg.CreateEntity<EPos>();
    reg.DestroyEntity(e);
    ASSERT_FALSE(reg.IsValid(e));

    Astra::Entity arr[1] = { e };
    reg.AddComponents(std::span<Astra::Entity>(arr, 1), EPos{9, 9, 9});

    size_t seen = 0;
    reg.CreateView<EPos>().ForEach([&](Astra::Entity, EPos&) { ++seen; });
    EXPECT_EQ(seen, 0u);
    EXPECT_FALSE(reg.HasComponent<EPos>(e));
}

TEST(EntityValidity, CreateBatchReportsCount)
{
    Astra::EntityManager em;
    std::vector<Astra::Entity> out(100);
    std::size_t created = em.CreateBatch(100, out.begin());
    EXPECT_EQ(created, 100u);
    for (auto& e : out)
    {
        EXPECT_TRUE(e.IsValid());
    }
}
