#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "../TestComponents.hpp"
#include "Astra/Registry/Registry.hpp"

// Define test resource types (singleton components)
namespace Astra::Test
{
    // Small resource that fits in SBO (≤64 bytes)
    struct TimeResource
    {
        float deltaTime = 0.016f;
        float totalTime = 0.0f;
        float timeScale = 1.0f;
        int frameCount = 0;
    };
    
    // Medium resource close to SBO limit
    struct RenderSettings
    {
        int width = 1920;
        int height = 1080;
        int refreshRate = 60;
        bool vsync = true;
        bool fullscreen = false;
        float brightness = 1.0f;
        float contrast = 1.0f;
        float gamma = 2.2f;
        int antiAliasing = 4;
        int shadowQuality = 3;
        int textureQuality = 3;
        int effectQuality = 3;
    };
    
    // Large resource that exceeds SBO (>64 bytes)
    struct PhysicsSettings
    {
        float gravity[3] = {0.0f, -9.81f, 0.0f};
        float worldMin[3] = {-1000.0f, -1000.0f, -1000.0f};
        float worldMax[3] = {1000.0f, 1000.0f, 1000.0f};
        int solverIterations = 8;
        int velocityIterations = 1;
        float fixedTimeStep = 0.02f;
        int maxSubSteps = 4;
        float contactOffset = 0.01f;
        float restOffset = 0.0f;
        float bounceThreshold = 2.0f;
        float sleepThreshold = 0.005f;
        float defaultFriction = 0.5f;
        float defaultRestitution = 0.0f;
        bool enableCCD = true;
        bool enableAdaptiveForce = true;
        float padding[8] = {0}; // Ensure this exceeds 64 bytes
    };
    
    // Tag resource (empty)
    struct DebugModeResource {};
    
    // Resource with specific alignment requirements
    struct AlignedResource
    {
        alignas(32) float data[8] = {0};
    };
}

class ResourceTest : public ::testing::Test
{
protected:
    std::unique_ptr<Astra::Registry> registry;
    
    void SetUp() override
    {
        registry = std::make_unique<Astra::Registry>();
    }
    
    void TearDown() override
    {
        registry.reset();
    }
};

// Basic Operations Tests

TEST_F(ResourceTest, SetAndGetResource)
{
    using namespace Astra::Test;
    
    // Test small resource (SBO)
    auto* time = registry->SetResource(TimeResource{0.016f, 100.0f, 1.5f, 1000});
    ASSERT_NE(time, nullptr);
    EXPECT_FLOAT_EQ(time->deltaTime, 0.016f);
    EXPECT_FLOAT_EQ(time->totalTime, 100.0f);
    EXPECT_FLOAT_EQ(time->timeScale, 1.5f);
    EXPECT_EQ(time->frameCount, 1000);
    
    // Get the same resource
    auto* retrievedTime = registry->GetResource<TimeResource>();
    ASSERT_NE(retrievedTime, nullptr);
    EXPECT_EQ(time, retrievedTime);
    
    // Test large resource (heap allocation)
    auto* physics = registry->SetResource(PhysicsSettings{});
    ASSERT_NE(physics, nullptr);
    EXPECT_FLOAT_EQ(physics->gravity[1], -9.81f);
    
    auto* retrievedPhysics = registry->GetResource<PhysicsSettings>();
    ASSERT_NE(retrievedPhysics, nullptr);
    EXPECT_EQ(physics, retrievedPhysics);
}

TEST_F(ResourceTest, UpdateResource)
{
    using namespace Astra::Test;
    
    // Set initial resource
    registry->SetResource(TimeResource{0.016f, 0.0f, 1.0f, 0});
    
    // Update the resource
    auto* updated = registry->SetResource(TimeResource{0.033f, 100.0f, 2.0f, 60});
    ASSERT_NE(updated, nullptr);
    EXPECT_FLOAT_EQ(updated->deltaTime, 0.033f);
    EXPECT_FLOAT_EQ(updated->totalTime, 100.0f);
    EXPECT_FLOAT_EQ(updated->timeScale, 2.0f);
    EXPECT_EQ(updated->frameCount, 60);
}

TEST_F(ResourceTest, HasResource)
{
    using namespace Astra::Test;
    
    // Initially no resources
    EXPECT_FALSE(registry->HasResource<TimeResource>());
    EXPECT_FALSE(registry->HasResource<RenderSettings>());
    
    // Add a resource
    registry->SetResource(TimeResource{});
    EXPECT_TRUE(registry->HasResource<TimeResource>());
    EXPECT_FALSE(registry->HasResource<RenderSettings>());
    
    // Add another resource
    registry->SetResource(RenderSettings{});
    EXPECT_TRUE(registry->HasResource<TimeResource>());
    EXPECT_TRUE(registry->HasResource<RenderSettings>());
}

TEST_F(ResourceTest, RemoveResource)
{
    using namespace Astra::Test;
    
    // Add resources
    registry->SetResource(TimeResource{});
    registry->SetResource(RenderSettings{});
    EXPECT_TRUE(registry->HasResource<TimeResource>());
    EXPECT_TRUE(registry->HasResource<RenderSettings>());
    
    // Remove one resource
    registry->RemoveResource<TimeResource>();
    EXPECT_FALSE(registry->HasResource<TimeResource>());
    EXPECT_TRUE(registry->HasResource<RenderSettings>());
    
    // Remove should be safe to call on non-existent resource
    registry->RemoveResource<TimeResource>(); // Should not crash
    EXPECT_FALSE(registry->HasResource<TimeResource>());
    
    // Remove the other resource
    registry->RemoveResource<RenderSettings>();
    EXPECT_FALSE(registry->HasResource<RenderSettings>());
}

TEST_F(ResourceTest, ClearResources)
{
    using namespace Astra::Test;
    
    // Add multiple resources
    registry->SetResource(TimeResource{});
    registry->SetResource(RenderSettings{});
    registry->SetResource(PhysicsSettings{});
    EXPECT_TRUE(registry->HasResource<TimeResource>());
    EXPECT_TRUE(registry->HasResource<RenderSettings>());
    EXPECT_TRUE(registry->HasResource<PhysicsSettings>());
    
    // Clear all resources
    registry->ClearResources();
    EXPECT_FALSE(registry->HasResource<TimeResource>());
    EXPECT_FALSE(registry->HasResource<RenderSettings>());
    EXPECT_FALSE(registry->HasResource<PhysicsSettings>());
}

// Memory Layout Tests

TEST_F(ResourceTest, SmallResourceUseSBO)
{
    using namespace Astra::Test;
    
    // Small resources should use Small Buffer Optimization
    auto* time = registry->SetResource(TimeResource{});
    ASSERT_NE(time, nullptr);
    
    // Verify we can store multiple small resources
    auto* debug = registry->SetResource(DebugModeResource{});
    ASSERT_NE(debug, nullptr);
    
    // Both should be accessible
    EXPECT_NE(registry->GetResource<TimeResource>(), nullptr);
    EXPECT_NE(registry->GetResource<DebugModeResource>(), nullptr);
}

TEST_F(ResourceTest, LargeResourceUseHeap)
{
    using namespace Astra::Test;
    
    // Large resource should use heap allocation from chunks
    auto* physics = registry->SetResource(PhysicsSettings{});
    ASSERT_NE(physics, nullptr);
    
    // Verify the resource is properly stored
    auto* retrieved = registry->GetResource<PhysicsSettings>();
    EXPECT_EQ(physics, retrieved);
    
    // Should work with multiple large resources
    RenderSettings largeRender;
    for (int i = 0; i < 10; ++i) {
        largeRender.width = 1920 + i;
        auto* render = registry->SetResource(std::move(largeRender));
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->width, 1920 + i);
    }
}

TEST_F(ResourceTest, TagResourceZeroMemory)
{
    using namespace Astra::Test;
    
    // Tag resources should not allocate memory
    auto* debug = registry->SetResource(DebugModeResource{});
    ASSERT_NE(debug, nullptr);
    
    // Should be retrievable
    EXPECT_TRUE(registry->HasResource<DebugModeResource>());
    EXPECT_NE(registry->GetResource<DebugModeResource>(), nullptr);
    
    // Should be removable
    registry->RemoveResource<DebugModeResource>();
    EXPECT_FALSE(registry->HasResource<DebugModeResource>());
}

// Stress Tests

TEST_F(ResourceTest, ManyResourceTypes)
{
    using namespace Astra::Test;
    
    // Define many resource types
    struct Resource1 { int value = 1; };
    struct Resource2 { int value = 2; };
    struct Resource3 { int value = 3; };
    struct Resource4 { int value = 4; };
    struct Resource5 { int value = 5; };
    struct Resource6 { int value = 6; };
    struct Resource7 { int value = 7; };
    struct Resource8 { int value = 8; };
    struct Resource9 { int value = 9; };
    struct Resource10 { int value = 10; };
    
    // Add all resources
    registry->SetResource(Resource1{});
    registry->SetResource(Resource2{});
    registry->SetResource(Resource3{});
    registry->SetResource(Resource4{});
    registry->SetResource(Resource5{});
    registry->SetResource(Resource6{});
    registry->SetResource(Resource7{});
    registry->SetResource(Resource8{});
    registry->SetResource(Resource9{});
    registry->SetResource(Resource10{});
    
    // Verify all are accessible
    EXPECT_EQ(registry->GetResource<Resource1>()->value, 1);
    EXPECT_EQ(registry->GetResource<Resource2>()->value, 2);
    EXPECT_EQ(registry->GetResource<Resource3>()->value, 3);
    EXPECT_EQ(registry->GetResource<Resource4>()->value, 4);
    EXPECT_EQ(registry->GetResource<Resource5>()->value, 5);
    EXPECT_EQ(registry->GetResource<Resource6>()->value, 6);
    EXPECT_EQ(registry->GetResource<Resource7>()->value, 7);
    EXPECT_EQ(registry->GetResource<Resource8>()->value, 8);
    EXPECT_EQ(registry->GetResource<Resource9>()->value, 9);
    EXPECT_EQ(registry->GetResource<Resource10>()->value, 10);
}

TEST_F(ResourceTest, RapidAddRemove)
{
    using namespace Astra::Test;
    
    // Rapidly add and remove resources
    for (int i = 0; i < 1000; ++i)
    {
        // Add resources
        auto* time = registry->SetResource(TimeResource{0.016f * i, float(i), 1.0f, i});
        ASSERT_NE(time, nullptr);
        EXPECT_EQ(time->frameCount, i);
        
        auto* render = registry->SetResource(RenderSettings{1920 + i, 1080, 60});
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->width, 1920 + i);
        
        // Verify they exist
        EXPECT_TRUE(registry->HasResource<TimeResource>());
        EXPECT_TRUE(registry->HasResource<RenderSettings>());
        
        // Remove them
        registry->RemoveResource<TimeResource>();
        registry->RemoveResource<RenderSettings>();
        
        // Verify they're gone
        EXPECT_FALSE(registry->HasResource<TimeResource>());
        EXPECT_FALSE(registry->HasResource<RenderSettings>());
    }
}

TEST_F(ResourceTest, UpdateManyTimes)
{
    using namespace Astra::Test;
    
    // Initial resource
    registry->SetResource(TimeResource{0.016f, 0.0f, 1.0f, 0});
    
    // Update many times
    for (int i = 1; i <= 1000; ++i)
    {
        auto* time = registry->SetResource(TimeResource{0.016f, 0.016f * i, 1.0f, i});
        ASSERT_NE(time, nullptr);
        EXPECT_FLOAT_EQ(time->totalTime, 0.016f * i);
        EXPECT_EQ(time->frameCount, i);
    }
    
    // Final state check
    auto* finalTime = registry->GetResource<TimeResource>();
    ASSERT_NE(finalTime, nullptr);
    EXPECT_FLOAT_EQ(finalTime->totalTime, 0.016f * 1000);
    EXPECT_EQ(finalTime->frameCount, 1000);
}

// Thread Safety Tests (if ResourceStorage supports concurrent access)

TEST_F(ResourceTest, ConcurrentReads)
{
    using namespace Astra::Test;
    
    // Add resources
    registry->SetResource(TimeResource{0.016f, 100.0f, 1.0f, 1000});
    registry->SetResource(RenderSettings{1920, 1080, 60});
    registry->SetResource(PhysicsSettings{});
    
    // Multiple threads reading simultaneously
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    
    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back([this, &successCount]()
        {
            for (int j = 0; j < 100; ++j)
            {
                auto* time = registry->GetResource<TimeResource>();
                auto* render = registry->GetResource<RenderSettings>();
                auto* physics = registry->GetResource<PhysicsSettings>();
                
                if (time && render && physics)
                {
                    successCount++;
                }
            }
        });
    }
    
    for (auto& thread : threads)
    {
        thread.join();
    }
    
    EXPECT_EQ(successCount, 1000); // 10 threads * 100 iterations
}

// Edge Cases

TEST_F(ResourceTest, GetNonExistentResource)
{
    using namespace Astra::Test;
    
    // Getting non-existent resource should return nullptr
    EXPECT_EQ(registry->GetResource<TimeResource>(), nullptr);
    EXPECT_EQ(registry->GetResource<RenderSettings>(), nullptr);
    EXPECT_EQ(registry->GetResource<PhysicsSettings>(), nullptr);
}

TEST_F(ResourceTest, RemoveNonExistentResource)
{
    using namespace Astra::Test;
    
    // Removing non-existent resource should not crash
    registry->RemoveResource<TimeResource>();
    registry->RemoveResource<RenderSettings>();
    registry->RemoveResource<PhysicsSettings>();
    
    // Registry should still be functional
    auto* time = registry->SetResource(TimeResource{});
    ASSERT_NE(time, nullptr);
    EXPECT_TRUE(registry->HasResource<TimeResource>());
}

TEST_F(ResourceTest, ClearEmptyRegistry)
{
    // Clearing empty registry should not crash
    registry->ClearResources();
    
    // Registry should still be functional
    using namespace Astra::Test;
    auto* time = registry->SetResource(TimeResource{});
    ASSERT_NE(time, nullptr);
    EXPECT_TRUE(registry->HasResource<TimeResource>());
}

// Performance-Related Tests

TEST_F(ResourceTest, AccessPatternCache)
{
    using namespace Astra::Test;
    
    // Add resources that should be in cache
    registry->SetResource(TimeResource{});
    registry->SetResource(RenderSettings{});
    registry->SetResource(PhysicsSettings{});
    
    // Access in patterns that test cache behavior
    for (int i = 0; i < 1000; ++i)
    {
        // Sequential access (cache-friendly)
        auto* time = registry->GetResource<TimeResource>();
        time->frameCount++;
        
        auto* render = registry->GetResource<RenderSettings>();
        render->width = 1920 + (i % 10);
        
        auto* physics = registry->GetResource<PhysicsSettings>();
        physics->gravity[1] = -9.81f - (i * 0.001f);
    }
    
    // Verify final state
    EXPECT_EQ(registry->GetResource<TimeResource>()->frameCount, 1000);
    EXPECT_EQ(registry->GetResource<RenderSettings>()->width, 1920 + (999 % 10));
    EXPECT_FLOAT_EQ(registry->GetResource<PhysicsSettings>()->gravity[1], -9.81f - 0.999f);
}

TEST_F(ResourceTest, MixedSizeResources)
{
    using namespace Astra::Test;
    
    // Mix of different sized resources to test memory allocation
    struct TinyResource { char data = 'A'; };
    struct SmallResource { int values[8] = {0}; };
    struct MediumResource { float matrix[12] = {0}; };
    struct LargeResource { double bigData[32] = {0}; };
    
    // Add them in various orders
    auto* large = registry->SetResource(LargeResource{});
    auto* tiny = registry->SetResource(TinyResource{'B'});
    auto* medium = registry->SetResource(MediumResource{});
    auto* small = registry->SetResource(SmallResource{});
    
    // Verify all are accessible
    ASSERT_NE(large, nullptr);
    ASSERT_NE(tiny, nullptr);
    ASSERT_NE(medium, nullptr);
    ASSERT_NE(small, nullptr);
    
    EXPECT_EQ(registry->GetResource<TinyResource>()->data, 'B');
    EXPECT_EQ(registry->GetResource<LargeResource>(), large);
}

// Alignment Tests

TEST_F(ResourceTest, AlignedResourceStorage)
{
    using namespace Astra::Test;
    
    // Set aligned resource
    auto* aligned = registry->SetResource(AlignedResource{});
    ASSERT_NE(aligned, nullptr);
    
    // Check alignment (should be 32-byte aligned)
    EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % 32, 0u);
    
    // Verify data access
    for (int i = 0; i < 8; ++i)
    {
        aligned->data[i] = float(i);
    }
    
    auto* retrieved = registry->GetResource<AlignedResource>();
    ASSERT_NE(retrieved, nullptr);
    for (int i = 0; i < 8; ++i)
    {
        EXPECT_FLOAT_EQ(retrieved->data[i], float(i));
    }
}