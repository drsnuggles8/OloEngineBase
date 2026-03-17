#pragma once

#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCommand.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/glm.hpp>
#include <imgui.h>
#include <filesystem>

namespace OloEngine
{
    class ShaderGraphEditorPanel
    {
      public:
        ShaderGraphEditorPanel() = default;
        ~ShaderGraphEditorPanel() = default;

        void OnImGuiRender();

        void OpenShaderGraph(const std::filesystem::path& path);
        void OpenShaderGraph(AssetHandle handle);
        void NewShaderGraph();

        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_IsDirty;
        }
        void SaveIfNeeded()
        {
            if (m_IsDirty)
                SaveShaderGraph();
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

        // Undo/Redo (public for EditorLayer keyboard shortcuts)
        void Undo();
        void Redo();

      private:
        // Canvas
        void DrawCanvas();
        void DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin);

        // Node rendering
        void DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, ShaderGraphNode& node);
        ImVec2 GetNodeSize(const ShaderGraphNode& node) const;
        ImU32 GetNodeColor(ShaderGraphNodeCategory category) const;
        ImU32 GetNodeHeaderColor(ShaderGraphNodeCategory category) const;
        ImU32 GetPinColor(ShaderGraphPinType type) const;

        // Pin helpers
        struct PinInfo
        {
            ImVec2 Position;
            UUID PinID;
            UUID NodeID;
            std::string Name;
            ShaderGraphPinType Type;
            bool IsOutput;
        };
        std::vector<PinInfo> GetNodePins(const ShaderGraphNode& node, const ImVec2& nodeScreenPos) const;

        // Interaction
        void HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void HandleNodeInteraction(const ImVec2& canvasOrigin);
        void HandleConnectionDrag(const ImVec2& canvasOrigin);

        // Toolbar & property panel
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(ShaderGraphNode& node);
        void DrawPreviewPanel();

        // Context menu
        void DrawContextMenu(const ImVec2& canvasOrigin);

        // Serialization
        void SaveShaderGraph();
        void SaveShaderGraphAs();
        void LoadShaderGraph(const std::filesystem::path& path);
        void PerformPendingLoad();

        // Node operations
        UUID CreateNode(const std::string& typeName, const glm::vec2& position);
        void DeleteNode(UUID nodeID);
        void DeleteLink(UUID linkID);

        // Copy/Paste
        void CopySelectedNode();
        void PasteNodes(const glm::vec2& position);

        // Keyboard shortcuts
        void HandleKeyboardShortcuts();

        // Auto-layout
        void AutoLayoutNodes();

        // Coordinate transforms
        ImVec2 WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const;
        glm::vec2 ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const;

      private:
        bool m_IsOpen = true;
        bool m_IsFocused = false;

        // Asset state
        std::filesystem::path m_CurrentFilePath;
        AssetHandle m_CurrentAssetHandle = 0;
        Ref<ShaderGraphAsset> m_GraphAsset;
        bool m_IsDirty = false;

        // Deferred loading state
        std::filesystem::path m_PendingLoadPath;
        AssetHandle m_PendingLoadHandle = 0;
        int m_PendingLoadFrameDelay = 0;

        // Canvas state
        glm::vec2 m_ScrollOffset = { 0.0f, 0.0f };
        f32 m_Zoom = 1.0f;
        bool m_IsPanning = false;

        // Selection
        UUID m_SelectedNodeID = 0;

        // Connection dragging
        bool m_IsDraggingConnection = false;
        UUID m_DragStartPinID = 0;
        bool m_DragStartIsOutput = false;
        ImVec2 m_DragEndPos = {};

        // Node dragging
        bool m_IsDraggingNode = false;
        UUID m_DragNodeID = 0;
        glm::vec2 m_DragNodeStartPos = {};
        ImVec2 m_DragMouseStartPos = {};

        // Context menu
        bool m_ShowContextMenu = false;
        ImVec2 m_ContextMenuPos = {};
        char m_NodeSearchFilter[128] = {};

        // Compile preview
        ShaderGraphCompileResult m_LastCompileResult;
        bool m_AutoCompile = false;

        // Undo/Redo
        ShaderGraphCommandHistory m_CommandHistory;

        // Copy/Paste
        std::string m_CopiedNodeTypeName;
        std::string m_CopiedParameterName;
        std::string m_CopiedCustomFunctionBody;
        glm::ivec3 m_CopiedWorkgroupSize{ 16, 16, 1 };
        int m_CopiedBufferBinding = 0;
        std::vector<ShaderGraphPin> m_CopiedInputs;
        std::vector<ShaderGraphPin> m_CopiedOutputs;
        bool m_HasCopiedNode = false;

        // Layout constants
        static constexpr f32 s_GridSize = 32.0f;
        static constexpr f32 s_NodeWidth = 180.0f;
        static constexpr f32 s_PinRadius = 5.0f;
        static constexpr f32 s_PinSpacing = 22.0f;
        static constexpr f32 s_HeaderHeight = 26.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
    };

} // namespace OloEngine
