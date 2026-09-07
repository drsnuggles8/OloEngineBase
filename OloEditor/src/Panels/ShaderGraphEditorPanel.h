#pragma once

#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCommand.h"
#include "OloEngine/Asset/Asset.h"
#include "Panels/Graph/GraphCanvas.h"

#include <glm/glm.hpp>
#include <imgui.h>
#include <filesystem>
#include <optional>

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
        /// Returns true if saved or no changes needed, false if the user cancelled or save failed
        [[nodiscard]] bool SaveIfNeeded()
        {
            if (!m_IsDirty)
                return true;
            SaveShaderGraph();
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

        // Undo/Redo (public for EditorLayer keyboard shortcuts)
        void Undo();
        void Redo();

      private:
        // Canvas
        void DrawCanvas(f32 width);
        void DrawNodes();
        void DrawConnections();
        void DrawConnectionInProgress();

        // Node rendering
        void DrawNode(ShaderGraphNode& node);
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

        enum class PinDirectionFilter
        {
            Any,
            InputsOnly,
            OutputsOnly
        };
        /// The pin NEAREST `screenPos` within the hit radius, or nullptr. Nearest
        /// rather than first-found because the radius has a screen-pixel floor,
        /// so adjacent pins' hit circles overlap once zoomed out far enough.
        /// The result points at a member scratch slot and is invalidated by the
        /// next call.
        [[nodiscard]] const PinInfo* HitTestPin(ImVec2 screenPos, PinDirectionFilter filter = PinDirectionFilter::Any) const;

        /// A link resolved to the two SCREEN points its wire runs between.
        struct WireEndpoints
        {
            ImVec2 Source{};
            ImVec2 Target{};
            ShaderGraphPinType SourceType = ShaderGraphPinType::Float;
        };
        /// False when either end names a pin that no longer exists, in which case
        /// `out` is untouched and the wire is neither drawn nor clickable — the
        /// two must agree, which is why they share this lookup.
        [[nodiscard]] bool GetLinkEndpoints(const ShaderGraphLink& link, WireEndpoints& out) const;

        // Interaction
        /// Returns true when the click was consumed here (a wire was cut), so
        /// the caller skips node hit-testing for this frame.
        bool HandleCanvasInput();
        void HandleNodeInteraction();
        void HandleConnectionDrag();
        /// The link whose wire passes nearest `screenPos`, or 0 if none is close
        /// enough to have been aimed at. Uses the canvas' own bezier sampling so
        /// the hit curve is the curve that was drawn.
        [[nodiscard]] UUID HitTestLink(ImVec2 screenPos) const;

        // Toolbar & property panel
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(ShaderGraphNode& node);
        void DrawPreviewPanel();

        // Context menu
        void DrawContextMenu();

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

        // Canvas view: pan, zoom, grid, the screen<->graph transforms and wire
        // drawing/hit-testing all live in the shared widget, not here.
        EditorUI::GraphCanvas m_Canvas;

        // Selection
        UUID m_SelectedNodeID = 0;

        /// Storage behind the pointer HitTestPin returns; not state, just a slot
        /// that outlives the call.
        mutable std::optional<PinInfo> m_HitPin;

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
        // Captured in GRAPH space at click time. Storing the screen position and
        // converting it when the popup is drawn placed the new node wrongly
        // whenever the view moved between the two.
        glm::vec2 m_ContextMenuGraphPos = {};
        char m_NodeSearchFilter[128] = {};

        // Compile preview
        ShaderGraphCompileResult m_LastCompileResult;
        bool m_AutoCompile = false;

        // Undo/Redo
        ShaderGraphCommandHistory m_CommandHistory;

        // Pending edit tracking for deferred undo commands (ImGui interactive widgets)
        std::string m_PendingStringOldValue;
        glm::ivec3 m_PendingWorkgroupOldValue{};
        int m_PendingBufferBindingOldValue = 0;
        ShaderGraphPinValue m_PendingPinOldValue;

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
        static constexpr f32 s_NodeWidth = 180.0f;
        static constexpr f32 s_PinRadius = 5.0f;
        static constexpr f32 s_PinSpacing = 22.0f;
        static constexpr f32 s_HeaderHeight = 26.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
        /// Screen pixels. A pin hit box that scales with zoom shrinks to 1.5px at
        /// the canvas' minimum zoom, which no mouse can hit; a floor keeps every
        /// pin reachable at every zoom.
        static constexpr f32 s_PinHitRadiusMin = 9.0f;
        /// Screen pixels from a wire that still counts as clicking it.
        static constexpr f32 s_WireHitDistance = 8.0f;
    };

} // namespace OloEngine
