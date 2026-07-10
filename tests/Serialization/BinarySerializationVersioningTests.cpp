#include <gtest/gtest.h>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <filesystem>

// Split from BinarySerializationTests.cpp (2026-07-10 test-suite audit): the
// core reader/writer + STL-container tests and the component-versioning /
// migration tests are two separable themes. Fixture and the local POD types
// duplicated verbatim (< 20 lines each, both in an anonymous namespace so
// there is no ODR conflict with the identically-named types still defined in
// BinarySerializationTests.cpp); TEST bodies below are byte-identical to the
// ones removed from that file. `OldFormatComponent` was dropped entirely (it
// was unused dead code in the original file, not moved).
namespace
{
    struct Position
    {
        float x, y, z;

        bool operator==(const Position& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    // Component with version 2 (current format)
    struct CurrentFormatComponent
    {
        int id;
        float value;
        std::string description;  // Added in version 2

        bool operator==(const CurrentFormatComponent& other) const
        {
            return id == other.id &&
                   value == other.value &&
                   description == other.description;
        }
    };
}

// Specializations for versioning tests - must come after struct definitions
namespace Astra
{
    template<>
    struct SerializationTraits<::CurrentFormatComponent>
    {
        static constexpr bool HasCustomSerializer = true;
        static constexpr uint32_t Version = 2;
        static constexpr uint32_t MinVersion = 1;

        // Separate methods for Writer and Reader to avoid template issues
        static void Serialize(BinaryWriter& writer, ::CurrentFormatComponent& value)
        {
            writer(value.id)(value.value)(value.description);
        }

        static void Serialize(BinaryReader& reader, ::CurrentFormatComponent& value)
        {
            reader(value.id)(value.value)(value.description);
        }

        // Migration from version 1 to version 2
        static void Migrate(BinaryReader& reader, ::CurrentFormatComponent& value, uint32_t fromVersion)
        {
            if (fromVersion == 1)
            {
                // Version 1 only had id and value
                reader(value.id)(value.value);
                value.description = "Migrated from v1";
            }
        }
    };
}

class BinarySerializationTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_tempPath = std::filesystem::temp_directory_path() / "astra_test.bin";
    }

    void TearDown() override
    {
        if (std::filesystem::exists(m_tempPath))
        {
            std::filesystem::remove(m_tempPath);
        }
    }

    std::filesystem::path m_tempPath;
};

TEST_F(BinarySerializationTests, VersionedComponentWriteRead)
{
    using namespace Astra;

    CurrentFormatComponent comp;
    comp.id = 123;
    comp.value = 456.789f;
    comp.description = "Test component v2";

    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer.WriteVersionedComponent(comp);
        EXPECT_FALSE(writer.HasError());
    }

    {
        BinaryReader reader(buffer);
        CurrentFormatComponent readComp;
        auto result = reader.ReadVersionedComponent(readComp);

        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(readComp, comp);
    }
}

TEST_F(BinarySerializationTests, VersionMigration)
{
    using namespace Astra;

    // Simulate writing old version 1 format
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);

        // Write component hash
        uint64_t hash = TypeID<CurrentFormatComponent>::Hash();
        writer.WriteComponentHash(hash);

        // Write version 1
        uint32_t version = 1;
        writer(version);

        // Write version 1 data (only id and value)
        int id = 999;
        float value = 3.14159f;
        writer(id)(value);
    }

    // Read and migrate to version 2
    {
        BinaryReader reader(buffer);
        CurrentFormatComponent comp;
        auto result = reader.ReadVersionedComponent(comp);

        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(comp.id, 999);
        EXPECT_EQ(comp.value, 3.14159f);
        EXPECT_EQ(comp.description, "Migrated from v1");
    }
}

TEST_F(BinarySerializationTests, VersionTooOld)
{
    using namespace Astra;

    // Simulate version 0 which is below MinVersion
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);

        // Write component hash
        uint64_t hash = TypeID<CurrentFormatComponent>::Hash();
        writer.WriteComponentHash(hash);

        // Write version 0 (below MinVersion)
        uint32_t version = 0;
        writer(version);

        // Write some dummy data
        writer(int(1))(float(2.0f));
    }

    {
        BinaryReader reader(buffer);
        CurrentFormatComponent comp;
        auto result = reader.ReadVersionedComponent(comp);

        EXPECT_TRUE(result.IsErr());
        EXPECT_EQ(*result.GetError(), SerializationError::UnsupportedVersion);
    }
}

TEST_F(BinarySerializationTests, ComponentTypeMismatch)
{
    using namespace Astra;

    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);

        // Write wrong component hash
        uint64_t wrongHash = 0xDEADBEEF;
        writer.WriteComponentHash(wrongHash);

        // Write version
        uint32_t version = 1;
        writer(version);

        // Write some data
        writer(int(1))(float(2.0f));
    }

    {
        BinaryReader reader(buffer);
        CurrentFormatComponent comp;
        auto result = reader.ReadVersionedComponent(comp);

        EXPECT_TRUE(result.IsErr());
        EXPECT_EQ(*result.GetError(), SerializationError::UnknownComponent);
    }
}

TEST_F(BinarySerializationTests, PODComponentVersioning)
{
    using namespace Astra;

    // Test that POD components work with versioning
    Position pos{1.0f, 2.0f, 3.0f};

    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer.WriteVersionedComponent(pos);
    }

    {
        BinaryReader reader(buffer);
        Position readPos;
        auto result = reader.ReadVersionedComponent(readPos);

        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(readPos, pos);
    }
}

TEST_F(BinarySerializationTests, MultipleVersionedComponents)
{
    using namespace Astra;

    Position pos1{1.0f, 2.0f, 3.0f};
    CurrentFormatComponent comp1{100, 1.5f, "First"};
    Position pos2{4.0f, 5.0f, 6.0f};
    CurrentFormatComponent comp2{200, 2.5f, "Second"};

    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer.WriteVersionedComponent(pos1);
        writer.WriteVersionedComponent(comp1);
        writer.WriteVersionedComponent(pos2);
        writer.WriteVersionedComponent(comp2);
    }

    {
        BinaryReader reader(buffer);
        Position readPos1, readPos2;
        CurrentFormatComponent readComp1, readComp2;

        auto r1 = reader.ReadVersionedComponent(readPos1);
        auto r2 = reader.ReadVersionedComponent(readComp1);
        auto r3 = reader.ReadVersionedComponent(readPos2);
        auto r4 = reader.ReadVersionedComponent(readComp2);

        EXPECT_TRUE(r1.IsOk());
        EXPECT_TRUE(r2.IsOk());
        EXPECT_TRUE(r3.IsOk());
        EXPECT_TRUE(r4.IsOk());

        EXPECT_EQ(readPos1, pos1);
        EXPECT_EQ(readComp1, comp1);
        EXPECT_EQ(readPos2, pos2);
        EXPECT_EQ(readComp2, comp2);
    }
}
