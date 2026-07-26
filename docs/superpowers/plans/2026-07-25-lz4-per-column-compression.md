# LZ4 Per-Column Compression Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `SaveConfig::compressionMode = LZ4` actually compress a saved world, via per-column block compression that is orthogonal to per-element versioning (covers all component types), reusing the already-fixed codec.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-25-lz4-per-column-compression-design.md`. In `Archetype::Serialize`/`Deserialize`, factor each chunk-column's encode/decode (per-element data + IM-9 disabled-bit section) into shared helpers. In LZ4 mode, serialize a column into an in-memory sub-`BinaryWriter` buffer and `WriteCompressedBlock` it; on read, `ReadCompressedBlock` then run the same decode over an in-memory sub-`BinaryReader`. `None` mode stays byte-identical to today. Bump the on-disk format to v5; the header's `compressionMode` + version discriminate the read path.

**Tech Stack:** C++20 header-only, GoogleTest, MSBuild (Windows), the existing `BinaryWriter`/`BinaryReader`/`Compression` machinery.

## Global Constraints

- **Base branch:** off `fix/tier0-correctness-batch` @ `a2fbe3a` (it carries the codec fixes C3/I11/IM-18 this feature depends on) — or off `dev` once that branch is merged in. Record the actual base SHA in the ledger.
- **Baseline (must stay green every task):** AstraTest **Debug 771 / Release 769 / Dist 769**, all pass. Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Debug|Release|Dist> -p:Platform=x64 -m` (regenerate first with `D:\dev\_shared\tools\premake5.exe vs2022`). Test exe: `bin\<Config>-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`. IDE clang diagnostics for this project are FALSE POSITIVES — judge only by the MSBuild result.
- **Orthogonal to versioning (binding):** compression wraps the *serialized* column bytes (the existing `serializeVersioned`/`serialize` per-element output). Do NOT change the per-element encoding, and do NOT special-case trivially-copyable types by bypassing versioning. The old `else if (desc.is_trivially_copyable) → WriteCompressedBlock(rawArray)` branch (a versioning bypass) is REMOVED.
- **`None` mode unchanged (binding):** when `m_compressionMode != CompressionMode::LZ4`, the write path must produce byte-identical output to the pre-change v4 `None` layout (modulo the version field), taking no sub-buffer and no block framing.
- **Nested sub-writer/sub-reader run with checksum disabled** (`SetChecksumEnabled(false)`) — a column sub-buffer has no `BinaryHeader`; its integrity is covered by the outer stream's checksum. Write and read must use the same setting.
- **Codec is frozen:** no changes to `WriteCompressedBlock`/`ReadCompressedBlock`/`Compression::*`. This plan only wires them in.
- **Model recipe (SDD):** Task 1 sonnet; Task 2 sonnet (mechanical extraction); **Task 3 OPUS** (format-critical, nested writers, remove dead branches); Task 4 sonnet. Final whole-branch review OPUS.

---

### Task 1: Bump on-disk format to v5

**Files:**
- Modify: `include/Astra/Serialization/BinaryArchive.hpp:92` (`BINARY_FORMAT_VERSION`)
- Test: `tests/Serialization/BinarySerializationVersioningTests.cpp` (and any test asserting the literal version)

**Interfaces:**
- Consumes: nothing.
- Produces: `BinaryArchive::BINARY_FORMAT_VERSION == 5`. Every save now writes version 5; reads accept 5 and all legacy versions ≤ 4.

- [ ] **Step 1: Find every place the format version literal is asserted or hard-required.**

Run (Bash):
```bash
grep -rn "BINARY_FORMAT_VERSION\|GetVersion()\|== 4\|version == 4\|formatVersion" include/Astra/Serialization include/Astra/Archetype tests | grep -iv "//"
```
Expected: the definition at `BinaryArchive.hpp:92`, the IM-9 `GetVersion() >= 4` gates in `Archetype.hpp`, any header-validation compare, and any test that asserts the version number. Note each — the read path must accept BOTH 4 and 5.

- [ ] **Step 2: Confirm the reader accepts a version RANGE, not an exact match.**

Read the header-validation logic (search result from Step 1, e.g. in `BinaryReader`/`Archetype::Deserialize` header read). It must reject `version > BINARY_FORMAT_VERSION` and `version == 0`, but ACCEPT any `1..BINARY_FORMAT_VERSION`. If it currently hard-compares `== 4`, change it to `<= BINARY_FORMAT_VERSION` (with the existing legacy handling preserved). If it already uses `<=`/a range, no change.

**Cross-version coverage (spec §6):** "a v4 file loads under the v5 reader" is satisfied by this range-accept plus the existing legacy-version tests (e.g. `FormatV2Test`, `BinarySerializationVersioningTests`), which already prove older-version archives load. Do NOT try to synthesize a v4 binary fixture (the code no longer emits v4) unless the repo already has a committed version-fixture harness; if it does, add a v4-fixture load case there.

- [ ] **Step 3: Bump the constant.**

In `include/Astra/Serialization/BinaryArchive.hpp:92`, change `BINARY_FORMAT_VERSION` from `4` to `5`.

- [ ] **Step 4: Update version-asserting tests.**

For each test flagged in Step 1 that asserts the numeric version, update the expected value to `5` for freshly-written archives. Do NOT change any test that deliberately loads a fixed older-version fixture.

- [ ] **Step 5: Build + test all three configs.**

Run (PowerShell): regenerate + build Debug/Release/Dist, run `AstraTest --gtest_brief=1` each.
Expected: **771 / 769 / 769**, all pass. (v5 saves round-trip as v5; the IM-9 `>= 4` gates still fire for v5; legacy loads unaffected.)

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Serialization/BinaryArchive.hpp tests
git commit -m "feat(serialization): bump BINARY_FORMAT_VERSION 4->5 for the compressed-column layout"
```

---

### Task 2: Extract shared column encode/decode helpers (no behavior change)

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (`Serialize` column loop ~932-994; `Deserialize` column loop ~1266-1392; add two private helpers)

**Interfaces:**
- Consumes: `m_componentDescriptors` (element type is the struct with `.id`, `.size`, `.serializeVersioned`, `.serialize`, `.is_trivially_copyable`, `.isEnableable` — hereafter `Desc`), `Chunk*`, `m_columnMeta.idToColumn`.
- Produces two private member functions on `Archetype`:
  - `void SerializeColumn(BinaryWriter& w, Chunk* chunk, const Desc& desc, size_t chunkEntityCount) const;`
    — writes this column's per-element serialized data, then (iff `desc.isEnableable`) the disabled-bit section (`disabledCount` + words), exactly as the current inline loop does. Assumes `chunk->GetComponentArrayByID(desc.id)` is non-null (caller skips null/tag columns).
  - `ResultType DeserializeColumn(BinaryReader& r, Chunk* chunk, Archetype* archetype, const Desc& desc, size_t chunkEntityCount, bool hasDisabledSection);`
    — reads this column's per-element data into the chunk's component array, then (iff `hasDisabledSection`) reads+validates the disabled section and applies it iff `desc.isEnableable` (dropping it otherwise), exactly as the current inline logic does. Returns `ResultType::Err(...)` on corruption; `ResultType::Ok()`-equivalent (whatever the loop uses to signal per-column success) otherwise.

- [ ] **Step 1: Add `SerializeColumn`** as a private member of `Archetype`. Move the body of the current per-column write — the `if (desc.serializeVersioned || desc.serialize) {...} else if (desc.is_trivially_copyable) {...} else {...}` block AND the `if (desc.isEnableable) { disabled section }` block (currently `Archetype.hpp:939-994`) — into it, VERBATIM, writing to the passed `w` instead of the outer `writer`. Keep the `else if (is_trivially_copyable) → WriteCompressedBlock` branch here for now (Task 3 removes it); this task is a pure move.

- [ ] **Step 2: Replace the Serialize call site** (`Archetype.hpp` ~939-994) with:

```cpp
                    // (componentArray null / tag columns already skipped by the
                    //  `if (!componentArray) continue;` above)
                    SerializeColumn(writer, chunk.get(), desc, chunkEntityCount);
```
(Keep the surrounding chunk loop, `GetComponentArrayByID` null-skip, and `arraySize` computation as they are — `SerializeColumn` recomputes what it needs.)

- [ ] **Step 3: Add `DeserializeColumn`** as a private member. Move the current per-column read — the component-data read branch (`Archetype.hpp` ~1266-1315) AND the disabled-section read/validate/apply block (`~1317-1392`) — into it VERBATIM, reading from the passed `r`, using the passed `hasDisabledSection` in place of the inline `diskHasDisabledSection[di]`, and returning `ResultType::Err(...)` where the inline code does. Keep the `ReadCompressedBlock` branch for now (Task 3 removes it).

- [ ] **Step 4: Replace the Deserialize call site** with a call passing the already-read flag:

```cpp
                    auto colResult = DeserializeColumn(reader, chunk, archetype.get(), desc,
                                                       static_cast<size_t>(chunkEntityCount),
                                                       diskHasDisabledSection[di]);
                    if (colResult.IsErr())
                        return colResult;
```
(Preserve the existing `di` index and `diskHasDisabledSection` vector from the IM-9 code; only the per-column body moves.)

- [ ] **Step 5: Build + test all three configs.**
Expected: **771 / 769 / 769**, all pass — this is a pure refactor, so any diff in behavior is a bug. If serialization tests fail, the extraction changed byte layout — revert and re-extract more carefully.

- [ ] **Step 6: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp
git commit -m "refactor(archetype): extract SerializeColumn/DeserializeColumn helpers (no behavior change)"
```

---

### Task 3: Compressed column write + read path (the core)

**Files:**
- Modify: `include/Astra/Archetype/Archetype.hpp` (Serialize + Deserialize column call sites; the two helpers' internals for the removed dead branch)
- Test: `tests/Registry/RegistrySerializationTest.cpp` (a new `>4MB` LZ4 round-trip test)

**Interfaces:**
- Consumes: Task 2's `SerializeColumn`/`DeserializeColumn`; `BinaryWriter(std::vector<std::byte>&)` (memory ctor, `BinaryWriter.hpp:87`); `BinaryReader(std::span<const std::byte>)` (memory ctor, `BinaryReader.hpp:66`); `writer.WriteCompressedBlock(const void*, size_t)` (`BinaryWriter.hpp:224`); `reader.ReadCompressedBlock() → Result<std::vector<uint8_t>, SerializationError>` (`BinaryReader.hpp:186`); `SetChecksumEnabled(bool)` (`BinaryWriter.hpp:567`, `BinaryReader.hpp:835`); `writer`/`reader` expose the active `CompressionMode` (see Step 1).
- Produces: LZ4-mode saves whose columns are compressed blocks; a save+load round-trip that works for columns > 4 MB.

- [ ] **Step 1: Confirm how the column loop learns the active compression mode.**

`WriteCompressedBlock` already reads the writer's private `m_compressionMode`. The `Archetype::Serialize`/`Deserialize` loops need the same signal to choose the compressed vs inline path. Check for an accessor:
```bash
grep -n "CompressionMode\|GetCompressionMode\|compressionMode" include/Astra/Serialization/BinaryWriter.hpp include/Astra/Serialization/BinaryReader.hpp
```
If `BinaryWriter`/`BinaryReader` already expose `GetCompressionMode()` (or the header's mode via `BinaryReader`), use it. If not, add a trivial `[[nodiscard]] CompressionMode GetCompressionMode() const noexcept { return m_compressionMode; }` to `BinaryWriter`, and on `BinaryReader` expose the header's mode (`m_compressionMode` is set from the header at `BinaryReader.hpp:176`) via `GetCompressionMode()`. This is the only allowed additive codec-adjacent change.

- [ ] **Step 2: Write the failing round-trip test** in `tests/Registry/RegistrySerializationTest.cpp`. Use an existing large-ish POD component or add a test-local one whose serialized column exceeds 4 MB (e.g. a component holding `std::array<uint32_t, N>` with N chosen so `N*4 * entityCount > 4*1024*1024`; a single entity with a `>4MB` array works and directly exercises the C3 multi-block case):

```cpp
TEST(RegistrySerializationTest, LZ4_CompressesAndRoundTripsLargeColumn)
{
    using Big = Astra::Test::/* a registered POD component with a >4MB payload; see note */;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "astra_lz4_big.bin";

    // Save WITH LZ4.
    {
        Astra::Registry reg;
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Big>(e, MakeBig(/* deterministic fill */));
        Astra::SaveConfig cfg; cfg.compressionMode = Astra::CompressionMode::LZ4;
        ASSERT_TRUE(reg.Save(path, cfg).IsOk());
    }
    // Load and verify the payload survived (previously: saved fine, never loaded).
    {
        Astra::Registry reg;
        ASSERT_TRUE(reg.Load(path).IsOk());
        // exactly one entity, its Big component byte-equal to what we saved
        size_t seen = 0;
        reg.CreateView<Big>().ForEach([&](Astra::Entity, const Big& b){ ++seen; EXPECT_EQ(b, MakeBig()); });
        EXPECT_EQ(seen, 1u);
    }
    std::filesystem::remove(path);
}
```
(Note: register `Big` via the shared test-component mechanism; reuse an existing >4MB-capable component if one exists to respect the TypeID ceiling. If `Big`'s equality isn't defined, compare a representative field.)

- [ ] **Step 3: Run it — verify it FAILS** (compression is still dead, so today it either saves-uncompressed-and-loads OR, once the compressed write lands without the read, fails to load). Run:
`bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_filter=RegistrySerializationTest.LZ4_CompressesAndRoundTripsLargeColumn`
Expected before implementation: the test passes trivially against the uncompressed path (LZ4 is dead). To make it a real RED, temporarily assert the on-disk file is smaller than an equivalent `None` save (see Step 7's ratio check) — that assertion FAILS today because no compression happens. Keep that assertion in the final test.

- [ ] **Step 4: Implement the compressed WRITE path.** In `Archetype::Serialize`'s column loop, branch on the writer's mode:

```cpp
                    if (writer.GetCompressionMode() == CompressionMode::LZ4)
                    {
                        // Per-column block: serialize the column (data + disabled section)
                        // into a sub-buffer with checksum off, then compress that buffer as
                        // one block into the main stream. WriteCompressedBlock stores raw when
                        // below threshold / incompressible, so tiny columns never inflate.
                        std::vector<std::byte> colBuf;
                        {
                            BinaryWriter sub(colBuf);
                            sub.SetChecksumEnabled(false);
                            SerializeColumn(sub, chunk.get(), desc, chunkEntityCount);
                            sub.Flush();
                        }
                        writer.WriteCompressedBlock(colBuf.data(), colBuf.size());
                    }
                    else
                    {
                        SerializeColumn(writer, chunk.get(), desc, chunkEntityCount);
                    }
```

- [ ] **Step 5: Implement the compressed READ path.** In `Archetype::Deserialize`'s column loop, mirror it, branching on the header's mode:

```cpp
                    ResultType colResult = ResultType::Ok();
                    if (reader.GetCompressionMode() == CompressionMode::LZ4)
                    {
                        auto blk = reader.ReadCompressedBlock();
                        if (blk.IsErr())
                            return ResultType::Err(SerializationError::CorruptedData);
                        const auto& bytes = *blk.GetValue(); // std::vector<uint8_t>
                        BinaryReader sub(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                        sub.SetChecksumEnabled(false);
                        colResult = DeserializeColumn(sub, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      diskHasDisabledSection[di]);
                    }
                    else
                    {
                        colResult = DeserializeColumn(reader, chunk, archetype.get(), desc,
                                                      static_cast<size_t>(chunkEntityCount),
                                                      diskHasDisabledSection[di]);
                    }
                    if (colResult.IsErr())
                        return colResult;
```

- [ ] **Step 6: Remove the dead trivially-copyable compressed branch** now inside `SerializeColumn`/`DeserializeColumn`. In `SerializeColumn`, delete the `else if (desc.is_trivially_copyable) { writer.WriteCompressedBlock(componentArray, arraySize); }` arm and its `else { ASTRA_ASSERT(false, ...) }`; every registered component has `serializeVersioned` (`ComponentRegistry.hpp:212`), so the first arm always runs — replace the whole `if/else if/else` with just the per-element serialize logic. Mirror-delete the `ReadCompressedBlock` arm in `DeserializeColumn`. (This kills the versioning-bypass; compression now comes only from the per-column wrapper in Steps 4-5.)

- [ ] **Step 7: Make the test assert real compression.** In the round-trip test, save the same world twice — once `None`, once `LZ4` — and assert the LZ4 file is materially smaller AND still loads equal:
```cpp
    auto sizeOf = [](const std::filesystem::path& p){ return std::filesystem::file_size(p); };
    // ... save none -> pathNone, save lz4 -> pathLz4, both load-equal ...
    EXPECT_LT(sizeOf(pathLz4), sizeOf(pathNone)); // compression actually engaged
```

- [ ] **Step 8: Run the new test + full suite, all three configs.**
Expected: the round-trip test PASSES (including the size assertion), and the full suite is **≥ baseline + 1** (the new test) in each config, all pass. If any existing serialization test fails, the None-mode path drifted — it must be byte-identical; fix.

- [ ] **Step 9: Commit**

```bash
git add include/Astra/Archetype/Archetype.hpp include/Astra/Serialization/BinaryWriter.hpp include/Astra/Serialization/BinaryReader.hpp tests/Registry/RegistrySerializationTest.cpp
git commit -m "feat(serialization): per-column LZ4 compression on the save path (orthogonal to versioning; removes dead bypass; fixes >4MB round-trip)"
```

---

### Task 4: Acceptance + edge-case tests

**Files:**
- Test: `tests/Registry/RegistrySerializationTest.cpp` (or `tests/Serialization/BinarySerializationTests.cpp`)

**Interfaces:**
- Consumes: the LZ4 save/load path from Task 3.
- Produces: coverage for the spec §6 cases not already covered by Task 3's round-trip.

- [ ] **Step 1: `None` mode is byte-identical across the change.** Add a test that saves a small mixed world with `compressionMode = None` and asserts the load round-trips exactly (component values + entity count). **Include at least one empty/tag component in this world** — tag columns have a null `componentArray` and are skipped by the `if (!componentArray) continue;` in both loops, so they never form a block; this world exercises that skip under both `None` and (reuse the same world for) an LZ4 save to confirm tags are compression-safe. (Byte-identical to pre-change is implicitly held by the untouched None path + the existing serialization suite; this test locks the round-trip.)

```cpp
TEST(RegistrySerializationTest, NoneMode_RoundTripsUnchanged)
{
    // world with a POD component, a non-trivial component, AND a tag/empty component;
    // save None -> load -> assert every component + entity matches; tag entities still tagged
}
```

- [ ] **Step 2: Non-trivial / custom-`Serialize` component compresses + round-trips.** Prove orthogonality to versioning — a component with a `std::string`/`std::vector` member or a custom `Serialize` hook must survive an LZ4 save+load with its hook honored:

```cpp
TEST(RegistrySerializationTest, LZ4_NonTrivialComponentRoundTrips)
{
    // a component bearing std::string / a custom Serialize; N entities;
    // save LZ4, load, assert each value (incl. the string / hook-remapped field) matches
}
```

- [ ] **Step 3: Mixed enableable + disabled bits under compression.** Save an archetype where an enableable component has some entities disabled, with LZ4 on; load and assert the enabled/disabled state survives (the disabled section rides inside the compressed column buffer):

```cpp
TEST(RegistrySerializationTest, LZ4_EnableableDisabledBitsSurvive)
{
    // enableable component, disable a subset, save LZ4, load,
    // assert the same subset is disabled (via the enabled-filtered view count / IsEnabled)
}
```

- [ ] **Step 4: Below-threshold small column stays raw and round-trips.** A tiny component/world under `compressionThreshold` saved with LZ4 must still load (WriteCompressedBlock stores it raw):

```cpp
TEST(RegistrySerializationTest, LZ4_SmallColumnBelowThresholdRoundTrips)
{
    // a few entities with a small component; save LZ4; load; assert equality
}
```

- [ ] **Step 5: Run the new tests + full suite, all three configs.**
Expected: all new tests pass; full suite green in Debug/Release/Dist.

- [ ] **Step 6: Commit**

```bash
git add tests
git commit -m "test(serialization): LZ4 per-column acceptance cases (None-identical, non-trivial, enableable, below-threshold)"
```

---

### Finishing — whole-branch review + integration

- [ ] **Opus whole-branch review.** Run `scripts/review-package <base> HEAD` and dispatch the final code-reviewer (superpowers:requesting-code-review) on the OPUS model. Focus: None-mode byte-identity, write/read symmetry of the compressed path, the sub-writer/sub-reader checksum+flush handling, the dead-branch removal completeness, and v5/v4 read gating. Feed it the deferred-Minor list from `docs/reviews/2026-07-25-astra-tier0-correctness-register.md` for context (nothing here should touch those).
- [ ] **Dispatch ONE fix subagent** for any Critical/Important findings (with the covering test files named); re-review after fixes.
- [ ] **3-config verify** (controller-run, independent of the implementers): Debug/Release/Dist all green, counts = baseline + the new tests.
- [ ] **Confirm with the user, then FF-merge** the feature branch into its base (`fix/tier0-correctness-batch` or `dev`), delete the branch, don't push. Update memory: `astra-remediation-roadmap` (LZ4 dispatch shipped → register §3 fully closed) and `astra-perf-optimization` if bench-relevant.
