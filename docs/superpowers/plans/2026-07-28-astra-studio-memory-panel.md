# AstraStudio Memory Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new AstraStudio **Memory** panel visualizing a chunk's physical layout (cache-line-aligned SoA columns, padding, disabled-bit words, slack) with per-cache-line occupancy, an entity probe, and full zoom/pan — backed by an Inspector-facade extension whose per-frame cost is independent of entity count.

**Architecture:** Two tiny read-only offset accessors on `ArchetypeChunk` feed an additive `Astra::Debug` facade extension (per-chunk POD layout + accounting in `ChunkInfo`; per-entity data only via on-demand `CaptureChunkDetail` for the selected chunk; capture-into overload kills alloc churn). A new header-only `studio/MemoryPanel.hpp` renders the user-locked composite: chunk strip + always-on overview bar + Bytes/Entities tabs + legend/probe footer, all via ImDrawList.

**Tech Stack:** C++20 header-only, MSVC (MSBuild x64), GoogleTest, Dear ImGui (docking) + GLFW + OpenGL3 studio app, premake-generated checked-in `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-28-astra-studio-memory-panel-design.md`

## Global Constraints

- Branch: `feature/studio-memory-panel` off dev @ `c6925d4`; finish = local FF merge to dev, delete branch, do NOT push.
- Build: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Config> -p:Platform=x64 -m` (PowerShell; build the whole solution — `-t:AstraTest` does not work).
- Tests: `bin/<Config>-windows-x86_64/AstraTest/AstraTest.exe`; studio: `bin/<Config>-windows-x86_64/AstraStudio/AstraStudio.exe`.
- Tests reuse `Astra::Test::*` components ONLY (128-TypeID ceiling; never define new component types in tests). Enableable test component: `Astra::Test::Timer` (`tests/TestComponents.hpp:180`).
- Engine changes limited to the two `ArchetypeChunk` accessors. No new includes in `Astra.hpp`. `Debug/Inspector.hpp` stays opt-in (not in the umbrella).
- Facade hot-loop rule: `Capture` stays O(archetypes × columns + chunks) POD work — no per-entity copies outside `CaptureChunkDetail`.
- `CompressionTest.PerformanceBenchmark` is a known flake — a lone failure of only that test is not a regression; rerun it isolated.
- Panel palette (validated dark 8-slot categorical, fixed order): `#3987e5 #d95926 #199e70 #c98500 #d55181 #008300 #9085e9 #e66767`; pad `#34342f`, bits gray `#898781` (dimmed), slack = hatch on `#232322`, dead = 20% hue toward `#232322`, probe = white.
- Commit messages end with the standing Co-Authored-By/Claude-Session trailer used on this branch's parent commits.

---

### Task 1: Branch + ArchetypeChunk offset accessors

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (public accessor block near `GetChunkBytes()`, ~line 529)
- Test: `tests/Debug/InspectorTest.cpp`

**Interfaces:**
- Consumes: existing `ArchetypeChunk` members `m_columns`, `m_memory`, `m_meta`; public `GetChunkBytes()`, `GetCapacity()`.
- Produces (later tasks rely on these exact signatures):
  - `ASTRA_NODISCARD size_t ArchetypeChunk::GetColumnOffset(uint16_t column) const` — byte offset of the column base within the chunk arena; column 0 is offset 0.
  - `ASTRA_NODISCARD size_t ArchetypeChunk::GetDisabledWordsOffset(uint16_t column) const` — byte offset of the column's disabled-bit words, or `std::numeric_limits<size_t>::max()` (== `SIZE_MAX`) when the column is not enableable.

- [ ] **Step 1: Create the branch**

```powershell
git checkout -b feature/studio-memory-panel
```

- [ ] **Step 2: Write the failing test**

Append to `tests/Debug/InspectorTest.cpp` (inside no namespace; uses the file's existing `using` declarations):

```cpp
TEST(Inspector, ChunkColumnOffsetsAlignedAscendingWithinArena)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();

    Astra::ArchetypeManager* mgr = reg.GetArchetypeManager();
    ASSERT_NE(mgr, nullptr);
    bool sawChunk = false;
    for (Astra::Archetype* a : mgr->GetArchetypes())
    {
        if (!a || a->GetEntityCount() == 0) continue;
        const Astra::ArchetypeColumnMeta& meta = a->GetColumnMeta();
        for (const auto& chunk : a->GetChunks())
        {
            sawChunk = true;
            size_t prevEnd = 0;
            for (uint16_t c = 0; c < meta.columnCount; ++c)
            {
                const size_t off = chunk->GetColumnOffset(c);
                EXPECT_EQ(off % Astra::CACHE_LINE_SIZE, 0u);   // cache-line aligned base
                EXPECT_GE(off, prevEnd);                        // ascending, non-overlapping
                if (c == 0) EXPECT_EQ(off, 0u);                 // first column at arena start
                prevEnd = off + size_t(meta.columns[c].stride) * chunk->GetCapacity();
                EXPECT_LE(prevEnd, chunk->GetChunkBytes());
                // Position/Velocity are not enableable: sentinel expected.
                EXPECT_EQ(chunk->GetDisabledWordsOffset(c), SIZE_MAX);
            }
        }
    }
    EXPECT_TRUE(sawChunk);
}
```

- [ ] **Step 3: Build to verify it fails (header-only: RED = compile error)**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```

Expected: FAIL with `error C2039: 'GetColumnOffset': is not a member of 'Astra::ArchetypeChunk'`.

- [ ] **Step 4: Add the accessors**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, in the public section directly below `GetChunkBytes()` (line ~529):

```cpp
        // Debug/inspection accessors (Inspector facade): byte offsets within the
        // chunk arena. Kept here so tools never re-derive InitializeColumns' layout.
        ASTRA_NODISCARD size_t GetColumnOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            return static_cast<size_t>(static_cast<const std::byte*>(m_columns[column].base) -
                                       static_cast<const std::byte*>(m_memory));
        }

        // Offset of the column's disabled-bit words, or size_t max if not enableable.
        ASTRA_NODISCARD size_t GetDisabledWordsOffset(uint16_t column) const
        {
            ASTRA_ASSERT(column < m_meta->columnCount, "Column ordinal out of bounds");
            const uint64_t* words = m_columns[column].disabledWords;
            if (!words) return std::numeric_limits<size_t>::max();
            return static_cast<size_t>(reinterpret_cast<const std::byte*>(words) -
                                       static_cast<const std::byte*>(m_memory));
        }
```

(`<limits>` is already included by this header.)

- [ ] **Step 5: Build + run to verify pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Inspector.*
```

Expected: build succeeds; all `Inspector.*` tests PASS (3 tests).

- [ ] **Step 6: Commit**

```powershell
git add include/Astra/Archetype/ArchetypeChunkPool.hpp tests/Debug/InspectorTest.cpp
git commit -m "feat(debug): ArchetypeChunk column/disabled-words offset accessors"
```

---

### Task 2: Facade layout + accounting + capture-into

**Files:**
- Modify: `include/Astra/Debug/Inspector.hpp` (full rewrite of structs + `Capture`)
- Test: `tests/Debug/InspectorTest.cpp`

**Interfaces:**
- Consumes: Task 1 accessors; existing `ArchetypeChunk::WordsForCapacity(size_t)` (public static), `GetChunkBytes()`, `GetDisabledCount(int)`.
- Produces (exact, later tasks + panel rely on these):
  - `struct Astra::Debug::ChunkColumnLayout { size_t offset, bytes, disabledOffset /*SIZE_MAX sentinel*/, disabledBytes; uint32_t disabledCount; }`
  - `Astra::Debug::ChunkInfo` gains: `size_t chunkBytes; std::vector<ChunkColumnLayout> columns; size_t columnBytes, padBytes, bitsBytes, slackBytes;` (invariant: the four sum to `chunkBytes`)
  - `Astra::Debug::ArchetypeInfo::bytesReserved`, `Astra::Debug::RegistrySnapshot::bytesReserved`, `Astra::Debug::InspectorSnapshot::cacheLineBytes`
  - `void Astra::Debug::Capture(Registry&, InspectorSnapshot& out)` (capture-into; clears + refills, retains outer vector capacity)
  - `ASTRA_NODISCARD InspectorSnapshot Astra::Debug::Capture(Registry&)` (wrapper, unchanged semantics)

- [ ] **Step 1: Write the failing tests**

Append to `tests/Debug/InspectorTest.cpp`:

```cpp
TEST(Inspector, ChunkAccountingSumsToChunkBytes)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();

    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.cacheLineBytes, Astra::CACHE_LINE_SIZE);

    const auto* pv = FindBySignature(snap, "Velocity");
    ASSERT_NE(pv, nullptr);
    EXPECT_GT(pv->bytesReserved, 0u);
    EXPECT_GE(pv->bytesReserved, pv->bytesAllocated);   // arena >= layout-derived

    size_t reserved = 0;
    for (const auto& a : snap.archetypes) reserved += a.bytesReserved;
    EXPECT_EQ(snap.registry.bytesReserved, reserved);

    for (const auto& ch : pv->chunks)
    {
        ASSERT_EQ(ch.columns.size(), pv->columns.size());   // parallel arrays
        EXPECT_GT(ch.chunkBytes, 0u);
        size_t prevEnd = 0, cols = 0;
        for (size_t c = 0; c < ch.columns.size(); ++c)
        {
            const auto& cl = ch.columns[c];
            EXPECT_EQ(cl.offset % snap.cacheLineBytes, 0u);
            EXPECT_GE(cl.offset, prevEnd);
            EXPECT_EQ(cl.bytes, size_t(pv->columns[c].stride) * ch.capacity);
            EXPECT_EQ(cl.disabledOffset, SIZE_MAX);          // not enableable
            EXPECT_EQ(cl.disabledBytes, 0u);
            EXPECT_EQ(cl.disabledCount, 0u);
            prevEnd = cl.offset + cl.bytes;
            cols += cl.bytes;
        }
        EXPECT_EQ(ch.columnBytes, cols);
        EXPECT_EQ(ch.bitsBytes, 0u);
        EXPECT_EQ(ch.columnBytes + ch.padBytes + ch.bitsBytes + ch.slackBytes, ch.chunkBytes);
    }
}

TEST(Inspector, CaptureIntoMatchesByValueAndRefills)
{
    Astra::Registry reg;
    for (int i = 0; i < 50; ++i) (void)reg.CreateEntity<Position, Velocity>();

    Astra::Debug::InspectorSnapshot byValue = Astra::Debug::Capture(reg);
    Astra::Debug::InspectorSnapshot into;
    Astra::Debug::Capture(reg, into);

    EXPECT_EQ(into.registry.entityCount, byValue.registry.entityCount);
    EXPECT_EQ(into.registry.bytesReserved, byValue.registry.bytesReserved);
    ASSERT_EQ(into.archetypes.size(), byValue.archetypes.size());
    for (size_t i = 0; i < into.archetypes.size(); ++i)
    {
        EXPECT_EQ(into.archetypes[i].signature, byValue.archetypes[i].signature);
        EXPECT_EQ(into.archetypes[i].entityCount, byValue.archetypes[i].entityCount);
        EXPECT_EQ(into.archetypes[i].chunks.size(), byValue.archetypes[i].chunks.size());
    }

    // Refill the same object after mutation: results track the registry, no stale rows.
    for (int i = 0; i < 50; ++i) (void)reg.CreateEntity<Position>();
    Astra::Debug::Capture(reg, into);
    EXPECT_EQ(into.registry.entityCount, 100u);
}
```

- [ ] **Step 2: Build to verify RED**

Same MSBuild command as Task 1 Step 3. Expected: FAIL — `'cacheLineBytes': is not a member`.

- [ ] **Step 3: Rewrite `include/Astra/Debug/Inspector.hpp`**

Replace the file's structs and `Capture` with (keep the header comment block lines 1–7 as-is; note the new `<cstdint>` include for `SIZE_MAX`):

```cpp
#include <cstdint>
#include <string>
#include <vector>

#include "../Registry/Registry.hpp"

namespace Astra::Debug
{
    struct ColumnInfo
    {
        std::string name;
        ComponentID id{};
        size_t size = 0;
        size_t alignment = 0;
        uint32_t stride = 0;
        bool isEnableable = false;
    };

    // Physical placement of one storage column inside one chunk's arena.
    struct ChunkColumnLayout
    {
        size_t offset = 0;                    // cache-line-aligned base offset
        size_t bytes = 0;                     // stride * capacity
        size_t disabledOffset = SIZE_MAX;     // disabled-bit words region, or SIZE_MAX
        size_t disabledBytes = 0;
        uint32_t disabledCount = 0;
    };

    struct ChunkInfo
    {
        size_t count = 0;
        size_t capacity = 0;
        size_t chunkBytes = 0;                       // arena size
        std::vector<ChunkColumnLayout> columns;      // parallel to ArchetypeInfo::columns
        // Accounting; invariant: the four sum exactly to chunkBytes.
        size_t columnBytes = 0, padBytes = 0, bitsBytes = 0, slackBytes = 0;
    };

    struct ArchetypeInfo
    {
        std::string signature;
        size_t componentCount = 0;
        size_t tagCount = 0;
        std::vector<ColumnInfo> columns;
        std::vector<ChunkInfo> chunks;
        size_t entityCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;                // layout-derived (unchanged semantics)
        size_t bytesAllocated = 0;           // layout-derived (unchanged semantics)
        size_t bytesReserved = 0;            // true arena footprint: sum of chunkBytes
    };

    struct RegistrySnapshot
    {
        size_t entityCount = 0;
        size_t archetypeCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;
        size_t bytesAllocated = 0;
        size_t bytesReserved = 0;
    };

    struct InspectorSnapshot
    {
        RegistrySnapshot registry;
        std::vector<ArchetypeInfo> archetypes;
        size_t cacheLineBytes = 0;           // so panels never include engine internals
    };

    // Capture-into: clears and refills `out`, retaining outer vector capacity.
    // Runs every studio frame -- O(archetypes x columns + chunks) POD work only;
    // per-entity copies live in CaptureChunkDetail (selected chunk only).
    inline void Capture(Registry& registry, InspectorSnapshot& out)
    {
        out.registry = RegistrySnapshot{};
        out.archetypes.clear();
        out.cacheLineBytes = CACHE_LINE_SIZE;

        ArchetypeManager* manager = registry.GetArchetypeManager();
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!manager || !components)
            return;

        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;

            ArchetypeInfo info;
            const ComponentMask& mask = archetype->GetMask();
            info.componentCount = mask.Count();

            for (size_t bit = 0; bit < MAX_COMPONENTS; ++bit)
            {
                if (!mask.Test(bit))
                    continue;
                const ComponentDescriptor* desc =
                    components->GetComponentDescriptor(static_cast<ComponentID>(bit));
                const char* name = (desc && desc->name) ? desc->name : "?";
                if (!info.signature.empty())
                    info.signature += " + ";
                info.signature += name;
                if (desc && desc->is_empty)
                    ++info.tagCount;
            }
            if (info.signature.empty())
                info.signature = "(empty)";

            const ArchetypeColumnMeta& meta = archetype->GetColumnMeta();
            size_t rowStride = 0;
            info.columns.reserve(meta.columnCount);
            for (uint16_t c = 0; c < meta.columnCount; ++c)
            {
                const auto& col = meta.columns[c];
                ColumnInfo ci;
                ci.id = col.id;
                ci.stride = col.stride;
                if (col.descriptor)
                {
                    ci.name = col.descriptor->name ? col.descriptor->name : "?";
                    ci.size = col.descriptor->size;
                    ci.alignment = col.descriptor->alignment;
                    ci.isEnableable = col.descriptor->isEnableable;
                }
                rowStride += col.stride;
                info.columns.push_back(std::move(ci));
            }

            info.entityCount = archetype->GetEntityCount();
            info.chunkCount = archetype->GetChunkCount();
            info.chunks.reserve(info.chunkCount);
            for (const auto& chunk : archetype->GetChunks())
            {
                ChunkInfo ch;
                ch.count = chunk->GetCount();
                ch.capacity = chunk->GetCapacity();
                ch.chunkBytes = chunk->GetChunkBytes();

                // Columns first (ascending offsets), then their disabled-bit
                // regions (carved after all columns, in enableable-ordinal order,
                // which is also ascending) -- cursor walk yields pad as the gaps.
                size_t cursor = 0;
                ch.columns.reserve(meta.columnCount);
                for (uint16_t c = 0; c < meta.columnCount; ++c)
                {
                    ChunkColumnLayout cl;
                    cl.offset = chunk->GetColumnOffset(c);
                    cl.bytes = size_t(meta.columns[c].stride) * ch.capacity;
                    cl.disabledOffset = chunk->GetDisabledWordsOffset(c);
                    if (cl.disabledOffset != SIZE_MAX)
                    {
                        cl.disabledBytes = ArchetypeChunk::WordsForCapacity(ch.capacity) * 8;
                        cl.disabledCount = chunk->GetDisabledCount(int(c));
                    }
                    ch.columnBytes += cl.bytes;
                    ch.padBytes += cl.offset - cursor;
                    cursor = cl.offset + cl.bytes;
                    ch.columns.push_back(cl);
                }
                for (const ChunkColumnLayout& cl : ch.columns)
                {
                    if (cl.disabledOffset == SIZE_MAX)
                        continue;
                    ch.padBytes += cl.disabledOffset - cursor;
                    ch.bitsBytes += cl.disabledBytes;
                    cursor = cl.disabledOffset + cl.disabledBytes;
                }
                ch.slackBytes = ch.chunkBytes - cursor;

                info.bytesUsed += ch.count * rowStride;
                info.bytesAllocated += ch.capacity * rowStride;
                info.bytesReserved += ch.chunkBytes;
                info.chunks.push_back(std::move(ch));
            }

            out.registry.entityCount += info.entityCount;
            out.registry.chunkCount += info.chunkCount;
            out.registry.bytesUsed += info.bytesUsed;
            out.registry.bytesAllocated += info.bytesAllocated;
            out.registry.bytesReserved += info.bytesReserved;
            out.archetypes.push_back(std::move(info));
        }
        out.registry.archetypeCount = out.archetypes.size();
    }

    ASTRA_NODISCARD inline InspectorSnapshot Capture(Registry& registry)
    {
        InspectorSnapshot snap;
        Capture(registry, snap);
        return snap;
    }
} // namespace Astra::Debug
```

- [ ] **Step 4: Build + run all Inspector tests**

Same commands as Task 1 Step 5. Expected: PASS (5 tests). The pre-existing `SnapshotReportsArchetypesEntitiesAndColumns` must still pass untouched (additive fields only).

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Debug/Inspector.hpp tests/Debug/InspectorTest.cpp
git commit -m "feat(debug): chunk layout+accounting in snapshot, bytesReserved, capture-into"
```

---

### Task 3: CaptureChunkDetail (selected-chunk entities + disabled bits)

**Files:**
- Modify: `include/Astra/Debug/Inspector.hpp` (append below the `Capture` wrapper, inside the namespace)
- Test: `tests/Debug/InspectorTest.cpp`

**Interfaces:**
- Consumes: Task 2 structs; `ArchetypeChunk::GetEntities() const`, `GetDisabledWords(int)`, `WordsForCapacity`; `Archetype::GetColumnMeta()/GetChunks()`.
- Produces:
  - `struct Astra::Debug::ChunkDetail { std::vector<Entity> entities; std::vector<std::vector<uint64_t>> disabledWords; }` — `disabledWords` indexed by **column ordinal** (empty vector for non-enableable columns).
  - `bool Astra::Debug::CaptureChunkDetail(Registry&, size_t archetypeIndex, size_t chunkIndex, ChunkDetail& out)` — `archetypeIndex` is the **snapshot index** (null archetypes skipped exactly as `Capture` does); returns false on stale/out-of-range indices with `out` cleared.

- [ ] **Step 1: Write the failing tests**

Append to `tests/Debug/InspectorTest.cpp`. Add `using Astra::Test::Timer;` next to the existing using-declarations at the top of the file (`Timer` is the suite's enableable component).

```cpp
TEST(Inspector, ChunkDetailEntitiesAndDisabledBits)
{
    Astra::Registry reg;
    std::vector<Astra::Entity> es;
    for (int i = 0; i < 100; ++i) es.push_back(reg.CreateEntity<Position, Timer>());
    for (int i = 0; i < 100; i += 10) ASSERT_TRUE(reg.SetEnabled<Timer>(es[size_t(i)], false));

    auto snap = Astra::Debug::Capture(reg);
    const auto* at = FindBySignature(snap, "Timer");
    ASSERT_NE(at, nullptr);
    const size_t archIdx = size_t(at - snap.archetypes.data());

    // Locate the Timer column ordinal by name.
    size_t timerCol = SIZE_MAX;
    for (size_t c = 0; c < at->columns.size(); ++c)
        if (at->columns[c].name.find("Timer") != std::string::npos) timerCol = c;
    ASSERT_NE(timerCol, SIZE_MAX);
    EXPECT_TRUE(at->columns[timerCol].isEnableable);

    size_t disabledTotal = 0, rowsSeen = 0;
    Astra::Debug::ChunkDetail det;
    for (size_t ci = 0; ci < at->chunks.size(); ++ci)
    {
        const auto& ch = at->chunks[ci];
        EXPECT_NE(ch.columns[timerCol].disabledOffset, SIZE_MAX);
        EXPECT_GT(ch.columns[timerCol].disabledBytes, 0u);

        ASSERT_TRUE(Astra::Debug::CaptureChunkDetail(reg, archIdx, ci, det));
        ASSERT_EQ(det.entities.size(), ch.count);
        ASSERT_EQ(det.disabledWords.size(), ch.columns.size());
        const auto& words = det.disabledWords[timerCol];
        ASSERT_FALSE(words.empty());

        size_t popcount = 0;
        for (size_t r = 0; r < ch.count; ++r)
        {
            const bool bit = (words[r / 64] >> (r % 64)) & 1u;
            if (bit) ++popcount;
            // Bit state must agree with the Registry's own view of that entity.
            EXPECT_EQ(reg.IsEnabled<Timer>(det.entities[r]), !bit);
            ++rowsSeen;
        }
        EXPECT_EQ(popcount, size_t(ch.columns[timerCol].disabledCount));
        disabledTotal += popcount;
    }
    EXPECT_EQ(rowsSeen, 100u);
    EXPECT_EQ(disabledTotal, 10u);
}

TEST(Inspector, ChunkDetailRejectsStaleIndices)
{
    Astra::Registry reg;
    for (int i = 0; i < 10; ++i) (void)reg.CreateEntity<Position>();
    auto snap = Astra::Debug::Capture(reg);

    Astra::Debug::ChunkDetail det;
    EXPECT_FALSE(Astra::Debug::CaptureChunkDetail(reg, snap.archetypes.size() + 5, 0, det));
    EXPECT_TRUE(det.entities.empty());

    const auto* p = FindBySignature(snap, "Position");
    ASSERT_NE(p, nullptr);
    const size_t archIdx = size_t(p - snap.archetypes.data());
    EXPECT_FALSE(Astra::Debug::CaptureChunkDetail(reg, archIdx, 999, det));
    EXPECT_TRUE(Astra::Debug::CaptureChunkDetail(reg, archIdx, 0, det));
    EXPECT_EQ(det.entities.size(), 10u);
}
```

- [ ] **Step 2: Build to verify RED**

Expected: FAIL — `'ChunkDetail': is not a member of 'Astra::Debug'`.

- [ ] **Step 3: Implement**

Append inside `namespace Astra::Debug`, after the by-value `Capture` wrapper:

```cpp
    // Per-entity data for ONE chunk -- the panel's selected chunk only, so the
    // per-frame cost is O(one chunk's capacity), never O(total entities).
    struct ChunkDetail
    {
        std::vector<Entity> entities;                       // row -> Entity
        std::vector<std::vector<uint64_t>> disabledWords;   // by column ordinal; empty = not enableable
    };

    // Same-thread, same-frame as Capture, so snapshot indices stay consistent.
    // archetypeIndex counts non-null archetypes exactly as Capture does.
    inline bool CaptureChunkDetail(Registry& registry, size_t archetypeIndex,
                                   size_t chunkIndex, ChunkDetail& out)
    {
        out.entities.clear();
        out.disabledWords.clear();

        ArchetypeManager* manager = registry.GetArchetypeManager();
        if (!manager)
            return false;

        Archetype* target = nullptr;
        size_t index = 0;
        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;
            if (index++ == archetypeIndex) { target = archetype; break; }
        }
        if (!target)
            return false;

        const auto& chunks = target->GetChunks();
        if (chunkIndex >= chunks.size())
            return false;
        const auto& chunk = chunks[chunkIndex];

        out.entities = chunk->GetEntities();

        const ArchetypeColumnMeta& meta = target->GetColumnMeta();
        out.disabledWords.resize(meta.columnCount);
        const size_t words = ArchetypeChunk::WordsForCapacity(chunk->GetCapacity());
        for (uint16_t e = 0; e < meta.enableableColumnCount; ++e)
        {
            const uint16_t c = meta.enableableColumns[e];
            const uint64_t* w = chunk->GetDisabledWords(int(c));
            if (w) out.disabledWords[c].assign(w, w + words);
        }
        return true;
    }
```

- [ ] **Step 4: Build + run**

Same commands. Expected: PASS (7 `Inspector.*` tests).

- [ ] **Step 5: Commit**

```powershell
git add include/Astra/Debug/Inspector.hpp tests/Debug/InspectorTest.cpp
git commit -m "feat(debug): CaptureChunkDetail -- selected-chunk entities + disabled bits"
```

---

### Task 4: MemoryPanel scaffold + wiring + demo enableable data

**Files:**
- Create: `studio/MemoryPanel.hpp`
- Modify: `studio/StudioApp.hpp` (include + member + Draw call), `studio/Components.hpp` (Health enableable), `studio/WorkloadRunner.hpp` (disable seeding)

**Interfaces:**
- Consumes: `Astra::Debug::InspectorSnapshot/ChunkInfo/ChunkDetail/CaptureChunkDetail` (Tasks 2–3).
- Produces:
  - `class Studio::MemoryPanel` with `void Draw(const Astra::Debug::InspectorSnapshot& snap, int selectedArchetype, Astra::Registry& registry)`.
  - Private members later tasks use verbatim: `m_mode` (0 Bytes / 1 Entities), `m_chunk`, `m_zoom` (px/cell), `m_entZoom`, `m_entPan`, `m_probe` (row, −1 none), `m_detail`, `m_hasDetail`, `m_regions`, `m_visibleByteBegin/End`, `m_scrollRequest`, `m_overviewDragByte`; helpers `Series`, `Mix`, `FormatBytes`, `BuildRegions`; constants `kSeries[8]`, `kCellBase`, `kPadCol`, `kBitsCol`, `kHatchCol`, `kProbeCol`, `kLinesPerRow = 32`, `kFooterH = 58.0f`; stubs `DrawOverviewBar/DrawBytes/DrawEntities` filled by Tasks 5–7.
  - `struct Region { size_t begin, end; int column; int kind; }` — kind: 0 column, 1 bits, 2 pad, 3 slack; `m_regions` sorted by `begin`, covering `[0, chunkBytes)`.

- [ ] **Step 1: Make `Studio::Health` enableable and seed disabled entities**

`studio/Components.hpp` — replace the `Health` line:

```cpp
    struct Health
    {
        static constexpr bool AstraEnableable = true;   // demo data for the Memory panel's disabled-bit layers
        int current = 100, max = 100;
    };
```

`studio/WorkloadRunner.hpp` — in `Spawn`, replace the `Fighters` case body:

```cpp
                case Preset::Fighters:
                    e = m_registry.CreateEntityWith(
                        Position{frand(), frand(), 0}, Velocity{-1, 2, 0}, Health{});
                    // ~8% disabled so the Memory panel's bit layers show real data.
                    if (e.IsValid() && (i % 13) == 0)
                        (void)m_registry.SetEnabled<Health>(e, false);
                    break;
```

- [ ] **Step 2: Create `studio/MemoryPanel.hpp`**

```cpp
#pragma once

// Memory panel (spec 2026-07-28): chunk memory-layout / cache-line viz.
// Composite: chunk strip + always-on overview bar + Bytes/Entities tabs +
// legend/probe footer. Renders snapshot PODs only; the sole live-registry
// touch is CaptureChunkDetail for the selected chunk (O(one chunk)/frame).

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>

namespace Studio
{
    class MemoryPanel
    {
    public:
        void Draw(const Astra::Debug::InspectorSnapshot& snap, int selectedArchetype,
                  Astra::Registry& registry)
        {
            ImGui::Begin("Memory");
            if (selectedArchetype < 0 || selectedArchetype >= int(snap.archetypes.size()))
            {
                ImGui::TextDisabled("Select an archetype in the Archetypes panel.");
                ImGui::End();
                return;
            }
            const auto& a = snap.archetypes[size_t(selectedArchetype)];
            ImGui::TextUnformatted(a.signature.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(follows Archetypes selection)");
            if (a.columns.empty())
            {
                ImGui::TextDisabled("Tag-only archetype: no storage columns to map.");
                ImGui::End();
                return;
            }
            if (a.chunks.empty())
            {
                ImGui::TextDisabled("No chunks allocated yet.");
                ImGui::End();
                return;
            }

            m_chunk = std::clamp(m_chunk, 0, int(a.chunks.size()) - 1);
            const auto& ch = a.chunks[size_t(m_chunk)];
            m_hasDetail = Astra::Debug::CaptureChunkDetail(
                registry, size_t(selectedArchetype), size_t(m_chunk), m_detail);
            if (m_probe >= int(ch.count)) m_probe = -1;
            BuildRegions(ch);

            DrawChunkStrip(a, snap);
            DrawOverviewBar(a, ch);

            if (ImGui::BeginTabBar("mode"))
            {
                if (ImGui::BeginTabItem("Bytes"))    { m_mode = 0; ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Entities")) { m_mode = 1; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::BeginChild("content", ImVec2(0, -kFooterH));
            if (m_mode == 0) DrawBytes(a, ch); else DrawEntities(a, ch);
            ImGui::EndChild();

            DrawFooter(a, ch);
            ImGui::End();
        }

    private:
        // -- validated dark 8-slot categorical palette (fixed order; ordinal >= 8 folds to gray)
        static constexpr ImU32 kSeries[8] = {
            IM_COL32(0x39, 0x87, 0xE5, 255), IM_COL32(0xD9, 0x59, 0x26, 255),
            IM_COL32(0x19, 0x9E, 0x70, 255), IM_COL32(0xC9, 0x85, 0x00, 255),
            IM_COL32(0xD5, 0x51, 0x81, 255), IM_COL32(0x00, 0x83, 0x00, 255),
            IM_COL32(0x90, 0x85, 0xE9, 255), IM_COL32(0xE6, 0x67, 0x67, 255)};
        static constexpr ImU32 kCellBase = IM_COL32(0x23, 0x23, 0x22, 255);
        static constexpr ImU32 kPadCol   = IM_COL32(0x34, 0x34, 0x2F, 255);
        static constexpr ImU32 kBitsCol  = IM_COL32(0x89, 0x87, 0x81, 255);
        static constexpr ImU32 kHatchCol = IM_COL32(0x2E, 0x2E, 0x2C, 255);
        static constexpr ImU32 kProbeCol = IM_COL32(255, 255, 255, 255);
        static constexpr int   kLinesPerRow = 32;   // 2 KB per grid row
        static constexpr float kFooterH = 58.0f;

        static ImU32 Series(size_t ordinal)
        {
            return ordinal < 8 ? kSeries[ordinal] : kBitsCol;
        }

        static ImU32 Mix(ImU32 from, ImU32 to, float t)
        {
            auto lerp = [&](int shift) {
                const int f = int(from >> shift) & 0xFF, s = int(to >> shift) & 0xFF;
                return ImU32(f + int(t * float(s - f))) << shift;
            };
            return lerp(0) | lerp(8) | lerp(16) | (0xFFu << 24);
        }

        static std::string FormatBytes(size_t b)
        {
            char buf[32];
            if (b >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.2f MB", double(b) / (1024.0 * 1024.0));
            else if (b >= 1024)   std::snprintf(buf, sizeof(buf), "%.1f KB", double(b) / 1024.0);
            else                  std::snprintf(buf, sizeof(buf), "%zu B", b);
            return buf;
        }

        struct Region { size_t begin, end; int column; int kind; };  // kind: 0 col, 1 bits, 2 pad, 3 slack

        void BuildRegions(const Astra::Debug::ChunkInfo& ch)
        {
            m_regions.clear();
            for (size_t c = 0; c < ch.columns.size(); ++c)
            {
                const auto& cl = ch.columns[c];
                m_regions.push_back({cl.offset, cl.offset + cl.bytes, int(c), 0});
                if (cl.disabledOffset != SIZE_MAX)
                    m_regions.push_back({cl.disabledOffset, cl.disabledOffset + cl.disabledBytes, int(c), 1});
            }
            std::sort(m_regions.begin(), m_regions.end(),
                      [](const Region& l, const Region& r) { return l.begin < r.begin; });
            std::vector<Region> filled;
            filled.reserve(m_regions.size() * 2 + 1);
            size_t cursor = 0;
            for (const Region& r : m_regions)
            {
                if (r.begin > cursor) filled.push_back({cursor, r.begin, -1, 2});
                filled.push_back(r);
                cursor = r.end;
            }
            if (cursor < ch.chunkBytes) filled.push_back({cursor, ch.chunkBytes, -1, 3});
            m_regions.swap(filled);
        }

        void DrawChunkStrip(const Astra::Debug::ArchetypeInfo& a,
                            const Astra::Debug::InspectorSnapshot& snap)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("chunks");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < a.chunks.size(); ++i)
            {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::PushID(int(i));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("c", ImVec2(26, 16))) { m_chunk = int(i); m_probe = -1; }
                const auto& ci = a.chunks[i];
                const float f = ci.capacity ? float(ci.count) / float(ci.capacity) : 0.0f;
                dl->AddRectFilled(p, ImVec2(p.x + 26, p.y + 16), kCellBase, 2.0f);
                dl->AddRectFilled(p, ImVec2(p.x + 26.0f * f, p.y + 16), IM_COL32(0x52, 0x51, 0x4E, 255), 2.0f);
                if (int(i) == m_chunk)
                    dl->AddRect(p, ImVec2(p.x + 26, p.y + 16), kProbeCol, 2.0f, 0, 2.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("chunk %zu: %zu/%zu entities, %s",
                                      i, ci.count, ci.capacity, FormatBytes(ci.chunkBytes).c_str());
                ImGui::PopID();
            }
            const auto& ch = a.chunks[size_t(m_chunk)];
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextDisabled("chunk %d | %s | %zu/%zu | slack %.0f%% | line %zu B",
                m_chunk, FormatBytes(ch.chunkBytes).c_str(), ch.count, ch.capacity,
                ch.chunkBytes ? 100.0 * double(ch.slackBytes) / double(ch.chunkBytes) : 0.0,
                snap.cacheLineBytes);
        }

        void DrawFooter(const Astra::Debug::ArchetypeInfo& a, const Astra::Debug::ChunkInfo& ch)
        {
            ImGui::Separator();
            for (size_t c = 0; c < a.columns.size(); ++c)
            {
                if (c) ImGui::SameLine(0.0f, 10.0f);
                ImVec4 col = ImGui::ColorConvertU32ToFloat4(Series(c));
                ImGui::ColorButton(("##sw" + std::to_string(c)).c_str(), col,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(11, 11));
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextUnformatted(a.columns[c].name.c_str());
            }
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::TextDisabled("cols %s | pad %s | bits %s | slack %s",
                FormatBytes(ch.columnBytes).c_str(), FormatBytes(ch.padBytes).c_str(),
                FormatBytes(ch.bitsBytes).c_str(), FormatBytes(ch.slackBytes).c_str());
            if (m_probe >= 0 && size_t(m_probe) < ch.count)
            {
                std::string line = "probe row " + std::to_string(m_probe);
                if (m_hasDetail && size_t(m_probe) < m_detail.entities.size())
                    line += " | entity " + std::to_string(m_detail.entities[size_t(m_probe)].GetID());
                for (size_t c = 0; c < ch.columns.size(); ++c)
                    line += " | " + a.columns[c].name + " @+" +
                            std::to_string(ch.columns[c].offset + size_t(m_probe) * a.columns[c].stride);
                ImGui::TextUnformatted(line.c_str());
            }
            else
            {
                ImGui::TextDisabled("probe: hover to inspect, click to pin");
            }
        }

        // Filled in by Tasks 5-7.
        void DrawOverviewBar(const Astra::Debug::ArchetypeInfo&, const Astra::Debug::ChunkInfo&) {}
        void DrawBytes(const Astra::Debug::ArchetypeInfo&, const Astra::Debug::ChunkInfo&)
        {
            ImGui::TextDisabled("Bytes view: Task 6");
        }
        void DrawEntities(const Astra::Debug::ArchetypeInfo&, const Astra::Debug::ChunkInfo&)
        {
            ImGui::TextDisabled("Entities view: Task 7");
        }

        int m_mode = 0;                 // 0 Bytes, 1 Entities
        int m_chunk = 0;
        float m_zoom = 14.0f;           // Bytes: px per cache-line cell, clamp [1, 32]
        float m_entZoom = 1.0f;         // Entities: x scale, clamp [1, 64]
        float m_entPan = 0.0f;          // Entities: pan px
        int m_probe = -1;               // pinned probe row, -1 none
        bool m_hasDetail = false;
        Astra::Debug::ChunkDetail m_detail;         // reused across frames
        std::vector<Region> m_regions;              // sorted, covers [0, chunkBytes)
        size_t m_visibleByteBegin = 0, m_visibleByteEnd = 0;   // Bytes viewport (overview box)
        float m_scrollRequest = -1.0f;              // Bytes: pending SetScrollY
        long long m_overviewDragByte = -1;          // overview drag -> Bytes centers this byte
    };
}
```

- [ ] **Step 3: Wire into StudioApp**

`studio/StudioApp.hpp`:
- Add `#include "MemoryPanel.hpp"` after the `WorkloadRunner.hpp` include.
- Add member `MemoryPanel m_memoryPanel;` next to `m_runner`.
- In `RenderFrame()`, after `DrawArchetypesPanel();` add:

```cpp
            m_memoryPanel.Draw(m_snapshot, m_selectedArchetype, m_registry);
```

- [ ] **Step 4: Build + tests + manual smoke**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Inspector.*
```

Expected: build clean, 7 Inspector tests PASS. Then run `bin/Debug-windows-x86_64/AstraStudio/AstraStudio.exe` and verify: Memory panel appears; hint text with nothing selected; selecting archetypes in the Archetypes panel switches it; chunk strip renders/selects/tooltips; tabs switch stub texts; footer shows legend chips + accounting; Fighters preset spawns show `bits > 0 B` in the footer (Health enableable).

- [ ] **Step 5: Commit**

```powershell
git add studio/MemoryPanel.hpp studio/StudioApp.hpp studio/Components.hpp studio/WorkloadRunner.hpp
git commit -m "feat(studio): Memory panel scaffold, chunk strip, footer, enableable demo data"
```

---

### Task 5: Overview bar (minimap + accounting + scrubber)

**Files:**
- Modify: `studio/MemoryPanel.hpp` (replace the `DrawOverviewBar` stub)

**Interfaces:**
- Consumes: Task 4 members/constants verbatim; `m_regions`; `m_visibleByteBegin/End` (written by Task 6; 0/0 until then → no viewport box drawn).
- Produces: `m_overviewDragByte` set while the bar is dragged (Task 6 consumes + resets it to −1).

- [ ] **Step 1: Replace the `DrawOverviewBar` stub**

```cpp
        void DrawOverviewBar(const Astra::Debug::ArchetypeInfo& a, const Astra::Debug::ChunkInfo& ch)
        {
            const float h = 22.0f;
            const float w = std::max(50.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("overview", ImVec2(w, h));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const auto X = [&](size_t byte) {
                return p0.x + w * float(double(byte) / double(ch.chunkBytes));
            };

            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kCellBase, 3.0f);
            for (const Region& r : m_regions)
            {
                const ImVec2 r0(X(r.begin), p0.y), r1(X(r.end), p0.y + h);
                if (r.kind == 0)
                {
                    const auto& cl = ch.columns[size_t(r.column)];
                    const size_t liveEnd = cl.offset + ch.count * a.columns[size_t(r.column)].stride;
                    dl->AddRectFilled(ImVec2(X(cl.offset), p0.y), ImVec2(X(liveEnd), p0.y + h),
                                      Series(size_t(r.column)));
                    dl->AddRectFilled(ImVec2(X(liveEnd), p0.y), r1,
                                      Mix(kCellBase, Series(size_t(r.column)), 0.20f));
                }
                else if (r.kind == 1) dl->AddRectFilled(r0, r1, Mix(kCellBase, kBitsCol, 0.55f));
                else if (r.kind == 2) dl->AddRectFilled(r0, r1, kPadCol);
                else
                {
                    // Slack: 45-degree hatch.
                    dl->PushClipRect(r0, r1, true);
                    for (float x = r0.x - h; x < r1.x; x += 6.0f)
                        dl->AddLine(ImVec2(x, p0.y + h), ImVec2(x + h, p0.y), kHatchCol, 1.0f);
                    dl->PopClipRect();
                }
            }

            if (m_probe >= 0)
                for (size_t c = 0; c < ch.columns.size(); ++c)
                {
                    const float x = X(ch.columns[c].offset + size_t(m_probe) * a.columns[c].stride);
                    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + 2.0f, p0.y + h), kProbeCol);
                }

            if (m_mode == 0 && m_visibleByteEnd > m_visibleByteBegin)
                dl->AddRect(ImVec2(X(m_visibleByteBegin), p0.y),
                            ImVec2(X(std::min(m_visibleByteEnd, ch.chunkBytes)), p0.y + h),
                            kProbeCol, 2.0f, 0, 1.5f);

            if (ImGui::IsItemActive() && m_mode == 0)
            {
                const float mx = std::clamp(ImGui::GetMousePos().x, p0.x, p0.x + w);
                m_overviewDragByte = (long long)(double(mx - p0.x) / double(w) * double(ch.chunkBytes));
            }
            if (ImGui::IsItemHovered())
            {
                const float mx = std::clamp(ImGui::GetMousePos().x, p0.x, p0.x + w);
                const size_t byte = size_t(double(mx - p0.x) / double(w) * double(ch.chunkBytes));
                const char* what = "slack";
                int column = -1;
                for (const Region& r : m_regions)
                    if (byte >= r.begin && byte < r.end)
                    {
                        column = r.column;
                        what = r.kind == 0 ? "column" : r.kind == 1 ? "disabled bits" : r.kind == 2 ? "padding" : "slack";
                        break;
                    }
                if (column >= 0)
                    ImGui::SetTooltip("byte %zu | %s: %s", byte, what, a.columns[size_t(column)].name.c_str());
                else
                    ImGui::SetTooltip("byte %zu | %s", byte, what);
            }
        }
```

- [ ] **Step 2: Build + manual smoke**

Build Debug (same command). Run AstraStudio: overview bar shows proportional live/dead column segments, pad slots, dotted-gray bits region (Fighters preset), hatched slack; hover tooltip names the region; with a Fighters spawn the Health bits sliver appears. No viewport box yet (Bytes writes it in Task 6).

- [ ] **Step 3: Commit**

```powershell
git add studio/MemoryPanel.hpp
git commit -m "feat(studio): Memory panel overview bar (minimap, accounting, probe ticks)"
```

---

### Task 6: Bytes tab — cache-line grid with occupancy, tooltips, zoom/pan

**Files:**
- Modify: `studio/MemoryPanel.hpp` (replace the `DrawBytes` stub)

**Interfaces:**
- Consumes: Task 4 members; Task 5's `m_overviewDragByte` (consume + reset −1).
- Produces: writes `m_visibleByteBegin/End` each frame (overview viewport box); `m_scrollRequest` honored at child begin; sets `m_probe` on click.

- [ ] **Step 1: Replace the `DrawBytes` stub**

```cpp
        void DrawBytes(const Astra::Debug::ArchetypeInfo& a, const Astra::Debug::ChunkInfo& ch)
        {
            const size_t lineB = 64;   // snapshot cacheLineBytes is fixed 64 on x64; grid math local
            const float pitch = m_zoom + 2.0f;
            const size_t lines = (ch.chunkBytes + lineB - 1) / lineB;
            const size_t rows = (lines + kLinesPerRow - 1) / kLinesPerRow;

            ImGui::BeginChild("bytes", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (m_scrollRequest >= 0.0f) { ImGui::SetScrollY(m_scrollRequest); m_scrollRequest = -1.0f; }
            if (m_overviewDragByte >= 0)
            {
                const size_t row = size_t(m_overviewDragByte) / lineB / kLinesPerRow;
                m_scrollRequest = std::max(0.0f, float(row) * pitch - ImGui::GetWindowHeight() * 0.5f);
                m_overviewDragByte = -1;
            }
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(float(kLinesPerRow) * pitch, float(rows) * pitch));
            ImDrawList* dl = ImGui::GetWindowDrawList();

            const float scrollY = ImGui::GetScrollY();
            const float viewH = ImGui::GetWindowHeight();
            const size_t rowFirst = size_t(std::max(0.0f, scrollY / pitch));
            const size_t rowLast = std::min(rows, size_t((scrollY + viewH) / pitch) + 1);
            m_visibleByteBegin = rowFirst * size_t(kLinesPerRow) * lineB;
            m_visibleByteEnd = std::min(ch.chunkBytes, rowLast * size_t(kLinesPerRow) * lineB);

            // Probe cache lines (one per column) for outline pass.
            size_t probeLines[8] = {};
            size_t probeLineCount = 0;
            if (m_probe >= 0)
                for (size_t c = 0; c < ch.columns.size() && probeLineCount < 8; ++c)
                    probeLines[probeLineCount++] =
                        (ch.columns[c].offset + size_t(m_probe) * a.columns[c].stride) / lineB;

            for (size_t row = rowFirst; row < rowLast; ++row)
                for (int k = 0; k < kLinesPerRow; ++k)
                {
                    const size_t line = row * size_t(kLinesPerRow) + size_t(k);
                    if (line >= lines) break;
                    const size_t s = line * lineB;
                    const size_t e = std::min(s + lineB, ch.chunkBytes);
                    const ImVec2 c0(origin.x + float(k) * pitch, origin.y + float(row) * pitch);
                    const ImVec2 c1(c0.x + m_zoom, c0.y + m_zoom);

                    const Region* dom = nullptr;
                    size_t domOv = 0, live = 0;
                    for (const Region& r : m_regions)
                    {
                        if (r.end <= s) continue;
                        if (r.begin >= e) break;
                        const size_t ov = std::min(e, r.end) - std::max(s, r.begin);
                        if (r.kind == 0)
                        {
                            const auto& cl = ch.columns[size_t(r.column)];
                            const size_t liveEnd = cl.offset + ch.count * a.columns[size_t(r.column)].stride;
                            if (liveEnd > std::max(s, cl.offset))
                                live += std::min(e, liveEnd) - std::max(s, cl.offset);
                        }
                        if (!dom || ov > domOv) { dom = &r; domOv = ov; }
                    }

                    if (!dom || dom->kind == 2) dl->AddRectFilled(c0, c1, kPadCol, 2.0f);
                    else if (dom->kind == 1) dl->AddRectFilled(c0, c1, Mix(kCellBase, kBitsCol, 0.55f), 2.0f);
                    else if (dom->kind == 3)
                    {
                        dl->AddRectFilled(c0, c1, kCellBase, 2.0f);
                        dl->AddLine(ImVec2(c0.x, c1.y), ImVec2(c1.x, c0.y), kHatchCol, 1.0f);
                    }
                    else
                    {
                        const float f = float(double(live) / double(lineB));
                        dl->AddRectFilled(c0, c1, Mix(kCellBase, Series(size_t(dom->column)), 0.18f + 0.82f * f), 2.0f);
                        if (domOv < e - s)   // line crosses a region boundary: pad notch
                            dl->AddRectFilled(ImVec2(c1.x - 3.0f, c0.y), c1, kPadCol, 2.0f);
                    }

                    for (size_t p = 0; p < probeLineCount; ++p)
                        if (probeLines[p] == line)
                            dl->AddRect(c0, c1, kProbeCol, 2.0f, 0, 2.0f);
                }

            // -------- interaction --------
            if (ImGui::IsWindowHovered())
            {
                ImGuiIO& io = ImGui::GetIO();
                const ImVec2 m = ImGui::GetMousePos();
                const int gx = int((m.x - origin.x) / pitch);
                const int gy = int((m.y - origin.y) / pitch);
                const size_t line = size_t(gy) * size_t(kLinesPerRow) + size_t(gx);
                const bool onGrid = gx >= 0 && gx < kLinesPerRow && gy >= 0 && line < lines;

                if (io.KeyCtrl && io.MouseWheel != 0.0f)
                {
                    const float oldPitch = pitch;
                    m_zoom = std::clamp(m_zoom * (1.0f + 0.15f * io.MouseWheel), 1.0f, 32.0f);
                    const float newPitch = m_zoom + 2.0f;
                    // Keep the content point under the cursor stable.
                    const float contentY = m.y - origin.y;
                    m_scrollRequest = std::max(0.0f, scrollY + contentY / oldPitch * (newPitch - oldPitch));
                }
                else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    ImGui::SetScrollY(scrollY - io.MouseDelta.y);
                    ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
                }

                if (onGrid)
                {
                    const size_t s = line * lineB;
                    const size_t e = std::min(s + lineB, ch.chunkBytes);
                    const Region* reg = nullptr;
                    for (const Region& r : m_regions)
                        if (s < r.end && r.begin < e &&
                            (!reg || std::min(e, r.end) - std::max(s, r.begin) >
                                     std::min(e, reg->end) - std::max(s, reg->begin)))
                            reg = &r;
                    ImGui::BeginTooltip();
                    ImGui::Text("line %zu | bytes %zu-%zu", line, s, e);
                    if (reg && reg->kind == 0)
                    {
                        const auto& cl = ch.columns[size_t(reg->column)];
                        const uint32_t stride = a.columns[size_t(reg->column)].stride;
                        const size_t r0 = s > cl.offset ? (s - cl.offset) / stride : 0;
                        const size_t r1 = std::min(ch.capacity - 1, (std::min(e, cl.offset + cl.bytes) - 1 - cl.offset) / stride);
                        ImGui::Text("%s | rows %zu-%zu (live < %zu)",
                                    a.columns[size_t(reg->column)].name.c_str(), r0, r1, ch.count);
                        if (m_hasDetail && r0 < ch.count)
                            ImGui::Text("first entity: %u",
                                        unsigned(m_detail.entities[std::min(r0, ch.count - 1)].GetID()));
                        // Click pins the probe to the first row in this line.
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0.0f &&
                            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0f)
                            m_probe = (r0 < ch.count) ? int(r0) : -1;
                    }
                    else if (reg && reg->kind == 1)
                        ImGui::Text("disabled-bit words: %s", a.columns[size_t(reg->column)].name.c_str());
                    else if (reg && reg->kind == 2) ImGui::TextUnformatted("alignment padding");
                    else ImGui::TextUnformatted("slack (unused arena tail)");
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndChild();
        }
```

- [ ] **Step 2: Build + manual smoke**

Build Debug; run AstraStudio. Verify: grid renders with brightness = occupancy (spawn more entities → cells brighten; Clear → dim); pad/bits/slack encodings match the overview; scrolling moves the white viewport box on the overview bar; dragging on the overview bar scrolls the grid; ctrl+wheel zooms about the cursor between 1–32 px; drag pans; hover tooltip shows line/bytes/column/rows; click pins the probe → white outlines appear (one per column) and the footer readout fills in; probe ticks appear on the overview bar.

- [ ] **Step 3: Commit**

```powershell
git add studio/MemoryPanel.hpp
git commit -m "feat(studio): Memory panel Bytes tab -- cache-line grid, occupancy, zoom/pan, probe"
```

---

### Task 7: Entities tab — SoA lanes with ticks, notches, probe

**Files:**
- Modify: `studio/MemoryPanel.hpp` (replace the `DrawEntities` stub)

**Interfaces:**
- Consumes: Task 4 members (`m_entZoom`, `m_entPan`, `m_probe`, `m_detail`, `m_hasDetail`); `ChunkDetail.disabledWords` indexed by column ordinal.
- Produces: sets `m_probe` on click (shared with Bytes tab + footer + overview).

- [ ] **Step 1: Replace the `DrawEntities` stub**

```cpp
        void DrawEntities(const Astra::Debug::ArchetypeInfo& a, const Astra::Debug::ChunkInfo& ch)
        {
            const float labelW = 170.0f, bytesW = 110.0f, laneH = 20.0f, gapY = 4.0f;
            ImGui::BeginChild("lanes", ImVec2(0, 0));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 org = ImGui::GetCursorScreenPos();
            const float trackW = std::max(50.0f, ImGui::GetContentRegionAvail().x - labelW - bytesW - 16.0f);
            const float scale = trackW * m_entZoom / float(ch.capacity);   // px per entity
            m_entPan = std::clamp(m_entPan, 0.0f, std::max(0.0f, trackW * (m_entZoom - 1.0f)));
            const float x0 = org.x + labelW;
            const auto entX = [&](float i) { return x0 + i * scale - m_entPan; };

            int hoverRow = -1;
            for (size_t c = 0; c < a.columns.size(); ++c)
            {
                const float y = org.y + float(c) * (laneH + gapY);
                const uint32_t stride = a.columns[c].stride;

                char label[96];
                std::snprintf(label, sizeof(label), "%s  %u B", a.columns[c].name.c_str(), stride);
                dl->AddText(ImVec2(org.x, y + 3.0f), IM_COL32(0xC3, 0xC2, 0xB7, 255), label);

                dl->PushClipRect(ImVec2(x0, y), ImVec2(x0 + trackW, y + laneH), true);
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + trackW, y + laneH), kCellBase, 3.0f);
                dl->AddRectFilled(ImVec2(entX(0), y), ImVec2(entX(float(ch.count)), y + laneH), Series(c));
                dl->AddRectFilled(ImVec2(entX(float(ch.count)), y), ImVec2(entX(float(ch.capacity)), y + laneH),
                                  Mix(kCellBase, Series(c), 0.20f));

                const float step = 64.0f / float(stride);          // entities per cache line
                if (step * scale >= 3.0f)
                    for (float t = step; t < float(ch.capacity); t += step)
                        dl->AddLine(ImVec2(entX(t), y), ImVec2(entX(t), y + laneH),
                                    IM_COL32(0, 0, 0, 115), 1.0f);

                if (m_hasDetail && c < m_detail.disabledWords.size() && !m_detail.disabledWords[c].empty())
                {
                    const auto& words = m_detail.disabledWords[c];
                    for (size_t w = 0; w < words.size(); ++w)
                        for (uint64_t bits = words[w]; bits; bits &= bits - 1)
                        {
                            const size_t idx = w * 64 + size_t(std::countr_zero(bits));
                            if (idx >= ch.count) break;
                            dl->AddRectFilled(ImVec2(entX(float(idx)), y),
                                              ImVec2(entX(float(idx)) + std::max(2.0f, scale * 0.6f), y + laneH),
                                              IM_COL32(0x12, 0x12, 0x12, 255), 1.0f);
                        }
                }
                dl->PopClipRect();

                char bytesLbl[64];
                std::snprintf(bytesLbl, sizeof(bytesLbl), "%s%s%u off",
                              FormatBytes(ch.columns[c].bytes).c_str(),
                              ch.columns[c].disabledCount ? " | " : "",
                              ch.columns[c].disabledCount);
                if (!ch.columns[c].disabledCount)
                    std::snprintf(bytesLbl, sizeof(bytesLbl), "%s", FormatBytes(ch.columns[c].bytes).c_str());
                dl->AddText(ImVec2(x0 + trackW + 8.0f, y + 3.0f), IM_COL32(0x89, 0x87, 0x81, 255), bytesLbl);
            }

            const float lanesBottom = org.y + float(a.columns.size()) * (laneH + gapY);
            if (m_probe >= 0)
            {
                const float px = entX(float(m_probe) + 0.5f);
                if (px >= x0 && px <= x0 + trackW)
                {
                    dl->AddRectFilled(ImVec2(px - 1.0f, org.y - 2.0f), ImVec2(px + 1.0f, lanesBottom), kProbeCol);
                    char flag[32];
                    std::snprintf(flag, sizeof(flag), "row %d", m_probe);
                    dl->AddText(ImVec2(px - 20.0f, lanesBottom + 2.0f), kProbeCol, flag);
                }
            }
            ImGui::Dummy(ImVec2(labelW + trackW + bytesW, lanesBottom - org.y + 18.0f));

            // -------- interaction --------
            if (ImGui::IsWindowHovered())
            {
                ImGuiIO& io = ImGui::GetIO();
                const ImVec2 m = ImGui::GetMousePos();
                const bool onTracks = m.x >= x0 && m.x <= x0 + trackW && m.y >= org.y && m.y < lanesBottom;
                if (io.KeyCtrl && io.MouseWheel != 0.0f && onTracks)
                {
                    const float entUnder = (m.x - x0 + m_entPan) / scale;
                    m_entZoom = std::clamp(m_entZoom * (1.0f + 0.15f * io.MouseWheel), 1.0f, 64.0f);
                    const float newScale = trackW * m_entZoom / float(ch.capacity);
                    m_entPan = std::max(0.0f, entUnder * newScale - (m.x - x0));
                }
                else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && onTracks)
                {
                    m_entPan = std::max(0.0f, m_entPan - io.MouseDelta.x);
                }
                if (onTracks)
                {
                    const int lane = int((m.y - org.y) / (laneH + gapY));
                    const int row = int((m.x - x0 + m_entPan) / scale);
                    if (lane >= 0 && lane < int(a.columns.size()) && row >= 0 && row < int(ch.capacity))
                    {
                        hoverRow = row;
                        const size_t c = size_t(lane);
                        const uint32_t stride = a.columns[c].stride;
                        ImGui::BeginTooltip();
                        ImGui::Text("%s | row %d%s", a.columns[c].name.c_str(), row,
                                    row < int(ch.count) ? "" : " (no entity)");
                        ImGui::Text("byte +%zu | cache line %zu",
                                    ch.columns[c].offset + size_t(row) * stride,
                                    (ch.columns[c].offset + size_t(row) * stride) / 64);
                        if (m_hasDetail && row < int(m_detail.entities.size()))
                        {
                            ImGui::Text("entity %u v%u",
                                        unsigned(m_detail.entities[size_t(row)].GetID()),
                                        unsigned(m_detail.entities[size_t(row)].GetVersion()));
                            if (c < m_detail.disabledWords.size() && !m_detail.disabledWords[c].empty())
                                ImGui::Text("%s", ((m_detail.disabledWords[c][size_t(row) / 64] >>
                                                    (size_t(row) % 64)) & 1u) ? "DISABLED" : "enabled");
                        }
                        ImGui::EndTooltip();
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0f)
                            m_probe = row < int(ch.count) ? row : -1;
                    }
                }
            }
            (void)hoverRow;
            ImGui::EndChild();
        }
```

Add `#include <bit>` to the include block at the top of `studio/MemoryPanel.hpp` (`std::countr_zero`).

- [ ] **Step 2: Build + manual smoke**

Build Debug; run AstraStudio. Verify: one lane per column with live/dead split tracking spawn/clear; tick density differs per stride (Health ticks sparser than Sprite's); Fighters preset shows dark disabled notches on the Health lane and "N off" in the right label; hover tooltip shows row/byte/line/entity/enabled; click pins probe → white rule crosses all lanes, footer readout and overview ticks update, and switching to Bytes shows the same probe's outlined cells; ctrl+wheel x-zooms about the cursor, drag pans.

- [ ] **Step 3: Commit**

```powershell
git add studio/MemoryPanel.hpp
git commit -m "feat(studio): Memory panel Entities tab -- SoA lanes, line ticks, disabled notches"
```

---

### Task 8: 3-config gate + finish

**Files:**
- None (build/test gate)

- [ ] **Step 1: Build all three configs**

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Release -p:Platform=x64 -m
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Dist -p:Platform=x64 -m
```

Expected: all three succeed.

- [ ] **Step 2: Run the full suite in all three configs**

```powershell
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe
bin/Release-windows-x86_64/AstraTest/AstraTest.exe
bin/Dist-windows-x86_64/AstraTest/AstraTest.exe
```

Expected: zero failures, and the count equals the branch-point baseline (~805 on dev @ `c6925d4`; verify the exact number by running the suite once at the branch point) + the 5 new Inspector tests. Gate on "all green + intended new tests," not an absolute number. A lone `CompressionTest.PerformanceBenchmark` failure = known flake, rerun isolated.

- [ ] **Step 3: Final studio smoke in Release**

Run `bin/Release-windows-x86_64/AstraStudio/AstraStudio.exe`: spawn Mixed 10000, auto-step on; Memory panel stays smooth; all Task 4–7 checklist behaviors hold.

- [ ] **Step 4: Report for review**

Stop here — the finishing decision (whole-branch review, FF merge to dev, branch delete) is the controller's per the standing SDD pattern; do not merge inside a task.

---

## Self-Review (done at write time)

- **Spec coverage:** engine accessors (T1), facade layout/accounting/bytesReserved/cacheLineBytes/capture-into (T2), CaptureChunkDetail + hot-loop scoping (T3), composite chrome + selection-follow + edge handling (T4), overview bar (T5), Bytes grid + zoom/pan + tooltips + viewport link (T6), Entities lanes + notches + probe (T7), 3-config gate + suite (T8). Palette + encodings match the spec's validated values.
- **Placeholder scan:** none; all steps carry complete code/commands.
- **Type consistency:** `ChunkColumnLayout`/`ChunkInfo`/`ChunkDetail` field names identical across T2/T3 code and T4–T7 consumers; `Draw(snap, selectedArchetype, registry)` signature consistent in T4 wiring; `m_overviewDragByte` produced (T5) and consumed (T6); `disabledWords` indexed by column ordinal in both T3 and T7.
