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
#include <cmath>
#include <fstream>
#include <limits>
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

        DrawCanvas(canvasWidth);

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
        m_Canvas.ResetView();
        m_IsDraggingNode = false;
        m_IsCreatingConnection = false;
        m_ConnectionSourceNodeID.clear();
        m_IsEditingProperties = false;
    }

    // =========================================================================
    // Canvas
    // =========================================================================

    void SkillTreeEditorPanel::DrawCanvas(f32 width)
    {
        // Begin() paints the background and grid, consumes pan/zoom and clips to
        // its own child region - everything this function used to do by hand.
        if (!m_Canvas.Begin("##SkillTreeCanvas", ImVec2(width, 0.0f)))
        {
            return;
        }

        // Serviced here rather than in the menu handler because FitToBounds needs
        // the canvas' size, and that is only known once Begin() has run.
        if (m_FrameAllRequested)
        {
            FrameAll();
            m_FrameAllRequested = false;
        }

        DrawEdges();
        DrawNodes();
        DrawConnectionInProgress();

        HandleCanvasInput();
        HandleNodeInteraction();
        HandleConnectionDrag();
        DrawContextMenu();

        m_Canvas.End();
    }

    ImVec2 SkillTreeEditorPanel::GetNodeSize() const
    {
        f32 const zoom = m_Canvas.GetZoom();
        return ImVec2(s_NodeWidth * zoom, (s_NodeHeaderHeight + s_NodeBodyHeight) * zoom);
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

    void SkillTreeEditorPanel::DrawNodes()
    {
        if (!m_Tree)
        {
            return;
        }
        for (const auto& node : m_Tree->m_Nodes)
        {
            DrawNode(node);
        }
    }

    void SkillTreeEditorPanel::DrawNode(const SkillTreeNode& node)
    {
        ImDrawList* drawList = m_Canvas.GetDrawList();
        f32 const zoom = m_Canvas.GetZoom();
        ImVec2 const nodePos = m_Canvas.ToScreen(node.EditorPosition);
        ImVec2 const nodeSize = GetNodeSize();
        ImVec2 const nodeEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y);
        ImVec2 const headerEnd = ImVec2(nodePos.x + nodeSize.x, nodePos.y + s_NodeHeaderHeight * zoom);

        bool const isSelected = (node.NodeID == m_SelectedNodeID);

        // Node body + header
        f32 const rounding = 6.0f * zoom;
        drawList->AddRectFilled(nodePos, nodeEnd, GetNodeColor(node.Payload), rounding);
        drawList->AddRectFilled(nodePos, headerEnd, GetNodeHeaderColor(node.Payload), rounding, ImDrawFlags_RoundCornersTop);

        // Selection outline
        if (isSelected)
        {
            drawList->AddRect(nodePos, nodeEnd, IM_COL32(255, 200, 50, 220), rounding, 0, 2.5f * zoom);
        }

        // Header text: display name (falls back to the id)
        f32 const fontSize = 13.0f * zoom;
        const std::string& headerText = node.DisplayName.empty() ? node.NodeID : node.DisplayName;
        ImVec2 const textPos = ImVec2(nodePos.x + s_NodePadding * zoom, nodePos.y + 5.0f * zoom);
        drawList->AddText(nullptr, fontSize, textPos, IM_COL32(255, 255, 255, 240), headerText.c_str());

        // Body line 1: level requirement / cost
        f32 contentY = nodePos.y + (s_NodeHeaderHeight + 4.0f) * zoom;
        std::string costLine = "Lv " + std::to_string(node.LevelRequirement) + " / cost " + std::to_string(node.SkillPointCost);
        drawList->AddText(nullptr, 11.0f * zoom,
                          ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                          IM_COL32(200, 200, 200, 220), costLine.c_str());
        contentY += 15.0f * zoom;

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
        drawList->AddText(nullptr, 11.0f * zoom,
                          ImVec2(nodePos.x + s_NodePadding * zoom, contentY),
                          payloadColor, payloadLine.c_str());

        // Ports: input (left, prerequisites arrive here), output (right,
        // dependents leave from here)
        f32 const radius = s_NodePortRadius * zoom;
        ImU32 const portFill = IM_COL32(40, 40, 45, 255);
        ImVec2 const inPort = GetInputPortPos(nodePos);
        ImVec2 const outPort = GetOutputPortPos(nodePos);
        drawList->AddCircleFilled(inPort, radius, portFill);
        drawList->AddCircle(inPort, radius, IM_COL32(100, 150, 255, 220), 12, 2.0f * zoom);
        drawList->AddCircleFilled(outPort, radius, portFill);
        drawList->AddCircle(outPort, radius, IM_COL32(100, 200, 100, 220), 12, 2.0f * zoom);
    }

    void SkillTreeEditorPanel::DrawEdges()
    {
        if (!m_Tree)
        {
            return;
        }

        for (const auto& dependent : m_Tree->m_Nodes)
        {
            ImVec2 const endPos = GetInputPortPos(m_Canvas.ToScreen(dependent.EditorPosition));

            for (const auto& prereqId : dependent.Prerequisites)
            {
                i32 const prereqIndex = FindNodeIndex(prereqId);
                if (prereqIndex < 0)
                {
                    continue;
                }
                const SkillTreeNode& prereq = m_Tree->m_Nodes[static_cast<sizet>(prereqIndex)];
                ImVec2 const startPos = GetOutputPortPos(m_Canvas.ToScreen(prereq.EditorPosition));

                // Prerequisite edges are directional - which end is the
                // prerequisite is the whole meaning of the edge - so they get the
                // widget's arrowed wire. Thickness is in SCREEN pixels,
                // deliberately not scaled by zoom: a hairline at the canvas'
                // minimum zoom is invisible. See GraphCanvas::DrawWire.
                m_Canvas.DrawDirectionalWire(startPos, endPos, IM_COL32(180, 180, 200, 200), 2.0f);
            }
        }
    }

    void SkillTreeEditorPanel::DrawConnectionInProgress()
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

        ImVec2 const srcNodePos = m_Canvas.ToScreen(m_Tree->m_Nodes[static_cast<sizet>(srcIndex)].EditorPosition);
        m_Canvas.DrawWire(GetOutputPortPos(srcNodePos), m_ConnectionEndPos, IM_COL32(255, 255, 100, 180), 2.0f);
    }

    // =========================================================================
    // Interaction
    // =========================================================================

    std::string SkillTreeEditorPanel::HitTestNode(const ImVec2& screenPos) const
    {
        if (!m_Tree)
        {
            return {};
        }

        ImVec2 const nodeSize = GetNodeSize();
        // Reverse iterate so nodes drawn last (topmost) are hit first
        for (auto it = m_Tree->m_Nodes.rbegin(); it != m_Tree->m_Nodes.rend(); ++it)
        {
            ImVec2 const nodePos = m_Canvas.ToScreen(it->EditorPosition);
            if (screenPos.x >= nodePos.x && screenPos.x <= nodePos.x + nodeSize.x &&
                screenPos.y >= nodePos.y && screenPos.y <= nodePos.y + nodeSize.y)
            {
                return it->NodeID;
            }
        }
        return {};
    }

    void SkillTreeEditorPanel::HandleCanvasInput()
    {
        // Zoom and pan were handled here by hand; GraphCanvas::Begin has already
        // consumed both by the time this runs.
        bool const isHovered = m_Canvas.IsHovered();

        // Right-CLICK, not right-press and not any right-release: the canvas pans
        // on right-DRAG, and only it owns the threshold that tells the two
        // gestures apart.
        if (m_Canvas.WasRightClicked() && !m_IsCreatingConnection && !m_SuppressNextContextMenu)
        {
            ImVec2 const mousePos = ImGui::GetIO().MousePos;
            // Captured in GRAPH space: storing the screen position and converting
            // it when the popup is drawn places a new node wrongly whenever the
            // view moves in between.
            m_ContextMenuGraphPos = m_Canvas.ToGraph(mousePos);
            m_ContextMenuNodeID = HitTestNode(mousePos);
            m_ShowContextMenu = true;
        }
        // Reset on ANY right release, not just a hovered one, so a gesture that
        // ends outside the canvas can't suppress the next real click.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            m_SuppressNextContextMenu = false;
        }

        // Click on empty space deselects. Suppressed while panning, or dragging
        // the background would also clear the selection.
        if (isHovered && !m_Canvas.IsPanning() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDraggingNode && !m_IsCreatingConnection)
        {
            if (HitTestNode(ImGui::GetIO().MousePos).empty())
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

    void SkillTreeEditorPanel::HandleNodeInteraction()
    {
        if (!m_Tree || m_IsCreatingConnection)
        {
            return;
        }

        ImVec2 const mousePos = ImGui::GetIO().MousePos;

        // Only NEW presses are suppressed while the canvas is panning. The drag
        // and release paths below must keep running, or a pan latched mid-drag
        // (a second button going down) would strand m_IsDraggingNode with a stale
        // offset and never push its undo command.
        if (m_Canvas.IsHovered() && !m_Canvas.IsPanning() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            // Output-port hit starts a prerequisite connection drag (this node
            // becomes the prerequisite of whatever node the drag is released on).
            // The hit radius has a screen-pixel floor because a port that scales
            // with zoom is untargetable once zoomed out.
            f32 const portHitRadius = std::max(s_PortHitRadiusMin, m_Canvas.Scaled(s_NodePortRadius * 2.0f));
            bool startedConnection = false;
            for (auto it = m_Tree->m_Nodes.rbegin(); it != m_Tree->m_Nodes.rend(); ++it)
            {
                ImVec2 const outPort = GetOutputPortPos(m_Canvas.ToScreen(it->EditorPosition));
                f32 const dist = std::hypot(mousePos.x - outPort.x, mousePos.y - outPort.y);
                if (dist <= portHitRadius)
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
                if (std::string hit = HitTestNode(mousePos); !hit.empty())
                {
                    m_SelectedNodeID = hit;
                    m_IsDraggingNode = true;
                    if (i32 idx = FindNodeIndex(hit); idx >= 0)
                    {
                        ImVec2 const nodePos = m_Canvas.ToScreen(m_Tree->m_Nodes[static_cast<sizet>(idx)].EditorPosition);
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
                node->EditorPosition = m_Canvas.ToGraph(targetScreen);
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

    void SkillTreeEditorPanel::HandleConnectionDrag()
    {
        if (!m_IsCreatingConnection)
        {
            return;
        }

        m_ConnectionEndPos = ImGui::GetIO().MousePos;

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (std::string target = HitTestNode(m_ConnectionEndPos); !target.empty())
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

    void SkillTreeEditorPanel::DrawContextMenu()
    {
        if (m_ShowContextMenu)
        {
            ImGui::OpenPopup("##SkillTreeContextMenu");
            m_ShowContextMenu = false;
        }

        if (ImGui::BeginPopup("##SkillTreeContextMenu"))
        {
            glm::vec2 const worldPos = m_ContextMenuGraphPos;

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
                    m_ConnectionEndPos = m_Canvas.ToScreen(m_ContextMenuGraphPos);
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
                if (ImGui::MenuItem("Reset View"))
                {
                    m_Canvas.ResetView();
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
        m_Canvas.ResetView();
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

    void SkillTreeEditorPanel::FrameAll()
    {
        if (!m_Tree || m_Tree->m_Nodes.empty())
        {
            m_Canvas.ResetView();
            return;
        }

        glm::vec2 min(std::numeric_limits<f32>::max());
        glm::vec2 max(std::numeric_limits<f32>::lowest());
        for (const auto& node : m_Tree->m_Nodes)
        {
            min = glm::min(min, node.EditorPosition);
            max = glm::max(max, node.EditorPosition + glm::vec2(s_NodeWidth, s_NodeHeaderHeight + s_NodeBodyHeight));
        }
        m_Canvas.FitToBounds(min, max);
    }

    void SkillTreeEditorPanel::SurfaceError(const std::string& message)
    {
        m_ErrorMessage = message;
        m_ShowErrorPopup = true;
    }

} // namespace OloEngine
