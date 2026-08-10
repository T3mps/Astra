#include <gtest/gtest.h>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Reflection/Macros.hpp>
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

// ---- Task 4: meta transitions across slot push/pop (end-to-end) -----------
// Two fresh reflected probe types, budgeted against the ComponentID ceiling:
// neither may reuse Astra_Test_ModMeta::MetaProbe (MetaRebindTest.cpp) --
// cross-file coupling through the shared MetaRegistry is order-fragile --
// nor Astra::Test::Position/Velocity, which are NOT reflected. The shared
// AstraTest binary raises ASTRA_MAX_COMPONENTS to 192 (test-project-only,
// see premake5.lua) specifically so these two new ids fit: the shipped
// default of 128 was already fully exhausted process-wide across every
// suite in this binary before these types were added.

namespace Astra_Test_ModMeta2
{
    struct ReflectedOwned { int x = 0; };
    ASTRA_REFLECT_TYPE(ReflectedOwned)
        ASTRA_REFLECT_FIELD(ReflectedOwned, x)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, MetaRebindsAcrossPushAndPop)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta2::ReflectedOwned>::Hash();

    creg->RegisterComponent<Astra_Test_ModMeta2::ReflectedOwned>();   // anonymous base
    const Astra::TypeMeta* meta = Astra::MetaRegistry::Instance().Get(hash);
    ASSERT_NE(meta, nullptr);
    const auto idR = Astra::TypeID<Astra_Test_ModMeta2::ReflectedOwned>::Value();
    ASSERT_EQ(creg->GetComponentDescriptor(idR)->meta, meta);

    {
        auto mod = Astra::ComponentModule::Open(creg, "MetaOwner");
        mod.Register<Astra_Test_ModMeta2::ReflectedOwned>();          // push: meta rebound (same address)
        EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), meta);   // address stable through push
        EXPECT_EQ(meta->fields.size(), 1u);
    }
    // Pop back to the anonymous base: its thunk re-ran, address still stable,
    // contents rebuilt, component link intact.
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), meta);
    EXPECT_EQ(meta->fields.size(), 1u);
    EXPECT_EQ(Astra::MetaRegistry::Instance().GetByComponentId(idR), meta);
    EXPECT_NE(creg->GetComponentDescriptor(idR), nullptr);            // base descriptor restored
}

namespace Astra_Test_ModMeta2
{
    struct ReflectedEphemeral { int y = 0; };   // type budget: 4th id-consuming type
    ASTRA_REFLECT_TYPE(ReflectedEphemeral)
        ASTRA_REFLECT_FIELD(ReflectedEphemeral, y)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, MetaErasedWhenSlotPopsToEmpty)
{
    // Spec §3.6 meta-lifecycle rule (plan-review finding 1): a module-owned
    // reflected component with NO shadow survivor takes its meta with it --
    // unload-before-load then reinstalls FRESH, never comparing against a
    // stale entry whose typeName views unmapped storage.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta2::ReflectedEphemeral>::Hash();

    {
        auto genN = Astra::ComponentModule::Open(creg, "EphemeralGenN");
        genN.Register<Astra_Test_ModMeta2::ReflectedEphemeral>();
        ASSERT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    }
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), nullptr);    // meta erased with the slot

    auto genN1 = Astra::ComponentModule::Open(creg, "EphemeralGenN1");
    genN1.Register<Astra_Test_ModMeta2::ReflectedEphemeral>();        // absent path: fresh install
    EXPECT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash)->fields.size(), 1u);
}

// ---- Task 5: RegisterMeta<Ts...> -- module-owned NON-component reflection --

namespace Astra_Test_ModMeta3
{
    struct PluginPrivate { double d = 0.0; };   // reflected, never a component
    ASTRA_REFLECT_TYPE(PluginPrivate)
        ASTRA_REFLECT_FIELD(PluginPrivate, d)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, RegisterMetaAdoptsAndErasesOnDestruction)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta3::PluginPrivate>::Hash();

    // The static-init drain installed it anonymously the first time any
    // MetaRegistry::Instance() ran in this binary.
    ASSERT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);

    {
        auto mod = Astra::ComponentModule::Open(creg, "PrivateMetaOwner");
        mod.RegisterMeta<Astra_Test_ModMeta3::PluginPrivate>();
        EXPECT_NE(Astra::MetaRegistry::Instance().Get(hash), nullptr);
    }
    // Owned meta erased with the handle: GetMeta/GetByName can no longer
    // reach closures that would have died with the module.
    EXPECT_EQ(Astra::MetaRegistry::Instance().Get(hash), nullptr);
}

// ---- Task 7: UnregisterModuleRange unified with the shadow owner stack ----
// Reuses Astra::Test::Position (in place of the brief's fictional
// Astra_Test_Mod::OwnedA) for the same ComponentID-budget reason as the
// Task 3 section above.

TEST(ComponentModule, RangePurgeStripsShadowsAndRestoresSurvivor)
{
    // A non-RAII "module" (simulated by an anonymous base) is shadowed by an
    // owned override; purging the OVERRIDE's address range must restore the
    // base -- and purging a range covering a SHADOWED entry must strip it
    // without touching the live one.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const auto idA = Astra::TypeID<Astra::Test::Position>::Value();

    creg->RegisterComponent<Astra::Test::Position>();            // base
    auto mod = Astra::ComponentModule::Open(creg, "PurgeVictim");
    mod.Register<Astra::Test::Position>();                       // override (live)

    // Purge the whole image (every fn pointer in this test binary lies in
    // range): strips BOTH entries -- live override AND shadowed base -- so
    // the slot must end EMPTY, not restored-to-a-purged-entry.
    const auto* liveDesc = creg->GetComponentDescriptor(idA);
    ASSERT_NE(liveDesc, nullptr);
    const void* base = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(liveDesc->defaultConstruct) & ~uintptr_t(0xFFFF));
    const size_t dropped = creg->UnregisterModuleRange(base, size_t(1) << 30);
    EXPECT_GE(dropped, 1u);
    EXPECT_EQ(creg->GetComponentDescriptor(idA), nullptr);        // no dangling restore
    mod.Reset();                                                  // idempotent after purge: no crash
}

TEST(ComponentModule, ComponentNamesDoNotGrowOnRebind)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "NameReuse");
    mod.Register<Astra::Test::Position>();
    const size_t after1 = creg->ComponentNameCount();
    mod.Register<Astra::Test::Position>();
    mod.Register<Astra::Test::Position>();
    EXPECT_EQ(creg->ComponentNameCount(), after1);                // reused by content, not appended
}
