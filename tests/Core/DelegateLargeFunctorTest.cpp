#include <gtest/gtest.h>
#include <Astra/Core/Delegate.hpp>

namespace
{
    static int g_alive = 0;
    struct BigFunctor
    {
        char payload[64];   // > SMALL_BUFFER_SIZE (32) => heap path
        int id;
        BigFunctor(int i) : payload{}, id(i) { ++g_alive; }
        BigFunctor(const BigFunctor& o) : id(o.id) { ++g_alive; }
        BigFunctor(BigFunctor&& o) noexcept : id(o.id) { ++g_alive; }
        ~BigFunctor() { --g_alive; }
        int operator()(int x) const { return x + id; }
    };
}

TEST(DelegateLargeFunctor, ConstructInvokeCopyDestroy)
{
    g_alive = 0;
    {
        Astra::Delegate<int(int)> d(BigFunctor{7});
        EXPECT_EQ(d(10), 17);

        Astra::Delegate<int(int)> copy(d);   // shared_ptr copy — same functor
        EXPECT_EQ(copy(1), 8);

        Astra::Delegate<int(int)> moved(std::move(d));
        EXPECT_EQ(moved(2), 9);
    }
    EXPECT_EQ(g_alive, 0);   // no leaks, no double-destroy
}
