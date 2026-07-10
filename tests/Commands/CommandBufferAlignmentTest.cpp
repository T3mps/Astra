#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Commands/CommandBuffer.hpp>

namespace
{
    struct APos { float x, y, z; };
    struct alignas(16) Align16 { float v[4]; };
}

TEST(CommandBufferAlignment, Align16SurvivesParityShiftingPredecessor)
{
    Astra::Registry reg;
    auto victim = reg.CreateEntity<APos>();

    Astra::CommandBuffer cmd(&reg);
    // DestroyEntities of 2 entities: totalSize 8+4+8=20 -> aligned 24; 24 % 16 == 8.
    // Before the fix this shifted every following command start to ≡8 (mod 16).
    Astra::Entity junk[2] = { Astra::Entity::Invalid(), Astra::Entity::Invalid() };
    cmd.DestroyEntities(std::span<const Astra::Entity>(junk, 2));

    Align16 a{}; a.v[0]=1; a.v[1]=2; a.v[2]=3; a.v[3]=4;
    cmd.AddComponent(victim, Align16(a));

    ASSERT_TRUE(cmd.Execute().IsOk());

    auto* got = reg.GetComponent<Align16>(victim);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->v[0], 1.0f);
    EXPECT_EQ(got->v[1], 2.0f);
    EXPECT_EQ(got->v[2], 3.0f);
    EXPECT_EQ(got->v[3], 4.0f);
}
