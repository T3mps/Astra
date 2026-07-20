#include <gtest/gtest.h>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>  // completes TypeContext::Meta()

#include "../Support/DiagnosticsTestGuards.hpp"

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

namespace  // Theme E collision-detection capture
{
    struct ECapture { int errors = 0; };
    inline void ECaptureSink(const Astra::LogRecord& r, void* user) noexcept
    {
        if (r.level == Astra::LogLevel::Error) static_cast<ECapture*>(user)->errors++;
    }
    Astra::TypeIdentity MakeStructId(uint32_t size, uint32_t align, uint8_t flags = 0)
    {
        Astra::TypeIdentity id; id.size = size; id.align = align; id.flags = flags; return id;
    }
}

// Class 1: same name-hash, same name, but a provably different type (differing
// structural identity, as two distinct anonymous-namespace types would) -> refused.
TEST(TypeContextCollision, SameNameDistinctIdentityIsRefused)
{
    ECapture cap;
    Astra::Testing::ScopedLogSink sink(&ECaptureSink, &cap);
    Astra::Testing::ScopedAssertHandler handler(nullptr);  // default handler routes through the sink
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    ASSERT_NE(id0, Astra::INVALID_COMPONENT);
    const auto id1 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(16, 8));  // different size => different type
    EXPECT_EQ(id1, Astra::INVALID_COMPONENT);
    EXPECT_GE(cap.errors, 1);
}

// Class 2: same name-hash but a DIFFERENT name (a real XXHash collision of two
// differently-named types) -> refused, in all configs (upgrades the old
// Debug-only name-assert to a graceful all-config refuse).
TEST(TypeContextCollision, DifferentNameSameHashIsRefused)
{
    ECapture cap;
    Astra::Testing::ScopedLogSink sink(&ECaptureSink, &cap);
    Astra::Testing::ScopedAssertHandler handler(nullptr);
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(4321, "A");
    ASSERT_NE(id0, Astra::INVALID_COMPONENT);
    const auto id1 = ctx.GetOrAssignComponentID(4321, "B");  // same hash, different name
    EXPECT_EQ(id1, Astra::INVALID_COMPONENT);
    EXPECT_GE(cap.errors, 1);
}

// Non-collision: identical name + identity re-registration is idempotent.
TEST(TypeContextCollision, SameNameSameIdentityIsIdempotent)
{
    Astra::TypeContext ctx;
    const auto id0 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    const auto id1 = ctx.GetOrAssignComponentID(1234, "Foo", MakeStructId(8, 8));
    EXPECT_EQ(id0, id1);
    EXPECT_NE(id0, Astra::INVALID_COMPONENT);
}
