#include <gtest/gtest.h>
#include <Astra/Core/ModuleIdentity.hpp>
#include <Astra/Core/TypeContext.hpp>

// Spec 2026-09-09 §3.1: one identity per EXE/DLL image -- a per-module
// inline-function-local static whose own address is the token.

TEST(ModuleIdentity, TokenIsStableAndNonNull)
{
    const Astra::ModuleToken a = Astra::Detail::CurrentModuleIdentity().token;
    const Astra::ModuleToken b = Astra::Detail::CurrentModuleIdentity().token;
    EXPECT_NE(a, nullptr);
    EXPECT_EQ(a, b);
}

TEST(ModuleIdentity, DefaultResidencyIsTransient)
{
    // Default = today's plugin semantics; nothing changes for code that never
    // declares residency.
    Astra::SetTypeContext(nullptr);   // reset any residency a prior test declared
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Transient);
}

TEST(ModuleIdentity, ScopedOverrideRestoresOnExit)
{
    static int s_fakeImage = 0;   // any distinct address stands in for another image
    const Astra::Detail::ModuleIdentity before = Astra::Detail::CurrentModuleIdentity();
    {
        Astra::Detail::ScopedModuleIdentity scope(&s_fakeImage, Astra::ModuleResidency::Resident);
        EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().token, &s_fakeImage);
        EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Resident);
    }
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().token, before.token);
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, before.residency);
}

TEST(ModuleIdentity, SetTypeContextStampsResidency)
{
    // The residency argument is stored on the module identity BEFORE the
    // pending-meta drain runs, so drained baselines see it (§3.1). Exercised
    // through the nullptr (uninstall, no-drain) path ON PURPOSE: if this test
    // happened to be the first drain in the process, a Resident install would
    // pin every ASTRA_REFLECT_TYPE'd meta in the binary and break the
    // transient-erase tests elsewhere in the suite.
    Astra::SetTypeContext(nullptr, Astra::ModuleResidency::Resident);
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Resident);
    Astra::SetTypeContext(nullptr);   // defaulted argument resets to Transient
    EXPECT_EQ(Astra::Detail::CurrentModuleIdentity().residency, Astra::ModuleResidency::Transient);
}
