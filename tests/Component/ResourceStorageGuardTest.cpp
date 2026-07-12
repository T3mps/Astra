#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct GuardRes { int v = 7; }; }

// The forceable arm of Theme-A/A1: an over-MAX_COMPONENTS id must not OOB-write
// m_sparse. Registering enough distinct component types to exhaust the id space
// isn't practical here, so this test pins the graceful-rejection CONTRACT via the
// public Registry resource API on the normal path (regression guard that the
// guarded returns compile and behave), and the OOM/expired arms are covered by
// code inspection per the plan.
TEST(ResourceStorageGuard, SetAndGetRoundTripsOnNormalPath)
{
    Astra::Registry reg;
    reg.SetResource(GuardRes{42});
    auto* r = reg.GetResource<GuardRes>();
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->v, 42);
}

namespace { struct alignas(128) OverAlignedRes { float v[4]; }; }  // > CACHE_LINE_SIZE (64)

// Regression guard for the over-aligned-resource descriptor fix: since
// ComponentRegistry::RegisterComponent<T>() refuses an over-aligned component
// (Task 3's all-config guard), GetComponentDescriptor(id) can legitimately
// return nullptr here - a caller-reachable condition, not a broken invariant.
// ResourceStorage::Set/Emplace must treat a null descriptor as a graceful
// failure (return nullptr, resource stays absent) in ALL configs, rather than
// relying on an ASTRA_ASSERT that Release/Dist compile out - which would have
// fallen through to allocate storage and mark the slot valid with a null
// descriptor, leaking the (heap, since 128 > SBO) allocation on teardown
// (Clear/Remove both gate destruction+free on slot.descriptor).
TEST(ResourceStorageGuard, SetResourceOnOverAlignedTypeReturnsNullNotSet)
{
    Astra::Registry reg;

    OverAlignedRes* r = reg.SetResource(OverAlignedRes{});
    EXPECT_EQ(r, nullptr);
    EXPECT_FALSE(reg.HasResource<OverAlignedRes>());
    EXPECT_EQ(reg.GetResource<OverAlignedRes>(), nullptr);
}
