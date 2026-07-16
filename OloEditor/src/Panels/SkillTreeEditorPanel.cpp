#include "OloEnginePCH.h"
#include "SkillTreeEditorPanel.h"

#include "../UndoRedo/EditorCommand.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace OloEngine
{
    // =========================================================================
    // Undo/Redo helpers
    // =========================================================================

    SkillTreeEditorSnapshot SkillTreeEditorPanel::CaptureSnapshot() const
    {
        if (!m_Tree)
        {
            return {};
        }
        return { m_Tree->m_TreeID, m_Tree->m_DisplayName, m_Tree->m_Nodes };
    }

    void SkillTreeEditorPanel::RestoreSnapshot(const SkillTreeEditorSnapshot& snapshot)
    {
        if (!m_Tree)
        {
            m_Tree = Ref<SkillTreeDatabase>::Create();
        }
        m_Tree->m_TreeID = snapshot.TreeID;
        m_Tree->m_DisplayName = snapshot.DisplayName;
        m_Tree->m_Nodes = snapshot.Nodes;
        m_Tree->RebuildIndex();
        m_SelectedNodeID.clear();
        m_IsDirty = true;
    }

    void SkillTreeEditorPanel::PushUndoCommand(const SkillTreeEditorSnapshot& oldState, const std::string& description)
    {
        if (!m_CommandHistory)
        {
            return;
        }

        auto newState = CaptureSnapshot();
        // Skip no-op commands (bitwise/value equality via the defaulted ==,
        // same intent as the Dialogue editor's structural no-op check)
        if (oldState == newState)
        {
            return;
        }

        auto* panel = this;
        m_CommandHistory->PushAlreadyExecuted(
            std::make_unique<SkillTreeEditorChangeCommand>(
                oldState, std::move(newState),
                [panel](const SkillTreeEditorSnapshot& s)
                { panel->RestoreSnapshot(s); },
                description));
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void SkillTreeEditorPanel::OnImGuiRender()
    {
        if (!m_IsOpen)
        {
            m_IsFocused = false;
            return;
        }

        if (!m_Tree)
        {
            NewTree();
        }

        ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
        std::string windowTitle = "Skill Tree Editor";
        if (!m_FilePath.empty())
        {
            windowTitle += " - " + m_FilePath.filename().string();
        }
        if (m_IsDirty)
        {
            windowTitle += " *";
        }
        windowTitle += "###SkillTreeEditor";

        if (!ImGui::Begin(windowTitle.c_str(), &m_IsOpen, ImGuiWindowFlags_MenuBar))
        {
            m_IsFocused = false;
            ImGui::End();
            return;
        }

        m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        DrawToolbar();

        // Error popup (save validation failures, cycle rejections, load errors)
        if (m_ShowErrorPopup)
        {
            ImGui::OpenPopup("Skill Tree Error");
            m_ShowErrorPopup = false;
        }
        if (ImGui::BeginPopupModal("Skill Tree Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Layout: left = canvas, right = property panel
        f32 const availWidth = ImGui::GetContentRegionAvail().x;
        f32 const canvasWidth = std::max(availWidth - s_PropertyPanelWidth, 100.0f);

        ImGui::BeginChild("##SkillTreeCanvas", ImVec2(canvasWidth, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        DrawCanvas();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##SkillTreeProperties", ImVec2(s_PropertyPanelWidth, 0), ImGuiChildFlags_Borders);
        DrawPropertyPanel();
        ImGui::EndChild();

        ImGui::End();
    }

    void SkillTreeEditorPanel::OpenSkillTree(const std::filesystem::path& path)
    {
        LoadTree(path);
    }

    void SkillTreeEditorPanel::OpenSkillTree(AssetHandle handle)
    {
        auto metadata = AssetManager::GetAssetMetadata(handle);
        if (!metadata.IsValid())
        {
            OLO_CORE_WARN("SkillTreeEditorPanel - No metadata for asset handle {}", static_cast<u64>(handle));
            return;
        }

        // EditorAssetManager stores registry paths relative to the PROJECT
        // directory (see EditorAssetManager::GetRelativePath), while some
        // tooling records asset-directory-relative paths. Resolve against the
        // project root first, then fall back to the asset-directory join.
        std::filesystem::path fsPath = Project::GetProjectDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(fsPath))
        {
            fsPath = Project::GetAssetFileSystemPath(metadata.FilePath);
        }

        LoadTree(fsPath);
        // LoadTree leaves prior state untouched on failure, so only claim the
        // handle when THIS file actually became the working copy.
        if (m_Tree && m_FilePath == fsPath)
        {
            m_CurrentAssetHandle = handle;
        }
    }

    void SkillTreeEditorPanel::NewTree()
    {
        m_Tree = Ref<SkillTreeDatabase>::Create();
        m_Tree->m_TreeID = "new_skill_tree";
        m_Tree->m_DisplayName = "New Skill Tree";

        SkillTreeNode rootNode;
        rootNode.NodeID = "root";
        rootNode.DisplayName = "New Skill";
        rootNode.EditorPosition = { 0.0f, 0.0f };
        m_Tree->m_Nodes.push_back(std::move(rootNode));
        m_Tree->RebuildIndex();

        m_FilePath.clear();
        m_CurrentAssetHandle = 0;
        m_IsDirty = false;
        m_SelectedNodeID.clear();
        m_NodeIDEditSource.clear();
        m_NodeIDError.clear();
        m_ScrollOffset = { 100.0f, 100.0f };
        m_Zoom = 1.0f;
        m_IsDraggingNode = false;
        m_IsCreatingConnection = false;
        m_ConnectionSourceNodeID.clear();
        m_IsEditingProperties = false;
    }

    // =========================================================================
    // Canvas
    // =========================================================================

    void SkillTreeEditorPanel::DrawCanvas()
    {
        ImVec2 const canvasOrigin = ImGui::GetCursorScreenPos();
        ImVec2 const canvasSize = ImGui::GetContentRegionAvail();
        ImVec2 const canvasEnd = ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y);

        if (m_FrameAllRequested)
        {
            FrameAll(canvasSize);
            m_FrameAllRequested = false;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasOrigin, canvasEnd, IM_COL32(30, 30, 35, 255));

        // Clip
        drawList->PushClipRect(canvasOrigin, canvasEnd, true);

        DrawGrid(drawList, canvasOrigin, canvasSize);
        DrawEdges(drawList, canvasOrigin);
        DrawNodes(drawList, canvasOrigin);
        DrawConnectionInProgress(drawList, canvasOrigin);

        drawList->PopClipRect();

        // Invisible button for canvas interaction
        ImGui::SetCursorScreenPos(canvasOrigin);
        ImGui::InvisibleButton("##skilltreecanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

        HandleCanvasInput(canvasOrigin, canvasSize);
        HandleNodeInteraction(canvasOrigin);
        HandleConnectionDrag(canvasOrigin);
        DrawContextMenu(canvasOrigin);
    }

    void SkillTreeEditorPanel::DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize) const
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

    ImVec2 SkillTreeEditorPanel::GetNodeSize() const
    {
        return ImVec2(s_NodeWidth * m_Zoom, (s_NodeHeaderHeight + s_NodeBodyHeight) * m_Zoom);
    }

    ImVec2 SkillTreeEditorPanel::GetInputPortPos(const ImVec2& nodeScreenPos) const
    {
        ImVec2 const nodeSize = GetNodeSize();
        return ImVec2(nodeScreenPos.x, nodeScreenPos.y + nodeSize.y * 0.5f);
    }

    ImVec2 SkillTreeEditorPanel::GetOutputPortPos(const ImVec2& nodeScreenPos) const
    {
        ImVec2 const nodeSize = GetNodeSize();
        return ImVec2(nodeScreenPos.x + nodeSize.x, nodeScreenPos.y + nodeSize.y * 0.5f);
    }

    auto SkillTreeEditorPanel::GetNodeColor(SkillTreeNode::PayloadKind payload) const -> ImU32
    {
        switch (payload)
        {
            case SkillTreeNode::PayloadKind::Ability:
                return IM_COL32(70, 55, 50, 230);
            case SkillTreeNode::PayloadKind::PassiveEffect:
                return IM_COL32(50, 55, 75, 230);
            case SkillTreeNode::PayloadKind::None:
            default:
                return IM_COL32(55, 55, 55, 230);
        }
    }

    auto SkillTreeEditorPanel::GetNodeHeaderColor(SkillTreeNode::PayloadKind payload) const -> ImU32
    {
        switch (payload)
        {
            case SkillTreeNode::PayloadKind::Ability:
                return IM_COL32(180, 100, 60, 240);
            case SkillTreeNode::PayloadKind::PassiveEffect:
                return IM_COL32(60, 100, 170, 240);
            case SkillTreeNode::PayloadKind::None:
            default:
                return IM_COL32(100, 100, 100, 240);
        }
    }

    void SkillTreeEditorPanel::DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_Tree)
        {
            return;
        }
        for (const auto& node : m_Tree->m_Nodes)
        {
            DrawNode(drawList, canvasOrigin, node);
        }
    }

    void SkillTreeEditorPanel::DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, const SkillTreeNode& node)
    {
        ImVec2 const nodePos = WorldToScreen(node.EditorPosition, canvasOrigin);
        ImVec2 const nodeSize = GetNodeSize();
        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        ImVec2 const headerEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + s_NodeHeaderHeight * m_Zoom);

        bool const isSelected = (node.NodeID == m_SelectedNodeID);

        // Node body + header
        f32 const rounding = 6.0f * m_Zoom;
        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.Payload), rounding);
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.Payload), rounding, ImDrawFlags_RoundCornersTop);

        // Selection outline
        if (isSelected)
        {
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 220), rounding, 0, 2.5f * m_Zoom);
        }

        // Header text: display name (falls back to the id)
        f32 const fontSize = 13.0f * m_Zoom;
        const std::string& headerText = node.DisplayName.empty() ? node.NodeID : node.DisplayName;
        ImVec2 const textPos = ImVec2(nodePos.x + s_NodePadding * m_Zoom, nodePos.y + 5.0f * m_Zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 240), headerText.c_str());

        // Body line 1: level requirement / cost
        f32 contentY = nodePos.y + (s_NodeHeaderHeight + 4.0f) * m_Zoom;
        std::string costLine = "Lv " + std::to_string(node.LevelRequirement) + " / cost " + std::to_string(node.SkillPointCost);
        drawList->AddText(nullptr, 11.0f * m_Zoom,
                          ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                          IM_COL32(200, 200, 200, 220), costLine.c_str());
        contentY += 15.0f * m_Zoom;

        // Body line 2: payload badge
        std::string payloadLine;
        ImU32 payloadColor = IM_COL32(150, 150, 150, 200);
        if (node.Payload == SkillTreeNode::PayloadKind::Ability)
        {
            payloadLine = "Ability: " + (node.GrantedAbility.Name.empty() ? std::string("<unnamed>") : node.GrantedAbility.Name);
            payloadColor = IM_COL32(255, 180, 150, 220);
        }
        else if (node.Payload == SkillTreeNode::PayloadKind::PassiveEffect)
        {
            payloadLine = "Passive: " + (node.PassiveEffect.Name.empty() ? std::string("<unnamed>") : node.PassiveEffect.Name);
            payloadColor = IM_COL32(150, 200, 255, 220);
        }
        else
        {
            payloadLine = "(no payload)";
        }
        drawList->AddText(nullptr, 11.0f * m_Zoom,
                          ImVec2(nodePos.x + s_NodePadding * m_Zoom, contentY),
                          payloadColor, payloadLine.c_str());

        // Ports: input (left, prerequisites arrive here), output (right,
        // dependents leave from here)
        f32 const radius = s_NodePortRadius * m_Zoom;
        ImU32 const portFill = IM_COL32(40, 40, 45, 255);
        ImVec2 const inPort = GetInputPortPos(nodePos);
        ImVec2 const outPort = GetOutputPortPos(nodePos);
        drawList->AddCircleFilled(inPort, radius, portFill);
        drawList->AddCircle(inPort, radius, IM_COL32(100, 150, 255, 220), 12, 2.0f * m_Zoom);
        drawList->AddCircleFilled(outPort, radius, portFill);
        drawList->AddCircle(outPort, radius, IM_COL32(100, 200, 100, 220), 12, 2.0f * m_Zoom);
    }

    void SkillTreeEditorPanel::DrawEdges(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_Tree)
        {
            return;
        }

        for (const auto& dependent : m_Tree->m_Nodes)
        {
            ImVec2 const dependentPos = WorldToScreen(dependent.EditorPosition, canvasOrigin);
            ImVec2 const endPos = GetInputPortPos(dependentPos);

            for (const auto& prereqId : dependent.Prerequisites)
            {
                i32 const prereqIndex = FindNodeIndex(prereqId);
                if (prereqIndex < 0)
                {
                    continue;
                }
                const SkillTreeNode& prereq = m_Tree->m_Nodes[static_cast<sizet>(prereqIndex)];
                ImVec2 const prereqPos = WorldToScreen(prereq.EditorPosition, canvasOrigin);
                ImVec2 const startPos = GetOutputPortPos(prereqPos);

                f32 const dist = std::abs(endPos.x - startPos.x) * 0.5f;
                ImVec2 const cp1 = ImVec2(startPos.x + dist, startPos.y);
                ImVec2 const cp2 = ImVec2(endPos.x - dist, endPos.y);

                ImU32 const lineColor = IM_COL32(180, 180, 200, 200);
                drawList->AddBezierCubic(startPos, cp1, cp2, endPos, lineColor, 2.0f * m_Zoom);

                // Arrow head at the dependent end
                glm::vec2 rawDir(endPos.x - cp2.x, endPos.y - cp2.y);
                f32 const dirLen = glm::length(rawDir);
                if (dirLen < 1e-6f)
                {
                    continue;
                }
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
    }

    void SkillTreeEditorPanel::DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin)
    {
        if (!m_IsCreatingConnection || !m_Tree)
        {
            return;
        }

        i32 const srcIndex = FindNodeIndex(m_ConnectionSourceNodeID);
        if (srcIndex < 0)
        {
            return;
        }

        ImVec2 const srcNodePos = WorldToScreen(m_Tree->m_Nodes[static_cast<sizet>(srcIndex)].EditorPosition, canvasOrigin);
        ImVec2 const startPos = GetOutputPortPos(srcNodePos);
        ImVec2 const endPos = m_ConnectionEndPos;

        f32 const dist = std::abs(endPos.x - startPos.x) * 0.5f;
        ImVec2 const cp1 = ImVec2(startPos.x + dist, startPos.y);
        ImVec2 const cp2 = ImVec2(endPos.x - dist, endPos.y);

        drawList->AddBezierCubic(startPos, cp1, cp2, endPos, IM_COL32(255, 255, 100, 180), 2.0f * m_Zoom);
    }

    // =========================================================================
    // Interaction
    // =========================================================================

    std::string SkillTreeEditorPanel::HitTestNode(const ImVec2& screenPos, const ImVec2& canvasOrigin) const
    {
        if (!m_Tree)
        {
            return {};
        }

        ImVec2 const nodeSize = GetNodeSize();
        // Reverse iterate so nodes drawn last (topmost) are hit first
        for (auto it = m_Tree->m_Nodes.rbegin(); it != m_Tree->m_Nodes.rend(); ++it)
        {
            ImVec2 const nodePos = WorldToScreen(it->EditorPosition, canvasOrigin);
            if (screenPos.x >= nodePos.x && screenPos.x <= nodePos.x + nodeSize.x &&
                screenPos.y >= nodePos.y && screenPos.y <= nodePos.y + nodeSize.y)
            {
                return it->NodeID;
            }
        }
        return {};
    }

    void SkillTreeEditorPanel::HandleCanvasInput(const ImVec2& canvasOrigin, [[maybe_unused]] const ImVec2& canvasSize)
    {
        bool const isHovered = ImGui::IsItemHovered();

        // Zoom with scroll wheel (bit-exact zero check — ImGui reports 0.0f
        // when no wheel motion this frame; discrete tick values never alias
        // to subnormals, cpp-coding-quality §2a)
        if (isHovered)
        {
            f32 const scroll = ImGui::GetIO().MouseWheel;
            constexpr f32 noScroll = 0.0f;
            if (std::memcmp(&scroll, &noScroll, sizeof(f32)) != 0)
            {
                ImVec2 const mousePos = ImGui::GetIO().MousePos;
                glm::vec2 const worldBefore = ScreenToWorld(mousePos, canvasOrigin);

                m_Zoom = std::clamp(m_Zoom + scroll * 0.1f * m_Zoom, s_MinZoom, s_MaxZoom);

                glm::vec2 const worldAfter = ScreenToWorld(mousePos, canvasOrigin);
                m_ScrollOffset += (worldAfter - worldBefore);
            }
        }

        // Pan with middle or right mouse drag
        bool const wantPan = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                             ImGui::IsMouseDragging(ImGuiMouseButton_Right);
        if (isHovered && wantPan)
        {
            ImVec2 const delta = ImGui::GetIO().MouseDelta;
            m_ScrollOffset.x += delta.x / m_Zoom;
            m_ScrollOffset.y += delta.y / m_Zoom;
            m_IsPanning = true;
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
            {
                m_RightDragPanned = true;
            }
        }
        else
        {
            m_IsPanning = false;
        }

        // Right-click (release without a pan drag) opens the context menu.
        // The flags reset on ANY right release — not just hovered ones — so a
        // pan that ends outside the canvas can't suppress the next real click.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            if (isHovered && !m_RightDragPanned && !m_IsCreatingConnection && !m_SuppressNextContextMenu)
            {
                m_ContextMenuPos = ImGui::GetIO().MousePos;
                m_ContextMenuNodeID = HitTestNode(m_ContextMenuPos, canvasOrigin);
                m_ShowContextMenu = true;
            }
            m_RightDragPanned = false;
            m_SuppressNextContextMenu = false;
        }

        // Click on empty space deselects
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDraggingNode && !m_IsCreatingConnection)
        {
            if (HitTestNode(ImGui::GetIO().MousePos, canvasOrigin).empty())
            {
                m_SelectedNodeID.clear();
            }
        }

        // Delete selected node
        if (!m_SelectedNodeID.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsAnyItemActive())
        {
            DeleteNode(m_SelectedNodeID);
        }

        // Keyboard shortcuts (gated on window focus so a background panel
        // can't steal a global Ctrl+S)
        if (m_IsFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            SaveTree();
        }
        if (m_IsFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
        {
            NewTree();
        }
    }

    void SkillTreeEditorPanel::HandleNodeInteraction(const ImVec2& canvasOrigin)
    {
        if (!m_Tree || m_IsCreatingConnection)
        {
            return;
        }

        ImVec2 const mousePos = ImGui::GetIO().MousePos;

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            // Output-port hit starts a prerequisite connection drag (this node
            // becomes the prerequisite of whatever node the drag is released on)
            bool startedConnection = false;
            for (auto it = m_Tree->m_Nodes.rbegin(); it != m_Tree->m_Nodes.rend(); ++it)
            {
                ImVec2 const nodePos = WorldToScreen(it->EditorPosition, canvasOrigin);
                ImVec2 const outPort = GetOutputPortPos(nodePos);
                f32 const dist = std::hypot(mousePos.x - outPort.x, mousePos.y - outPort.y);
                if (dist <= s_NodePortRadius * m_Zoom * 2.0f)
                {
                    m_IsCreatingConnection = true;
                    m_ConnectionSourceNodeID = it->NodeID;
                    m_ConnectionEndPos = mousePos;
                    startedConnection = true;
                    break;
                }
            }

            if (!startedConnection)
            {
                if (std::string hit = HitTestNode(mousePos, canvasOrigin); !hit.empty())
                {
                    m_SelectedNodeID = hit;
                    m_IsDraggingNode = true;
                    if (i32 idx = FindNodeIndex(hit); idx >= 0)
                    {
                        ImVec2 const nodePos = WorldToScreen(m_Tree->m_Nodes[static_cast<sizet>(idx)].EditorPosition, canvasOrigin);
                        m_DragStartOffset = glm::vec2(mousePos.x - nodePos.x, mousePos.y - nodePos.y);
                    }
                    m_DragStartSnapshot = CaptureSnapshot();
                }
            }
        }

        // Drag the selected node (writes EditorPosition)
        if (m_IsDraggingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            if (SkillTreeNode* node = FindNodeMutable(m_SelectedNodeID); node)
            {
                ImVec2 const targetScreen = ImVec2(
                    mousePos.x - m_DragStartOffset.x,
                    mousePos.y - m_DragStartOffset.y);
                node->EditorPosition = ScreenToWorld(targetScreen, canvasOrigin);
                m_IsDirty = true;
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_IsDraggingNode)
            {
                PushUndoCommand(m_DragStartSnapshot, "Move Skill Node");
            }
            m_IsDraggingNode = false;
        }
    }

    void SkillTreeEditorPanel::HandleConnectionDrag(const ImVec2& canvasOrigin)
    {
        if (!m_IsCreatingConnection)
        {
            return;
        }

        m_ConnectionEndPos = ImGui::GetIO().MousePos;

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (std::string target = HitTestNode(m_ConnectionEndPos, canvasOrigin); !target.empty())
            {
                TryAddPrerequisite(m_ConnectionSourceNodeID, target);
            }
            m_IsCreatingConnection = false;
            m_ConnectionSourceNodeID.clear();
        }

        // Cancel with right click (and swallow the context menu that release
        // would otherwise open)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_IsCreatingConnection = false;
            m_ConnectionSourceNodeID.clear();
            m_SuppressNextContextMenu = true;
        }
    }

    void SkillTreeEditorPanel::DrawContextMenu(const ImVec2& canvasOrigin)
    {
        if (m_ShowContextMenu)
        {
            ImGui::OpenPopup("##SkillTreeContextMenu");
            m_ShowContextMenu = false;
        }

        if (ImGui::BeginPopup("##SkillTreeContextMenu"))
        {
            glm::vec2 const worldPos = ScreenToWorld(m_ContextMenuPos, canvasOrigin);

            if (ImGui::MenuItem("Add Node"))
            {
                AddNode(worldPos);
            }

            if (!m_ContextMenuNodeID.empty())
            {
                ImGui::Separator();
                if (ImGui::MenuItem("Add Prerequisite (drag to dependent)"))
                {
                    // This node becomes the prerequisite; the user then clicks
                    // the dependent node to complete the edge.
                    m_IsCreatingConnection = true;
                    m_ConnectionSourceNodeID = m_ContextMenuNodeID;
                    m_ConnectionEndPos = m_ContextMenuPos;
                }
                if (ImGui::MenuItem("Delete Node"))
                {
                    DeleteNode(m_ContextMenuNodeID);
                }
            }

            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void SkillTreeEditorPanel::DrawToolbar()
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N"))
                {
                    NewTree();
                }
                if (ImGui::MenuItem("Open..."))
                {
                    std::string filepath = FileDialogs::OpenFile(
                        "Skill Tree (*.oloskilltree)\0*.oloskilltree\0"
                        "All Files (*.*)\0*.*\0");
                    if (!filepath.empty())
                    {
                        OpenSkillTree(std::filesystem::path(filepath));
                    }
                }
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                {
                    SaveTree();
                }
                if (ImGui::MenuItem("Save As..."))
                {
                    SaveTreeAs();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Reset Zoom"))
                {
                    m_Zoom = 1.0f;
                }
                if (ImGui::MenuItem("Frame All"))
                {
                    m_FrameAllRequested = true;
                }
                ImGui::EndMenu();
            }

            if (m_IsDirty)
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "(unsaved)");
            }

            ImGui::EndMenuBar();
        }
    }

    // =========================================================================
    // Property panel
    // =========================================================================

    void SkillTreeEditorPanel::DrawPropertyPanel()
    {
        if (!m_Tree)
        {
            return;
        }

        if (SkillTreeNode* node = FindNodeMutable(m_SelectedNodeID); node)
        {
            ImGui::Text("Node Properties");
            ImGui::Separator();
            DrawNodeProperties(*node);
        }
        else
        {
            DrawTreeProperties();
        }
    }

    void SkillTreeEditorPanel::DrawTreeProperties()
    {
        // Snapshot at start of an edit session (not every frame)
        if (m_CommandHistory && !m_IsEditingProperties)
        {
            m_PropertyEditSnapshot = CaptureSnapshot();
        }

        bool anyChanged = false;

        ImGui::Text("Skill Tree");
        ImGui::Separator();

        if (ImGui::InputText("Tree ID", &m_Tree->m_TreeID))
        {
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::InputText("Display Name", &m_Tree->m_DisplayName))
        {
            m_IsDirty = true;
            anyChanged = true;
        }

        ImGui::Text("Nodes: %d", static_cast<int>(m_Tree->m_Nodes.size()));

        ImGui::Separator();
        if (std::string error; m_Tree->Validate(&error))
        {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Valid");
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Select a node on the canvas to edit it.\nRight-click the canvas to add nodes.");

        // Track property edit sessions for undo
        if (m_CommandHistory)
        {
            if (anyChanged)
            {
                m_IsEditingProperties = true;
            }
            if (m_IsEditingProperties && GImGui->ActiveId == 0)
            {
                PushUndoCommand(m_PropertyEditSnapshot, "Edit Skill Tree Properties");
                m_IsEditingProperties = false;
            }
        }
    }

    void SkillTreeEditorPanel::DrawNodeProperties(SkillTreeNode& node)
    {
        // Snapshot at start of an edit session (not every frame)
        if (m_CommandHistory && !m_IsEditingProperties)
        {
            m_PropertyEditSnapshot = CaptureSnapshot();
        }

        bool anyChanged = false;

        // --- NodeID (committed on deactivate; refuses empty/duplicate ids) ---
        if (m_NodeIDEditSource != node.NodeID)
        {
            m_NodeIDEditSource = node.NodeID;
            m_NodeIDEditBuffer = node.NodeID;
            m_NodeIDError.clear();
        }
        ImGui::InputText("Node ID", &m_NodeIDEditBuffer);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            if (m_NodeIDEditBuffer.empty())
            {
                m_NodeIDError = "Node ID cannot be empty";
                m_NodeIDEditBuffer = node.NodeID;
            }
            else if (m_NodeIDEditBuffer != node.NodeID && FindNodeIndex(m_NodeIDEditBuffer) != -1)
            {
                m_NodeIDError = "Duplicate Node ID: " + m_NodeIDEditBuffer;
                m_NodeIDEditBuffer = node.NodeID;
            }
            else if (m_NodeIDEditBuffer != node.NodeID)
            {
                // Rename, updating every prerequisite reference so the DAG
                // stays consistent
                const std::string oldId = node.NodeID;
                node.NodeID = m_NodeIDEditBuffer;
                for (auto& other : m_Tree->m_Nodes)
                {
                    for (auto& prereq : other.Prerequisites)
                    {
                        if (prereq == oldId)
                        {
                            prereq = node.NodeID;
                        }
                    }
                }
                m_Tree->RebuildIndex();
                m_SelectedNodeID = node.NodeID;
                m_NodeIDEditSource = node.NodeID;
                m_NodeIDError.clear();
                m_IsDirty = true;
                anyChanged = true;
            }
            else
            {
                m_NodeIDError.clear();
            }
        }
        if (!m_NodeIDError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_NodeIDError.c_str());
        }

        if (ImGui::InputText("Display Name", &node.DisplayName))
        {
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::InputTextMultiline("Description", &node.Description, ImVec2(-1, 80)))
        {
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::DragInt("Level Requirement", &node.LevelRequirement, 0.1f))
        {
            node.LevelRequirement = std::max(node.LevelRequirement, 1);
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::DragInt("Skill Point Cost", &node.SkillPointCost, 0.1f))
        {
            node.SkillPointCost = std::max(node.SkillPointCost, 0);
            m_IsDirty = true;
            anyChanged = true;
        }

        // --- Prerequisites ---
        ImGui::Separator();
        ImGui::Text("Prerequisites:");
        if (node.Prerequisites.empty())
        {
            ImGui::TextDisabled("(none - root node)");
        }
        for (sizet i = 0; i < node.Prerequisites.size(); ++i)
        {
            ImGui::BulletText("%s", node.Prerequisites[i].c_str());
            ImGui::SameLine();
            if (std::string removeLabel = "X##prereq" + std::to_string(i); ImGui::SmallButton(removeLabel.c_str()))
            {
                node.Prerequisites.erase(node.Prerequisites.begin() + static_cast<ptrdiff_t>(i));
                m_IsDirty = true;
                anyChanged = true;
                break;
            }
        }
        ImGui::TextDisabled("Drag from a node's right port onto this\nnode to add a prerequisite.");

        // --- Payload ---
        ImGui::Separator();
        {
            const char* payloadKinds[] = { "None", "Ability", "Passive Effect" };
            int payloadIdx = static_cast<int>(node.Payload);
            if (ImGui::Combo("Payload", &payloadIdx, payloadKinds, 3))
            {
                node.Payload = static_cast<SkillTreeNode::PayloadKind>(payloadIdx);
                m_IsDirty = true;
                anyChanged = true;
            }
        }

        if (node.Payload == SkillTreeNode::PayloadKind::Ability)
        {
            DrawAbilityPayloadProperties(node, anyChanged);
        }
        else if (node.Payload == SkillTreeNode::PayloadKind::PassiveEffect)
        {
            DrawPassiveEffectPayloadProperties(node, anyChanged);
        }
        else
        {
            // No additional handling required.
        }

        // --- Position ---
        ImGui::Separator();
        if (ImGui::DragFloat2("Position", &node.EditorPosition.x, 1.0f))
        {
            m_IsDirty = true;
            anyChanged = true;
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
                PushUndoCommand(m_PropertyEditSnapshot, "Edit Skill Node");
                m_IsEditingProperties = false;
            }
        }
    }

    void SkillTreeEditorPanel::DrawAbilityPayloadProperties(SkillTreeNode& node, bool& anyChanged)
    {
        auto& def = node.GrantedAbility;

        if (ImGui::InputText("Ability Name", &def.Name))
        {
            m_IsDirty = true;
            anyChanged = true;
        }

        std::string tagStr = def.AbilityTag.GetTagString();
        if (ImGui::InputText("Ability Tag", &tagStr))
        {
            def.AbilityTag = GameplayTag(std::move(tagStr));
            m_IsDirty = true;
            anyChanged = true;
        }

        if (ImGui::DragFloat("Cooldown", &def.CooldownDuration, 0.1f))
        {
            def.CooldownDuration = std::clamp(def.CooldownDuration, 0.0f, 600.0f);
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::DragFloat("Resource Cost", &def.ResourceCost, 0.1f))
        {
            def.ResourceCost = std::clamp(def.ResourceCost, 0.0f, 10000.0f);
            m_IsDirty = true;
            anyChanged = true;
        }
        if (ImGui::InputText("Cost Attribute", &def.CostAttribute))
        {
            m_IsDirty = true;
            anyChanged = true;
        }
    }

    void SkillTreeEditorPanel::DrawPassiveEffectPayloadProperties(SkillTreeNode& node, bool& anyChanged)
    {
        auto& effect = node.PassiveEffect;

        if (ImGui::InputText("Effect Name", &effect.Name))
        {
            m_IsDirty = true;
            anyChanged = true;
        }

        ImGui::Text("Modifiers:");
        for (sizet m = 0; m < effect.Modifiers.size(); ++m)
        {
            auto& mod = effect.Modifiers[m];
            ImGui::PushID(static_cast<int>(m));

            if (ImGui::InputText("Attribute", &mod.AttributeName))
            {
                m_IsDirty = true;
                anyChanged = true;
            }

            int op = static_cast<int>(mod.Op);
            const char* ops[] = { "Add", "Multiply", "Override" };
            if (ImGui::Combo("Op", &op, ops, 3))
            {
                mod.Op = static_cast<AttributeModifier::Operation>(op);
                m_IsDirty = true;
                anyChanged = true;
            }

            if (ImGui::DragFloat("Magnitude", &mod.Magnitude, 0.1f))
            {
                m_IsDirty = true;
                anyChanged = true;
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                effect.Modifiers.erase(effect.Modifiers.begin() + static_cast<ptrdiff_t>(m));
                m_IsDirty = true;
                anyChanged = true;
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }
        if (ImGui::SmallButton("Add Modifier"))
        {
            effect.Modifiers.emplace_back();
            m_IsDirty = true;
            anyChanged = true;
        }
    }

    // =========================================================================
    // Serialization
    // =========================================================================

    void SkillTreeEditorPanel::SaveTree()
    {
        if (!m_Tree)
        {
            return;
        }
        if (m_FilePath.empty())
        {
            SaveTreeAs();
            return;
        }

        m_Tree->RebuildIndex();
        if (std::string error; !m_Tree->Validate(&error))
        {
            SurfaceError("Cannot save - skill tree is invalid:\n" + error);
            return;
        }

        std::string yamlString = SkillTreeDatabaseSerializer().TestSerializeToYAML(m_Tree);

        std::ofstream fout(m_FilePath);
        if (!fout)
        {
            OLO_CORE_ERROR("SkillTreeEditorPanel - Failed to save: {}", m_FilePath.string());
            SurfaceError("Failed to open file for writing:\n" + m_FilePath.string());
            return;
        }
        fout << yamlString;
        fout.close();

        m_IsDirty = false;
        OLO_CORE_INFO("SkillTreeEditorPanel - Saved: {}", m_FilePath.string());
    }

    void SkillTreeEditorPanel::SaveTreeAs()
    {
        std::string filepath = FileDialogs::SaveFile(
            "Skill Tree (*.oloskilltree)\0*.oloskilltree\0"
            "All Files (*.*)\0*.*\0");
        if (filepath.empty())
        {
            return;
        }

        std::filesystem::path path(filepath);
        if (path.extension() != ".oloskilltree")
        {
            path += ".oloskilltree";
        }

        m_FilePath = path;
        m_CurrentAssetHandle = 0;
        SaveTree();
    }

    void SkillTreeEditorPanel::LoadTree(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("SkillTreeEditorPanel - File not found: {}", path.string());
            SurfaceError("File not found:\n" + path.string());
            return;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("SkillTreeEditorPanel - Failed to open file: {}", path.string());
            SurfaceError("Failed to open file:\n" + path.string());
            return;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        // Deserialize into a fresh detached working copy
        auto tree = Ref<SkillTreeDatabase>::Create();
        if (!SkillTreeDatabaseSerializer().TestDeserializeFromYAML(ss.str(), tree))
        {
            OLO_CORE_ERROR("SkillTreeEditorPanel - Failed to deserialize: {}", path.string());
            SurfaceError("Failed to load skill tree (invalid YAML or failed validation, see log):\n" + path.string());
            return;
        }

        m_Tree = tree;
        m_FilePath = path;
        m_CurrentAssetHandle = 0;
        m_IsDirty = false;
        m_SelectedNodeID.clear();
        m_NodeIDEditSource.clear();
        m_NodeIDError.clear();
        m_IsDraggingNode = false;
        m_IsCreatingConnection = false;
        m_ConnectionSourceNodeID.clear();
        m_IsEditingProperties = false;
        m_Zoom = 1.0f;
        m_ScrollOffset = { 0.0f, 0.0f };
        m_FrameAllRequested = true;

        OLO_CORE_INFO("SkillTreeEditorPanel - Loaded: {}", path.string());
    }

    // =========================================================================
    // Node operations
    // =========================================================================

    void SkillTreeEditorPanel::AddNode(const glm::vec2& position)
    {
        if (!m_Tree)
        {
            return;
        }

        auto oldSnapshot = CaptureSnapshot();

        SkillTreeNode node;
        node.NodeID = GenerateNodeID();
        node.DisplayName = "New Skill";
        node.EditorPosition = position;
        m_SelectedNodeID = node.NodeID;
        m_Tree->m_Nodes.push_back(std::move(node));
        m_Tree->RebuildIndex();
        m_IsDirty = true;

        PushUndoCommand(oldSnapshot, "Add Skill Node");
    }

    void SkillTreeEditorPanel::DeleteNode(const std::string& nodeId)
    {
        if (!m_Tree)
        {
            return;
        }
        i32 const idx = FindNodeIndex(nodeId);
        if (idx < 0)
        {
            return;
        }

        auto oldSnapshot = CaptureSnapshot();

        m_Tree->m_Nodes.erase(m_Tree->m_Nodes.begin() + idx);
        // Strip the deleted node from every remaining prerequisite list so no
        // dangling references survive
        for (auto& node : m_Tree->m_Nodes)
        {
            std::erase(node.Prerequisites, nodeId);
        }
        m_Tree->RebuildIndex();

        if (m_SelectedNodeID == nodeId)
        {
            m_SelectedNodeID.clear();
        }
        m_IsDirty = true;

        PushUndoCommand(oldSnapshot, "Delete Skill Node");
    }

    bool SkillTreeEditorPanel::TryAddPrerequisite(const std::string& sourceId, const std::string& targetId)
    {
        if (!m_Tree)
        {
            return false;
        }
        // Reject self edges
        if (sourceId == targetId)
        {
            return false;
        }
        SkillTreeNode* target = FindNodeMutable(targetId);
        if (!target || FindNodeIndex(sourceId) < 0)
        {
            return false;
        }
        // Reject duplicates
        if (std::ranges::find(target->Prerequisites, sourceId) != target->Prerequisites.end())
        {
            return false;
        }

        auto oldSnapshot = CaptureSnapshot();

        // Tentatively add, then validate — Validate() detects prerequisite
        // cycles; roll back and surface the error if the edge is rejected.
        target->Prerequisites.push_back(sourceId);
        if (std::string error; !m_Tree->Validate(&error))
        {
            target->Prerequisites.pop_back();
            SurfaceError("Cannot add prerequisite:\n" + error);
            return false;
        }

        m_IsDirty = true;
        PushUndoCommand(oldSnapshot, "Add Prerequisite");
        return true;
    }

    // =========================================================================
    // Coordinate transforms
    // =========================================================================

    ImVec2 SkillTreeEditorPanel::WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const
    {
        return ImVec2(
            canvasOrigin.x + (worldPos.x + m_ScrollOffset.x) * m_Zoom,
            canvasOrigin.y + (worldPos.y + m_ScrollOffset.y) * m_Zoom);
    }

    glm::vec2 SkillTreeEditorPanel::ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const
    {
        return glm::vec2(
            (screenPos.x - canvasOrigin.x) / m_Zoom - m_ScrollOffset.x,
            (screenPos.y - canvasOrigin.y) / m_Zoom - m_ScrollOffset.y);
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    SkillTreeNode* SkillTreeEditorPanel::FindNodeMutable(const std::string& nodeId)
    {
        if (!m_Tree || nodeId.empty())
        {
            return nullptr;
        }
        for (auto& node : m_Tree->m_Nodes)
        {
            if (node.NodeID == nodeId)
            {
                return &node;
            }
        }
        return nullptr;
    }

    i32 SkillTreeEditorPanel::FindNodeIndex(const std::string& nodeId) const
    {
        if (!m_Tree)
        {
            return -1;
        }
        auto const nodeCount = m_Tree->m_Nodes.size();
        for (sizet i = 0; i < nodeCount; ++i)
        {
            if (m_Tree->m_Nodes[i].NodeID == nodeId)
            {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    std::string SkillTreeEditorPanel::GenerateNodeID() const
    {
        sizet counter = m_Tree ? m_Tree->m_Nodes.size() + 1 : 1;
        std::string candidate = "node_" + std::to_string(counter);
        while (FindNodeIndex(candidate) != -1)
        {
            ++counter;
            candidate = "node_" + std::to_string(counter);
        }
        return candidate;
    }

    void SkillTreeEditorPanel::FrameAll(const ImVec2& canvasSize)
    {
        if (!m_Tree || m_Tree->m_Nodes.empty() || canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
        {
            return;
        }

        f32 minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (const auto& node : m_Tree->m_Nodes)
        {
            minX = std::min(minX, node.EditorPosition.x);
            minY = std::min(minY, node.EditorPosition.y);
            maxX = std::max(maxX, node.EditorPosition.x + s_NodeWidth);
            maxY = std::max(maxY, node.EditorPosition.y + s_NodeHeaderHeight + s_NodeBodyHeight);
        }

        f32 const w = maxX - minX + 100.0f;
        f32 const h = maxY - minY + 100.0f;
        m_Zoom = std::clamp(std::min(canvasSize.x / w, canvasSize.y / h), s_MinZoom, s_MaxZoom);
        m_ScrollOffset = glm::vec2(-(minX - 50.0f), -(minY - 50.0f));
    }

    void SkillTreeEditorPanel::SurfaceError(const std::string& message)
    {
        m_ErrorMessage = message;
        m_ShowErrorPopup = true;
    }

} // namespace OloEngine
