#pragma once

#include "../UndoRedo/SpecializedCommands.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"
#include "Panels/Graph/GraphCanvas.h"

#include <imgui.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <string>

namespace OloEngine
{
    class CommandHistory;

    // Node-canvas editor for SkillTreeDatabase assets (.oloskilltree, issue
    // #635). Works on a detached copy deserialized straight from the file (via
    // SkillTreeDatabaseSerializer's YAML test hooks), so edits never touch the
    // AssetManager-cached asset until the file is saved and hot-reloaded.
    // Architecture cloned from DialogueEditorPanel.
    class SkillTreeEditorPanel
    {
      public:
        SkillTreeEditorPanel() = default;
        ~SkillTreeEditorPanel() = default;

        void OnImGuiRender();

        // Open a skill tree for editing
        void OpenSkillTree(const std::filesystem::path& path);
        void OpenSkillTree(AssetHandle handle);

        // Reset the panel to a fresh single-node tree (e.g. on project switch)
        void NewTree();

        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_IsDirty;
        }
        /// Returns true if saved or no changes needed, false if the user cancelled or save failed
        [[nodiscard]] bool SaveIfNeeded()
        {
            if (!m_IsDirty)
            {
                return true;
            }
            SaveTree();
            return !m_IsDirty;
        }
        [[nodiscard]] bool IsOpen() const
        {
            return m_IsOpen;
        }
        [[nodiscard]] bool IsFocused() const
        {
            return m_IsFocused;
        }
        void SetOpen(bool open)
        {
            m_IsOpen = open;
        }

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

        // Restore tree state from an undo snapshot
        void RestoreSnapshot(const SkillTreeEditorSnapshot& snapshot);

      private:
        // --- Canvas rendering ---
        void DrawCanvas(f32 width);
        void DrawNodes();
        void DrawNode(const SkillTreeNode& node);
        void DrawEdges();
        void DrawConnectionInProgress();
        [[nodiscard]] ImVec2 GetNodeSize() const;
        [[nodiscard]] ImVec2 GetInputPortPos(const ImVec2& nodeScreenPos) const;
        [[nodiscard]] ImVec2 GetOutputPortPos(const ImVec2& nodeScreenPos) const;
        [[nodiscard]] ImU32 GetNodeColor(SkillTreeNode::PayloadKind payload) const;
        [[nodiscard]] ImU32 GetNodeHeaderColor(SkillTreeNode::PayloadKind payload) const;

        // --- Interaction ---
        void HandleCanvasInput();
        void HandleNodeInteraction();
        void HandleConnectionDrag();
        void DrawContextMenu();
        [[nodiscard]] std::string HitTestNode(const ImVec2& screenPos) const;

        // --- Toolbar & property panel ---
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(SkillTreeNode& node);
        void DrawTreeProperties();
        void DrawAbilityPayloadProperties(SkillTreeNode& node, bool& anyChanged);
        void DrawPassiveEffectPayloadProperties(SkillTreeNode& node, bool& anyChanged);

        // --- Serialization ---
        void SaveTree();
        void SaveTreeAs();
        void LoadTree(const std::filesystem::path& path);

        // --- Node operations ---
        void AddNode(const glm::vec2& position);
        void DeleteNode(const std::string& nodeId);
        // Adds sourceId into target's Prerequisites. Rejects self edges,
        // duplicates, and edges that would create a cycle (tentative add +
        // Validate + rollback). Returns true when the edge was added.
        bool TryAddPrerequisite(const std::string& sourceId, const std::string& targetId);

        // --- Helpers ---
        [[nodiscard]] SkillTreeNode* FindNodeMutable(const std::string& nodeId);
        [[nodiscard]] i32 FindNodeIndex(const std::string& nodeId) const;
        [[nodiscard]] std::string GenerateNodeID() const;
        void FrameAll();
        void SurfaceError(const std::string& message);

        // --- Undo ---
        [[nodiscard]] SkillTreeEditorSnapshot CaptureSnapshot() const;
        void PushUndoCommand(const SkillTreeEditorSnapshot& oldState, const std::string& description);

      private:
        bool m_IsOpen = true;
        bool m_IsFocused = false;

        // Asset state (detached working copy)
        Ref<SkillTreeDatabase> m_Tree;
        std::filesystem::path m_FilePath;
        AssetHandle m_CurrentAssetHandle = 0;
        bool m_IsDirty = false;

        // Undo/redo
        CommandHistory* m_CommandHistory = nullptr;
        bool m_IsEditingProperties = false;
        SkillTreeEditorSnapshot m_PropertyEditSnapshot;
        SkillTreeEditorSnapshot m_DragStartSnapshot;

        // Canvas view: pan, zoom, the grid, the screen<->graph transforms and
        // wire drawing all live in the shared widget, not here.
        EditorUI::GraphCanvas m_Canvas;
        // Framing needs the canvas' size, which only exists once Begin() has run,
        // so the menu item requests it and DrawCanvas services it.
        bool m_FrameAllRequested = false;

        // Selection / node drag
        std::string m_SelectedNodeID;
        bool m_IsDraggingNode = false;
        glm::vec2 m_DragStartOffset{ 0.0f, 0.0f };

        // Prerequisite connection drag (source = the prerequisite node)
        bool m_IsCreatingConnection = false;
        std::string m_ConnectionSourceNodeID;
        ImVec2 m_ConnectionEndPos{ 0.0f, 0.0f };
        bool m_SuppressNextContextMenu = false;

        // Context menu
        bool m_ShowContextMenu = false;
        // Captured in GRAPH space at click time. Storing the screen position and
        // converting it when the popup is drawn placed a new node wrongly
        // whenever the view moved between the two.
        glm::vec2 m_ContextMenuGraphPos{ 0.0f, 0.0f };
        std::string m_ContextMenuNodeID; // empty = canvas background

        // Error popup (save validation failures, cycle rejections, load errors)
        bool m_ShowErrorPopup = false;
        std::string m_ErrorMessage;

        // NodeID rename staging (committed on deactivate so partial typing
        // can't collide with another node's id)
        std::string m_NodeIDEditSource;
        std::string m_NodeIDEditBuffer;
        std::string m_NodeIDError;

        // Layout constants
        static constexpr f32 s_NodeWidth = 200.0f;
        static constexpr f32 s_NodeHeaderHeight = 26.0f;
        static constexpr f32 s_NodeBodyHeight = 46.0f;
        static constexpr f32 s_NodePortRadius = 6.0f;
        static constexpr f32 s_NodePadding = 8.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
        /// Screen pixels. A port hit box that scales with zoom shrinks to under
        /// two pixels at the canvas' minimum zoom, which no mouse can hit.
        static constexpr f32 s_PortHitRadiusMin = 10.0f;
    };

} // namespace OloEngine
