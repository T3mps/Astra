# AstraStudio Memory panel — chunk memory-layout / cache-line visualization (design)

**Date:** 2026-07-28
**Status:** Design approved (composite layout + full interactivity user-locked); ready for implementation planning
**Predecessor:** `2026-07-27-astra-studio-mvp-design.md` — this spec delivers that spec's deferred "chunk memory-layout map + cache-line occupancy viz" and its deferred "exact chunk-footprint accounting".
**Branch:** off dev @ `3b5ef5b` (e.g. `feature/studio-memory-panel`); finish = local FF merge to dev, branch deleted, not pushed.

---

## 1. Goal

A new AstraStudio **Memory** panel that visualizes one chunk's physical layout — cache-line-aligned SoA columns, alignment padding, enableable disabled-bit words, and slack — plus per-cache-line occupancy and an entity probe showing how one entity's components scatter across columns. Layered goals (user-locked, all three): byte accounting, SoA teaching view, cache-line occupancy.

Three treatments were mocked in-browser (page grid / address bar + lens / SoA lanes; session `.superpowers/brainstorm/4829-1785252870/`, gitignored). **User-locked composition — a single composite panel:**

- **Overview bar** (always on): proportional address-space bar of the whole chunk — the minimap, accounting view, and zoom scrubber in one. A white viewport box shows the region the Bytes view has zoomed to; white ticks mark the probe entity.
- **Bytes tab**: the chunk as a grid of 64 B cache-line cells (32 per row = 2 KB/row). Cell hue = column, brightness = live-byte fraction of that line; padding / disabled-bit words / slack have distinct encodings. Zoomed, this subsumes the "lens" of the bar treatment.
- **Entities tab**: one lane per column, x = entity index. Cache-line boundary ticks (denser for fatter strides), disabled-entity notches in-place, live/dead split at `count`. The SoA mental model.
- **Shared across tabs**: chunk-selector strip, legend + probe-readout footer, tooltips, probe state.

Navigation is selection-linked: the panel renders whatever archetype is selected in the existing Archetypes panel (no duplicate pickers); a chunk strip inside the panel picks the chunk.

Interactivity (user-locked, v1): hover tooltips, entity probe (hover = transient, click = pin), **ctrl+wheel zoom centered on cursor and drag-pan** in both tabs.

## 2. Data layer

### 2.1 Engine accessors (tiny, additive — `ArchetypeChunk`)

Column base offsets are private today; the facade must not infer them from `GetComponentArrayByID` pointer diffs (that would smuggle in the "column 0 sits at arena start" invariant). Add:

```cpp
ASTRA_NODISCARD size_t GetColumnOffset(uint16_t column) const;        // m_columns[c].base − m_memory
ASTRA_NODISCARD size_t GetDisabledWordsOffset(uint16_t column) const; // SIZE_MAX if not enableable
```

Already public and reused as-is: `GetChunkBytes()`, `GetCount()`, `GetCapacity()`, `GetEntities()`, `GetDisabledWords(column)`, `GetDisabledCount(column)`.

### 2.2 Inspector facade extension (`include/Astra/Debug/Inspector.hpp`, all additive)

**Hot-loop rule (user directive):** `Capture` runs every frame; it stays O(archetypes × columns + chunks) POD work regardless of entity count. Per-entity-sized copies are scoped to the *selected* chunk via a separate on-demand call.

```cpp
struct ChunkColumnLayout {
    size_t offset = 0, bytes = 0;                  // bytes = stride × capacity
    size_t disabledOffset = SIZE_MAX, disabledBytes = 0;
    uint32_t disabledCount = 0;                    // scalar only — words are in ChunkDetail
};
struct ChunkInfo {
    size_t count = 0, capacity = 0;                // existing fields
    size_t chunkBytes = 0;                         // arena size (GetChunkBytes)
    std::vector<ChunkColumnLayout> columns;        // parallel to ArchetypeInfo::columns
    size_t columnBytes = 0, padBytes = 0, bitsBytes = 0, slackBytes = 0;
    // invariant: columnBytes + padBytes + bitsBytes + slackBytes == chunkBytes (unit-tested)
};

struct ChunkDetail {                               // selected chunk only, O(one chunk)
    std::vector<Entity> entities;                  // row → Entity (probe tooltips)
    std::vector<std::vector<uint64_t>> disabledWords; // per enableable column (lane notches)
};
bool CaptureChunkDetail(Registry&, size_t archetypeIndex, size_t chunkIndex, ChunkDetail& out);
// same-thread, same-frame as Capture → indices consistent; false if stale (caller skips detail layers)
```

Additional snapshot fields:
- `InspectorSnapshot::cacheLineBytes` — so panels never include engine internals.
- `ArchetypeInfo::bytesReserved` and `RegistrySnapshot::bytesReserved` — Σ `GetChunkBytes()`, the true arena footprint (includes pad/bits/slack). Existing layout-derived `bytesUsed`/`bytesAllocated` keep their semantics untouched; the Registry panel may additionally display "reserved". This closes the MVP spec's deferred exact-footprint accounting.

**Capture-into overload** to kill per-frame allocation churn: `void Capture(Registry&, InspectorSnapshot& out)` — `clear()` + refill with retained outer vector capacities and an upfront `reserve`; the existing by-value `Capture` becomes a wrapper. Inner string reuse is deliberately not chased until measured.

## 3. Studio panel (`studio/MemoryPanel.hpp`)

Header-only class matching StudioApp's style. StudioApp changes are minimal: own a `MemoryPanel`, call `m_memoryPanel.Draw(m_snapshot, m_selectedArchetype, m_registry)` in `RenderFrame` (registry passed only for `CaptureChunkDetail`). No refactor of existing panels.

**Panel state:** view mode (Bytes/Entities), selected chunk index, per-mode zoom + pan, probe row (−1 = none; hover transient, click pins), reused `ChunkDetail` buffer.

**Rendering (ImDrawList):**
- Overview bar: one `AddRectFilled` per region (per column: live / dead; plus pad gaps, bits, slack) — ≤ a few dozen rects; viewport box; probe ticks; drag = pan.
- Bytes grid: cells at `zoom` px per 64 B line, 32 lines/row; only visible rows drawn (scroll clipping) so 512 KB chunks (8192 lines) stay trivial. Cell color = column hue mixed toward the surface by live fraction; boundary cells show a padding notch; pad/bits/slack use the mock encodings (neutral / dotted / hatched).
- Entities lanes: per-column lane, live/dead split at `count`, cache-line ticks every `64/stride` entities (skipped when denser than ~3 px), disabled notches from `ChunkDetail.disabledWords`, probe line crossing all lanes.
- Tooltips: hovered cell/lane → line index, byte range, component, entity rows spanned, live/dead/pad split, Entity id + disabled state from `ChunkDetail`.

**Colors:** fixed 8-slot dark categorical palette, validated with the dataviz six-checks validator on the dark surface (`#1a1a19`): `#3987e5` `#d95926` `#199e70` `#c98500` `#d55181` `#008300` `#9085e9` `#e66767`, assigned by column ordinal in fixed order; ordinal ≥ 8 folds to neutral gray (identity via tooltip). Structural encodings are non-categorical: pad `#34342f`, bits = dotted gray, slack = 45° hatch, dead = hue mixed ~20 % toward surface, probe = white.

**Interaction:** ctrl+wheel zoom centered on cursor (clamped [1 px .. 32 px] per line), drag pan, hover/click probe shared across tabs and overview bar, chunk strip click selects.

## 4. Edge handling

- No archetype selected → hint text; tag-only archetype (zero storage columns) → explanatory message.
- Live workload changes counts every frame: chunk index and probe row clamped every frame; `CaptureChunkDetail` returning false skips detail layers (no assert).
- Enableable-free archetypes: no bits region, no notches, zero cost (mirrors engine's zero-cost gate).
- Empty chunks / empty registry render as accounting-only (slack = whole arena).

## 5. Testing

Facade tests in `tests/Debug/InspectorTest.cpp` (AstraTest; **reuse `Astra::Test::*` component types** — 128-TypeID ceiling):
- Offsets cache-line-aligned, ascending, non-overlapping; every column's `bytes = stride × capacity`.
- Accounting invariant sums exactly to `chunkBytes`.
- Disabled region present only for enableable columns; `disabledCount` + `ChunkDetail.disabledWords` match actual `SetEnabled` calls.
- `ChunkDetail.entities` size == `count` and rows match live entities; stale-index call returns false.
- `bytesReserved` ≥ layout-derived `bytesAllocated`; empty-registry snapshot.
- Capture-into overload produces results identical to by-value `Capture` across two refills.

GUI: manual smoke per MVP precedent (panel renders, tabs/zoom/probe work against a live workload). 3-config build gate (Debug/Release/Dist).

## 6. Success criteria

- Memory panel renders the composite (chunk strip + overview bar + Bytes/Entities tabs + footer) for any archetype the Archetypes panel selects, live while the workload mutates.
- Per-frame `Capture` cost independent of entity count (heavy copies only via `CaptureChunkDetail`, O(selected chunk)).
- Facade tests green in all three configs; engine changes limited to the two `ArchetypeChunk` offset accessors; no new includes in `Astra.hpp`.
- Accounting invariant holds for every chunk in every test registry.
