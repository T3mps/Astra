# AstraStudio MVP — ImGui visualization & workload suite (design)

**Date:** 2026-07-27
**Status:** Design approved (backend + data-access decisions user-locked); ready for implementation planning
**Terminology note:** Astra's container is the **`Registry`** (not "world" — that is flecs vocabulary). All names in this suite use Registry terms: `RegistrySnapshot`, `RegistryPanel`, `Capture(Registry&)`.

---

## 1. Goal & value

A native tool app, **AstraStudio**, that runs simulation workloads against a live `Astra::Registry` and visualizes engine internals: archetypes, archetype/chunk column layout, entities, and memory statistics. This directly addresses the 2026-07-27 full review's top ecosystem gap ("no inspector/explorer") and gives the perf program a workload playground.

Two user-locked decisions:
1. **Backend: GLFW + OpenGL 3** (cross-platform; aligns with the review's get-off-single-platform push). ImGui **docking branch** for dockable panels.
2. **Data access: a clean, read-only `Astra::Debug` Inspector facade** — the GUI renders snapshot PODs, never live internals. The facade is the single choke point that touches internals, is independently unit-testable, and survives the planned demotion of `GetArchetypeManager()`-style leaky accessors (review P1). It is **the** reusable inspector; the GUI is just its first consumer.

## 2. Scope

**MVP (this spec):**
- Vendor `imgui` (docking) + `glfw` under `vendor/`; premake static-lib projects; new `AstraStudio` app project.
- **No glad**: `imgui_impl_opengl3` embeds its own GL loader (`imgui_impl_opengl3_loader.h`); app-side GL is limited to GL 1.1 calls (clear/viewport) available from system headers. GL 3.3 core context, GLSL `#version 330`.
- `include/Astra/Debug/Inspector.hpp` — **opt-in** header (NOT added to the `Astra.hpp` umbrella; keeps core lean per review), namespace `Astra::Debug`.
- Three panels over a live Registry: **RegistryPanel** (summary stats), **ArchetypesPanel** (sortable archetype table + column preview), **WorkloadPanel** (spawn/clear/step presets so the data is live).
- Unit tests for the facade (in AstraTest, reusing `Astra::Test::*` types — 128-TypeID ceiling).

**Deferred (own specs later):** chunk **memory-layout map** + cache-line occupancy viz (custom draw), entity browser with reflected component values, relations graph, richer workload library under the SystemScheduler, and **Tracy** integration (real cache/timing sampling; separate client+server model). Rationale: those carry the visual-design risk; the MVP proves the facade + shell spine first.

## 3. Inspector facade (`include/Astra/Debug/Inspector.hpp`)

Read-only snapshot API; all strings/values copied out (no pointers into live state):

```cpp
namespace Astra::Debug
{
    struct ColumnInfo      { std::string name; ComponentID id; size_t size, alignment; uint32_t stride; bool isEnableable; };
    struct ChunkInfo       { size_t count, capacity; };
    struct ArchetypeInfo   { std::string signature;            // component names from mask (tags included)
                             size_t componentCount, tagCount;
                             std::vector<ColumnInfo> columns;  // storage-bearing only (tags excluded, as in ArchetypeColumnMeta)
                             std::vector<ChunkInfo> chunks;
                             size_t entityCount, chunkCount, bytesUsed, bytesAllocated; };
    struct RegistrySnapshot{ size_t entityCount, archetypeCount, chunkCount, bytesUsed, bytesAllocated; };
    struct InspectorSnapshot { RegistrySnapshot registry; std::vector<ArchetypeInfo> archetypes; };

    InspectorSnapshot Capture(Registry& registry);   // read-only walk; non-const only because the enumeration accessors are non-const today
}
```

Verified data sources (current code): `Registry::GetArchetypeManager()`; `ArchetypeManager::GetArchetypes()` (ranges view of `Archetype*`, ArchetypeManager.hpp:600); `Archetype::GetMask()/GetEntityCount()/GetChunkCount()/GetChunks()/GetColumnMeta()` (Archetype.hpp:1417–1429); `ArchetypeColumnMeta::columns[i] = {id, stride, descriptor}` (ArchetypeChunkPool.hpp:30–54); `ComponentDescriptor{name, size, alignment, isEnableable, is_empty}` (Component.hpp:64–94); `ArchetypeChunk::GetCapacity()/GetCount()`. Signature strings come from mask bits resolved to descriptor names (tags included; storage columns exclude tags). **Byte figures are layout-derived for the MVP** (`Σ stride × capacity` / `× count` per chunk) — exact chunk-footprint accounting arrives with the memory-map panel.

MVP threading model: single-threaded — workload steps and `Capture` run on the UI thread between frames. No locking. (A live-parallel mode would need a snapshot fence; out of scope.)

## 4. Studio app (`studio/`)

- `studio/main.cpp` — GLFW window + GL 3.3 core context + ImGui (docking) init; per-frame: poll → new frame → dockspace → panels → render.
- `studio/StudioApp.{hpp,cpp}` — owns the `Registry`, a `WorkloadRunner`, the panels; captures an `InspectorSnapshot` each frame (cheap at demo scale) and hands it to panels.
- `studio/Panels/*` — `RegistryPanel` (summary), `ArchetypesPanel` (table: signature, entities, chunks, bytes, occupancy %; selected row lists its `ColumnInfo`s), `WorkloadPanel` (preset picker, count slider, Spawn/Clear/Step buttons, auto-step toggle).
- `studio/Components.hpp` — studio-local demo components (Position/Velocity/Health/etc. mixes). AstraStudio is its own binary with a fresh 128-TypeID budget; it does NOT include tests/ headers.
- `WorkloadRunner` — spawns N entities across preset component mixes; `Step()` runs a simple movement `View::ForEach`; drives archetype variety so panels have real data.

## 5. Build integration (premake)

- `vendor/imgui/premake5.lua` — StaticLib: `imgui*.cpp` + `backends/imgui_impl_glfw.cpp` + `backends/imgui_impl_opengl3.cpp`.
- `vendor/glfw/premake5.lua` — StaticLib: common + per-platform sources (`_GLFW_WIN32` / `_GLFW_X11`).
- Root `premake5.lua`: new `IncludeDir` entries; both projects in the `Dependencies` group; `AstraStudio` ConsoleApp project (`studio/**`), links `ImGui`, `GLFW`, `opengl32` (Windows) / `GL`, `pthread`, `dl` (Linux). Same warning/config filters as sibling projects; exceptions off, RTTI off.

## 6. Testing

- `tests/Debug/InspectorTest.cpp` (AstraTest binary; **reuse `Astra::Test::*` components** for the TypeID ceiling): build a known Registry → assert snapshot archetype count, signatures, entity counts, column names/strides, chunk count/capacity consistency, empty-Registry snapshot.
- GUI itself: manual smoke check per task (app launches, panels render, workload mutates tables live). No automated GUI tests in MVP.

## 7. Success criteria

- `AstraStudio` builds and runs in all three configs on Windows (Linux compile parity best-effort in MVP).
- Live panels: spawning/stepping workloads visibly updates Registry/Archetype tables every frame.
- `Astra::Debug::Capture` unit-tested green in AstraTest; core headers unchanged except the new opt-in `Debug/Inspector.hpp`.
- No new includes in `Astra.hpp`; no change to existing project builds.
