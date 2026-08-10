#include <gtest/gtest.h>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include "../TestComponents.hpp"

// Reuses the shared Astra::Test::Position/Velocity types rather than minting
// fresh ones: the test binary sits near the 128 ComponentID ceiling (see
// TestComponents.hpp's own type roster), and TypeID<T>::Value() assigns ids
// process-wide -- two brand-new types here previously tipped an unrelated,
// later-running suite (SystemSchedulerResourceConflict) over MAX_COMPONENTS,
// which Bitmap::Set treats as fatal by design. Position/Velocity are already
// registered elsewhere in this binary, so reusing them costs zero net-new ids.
namespace
{
    // Open() requires an EXPLICITLY installed TypeContext slot (spec §3.1).
    // Installing the module-default context satisfies the tripwire while
    // keeping every id identical to what TypeID<T>::Value() mints for the
    // rest of this test binary -- no cross-suite id divergence.
    struct InstalledContext
    {
        InstalledContext()  { Astra::SetTypeContext(&Astra::DefaultTypeContext()); }
        ~InstalledContext() { Astra::SetTypeContext(nullptr); }
    };
}

TEST(ComponentModule, OpenRefusesWithoutInstalledContext)
{
    ASSERT_EQ(Astra::Detail::CurrentTypeContextSlot(), nullptr);  // precondition
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "NoContext");
    EXPECT_FALSE(static_cast<bool>(mod));            // observable refusal (ENSURE logs, continues)
    mod.Register<Astra::Test::Position>();          // must be a safe no-op
    EXPECT_EQ(creg->Size(), 0u);
}

TEST(ComponentModule, OpenAndRegisterOwnsDescriptor)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "TestModule");
    ASSERT_TRUE(static_cast<bool>(mod));

    mod.Register<Astra::Test::Position, Astra::Test::Velocity>();

    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();
    const auto* desc = creg->GetComponentDescriptor(idA);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->id, idA);
    EXPECT_NE(desc->defaultConstruct, nullptr);
    EXPECT_NE(creg->GetOwner(idA), 0u);              // owned, not anonymous
    EXPECT_EQ(creg->Size(), 2u);
}

TEST(ComponentModule, AnonymousRegisterComponentIsOwnerZero)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Astra::Test::Position>();
    EXPECT_EQ(creg->GetOwner(Astra::TypeID<Astra::Test::Position>::Value()), 0u);
}

TEST(ComponentModule, OpenRefusesNullRegistry)
{
    InstalledContext ctx;
    Astra::ComponentModule mod = Astra::ComponentModule::Open(nullptr, "Null");
    EXPECT_FALSE(static_cast<bool>(mod));
}

// ---- Task 3: shadow owner stack + full Reset() removal sweep --------------
// Reuses Astra::Test::Position/Velocity (in place of the brief's fictional
// Astra_Test_Mod::OwnedA/OwnedB) for the same reason as above: no new
// ComponentIDs minted in the shared test binary.

TEST(ComponentModule, DestructionClearsOwnedSlotToCleanMiss)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();
    {
        auto mod = Astra::ComponentModule::Open(creg, "Ephemeral");
        mod.Register<Astra::Test::Position>();
        ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);
    }
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);   // clean miss, id stays reserved
    EXPECT_EQ(creg->GetOwner(idA), 0u);
}

TEST(ComponentModule, ReloadLoadBeforeUnload)
{
    // Gen N+1 registers while gen N is still alive (real PluginHost order):
    // push shadows gen N; destroying gen N later must drop its SHADOWED
    // entry and leave gen N+1's live entry untouched.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();

    auto genN = Astra::ComponentModule::Open(creg, "GenN");
    genN.Register<Astra::Test::Position>();
    const uint32_t ownerN = creg->GetOwner(idA);

    auto genN1 = Astra::ComponentModule::Open(creg, "GenN1");
    genN1.Register<Astra::Test::Position>();
    const uint32_t ownerN1 = creg->GetOwner(idA);
    EXPECT_NE(ownerN, ownerN1);

    genN.Reset();                                            // old image goes away
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);   // no unregistered window
    EXPECT_EQ(creg->GetOwner(idA), ownerN1);
}

TEST(ComponentModule, ReloadUnloadBeforeLoad)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();

    auto genN = Astra::ComponentModule::Open(creg, "GenN");
    genN.Register<Astra::Test::Position>();
    genN.Reset();
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);   // clean miss window

    auto genN1 = Astra::ComponentModule::Open(creg, "GenN1");
    genN1.Register<Astra::Test::Position>();
    EXPECT_NE(creg->GetComponentDescriptor(idA), nullptr);
}

TEST(ComponentModule, OverrideRestoresPreviousOwnerOnUnload)
{
    // Engine registers anonymously; a module overrides; module death must
    // RESTORE the anonymous base entry -- the "engine roster survives" case.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idB = Astra::TypeID<Astra::Test::Velocity>::Value();

    creg->RegisterComponent<Astra::Test::Velocity>();        // anonymous base (owner 0)
    const auto* base = creg->GetComponentDescriptor(idB);
    ASSERT_NE(base, nullptr);

    {
        auto mod = Astra::ComponentModule::Open(creg, "Override");
        mod.Register<Astra::Test::Velocity>();
        EXPECT_NE(creg->GetOwner(idB), 0u);
    }
    const auto* restored = creg->GetComponentDescriptor(idB);
    ASSERT_NE(restored, nullptr);                            // base came back
    EXPECT_EQ(restored, base);                               // SAME address: m_components[id] is stable
    EXPECT_EQ(creg->GetOwner(idB), 0u);
    EXPECT_NE(restored->defaultConstruct, nullptr);
}

TEST(ComponentModule, SameModuleReRegisterDoesNotGrowShadow)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "Twice");
    mod.Register<Astra::Test::Position>();
    mod.Register<Astra::Test::Position>();                   // in-place rebind of own entry
    mod.Reset();
    EXPECT_EQ(creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value()), nullptr);
    // (no shadow left behind: Position pops to empty, not to a stale copy of itself)
}

TEST(ComponentModule, MidStackRemovalAtDepthTwo)
{
    // Plan-review finding 2: the nontrivial ReleaseModule branch. Base
    // (anonymous) -> modA overrides -> modB overrides; destroying modA
    // (MID-stack) must not disturb modB's live entry; destroying modB then
    // restores the BASE (modA's entry is gone from the middle).
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();

    creg->RegisterComponent<Astra::Test::Position>();        // base, owner 0
    auto modA = Astra::ComponentModule::Open(creg, "DepthA");
    modA.Register<Astra::Test::Position>();
    auto modB = Astra::ComponentModule::Open(creg, "DepthB");
    modB.Register<Astra::Test::Position>();
    const uint32_t ownerB = creg->GetOwner(idA);

    modA.Reset();                                            // mid-stack removal
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);
    EXPECT_EQ(creg->GetOwner(idA), ownerB);                  // live entry untouched

    modB.Reset();                                            // pops PAST the removed middle
    ASSERT_NE(creg->GetComponentDescriptor(idA), nullptr);   // base restored
    EXPECT_EQ(creg->GetOwner(idA), 0u);
    EXPECT_NE(creg->GetComponentDescriptor(idA)->defaultConstruct, nullptr);
}

TEST(ComponentModule, MoveAndDoubleResetAreIdempotent)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto a = Astra::ComponentModule::Open(creg, "Mover");
    a.Register<Astra::Test::Position>();
    Astra::ComponentModule b = std::move(a);
    EXPECT_FALSE(static_cast<bool>(a));
    a.Reset();                                               // moved-from reset: no-op
    EXPECT_TRUE(static_cast<bool>(b));
    b.Reset();
    b.Reset();                                               // double reset: no-op
    EXPECT_EQ(creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value()), nullptr);
}
