#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "Panels/Graph/GraphCanvas.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace OloEngine
{
    class CommandHistory;

    class DialogueEditorPanel
    {
      public:
        DialogueEditorPanel() = default;
        ~DialogueEditorPanel() = default;

        void OnImGuiRender();

        // Open a dialogue tree for editing
        void OpenDialogue(const std::filesystem::path& path);
        void OpenDialogue(AssetHandle handle);

        // Check if panel has unsaved changes
        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_IsDirty;
        }
        [[nodiscard]] bool IsOpen() const
        {
            return m_IsOpen;
        }
        [[nodiscard]] bool IsFocused() const
        {
            return m_IsFocused;
        }

        // Reset the panel to a clean state (e.g. on project switch)
        void NewDialogue();

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

        // Restore dialogue state from an undo snapshot
        void RestoreSnapshot(const DialogueEditorSnapshot& snapshot);

      private:
        // --- Canvas rendering ---
        void DrawCanvas(f32 width);
        void DrawNodes();
        void DrawConnections();
        void DrawConnectionInProgress();
        void DrawMinimap();

        // --- Node rendering ---
        void DrawNode(DialogueNodeData& node);
        ImVec2 GetNodeSize(const DialogueNodeData& node) const;
        ImU32 GetNodeColor(const std::string& type) const;
        ImU32 GetNodeHeaderColor(const std::string& type) const;
        // --- Port helpers ---
        struct PortInfo
        {
            ImVec2 Position; // Screen-space
            UUID NodeID;
            std::string Name;
            bool IsOutput;
        };
        std::vector<PortInfo> GetNodePorts(const DialogueNodeData& node, const ImVec2& nodeScreenPos) const;

        // --- Interaction ---
        void HandleCanvasInput();
        /// Panel-level keys (Delete, Ctrl+S, Ctrl+N). Runs outside the canvas so
        /// they keep working on a frame the canvas child is clipped away.
        void HandleShortcuts();
        /// Ends any gesture still in flight. Called when the canvas is not drawn
        /// this frame, because then there is no release to observe.
        void CancelInteractions();
        void HandleNodeInteraction();
        void HandleConnectionDrag();
        /// The node under `screenPos`, topmost first, or nullptr.
        [[nodiscard]] const DialogueNodeData* HitTestNode(ImVec2 screenPos) const;

        // --- Toolbar & property panel ---
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(DialogueNodeData& node);

        // --- Preview / playtest ---
        void DrawPreviewPanel();
        void PreviewAdvance(u32 hopCount = 0);
        void PreviewSelectChoice(i32 index);
        void PreviewReset();

        // --- Context menu ---
        void DrawContextMenu();

        // --- Serialization ---
        void SaveDialogue();
        void SaveDialogueAs();
        void LoadDialogue(const std::filesystem::path& path);

        // --- Node operations ---
        UUID CreateNode(const std::string& type, const glm::vec2& position);
        void DeleteNode(UUID nodeID);
        void DeleteConnection(size_t index);
        void DuplicateNode(UUID nodeID);

        // --- Helpers ---
        /// Fits every node into the viewport. Needs the canvas' size, so it is
        /// only meaningful once Begin() has run at least once.
        void FrameAll();
        DialogueNodeData* FindNodeMutable(UUID nodeID);
        UUID GenerateNodeID();
        std::string ResolveSourcePort(const std::string& portName, UUID sourceNodeID);

      private:
        bool m_IsOpen = true;
        /// Whether this panel (or a child of it) has keyboard focus. The panel's
        /// shortcuts are global ImGui::IsKeyPressed reads, so without this a
        /// background Dialogue Editor answers a Ctrl+S meant for another panel.
        bool m_IsFocused = false;

        // Asset state
        std::filesystem::path m_CurrentFilePath;
        AssetHandle m_CurrentAssetHandle = 0;
        std::vector<DialogueNodeData> m_Nodes;
        std::vector<DialogueConnection> m_Connections;
        UUID m_RootNodeID = 0;
        bool m_IsDirty = false;

        // Undo/redo
        CommandHistory* m_CommandHistory = nullptr;
        bool m_IsEditingProperties = false;
        DialogueEditorSnapshot m_PropertyEditSnapshot;
        void PushDialogueUndoCommand(const DialogueEditorSnapshot& oldState, const std::string& description);
        [[nodiscard]] DialogueEditorSnapshot CaptureSnapshot() const;

        // Canvas view: pan, zoom, the grid, the screen<->graph transforms and
        // wire drawing all live in the shared widget, not here.
        EditorUI::GraphCanvas m_Canvas;

        // Selection
        UUID m_SelectedNodeID = 0;
        std::vector<UUID> m_MultiSelectedNodes;
        bool m_IsDraggingNode = false;
        glm::vec2 m_DragStartOffset = { 0.0f, 0.0f };
        DialogueEditorSnapshot m_DragStartSnapshot;

        // Connection creation
        bool m_IsCreatingConnection = false;
        UUID m_ConnectionStartNodeID = 0;
        std::string m_ConnectionStartPort;
        bool m_ConnectionStartIsOutput = false;
        ImVec2 m_ConnectionEndPos = { 0.0f, 0.0f };
        /// A right-press cancels a connection drag; its RELEASE would otherwise
        /// read as a plain right-click and pop the context menu on top of it.
        bool m_SuppressNextContextMenu = false;

        // Context menu
        bool m_ShowContextMenu = false;
        // Captured in GRAPH space at click time. Storing the screen position and
        // converting it when the popup is drawn placed a new node wrongly
        // whenever the view moved between the two.
        glm::vec2 m_ContextMenuGraphPos = { 0.0f, 0.0f };

        // Preview state
        bool m_ShowPreview = false;
        UUID m_PreviewCurrentNodeID = 0;
        std::string m_PreviewCurrentText;
        std::string m_PreviewCurrentSpeaker;
        std::vector<DialogueChoice> m_PreviewChoices;
        DialogueVariables m_PreviewVariables;
        bool m_PreviewActive = false;

        // Node ID counter
        u64 m_NextNodeID = 1000;

        // Layout constants
        static constexpr f32 s_NodeWidth = 220.0f;
        static constexpr f32 s_NodeHeaderHeight = 28.0f;
        static constexpr f32 s_NodePortRadius = 6.0f;
        static constexpr f32 s_NodePortSpacing = 22.0f;
        static constexpr f32 s_NodePadding = 8.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
        static constexpr f32 s_MinimapSize = 150.0f;
        /// Screen pixels. A port hit box that scales with zoom shrinks to under
        /// two pixels at the canvas' minimum zoom, which no mouse can hit.
        static constexpr f32 s_PortHitRadiusMin = 10.0f;
    };

} // namespace OloEngine
