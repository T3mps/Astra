# Astra ECS

A high-performance, archetype-based Entity Component System (ECS) library for modern C++20, featuring SIMD optimizations, relationship graphs, and cache-efficient iteration.

## Features

- **Archetype-based storage** - Entities with identical component sets grouped in contiguous 16KB chunks
- **SIMD acceleration** - Hardware-optimized operations (SSE2/SSE4.2/AVX2/NEON)
- **Relationship graphs** - Hierarchical parent-child and bidirectional entity links
- **Advanced queries** - Compile-time optimized queries with Optional, Not, Any, OneOf modifiers
- **Memory optimized** - Custom chunk allocator with huge page support (2MB pages)
- **Modern C++20** - Concepts, ranges, fold expressions, if constexpr

## Quick Start

```cpp
#include <Astra/Astra.hpp>

// Components must be trivially copyable
struct Position {
    float x, y, z;
};

struct Velocity {
    float dx, dy, dz;
};

int main() {
    // Create registry
    Astra::Registry registry;
    
    // Create entity with components
    auto entity = registry.CreateEntity<Position, Velocity>(
        Position{0, 0, 0},
        Velocity{1, 0, 0}
    );
    
    // Query and iterate
    auto view = registry.CreateView<Position, Velocity>();
    view.ForEach([](Astra::Entity e, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
        pos.z += vel.dz;
    });
    
    return 0;
}
```

## Building

### Requirements

- C++20 compatible compiler:
  - MSVC 2019+ (Windows)
  - GCC 9+ (Linux)
  - Clang 10+ (macOS/Linux)
- Premake5

### Build Instructions

#### Windows (Visual Studio)
```bash
# Generate Visual Studio 2022 solution
scripts/generate_vs2022.bat

# Open generated solution
Astra.sln
```

### Build Configurations

- **Debug** - Debug symbols, assertions enabled (`ASTRA_BUILD_DEBUG`)
- **Release** - Optimized with debug symbols (`ASTRA_BUILD_RELEASE`)
- **Dist** - Maximum optimization, no debug symbols (`ASTRA_BUILD_DIST`)

## Architecture Overview

### Archetype-Based Storage

Astra groups entities with identical component sets into "archetypes", storing components in Structure-of-Arrays format within 16KB memory chunks:

```
Archetype [Position, Velocity]:
  Chunk 0 (16KB):
    [Position][Position][Position]... (contiguous array)
    [Velocity][Velocity][Velocity]... (contiguous array)
    [Entity][Entity][Entity]...       (entity IDs)
  Chunk 1 (16KB):
    ... more entities ...
```

This design ensures:
- **Cache locality** - Components accessed together are stored together
- **SIMD-friendly** - Component arrays are naturally vectorizable
- **Memory efficiency** - Minimal fragmentation with chunk allocation
- **Fast iteration** - Linear memory access pattern

## Core Concepts

### Components

Components are simple data structures that hold entity data:

```cpp
// Components must be trivially copyable
struct Transform
{
    float x, y, z;
    float rotation;
    float scale;
};

struct Health
{
    int current;
    int max;
};

// Register component (optional, for runtime type info)
auto componentRegistry = registry.GetComponentRegistry();
componentRegistry->RegisterComponent<Transform>();
```

### Entities

Entities are lightweight IDs that reference component data:

```cpp
// Create entity with components
auto player = registry.CreateEntity<Transform, Health>(
    Transform{100, 0, 50, 0, 1},
    Health{100, 100}
);

// Add/remove components
registry.AddComponent<Velocity>(player, Velocity{0, 0, 0});
registry.RemoveComponent<Velocity>(player);

// Access components
if (auto* health = registry.GetComponent<Health>(player))
{
    health->current -= 10;
}

// Destroy entity
registry.DestroyEntity(player);
```

### Views and Queries

Views provide efficient iteration over entities with specific components:

```cpp
// Basic view - entities with Position AND Velocity
auto view = registry.CreateView<Position, Velocity>();

// With query modifiers
auto enemies = registry.CreateView<Position, Enemy, Astra::Not<Dead>>();
auto targets = registry.CreateView<Position, Astra::Any<Player, Enemy>>();
auto renderables = registry.CreateView<Transform, Astra::Optional<Sprite>>();

// Iteration methods (Optimized)
view.ForEach([](Astra::Entity e, Position& pos, Velocity& vel) {
    pos.x += vel.dx;
});

// Or use range-based for loop (Slower)
for (auto [entity, pos, vel] : view)
{
    pos->x += vel->dx;
}
```

### Query Modifiers

- `Not<T>` - Exclude entities with component T
- `Optional<T>` - Include component T if present (can be nullptr)
- `AnyOf<T...>` - Require at least one of the specified components
- `OneOf<T...>` - Require exactly one of the specified components

### Relationships

Astra supports entity relationships for hierarchies and graphs:

```cpp
// Parent-child relationships
auto parent = registry.CreateEntity<Transform>();
auto child = registry.CreateEntity<Transform>();
registry.SetParent(child, parent);

// Query relationships
auto relations = registry.GetRelations(parent);
for (Astra::Entity child : relations.GetChildren()) {
    // Process children
}

// Filtered relationships
auto physicsChildren = registry.GetRelations<RigidBody>(parent);
physicsChildren.ForEachChild([](Entity e, RigidBody& rb)
{
    // Only children with RigidBody component
});

// Entity links (many-to-many)
registry.AddLink(entity1, entity2);
for (Astra::Entity linked : relations.GetLinks()) {
    // Process linked entities
}
```

### Batch Operations

Optimize entity creation and destruction:

```cpp
// Batch create entities
std::vector<Astra::Entity> enemies(1000);
registry.CreateEntities<Position, Velocity>(1000, enemies,
    [](size_t i) {
        return std::make_tuple(
            Position{i * 10.0f, 0, 0},
            Velocity{-1, 0, 0}
        );
    });

// Batch destroy
registry.DestroyEntities(enemies);
```

### Memory Configuration

Configure memory allocation:

```cpp
Astra::Registry::Config config;
config.useHugePages = true;  // Use 2MB pages if available
config.chunkSize = 16384;    // 16KB chunks (default)
config.initialChunkCount = 100;
Astra::Registry registry(config);
```

## Examples

### Movement System

```cpp
void UpdateMovement(Astra::Registry& registry, float deltaTime) {
    auto view = registry.CreateView<Position, Velocity, Astra::Not<Frozen>>();
    
    view.ForEach([deltaTime](Astra::Entity e, Position& pos, Velocity& vel) {
        pos.x += vel.dx * deltaTime;
        pos.y += vel.dy * deltaTime;
        pos.z += vel.dz * deltaTime;
    });
}
```

### Hierarchy Transform

```cpp
void UpdateWorldTransforms(Astra::Registry& registry, Astra::Entity root) {
    auto relations = registry.GetRelations<Transform>(root);
    
    relations.ForEachDescendant(
        [](Astra::Entity e, size_t depth, Transform& local) {
            // Update world transform based on parent
            // Depth indicates hierarchy level
        },
        Astra::TraversalOrder::DepthFirst
    );
}
```

## Benchmarking

Run the included benchmarks!
<details>
<summary><b>📊 View Benchmark Results</b> (Click to expand)</summary>
<pre>
Run on (20 X 3610 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x10)
  L1 Instruction 32 KiB (x10)
  L2 Unified 1280 KiB (x10)
  L3 Unified 25600 KiB (x1)
------------------------------------------------------------------------------------------------------
Benchmark                                            Time             CPU   Iterations UserCounters...
------------------------------------------------------------------------------------------------------
BM_CreateEntities/10000                        1699554 ns      1675603 ns          373 items_per_second=5.968M/s
BM_CreateEntities/100000                      19845462 ns     19847973 ns           37 items_per_second=5.0383M/s
BM_CreateEntities/1000000                    418826150 ns    421875000 ns            2 items_per_second=2.37037M/s
BM_CreateEntitiesBatch/10000                   1201376 ns      1123047 ns          640 items_per_second=8.90435M/s
BM_CreateEntitiesBatch/100000                 13220805 ns     12957317 ns           41 items_per_second=7.71765M/s
BM_CreateEntitiesBatch/1000000               261201800 ns    260416667 ns            3 items_per_second=3.84M/s
BM_AddComponents/10000                         2795297 ns      2567488 ns          213 items_per_second=7.78971M/s
BM_AddComponents/100000                       31252659 ns     31250000 ns           22 items_per_second=6.4M/s
BM_AddComponents/1000000                     329732600 ns    335937500 ns            2 items_per_second=5.95349M/s
BM_RemoveComponents/10000                      1833717 ns      1708984 ns          320 items_per_second=5.85143M/s
BM_RemoveComponents/100000                    22870235 ns     22518382 ns           34 items_per_second=4.44082M/s
BM_RemoveComponents/1000000                  314693700 ns    312500000 ns            2 items_per_second=3.2M/s
BM_AddComponentsBatch/10000                    3015544 ns      3373580 ns          264 items_per_second=5.92842M/s
BM_AddComponentsBatch/100000                  35663590 ns     31250000 ns           21 items_per_second=6.4M/s
BM_AddComponentsBatch/1000000                322411300 ns    328125000 ns            2 items_per_second=6.09524M/s
BM_RemoveComponentsBatch/10000                 1492361 ns      1674107 ns          448 items_per_second=5.97333M/s
BM_RemoveComponentsBatch/100000               19771757 ns     19425676 ns           37 items_per_second=5.14783M/s
BM_RemoveComponentsBatch/1000000             232845400 ns    229166667 ns            3 items_per_second=4.36364M/s
BM_IterateSingleComponent/10000                  10194 ns        10045 ns        74667 items_per_second=995.56M/s
BM_IterateSingleComponent/100000                114100 ns       112305 ns         6400 items_per_second=890.435M/s
BM_IterateSingleComponent/1000000              1180738 ns      1196289 ns          640 items_per_second=835.918M/s
BM_IterateTwoComponents/10000                    22108 ns        22461 ns        32000 items_per_second=445.217M/s
BM_IterateTwoComponents/100000                  232710 ns       235395 ns         2987 items_per_second=424.818M/s
BM_IterateTwoComponents/1000000                2683937 ns      2698293 ns          249 items_per_second=370.605M/s
BM_IterateTwoComponentsHalf/10000                11740 ns        11719 ns        56000 items_per_second=426.667M/s
BM_IterateTwoComponentsHalf/100000              113819 ns       114746 ns         6400 items_per_second=435.745M/s
BM_IterateTwoComponentsHalf/1000000            1247279 ns      1227679 ns          560 items_per_second=407.273M/s
BM_IterateTwoComponentsOne/10000                  4.18 ns         4.08 ns    172307692 items_per_second=245.06M/s
BM_IterateTwoComponentsOne/100000                 4.21 ns         4.24 ns    165925926 items_per_second=235.984M/s
BM_IterateTwoComponentsOne/1000000                4.21 ns         4.20 ns    160000000 items_per_second=238.14M/s
BM_IterateThreeComponents/10000                  30004 ns        29646 ns        26353 items_per_second=337.318M/s
BM_IterateThreeComponents/100000                295686 ns       291561 ns         2358 items_per_second=342.982M/s
BM_IterateThreeComponents/1000000              4757289 ns      4632994 ns          172 items_per_second=215.843M/s
BM_IterateFiveComponents/10000                   45117 ns        44993 ns        14933 items_per_second=222.259M/s
BM_IterateFiveComponents/100000                 464404 ns       464965 ns         1445 items_per_second=215.07M/s
BM_IterateFiveComponents/1000000               5423535 ns      5301339 ns          112 items_per_second=188.632M/s
BM_ThreadInfo                                      565 ns          572 ns      1120000 Hardware Threads: 20
BM_ParallelIterateSingleComponent/10000          31182 ns        10358 ns        49778 items_per_second=965.392M/s
BM_ParallelIterateSingleComponent/100000        226136 ns        19252 ns        37333 items_per_second=5.19416G/s
BM_ParallelIterateSingleComponent/1000000      2196255 ns        43750 ns        10000 items_per_second=22.8571G/s
BM_ParallelIterateTwoComponents/10000            50692 ns        17787 ns        29867 items_per_second=562.202M/s
BM_ParallelIterateTwoComponents/100000          424822 ns        23717 ns        11200 items_per_second=4.21647G/s
BM_ParallelIterateTwoComponents/1000000        4259113 ns        46875 ns         1000 items_per_second=21.3333G/s
BM_ParallelIterateTwoComponentsHalf/10000        30784 ns        12905 ns        89600 items_per_second=387.459M/s
BM_ParallelIterateTwoComponentsHalf/100000      217012 ns        19392 ns       112000 items_per_second=2.57842G/s
BM_ParallelIterateTwoComponentsHalf/1000000    2119719 ns        36272 ns         5600 items_per_second=13.7846G/s
BM_ParallelIterateTwoComponentsOne/10000          6.15 ns         6.14 ns    112000000 items_per_second=162.909M/s
BM_ParallelIterateTwoComponentsOne/100000         6.17 ns         6.14 ns    112000000 items_per_second=162.909M/s
BM_ParallelIterateTwoComponentsOne/1000000        6.10 ns         6.14 ns    112000000 items_per_second=162.909M/s
BM_ParallelIterateThreeComponents/10000          68054 ns        14893 ns        64000 items_per_second=671.475M/s
BM_ParallelIterateThreeComponents/100000        596930 ns        26562 ns        10000 items_per_second=3.76471G/s
BM_ParallelIterateThreeComponents/1000000      5955497 ns       140625 ns         1000 items_per_second=7.11111G/s
BM_ParallelIterateFiveComponents/10000          100959 ns        16044 ns        89600 items_per_second=623.304M/s
BM_ParallelIterateFiveComponents/100000         966520 ns        34375 ns        10000 items_per_second=2.90909G/s
BM_ParallelIterateFiveComponents/1000000       9728347 ns       140625 ns         1000 items_per_second=7.11111G/s
BM_GetComponent/10000                           108224 ns       104980 ns         6400 items_per_second=95.2558M/s
BM_GetComponent/100000                         1780665 ns      1801273 ns          373 items_per_second=55.5163M/s
BM_GetComponent/1000000                       67075456 ns     65972222 ns            9 items_per_second=15.1579M/s
BM_GetMultipleComponents/10000                  166540 ns       164958 ns         4073 items_per_second=121.243M/s
BM_GetMultipleComponents/100000                2697545 ns      2698293 ns          249 items_per_second=74.1209M/s
BM_GetMultipleComponents/1000000              93243686 ns     93750000 ns            7 items_per_second=21.3333M/s
BM_HierarchyTraversal/1000                      262696 ns       245536 ns         2800 items_per_second=5.55927M/s
BM_HierarchyTraversal/10000                     258252 ns       256696 ns         2800 items_per_second=5.31757M/s
BM_HierarchyTraversal/100000                  11633988 ns     11718750 ns           56 items_per_second=4.77756M/s
BM_HierarchyForEach/1000                        205375 ns       205078 ns         3200 items_per_second=6.656M/s
BM_HierarchyForEach/10000                       208496 ns       205078 ns         3200 items_per_second=6.656M/s
BM_HierarchyForEach/100000                    10072355 ns     10000000 ns           75 items_per_second=5.5987M/s
BM_FilteredHierarchyTraversal/1000              267361 ns       266841 ns         2635 items_per_second=2.55583M/s
BM_FilteredHierarchyTraversal/10000             268564 ns       266841 ns         2635 items_per_second=2.55583M/s
BM_FilteredHierarchyTraversal/100000          13146475 ns     12834821 ns           56 items_per_second=2.18102M/s
BM_SystemScheduler_Sequential/10000              12634 ns        11998 ns        56000 items_per_second=2.50047G/s
BM_SystemScheduler_Sequential/100000            141028 ns       141246 ns         4978 items_per_second=2.12395G/s
BM_SystemScheduler_Lambda/10000                   9824 ns         9835 ns        74667 items_per_second=2.03348G/s
BM_SystemScheduler_Lambda/100000                126389 ns       119978 ns         5600 items_per_second=1.66698G/s
BM_SystemScheduler_Parallel/10000                10895 ns        10742 ns        64000 items_per_second=3.72364G/s
BM_SystemScheduler_Parallel/100000              137460 ns       136021 ns         4480 items_per_second=2.94072G/s
BM_SystemScheduler_ManyIndependent                7456 ns         7254 ns       112000 items_per_second=689.231k/s
BM_SystemScheduler_WithDependencies              32492 ns        32227 ns        21333 items_per_second=1.55149G/s
BM_SystemScheduler_CustomExecutor/10000           9802 ns         9766 ns        64000 items_per_second=2.048G/s
BM_SystemScheduler_CustomExecutor/100000        122388 ns       119978 ns         5600 items_per_second=1.66698G/s
</pre>
</details>

## Contributing

Contributions are welcome! Please ensure:

1. Code follows existing style conventions
2. All tests pass
3. Benchmarks show no performance regression
4. New features include tests

## License

Astra is available under the MIT License. See LICENSE file for details.

## Acknowledgments

Inspired by:
- [EnTT](https://github.com/skypjack/entt) - Modern C++ ECS
- [Flecs](https://github.com/SanderMertens/flecs) - Fast and lightweight C ECS
- [DOTS](https://unity.com/dots) - Unity's Data-Oriented Technology Stack
- [Mass](https://dev.epicgames.com/documentation/en-us/unreal-engine/mass-entity-in-unreal-engine) - Unreal Mass Entity
