#include <gtest/gtest.h>
#include <thread>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Reflection/Macros.hpp>
#include <Astra/Core/Log.hpp>
#include "../TestComponents.hpp"
#include "../Support/DiagnosticsTestGuards.hpp"

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

    // Final-review FIX 2: the RELOADED descriptor must point at the freshly
    // installed meta. The pre-fix ordering built the descriptor from a
    // Meta().Get() taken BEFORE the rebind -- on this unload-before-load path
    // the entry had just been erased, so desc.meta/visitFields were null
    // FOREVER for gen N+1 (reflection-driven serialization silently degraded).
    const auto* reloaded = creg->GetComponentDescriptor(
        Astra::TypeID<Astra_Test_ModMeta2::ReflectedEphemeral>::Value());
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->meta, Astra::MetaRegistry::Instance().Get(hash));
    EXPECT_NE(reloaded->meta, nullptr);
    EXPECT_NE(reloaded->visitFields, nullptr);
}

// ---- Final-review FIX 1: over-aligned refusal on the MODULE path ----------
// The anonymous RegisterComponent path has always refused alignment >
// CACHE_LINE_SIZE (RegistrationGuardTest); ComponentModule::RegisterOne
// discarded that refusal and installed MakeDescriptor's early-return
// descriptor -- a live slot whose function pointers were indeterminate.
// The AstraTest binary raises ASTRA_MAX_COMPONENTS to 192, so this one extra
// id-consuming type fits the budget (see the Task 4 note above).

namespace Astra_Test_ModAlign
{
    struct alignas(128) OverAligned { float v[4]; };   // > CACHE_LINE_SIZE (64)
}

TEST(ComponentModule, OverAlignedTypeIsRefusedOnModulePath)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "OverAlignedOwner");
    ASSERT_TRUE(static_cast<bool>(mod));

    mod.Register<Astra_Test_ModAlign::OverAligned>();

    const auto id = Astra::TypeID<Astra_Test_ModAlign::OverAligned>::Value();
    EXPECT_EQ(creg->GetComponentDescriptor(id), nullptr);   // clean miss, not a live slot
    EXPECT_EQ(creg->GetOwner(id), 0u);                      // never owned
    EXPECT_EQ(creg->Size(), 0u);
}

namespace Astra_Test_ModAlign
{
    // BOTH reflected AND over-aligned -- the combination the scoped re-review
    // caught. Consumes NO ComponentID (the hoisted guard refuses before the
    // mint), so it costs nothing against the 192 test ceiling.
    struct alignas(128) ReflectedOverAligned { int z = 0; };
    ASTRA_REFLECT_TYPE(ReflectedOverAligned)
        ASTRA_REFLECT_FIELD(ReflectedOverAligned, z)
    ASTRA_REFLECT_TYPE_END()
}

TEST(ComponentModule, ReflectedOverAlignedTypeNeverTouchesMetaRegistry)
{
    // Follow-up defect: with the meta phase moved BEFORE the descriptor phase,
    // a reflected + over-aligned type used to commit a live, component-LINKED
    // TypeMeta and only then have its descriptor refused. The meta and its
    // hash<->id link then outlived the module forever -- ReleaseModule skips an
    // id whose m_present bit was never set, and the guarded MetaRegistry::Erase
    // permanently refuses a component-linked hash. The alignment refusal is now
    // hoisted above every side effect.
    InstalledContext ctx;
    auto& meta = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModAlign::ReflectedOverAligned>::Hash();

    // Pre-state: the static-init drain installed this meta ANONYMOUSLY (no
    // component link). That entry must survive Register untouched.
    const Astra::TypeMeta* metaBefore = meta.Get(hash);
    ASSERT_NE(metaBefore, nullptr);
    ASSERT_EQ(meta.GetComponentId(hash), Astra::INVALID_COMPONENT);   // not linked yet

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto mod = Astra::ComponentModule::Open(creg, "ReflectedOverAlignedOwner");
    ASSERT_TRUE(static_cast<bool>(mod));

    // Two synthetic id mints straddling the Register: if Register consumed a
    // ComponentID the delta would be 2, not 1. Deliberately raw-hash calls (no
    // TypeIdentity), so they cannot collide with any real type -- and note the
    // test never calls TypeID<ReflectedOverAligned>::Value(), which would mint
    // the very id it is asserting was never minted.
    auto& tctx = Astra::DefaultTypeContext();
    const Astra::ComponentID probeA = tctx.GetOrAssignComponentID(0xA57A0A11u, "Astra_Test_ModAlign_ProbeA");
    mod.Register<Astra_Test_ModAlign::ReflectedOverAligned>();
    const Astra::ComponentID probeB = tctx.GetOrAssignComponentID(0xA57A0A12u, "Astra_Test_ModAlign_ProbeB");
    EXPECT_EQ(probeB, static_cast<Astra::ComponentID>(probeA + 1));   // no id consumed

    // Registry side: nothing installed, by-hash lookup finds nothing.
    EXPECT_EQ(creg->GetComponentDescriptorByHash(hash), nullptr);
    EXPECT_FALSE(creg->GetComponentIDFromHash(hash).IsOk());
    EXPECT_EQ(creg->Size(), 0u);

    // Meta side: the drain-installed entry is untouched and STILL UNLINKED.
    EXPECT_EQ(meta.Get(hash), metaBefore);
    EXPECT_EQ(meta.GetComponentId(hash), Astra::INVALID_COMPONENT);
    EXPECT_EQ(meta.GetTypeHash(probeA), 0u);                          // no stray reverse link

    mod.Reset();                                                      // nothing to erase or restore
    EXPECT_EQ(meta.Get(hash), metaBefore);                            // survives teardown intact
    EXPECT_EQ(meta.GetComponentId(hash), Astra::INVALID_COMPONENT);
}

TEST(ComponentModule, InstallOwnedRefusesOverAlignedDescriptor)
{
    // Defense in depth: the public entry point repeats the guard, so a
    // hand-built descriptor that never went through MakeDescriptor is refused
    // too.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentDescriptor desc{};
    desc.id = 0;
    desc.size = 64;
    desc.alignment = Astra::CACHE_LINE_SIZE * 2;
    EXPECT_EQ(creg->InstallOwned(0, 1u, desc, Astra::Detail::CurrentModuleIdentity().token), Astra::InstallResult::Refused);
    EXPECT_EQ(creg->Size(), 0u);
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

// ---- Task 8: concurrency smoke + refusal-path unit test -------------------
// Reuses Astra::Test::Position/Velocity in place of the brief's fictional
// Astra_Test_Mod::OwnedA/OwnedB, for the same ComponentID-budget reason as
// every other section in this file.

TEST(ComponentModule, ConcurrentRegisterFromTwoModules)
{
    // Two threads, two handles, disjoint types, one shared registry: both
    // threads repeatedly take m_registrationMutex (InstallOwned) at the same
    // time. This does not prove lock-freedom of anything -- it proves the
    // registration path has no torn-state/crash under real concurrent
    // contention, which the rest of the suite (single-threaded) cannot.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    auto m1 = Astra::ComponentModule::Open(creg, "T1");
    auto m2 = Astra::ComponentModule::Open(creg, "T2");
    ASSERT_TRUE(static_cast<bool>(m1));
    ASSERT_TRUE(static_cast<bool>(m2));

    std::thread t1([&] { for (int i = 0; i < 100; ++i) m1.Register<Astra::Test::Position>(); });
    std::thread t2([&] { for (int i = 0; i < 100; ++i) m2.Register<Astra::Test::Velocity>(); });
    t1.join();
    t2.join();

    EXPECT_NE(creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::Position>::Value()), nullptr);
    EXPECT_NE(creg->GetComponentDescriptor(Astra::TypeID<Astra::Test::Velocity>::Value()), nullptr);
}

TEST(ComponentModule, InstallOwnedRefusesInvalidId)
{
    // Spec §4 item 5: the module path must never own a refused id.
    // INVALID_COMPONENT cannot be fabricated through a public collision in
    // one TU (that is Theme E's territory, exercised process-wide elsewhere),
    // so exercise ComponentRegistry::InstallOwned's own guard directly --
    // the same branch ComponentModule::RegisterOne's early-return relies on.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Astra::ComponentDescriptor desc{};
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    EXPECT_EQ(creg->InstallOwned(Astra::INVALID_COMPONENT, 1u, desc, me), Astra::InstallResult::Refused);
    EXPECT_EQ(creg->InstallOwned(static_cast<Astra::ComponentID>(Astra::MAX_COMPONENTS), 1u, desc, me), Astra::InstallResult::Refused);
    EXPECT_EQ(creg->Size(), 0u);
}

// ============================================================================
// Birth-context affinity (2026-08-10): a registry constructed under one
// TypeContext must refuse (all-config, registration paths) or assert (Debug,
// hot accessors) when touched from a module whose ambient context differs --
// the cross-module aliasing class (unpinned host exe minting foreign ids).
// A second TypeContext stands in for "another module's default context";
// NOTHING is ever minted into it, so no cross-suite state is disturbed.
// ============================================================================

TEST(ComponentModule, RegisterComponentRefusesForeignContext)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();   // birth = installed default ctx

    Astra::TypeContext foreign;                                  // "another module's" context
    Astra::SetTypeContext(&foreign);
    creg->RegisterComponent<Astra::Test::Position>();            // ambient != birth -> refused
    Astra::SetTypeContext(&Astra::DefaultTypeContext());         // restore before asserting

    EXPECT_EQ(creg->Size(), 0u);
    creg->RegisterComponent<Astra::Test::Position>();            // matching context: works
    EXPECT_EQ(creg->Size(), 1u);
}

TEST(ComponentModule, OpenRefusesForeignContext)
{
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();    // birth = installed default ctx

    Astra::TypeContext foreign;
    Astra::SetTypeContext(&foreign);                             // installed, but NOT creg's birth
    Astra::ComponentModule mod = Astra::ComponentModule::Open(creg, "ForeignCtx");
    Astra::SetTypeContext(&Astra::DefaultTypeContext());

    EXPECT_FALSE(static_cast<bool>(mod));                        // observable refusal
    EXPECT_EQ(creg->Size(), 0u);
}

#if defined(ASTRA_BUILD_DEBUG) || defined(ASTRA_ENABLE_ASSERTS)
TEST(ComponentModuleDeathTest, AccessorAssertsOnForeignContext)
{
    // Debug tripwire at the damage site: a hot accessor touched from a
    // mismatched-context module dies loudly instead of silently aliasing.
    InstalledContext ctx;
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Astra::Test::Position>();
    Astra::Registry reg(creg);

    Astra::TypeContext foreign;
    Astra::SetTypeContext(&foreign);
    EXPECT_DEATH((void)reg.GetComponent<Astra::Test::Position>(Astra::Entity{}),
                 "birth context");
    Astra::SetTypeContext(&Astra::DefaultTypeContext());
}
#endif

// ============================================================================
// Meta binder scoping (spec 2026-09-09 §4 tests 9-13): several registries on
// ONE context. ScopedModuleIdentity lets this single binary play a resident
// "engine" image and a transient "plugin" image -- distinct addresses are
// distinct module tokens. Reuses the two reflected component types declared
// above (zero new ids).
// ============================================================================

namespace
{
    int s_engineImage = 0;   // stand-in image anchors
    int s_pluginImage = 0;

    // Shuffle-safe precondition for the erase-on-last-release tests below:
    // leave NO MetaRegistry entry for T. The binary's static drain installs a
    // zero-ref, unpinned binder for every ASTRA_REFLECT_TYPE'd meta under the
    // default (Transient) identity, and only a module pop under that same
    // identity removes it (MetaErasedWhenSlotPopsToEmpty happens to do so, but
    // under --gtest_shuffle nothing guarantees it ran first). So each test pops
    // it for itself: acquire + release under the default identity, exactly the
    // path that test proves. Returns whether the entry is now absent; a false
    // means some OTHER binder still holds T -- a real leak, not an ordering
    // accident. Call it BEFORE any ScopedModuleIdentity in the test: the binder
    // to pop belongs to the default token.
    template<typename T>
    ASTRA_NODISCARD bool PopDrainedMeta()
    {
        auto& meta = Astra::MetaRegistry::Instance();
        const uint64_t hash = Astra::TypeID<T>::Hash();
        if (meta.Get(hash) != nullptr)
        {
            auto scratch = std::make_shared<Astra::ComponentRegistry>();
            auto pop = Astra::ComponentModule::Open(scratch, "PopDrainedMeta");
            if (!static_cast<bool>(pop)) return false;   // Open refused (no context installed): fail loudly, not "still held"
            pop.Register<T>();
        }                                        // pop dies: last ref released, entry erased
        return meta.Get(hash) == nullptr;
    }
}

// ORDERING GUARD for every test below that uses these anchors: declare the
// InstalledContext fixture BEFORE any ScopedModuleIdentity. SetTypeContext
// drains this binary's pending static metas into the context, and the drain
// records CurrentModuleIdentity() -- so were this fixture ever the process's
// first drain while a Resident identity was in scope, every static meta in
// the binary would get a PINNED binder under a stand-in token and could
// never be erased, silently breaking the erase-on-last-release tests.

TEST(ComponentModule, ResidentEngineRosterSurvivesEveryRegistryTeardown)
{
    // Arcane shape: ~N short-lived Runtimes, each with its own registry and
    // its own engine handle, against one process-wide context. After the
    // LAST handle goes, registry-less GetMeta must still resolve. Both
    // teardown orders.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    // A token PRIVATE to this test: refs are keyed by (hash, token), and the
    // anonymous registrations other tests make under s_engineImage are never
    // released (RegistryDestructionReleasesNothing), so sharing that token
    // would make the absolute ref counts below depend on run order.
    static int s_rosterImage = 0;
    Astra::Detail::ScopedModuleIdentity engine(&s_rosterImage, Astra::ModuleResidency::Resident);
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();

    for (int order = 0; order < 2; ++order)
    {
        auto regA = std::make_shared<Astra::ComponentRegistry>();
        auto regB = std::make_shared<Astra::ComponentRegistry>();
        auto hA = Astra::ComponentModule::Open(regA, "EngineA");
        auto hB = Astra::ComponentModule::Open(regB, "EngineB");
        ASSERT_TRUE(static_cast<bool>(hA));
        ASSERT_TRUE(static_cast<bool>(hB));
        hA.Register<T>();
        hB.Register<T>();
        const Astra::TypeMeta* address = meta.Get(hash);
        ASSERT_NE(address, nullptr);
        EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
        EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);
        EXPECT_EQ(meta.Refs(hash, &s_rosterImage), 2u);
        EXPECT_TRUE(meta.IsPinned(hash, &s_rosterImage));

        Astra::ComponentModule& first  = (order == 0) ? hA : hB;
        Astra::ComponentModule& second = (order == 0) ? hB : hA;
        const std::shared_ptr<Astra::ComponentRegistry>& survivor = (order == 0) ? regB : regA;

        first.Reset();
        EXPECT_EQ(meta.Get(hash), address);                               // Held: still there, same address
        EXPECT_EQ(survivor->GetComponentDescriptor(id)->meta, address);   // the other registry is unharmed
        EXPECT_EQ(meta.Refs(hash, &s_rosterImage), 1u);

        second.Reset();
        EXPECT_EQ(meta.Get(hash), address);                               // Retained: pinned at zero refs
        EXPECT_EQ(meta.Refs(hash, &s_rosterImage), 0u);
        // Retained BECAUSE pinned -- not the look-alike §3.6 Retained, where
        // the survivors are all null-build and nothing can rebuild the content.
        EXPECT_TRUE(meta.IsPinned(hash, &s_rosterImage));
        EXPECT_EQ(Astra::GetMeta(hash), address);                         // registry-less lookup works
        EXPECT_EQ(meta.GetByComponentId(id), address);                    // link rows intact

        auto regC = std::make_shared<Astra::ComponentRegistry>();
        auto hC = Astra::ComponentModule::Open(regC, "EngineC");
        hC.Register<T>();
        EXPECT_EQ(regC->GetComponentDescriptor(id)->meta, address);       // a third registry: same address
        EXPECT_EQ(meta.Refs(hash, &s_rosterImage), 1u);
    }                                                                     // hC/regC die here: refs back to 0
}

TEST(ComponentModule, TransientPluginMetaOutlivesFirstRegistryOnly)
{
    // The original bug: a plugin-owned type in TWO registries. The first
    // teardown must not erase what the second still caches; the second
    // teardown erases.
    using T = Astra_Test_ModMeta2::ReflectedEphemeral;
    InstalledContext ctx;
    ASSERT_TRUE(PopDrainedMeta<T>()) << "precondition: no entry for T (another binder still holds it)";
    Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();
    ASSERT_EQ(meta.Get(hash), nullptr);

    auto regA = std::make_shared<Astra::ComponentRegistry>();
    auto regB = std::make_shared<Astra::ComponentRegistry>();
    auto hA = Astra::ComponentModule::Open(regA, "PluginA");
    auto hB = Astra::ComponentModule::Open(regB, "PluginB");
    hA.Register<T>();
    hB.Register<T>();
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 2u);
    EXPECT_FALSE(meta.IsPinned(hash, &s_pluginImage));

    hA.Reset();
    EXPECT_EQ(meta.Get(hash), address);                                   // Held: B still caches it
    EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 1u);

    hB.Reset();
    EXPECT_EQ(meta.Get(hash), nullptr);                                   // Erased with the last ref
    EXPECT_EQ(meta.GetByComponentId(id), nullptr);
    EXPECT_EQ(meta.BinderCount(hash), 0u);
}

TEST(ComponentModule, OverriderDepartsWhileAnotherRegistryHolds)
{
    // Engine (resident) registers T anonymously in A and B; a transient
    // plugin overrides T in A only. When the plugin departs, A restores the
    // engine survivor, the meta rebinds to the engine's thunk, and B's cached
    // pointer never moved.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();

    auto regA = std::make_shared<Astra::ComponentRegistry>();
    auto regB = std::make_shared<Astra::ComponentRegistry>();
    {
        Astra::Detail::ScopedModuleIdentity engine(&s_engineImage, Astra::ModuleResidency::Resident);
        regA->RegisterComponent<T>();
        regB->RegisterComponent<T>();
    }
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(meta.TopBinder(hash), &s_engineImage);
    const uint32_t engineRefs = meta.Refs(hash, &s_engineImage);
    ASSERT_GE(engineRefs, 2u);

    {
        Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
        auto hP = Astra::ComponentModule::Open(regA, "PluginOverride");
        hP.Register<T>();                                                 // shadow push over the engine's slot in A
        EXPECT_EQ(meta.TopBinder(hash), &s_pluginImage);
        EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 1u);
        EXPECT_NE(regA->GetOwner(id), 0u);
        EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
    }                                                                     // plugin departs
    EXPECT_EQ(meta.TopBinder(hash), &s_engineImage);                      // Rebound to the engine's thunk
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), 0u);
    EXPECT_EQ(meta.Get(hash), address);
    EXPECT_EQ(regA->GetOwner(id), 0u);                                    // engine survivor restored in A
    EXPECT_EQ(regA->GetComponentDescriptor(id)->meta, address);
    EXPECT_EQ(regB->GetComponentDescriptor(id)->meta, address);           // B never moved
    EXPECT_EQ(meta.Refs(hash, &s_engineImage), engineRefs);               // engine refs untouched
}

TEST(ComponentModule, RangePurgeReleasesEachEntryAgainstItsOwnModule)
{
    // A non-RAII plugin registered anonymously under ITS token; the host's
    // range purge must release against the PLUGIN's binder, and the meta
    // survives because other binders still hold the type.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    auto& meta = Astra::MetaRegistry::Instance();
    const uint32_t before = meta.Refs(hash, &s_pluginImage);

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    {
        Astra::Detail::ScopedModuleIdentity plugin(&s_pluginImage, Astra::ModuleResidency::Transient);
        creg->RegisterComponent<T>();
    }
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), before + 1);
    const Astra::TypeMeta* address = meta.Get(hash);
    ASSERT_NE(address, nullptr);
    const auto* liveDesc = creg->GetComponentDescriptor(id);
    ASSERT_NE(liveDesc, nullptr);
    const void* base = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(liveDesc->defaultConstruct) & ~uintptr_t(0xFFFF));
    const size_t dropped = creg->UnregisterModuleRange(base, size_t(1) << 30);
    EXPECT_GE(dropped, 1u);
    EXPECT_EQ(creg->GetComponentDescriptor(id), nullptr);
    EXPECT_EQ(meta.Refs(hash, &s_pluginImage), before);                   // released against the plugin's binder
    EXPECT_EQ(meta.Get(hash), address);                                   // other binders still hold: not erased
}

TEST(ComponentModule, RegistryDestructionReleasesNothing)
{
    // Spec 2026-09-09 §3.6 (c): anonymous entries outlive their registry
    // exactly as before -- Arcane's registry-less GetMeta readers depend on
    // it. Over-holding is the safe direction.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    auto& meta = Astra::MetaRegistry::Instance();
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    const uint32_t before = meta.Refs(hash, me);
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        creg->RegisterComponent<T>();
        EXPECT_EQ(meta.Refs(hash, me), before + 1);
    }                                                                     // registry dies WITHOUT releasing
    EXPECT_EQ(meta.Refs(hash, me), before + 1);
    EXPECT_NE(Astra::GetMeta(hash), nullptr);
}

TEST(ComponentModule, InstallOwnedRefusesSameOwnerMetaNullnessFlip)
{
    // Final-review wave: a same-owner in-place Replaced takes NO new meta ref
    // because the slot's meta nullness is assumed unchanged. It CAN flip --
    // mod.Register<T>() before T has a factory, a runtime ReflectType<T>, then
    // Register<T>() again -- and after that RestoreOrClearSlot emits a release
    // nobody acquired. Harmless inside one registry (ENSURE + Unbound), but
    // the stray decrement is keyed by the same MODULE TOKEN, so it can drop
    // ANOTHER registry's ref and erase a meta that is still cached. Refused
    // all-config, with the slot left untouched.
    //
    // Driven through InstallOwned directly with two hand-built descriptors:
    // reaching the flip through ComponentModule::Register would need a type
    // whose factory appears mid-test, and the binary has no ComponentID
    // budget for a fresh one.
    using T = Astra_Test_ModMeta2::ReflectedOwned;
    InstalledContext ctx;
    const uint64_t hash = Astra::TypeID<T>::Hash();
    const auto id = Astra::TypeID<T>::Value();
    const Astra::TypeMeta* live = Astra::MetaRegistry::Instance().Get(hash);
    ASSERT_NE(live, nullptr) << "precondition: T's static reflection has drained into this context";

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    const uint32_t owner = creg->OpenModuleId("NullnessFlip");
    int image = 0;                                   // stand-in module token

    Astra::ComponentDescriptor unreflected{};
    unreflected.id = id;
    unreflected.hash = hash;
    unreflected.size = sizeof(T);
    unreflected.alignment = alignof(T);
    unreflected.name = "NullnessFlipProbe";
    unreflected.meta = nullptr;                      // registered before the factory existed
    ASSERT_EQ(creg->InstallOwned(id, owner, unreflected, &image), Astra::InstallResult::Installed);
    ASSERT_NE(creg->GetComponentDescriptor(id), nullptr);
    ASSERT_EQ(creg->GetComponentDescriptor(id)->meta, nullptr);

    Astra::ComponentDescriptor reflected = unreflected;
    reflected.meta = live;                           // same owner, nullness FLIPPED

    // ENSURE logs and continues: that line IS the observable refusal.
    EXPECT_EQ(creg->InstallOwned(id, owner, reflected, &image), Astra::InstallResult::Refused);
    ASSERT_NE(creg->GetComponentDescriptor(id), nullptr);
    EXPECT_EQ(creg->GetComponentDescriptor(id)->meta, nullptr);   // slot untouched
    EXPECT_EQ(creg->GetOwner(id), owner);

    // The unflipped same-owner replace still works.
    EXPECT_EQ(creg->InstallOwned(id, owner, unreflected, &image), Astra::InstallResult::Replaced);
    EXPECT_EQ(creg->GetComponentDescriptor(id)->meta, nullptr);
}

namespace
{
    // Captures "overrides" notices by level: a same-name shadow-push must NOT
    // surface at info (it reads as a conflict); a different-name override must.
    struct OverrideCapture { int info = 0; int belowInfo = 0; };
    void OverrideSink(const Astra::LogRecord& r, void* user) noexcept
    {
        auto* c = static_cast<OverrideCapture*>(user);
        if (std::string_view(r.message).find("overrides") == std::string_view::npos) return;
        if (r.level == Astra::LogLevel::Info) c->info++;
        else                                  c->belowInfo++;   // trace/debug (if delivered)
    }

    // RAII for the process-wide runtime log level (ScopedLogSink restores the sink
    // but not the level; leaving it raised would perturb other shuffled tests).
    struct ScopedLogLevel
    {
        Astra::LogLevel prev;
        explicit ScopedLogLevel(Astra::LogLevel l) : prev(Astra::GetLogLevel()) { Astra::SetLogLevel(l); }
        ~ScopedLogLevel() { Astra::SetLogLevel(prev); }
    };
}

// Two DISTINCT module handles with the SAME name on ONE shared ComponentRegistry
// -- a second Runtime/world opening its own "Arcane" engine roster. The second
// Register takes InstallOwned's different-owner branch (distinct owner ids) but
// both resolve to the same module name: the designed shadow-push, which must be
// logged at trace, not info, so it does not read as a conflict. A genuinely
// different-named override is a real conflict and stays at info.
TEST(ComponentModule, SameNameOverrideLogsBelowInfoDifferentNameStaysInfo)
{
    InstalledContext ctx;
    ScopedLogLevel level(Astra::LogLevel::Trace);   // deliver everything the compile floor allows
    OverrideCapture cap;
    Astra::Testing::ScopedLogSink sink(&OverrideSink, &cap);

    auto creg = std::make_shared<Astra::ComponentRegistry>();

    auto first = Astra::ComponentModule::Open(creg, "Same");
    ASSERT_TRUE(static_cast<bool>(first));
    first.Register<Astra::Test::Position>();

    auto second = Astra::ComponentModule::Open(creg, "Same");   // same name, distinct handle
    ASSERT_TRUE(static_cast<bool>(second));
    second.Register<Astra::Test::Position>();                   // same-name override -> NOT info
    EXPECT_EQ(cap.info, 0) << "a same-name shadow-push must not surface as an info-level override";

    auto other = Astra::ComponentModule::Open(creg, "Other");   // different name
    ASSERT_TRUE(static_cast<bool>(other));
    other.Register<Astra::Test::Position>();                    // different-name override -> info
    EXPECT_GE(cap.info, 1) << "a different-name override is a real conflict and stays at info";
}
