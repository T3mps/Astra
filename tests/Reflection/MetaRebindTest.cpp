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
