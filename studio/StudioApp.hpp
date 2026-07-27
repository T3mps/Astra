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
