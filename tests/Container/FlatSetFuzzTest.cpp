#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <Astra/Container/FlatSet.hpp>

namespace
{
    // Differential test vs std::unordered_set. Small key space forces
    // collisions, tombstone churn, and rehash cycles.
    void RunDifferential(uint64_t seed, int ops, int keySpace)
    {
        std::mt19937_64 rng(seed);
        Astra::FlatSet<int> dut;
        std::unordered_set<int> ref;

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
                        dut.Insert(key); ref.insert(key);
                    }
                    break;
            }

            ASSERT_EQ(dut.Size(), ref.size()) << "seed=" << seed << " op=" << op;

            // Periodic Contains spot-check and full two-way state comparison
            if (op % 257 == 0)
            {
                for (int k : ref)
                {
                    ASSERT_TRUE(dut.Contains(k)) << "missing key " << k << " seed=" << seed << " op=" << op;
                    auto it = dut.Find(k);
                    ASSERT_NE(it, dut.end()) << "Find missed key " << k << " seed=" << seed << " op=" << op;
                    ASSERT_EQ(*it, k);
                }
                size_t iterCount = 0;
                for (int k : dut)
                {
                    ASSERT_NE(ref.find(k), ref.end()) << "phantom key " << k << " seed=" << seed << " op=" << op;
                    ++iterCount;
                }
                ASSERT_EQ(iterCount, ref.size());
            }
        }
    }

    TEST(FlatSetFuzz, ChurnSmallKeySpace)   { RunDifferential(0x5E7A1, 60000, 64); }
    TEST(FlatSetFuzz, ChurnMediumKeySpace)  { RunDifferential(0x5E7A2, 60000, 1024); }
    TEST(FlatSetFuzz, ChurnLargeKeySpace)   { RunDifferential(0x5E7A3, 60000, 100000); }
    TEST(FlatSetFuzz, ManySeeds)
    {
        for (uint64_t s = 1; s <= 16; ++s) RunDifferential(s * 6271, 8000, 128);
    }

    // Adversarial hash: collapses all keys to 16 H1 buckets, stressing
    // long-probe chains, group wraparound, and H2 false-match handling.
    struct AwfulHash
    {
        size_t operator()(int k) const noexcept { return static_cast<size_t>(k) & 0xF; }
    };

    TEST(FlatSetFuzz, AdversarialHashStillCorrect)
    {
        Astra::FlatSet<int, AwfulHash> dut;
        std::unordered_set<int> ref;
        std::mt19937_64 rng(0xBADC0DE);
        for (int op = 0; op < 20000; ++op)
        {
            const int key = static_cast<int>(rng() % 512);
            if (rng() % 3 == 0) { dut.Erase(key); ref.erase(key); }
            else { dut.Insert(key); ref.insert(key); }
            ASSERT_EQ(dut.Size(), ref.size()) << "op=" << op;
        }
        for (int k : ref)
        {
            ASSERT_TRUE(dut.Contains(k)) << k;
            auto it = dut.Find(k);
            ASSERT_NE(it, dut.end()) << k;
        }
    }

    // Duplicate-rejection: Emplace returns pair<iterator,bool>; second insert
    // of the same value must return false and leave Size()==1.
    TEST(FlatSetFuzz, EmplaceDuplicateRejected)
    {
        Astra::FlatSet<int> set;
        auto [it1, ins1] = set.Emplace(42);
        EXPECT_TRUE(ins1);
        EXPECT_EQ(*it1, 42);
        auto [it2, ins2] = set.Emplace(42);
        EXPECT_FALSE(ins2);
        EXPECT_EQ(*it2, 42);
        EXPECT_EQ(set.Size(), 1u);
    }

    // All keys collapse to H1 group 0 with a NONZERO H2, isolating the
    // tombstone probe-termination bug from any H2-range concern.
    struct GroupColliderHash
    {
        size_t operator()(int) const noexcept { return 1ULL << 57; }
    };

    // Regression: a value whose probe chain overflows into a later group must
    // still be found by Emplace's duplicate scan when an earlier group on the
    // chain holds a tombstone. The buggy interleaved scan inserted a second
    // copy at the tombstone (size +1 overcount, phantom survivor after Erase).
    TEST(FlatSetFuzz, ReinsertAfterTombstoneDoesNotDuplicate)
    {
        Astra::FlatSet<int, GroupColliderHash> s;
        s.Reserve(32);                              // 2 groups of 16
        for (int k = 0; k < 16; ++k) s.Insert(k);   // fill group 0
        s.Insert(16);                               // overflows into group 1
        ASSERT_EQ(s.Size(), 17u);
        s.Erase(0);                                 // tombstone in group 0
        auto [it, inserted] = s.Emplace(16);        // re-insert existing value
        EXPECT_FALSE(inserted);
        EXPECT_EQ(s.Size(), 16u);
        int copies = 0;
        for (int k : s) if (k == 16) ++copies;
        EXPECT_EQ(copies, 1);
        s.Erase(16);
        EXPECT_FALSE(s.Contains(16));
        EXPECT_EQ(s.Size(), 15u);
    }

    // Keys with the same low bits share the same H1 group at small capacity,
    // stressing wraparound and multi-group probe chains.
    TEST(FlatSetFuzz, CollidingKeysChurn)
    {
        Astra::FlatSet<int> dut;
        std::unordered_set<int> ref;
        std::mt19937_64 rng(0xC011DEF);
        const int numKeys = 64;
        std::vector<int> keys;
        keys.reserve(numKeys);
        for (int i = 0; i < numKeys; ++i)
            keys.push_back(i * 4096);

        for (int op = 0; op < 30000; ++op)
        {
            const int key = keys[rng() % static_cast<uint64_t>(numKeys)];
            if (rng() % 3 == 0) { dut.Erase(key); ref.erase(key); }
            else { dut.Insert(key); ref.insert(key); }
            ASSERT_EQ(dut.Size(), ref.size()) << "op=" << op;
        }
        for (int k : ref)
        {
            ASSERT_TRUE(dut.Contains(k)) << "missing colliding key " << k;
        }
    }
}
