# W1 — Unified Paged Entity Record — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ArchetypeManager`'s `std::unordered_map<Entity, EntityRecord> m_entityMap` with a paged, id-indexed record table that shares its slot with the liveness version, so validate + locate is one paged lookup (no hashing, no per-create heap allocation).

**Architecture:** Widen the existing (EntityManager-only) `EntityTable` paged-segment store from a `VersionType` payload to an `EntityRecord { Archetype*, EntityLocation, version }` payload. `EntityManager` keeps owning it and drives the version field (Create/Destroy/IsValid). `ArchetypeManager` drops its map and holds a raw pointer to the same table (injected by the non-movable `Registry`, so the pointer is stable), writing only `archetype`/`location`.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln` via MSBuild, 3 configs), GoogleTest, premake5 project generation.

**Spec:** `docs/superpowers/specs/2026-07-21-w1-unified-entity-record-design.md`

## Global Constraints

Copied verbatim from the spec — every task must honor all of these:

- **Header-only, C++20.** No new translation units in `include/`; no new third-party dependencies.
- **Exception-free + RTTI-off compatible.** No `throw`, no `dynamic_cast`/`typeid`. Match surrounding `ASTRA_*` macros and `noexcept` usage.
- **`ArchetypeManager` must NEVER write `EntityRecord::version`.** Only `EntityManager::Destroy`/`SetVersion` mutate `version`; `EntityManager::Destroy` reads `GetVersion(id)` to compute the recycled next-version, so a stray write breaks id recycling.
- **Deserialize ordering:** `EntityManager` restores versions **before** `ArchetypeManager` restores locations (they share the same slots). `ArchetypeManager` must not `Clear()`/`Reserve()` the shared table — `EntityManager` owns that lifecycle.
- **Public API + on-disk serialization format unchanged.** `Registry`/`ArchetypeManager` signatures stay; `EntityRecord` only gains a `version` field (source-compatible for existing `archetype`/`location` readers).
- **Find semantics:** a slot is "located" iff `version == entity.GetVersion() && archetype != nullptr` (reproduces the old map's "entry exists ⇔ located").
- **Stay clear of the TypeID ceiling in tests:** reuse `tests/TestComponents.hpp` (`Astra::Test::*`) / existing component types; `EntityTable` tests use `reinterpret_cast<Archetype*>` dummies and register **zero** components.
- **Verification is 3-config:** MSVC **Debug, Release, Dist** must all stay green. Build the whole solution (`-t:AstraTest` does not work). Baseline was ~602 Debug / 600 Release+Dist on `dev`; gate on "all configs green + intended new tests", not an absolute count.

**Build / test / bench commands (reused throughout):**

```bash
# Build one config (repeat for Debug, Release, Dist):
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m

# Regenerate the solution after ADDING a new source/test file:
D:/dev/_shared/tools/premake5 vs2022

# Run the tests (per config):
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter='EntityRecordTable.*'

# Re-run the head-to-head benchmark (Astra side) — see bench-compare/ and its build_one.bat.
```

> If the working tree is dirty with unrelated untracked files, build in an isolated `git worktree add --detach <scratch> HEAD` and run premake inside it (`vendor/` is tracked so it builds standalone). We are on branch `perf/w1-unified-entity-record`.

---

### Task 1: Extract `EntityLocation` into its own header

Decouples `EntityLocation` from the heavy `Archetype.hpp` so `EntityRecord.hpp` (needed by the Entity layer) can include it without pulling in the archetype/chunk machinery. Pure move — no behavior change.

**Files:**
- Create: `include/Astra/Archetype/EntityLocation.hpp`
- Modify: `include/Astra/Archetype/Archetype.hpp` (remove the `struct EntityLocation {…}` block, add an include)

**Interfaces:**
- Produces: `Astra::EntityLocation` (unchanged definition) now available from a lightweight header.

- [ ] **Step 1: Create the new header with the moved struct**

Create `include/Astra/Archetype/EntityLocation.hpp` containing the **entire** existing `struct EntityLocation { … };` block currently in `Archetype.hpp` (approx. lines 44–72 — copy it verbatim, including all constructors and `Get*`/`Create` members and any comparison operators):

```cpp
#pragma once

#include <cstdint>
#include <limits>

#include "../Core/Base.hpp"

namespace Astra
{
    struct EntityLocation
    {
        uint32_t chunkIndex;
        uint32_t entityIndex;

        constexpr EntityLocation() noexcept :
            chunkIndex(std::numeric_limits<uint32_t>::max()),
            entityIndex(std::numeric_limits<uint32_t>::max())
        {}

        constexpr EntityLocation(uint32_t chunk, uint32_t entity) noexcept : chunkIndex(chunk), entityIndex(entity) {}

        ASTRA_NODISCARD constexpr static EntityLocation Create(size_t chunkIndex, size_t entityIndex) noexcept
        {
            return EntityLocation(static_cast<uint32_t>(chunkIndex), static_cast<uint32_t>(entityIndex));
        }

        ASTRA_NODISCARD constexpr size_t GetChunkIndex() const noexcept { return chunkIndex; }
        ASTRA_NODISCARD constexpr size_t GetEntityIndex() const noexcept { return entityIndex; }
        // NOTE: copy any remaining members/operators present in the original block verbatim.
    };
}
```

- [ ] **Step 2: Replace the definition in `Archetype.hpp` with an include**

In `include/Astra/Archetype/Archetype.hpp`, delete the `struct EntityLocation { … };` block and add near the other includes at the top:

```cpp
#include "EntityLocation.hpp"
```

- [ ] **Step 3: Build one config to confirm the move compiles**

Run: `"C:/Program Files/.../MSBuild.exe" Astra.sln -p:Configuration=Release -p:Platform=x64 -m`
Expected: PASS (no errors; `EntityLocation` resolves everywhere via the new header).

- [ ] **Step 4: Commit**

```bash
git add include/Astra/Archetype/EntityLocation.hpp include/Astra/Archetype/Archetype.hpp
git commit -m "refactor(entity): extract EntityLocation into its own lightweight header"
```

---

### Task 2: Create the shared `EntityRecord` header (widened with `version`)

Promotes `EntityRecord` from a nested `ArchetypeManager` type to a top-level `Astra::EntityRecord` that both `EntityManager`'s table and `ArchetypeManager` can name, and adds the `version` field that unifies liveness with location.

**Files:**
- Create: `include/Astra/Entity/EntityRecord.hpp`
- Test: `tests/Entity/EntityTest.cpp` (append static-assert-style checks — existing file, no premake regen)

**Interfaces:**
- Produces: `struct Astra::EntityRecord { Archetype* archetype; EntityLocation location; Entity::VersionType version; }` — trivially copyable; `version == 0` ⇒ dead/empty.

- [ ] **Step 1: Write the header**

Create `include/Astra/Entity/EntityRecord.hpp`:

```cpp
#pragma once

#include <type_traits>

#include "../Archetype/EntityLocation.hpp"
#include "Entity.hpp"

namespace Astra
{
    class Archetype;  // forward declaration — EntityRecord only stores the pointer

    // Unified entity slot: liveness (version) + storage location (archetype + location),
    // co-located so a single paged lookup can both validate a handle and locate it.
    // version == 0 (EntityTable::NULL_VERSION) marks a dead/empty slot; a slot is
    // "located" iff version matches the handle AND archetype != nullptr.
    struct EntityRecord
    {
        Archetype*           archetype = nullptr;  // null ⇒ no location assigned
        EntityLocation       location;             // {chunkIndex, entityIndex}
        Entity::VersionType  version   = 0;        // 0 ⇒ dead/empty (NULL_VERSION)
    };

    static_assert(std::is_trivially_copyable_v<EntityRecord>,
        "EntityRecord must be trivially copyable so the paged table can memcpy/relocate slots");
    static_assert(std::is_trivially_destructible_v<EntityRecord>,
        "EntityRecord must be trivially destructible for cheap segment teardown");
}
```

- [ ] **Step 2: Add a compile-time check in the entity test file**

Append to `tests/Entity/EntityTest.cpp` (near the top-level includes and one new `TEST`):

```cpp
#include <Astra/Entity/EntityRecord.hpp>

TEST(EntityRecord, DefaultIsDeadAndUnlocated)
{
    Astra::EntityRecord r;
    EXPECT_EQ(r.version, 0u);            // dead by default
    EXPECT_EQ(r.archetype, nullptr);    // unlocated by default
}
```

- [ ] **Step 3: Build + run the entity tests (Debug)**

Run: `"…/MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m` then
`bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter='EntityRecord.*:Entity.*'`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/Astra/Entity/EntityRecord.hpp tests/Entity/EntityTest.cpp
git commit -m "feat(entity): add shared EntityRecord {archetype, location, version} header"
```

---

### Task 3: Widen `EntityTable` to store `EntityRecord` + add the location API + fix segment sizing

The core structural change. `EntityTable` keeps ALL its paging/pooling/huge-page/autorelease machinery; only the per-slot payload changes from `VersionType` to `EntityRecord`, the version API now reads/writes `slot.version`, a location API is added, and the huge-page byte math is derived from `sizeof(EntityRecord)` (a 24 B slot no longer fits the old 64 KB segment stride).

**Files:**
- Modify: `include/Astra/Entity/EntityTable.hpp`
- Create: `tests/Entity/EntityRecordTableTest.cpp` (new file → premake regen required)

**Interfaces:**
- Consumes: `Astra::EntityRecord`, `Astra::EntityLocation` (Tasks 1–2).
- Produces (new public methods on `EntityTable`, `IDType = Entity::StorageType`):
  - `EntityRecord* GetRecord(IDType id) noexcept;` — pointer to the slot, or `nullptr` if the segment doesn't exist.
  - `EntityRecord* GetOrCreateRecord(IDType id);` — creates the segment if needed; never null.
  - `void SetRecord(IDType id, Archetype* archetype, EntityLocation location);` — writes `archetype`/`location` only (never `version`); creates the segment if needed.
  - `template<class F> void ForEachRecord(F&& f) const;` — invokes `f(IDType id, const EntityRecord&)` for every slot with `version != NULL_VERSION`.
  - Unchanged version face: `SetVersion / GetVersion / IsAlive / Destroy / AliveCount / begin() / end() / Clear / Reserve / ShrinkToFit`, now backed by `slot.version`.

- [ ] **Step 1: Write the failing table tests**

Create `tests/Entity/EntityRecordTableTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Astra/Entity/EntityTable.hpp>
#include <Astra/Entity/EntityRecord.hpp>

using namespace Astra;
using IDType = EntityTable::IDType;

// EntityTable never dereferences Archetype*, so opaque non-null dummies are fine.
static Archetype* Fake(uintptr_t v) { return reinterpret_cast<Archetype*>(v); }

TEST(EntityRecordTable, VersionFaceUnchanged)
{
    EntityTable t;
    t.SetVersion(5, 1);
    EXPECT_TRUE(t.IsAlive(5, 1));
    EXPECT_FALSE(t.IsAlive(5, 2));
    EXPECT_EQ(t.GetVersion(5), 1u);
    t.Destroy(5);
    EXPECT_FALSE(t.IsAlive(5, 1));
    EXPECT_EQ(t.GetVersion(5), EntityTable::NULL_VERSION);
}

TEST(EntityRecordTable, LocationRoundTripLeavesVersionUntouched)
{
    EntityTable t;
    t.SetVersion(7, 1);                        // make the slot live
    t.SetRecord(7, Fake(0x1234), EntityLocation::Create(2, 3));

    EntityRecord* r = t.GetRecord(7);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->archetype, Fake(0x1234));
    EXPECT_EQ(r->location.chunkIndex, 2u);
    EXPECT_EQ(r->location.entityIndex, 3u);
    EXPECT_EQ(r->version, 1u);                 // SetRecord must NOT touch version
}

TEST(EntityRecordTable, GetRecordAbsentSegmentReturnsNull)
{
    EntityTable t;
    EXPECT_EQ(t.GetRecord(999999), nullptr);
}

TEST(EntityRecordTable, MultiSegmentPagingWithRecordPayload)
{
    EntityTable::Config cfg(1024);             // small segments → force many segments
    EntityTable t(cfg);
    for (IDType id = 0; id < 5000; id += 777)
    {
        t.SetVersion(id, 1);
        t.SetRecord(id, Fake(0x10 + id), EntityLocation::Create(id, id));
    }
    for (IDType id = 0; id < 5000; id += 777)
    {
        EntityRecord* r = t.GetRecord(id);
        ASSERT_NE(r, nullptr) << "id=" << id;
        EXPECT_EQ(r->location.chunkIndex, id);
        EXPECT_EQ(r->archetype, Fake(0x10 + id));
    }
}

TEST(EntityRecordTable, ForEachRecordVisitsOnlyLiveSlots)
{
    EntityTable t;
    t.SetVersion(1, 1); t.SetRecord(1, Fake(0xA), EntityLocation::Create(0, 0));
    t.SetVersion(2, 1); t.SetRecord(2, Fake(0xB), EntityLocation::Create(0, 1));
    t.SetVersion(3, 1);                        // live but never located (archetype == null)

    int located = 0, live = 0;
    t.ForEachRecord([&](IDType, const EntityRecord& rec) {
        ++live;
        if (rec.archetype) ++located;
    });
    EXPECT_EQ(live, 3);
    EXPECT_EQ(located, 2);
}
```

- [ ] **Step 2: Regenerate the solution and confirm the new tests FAIL to compile**

Run: `D:/dev/_shared/tools/premake5 vs2022` then build Debug.
Expected: FAIL — `SetRecord`/`GetRecord`/`ForEachRecord` do not exist yet.

- [ ] **Step 3: Widen the `Segment` payload**

In `include/Astra/Entity/EntityTable.hpp`, add `#include "EntityRecord.hpp"` and change the private `struct Segment` so the payload array is `EntityRecord` instead of `VersionType`:

```cpp
// members:
EntityRecord* records;                       // was: VersionType* versions;
std::unique_ptr<EntityRecord[]> ownedMemory; // was: std::unique_ptr<VersionType[]>

// huge-page ctor body:
Segment(IDType base, IDType cap, EntityRecord* hugePageMemory) :
    baseID(base), capacity(cap), records(hugePageMemory), ownedMemory(nullptr), isFromHugePage(true)
{
    std::uninitialized_fill_n(records, capacity, EntityRecord{});  // raw huge-page memory
}

// owned ctor body:
explicit Segment(IDType base, IDType cap) :
    baseID(base), capacity(cap), records(nullptr),
    ownedMemory(std::make_unique<EntityRecord[]>(cap)),          // default-constructs (version==0)
    isFromHugePage(false)
{
    records = ownedMemory.get();
}

// Reset():
void Reset(IDType newBaseID) noexcept
{
    baseID = newBaseID;
    aliveCount = 0;
    std::fill_n(records, capacity, EntityRecord{});               // objects already live here
}
```

Add `#include <memory>` / `#include <type_traits>` if not already present (for `uninitialized_fill_n`, include `<memory>`).

- [ ] **Step 4: Point the version face at `slot.version`**

Replace every `segment->versions[localIdx]` access with `segment->records[localIdx].version` in `SetVersion`, `GetVersion`, `Destroy`, and the iterator (`operator*` reads `m_currentSegment->records[m_localIdx].version`; `SkipToNextValid` tests `records[...].version != NULL_VERSION`). The `aliveCount`/autorelease bookkeeping is unchanged (it keys on version transitions).

- [ ] **Step 5: Fix segment byte-sizing for the 24 B payload**

Delete the payload-size-dependent constants `SEGMENT_SIZE` and `SEGMENTS_PER_HUGE_PAGE` and replace with size derived from `sizeof(EntityRecord)`. Add two private helpers:

```cpp
size_t SegmentBytes() const noexcept
{
    return static_cast<size_t>(m_config.entitiesPerSegment) * sizeof(EntityRecord);
}
size_t SegmentsPerHugePage() const noexcept
{
    const size_t sb = SegmentBytes();
    return sb ? (HUGE_PAGE_SIZE / sb) : 0;   // 0 ⇒ segment bigger than a huge page ⇒ heap fallback
}
```

In `GetOrCreateSegment`, update the huge-page branch to use them and an `EntityRecord*` offset:

```cpp
else if (m_hugePageMemory && m_nextHugePageSegment < SegmentsPerHugePage())
{
    EntityRecord* segmentMemory = reinterpret_cast<EntityRecord*>(
        m_hugePageMemory + (m_nextHugePageSegment * SegmentBytes()));
    segment = std::make_unique<Segment>(baseId, m_config.entitiesPerSegment, segmentMemory);
    m_nextHugePageSegment++;
}
```

(The heap-allocation fallback branch is unchanged; when `SegmentsPerHugePage()` is 0 or exhausted, allocation falls through to it.)

- [ ] **Step 6: Add the location API + `ForEachRecord`**

Add public methods (place near `GetVersion`):

```cpp
ASTRA_NODISCARD EntityRecord* GetRecord(IDType id) noexcept
{
    Segment* segment = GetSegment(id);
    if (!segment) ASTRA_UNLIKELY return nullptr;
    return &segment->records[segment->ToLocal(id)];
}

ASTRA_NODISCARD EntityRecord* GetOrCreateRecord(IDType id)
{
    Segment* segment = GetOrCreateSegment(id);
    return &segment->records[segment->ToLocal(id)];
}

void SetRecord(IDType id, Archetype* archetype, EntityLocation location)
{
    EntityRecord* r = GetOrCreateRecord(id);   // does NOT change version
    r->archetype = archetype;
    r->location  = location;
}

template<class F>
void ForEachRecord(F&& f) const
{
    for (const auto& segment : m_segments)
    {
        if (!segment) continue;
        for (IDType local = 0; local < segment->capacity; ++local)
        {
            const EntityRecord& rec = segment->records[local];
            if (rec.version != NULL_VERSION)
                f(static_cast<IDType>(segment->baseID + local), rec);
        }
    }
}
```

`GetRecord`/`SetRecord` reference `Archetype*` — `EntityRecord.hpp` already forward-declares `Astra::Archetype`, so no extra include is needed here (the table never dereferences it).

- [ ] **Step 7: Build + run the table tests (Debug)**

Run: build Debug, then `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter='EntityRecordTable.*'`
Expected: PASS (all five tests).

- [ ] **Step 8: Full 3-config build + full test run (regression gate)**

Build Debug, Release, Dist; run `AstraTest.exe` for each. Expected: all green (EntityManager still works via the version face). If `EntityManagerTest`/`EntityManagerSerializationTest` regress, the version-face rewrite in Step 4 missed a `versions[...]` site — fix and re-run.

- [ ] **Step 9: Commit**

```bash
git add include/Astra/Entity/EntityTable.hpp tests/Entity/EntityRecordTableTest.cpp
git commit -m "feat(entity): widen EntityTable to store EntityRecord + location API + size-aware segments"
```

---

### Task 4: Migrate `ArchetypeManager` onto the injected table + wire `Registry` (atomic)

This is one atomic task because the code does not compile between deleting `m_entityMap` and fixing its ~40 use sites — and that is the safety net: **a deleted `m_entityMap` turns "did I catch every site?" into a hard compile error at every one.** The `EntityManager` accessor and the 3 `Registry` constructor injections are included so the tree compiles again at task end.

**Files:**
- Modify: `include/Astra/Entity/EntityManager.hpp` (add the table accessor)
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (drop nested `EntityRecord` + `m_entityMap`; add `EntityTable* m_records`; ctor param; migrate sites)
- Modify: `include/Astra/Registry/Registry.hpp` (inject the table into the 3 `ArchetypeManager` constructions)

**Interfaces:**
- Consumes: `EntityTable::{GetRecord, GetOrCreateRecord, SetRecord, ForEachRecord}` (Task 3); `Astra::EntityRecord` (Task 2).
- Produces: `EntityManager::GetRecordTable() -> EntityTable&`; `ArchetypeManager(componentRegistry, chunkConfig, EntityTable* records)`.

- [ ] **Step 1: Add the `EntityManager` accessor**

In `include/Astra/Entity/EntityManager.hpp` (public section):

```cpp
ASTRA_NODISCARD EntityTable&       GetRecordTable()       noexcept { return m_table; }
ASTRA_NODISCARD const EntityTable& GetRecordTable() const noexcept { return m_table; }
```

- [ ] **Step 2: Swap the `ArchetypeManager` member + constructor + includes**

In `include/Astra/Archetype/ArchetypeManager.hpp`:
- Add includes: `#include "../Entity/EntityRecord.hpp"` and `#include "../Entity/EntityTable.hpp"`.
- **Delete** the nested `struct EntityRecord { Archetype* archetype; EntityLocation location; };` (lines ~38–42) — the type now comes from `EntityRecord.hpp` (unqualified `EntityRecord` inside the class still resolves to `Astra::EntityRecord`).
- **Delete** the member `std::unordered_map<Entity, EntityRecord> m_entityMap;` (line ~1473). Add: `EntityTable* m_records = nullptr;`.
- Add an `EntityTable* records` parameter to the constructor and store it:

```cpp
explicit ArchetypeManager(std::shared_ptr<ComponentRegistry> registry,
                          const ArchetypeChunkPool::Config& poolConfig = {},
                          EntityTable* records = nullptr) :
    m_chunkPool(poolConfig),
    m_componentRegistry(registry),
    m_records(records)
{
    ASTRA_ASSERT(registry, "ComponentRegistry must not be null");
    ASTRA_ASSERT(records,  "EntityRecordTable must not be null");
    // … existing body …
}
```

- [ ] **Step 3: Inject the table from all three `Registry` constructors**

In `include/Astra/Registry/Registry.hpp`, each of the 3 constructors builds `m_archetypeManager` with `std::make_shared<ArchetypeManager>(m_componentRegistry, <chunkConfig>)`. Add the table pointer as the third argument in each (member init order guarantees `m_entityManager` is already constructed):

```cpp
m_archetypeManager(std::make_shared<ArchetypeManager>(
    m_componentRegistry, config.chunkPoolConfig, &m_entityManager.GetRecordTable())),
```

(and the `chunkConfig` variant likewise). No other Registry change — it already validates via `IsValid` before every `ArchetypeManager` call.

- [ ] **Step 4: Migrate every `m_entityMap` site using these exact patterns**

Build Debug; the compiler now lists every remaining `m_entityMap` reference. Apply the matching pattern to each (inventory from `grep`: lines ~84, 103, 160–163, 177–180, 186–198, 210–239, 246–247, 252, 263–264, 324–325, 395–396, 454–455, 498–499, 508–509, 591, 744, 766, 795, 831, 927, 1095, 1188, 1213, 1226–1227, 1299, 1321, 1362). Keep applying until Debug compiles clean — a clean compile proves completeness.

**Pattern A — insert/assign a location** (e.g. `AddEntity`, `AddEntityWith`, `AddEntities…`, `SetEntityLocation`, batch inserts at 84/103/163/180/252/1188/1299):
```cpp
// before: m_entityMap[entity] = EntityRecord{archetype, location};
EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
rec->archetype = archetype;
rec->location  = location;     // NEVER assign rec->version
```

**Pattern B — find + read/use** (get/has/add/remove component lookups at 186, 246, 263, 324, 395, 454, 498, 508, 1226):
```cpp
// before: auto it = m_entityMap.find(entity);
//         if (it == m_entityMap.end()) return X;
//         EntityRecord& loc = it->second;
EntityRecord* rec = m_records->GetRecord(entity.GetID());
if (!rec || rec->version != entity.GetVersion() || !rec->archetype) ASTRA_UNLIKELY
    return X;
EntityRecord& loc = *rec;
```

**Pattern C — update the location of a swap-moved entity** (swap-remove back-patch at 1095, 1213, 1321, 1362, and the `movedEntity` update in `RemoveEntity` ~193):
```cpp
// before: m_entityMap[*movedEntity].location = oldLoc.location;
// The moved (swapped-in) entity is guaranteed live and located:
m_records->GetRecord(movedEntity.GetID())->location = newLocation;
```
(Use `.GetID()` on whatever entity variable the site uses — `*movedEntity`, `movedEntity`, `entityBatch[i].first`, etc.)

**Pattern D — erase / remove an entity's location** (RemoveEntity ~198, RemoveEntities ~239):
```cpp
// before: m_entityMap.erase(it);   // or m_entityMap.erase(entity);
if (EntityRecord* rec = m_records->GetRecord(entity.GetID())) ASTRA_LIKELY
{
    rec->archetype = nullptr;         // clear LOCATION ONLY
    rec->location  = EntityLocation{};
    // DO NOT touch rec->version — EntityManager::Destroy owns it.
}
```

**Pattern E — `GetEntityRecord`** (244):
```cpp
ASTRA_NODISCARD const EntityRecord* GetEntityRecord(Entity entity) const
{
    const EntityRecord* rec = m_records->GetRecord(entity.GetID());
    return (rec && rec->version == entity.GetVersion() && rec->archetype) ? rec : nullptr;
}
```

**Pattern F — `reserve`** (160, 177, 831):
```cpp
// before: m_entityMap.reserve(n);
m_records->Reserve(n);   // no-op-safe; EntityManager may already have reserved
```

**Pattern G — `clear` (whole-world reset)** (591, 795):
```cpp
// before: m_entityMap.clear();
// REMOVE. The shared table's lifecycle is owned by EntityManager::Clear();
// ArchetypeManager::Clear must only reset its archetypes/chunks, not the table.
```

**Pattern H — serialize iteration** (744 size, 766 loop):
```cpp
// before: writer(static_cast<uint32_t>(m_entityMap.size()));
//         for (const auto& [entity, location] : m_entityMap) { … }
// Collect located entities first (size must match what we write):
SmallVector<std::pair<Entity, EntityRecord>, 256> located;
m_records->ForEachRecord([&](EntityTable::IDType id, const EntityRecord& rec) {
    if (rec.archetype)
        located.push_back({ Entity(id, rec.version), rec });
});
writer(static_cast<uint32_t>(located.size()));
for (const auto& [entity, rec] : located) { /* write entity + rec.archetype/rec.location exactly as before */ }
```

**Pattern I — deserialize insert** (927):
```cpp
// before: m_entityMap[entity] = location;   // 'location' here is an EntityRecord
// Segments already exist (EntityManager restored versions first — see Task 5). Write location only:
EntityRecord* rec = m_records->GetOrCreateRecord(entity.GetID());
rec->archetype = location.archetype;
rec->location  = location.location;   // NEVER rec->version
```

- [ ] **Step 5: Build all 3 configs + run the full suite**

Build Debug/Release/Dist; run `AstraTest.exe` each. Expected: all green. Pay attention to `RegistryTest`, `ArchetypeManagerTest`, `EntityValidityTest`, `IterationSafetyTest`, `ViewInvalidationTest`, `RootArchetypeRoundTripTest`. If `RegistrySerializationTest` regresses, that is the deserialize ordering — fix in Task 5 (it is expected to be addressed there); if it still fails after Task 5, revisit.

> Serialization round-trip tests may fail here and are **fixed in Task 5**. All non-serialization tests must pass at this step.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Entity/EntityManager.hpp include/Astra/Archetype/ArchetypeManager.hpp include/Astra/Registry/Registry.hpp
git commit -m "refactor(archetype): replace m_entityMap with the shared paged EntityRecord table"
```

---

### Task 5: Guarantee deserialize ordering + serialize round-trip

The version and location now live in the same slots, so on load `EntityManager` must restore versions **before** `ArchetypeManager` restores locations (`GetOrCreateRecord` writes into slots whose `version` the EntityManager pass established, and the located-count in Pattern H depends on live slots). This task verifies/enforces the ordering at the `Registry` level and locks it with a round-trip test.

**Files:**
- Modify (if needed): `include/Astra/Registry/Registry.hpp` (deserialize orchestration order)
- Test: `tests/Registry/RegistrySerializationTest.cpp` (append a round-trip test — existing file, no premake regen)

**Interfaces:**
- Consumes: existing `Registry` Serialize/Deserialize, `EntityManager`/`ArchetypeManager` (de)serialize.

- [ ] **Step 1: Write the failing round-trip test**

Append to `tests/Registry/RegistrySerializationTest.cpp` (reuse `tests/TestComponents.hpp` types — do NOT introduce new component types):

```cpp
TEST(RegistrySerialization, UnifiedRecordRoundTripPreservesLivenessAndLocation)
{
    using namespace Astra;
    Registry reg;
    std::vector<Entity> ents;
    for (int i = 0; i < 100; ++i)
        ents.push_back(reg.CreateEntityWith(Test::Position{float(i), 0, 0}, Test::Velocity{1, 1, 1}));

    // Destroy a few so recycled versions and holes are exercised.
    for (int i = 0; i < 100; i += 10) reg.DestroyEntity(ents[i]);

    std::vector<std::byte> buffer;
    BinaryWriter writer(buffer);
    reg.Serialize(writer);

    Registry loaded;
    BinaryReader reader(buffer);
    ASSERT_TRUE(loaded.Deserialize(reader));   // adjust to the actual Serialize/Deserialize API

    for (int i = 0; i < 100; ++i)
    {
        const bool destroyed = (i % 10 == 0);
        EXPECT_EQ(loaded.IsValid(ents[i]), !destroyed) << "i=" << i;
        if (!destroyed)
        {
            const Test::Position* p = loaded.GetComponent<Test::Position>(ents[i]);
            ASSERT_NE(p, nullptr) << "i=" << i;
            EXPECT_FLOAT_EQ(p->x, float(i));   // location resolved correctly after load
        }
    }
}
```

Adjust the `Serialize`/`Deserialize`/`BinaryWriter`/`BinaryReader` calls to match the real signatures used by the existing tests in this file.

- [ ] **Step 2: Run it — expect FAIL or PASS**

Run: build Debug, `AstraTest.exe --gtest_filter='RegistrySerialization.UnifiedRecordRoundTrip*'`.
- If it **fails** (locations null / IsValid wrong after load) → the deserialize order is wrong; go to Step 3.
- If it **passes** → the existing `Registry::Deserialize` already loads `EntityManager` before `ArchetypeManager`; confirm by reading the method and add a one-line comment documenting the ordering invariant, then skip to Step 4.

- [ ] **Step 3: Enforce EntityManager-before-ArchetypeManager on load**

In `Registry::Deserialize` (read the method), ensure `m_entityManager.Deserialize(...)` runs **before** `m_archetypeManager->Deserialize(...)`. Add:

```cpp
// INVARIANT: EntityManager restores versions into the shared record table BEFORE
// ArchetypeManager writes locations into the same slots (Task 5 / W1 unified record).
```

Confirm `ArchetypeManager::Deserialize` does NOT clear/reserve the shared table (Pattern G already removed those); it only writes locations via `GetOrCreateRecord` (Pattern I).

- [ ] **Step 4: Full 3-config build + full suite**

Build Debug/Release/Dist; run `AstraTest.exe` each. Expected: all green, including all serialization tests (`RegistrySerializationTest`, `EntityManagerSerializationTest`, `BinarySerialization*`, `FormatV2Test`, `LoadRobustnessTest`, `RootArchetypeRoundTripTest`).

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Registry/Registry.hpp tests/Registry/RegistrySerializationTest.cpp
git commit -m "fix(serialization): order EntityManager-before-ArchetypeManager load + round-trip test"
```

---

### Task 6: Benchmark, update RESULTS.md, refresh memory

Confirm the perf win against the head-to-head and record it.

**Files:**
- Modify: `bench-compare/RESULTS.md`
- Modify: `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` (mark W1 done)
- Modify: memory `astra-perf-optimization.md` (W1 landed; next = W6/W2)

- [ ] **Step 1: Rebuild the Astra benchmark and run all three exes**

Rebuild `bench-compare/bench_astra.cpp` (per `bench-compare/`'s `build_one.bat`; needs `/I..\include /I..\vendor\Mosaic\include` + `advapi32.lib`) and run `bench_astra.exe`, `bench_entt.exe`, `bench_flecs.exe`. Capture the CSV (`lib,op,N,ns_per_op,Mops`).

- [ ] **Step 2: Record results + sanity-check the win**

Update `bench-compare/RESULTS.md` with the new Astra row. Expected direction: **random_get 147 → ~80 ns** (hash gone; validate+locate share a cache line), and create/add/remove improved by shedding the per-op hash + per-create heap allocation. **No iteration regression** (iterate1/2/3 unchanged — the record table is off the iteration hot path). If random_get did not move, verify `GetComponent` actually routes through `m_records->GetRecord` (not a leftover map path) and that `IsValid` + locate hit the same table.

- [ ] **Step 3: Mark W1 complete in the perf plan + memory**

In `docs/reviews/2026-07-21-astra-perf-optimization-plan.md`, note W1 as landed with the measured numbers. Update the `astra-perf-optimization.md` memory: W1 done (unified paged record), next = W6 (SplitHash mix) → W2 (edge arrays).

- [ ] **Step 4: Commit**

```bash
git add bench-compare/RESULTS.md docs/reviews/2026-07-21-astra-perf-optimization-plan.md
git commit -m "perf(bench): record W1 unified-record results (random_get + structural churn)"
```

---

## Finish (SDD close-out)

Per the project's SDD model: after all tasks pass in **Debug + Release + Dist** with the intended new tests green, fast-forward-merge `perf/w1-unified-entity-record` into `dev` locally, delete the branch, and **do not push**. Confirm with the user before merging.

---

## Self-review — spec coverage

- Unified `EntityRecord {archetype, location, version}` → Task 2. ✅
- Widen EntityTable + version face + location face + `ForEachRecord` + segment sizing → Task 3. ✅
- Mechanic P ownership (EntityManager owns, ArchetypeManager raw ptr, Registry injects) → Task 4 Steps 1–3. ✅
- ~40-site migration, compile-driven completeness, find-guard (`version && archetype`), never-write-version, clear-location-only → Task 4 Step 4 (Patterns A–I). ✅
- Stale-handle semantics preserved → Pattern B/E find-guard. ✅
- Destroy-ordering invariant (RemoveEntity clears location only; Destroy owns version) → Global Constraints + Pattern D. ✅
- Serialization equivalence + deserialize ordering → Task 5. ✅
- Segment 24 B huge-page math fix → Task 3 Step 5. ✅
- Thread-safety unchanged (no new concurrency) → no code change needed; single-threaded structural assumption inherited. ✅
- Tests: table unit tests (Task 3), record default (Task 2), serialize round-trip (Task 5) → ✅
- 3-config verification gates → Tasks 3/4/5 Steps. ✅
- Benchmark + RESULTS + memory → Task 6. ✅
- EntityLocation extraction (enabler for include hygiene) → Task 1. ✅
