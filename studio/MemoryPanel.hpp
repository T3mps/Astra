#pragma once

// Memory panel (spec 2026-07-28): chunk memory-layout / cache-line viz.
// Composite: chunk strip + always-on overview bar + Bytes/Entities tabs +
// legend/probe footer. Renders snapshot PODs only; the sole live-registry
// touch is CaptureChunkDetail for the selected chunk (O(one chunk)/frame).

#include <algorithm>
#include <bit>
#include <cstdint>
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

            if (selectedArchetype != m_lastArchetype)
            {
                m_lastArchetype = selectedArchetype;
                m_chunk = 0;
                m_probe = -1;
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

        int m_mode = 0;                 // 0 Bytes, 1 Entities
        int m_chunk = 0;
        int m_lastArchetype = -1;
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
