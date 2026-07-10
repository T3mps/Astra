#include <gtest/gtest.h>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <filesystem>
#include <array>

namespace
{
    // Test components
    struct Position
    {
        float x, y, z;
        
        bool operator==(const Position& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };
    
    struct ComplexComponent
    {
        std::string name;
        std::vector<int> values;
        float multiplier;
        
        void Serialize(auto& ar)
        {
            ar(name)(values)(multiplier);
        }
        
        bool operator==(const ComplexComponent& other) const
        {
            return name == other.name && 
                   values == other.values && 
                   multiplier == other.multiplier;
        }
    };
    
    struct VersionedComponent
    {
        int id;
        float value;
        std::string description;

        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(id)(value)(description);
        }

        bool operator==(const VersionedComponent& other) const
        {
            return id == other.id &&
                   value == other.value &&
                   description == other.description;
        }
    };
}

// Component-versioning / migration tests (CurrentFormatComponent, its
// SerializationTraits specialization, and OldFormatComponent - which was
// unused dead code) split out (2026-07-10 test-suite audit) into
// tests/Serialization/BinarySerializationVersioningTests.cpp.

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

TEST_F(BinarySerializationTests, HeaderWriteRead)
{
    using namespace Astra;
    
    // Write header
    {
        BinaryWriter writer(m_tempPath);
        BinaryHeader header;
        header.archetypeCount = 3;
        header.entityCount = 1000;
        writer.WriteHeader(header);
    }
    
    // Read and verify header
    {
        BinaryReader reader(m_tempPath);
        auto result = reader.ReadHeader();
        ASSERT_TRUE(result.IsOk());
        
        auto header = *result.GetValue();
        EXPECT_TRUE(header.IsValid());
        EXPECT_TRUE(header.IsVersionSupported());
        EXPECT_TRUE(header.IsEndianCompatible());
        EXPECT_EQ(header.archetypeCount, 3u);
        EXPECT_EQ(header.entityCount, 1000u);
    }
}

TEST_F(BinarySerializationTests, PODTypesSerialization)
{
    using namespace Astra;
    
    // Test data
    int intVal = 42;
    float floatVal = 3.14159f;
    double doubleVal = 2.71828;
    Position pos{1.0f, 2.0f, 3.0f};
    
    // Write to memory
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(intVal)(floatVal)(doubleVal)(pos);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Read back
    {
        BinaryReader reader(buffer);
        int readInt;
        float readFloat;
        double readDouble;
        Position readPos;
        
        reader(readInt)(readFloat)(readDouble)(readPos);
        EXPECT_FALSE(reader.HasError());
        
        EXPECT_EQ(readInt, intVal);
        EXPECT_EQ(readFloat, floatVal);
        EXPECT_EQ(readDouble, doubleVal);
        EXPECT_EQ(readPos, pos);
    }
}

TEST_F(BinarySerializationTests, StringSerialization)
{
    using namespace Astra;
    
    std::string shortStr = "Hello";
    std::string longStr(1000, 'A');
    std::string emptyStr = "";
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(shortStr)(longStr)(emptyStr);
    }
    
    {
        BinaryReader reader(buffer);
        std::string s1, s2, s3;
        reader(s1)(s2)(s3);
        
        EXPECT_EQ(s1, shortStr);
        EXPECT_EQ(s2, longStr);
        EXPECT_EQ(s3, emptyStr);
    }
}

TEST_F(BinarySerializationTests, VectorSerialization)
{
    using namespace Astra;
    
    std::vector<int> intVec = {1, 2, 3, 4, 5};
    std::vector<Position> posVec = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };
    std::vector<std::string> strVec = {"one", "two", "three"};
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(intVec)(posVec)(strVec);
    }
    
    {
        BinaryReader reader(buffer);
        std::vector<int> readInts;
        std::vector<Position> readPos;
        std::vector<std::string> readStrs;
        
        reader(readInts)(readPos)(readStrs);
        
        EXPECT_EQ(readInts, intVec);
        EXPECT_EQ(readPos, posVec);
        EXPECT_EQ(readStrs, strVec);
    }
}

TEST_F(BinarySerializationTests, CustomSerializationMethod)
{
    using namespace Astra;
    
    ComplexComponent comp;
    comp.name = "TestComponent";
    comp.values = {10, 20, 30, 40};
    comp.multiplier = 2.5f;
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(comp);
    }
    
    {
        BinaryReader reader(buffer);
        ComplexComponent readComp;
        reader(readComp);
        
        EXPECT_EQ(readComp, comp);
    }
}

TEST_F(BinarySerializationTests, SerializeMethodOnNonTrivialComponent)
{
    using namespace Astra;
    
    // Test component with Serialize method
    VersionedComponent comp;
    comp.id = 123;
    comp.value = 456.789f;
    comp.description = "Test Description";
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(comp);
    }
    
    {
        BinaryReader reader(buffer);
        VersionedComponent readComp;
        reader(readComp);
        
        EXPECT_EQ(readComp, comp);
    }
}

TEST_F(BinarySerializationTests, ComponentHashWriteRead)
{
    using namespace Astra;
    
    uint64_t hash1 = 0x123456789ABCDEF0ULL;
    uint64_t hash2 = 0xFEDCBA9876543210ULL;
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer.WriteComponentHash(hash1);
        writer.WriteComponentHash(hash2);
    }
    
    {
        BinaryReader reader(buffer);
        uint64_t readHash1 = reader.ReadComponentHash();
        uint64_t readHash2 = reader.ReadComponentHash();
        
        EXPECT_EQ(readHash1, hash1);
        EXPECT_EQ(readHash2, hash2);
    }
}

TEST_F(BinarySerializationTests, AlignmentPadding)
{
    using namespace Astra;
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(std::byte(1));  // 1 byte
        writer.WritePadding(4);  // Align to 4 bytes
        writer(uint32_t(0x12345678));  // 4 bytes
        writer(std::byte(2));  // 1 byte
        writer.WritePadding(8);  // Align to 8 bytes
        writer(uint64_t(0xDEADBEEFCAFEBABEULL));  // 8 bytes
    }
    
    {
        BinaryReader reader(buffer);
        std::byte b1;
        reader(b1);
        EXPECT_EQ(b1, std::byte{1});
        
        reader.SkipPadding(4);
        uint32_t i;
        reader(i);
        EXPECT_EQ(i, 0x12345678u);
        
        std::byte b2;
        reader(b2);
        EXPECT_EQ(b2, std::byte{2});
        
        reader.SkipPadding(8);
        uint64_t l;
        reader(l);
        EXPECT_EQ(l, 0xDEADBEEFCAFEBABEULL);
    }
}

TEST_F(BinarySerializationTests, FileWriteRead)
{
    using namespace Astra;
    
    std::vector<Position> positions;
    for (int i = 0; i < 1000; ++i)
    {
        positions.push_back({
            static_cast<float>(i),
            static_cast<float>(i * 2),
            static_cast<float>(i * 3)
        });
    }
    
    // Write to file
    {
        BinaryWriter writer(m_tempPath);
        writer(positions);
        EXPECT_FALSE(writer.HasError());
    }
    
    // Read from file
    {
        BinaryReader reader(m_tempPath);
        std::vector<Position> readPositions;
        reader(readPositions);
        EXPECT_FALSE(reader.HasError());
        EXPECT_EQ(readPositions, positions);
    }
}

TEST_F(BinarySerializationTests, ErrorHandling)
{
    using namespace Astra;
    
    // Test invalid magic
    {
        std::vector<std::byte> buffer;
        buffer.resize(32, std::byte(0));  // Wrong magic
        
        BinaryReader reader(buffer);
        auto result = reader.ReadHeader();
        EXPECT_TRUE(result.IsErr());
        EXPECT_EQ(*result.GetError(), SerializationError::InvalidMagic);
    }
    
    // Test corrupted data (reading past end)
    {
        std::vector<std::byte> buffer;
        BinaryWriter writer(buffer);
        writer(uint32_t(10));  // Write size 10
        // But don't write the actual data
        
        BinaryReader reader(buffer);
        std::vector<int> vec;
        reader(vec);
        EXPECT_TRUE(reader.HasError());
        EXPECT_EQ(reader.GetError(), SerializationError::CorruptedData);
    }
}

TEST_F(BinarySerializationTests, LargeDataSerialization)
{
    using namespace Astra;
    
    // Create large dataset
    std::vector<Position> positions(100000);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        positions[i] = {
            static_cast<float>(i % 1000),
            static_cast<float>((i * 7) % 1000),
            static_cast<float>((i * 13) % 1000)
        };
    }
    
    // Write and read
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(positions);
        EXPECT_GT(writer.GetBytesWritten(), positions.size() * sizeof(Position));
    }
    
    {
        BinaryReader reader(buffer);
        std::vector<Position> readPositions;
        reader(readPositions);
        EXPECT_EQ(readPositions.size(), positions.size());
        EXPECT_EQ(readPositions, positions);
    }
}

TEST_F(BinarySerializationTests, ArraySerialization)
{
    using namespace Astra;
    
    std::array<int, 5> intArray = {1, 2, 3, 4, 5};
    std::array<Position, 3> posArray = {{
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    }};
    std::array<std::string, 2> strArray = {"hello", "world"};
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(intArray)(posArray)(strArray);
    }
    
    {
        BinaryReader reader(buffer);
        std::array<int, 5> readInts;
        std::array<Position, 3> readPos;
        std::array<std::string, 2> readStrs;
        
        reader(readInts)(readPos)(readStrs);
        
        EXPECT_EQ(readInts, intArray);
        EXPECT_EQ(readPos, posArray);
        EXPECT_EQ(readStrs, strArray);
    }
}

TEST_F(BinarySerializationTests, PairTupleSerialization)
{
    using namespace Astra;
    
    std::pair<int, std::string> testPair = {42, "test"};
    std::tuple<int, float, std::string> testTuple = {100, 3.14f, "tuple"};
    std::pair<Position, std::vector<int>> complexPair = {
        {1.0f, 2.0f, 3.0f},
        {10, 20, 30}
    };
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(testPair)(testTuple)(complexPair);
    }
    
    {
        BinaryReader reader(buffer);
        std::pair<int, std::string> readPair;
        std::tuple<int, float, std::string> readTuple;
        std::pair<Position, std::vector<int>> readComplexPair;
        
        reader(readPair)(readTuple)(readComplexPair);
        
        EXPECT_EQ(readPair, testPair);
        EXPECT_EQ(readTuple, testTuple);
        EXPECT_EQ(readComplexPair, complexPair);
    }
}

TEST_F(BinarySerializationTests, OptionalSerialization)
{
    using namespace Astra;
    
    std::optional<int> hasValue = 42;
    std::optional<int> noValue = std::nullopt;
    std::optional<std::string> strValue = "optional";
    std::optional<Position> posNoValue = std::nullopt;
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(hasValue)(noValue)(strValue)(posNoValue);
    }
    
    {
        BinaryReader reader(buffer);
        std::optional<int> readHasValue;
        std::optional<int> readNoValue;
        std::optional<std::string> readStrValue;
        std::optional<Position> readPosNoValue;
        
        reader(readHasValue)(readNoValue)(readStrValue)(readPosNoValue);
        
        EXPECT_TRUE(readHasValue.has_value());
        EXPECT_EQ(*readHasValue, 42);
        EXPECT_FALSE(readNoValue.has_value());
        EXPECT_TRUE(readStrValue.has_value());
        EXPECT_EQ(*readStrValue, "optional");
        EXPECT_FALSE(readPosNoValue.has_value());
    }
}

TEST_F(BinarySerializationTests, MapSerialization)
{
    using namespace Astra;
    
    std::map<int, std::string> orderedMap = {
        {3, "three"},
        {1, "one"},
        {2, "two"}
    };
    
    std::unordered_map<std::string, Position> unorderedMap = {
        {"origin", {0.0f, 0.0f, 0.0f}},
        {"up", {0.0f, 1.0f, 0.0f}},
        {"right", {1.0f, 0.0f, 0.0f}}
    };
    
    std::map<int, std::vector<int>> nestedMap = {
        {1, {10, 11, 12}},
        {2, {20, 21, 22}}
    };
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(orderedMap)(unorderedMap)(nestedMap);
    }
    
    {
        BinaryReader reader(buffer);
        std::map<int, std::string> readOrdered;
        std::unordered_map<std::string, Position> readUnordered;
        std::map<int, std::vector<int>> readNested;
        
        reader(readOrdered)(readUnordered)(readNested);
        
        EXPECT_EQ(readOrdered, orderedMap);
        EXPECT_EQ(readUnordered.size(), unorderedMap.size());
        for (const auto& [key, value] : unorderedMap)
        {
            EXPECT_EQ(readUnordered[key], value);
        }
        EXPECT_EQ(readNested, nestedMap);
    }
}

TEST_F(BinarySerializationTests, SetSerialization)
{
    using namespace Astra;
    
    std::set<int> orderedSet = {5, 1, 3, 2, 4};
    std::unordered_set<std::string> unorderedSet = {"apple", "banana", "cherry"};
    std::set<float> floatSet = {3.14f, 2.71f, 1.41f};
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(orderedSet)(unorderedSet)(floatSet);
    }
    
    {
        BinaryReader reader(buffer);
        std::set<int> readOrdered;
        std::unordered_set<std::string> readUnordered;
        std::set<float> readFloat;
        
        reader(readOrdered)(readUnordered)(readFloat);
        
        EXPECT_EQ(readOrdered, orderedSet);
        EXPECT_EQ(readUnordered, unorderedSet);
        EXPECT_EQ(readFloat, floatSet);
    }
}

TEST_F(BinarySerializationTests, NestedContainerSerialization)
{
    using namespace Astra;
    
    // Complex nested structure
    std::vector<std::map<std::string, std::optional<int>>> nested = {
        {{"a", 1}, {"b", std::nullopt}, {"c", 3}},
        {{"x", 10}, {"y", 20}},
        {}
    };
    
    std::map<int, std::pair<std::string, std::vector<float>>> complex = {
        {1, {"first", {1.1f, 1.2f, 1.3f}}},
        {2, {"second", {2.1f, 2.2f}}}
    };
    
    std::optional<std::array<std::pair<int, int>, 3>> optArray = 
        std::array<std::pair<int, int>, 3>{{{1, 2}, {3, 4}, {5, 6}}};
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(nested)(complex)(optArray);
    }
    
    {
        BinaryReader reader(buffer);
        std::vector<std::map<std::string, std::optional<int>>> readNested;
        std::map<int, std::pair<std::string, std::vector<float>>> readComplex;
        std::optional<std::array<std::pair<int, int>, 3>> readOptArray;
        
        reader(readNested)(readComplex)(readOptArray);
        
        EXPECT_EQ(readNested, nested);
        EXPECT_EQ(readComplex, complex);
        EXPECT_TRUE(readOptArray.has_value());
        EXPECT_EQ(*readOptArray, *optArray);
    }
}


TEST_F(BinarySerializationTests, ChecksumVerification)
{
    using namespace Astra;
    
    // Test data
    std::vector<Position> positions = {
        {1.0f, 2.0f, 3.0f},
        {4.0f, 5.0f, 6.0f},
        {7.0f, 8.0f, 9.0f}
    };
    
    std::string text = "Test checksum verification";
    int value = 42;
    
    // Write with checksum
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        
        BinaryHeader header;
        header.entityCount = 100;
        writer.WriteHeader(header);
        
        writer(positions)(text)(value);
        
        // Finalize to update checksum in header
        writer.FinalizeHeader();
        
        EXPECT_FALSE(writer.HasError());
        EXPECT_GT(writer.GetChecksum(), 0u);
    }
    
    // Read and verify checksum
    {
        BinaryReader reader(buffer);
        
        auto headerResult = reader.ReadHeader();
        ASSERT_TRUE(headerResult.IsOk());
        
        std::vector<Position> readPositions;
        std::string readText;
        int readValue;
        
        reader(readPositions)(readText)(readValue);
        
        EXPECT_FALSE(reader.HasError());
        EXPECT_EQ(readPositions, positions);
        EXPECT_EQ(readText, text);
        EXPECT_EQ(readValue, value);
        
        // Verify checksum
        auto checksumResult = reader.VerifyChecksum();
        EXPECT_TRUE(checksumResult.IsOk());
        
        // Check that calculated checksum matches expected
        EXPECT_EQ(reader.GetChecksum(), reader.GetExpectedChecksum());
    }
}

TEST_F(BinarySerializationTests, ChecksumMismatchDetection)
{
    using namespace Astra;
    
    std::vector<Position> positions(100);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        positions[i] = {static_cast<float>(i), 0.0f, 0.0f};
    }
    
    // Write with checksum
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        
        BinaryHeader header;
        writer.WriteHeader(header);
        
        writer(positions);
        writer.FinalizeHeader();
    }
    
    // Corrupt the data (change a byte after the header)
    if (buffer.size() > sizeof(BinaryHeader) + 10)
    {
        buffer[sizeof(BinaryHeader) + 10] = std::byte(0xFF);
    }
    
    // Try to read with corrupted data
    {
        BinaryReader reader(buffer);
        
        auto headerResult = reader.ReadHeader();
        ASSERT_TRUE(headerResult.IsOk());
        
        std::vector<Position> readPositions;
        reader(readPositions);
        
        // Data read might succeed but checksum should fail
        auto checksumResult = reader.VerifyChecksum();
        EXPECT_TRUE(checksumResult.IsErr());
        if (checksumResult.IsErr())
        {
            EXPECT_EQ(*checksumResult.GetError(), SerializationError::ChecksumMismatch);
        }
        
        // Checksums should not match
        EXPECT_NE(reader.GetChecksum(), reader.GetExpectedChecksum());
    }
}

TEST_F(BinarySerializationTests, ChecksumDisabled)
{
    using namespace Astra;
    
    std::vector<int> data = {1, 2, 3, 4, 5};
    
    // Write without checksum
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer.SetChecksumEnabled(false);
        
        BinaryHeader header;
        writer.WriteHeader(header);
        writer(data);
        writer.FinalizeHeader();
        
        EXPECT_EQ(writer.GetChecksum(), 0u);
    }
    
    // Read without checksum verification
    {
        BinaryReader reader(buffer);
        reader.SetChecksumEnabled(false);
        
        auto headerResult = reader.ReadHeader();
        ASSERT_TRUE(headerResult.IsOk());
        
        std::vector<int> readData;
        reader(readData);
        
        EXPECT_EQ(readData, data);
        
        // Checksum verification should pass when disabled
        auto checksumResult = reader.VerifyChecksum();
        EXPECT_TRUE(checksumResult.IsOk());
    }
}

TEST_F(BinarySerializationTests, FileChecksumVerification)
{
    using namespace Astra;
    
    // Large dataset to test file path
    std::vector<Position> positions(10000);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        positions[i] = {
            static_cast<float>(i),
            static_cast<float>(i * 2),
            static_cast<float>(i * 3)
        };
    }
    
    // Write to file with checksum
    {
        BinaryWriter writer(m_tempPath);
        
        BinaryHeader header;
        header.entityCount = (uint32_t)positions.size();
        writer.WriteHeader(header);
        
        writer(positions);
        writer.FinalizeHeader();
        
        EXPECT_FALSE(writer.HasError());
    }
    
    // Read from file and verify checksum
    {
        BinaryReader reader(m_tempPath);
        
        auto headerResult = reader.ReadHeader();
        ASSERT_TRUE(headerResult.IsOk());
        
        std::vector<Position> readPositions;
        reader(readPositions);
        
        EXPECT_EQ(readPositions, positions);
        
        // Verify checksum
        auto checksumResult = reader.VerifyChecksum();
        EXPECT_TRUE(checksumResult.IsOk());
        EXPECT_EQ(reader.GetChecksum(), reader.GetExpectedChecksum());
    }
}

TEST_F(BinarySerializationTests, EmptyContainerSerialization)
{
    using namespace Astra;
    
    std::vector<int> emptyVec;
    std::map<int, std::string> emptyMap;
    std::set<float> emptySet;
    std::optional<int> emptyOpt;
    std::array<int, 0> emptyArray;
    
    std::vector<std::byte> buffer;
    {
        BinaryWriter writer(buffer);
        writer(emptyVec)(emptyMap)(emptySet)(emptyOpt)(emptyArray);
    }
    
    {
        BinaryReader reader(buffer);
        std::vector<int> readVec;
        std::map<int, std::string> readMap;
        std::set<float> readSet;
        std::optional<int> readOpt;
        std::array<int, 0> readArray;
        
        reader(readVec)(readMap)(readSet)(readOpt)(readArray);
        
        EXPECT_TRUE(readVec.empty());
        EXPECT_TRUE(readMap.empty());
        EXPECT_TRUE(readSet.empty());
        EXPECT_FALSE(readOpt.has_value());
        EXPECT_EQ(readArray.size(), 0u);
    }
}
