#include <gtest/gtest.h>
#include <bitset>
#include <cstdint>
#include <random>
#include <Astra/Container/Bitmap.hpp>

namespace
{
    TEST(BitmapFuzz, DifferentialVsBitset)
    {
        std::mt19937_64 rng(0xB17B17);
        Astra::Bitmap<128> dut;
        std::bitset<128> ref;
        for (int op = 0; op < 50000; ++op)
        {
            const size_t idx = rng() % 128;
            switch (rng() % 3)
            {
                case 0: dut.Set(idx);   ref.set(idx);   break;
                case 1: dut.Reset(idx); ref.reset(idx); break;
                case 2: ASSERT_EQ(dut.Test(idx), ref.test(idx)) << idx << " op=" << op; break;
            }
            ASSERT_EQ(dut.Count(), ref.count()) << "op=" << op;
            ASSERT_EQ(dut.Any(),   ref.any())   << "op=" << op;
            ASSERT_EQ(dut.None(),  ref.none())  << "op=" << op;
        }
    }

    TEST(BitmapFuzz, HasAllMatchesManualCheck)
    {
        std::mt19937_64 rng(0xCAFE);
        for (int trial = 0; trial < 2000; ++trial)
        {
            Astra::Bitmap<128> a, mask;
            std::bitset<128> ra, rmask;
            for (int i = 0; i < 24; ++i)
            {
                size_t x = rng() % 128; a.Set(x);    ra.set(x);
                size_t y = rng() % 128; mask.Set(y); rmask.set(y);
            }
            const bool expected = (ra & rmask) == rmask;
            ASSERT_EQ(a.HasAll(mask), expected) << "trial=" << trial;
        }
    }

    TEST(BitmapFuzz, BitwiseOpsMatchBitset)
    {
        std::mt19937_64 rng(0xFAC4DE);
        for (int trial = 0; trial < 2000; ++trial)
        {
            Astra::Bitmap<128> a, b;
            std::bitset<128> ra, rb;
            for (int i = 0; i < 32; ++i)
            {
                size_t x = rng() % 128; a.Set(x); ra.set(x);
                size_t y = rng() % 128; b.Set(y); rb.set(y);
            }
            const auto andRes = a & b;
            const auto orRes  = a | b;
            const auto randRes = ra & rb;
            const auto rorRes  = ra | rb;
            for (size_t i = 0; i < 128; ++i)
            {
                ASSERT_EQ(andRes.Test(i), randRes.test(i)) << i;
                ASSERT_EQ(orRes.Test(i),  rorRes.test(i))  << i;
            }
        }
    }

    TEST(BitmapFuzz, OutOfBoundsIsNoop)
    {
        Astra::Bitmap<128> b;
        b.Set(128);
        b.Set(9999);
        EXPECT_TRUE(b.None());
        EXPECT_FALSE(b.Test(128));
    }
}
