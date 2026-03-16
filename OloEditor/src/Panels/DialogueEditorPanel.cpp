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
#include <fstream>
#include <cmath>

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
            ImGui::End();
            return;
        }

        DrawToolbar();

        // Layout: left = canvas, right = property panel + preview
        f32 const availWidth = ImGui::GetContentRegionAvail().x;
        f32 const canvasWidth = m_ShowPreview || m_SelectedNodeID != 0
                                    ? availWidth - s_PropertyPanelWidth
                                    : availWidth;

        // Left side: node canvas
        ImGui::BeginChild("##NodeCanvas", ImVec2(canvasWidth, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        DrawCanvas();
        ImGui::EndChild();

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

    void DialogueEditorPanel::DrawCanvas()
    {
        ImVec2 const canvasOrigin = ImGui::GetCursorScreenPos();
        ImVec2 const canvasSize = ImGui::GetContentRegionAvail();
        ImVec2 const canvasEnd = ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasOrigin, canvasEnd, IM_COL32(30, 30, 35, 255));

        // Clip
        drawList->PushClipRect(canvasOrigin, canvasEnd, true);

        DrawGrid(drawList, canvasOrigin, canvasSize);
        DrawConnections(drawList, canvasOrigin);
        DrawNodes(drawList, canvasOrigin);
        DrawConnectionInProgress(drawList, canvasOrigin);
        DrawMinimap(drawList, canvasOrigin, canvasSize);

        drawList->PopClipRect();

        // Invisible button for canvas interaction
        ImGui::SetCursorScreenPos(canvasOrigin);
        ImGui::InvisibleButton("##canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

        HandleCanvasInput(canvasOrigin, canvasSize);
        HandleNodeInteraction(canvasOrigin);
        HandleConnectionDrag(canvasOrigin);
        DrawContextMenu(canvasOrigin);
    }

    void DialogueEditorPanel::DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        f32 const gridStep = s_GridSize * m_Zoom;
        ImU32 const gridColorMajor = IM_COL32(60, 60, 65, 200);
        ImU32 const gridColorMinor = IM_COL32(45, 45, 50, 200);

        f32 const startX = std::fmod(m_ScrollOffset.x * m_Zoom, gridStep);
        f32 const startY = std::fmod(m_ScrollOffset.y * m_Zoom, gridStep);

        i32 lineIndex = 0;
        for (f32 x = startX; x < canvasSize.x; x += gridStep, ++lineIndex)
        {
            ImU32 const color = (lineIndex % 4 == 0) ? gridColorMajor : gridColorMinor;
            drawList->AddLine(
                ImVec2(canvasOrigin.x + x, canvasOrigin.y),
                ImVec2(canvasOrigin.x + x, canvasOrigin.y + canvasSize.y),
                color);
        }

        lineIndex = 0;
        for (f32 y = startY; y < canvasSize.y; y += gridStep, ++lineIndex)
        {
            ImU32 const color = (lineIndex % 4 == 0) ? gridColorMajor : gridColorMinor;
            drawList->AddLine(
                ImVec2(canvasOrigin.x, canvasOrigin.y + y),
                ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + y),
                color);
        }
    }

    void DialogueEditorPanel::DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        for (auto& node : m_Nodes)
        {
            DrawNode(drawList, canvasOrigin, node);
        }
    }

    void DialogueEditorPanel::DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, DialogueNodeData& node)
    {
        ImVec2 const nodePos = WorldToScreen(node.EditorPosition, canvasOrigin);
        ImVec2 const nodeSize = GetNodeSize(node);

        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        ImVec2 const headerEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + s_NodeHeaderHeight * m_Zoom);

        bool const isSelected = (node.ID == m_SelectedNodeID);
        bool const isRoot = (node.ID == m_RootNodeID);

        // Node body
        f32 const rounding = 6.0f * m_Zoom;
        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.Type), rounding);

        // Header
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.Type), rounding, ImDrawFlags_RoundCornersTop);

        // Selection outline
        if (isSelected)
        {
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 220), rounding, 0, 2.5f * m_Zoom);
        }

        // Root node indicator
        if (isRoot)
        {
            drawList->AddRect(
                ImVec2(nodePos.x - 2.0f, nodePos.y - 2.0f),
                ImVec2(nodeEnd.x + 2.0f, nodeEnd.y + 2.0f),
                IM_COL32(50, 200, 50, 180), rounding + 2.0f, 0, 1.5f * m_Zoom);
        }

        // Header text
        f32 const fontSize = 13.0f * m_Zoom;
        std::string headerText = node.Name.empty() ? node.Type : node.Name;
        ImVec2 const textPos = ImVec2(nodePos.x + s_NodePadding * m_Zoom, nodePos.y + 4.0f * m_Zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 240), headerText.c_str());

        // Type badge (small text)
        if (!node.Name.empty())
        {
            std::string typeLabel = "[" + node.Type + "]";
            ImVec2 const badgeSize = ImGui::CalcTextSize(typeLabel.c_str());
            ImVec2 const badgePos = ImVec2(
                nodeEnd.x - (badgeSize.x + s_NodePadding) * m_Zoom,
                nodePos.y + 6.0f * m_Zoom);
            drawList->AddText(nullptr, 10.0f * m_Zoom, badgePos, IM_COL32(200, 200, 200, 150), typeLabel.c_str());
        }

        // Content preview
        f32 contentY = nodePos.y + s_NodeHeaderHeight * m_Zoom + 4.0f * m_Zoom;

        if (node.Type == "dialogue")
        {
            // Show speaker + text excerpt
            if (auto it = node.Properties.find("speaker"); it != node.Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                {
                    drawList->AddText(nullptr, 11.0f * m_Zoom,
                                      ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                                      IM_COL32(255, 200, 100, 220), str->c_str());
                    contentY += 14.0f * m_Zoom;
                }
            }
            if (auto it = node.Properties.find("text"); it != node.Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string preview = *str;
                    if (preview.size() > 35)
                        preview = preview.substr(0, 32) + "...";
                    drawList->AddText(nullptr, 10.0f * m_Zoom,
                                      ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                                      IM_COL32(200, 200, 200, 200), preview.c_str());
                }
            }
        }
        else if (node.Type == "condition")
        {
            if (auto it = node.Properties.find("conditionExpression"); it != node.Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string label = "if: " + *str;
                    drawList->AddText(nullptr, 11.0f * m_Zoom,
                                      ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                                      IM_COL32(150, 200, 255, 220), label.c_str());
                }
            }
        }
        else if (node.Type == "action")
        {
            if (auto it = node.Properties.find("actionName"); it != node.Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                {
                    std::string label = "do: " + *str;
                    drawList->AddText(nullptr, 11.0f * m_Zoom,
                                      ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                                      IM_COL32(255, 180, 150, 220), label.c_str());
                }
            }
        }

        // Draw ports
        auto ports = GetNodePorts(node, nodePos);
        for (const auto& port : ports)
        {
            ImU32 const portColor = port.IsOutput ? IM_COL32(100, 200, 100, 220) : IM_COL32(100, 150, 255, 220);
            ImU32 const portFill = IM_COL32(40, 40, 45, 255);
            f32 const radius = s_NodePortRadius * m_Zoom;

            drawList->AddCircleFilled(port.Position, radius, portFill);
            drawList->AddCircle(port.Position, radius, portColor, 12, 2.0f * m_Zoom);

            // Port label
            f32 const labelOffset = (radius + 4.0f * m_Zoom);
            ImVec2 labelPos;
            if (port.IsOutput)
            {
                ImVec2 const textSize = ImGui::CalcTextSize(port.Name.c_str());
                labelPos = ImVec2(port.Position.x - labelOffset - textSize.x * m_Zoom, port.Position.y - 5.0f * m_Zoom);
            }
            else
            {
                labelPos = ImVec2(port.Position.x + labelOffset, port.Position.y - 5.0f * m_Zoom);
            }
            drawList->AddText(nullptr, 10.0f * m_Zoom, labelPos, IM_COL32(180, 180, 180, 200), port.Name.c_str());
        }
    }

    ImVec2 DialogueEditorPanel::GetNodeSize(const DialogueNodeData& node) const
    {
        f32 width = s_NodeWidth * m_Zoom;
        f32 height = s_NodeHeaderHeight * m_Zoom;

        // Content area
        if (node.Type == "dialogue")
            height += 36.0f * m_Zoom; // speaker + text preview
        else if (node.Type == "condition" || node.Type == "action")
            height += 20.0f * m_Zoom;
        else
            height += 10.0f * m_Zoom;

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

        height += static_cast<f32>(portCount) * s_NodePortSpacing * m_Zoom;
        height += s_NodePadding * m_Zoom * 2.0f;

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
        std::vector<PortInfo> ports;
        ImVec2 const nodeSize = GetNodeSize(node);

        f32 const portAreaY = nodeScreenPos.y + s_NodeHeaderHeight * m_Zoom + nodeSize.y * 0.4f;

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
            falsePort.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + s_NodePortSpacing * m_Zoom);
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
                p.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + static_cast<f32>(i) * s_NodePortSpacing * m_Zoom);
                p.NodeID = node.ID;
                p.Name = choiceLabels[i];
                p.IsOutput = true;
                ports.push_back(p);
            }

            // "+" port for adding new choice connections
            PortInfo addPort;
            addPort.Position = ImVec2(nodeScreenPos.x + nodeSize.x, portAreaY + static_cast<f32>(portIndex) * s_NodePortSpacing * m_Zoom);
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

    void DialogueEditorPanel::DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin)
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
            ImVec2 const srcNodePos = WorldToScreen(srcNode->EditorPosition, canvasOrigin);
            ImVec2 const dstNodePos = WorldToScreen(dstNode->EditorPosition, canvasOrigin);

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

            // Draw bezier curve
            f32 const dist = std::abs(endPos.x - startPos.x) * 0.5f;
            ImVec2 const cp1 = ImVec2(startPos.x + dist, startPos.y);
            ImVec2 const cp2 = ImVec2(endPos.x - dist, endPos.y);

            ImU32 const lineColor = IM_COL32(180, 180, 200, 200);
            drawList->AddBezierCubic(startPos, cp1, cp2, endPos, lineColor, 2.0f * m_Zoom);

            // Arrow head at end
            glm::vec2 rawDir(endPos.x - cp2.x, endPos.y - cp2.y);
            f32 const dirLen = glm::length(rawDir);
            if (dirLen < 1e-6f)
                continue;
            glm::vec2 const dir = rawDir / dirLen;
            f32 const arrowSize = 8.0f * m_Zoom;
            ImVec2 const arrow1 = ImVec2(
                endPos.x - dir.x * arrowSize + dir.y * arrowSize * 0.4f,
                endPos.y - dir.y * arrowSize - dir.x * arrowSize * 0.4f);
            ImVec2 const arrow2 = ImVec2(
                endPos.x - dir.x * arrowSize - dir.y * arrowSize * 0.4f,
                endPos.y - dir.y * arrowSize + dir.x * arrowSize * 0.4f);
            drawList->AddTriangleFilled(endPos, arrow1, arrow2, lineColor);
        }
    }

    void DialogueEditorPanel::DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin)
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

        ImVec2 const srcNodePos = WorldToScreen(srcNode->EditorPosition, canvasOrigin);
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

        ImVec2 const endPos = m_ConnectionEndPos;
        f32 const dist = std::abs(endPos.x - startPos.x) * 0.5f;
        ImVec2 const cp1 = ImVec2(startPos.x + dist, startPos.y);
        ImVec2 const cp2 = ImVec2(endPos.x - dist, endPos.y);

        drawList->AddBezierCubic(startPos, cp1, cp2, endPos, IM_COL32(255, 255, 100, 180), 2.0f * m_Zoom);
    }

    void DialogueEditorPanel::DrawMinimap(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        if (m_Nodes.empty())
            return;

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

    void DialogueEditorPanel::HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        bool const isHovered = ImGui::IsItemHovered();

        // Zoom with scroll wheel
        if (isHovered)
        {
            f32 const scroll = ImGui::GetIO().MouseWheel;
            if (scroll != 0.0f)
            {
                ImVec2 const mousePos = ImGui::GetIO().MousePos;
                glm::vec2 const worldBefore = ScreenToWorld(mousePos, canvasOrigin);

                m_Zoom = std::clamp(m_Zoom + scroll * 0.1f * m_Zoom, s_MinZoom, s_MaxZoom);

                glm::vec2 const worldAfter = ScreenToWorld(mousePos, canvasOrigin);
                m_ScrollOffset += (worldAfter - worldBefore);
            }
        }

        // Pan with middle mouse button or Alt+left click
        bool const wantPan = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                             (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt);

        if (isHovered && wantPan)
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

        // Right-click context menu
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !m_IsCreatingConnection)
        {
            m_ContextMenuPos = ImGui::GetIO().MousePos;
            m_ShowContextMenu = true;
        }

        // Click on empty space to deselect
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt && !m_IsDraggingNode)
        {
            // Check if clicked on a node — if not, deselect
            bool clickedOnNode = false;
            ImVec2 const mousePos = ImGui::GetIO().MousePos;
            for (const auto& node : m_Nodes)
            {
                ImVec2 const nodePos = WorldToScreen(node.EditorPosition, canvasOrigin);
                ImVec2 const nodeSize = GetNodeSize(node);
                if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                    mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y)
                {
                    clickedOnNode = true;
                    break;
                }
            }

            if (!clickedOnNode)
            {
                m_SelectedNodeID = 0;
            }
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

    void DialogueEditorPanel::HandleNodeInteraction(const ImVec2& canvasOrigin)
    {
        ImVec2 const mousePos = ImGui::GetIO().MousePos;

        // Check node click and drag
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
        {
            // Iterate in reverse so topmost nodes are selected first
            for (auto it = m_Nodes.rbegin(); it != m_Nodes.rend(); ++it)
            {
                ImVec2 const nodePos = WorldToScreen(it->EditorPosition, canvasOrigin);
                ImVec2 const nodeSize = GetNodeSize(*it);

                if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                    mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y)
                {
                    // Check if clicking on a port first
                    auto ports = GetNodePorts(*it, nodePos);
                    bool clickedPort = false;
                    for (const auto& port : ports)
                    {
                        f32 const dist = std::hypot(mousePos.x - port.Position.x, mousePos.y - port.Position.y);
                        if (dist <= s_NodePortRadius * m_Zoom * 2.0f)
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
                        m_SelectedNodeID = it->ID;
                        m_IsDraggingNode = true;
                        m_DragStartOffset = glm::vec2(
                            mousePos.x - nodePos.x,
                            mousePos.y - nodePos.y);
                        m_DragStartSnapshot = CaptureSnapshot();
                    }
                    break;
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
                    node.EditorPosition = ScreenToWorld(targetScreen, canvasOrigin);
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

    void DialogueEditorPanel::HandleConnectionDrag(const ImVec2& canvasOrigin)
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

            for (const auto& node : m_Nodes)
            {
                if (node.ID == m_ConnectionStartNodeID)
                    continue; // No self-connections

                ImVec2 const nodePos = WorldToScreen(node.EditorPosition, canvasOrigin);
                auto ports = GetNodePorts(node, nodePos);

                for (const auto& port : ports)
                {
                    f32 const dist = std::hypot(mousePos.x - port.Position.x, mousePos.y - port.Position.y);
                    if (dist <= s_NodePortRadius * m_Zoom * 2.5f)
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
                            bool duplicate = std::ranges::any_of(m_Connections, [&](const auto& c)
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
                            bool duplicate = std::ranges::any_of(m_Connections, [&](const auto& c)
                                                                 { return c.SourceNodeID == conn.SourceNodeID && c.SourcePort == conn.SourcePort && c.TargetNodeID == conn.TargetNodeID && c.TargetPort == conn.TargetPort; });
                            if (!duplicate)
                            {
                                m_Connections.push_back(conn);
                                m_IsDirty = true;
                            }
                            connected = true;
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

        // Cancel with right click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_IsCreatingConnection = false;
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
                glm::vec2 const center = ScreenToWorld(
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f,
                           ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f),
                    ImGui::GetWindowPos());

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
                    m_ScrollOffset = { 0.0f, 0.0f };
                    m_Zoom = 1.0f;
                }
                if (ImGui::MenuItem("Zoom to Fit") && !m_Nodes.empty())
                {
                    f32 minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                    for (const auto& n : m_Nodes)
                    {
                        minX = std::min(minX, n.EditorPosition.x);
                        minY = std::min(minY, n.EditorPosition.y);
                        maxX = std::max(maxX, n.EditorPosition.x + s_NodeWidth);
                        maxY = std::max(maxY, n.EditorPosition.y + 120.0f);
                    }
                    f32 const w = maxX - minX + 100.0f;
                    f32 const h = maxY - minY + 100.0f;
                    ImVec2 const canvasSize = ImGui::GetContentRegionAvail();
                    m_Zoom = std::clamp(std::min(canvasSize.x / w, canvasSize.y / h), s_MinZoom, s_MaxZoom);
                    m_ScrollOffset = glm::vec2(-(minX - 50.0f), -(minY - 50.0f));
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
        bool isRoot = (node.ID == m_RootNodeID);
        if (ImGui::Checkbox("Root Node", &isRoot))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                std::string inputLabel = "##choice_" + std::to_string(static_cast<u64>(conn.TargetNodeID));
                if (ImGui::InputText(inputLabel.c_str(), labelBuf, sizeof(labelBuf)))
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
                std::string delLabel = "X##conn_" + std::to_string(connIdx);
                if (ImGui::SmallButton(delLabel.c_str()))
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
                if (auto* str = std::get_if<std::string>(&it->second))
                    m_PreviewCurrentSpeaker = *str;
            }
            if (auto it = node->Properties.find("text"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
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
                if (auto* str = std::get_if<std::string>(&it->second))
                    actionName = *str;
            }

            m_PreviewCurrentText = "[Action: " + actionName + "]";
            m_PreviewCurrentSpeaker.clear();
            m_PreviewChoices.clear();

            // Auto-advance past action nodes after display
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

    void DialogueEditorPanel::DrawContextMenu(const ImVec2& canvasOrigin)
    {
        if (m_ShowContextMenu)
        {
            ImGui::OpenPopup("##CanvasContextMenu");
            m_ShowContextMenu = false;
        }

        if (ImGui::BeginPopup("##CanvasContextMenu"))
        {
            glm::vec2 const worldPos = ScreenToWorld(m_ContextMenuPos, canvasOrigin);

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

        auto* startNode = FindNodeMutable(m_RootNodeID);
        if (startNode)
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

        auto* newNode = FindNodeMutable(newID);
        if (newNode)
        {
            newNode->Name = std::move(srcName);
            newNode->Properties = std::move(srcProperties);
        }

        PushDialogueUndoCommand(oldSnapshot, "Duplicate Node");
    }

    // =========================================================================
    // Coordinate Transforms
    // =========================================================================

    ImVec2 DialogueEditorPanel::WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const
    {
        return ImVec2(
            canvasOrigin.x + (worldPos.x + m_ScrollOffset.x) * m_Zoom,
            canvasOrigin.y + (worldPos.y + m_ScrollOffset.y) * m_Zoom);
    }

    glm::vec2 DialogueEditorPanel::ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const
    {
        return glm::vec2(
            (screenPos.x - canvasOrigin.x) / m_Zoom - m_ScrollOffset.x,
            (screenPos.y - canvasOrigin.y) / m_Zoom - m_ScrollOffset.y);
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
                          [&](const DialogueConnection& c)
                          { return c.SourceNodeID == sourceNodeID && c.SourcePort == portName; });
        }

        return portName;
    }

} // namespace OloEngine
