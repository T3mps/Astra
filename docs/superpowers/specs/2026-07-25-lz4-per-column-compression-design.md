# LZ4 Per-Column Compression — Design (2026-07-25)

**Status:** user-approved design (this session).

**Goal:** Make `SaveConfig::compressionMode = LZ4` actually compress saved worlds, via
**per-column block compression** — the columnar-format standard (Parquet/ORC/Arrow) and a
natural fit for Astra's archetype (columnar) storage. Compression is applied *orthogonally
to* the per-element versioning, so it covers every component type and preserves schema
evolution. The now-fixed codec (`WriteCompressedBlock`/`ReadCompressedBlock`, hardened by
C3/I11/IM-18) is reused as-is.

## 1. Context (why this is needed)

- **LZ4 is currently dead on the save path.** `ComponentDescriptor::serializeVersioned` is set
  unconditionally for every component (`ComponentRegistry.hpp:212`), so `Archetype::Serialize`'s
  `else if (desc.is_trivially_copyable) → writer.WriteCompressedBlock(...)` branch
  (`Archetype.hpp:952/956`) is unreachable. A default `Save` stamps an LZ4 header
  (`Registry.hpp:1530`) but writes **uncompressed** bytes. Confirmed by spot-check + review
  (register §3). The `WriteCompressedBlock`/`ReadCompressedBlock` call sites both live in that
  dead branch.
- **The codec itself is correct now** (post batch + review): C3 (multi-block >4 MB decode),
  I11 (decode amplification cap, clamped reserve), IM-18 (unaligned reads) are fixed.
- **Astra is columnar.** Archetype storage is per-component columns, and `Serialize` already
  writes column-by-column (per chunk). A column holds homogeneous values (all the `Position`s,
  all the `Health`s), which is exactly what LZ4 compresses well — so per-column compression is
  both the columnar-DB industry standard and the highest-ratio choice for this layout.
- **The user-facing choice already exists:** `SaveConfig::compressionMode` (`None`/`LZ4`) +
  `compressionLevel`. Keep it; the internal mechanism (per-column) is not a user knob.

## 2. Decisions (this session)

| Decision | Choice |
|---|---|
| Where compression applies | **Per-column blocks** (columnar standard), NOT per-element inline and NOT whole-archive. |
| Relationship to versioning | **Orthogonal** — compress the serialized (versioned) column bytes; keep the per-element format untouched. Covers all component types. |
| Granularity (v1) | **Per-chunk / per-column** (the existing serialize-loop boundary). Coalescing a column across chunks for a better ratio is a later optimization. |
| Format version | **Bump `BINARY_FORMAT_VERSION` 4 → 5.** `v5 + LZ4` = compressed columns; `v5 + None` = uncompressed (== v4 None layout); `v ≤ 4` = legacy (columns always uncompressed). |
| User config | **Unchanged** — `None`/`LZ4` + level; nothing new to learn. |

## 3. Architecture — two-phase column write/read

The per-chunk/per-column serialization becomes compression-aware. When
`m_compressionMode == LZ4`:

- **Write (per chunk, per column):**
  1. Serialize the column into a reusable in-memory buffer (`std::vector<std::byte>`) via a
     nested memory-mode `BinaryWriter` — running the *existing* per-element logic
     (`serializeVersioned` / `serialize` / raw-trivial) **plus** that column's IM-9 disabled-bit
     section, so the whole column payload lands in one buffer.
  2. `writer.WriteCompressedBlock(buffer.data(), buffer.size())` into the main stream.
     `WriteCompressedBlock` already compresses only when `size >= m_compressionThreshold` and
     stores the block raw otherwise, so tiny/incompressible columns never inflate.
- **Read (per chunk, per column):**
  1. `reader.ReadCompressedBlock()` → decompressed `std::vector` for that column.
  2. A nested memory-mode `BinaryReader` over those bytes runs the *existing* per-element
     deserialization + disabled-section read, unchanged.
- **`m_compressionMode == None`** keeps today's inline per-element write **byte-identical** —
  no temp buffer, no block framing, no behavior change. This is the zero-risk default.
- The current dead `else if (is_trivially_copyable) → WriteCompressedBlock(rawArray)` branch is
  **removed**. Compression is now applied uniformly *after* serialization, never as a
  versioning bypass.

**Why keeping per-instance version headers is fine:** for POD columns the per-element
`hash+version` prefix is identical across instances; LZ4 collapses the repetition to
near-nothing. Not worth changing the per-element format.

**Nested-writer checksum:** the per-column sub-`BinaryWriter`/sub-`BinaryReader` run with the
running checksum **disabled** — the column bytes are integrity-covered by the outer stream's
checksum and the block framing; a sub-buffer has no `BinaryHeader`, so its internal checksum
would be meaningless. (Set via the existing checksum-enable control; write and read use the
same setting symmetrically.)

**New unit — column (de)serialization helpers:** factor the "serialize one chunk-column to a
buffer" and "deserialize one chunk-column from a buffer" logic into small private helpers on
`Archetype` so the compressed and uncompressed paths share one implementation of the
per-element + disabled-section encoding (avoids duplicating that logic across the two modes).

## 4. On-disk format (v5)

- `BINARY_FORMAT_VERSION = 5` (in `BinaryArchive.hpp`).
- **`v5` reader** branches on the header's `compressionMode`:
  - `LZ4` → each chunk-column is a compressed block (framed by the existing `BlockHeader`:
    `uncompressedSize`, `compressedSize`), consumed via `ReadCompressedBlock`.
  - `None` → each chunk-column is inline per-element bytes (same as v4 `None`).
- **`v ≤ 4` reader** → legacy path: columns always inline/uncompressed, regardless of the
  header's `compressionMode` field (this is exactly how v4 behaves today, since LZ4 was dead —
  so the transient buggy v4-LZ4 files still load correctly as raw).
- No new block framing is invented; the existing `BlockHeader` written by `WriteCompressedBlock`
  and read by `ReadCompressedBlock` carries the sizes.
- The IM-9 per-descriptor `hasDisabledSection` flag (in the descriptor block) is unchanged and
  orthogonal to compression; the disabled-bit *section* itself rides inside the column buffer
  (so it is compressed together with the column data when LZ4 is on).

## 5. Codec & config

- **No codec changes.** `WriteCompressedBlock`/`ReadCompressedBlock`/`DecompressFrame` are used
  as-is (already review-hardened). `m_compressionThreshold` and `m_compressionLevel` come from
  `SaveConfig` and are already plumbed into `BinaryWriter::Config`.
- **User choice unchanged:** `SaveConfig::compressionMode` (`None`/`LZ4`) + `compressionLevel`.

## 6. Testing / acceptance

- **The C3 case, end to end:** a component column whose serialized bytes exceed 4 MB
  round-trips through `Registry::Save`(LZ4) → `Load` (previously saved-but-never-loaded).
- **Compression actually engages:** an LZ4 save of a large homogeneous column is materially
  smaller than the `None` save, and the file's body contains real compressed blocks (not just an
  LZ4 header over raw bytes).
- **`None` unchanged:** a `None`-mode v5 save is byte-identical to the pre-change v4 `None` save
  layout (modulo the version field), and round-trips.
- **Cross-version:** a v4 file (uncompressed) still loads under the v5 reader.
- **Coverage:** mixed enableable / non-enableable columns under compression; empty/tag columns
  (size-0, no block); non-trivial / custom-`Serialize` / string-bearing components compress and
  round-trip (proving orthogonality to versioning); a below-threshold small column stays raw and
  round-trips.

## 7. Out of scope (v1)

- Whole-archive framing; per-column-**across-chunks** coalescing for a better ratio.
- High-compression (HC) levels beyond the existing `CompressionLevel`.
- Exposing granularity as a config knob.
- Streaming (constant-memory) compression beyond what per-block already gives.
- Untrusted-load hardening of the compressed path beyond the I11 cap already in the codec.

## 8. Files touched

- `include/Astra/Serialization/BinaryArchive.hpp` — `BINARY_FORMAT_VERSION` 4 → 5.
- `include/Astra/Archetype/Archetype.hpp` — `Serialize`/`Deserialize` column loops: nested
  memory writer/reader + `WriteCompressedBlock`/`ReadCompressedBlock` when `LZ4`; version-gated
  read branches; remove the dead trivially-copyable compressed branch; new private
  column-(de)serialize helpers.
- (Read-only reuse: `BinaryWriter`/`BinaryReader` memory-mode ctors, `WriteCompressedBlock`/
  `ReadCompressedBlock`, `Compression::*` — no changes expected. If disabling the nested-writer
  checksum needs a new setter, that is a small additive change to `BinaryWriter`/`BinaryReader`.)
- Tests: `tests/Serialization/*` and/or `tests/Registry/RegistrySerializationTest.cpp` — the
  acceptance cases in §6.
