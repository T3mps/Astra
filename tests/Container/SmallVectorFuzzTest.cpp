#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <vector>
#include <Astra/Container/SmallVector.hpp>

namespace
{
    void RunDifferential(uint64_t seed, int ops)
    {
        std::mt19937_64 rng(seed);
        Astra::SmallVector<int, 4> dut;
        std::vector<int> ref;

        auto check = [&]
        {
            ASSERT_EQ(dut.size(), ref.size()) << "seed=" << seed;
            for (size_t i = 0; i < ref.size(); ++i)
                ASSERT_EQ(dut[i], ref[i]) << "i=" << i << " seed=" << seed;
        };

        for (int op = 0; op < ops; ++op)
        {
            switch (rng() % 10)
            {
                case 0: case 1: case 2: case 3:
                {
                    const int v = static_cast<int>(rng());
                    dut.push_back(v); ref.push_back(v); break;
                }
                case 4:
                    if (!ref.empty()) { dut.pop_back(); ref.pop_back(); }
                    break;
                case 5:
                {
                    const size_t pos = ref.empty() ? 0 : rng() % ref.size();
                    const int v = static_cast<int>(rng());
                    dut.insert(dut.begin() + pos, v); ref.insert(ref.begin() + pos, v);
                    break;
                }
                case 6:
                    if (!ref.empty())
                    {
                        const size_t pos = rng() % ref.size();
                        dut.erase(dut.begin() + pos); ref.erase(ref.begin() + pos);
                    }
                    break;
                case 7:
                {
                    const size_t n = rng() % 12;  // hovers around the inline boundary
                    dut.resize(n); ref.resize(n); break;
                }
                case 8:  // copy round-trip
                {
                    Astra::SmallVector<int, 4> copy(dut);
                    dut = copy; break;
                }
                case 9:  // move round-trip (steals heap when large)
                {
                    Astra::SmallVector<int, 4> moved(std::move(dut));
                    dut = std::move(moved); break;
                }
            }
            check();
        }
        dut.shrink_to_fit();
        check();
    }

    TEST(SmallVectorFuzz, SpillBoundaryChurn) { RunDifferential(0x5111A11, 40000); }
    TEST(SmallVectorFuzz, ManySeeds)
    {
        for (uint64_t s = 1; s <= 16; ++s) RunDifferential(s * 104729, 5000);
    }
}
