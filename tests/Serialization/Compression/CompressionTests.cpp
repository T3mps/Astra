#include <gtest/gtest.h>
#include <random>
#include <chrono>
#include <array>
#include <cstring>

#include "Astra/Serialization/Compression/Compression.hpp"

using namespace Astra;
using namespace Astra::Compression;

class CompressionTest : public ::testing::Test
{
protected:
    // Generate predictable test data
    std::vector<uint8_t> GenerateTestData(size_t size, uint8_t seed = 42)
    {
        std::vector<uint8_t> data(size);
        std::mt19937 gen(seed);
        std::uniform_int_distribution<> dis(0, 255);
        
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = static_cast<uint8_t>(dis(gen));
        }
        
        return data;
    }
    
    // Generate highly compressible data (lots of repetition)
    std::vector<uint8_t> GenerateRepetitiveData(size_t size)
    {
        std::vector<uint8_t> data(size);
        
        // Fill with repeating pattern
        const uint8_t pattern[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = pattern[i % 4];
        }
        
        return data;
    }
    
    // Generate worst-case data (random, incompressible)
    std::vector<uint8_t> GenerateRandomData(size_t size)
    {
        std::vector<uint8_t> data(size);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = static_cast<uint8_t>(dis(gen));
        }
        
        return data;
    }

    constexpr std::string GetMassiveData()
    {
        return "Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo voluptas nulla pariatur? At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga. Et harum quidem rerum facilis est et expedita distinctio. Nam libero tempore, cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime placeat facere possimus, omnis voluptas assumenda est, omnis dolor repellendus. Temporibus autem quibusdam et aut officiis debitis aut rerum necessitatibus saepe eveniet ut et voluptates repudiandae sint et molestiae non recusandae. Itaque earum rerum hic tenetur a sapiente delectus, ut aut reiciendis voluptatibus maiores alias consequatur aut perferendis doloribus asperiores repellat. Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo voluptas nulla pariatur? At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga. Et harum quidem rerum facilis est et expedita distinctio. Nam libero tempore, cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime placeat facere possimus, omnis voluptas assumenda est, omnis dolor repellendus. Temporibus autem quibusdam et aut officiis debitis aut rerum necessitatibus saepe eveniet ut et voluptates repudiandae sint et molestiae non recusandae. Itaque earum rerum hic tenetur a sapiente delectus, ut aut reiciendis voluptatibus maiores alias consequatur aut perferendis doloribus asperiores repellat. Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo voluptas nulla pariatur? At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga. Et harum quidem rerum facilis est et expedita distinctio. Nam libero tempore, cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime placeat facere possimus, omnis voluptas assumenda est, omnis dolor repellendus. Temporibus autem quibusdam et aut officiis debitis aut rerum necessitatibus saepe eveniet ut et voluptates repudiandae sint et molestiae non recusandae. Itaque earum rerum hic tenetur a sapiente delectus, ut aut reiciendis voluptatibus maiores alias consequatur aut perferendis doloribus asperiores repellat. Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo voluptas nulla pariatur? At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga. Et harum quidem rerum facilis est et expedita distinctio. Nam libero tempore, cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime placeat facere possimus, omnis voluptas assumenda est, omnis dolor repellendus. Temporibus autem quibusdam et aut officiis debitis aut rerum necessitatibus saepe eveniet ut et voluptates repudiandae sint et molestiae non recusandae. Itaque earum rerum hic tenetur a sapiente delectus, ut aut reiciendis voluptatibus maiores alias consequatur aut perferendis doloribus asperiores repellat.Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae";
    }
};

// Test basic compression and decompression
TEST_F(CompressionTest, BasicCompressionDecompression)
{
    // Use larger, more repetitive text that will compress well
    const std::string testData = 
        "Hello, World! This is a test string for LZ4 compression. "
        "Hello, World! This is a test string for LZ4 compression. "
        "Hello, World! This is a test string for LZ4 compression. "
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog.";
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(testData.data());
    
    // Compress
    auto compressed = CompressLZ4(dataPtr, testData.size());
    ASSERT_FALSE(compressed.empty());
    
    // LZ4 frame format has overhead, but should still compress repetitive data
    // For small non-repetitive data, it might expand
    if (testData.size() > 100)
    {
        EXPECT_LT(compressed.size(), testData.size()); // Should be smaller for larger data
    }
    
    // Decompress
    auto result = DecompressLZ4(compressed.data(), compressed.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    
    // Verify data matches
    EXPECT_EQ(std::memcmp(decompressed.data(), dataPtr, testData.size()), 0);
}

// Test empty data handling
TEST_F(CompressionTest, EmptyData)
{
    // Compress empty data
    auto compressed = CompressLZ4(nullptr, 0);
    EXPECT_TRUE(compressed.empty());
    
    // Decompress empty data
    auto result = DecompressLZ4(nullptr, 0);
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(*result.GetError(), SerializationError::CorruptedData);
}

// Test small data (< 64 bytes)
TEST_F(CompressionTest, SmallData)
{
    const std::string testData = "Small";
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(testData.data());
    
    auto compressed = CompressLZ4(dataPtr, testData.size());
    ASSERT_FALSE(compressed.empty());
    
    auto result = DecompressLZ4(compressed.data(), compressed.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), dataPtr, testData.size()), 0);
}

// Test large data (1MB)
TEST_F(CompressionTest, LargeData)
{
    const size_t size = 1024 * 1024; // 1MB
    auto testData = GenerateTestData(size);
    
    auto compressed = CompressLZ4(testData.data(), testData.size());
    ASSERT_FALSE(compressed.empty());
    
    auto result = DecompressLZ4(compressed.data(), compressed.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), testData.data(), testData.size()), 0);
}

// Test highly compressible data
TEST_F(CompressionTest, RepetitiveData)
{
    const size_t size = 10000;
    auto testData = GenerateRepetitiveData(size);
    
    auto compressed = CompressLZ4(testData.data(), testData.size());
    ASSERT_FALSE(compressed.empty());
    
    // Should achieve good compression ratio
    float ratio = CompressionRatio(testData.size(), compressed.size());
    EXPECT_GT(ratio, 5.0f); // Expect at least 5x compression for repetitive data
    
    auto result = DecompressLZ4(compressed.data(), compressed.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), testData.data(), testData.size()), 0);
}

// Test incompressible (random) data
TEST_F(CompressionTest, RandomData)
{
    const size_t size = 10000;
    auto testData = GenerateRandomData(size);
    
    auto compressed = CompressLZ4(testData.data(), testData.size());
    ASSERT_FALSE(compressed.empty());
    
    // Random data shouldn't compress well (might even expand)
    float ratio = CompressionRatio(testData.size(), compressed.size());
    EXPECT_LT(ratio, 1.5f); // Expect little to no compression
    
    auto result = DecompressLZ4(compressed.data(), compressed.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), testData.data(), testData.size()), 0);
}

// Test block compression/decompression with header
TEST_F(CompressionTest, BlockCompression)
{
    const std::string testData = "This is test data for block compression with header.";
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(testData.data());
    
    // Compress with block header
    auto compressedBlock = CompressBlock(dataPtr, testData.size());
    ASSERT_FALSE(compressedBlock.empty());
    EXPECT_GE(compressedBlock.size(), sizeof(BlockHeader));
    
    // Verify header
    const auto* header = reinterpret_cast<const BlockHeader*>(compressedBlock.data());
    EXPECT_EQ(header->uncompressedSize, testData.size());
    EXPECT_EQ(header->compressedSize, compressedBlock.size() - sizeof(BlockHeader));
    
    // Decompress block
    auto result = DecompressBlock(compressedBlock.data(), compressedBlock.size());
    ASSERT_TRUE(result.IsOk());
    
    auto& decompressed = *result.GetValue();
    ASSERT_EQ(decompressed.size(), testData.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), dataPtr, testData.size()), 0);
}

// Test invalid block decompression
TEST_F(CompressionTest, InvalidBlockDecompression)
{
    // Too small for header
    uint8_t tooSmall[4] = {0};
    auto result1 = DecompressBlock(tooSmall, sizeof(tooSmall));
    EXPECT_TRUE(result1.IsErr());
    EXPECT_EQ(*result1.GetError(), SerializationError::CorruptedData);
    
    // Invalid size in header
    BlockHeader badHeader(100, 50);
    uint8_t badData[sizeof(BlockHeader) + 10];
    std::memcpy(badData, &badHeader, sizeof(BlockHeader));
    
    auto result2 = DecompressBlock(badData, sizeof(badData));
    EXPECT_TRUE(result2.IsErr());
    EXPECT_EQ(*result2.GetError(), SerializationError::CorruptedData);
}

// Test LZ4 format detection
TEST_F(CompressionTest, FormatDetection)
{
    const std::string testData = "Test data";
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(testData.data());
    
    // Compress data
    auto compressed = CompressLZ4(dataPtr, testData.size());
    ASSERT_FALSE(compressed.empty());
    
    // Should detect as LZ4
    EXPECT_TRUE(IsLZ4Compressed(compressed.data(), compressed.size()));
    
    // Random data should not be detected as LZ4
    uint8_t randomData[100];
    std::memset(randomData, 0xAB, sizeof(randomData));
    EXPECT_FALSE(IsLZ4Compressed(randomData, sizeof(randomData)));
    
    // Too small to check
    EXPECT_FALSE(IsLZ4Compressed(randomData, 3));
}

// Test compression ratio calculation
TEST_F(CompressionTest, CompressionRatioCalculation)
{
    EXPECT_FLOAT_EQ(CompressionRatio(1000, 500), 2.0f);
    EXPECT_FLOAT_EQ(CompressionRatio(1000, 250), 4.0f);
    EXPECT_FLOAT_EQ(CompressionRatio(1000, 1000), 1.0f);
    EXPECT_FLOAT_EQ(CompressionRatio(1000, 0), 0.0f); // Edge case
}

// Test multiple compression/decompression cycles
TEST_F(CompressionTest, MultipleCycles)
{
    const size_t iterations = 10;
    auto originalData = GenerateTestData(1000);
    
    for (size_t i = 0; i < iterations; ++i)
    {
        auto compressed = CompressLZ4(originalData.data(), originalData.size());
        ASSERT_FALSE(compressed.empty());
        
        auto result = DecompressLZ4(compressed.data(), compressed.size());
        ASSERT_TRUE(result.IsOk());
        
        auto& decompressed = *result.GetValue();
        ASSERT_EQ(decompressed.size(), originalData.size());
        EXPECT_EQ(std::memcmp(decompressed.data(), originalData.data(), originalData.size()), 0);
    }
}

// Test boundary conditions
TEST_F(CompressionTest, BoundaryConditions)
{
    // Test various sizes around common boundaries
    const size_t sizes[] = { 1, 15, 16, 17, 63, 64, 65, 255, 256, 257, 4095, 4096, 4097 };
    
    for (size_t size : sizes)
    {
        auto testData = GenerateTestData(size);
        
        auto compressed = CompressLZ4(testData.data(), testData.size());
        ASSERT_FALSE(compressed.empty()) << "Failed at size: " << size;
        
        auto result = DecompressLZ4(compressed.data(), compressed.size());
        ASSERT_TRUE(result.IsOk()) << "Failed at size: " << size;
        
        auto& decompressed = *result.GetValue();
        ASSERT_EQ(decompressed.size(), testData.size()) << "Failed at size: " << size;
        EXPECT_EQ(std::memcmp(decompressed.data(), testData.data(), testData.size()), 0) 
            << "Failed at size: " << size;
    }
}

// Test text data compression (common use case)
TEST_F(CompressionTest, TextDataCompression)
{
    // Use the massive Lorem Ipsum text
    const std::string massiveText = GetMassiveData();
    std::cout << "\nText Compression Test (" << massiveText.size() << " bytes):" << std::endl;
    
    const auto* dataPtr = reinterpret_cast<const uint8_t*>(massiveText.data());
    
    // Test each compression level on text
    CompressionLevel levels[] = {
        CompressionLevel::Fastest,
        CompressionLevel::Fast,
        CompressionLevel::Balanced
    };
    
    for (auto level : levels)
    {
        auto start = std::chrono::high_resolution_clock::now();
        auto compressed = CompressLZ4(dataPtr, massiveText.size(), level);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto timeUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        float speedMBps = static_cast<float>((massiveText.size() / 1024.0 / 1024.0) / (timeUs / 1000000.0));
        
        ASSERT_FALSE(compressed.empty());
        
        // Text should compress well due to repetition
        float ratio = CompressionRatio(massiveText.size(), compressed.size());
        
        // Decompress to verify
        auto result = DecompressLZ4(compressed.data(), compressed.size());
        ASSERT_TRUE(result.IsOk());
        
        auto& decompressed = *result.GetValue();
        ASSERT_EQ(decompressed.size(), massiveText.size());
        
        // Verify content matches
        std::string decompressedText(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
        EXPECT_EQ(decompressedText, massiveText);
        
        const char* levelName = (level == CompressionLevel::Fastest) ? "Fastest" :
                               (level == CompressionLevel::Fast) ? "Fast" : "Balanced";
        
        std::cout << "  " << levelName << ": " 
                  << speedMBps << " MB/s, "
                  << "compressed to " << compressed.size() << " bytes "
                  << "(ratio: " << ratio << "x)" << std::endl;
        
        // Text should compress to at least 2x
        EXPECT_GT(ratio, 2.0f);
    }
}

// Benchmark compression/decompression speed
TEST_F(CompressionTest, PerformanceBenchmark)
{
    const size_t dataSize = 100 * 1024; // 100KB
    auto testData = GenerateTestData(dataSize);
    
    // Benchmark compression
    const size_t iterations = 100;
    auto compressStart = std::chrono::high_resolution_clock::now();
    
    std::vector<uint8_t> compressed;
    for (size_t i = 0; i < iterations; ++i)
    {
        compressed = CompressLZ4(testData.data(), testData.size());
    }
    
    auto compressEnd = std::chrono::high_resolution_clock::now();
    auto compressTime = std::chrono::duration_cast<std::chrono::microseconds>(compressEnd - compressStart).count();
    float compressMBps = static_cast<float>((dataSize * iterations / 1024.0 / 1024.0) / (compressTime / 1000000.0));
    
    // Benchmark decompression
    auto decompressStart = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < iterations; ++i)
    {
        auto result = DecompressLZ4(compressed.data(), compressed.size());
        ASSERT_TRUE(result.IsOk());
    }
    
    auto decompressEnd = std::chrono::high_resolution_clock::now();
    auto decompressTime = std::chrono::duration_cast<std::chrono::microseconds>(decompressEnd - decompressStart).count();
    float decompressMBps = static_cast<float>((dataSize * iterations / 1024.0 / 1024.0) / (decompressTime / 1000000.0));
    
    // Print performance stats (informational)
    std::cout << "\nCompression Performance:" << std::endl;
    std::cout << "  Compression speed: " << compressMBps << " MB/s" << std::endl;
    std::cout << "  Decompression speed: " << decompressMBps << " MB/s" << std::endl;
    std::cout << "  Compression ratio: " << CompressionRatio(testData.size(), compressed.size()) << "x" << std::endl;
    
    // smallz4 uses optimal parsing which is slower but achieves better compression
    // Adjusted expectations for optimal parsing mode:
#ifdef NDEBUG
    // Throughput thresholds are meaningless in unoptimized builds
    // (Debug LZ4 measures ~2 MB/s); only enforce them in optimized configs.
    EXPECT_GT(compressMBps, 10.0f);    // Expect > 10 MB/s compression (optimal is slower)
    EXPECT_GT(decompressMBps, 100.0f); // Expect > 100 MB/s decompression (still fast)
#endif
}