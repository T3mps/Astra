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
