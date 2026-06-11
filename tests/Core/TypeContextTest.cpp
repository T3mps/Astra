#include <gtest/gtest.h>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>  // completes TypeContext::Meta()

namespace
{
    struct CtxA { int v; };
    struct CtxB { float v; };

    TEST(TypeContext, AssignsDenseSequentialIds)
    {
        Astra::TypeContext ctx;
        EXPECT_EQ(ctx.GetOrAssignComponentID(111, "T1"), 0u);
        EXPECT_EQ(ctx.GetOrAssignComponentID(222, "T2"), 1u);
        EXPECT_EQ(ctx.GetOrAssignComponentID(333, "T3"), 2u);
    }

    TEST(TypeContext, SameHashSameId)
    {
        Astra::TypeContext ctx;
        const auto id = ctx.GetOrAssignComponentID(42, "T");
        EXPECT_EQ(ctx.GetOrAssignComponentID(42, "T"), id);
    }

    TEST(TypeContext, IndependentContextsAssignIndependently)
    {
        Astra::TypeContext a, b;
        EXPECT_EQ(a.GetOrAssignComponentID(7, "X"), 0u);
        (void)a.GetOrAssignComponentID(8, "Y");
        EXPECT_EQ(b.GetOrAssignComponentID(7, "X"), 0u);  // b never saw Y
    }

    TEST(TypeContext, TypeIdRoutesThroughCurrentContext)
    {
        const auto a1 = Astra::TypeID<CtxA>::Value();
        const auto a2 = Astra::TypeID<CtxA>::Value();
        const auto b1 = Astra::TypeID<CtxB>::Value();
        EXPECT_EQ(a1, a2);
        EXPECT_NE(a1, b1);
        // The active context resolves the stable hash back to the same id.
        EXPECT_EQ(Astra::GetTypeContext()->GetOrAssignComponentID(
                      Astra::TypeID<CtxA>::Hash(), Astra::TypeID<CtxA>::Name()),
                  a1);
    }

    TEST(TypeContext, MetaRegistryLivesInContext)
    {
        Astra::TypeContext ctx;
        ctx.Meta().Register(999u, "Fake");
        EXPECT_NE(ctx.Meta().Get(999u), nullptr);
        Astra::TypeContext other;
        EXPECT_EQ(other.Meta().Get(999u), nullptr);
    }

    TEST(TypeContext, PendingRegistrationsDrainIntoInstalledContext)
    {
        // Flush any static-registrar enqueues from OTHER translation units into
        // the currently active context first, so the simulated plugin enqueue
        // below is the only thing the install drains (otherwise this test
        // would steal in-tree reflection registrations into a throwaway
        // context and break later reflection tests).
        (void)Astra::MetaRegistry::Instance();

        // Simulates a plugin DLL: a static registrar enqueues BEFORE the host
        // installs the shared context; SetTypeContext must drain into it.
        Astra::Detail::PendingMetaQueue().emplace_back([](Astra::TypeContext& c)
        {
            c.Meta().Register(123456u, "QueuedType");
        });
        Astra::TypeContext fresh;
        Astra::TypeContext* prev = Astra::GetTypeContext();
        Astra::SetTypeContext(&fresh);
        EXPECT_NE(fresh.Meta().Get(123456u), nullptr);
        Astra::SetTypeContext(prev);  // restore for other tests
        EXPECT_EQ(prev->Meta().Get(123456u), nullptr);
    }
}
