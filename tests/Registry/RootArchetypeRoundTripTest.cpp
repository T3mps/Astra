#include <gtest/gtest.h>
#include <span>
#include <Astra/Astra.hpp>

// Task 6 (3.4.1 release-safety remediation, Phase B structural bug):
// ArchetypeManager::Serialize skipped the root (zero-component) archetype and
// Deserialize never repopulated it, so a zero-component entity became a
// dangling entity-map entry after Save/Load - invisible to iteration, and UB
// (entity-count underflow + OOB chunk access) on a later DestroyEntity.
TEST(RootArchetypeRoundTrip, ZeroComponentEntitySurvivesSaveLoad)
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(creg, {});
    Astra::Entity e = reg.CreateEntity();          // no components -> root archetype
    ASSERT_TRUE(reg.IsValid(e));

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());
    auto& reg2 = **loaded.GetValue();

    EXPECT_TRUE(reg2.IsValid(e));                  // was dangling before the fix
    reg2.DestroyEntity(e);                          // must not underflow / OOB
    EXPECT_FALSE(reg2.IsValid(e));
}
