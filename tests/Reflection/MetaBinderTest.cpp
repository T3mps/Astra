#include <gtest/gtest.h>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Core/ModuleIdentity.hpp>

// Spec 2026-09-09 §3.2/§3.3: the binder stack. Every test uses a LOCAL
// MetaRegistry (not Instance()) and a synthetic hash, so nothing here touches
// the shared default-context registry or consumes a ComponentID. "Content"
// is observed through fields.size(), which the identity check ignores.

namespace
{
    constexpr uint64_t kHash = 0xB1BDE50000000001ull;

    // Distinct addresses stand in for distinct module images.
    int s_imageA = 0;
    int s_imageB = 0;
    int s_imageC = 0;

    Astra::TypeMeta Make(size_t fieldCount)
    {
        Astra::TypeMeta m{};   // value-init: TypeMeta has no member initializers
        m.typeHash  = kHash;
        m.typeName  = "Astra_Test_Binder::Probe";
        m.size      = 8;
        m.alignment = 4;
        m.isTrivial = true;
        m.fields.resize(fieldCount);
        return m;
    }
    Astra::TypeMeta BuildOne()   { return Make(1); }
    Astra::TypeMeta BuildTwo()   { return Make(2); }
    Astra::TypeMeta BuildThree() { return Make(3); }

    Astra::Detail::ModuleIdentity Who(const void* token,
                                      Astra::ModuleResidency r = Astra::ModuleResidency::Transient)
    {
        return Astra::Detail::ModuleIdentity{ token, r };
    }

    // For the re-entrancy test: a build thunk that pushes a THIRD module on
    // top while Release is rebuilding for the module it thought was top.
    Astra::MetaRegistry* g_reentrantTarget = nullptr;
    Astra::TypeMeta ReentrantBuild()
    {
        g_reentrantTarget->Bind(kHash, Who(&s_imageC), &BuildTwo, nullptr);
        return Make(1);
    }
}

TEST(MetaBinder, BindInstallsThenSecondModuleTopsAndReleaseRebinds)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    const Astra::BindResult a = reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one);
    ASSERT_EQ(a.outcome, Astra::BindOutcome::Bound);
    ASSERT_NE(a.meta, nullptr);
    EXPECT_EQ(a.meta->fields.size(), 1u);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);

    Astra::TypeMeta two = BuildTwo();
    const Astra::BindResult b = reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two);
    ASSERT_EQ(b.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(b.meta, a.meta);                       // address stable across binders
    EXPECT_EQ(a.meta->fields.size(), 2u);            // content = top binder (B)
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(reg.BinderCount(kHash), 2u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.Get(kHash), a.meta);               // same address
    EXPECT_EQ(a.meta->fields.size(), 1u);            // content rebuilt from A's thunk
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Get(kHash), nullptr);
}

TEST(MetaBinder, RefsHoldThenLastReleaseErasesWithLinkRows)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));      // two registry slots
    reg.LinkToComponent(kHash, static_cast<Astra::ComponentID>(77));
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 2u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Held);
    EXPECT_NE(reg.GetByComponentId(static_cast<Astra::ComponentID>(77)), nullptr);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 1u);

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Get(kHash), nullptr);
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(77)), nullptr);
    EXPECT_EQ(reg.GetComponentId(kHash), Astra::INVALID_COMPONENT);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 0u);
}

TEST(MetaBinder, PinnedBinderIsRetainedAndOverriderRebindsBackToIt)
{
    Astra::MetaRegistry reg;
    // Resident "engine" drains a baseline: pinned, refs 0.
    ASSERT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA, Astra::ModuleResidency::Resident), &BuildOne, BuildOne()).outcome,
              Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.IsPinned(kHash, &s_imageA));
    const Astra::TypeMeta* address = reg.Get(kHash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->fields.size(), 1u);

    // Transient "plugin" overrides.
    Astra::TypeMeta two = BuildTwo();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(address->fields.size(), 2u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    // Plugin departs: content returns to the pinned engine thunk.
    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(address->fields.size(), 1u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);

    // Engine's own refs come and go; at zero it is RETAINED, never erased.
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Retained);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 0u);
}

TEST(MetaBinder, BindRefusesIdentityMismatchAndPushesNothing)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);

    Astra::TypeMeta impostor = BuildOne();
    impostor.size = 16;                               // same hash, different identity
    const Astra::BindResult r = reg.Bind(kHash, Who(&s_imageB), &BuildOne, &impostor);   // ENSURE logs, continues
    EXPECT_EQ(r.outcome, Astra::BindOutcome::Refused);
    EXPECT_EQ(r.meta, nullptr);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);
    EXPECT_EQ(reg.Get(kHash)->size, 8u);              // incumbent content untouched

    // Same refusal on the baseline path.
    Astra::TypeMeta impostor2 = BuildOne();
    impostor2.alignment = 8;
    EXPECT_EQ(reg.InstallBaseline(kHash, Who(&s_imageC), &BuildOne, std::move(impostor2)).outcome,
              Astra::BindOutcome::Refused);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
}

TEST(MetaBinder, BindAbsentEntryWithoutFreshIsRefused)
{
    Astra::MetaRegistry reg;
    const Astra::BindResult r = reg.Bind(kHash, Who(&s_imageA), &BuildOne, nullptr);
    EXPECT_EQ(r.outcome, Astra::BindOutcome::Refused);
    EXPECT_EQ(r.meta, nullptr);
    EXPECT_EQ(reg.Get(kHash), nullptr);
    EXPECT_EQ(reg.BinderCount(kHash), 0u);
}

TEST(MetaBinder, ReleaseOnUnknownTokenOrHashIsUnbound)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Unbound);        // no binder for B
    EXPECT_EQ(reg.Release(0xDEADull, &s_imageA), Astra::ReleaseOutcome::Unbound);    // no such entry
    EXPECT_EQ(reg.Refs(kHash, &s_imageA), 1u);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);

    // A binder at zero refs is not "held": releasing it again is a caller
    // bug, reported as Unbound (ENSURE logs, continues) and changes nothing.
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Unbound);
}

TEST(MetaBinder, AcquireWithoutBindRefuses)
{
    Astra::MetaRegistry reg;
    EXPECT_FALSE(reg.Acquire(kHash, &s_imageA));      // no entry (ENSURE logs, continues)
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_FALSE(reg.Acquire(kHash, &s_imageB));      // entry, but no binder for B
    EXPECT_EQ(reg.Refs(kHash, &s_imageB), 0u);
}

TEST(MetaBinder, ReentrantBindDuringReleaseSkipsTheStaleSwap)
{
    // Release(B) pops B, sees A as the new top, and runs A's thunk with the
    // mutex released. That thunk pushes C on top. On relock the top is no
    // longer A, so the swap MUST be skipped: content stays as it was. The
    // outcome is still Rebound (the release completed and a rebind was
    // attempted; the swap itself was superseded -- see Release's contract).
    Astra::MetaRegistry reg;
    g_reentrantTarget = &reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &ReentrantBuild, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));
    Astra::TypeMeta two = BuildTwo();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &two).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    ASSERT_EQ(reg.Get(kHash)->fields.size(), 2u);

    EXPECT_EQ(reg.Release(kHash, &s_imageB), Astra::ReleaseOutcome::Rebound);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageC);       // C landed on top during the rebuild window
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 2u);     // A's stale build was NOT swapped in
    EXPECT_EQ(reg.BinderCount(kHash), 2u);            // A, C
    g_reentrantTarget = nullptr;
}

TEST(MetaBinder, InstallBaselineOnExistingEntryPushesBottomAndKeepsContent)
{
    Astra::MetaRegistry reg;
    ASSERT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA), &BuildOne, BuildOne()).outcome, Astra::BindOutcome::Bound);
    const Astra::TypeMeta* address = reg.Get(kHash);
    ASSERT_NE(address, nullptr);

    const Astra::BindResult second = reg.InstallBaseline(kHash, Who(&s_imageB), &BuildTwo, BuildTwo());
    EXPECT_EQ(second.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(second.meta, address);
    EXPECT_EQ(address->fields.size(), 1u);            // first-wins content
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);       // B sits at the BOTTOM
    EXPECT_EQ(reg.BinderCount(kHash), 2u);

    EXPECT_EQ(reg.InstallBaseline(kHash, Who(&s_imageA), &BuildOne, BuildOne()).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.BinderCount(kHash), 2u);            // idempotent per module
}

TEST(MetaBinder, NullBuildBinderNeverTopsAndAllNullBuildReleaseRetains)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));

    // A module with no factory for this type registers it as a component:
    // it holds a ref but can never source content.
    const Astra::BindResult d = reg.Bind(kHash, Who(&s_imageB), nullptr, nullptr);
    EXPECT_EQ(d.outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageB));
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageA);       // inserted BELOW the top
    EXPECT_EQ(reg.BinderCount(kHash), 2u);

    // A departs: only a null-build binder remains. Nothing can rebuild the
    // content, so the entry is kept (address valid) and a warning is logged.
    const Astra::TypeMeta* address = reg.Get(kHash);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Retained);
    EXPECT_EQ(reg.Get(kHash), address);
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);
    EXPECT_EQ(reg.BinderCount(kHash), 1u);
}

TEST(MetaBinder, BindWithoutFreshOnNewTopReportsNeedsRebuildAndRebindHonorsTopOnly)
{
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);

    // B binds under a lock elsewhere (no thunk allowed): it lands on top and
    // is told to rebuild later.
    const Astra::BindResult b = reg.Bind(kHash, Who(&s_imageB), &BuildTwo, nullptr);
    EXPECT_EQ(b.outcome, Astra::BindOutcome::BoundNeedsRebuild);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 1u);     // content not yet B's
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageB);

    EXPECT_FALSE(reg.Rebind(kHash, &s_imageA, BuildThree()));   // A is not top: refused, no change
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 1u);
    EXPECT_TRUE(reg.Rebind(kHash, &s_imageB, BuildTwo()));      // B is top: swapped
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 2u);

    // Same-module re-bind with fresh while top: swaps (in-binary reload).
    Astra::TypeMeta three = BuildThree();
    EXPECT_EQ(reg.Bind(kHash, Who(&s_imageB), &BuildTwo, &three).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 3u);
    // Existing binder NOT top + fresh: no swap.
    Astra::TypeMeta again = BuildOne();
    EXPECT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &again).outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.Get(kHash)->fields.size(), 3u);
    EXPECT_EQ(reg.BinderCount(kHash), 2u);
}

TEST(MetaBinder, ComponentLinkedHashWithLiveRefsIsHeldNotErased)
{
    // Replaces MetaRebindTest.EraseGuardsComponentLinkedHashes: there is no
    // guarded Erase any more -- a component-linked hash simply has refs.
    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));                       // the component slot
    reg.LinkToComponent(kHash, static_cast<Astra::ComponentID>(5));
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));                       // a RegisterMeta adoption

    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Held);   // RegisterMeta teardown
    EXPECT_NE(reg.GetByComponentId(static_cast<Astra::ComponentID>(5)), nullptr);
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Erased); // slot clears
    EXPECT_EQ(reg.GetByComponentId(static_cast<Astra::ComponentID>(5)), nullptr);
}

TEST(MetaBinder, InstallBaselineRescuesAllNullBuildStack)
{
    // Fix round 1, spec 2026-09-09 §3.6: once every remaining binder is
    // null-build, first-wins bottom-insert would strand the entry with
    // unrebuildable content forever. A later InstallBaseline must instead
    // take the top and rescue the content.
    //
    // A fourth module image is needed here: A departs, D (null-build) is
    // the sole survivor, C rescues, and B installs a normal baseline
    // afterward -- all four identities must stay distinct through the test.
    int imageD = 0;

    Astra::MetaRegistry reg;
    Astra::TypeMeta one = BuildOne();
    ASSERT_EQ(reg.Bind(kHash, Who(&s_imageA), &BuildOne, &one).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &s_imageA));

    // D has no factory: lands below top, can never source content.
    ASSERT_EQ(reg.Bind(kHash, Who(&imageD), nullptr, nullptr).outcome, Astra::BindOutcome::Bound);
    EXPECT_TRUE(reg.Acquire(kHash, &imageD));

    // A departs: only D (null-build) remains -- degraded state.
    EXPECT_EQ(reg.Release(kHash, &s_imageA), Astra::ReleaseOutcome::Retained);
    const Astra::TypeMeta* address = reg.Get(kHash);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->fields.size(), 1u);
    EXPECT_EQ(reg.TopBinder(kHash), &imageD);

    // A later baseline drain rescues the entry: takes the TOP, not the
    // bottom, and swaps in its own content.
    const Astra::BindResult rescue = reg.InstallBaseline(kHash, Who(&s_imageC), &BuildTwo, BuildTwo());
    EXPECT_EQ(rescue.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(reg.Get(kHash), address);               // same address
    EXPECT_EQ(address->fields.size(), 2u);             // content rescued
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageC);
    EXPECT_EQ(reg.BinderCount(kHash), 2u);

    // Normal path is unchanged: with a non-null-build binder on top, a new
    // baseline still goes to the bottom and does not disturb content.
    const Astra::BindResult normal = reg.InstallBaseline(kHash, Who(&s_imageB), &BuildThree, BuildThree());
    EXPECT_EQ(normal.outcome, Astra::BindOutcome::Bound);
    EXPECT_EQ(address->fields.size(), 2u);             // untouched
    EXPECT_EQ(reg.TopBinder(kHash), &s_imageC);
    EXPECT_EQ(reg.BinderCount(kHash), 3u);
}
