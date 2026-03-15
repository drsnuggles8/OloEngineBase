#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace OloEngine
{
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
        [[nodiscard]] bool HasUnsavedChanges() const { return m_IsDirty; }
        [[nodiscard]] bool IsOpen() const { return m_IsOpen; }

    private:
        // --- Canvas rendering ---
        void DrawCanvas();
        void DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawMinimap(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize);

        // --- Node rendering ---
        void DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, DialogueNodeData& node);
        ImVec2 GetNodeSize(const DialogueNodeData& node) const;
        ImU32 GetNodeColor(const std::string& type) const;
        ImU32 GetNodeHeaderColor(const std::string& type) const;

        // --- Port helpers ---
        struct PortInfo
        {
            ImVec2 Position;    // Screen-space
            UUID NodeID;
            std::string Name;
            bool IsOutput;
        };
        std::vector<PortInfo> GetNodePorts(const DialogueNodeData& node, const ImVec2& nodeScreenPos) const;

        // --- Interaction ---
        void HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void HandleNodeInteraction(const ImVec2& canvasOrigin);
        void HandleConnectionDrag(const ImVec2& canvasOrigin);

        // --- Toolbar & property panel ---
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(DialogueNodeData& node);

        // --- Preview / playtest ---
        void DrawPreviewPanel();
        void PreviewAdvance();
        void PreviewSelectChoice(i32 index);
        void PreviewReset();

        // --- Context menu ---
        void DrawContextMenu(const ImVec2& canvasOrigin);

        // --- Serialization ---
        void SaveDialogue();
        void LoadDialogue(const std::filesystem::path& path);
        void NewDialogue();

        // --- Node operations ---
        UUID CreateNode(const std::string& type, const glm::vec2& position);
        void DeleteNode(UUID nodeID);
        void DeleteConnection(size_t index);
        void DuplicateNode(UUID nodeID);

        // --- Coordinate transforms ---
        ImVec2 WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const;
        glm::vec2 ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const;

        // --- Helpers ---
        DialogueNodeData* FindNodeMutable(UUID nodeID);
        UUID GenerateNodeID();

    private:
        bool m_IsOpen = true;

        // Asset state
        std::filesystem::path m_CurrentFilePath;
        AssetHandle m_CurrentAssetHandle = 0;
        std::vector<DialogueNodeData> m_Nodes;
        std::vector<DialogueConnection> m_Connections;
        UUID m_RootNodeID = 0;
        bool m_IsDirty = false;

        // Canvas state
        glm::vec2 m_ScrollOffset = { 0.0f, 0.0f };
        f32 m_Zoom = 1.0f;
        bool m_IsPanning = false;

        // Selection
        UUID m_SelectedNodeID = 0;
        std::vector<UUID> m_MultiSelectedNodes;
        bool m_IsDraggingNode = false;
        glm::vec2 m_DragStartOffset = { 0.0f, 0.0f };

        // Connection creation
        bool m_IsCreatingConnection = false;
        UUID m_ConnectionStartNodeID = 0;
        std::string m_ConnectionStartPort;
        bool m_ConnectionStartIsOutput = false;
        ImVec2 m_ConnectionEndPos = { 0.0f, 0.0f };

        // Context menu
        bool m_ShowContextMenu = false;
        ImVec2 m_ContextMenuPos = { 0.0f, 0.0f };

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
        static constexpr f32 s_GridSize = 32.0f;
        static constexpr f32 s_MinZoom = 0.25f;
        static constexpr f32 s_MaxZoom = 2.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
        static constexpr f32 s_MinimapSize = 150.0f;
    };

} // namespace OloEngine
