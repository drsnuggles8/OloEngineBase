#include "OloEnginePCH.h"
#include "DialogueEditorPanel.h"
#include "../UndoRedo/SpecializedCommands.h"
#include "OloEngine/Dialogue/DialogueTreeSerializer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <limits>

namespace OloEngine
{
    // =========================================================================
    // Undo/Redo helpers
    // =========================================================================

    DialogueEditorSnapshot DialogueEditorPanel::CaptureSnapshot() const
    {
        return { m_Nodes, m_Connections, m_RootNodeID };
    }

    void DialogueEditorPanel::RestoreSnapshot(const DialogueEditorSnapshot& snapshot)
    {
        m_Nodes = snapshot.Nodes;
        m_Connections = snapshot.Connections;
        m_RootNodeID = snapshot.RootNodeID;
        m_SelectedNodeID = 0;
        m_IsDirty = true;
    }

    void DialogueEditorPanel::PushDialogueUndoCommand(const DialogueEditorSnapshot& oldState, const std::string& description)
    {
        if (!m_CommandHistory)
        {
            return;
        }

        auto newState = CaptureSnapshot();

        // Skip no-op commands (quick structural comparison)
        if (oldState.RootNodeID == newState.RootNodeID && oldState.Nodes.size() == newState.Nodes.size() && oldState.Connections.size() == newState.Connections.size())
        {
            // Check if node IDs and connection endpoints match
            bool same = true;
            for (sizet i = 0; i < oldState.Nodes.size() && same; ++i)
            {
                same = (static_cast<u64>(oldState.Nodes[i].ID) == static_cast<u64>(newState.Nodes[i].ID) && oldState.Nodes[i].Name == newState.Nodes[i].Name && oldState.Nodes[i].Type == newState.Nodes[i].Type && oldState.Nodes[i].Properties == newState.Nodes[i].Properties && oldState.Nodes[i].EditorPosition == newState.Nodes[i].EditorPosition);
            }
            for (sizet i = 0; i < oldState.Connections.size() && same; ++i)
            {
                same = (static_cast<u64>(oldState.Connections[i].SourceNodeID) == static_cast<u64>(newState.Connections[i].SourceNodeID) && static_cast<u64>(oldState.Connections[i].TargetNodeID) == static_cast<u64>(newState.Connections[i].TargetNodeID) && oldState.Connections[i].SourcePort == newState.Connections[i].SourcePort && oldState.Connections[i].TargetPort == newState.Connections[i].TargetPort);
            }
            if (same)
            {
                return;
            }
        }

        auto* panel = this;
        m_CommandHistory->PushAlreadyExecuted(
            std::make_unique<DialogueEditorChangeCommand>(
                oldState, std::move(newState),
                [panel](const DialogueEditorSnapshot& s)
                { panel->RestoreSnapshot(s); },
                description));
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void DialogueEditorPanel::OnImGuiRender()
    {
        if (!m_IsOpen)
            return;

        ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
        std::string windowTitle = "Dialogue Editor";
        if (!m_CurrentFilePath.empty())
        {
            windowTitle += " - " + m_CurrentFilePath.filename().string();
            if (m_IsDirty)
                windowTitle += " *";
        }
        windowTitle += "###DialogueEditor";

        if (!ImGui::Begin(windowTitle.c_str(), &m_IsOpen, ImGuiWindowFlags_MenuBar))
        {
            m_IsFocused = false;
            ImGui::End();
            return;
        }

        m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        DrawToolbar();

        // Layout: left = canvas, right = property panel + preview
        f32 const availWidth = ImGui::GetContentRegionAvail().x;
        f32 const canvasWidth = m_ShowPreview || m_SelectedNodeID != 0
                                    ? availWidth - s_PropertyPanelWidth
                                    : availWidth;

        HandleShortcuts();

        // Left side: node canvas
        DrawCanvas(canvasWidth);

        // Right side: property panel + preview
        if (m_SelectedNodeID != 0 || m_ShowPreview)
        {
            ImGui::SameLine();
            ImGui::BeginChild("##PropertiesPanel", ImVec2(s_PropertyPanelWidth, 0), ImGuiChildFlags_Borders);

            if (m_SelectedNodeID != 0)
            {
                DrawPropertyPanel();
            }

            if (m_ShowPreview)
            {
                if (m_SelectedNodeID != 0)
                    ImGui::Separator();
                DrawPreviewPanel();
            }

            ImGui::EndChild();
        }

        ImGui::End();
    }

    void DialogueEditorPanel::OpenDialogue(const std::filesystem::path& path)
    {
        LoadDialogue(path);
        if (!m_Nodes.empty())
            m_CurrentFilePath = path;
    }

    void DialogueEditorPanel::OpenDialogue(AssetHandle handle)
    {
        // Resolve path from asset metadata's relative path
        auto metadata = AssetManager::GetAssetMetadata(handle);
        if (metadata.IsValid())
        {
            auto fsPath = Project::GetAssetFileSystemPath(metadata.FilePath);
            LoadDialogue(fsPath);
            if (!m_Nodes.empty())
            {
                m_CurrentAssetHandle = handle;
                m_CurrentFilePath = fsPath;
            }
        }
    }

    // =========================================================================
    // Canvas
    // =========================================================================

    void DialogueEditorPanel::DrawCanvas(f32 width)
    {
        // Begin() paints the background and grid, consumes pan/zoom and clips to
        // its own child region - everything this function used to do by hand.
        if (!m_Canvas.Begin("##NodeCanvas", ImVec2(width, 0.0f)))
        {
            // Clipped away: no geometry to hit-test against and no release to
            // observe, so anything in flight has to end here.
            CancelInteractions();
            return;
        }

        DrawConnections();
        DrawNodes();
        DrawConnectionInProgress();
        DrawMinimap();

        HandleCanvasInput();
        HandleNodeInteraction();
        HandleConnectionDrag();
        DrawContextMenu();

        m_Canvas.End();
    }

    void DialogueEditorPanel::DrawNodes()
    {
        for (auto& node : m_Nodes)
        {
            DrawNode(node);
        }
    }

    void DialogueEditorPanel::DrawNode(DialogueNodeData& node)
    {
        ImDrawList* drawList = m_Canvas.GetDrawList();
        f32 const zoom = m_Canvas.GetZoom();
        ImVec2 const nodePos = m_Canvas.ToScreen(node.EditorPosition);
        ImVec2 const nodeSize = GetNodeSize(node);

        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        ImVec2 const headerEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + s_NodeHeaderHeight * zoom);

        bool const isSelected = (node.ID == m_SelectedNodeID);
        bool const isRoot = (node.ID == m_RootNodeID);

        // Node body
        f32 const rounding = 6.0f * zoom;
        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.Type), rounding);

        // Header
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.Type), rounding, ImDrawFlags_RoundCornersTop);

        // Selection outline
        if (isSelected)
        {
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 220), rounding, 0, 2.5f * zoom);
        }

        // Root node indicator
        if (isRoot)
        {
            drawList->AddRect(
                ImVec2(nodePos.x - 2.0f, nodePos.y - 2.0f),
                ImVec2(nodeEnd.x + 2.0f, nodeEnd.y + 2.0f),
                IM_COL32(50, 200, 50, 180), rounding + 2.0f, 0, 1.5f * zoom);
        }

        // Header text
        f32 const fontSize = 13.0f * zoom;
        std::string headerText = node.Name.empty() ? node.Type : node.Name;
        ImVec2 const textPos = ImVec2(nodePos.x + s_NodePadding * zoom, nodePos.y + 4.0f * zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 240), headerText.c_str());

        // Type badge (small text)
        if (!node.Name.empty())
        {
            std::string typeLabel = "[" + node.Type + "]";
            ImVec2 const badgeSize = ImGui::CalcTextSize(typeLabel.c_str());
            ImVec2 const badgePos = ImVec2(
                nodeEnd.x - (badgeSize.x + s_NodePadding) * zoom,
                nodePos.y + 6.0f * zoom);
            drawList->AddText(nullptr, 10.0f * zoom, badgePos, IM_COL32(200, 200, 200, 150), typeLabel.c_str());
        }

        // Content preview
        f32 contentY = nodePos.y + s_NodeHeaderHeight * zoom + 4.0f * zoom;

        if (node.Type == "dialogue")
        {
            // Show speaker + text excerpt
            if (auto it = node.Properties.find("speaker"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                {
                    drawList->AddText(nullptr, 11.0f * zoom,
                                      ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                                      IM_COL32(255, 200, 100, 220), str->c_str());
                    contentY += 14.0f * zoom;
                }
            }
            if (auto it = node.Properties.find("text"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string preview = *str;
                    if (preview.size() > 35)
                        preview = preview.substr(0, 32) + "...";
                    drawList->AddText(nullptr, 10.0f * zoom,
                                      ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                                      IM_COL32(200, 200, 200, 200), preview.c_str());
                }
            }
        }
        else if (node.Type == "condition")
        {
            if (auto it = node.Properties.find("conditionExpression"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string label = "if: " + *str;
                    drawList->AddText(nullptr, 11.0f * zoom,
                                      ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                                      IM_COL32(150, 200, 255, 220), label.c_str());
                }
            }
        }
        else if (node.Type == "action")
        {
            if (auto it = node.Properties.find("actionName"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string label = "do: " + *str;
                    drawList->AddText(nullptr, 11.0f * zoom,
                                      ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                                      IM_COL32(255, 180, 150, 220), label.c_str());
                }
            }
        }
        else
        {
            // No additional handling required.
        }

        // Draw ports
        auto ports = GetNodePorts(node, nodePos);
        for (const auto& port : ports)
        {
            ImU32 const portColor = port.IsOutput ? IM_COL32(100, 200, 100, 220) : IM_COL32(100, 150, 255, 220);
            ImU32 const portFill = IM_COL32(40, 40, 45, 255);
            f32 const radius = s_NodePortRadius * zoom;

            drawList->AddCircleFilled(port.Position, radius, portFill);
            drawList->AddCircle(port.Position, radius, portColor, 12, 2.0f * zoom);

            // Port label
            f32 const labelOffset = (radius + 4.0f * zoom);
            ImVec2 labelPos;
            if (port.IsOutput)
            {
                ImVec2 const textSize = ImGui::CalcTextSize(port.Name.c_str());
                labelPos = ImVec2(port.Position.x - labelOffset - textSize.x * zoom, port.Position.y - 5.0f * zoom);
            }
            else
            {
                labelPos = ImVec2(port.Position.x + labelOffset, port.Position.y - 5.0f * zoom);
            }
            drawList->AddText(nullptr, 10.0f * zoom, labelPos, IM_COL32(180, 180, 180, 200), port.Name.c_str());
        }
    }

    ImVec2 DialogueEditorPanel::GetNodeSize(const DialogueNodeData& node) const
    {
        f32 const zoom = m_Canvas.GetZoom();
        f32 width = s_NodeWidth * zoom;
        f32 height = s_NodeHeaderHeight * zoom;

        // Content area
        if (node.Type == "dialogue")
            height += 36.0f * zoom; // speaker + text preview
        else if (node.Type == "condition" || node.Type == "action")
            height += 20.0f * zoom;
        else
            height += 10.0f * zoom;

        // Ports
        i32 portCount = 1; // At least input
        if (node.Type == "choice")
        {
            // Count outgoing connections for port count
            i32 outputCount = 0;
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID == node.ID)
                    ++outputCount;
            }
            portCount = std::max(portCount, outputCount + 1); // +1 for adding new
        }
        else if (node.Type == "condition")
        {
            portCount = 2; // true + false
        }
        else
        {
            portCount = 1; // single output
        }

        height += static_cast<f32>(portCount) * s_NodePortSpacing * zoom;
        height += s_NodePadding * zoom * 2.0f;

        return ImVec2(width, height);
    }

    auto DialogueEditorPanel::GetNodeColor(const std::string& type) const -> ImU32
    {
        if (type == "dialogue")
        {
            return IM_COL32(50, 55, 75, 230);
        }
        if (type == "choice")
        {
            return IM_COL32(60, 50, 70, 230);
        }
        if (type == "condition")
        {
            return IM_COL32(50, 65, 70, 230);
        }
        if (type == "action")
        {
            return IM_COL32(70, 55, 50, 230);
        }
        return IM_COL32(55, 55, 55, 230);
    }

    auto DialogueEditorPanel::GetNodeHeaderColor(const std::string& type) const -> ImU32
    {
        if (type == "dialogue")
        {
            return IM_COL32(60, 100, 170, 240);
        }
        if (type == "choice")
        {
            return IM_COL32(140, 80, 160, 240);
        }
        if (type == "condition")
        {
            return IM_COL32(70, 150, 160, 240);
        }
        if (type == "action")
        {
            return IM_COL32(180, 100, 60, 240);
        }
        return IM_COL32(100, 100, 100, 240);
    }

    std::vector<DialogueEditorPanel::PortInfo> DialogueEditorPanel::GetNodePorts(
        const DialogueNodeData& node, const ImVec2& nodeScreenPos) const
    {
        f32 const zoom = m_Canvas.GetZoom();
        std::vector<PortInfo> ports;
        ImVec2 const nodeSize = GetNodeSize(node);

        f32 const portAreaY = nodeScreenPos.y + s_NodeHeaderHeight * zoom + nodeSize.y * 0.4f;

        // Input port (all nodes except root conceptually have one, but we always draw it)
        {
            PortInfo input;
            input.Position = ImVec2(nodeScreenPos.x, portAreaY);
            input.NodeID = node.ID;
            input.Name = "in";
            input.IsOutput = false;
            ports.push_back(input);
        }

        // Output ports vary by type
        if (node.Type == "condition")
        {
            PortInfo truePort;
            truePort.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY);
            truePort.NodeID = node.ID;
            truePort.Name = "true";
            truePort.IsOutput = true;
            ports.push_back(truePort);

            PortInfo falsePort;
            falsePort.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + s_NodePortSpacing * zoom);
            falsePort.NodeID = node.ID;
            falsePort.Name = "false";
            falsePort.IsOutput = true;
            ports.push_back(falsePort);
        }
        else if (node.Type == "choice")
        {
            // One output per connected choice + one "add" slot
            i32 portIndex = 0;
            std::vector<std::string> choiceLabels;
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID == node.ID)
                {
                    choiceLabels.push_back(conn.SourcePort.empty() ? ("choice " + std::to_string(portIndex + 1)) : conn.SourcePort);
                    ++portIndex;
                }
            }

            for (i32 i = 0; i < static_cast<i32>(choiceLabels.size()); ++i)
            {
                PortInfo p;
                p.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + static_cast<f32>(i) * s_NodePortSpacing * zoom);
                p.NodeID = node.ID;
                p.Name = choiceLabels[i];
                p.IsOutput = true;
                ports.push_back(p);
            }

            // "+" port for adding new choice connections
            PortInfo addPort;
            addPort.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + static_cast<f32>(portIndex) * s_NodePortSpacing * zoom);
            addPort.NodeID = node.ID;
            addPort.Name = "+";
            addPort.IsOutput = true;
            ports.push_back(addPort);
        }
        else
        {
            // Single output
            PortInfo output;
            output.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY);
            output.NodeID = node.ID;
            output.Name = "out";
            output.IsOutput = true;
            ports.push_back(output);
        }

        return ports;
    }

    // =========================================================================
    // Connections
    // =========================================================================

    void DialogueEditorPanel::DrawConnections()
    {
        for (size_t ci = 0; ci < m_Connections.size(); ++ci)
        {
            const auto& conn = m_Connections[ci];

            // Find source and target nodes
            const DialogueNodeData* srcNode = nullptr;
            const DialogueNodeData* dstNode = nullptr;
            for (const auto& n : m_Nodes)
            {
                if (n.ID == conn.SourceNodeID)
                    srcNode = &n;
                if (n.ID == conn.TargetNodeID)
                    dstNode = &n;
            }
            if (!srcNode || !dstNode)
                continue;

            // Find port positions
            ImVec2 const srcNodePos = m_Canvas.ToScreen(srcNode->EditorPosition);
            ImVec2 const dstNodePos = m_Canvas.ToScreen(dstNode->EditorPosition);

            auto srcPorts = GetNodePorts(*srcNode, srcNodePos);
            auto dstPorts = GetNodePorts(*dstNode, dstNodePos);

            ImVec2 startPos = srcNodePos;
            ImVec2 endPos = dstNodePos;

            // Find matching output port on source
            for (const auto& port : srcPorts)
            {
                if (port.IsOutput && port.Name == conn.SourcePort)
                {
                    startPos = port.Position;
                    break;
                }
                if (port.IsOutput && conn.SourcePort.empty() && port.Name == "out")
                {
                    startPos = port.Position;
                    break;
                }
            }

            // Find input port on target
            for (const auto& port : dstPorts)
            {
                if (!port.IsOutput)
                {
                    endPos = port.Position;
                    break;
                }
            }

            // A dialogue edge is directional - it says which node comes next -
            // so it gets the widget's arrowed wire. Thickness is in SCREEN
            // pixels, deliberately not scaled by zoom: a hairline at the canvas'
            // minimum zoom is invisible. See GraphCanvas::DrawWire.
            m_Canvas.DrawDirectionalWire(startPos, endPos, IM_COL32(180, 180, 200, 200), 2.0f);
        }
    }

    void DialogueEditorPanel::DrawConnectionInProgress()
    {
        if (!m_IsCreatingConnection)
            return;

        const DialogueNodeData* srcNode = nullptr;
        for (const auto& n : m_Nodes)
        {
            if (n.ID == m_ConnectionStartNodeID)
            {
                srcNode = &n;
                break;
            }
        }
        if (!srcNode)
            return;

        ImVec2 const srcNodePos = m_Canvas.ToScreen(srcNode->EditorPosition);
        auto srcPorts = GetNodePorts(*srcNode, srcNodePos);

        ImVec2 startPos = srcNodePos;
        for (const auto& port : srcPorts)
        {
            if (port.Name == m_ConnectionStartPort)
            {
                startPos = port.Position;
                break;
            }
        }

        // DrawWire always leaves `from` rightwards and enters `to` leftwards, so
        // a wire dragged BACKWARDS out of an input port is the same curve with
        // its endpoints swapped, not a second set of mirrored control points.
        constexpr ImU32 pendingColor = IM_COL32(255, 255, 100, 180);
        if (m_ConnectionStartIsOutput)
            m_Canvas.DrawWire(startPos, m_ConnectionEndPos, pendingColor, 2.0f);
        else
            m_Canvas.DrawWire(m_ConnectionEndPos, startPos, pendingColor, 2.0f);
    }

    void DialogueEditorPanel::DrawMinimap()
    {
        if (m_Nodes.empty())
            return;

        ImDrawList* drawList = m_Canvas.GetDrawList();
        ImVec2 const canvasOrigin = m_Canvas.GetOrigin();
        ImVec2 const canvasSize = m_Canvas.GetSize();

        f32 const mmSize = s_MinimapSize;
        ImVec2 const mmOrigin = ImVec2(
            canvasOrigin.x + canvasSize.x - mmSize - 10.0f,
            canvasOrigin.y + canvasSize.y - mmSize - 10.0f);
        ImVec2 const mmEnd = ImVec2(mmOrigin.x + mmSize, mmOrigin.y + mmSize);

        // Background
        drawList->AddRectFilled(mmOrigin, mmEnd, IM_COL32(20, 20, 25, 180), 4.0f);
        drawList->AddRect(mmOrigin, mmEnd, IM_COL32(80, 80, 90, 200), 4.0f);

        // Find world bounds
        f32 minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (const auto& node : m_Nodes)
        {
            minX = std::min(minX, node.EditorPosition.x);
            minY = std::min(minY, node.EditorPosition.y);
            maxX = std::max(maxX, node.EditorPosition.x + s_NodeWidth);
            maxY = std::max(maxY, node.EditorPosition.y + 100.0f);
        }

        f32 const worldW = std::max(maxX - minX, 1.0f);
        f32 const worldH = std::max(maxY - minY, 1.0f);
        f32 const mmScale = std::min((mmSize - 8.0f) / worldW, (mmSize - 8.0f) / worldH);

        // Draw node dots
        for (const auto& node : m_Nodes)
        {
            f32 const nx = mmOrigin.x + 4.0f + (node.EditorPosition.x - minX) * mmScale;
            f32 const ny = mmOrigin.y + 4.0f + (node.EditorPosition.y - minY) * mmScale;
            ImU32 const color = (node.ID == m_SelectedNodeID)
                                    ? IM_COL32(255, 200, 50, 255)
                                    : GetNodeHeaderColor(node.Type);
            drawList->AddRectFilled(ImVec2(nx, ny), ImVec2(nx + 4.0f, ny + 3.0f), color);
        }
    }

    // =========================================================================
    // Interaction
    // =========================================================================

    void DialogueEditorPanel::HandleCanvasInput()
    {
        // Zoom and pan were handled here by hand; GraphCanvas::Begin has already
        // consumed both by the time this runs.
        bool const isHovered = m_Canvas.IsHovered();

        // Right-CLICK, not right-press: the canvas pans on right-DRAG, and only
        // it owns the threshold that tells the two gestures apart.
        if (m_Canvas.WasRightClicked() && !m_IsCreatingConnection && !m_SuppressNextContextMenu)
        {
            m_ContextMenuGraphPos = m_Canvas.ToGraph(ImGui::GetIO().MousePos);
            m_ShowContextMenu = true;
        }
        // Reset on ANY right release, not just a hovered one, so a gesture that
        // ends outside the canvas can't suppress the next real click.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            m_SuppressNextContextMenu = false;
        }

        // Click on empty space to deselect. Suppressed while the canvas is
        // panning: a left click can land in the middle of a right-drag pan (both
        // buttons down at once), and that must not also clear the selection.
        if (isHovered && !m_Canvas.IsPanning() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDraggingNode)
        {
            if (HitTestNode(ImGui::GetIO().MousePos) == nullptr)
            {
                m_SelectedNodeID = 0;
            }
        }
    }

    void DialogueEditorPanel::HandleShortcuts()
    {
        // Every one of these is a GLOBAL ImGui::IsKeyPressed read, so they all
        // need the focus gate: an open-but-unfocused Dialogue Editor would
        // otherwise answer a Ctrl+S meant for the scene, or delete the node it
        // has selected when Delete was aimed at another panel's list.
        if (!m_IsFocused)
        {
            return;
        }

        // Delete selected node
        if (m_SelectedNodeID != 0 && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsAnyItemActive())
        {
            DeleteNode(m_SelectedNodeID);
        }

        // Ctrl+S to save
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            SaveDialogue();
        }

        // Ctrl+N for new dialogue
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
        {
            NewDialogue();
        }
    }

    void DialogueEditorPanel::CancelInteractions()
    {
        if (m_IsDraggingNode)
        {
            // EditorPosition was already written frame by frame, so the move is
            // real and still needs its undo entry; only the drag is abandoned.
            PushDialogueUndoCommand(m_DragStartSnapshot, "Move Node");
            m_IsDraggingNode = false;
        }
        m_IsCreatingConnection = false;
        m_SuppressNextContextMenu = false;
    }

    const DialogueNodeData* DialogueEditorPanel::HitTestNode(ImVec2 screenPos) const
    {
        // Reverse iterate so nodes drawn last (topmost) are hit first.
        for (auto it = m_Nodes.rbegin(); it != m_Nodes.rend(); ++it)
        {
            ImVec2 const nodePos = m_Canvas.ToScreen(it->EditorPosition);
            ImVec2 const nodeSize = GetNodeSize(*it);
            if (screenPos.x >= nodePos.x && screenPos.x <= nodePos.x + nodeSize.x &&
                screenPos.y >= nodePos.y && screenPos.y <= nodePos.y + nodeSize.y)
            {
                return &*it;
            }
        }
        return nullptr;
    }

    void DialogueEditorPanel::HandleNodeInteraction()
    {
        ImVec2 const mousePos = ImGui::GetIO().MousePos;

        // Only NEW presses are suppressed while the canvas is panning. The drag
        // and release paths below must keep running, or a pan latched mid-drag
        // (a second button going down) would strand m_IsDraggingNode with a stale
        // offset and never push its undo command.
        if (m_Canvas.IsHovered() && !m_Canvas.IsPanning() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            // The hit radius has a screen-pixel floor because a port that scales
            // with zoom is untargetable once zoomed out.
            f32 const portHitRadius = std::max(s_PortHitRadiusMin, m_Canvas.Scaled(s_NodePortRadius * 2.0f));
            if (const DialogueNodeData* hit = HitTestNode(mousePos); hit != nullptr)
            {
                ImVec2 const nodePos = m_Canvas.ToScreen(hit->EditorPosition);

                // Check if clicking on a port first
                auto ports = GetNodePorts(*hit, nodePos);
                bool clickedPort = false;
                for (const auto& port : ports)
                {
                    f32 const dist = std::hypot(mousePos.x - port.Position.x, mousePos.y - port.Position.y);
                    if (dist <= portHitRadius)
                    {
                        // Start connection drag
                        m_IsCreatingConnection = true;
                        m_ConnectionStartNodeID = port.NodeID;
                        m_ConnectionStartPort = port.Name;
                        m_ConnectionStartIsOutput = port.IsOutput;
                        m_ConnectionEndPos = mousePos;
                        clickedPort = true;
                        break;
                    }
                }

                if (!clickedPort)
                {
                    m_SelectedNodeID = hit->ID;
                    m_IsDraggingNode = true;
                    m_DragStartOffset = glm::vec2(
                        mousePos.x - nodePos.x,
                        mousePos.y - nodePos.y);
                    m_DragStartSnapshot = CaptureSnapshot();
                }
            }
        }

        // Drag node
        if (m_IsDraggingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            for (auto& node : m_Nodes)
            {
                if (node.ID == m_SelectedNodeID)
                {
                    ImVec2 const targetScreen = ImVec2(
                        mousePos.x - m_DragStartOffset.x,
                        mousePos.y - m_DragStartOffset.y);
                    node.EditorPosition = m_Canvas.ToGraph(targetScreen);
                    m_IsDirty = true;
                    break;
                }
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_IsDraggingNode)
            {
                PushDialogueUndoCommand(m_DragStartSnapshot, "Move Node");
            }
            m_IsDraggingNode = false;
        }
    }

    void DialogueEditorPanel::HandleConnectionDrag()
    {
        if (!m_IsCreatingConnection)
            return;

        m_ConnectionEndPos = ImGui::GetIO().MousePos;

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // Check if released on a port
            ImVec2 const mousePos = ImGui::GetIO().MousePos;
            bool connected = false;
            auto oldSnapshot = CaptureSnapshot();
            // Slightly wider than the press radius, as before: releasing a drag
            // is a coarser gesture than starting one.
            f32 const dropRadius = std::max(s_PortHitRadiusMin * 1.25f, m_Canvas.Scaled(s_NodePortRadius * 2.5f));

            for (const auto& node : m_Nodes)
            {
                if (node.ID == m_ConnectionStartNodeID)
                    continue; // No self-connections

                ImVec2 const nodePos = m_Canvas.ToScreen(node.EditorPosition);
                auto ports = GetNodePorts(node, nodePos);

                for (const auto& port : ports)
                {
                    f32 const dist = std::hypot(mousePos.x - port.Position.x, mousePos.y - port.Position.y);
                    if (dist <= dropRadius)
                    {
                        // Ensure we connect output -> input
                        if (m_ConnectionStartIsOutput && !port.IsOutput)
                        {
                            DialogueConnection conn;
                            conn.SourceNodeID = m_ConnectionStartNodeID;
                            conn.TargetNodeID = port.NodeID;
                            conn.SourcePort = ResolveSourcePort(m_ConnectionStartPort, m_ConnectionStartNodeID);
                            conn.TargetPort = port.Name;
                            // Prevent duplicate connections
                            bool duplicate = std::ranges::any_of(m_Connections, [&conn](const auto& c)
                                                                 { return c.SourceNodeID == conn.SourceNodeID && c.SourcePort == conn.SourcePort && c.TargetNodeID == conn.TargetNodeID && c.TargetPort == conn.TargetPort; });
                            if (!duplicate)
                            {
                                m_Connections.push_back(conn);
                                m_IsDirty = true;
                            }
                            connected = true;
                        }
                        else if (!m_ConnectionStartIsOutput && port.IsOutput)
                        {
                            DialogueConnection conn;
                            conn.SourceNodeID = port.NodeID;
                            conn.TargetNodeID = m_ConnectionStartNodeID;
                            conn.SourcePort = ResolveSourcePort(port.Name, port.NodeID);
                            conn.TargetPort = m_ConnectionStartPort;
                            // Prevent duplicate connections
                            bool duplicate = std::ranges::any_of(m_Connections, [&conn](const auto& c)
                                                                 { return c.SourceNodeID == conn.SourceNodeID && c.SourcePort == conn.SourcePort && c.TargetNodeID == conn.TargetNodeID && c.TargetPort == conn.TargetPort; });
                            if (!duplicate)
                            {
                                m_Connections.push_back(conn);
                                m_IsDirty = true;
                            }
                            connected = true;
                        }
                        else
                        {
                            // No additional handling required.
                        }
                        break;
                    }
                }
                if (connected)
                    break;
            }

            if (connected)
            {
                PushDialogueUndoCommand(oldSnapshot, "Create Connection");
            }

            m_IsCreatingConnection = false;
        }

        // Cancel with right click, and swallow the context menu that the
        // matching release would otherwise open.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_IsCreatingConnection = false;
            m_SuppressNextContextMenu = true;
        }
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void DialogueEditorPanel::DrawToolbar()
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N"))
                    NewDialogue();
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                    SaveDialogue();
                if (ImGui::MenuItem("Save As..."))
                    SaveDialogueAs();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Add Node"))
            {
                // The centre of the CANVAS, not of the window: the old form fed
                // the window position in as the canvas origin, so "add node"
                // from the menu bar dropped nodes at an arbitrary graph position
                // that moved with the panel.
                ImVec2 const canvasOrigin = m_Canvas.GetOrigin();
                ImVec2 const canvasSize = m_Canvas.GetSize();
                glm::vec2 const center = m_Canvas.ToGraph(
                    ImVec2(canvasOrigin.x + canvasSize.x * 0.5f, canvasOrigin.y + canvasSize.y * 0.5f));

                if (ImGui::MenuItem("Dialogue Node"))
                    CreateNode("dialogue", center);
                if (ImGui::MenuItem("Choice Node"))
                    CreateNode("choice", center);
                if (ImGui::MenuItem("Condition Node"))
                    CreateNode("condition", center);
                if (ImGui::MenuItem("Action Node"))
                    CreateNode("action", center);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Reset View"))
                {
                    m_Canvas.ResetView();
                }
                if (ImGui::MenuItem("Zoom to Fit"))
                {
                    // The old form fitted against ImGui::GetContentRegionAvail()
                    // read inside the MENU BAR, which is not the canvas' size, so
                    // the resulting zoom was wrong by whatever the menu bar
                    // happened to measure.
                    FrameAll();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            ImGui::Checkbox("Preview", &m_ShowPreview);

            if (m_IsDirty)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "(unsaved)");
            }

            ImGui::EndMenuBar();
        }
    }

    // =========================================================================
    // Property Panel
    // =========================================================================

    void DialogueEditorPanel::DrawPropertyPanel()
    {
        auto* node = FindNodeMutable(m_SelectedNodeID);
        if (!node)
            return;

        ImGui::Text("Node Properties");
        ImGui::Separator();

        DrawNodeProperties(*node);
    }

    void DialogueEditorPanel::DrawNodeProperties(DialogueNodeData& node)
    {
        // Snapshot at start of edit session (not every frame)
        if (m_CommandHistory && !m_IsEditingProperties)
        {
            m_PropertyEditSnapshot = CaptureSnapshot();
        }

        bool anyChanged = false;
        // Node name
        char nameBuf[256];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", node.Name.c_str());
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
        {
            node.Name = nameBuf;
            m_IsDirty = true;
            anyChanged = true;
        }

        // Node type (read-only display)
        ImGui::Text("Type: %s", node.Type.c_str());

        // Node ID
        ImGui::Text("ID: %llu", static_cast<unsigned long long>(node.ID));

        // Root node toggle
        if (bool isRoot = (node.ID == m_RootNodeID); ImGui::Checkbox("Root Node", &isRoot))
        {
            if (isRoot)
            {
                m_RootNodeID = node.ID;
                m_IsDirty = true;
                anyChanged = true;
            }
        }

        ImGui::Separator();

        // Type-specific properties
        if (node.Type == "dialogue")
        {
            // Speaker
            std::string speaker;
            if (auto it = node.Properties.find("speaker"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    speaker = *str;
            }
            char speakerBuf[256];
            std::snprintf(speakerBuf, sizeof(speakerBuf), "%s", speaker.c_str());
            if (ImGui::InputText("Speaker", speakerBuf, sizeof(speakerBuf)))
            {
                node.Properties["speaker"] = std::string(speakerBuf);
                m_IsDirty = true;
                anyChanged = true;
            }

            // Text (multiline)
            std::string text;
            if (auto it = node.Properties.find("text"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    text = *str;
            }
            char textBuf[4096];
            std::snprintf(textBuf, sizeof(textBuf), "%s", text.c_str());
            if (ImGui::InputTextMultiline("Text", textBuf, sizeof(textBuf), ImVec2(-1, 100)))
            {
                node.Properties["text"] = std::string(textBuf);
                m_IsDirty = true;
                anyChanged = true;
            }
        }
        else if (node.Type == "condition")
        {
            std::string expr;
            if (auto it = node.Properties.find("conditionExpression"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    expr = *str;
            }
            char exprBuf[512];
            std::snprintf(exprBuf, sizeof(exprBuf), "%s", expr.c_str());
            if (ImGui::InputText("Condition", exprBuf, sizeof(exprBuf)))
            {
                node.Properties["conditionExpression"] = std::string(exprBuf);
                m_IsDirty = true;
                anyChanged = true;
            }

            std::string condArgs;
            if (auto it = node.Properties.find("conditionArgs"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    condArgs = *str;
            }
            char condArgsBuf[512];
            std::snprintf(condArgsBuf, sizeof(condArgsBuf), "%s", condArgs.c_str());
            if (ImGui::InputText("Condition Args", condArgsBuf, sizeof(condArgsBuf)))
            {
                node.Properties["conditionArgs"] = std::string(condArgsBuf);
                m_IsDirty = true;
                anyChanged = true;
            }

            ImGui::TextWrapped("Variable name checked against DialogueVariables.\ntrue/false ports route flow.");
        }
        else if (node.Type == "action")
        {
            std::string actionName;
            if (auto it = node.Properties.find("actionName"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    actionName = *str;
            }
            char actionBuf[256];
            std::snprintf(actionBuf, sizeof(actionBuf), "%s", actionName.c_str());
            if (ImGui::InputText("Action", actionBuf, sizeof(actionBuf)))
            {
                node.Properties["actionName"] = std::string(actionBuf);
                m_IsDirty = true;
                anyChanged = true;
            }

            std::string actionArgs;
            if (auto it = node.Properties.find("actionArgs"); it != node.Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    actionArgs = *str;
            }
            char argsBuf[512];
            std::snprintf(argsBuf, sizeof(argsBuf), "%s", actionArgs.c_str());
            if (ImGui::InputText("Arguments", argsBuf, sizeof(argsBuf)))
            {
                node.Properties["actionArgs"] = std::string(argsBuf);
                m_IsDirty = true;
                anyChanged = true;
            }
        }
        else if (node.Type == "choice")
        {
            ImGui::TextWrapped("Connect output ports to dialogue nodes.\nPort names become choice labels.");

            ImGui::Separator();
            ImGui::Text("Choice Labels:");

            // Show/edit labels for outgoing connections
            for (auto& conn : m_Connections)
            {
                if (conn.SourceNodeID != node.ID)
                    continue;

                char labelBuf[256];
                std::snprintf(labelBuf, sizeof(labelBuf), "%s", conn.SourcePort.c_str());
                if (std::string inputLabel = "##choice_" + std::to_string(static_cast<u64>(conn.TargetNodeID)); ImGui::InputText(inputLabel.c_str(), labelBuf, sizeof(labelBuf)))
                {
                    conn.SourcePort = labelBuf;
                    m_IsDirty = true;
                    anyChanged = true;
                }

                ImGui::SameLine();
                std::string delBtn = "X##del_" + std::to_string(static_cast<u64>(conn.TargetNodeID));
                if (ImGui::SmallButton(delBtn.c_str()))
                {
                    // Find and remove this connection
                    for (size_t i = 0; i < m_Connections.size(); ++i)
                    {
                        if (m_Connections[i].SourceNodeID == conn.SourceNodeID &&
                            m_Connections[i].TargetNodeID == conn.TargetNodeID)
                        {
                            DeleteConnection(i);
                            m_IsDirty = true;
                            break;
                        }
                    }
                    break; // Iterator invalidated
                }
            }
        }
        else
        {
            // No additional handling required.
        }

        // Position
        ImGui::Separator();
        if (ImGui::DragFloat2("Position", &node.EditorPosition.x, 1.0f))
        {
            m_IsDirty = true;
            anyChanged = true;
        }

        // Connections from/to this node
        ImGui::Separator();
        ImGui::Text("Connections:");
        i32 connIdx = 0;
        for (size_t i = 0; i < m_Connections.size(); ++i)
        {
            const auto& conn = m_Connections[i];
            if (conn.SourceNodeID == node.ID || conn.TargetNodeID == node.ID)
            {
                bool isSource = (conn.SourceNodeID == node.ID);
                const DialogueNodeData* other = nullptr;
                for (const auto& n : m_Nodes)
                {
                    if (n.ID == (isSource ? conn.TargetNodeID : conn.SourceNodeID))
                    {
                        other = &n;
                        break;
                    }
                }

                std::string label = isSource ? "->" : "<-";
                label += " " + (other ? other->Name : "???");
                if (!conn.SourcePort.empty())
                    label += " [" + conn.SourcePort + "]";

                ImGui::Text("%s", label.c_str());
                ImGui::SameLine();
                if (std::string delLabel = "X##conn_" + std::to_string(connIdx); ImGui::SmallButton(delLabel.c_str()))
                {
                    DeleteConnection(i);
                    break;
                }
                ++connIdx;
            }
        }

        // Track property edit sessions for undo
        if (m_CommandHistory)
        {
            if (anyChanged)
            {
                m_IsEditingProperties = true;
            }

            if (m_IsEditingProperties && GImGui->ActiveId == 0)
            {
                PushDialogueUndoCommand(m_PropertyEditSnapshot, "Edit Node Properties");
                m_IsEditingProperties = false;
            }
        }
    }

    // =========================================================================
    // Preview / Playtest
    // =========================================================================

    void DialogueEditorPanel::DrawPreviewPanel()
    {
        ImGui::Text("Dialogue Preview");
        ImGui::Separator();

        if (!m_PreviewActive)
        {
            if (ImGui::Button("Start Preview", ImVec2(-1, 0)))
            {
                PreviewReset();
                m_PreviewActive = true;
                if (m_RootNodeID != 0)
                {
                    m_PreviewCurrentNodeID = m_RootNodeID;
                    // Process the root node
                    PreviewAdvance();
                }
            }

            // Test variables
            ImGui::Separator();
            ImGui::Text("Test Variables:");

            static char varName[128] = "";
            static bool varValue = false;
            ImGui::InputText("Var Name", varName, sizeof(varName));
            ImGui::Checkbox("Value", &varValue);
            if (ImGui::Button("Set Variable") && varName[0] != '\0')
            {
                m_PreviewVariables.SetBool(varName, varValue);
            }

            return;
        }

        // Active preview
        if (!m_PreviewCurrentSpeaker.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", m_PreviewCurrentSpeaker.c_str());
        }

        if (!m_PreviewCurrentText.empty())
        {
            ImGui::TextWrapped("%s", m_PreviewCurrentText.c_str());
        }

        ImGui::Spacing();

        if (!m_PreviewChoices.empty())
        {
            ImGui::Text("Choices:");
            for (i32 i = 0; i < static_cast<i32>(m_PreviewChoices.size()); ++i)
            {
                std::string btnLabel = std::to_string(i + 1) + ". " + m_PreviewChoices[i].Text;
                if (ImGui::Button(btnLabel.c_str(), ImVec2(-1, 0)))
                {
                    PreviewSelectChoice(i);
                }
            }
        }
        else if (!m_PreviewCurrentText.empty())
        {
            if (ImGui::Button("Continue >>", ImVec2(-1, 0)))
            {
                // Follow default connection
                for (const auto& conn : m_Connections)
                {
                    if (conn.SourceNodeID == m_PreviewCurrentNodeID)
                    {
                        m_PreviewCurrentNodeID = conn.TargetNodeID;
                        PreviewAdvance();
                        break;
                    }
                }
            }
        }
        else
        {
            // No additional handling required.
        }

        ImGui::Spacing();
        if (ImGui::Button("Restart", ImVec2(-1, 0)))
        {
            PreviewReset();
            m_PreviewActive = true;
            m_PreviewCurrentNodeID = m_RootNodeID;
            PreviewAdvance();
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            m_PreviewActive = false;
            m_PreviewCurrentNodeID = 0;
        }
    }

    void DialogueEditorPanel::PreviewAdvance(u32 hopCount)
    {
        if (hopCount >= 256)
        {
            m_PreviewActive = false;
            m_PreviewCurrentText = "[Exceeded max hop count - possible cycle]";
            m_PreviewCurrentSpeaker.clear();
            m_PreviewChoices.clear();
            return;
        }

        auto* node = FindNodeMutable(m_PreviewCurrentNodeID);
        if (!node)
        {
            m_PreviewActive = false;
            m_PreviewCurrentText = "[End of dialogue]";
            m_PreviewCurrentSpeaker.clear();
            m_PreviewChoices.clear();
            return;
        }

        // Highlight current node in editor
        m_SelectedNodeID = node->ID;

        if (node->Type == "dialogue")
        {
            m_PreviewCurrentSpeaker.clear();
            m_PreviewCurrentText.clear();
            m_PreviewChoices.clear();

            if (auto it = node->Properties.find("speaker"); it != node->Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    m_PreviewCurrentSpeaker = *str;
            }
            if (auto it = node->Properties.find("text"); it != node->Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    m_PreviewCurrentText = *str;
            }
        }
        else if (node->Type == "choice")
        {
            m_PreviewChoices.clear();
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID != node->ID)
                    continue;

                DialogueChoice choice;
                choice.TargetNodeID = conn.TargetNodeID;
                choice.Text = conn.SourcePort.empty() ? "..." : conn.SourcePort;
                m_PreviewChoices.push_back(std::move(choice));
            }
        }
        else if (node->Type == "condition")
        {
            std::string conditionName;
            if (auto it = node->Properties.find("conditionExpression"); it != node->Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    conditionName = *str;
            }

            bool result = m_PreviewVariables.GetBool(conditionName);

            // Follow true/false branch
            UUID nextID = 0;
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID != node->ID)
                    continue;
                if (result && conn.SourcePort == "true")
                {
                    nextID = conn.TargetNodeID;
                    break;
                }
                if (!result && conn.SourcePort == "false")
                {
                    nextID = conn.TargetNodeID;
                    break;
                }
            }

            if (static_cast<u64>(nextID) == 0)
            {
                // Fallback to first connection
                for (const auto& conn : m_Connections)
                {
                    if (conn.SourceNodeID == node->ID)
                    {
                        nextID = conn.TargetNodeID;
                        break;
                    }
                }
            }

            if (static_cast<u64>(nextID) != 0)
            {
                m_PreviewCurrentNodeID = nextID;
                PreviewAdvance(hopCount + 1);
            }
            else
            {
                m_PreviewActive = false;
                m_PreviewCurrentText = "[Dead end - no matching branch]";
            }
        }
        else if (node->Type == "action")
        {
            std::string actionName;
            if (auto it = node->Properties.find("actionName"); it != node->Properties.end())
            {
                if (const auto* str = std::get_if<std::string>(&it->second))
                    actionName = *str;
            }

            m_PreviewCurrentText = "[Action: " + actionName + "]";
            m_PreviewCurrentSpeaker.clear();
            m_PreviewChoices.clear();

            // Auto-advance past action nodes after display
        }
        else
        {
            // No additional handling required.
        }
    }

    void DialogueEditorPanel::PreviewSelectChoice(i32 index)
    {
        if (index < 0 || index >= static_cast<i32>(m_PreviewChoices.size()))
            return;

        m_PreviewCurrentNodeID = m_PreviewChoices[index].TargetNodeID;
        PreviewAdvance();
    }

    void DialogueEditorPanel::PreviewReset()
    {
        m_PreviewCurrentNodeID = 0;
        m_PreviewCurrentText.clear();
        m_PreviewCurrentSpeaker.clear();
        m_PreviewChoices.clear();
        m_PreviewActive = false;
    }

    // =========================================================================
    // Context Menu
    // =========================================================================

    void DialogueEditorPanel::DrawContextMenu()
    {
        if (m_ShowContextMenu)
        {
            ImGui::OpenPopup("##CanvasContextMenu");
            m_ShowContextMenu = false;
        }

        if (ImGui::BeginPopup("##CanvasContextMenu"))
        {
            glm::vec2 const worldPos = m_ContextMenuGraphPos;

            ImGui::Text("Add Node:");
            ImGui::Separator();

            if (ImGui::MenuItem("Dialogue"))
            {
                CreateNode("dialogue", worldPos);
            }
            if (ImGui::MenuItem("Choice"))
            {
                CreateNode("choice", worldPos);
            }
            if (ImGui::MenuItem("Condition"))
            {
                CreateNode("condition", worldPos);
            }
            if (ImGui::MenuItem("Action"))
            {
                CreateNode("action", worldPos);
            }

            if (m_SelectedNodeID != 0)
            {
                ImGui::Separator();

                if (ImGui::MenuItem("Duplicate Node"))
                {
                    DuplicateNode(m_SelectedNodeID);
                }
                if (ImGui::MenuItem("Set as Root"))
                {
                    m_RootNodeID = m_SelectedNodeID;
                    m_IsDirty = true;
                }
                if (m_SelectedNodeID != m_RootNodeID && ImGui::MenuItem("Delete Node"))
                {
                    DeleteNode(m_SelectedNodeID);
                }
            }

            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Serialization
    // =========================================================================

    void DialogueEditorPanel::SaveDialogue()
    {
        if (m_CurrentFilePath.empty())
        {
            SaveDialogueAs();
            return;
        }

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "DialogueTree" << YAML::Value << YAML::BeginMap;

        out << YAML::Key << "RootNodeID" << YAML::Value << static_cast<u64>(m_RootNodeID);

        // Nodes
        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const auto& node : m_Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(node.ID);
            out << YAML::Key << "Type" << YAML::Value << node.Type;
            out << YAML::Key << "Name" << YAML::Value << node.Name;
            out << YAML::Key << "EditorPosition" << YAML::Value << YAML::Flow
                << YAML::BeginSeq << node.EditorPosition.x << node.EditorPosition.y << YAML::EndSeq;

            out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
            for (const auto& [key, value] : node.Properties)
            {
                out << YAML::Key << key << YAML::Value << YAML::BeginMap;

                std::visit([&out](auto&& arg)
                           {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        out << YAML::Key << "type" << YAML::Value << "bool";
                        out << YAML::Key << "value" << YAML::Value << arg;
                    }
                    else if constexpr (std::is_same_v<T, i32>)
                    {
                        out << YAML::Key << "type" << YAML::Value << "int";
                        out << YAML::Key << "value" << YAML::Value << arg;
                    }
                    else if constexpr (std::is_same_v<T, f32>)
                    {
                        out << YAML::Key << "type" << YAML::Value << "float";
                        out << YAML::Key << "value" << YAML::Value << arg;
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        out << YAML::Key << "type" << YAML::Value << "string";
                        out << YAML::Key << "value" << YAML::Value << arg;
                    }
                    else
                    {
                        // No additional handling required.
                    } }, value);

                out << YAML::EndMap;
            }
            out << YAML::EndMap;

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        // Connections
        out << YAML::Key << "Connections" << YAML::Value << YAML::BeginSeq;
        for (const auto& conn : m_Connections)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "SourceNodeID" << YAML::Value << static_cast<u64>(conn.SourceNodeID);
            out << YAML::Key << "TargetNodeID" << YAML::Value << static_cast<u64>(conn.TargetNodeID);
            out << YAML::Key << "SourcePort" << YAML::Value << conn.SourcePort;
            out << YAML::Key << "TargetPort" << YAML::Value << conn.TargetPort;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;
        out << YAML::EndMap;

        std::ofstream fout(m_CurrentFilePath);
        if (!fout)
        {
            OLO_CORE_ERROR("DialogueEditorPanel - Failed to save: {}", m_CurrentFilePath.string());
            return;
        }
        fout << out.c_str();
        fout.close();

        m_IsDirty = false;
        OLO_CORE_INFO("DialogueEditorPanel - Saved: {}", m_CurrentFilePath.string());
    }

    void DialogueEditorPanel::SaveDialogueAs()
    {
        std::string filepath = FileDialogs::SaveFile(
            "Dialogue Tree (*.olodialogue)\0*.olodialogue\0"
            "All Files (*.*)\0*.*\0");
        if (filepath.empty())
            return;

        std::filesystem::path path(filepath);
        if (path.extension() != ".olodialogue")
            path += ".olodialogue";

        m_CurrentFilePath = path;
        SaveDialogue();
    }

    void DialogueEditorPanel::LoadDialogue(const std::filesystem::path& path)
    {
        m_Nodes.clear();
        m_Connections.clear();
        m_SelectedNodeID = 0;
        m_RootNodeID = 0;
        m_IsDirty = false;
        m_NextNodeID = 1000;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("DialogueEditorPanel - File not found: {}", path.string());
            m_CurrentFilePath.clear();
            m_CurrentAssetHandle = 0;
            return;
        }

        try
        {
            YAML::Node data = YAML::LoadFile(path.string());
            auto dialogueTree = data["DialogueTree"];
            if (!dialogueTree)
            {
                OLO_CORE_ERROR("DialogueEditorPanel - Invalid dialogue file: {}", path.string());
                m_CurrentFilePath.clear();
                m_CurrentAssetHandle = 0;
                return;
            }

            m_RootNodeID = dialogueTree["RootNodeID"].as<u64>();

            // Load nodes
            if (auto nodes = dialogueTree["Nodes"])
            {
                for (const auto& nodeYaml : nodes)
                {
                    DialogueNodeData node;
                    node.ID = nodeYaml["ID"].as<u64>();
                    node.Type = nodeYaml["Type"].as<std::string>();
                    node.Name = nodeYaml["Name"].as<std::string>();

                    if (auto pos = nodeYaml["EditorPosition"])
                    {
                        if (pos.IsSequence() && pos.size() >= 2)
                        {
                            node.EditorPosition.x = pos[0].as<f32>();
                            node.EditorPosition.y = pos[1].as<f32>();
                        }
                    }

                    if (auto props = nodeYaml["Properties"])
                    {
                        for (auto propIt = props.begin(); propIt != props.end(); ++propIt)
                        {
                            std::string propKey = propIt->first.as<std::string>();
                            auto propData = propIt->second;
                            std::string propType = propData["type"].as<std::string>("string");

                            if (propType == "bool")
                                node.Properties[propKey] = propData["value"].as<bool>();
                            else if (propType == "int")
                                node.Properties[propKey] = propData["value"].as<i32>();
                            else if (propType == "float")
                                node.Properties[propKey] = propData["value"].as<f32>();
                            else
                                node.Properties[propKey] = propData["value"].as<std::string>("");
                        }
                    }

                    // Track max ID for new node generation
                    if (static_cast<u64>(node.ID) >= m_NextNodeID)
                        m_NextNodeID = static_cast<u64>(node.ID) + 1;

                    m_Nodes.push_back(std::move(node));
                }
            }

            // Load connections
            if (auto connections = dialogueTree["Connections"])
            {
                for (const auto& connYaml : connections)
                {
                    DialogueConnection conn;
                    conn.SourceNodeID = connYaml["SourceNodeID"].as<u64>();
                    conn.TargetNodeID = connYaml["TargetNodeID"].as<u64>();
                    conn.SourcePort = connYaml["SourcePort"].as<std::string>("");
                    conn.TargetPort = connYaml["TargetPort"].as<std::string>("");
                    m_Connections.push_back(std::move(conn));
                }
            }

            m_CurrentFilePath = path;
            OLO_CORE_INFO("DialogueEditorPanel - Loaded: {}", path.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("DialogueEditorPanel - YAML parse error: {}", e.what());
            m_CurrentFilePath.clear();
            m_CurrentAssetHandle = 0;
        }
    }

    void DialogueEditorPanel::NewDialogue()
    {
        m_Nodes.clear();
        m_Connections.clear();
        m_SelectedNodeID = 0;
        m_IsDirty = false;
        m_NextNodeID = 1000;
        m_CurrentFilePath.clear();
        m_CurrentAssetHandle = 0;

        // Create a default start node
        m_RootNodeID = CreateNode("dialogue", { 100.0f, 200.0f });

        if (auto* startNode = FindNodeMutable(m_RootNodeID); startNode)
        {
            startNode->Name = "Start";
            startNode->Properties["speaker"] = std::string("NPC");
            startNode->Properties["text"] = std::string("Hello there!");
        }

        m_IsDirty = true;
    }

    // =========================================================================
    // Node Operations
    // =========================================================================

    UUID DialogueEditorPanel::CreateNode(const std::string& type, const glm::vec2& position)
    {
        auto oldSnapshot = CaptureSnapshot();

        UUID id = GenerateNodeID();

        DialogueNodeData node;
        node.ID = id;
        node.Type = type;
        node.EditorPosition = position;

        if (type == "dialogue")
        {
            node.Name = "Dialogue " + std::to_string(static_cast<u64>(id));
            node.Properties["speaker"] = std::string("");
            node.Properties["text"] = std::string("");
        }
        else if (type == "choice")
        {
            node.Name = "Choice " + std::to_string(static_cast<u64>(id));
        }
        else if (type == "condition")
        {
            node.Name = "Condition " + std::to_string(static_cast<u64>(id));
            node.Properties["conditionExpression"] = std::string("");
        }
        else if (type == "action")
        {
            node.Name = "Action " + std::to_string(static_cast<u64>(id));
            node.Properties["actionName"] = std::string("");
            node.Properties["actionArgs"] = std::string("");
        }
        else
        {
            // No additional handling required.
        }

        m_Nodes.push_back(std::move(node));
        m_SelectedNodeID = id;
        m_IsDirty = true;

        // If first node, make it root
        if (m_Nodes.size() == 1)
            m_RootNodeID = id;

        PushDialogueUndoCommand(oldSnapshot, "Create " + type + " Node");

        return id;
    }

    void DialogueEditorPanel::DeleteNode(UUID nodeID)
    {
        // Don't delete root node
        if (nodeID == m_RootNodeID)
            return;

        auto oldSnapshot = CaptureSnapshot();

        // Remove connections
        m_Connections.erase(
            std::remove_if(m_Connections.begin(), m_Connections.end(),
                           [nodeID](const DialogueConnection& c)
                           {
                               return c.SourceNodeID == nodeID || c.TargetNodeID == nodeID;
                           }),
            m_Connections.end());

        // Remove node
        m_Nodes.erase(
            std::remove_if(m_Nodes.begin(), m_Nodes.end(),
                           [nodeID](const DialogueNodeData& n)
                           { return n.ID == nodeID; }),
            m_Nodes.end());

        if (m_SelectedNodeID == nodeID)
            m_SelectedNodeID = 0;

        m_IsDirty = true;
        PushDialogueUndoCommand(oldSnapshot, "Delete Node");
    }

    void DialogueEditorPanel::DeleteConnection(size_t index)
    {
        if (index < m_Connections.size())
        {
            auto oldSnapshot = CaptureSnapshot();
            m_Connections.erase(m_Connections.begin() + static_cast<ptrdiff_t>(index));
            m_IsDirty = true;
            PushDialogueUndoCommand(oldSnapshot, "Delete Connection");
        }
    }

    void DialogueEditorPanel::DuplicateNode(UUID nodeID)
    {
        const auto* srcNode = FindNodeMutable(nodeID);
        if (!srcNode)
            return;

        // Capture state before the whole duplicate operation
        auto oldSnapshot = CaptureSnapshot();

        // Copy data before CreateNode, which may reallocate m_Nodes and invalidate srcNode
        std::string srcType = srcNode->Type;
        glm::vec2 srcPos = srcNode->EditorPosition + glm::vec2(30.0f, 30.0f);
        std::string srcName = srcNode->Name + " (copy)";
        auto srcProperties = srcNode->Properties;

        // Temporarily disable CommandHistory to avoid double-push from CreateNode
        auto* savedHistory = m_CommandHistory;
        m_CommandHistory = nullptr;
        UUID newID = CreateNode(srcType, srcPos);
        m_CommandHistory = savedHistory;

        if (auto* newNode = FindNodeMutable(newID); newNode)
        {
            newNode->Name = std::move(srcName);
            newNode->Properties = std::move(srcProperties);
        }

        PushDialogueUndoCommand(oldSnapshot, "Duplicate Node");
    }

    // =========================================================================
    // Coordinate Transforms
    // =========================================================================

    void DialogueEditorPanel::FrameAll()
    {
        if (m_Nodes.empty())
        {
            m_Canvas.ResetView();
            return;
        }

        glm::vec2 min(std::numeric_limits<f32>::max());
        glm::vec2 max(std::numeric_limits<f32>::lowest());
        for (const auto& node : m_Nodes)
        {
            min = glm::min(min, node.EditorPosition);
            // Node height varies with content; 120 graph units is the tallest a
            // dialogue node gets, which is what the old fit used.
            max = glm::max(max, node.EditorPosition + glm::vec2(s_NodeWidth, 120.0f));
        }
        m_Canvas.FitToBounds(min, max);
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    DialogueNodeData* DialogueEditorPanel::FindNodeMutable(UUID nodeID)
    {
        for (auto& n : m_Nodes)
        {
            if (n.ID == nodeID)
                return &n;
        }
        return nullptr;
    }

    UUID DialogueEditorPanel::GenerateNodeID()
    {
        return UUID(m_NextNodeID++);
    }

    std::string DialogueEditorPanel::ResolveSourcePort(const std::string& portName, UUID sourceNodeID)
    {
        // "+" is a virtual add-slot on choice nodes — assign a unique label
        if (portName == "+")
        {
            i32 maxIndex = 0;
            for (const auto& c : m_Connections)
            {
                if (c.SourceNodeID == sourceNodeID && c.SourcePort.starts_with("choice "))
                {
                    auto suffix = c.SourcePort.substr(7);
                    try
                    {
                        i32 const idx = std::stoi(suffix);
                        if (idx > maxIndex)
                            maxIndex = idx;
                    }
                    catch (...)
                    {
                    }
                }
            }
            return "choice " + std::to_string(maxIndex + 1);
        }

        // Deterministic ports (out, true, false) — replace existing connection
        if (portName == "out" || portName == "true" || portName == "false")
        {
            std::erase_if(m_Connections,
                          [&sourceNodeID, &portName](const DialogueConnection& c)
                          { return c.SourceNodeID == sourceNodeID && c.SourcePort == portName; });
        }

        return portName;
    }

} // namespace OloEngine
