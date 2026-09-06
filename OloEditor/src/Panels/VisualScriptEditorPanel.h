#pragma once

#include "Panels/Graph/GraphCanvas.h"

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptVM.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class Scene;

    //==============================================================================
    /// The node-graph gameplay editor (issue #634, AC#3 and AC#7).
    ///
    /// Two things here have no precedent in the repo's seven other graph canvases
    /// and are where the real work is:
    ///
    /// 1. **Exec wires vs data wires.** Control flow is drawn thick, white and
    ///    directional (with a midpoint arrowhead); data is thin and coloured by
    ///    pin type. They are also structurally unmixable — `CheckLinkCompatibility`
    ///    refuses a cross-wire, and the same function drives the link-drag
    ///    highlight, so what the canvas shows and what the compiler accepts can
    ///    never disagree.
    /// 2. **A live debug overlay.** While the scene is playing and the selected
    ///    entity runs this graph, executed nodes glow and fade behind the
    ///    execution, output pins show their current values, and a breakpoint
    ///    freezes that one graph instance while the rest of the scene keeps
    ///    running.
    ///
    /// The viewport maths (pan, zoom, grid, wire beziers, hit-testing) lives in
    /// the reusable `EditorUI::GraphCanvas` rather than being an eighth private
    /// copy — see that header.
    class VisualScriptEditorPanel
    {
      public:
        void OnImGuiRender();

        /// The scene whose runtime the debug overlay reads. Null / not-playing
        /// simply means no overlay.
        void SetContext(const Ref<Scene>& scene)
        {
            m_Scene = scene;
        }
        /// Which entity's graph instance the debugger watches. Fed from the
        /// scene-hierarchy selection.
        void SetSelectedEntity(Entity entity)
        {
            m_DebugEntity = entity;
        }

        void OpenGraph(const std::filesystem::path& path);
        void OpenGraph(AssetHandle handle);
        void NewGraph();

        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_IsDirty;
        }
        /// True when there was nothing to save or the save succeeded.
        [[nodiscard]] bool SaveIfNeeded();

        [[nodiscard]] bool IsOpen() const
        {
            return m_IsOpen;
        }
        void SetOpen(bool open)
        {
            m_IsOpen = open;
        }
        [[nodiscard]] bool IsFocused() const
        {
            return m_IsFocused;
        }

        void Undo();
        void Redo();

        /// Hot-reload (AC#1): the asset changed on disk. Reloads unless the user
        /// has unsaved edits to the same graph, in which case it asks first —
        /// silently clobbering an in-progress edit is worse than a stale view.
        void NotifyAssetReloaded(AssetHandle handle, const std::filesystem::path& path);

      private:
        //-- Undo ------------------------------------------------------------------
        /// Whole-asset value snapshots rather than a command hierarchy. A graph is
        /// a few KB, every edit is structural (a node/link/variable appearing or
        /// disappearing), and a command per operation would be ~15 classes whose
        /// inverse operations are the actual bug surface. `VisualScriptAsset`
        /// itself is non-copyable (Asset deletes its copy ctor), so the snapshot
        /// holds the contents.
        struct Snapshot
        {
            VisualScript::VisualScriptGraph m_EventGraph;
            std::vector<VisualScript::VisualScriptGraph> m_Functions;
            std::vector<VisualScript::VisualScriptVariable> m_Variables;
            u32 m_NodeBudgetPerTick = 10000;
        };
        [[nodiscard]] Snapshot CaptureSnapshot() const;
        void ApplySnapshot(const Snapshot& snapshot);
        /// Call BEFORE mutating. Pushes the pre-edit state and clears the redo
        /// stack.
        void PushUndo();

        //-- Layout ----------------------------------------------------------------
        /// Everything the canvas needs about one node this frame. Rebuilt each
        /// frame because a pin list can change when the author edits a property
        /// (a Sequence's output count, a variable accessor's type).
        struct NodeLayout
        {
            VisualScript::NodeId m_Id = VisualScript::kInvalidNodeId;
            const VisualScript::NodeTypeDescriptor* m_Type = nullptr;
            std::vector<VisualScript::PinDescriptor> m_Pins;
            glm::vec2 m_Position{ 0.0f, 0.0f };
            glm::vec2 m_Size{ 0.0f, 0.0f };
            /// Screen-space pin centres, parallel to m_Pins.
            std::vector<ImVec2> m_PinPositions;
        };
        void RebuildLayout();
        [[nodiscard]] const NodeLayout* FindLayout(VisualScript::NodeId id) const;
        [[nodiscard]] VisualScript::VisualScriptGraph& ActiveGraph();
        [[nodiscard]] const VisualScript::VisualScriptGraph& ActiveGraph() const;

        //-- Drawing ---------------------------------------------------------------
        void DrawToolbar();
        void DrawCanvas();
        void DrawNodes();
        void DrawNode(const NodeLayout& layout);
        void DrawLinks();
        void DrawPendingLink();
        void DrawSidebar();
        void DrawVariablesSection();
        void DrawNodeDetailsSection();
        void DrawDebugSection();
        void DrawContextMenu();

        //-- Interaction -----------------------------------------------------------
        void HandleInput();
        void HandleShortcuts();
        [[nodiscard]] i32 HitTestPin(ImVec2 screenPos, VisualScript::NodeId& outNode) const;
        [[nodiscard]] VisualScript::NodeId HitTestNode(ImVec2 screenPos) const;
        [[nodiscard]] VisualScript::LinkId HitTestLink(ImVec2 screenPos) const;
        void CompleteLinkDrag(VisualScript::NodeId targetNode, i32 targetPin);

        //-- Graph operations ------------------------------------------------------
        void CreateNode(const std::string& typeName, glm::vec2 position);
        void DeleteSelection();
        void CopySelection();
        void PasteClipboard(glm::vec2 position);
        void AutoLayout();
        void FitView();

        //-- Asset -----------------------------------------------------------------
        void Save();
        void SaveAs();
        void LoadFrom(const std::filesystem::path& path);
        void Compile();

        //-- Colours ---------------------------------------------------------------
        [[nodiscard]] static ImU32 PinColor(VisualScript::PinType type);
        [[nodiscard]] static ImU32 CategoryColor(const std::string& category);

        //-- Debug -----------------------------------------------------------------
        /// The live instance for the watched entity, or null. Also (re)arms
        /// tracing and breakpoints on it, so the panel is the only thing that
        /// ever turns debugging on.
        [[nodiscard]] VisualScript::VisualScriptInstance* ResolveDebugInstance();

        /// Storage index of the graph currently on screen, matching
        /// VisualScriptInstance's convention: 0 is the event graph, 1..N are the
        /// functions. `m_ActiveFunction` is -1-based, so this is the one place
        /// the two numberings are reconciled.
        [[nodiscard]] i32 ActiveGraphIndex() const
        {
            return m_ActiveFunction + 1;
        }

        bool m_IsOpen = true;
        bool m_IsFocused = false;
        bool m_IsDirty = false;

        Ref<VisualScript::VisualScriptAsset> m_Asset;
        std::filesystem::path m_FilePath;
        AssetHandle m_AssetHandle = 0;

        EditorUI::GraphCanvas m_Canvas;
        std::vector<NodeLayout> m_Layout;
        /// -1 = the event graph; >= 0 = that index into m_Asset->m_Functions.
        i32 m_ActiveFunction = -1;

        std::unordered_set<VisualScript::NodeId> m_Selection;
        VisualScript::NodeId m_DetailsNode = VisualScript::kInvalidNodeId;

        bool m_DraggingNodes = false;
        glm::vec2 m_DragOrigin{ 0.0f, 0.0f };
        std::vector<std::pair<VisualScript::NodeId, glm::vec2>> m_DragStart;
        /// Captured when a node drag STARTS, committed to the undo stack on
        /// release only if something actually moved. Pushing at press time
        /// instead would put a no-op entry on the stack — and mark the graph
        /// dirty — every time the author merely clicked a node to select it.
        std::optional<Snapshot> m_PendingDragUndo;

        bool m_DraggingLink = false;
        VisualScript::NodeId m_LinkSourceNode = VisualScript::kInvalidNodeId;
        i32 m_LinkSourcePin = -1;
        bool m_LinkFromOutput = true;

        bool m_BoxSelecting = false;
        ImVec2 m_BoxStart{ 0.0f, 0.0f };

        /// Filter text for the Get/Set Component Field pickers. The registry
        /// carries ~1.1k fields across ~130 components, so a plain combo is a
        /// scroll, not a choice. Held on the panel rather than as function-local
        /// statics so two panels (or a panel reopened on a different node) do not
        /// share one another's typing.
        char m_FieldPickerComponentSearch[64] = {};
        char m_FieldPickerFieldSearch[64] = {};

        bool m_ContextMenuOpen = false;
        glm::vec2 m_ContextMenuGraphPos{ 0.0f, 0.0f };
        char m_NodeSearch[128] = {};

        std::vector<VisualScript::VisualScriptNode> m_Clipboard;

        std::vector<Snapshot> m_UndoStack;
        std::vector<Snapshot> m_RedoStack;

        std::vector<VisualScript::CompileDiagnostic> m_CompileErrors;
        bool m_CompiledOk = true;

        Ref<Scene> m_Scene;
        /// Resolved once per frame in DrawCanvas. Looking it up per node would be
        /// a hash lookup per node per frame for a value that cannot change
        /// mid-frame.
        const VisualScript::VisualScriptInstance* m_FrameDebugInstance = nullptr;
        /// Where the right button went down, so a right-DRAG (pan) and a
        /// right-CLICK (context menu) can be told apart at release without
        /// reaching into ImGui's internal drag state, which has moved between
        /// versions.
        ImVec2 m_RightPressPos{ 0.0f, 0.0f };
        Entity m_DebugEntity;
        bool m_TraceEnabled = true;
        bool m_ShowPinValues = true;
        std::unordered_set<u64> m_Breakpoints;

        static constexpr f32 s_NodeMinWidth = 150.0f;
        static constexpr f32 s_HeaderHeight = 24.0f;
        static constexpr f32 s_RowHeight = 20.0f;
        static constexpr f32 s_PinRadius = 4.5f;
        static constexpr f32 s_SidebarWidth = 300.0f;
        static constexpr sizet s_MaxUndoDepth = 64;
        /// How long an executed node keeps glowing, in execution steps rather
        /// than seconds — a graph that ran 4 nodes this tick shows all 4, and a
        /// graph in a tight loop shows the most recent ones instead of a solid
        /// block of light.
        static constexpr u64 s_TraceWindow = 24;
    };

} // namespace OloEngine
