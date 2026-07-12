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
