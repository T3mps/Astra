# AstraStudio MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dockable ImGui tool app (`AstraStudio`, GLFW + OpenGL 3) that runs simulation workloads against a live `Astra::Registry` and visualizes archetypes/layout/memory via a new reusable read-only `Astra::Debug` Inspector facade.

**Architecture:** Vendor `imgui` (docking branch) and `glfw` as premake StaticLib dependency projects (no glad — `imgui_impl_opengl3` embeds its own GL loader; the app itself makes only GL 1.1 calls). Add an opt-in header `include/Astra/Debug/Inspector.hpp` that snapshots Registry internals into POD structs (`InspectorSnapshot`); the GUI renders snapshots only. A `WorkloadRunner` spawns/steps preset entity mixes so panels show live data. Single-threaded: workload step + `Capture` run on the UI thread.

**Tech Stack:** C++20, premake5/MSBuild (Windows primary, Linux compile parity best-effort), Dear ImGui (docking), GLFW 3.4, OpenGL 3.3 core, GoogleTest (facade tests only).

**Spec:** `docs/superpowers/specs/2026-07-27-astra-studio-mvp-design.md`

## Global Constraints

- **Terminology:** Astra's container is the **`Registry`** — never "world". Type/panel names: `RegistrySnapshot`, `RegistryPanel`.
- **Core stays lean:** `Debug/Inspector.hpp` is opt-in; do NOT add it to `include/Astra/Astra.hpp`. No changes to any existing Astra header except adding the new file.
- **TypeID ceiling (AstraTest binary only):** the facade tests in AstraTest MUST reuse `Astra::Test::*` components from `tests/TestComponents.hpp`. The `AstraStudio` binary is separate and defines its own components freely (fresh 128 budget).
- **Verified API surface the facade uses (do not re-derive):** `Registry::GetArchetypeManager()` → `ArchetypeManager*` (Registry.hpp:1192); `Registry::GetComponentRegistry()` → `ComponentRegistry*` (Registry.hpp:1189); `ArchetypeManager::GetArchetypes()` → ranges view of `Archetype*` (ArchetypeManager.hpp:600, non-const); `Archetype::GetMask()/GetEntityCount()/GetChunkCount()/GetChunks()/GetColumnMeta()` (Archetype.hpp:1417–1429, const); `ArchetypeColumnMeta{columnCount, columns[i]={id,stride,descriptor}}` (ArchetypeChunkPool.hpp:30–54); `ComponentDescriptor{id,size,alignment,name,is_empty,isEnableable}` (Component.hpp:80–94); `ComponentRegistry::GetComponentDescriptor(ComponentID)` → `const ComponentDescriptor*` (ComponentRegistry.hpp:82); `ComponentMask::Count()` (Bitmap.hpp:162); `ArchetypeChunk::GetCount()/GetCapacity()`.
- **Build commands** (`<Cfg>` ∈ {Debug, Release, Dist}): regen `premake5 vs2022` (needed whenever files/projects are added); build `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Astra.sln -p:Configuration=<Cfg> -p:Platform=x64 -m`; tests `bin/<Cfg>-windows-x86_64/AstraTest/AstraTest.exe`; studio `bin/<Cfg>-windows-x86_64/AstraStudio/AstraStudio.exe`. No `-t:` project targeting (MSB4057). IDE/clangd diagnostics are false positives — judge only by MSBuild.
- **GUI verification is manual:** launch the exe, verify visually, close it. Automated tests cover only the facade.
- **Branch:** all work on `feat/astra-studio` (created off `dev` in Task 1).

---

## File Structure

**New (vendored):** `vendor/imgui/` (docking; core + `backends/imgui_impl_glfw.*`, `backends/imgui_impl_opengl3.*`, `imgui_impl_opengl3_loader.h`; + `vendor/imgui/premake5.lua` we write); `vendor/glfw/` (3.4 source; + `vendor/glfw/premake5.lua` we write).

**New (ours):**
- `include/Astra/Debug/Inspector.hpp` — the whole facade (header-only).
- `tests/Debug/InspectorTest.cpp` — facade unit tests.
- `studio/main.cpp` — GLFW/GL/ImGui bootstrap + frame loop.
- `studio/Components.hpp` — studio-local demo components.
- `studio/WorkloadRunner.hpp` — spawn/clear/step presets (header-only, studio-local).
- `studio/StudioApp.hpp` — owns Registry + runner + panels; renders all panels (header-only, studio-local).

**Modified:** root `premake5.lua` only (IncludeDirs, dependency group, `AstraStudio` project).

---

### Task 1: Vendor ImGui + GLFW, premake projects, window shell

**Files:**
- Create: `vendor/imgui/**` (vendored), `vendor/glfw/**` (vendored), `vendor/imgui/premake5.lua`, `vendor/glfw/premake5.lua`, `studio/main.cpp`
- Modify: `premake5.lua`

**Interfaces:**
- Produces: `ImGui` and `GLFW` StaticLib premake projects; `AstraStudio` ConsoleApp that opens a 1600×900 dockable ImGui window (demo window visible). `studio/main.cpp` calls `RunStudio()` if a `studio/StudioApp.hpp` exists later — for now it draws `ImGui::ShowDemoWindow()`.

- [ ] **Step 1: Create the branch**

```bash
git checkout dev
git checkout -b feat/astra-studio
```

- [ ] **Step 2: Vendor the sources**

```bash
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git vendor/imgui
git clone --depth 1 --branch 3.4 https://github.com/glfw/glfw.git vendor/glfw
rm -rf vendor/imgui/.git vendor/glfw/.git vendor/glfw/docs vendor/glfw/examples vendor/glfw/tests vendor/imgui/examples vendor/imgui/docs
```

Verify `vendor/imgui/backends/imgui_impl_opengl3_loader.h` exists (the embedded GL loader — this is why we skip glad).

- [ ] **Step 3: Write `vendor/imgui/premake5.lua`**

```lua
project "ImGui"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "../../ide"
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "imgui.cpp", "imgui_demo.cpp", "imgui_draw.cpp",
        "imgui_tables.cpp", "imgui_widgets.cpp",
        "backends/imgui_impl_glfw.cpp", "backends/imgui_impl_opengl3.cpp",
        "*.h", "backends/imgui_impl_glfw.h", "backends/imgui_impl_opengl3.h"
    }
    includedirs { ".", "../glfw/include" }
    filter "system:windows"
        systemversion "latest"
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
    filter "configurations:Release or configurations:Dist"
        runtime "Release"
        optimize "speed"
    filter {}
```

- [ ] **Step 4: Write `vendor/glfw/premake5.lua`**

```lua
project "GLFW"
    kind "StaticLib"
    language "C"
    staticruntime "on"
    location "../../ide"
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "include/GLFW/*.h",
        "src/context.c", "src/init.c", "src/input.c", "src/monitor.c",
        "src/platform.c", "src/vulkan.c", "src/window.c",
        "src/egl_context.c", "src/osmesa_context.c",
        "src/null_init.c", "src/null_joystick.c", "src/null_monitor.c", "src/null_window.c"
    }
    filter "system:windows"
        systemversion "latest"
        defines { "_GLFW_WIN32", "_CRT_SECURE_NO_WARNINGS" }
        files {
            "src/win32_init.c", "src/win32_joystick.c", "src/win32_module.c",
            "src/win32_monitor.c", "src/win32_thread.c", "src/win32_time.c",
            "src/win32_window.c", "src/wgl_context.c"
        }
    filter "system:linux"
        defines { "_GLFW_X11" }
        files {
            "src/x11_init.c", "src/x11_monitor.c", "src/x11_window.c",
            "src/xkb_unicode.c", "src/glx_context.c", "src/linux_joystick.c",
            "src/posix_module.c", "src/posix_poll.c", "src/posix_thread.c", "src/posix_time.c"
        }
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
    filter "configurations:Release or configurations:Dist"
        runtime "Release"
        optimize "speed"
    filter {}
```

- [ ] **Step 5: Wire the root `premake5.lua`**

Add to the `IncludeDir` table (after the Mosaic line):

```lua
    IncludeDir["ImGui"] = "vendor/imgui"
    IncludeDir["GLFW"] = "vendor/glfw/include"
```

Add to the `Dependencies` group (after the GoogleBenchmark include):

```lua
        include "vendor/imgui"
        include "vendor/glfw"
```

Add a new project after `AstraBenchmark` (inside the `Astra` group, before the compile-check helpers):

```lua
        project "AstraStudio"
            kind "ConsoleApp"
            language "C++"
            cppdialect "C++20"
            staticruntime "on"
            location "ide"
            targetdir ("bin/" .. outputdir .. "/%{prj.name}")
            objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
            files { "studio/**.hpp", "studio/**.cpp" }
            includedirs {
                "%{IncludeDir.Astra}", "%{IncludeDir.Mosaic}",
                "%{IncludeDir.ImGui}", "%{IncludeDir.ImGui}/backends", "%{IncludeDir.GLFW}"
            }
            links { "ImGui", "GLFW" }
            filter "system:windows"
                systemversion "latest"
                links { "opengl32", "gdi32" }
                buildoptions { "/Zc:__cplusplus", "/arch:AVX", "/bigobj", "/fp:fast" }
                defines { "__SSE2__", "__SSE4_2__" }
            filter "system:linux"
                links { "GL", "X11", "pthread", "dl" }
                buildoptions { "-mavx" }
            filter "configurations:Debug"
                runtime "Debug"
                symbols "on"
                optimize "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DEBUG", "_DEBUG" }
            filter "configurations:Release"
                runtime "Release"
                optimize "speed"
                symbols "on"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_RELEASE", "NDEBUG" }
            filter "configurations:Dist"
                runtime "Release"
                optimize "full"
                symbols "off"
                exceptionhandling "off"
                rtti "off"
                defines { "ASTRA_BUILD_DIST", "NDEBUG" }
            filter {}
```

- [ ] **Step 6: Write `studio/main.cpp`**

```cpp
#include <cstdio>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

int main()
{
    glfwSetErrorCallback([](int code, const char* desc)
        { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); });
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "AstraStudio", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        ImGui::ShowDemoWindow();   // Task 3 replaces this with StudioApp panels

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

(GL 1.1 calls — `glViewport`/`glClearColor`/`glClear` — come from `GLFW/glfw3.h`'s pulled-in `GL/gl.h`; no loader needed for them. `DockSpaceOverViewport`'s first parameter is the dockspace id (0 = auto) on current docking branch; if the vendored revision has the older 1-arg signature, call `ImGui::DockSpaceOverViewport(ImGui::GetMainViewport())` instead — check the signature in `vendor/imgui/imgui.h` and use whichever compiles.)

- [ ] **Step 7: Regen, build, run**

```bash
premake5 vs2022
```

Build Debug (full solution). Expected: `ImGui`, `GLFW`, `AstraStudio` all compile and link; existing projects unaffected. Then launch `bin/Debug-windows-x86_64/AstraStudio/AstraStudio.exe` — expect a dark 1600×900 window with the ImGui demo window, dockable. Close it.

- [ ] **Step 8: Commit**

```bash
git add vendor/imgui vendor/glfw premake5.lua studio/main.cpp
git commit -m "feat(studio): vendor ImGui(docking)+GLFW, premake projects, AstraStudio window shell (studio task 1)"
```

---

### Task 2: `Astra::Debug` Inspector facade + unit tests

**Files:**
- Create: `include/Astra/Debug/Inspector.hpp`
- Test: `tests/Debug/InspectorTest.cpp`

**Interfaces:**
- Consumes: the verified API surface in Global Constraints (ArchetypeManager/Archetype/ColumnMeta/ComponentRegistry accessors).
- Produces: `Astra::Debug::{ColumnInfo, ChunkInfo, ArchetypeInfo, RegistrySnapshot, InspectorSnapshot}` and `Astra::Debug::Capture(Registry&) -> InspectorSnapshot` — exactly as specced (spec §3). Tasks 3–4 render these PODs.

- [ ] **Step 1: Write the failing test**

Create `tests/Debug/InspectorTest.cpp`:

```cpp
#include <algorithm>

#include <gtest/gtest.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>
#include "../TestComponents.hpp"

namespace
{
    using Astra::Test::Position;
    using Astra::Test::Velocity;
    using Astra::Test::Player;   // empty tag

    const Astra::Debug::ArchetypeInfo* FindBySignature(
        const Astra::Debug::InspectorSnapshot& snap, std::string_view needle)
    {
        for (const auto& a : snap.archetypes)
            if (a.signature.find(needle) != std::string::npos) return &a;
        return nullptr;
    }
}

TEST(Inspector, EmptyRegistrySnapshotIsEmpty)
{
    Astra::Registry reg;
    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.registry.entityCount, 0u);
    // The root (component-less) archetype may exist; every archetype must be empty.
    for (const auto& a : snap.archetypes)
        EXPECT_EQ(a.entityCount, 0u);
}

TEST(Inspector, SnapshotReportsArchetypesEntitiesAndColumns)
{
    Astra::Registry reg;
    for (int i = 0; i < 100; ++i) (void)reg.CreateEntity<Position, Velocity>();
    for (int i = 0; i < 25; ++i)  (void)reg.CreateEntity<Position, Player>();

    auto snap = Astra::Debug::Capture(reg);
    EXPECT_EQ(snap.registry.entityCount, 125u);
    EXPECT_GE(snap.registry.archetypeCount, 2u);
    EXPECT_GT(snap.registry.chunkCount, 0u);
    EXPECT_GT(snap.registry.bytesAllocated, 0u);

    const auto* pv = FindBySignature(snap, "Velocity");
    ASSERT_NE(pv, nullptr);
    EXPECT_EQ(pv->entityCount, 100u);
    ASSERT_EQ(pv->columns.size(), 2u);   // Position + Velocity (both storage-bearing)
    // Columns carry real descriptor data.
    for (const auto& c : pv->columns)
    {
        EXPECT_FALSE(c.name.empty());
        EXPECT_GT(c.size, 0u);
        EXPECT_EQ(c.stride, c.size);
        EXPECT_GT(c.alignment, 0u);
    }
    // Chunk bookkeeping is self-consistent.
    EXPECT_EQ(pv->chunks.size(), pv->chunkCount);
    size_t summed = 0;
    for (const auto& ch : pv->chunks) { EXPECT_LE(ch.count, ch.capacity); summed += ch.count; }
    EXPECT_EQ(summed, pv->entityCount);
    EXPECT_GE(pv->bytesAllocated, pv->bytesUsed);

    // Tag component: in the signature and tag count, NOT in storage columns.
    const auto* pp = FindBySignature(snap, "Player");
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->entityCount, 25u);
    EXPECT_EQ(pp->tagCount, 1u);
    EXPECT_EQ(pp->columns.size(), 1u);   // Position only; Player is empty/tag
    EXPECT_EQ(pp->componentCount, 2u);   // mask counts both
}
```

- [ ] **Step 2: Run to verify it fails**

`premake5 vs2022` (new .cpp), build Debug. Expected: compile error — `Astra/Debug/Inspector.hpp` does not exist. RED.

- [ ] **Step 3: Implement the facade**

Create `include/Astra/Debug/Inspector.hpp`:

```cpp
#pragma once

// Read-only Registry introspection facade (AstraStudio MVP, spec 2026-07-27).
// Opt-in: NOT included by Astra.hpp. Snapshots internals into POD structs so
// tools never hold pointers into live engine state. This header is the single
// sanctioned place that walks ArchetypeManager/Archetype/ColumnMeta for
// inspection; keep tool code out of engine internals.

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

    struct ChunkInfo
    {
        size_t count = 0;
        size_t capacity = 0;
    };

    struct ArchetypeInfo
    {
        std::string signature;              // all mask components (tags included), " + "-joined; "(empty)" for the root
        size_t componentCount = 0;          // mask popcount
        size_t tagCount = 0;                // empty (tag) components in the mask
        std::vector<ColumnInfo> columns;    // storage-bearing only (tags excluded)
        std::vector<ChunkInfo> chunks;
        size_t entityCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;               // layout-derived: sum(count * rowStride) per chunk
        size_t bytesAllocated = 0;          // layout-derived: sum(capacity * rowStride) per chunk
    };

    struct RegistrySnapshot
    {
        size_t entityCount = 0;
        size_t archetypeCount = 0;
        size_t chunkCount = 0;
        size_t bytesUsed = 0;
        size_t bytesAllocated = 0;
    };

    struct InspectorSnapshot
    {
        RegistrySnapshot registry;
        std::vector<ArchetypeInfo> archetypes;
    };

    // Read-only walk. Takes Registry& (not const) only because
    // ArchetypeManager::GetArchetypes() is non-const today.
    inline InspectorSnapshot Capture(Registry& registry)
    {
        InspectorSnapshot snap;
        ArchetypeManager* manager = registry.GetArchetypeManager();
        const ComponentRegistry* components = registry.GetComponentRegistry();
        if (!manager || !components)
            return snap;

        for (Archetype* archetype : manager->GetArchetypes())
        {
            if (!archetype)
                continue;

            ArchetypeInfo info;
            const ComponentMask& mask = archetype->GetMask();
            info.componentCount = mask.Count();

            // Signature from mask bits (tags included).
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

            // Storage-bearing columns from the shared per-archetype metadata.
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
                info.bytesUsed += ch.count * rowStride;
                info.bytesAllocated += ch.capacity * rowStride;
                info.chunks.push_back(ch);
            }

            snap.registry.entityCount += info.entityCount;
            snap.registry.chunkCount += info.chunkCount;
            snap.registry.bytesUsed += info.bytesUsed;
            snap.registry.bytesAllocated += info.bytesAllocated;
            snap.archetypes.push_back(std::move(info));
        }
        snap.registry.archetypeCount = snap.archetypes.size();
        return snap;
    }
} // namespace Astra::Debug
```

- [ ] **Step 4: Run to verify it passes**

Build Debug; run `bin/Debug-windows-x86_64/AstraTest/AstraTest.exe --gtest_filter=Inspector.*`. Expected: 2 tests PASS. Then run the FULL suite (no filter) — no regressions.

- [ ] **Step 5: Commit**

```bash
git add include/Astra/Debug/Inspector.hpp tests/Debug/InspectorTest.cpp
git commit -m "feat(debug): read-only Inspector facade (InspectorSnapshot/Capture) + tests (studio task 2)"
```

---

### Task 3: StudioApp + RegistryPanel + ArchetypesPanel (seeded Registry)

**Files:**
- Create: `studio/Components.hpp`, `studio/StudioApp.hpp`
- Modify: `studio/main.cpp` (replace the demo window with `StudioApp`)

**Interfaces:**
- Consumes: `Astra::Debug::Capture` / `InspectorSnapshot` (Task 2); ImGui tables API.
- Produces: `Studio::StudioApp` with `StudioApp()` (seeds the Registry), `void RenderFrame()` (captures a snapshot + draws all panels). Task 4 adds workload controls into the same class.

- [ ] **Step 1: Write `studio/Components.hpp`**

```cpp
#pragma once

// Studio-local demo components. AstraStudio is its own binary with a fresh
// TypeID budget; these are NOT the AstraTest components.

namespace Studio
{
    struct Position { float x = 0, y = 0, z = 0; };
    struct Velocity { float dx = 0, dy = 0, dz = 0; };
    struct Health   { int current = 100, max = 100; };
    struct Sprite   { int textureId = 0; float scale = 1.0f; };
    struct Lifetime { float seconds = 5.0f; };
    struct Frozen   { };   // tag
}
```

- [ ] **Step 2: Write `studio/StudioApp.hpp`**

```cpp
#pragma once

#include <cstdio>
#include <string>

#include <imgui.h>
#include <Astra/Astra.hpp>
#include <Astra/Debug/Inspector.hpp>

#include "Components.hpp"

namespace Studio
{
    class StudioApp
    {
    public:
        StudioApp()
        {
            // Seed so panels show real data before any workload runs (Task 4
            // replaces this fixed seed with interactive spawning).
            for (int i = 0; i < 500; ++i) (void)m_registry.CreateEntity<Position, Velocity>();
            for (int i = 0; i < 200; ++i) (void)m_registry.CreateEntity<Position, Velocity, Health>();
            for (int i = 0; i < 50;  ++i) (void)m_registry.CreateEntity<Position, Sprite, Frozen>();
        }

        void RenderFrame()
        {
            m_snapshot = Astra::Debug::Capture(m_registry);
            DrawRegistryPanel();
            DrawArchetypesPanel();
        }

    protected:
        static std::string PrettyBytes(size_t b)
        {
            char buf[32];
            if (b >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.2f MB", double(b) / (1024.0 * 1024.0));
            else if (b >= 1024)   std::snprintf(buf, sizeof(buf), "%.1f KB", double(b) / 1024.0);
            else                  std::snprintf(buf, sizeof(buf), "%zu B", b);
            return buf;
        }

        void DrawRegistryPanel()
        {
            ImGui::Begin("Registry");
            const auto& r = m_snapshot.registry;
            ImGui::Text("Entities:    %zu", r.entityCount);
            ImGui::Text("Archetypes:  %zu", r.archetypeCount);
            ImGui::Text("Chunks:      %zu", r.chunkCount);
            ImGui::Text("Memory used: %s / %s",
                PrettyBytes(r.bytesUsed).c_str(), PrettyBytes(r.bytesAllocated).c_str());
            if (r.bytesAllocated > 0)
            {
                float occ = float(double(r.bytesUsed) / double(r.bytesAllocated));
                ImGui::ProgressBar(occ, ImVec2(-1, 0), "occupancy");
            }
            ImGui::End();
        }

        void DrawArchetypesPanel()
        {
            ImGui::Begin("Archetypes");
            const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("archetypes", 5, flags))
            {
                ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Entities");
                ImGui::TableSetupColumn("Chunks");
                ImGui::TableSetupColumn("Bytes");
                ImGui::TableSetupColumn("Occupancy");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (int i = 0; i < int(m_snapshot.archetypes.size()); ++i)
                {
                    const auto& a = m_snapshot.archetypes[size_t(i)];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(a.signature.c_str(), m_selectedArchetype == i,
                            ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedArchetype = i;
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%zu", a.entityCount);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%zu", a.chunkCount);
                    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(PrettyBytes(a.bytesAllocated).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.0f%%", a.bytesAllocated
                        ? 100.0 * double(a.bytesUsed) / double(a.bytesAllocated) : 0.0);
                }
                ImGui::EndTable();
            }

            // Column preview for the selected archetype.
            if (m_selectedArchetype >= 0 && m_selectedArchetype < int(m_snapshot.archetypes.size()))
            {
                const auto& a = m_snapshot.archetypes[size_t(m_selectedArchetype)];
                ImGui::SeparatorText("Columns");
                ImGui::Text("%zu components (%zu tags, %zu storage columns)",
                    a.componentCount, a.tagCount, a.columns.size());
                if (ImGui::BeginTable("columns", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("ID");
                    ImGui::TableSetupColumn("Size");
                    ImGui::TableSetupColumn("Align");
                    ImGui::TableSetupColumn("Enableable");
                    ImGui::TableHeadersRow();
                    for (const auto& c : a.columns)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(c.name.c_str());
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", unsigned(c.id));
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%zu", c.size);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%zu", c.alignment);
                        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(c.isEnableable ? "yes" : "no");
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::End();
        }

        Astra::Registry m_registry;
        Astra::Debug::InspectorSnapshot m_snapshot;
        int m_selectedArchetype = -1;
    };
}
```

- [ ] **Step 3: Wire it into `studio/main.cpp`**

Add includes and replace the demo-window line:

```cpp
#include "StudioApp.hpp"
// ... after ImGui init, before the loop:
    Studio::StudioApp app;
// ... in the loop, replace ImGui::ShowDemoWindow(); with:
        app.RenderFrame();
```

- [ ] **Step 4: Build and run**

Build Debug, launch AstraStudio. Expected: dockable **Registry** panel (750 entities, ≥3 archetypes, non-zero memory) and **Archetypes** table; clicking a row shows its columns (e.g. `Position + Velocity + Health` → 3 storage columns; the `Frozen` archetype shows tagCount 1 with Frozen absent from columns). Close.

- [ ] **Step 5: Commit**

```bash
git add studio/Components.hpp studio/StudioApp.hpp studio/main.cpp
git commit -m "feat(studio): StudioApp + Registry/Archetypes panels over Inspector snapshots (studio task 3)"
```

---

### Task 4: WorkloadRunner + WorkloadPanel

**Files:**
- Create: `studio/WorkloadRunner.hpp`
- Modify: `studio/StudioApp.hpp` (replace fixed seeding with the runner + add the panel)

**Interfaces:**
- Consumes: `Studio::*` components; `Registry::CreateEntity`/`DestroyEntity`; `Registry::CreateView<...>().ForEach`.
- Produces: `Studio::WorkloadRunner` — `void Spawn(Preset, int count)`, `void Clear()`, `void Step(float dt)`, `size_t Spawned() const`, `uint64_t StepsRun() const`.

- [ ] **Step 1: Write `studio/WorkloadRunner.hpp`**

```cpp
#pragma once

#include <cstdlib>
#include <vector>

#include <Astra/Astra.hpp>

#include "Components.hpp"

namespace Studio
{
    enum class Preset : int { Movers = 0, Fighters = 1, Decor = 2, Mixed = 3 };
    inline const char* PresetNames[] = { "Movers (Pos+Vel)", "Fighters (Pos+Vel+Health)",
                                         "Decor (Pos+Sprite+Frozen)", "Mixed (round-robin)" };

    class WorkloadRunner
    {
    public:
        explicit WorkloadRunner(Astra::Registry& registry) : m_registry(registry) {}

        void Spawn(Preset preset, int count)
        {
            auto frand = [] { return float(std::rand()) / float(RAND_MAX) * 100.0f; };
            for (int i = 0; i < count; ++i)
            {
                Preset p = (preset == Preset::Mixed) ? Preset(i % 3) : preset;
                Astra::Entity e = Astra::Entity::Invalid();
                switch (p)
                {
                case Preset::Movers:
                    e = m_registry.CreateEntity<Position, Velocity>(
                        Position{frand(), frand(), 0}, Velocity{1, 1, 0});
                    break;
                case Preset::Fighters:
                    e = m_registry.CreateEntity<Position, Velocity, Health>(
                        Position{frand(), frand(), 0}, Velocity{-1, 2, 0}, Health{});
                    break;
                default:
                    e = m_registry.CreateEntity<Position, Sprite, Frozen>(
                        Position{frand(), frand(), 0}, Sprite{}, Frozen{});
                    break;
                }
                if (e.IsValid()) m_spawned.push_back(e);
            }
        }

        void Clear()
        {
            for (Astra::Entity e : m_spawned)
                if (m_registry.IsValid(e)) (void)m_registry.DestroyEntity(e);
            m_spawned.clear();
        }

        void Step(float dt)
        {
            auto view = m_registry.CreateView<Position, const Velocity>();
            view.ForEach([dt](Position& p, const Velocity& v)
            {
                p.x += v.dx * dt; p.y += v.dy * dt; p.z += v.dz * dt;
            });
            ++m_steps;
        }

        size_t   Spawned() const { return m_spawned.size(); }
        uint64_t StepsRun() const { return m_steps; }

    private:
        Astra::Registry& m_registry;
        std::vector<Astra::Entity> m_spawned;
        uint64_t m_steps = 0;
    };
}
```

(If `CreateEntity<Ts...>(values...)` value-construction overload signatures differ, fall back to `CreateEntity<Ts...>()` + `GetComponent<T>(e)` writes — check Registry.hpp's CreateEntity overloads and use what compiles; the panel behavior is identical.)

- [ ] **Step 2: Wire runner + panel into `StudioApp.hpp`**

In `StudioApp`: add member `WorkloadRunner m_runner{m_registry};`, delete the constructor seeding loops (keep a small `m_runner.Spawn(Preset::Mixed, 300);` in the ctor so first launch isn't blank), add `DrawWorkloadPanel()` to `RenderFrame()` (before the capture, so auto-step mutates then the same frame snapshots):

```cpp
        void RenderFrame()
        {
            if (m_autoStep) m_runner.Step(ImGui::GetIO().DeltaTime);
            m_snapshot = Astra::Debug::Capture(m_registry);
            DrawWorkloadPanel();
            DrawRegistryPanel();
            DrawArchetypesPanel();
        }

        void DrawWorkloadPanel()
        {
            ImGui::Begin("Workload");
            ImGui::Combo("Preset", &m_presetIndex, PresetNames, IM_ARRAYSIZE(PresetNames));
            ImGui::SliderInt("Count", &m_spawnCount, 100, 100000, "%d", ImGuiSliderFlags_Logarithmic);
            if (ImGui::Button("Spawn")) m_runner.Spawn(Preset(m_presetIndex), m_spawnCount);
            ImGui::SameLine();
            if (ImGui::Button("Clear")) m_runner.Clear();
            ImGui::SameLine();
            if (ImGui::Button("Step")) m_runner.Step(1.0f / 60.0f);
            ImGui::Checkbox("Auto-step (per frame)", &m_autoStep);
            ImGui::Text("Spawned: %zu   Steps: %llu",
                m_runner.Spawned(), (unsigned long long)m_runner.StepsRun());
            ImGui::Text("Frame: %.2f ms (%.0f FPS)",
                1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();
        }

        WorkloadRunner m_runner{m_registry};
        int m_presetIndex = int(Preset::Mixed);
        int m_spawnCount = 1000;
        bool m_autoStep = false;
    };
```

(Member-order note: `m_runner` must be declared AFTER `m_registry` so its `Registry&` is valid at construction.)

- [ ] **Step 3: Build and run**

Build Debug, launch. Expected: **Workload** panel; Spawn 10k Fighters → Registry/Archetypes tables update live (entity counts, chunk counts, bytes grow); Auto-step keeps FPS interactive; Clear drops counts back; Step increments the counter. Close.

- [ ] **Step 4: Commit**

```bash
git add studio/WorkloadRunner.hpp studio/StudioApp.hpp
git commit -m "feat(studio): WorkloadRunner + Workload panel (spawn/clear/step presets, live tables) (studio task 4)"
```

---

### Task 5: 3-config gate + docs stub

**Files:**
- Create: `studio/README.md`
- No production changes expected; fix any config-specific build breaks found.

- [ ] **Step 1: Build all three configs**

Build Debug, Release, Dist (full solution each). Expected: all green, including `AstraTest` (full suite run in Debug at minimum — no regressions) and `AstraStudio` in all three.

- [ ] **Step 2: Run Release studio once**

Launch `bin/Release-windows-x86_64/AstraStudio/AstraStudio.exe`; spawn 100k Movers with auto-step — confirm it stays interactive and tables update. Close.

- [ ] **Step 3: Write `studio/README.md`**

```markdown
# AstraStudio

Dockable ImGui tool for running simulation workloads against a live `Astra::Registry`
and inspecting archetypes, chunk/column layout, and memory statistics.

- Build: `premake5 vs2022`, then build the `AstraStudio` project (Astra.sln).
- Data access goes exclusively through the read-only facade
  `include/Astra/Debug/Inspector.hpp` (`Astra::Debug::Capture`) — tool code
  never touches engine internals directly.
- Panels: Registry (summary), Archetypes (table + column preview), Workload
  (spawn/clear/step presets).
- Deferred/planned: chunk memory-layout map + cache-line viz, entity browser,
  relations graph, Tracy profiling integration.
```

- [ ] **Step 4: Commit**

```bash
git add studio/README.md
git commit -m "docs(studio): README + 3-config gate (studio task 5)"
```

---

## Self-Review (completed during planning)

**Spec coverage:** §2 vendoring/no-glad → Task 1; §3 facade (verified APIs, layout-derived bytes, signature-from-mask, tags) → Task 2; §4 shell/StudioApp/panels/components/runner → Tasks 1, 3, 4; §5 premake → Task 1; §6 testing (facade unit tests + manual GUI smoke) → Tasks 2–5; §7 success criteria → Task 5 gate. Terminology: `RegistrySnapshot`/`RegistryPanel`/`Capture(Registry&)` used consistently; no "world".

**Placeholder scan:** none. Two explicitly-marked adapt points are verification instructions with concrete fallbacks (the `DockSpaceOverViewport` arity and the `CreateEntity` value-overloads), not TBDs — both name the exact alternative call.

**Type consistency:** `InspectorSnapshot{registry, archetypes}`, `ArchetypeInfo{signature, componentCount, tagCount, columns, chunks, entityCount, chunkCount, bytesUsed, bytesAllocated}`, `ColumnInfo{name, id, size, alignment, stride, isEnableable}` match between Task 2's header, Task 2's tests, and Task 3's panels. `WorkloadRunner{Spawn, Clear, Step, Spawned, StepsRun}` matches Task 4's panel usage. `Studio::` namespace throughout studio code.

**Known risks:** (1) vendored-revision API drift (docking-branch signatures) — both call sites that could drift carry compile-time-checkable fallbacks; (2) `GetArchetypes()` non-const forces `Capture(Registry&)` — documented in the spec, revisit if a const enumeration lands; (3) GLFW premake file list is the standard 3.4 set — if the vendored checkout differs, match `src/CMakeLists.txt`'s platform groups.
