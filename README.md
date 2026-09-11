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

// Components must be nothrow-move-constructible and nothrow-destructible.
// Trivially copyable components get memcpy fast paths; non-trivial types
// are fully supported via type-erased descriptors.
struct Position {
    float x, y, z;
};

struct Velocity {
    float dx, dy, dz;
};

int main() {
    // Create registry
    Astra::Registry registry;
    
    // Default-construct components
    auto entity = registry.CreateEntity<Position, Velocity>();
    
    // Or supply values directly
    auto other = registry.CreateEntityWith(Position{0, 0, 0}, Velocity{1, 0, 0});
    
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
  - MSVC 2022+ (Windows)
  - GCC 11+ (Linux)
  - Clang 13+ (macOS/Linux)
- Premake 5.0.0-beta6 or newer

### Build Instructions

#### Windows (Visual Studio)
```bash
# Generate Visual Studio 2022 (or later) solution
scripts/generate_vs2022.bat

# Open generated solution
Astra.sln
```

CI downloads premake 5.0.0-beta6 automatically. For local builds the premake5
binary must be on PATH or the scripts directory.

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

Components are data structures attached to entities. The `Component` concept
requires nothrow-move-constructible and nothrow-destructible. Trivially copyable
types get memcpy fast paths automatically; non-trivial types are fully supported
via type-erased descriptors.

```cpp
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
// Default-construct components
auto player = registry.CreateEntity<Transform, Health>();

// Or supply values
auto player = registry.CreateEntityWith(
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

// Or use range-based for loop - dereference yields references, not pointers
for (auto [entity, pos, vel] : view)
{
    // Range-based - Clean syntax (~3-4ns/entity)
    pos.x += vel.dx;
}
```

Structural mutation (create/destroy entity, add/remove component) during `ForEach` is unsupported; to change entity structure while iterating, record the changes into a `CommandBuffer` and call `Execute()` after the loop.

### Query Modifiers

- `Not<T>` - Exclude entities with component T
- `Optional<T>` - Include component T if present (can be nullptr)
- `AnyOf<T...>` - Require at least one of the specified components
- `OneOf<T...>` - Require exactly one of the specified components
- `Changed<T>` / `Added<T>` - Match only entities whose `T` changed / was added since a tick (see "Change detection" under Advanced Features below). Filter-what-you-fetch: `T` must *also* be listed as a fetched term in the view (`Changed<T>` requires `T` for matching, like `With<T>`, but contributes nothing to `ViewAccess` by itself) -- `CreateView<const Position, Changed<Position>>()`, not `CreateView<Changed<Position>>()`. A view with a change filter is iteration-only this stage (no `Size`/`Empty`/`Contains`/`Get`/`Single`/range-for -- each is a compile error naming the fix): use `view.ForEach(ctx, fn)` from a `SystemContext&` system (reads `ctx.LastRun()`) or the explicit form:

```cpp
auto v = registry.CreateView<const WorldTransform, Astra::Changed<WorldTransform>>();
v.Since(lastFrameTick).ForEach([](const WorldTransform& wt) { /* only entities changed since lastFrameTick */ });
```

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
// Default-construct 1000 entities (value-initialized: NSDMIs apply, PODs zeroed)
std::vector<Astra::Entity> enemies(1000);
registry.CreateEntities<Position, Velocity>(1000, enemies);

// Or supply per-entity values via a generator
registry.CreateEntitiesWith<Position, Velocity>(1000, enemies,
    [](size_t i) {
        return std::make_tuple(
            Position{static_cast<float>(i) * 10.0f, 0, 0},
            Velocity{-1, 0, 0}
        );
    });

// Batch destroy
registry.DestroyEntities(enemies);
```

## Behavioral Contracts (3.4)

The following behaviors are guaranteed across all build configurations (Debug, Release, Dist):

- Default-constructed components are **value-initialized** in every build configuration (NSDMIs apply; trivially-default-constructible types are zeroed) — single and batch creation agree.
- `Events::ComponentRemoved` fires **before** removal; the component pointer is valid only during the handler.
- CommandBuffer payloads support component alignment up to 16 bytes (compile-time enforced); component storage supports alignment up to 64 bytes. Over-aligned components must use direct Registry APIs.
- `CommandBuffer::CreateEntity`/`CreateEntities` allocate at record time and must be recorded only from the Registry-owning thread; all other commands may be recorded from workers via `ParallelCommandBuffer`.
- Invalid entity handles (exhaustion) and destroyed handles never enter component storage; batch creation reports how many entities were actually created and marks unfulfilled slots `Entity::Invalid()`.
- Archives are format v2: ISA-portable checksums, fixed-width sizes, and **resources are persisted**. v1 archives load (64-bit producers; checksum verified only on the producing ISA family).
- Views may be cached across frames: they observe entities added to pre-existing archetypes and survive `Defragment()`.
- Recoverable inputs (cycles, self-links, invalid entities) are rejected gracefully in all configs — Debug no longer asserts on them.

## Advanced Features

### Threading Model

Astra creates no threads. Registries are single-threaded by design: structural
changes (create/destroy/add/remove) must not race. The job system is an open
seam: inject an `IWorkScheduler` (e.g. an enkiTS adapter) via
`Registry::Config::workScheduler` and `ParallelForEach` /
`ParallelForEachDescendant` / `ParallelExecutor` will use it -- with no
scheduler injected they run sequentially inline. Structural changes from worker
threads are deferred via `CommandBuffer` (thread-safe). `RelationshipGraph`
traversal caches and `MetaRegistry` are internally synchronized so concurrent
reads through an injected scheduler stay safe.

```cpp
// No scheduler -- all Parallel* APIs run sequentially inline (the default)
Astra::Registry registry;

// With a scheduler -- parallel iteration uses the injected implementation
auto scheduler = std::make_shared<MyEnkiTSAdapter>();
Astra::Registry::Config config;
config.workScheduler = scheduler;
Astra::Registry registry(config);
```

#### System scheduling contract (built-in `SystemScheduler`)

The built-in scheduler is an **opt-in convenience**, not a guarantee. When you
inject an `IWorkScheduler` and use `ParallelExecutor`, it groups systems that
declare **disjoint** component masks and runs them concurrently. The rules:

- **Declared masks are a promise of purity.** A system in a multi-member
  parallel group must touch only the components in its `Reads`/`Writes` and must
  perform **no** structural changes (create/destroy entity, add/remove
  component). In Debug, an undeclared structural change trips an assert.
- **`Astra::Exclusive`** — mark a system that does structural changes or reaches
  outside its declared masks. It runs in its own solo group (nothing concurrent):

  ```cpp
  struct SpawnSystem : Astra::SystemTraits<Astra::Writes<Position>, Astra::Exclusive>
  {
      void operator()(Astra::Registry& r) { r.CreateEntity<Position>(); /* safe: solo */ }
  };
  ```

- **Ordering.** Only component-mask dependencies are honored. Within a parallel
  group, order is concurrent and unspecified. Independent systems are **not**
  reordered across insertion order (the plan is insertion-order-stable, and
  `SequentialExecutor` and `ParallelExecutor` produce identical observable
  order). A hidden dependency between two mask-independent systems in the same
  group (through a resource, event, or side effect) is a *system-order
  ambiguity* and is not honored — express it via masks.
- **Registration** (`AddSystem`) returns `Result<void, SystemError>` and must
  not race `Execute` (single-writer, like the `Registry` itself).

### Change detection

Astra can answer "did this component change since I last looked" at two tiers, both automatic
from how you already declare access -- there is no separate "mark it dirty" call on the common
path:

1. **Coarse, per chunk-per-column, always on, free.** Every component column in every chunk
   carries a 4-byte `Tick` version. It is stamped by one plain store whenever a view's access to
   that column is non-const (declare read-only views/args `const` to opt out) or a Registry write
   path (`Set`/`Emplace`/`Add`/`Modified`/`SetIfNeq`/Commands/archetype moves/`Deserialize`)
   touches the entity. No opt-in, no per-entity storage.
2. **Exact, per entity, opt-in, 8 bytes/entity/column.** A component that declares
   `static constexpr bool AstraChangeTracked = true;` (or specializes
   `Astra::ChangeTrackedTraits<T>`) gets a per-entity `{added, changed}` tick pair, carried across
   archetype moves and swap-removes exactly like the enableable-components bits. The type author
   pays this cost explicitly; every other component is unaffected.

`Changed<T>`/`Added<T>` (see Query Modifiers, above) reject a whole chunk first from its coarse
version before doing any per-entity work; only for a *tracked* `T` do they then run-scan the
chunk's tick column for per-entity precision (unioned with any enabled-run scan already in play
for that view). An *untracked* `T` stays chunk-granular: every entity in a stamped chunk is
reported, including ones the writer didn't actually touch that pass -- a documented false
positive, the same trade-off Unity DOTS and Bevy make at this tier.

```cpp
struct WorldTransform { static constexpr bool AstraChangeTracked = true; Mat4 value; };

auto v = registry.CreateView<WorldTransform>();   // non-const, tracked -> hands out Mut<T>
v.ForEach([](Astra::Mut<WorldTransform> wt) {
    wt.Write().value = someMatrix;   // marks changed
    (void)wt.Read();                 // never marks
    wt.SetIfNeq(someMatrix);         // marks only if someMatrix != current
    bool recent = wt.IsChanged(lastRun);
});
// existing code that takes `WorldTransform&` still compiles: Mut<T>'s implicit
// conversion to T& marks unconditionally, so a caller that only reads through
// it still gets a (harmless, chunk-already-stamped) mark -- use .Read() when
// you specifically want to avoid marking.
```

`Registry::Modified<T>(e)` marks at both tiers for raw-pointer code that held a `T*` across a
frame boundary -- call it after mutating through the pointer. `Registry::SetIfNeq<T>(e, value)`
(and `Mut<T>::SetIfNeq`) compares first and only stores + marks on inequality (`T` must be
`equality_comparable`); it is an explicit opt-in, never automatic (the default mark path does not
compare-then-store, by design).

**Tick semantics.** `Registry::CurrentTick()`/`AdvanceTick()` own a process-relative,
never-serialized `uint32_t` counter starting at 1 (`0` means "never"); comparisons are the
signed-difference `IsNewer(a, b) == (int32_t(a - b) > 0)`, valid for roughly 2^31
`AdvanceTick()` calls between two ticks being compared before it wraps -- documented, not
periodically corrected in this stage. `SystemContext::LastRun()`/`ThisRun()` give a scheduled
system its own previous/current tick; `SystemScheduler` advances the counter once per *segment*
of its execution plan (not once per individual system in that segment). A system's first run sees
`LastRun() == 0`, so every stamped chunk reads as changed -- the whole world looks new on frame
one, the Bevy semantic. Loading a save has the same effect: nothing tick-related is serialized,
and every restored chunk/entity is stamped with the *loader's* tick, so the first system to look
after a load sees everything as changed exactly once, then settles. A `Registry&`-only system (no
`SystemContext&` overload) receives no ticks automatically; filter with an explicit tick you store
yourself and drive `Changed`/`Added` views with `Since(tick)`.

Two more scheduler guarantees: after each plan segment finishes, `Execute()` advances the tick
once more before flushing deferred commands, so a flushed command -- or any write your own code
makes to the registry between two `Execute()` calls -- is always newer than every system's
`lastRun` and is never missed as a false negative. And if a `SystemScheduler` is reused against a
newly-constructed `Registry` whose tick counter is not strictly newer than a system's cached
`lastRun` (the common case: both start at 1), `Execute()` detects the staleness and resets every
system's `lastRun` to 0, so the first run against the new registry still sees everything rather
than nothing.

**`Single()`** stamps only the one entity's chunk it actually returns (through `Get`); its
internal count/locate pass is not itself a write, so nothing is stamped or marked on the `Empty`
or `MultipleMatched` paths, and no chunk it merely passed over is touched on success either.

**A stamped `ForEach` over an enableable-filtered view stamps every chunk it visits**, even one
whose enabled-entity intersection turns out empty for that pass -- consistent with "every chunk it
visits" at chunk granularity, not per actually-yielded run. A future change may narrow this to
only chunks that yielded at least one entity.

**Iteration-only, this stage.** A view with a change filter drops `Size`/`Empty`/`Contains`/
`Get`/`Single`/range-for entirely -- each is a compile-time error naming the fix
(`ForEach(ctx, fn)` or `Since(tick).ForEach(fn)`). Range-for over a *non-const* change-tracked
yield is refused at compile time for a related reason: the range-for iterator hands back a plain
`T&` and has no way to mark a change on dereference, so it is refused rather than silently
under-reporting changes. Iterate a tracked component through `ForEach` (which yields `Mut<T>`) or
request `const T` in the range-for.

**Main-thread writes between frames are seen.** Because ticks only ever advance and a system's
`lastRun` is only ever compared against, never rewound, a write your own code makes directly to a
tracked or coarse component (e.g. an editor moving a transform) between two `Execute()` calls
reads as changed to the very next system that looks at it -- there is no window where an
out-of-band write goes unnoticed.

### Memory Configuration

Configure memory allocation via `ArchetypeChunkPool::Config`:

```cpp
Astra::Registry::Config config;
config.chunkPoolConfig.chunkSize     = 16384;  // bytes per chunk (default 16KB)
config.chunkPoolConfig.chunksPerBlock = 128;   // chunks per allocator block
config.chunkPoolConfig.maxChunks     = 4096;   // hard cap
config.chunkPoolConfig.initialBlocks = 0;      // pre-warm blocks at startup
config.chunkPoolConfig.useHugePages  = true;   // 2MB huge pages when available
Astra::Registry registry(config);
```

### Multi-module (DLL) Usage

By default every module gets its own `DefaultTypeContext`, which assigns
component IDs independently. If a host EXE and a plugin DLL must share one
`Registry`, they must agree on IDs. The host creates a shared context and hands
it to each plugin before any ECS use:

```cpp
// Host EXE
auto ctx = std::make_unique<Astra::TypeContext>();
Astra::SetTypeContext(ctx.get());   // install in this module
LoadPlugin("myplugin.dll", ctx.get());

// Plugin DLL -- called by the host immediately after LoadLibrary
extern "C" void PluginInit(Astra::TypeContext* ctx)
{
    // Must run before any TypeID<T>::Value() or Registry use in this module.
    // Do NOT call TypeID<T>::Value() from your own static initializers --
    // IDs are cached in per-module statics on first access, which happens
    // before this call when triggered by a static initializer.
    Astra::SetTypeContext(ctx);
}
```

`SetTypeContext` also drains any pending static meta-registrations into the
context. The pending queue is module-local; registrations enqueued before
`SetTypeContext` is called (e.g. from `ASTRA_REFLECT` macros in static
initializers) are flushed when the context is installed.

A module that never unmaps (the engine EXE/DLL) should install with
`Astra::SetTypeContext(ctx, Astra::ModuleResidency::Resident)`: its reflected
metas then survive the last `ComponentModule` handle, so registry-less
`Astra::GetMeta` keeps resolving. Plugins keep the default `Transient`.

**Hot-reload sequence:** serialize world -> unload DLL -> load new DLL ->
`SetTypeContext` -> in the new image's `Init`, open a module handle and
register the types whose descriptors may have changed -> deserialize:

```cpp
// New image's Init, after SetTypeContext
auto mod = Astra::ComponentModule::Open(componentRegistry, "MyPlugin");
mod.Register<MyTypes...>();   // unconditionally rebuilds each descriptor
```

`Register<Ts...>()` rebuilds the descriptor for each type unconditionally
(move/copy/serialize/reflection function pointers) so they target the newly
loaded module code. Type IDs are stable across reloads because `TypeID`
resolves by XXHash64 of the type name through the shared `TypeContext`. The
old image's handle is destroyed in *its* `Shutdown`, before the DLL is
unloaded; that destruction automatically restores each descriptor to a
still-live earlier owner or clears it to unregistered, so nothing is ever
left pointing at freed code. The handle itself must be heap-held (e.g.
`std::unique_ptr<Astra::ComponentModule>`) and explicitly reset in
`Shutdown` -- never a DLL-static, since a static's destructor would run
under the loader lock during unload.

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
- [Flecs](https://github.com/SanderMertens/flecs) - Fast and lightweight C ECS
- [DOTS](https://unity.com/dots) - Unity's Data-Oriented Technology Stack
- [Mass](https://dev.epicgames.com/documentation/en-us/unreal-engine/mass-entity-in-unreal-engine) - Unreal Mass Entity
