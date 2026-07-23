# Phase C — Chunk Storage Modernization (W5+W7+W4+W3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store component-column metadata **once per archetype** (`ArchetypeColumnMeta`: shared `const ComponentDescriptor*` + `int16 idToColumn[128]` + `isComplex`) and give each chunk a **packed columns array** instead of the 128-slot `ComponentArrayInfo` that embeds a ~176 B `ComponentDescriptor` by value (~23 KB/chunk) — then use the new layout for a trivial-`memcpy` transition move.

**Architecture:** Per-archetype `ArchetypeColumnMeta` (built once in `Archetype::Initialize`, address-stable, `descriptors` point into the archetype's canonical `m_componentDescriptors`). Per-chunk: `m_meta` back-pointer + `Column{void* base; uint32 stride}` array (fixed `MAX_COMPONENTS` capacity, `[0,columnCount)` live). `Get(id)` resolves `idToColumn[id]→column→base+stride`. Cross-archetype move merge-joins the two archetypes' sorted column-id lists and `memcpy`s trivially-copyable columns.

**Tech Stack:** Header-only C++20, MSVC (`Astra.sln` via MSBuild, 3 configs), GoogleTest.

**Spec:** `docs/superpowers/specs/2026-07-22-phase-c-chunk-storage-design.md`

## Global Constraints

- **Header-only, C++20.** Exception-free (no `throw`), RTTI-off (no `<any>`/`typeid`/`dynamic_cast`). Match surrounding `ASTRA_*` / `noexcept` / `ASTRA_ASSERT` / `ASTRA_NODISCARD` / `ASTRA_FORCEINLINE` style.
- **No on-disk serialization format change.** Chunk metadata is runtime-only; the format iterates `m_componentDescriptors` and stores data by `ComponentID`. The serialization suite is the regression net.
- **No public `Registry` API change.**
- **`MAX_COMPONENTS` (128) is the `ComponentID` ceiling.** `ComponentID` is `std::uint16_t`.
- **Trivial fast paths must never apply to non-trivially-relocatable components.** The `isComplex` / per-column `descriptor->is_trivially_copyable` check is a **correctness boundary** — a `memcpy` of a move-only or self-referential type is a bug. Non-trivial types keep the `MoveConstruct`/`Destruct` path.
- **`ArchetypeManager` must never write `EntityRecord::version`** (unchanged from W1).
- **Tags (zero-`size` components) are excluded from data columns**; `idToColumn[tagId] = -1`; presence stays in the `ComponentMask`; `Get<Tag>` short-circuits at compile time.
- **TypeID ceiling:** reuse `tests/TestComponents.hpp` (`Astra::Test::Position/Velocity/Health`) and the existing move-only test type (search for `Tracked` / a move-only component in `tests/`); register no new component types. Chunk/meta unit tests build descriptors for existing registered types.
- **3-config green:** MSVC **Debug, Release, Dist** all pass.

**Build / test / bench commands:**
```bash
# Build one config (repeat Debug/Release/Dist):
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
# Run tests:
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe
bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter='ArchetypeTest.*:ArchetypeManagerTest.*'
# Bench (Task 5): rebuild bench_astra via bench-compare/build_one.bat, run all three exes.
```
No new source **files** are added (edits to existing headers/tests only), so **no premake regeneration is needed**. Judge success ONLY by the MSVC build + GoogleTest — IDE clang/clangd diagnostics (missing `Mosaic/Platform.hpp`, `MOSAIC_NODISCARD`, `gtest not found`, `std::byte`, spurious unused-include/undeclared) are KNOWN false positives. Stale PDB lock → `taskkill //F //IM mspdbsrv.exe` + clean rebuild.

Baseline test counts on this branch (= dev `a8712d6`): **Debug 689 / Release 687 / Dist 687**.

Branch: `perf/phase-c-chunk-storage` (off dev `a8712d6`).

**Reference — `ComponentDescriptor` fields used by this plan** (`include/Astra/Component/Component.hpp:36`): `id` (ComponentID), `size` (size_t; 0 ⇒ tag), `is_trivially_copyable` (bool), `is_trivially_default_constructible` (bool), `is_empty` (bool); methods `DefaultConstruct(void*)`, `MoveConstruct(void* dst, void* src)`, `Destruct(void*)`, `ConstructWith(void*, const void*)`, `BatchDefaultConstruct(void*, size_t)`.

---

### Task 1: `ArchetypeColumnMeta` — per-archetype metadata, built once (additive)

Adds the shared metadata struct and builds it in `Archetype`, **without yet consuming it in the chunk** (that is Task 2). Purely additive: the tree compiles and the full suite passes throughout.

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (define `ArchetypeColumnMeta` at namespace scope, before `class ArchetypeChunkPool`)
- Modify: `include/Astra/Archetype/Archetype.hpp` (add `m_columnMeta` member + `BuildColumnMeta()`, call it in `Initialize`; add a `GetColumnMeta()` accessor)
- Test: `tests/Registry/ArchetypeTest.cpp` (append meta unit tests — existing file)

**Interfaces:**
- Produces:
  - `struct Astra::ArchetypeColumnMeta` with: `uint16_t columnCount`; `bool isComplex`; `struct ColumnDesc { ComponentID id; uint32_t stride; const ComponentDescriptor* descriptor; }`; `ColumnDesc columns[MAX_COMPONENTS]` (`[0,columnCount)` live, ascending `id`); `int16_t idToColumn[MAX_COMPONENTS]` (column index, or `-1`).
  - `const ArchetypeColumnMeta& Archetype::GetColumnMeta() const noexcept`.

- [ ] **Step 1: Write the failing meta unit tests**

Append to `tests/Registry/ArchetypeTest.cpp` (match its existing includes/namespace; it already constructs `Archetype` + registers descriptors for `Astra::Test::*` — mirror that setup; confirm the exact descriptor-registration helper the file already uses and reuse it). The test must cover: ascending column ids, `idToColumn` (present-storage → col, tag → -1, absent → -1), `columnCount` excludes tags, `isComplex` for a trivial vs a non-trivial archetype.

```cpp
// W5: per-archetype column metadata is built once, excludes tags, sorts columns by id,
// maps id->column, and flags complex (non-trivially-copyable) component sets.
TEST(ArchetypeColumnMeta, BuiltOnceExcludesTagsAndMapsIds)
{
    using namespace Astra;
    // Build an archetype over Position + Velocity (both non-empty, trivially copyable).
    // Use the file's existing descriptor-build helper for Astra::Test::Position/Velocity.
    Archetype a(/* mask over Position+Velocity, per this file's existing pattern */);
    a.Initialize(/* {Position, Velocity} descriptors, ascending or not */);

    const ArchetypeColumnMeta& m = a.GetColumnMeta();
    ASSERT_EQ(m.columnCount, 2u);
    // Columns ascending by id:
    EXPECT_LT(m.columns[0].id, m.columns[1].id);
    // idToColumn round-trips for present components:
    EXPECT_EQ(m.idToColumn[m.columns[0].id], 0);
    EXPECT_EQ(m.idToColumn[m.columns[1].id], 1);
    // stride == descriptor size; descriptor pointer non-null and points at the right id:
    EXPECT_EQ(m.columns[0].stride, static_cast<uint32_t>(m.columns[0].descriptor->size));
    EXPECT_EQ(m.columns[0].descriptor->id, m.columns[0].id);
    // An id NOT in the archetype maps to -1:
    ComponentID absent = TypeID<Test::Health>::Value();
    EXPECT_EQ(m.idToColumn[absent], -1);
    // Trivially-copyable set ⇒ not complex:
    EXPECT_FALSE(m.isComplex);
}
```
Add a second test `ArchetypeColumnMeta.TagExcludedAndComplexFlag` that builds an archetype containing a zero-size tag component and a non-trivially-copyable component (reuse an existing empty/tag test type and the move-only `Tracked` type): assert the tag id has `idToColumn == -1` and is NOT counted in `columnCount`, and that `isComplex == true`.

> **Before finalizing:** read `tests/Registry/ArchetypeTest.cpp`'s existing `Archetype` construction/registration pattern and adapt the two tests to it verbatim (mask construction, descriptor list source, the empty/tag and move-only test types actually available). Do NOT register new component types.

- [ ] **Step 2: Build Debug, confirm the tests FAIL to compile**

Expected: FAIL — `ArchetypeColumnMeta` / `GetColumnMeta` don't exist yet.

- [ ] **Step 3: Define `ArchetypeColumnMeta`**

In `include/Astra/Archetype/ArchetypeChunkPool.hpp`, at namespace `Astra` scope, **before** `class ArchetypeChunkPool` (after the includes; `ComponentDescriptor`, `ComponentID`, `MAX_COMPONENTS` come from the already-included `Component.hpp`):

```cpp
// Per-archetype column metadata, built once and shared by every chunk of the archetype.
// The heavy ComponentDescriptor lives here once (via pointer into the archetype's canonical
// list) instead of by-value in every chunk slot. Columns are storage-bearing components only
// (tags excluded), sorted ascending by id so cross-archetype moves can merge-join.
struct ArchetypeColumnMeta
{
    struct ColumnDesc
    {
        ComponentID id{0};
        uint32_t stride{0};                          // == descriptor->size
        const ComponentDescriptor* descriptor{nullptr};
    };

    uint16_t  columnCount{0};                        // N: storage-bearing components
    bool      isComplex{false};                      // any column not trivially copyable
    ColumnDesc columns[MAX_COMPONENTS]{};            // [0, columnCount) valid, ascending id
    int16_t   idToColumn[MAX_COMPONENTS];            // id -> column index, or -1 (absent OR tag)

    ArchetypeColumnMeta() { for (auto& c : idToColumn) c = -1; }
};
```

- [ ] **Step 4: Add `m_columnMeta` + `BuildColumnMeta()` + `GetColumnMeta()` to `Archetype`**

In `include/Astra/Archetype/Archetype.hpp`: add the private member near `m_componentDescriptors` (`~:1444`):
```cpp
ArchetypeColumnMeta m_columnMeta;
```
Add a private method (place near `Initialize`):
```cpp
void BuildColumnMeta()
{
    m_columnMeta = ArchetypeColumnMeta{};   // resets idToColumn to -1, columnCount/isComplex to 0/false
    for (const ComponentDescriptor& desc : m_componentDescriptors)
    {
        if (desc.size == 0) continue;       // tag: no storage column (idToColumn stays -1)
        const uint16_t col = m_columnMeta.columnCount++;
        m_columnMeta.columns[col] = { desc.id, static_cast<uint32_t>(desc.size), &desc };
        if (!desc.is_trivially_copyable) m_columnMeta.isComplex = true;
    }
    // Guarantee ascending-by-id column order (merge-join requirement in W3). If
    // m_componentDescriptors is already id-ascending this is a no-op; sort defensively.
    std::sort(m_columnMeta.columns, m_columnMeta.columns + m_columnMeta.columnCount,
              [](const ArchetypeColumnMeta::ColumnDesc& a, const ArchetypeColumnMeta::ColumnDesc& b)
              { return a.id < b.id; });
    for (uint16_t c = 0; c < m_columnMeta.columnCount; ++c)
        m_columnMeta.idToColumn[m_columnMeta.columns[c].id] = static_cast<int16_t>(c);
}
```
Call `BuildColumnMeta();` in `Initialize()` **immediately after `m_componentDescriptors = componentDescriptors;`** (`Archetype.hpp:73`) so `m_columnMeta.descriptors` point into the now-stable `m_componentDescriptors` (built once, never mutated). Add the public accessor near `GetComponentDescriptors()` (`:1057`):
```cpp
ASTRA_NODISCARD const ArchetypeColumnMeta& GetColumnMeta() const noexcept { return m_columnMeta; }
```
Ensure `<algorithm>` is included in `Archetype.hpp` (for `std::sort`) — add if absent.

> **Stability note:** `m_columnMeta.columns[c].descriptor` points into `m_componentDescriptors` (a `std::vector` set once in `Initialize` and never mutated). `Archetype` is heap-allocated via `std::make_unique` and not moved in practice, so these pointers are stable for the archetype's lifetime. Do not reassign `m_componentDescriptors` after `Initialize`.

- [ ] **Step 5: Build Debug + run the meta tests**

Run: build Debug, `AstraTest.exe --gtest_filter='ArchetypeColumnMeta.*'`. Expected: PASS.

- [ ] **Step 6: Full Debug suite (additive regression check)**

Run: `AstraTest.exe` (Debug). Expected: **Debug 691** (689 baseline + 2 new — verify the +2 delta). The chunk still uses the old 128-slot array; this task only ADDED the meta.

- [ ] **Step 7: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp tests/Registry/ArchetypeTest.cpp
git commit -m "feat(archetype): per-archetype ArchetypeColumnMeta built once (W5 metadata scaffold)"
```

---

### Task 2: Convert `Chunk` to `m_meta` + packed columns (W5 + W7) — behavior-preserving

Replace the per-chunk `std::array<ComponentArrayInfo, MAX_COMPONENTS>` **and** the duplicate `std::vector<ComponentDescriptor> m_componentDescriptors` with the shared `m_meta` pointer + a packed `Column` array. `Get(id)` now resolves via `idToColumn` (**W7**). This is a **behavior-preserving refactor** — no red-green per feature; the existing full suite is the guard, plus Task 1's meta tests. This is the large, invasive task (**opus**, 3-config).

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`Chunk`: members, ctor, `InitializeColumns`, every accessor/hot loop; `CreateChunk` signature)
- Modify: `include/Astra/Archetype/Archetype.hpp` (all `CreateChunk` call sites; `MoveEntityFrom` + `MoveEntitiesBetweenChunks` rewired to meta/columns — still per-element; the `Deserialize` chunk-creation path)

**Interfaces:**
- Consumes: `Archetype::GetColumnMeta()` / `ArchetypeColumnMeta` (Task 1).
- Produces (new/changed `Chunk` surface): `Chunk` ctor takes `const ArchetypeColumnMeta*`; `CreateChunk(size_t, const ArchetypeColumnMeta*)`; `Get`/`GetComponentPointer`/`GetComponentArrayByID` resolve via `idToColumn`. Removes: `GetComponentArrays()`, `ComponentArrayInfo`, `m_componentArrays`, per-chunk `m_componentDescriptors`.

- [ ] **Step 1: Replace the `Chunk` storage members**

In `Chunk` (private section, `~ArchetypeChunkPool.hpp:554-560`): remove `std::vector<ComponentDescriptor> m_componentDescriptors;` and `std::array<ComponentArrayInfo, MAX_COMPONENTS> m_componentArrays{};`. Remove the `ComponentArrayInfo` struct (`:478-484`). Add:
```cpp
struct Column { void* base{nullptr}; uint32_t stride{0}; };
const ArchetypeColumnMeta* m_meta{nullptr};
Column m_columns[MAX_COMPONENTS]{};   // [0, m_meta->columnCount) live; fixed capacity avoids a per-chunk heap alloc
```
Update the `Chunk` move ctor (`:78-86`) to move `m_meta` (pointer) and `m_columns` (array — memberwise copy of trivial `Column`s is fine) instead of `m_componentDescriptors`/`m_componentArrays`.

- [ ] **Step 2: Rewrite the ctor + `InitializeColumns`**

Change the private ctor (`:505`) signature from `const std::vector<ComponentDescriptor>& componentDescriptors` to `const ArchetypeColumnMeta* meta`; store `m_meta(meta)`; drop the `m_componentDescriptors(...)` init. Replace `InitializeComponentArrays()` (`:517-552`) with:
```cpp
void InitializeColumns()
{
    size_t offset = 0;
    for (uint16_t c = 0; c < m_meta->columnCount; ++c)
    {
        offset = (offset + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);  // cache-line align each column
        m_columns[c].base   = static_cast<std::byte*>(m_memory) + offset;
        m_columns[c].stride = m_meta->columns[c].stride;
        offset += static_cast<size_t>(m_meta->columns[c].stride) * m_capacity;
    }
    ASTRA_ASSERT(offset <= m_chunkSize, "Component layout exceeds chunk size");
}
```
(Tags are absent from `columns`, so there are no `base==nullptr` storage columns — the old `desc.size==0` branch is gone.) Call `InitializeColumns()` where `InitializeComponentArrays()` was called.

- [ ] **Step 3: Rewrite `CreateChunk` + all call sites**

`ArchetypeChunkPool::CreateChunk` (`:647`): change param to `const ArchetypeColumnMeta* meta`; pass `meta` to `new Chunk(entitiesPerChunk, meta, memory, m_config.chunkSize)`. Update every call site in `Archetype.hpp` to pass `&m_columnMeta` (or `&GetColumnMeta()`): lines ~128, ~179, ~248, ~1149, ~1342. For the `Deserialize` static method (`Archetype.hpp:852`, which builds a local `descriptors` vector and reconstructs an archetype): the reconstructed archetype must have its `m_columnMeta` built (via `Initialize`/`BuildColumnMeta`) before its chunks are created; pass that archetype's `&GetColumnMeta()` to `CreateChunk`. Verify the deserialize flow builds the meta before creating chunks.

- [ ] **Step 4: Rewrite `Get`/pointer accessors to use `idToColumn` (W7)**

Replace `GetComponentPointer` (`:490-500`), `GetComponentArrayByID` (`:464-467`), and the `id`-indexed reads in `GetComponent<T>`/`GetComponentArray<T>` (`:406-462`). New resolver:
```cpp
void* GetComponentPointer(ComponentID id, size_t index) const
{
    ASTRA_ASSERT(id < MAX_COMPONENTS, "ComponentID out of bounds");
    ASTRA_ASSERT(index < m_count, "Index out of bounds");
    const int col = m_meta->idToColumn[id];
    if (col < 0) ASTRA_UNLIKELY return nullptr;       // absent or tag: no storage
    return static_cast<std::byte*>(m_columns[col].base) + index * m_columns[col].stride;
}
void* GetComponentArrayByID(ComponentID id) const
{
    const int col = m_meta->idToColumn[id];
    return col < 0 ? nullptr : m_columns[col].base;
}
```
`GetComponent<T>`/`GetComponentArray<T>` keep their compile-time `std::is_empty_v<T>` tag short-circuit (returning the static instance / nullptr); for non-empty `T`, resolve the base via `GetComponentArrayByID(TypeID<BaseType>::Value())` (or the `idToColumn` inline). Delete the `GetComponentArrays()` accessor (`:486`) and update its consumers (Step 5).

- [ ] **Step 5: Rewrite the `0..MAX_COMPONENTS` hot loops to `0..columnCount`**

Apply this transformation to every loop that scanned `for (ComponentID id = 0; id < MAX_COMPONENTS; ++id) { const auto& info = m_componentArrays[id]; if (!info.isValid || info.base == nullptr) continue; ... info.stride ... info.descriptor.X(...) ... }`:
```
for (uint16_t c = 0; c < m_meta->columnCount; ++c)
{
    void* base = m_columns[c].base;
    const uint32_t stride = m_columns[c].stride;
    const ComponentDescriptor& desc = *m_meta->columns[c].descriptor;
    // ... use base + index*stride and desc.DefaultConstruct/MoveConstruct/Destruct(...) ...
}
```
Representative full rewrite — `AddEntity` (`:110-130`):
```cpp
size_t AddEntity(Entity entity)
{
    ASTRA_ASSERT(m_count < m_capacity, "Chunk is full, cannot add more entities");
    size_t index = m_count++;
    m_entities.push_back(entity);
    for (uint16_t c = 0; c < m_meta->columnCount; ++c)
    {
        void* ptr = static_cast<std::byte*>(m_columns[c].base) + index * m_columns[c].stride;
        m_meta->columns[c].descriptor->DefaultConstruct(ptr);
    }
    return index;
}
```
Apply the same pattern (iterate `0..columnCount`, `base=m_columns[c].base`, `stride=m_columns[c].stride`, `desc=*m_meta->columns[c].descriptor`) to: `~Chunk` (`:91-108`), `AddEntityWithComponents` (`:167-199` — the "default construct the not-provided" pass; keep the `willBeConstructed` check via `desc.id`), `BatchAddEntities` (`:201-218`), `RemoveEntity` (`:351-404` — both the swap and the last-index branches), and `BatchMoveComponentsFrom` (`:220-266` — iterate `dst`'s columns; for each, find the matching `src` column via `srcChunk.m_meta->idToColumn[dstId]`; keep the trivially-copyable contiguous-`memcpy` fast path using `descriptor->is_trivially_copyable`). `ConstructComponentAt` (`:133-164`) and `BatchConstructComponent` (`:282-349`) resolve their single component via `m_meta->idToColumn[TypeID<T>::Value()]` (early-return if `< 0`).

- [ ] **Step 6: Rewrite `Archetype::MoveEntityFrom` + `MoveEntitiesBetweenChunks` (per-element, behavior-preserving)**

`MoveEntityFrom` (`Archetype.hpp:412-450`) currently iterates `m_componentDescriptors` and probes `dstChunk->GetComponentArrays()[id]` / `srcChunk->...[id]`. `GetComponentArrays()` is deleted — rewrite to iterate the **dst** archetype's columns and resolve the src column via the src meta (this is the per-element version; W3 adds the memcpy fast path in Task 4):
```cpp
void MoveEntityFrom(EntityLocation dst, Archetype& srcArchetype, EntityLocation src)
{
    auto& dstChunk = m_chunks[dst.GetChunkIndex()];
    auto& srcChunk = srcArchetype.m_chunks[src.GetChunkIndex()];
    const ArchetypeColumnMeta& dm = m_columnMeta;
    const ArchetypeColumnMeta& sm = srcArchetype.m_columnMeta;
    for (uint16_t c = 0; c < dm.columnCount; ++c)
    {
        const ComponentID id = dm.columns[c].id;
        void* dstPtr = dstChunk->GetComponentPointer(id, dst.GetEntityIndex());
        const int sc = sm.idToColumn[id];
        if (sc >= 0) ASTRA_LIKELY
        {
            void* srcPtr = srcChunk->GetComponentPointer(id, src.GetEntityIndex());
            dm.columns[c].descriptor->MoveConstruct(dstPtr, srcPtr);
        }
        else ASTRA_UNLIKELY
        {
            dm.columns[c].descriptor->DefaultConstruct(dstPtr);
        }
    }
}
```
Confirm `GetComponentPointer` is accessible from `Archetype` (it is — public on `Chunk`). Rewrite `MoveEntitiesBetweenChunks` (`Archetype.hpp:1382`) the same way if it references `GetComponentArrays()`/`m_componentArrays` — read it and convert any 128-slot access to `columnCount`/`idToColumn`.

- [ ] **Step 7: Build Debug, fix every remaining `m_componentArrays` / `ComponentArrayInfo` / `GetComponentArrays` reference**

The deleted members/struct make every leftover a hard compile error (the completeness net). Grep the tree: `grep -rn "m_componentArrays\|ComponentArrayInfo\|GetComponentArrays\|m_componentDescriptors" include/ tests/` — `include/` must have ZERO chunk references left (archetype's own `m_componentDescriptors` stays; the per-chunk copy is gone). Fix any test that reached into `GetComponentArrays()`.

- [ ] **Step 8: Full 3-config build + full suite (behavior-preserving gate)**

Build Debug/Release/Dist; run `AstraTest.exe` each. Expected: **Debug 691 / Release 689 / Dist 689** (baseline + Task 1's 2 tests; Task 2 adds none). ALL green — especially serialization (`RegistrySerializationTest`, `FormatV2Test`, `LoadRobustnessTest`), `RootArchetypeRoundTripTest`, iteration, `ArchetypeManagerTest`, and every move-semantics test. A serialization/round-trip failure means a real behavior change — STOP and diagnose (do not adjust the test).

- [ ] **Step 9: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp include/Astra/Archetype/Archetype.hpp tests/
git commit -m "perf(archetype): pack chunk columns + shared metadata; idToColumn get (W5+W7)"
```

---

### Task 3: Fast-append (W4)

Skip redundant default-construction on the create path. Small, low-risk.

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeChunkPool.hpp` (`AddEntityWithComponents`, and `AddEntity` for trivial types)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` or `ArchetypeTest.cpp` (append a create-correctness test)

- [ ] **Step 1: Write a create-correctness guard test**

Append (reuse fixtures; `Astra::Test::Position/Velocity`): create a batch of entities that each provide values for every component in their archetype, and assert every component reads back its exact value across the batch (guards that avoiding double-construct of provided components doesn't leave stale/zeroed data or off-by-one column writes). The batch (> one chunk's worth if cheap) exercises the append path repeatedly.

```cpp
// W4: fast-append must not leave provided components default/zeroed, and must write each
// entity's values into the correct packed column (no cross-column or stale-slot corruption).
TEST_F(ArchetypeManagerTest, FastAppendPreservesAllProvidedValues)
{
    using namespace Astra; using namespace Astra::Test;
    constexpr int N = 500;
    std::vector<Entity> ents;
    for (int i = 0; i < N; ++i)
    {
        Entity e(static_cast<uint32_t>(i + 1), 1);
        manager->AddEntityWith<Position, Velocity>(
            e, Position{float(i), float(i) + 0.5f, float(i) + 0.25f}, Velocity{float(-i), 0.0f, 0.0f});
        ents.push_back(e);
    }
    for (int i = 0; i < N; ++i)
    {
        Position* p = manager->GetComponent<Position>(ents[i]);
        Velocity* v = manager->GetComponent<Velocity>(ents[i]);
        ASSERT_NE(p, nullptr) << "i=" << i; ASSERT_NE(v, nullptr) << "i=" << i;
        EXPECT_FLOAT_EQ(p->x, float(i)); EXPECT_FLOAT_EQ(p->y, float(i) + 0.5f);
        EXPECT_FLOAT_EQ(v->dx, float(-i));
    }
}
```
> Confirm the real `AddEntityWith`/`GetComponent` signatures, `Entity`'s ctor, and `Velocity`'s field names (`dx/dy/dz` per the W6+W2 T3 finding) against the fixture before finalizing. The "present-but-not-provided defaults correctly" path is exercised by the existing transition tests (transitions default-construct the newly-added component) — no separate case needed here.

- [ ] **Step 2: Fast-append in `AddEntityWithComponents`**

In `AddEntityWithComponents` (now `0..columnCount` after Task 2): keep only the "default-construct the NOT-provided columns" pass (already present via the `willBeConstructed` check), then construct provided values. For provided columns, do NOT default-construct first. For a not-provided column whose descriptor `is_trivially_default_constructible`, `DefaultConstruct` already `memset`s (cheap) — leave it. No functional change beyond not double-constructing provided columns (verify the pre-Task-2 code didn't already double-construct; if it did, this removes it).

- [ ] **Step 3: Fast-append in `AddEntity`**

`AddEntity` default-constructs all columns (no values provided) — this is correct as-is for the no-value path. The win: for `is_trivially_default_constructible` columns the `DefaultConstruct` is already a `memset`; the chunk memory is zeroed at creation, so a further optimization (skip the `memset` for freshly-zeroed slots) is possible but risky (slots are reused after swap-and-pop and hold stale bytes — see `DefaultConstruct`'s own comment). **Do NOT skip default-construct for reused slots.** Leave `AddEntity` as the Task-2 form. (This step is a deliberate no-op guard against an unsafe "optimization" — document why in the commit.)

- [ ] **Step 4: Build Debug + run the test + full Debug suite**

Expected: new test PASSES; full suite **Debug 692** (689 baseline + Task 1's 2 + this 1). 

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/ArchetypeChunkPool.hpp tests/
git commit -m "perf(archetype): fast-append avoids double-construct of provided components (W4)"
```

---

### Task 4: Trivial `memcpy` transition move + merge-join + `isComplex` (W3)

The correctness-critical task: add a `memcpy` fast path to the cross-archetype move, gated on trivial-relocatability. **opus**, 3-config. This is where the add/remove win comes from.

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`MoveEntityFrom` — add the fast path)
- Test: `tests/Registry/ArchetypeManagerTest.cpp` (append a move-only/complex correctness guard + a trivial correctness guard)

**Interfaces:**
- Consumes: `ArchetypeColumnMeta::{columns, columnCount, idToColumn, isComplex}`, `ComponentDescriptor::is_trivially_copyable`.

- [ ] **Step 1: Write the correctness-boundary guard tests**

Append to `tests/Registry/ArchetypeManagerTest.cpp`. Two tests. Uses `Astra::Test::Tracked` (`tests/TestComponents.hpp:298`) — a **move-only, non-trivially-copyable** component with a public `int value` and a public `static inline int s_live` that counts live instances (`++s_live` on every ctor, `--s_live` on dtor). `Tracked` is already registered/used in this file, so no new TypeID is consumed.

```cpp
// W3: trivially-copyable components move via memcpy and keep their exact values.
TEST_F(ArchetypeManagerTest, TrivialTransitionMovePreservesValues)
{
    using namespace Astra; using namespace Astra::Test;
    Entity e(2, 1);
    manager->AddEntityWith<Position>(e, Position{3, 4, 5});   // archetype {Position}
    manager->AddComponent<Velocity>(e, Velocity{6, 7, 8});    // transition memcpy's Position across
    Position* p = manager->GetComponent<Position>(e);
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 3.0f); EXPECT_FLOAT_EQ(p->y, 4.0f); EXPECT_FLOAT_EQ(p->z, 5.0f);
    Velocity* v = manager->GetComponent<Velocity>(e);
    ASSERT_NE(v, nullptr); EXPECT_FLOAT_EQ(v->dx, 6.0f);
}

// W3 (LOAD-BEARING): a non-trivially-relocatable component MUST take the MoveConstruct
// path, never memcpy. s_live counts live instances; MoveConstruct does ++s_live but a
// bitwise memcpy does not — so after the source slot is destructed a buggy memcpy leaves
// s_live imbalanced by one. value alone can't catch it (both copy the int); s_live can.
TEST_F(ArchetypeManagerTest, ComplexTransitionMoveUsesMoveConstructNotMemcpy)
{
    using namespace Astra; using namespace Astra::Test;
    const int baseLive = Tracked::s_live;
    Entity e(3, 1);
    manager->AddEntityWith<Tracked>(e, Tracked{42});          // archetype {Tracked}; net +1 live
    ASSERT_EQ(Tracked::s_live, baseLive + 1);

    // Transition {Tracked} -> {Tracked, Position}: MoveConstruct Tracked into the new
    // archetype (++s_live), then the source slot is destructed (--s_live). Net change ZERO.
    manager->AddComponent<Position>(e, Position{1, 2, 3});
    EXPECT_EQ(Tracked::s_live, baseLive + 1) << "memcpy of a move-only type imbalances s_live";
    Tracked* t = manager->GetComponent<Tracked>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->value, 42);                                  // value survives the move

    manager->DestroyEntity(e);                                // destructs the one live Tracked
    EXPECT_EQ(Tracked::s_live, baseLive) << "destroy must return the live count to baseline";
}
```
> Confirm the real `AddEntityWith`/`AddComponent`/`GetComponent`/`DestroyEntity` signatures and `Velocity`'s field names (`dx/dy/dz`) against the fixture before finalizing. Run both — they PASS against the Task-2 per-element `MoveEntityFrom` (which `MoveConstruct`s everything); they are the guard that the Step-2 `memcpy` fast path does not regress `Tracked`. If the `Complex` test fails after Step 2, the `is_trivially_copyable` gate is wrong — a real bug, do not adjust the test.

- [ ] **Step 2: Add the merge-join + `memcpy` fast path to `MoveEntityFrom`**

Replace the Task-2 `MoveEntityFrom` body with a merge-join over the two archetypes' ascending `columns` that memcpys trivially-copyable matched columns and `MoveConstruct`s the rest; default-construct dst-only columns:
```cpp
void MoveEntityFrom(EntityLocation dst, Archetype& srcArchetype, EntityLocation src)
{
    auto& dstChunk = m_chunks[dst.GetChunkIndex()];
    auto& srcChunk = srcArchetype.m_chunks[src.GetChunkIndex()];
    const ArchetypeColumnMeta& dm = m_columnMeta;
    const ArchetypeColumnMeta& sm = srcArchetype.m_columnMeta;
    const size_t di = dst.GetEntityIndex();
    const size_t si = src.GetEntityIndex();

    uint16_t a = 0, b = 0;   // merge-join over dm.columns (ascending) and sm.columns (ascending)
    while (a < dm.columnCount)
    {
        const ComponentID dId = dm.columns[a].id;
        void* dstPtr = dstChunk->GetComponentPointer(dId, di);
        // Advance src past any ids less than the dst id (src-only components are dropped by this move).
        while (b < sm.columnCount && sm.columns[b].id < dId) ++b;
        if (b < sm.columnCount && sm.columns[b].id == dId)   // matched: move src -> dst
        {
            void* srcPtr = srcChunk->GetComponentPointer(dId, si);
            const ComponentDescriptor& desc = *dm.columns[a].descriptor;
            if (desc.is_trivially_copyable) std::memcpy(dstPtr, srcPtr, dm.columns[a].stride);
            else                            desc.MoveConstruct(dstPtr, srcPtr);
            ++b;
        }
        else                                                  // dst-only: default-construct
        {
            dm.columns[a].descriptor->DefaultConstruct(dstPtr);
        }
        ++a;
    }
}
```
Notes: (a) the per-column `is_trivially_copyable` check is the correctness gate — do not replace it with a blanket `memcpy`; (b) `isComplex` can gate a whole-archetype early decision if desired, but the per-column check is sufficient and safe — keep it simple; (c) semantics must match the old code: matched → move, dst-only → default-construct, src-only → dropped (its slot is destructed by the caller's source-slot cleanup, unchanged). Confirm against the old `MoveEntityFrom` + `RemoveEntity`/caller that source-slot destruction is handled by the caller (it was — `MoveEntityFrom` only constructs into dst).

- [ ] **Step 3: Build Debug + run both guard tests + the full suite**

Run: build Debug; `AstraTest.exe --gtest_filter='ArchetypeManagerTest.*TransitionMove*'` then full `AstraTest.exe`. Expected: both guards PASS; full suite green. The complex/move-only guard passing is the proof the `memcpy` gate is correct.

- [ ] **Step 4: Full 3-config build + full suite**

Build Debug/Release/Dist; run each. Expected: all green. Serialization + move-semantics suites especially.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp tests/
git commit -m "perf(archetype): trivial-memcpy transition move via merge-join, gated on is_trivially_copyable (W3)"
```

---

### Task 5: Benchmark, update RESULTS.md + perf plan

**Files:**
- Modify: `bench-compare/RESULTS.md`
- Modify: `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` (mark Phase C done)

- [ ] **Step 1: Rebuild `bench_astra` + run all three**

Public API unchanged, so `bench_astra.cpp` compiles as-is:
`cmd //c '"D:\dev\starworks\Astra\bench-compare\build_one.bat" /std:c++20 /O2 /DNDEBUG /EHsc /I..\include /I..\vendor\Mosaic\include bench_astra.cpp advapi32.lib'`
Run `./bench_astra.exe`, `./bench_entt.exe`, `./bench_flecs.exe`. If the machine is noisy (as in W6+W2), use the same-session A/B method: build a second `bench_astra` from the pre-Phase-C commit (`a8712d6`) in a temp `git worktree`, interleave ≥6 runs, report medians of the paired deltas.

- [ ] **Step 2: Update RESULTS.md — honestly**

Update the Astra row(s). Expected direction: **add/remove move materially** (W3 memcpy move — the primary target, e.g. toward ~90-100 add / ~55-65 remove); iterate2 may improve (W5 cache density); create/random_get roughly flat-to-slightly-better (small post-W1 headroom — W4/W7). **Be honest** — if add/remove didn't move, verify `MoveEntityFrom` actually took the `memcpy` path for trivial components (not a leftover per-element path). Note the residual gap to flecs.

- [ ] **Step 3: Mark Phase C done in the perf plan**

In `docs/reviews/2026-07-21-astra-perf-optimization-plan.md` §3/§7, mark W5/W7/W4/W3 landed with the measured numbers; note next = Phase D (command-buffer batch path + change-detection — roadmap features, not benchmark movers).

- [ ] **Step 4: Commit**

```bash
git add bench-compare/RESULTS.md docs/reviews/2026-07-21-astra-perf-optimization-plan.md
git commit -m "perf(bench): record Phase C results (chunk storage modernization W5+W7+W4+W3)"
```

---

## Finish (SDD close-out)
After all tasks pass in Debug/Release/Dist, run an authoritative 3-config build+full-suite on the final commit, then fast-forward-merge `perf/phase-c-chunk-storage` into `dev` locally, delete the branch, do not push. Confirm with the user before merging. (Controller updates the perf memory + the W4/W7-measured-vs-predicted honesty note.)

---

## Self-review — spec coverage
- W5 metadata-once (`ArchetypeColumnMeta`, shared descriptor ptr, drop dup vector) → Task 1 + Task 2. ✅
- W5 packed columns + `0..N` hot loops → Task 2 Steps 1-6. ✅
- W7 compact `idToColumn` get → Task 2 Step 4 (coupled with W5 — the 128-slot array is gone, so get MUST resolve via idToColumn). ✅
- Tags excluded from columns, `idToColumn=-1`, mask carries presence → Task 1 Step 4 (`BuildColumnMeta` skips `size==0`) + Task 2 (no `base==nullptr` columns). ✅
- W4 fast-append (no double-construct of provided; safe re: reused slots) → Task 3. ✅
- W3 trivial memcpy move + merge-join + `is_trivially_copyable` correctness gate → Task 4. ✅
- Correctness boundary: move-only/complex type never memcpy'd → Task 4 Step 1 load-bearing guard + Step 2 gate. ✅
- No on-disk format change (chunk metadata runtime-only) → serialization suite gate in Task 2 Step 8. ✅
- No new files / no premake regen → all tasks edit existing headers + tests. ✅
- 3-config verification → Task 2 Step 8, Task 4 Step 4, Finish. ✅
- Benchmark + honest W4/W7 measurement → Task 5. ✅
