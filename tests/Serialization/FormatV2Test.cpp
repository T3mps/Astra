#include <gtest/gtest.h>
#include <Astra/Astra.hpp>

namespace { struct FPos { float x, y, z; }; }

TEST(FormatV2, HeaderVersionMatchesCurrentFormatVersion)
{
    // Was HeaderVersionIs2 / hardcoded EXPECT_EQ(version, 2) before the v3
    // root-archetype format bump (BINARY_FORMAT_VERSION now 3, see
    // BinaryArchive.hpp). A freshly-saved archive is always stamped with
    // whatever BINARY_FORMAT_VERSION currently is, so assert against that
    // symbol rather than a literal that goes stale on the next format bump.
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    reg.CreateEntityWith(FPos{1, 2, 3});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    const auto& bytes = *saved.GetValue();
    // BinaryHeader: magic[5], then uint16 version at offset 5 (packed struct).
    uint16_t version = 0;
    std::memcpy(&version, bytes.data() + 5, sizeof(version));
    EXPECT_EQ(version, Astra::BINARY_FORMAT_VERSION);
}

TEST(FormatV2, ChecksumIsPortableFunctionAndDetectsCorruption)
{
    // Determinism of the portable checksum itself:
    const char data[] = "astra-format-v2-checksum";
    uint32_t a = Astra::Checksum::Portable(data, sizeof(data));
    uint32_t b = Astra::Checksum::Portable(data, sizeof(data));
    EXPECT_EQ(a, b);
    char tampered[sizeof(data)];
    std::memcpy(tampered, data, sizeof(data));
    tampered[3] ^= 0x1;
    EXPECT_NE(a, Astra::Checksum::Portable(tampered, sizeof(tampered)));

    // End-to-end: corrupting a payload byte must fail the checksum on load.
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    for (int i = 0; i < 100; ++i) reg.CreateEntityWith(FPos{float(i), 0, 0});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto bytes = *saved.GetValue();
    bytes[bytes.size() / 2] ^= std::byte{0x1};

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<FPos>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(bytes), creg);
    EXPECT_TRUE(loaded.IsErr());
}

TEST(FormatV2, RoundTripSurvives)
{
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponent<FPos>();
    for (int i = 0; i < 1000; ++i) reg.CreateEntityWith(FPos{float(i), float(i * 2), 0});
    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<FPos>();
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ((*loaded.GetValue())->Size(), 1000u);
}
