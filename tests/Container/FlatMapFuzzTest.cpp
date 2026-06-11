#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <Astra/Container/FlatMap.hpp>

namespace
{
    // Differential test vs std::unordered_map. Small key space forces
    // collisions, tombstone churn, and rehash cycles.
    void RunDifferential(uint64_t seed, int ops, int keySpace)
    {
        std::mt19937_64 rng(seed);
        Astra::FlatMap<int, int> dut;
        std::unordered_map<int, int> ref;

        for (int op = 0; op < ops; ++op)
        {
            const int key = static_cast<int>(rng() % static_cast<uint64_t>(keySpace));
            switch (rng() % 100)
            {
                case 0:  // rare: Clear
                    dut.Clear(); ref.clear(); break;
                case 1: case 2:  // Reserve at awkward sizes
                    dut.Reserve(static_cast<size_t>(rng() % 512)); break;
                default:
                    if (rng() % 3 == 0)
                    {
                        dut.Erase(key); ref.erase(key);
                    }
                    else
                    {
                        const int val = static_cast<int>(rng());
                        dut[key] = val; ref[key] = val;
                    }
                    break;
            }

            ASSERT_EQ(dut.Size(), ref.size()) << "seed=" << seed << " op=" << op;
            if (op % 257 == 0)  // periodic full-state check
            {
                for (const auto& [k, v] : ref)
                {
                    auto it = dut.Find(k);
                    ASSERT_NE(it, dut.end()) << "missing key " << k << " seed=" << seed << " op=" << op;
                    ASSERT_EQ(it->second, v) << "wrong value for " << k << " seed=" << seed << " op=" << op;
                }
                size_t iterCount = 0;
                for (const auto& [k, v] : dut)
                {
                    auto rit = ref.find(k);
                    ASSERT_NE(rit, ref.end()) << "phantom key " << k << " seed=" << seed << " op=" << op;
                    ASSERT_EQ(rit->second, v);
                    ++iterCount;
                }
                ASSERT_EQ(iterCount, ref.size());
            }
        }
    }

    TEST(FlatMapFuzz, ChurnSmallKeySpace)   { RunDifferential(0xA57A1, 60000, 64); }
    TEST(FlatMapFuzz, ChurnMediumKeySpace)  { RunDifferential(0xA57A2, 60000, 1024); }
    TEST(FlatMapFuzz, ChurnLargeKeySpace)   { RunDifferential(0xA57A3, 60000, 100000); }
    TEST(FlatMapFuzz, ManySeeds)
    {
        for (uint64_t s = 1; s <= 16; ++s) RunDifferential(s * 7919, 8000, 128);
    }

    // Collapse the hash so probing chains span many groups and H2 metadata
    // degenerates -- exercises long-probe and wraparound paths.
    struct AwfulHash
    {
        size_t operator()(int k) const noexcept { return static_cast<size_t>(k) & 0xF; }
    };

    TEST(FlatMapFuzz, AdversarialHashStillCorrect)
    {
        Astra::FlatMap<int, int, AwfulHash> dut;
        std::unordered_map<int, int> ref;
        std::mt19937_64 rng(0xBADBAD);
        for (int op = 0; op < 20000; ++op)
        {
            const int key = static_cast<int>(rng() % 512);
            if (rng() % 3 == 0) { dut.Erase(key); ref.erase(key); }
            else { dut[key] = key * 3; ref[key] = key * 3; }
            ASSERT_EQ(dut.Size(), ref.size()) << "op=" << op;
        }
        for (const auto& [k, v] : ref)
        {
            auto it = dut.Find(k);
            ASSERT_NE(it, dut.end()) << k;
            ASSERT_EQ(it->second, v);
        }
    }

    // All keys collapse to H1 group 0 with a NONZERO H2, isolating the
    // tombstone probe-termination bug from any H2-range concern.
    struct GroupColliderHash
    {
        size_t operator()(int) const noexcept { return 1ULL << 57; }
    };

    // Regression: a key whose probe chain overflows into a later group must
    // still be found by Emplace's duplicate scan when an earlier group on the
    // chain holds a tombstone. The buggy interleaved scan inserted a second
    // copy at the tombstone (size +1 overcount, phantom survivor after Erase).
    TEST(FlatMapFuzz, ReinsertAfterTombstoneDoesNotDuplicate)
    {
        Astra::FlatMap<int, int, GroupColliderHash> m;
        m.Reserve(32);                            // 2 groups of 16
        for (int k = 0; k < 16; ++k) m[k] = k;    // fill group 0
        m[16] = 16;                               // overflows into group 1
        ASSERT_EQ(m.Size(), 17u);
        m.Erase(0);                               // tombstone in group 0
        m[16] = 99;                               // re-insert existing key
        EXPECT_EQ(m.Size(), 16u);
        int copies = 0;
        for (const auto& kv : m) if (kv.first == 16) ++copies;
        EXPECT_EQ(copies, 1);
        EXPECT_EQ(m.Find(16)->second, 99);
        m.Erase(16);
        EXPECT_FALSE(m.Contains(16));
        EXPECT_EQ(m.Size(), 15u);
    }

    // Keys with the same low bits share the same H1 group at small capacity,
    // stressing wraparound and multi-group probe chains.
    TEST(FlatMapFuzz, CollidingKeysChurn)
    {
        Astra::FlatMap<int, int> dut;
        std::unordered_map<int, int> ref;
        std::mt19937_64 rng(0xC011DE);
        // Use keys base + i*4096 so low 12 bits are the same -> same H1 bucket family.
        const int numKeys = 64;
        std::vector<int> keys;
        keys.reserve(numKeys);
        for (int i = 0; i < numKeys; ++i)
            keys.push_back(i * 4096);

        for (int op = 0; op < 30000; ++op)
        {
            const int key = keys[rng() % static_cast<uint64_t>(numKeys)];
            if (rng() % 3 == 0) { dut.Erase(key); ref.erase(key); }
            else { dut[key] = key ^ op; ref[key] = key ^ op; }
            ASSERT_EQ(dut.Size(), ref.size()) << "op=" << op;
        }
        for (const auto& [k, v] : ref)
        {
            auto it = dut.Find(k);
            ASSERT_NE(it, dut.end()) << "missing colliding key " << k;
            ASSERT_EQ(it->second, v);
        }
    }
}
