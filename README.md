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
    
    // Query and iterate - 1.05ns per entity at 10K scale
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
- CMake 3.16+ or Premake5

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

### Query System

Astra's query system uses compile-time validation and optimization:

```cpp
// Basic queries
auto movables = registry.CreateView<Position, Velocity>();

// Advanced query modifiers
auto enemies = registry.CreateView<Position, Enemy, Not<Dead>>();
auto renderables = registry.CreateView<Transform, Optional<Sprite>>();
auto targets = registry.CreateView<Position, Any<Player, Enemy, NPC>>();
auto weapons = registry.CreateView<Item, OneOf<Sword, Bow, Staff>>();
```

Query modifiers:
- `Optional<T>` - Component may or may not exist (nullptr if absent)
- `Not<T>` - Exclude entities with component T
- `Any<T...>` - At least one of the specified components
- `OneOf<T...>` - Exactly one of the specified components

### Relationship System

Separate from component storage to prevent archetype fragmentation:

```cpp
// Hierarchies
registry.SetParent(child, parent);
auto relations = registry.GetRelations(parent);
for (Astra::Entity child : relations.GetChildren()) {
    // Process children
}

// Filtered relationships
auto physicsChildren = registry.GetRelations<RigidBody>(parent);
physicsChildren.ForEachDescendant([](Entity e, size_t depth, RigidBody& rb) {
    // Only descendants with RigidBody
});

// Bidirectional links
registry.AddLink(entity1, entity2);
```

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

// Iteration methods
view.ForEach([](Astra::Entity e, Position& pos, Velocity& vel) {
    // ForEach - Fastest (~1.05ns/entity)
    pos.x += vel.dx;
});

// Or use range-based for loop
for (auto [entity, pos, vel] : view)
{
    // Range-based - Clean syntax (~3-4ns/entity)
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

## Advanced Features

### Thread Safety

Enable thread-safe operations:

```cpp
Astra::Registry::Config config;
config.threadSafe = true;
config.initialArchetypeCapacity = 256;
Astra::Registry registry(config);
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

### SIMD Configuration

Astra automatically detects and uses available SIMD instructions:

- **x86/x64**: SSE2 (required), SSE4.2, AVX2
- **ARM**: NEON

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
- [Flecs](https://github.com/SanderMertens/flecs) - Fast and lightweight ECS
- [DOTS](https://unity.com/dots) - Unity's Data-Oriented Technology Stack
