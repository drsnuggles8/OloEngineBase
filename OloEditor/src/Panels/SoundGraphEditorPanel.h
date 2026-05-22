#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
    // Forward declaration so Ref<SoundGraphSound> compiles without dragging miniaudio into
    // the editor header. The .cpp pulls SoundGraphSound.h for the full definition.
    class SoundGraphSound;
} // namespace OloEngine::Audio::SoundGraph

namespace OloEngine
{
    class CommandHistory;

    // Visual node-graph editor for SoundGraph assets. Mirrors the structural shape of
    // ShaderGraphEditorPanel / DialogueEditorPanel: pan/zoom canvas drawn with ImDrawList,
    // nodes drawn as rectangles with input/output pins, bezier wires between pins, a
    // right-click palette for adding new nodes, and a right-hand property sidebar that
    // edits the selected node's metadata.
    class SoundGraphEditorPanel
    {
      public:
        SoundGraphEditorPanel() = default;
        ~SoundGraphEditorPanel() = default;

        void OnImGuiRender();

        // Open a SoundGraph asset for editing. Loading is deferred to the next frame so
        // ImGui state from the previous draw doesn't reference an old graph mid-frame.
        void OpenSoundGraph(const std::filesystem::path& path);
        void OpenSoundGraph(AssetHandle handle);
        void NewSoundGraph();

        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_IsDirty;
        }
        [[nodiscard]] bool SaveIfNeeded()
        {
            if (!m_IsDirty)
                return true;
            SaveSoundGraph();
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

        // Called by EditorLayer when an AssetReloadedEvent fires for the SoundGraph type.
        // If the event matches the file currently open in the panel and the panel has no
        // pending edits, the disk version is loaded silently; otherwise an "external change
        // detected" modal is shown so the user can choose between discarding their edits
        // or keeping them. Echoes from the panel's own save are suppressed by comparing
        // against m_LastSelfSaveTime.
        void NotifyAssetReloaded(AssetHandle handle, const std::filesystem::path& path);

      private:
        // Canvas
        void DrawCanvas();
        void DrawGrid(ImDrawList* drawList, const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void DrawNodes(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnections(ImDrawList* drawList, const ImVec2& canvasOrigin);
        void DrawConnectionInProgress(ImDrawList* drawList, const ImVec2& canvasOrigin);

        // Node rendering
        void DrawNode(ImDrawList* drawList, const ImVec2& canvasOrigin, SoundGraphNodeData& node);
        ImVec2 GetNodeSize(const SoundGraphNodeData& node) const;
        ImU32 GetNodeColor(const std::string& type) const;
        ImU32 GetNodeHeaderColor(const std::string& type) const;

        // Pin helpers — node "endpoints" the editor displays on left (inputs) / right (outputs).
        struct PinInfo
        {
            ImVec2 Position; // screen-space center of the pin circle
            UUID NodeID;
            std::string Name;
            bool IsOutput;
            bool IsEvent; // event pins are drawn slightly differently from value pins
        };
        std::vector<PinInfo> GetNodePins(const SoundGraphNodeData& node, const ImVec2& nodeScreenPos) const;

        // Interaction
        void HandleCanvasInput(const ImVec2& canvasOrigin, const ImVec2& canvasSize);
        void HandleNodeInteraction(const ImVec2& canvasOrigin);
        void HandleConnectionDrag(const ImVec2& canvasOrigin);

        // Toolbar & property panel
        void DrawToolbar();
        void DrawPropertyPanel();
        void DrawNodeProperties(SoundGraphNodeData& node);

        // Context menu (node palette)
        void DrawContextMenu(const ImVec2& canvasOrigin);

        // Serialization
        void SaveSoundGraph();
        void SaveSoundGraphAs();
        void LoadSoundGraph(const std::filesystem::path& path);
        void PerformPendingLoad();

        // Node operations
        UUID CreateNode(const std::string& typeName, const glm::vec2& position);
        void DeleteNode(UUID nodeID);
        void DeleteConnection(sizet connectionIndex);

        // Copy/paste of selected nodes. Connections between copied nodes are NOT captured;
        // the user re-wires after pasting. Paste anchors the clipboard centroid at
        // pasteWorldPos so it lands near the cursor regardless of how the user moves the
        // canvas after copying.
        void CopySelectedNodes();
        void PasteNodes(const glm::vec2& pasteWorldPos);

        // Snapshot current asset state for a discrete mutation. Returns nullptr if there
        // is no command history wired or no active graph asset.
        Ref<SoundGraphAsset> SnapshotAsset() const;
        // Push an undo command capturing oldSnap (taken before the mutation) and the current
        // post-mutation state. Safe to call with a nullptr oldSnap (no-op).
        void PushSnapshot(Ref<SoundGraphAsset> oldSnap, std::string description);

        // Session-style undo for continuous-edit widgets (sliders, drag fields, node body
        // drag). BeginEditSession snapshots once at the start; EndEditSession flushes a
        // single undo entry when ImGui reports no widget is active, collapsing a multi-
        // frame drag into one history entry rather than 60-per-second.
        void BeginEditSession();
        void EndEditSession(const char* description);

        // Undo / Redo (called from the Edit menu and the keyboard shortcut handler).
        void Undo();
        void Redo();

        // Resolve a (nodeID, endpoint, direction) triplet to a screen-space pin position.
        // Handles both real nodes and the graph-output pseudo-node. Returns false if
        // nothing matches.
        bool ResolvePinScreenPos(UUID nodeID, const std::string& endpoint, bool wantOutput,
                                 const ImVec2& canvasOrigin, ImVec2& outPos) const;

        // Approximate bezier hit-test. Returns the index of the asset connection nearest
        // to mousePos (within hitDistancePx), or size_t(-1) if no wire is within range.
        // Cheap-and-cheerful: samples each wire as N straight segments and uses
        // point-to-segment distance.
        sizet HitTestConnection(const ImVec2& mousePos, const ImVec2& canvasOrigin,
                                f32 hitDistancePx) const;

        // Coordinate transforms
        ImVec2 WorldToScreen(const glm::vec2& worldPos, const ImVec2& canvasOrigin) const;
        glm::vec2 ScreenToWorld(const ImVec2& screenPos, const ImVec2& canvasOrigin) const;

      private:
        bool m_IsOpen = true;
        bool m_IsFocused = false;

        // Asset state
        std::filesystem::path m_CurrentFilePath;
        AssetHandle m_CurrentAssetHandle = 0;
        Ref<SoundGraphAsset> m_GraphAsset;
        bool m_IsDirty = false;

        // Deferred loading state — see PerformPendingLoad().
        std::filesystem::path m_PendingLoadPath;
        AssetHandle m_PendingLoadHandle = 0;
        int m_PendingLoadFrameDelay = 0;

        // Canvas state
        glm::vec2 m_ScrollOffset = { 0.0f, 0.0f };
        f32 m_Zoom = 1.0f;
        bool m_IsPanning = false;

        // Selection. m_SelectedNodeID is the "primary" selection that drives the property
        // sidebar (single-node-focused UI). m_SelectedNodes is the full selection set used
        // by bulk operations (delete, copy, drag-move all). A plain click sets both to a
        // single node; shift-click toggles membership in m_SelectedNodes without changing
        // m_SelectedNodeID; box-select replaces m_SelectedNodes.
        UUID m_SelectedNodeID = 0;
        std::unordered_set<UUID> m_SelectedNodes;

        // Box-select state. Active while the user is dragging on empty canvas.
        bool m_IsBoxSelecting = false;
        ImVec2 m_BoxSelectStart = {};

        // Node clipboard for Ctrl+C / Ctrl+V. Stores the node data verbatim (including
        // original IDs and positions); paste generates fresh UUIDs and offsets the
        // positions so duplicates don't overlap the originals. Connections are NOT
        // captured — the user can re-wire after pasting.
        std::vector<SoundGraphNodeData> m_Clipboard;
        glm::vec2 m_ClipboardPivotWorld = {}; // Centroid of the copied nodes (in world space).

        // Graph Output / Graph Input pseudo-nodes. UUID(0) is the editor-side sentinel that
        // the asset compiler maps to "this endpoint is the graph itself". Both pseudo-nodes
        // share the sentinel ID; the asset's connection source vs target side disambiguates:
        //   - Graph Output: input pins (OutLeft / OutRight). Connections have m_TargetNodeID = 0.
        //   - Graph Input:  output pins (one per defined graph parameter). Connections have
        //     m_SourceNodeID = 0. Parameter list lives in asset->m_GraphInputs (name → type).
        // Positions are per-session UI state (not serialized).
        glm::vec2 m_GraphOutputNodePos = { 600.0f, 100.0f };
        bool m_DraggingGraphOutputNode = false;
        glm::vec2 m_GraphOutputDragStart = {};

        glm::vec2 m_GraphInputNodePos = { 50.0f, 100.0f };
        bool m_DraggingGraphInputNode = false;
        glm::vec2 m_GraphInputDragStart = {};

        // Edit-session state for continuous widgets (mirrors AnimationGraphEditorPanel).
        Ref<SoundGraphAsset> m_EditSessionSnapshot;
        bool m_EditSessionActive = false;

        // External-file-change tracking. m_LastSelfSaveTime records when SaveSoundGraph
        // last wrote to disk; AssetReloadedEvents arriving within m_SelfSaveEchoWindow of
        // that timestamp are treated as echoes of our own write and ignored. If a genuine
        // external change is detected while the user has unsaved edits, m_ShowExternalReloadDialog
        // is raised and the (handle, path) pair to reload is parked in m_PendingExternal*
        // until the modal resolves. Without an active dirty flag we just queue the load.
        std::chrono::steady_clock::time_point m_LastSelfSaveTime{};
        static constexpr std::chrono::milliseconds m_SelfSaveEchoWindow{ 2000 };
        bool m_ShowExternalReloadDialog = false;
        AssetHandle m_PendingExternalReloadHandle = 0;
        std::filesystem::path m_PendingExternalReloadPath;

        // Live preview state. m_PreviewSound owns a transient SoundGraphSound built from
        // the current asset; toggled from the toolbar Play/Stop button. Lives only for the
        // duration of the preview — Stop releases it. Keeps a real AssetReloadedEvent-style
        // pipeline out of the picture so editing the graph mid-preview just stops and
        // restarts cleanly. Forward-declared by Ref<>; full type comes in the .cpp via
        // SoundGraphSound.h.
        Ref<Audio::SoundGraph::SoundGraphSound> m_PreviewSound;
        bool m_IsPreviewPlaying = false;

      private:
        // Implementation helpers for live preview (called only from the toolbar).
        void StartPreview();
        void StopPreview();

        // "Add Parameter" inline dialog state, opened from the Graph Input pseudo-node's
        // right-click menu. m_NewParamName is reused as a scratch buffer for both add and
        // rename flows; m_NewParamRequested signals OnImGuiRender to show the dialog.
        // m_NewParamTypeIndex indexes the Combo options ("Float" / "Int" / "Bool").
        bool m_ShowAddParamDialog = false;
        char m_NewParamName[64] = {};
        i32 m_NewParamTypeIndex = 0;

        // "Rename Parameter" dialog state. m_RenameParamOldName captures the existing name
        // (set when the user picks "Rename '...'" from the Graph Input context menu);
        // m_RenameParamNewName is the editable buffer for the new name.
        bool m_ShowRenameParamDialog = false;
        std::string m_RenameParamOldName;
        char m_RenameParamNewName[64] = {};

        // Connection dragging — a drag in progress originates from a pin on a node.
        bool m_IsDraggingConnection = false;
        UUID m_DragStartNodeID = 0;
        std::string m_DragStartEndpoint;
        bool m_DragStartIsOutput = false;
        bool m_DragStartIsEvent = false;
        ImVec2 m_DragEndPos = {};

        // Node dragging. m_DragNodeID is the "anchor" — the node the user clicked. When
        // the anchor is part of m_SelectedNodes (multi-select), m_DragNodeStartPositions
        // captures the starting world position of every selected node so the drag applies
        // a uniform delta to all of them. Single-node drag just stores one entry.
        bool m_IsDraggingNode = false;
        UUID m_DragNodeID = 0;
        glm::vec2 m_DragNodeStartPos = {};
        ImVec2 m_DragMouseStartPos = {};
        std::unordered_map<UUID, glm::vec2> m_DragNodeStartPositions;

        // Context menu (node palette). m_RightClickNodeID is non-zero when the right-click
        // landed on a specific node, in which case the popup shows node-specific actions
        // (Delete, etc.) instead of the add-node palette.
        bool m_ShowContextMenu = false;
        ImVec2 m_ContextMenuPos = {};
        char m_NodeSearchFilter[128] = {};
        UUID m_RightClickNodeID = 0;

        // Right-click hit on the Graph Input pseudo-node — distinct from m_RightClickNodeID
        // because both pseudo-nodes share UUID(0), and we want Graph Input's menu to expose
        // Add Parameter / Delete Parameter actions vs Graph Output's read-only blurb.
        bool m_RightClickGraphInput = false;

        // When a right-click hits a connection wire, we record its index into the asset's
        // connection vector so the context menu can offer "Delete Connection". -1 means
        // no wire was hit. Indexes are invalidated as soon as the connection list mutates,
        // so we only ever read this within the same frame the click happened.
        sizet m_RightClickConnectionIndex = static_cast<sizet>(-1);

        // Undo/redo plumbing — wired by EditorLayer like other panels.
        CommandHistory* m_CommandHistory = nullptr;

        // Layout constants (matching ShaderGraph defaults for visual consistency).
        static constexpr f32 s_GridSize = 32.0f;
        static constexpr f32 s_NodeWidth = 180.0f;
        static constexpr f32 s_PinRadius = 5.0f;
        static constexpr f32 s_PinSpacing = 22.0f;
        static constexpr f32 s_HeaderHeight = 26.0f;
        static constexpr f32 s_PropertyPanelWidth = 300.0f;
    };
} // namespace OloEngine
