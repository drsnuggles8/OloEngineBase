#include "OloEnginePCH.h"
#include "SoundGraphEditorPanel.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
// GraphGeneration.h provides the full Prototype definition and the asset→prototype
// compile helper. SoundGraphAsset.h only forward-declares Prototype, so any TU that
// ever destroys a SoundGraphAsset (which the panel does via Ref<SoundGraphAsset>) needs
// the complete type for ~Ref to compile.
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/NodeSchema.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "../UndoRedo/EditorCommand.h"
#include "../UndoRedo/SoundGraphCommands.h"

#include <cstdio>

namespace OloEngine
{
    namespace
    {
        // Editor-only identifier for the graph-output pseudo-node. Matches the sentinel
        // the asset compiler uses to identify connections targeting the graph's output
        // endpoints. Wrapped in `namespace OloEngine` so unqualified `u64` / `f32` / `UUID`
        // resolve via the typedefs Base.h injects into the engine namespace.
        constexpr u64 kGraphOutputNodeIDValue = 0ULL;
        const UUID kGraphOutputNodeID{ kGraphOutputNodeIDValue };
        constexpr f32 kGraphOutputNodeWidth = 140.0f;
        constexpr f32 kGraphOutputNodeHeaderHeight = 26.0f;
        constexpr f32 kGraphOutputNodePinSpacing = 22.0f;
    } // namespace
} // namespace OloEngine

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    // =========================================================================
    // Main render entry
    // =========================================================================

    void SoundGraphEditorPanel::OnImGuiRender()
    {
        if (!m_IsOpen)
            return;

        ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
        std::string windowTitle = "Sound Graph Editor";
        if (!m_CurrentFilePath.empty())
        {
            windowTitle += " - " + m_CurrentFilePath.filename().string();
            if (m_IsDirty)
                windowTitle += " *";
        }
        windowTitle += "###SoundGraphEditor";

        if (!ImGui::Begin(windowTitle.c_str(), &m_IsOpen, ImGuiWindowFlags_MenuBar))
        {
            m_IsFocused = false;
            ImGui::End();
            return;
        }

        m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow);

        // Deferred load — display a placeholder for a frame before doing blocking I/O so
        // the open transition is visibly snappy.
        if (!m_PendingLoadPath.empty())
        {
            if (++m_PendingLoadFrameDelay >= 2)
            {
                PerformPendingLoad();
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Loading sound graph...");
                ImGui::End();
                return;
            }
        }

        DrawToolbar();

        f32 const availWidth = ImGui::GetContentRegionAvail().x;
        f32 const canvasWidth = (m_SelectedNodeID != 0) ? availWidth - s_PropertyPanelWidth : availWidth;

        ImGui::BeginChild("##SGRCanvas", ImVec2(canvasWidth, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        DrawCanvas();
        ImGui::EndChild();

        if (m_SelectedNodeID != 0)
        {
            ImGui::SameLine();
            ImGui::BeginChild("##SGRProperties", ImVec2(s_PropertyPanelWidth, 0), ImGuiChildFlags_Borders);
            DrawPropertyPanel();
            ImGui::EndChild();
        }

        // External-change reload dialog — raised by NotifyAssetReloaded when a file
        // arrives from the filewatch with the editor holding unsaved changes.
        if (m_ShowExternalReloadDialog)
        {
            ImGui::OpenPopup("External Change Detected##SGRReload");
            m_ShowExternalReloadDialog = false;
        }
        if (ImGui::BeginPopupModal("External Change Detected##SGRReload", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("This sound graph was modified outside the editor:");
            ImGui::TextDisabled("%s", m_PendingExternalReloadPath.string().c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("You also have unsaved changes in this panel.");
            ImGui::Spacing();
            if (ImGui::Button("Discard my changes & reload", ImVec2(240, 0)))
            {
                m_PendingLoadPath = m_PendingExternalReloadPath;
                m_PendingLoadHandle = m_PendingExternalReloadHandle;
                m_PendingLoadFrameDelay = 0;
                m_PendingExternalReloadPath.clear();
                m_PendingExternalReloadHandle = 0;
                m_IsDirty = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep my edits", ImVec2(140, 0)))
            {
                m_PendingExternalReloadPath.clear();
                m_PendingExternalReloadHandle = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Add Parameter dialog — opened from the Graph Input pseudo-node's right-click menu.
        if (m_ShowAddParamDialog)
        {
            ImGui::OpenPopup("Add Graph Parameter##SGRAddParam");
            m_ShowAddParamDialog = false;
        }
        // Rename Parameter dialog — opened from the per-parameter submenu's "Rename...".
        if (m_ShowRenameParamDialog)
        {
            ImGui::OpenPopup("Rename Graph Parameter##SGRRenameParam");
            m_ShowRenameParamDialog = false;
        }
        if (ImGui::BeginPopupModal("Rename Graph Parameter##SGRRenameParam", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextDisabled("Old name: %s", m_RenameParamOldName.c_str());
            ImGui::TextDisabled("New name");
            ImGui::InputText("##sgrrenameparam", m_RenameParamNewName, sizeof(m_RenameParamNewName));
            if (const bool canRename = m_GraphAsset && std::strlen(m_RenameParamNewName) > 0; ImGui::Button("Rename", ImVec2(120, 0)) && canRename)
            {
                std::string newName = m_RenameParamNewName;
                Ref<SoundGraphAsset> snap = SnapshotAsset();
                if (m_GraphAsset->RenameGraphInput(m_RenameParamOldName, newName))
                {
                    m_IsDirty = true;
                    PushSnapshot(std::move(snap),
                                 "Rename Parameter '" + m_RenameParamOldName + "' → '" + newName + "'");
                }
                m_RenameParamOldName.clear();
                std::memset(m_RenameParamNewName, 0, sizeof(m_RenameParamNewName));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                m_RenameParamOldName.clear();
                std::memset(m_RenameParamNewName, 0, sizeof(m_RenameParamNewName));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Add Graph Parameter##SGRAddParam", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextDisabled("Name");
            ImGui::InputText("##sgrnewparamname", m_NewParamName, sizeof(m_NewParamName));
            ImGui::TextDisabled("Type");
            // String order must match the index→string mapping below — keep in sync.
            const char* typeOptions[] = { "Float", "Int", "Bool" };
            ImGui::Combo("##sgrnewparamtype", &m_NewParamTypeIndex, typeOptions, IM_ARRAYSIZE(typeOptions));

            if (const bool canCreate = m_GraphAsset && std::strlen(m_NewParamName) > 0; ImGui::Button("Create", ImVec2(120, 0)) && canCreate)
            {
                std::string name = m_NewParamName;
                const char* typeStr = typeOptions[std::clamp(m_NewParamTypeIndex, 0, IM_ARRAYSIZE(typeOptions) - 1)];
                Ref<SoundGraphAsset> snap = SnapshotAsset();
                if (m_GraphAsset->AddGraphInput(name, typeStr))
                {
                    m_IsDirty = true;
                    PushSnapshot(std::move(snap), "Add Parameter '" + name + "'");
                }
                std::memset(m_NewParamName, 0, sizeof(m_NewParamName));
                m_NewParamTypeIndex = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void SoundGraphEditorPanel::DrawToolbar()
    {
        if (!ImGui::BeginMenuBar())
            return;

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New"))
                NewSoundGraph();
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
            {
                std::string filepath = FileDialogs::OpenFile("OloEngine Sound Graph (*.olosoundgraph)\0*.olosoundgraph\0All Files\0*.*\0");
                if (!filepath.empty())
                    OpenSoundGraph(std::filesystem::path(filepath));
            }
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_GraphAsset != nullptr))
                SaveSoundGraph();
            if (ImGui::MenuItem("Save As...", nullptr, false, m_GraphAsset != nullptr))
                SaveSoundGraphAs();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit", m_GraphAsset != nullptr))
        {
            const bool canUndo = m_CommandHistory && m_CommandHistory->CanUndo();
            const bool canRedo = m_CommandHistory && m_CommandHistory->CanRedo();
            std::string undoLabel = canUndo ? "Undo " + m_CommandHistory->GetUndoDescription() : "Undo";
            std::string redoLabel = canRedo ? "Redo " + m_CommandHistory->GetRedoDescription() : "Redo";
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
                Undo();
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
                Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, !m_SelectedNodes.empty()))
                CopySelectedNodes();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_Clipboard.empty()))
            {
                // The Edit menu doesn't have access to the canvas cursor; paste at a
                // sensible default world position (slightly offset from the origin) so the
                // result is at least visible. Ctrl+V on the canvas anchors at the cursor.
                PasteNodes({ 0.0f, 0.0f });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Selected", "Del", false, !m_SelectedNodes.empty()))
            {
                Ref<SoundGraphAsset> snap = SnapshotAsset();
                std::vector<UUID> toDelete(m_SelectedNodes.begin(), m_SelectedNodes.end());
                m_SelectedNodes.clear();
                m_SelectedNodeID = 0;
                for (UUID id : toDelete)
                    DeleteNode(id);
                PushSnapshot(std::move(snap), toDelete.size() == 1 ? "Delete Node" : "Delete Nodes");
            }
            ImGui::EndMenu();
        }

        if (m_GraphAsset)
        {
            // Live preview toggle. Compiles the current asset to a Prototype, wraps it in
            // a transient SoundGraphSound, and plays — so the user can iterate on the graph
            // without attaching it to a scene entity. Stop releases the SoundGraphSound.
            ImGui::TextDisabled("|");
            if (m_IsPreviewPlaying)
            {
                if (ImGui::SmallButton("Stop"))
                    StopPreview();
            }
            else
            {
                if (ImGui::SmallButton("Play"))
                    StartPreview();
            }

            ImGui::TextDisabled("|");
            ImGui::Text("Nodes: %zu  Connections: %zu", m_GraphAsset->GetNodeCount(), m_GraphAsset->GetConnectionCount());

            // Surface validation errors on the toolbar so the user can spot a misformed
            // graph at a glance instead of digging through the log for compile warnings.
            const auto errors = m_GraphAsset->GetValidationErrors();
            if (!errors.empty())
            {
                ImGui::TextDisabled("|");
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 120, 255));
                ImGui::Text("%zu issue%s", errors.size(), errors.size() == 1 ? "" : "s");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    for (const auto& err : errors)
                        ImGui::BulletText("%s", err.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("No graph loaded");
        }

        ImGui::EndMenuBar();
    }

    // =========================================================================
    // Canvas
    // =========================================================================

    void SoundGraphEditorPanel::DrawCanvas()
    {
        ImVec2 const canvasOrigin = ImGui::GetCursorScreenPos();
        ImVec2 const canvasSize = ImGui::GetContentRegionAvail();
        ImVec2 const canvasEnd = ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(canvasOrigin, canvasEnd, IM_COL32(30, 30, 35, 255));
        drawList->PushClipRect(canvasOrigin, canvasEnd, true);

        DrawGrid(drawList, canvasOrigin, canvasSize);
        DrawConnections(drawList, canvasOrigin);
        DrawNodes(drawList, canvasOrigin);
        DrawConnectionInProgress(drawList, canvasOrigin);

        // Box-select marquee. Drawn last so it overlays nodes during the drag.
        if (m_IsBoxSelecting)
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const ImVec2 rmin(std::min(m_BoxSelectStart.x, mp.x), std::min(m_BoxSelectStart.y, mp.y));
            const ImVec2 rmax(std::max(m_BoxSelectStart.x, mp.x), std::max(m_BoxSelectStart.y, mp.y));
            drawList->AddRectFilled(rmin, rmax, IM_COL32(120, 180, 255, 40));
            drawList->AddRect(rmin, rmax, IM_COL32(120, 180, 255, 200));
        }

        drawList->PopClipRect();

        ImGui::SetCursorScreenPos(canvasOrigin);
        ImGui::InvisibleButton("##sgrcanvas", canvasSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

        HandleCanvasInput(canvasOrigin, canvasSize);
        HandleNodeInteraction(canvasOrigin);
        HandleConnectionDrag(canvasOrigin);
        DrawContextMenu(canvasOrigin);
    }

    void SoundGraphEditorPanel::DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        f32 const gridStep = s_GridSize * m_Zoom;
        ImU32 const gridColor = IM_COL32(50, 50, 55, 255);
        ImU32 const gridColorMajor = IM_COL32(60, 60, 70, 255);

        f32 const offX = std::fmod(m_ScrollOffset.x * m_Zoom, gridStep);
        f32 const offY = std::fmod(m_ScrollOffset.y * m_Zoom, gridStep);

        for (f32 x = canvasOrigin.x + offX; x < canvasOrigin.x + canvasSize.x; x += gridStep)
        {
            int lineIndex = static_cast<int>((x - canvasOrigin.x - offX) / gridStep);
            drawList->AddLine(ImVec2(x, canvasOrigin.y), ImVec2(x, canvasOrigin.y + canvasSize.y),
                              (lineIndex % 4 == 0) ? gridColorMajor : gridColor);
        }
        for (f32 y = canvasOrigin.y + offY; y < canvasOrigin.y + canvasSize.y; y += gridStep)
        {
            int lineIndex = static_cast<int>((y - canvasOrigin.y - offY) / gridStep);
            drawList->AddLine(ImVec2(canvasOrigin.x, y), ImVec2(canvasOrigin.x + canvasSize.x, y),
                              (lineIndex % 4 == 0) ? gridColorMajor : gridColor);
        }
    }

    // =========================================================================
    // Node rendering
    // =========================================================================

    namespace
    {
        // Centralized geometry for the graph-output pseudo-node so renderer and hit-tester
        // agree on pin positions. Returns the screen-space rect (origin + size) and the two
        // input pin centers (OutLeft, OutRight).
        struct GraphOutputGeometry
        {
            ImVec2 NodePos;
            ImVec2 NodeSize;
            ImVec2 OutLeftPinPos;
            ImVec2 OutRightPinPos;
        };
    } // namespace

    static GraphOutputGeometry ComputeGraphOutputGeometry(const ImVec2& nodeScreenPos, f32 zoom)
    {
        GraphOutputGeometry g;
        g.NodePos = nodeScreenPos;
        const f32 width = kGraphOutputNodeWidth * zoom;
        const f32 height = (kGraphOutputNodeHeaderHeight + 3.0f * kGraphOutputNodePinSpacing) * zoom;
        g.NodeSize = ImVec2(width, height);
        g.OutLeftPinPos = ImVec2(nodeScreenPos.x,
                                 nodeScreenPos.y + (kGraphOutputNodeHeaderHeight + 1.0f * kGraphOutputNodePinSpacing) * zoom);
        g.OutRightPinPos = ImVec2(nodeScreenPos.x,
                                  nodeScreenPos.y + (kGraphOutputNodeHeaderHeight + 2.0f * kGraphOutputNodePinSpacing) * zoom);
        return g;
    }

    // Compute Bezier control points for a wire that flows out of an OUTPUT pin (right-
    // facing) and into an INPUT pin (left-facing). The horizontal tangent magnitude grows
    // with horizontal distance, clamped to a minimum so close-together pins still get a
    // gentle curve and a maximum so long-distance wires don't bulge absurdly. For
    // "back-routing" (target left of source) the tangents still point outward from each
    // pin, which produces a smooth S/loop shape instead of the degenerate near-straight
    // wire that strict mid-point control gives.
    static void ComputeWireControlPoints(const ImVec2& src, const ImVec2& dst,
                                         bool srcIsOutput, bool dstIsOutput,
                                         ImVec2& outCP1, ImVec2& outCP2)
    {
        constexpr f32 kMinTangent = 40.0f;
        constexpr f32 kMaxTangent = 220.0f;
        const f32 tangent = std::clamp(std::abs(dst.x - src.x) * 0.5f, kMinTangent, kMaxTangent);
        const f32 srcSign = srcIsOutput ? 1.0f : -1.0f;
        const f32 dstSign = dstIsOutput ? 1.0f : -1.0f;
        outCP1 = ImVec2(src.x + srcSign * tangent, src.y);
        outCP2 = ImVec2(dst.x + dstSign * tangent, dst.y);
    }

    void SoundGraphEditorPanel::DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        // Iterate via index-into-stored-vector so we can pass a non-const ref to DrawNode.
        // SoundGraphAsset doesn't expose a mutable accessor, so we cast around. This is
        // acceptable here because the editor *is* the writer of these fields; only the
        // m_NodeIdMap invariant matters (which we don't disturb just by editing PosX/PosY).
        auto& nodes = const_cast<std::vector<SoundGraphNodeData>&>(m_GraphAsset->GetNodes());
        for (auto& node : nodes)
        {
            DrawNode(drawList, canvasOrigin, node);
        }

        // Graph Input pseudo-node — output pins on the right side, one per graph parameter.
        // Drawn first so the (typically more-interacted-with) Output node visually wins.
        {
            const auto& inputs = m_GraphAsset->GetGraphInputs();
            const ImVec2 inNodeScreen = WorldToScreen(m_GraphInputNodePos, canvasOrigin);
            const f32 width = kGraphOutputNodeWidth * m_Zoom;
            const sizet pinCount = std::max<sizet>(inputs.size(), 1); // reserve at least one row even when empty
            const f32 height = (kGraphOutputNodeHeaderHeight + (pinCount + 1) * kGraphOutputNodePinSpacing) * m_Zoom;
            const ImVec2 nodeEnd(inNodeScreen.x + width, inNodeScreen.y + height);
            drawList->AddRectFilled(inNodeScreen, nodeEnd, IM_COL32(50, 45, 65, 240), 4.0f);
            const ImVec2 headerEnd(nodeEnd.x, inNodeScreen.y + kGraphOutputNodeHeaderHeight * m_Zoom);
            drawList->AddRectFilled(inNodeScreen, headerEnd, IM_COL32(110, 80, 160, 255), 4.0f, ImDrawFlags_RoundCornersTop);
            drawList->AddRect(inNodeScreen, nodeEnd, IM_COL32(180, 150, 220, 200), 4.0f, 0, 1.5f);
            const f32 fontSize = 13.0f * m_Zoom;
            const ImVec2 titlePos(inNodeScreen.x + 8.0f * m_Zoom, inNodeScreen.y + 5.0f * m_Zoom);
            drawList->AddText(nullptr, fontSize, titlePos, IM_COL32(255, 255, 255, 255), "Graph Input");
            if (inputs.empty())
            {
                const f32 hintFont = 11.0f * m_Zoom;
                drawList->AddText(nullptr, hintFont,
                                  ImVec2(inNodeScreen.x + 8.0f * m_Zoom,
                                         inNodeScreen.y + (kGraphOutputNodeHeaderHeight + kGraphOutputNodePinSpacing * 0.5f) * m_Zoom),
                                  IM_COL32(160, 160, 180, 220), "(right-click to add params)");
            }
            // Pin output on the right edge per parameter (sorted for stable order).
            std::vector<std::string> sortedNames;
            sortedNames.reserve(inputs.size());
            for (const auto& [name, _] : inputs)
                sortedNames.push_back(name);
            std::ranges::sort(sortedNames);
            const ImU32 pinFill = IM_COL32(180, 140, 230, 255);
            const ImU32 pinBorder = IM_COL32(200, 200, 200, 255);
            const f32 labelFont = 11.0f * m_Zoom;
            for (sizet i = 0; i < sortedNames.size(); ++i)
            {
                const f32 py = inNodeScreen.y + (kGraphOutputNodeHeaderHeight + (static_cast<f32>(i) + 1.0f) * kGraphOutputNodePinSpacing) * m_Zoom;
                const ImVec2 pinPos(nodeEnd.x, py);
                drawList->AddCircleFilled(pinPos, s_PinRadius * m_Zoom, pinFill);
                drawList->AddCircle(pinPos, s_PinRadius * m_Zoom, pinBorder);
                const ImVec2 textSize = ImGui::CalcTextSize(sortedNames[i].c_str());
                const f32 scaledW = textSize.x * (labelFont / ImGui::GetFontSize());
                drawList->AddText(nullptr, labelFont,
                                  ImVec2(pinPos.x - s_PinRadius * m_Zoom - 4.0f * m_Zoom - scaledW,
                                         pinPos.y - labelFont * 0.5f),
                                  IM_COL32(200, 200, 200, 255), sortedNames[i].c_str());
            }

            // Tooltip on hover. The pseudo-node is drawn via ImDrawList (no ImGui item to
            // attach IsItemHovered to), so we hit-test the rect manually. ImGui::SetTooltip
            // is fine to call here as long as the canvas window is hovered.
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                mp.x >= inNodeScreen.x && mp.x <= nodeEnd.x &&
                mp.y >= inNodeScreen.y && mp.y <= nodeEnd.y)
            {
                ImGui::SetTooltip("Graph Input (pseudo-node — cannot be deleted)\n\n"
                                  "Exposes graph parameters as output pins. Wire them into\n"
                                  "nodes to drive values from outside the graph.\n\n"
                                  "Right-click to add or rename parameters.");
            }
        }

        // Graph-output pseudo-node. Drawn last so it always renders in front of real nodes.
        const ImVec2 outNodePos = WorldToScreen(m_GraphOutputNodePos, canvasOrigin);
        const GraphOutputGeometry g = ComputeGraphOutputGeometry(outNodePos, m_Zoom);
        const ImVec2 nodeEnd(g.NodePos.x + g.NodeSize.x, g.NodePos.y + g.NodeSize.y);

        drawList->AddRectFilled(g.NodePos, nodeEnd, IM_COL32(35, 55, 50, 240), 4.0f);
        const ImVec2 headerEnd(nodeEnd.x, g.NodePos.y + kGraphOutputNodeHeaderHeight * m_Zoom);
        drawList->AddRectFilled(g.NodePos, headerEnd, IM_COL32(60, 130, 100, 255), 4.0f, ImDrawFlags_RoundCornersTop);
        drawList->AddRect(g.NodePos, nodeEnd, IM_COL32(140, 200, 170, 200), 4.0f, 0, 1.5f);

        const f32 fontSize = 13.0f * m_Zoom;
        const ImVec2 titlePos(g.NodePos.x + 8.0f * m_Zoom, g.NodePos.y + 5.0f * m_Zoom);
        drawList->AddText(nullptr, fontSize, titlePos, IM_COL32(255, 255, 255, 255), "Graph Output");

        // Pin circles + labels.
        const ImU32 pinColor = IM_COL32(90, 170, 220, 255);
        const ImU32 pinBorder = IM_COL32(200, 200, 200, 255);
        const f32 labelFont = 11.0f * m_Zoom;
        drawList->AddCircleFilled(g.OutLeftPinPos, s_PinRadius * m_Zoom, pinColor);
        drawList->AddCircle(g.OutLeftPinPos, s_PinRadius * m_Zoom, pinBorder);
        drawList->AddText(nullptr, labelFont,
                          ImVec2(g.OutLeftPinPos.x + s_PinRadius * m_Zoom + 4.0f * m_Zoom,
                                 g.OutLeftPinPos.y - labelFont * 0.5f),
                          IM_COL32(200, 200, 200, 255), "OutLeft");

        drawList->AddCircleFilled(g.OutRightPinPos, s_PinRadius * m_Zoom, pinColor);
        drawList->AddCircle(g.OutRightPinPos, s_PinRadius * m_Zoom, pinBorder);
        drawList->AddText(nullptr, labelFont,
                          ImVec2(g.OutRightPinPos.x + s_PinRadius * m_Zoom + 4.0f * m_Zoom,
                                 g.OutRightPinPos.y - labelFont * 0.5f),
                          IM_COL32(200, 200, 200, 255), "OutRight");

        // Tooltip on hover (matches the Graph Input pseudo-node's hover behavior).
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            mp.x >= g.NodePos.x && mp.x <= nodeEnd.x &&
            mp.y >= g.NodePos.y && mp.y <= nodeEnd.y)
        {
            ImGui::SetTooltip("Graph Output (pseudo-node — cannot be deleted)\n\n"
                              "Connect node outputs to OutLeft / OutRight to route audio\n"
                              "to the engine's mixer. Wires landing here become the graph's\n"
                              "stereo output.");
        }
    }

    // Geometry helper for the Graph Input pseudo-node. Returns the rect plus the screen
    // position of the named output pin (or {0,0} + foundOut=false if unknown).
    struct GraphInputGeometry
    {
        ImVec2 NodePos;
        ImVec2 NodeSize;
    };

    static GraphInputGeometry ComputeGraphInputGeometry(const ImVec2& nodeScreenPos, sizet pinCount, f32 zoom)
    {
        GraphInputGeometry g;
        g.NodePos = nodeScreenPos;
        const f32 width = kGraphOutputNodeWidth * zoom;
        const sizet rows = std::max<sizet>(pinCount, 1);
        const f32 height = (kGraphOutputNodeHeaderHeight + (rows + 1) * kGraphOutputNodePinSpacing) * zoom;
        g.NodeSize = ImVec2(width, height);
        return g;
    }

    // Pin lookup helper for the Graph Input pseudo-node — finds an output pin by name and
    // returns its screen position. Pins are spaced down the right edge in the same order
    // the renderer uses (sorted by name for stability).
    static bool FindGraphInputPin(const SoundGraphAsset& asset, const ImVec2& nodeScreenPos,
                                  const std::string& name, f32 zoom, ImVec2& outPos)
    {
        const auto& inputs = asset.GetGraphInputs();
        std::vector<std::string> sorted;
        sorted.reserve(inputs.size());
        for (const auto& [n, _] : inputs)
            sorted.push_back(n);
        std::ranges::sort(sorted);
        auto it = std::ranges::find(sorted, name);
        if (it == sorted.end())
            return false;
        const sizet idx = static_cast<sizet>(std::distance(sorted.begin(), it));
        const f32 width = kGraphOutputNodeWidth * zoom;
        outPos = ImVec2(nodeScreenPos.x + width,
                        nodeScreenPos.y + (kGraphOutputNodeHeaderHeight + (static_cast<f32>(idx) + 1.0f) * kGraphOutputNodePinSpacing) * zoom);
        return true;
    }

    void SoundGraphEditorPanel::DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, SoundGraphNodeData& node)
    {
        glm::vec2 const worldPos(node.m_PosX, node.m_PosY);
        ImVec2 const nodePos = WorldToScreen(worldPos, canvasOrigin);
        ImVec2 const nodeSize = GetNodeSize(node);
        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        // A node is selected if it's the primary selection (drives the property sidebar)
        // OR it's a member of the multi-select set. Visually they look the same — the
        // selection border doesn't distinguish primary from secondary right now.
        bool const isSelected = (m_SelectedNodeID == node.m_ID) || (m_SelectedNodes.count(node.m_ID) > 0);

        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.m_Type), 4.0f);

        ImVec2 const headerEnd = ImVec2(nodeEnd.x, nodePos.y + s_HeaderHeight * m_Zoom);
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.m_Type), 4.0f, ImDrawFlags_RoundCornersTop);

        if (isSelected)
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 255), 4.0f, 0, 2.0f);

        f32 const fontSize = 13.0f * m_Zoom;
        std::string title = node.m_Name.empty() ? node.m_Type : node.m_Name;
        ImVec2 const textPos = ImVec2(nodePos.x + 8.0f * m_Zoom, nodePos.y + 5.0f * m_Zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 255), title.c_str());

        // Pins discovered from the connections referencing this node.
        auto pins = GetNodePins(node, nodePos);
        for (const auto& pin : pins)
        {
            ImU32 const fill = pin.IsEvent ? IM_COL32(220, 180, 90, 255) : IM_COL32(90, 170, 220, 255);
            drawList->AddCircleFilled(pin.Position, s_PinRadius * m_Zoom, fill);
            drawList->AddCircle(pin.Position, s_PinRadius * m_Zoom, IM_COL32(200, 200, 200, 255));

            f32 const labelFontSize = 11.0f * m_Zoom;
            if (pin.IsOutput)
            {
                ImVec2 textSize = ImGui::CalcTextSize(pin.Name.c_str());
                textSize.x *= (labelFontSize / ImGui::GetFontSize());
                drawList->AddText(nullptr, labelFontSize,
                                  ImVec2(pin.Position.x - s_PinRadius * m_Zoom - 4.0f * m_Zoom - textSize.x,
                                         pin.Position.y - labelFontSize * 0.5f),
                                  IM_COL32(200, 200, 200, 255), pin.Name.c_str());
            }
            else
            {
                drawList->AddText(nullptr, labelFontSize,
                                  ImVec2(pin.Position.x + s_PinRadius * m_Zoom + 4.0f * m_Zoom,
                                         pin.Position.y - labelFontSize * 0.5f),
                                  IM_COL32(200, 200, 200, 255), pin.Name.c_str());
            }
        }
    }

    ImVec2 SoundGraphEditorPanel::GetNodeSize(const SoundGraphNodeData& node) const
    {
        // Width is fixed; height grows with the larger of the two pin sides (inputs vs
        // outputs). Pins come from the schema (declared pins) merged with whatever
        // connections reference — same union GetNodePins uses, so the node body always
        // contains every visible pin row.
        std::unordered_set<std::string> inputs;
        std::unordered_set<std::string> outputs;
        if (const Audio::SoundGraph::NodePinSchema* pinSchema = Audio::SoundGraph::GetNodePinSchema(node.m_Type))
        {
            for (const auto& p : pinSchema->Inputs)
                inputs.insert(p.Name);
            for (const auto& p : pinSchema->Outputs)
                outputs.insert(p.Name);
        }
        if (m_GraphAsset)
        {
            for (const auto& c : m_GraphAsset->GetConnections())
            {
                if (c.m_TargetNodeID == node.m_ID)
                    inputs.insert(c.m_TargetEndpoint);
                if (c.m_SourceNodeID == node.m_ID)
                    outputs.insert(c.m_SourceEndpoint);
            }
        }
        const sizet inputCount = inputs.size();
        const sizet outputCount = outputs.size();

        f32 const pinRows = static_cast<f32>(std::max<sizet>(inputCount, outputCount));
        f32 const bodyHeight = s_HeaderHeight + (pinRows + 1) * s_PinSpacing;
        return ImVec2(s_NodeWidth * m_Zoom, bodyHeight * m_Zoom);
    }

    std::vector<SoundGraphEditorPanel::PinInfo> SoundGraphEditorPanel::GetNodePins(const SoundGraphNodeData& node, const ImVec2& nodeScreenPos) const
    {
        std::vector<PinInfo> result;
        if (!m_GraphAsset)
            return result;

        ImVec2 const nodeSize = GetNodeSize(node);

        // Pin enumeration is schema-driven so a freshly-placed node shows all wireable
        // pins immediately. We seed inputs/outputs from NodePinSchema if available, then
        // append any extra endpoints that connections reference but the schema doesn't
        // list (covers user-extended node types and any drift between schema and runtime).
        // Use vectors (not sets) so the on-screen order is stable across frames.
        struct EndpointKey
        {
            std::string Name;
            bool IsEvent;
        };
        std::vector<EndpointKey> inputs;
        std::vector<EndpointKey> outputs;
        std::unordered_set<std::string> seenIn, seenOut;

        if (const Audio::SoundGraph::NodePinSchema* pinSchema = Audio::SoundGraph::GetNodePinSchema(node.m_Type))
        {
            for (const auto& pin : pinSchema->Inputs)
            {
                if (seenIn.insert(pin.Name).second)
                    inputs.push_back({ pin.Name, pin.IsEvent });
            }
            for (const auto& pin : pinSchema->Outputs)
            {
                if (seenOut.insert(pin.Name).second)
                    outputs.push_back({ pin.Name, pin.IsEvent });
            }
        }

        for (const auto& c : m_GraphAsset->GetConnections())
        {
            if (c.m_TargetNodeID == node.m_ID && seenIn.insert(c.m_TargetEndpoint).second)
                inputs.push_back({ c.m_TargetEndpoint, c.m_IsEvent });
            if (c.m_SourceNodeID == node.m_ID && seenOut.insert(c.m_SourceEndpoint).second)
                outputs.push_back({ c.m_SourceEndpoint, c.m_IsEvent });
        }

        // Inputs along the left edge, outputs along the right edge.
        for (sizet i = 0; i < inputs.size(); ++i)
        {
            PinInfo pin;
            pin.Position = ImVec2(nodeScreenPos.x,
                                  nodeScreenPos.y + (s_HeaderHeight + (static_cast<f32>(i) + 1) * s_PinSpacing) * m_Zoom);
            pin.NodeID = node.m_ID;
            pin.Name = inputs[i].Name;
            pin.IsOutput = false;
            pin.IsEvent = inputs[i].IsEvent;
            result.push_back(pin);
        }
        for (sizet i = 0; i < outputs.size(); ++i)
        {
            PinInfo pin;
            pin.Position = ImVec2(nodeScreenPos.x + nodeSize.x,
                                  nodeScreenPos.y + (s_HeaderHeight + (static_cast<f32>(i) + 1) * s_PinSpacing) * m_Zoom);
            pin.NodeID = node.m_ID;
            pin.Name = outputs[i].Name;
            pin.IsOutput = true;
            pin.IsEvent = outputs[i].IsEvent;
            result.push_back(pin);
        }
        return result;
    }

    // =========================================================================
    // Connection rendering
    // =========================================================================

    // Resolves a (nodeID, endpointName, isOutputDirection) triplet to a screen-space pin
    // position. For real nodes, looks up pins via GetNodePins. For the graph-output
    // pseudo-node (UUID(0)), uses the fixed OutLeft/OutRight geometry. Member function so
    // we can call the panel's private helpers without friend gymnastics.
    bool SoundGraphEditorPanel::ResolvePinScreenPos(UUID nodeID, const std::string& endpoint,
                                                    bool wantOutput, const ImVec2& canvasOrigin,
                                                    ImVec2& outPos) const
    {
        if (nodeID == kGraphOutputNodeID)
        {
            // Both Graph Output (input pins) and Graph Input (output pins) share UUID(0);
            // wantOutput disambiguates which pseudo-node's geometry to consult.
            if (wantOutput)
            {
                if (!m_GraphAsset)
                    return false;
                const ImVec2 inNodeScreen = WorldToScreen(m_GraphInputNodePos, canvasOrigin);
                return FindGraphInputPin(*m_GraphAsset, inNodeScreen, endpoint, m_Zoom, outPos);
            }
            const ImVec2 outNodeScreen = WorldToScreen(m_GraphOutputNodePos, canvasOrigin);
            const GraphOutputGeometry g = ComputeGraphOutputGeometry(outNodeScreen, m_Zoom);
            if (endpoint == "OutLeft")
            {
                outPos = g.OutLeftPinPos;
                return true;
            }
            if (endpoint == "OutRight")
            {
                outPos = g.OutRightPinPos;
                return true;
            }
            return false;
        }

        if (!m_GraphAsset)
            return false;
        const auto* node = m_GraphAsset->GetNode(nodeID);
        if (!node)
            return false;
        auto pins = GetNodePins(*node, WorldToScreen({ node->m_PosX, node->m_PosY }, canvasOrigin));
        for (const auto& pin : pins)
        {
            if (pin.IsOutput == wantOutput && pin.Name == endpoint)
            {
                outPos = pin.Position;
                return true;
            }
        }
        return false;
    }

    sizet SoundGraphEditorPanel::HitTestConnection(const ImVec2& mousePos, const ImVec2& canvasOrigin,
                                                   f32 hitDistancePx) const
    {
        if (!m_GraphAsset)
            return static_cast<sizet>(-1);

        const auto& connections = m_GraphAsset->GetConnections();
        const f32 hitSq = hitDistancePx * hitDistancePx;

        for (sizet i = 0; i < connections.size(); ++i)
        {
            const auto& c = connections[i];
            ImVec2 srcPos{}, dstPos{};
            if (!ResolvePinScreenPos(c.m_SourceNodeID, c.m_SourceEndpoint, true, canvasOrigin, srcPos))
                continue;
            if (!ResolvePinScreenPos(c.m_TargetNodeID, c.m_TargetEndpoint, false, canvasOrigin, dstPos))
                continue;

            // Sample the bezier as kSamples segments; compute point-to-segment distance
            // against each. 16 samples is more than enough for the click-detection accuracy
            // we need (typically a few pixels) at typical zoom levels. Control points must
            // match those used by DrawConnections or hit-test won't line up with the
            // visible wire — especially for back-routed wires.
            constexpr i32 kSamples = 16;
            ImVec2 cp1{}, cp2{};
            ComputeWireControlPoints(srcPos, dstPos, /*srcIsOutput=*/true, /*dstIsOutput=*/false, cp1, cp2);

            ImVec2 prev = srcPos;
            for (i32 s = 1; s <= kSamples; ++s)
            {
                const f32 t = static_cast<f32>(s) / static_cast<f32>(kSamples);
                const f32 u = 1.0f - t;
                const f32 b0 = u * u * u;
                const f32 b1 = 3.0f * u * u * t;
                const f32 b2 = 3.0f * u * t * t;
                const f32 b3 = t * t * t;
                const ImVec2 cur(b0 * srcPos.x + b1 * cp1.x + b2 * cp2.x + b3 * dstPos.x,
                                 b0 * srcPos.y + b1 * cp1.y + b2 * cp2.y + b3 * dstPos.y);

                // Point-to-segment distance. ax + b*y form via vector projection.
                const f32 segX = cur.x - prev.x;
                const f32 segY = cur.y - prev.y;
                if (const f32 segLenSq = segX * segX + segY * segY; segLenSq > 1e-6f)
                {
                    const f32 pmX = mousePos.x - prev.x;
                    const f32 pmY = mousePos.y - prev.y;
                    const f32 tt = std::clamp((pmX * segX + pmY * segY) / segLenSq, 0.0f, 1.0f);
                    const f32 projX = prev.x + tt * segX;
                    const f32 projY = prev.y + tt * segY;
                    const f32 dxp = mousePos.x - projX;
                    const f32 dyp = mousePos.y - projY;
                    if (dxp * dxp + dyp * dyp <= hitSq)
                        return i;
                }
                prev = cur;
            }
        }
        return static_cast<sizet>(-1);
    }

    void SoundGraphEditorPanel::DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        // Determine which wire (if any) the mouse is hovering. Skip this when the canvas
        // is busy with other interactions so highlights don't flicker mid-drag.
        sizet hoveredIndex = static_cast<sizet>(-1);
        if (const bool canHoverWire = !m_IsDraggingConnection && !m_IsDraggingNode && !m_IsPanning && !m_IsBoxSelecting; canHoverWire && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
            hoveredIndex = HitTestConnection(ImGui::GetIO().MousePos, canvasOrigin, 6.0f * m_Zoom);

        const auto& connections = m_GraphAsset->GetConnections();
        for (sizet i = 0; i < connections.size(); ++i)
        {
            const auto& connection = connections[i];
            ImVec2 srcPos{}, dstPos{};
            const bool foundSrc = ResolvePinScreenPos(connection.m_SourceNodeID, connection.m_SourceEndpoint,
                                                      /*wantOutput=*/true, canvasOrigin, srcPos);
            const bool foundDst = ResolvePinScreenPos(connection.m_TargetNodeID, connection.m_TargetEndpoint,
                                                      /*wantOutput=*/false, canvasOrigin, dstPos);
            if (!foundSrc || !foundDst)
                continue;

            const bool isHovered = (i == hoveredIndex);
            ImU32 wireColor;
            if (connection.m_IsEvent)
                wireColor = isHovered ? IM_COL32(255, 220, 130, 255) : IM_COL32(220, 180, 90, 255);
            else
                wireColor = isHovered ? IM_COL32(200, 230, 255, 255) : IM_COL32(150, 200, 250, 255);
            f32 const thickness = (isHovered ? 3.0f : 2.0f) * m_Zoom;

            ImVec2 cp1{}, cp2{};
            ComputeWireControlPoints(srcPos, dstPos, /*srcIsOutput=*/true, /*dstIsOutput=*/false, cp1, cp2);
            drawList->AddBezierCubic(srcPos, cp1, cp2, dstPos, wireColor, thickness);
        }
    }

    void SoundGraphEditorPanel::DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_IsDraggingConnection || !m_GraphAsset)
            return;

        // Uses ResolvePinScreenPos so both real nodes and the graph-output pseudo-node
        // (m_DragStartNodeID == kGraphOutputNodeID) are handled uniformly.
        ImVec2 startPinPos{};
        if (!ResolvePinScreenPos(m_DragStartNodeID, m_DragStartEndpoint, m_DragStartIsOutput,
                                 canvasOrigin, startPinPos))
            return;

        // The drag end is "the other side" — if the user grabbed an output, the floating
        // end is acting as an input, and vice versa.
        ImVec2 cp1{}, cp2{};
        ComputeWireControlPoints(startPinPos, m_DragEndPos, /*srcIsOutput=*/m_DragStartIsOutput,
                                 /*dstIsOutput=*/!m_DragStartIsOutput, cp1, cp2);
        drawList->AddBezierCubic(startPinPos, cp1, cp2, m_DragEndPos,
                                 IM_COL32(255, 255, 255, 200), 2.0f * m_Zoom);
    }

    // =========================================================================
    // Node + canvas interaction
    // =========================================================================

    void SoundGraphEditorPanel::HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        (void)canvasOrigin;
        (void)canvasSize;

        if (!ImGui::IsItemHovered() && !m_IsPanning && !m_IsDraggingNode)
            return;

        // Middle-mouse pan.
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        {
            ImVec2 const delta = ImGui::GetIO().MouseDelta;
            m_ScrollOffset.x += delta.x / m_Zoom;
            m_ScrollOffset.y += delta.y / m_Zoom;
            m_IsPanning = true;
        }
        else
        {
            m_IsPanning = false;
        }

        // Scroll-wheel zoom around the cursor.
        if (ImGui::IsItemHovered())
        {
            f32 const wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                f32 const oldZoom = m_Zoom;
                m_Zoom = std::clamp(m_Zoom * (1.0f + wheel * 0.1f), 0.25f, 3.0f);
                if (m_Zoom != oldZoom)
                {
                    ImVec2 const mousePos = ImGui::GetIO().MousePos;
                    glm::vec2 const worldBefore = ScreenToWorld(mousePos, canvasOrigin);
                    glm::vec2 const worldAfter = ScreenToWorld(mousePos, canvasOrigin);
                    (void)worldAfter; // Centering on cursor is a future polish item.
                    (void)worldBefore;
                }
            }
        }
    }

    void SoundGraphEditorPanel::HandleNodeInteraction(const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        // Delete key: nuke the selected node(s). Only when the panel has focus so we don't
        // hijack Delete in other panels (Scene hierarchy, etc.) that share the editor window.
        if (m_IsFocused && !m_SelectedNodes.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            Ref<SoundGraphAsset> snap = SnapshotAsset();
            std::vector<UUID> toDelete(m_SelectedNodes.begin(), m_SelectedNodes.end());
            m_SelectedNodes.clear();
            m_SelectedNodeID = 0;
            for (UUID id : toDelete)
                DeleteNode(id);
            PushSnapshot(std::move(snap), toDelete.size() == 1 ? "Delete Node" : "Delete Nodes");
        }

        // Keyboard shortcuts. Limited to when the panel has focus so they don't collide
        // with the global editor shortcuts (which are also Ctrl+Z / Ctrl+Y / Ctrl+C / V).
        if (m_IsFocused && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z))
                Undo();
            if (ImGui::IsKeyPressed(ImGuiKey_Y))
                Redo();
            if (ImGui::IsKeyPressed(ImGuiKey_C))
                CopySelectedNodes();
            if (ImGui::IsKeyPressed(ImGuiKey_V))
                PasteNodes(ScreenToWorld(ImGui::GetIO().MousePos, canvasOrigin));
        }

        if (!ImGui::IsItemHovered() && !m_IsDraggingNode)
            return;

        ImVec2 const mousePos = ImGui::GetIO().MousePos;

        // Right-click hit-test runs BEFORE the context menu opens. Priority order:
        // Graph Output pseudo-node > Graph Input pseudo-node > real node > wire. Wires last
        // because they're thin and easy to accidentally hit when aiming at a node.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_RightClickNodeID = 0;
            m_RightClickGraphInput = false;
            m_RightClickConnectionIndex = static_cast<sizet>(-1);

            const ImVec2 outNodeScreen = WorldToScreen(m_GraphOutputNodePos, canvasOrigin);
            const GraphOutputGeometry g = ComputeGraphOutputGeometry(outNodeScreen, m_Zoom);
            const ImVec2 inNodeScreen = WorldToScreen(m_GraphInputNodePos, canvasOrigin);
            const GraphInputGeometry gi = ComputeGraphInputGeometry(inNodeScreen,
                                                                    m_GraphAsset->GetGraphInputs().size(), m_Zoom);
            if (mousePos.x >= g.NodePos.x && mousePos.x <= g.NodePos.x + g.NodeSize.x &&
                mousePos.y >= g.NodePos.y && mousePos.y <= g.NodePos.y + g.NodeSize.y)
            {
                m_RightClickNodeID = kGraphOutputNodeID;
            }
            else if (mousePos.x >= gi.NodePos.x && mousePos.x <= gi.NodePos.x + gi.NodeSize.x &&
                     mousePos.y >= gi.NodePos.y && mousePos.y <= gi.NodePos.y + gi.NodeSize.y)
            {
                m_RightClickGraphInput = true;
            }
            else
            {
                const auto& nodes = m_GraphAsset->GetNodes();
                for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
                {
                    const auto& node = *it;
                    ImVec2 const nodePos = WorldToScreen({ node.m_PosX, node.m_PosY }, canvasOrigin);
                    ImVec2 const nodeSize = GetNodeSize(node);
                    if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                        mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y)
                    {
                        m_RightClickNodeID = node.m_ID;
                        m_SelectedNodeID = node.m_ID; // Select what you right-click on.
                        break;
                    }
                }

                // No node hit? Try wires next.
                if (m_RightClickNodeID == 0)
                {
                    m_RightClickConnectionIndex = HitTestConnection(mousePos, canvasOrigin, 6.0f * m_Zoom);
                }
            }
        }

        // Left-click: hit-test nodes (top-down so visually-front nodes win) for selection,
        // and check pin proximity to start a connection drag.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            UUID hitNode = 0;
            bool startedConnection = false;

            // Graph-output pseudo-node is drawn LAST, so it wins front-most hit tests.
            // Pin hit-test for the OutLeft / OutRight input pins; body hit-test starts a
            // pseudo-node drag.
            {
                const ImVec2 outNodeScreen = WorldToScreen(m_GraphOutputNodePos, canvasOrigin);
                const GraphOutputGeometry g = ComputeGraphOutputGeometry(outNodeScreen, m_Zoom);
                const f32 hitRadius = s_PinRadius * m_Zoom + 3.0f;
                struct
                {
                    const char* Name;
                    ImVec2 Pos;
                } pinList[2] = {
                    { "OutLeft", g.OutLeftPinPos },
                    { "OutRight", g.OutRightPinPos },
                };
                for (auto& p : pinList)
                {
                    const f32 dx = mousePos.x - p.Pos.x;
                    const f32 dy = mousePos.y - p.Pos.y;
                    if (dx * dx + dy * dy <= hitRadius * hitRadius)
                    {
                        m_IsDraggingConnection = true;
                        m_DragStartNodeID = kGraphOutputNodeID;
                        m_DragStartEndpoint = p.Name;
                        m_DragStartIsOutput = false; // pseudo-node pins are graph inputs
                        m_DragStartIsEvent = false;
                        m_DragEndPos = mousePos;
                        startedConnection = true;
                        break;
                    }
                }
                if (!startedConnection)
                {
                    if (mousePos.x >= g.NodePos.x && mousePos.x <= g.NodePos.x + g.NodeSize.x &&
                        mousePos.y >= g.NodePos.y && mousePos.y <= g.NodePos.y + g.NodeSize.y)
                    {
                        // Pseudo-node body drag — moves the pseudo-node around the canvas.
                        m_DraggingGraphOutputNode = true;
                        m_GraphOutputDragStart = m_GraphOutputNodePos;
                        m_DragMouseStartPos = mousePos;
                        // Pretend we "hit" this slot so the real-node loop below skips.
                        startedConnection = true;
                    }
                }
            }

            // Graph-input pseudo-node hit-test. Pins on the right edge, output direction
            // (a drag from one of these creates a "from graph input" connection — source
            // node id = 0 in the asset, GraphValue_NodeValue at compile time).
            if (!startedConnection)
            {
                const auto& inputs = m_GraphAsset->GetGraphInputs();
                if (!inputs.empty())
                {
                    const ImVec2 inNodeScreen = WorldToScreen(m_GraphInputNodePos, canvasOrigin);
                    const GraphInputGeometry gi = ComputeGraphInputGeometry(inNodeScreen, inputs.size(), m_Zoom);
                    const f32 hitRadius = s_PinRadius * m_Zoom + 3.0f;

                    // Sorted name order matches the renderer for stable pin positions.
                    std::vector<std::string> sorted;
                    sorted.reserve(inputs.size());
                    for (const auto& [n, _] : inputs)
                        sorted.push_back(n);
                    std::ranges::sort(sorted);
                    for (sizet i = 0; i < sorted.size(); ++i)
                    {
                        const f32 py = inNodeScreen.y + (kGraphOutputNodeHeaderHeight + (static_cast<f32>(i) + 1.0f) * kGraphOutputNodePinSpacing) * m_Zoom;
                        const ImVec2 pinPos(gi.NodePos.x + gi.NodeSize.x, py);
                        const f32 dx = mousePos.x - pinPos.x;
                        const f32 dy = mousePos.y - pinPos.y;
                        if (dx * dx + dy * dy <= hitRadius * hitRadius)
                        {
                            m_IsDraggingConnection = true;
                            m_DragStartNodeID = kGraphOutputNodeID; // sentinel 0
                            m_DragStartEndpoint = sorted[i];
                            m_DragStartIsOutput = true; // Graph Input pins are graph "outputs" to the editor
                            m_DragStartIsEvent = false;
                            m_DragEndPos = mousePos;
                            startedConnection = true;
                            break;
                        }
                    }
                    if (!startedConnection &&
                        mousePos.x >= gi.NodePos.x && mousePos.x <= gi.NodePos.x + gi.NodeSize.x &&
                        mousePos.y >= gi.NodePos.y && mousePos.y <= gi.NodePos.y + gi.NodeSize.y)
                    {
                        m_DraggingGraphInputNode = true;
                        m_GraphInputDragStart = m_GraphInputNodePos;
                        m_DragMouseStartPos = mousePos;
                        startedConnection = true;
                    }
                }
                else
                {
                    // Empty input node still gets a body hit-test (allows dragging the
                    // empty stub around before any params are added).
                    const ImVec2 inNodeScreen = WorldToScreen(m_GraphInputNodePos, canvasOrigin);
                    const GraphInputGeometry gi = ComputeGraphInputGeometry(inNodeScreen, 0, m_Zoom);
                    if (mousePos.x >= gi.NodePos.x && mousePos.x <= gi.NodePos.x + gi.NodeSize.x &&
                        mousePos.y >= gi.NodePos.y && mousePos.y <= gi.NodePos.y + gi.NodeSize.y)
                    {
                        m_DraggingGraphInputNode = true;
                        m_GraphInputDragStart = m_GraphInputNodePos;
                        m_DragMouseStartPos = mousePos;
                        startedConnection = true;
                    }
                }
            }

            // Iterate in reverse: last-drawn (frontmost) gets priority.
            const auto& nodes = m_GraphAsset->GetNodes();
            for (auto it = nodes.rbegin(); !startedConnection && it != nodes.rend(); ++it)
            {
                const auto& node = *it;
                ImVec2 const nodePos = WorldToScreen({ node.m_PosX, node.m_PosY }, canvasOrigin);
                ImVec2 const nodeSize = GetNodeSize(node);

                // Pin hit-test first — pins extend slightly outside the node rect.
                auto pins = GetNodePins(node, nodePos);
                for (const auto& pin : pins)
                {
                    f32 const dx = mousePos.x - pin.Position.x;
                    f32 const dy = mousePos.y - pin.Position.y;
                    f32 const hitRadius = s_PinRadius * m_Zoom + 3.0f;
                    if (dx * dx + dy * dy <= hitRadius * hitRadius)
                    {
                        m_IsDraggingConnection = true;
                        m_DragStartNodeID = node.m_ID;
                        m_DragStartEndpoint = pin.Name;
                        m_DragStartIsOutput = pin.IsOutput;
                        m_DragStartIsEvent = pin.IsEvent;
                        m_DragEndPos = mousePos;
                        startedConnection = true;
                        break;
                    }
                }
                if (startedConnection)
                    break;

                // Body hit-test.
                if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                    mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y)
                {
                    hitNode = node.m_ID;
                    break;
                }
            }

            if (!startedConnection)
            {
                const bool shiftHeld = ImGui::GetIO().KeyShift;
                if (hitNode != 0)
                {
                    if (shiftHeld)
                    {
                        // Shift+click toggles membership in the multi-selection set without
                        // changing the primary selection (which the property sidebar uses).
                        if (m_SelectedNodes.count(hitNode) > 0)
                            m_SelectedNodes.erase(hitNode);
                        else
                            m_SelectedNodes.insert(hitNode);
                        // Keep the primary selection pointed at a still-selected node so
                        // the sidebar doesn't go blank after a deselect.
                        if (m_SelectedNodes.count(m_SelectedNodeID) == 0)
                            m_SelectedNodeID = m_SelectedNodes.empty() ? UUID{ 0 } : *m_SelectedNodes.begin();
                    }
                    else
                    {
                        // Plain click: single-select.
                        m_SelectedNodeID = hitNode;
                        m_SelectedNodes.clear();
                        m_SelectedNodes.insert(hitNode);
                    }

                    // Begin node drag — snapshot before any position mutation so the
                    // entire drag collapses into one undo entry. If the anchor is part
                    // of a multi-selection, capture every selected node's starting
                    // position so the drag applies a uniform delta to all of them.
                    const auto* nodePtr = m_GraphAsset->GetNode(hitNode);
                    if (nodePtr)
                    {
                        m_IsDraggingNode = true;
                        m_DragNodeID = hitNode;
                        m_DragNodeStartPos = { nodePtr->m_PosX, nodePtr->m_PosY };
                        m_DragMouseStartPos = mousePos;
                        m_DragNodeStartPositions.clear();
                        if (m_SelectedNodes.count(hitNode) > 0 && m_SelectedNodes.size() > 1)
                        {
                            for (UUID id : m_SelectedNodes)
                            {
                                if (const auto* sel = m_GraphAsset->GetNode(id))
                                    m_DragNodeStartPositions[id] = { sel->m_PosX, sel->m_PosY };
                            }
                        }
                        else
                        {
                            m_DragNodeStartPositions[hitNode] = m_DragNodeStartPos;
                        }
                        BeginEditSession();
                    }
                }
                else
                {
                    // Empty-canvas click: clear selection (unless shift is held), and
                    // start a box-select drag for the multi-selection rectangle.
                    if (!shiftHeld)
                    {
                        m_SelectedNodeID = 0;
                        m_SelectedNodes.clear();
                    }
                    m_IsBoxSelecting = true;
                    m_BoxSelectStart = mousePos;
                }
            }
        }

        // Move node(s) while dragging. Walk the asset's node vector once, applying the
        // captured starting position + delta to each entry that's in our drag set. This
        // handles both single-node and multi-node drags through one path.
        if (m_IsDraggingNode && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // SoundGraphAsset only exposes a const node accessor; we already cast around in
            // DrawNodes, so do the same here to write back position changes.
            auto& nodes = const_cast<std::vector<SoundGraphNodeData>&>(m_GraphAsset->GetNodes());
            ImVec2 const delta = ImVec2(mousePos.x - m_DragMouseStartPos.x, mousePos.y - m_DragMouseStartPos.y);
            const f32 worldDeltaX = delta.x / m_Zoom;
            const f32 worldDeltaY = delta.y / m_Zoom;
            for (auto& n : nodes)
            {
                auto it = m_DragNodeStartPositions.find(n.m_ID);
                if (it == m_DragNodeStartPositions.end())
                    continue;
                n.m_PosX = it->second.x + worldDeltaX;
                n.m_PosY = it->second.y + worldDeltaY;
                m_IsDirty = true;
            }
        }

        if (m_IsDraggingNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const bool wasMultiDrag = m_DragNodeStartPositions.size() > 1;
            m_IsDraggingNode = false;
            m_DragNodeID = 0;
            m_DragNodeStartPositions.clear();
            EndEditSession(wasMultiDrag ? "Move Nodes" : "Move Node");
        }

        // Box-select. The marquee rectangle is drawn in DrawCanvas (cheap — we read the
        // state here and there). On release we hit-test every node against the rect and
        // replace m_SelectedNodes with whatever overlaps.
        if (m_IsBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const ImVec2 rectMin(std::min(m_BoxSelectStart.x, mousePos.x),
                                 std::min(m_BoxSelectStart.y, mousePos.y));
            const ImVec2 rectMax(std::max(m_BoxSelectStart.x, mousePos.x),
                                 std::max(m_BoxSelectStart.y, mousePos.y));
            if (const bool meaningfulDrag = (rectMax.x - rectMin.x > 4.0f) && (rectMax.y - rectMin.y > 4.0f); meaningfulDrag)
            {
                m_SelectedNodes.clear();
                for (const auto& node : m_GraphAsset->GetNodes())
                {
                    ImVec2 const nodeScreen = WorldToScreen({ node.m_PosX, node.m_PosY }, canvasOrigin);
                    ImVec2 const nodeSize = GetNodeSize(node);
                    ImVec2 const nodeMax(nodeScreen.x + nodeSize.x, nodeScreen.y + nodeSize.y);
                    // Standard AABB overlap test: rectangles overlap iff neither is fully
                    // to one side of the other.
                    const bool overlaps = (nodeScreen.x <= rectMax.x) && (nodeMax.x >= rectMin.x) &&
                                          (nodeScreen.y <= rectMax.y) && (nodeMax.y >= rectMin.y);
                    if (overlaps)
                        m_SelectedNodes.insert(node.m_ID);
                }
                m_SelectedNodeID = m_SelectedNodes.empty() ? UUID{ 0 } : *m_SelectedNodes.begin();
            }
            m_IsBoxSelecting = false;
        }

        // Graph-output pseudo-node drag. Its position is per-session UI state; we don't mark
        // the asset dirty since it isn't serialized.
        if (m_DraggingGraphOutputNode)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 const delta = ImVec2(mousePos.x - m_DragMouseStartPos.x, mousePos.y - m_DragMouseStartPos.y);
                m_GraphOutputNodePos.x = m_GraphOutputDragStart.x + delta.x / m_Zoom;
                m_GraphOutputNodePos.y = m_GraphOutputDragStart.y + delta.y / m_Zoom;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                m_DraggingGraphOutputNode = false;
            }
        }

        // Same for Graph Input pseudo-node.
        if (m_DraggingGraphInputNode)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 const delta = ImVec2(mousePos.x - m_DragMouseStartPos.x, mousePos.y - m_DragMouseStartPos.y);
                m_GraphInputNodePos.x = m_GraphInputDragStart.x + delta.x / m_Zoom;
                m_GraphInputNodePos.y = m_GraphInputDragStart.y + delta.y / m_Zoom;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                m_DraggingGraphInputNode = false;
            }
        }
    }

    void SoundGraphEditorPanel::HandleConnectionDrag(const ImVec2& canvasOrigin)
    {
        if (!m_IsDraggingConnection)
            return;

        m_DragEndPos = ImGui::GetIO().MousePos;

        // Escape mid-drag cancels the connection without committing anything. Cheap to
        // recover from a misclicked pin without having to find empty canvas to drop on.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_IsDraggingConnection = false;
            m_DragStartNodeID = 0;
            m_DragStartEndpoint.clear();
            return;
        }

        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            return;

        // On release: hit-test the pin under the cursor. Valid if it's a different node, the
        // opposite direction (we drag output→input or input→output), and matches the value vs
        // event "kind" of the start pin. Self-loops and duplicate connections are rejected.
        if (m_GraphAsset)
        {
            // Graph-output pseudo-node pins come first so a drop on one wins over a real
            // node that happens to overlap.
            if (m_DragStartNodeID != kGraphOutputNodeID)
            {
                const ImVec2 outNodeScreen = WorldToScreen(m_GraphOutputNodePos, canvasOrigin);
                const GraphOutputGeometry g = ComputeGraphOutputGeometry(outNodeScreen, m_Zoom);
                const f32 hitRadius = s_PinRadius * m_Zoom + 4.0f;
                struct
                {
                    const char* Name;
                    ImVec2 Pos;
                } pinList[2] = {
                    { "OutLeft", g.OutLeftPinPos },
                    { "OutRight", g.OutRightPinPos },
                };
                for (auto& p : pinList)
                {
                    const f32 dx = m_DragEndPos.x - p.Pos.x;
                    if (const f32 dy = m_DragEndPos.y - p.Pos.y; dx * dx + dy * dy > hitRadius * hitRadius)
                        continue;
                    // Drop on a pseudo-node input pin. Only valid if the drag started from
                    // a real node's output and was a value (not event) connection.
                    if (!m_DragStartIsOutput || m_DragStartIsEvent)
                        break;

                    SoundGraphConnection connection;
                    connection.m_SourceNodeID = m_DragStartNodeID;
                    connection.m_SourceEndpoint = m_DragStartEndpoint;
                    connection.m_TargetNodeID = kGraphOutputNodeID;
                    connection.m_TargetEndpoint = p.Name;
                    connection.m_IsEvent = false;
                    {
                        Ref<SoundGraphAsset> snap = SnapshotAsset();
                        if (m_GraphAsset->AddConnection(connection))
                        {
                            m_IsDirty = true;
                            PushSnapshot(std::move(snap), std::string("Connect to Graph Output ") + p.Name);
                        }
                    }
                    goto endConnectionDrag;
                }
            }

            const auto& nodes = m_GraphAsset->GetNodes();
            for (const auto& node : nodes)
            {
                if (node.m_ID == m_DragStartNodeID)
                    continue;

                ImVec2 const nodePos = WorldToScreen({ node.m_PosX, node.m_PosY }, canvasOrigin);
                auto pins = GetNodePins(node, nodePos);
                for (const auto& pin : pins)
                {
                    f32 const dx = m_DragEndPos.x - pin.Position.x;
                    f32 const dy = m_DragEndPos.y - pin.Position.y;
                    if (f32 const hitRadius = s_PinRadius * m_Zoom + 4.0f; dx * dx + dy * dy > hitRadius * hitRadius)
                        continue;
                    if (pin.IsOutput == m_DragStartIsOutput)
                        break; // same-direction pin — not a valid wire endpoint
                    if (pin.IsEvent != m_DragStartIsEvent)
                        break; // can't connect event pins to value pins

                    SoundGraphConnection connection;
                    if (m_DragStartIsOutput)
                    {
                        connection.m_SourceNodeID = m_DragStartNodeID;
                        connection.m_SourceEndpoint = m_DragStartEndpoint;
                        connection.m_TargetNodeID = pin.NodeID;
                        connection.m_TargetEndpoint = pin.Name;
                    }
                    else
                    {
                        connection.m_SourceNodeID = pin.NodeID;
                        connection.m_SourceEndpoint = pin.Name;
                        connection.m_TargetNodeID = m_DragStartNodeID;
                        connection.m_TargetEndpoint = m_DragStartEndpoint;
                    }
                    connection.m_IsEvent = m_DragStartIsEvent;

                    {
                        Ref<SoundGraphAsset> snap = SnapshotAsset();
                        if (m_GraphAsset->AddConnection(connection))
                        {
                            m_IsDirty = true;
                            PushSnapshot(std::move(snap), "Add Connection");
                        }
                    }
                    break;
                }
            }
        }

    endConnectionDrag:
        m_IsDraggingConnection = false;
        m_DragStartNodeID = 0;
        m_DragStartEndpoint.clear();
    }

    // =========================================================================
    // Property panel (placeholder — M8 will flesh out)
    // =========================================================================

    void SoundGraphEditorPanel::DrawPropertyPanel()
    {
        if (!m_GraphAsset || m_SelectedNodeID == 0)
        {
            ImGui::TextDisabled("Select a node to edit its properties.");
            return;
        }

        // When multiple nodes are selected, surface that fact at the top — the sidebar
        // still drives off m_SelectedNodeID (the primary), but the user shouldn't be
        // confused into thinking only that one is selected.
        if (m_SelectedNodes.size() > 1)
        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f),
                               "%zu nodes selected (editing primary)", m_SelectedNodes.size());
            ImGui::Separator();
        }

        // M8 will turn this into a proper schema-driven editor backed by SoundGraph::Factory
        // introspection. Today it just exposes the raw m_Properties string map so the user
        // can confirm selection works end-to-end.
        auto* node = m_GraphAsset->GetNode(m_SelectedNodeID);
        if (!node)
        {
            ImGui::TextDisabled("Selected node not found.");
            return;
        }
        DrawNodeProperties(*node);
    }

    void SoundGraphEditorPanel::DrawNodeProperties(SoundGraphNodeData& node)
    {
        ImGui::Text("Type: %s", node.m_Type.c_str());

        // Visible Delete Node button — easier to discover than Delete key / right-click /
        // Edit menu. Captures the selected-node ID up front so the deletion is local to this
        // node, even if the selection is part of a multi-select. The right-click and
        // keyboard Delete paths handle multi-select deletion; this one is intentionally
        // single-node so a misclick doesn't nuke the whole selection.
        ImGui::SameLine();
        const f32 buttonWidth = 90.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonWidth));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(160, 60, 60, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 80, 80, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(120, 40, 40, 255));
        const bool deleteClicked = ImGui::Button("Delete Node", ImVec2(buttonWidth, 0.0f));
        ImGui::PopStyleColor(3);
        if (deleteClicked)
        {
            const UUID nodeID = node.m_ID;
            std::string description = std::string("Delete Node '") +
                                      (node.m_Name.empty() ? node.m_Type : node.m_Name) + "'";
            Ref<SoundGraphAsset> snap = SnapshotAsset();
            m_SelectedNodes.erase(nodeID);
            if (m_SelectedNodeID == nodeID)
                m_SelectedNodeID = m_SelectedNodes.empty() ? UUID{ 0 } : *m_SelectedNodes.begin();
            DeleteNode(nodeID);
            PushSnapshot(std::move(snap), std::move(description));
            return; // node reference is now dangling — bail
        }

        ImGui::Separator();

        // Begin a session before the first interactive widget; the End at the bottom of
        // this function flushes one undo entry per cohesive interaction (a slider drag,
        // a property tweak, etc.) rather than 60 entries per second.
        BeginEditSession();

        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", node.m_Name.c_str());
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
        {
            node.m_Name = nameBuf;
            m_IsDirty = true;
        }

        ImGui::Separator();

        // Schema-driven typed parameter editor. If we have a schema for this node type,
        // render typed widgets that round-trip values through m_Properties as strings (so
        // the YAML serializer doesn't need to know about types). Unschemized types fall
        // back to the raw string-map editor.
        if (const Audio::SoundGraph::NodeSchema* schema = Audio::SoundGraph::GetNodeSchema(node.m_Type); schema)
        {
            ImGui::Text("Parameters");
            ImGui::Separator();

            for (const auto& param : *schema)
            {
                auto& valueStr = node.m_Properties[param.Name]; // creates if missing
                switch (param.Kind)
                {
                    case Audio::SoundGraph::NodeParamKind::Float:
                    {
                        if (f32 v = Audio::SoundGraph::ParsePropertyFloat(param, valueStr); ImGui::DragFloat(param.Name.c_str(), &v, param.Step, param.MinFloat, param.MaxFloat))
                        {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                            valueStr = buf;
                            m_IsDirty = true;
                        }
                        if (param.Tooltip && ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", param.Tooltip);
                        break;
                    }
                    case Audio::SoundGraph::NodeParamKind::Int:
                    {
                        if (i32 v = Audio::SoundGraph::ParsePropertyInt(param, valueStr); ImGui::DragInt(param.Name.c_str(), &v))
                        {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%d", v);
                            valueStr = buf;
                            m_IsDirty = true;
                        }
                        if (param.Tooltip && ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", param.Tooltip);
                        break;
                    }
                    case Audio::SoundGraph::NodeParamKind::Bool:
                    {
                        if (bool v = Audio::SoundGraph::ParsePropertyBool(param, valueStr); ImGui::Checkbox(param.Name.c_str(), &v))
                        {
                            valueStr = v ? "true" : "false";
                            m_IsDirty = true;
                        }
                        if (param.Tooltip && ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", param.Tooltip);
                        break;
                    }
                    case Audio::SoundGraph::NodeParamKind::AudioAsset:
                    {
                        const u64 handle = Audio::SoundGraph::ParsePropertyAssetHandle(valueStr);

                        // Resolve handle → human-readable display. Filename is what the user
                        // actually wants to see; the integer handle is registry plumbing.
                        // Stash the full relative path in a tooltip for users debugging stale
                        // references.
                        std::string displayText;
                        std::string fullPath;
                        if (handle == 0)
                        {
                            displayText = "(none)";
                        }
                        else
                        {
                            AssetMetadata md = AssetManager::GetAssetMetadata(handle);
                            if (!md.FilePath.empty())
                            {
                                displayText = md.FilePath.filename().string();
                                fullPath = md.FilePath.generic_string();
                            }
                            else
                            {
                                displayText = "<missing asset>";
                            }
                        }

                        ImGui::PushID(param.Name.c_str());

                        const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
                        const f32 browseW = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const f32 clearW = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const f32 buttonsTotal = browseW + spacing + (handle != 0 ? (clearW + spacing) : 0.0f);
                        const f32 labelW = ImGui::CalcTextSize(param.Name.c_str()).x + spacing;
                        const f32 fieldW = std::max(60.0f, ImGui::GetContentRegionAvail().x - buttonsTotal - labelW);

                        // Read-only filename field — looks like an input, doubles as the
                        // drag-drop target. CONTENT_BROWSER_AUDIO is the payload type emitted
                        // by the content browser for audio files; ImportAsset re-validates so a
                        // wrong-typed drop doesn't silently bind.
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s", displayText.c_str());
                        ImGui::SetNextItemWidth(fieldW);
                        ImGui::InputText("##field", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_AUDIO"))
                            {
                                std::filesystem::path assetPath(static_cast<const char*>(payload->Data));
                                if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                                {
                                    AssetHandle imported = assetManager->ImportAsset(assetPath);
                                    if (imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Audio)
                                    {
                                        valueStr = std::to_string(static_cast<u64>(imported));
                                        m_IsDirty = true;
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                        {
                            std::string tip;
                            if (param.Tooltip)
                                tip = param.Tooltip;
                            if (!fullPath.empty())
                            {
                                if (!tip.empty())
                                    tip += "\n\n";
                                tip += "Asset: " + fullPath;
                            }
                            if (tip.empty())
                                tip = "Drag an audio file here, or use the … button.";
                            else
                                tip += "\n\nDrag an audio file here, or use the … button.";
                            ImGui::SetTooltip("%s", tip.c_str());
                        }

                        // Browse button — opens a file dialog and imports + binds in one step.
                        ImGui::SameLine(0.0f, spacing);
                        if (ImGui::Button("...", ImVec2(browseW, 0)))
                        {
                            std::string filepath = FileDialogs::OpenFile(
                                "Audio (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0");
                            if (!filepath.empty())
                            {
                                if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                                {
                                    AssetHandle imported = assetManager->ImportAsset(filepath);
                                    if (imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Audio)
                                    {
                                        valueStr = std::to_string(static_cast<u64>(imported));
                                        m_IsDirty = true;
                                    }
                                    else
                                    {
                                        OLO_CORE_WARN("SoundGraphEditor: '{}' could not be imported as an Audio asset.",
                                                      filepath);
                                    }
                                }
                            }
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Browse for an audio file");

                        // Clear button — only when assigned, mirrors the rest of the editor's
                        // asset-reference clear UX.
                        if (handle != 0)
                        {
                            ImGui::SameLine(0.0f, spacing);
                            if (ImGui::Button("X", ImVec2(clearW, 0)))
                            {
                                valueStr = "0";
                                m_IsDirty = true;
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Clear");
                        }

                        // Property label, on the right (matches ImGui's natural layout for
                        // labeled widgets).
                        ImGui::SameLine();
                        ImGui::TextUnformatted(param.Name.c_str());

                        ImGui::PopID();
                        break;
                    }
                }
            }
            EndEditSession("Edit Node Properties");
            return;
        }

        // Fallback: no schema, just expose the raw string properties.
        ImGui::Text("Properties (%zu) — no schema for type '%s'", node.m_Properties.size(), node.m_Type.c_str());
        for (auto& [key, value] : node.m_Properties)
        {
            char valBuf[256];
            std::snprintf(valBuf, sizeof(valBuf), "%s", value.c_str());
            if (ImGui::InputText(key.c_str(), valBuf, sizeof(valBuf)))
            {
                value = valBuf;
                m_IsDirty = true;
            }
        }

        EndEditSession("Edit Node Properties");
    }

    // =========================================================================
    // Context menu (M6 — empty placeholder)
    // =========================================================================

    void SoundGraphEditorPanel::DrawContextMenu(const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        // Right-click anywhere on the canvas opens the popup. HandleNodeInteraction has
        // already set m_RightClickNodeID / m_RightClickConnectionIndex; the popup body
        // branches on those to show the right action set.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_ShowContextMenu = true;
            m_ContextMenuPos = ImGui::GetIO().MousePos;
            std::memset(m_NodeSearchFilter, 0, sizeof(m_NodeSearchFilter));
            ImGui::OpenPopup("##SGRContextMenu");
        }

        // Wire-hit branch: delete connection. Takes precedence over the palette since the
        // hit-test already determined the user clicked on a wire (not empty canvas).
        if (m_RightClickConnectionIndex != static_cast<sizet>(-1))
        {
            if (ImGui::BeginPopup("##SGRContextMenu"))
            {
                if (m_RightClickConnectionIndex < m_GraphAsset->GetConnections().size())
                {
                    const auto& c = m_GraphAsset->GetConnections()[m_RightClickConnectionIndex];
                    std::string label = (c.m_SourceNodeID == kGraphOutputNodeID ? "*graph*" : std::to_string(static_cast<u64>(c.m_SourceNodeID))) +
                                        "." + c.m_SourceEndpoint + " → " +
                                        (c.m_TargetNodeID == kGraphOutputNodeID ? "*graph*" : std::to_string(static_cast<u64>(c.m_TargetNodeID))) +
                                        "." + c.m_TargetEndpoint;
                    ImGui::TextDisabled("Connection: %s", label.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Connection"))
                    {
                        Ref<SoundGraphAsset> snap = SnapshotAsset();
                        DeleteConnection(m_RightClickConnectionIndex);
                        m_RightClickConnectionIndex = static_cast<sizet>(-1);
                        PushSnapshot(std::move(snap), "Delete Connection");
                    }
                }
                ImGui::EndPopup();
            }
            else
            {
                m_RightClickConnectionIndex = static_cast<sizet>(-1);
            }
            return;
        }

        // Graph Input pseudo-node menu: Add Parameter / list of params with delete.
        if (m_RightClickGraphInput)
        {
            if (ImGui::BeginPopup("##SGRContextMenu"))
            {
                ImGui::TextDisabled("Graph Input");
                ImGui::Separator();
                if (ImGui::MenuItem("Add Parameter..."))
                {
                    m_ShowAddParamDialog = true;
                    std::memset(m_NewParamName, 0, sizeof(m_NewParamName));
                }

                // Inline list of existing params with a delete button each.
                if (const auto& inputs = m_GraphAsset->GetGraphInputs(); !inputs.empty())
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("Existing (%zu):", inputs.size());
                    std::vector<std::string> sorted;
                    sorted.reserve(inputs.size());
                    for (const auto& [name, _] : inputs)
                        sorted.push_back(name);
                    std::ranges::sort(sorted);
                    for (const auto& name : sorted)
                    {
                        if (ImGui::BeginMenu(name.c_str()))
                        {
                            if (ImGui::MenuItem("Rename..."))
                            {
                                m_ShowRenameParamDialog = true;
                                m_RenameParamOldName = name;
                                std::snprintf(m_RenameParamNewName, sizeof(m_RenameParamNewName), "%s", name.c_str());
                                m_RightClickGraphInput = false;
                                ImGui::EndMenu();
                                break; // dialog will open next frame; we're done here
                            }
                            if (ImGui::MenuItem("Delete"))
                            {
                                Ref<SoundGraphAsset> snap = SnapshotAsset();
                                m_GraphAsset->RemoveGraphInput(name);
                                m_IsDirty = true;
                                PushSnapshot(std::move(snap), "Remove Parameter '" + name + "'");
                                m_RightClickGraphInput = false;
                                ImGui::EndMenu();
                                break; // sorted list is now stale
                            }
                            ImGui::EndMenu();
                        }
                    }
                }
                ImGui::EndPopup();
            }
            else
            {
                m_RightClickGraphInput = false;
            }
            return;
        }

        // Node-specific actions take priority when a node was right-clicked. Render those,
        // then return early so we don't also show the palette in the same popup. The
        // graph-output pseudo-node gets a dedicated (read-only) variant — you can't delete
        // it but it's useful to confirm what you right-clicked on.
        if (m_RightClickNodeID != 0)
        {
            if (ImGui::BeginPopup("##SGRContextMenu"))
            {
                if (m_RightClickNodeID == kGraphOutputNodeID)
                {
                    ImGui::TextDisabled("Graph Output (always present)");
                    ImGui::Separator();
                    ImGui::TextDisabled("Drag node outputs onto OutLeft/OutRight");
                    ImGui::TextDisabled("to route audio to the speaker.");
                }
                else if (const auto* node = m_GraphAsset->GetNode(m_RightClickNodeID))
                {
                    ImGui::TextDisabled("Node: %s", node->m_Name.empty() ? node->m_Type.c_str() : node->m_Name.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Node", "Del"))
                    {
                        Ref<SoundGraphAsset> snap = SnapshotAsset();
                        std::string description = std::string("Delete Node '") +
                                                  (node->m_Name.empty() ? node->m_Type : node->m_Name) + "'";
                        DeleteNode(m_RightClickNodeID);
                        m_RightClickNodeID = 0;
                        PushSnapshot(std::move(snap), std::move(description));
                    }
                }
                else
                {
                    // Node went away mid-frame (rare race during deletion); fall through.
                    m_RightClickNodeID = 0;
                }
                ImGui::EndPopup();
            }
            else
            {
                // Popup closed without picking anything; reset so the next right-click on
                // empty canvas shows the palette as expected.
                m_RightClickNodeID = 0;
            }
            return;
        }

        // Hardcoded palette mirroring SoundGraphFactory.cpp's s_NodeProcessors registry.
        // Grouping is for readability; the factory accepts the bare type name. Keep this
        // in sync with the factory when adding new node types.
        struct PaletteEntry
        {
            const char* Category;
            const char* TypeName;
        };
        static const PaletteEntry s_Palette[] = {
            { "Generators", "SineOscillator" },
            { "Generators", "SquareOscillator" },
            { "Generators", "SawtoothOscillator" },
            { "Generators", "TriangleOscillator" },
            { "Generators", "Noise" },
            { "Generators", "WavePlayer" },
            { "Math (float)", "Add<float>" },
            { "Math (float)", "Subtract<float>" },
            { "Math (float)", "Multiply<float>" },
            { "Math (float)", "Divide<float>" },
            { "Math (float)", "Min<float>" },
            { "Math (float)", "Max<float>" },
            { "Math (float)", "Clamp<float>" },
            { "Math (float)", "MapRange<float>" },
            { "Math (float)", "Power<float>" },
            { "Math (float)", "Abs<float>" },
            { "Math (int)", "Add<int>" },
            { "Math (int)", "Subtract<int>" },
            { "Math (int)", "Multiply<int>" },
            { "Envelope", "ADEnvelope" },
            { "Envelope", "ADSREnvelope" },
            { "Trigger", "RepeatTrigger" },
            { "Trigger", "TriggerCounter" },
            { "Trigger", "DelayedTrigger" },
            { "Array (float)", "GetRandom<float>" },
            { "Array (int)", "GetRandom<int>" },
            { "Music", "BPMToSeconds" },
            { "Music", "NoteToFrequency" },
            { "Music", "FrequencyToNote" },
        };

        if (ImGui::BeginPopup("##SGRContextMenu"))
        {
            ImGui::Text("Add Node");
            ImGui::Separator();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##sgrnodefilter", "Filter...", m_NodeSearchFilter, sizeof(m_NodeSearchFilter));

            std::string filter = m_NodeSearchFilter;
            std::ranges::transform(filter, filter.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });

            const char* currentCategory = nullptr;
            for (const auto& entry : s_Palette)
            {
                // Filter check (case-insensitive substring on type name or category).
                if (!filter.empty())
                {
                    std::string nameLower = entry.TypeName;
                    std::ranges::transform(nameLower, nameLower.begin(),
                                           [](unsigned char c)
                                           { return static_cast<char>(std::tolower(c)); });
                    std::string categoryLower = entry.Category;
                    std::ranges::transform(categoryLower, categoryLower.begin(),
                                           [](unsigned char c)
                                           { return static_cast<char>(std::tolower(c)); });
                    if (nameLower.find(filter) == std::string::npos && categoryLower.find(filter) == std::string::npos)
                        continue;
                }

                if (currentCategory == nullptr || std::strcmp(entry.Category, currentCategory) != 0)
                {
                    currentCategory = entry.Category;
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", entry.Category);
                }

                if (ImGui::MenuItem(entry.TypeName))
                {
                    Ref<SoundGraphAsset> snap = SnapshotAsset();
                    glm::vec2 const worldPos = ScreenToWorld(m_ContextMenuPos, canvasOrigin);
                    UUID const newID = CreateNode(entry.TypeName, worldPos);
                    if (newID != 0)
                    {
                        m_SelectedNodeID = newID;
                        PushSnapshot(std::move(snap), std::string("Add Node '") + entry.TypeName + "'");
                    }
                }
            }

            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Asset lifecycle
    // =========================================================================

    void SoundGraphEditorPanel::OpenSoundGraph(const std::filesystem::path& path)
    {
        m_PendingLoadPath = path;
        m_PendingLoadHandle = 0;
        m_PendingLoadFrameDelay = 0;
        m_IsOpen = true;
        ImGui::SetWindowFocus("###SoundGraphEditor");
    }

    void SoundGraphEditorPanel::OpenSoundGraph(AssetHandle handle)
    {
        if (handle == 0)
            return;

        AssetMetadata md = AssetManager::GetAssetMetadata(handle);
        if (!md.FilePath.empty())
        {
            std::filesystem::path absolute = Project::GetAssetDirectory() / md.FilePath;
            m_PendingLoadPath = absolute;
            m_PendingLoadHandle = handle;
            m_PendingLoadFrameDelay = 0;
            m_IsOpen = true;
            ImGui::SetWindowFocus("###SoundGraphEditor");
        }
    }

    void SoundGraphEditorPanel::NewSoundGraph()
    {
        StopPreview(); // any prior preview belongs to the asset we're replacing
        m_GraphAsset = Ref<SoundGraphAsset>::Create();
        m_GraphAsset->SetName("Untitled Sound Graph");
        m_CurrentFilePath.clear();
        m_CurrentAssetHandle = 0;
        m_SelectedNodeID = 0;
        m_IsDirty = false;
        m_ScrollOffset = { 0.0f, 0.0f };
        m_Zoom = 1.0f;
    }

    void SoundGraphEditorPanel::LoadSoundGraph(const std::filesystem::path& path)
    {
        auto asset = Ref<SoundGraphAsset>::Create();
        if (!Audio::SoundGraph::SoundGraphSerializer::Deserialize(*asset, path))
        {
            OLO_CORE_ERROR("SoundGraphEditorPanel: failed to load '{}'", path.string());
            return;
        }
        m_GraphAsset = asset;
        m_CurrentFilePath = path;
        m_SelectedNodeID = 0;
        m_IsDirty = false;
        m_ScrollOffset = { 0.0f, 0.0f };
        m_Zoom = 1.0f;
    }

    void SoundGraphEditorPanel::PerformPendingLoad()
    {
        if (m_PendingLoadPath.empty())
            return;
        StopPreview(); // stop any in-flight preview for the previous asset
        std::filesystem::path path = m_PendingLoadPath;
        AssetHandle handle = m_PendingLoadHandle;
        m_PendingLoadPath.clear();
        m_PendingLoadHandle = 0;
        m_PendingLoadFrameDelay = 0;

        LoadSoundGraph(path);
        m_CurrentAssetHandle = handle;
    }

    void SoundGraphEditorPanel::NotifyAssetReloaded(AssetHandle handle, const std::filesystem::path& path)
    {
        // Only react if the reloaded asset is the one currently in the panel. Match by
        // handle when we have one (panel-driven load), otherwise fall back to path equality
        // (covers New-then-not-yet-registered files and direct path-based opens).
        if (!m_GraphAsset)
            return;

        const bool handlesMatch = (m_CurrentAssetHandle != 0) && (handle == m_CurrentAssetHandle);
        bool pathsMatch = false;
        if (!m_CurrentFilePath.empty() && !path.empty())
        {
            std::error_code ec;
            pathsMatch = std::filesystem::equivalent(m_CurrentFilePath, path, ec);
            if (ec)
                pathsMatch = false;
        }
        if (!handlesMatch && !pathsMatch)
            return;

        // Suppress the event that fires from our own SaveSoundGraph (and the filewatch echo
        // that follows it). m_SelfSaveEchoWindow generously bounds the round trip.
        if (const auto sinceSelfSave = std::chrono::steady_clock::now() - m_LastSelfSaveTime; sinceSelfSave < m_SelfSaveEchoWindow)
            return;

        // If the user has unsaved edits, defer the reload and ask them via modal. Without
        // edits we can reload immediately — go through the deferred-load path so the load
        // happens on the next frame and doesn't fight ImGui mid-draw.
        if (m_IsDirty)
        {
            m_PendingExternalReloadHandle = handle;
            m_PendingExternalReloadPath = path;
            m_ShowExternalReloadDialog = true;
            OLO_CORE_INFO("SoundGraphEditorPanel: external change detected for '{}', user has unsaved edits — prompting.",
                          path.string());
        }
        else
        {
            m_PendingLoadPath = path;
            m_PendingLoadHandle = handle;
            m_PendingLoadFrameDelay = 0;
            OLO_CORE_INFO("SoundGraphEditorPanel: external change detected for '{}', reloading.",
                          path.string());
        }
    }

    void SoundGraphEditorPanel::SaveSoundGraph()
    {
        if (!m_GraphAsset)
            return;
        if (m_CurrentFilePath.empty())
        {
            OLO_CORE_WARN("SoundGraphEditorPanel: Save called with no path; use Save As (not yet implemented).");
            return;
        }

        // Compile the asset to a Prototype and stash it on the asset before serializing.
        // The hot-reload dispatcher in EditorLayer reads GetCompiledPrototype() to construct
        // a runtime SoundGraph; without this step the asset stays at "loaded YAML, no
        // executable graph" and no audio plays.
        if (Ref<Audio::SoundGraph::Prototype> prototype = Audio::SoundGraph::CompileAssetToPrototype(*m_GraphAsset); prototype)
        {
            m_GraphAsset->SetCompiledPrototype(prototype);
        }
        else
        {
            OLO_CORE_WARN("SoundGraphEditorPanel: asset compile produced no prototype; live audio "
                          "will not be available for this graph until the issue is fixed.");
        }

        if (!Audio::SoundGraph::SoundGraphSerializer::Serialize(*m_GraphAsset, m_CurrentFilePath))
        {
            OLO_CORE_ERROR("SoundGraphEditorPanel: failed to save '{}'", m_CurrentFilePath.string());
            return;
        }
        m_IsDirty = false;
        // Record self-save timestamp so the project-wide file watcher's echo (which arrives
        // a moment after we write) doesn't pop the external-change modal.
        m_LastSelfSaveTime = std::chrono::steady_clock::now();
        OLO_CORE_TRACE("SoundGraphEditorPanel: saved '{}'", m_CurrentFilePath.string());

        // Fire an AssetReloadedEvent so the hot-reload dispatcher in EditorLayer can swap the
        // live SoundGraph instances on running scenes to the freshly saved version.
        if (m_CurrentAssetHandle != 0)
        {
            AssetReloadedEvent evt(m_CurrentAssetHandle, AssetType::SoundGraph, m_CurrentFilePath);
            Application::Get().OnEvent(evt);
        }
    }

    void SoundGraphEditorPanel::StartPreview()
    {
        if (!m_GraphAsset)
            return;

        // Stop anything currently playing so a re-Play after an edit produces clean state.
        StopPreview();

        // Compile the (possibly unsaved) asset and stamp the result so subsequent live
        // edits during preview can re-use the prototype path. We don't write to disk here
        // — preview is intentionally non-destructive.
        Ref<Audio::SoundGraph::Prototype> prototype = Audio::SoundGraph::CompileAssetToPrototype(*m_GraphAsset);
        if (!prototype)
        {
            OLO_CORE_WARN("SoundGraphEditorPanel::StartPreview - compile failed; nothing to play");
            return;
        }
        m_GraphAsset->SetCompiledPrototype(prototype);

        Ref<Audio::SoundGraph::SoundGraph> graphInstance = Audio::SoundGraph::CreateInstance(prototype);
        if (!graphInstance)
        {
            OLO_CORE_WARN("SoundGraphEditorPanel::StartPreview - CreateInstance returned null");
            return;
        }

        m_PreviewSound = Ref<Audio::SoundGraph::SoundGraphSound>::Create();
        m_PreviewSound->InitializeAudioCallback();
        if (!m_PreviewSound->InitializeFromGraph(graphInstance))
        {
            OLO_CORE_WARN("SoundGraphEditorPanel::StartPreview - InitializeFromGraph failed");
            m_PreviewSound = nullptr;
            return;
        }

        // Preview defaults: full volume, unity pitch, non-looping. The user can wire a
        // dedicated preview-settings panel later if these aren't sufficient.
        m_PreviewSound->SetVolume(1.0f);
        m_PreviewSound->SetPitch(1.0f);
        m_PreviewSound->SetLooping(false);
        m_PreviewSound->Play();
        m_IsPreviewPlaying = true;
        OLO_CORE_TRACE("SoundGraphEditorPanel::StartPreview - playing '{}'", m_GraphAsset->GetName());
    }

    void SoundGraphEditorPanel::StopPreview()
    {
        if (m_PreviewSound)
        {
            m_PreviewSound->Stop();
            m_PreviewSound->ReleaseResources();
            m_PreviewSound = nullptr;
        }
        m_IsPreviewPlaying = false;
    }

    void SoundGraphEditorPanel::SaveSoundGraphAs()
    {
        if (!m_GraphAsset)
            return;
        std::string filepath = FileDialogs::SaveFile("OloEngine Sound Graph (*.olosoundgraph)\0*.olosoundgraph\0All Files\0*.*\0");
        if (filepath.empty())
            return;

        std::filesystem::path savePath(filepath);
        if (savePath.extension() != ".olosoundgraph")
            savePath.replace_extension(".olosoundgraph");

        m_CurrentFilePath = savePath;
        // The new path isn't associated with an asset handle until the user re-imports it
        // through the content browser. Reset so we don't accidentally fire a reload event
        // for the OLD handle pointing at the new file.
        m_CurrentAssetHandle = 0;
        SaveSoundGraph();
    }

    // =========================================================================
    // Undo / Redo + snapshot helpers
    // =========================================================================

    Ref<SoundGraphAsset> SoundGraphEditorPanel::SnapshotAsset() const
    {
        if (!m_CommandHistory || !m_GraphAsset)
            return nullptr;
        return m_GraphAsset->Clone();
    }

    void SoundGraphEditorPanel::PushSnapshot(Ref<SoundGraphAsset> oldSnap, std::string description)
    {
        if (!m_CommandHistory || !oldSnap || !m_GraphAsset)
            return;
        Ref<SoundGraphAsset> newSnap = m_GraphAsset->Clone();
        if (!newSnap)
            return;

        // The apply callback captures `this`. The panel outlives the command history (both
        // are owned by EditorLayer), so this is safe; if you ever reorder ownership, the
        // CommandHistory must be cleared before the panel is destroyed.
        m_CommandHistory->PushAlreadyExecuted(
            std::make_unique<SoundGraphChangeCommand>(
                std::move(oldSnap), std::move(newSnap),
                [this](Ref<SoundGraphAsset> snap)
                {
                    if (!snap)
                        return;
                    m_GraphAsset = snap;
                    // Invalidate selection in case the selected node no longer exists in
                    // the restored snapshot.
                    if (m_SelectedNodeID != 0 && m_GraphAsset->GetNode(m_SelectedNodeID) == nullptr)
                        m_SelectedNodeID = 0;
                    m_IsDirty = true;
                },
                std::move(description)));
    }

    void SoundGraphEditorPanel::Undo()
    {
        if (m_CommandHistory && m_CommandHistory->CanUndo())
            m_CommandHistory->Undo();
    }

    void SoundGraphEditorPanel::Redo()
    {
        if (m_CommandHistory && m_CommandHistory->CanRedo())
            m_CommandHistory->Redo();
    }

    void SoundGraphEditorPanel::BeginEditSession()
    {
        if (m_EditSessionActive)
            return;
        m_EditSessionSnapshot = SnapshotAsset();
        m_EditSessionActive = (m_EditSessionSnapshot != nullptr);
    }

    void SoundGraphEditorPanel::EndEditSession(const char* description)
    {
        if (!m_EditSessionActive)
            return;
        // Flush when no ImGui widget is active and no mouse button is held — covers both
        // slider drags (where ActiveId drops to 0 on release) and node-body drags (which
        // don't activate an ImGui ID at all but do hold the left mouse button).
        const bool widgetIdle = (GImGui->ActiveId == 0);
        const bool mouseIdle = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (widgetIdle && mouseIdle)
        {
            PushSnapshot(std::move(m_EditSessionSnapshot), description);
            m_EditSessionSnapshot = nullptr;
            m_EditSessionActive = false;
        }
    }

    // =========================================================================
    // Node operations
    // =========================================================================

    UUID SoundGraphEditorPanel::CreateNode(const std::string& typeName, const glm::vec2& position)
    {
        if (!m_GraphAsset)
            return 0;
        SoundGraphNodeData node;
        node.m_ID = UUID();
        node.m_Type = typeName;
        node.m_Name = typeName;
        node.m_PosX = position.x;
        node.m_PosY = position.y;
        if (m_GraphAsset->AddNode(node))
        {
            m_IsDirty = true;
            return node.m_ID;
        }
        return 0;
    }

    void SoundGraphEditorPanel::DeleteNode(UUID nodeID)
    {
        // nodeID == 0 is the graph-output pseudo-node sentinel; never delete it.
        if (!m_GraphAsset || nodeID == kGraphOutputNodeID)
            return;
        if (m_GraphAsset->RemoveNode(nodeID))
        {
            m_IsDirty = true;
            if (m_SelectedNodeID == nodeID)
                m_SelectedNodeID = 0;
        }
    }

    void SoundGraphEditorPanel::CopySelectedNodes()
    {
        if (!m_GraphAsset || m_SelectedNodes.empty())
            return;
        m_Clipboard.clear();
        glm::vec2 centroid(0.0f, 0.0f);
        for (UUID id : m_SelectedNodes)
        {
            const auto* node = m_GraphAsset->GetNode(id);
            if (!node)
                continue;
            m_Clipboard.push_back(*node);
            centroid.x += node->m_PosX;
            centroid.y += node->m_PosY;
        }
        if (!m_Clipboard.empty())
        {
            centroid /= static_cast<f32>(m_Clipboard.size());
            m_ClipboardPivotWorld = centroid;
        }
    }

    void SoundGraphEditorPanel::PasteNodes(const glm::vec2& pasteWorldPos)
    {
        if (!m_GraphAsset || m_Clipboard.empty())
            return;

        Ref<SoundGraphAsset> snap = SnapshotAsset();

        // Replace the selection with the freshly-pasted set so the user can immediately
        // drag or delete them as a group.
        m_SelectedNodes.clear();
        m_SelectedNodeID = 0;

        const glm::vec2 offset = pasteWorldPos - m_ClipboardPivotWorld;
        for (const auto& src : m_Clipboard)
        {
            SoundGraphNodeData copy = src;
            // Fresh UUID — the original's UUID is still in use by the source node.
            copy.m_ID = UUID();
            copy.m_PosX += offset.x;
            copy.m_PosY += offset.y;
            if (m_GraphAsset->AddNode(copy))
            {
                m_SelectedNodes.insert(copy.m_ID);
                if (m_SelectedNodeID == 0)
                    m_SelectedNodeID = copy.m_ID;
            }
        }

        if (!m_SelectedNodes.empty())
        {
            m_IsDirty = true;
            PushSnapshot(std::move(snap),
                         m_Clipboard.size() == 1 ? "Paste Node" : "Paste Nodes");
        }
    }

    void SoundGraphEditorPanel::DeleteConnection(sizet connectionIndex)
    {
        // M7 will use this for explicit wire deletion. SoundGraphAsset removes by endpoint
        // identity, not index, so we look up the connection details first.
        if (!m_GraphAsset)
            return;
        const auto& connections = m_GraphAsset->GetConnections();
        if (connectionIndex >= connections.size())
            return;
        const auto& c = connections[connectionIndex];
        if (m_GraphAsset->RemoveConnection(c.m_SourceNodeID, c.m_SourceEndpoint, c.m_TargetNodeID, c.m_TargetEndpoint, c.m_IsEvent))
        {
            m_IsDirty = true;
        }
    }

    // =========================================================================
    // Coordinate transforms + node coloring
    // =========================================================================

    ImVec2 SoundGraphEditorPanel::WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const
    {
        return ImVec2(canvasOrigin.x + (worldPos.x + m_ScrollOffset.x) * m_Zoom,
                      canvasOrigin.y + (worldPos.y + m_ScrollOffset.y) * m_Zoom);
    }

    glm::vec2 SoundGraphEditorPanel::ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const
    {
        return { (screenPos.x - canvasOrigin.x) / m_Zoom - m_ScrollOffset.x,
                 (screenPos.y - canvasOrigin.y) / m_Zoom - m_ScrollOffset.y };
    }

    ImU32 SoundGraphEditorPanel::GetNodeColor(const std::string& /*type*/) const
    {
        return IM_COL32(45, 45, 55, 240);
    }

    ImU32 SoundGraphEditorPanel::GetNodeHeaderColor(const std::string& type) const
    {
        // Group node types into categories for visual grouping. Cheap hash-based bucketing.
        static const std::unordered_map<std::string, ImU32> s_CategoryColors = {
            { "SineOscillator", IM_COL32(80, 140, 200, 255) },
            { "SquareOscillator", IM_COL32(80, 140, 200, 255) },
            { "SawtoothOscillator", IM_COL32(80, 140, 200, 255) },
            { "TriangleOscillator", IM_COL32(80, 140, 200, 255) },
            { "Noise", IM_COL32(80, 140, 200, 255) },
            { "WavePlayer", IM_COL32(80, 180, 120, 255) },
            { "ADEnvelope", IM_COL32(200, 140, 80, 255) },
            { "ADSREnvelope", IM_COL32(200, 140, 80, 255) },
        };
        if (auto it = s_CategoryColors.find(type); it != s_CategoryColors.end())
            return it->second;
        // Math nodes (Add<float>, Multiply<int>, ...) and others share a default.
        return IM_COL32(120, 120, 130, 255);
    }
} // namespace OloEngine
