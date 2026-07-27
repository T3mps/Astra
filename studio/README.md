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
