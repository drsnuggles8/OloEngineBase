#include "OloEnginePCH.h"
#include "ShaderGraphEditorPanel.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <queue>

namespace OloEngine
{
    // ─────────────────────────────────────────────────────────────
    //  Helpers
    // ─────────────────────────────────────────────────────────────

    static bool IsParameterNode(const std::string& typeName)
    {
        return typeName == ShaderGraphNodeTypes::FloatParameter || typeName == ShaderGraphNodeTypes::Vec3Parameter || typeName == ShaderGraphNodeTypes::Vec4Parameter || typeName == ShaderGraphNodeTypes::ColorParameter || typeName == ShaderGraphNodeTypes::Texture2DParameter || typeName == ShaderGraphNodeTypes::ComputeBufferInput || typeName == ShaderGraphNodeTypes::ComputeBufferStore;
    }

    // =========================================================================
    // Main render entry
    // =========================================================================

    void ShaderGraphEditorPanel::OnImGuiRender()
    {
        if (!m_IsOpen)
            return;

        ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
        std::string windowTitle = "Shader Graph Editor";
        if (!m_CurrentFilePath.empty())
        {
            windowTitle += " - " + m_CurrentFilePath.filename().string();
            if (m_IsDirty)
                windowTitle += " *";
        }
        windowTitle += "###ShaderGraphEditor";

        if (!ImGui::Begin(windowTitle.c_str(), &m_IsOpen, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Handle deferred loading — show "Loading..." for one frame before blocking I/O
        if (!m_PendingLoadPath.empty())
        {
            if (++m_PendingLoadFrameDelay >= 2)
            {
                PerformPendingLoad();
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Loading shader graph...");
                ImGui::End();
                return;
            }
        }

        DrawToolbar();

        // Handle keyboard shortcuts (must be before child windows to capture focus)
        HandleKeyboardShortcuts();

        f32 const availWidth = ImGui::GetContentRegionAvail().x;
        f32 const canvasWidth = (m_SelectedNodeID != 0 || m_LastCompileResult.Success)
                                    ? availWidth - s_PropertyPanelWidth
                                    : availWidth;

        // Left: node canvas
        ImGui::BeginChild("##SGCanvas", ImVec2(canvasWidth, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        DrawCanvas();
        ImGui::EndChild();

        // Right: property panel + preview
        if (m_SelectedNodeID != 0 || m_LastCompileResult.Success)
        {
            ImGui::SameLine();
            ImGui::BeginChild("##SGPropertiesPanel", ImVec2(s_PropertyPanelWidth, 0), ImGuiChildFlags_Borders);

            if (m_SelectedNodeID != 0 && m_GraphAsset)
            {
                auto* node = m_GraphAsset->GetGraph().FindNode(m_SelectedNodeID);
                if (node)
                    DrawNodeProperties(*node);
            }

            DrawPreviewPanel();
            ImGui::EndChild();
        }

        ImGui::End();
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void ShaderGraphEditorPanel::DrawToolbar()
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New"))
                    NewShaderGraph();
                if (ImGui::MenuItem("Open...", "Ctrl+O"))
                {
                    std::string filepath = FileDialogs::OpenFile("OloEngine Shader Graph (*.olosg)\0*.olosg\0All Files\0*.*\0");
                    if (!filepath.empty())
                        OpenShaderGraph(std::filesystem::path(filepath));
                }
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                    SaveShaderGraph();
                if (ImGui::MenuItem("Save As..."))
                    SaveShaderGraphAs();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                std::string undoLabel = m_CommandHistory.CanUndo()
                                            ? "Undo " + m_CommandHistory.GetUndoDescription()
                                            : "Undo";
                if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, m_CommandHistory.CanUndo()))
                    Undo();

                std::string redoLabel = m_CommandHistory.CanRedo()
                                            ? "Redo " + m_CommandHistory.GetRedoDescription()
                                            : "Redo";
                if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, m_CommandHistory.CanRedo()))
                    Redo();

                ImGui::Separator();

                if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_SelectedNodeID != 0))
                    CopySelectedNode();
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, m_HasCopiedNode))
                    PasteNodes(glm::vec2(200.0f, 200.0f));
                if (ImGui::MenuItem("Delete", "Del", false, m_SelectedNodeID != 0))
                {
                    DeleteNode(m_SelectedNodeID);
                    m_SelectedNodeID = 0;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Graph"))
            {
                if (ImGui::MenuItem("Compile"))
                {
                    if (m_GraphAsset)
                    {
                        m_GraphAsset->MarkDirty();
                        m_LastCompileResult = m_GraphAsset->Compile();
                    }
                }
                if (ImGui::MenuItem("Auto Layout"))
                    AutoLayoutNodes();
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }
    }

    // =========================================================================
    // Canvas
    // =========================================================================

    void ShaderGraphEditorPanel::DrawCanvas()
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

        drawList->PopClipRect();

        ImGui::SetCursorScreenPos(canvasOrigin);
        ImGui::InvisibleButton("##sgcanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

        HandleCanvasInput(canvasOrigin, canvasSize);
        HandleNodeInteraction(canvasOrigin);
        HandleConnectionDrag(canvasOrigin);
        DrawContextMenu(canvasOrigin);
    }

    void ShaderGraphEditorPanel::DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize)
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

    void ShaderGraphEditorPanel::DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        for (auto& node : m_GraphAsset->GetGraph().GetNodes())
            DrawNode(drawList, canvasOrigin, *node);
    }

    void ShaderGraphEditorPanel::DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, ShaderGraphNode& node)
    {
        ImVec2 const nodePos = WorldToScreen(node.EditorPosition, canvasOrigin);
        ImVec2 const nodeSize = GetNodeSize(node);
        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        bool const isSelected = (m_SelectedNodeID == node.ID);

        // Node body
        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.Category), 4.0f);

        // Header
        ImVec2 const headerEnd = ImVec2(nodeEnd.x, nodePos.y + s_HeaderHeight * m_Zoom);
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.Category), 4.0f, ImDrawFlags_RoundCornersTop);

        // Selection border
        if (isSelected)
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 255), 4.0f, 0, 2.0f);

        // Title text
        f32 const fontSize = 13.0f * m_Zoom;
        ImVec2 const textPos = ImVec2(nodePos.x + 8.0f * m_Zoom, nodePos.y + 5.0f * m_Zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 255), node.TypeName.c_str());

        // Per-node preview thumbnail (color swatch for color/vector parameter nodes)
        if (!node.ParameterName.empty() || node.TypeName == ShaderGraphNodeTypes::CustomFunction)
        {
            constexpr f32 swatchSize = 16.0f;
            f32 const swatchSizeScaled = swatchSize * m_Zoom;
            ImVec2 const swatchPos = ImVec2(
                nodeEnd.x - swatchSizeScaled - 4.0f * m_Zoom,
                nodePos.y + (s_HeaderHeight - swatchSize) * 0.5f * m_Zoom);
            ImVec2 const swatchEnd = ImVec2(swatchPos.x + swatchSizeScaled, swatchPos.y + swatchSizeScaled);

            // Determine preview color from first output or first input default
            ImU32 previewColor = IM_COL32(128, 128, 128, 255);

            for (const auto& pin : node.Inputs)
            {
                if (std::holds_alternative<glm::vec3>(pin.DefaultValue))
                {
                    auto c = std::get<glm::vec3>(pin.DefaultValue);
                    previewColor = IM_COL32(
                        static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
                        static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                        static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f), 255);
                    break;
                }
                if (std::holds_alternative<glm::vec4>(pin.DefaultValue))
                {
                    auto c = std::get<glm::vec4>(pin.DefaultValue);
                    previewColor = IM_COL32(
                        static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
                        static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                        static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f),
                        static_cast<int>(std::clamp(c.a, 0.0f, 1.0f) * 255.0f));
                    break;
                }
                if (std::holds_alternative<f32>(pin.DefaultValue))
                {
                    int v = static_cast<int>(std::clamp(std::get<f32>(pin.DefaultValue), 0.0f, 1.0f) * 255.0f);
                    previewColor = IM_COL32(v, v, v, 255);
                    break;
                }
            }

            drawList->AddRectFilled(swatchPos, swatchEnd, previewColor, 2.0f);
            drawList->AddRect(swatchPos, swatchEnd, IM_COL32(200, 200, 200, 180), 2.0f);
        }

        // Draw pins
        auto pins = GetNodePins(node, nodePos);
        for (const auto& pin : pins)
        {
            ImU32 color = GetPinColor(pin.Type);
            drawList->AddCircleFilled(pin.Position, s_PinRadius * m_Zoom, color);
            drawList->AddCircle(pin.Position, s_PinRadius * m_Zoom, IM_COL32(200, 200, 200, 255));

            // Pin label
            f32 const labelFontSize = 11.0f * m_Zoom;
            if (pin.IsOutput)
            {
                ImVec2 textSize = ImGui::CalcTextSize(pin.Name.c_str());
                textSize.x *= (labelFontSize / ImGui::GetFontSize());
                drawList->AddText(nullptr, labelFontSize,
                                  ImVec2(pin.Position.x - s_PinRadius * m_Zoom - 4.0f * m_Zoom - textSize.x, pin.Position.y - labelFontSize * 0.5f),
                                  IM_COL32(200, 200, 200, 255), pin.Name.c_str());
            }
            else
            {
                drawList->AddText(nullptr, labelFontSize,
                                  ImVec2(pin.Position.x + s_PinRadius * m_Zoom + 4.0f * m_Zoom, pin.Position.y - labelFontSize * 0.5f),
                                  IM_COL32(200, 200, 200, 255), pin.Name.c_str());
            }
        }
    }

    ImVec2 ShaderGraphEditorPanel::GetNodeSize(const ShaderGraphNode& node) const
    {
        f32 const pinCount = static_cast<f32>(std::max(node.Inputs.size(), node.Outputs.size()));
        f32 const bodyHeight = s_HeaderHeight + (pinCount + 1) * s_PinSpacing;
        return ImVec2(s_NodeWidth * m_Zoom, bodyHeight * m_Zoom);
    }

    std::vector<ShaderGraphEditorPanel::PinInfo> ShaderGraphEditorPanel::GetNodePins(const ShaderGraphNode& node, const ImVec2& nodeScreenPos) const
    {
        std::vector<PinInfo> result;
        ImVec2 const nodeSize = GetNodeSize(node);

        // Input pins on left
        for (size_t i = 0; i < node.Inputs.size(); ++i)
        {
            PinInfo pin;
            pin.Position = ImVec2(nodeScreenPos.x, nodeScreenPos.y + (s_HeaderHeight + (static_cast<f32>(i) + 1) * s_PinSpacing) * m_Zoom);
            pin.PinID = node.Inputs[i].ID;
            pin.NodeID = node.ID;
            pin.Name = node.Inputs[i].Name;
            pin.Type = node.Inputs[i].Type;
            pin.IsOutput = false;
            result.push_back(pin);
        }

        // Output pins on right
        for (size_t i = 0; i < node.Outputs.size(); ++i)
        {
            PinInfo pin;
            pin.Position = ImVec2(nodeScreenPos.x + nodeSize.x, nodeScreenPos.y + (s_HeaderHeight + (static_cast<f32>(i) + 1) * s_PinSpacing) * m_Zoom);
            pin.PinID = node.Outputs[i].ID;
            pin.NodeID = node.ID;
            pin.Name = node.Outputs[i].Name;
            pin.Type = node.Outputs[i].Type;
            pin.IsOutput = true;
            result.push_back(pin);
        }

        return result;
    }

    // =========================================================================
    // Connection rendering
    // =========================================================================

    void ShaderGraphEditorPanel::DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        for (const auto& link : m_GraphAsset->GetGraph().GetLinks())
        {
            // Find source and target pin positions
            const auto* outNode = m_GraphAsset->GetGraph().FindNodeByPinID(link.OutputPinID);
            const auto* inNode = m_GraphAsset->GetGraph().FindNodeByPinID(link.InputPinID);
            if (!outNode || !inNode)
                continue;

            ImVec2 srcPos, dstPos;
            ShaderGraphPinType srcType = ShaderGraphPinType::Float;

            auto srcPins = GetNodePins(*outNode, WorldToScreen(outNode->EditorPosition, canvasOrigin));
            for (const auto& pin : srcPins)
            {
                if (pin.PinID == link.OutputPinID)
                {
                    srcPos = pin.Position;
                    srcType = pin.Type;
                    break;
                }
            }

            auto dstPins = GetNodePins(*inNode, WorldToScreen(inNode->EditorPosition, canvasOrigin));
            for (const auto& pin : dstPins)
            {
                if (pin.PinID == link.InputPinID)
                {
                    dstPos = pin.Position;
                    break;
                }
            }

            // Bezier curve
            f32 const curvature = 50.0f * m_Zoom;
            ImVec2 const cp1 = ImVec2(srcPos.x + curvature, srcPos.y);
            ImVec2 const cp2 = ImVec2(dstPos.x - curvature, dstPos.y);

            ImU32 color = GetPinColor(srcType);
            drawList->AddBezierCubic(srcPos, cp1, cp2, dstPos, color, 2.0f * m_Zoom);
        }
    }

    void ShaderGraphEditorPanel::DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_IsDraggingConnection)
            return;

        ImVec2 startPos = {};
        if (m_GraphAsset)
        {
            const auto* node = m_GraphAsset->GetGraph().FindNodeByPinID(m_DragStartPinID);
            if (node)
            {
                auto pins = GetNodePins(*node, WorldToScreen(node->EditorPosition, canvasOrigin));
                for (const auto& pin : pins)
                {
                    if (pin.PinID == m_DragStartPinID)
                    {
                        startPos = pin.Position;
                        break;
                    }
                }
            }
        }

        ImVec2 endPos = ImGui::GetMousePos();
        f32 const curvature = 50.0f * m_Zoom;

        if (m_DragStartIsOutput)
        {
            ImVec2 const cp1 = ImVec2(startPos.x + curvature, startPos.y);
            ImVec2 const cp2 = ImVec2(endPos.x - curvature, endPos.y);
            drawList->AddBezierCubic(startPos, cp1, cp2, endPos, IM_COL32(200, 200, 200, 180), 2.0f * m_Zoom);
        }
        else
        {
            ImVec2 const cp1 = ImVec2(startPos.x - curvature, startPos.y);
            ImVec2 const cp2 = ImVec2(endPos.x + curvature, endPos.y);
            drawList->AddBezierCubic(startPos, cp1, cp2, endPos, IM_COL32(200, 200, 200, 180), 2.0f * m_Zoom);
        }
    }

    // =========================================================================
    // Input handling
    // =========================================================================

    void ShaderGraphEditorPanel::HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize)
    {
        bool const isHovered = ImGui::IsItemHovered();

        // Pan with middle mouse
        if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            ImVec2 const delta = ImGui::GetIO().MouseDelta;
            m_ScrollOffset.x += delta.x / m_Zoom;
            m_ScrollOffset.y += delta.y / m_Zoom;
        }

        // Zoom with scroll
        if (isHovered)
        {
            f32 const scroll = ImGui::GetIO().MouseWheel;
            if (scroll != 0.0f)
            {
                f32 const zoomDelta = scroll * 0.1f;
                m_Zoom = std::clamp(m_Zoom + zoomDelta, 0.25f, 3.0f);
            }
        }

        // Deselect on click in empty space
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDraggingConnection && !m_IsDraggingNode)
        {
            m_SelectedNodeID = 0;
        }

        // Context menu on right click
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_ShowContextMenu = true;
            m_ContextMenuPos = ImGui::GetMousePos();
        }
    }

    void ShaderGraphEditorPanel::HandleNodeInteraction(const ImVec2& canvasOrigin)
    {
        if (!m_GraphAsset)
            return;

        ImVec2 const mousePos = ImGui::GetMousePos();

        // Check pin hover for connection start
        for (auto& node : m_GraphAsset->GetGraph().GetNodes())
        {
            ImVec2 const nodeScreenPos = WorldToScreen(node->EditorPosition, canvasOrigin);
            auto pins = GetNodePins(*node, nodeScreenPos);

            for (const auto& pin : pins)
            {
                f32 const dist = std::sqrt(
                    (mousePos.x - pin.Position.x) * (mousePos.x - pin.Position.x) +
                    (mousePos.y - pin.Position.y) * (mousePos.y - pin.Position.y));

                if (dist <= s_PinRadius * m_Zoom * 2.0f)
                {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        m_IsDraggingConnection = true;
                        m_DragStartPinID = pin.PinID;
                        m_DragStartIsOutput = pin.IsOutput;
                        return;
                    }
                }
            }

            // Node selection and dragging
            ImVec2 const nodeSize = GetNodeSize(*node);
            bool const isInNode = mousePos.x >= nodeScreenPos.x && mousePos.x <= nodeScreenPos.x + nodeSize.x &&
                                  mousePos.y >= nodeScreenPos.y && mousePos.y <= nodeScreenPos.y + nodeSize.y;

            if (isInNode && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDraggingConnection)
            {
                m_SelectedNodeID = node->ID;
                m_IsDraggingNode = true;
                m_DragNodeID = node->ID;
                m_DragNodeStartPos = node->EditorPosition;
                m_DragMouseStartPos = mousePos;
                return;
            }
        }

        // Node dragging
        if (m_IsDraggingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            auto* node = m_GraphAsset->GetGraph().FindNode(m_DragNodeID);
            if (node)
            {
                ImVec2 const delta = ImVec2(mousePos.x - m_DragMouseStartPos.x, mousePos.y - m_DragMouseStartPos.y);
                node->EditorPosition.x = m_DragNodeStartPos.x + delta.x / m_Zoom;
                node->EditorPosition.y = m_DragNodeStartPos.y + delta.y / m_Zoom;
                m_IsDirty = true;
            }
        }

        if (m_IsDraggingNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // Record position change as undoable command
            if (m_GraphAsset)
            {
                auto* node = m_GraphAsset->GetGraph().FindNode(m_DragNodeID);
                if (node && node->EditorPosition != m_DragNodeStartPos)
                {
                    // Node was already moved during drag — record the move for undo
                    // We temporarily set position back then execute the command
                    glm::vec2 finalPos = node->EditorPosition;
                    node->EditorPosition = m_DragNodeStartPos;
                    auto cmd = CreateScope<MoveNodeCommand>(m_DragNodeID, m_DragNodeStartPos, finalPos);
                    m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());
                    m_GraphAsset->MarkDirty();
                }
            }
            m_IsDraggingNode = false;
            m_DragNodeID = 0;
        }
    }

    void ShaderGraphEditorPanel::HandleConnectionDrag(const ImVec2& canvasOrigin)
    {
        if (!m_IsDraggingConnection || !m_GraphAsset)
            return;

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            ImVec2 const mousePos = ImGui::GetMousePos();

            // Find target pin
            for (auto& node : m_GraphAsset->GetGraph().GetNodes())
            {
                ImVec2 const nodeScreenPos = WorldToScreen(node->EditorPosition, canvasOrigin);
                auto pins = GetNodePins(*node, nodeScreenPos);

                for (const auto& pin : pins)
                {
                    f32 const dist = std::sqrt(
                        (mousePos.x - pin.Position.x) * (mousePos.x - pin.Position.x) +
                        (mousePos.y - pin.Position.y) * (mousePos.y - pin.Position.y));

                    if (dist <= s_PinRadius * m_Zoom * 2.0f && pin.IsOutput != m_DragStartIsOutput)
                    {
                        UUID outPin = m_DragStartIsOutput ? m_DragStartPinID : pin.PinID;
                        UUID inPin = m_DragStartIsOutput ? pin.PinID : m_DragStartPinID;

                        auto cmd = CreateScope<AddLinkCommand>(outPin, inPin);
                        m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());
                        m_IsDirty = true;
                        m_GraphAsset->MarkDirty();
                        break;
                    }
                }
            }

            m_IsDraggingConnection = false;
            m_DragStartPinID = 0;
        }
    }

    // =========================================================================
    // Context menu
    // =========================================================================

    void ShaderGraphEditorPanel::DrawContextMenu(const ImVec2& canvasOrigin)
    {
        if (m_ShowContextMenu)
        {
            ImGui::OpenPopup("##SGContextMenu");
            m_ShowContextMenu = false;
            m_NodeSearchFilter[0] = '\0';
        }

        if (ImGui::BeginPopup("##SGContextMenu"))
        {
            glm::vec2 const worldPos = ScreenToWorld(m_ContextMenuPos, canvasOrigin);

            // Search filter
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##NodeSearch", "Search nodes...", m_NodeSearchFilter, sizeof(m_NodeSearchFilter));
            ImGui::Separator();

            bool hasFilter = m_NodeSearchFilter[0] != '\0';

            if (hasFilter)
            {
                // Flat filtered list when searching
                std::string filterLower = m_NodeSearchFilter;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

                auto allTypes = GetAllNodeTypeNames();
                for (const auto& type : allTypes)
                {
                    // Case-insensitive substring match
                    std::string typeLower = type;
                    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);

                    if (typeLower.find(filterLower) != std::string::npos)
                    {
                        if (ImGui::MenuItem(type.c_str()))
                            CreateNode(type, worldPos);
                    }
                }
            }
            else
            {
                // Group by category when not searching
                auto drawCategory = [&](const char* label, ShaderGraphNodeCategory category)
                {
                    if (ImGui::BeginMenu(label))
                    {
                        auto types = GetNodeTypeNamesByCategory(category);
                        for (const auto& type : types)
                        {
                            if (ImGui::MenuItem(type.c_str()))
                                CreateNode(type, worldPos);
                        }
                        ImGui::EndMenu();
                    }
                };

                drawCategory("Input", ShaderGraphNodeCategory::Input);
                drawCategory("Math", ShaderGraphNodeCategory::Math);
                drawCategory("Texture", ShaderGraphNodeCategory::Texture);
                drawCategory("Utility", ShaderGraphNodeCategory::Utility);
                drawCategory("Custom", ShaderGraphNodeCategory::Custom);
                drawCategory("Compute", ShaderGraphNodeCategory::Compute);
                drawCategory("Output", ShaderGraphNodeCategory::Output);
            }

            ImGui::Separator();
            if (m_SelectedNodeID != 0 && ImGui::MenuItem("Delete Selected"))
            {
                DeleteNode(m_SelectedNodeID);
                m_SelectedNodeID = 0;
            }

            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Property panel
    // =========================================================================

    void ShaderGraphEditorPanel::DrawNodeProperties(ShaderGraphNode& node)
    {
        ImGui::Text("Node: %s", node.TypeName.c_str());
        ImGui::Separator();

        // Parameter name
        if (IsParameterNode(node.TypeName))
        {
            char buf[128];
            std::strncpy(buf, node.ParameterName.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Parameter Name", buf, sizeof(buf)))
            {
                node.ParameterName = buf;
                m_IsDirty = true;
                m_GraphAsset->MarkDirty();
            }
        }

        // Custom function body
        if (node.TypeName == ShaderGraphNodeTypes::CustomFunction)
        {
            char bodyBuf[512];
            std::strncpy(bodyBuf, node.CustomFunctionBody.c_str(), sizeof(bodyBuf) - 1);
            bodyBuf[sizeof(bodyBuf) - 1] = '\0';
            ImGui::Text("GLSL Expression:");
            ImGui::TextWrapped("Use input pin names (A, B) in the expression");
            if (ImGui::InputTextMultiline("##CustomBody", bodyBuf, sizeof(bodyBuf),
                                          ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4)))
            {
                node.CustomFunctionBody = bodyBuf;
                m_IsDirty = true;
                m_GraphAsset->MarkDirty();
            }
        }

        // Compute output workgroup size
        if (node.TypeName == ShaderGraphNodeTypes::ComputeOutput)
        {
            ImGui::Text("Workgroup Size:");
            bool changed = false;
            changed |= ImGui::DragInt("X", &node.WorkgroupSize.x, 1.0f, 1, 1024);
            changed |= ImGui::DragInt("Y", &node.WorkgroupSize.y, 1.0f, 1, 1024);
            changed |= ImGui::DragInt("Z", &node.WorkgroupSize.z, 1.0f, 1, 64);
            if (changed)
            {
                m_IsDirty = true;
                m_GraphAsset->MarkDirty();
            }
        }

        // Buffer binding index
        if (node.TypeName == ShaderGraphNodeTypes::ComputeBufferInput || node.TypeName == ShaderGraphNodeTypes::ComputeBufferStore)
        {
            if (ImGui::DragInt("Buffer Binding", &node.BufferBinding, 1.0f, 0, 15))
            {
                m_IsDirty = true;
                m_GraphAsset->MarkDirty();
            }
        }

        // Default values for inputs
        ImGui::Text("Inputs:");
        for (auto& pin : node.Inputs)
        {
            // Only show defaults for unconnected pins
            if (m_GraphAsset->GetGraph().GetLinkForInputPin(pin.ID))
            {
                ImGui::TextDisabled("  %s (connected)", pin.Name.c_str());
                continue;
            }

            ImGui::PushID(static_cast<int>(static_cast<u64>(pin.ID)));
            bool changed = false;

            if (pin.Type == ShaderGraphPinType::Float)
            {
                f32 val = std::holds_alternative<f32>(pin.DefaultValue) ? std::get<f32>(pin.DefaultValue) : 0.0f;
                if (ImGui::DragFloat(pin.Name.c_str(), &val, 0.01f))
                {
                    pin.DefaultValue = val;
                    changed = true;
                }
            }
            else if (pin.Type == ShaderGraphPinType::Vec2)
            {
                glm::vec2 val = std::holds_alternative<glm::vec2>(pin.DefaultValue) ? std::get<glm::vec2>(pin.DefaultValue) : glm::vec2(0.0f);
                if (ImGui::DragFloat2(pin.Name.c_str(), &val.x, 0.01f))
                {
                    pin.DefaultValue = val;
                    changed = true;
                }
            }
            else if (pin.Type == ShaderGraphPinType::Vec3)
            {
                glm::vec3 val = std::holds_alternative<glm::vec3>(pin.DefaultValue) ? std::get<glm::vec3>(pin.DefaultValue) : glm::vec3(0.0f);
                if (ImGui::ColorEdit3(pin.Name.c_str(), &val.x))
                {
                    pin.DefaultValue = val;
                    changed = true;
                }
            }
            else if (pin.Type == ShaderGraphPinType::Vec4)
            {
                glm::vec4 val = std::holds_alternative<glm::vec4>(pin.DefaultValue) ? std::get<glm::vec4>(pin.DefaultValue) : glm::vec4(0.0f);
                if (ImGui::ColorEdit4(pin.Name.c_str(), &val.x))
                {
                    pin.DefaultValue = val;
                    changed = true;
                }
            }
            else
            {
                ImGui::Text("  %s (%s)", pin.Name.c_str(), PinTypeToString(pin.Type));
            }

            if (changed)
            {
                m_IsDirty = true;
                m_GraphAsset->MarkDirty();
            }

            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Delete Node"))
        {
            DeleteNode(node.ID);
            m_SelectedNodeID = 0;
        }
    }

    void ShaderGraphEditorPanel::DrawPreviewPanel()
    {
        ImGui::Separator();
        ImGui::Text("Compile Preview");

        // Auto-compile when graph changes
        if (m_AutoCompile && m_GraphAsset && m_GraphAsset->IsDirty())
        {
            m_LastCompileResult = m_GraphAsset->Compile();
        }

        if (ImGui::Button("Compile"))
        {
            if (m_GraphAsset)
            {
                m_GraphAsset->MarkDirty();
                m_LastCompileResult = m_GraphAsset->Compile();
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-Compile", &m_AutoCompile);

        if (m_LastCompileResult.Success)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Compilation Successful");
            ImGui::Text("Parameters: %zu", m_LastCompileResult.ExposedParameters.size());

            // List exposed parameters with types
            if (!m_LastCompileResult.ExposedParameters.empty() && ImGui::TreeNode("Exposed Parameters"))
            {
                for (const auto& param : m_LastCompileResult.ExposedParameters)
                {
                    ImGui::BulletText("%s : %s", param.Name.c_str(),
                                      PinTypeToGLSL(param.Type));
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Generated Source"))
            {
                if (ImGui::Button("Copy to Clipboard"))
                    ImGui::SetClipboardText(m_LastCompileResult.ShaderSource.c_str());
                ImGui::TextWrapped("%s", m_LastCompileResult.ShaderSource.c_str());
                ImGui::TreePop();
            }
        }
        else if (!m_LastCompileResult.ErrorLog.empty())
        {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "Compilation Failed");
            ImGui::TextWrapped("%s", m_LastCompileResult.ErrorLog.c_str());
        }

        if (!m_LastCompileResult.ErrorLog.empty() && m_LastCompileResult.Success)
        {
            if (ImGui::TreeNode("Warnings"))
            {
                ImGui::TextWrapped("%s", m_LastCompileResult.ErrorLog.c_str());
                ImGui::TreePop();
            }
        }
    }

    // =========================================================================
    // Colors
    // =========================================================================

    ImU32 ShaderGraphEditorPanel::GetNodeColor(ShaderGraphNodeCategory category) const
    {
        switch (category)
        {
            case ShaderGraphNodeCategory::Input:
                return IM_COL32(40, 60, 80, 230);
            case ShaderGraphNodeCategory::Math:
                return IM_COL32(50, 50, 70, 230);
            case ShaderGraphNodeCategory::Texture:
                return IM_COL32(60, 50, 70, 230);
            case ShaderGraphNodeCategory::Utility:
                return IM_COL32(50, 60, 60, 230);
            case ShaderGraphNodeCategory::Custom:
                return IM_COL32(60, 50, 50, 230);
            case ShaderGraphNodeCategory::Compute:
                return IM_COL32(40, 55, 65, 230);
            case ShaderGraphNodeCategory::Output:
                return IM_COL32(70, 40, 40, 230);
        }
        return IM_COL32(50, 50, 60, 230);
    }

    ImU32 ShaderGraphEditorPanel::GetNodeHeaderColor(ShaderGraphNodeCategory category) const
    {
        switch (category)
        {
            case ShaderGraphNodeCategory::Input:
                return IM_COL32(50, 90, 130, 255);
            case ShaderGraphNodeCategory::Math:
                return IM_COL32(80, 80, 120, 255);
            case ShaderGraphNodeCategory::Texture:
                return IM_COL32(100, 70, 120, 255);
            case ShaderGraphNodeCategory::Utility:
                return IM_COL32(70, 100, 90, 255);
            case ShaderGraphNodeCategory::Custom:
                return IM_COL32(130, 80, 50, 255);
            case ShaderGraphNodeCategory::Compute:
                return IM_COL32(50, 100, 130, 255);
            case ShaderGraphNodeCategory::Output:
                return IM_COL32(130, 50, 50, 255);
        }
        return IM_COL32(70, 70, 90, 255);
    }

    ImU32 ShaderGraphEditorPanel::GetPinColor(ShaderGraphPinType type) const
    {
        switch (type)
        {
            case ShaderGraphPinType::Float:
                return IM_COL32(150, 200, 150, 255);
            case ShaderGraphPinType::Vec2:
                return IM_COL32(150, 200, 255, 255);
            case ShaderGraphPinType::Vec3:
                return IM_COL32(255, 255, 150, 255);
            case ShaderGraphPinType::Vec4:
                return IM_COL32(255, 200, 150, 255);
            case ShaderGraphPinType::Bool:
                return IM_COL32(255, 150, 150, 255);
            case ShaderGraphPinType::Texture2D:
                return IM_COL32(200, 150, 255, 255);
            default:
                return IM_COL32(200, 200, 200, 255);
        }
    }

    // =========================================================================
    // Coordinate transforms
    // =========================================================================

    ImVec2 ShaderGraphEditorPanel::WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const
    {
        return ImVec2(
            canvasOrigin.x + (worldPos.x + m_ScrollOffset.x) * m_Zoom,
            canvasOrigin.y + (worldPos.y + m_ScrollOffset.y) * m_Zoom);
    }

    glm::vec2 ShaderGraphEditorPanel::ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const
    {
        return glm::vec2(
            (screenPos.x - canvasOrigin.x) / m_Zoom - m_ScrollOffset.x,
            (screenPos.y - canvasOrigin.y) / m_Zoom - m_ScrollOffset.y);
    }

    // =========================================================================
    // Node operations
    // =========================================================================

    UUID ShaderGraphEditorPanel::CreateNode(const std::string& typeName, const glm::vec2& position)
    {
        if (!m_GraphAsset)
            return 0;

        auto cmd = CreateScope<AddNodeCommand>(typeName, position);
        auto* addCmd = cmd.get();
        m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());

        UUID id = addCmd->GetNodeID();
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
        return id;
    }

    void ShaderGraphEditorPanel::DeleteNode(UUID nodeID)
    {
        if (!m_GraphAsset)
            return;

        auto cmd = CreateScope<RemoveNodeCommand>(nodeID);
        m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    void ShaderGraphEditorPanel::DeleteLink(UUID linkID)
    {
        if (!m_GraphAsset)
            return;

        auto cmd = CreateScope<RemoveLinkCommand>(linkID);
        m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    // =========================================================================
    // Undo / Redo
    // =========================================================================

    void ShaderGraphEditorPanel::Undo()
    {
        if (!m_GraphAsset || !m_CommandHistory.CanUndo())
            return;

        m_CommandHistory.Undo(m_GraphAsset->GetGraph());
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    void ShaderGraphEditorPanel::Redo()
    {
        if (!m_GraphAsset || !m_CommandHistory.CanRedo())
            return;

        m_CommandHistory.Redo(m_GraphAsset->GetGraph());
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    // =========================================================================
    // Auto-layout
    // =========================================================================

    void ShaderGraphEditorPanel::AutoLayoutNodes()
    {
        if (!m_GraphAsset)
            return;

        auto& graph = m_GraphAsset->GetGraph();
        const auto& nodes = graph.GetNodes();
        if (nodes.empty())
            return;

        // Compute depth of each node from output (output = depth 0, its inputs = depth 1, etc.)
        std::unordered_map<u64, int> nodeDepth;
        int maxDepth = 0;

        // BFS from output node backwards
        const auto* outputNode = graph.FindOutputNode();
        if (!outputNode)
        {
            // No output node — just lay out linearly
            f32 x = 0.0f;
            for (const auto& node : nodes)
            {
                node->EditorPosition = glm::vec2(x, 0.0f);
                x += s_NodeWidth + 60.0f;
            }
            m_IsDirty = true;
            m_GraphAsset->MarkDirty();
            return;
        }

        // Mark all nodes with depth -1 (unvisited)
        for (const auto& node : nodes)
            nodeDepth[static_cast<u64>(node->ID)] = -1;

        // BFS backwards from output
        std::queue<UUID> bfsQueue;
        bfsQueue.push(outputNode->ID);
        nodeDepth[static_cast<u64>(outputNode->ID)] = 0;

        while (!bfsQueue.empty())
        {
            UUID currentID = bfsQueue.front();
            bfsQueue.pop();

            const auto* currentNode = graph.FindNode(currentID);
            if (!currentNode)
                continue;

            int currentDepthVal = nodeDepth[static_cast<u64>(currentID)];

            // For each input pin, find connected output pin's node
            for (const auto& inputPin : currentNode->Inputs)
            {
                const auto* link = graph.GetLinkForInputPin(inputPin.ID);
                if (!link)
                    continue;

                const auto* sourceNode = graph.FindNodeByPinID(link->OutputPinID);
                if (!sourceNode)
                    continue;

                int newDepth = currentDepthVal + 1;
                u64 sourceID = static_cast<u64>(sourceNode->ID);

                if (nodeDepth[sourceID] < newDepth)
                {
                    nodeDepth[sourceID] = newDepth;
                    maxDepth = std::max(maxDepth, newDepth);
                    bfsQueue.push(sourceNode->ID);
                }
            }
        }

        // Group nodes by depth column
        std::vector<std::vector<ShaderGraphNode*>> columns(maxDepth + 1);
        std::vector<ShaderGraphNode*> disconnected;

        for (const auto& node : nodes)
        {
            u64 id = static_cast<u64>(node->ID);
            if (nodeDepth[id] >= 0)
                columns[static_cast<size_t>(nodeDepth[id])].push_back(node.get());
            else
                disconnected.push_back(node.get());
        }

        // Position nodes: rightmost column = output, leftward = inputs
        constexpr f32 columnSpacing = 280.0f;
        constexpr f32 rowSpacing = 120.0f;

        for (int col = 0; col <= maxDepth; ++col)
        {
            f32 x = static_cast<f32>(maxDepth - col) * columnSpacing;
            f32 totalHeight = static_cast<f32>(columns[static_cast<size_t>(col)].size()) * rowSpacing;
            f32 startY = -totalHeight / 2.0f;

            for (size_t row = 0; row < columns[static_cast<size_t>(col)].size(); ++row)
            {
                columns[static_cast<size_t>(col)][row]->EditorPosition =
                    glm::vec2(x, startY + static_cast<f32>(row) * rowSpacing);
            }
        }

        // Place disconnected nodes below
        f32 disconnectedY = static_cast<f32>(maxDepth + 1) * rowSpacing;
        for (size_t i = 0; i < disconnected.size(); ++i)
        {
            disconnected[i]->EditorPosition =
                glm::vec2(static_cast<f32>(i) * columnSpacing, disconnectedY);
        }

        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    // =========================================================================
    // Copy / Paste
    // =========================================================================

    void ShaderGraphEditorPanel::CopySelectedNode()
    {
        if (!m_GraphAsset || m_SelectedNodeID == 0)
            return;

        const auto* node = m_GraphAsset->GetGraph().FindNode(m_SelectedNodeID);
        if (!node)
            return;

        m_CopiedNodeTypeName = node->TypeName;
        m_CopiedParameterName = node->ParameterName;
        m_CopiedCustomFunctionBody = node->CustomFunctionBody;
        m_CopiedWorkgroupSize = node->WorkgroupSize;
        m_CopiedBufferBinding = node->BufferBinding;
        m_CopiedInputs = node->Inputs;
        m_CopiedOutputs = node->Outputs;
        m_HasCopiedNode = true;
    }

    void ShaderGraphEditorPanel::PasteNodes(const glm::vec2& position)
    {
        if (!m_GraphAsset || !m_HasCopiedNode)
            return;

        // Use AddNodeCommand so paste is undoable
        auto cmd = CreateScope<AddNodeCommand>(m_CopiedNodeTypeName, position);
        auto* cmdPtr = cmd.get();
        m_CommandHistory.Execute(std::move(cmd), m_GraphAsset->GetGraph());

        // Apply copied properties to the newly created node
        UUID id = cmdPtr->GetNodeID();
        auto* newNode = m_GraphAsset->GetGraph().FindNode(id);
        if (newNode)
        {
            newNode->ParameterName = m_CopiedParameterName;
            newNode->CustomFunctionBody = m_CopiedCustomFunctionBody;
            newNode->WorkgroupSize = m_CopiedWorkgroupSize;
            newNode->BufferBinding = m_CopiedBufferBinding;

            // Copy default values from saved pins
            for (size_t i = 0; i < newNode->Inputs.size() && i < m_CopiedInputs.size(); ++i)
                newNode->Inputs[i].DefaultValue = m_CopiedInputs[i].DefaultValue;
        }

        m_SelectedNodeID = id;
        m_IsDirty = true;
        m_GraphAsset->MarkDirty();
    }

    // =========================================================================
    // Keyboard shortcuts
    // =========================================================================

    void ShaderGraphEditorPanel::HandleKeyboardShortcuts()
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
            return;

        // Don't capture shortcuts when an input widget is active
        if (ImGui::GetIO().WantTextInput)
            return;

        bool const ctrl = ImGui::GetIO().KeyCtrl;

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            Undo();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelectedNode();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            PasteNodes(glm::vec2(200.0f, 200.0f));
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            SaveShaderGraph();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            std::string filepath = FileDialogs::OpenFile("OloEngine Shader Graph (*.olosg)\0*.olosg\0All Files\0*.*\0");
            if (!filepath.empty())
                OpenShaderGraph(std::filesystem::path(filepath));
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_SelectedNodeID != 0)
        {
            DeleteNode(m_SelectedNodeID);
            m_SelectedNodeID = 0;
        }
    }

    // =========================================================================
    // File operations
    // =========================================================================

    void ShaderGraphEditorPanel::NewShaderGraph()
    {
        m_GraphAsset = Ref<ShaderGraphAsset>::Create();
        m_GraphAsset->GetGraph().SetName("NewShaderGraph");

        // Start with a PBR output node
        auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
        outputNode->EditorPosition = glm::vec2(400.0f, 200.0f);
        m_GraphAsset->GetGraph().AddNode(std::move(outputNode));

        m_CurrentFilePath.clear();
        m_CurrentAssetHandle = 0;
        m_IsDirty = false;
        m_SelectedNodeID = 0;
        m_ScrollOffset = { 0.0f, 0.0f };
        m_Zoom = 1.0f;
        m_LastCompileResult = {};
        m_CommandHistory.Clear();
    }

    void ShaderGraphEditorPanel::OpenShaderGraph(const std::filesystem::path& path)
    {
        m_PendingLoadPath = path;
        m_PendingLoadHandle = 0;
        m_PendingLoadFrameDelay = 0;
    }

    void ShaderGraphEditorPanel::OpenShaderGraph(AssetHandle handle)
    {
        auto metadata = AssetManager::GetAssetMetadata(handle);
        if (metadata.IsValid())
        {
            m_PendingLoadPath = Project::GetAssetFileSystemPath(metadata.FilePath);
            m_PendingLoadHandle = handle;
            m_PendingLoadFrameDelay = 0;
        }
    }

    void ShaderGraphEditorPanel::SaveShaderGraph()
    {
        if (m_CurrentFilePath.empty())
        {
            SaveShaderGraphAs();
            return;
        }

        if (!m_GraphAsset)
            return;

        ShaderGraphSerializer serializer;
        AssetMetadata metadata;
        metadata.FilePath = std::filesystem::relative(m_CurrentFilePath, Project::GetAssetDirectory());
        metadata.Handle = m_CurrentAssetHandle;
        serializer.Serialize(metadata, m_GraphAsset);
        m_IsDirty = false;
    }

    void ShaderGraphEditorPanel::SaveShaderGraphAs()
    {
        if (!m_GraphAsset)
            return;

        std::string filepath = FileDialogs::SaveFile("OloEngine Shader Graph (*.olosg)\0*.olosg\0All Files\0*.*\0");
        if (filepath.empty())
            return;

        // Ensure .olosg extension
        std::filesystem::path savePath(filepath);
        if (savePath.extension() != ".olosg")
            savePath.replace_extension(".olosg");

        m_CurrentFilePath = savePath;
        SaveShaderGraph();
    }

    void ShaderGraphEditorPanel::LoadShaderGraph(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("ShaderGraphEditorPanel - File does not exist: {}", path.string());
            return;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ShaderGraphEditorPanel - Failed to open file: {}", path.string());
            return;
        }

        file.seekg(0, std::ios::end);
        std::string content(static_cast<size_t>(file.tellg()), '\0');
        file.seekg(0, std::ios::beg);
        file.read(content.data(), static_cast<std::streamsize>(content.size()));

        ShaderGraphSerializer serializer;
        m_GraphAsset = Ref<ShaderGraphAsset>::Create();
        if (!serializer.TestDeserializeFromYAML(content, m_GraphAsset))
        {
            OLO_CORE_ERROR("ShaderGraphEditorPanel - Failed to deserialize: {}", path.string());
            m_GraphAsset = nullptr;
            return;
        }

        m_IsDirty = false;
        m_SelectedNodeID = 0;
        m_ScrollOffset = { 0.0f, 0.0f };
        m_Zoom = 1.0f;
        m_LastCompileResult = {};
        m_CommandHistory.Clear();
    }

    void ShaderGraphEditorPanel::PerformPendingLoad()
    {
        auto loadPath = std::move(m_PendingLoadPath);
        auto loadHandle = m_PendingLoadHandle;
        m_PendingLoadPath.clear();
        m_PendingLoadFrameDelay = 0;

        LoadShaderGraph(loadPath);
        if (m_GraphAsset)
        {
            m_CurrentFilePath = loadPath;
            if (loadHandle)
                m_CurrentAssetHandle = loadHandle;
        }
    }

} // namespace OloEngine
