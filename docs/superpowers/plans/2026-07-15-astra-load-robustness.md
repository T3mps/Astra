# Registry::Load Robustness Floor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `Registry::Load` fail cleanly (`Err`) on any truncated / bit-rotted / version-skewed / inconsistent save instead of corrupting the heap, terminating on an unbounded allocation, or hanging.

**Architecture:** Six focused tasks. Task 1 adds a reusable "bound a length by the bytes that remain" primitive to `BinaryReader` and fixes two reader-local length bugs. Tasks 2–5 apply that primitive and add structural bounds checks at each `Deserialize` site (Archetype chunk data, entity-map indices, entity/relationship counts, and the relationship cycle-hang). Task 6 adds an offset-agnostic corruption/truncation sweep as the integration net plus the trust-boundary doc comment. Every check is *check-before-allocate* — the shipping build is exception-free, so a thrown `bad_alloc` is `std::terminate`, not a catchable error.

**Tech Stack:** Header-only C++20; GoogleTest; MSVC-primary (CI also builds Linux gcc/clang); premake5-generated `Astra.sln`.

**Spec:** `docs/superpowers/specs/2026-07-15-astra-load-robustness-design.md` (approved).

## Global Constraints

- Header-only C++20; MSVC-primary; CI also builds Linux gcc/clang.
- **Exception-free & RTTI-off in shipping** → *check-before-allocate*; never `try`/`catch` an allocation. A robustness check that must hold in Release/Dist is a real `if (…) { set error; return; }`, **never** an `ASTRA_ASSERT` (asserts compile out unless `ASTRA_ENABLE_ASSERTS`).
- No public API change; no new dependency; reuse `SerializationError::CorruptedData` / `SizeMismatch` (add a variant only if it materially helps a caller — default is reuse).
- Zero behavior change on valid saves: every check rejects already-malformed input; a well-formed save takes an identical path.
- Namespace `Astra`.
- Build via MSBuild on the **whole solution** — `-t:AstraTest` does NOT work. A **new** test file requires `D:\dev\_shared\tools\premake5 vs2022` regen; appending to an existing test file does not. Never `git add` `ide/`, `Astra.sln`, `Makefile`, `*.make`.
- Baseline suite: **585 Debug / 583 Release+Dist** (dev @ `e56d8fd`). IDE/clang-tidy diagnostics are misconfigured false positives — judge only by the MSVC build.
- All file:line anchors below are from the recon at branch point; **confirm each against the live tree before editing** (line numbers drift).

**Build command (per config):**
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=Debug -p:Platform=x64 -m
```
**Run tests:** `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=<Suite>.*`

---

### Task 1: BinaryReader safety primitives + reader-local length fixes

**Files:**
- Modify: `include/Astra/Serialization/BinaryReader.hpp` (public methods near the other read methods; the vector `operator()` ~:293-329; `ReadCompressedBlock` ~:185-246)
- Create: `tests/Serialization/LoadRobustnessTest.cpp` (new file → premake regen this task)

**Interfaces:**
- Produces (consumed by Tasks 2–5):
  - `size_t BinaryReader::Remaining() const noexcept` — bytes not yet consumed.
  - `uint64_t BinaryReader::ReadBoundedCount(size_t minBytesPerElement)` — reads a `uint64` count; if it exceeds `Remaining()/minBytesPerElement`, sets `m_error = CorruptedData` and returns 0. Caller checks `HasError()`.

- [ ] **Step 1: Create the new test file with a byte-appender helper and the reader tests (they will not compile/link yet — RED).**

Create `tests/Serialization/LoadRobustnessTest.cpp`:
```cpp
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include "Astra/Serialization/BinaryReader.hpp"

namespace
{
    // Little-endian append helpers for hand-built reader buffers.
    void AppendU64(std::vector<std::byte>& b, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    }
    void AppendBytes(std::vector<std::byte>& b, size_t n, std::byte fill = std::byte{0})
    {
        b.insert(b.end(), n, fill);
    }
}

// ReadBoundedCount rejects a count larger than the remaining buffer could hold.
TEST(LoadRobustness, ReadBoundedCountRejectsOversizedCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 1'000'000'000ull);   // claims a billion elements...
    // ...but nothing follows, so Remaining() after the count is 0.
    Astra::BinaryReader reader(std::span<const std::byte>(buf));
    const uint64_t n = reader.ReadBoundedCount(8);
    EXPECT_TRUE(reader.HasError());
    EXPECT_EQ(*reader.GetError() == Astra::SerializationError::CorruptedData, true);
    EXPECT_EQ(n, 0u);
}

// ReadBoundedCount accepts a count the remaining buffer can justify.
TEST(LoadRobustness, ReadBoundedCountAcceptsFeasibleCount)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 2);                   // 2 elements...
    AppendBytes(buf, 16);                // ...16 bytes follow (8 each)
    Astra::BinaryReader reader(std::span<const std::byte>(buf));
    const uint64_t n = reader.ReadBoundedCount(8);
    EXPECT_FALSE(reader.HasError());
    EXPECT_EQ(n, 2u);
}

// The POD-vector read must not integer-overflow its bounds check (size * sizeof(T)).
TEST(LoadRobustness, VectorReadRejectsOverflowingSize)
{
    std::vector<std::byte> buf;
    AppendU64(buf, 0x2000000000000000ull); // 2^61; *8 wraps to 0 in the buggy check
    Astra::BinaryReader reader(std::span<const std::byte>(buf));
    std::vector<uint64_t> v;
    reader(v);                              // must set error, NOT resize(2^61)
    EXPECT_TRUE(reader.HasError());
}
```

- [ ] **Step 2: Regenerate the solution and run — verify RED.**

Run: `D:\dev\_shared\tools\premake5 vs2022` then build Debug and run
`bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=LoadRobustness.*`
Expected: compile error (`ReadBoundedCount`/`Remaining` undeclared), OR after a stub, `VectorReadRejectsOverflowingSize` crashes/aborts on `bad_alloc`. That crash IS the bug this task fixes.

- [ ] **Step 3: Add `Remaining()` and `ReadBoundedCount()` to `BinaryReader` (public, near the other read methods).**
```cpp
// Bytes not yet consumed. Invariant m_position <= m_size (ReadBytes enforces it) → no underflow.
[[nodiscard]] size_t Remaining() const noexcept { return m_size - m_position; }

// Read a uint64 element-count, rejecting it if it exceeds what the remaining buffer could hold:
// N elements need at least N*minBytesPerElement bytes downstream, so N > Remaining()/per cannot be
// real. Bounds every reserve/loop against input size (the standard "length <= remaining" rule) and
// doubles as a truncation detector. On rejection: m_error = CorruptedData, returns 0.
[[nodiscard]] uint64_t ReadBoundedCount(size_t minBytesPerElement)
{
    uint64_t count = 0;
    (*this)(count);
    if (HasError()) return 0;
    const size_t per = (minBytesPerElement == 0) ? 1 : minBytesPerElement;
    if (count > static_cast<uint64_t>(Remaining() / per))
    {
        m_error = SerializationError::CorruptedData;
        return 0;
    }
    return count;
}
```

- [ ] **Step 4: Fix the `size * sizeof(T)` integer overflow in the POD vector read (~:304).**

Replace:
```cpp
                if (size * sizeof(T) > (m_size - m_position))
```
with (overflow-safe — divide, don't multiply; `sizeof(T) >= 1` for a complete type):
```cpp
                if (size > (m_size - m_position) / sizeof(T))
```

- [ ] **Step 5: Guard the uncompressed-block pre-allocation in `ReadCompressedBlock` (~:206).**

Immediately before `std::vector<uint8_t> data(originalSize);` insert:
```cpp
                if (originalSize > Remaining())
                {
                    m_error = SerializationError::CorruptedData;
                    return Result<std::vector<uint8_t>, SerializationError>::Err(SerializationError::CorruptedData);
                }
```

- [ ] **Step 6: Build all three configs and run — verify GREEN + no regressions.**

Build Debug/Release/Dist; run each `AstraTest.exe`. Expected: `LoadRobustness.*` pass (3 tests); full suite = **588 Debug / 586 Release+Dist** (585/583 + 3). Output pristine.

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Serialization/BinaryReader.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "feat(serialization): bounded-count reader primitive + length-safety fixes"
```

---

### Task 2: Archetype chunk-data bounds (heap-overflow fix)

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`Deserialize`: `descriptorCount` ~:775-777; `chunkCount` loop ~:844; per-chunk `chunkEntityCount` ~:847; `arraySize`/memcpy ~:881,:927; custom path ~:889-901). `entitiesPerChunk` is already validated ~:809-827.
- Modify: `tests/Serialization/LoadRobustnessTest.cpp` (add test)

**Interfaces:**
- Consumes: `BinaryReader::ReadBoundedCount` (Task 1).

- [ ] **Step 1: Write the failing test — a chunk claiming more entities than its capacity.**

Add to `LoadRobustnessTest.cpp` (include `Astra/Registry/Registry.hpp` and `../TestComponents.hpp` at the top if not present):
```cpp
#include "Astra/Registry/Registry.hpp"
#include "../TestComponents.hpp"

// A save whose per-chunk entity count exceeds the chunk capacity must be rejected,
// not memcpy'd past the end of the heap chunk. The implementer locates the
// chunkEntityCount field by mirroring Archetype::Serialize's write order (it is the
// per-chunk count written just before the chunk's component blocks) and overwrites it
// with a value > entitiesPerChunk; a clean, deterministic corruption.
TEST(LoadRobustness, ChunkEntityCountOverCapacityIsRejected)
{
    using namespace Astra::Test;
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity>();
    reg.CreateEntityWith(Position{1,2,3}, Velocity{4,5,6});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    std::vector<std::byte> buf = std::move(*saved.GetValue());

    // Overwrite the (little-endian) chunkEntityCount field with a huge value.
    // Offset is computed from Archetype::Serialize's write order — see that function.
    const size_t chunkCountOffset = /* implementer: computed from Serialize order */ 0;
    ASSERT_NE(chunkCountOffset, 0u) << "compute the offset from Archetype::Serialize";
    for (int i = 0; i < 4; ++i)
        buf[chunkCountOffset + i] = std::byte{0xFF};

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity>();
    auto loaded = Astra::Registry::Load(buf, cr);
    EXPECT_TRUE(loaded.IsErr());   // must fail cleanly — no crash, no OOB
}
```
> Note to implementer: if computing a stable offset proves brittle, instead assert the guard at the source by unit-testing `Archetype::Deserialize` behavior, or fold this case into the Task 6 sweep and keep here only the capacity-guard code with a comment. The **required** deliverable is the production guard in Step 3; the test must exercise it without relying on a fragile magic offset.

- [ ] **Step 2: Run — verify RED** (crash/OOB or a wrong `IsOk`).
Run: `AstraTest.exe --gtest_filter=LoadRobustness.ChunkEntityCountOverCapacityIsRejected`

- [ ] **Step 3: Add the capacity guard in `Archetype::Deserialize`.**

Immediately after `chunkEntityCount` is read (and after `entitiesPerChunk` is known/validated), before the `AddEntity` loop / `arraySize` computation / any `memcpy`:
```cpp
            if (chunkEntityCount > entitiesPerChunk)
            {
                return Err(SerializationError::CorruptedData);   // match the function's existing Err form
            }
```
(Use whatever the function's existing failure form is — the recon shows it already returns `Err(SizeMismatch)` at ~:825; mirror that style. Confirm the `entitiesPerChunk` variable name in situ.)

- [ ] **Step 4: Bound the count reads in the same function.**

Replace the raw reads of `descriptorCount` and the chunk count with `ReadBoundedCount`, using each element's minimum on-disk size (the fixed prefix each descriptor / chunk writes — read `Serialize` to determine it; a safe conservative floor is the sum of the fixed-width fields, never below 1):
```cpp
            const uint64_t descriptorCount = reader.ReadBoundedCount(/*minBytesPerDescriptor*/ 16);
            if (reader.HasError()) return Err(SerializationError::CorruptedData);
```
Do the same for the chunk-count read that drives the chunk loop.

- [ ] **Step 5: Run — verify GREEN.** The corruption test now returns `Err`; existing serialization round-trip tests still pass.

- [ ] **Step 6: Build all three configs; full suite green.**

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Archetype/Archetype.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "fix(archetype): bound chunkEntityCount by capacity and counts by buffer on load"
```

---

### Task 3: ArchetypeManager entity-map index validation

**Files:**
- Modify: `include/Astra/Archetype/ArchetypeManager.hpp` (`Deserialize`: `archetypeCount`/`entityCount` ~:800; entity-map loop ~:871; the `(entity, archetypeIndex, chunkIndex, entityIndex)` read ~:878; the `archetypeIndex < size()` branch ~:883 and the raw store ~:887)
- Modify: `tests/Serialization/LoadRobustnessTest.cpp`

**Interfaces:**
- Consumes: `BinaryReader::ReadBoundedCount` (Task 1).

- [ ] **Step 1: Write the failing test — an entity-map record with an out-of-range `chunkIndex`.**

Add a test that saves a one-entity registry, overwrites the `chunkIndex` field of the sole entity-map record with `0xFFFFFFFF` (offset mirrored from `ArchetypeManager::Serialize`), then asserts `Load` returns `Err` (today it stores the bad index and a later access is OOB). Same offset caveat/fallback as Task 2 Step 1.

- [ ] **Step 2: Run — verify RED.**

- [ ] **Step 3: Validate the indices before storing the record (~:883-887).**

By the entity-map loop, archetypes and their chunks already exist. Replace the "check archetypeIndex, else silently drop" logic with full validation that returns `Err` on any out-of-range index:
```cpp
            if (archetypeIndex >= m_archetypes.size())
                return false;   // match Deserialize's existing bool-failure convention (maps to CorruptedData)
            Archetype* arch = m_archetypes[archetypeIndex].archetype.get();
            if (chunkIndex >= arch->GetChunkCount() ||
                entityIndex >= arch->GetChunkEntityCount(chunkIndex))
                return false;
            // ...then store the validated location as before.
```
(Confirm the accessors' real names — `GetChunkCount()` / per-chunk entity count. If none exist, add minimal `const` accessors on `Archetype`. Match `Deserialize`'s existing return convention — the recon shows it returns `bool` mapped to `CorruptedData` by the caller.)

- [ ] **Step 4: Bound `archetypeCount` and `entityCount` with `ReadBoundedCount`** (min bytes = the per-archetype and per-entity-map-record fixed footprint), each followed by `if (reader.HasError()) return false;`.

- [ ] **Step 5: Run — verify GREEN** (corruption → `Err`; round-trip tests pass).

- [ ] **Step 6: Build all three configs; full suite green.**

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Archetype/ArchetypeManager.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "fix(archetype): validate entity-map chunk/entity indices on load"
```

---

### Task 4: EntityManager + RelationshipGraph count bounds

**Files:**
- Modify: `include/Astra/Entity/EntityManager.hpp` (`Deserialize`: `recycledCount` ~:356-358; alive-count loop ~:386)
- Modify: `include/Astra/Registry/RelationshipGraph.hpp` (`Deserialize`: counts ~:462, :489, :506, :531, :548)
- Modify: `tests/Serialization/LoadRobustnessTest.cpp`

**Interfaces:**
- Consumes: `BinaryReader::ReadBoundedCount` (Task 1).

- [ ] **Step 1: Write the failing test — a garbage recycled/relationship count.**

Save a registry that has a recycled entity and a parent/child relationship; overwrite one count field with `0xFFFFFFFF`; assert `Load` returns `Err` rather than terminating on a multi-GB `reserve`. (Offset caveat/fallback as before; the Task 6 sweep is the backstop.)

- [ ] **Step 2: Run — verify RED** (terminate on `bad_alloc`, or wrong `IsOk`).

- [ ] **Step 3: Replace each raw count read with `ReadBoundedCount`** in both files, min-bytes = the fixed per-element footprint, each followed by an `if (reader.HasError()) return …;` in the function's existing failure style. Sites: `recycledCount`, the alive-count, and the five relationship counts (parent/child/link).

- [ ] **Step 4: Run — verify GREEN.**

- [ ] **Step 5: Build all three configs; full suite green.**

- [ ] **Step 6: Commit.**
```bash
git add include/Astra/Entity/EntityManager.hpp include/Astra/Registry/RelationshipGraph.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "fix(serialization): bound entity/relationship counts by buffer on load"
```

---

### Task 5: RelationshipGraph cycle-safe ancestor walk (hang fix)

**Files:**
- Modify: `include/Astra/Registry/RelationshipGraph.hpp` (`IsAncestorOf` ~:135-145; audit sibling `GetParent`-walks)
- Modify: `tests/Serialization/LoadRobustnessTest.cpp`

- [ ] **Step 1: Write the failing test — a cyclic parent map must not hang the traversal.**

A crafted save with a cyclic parent map (A→B, B→A) loads (the load path writes `m_parents` directly, no cycle check), and then `IsAncestorOf` must terminate. Build the cyclic buffer by mirroring `RelationshipGraph::Serialize` for two entities, `Load`, then call the API that reaches `IsAncestorOf` (e.g. `SetParent`) or `IsAncestorOf` directly if reachable in test, and assert it returns (no hang). Guard against a hang in the test with a bounded expectation, e.g. run the call and assert it completes and returns a defined bool.
```cpp
// Sketch — implementer mirrors RelationshipGraph::Serialize to craft the cyclic buffer.
TEST(LoadRobustness, CyclicParentMapDoesNotHang)
{
    // ... craft buffer with m_parents = { A->B, B->A } ...
    // auto loaded = Astra::Registry::Load(buf, cr);   // loads the cyclic map
    // bool r = loaded->...IsAncestorOf(A, B);          // must RETURN, not spin
    // SUCCEED();  // reaching here proves no infinite loop
}
```
> If crafting the cyclic buffer is impractical, test `IsAncestorOf`/`RelationshipGraph` directly by deserializing a hand-built relationship block, or add a test-only constructor path. The **required** deliverable is that the traversal is cycle-safe.

- [ ] **Step 2: Run — verify RED.** Before the fix this test **hangs** — run it with a short timeout; a hang is the RED. (Do not leave a hanging run unattended; kill and record it.)

- [ ] **Step 3: Make `IsAncestorOf` cycle-safe.**

Add a visited guard (or a step cap bounded by the parent-map size) so the walk terminates on a cycle, returning a defined result:
```cpp
        [[nodiscard]] bool IsAncestorOf(Entity ancestor, Entity descendant) const
        {
            Entity current = GetParent(descendant);
            size_t steps = 0;
            const size_t limit = m_parents.Size();   // a valid chain can't exceed the map size
            while (current.IsValid())
            {
                if (current == ancestor) return true;
                if (++steps > limit) return false;    // cycle → bail, defined result
                current = GetParent(current);
            }
            return false;
        }
```
(Confirm `m_parents.Size()`/accessor name in situ. A `std::unordered_set<Entity> visited` guard is an acceptable alternative if preferred for clarity.)

- [ ] **Step 4: Audit and guard sibling walks.** Grep the file for other `while (… IsValid()) … = GetParent(…)` (or descendant) walks lacking a visited/step guard; apply the same guard. Note in the commit which were touched.

- [ ] **Step 5: Run — verify GREEN** (test returns; no hang). Existing relationship tests pass.

- [ ] **Step 6: Build all three configs; full suite green.**

- [ ] **Step 7: Commit.**
```bash
git add include/Astra/Registry/RelationshipGraph.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "fix(relationships): make IsAncestorOf cycle-safe so a corrupt parent map can't hang"
```

---

### Task 6: Corruption/truncation sweep + trust-boundary documentation

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (doc comment on `Load`)
- Modify: `tests/Serialization/LoadRobustnessTest.cpp` (the sweep)

- [ ] **Step 1: Write the integration sweep.**

Save a *rich* registry (several entities, multiple component types, a parent/child relationship, a recycled entity), then:
```cpp
TEST(LoadRobustness, TruncationSweepNeverCrashes)
{
    using namespace Astra::Test;
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity, Health, Name>();
    auto a = reg.CreateEntityWith(Position{1,2,3}, Velocity{4,5,6});
    auto b = reg.CreateEntityWith(Health{100}, Name{"child"});
    reg.SetParent(b, a);
    reg.DestroyEntity(reg.CreateEntity());   // create a recycled slot
    auto saved = reg.Save(); ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity, Health, Name>();

    // Every truncation prefix must return Ok or Err — never crash/OOB/hang.
    for (size_t len = 0; len <= full.size(); ++len)
    {
        std::vector<std::byte> t(full.begin(), full.begin() + len);
        auto r = Astra::Registry::Load(t, cr);
        (void)r;   // reaching the next iteration is the assertion
    }
    SUCCEED();
}

TEST(LoadRobustness, ByteCorruptionSweepNeverCrashes)
{
    using namespace Astra::Test;
    Astra::Registry reg;
    reg.GetComponentRegistry()->RegisterComponents<Position, Velocity, Health, Name>();
    reg.CreateEntityWith(Position{1,2,3}, Velocity{4,5,6});
    reg.CreateEntityWith(Health{9}, Name{"x"});
    auto saved = reg.Save(); ASSERT_TRUE(saved.IsOk());
    const std::vector<std::byte> full = std::move(*saved.GetValue());

    auto cr = std::make_shared<Astra::ComponentRegistry>();
    cr->RegisterComponents<Position, Velocity, Health, Name>();

    // Overwrite each aligned 4-byte window with 0xFFFFFFFF and with 0x00000000.
    for (size_t off = 0; off + 4 <= full.size(); off += 4)
    {
        for (std::byte fill : { std::byte{0xFF}, std::byte{0x00} })
        {
            std::vector<std::byte> c = full;
            for (int i = 0; i < 4; ++i) c[off + i] = fill;
            auto r = Astra::Registry::Load(c, cr);
            (void)r;
        }
    }
    SUCCEED();
}
```
This is the offset-agnostic proof of the whole floor: it exercises every buffer-derived bound and every OOB guard without fragile field offsets. Run it under Debug (an OOB would abort under ASan on the Linux leg; on MSVC an OOB/terminate fails the run).

- [ ] **Step 2: Run — verify GREEN across the sweep.**

Run: `AstraTest.exe --gtest_filter=LoadRobustness.*`. Both sweeps pass (no crash/hang). If any offset crashes/hangs, that is an uncovered hole — fix it in the owning task's file before proceeding. (RED evidence for the sweep: on `dev` / with any Task 1–5 fix reverted, a truncation or corruption offset crashes or hangs — note this rather than committing a red state.)

- [ ] **Step 3: Add the trust-boundary doc comment on `Registry::Load`.**

Above the `Load` overloads (~:1476), add:
```cpp
        // Load validates structural integrity enough to fail cleanly -- returning
        // Err(SerializationError) -- on truncated, corrupt, or version-skewed input, without
        // out-of-bounds access, unbounded allocation, or hangs. It is NOT a security boundary:
        // it does not guarantee rejection of every maliciously-crafted archive, and callers must
        // not load saves from untrusted sources without their own validation.
```

- [ ] **Step 4: Build all three configs; full suite green.** Expected total after all tasks: **≈590 Debug / 588 Release+Dist** (585/583 + the new `LoadRobustness` tests — report the actual count; the death-test delta of 2 between Debug and Release still holds).

- [ ] **Step 5: Commit.**
```bash
git add include/Astra/Registry/Registry.hpp tests/Serialization/LoadRobustnessTest.cpp
git commit -m "test(serialization): corruption/truncation sweep; document the Load trust boundary"
```

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §4 Group 1a→Task 2; 1b→Task 3; Group 2 primitive/2c/2d→Task 1, 2b sites→Tasks 2/3/4; Group 3→Task 5; §7 sweep→Task 6; §9 doc→Task 6. All covered.
- **No placeholders in production code:** every production change has literal code. The two *test* offsets left to the implementer (Task 2/3 field offsets) are explicitly bounded work with a stated fallback (the Task 6 sweep), not silent TODOs — the memory-safety property is proven regardless by Task 6.
- **Type consistency:** `ReadBoundedCount(size_t)`/`Remaining()` defined in Task 1 are used with those exact signatures in Tasks 2–5; `Err(...)`/`return false` failure forms are flagged "match the function's existing convention" since Deserialize methods differ (some return `bool`, some `Result`).
- **Exception-free honored:** every guard is `if (…) { error; return; }`, no `try`/`catch`, no assert used as a shipping check.
