#include <gtest/gtest.h>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Reflection/Macros.hpp>
#include <Astra/Core/TypeID.hpp>

namespace Astra_Test_ModMeta
{
    struct MetaProbe { int a = 0; float b = 0.0f; };
    ASTRA_REFLECT_TYPE(MetaProbe)
        ASTRA_REFLECT_FIELD(MetaProbe, a)
        ASTRA_REFLECT_FIELD(MetaProbe, b)
    ASTRA_REFLECT_TYPE_END()
}

TEST(MetaRebind, FactoryIsRetainedPerType)
{
    // The reflect block above must have stored a rebuildable factory.
    ASSERT_TRUE(static_cast<bool>(
        Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn));
    Astra::TypeMeta rebuilt = Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn();
    EXPECT_EQ(rebuilt.typeHash, Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash());
    EXPECT_EQ(rebuilt.fields.size(), 2u);
}

TEST(MetaRebind, RebindInPlaceKeepsAddressAndLink)
{
    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash();
    const Astra::TypeMeta* before = reg.Get(hash);
    ASSERT_NE(before, nullptr);   // drained by Instance() from the reflect block

    // 9999: far above any real ComponentID -- must not stomp a live suite's
    // GetByComponentId row in the shared default-context registry.
    reg.LinkToComponent(hash, static_cast<Astra::ComponentID>(9999));

    Astra::TypeMeta fresh = Astra::Detail::MetaFactory<Astra_Test_ModMeta::MetaProbe>::fn();
    Astra::TypeMeta* after = reg.RebindInPlace(std::move(fresh));

    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after, before);                      // address stable: descriptors cache this pointer
    EXPECT_EQ(after->fields.size(), 2u);           // contents are the fresh build
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(9999)), before); // link survives (side maps keyed by hash)
}

TEST(MetaRebind, RebindInPlaceInstallsWhenAbsent)
{
    // NOTE: this test (and the 9999 link above) permanently mutates the
    // shared default-context registry -- harmless: synthetic hashes/ids no
    // real suite reads (plan-review finding 6).
    auto& reg = Astra::MetaRegistry::Instance();
    Astra::TypeMeta synthetic;
    synthetic.typeHash = 0xA110C8ED00000001ull;    // no reflect block uses this hash
    synthetic.typeName = "Astra_Test_ModMeta::SyntheticNeverReflected";
    Astra::TypeMeta* installed = reg.RebindInPlace(std::move(synthetic));
    ASSERT_NE(installed, nullptr);
    EXPECT_EQ(reg.Get(0xA110C8ED00000001ull), installed);
}

TEST(MetaRebind, DrainInstallsBaselineBinderForThisModule)
{
    // Spec 2026-09-09 §3.5 flow 1: the static-init drain ran in THIS module,
    // so MetaProbe's entry carries a refs-0, unpinned (this binary never
    // declared Resident) binder under this module's token.
    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::MetaProbe>::Hash();
    const Astra::ModuleToken me = Astra::Detail::CurrentModuleIdentity().token;
    ASSERT_NE(reg.Get(hash), nullptr);
    EXPECT_GE(reg.BinderCount(hash), 1u);
    EXPECT_EQ(reg.TopBinder(hash), me);
    EXPECT_FALSE(reg.IsPinned(hash, me));
}

namespace Astra_Test_ModMeta
{
    struct ManualProbe { int m = 0; };   // reflected by hand below; never a component (zero ids)
}

TEST(MetaRebind, ReflectTypeStoresFactoryAndInstallsBaseline)
{
    ASSERT_FALSE(static_cast<bool>(Astra::Detail::MetaFactory<Astra_Test_ModMeta::ManualProbe>::fn));
    using Probe = Astra_Test_ModMeta::ManualProbe;
    Astra::TypeMeta* meta = Astra::ReflectType<Probe>(
        [](Astra::Detail::TypeMetaBuilder<Probe>& b)
        {
            b.Field<decltype(Probe::m), &Probe::m>("m");   // what ASTRA_REFLECT_FIELD expands to
        });
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->fields.size(), 1u);
    EXPECT_TRUE(static_cast<bool>(Astra::Detail::MetaFactory<Astra_Test_ModMeta::ManualProbe>::fn));

    auto& reg = Astra::MetaRegistry::Instance();
    const uint64_t hash = Astra::TypeID<Astra_Test_ModMeta::ManualProbe>::Hash();
    EXPECT_EQ(reg.Get(hash), meta);
    EXPECT_EQ(reg.TopBinder(hash), Astra::Detail::CurrentModuleIdentity().token);
    EXPECT_EQ(reg.Refs(hash, Astra::Detail::CurrentModuleIdentity().token), 0u);

    // Idempotent: a second manual registration returns the same entry.
    Astra::TypeMeta* again = Astra::ReflectType<Probe>(
        [](Astra::Detail::TypeMetaBuilder<Probe>& b)
        {
            b.Field<decltype(Probe::m), &Probe::m>("m");
        });
    EXPECT_EQ(again, meta);
    EXPECT_EQ(reg.BinderCount(hash), 1u);
}

namespace Astra_Test_ModMeta
{
    enum class ManualMode : uint8_t { Alpha = 0, Beta = 1 };   // reflected by hand below; never a component
}

TEST(MetaRebind, ReflectEnumStoresFactoryAndInstallsBaseline)
{
    using Mode = Astra_Test_ModMeta::ManualMode;
    ASSERT_FALSE(static_cast<bool>(Astra::Detail::MetaFactory<Mode>::fn));
    Astra::TypeMeta* meta = Astra::ReflectEnum<Mode>(
        [](Astra::Detail::EnumInfoBuilder<Mode>& eb)
        {
            eb.Value("Alpha", Mode::Alpha);   // what ASTRA_REFLECT_ENUM_VALUE expands to
            eb.Value("Beta",  Mode::Beta);
        });
    ASSERT_NE(meta, nullptr);
    EXPECT_TRUE(meta->isEnum);
    ASSERT_NE(meta->enumInfo, nullptr);

    // The retained factory must rebuild the SAME enum meta -- this is the
    // b.Enum(eb.Build()) path a hot-reload rebind would run, and it has no
    // other in-tree caller.
    ASSERT_TRUE(static_cast<bool>(Astra::Detail::MetaFactory<Mode>::fn));
    Astra::TypeMeta rebuilt = Astra::Detail::MetaFactory<Mode>::fn();
    EXPECT_TRUE(rebuilt.isEnum);
    ASSERT_NE(rebuilt.enumInfo, nullptr);
    EXPECT_EQ(rebuilt.typeHash, meta->typeHash);

    auto& reg = Astra::MetaRegistry::Instance();
    EXPECT_EQ(reg.TopBinder(meta->typeHash), Astra::Detail::CurrentModuleIdentity().token);
    EXPECT_EQ(reg.Refs(meta->typeHash, Astra::Detail::CurrentModuleIdentity().token), 0u);
}
