#pragma once

#include "OloEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/AnimationPanel.h"
#include "Panels/PostProcessSettingsPanel.h"
#include "Panels/RendererSettingsPanel.h"
#include "Panels/TerrainEditorPanel.h"
#include "Panels/InstanceScatterBrushPanel.h"
#include "Panels/StreamingPanel.h"
#include "Panels/InputSettingsPanel.h"
#include "Panels/NetworkDebugPanel.h"
#include "Panels/ThreadInspectorPanel.h"
#include "Panels/ConsolePanel.h"
#include "Panels/DialogueEditorPanel.h"
#include "Panels/SkillTreeEditorPanel.h"
#include "Panels/CinematicTimelinePanel.h"
#include "Panels/NavMeshPanel.h"
#include "Panels/BehaviorTreeEditorPanel.h"
#include "Panels/FSMEditorPanel.h"
#include "Panels/ShaderGraphEditorPanel.h"
#include "Panels/SoundGraphEditorPanel.h"
#include "Panels/AnimationGraphEditorPanel.h"
#include "Panels/LocalizationPanel.h"
#include "Panels/SaveGamePanel.h"
#include "Panels/StatisticsPanel.h"
#include "Panels/EditorPreferencesPanel.h"
#include "Panels/GamepadDebugPanel.h"
#include "Panels/ShaderEditorPanel.h"
#include "Panels/AudioEventsPanel.h"

#include "MCP/McpServer.h" // McpInputEvent / McpInputPlan (the input-injection queue below holds them by value)
#include "UndoRedo/EditorCommand.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/SceneMeshRaycast.h"

#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace OloEngine
{
    // (MCP::McpServer and the input-injection payload structs come from MCP/McpServer.h.)

    class AssetLoadedEvent;
    class AssetReloadedEvent;
    class AssetImportedEvent;
    class AssetPackBuilderPanel;
    class BuildGamePanel;

    class EditorLayer : public Layer
    {
      public:
        EditorLayer();
        ~EditorLayer() override;

        void OnAttach() override;
        void OnDetach() override;

        void OnUpdate(Timestep ts) override;
        void OnImGuiRender() override;
        void OnEvent(Event& e) override;

      private:
        bool OnKeyPressed(KeyPressedEvent const& e);
        bool OnMouseButtonPressed(MouseButtonPressedEvent const& e);
        bool OnWindowResized(WindowResizeEvent const& e);
        bool OnAssetLoaded(AssetLoadedEvent const& e);
        bool OnAssetReloaded(AssetReloadedEvent const& e);
        bool OnAssetImported(AssetImportedEvent const& e);

        void OnOverlayRender() const;
        void OnOverlayRender3D() const;

        void NewProject();
        bool OpenProject();
        bool OpenProject(const std::filesystem::path& path);
        void SaveProject();

        void NewScene();
        void OpenScene();
        bool OpenScene(const std::filesystem::path& path);
        // Deserialize `loadPath` and install it as the editor scene, recording
        // `scenePath` as its on-disk identity (they differ only for auto-save
        // recovery). The shared, modal-free "Open Scene" finalization used by
        // OpenScene and the MCP olo_scene_open hook. Returns false on a failed load.
        bool LoadEditorSceneFile(const std::filesystem::path& loadPath, const std::filesystem::path& scenePath);
        // Point the editor camera at a terrain in the scene (terrain spans world
        // [0, worldSize] from its transform, so the default origin-focused camera
        // would otherwise look right past it). No-op when the scene has no terrain.
        void FrameEditorCameraOnTerrain(const Ref<Scene>& scene);
        // Orbit-frame the editor camera on one entity (MCP olo_camera_frame_entity).
        // Returns false when the entity doesn't exist in the active scene.
        bool FrameEditorCameraOnEntity(u64 entityUuid);
        bool SaveScene();
        bool SaveSceneAs();

        void SerializeScene(Ref<Scene> const scene, const std::filesystem::path& path) const;

        // Auto-save
        void AutoSaveScene();
        void UI_AutoSaveRecoveryModal();
        void DeleteAutoSaveFile() const;
        bool LoadSceneInternal(const std::filesystem::path& scenePath);

        void OnScenePlay();
        void OnSceneSimulate();
        void OnSceneStop();
        void OnScenePause();

        void OnDuplicateEntity();
        void OnCopyEntity();
        void OnPasteEntity();

        // Asset Pack Building
        // Initiates an asynchronous build process for packaging project assets
        void BuildAssetPack();

        // Shader Pack Building
        // Bundles all compiled SPIR-V into a single binary file for distribution builds
        void BuildShaderPack() const;

        // Asset reference validation (issue #455)
        // Sweeps the active asset manager's dependency registry for dangling
        // (missing/moved/deleted) references and logs a report to the console.
        void ValidateAssetReferences() const;

        // Build status and progress queries
        bool IsBuildInProgress() const
        {
            return m_BuildInProgress.load();
        }
        f32 GetBuildProgress() const
        {
            return m_BuildProgress.load();
        }
        void CancelBuild()
        {
            m_BuildCancelRequested.store(true);
        }

        // UI Panels
        void UI_MenuBar();
        void UI_Toolbar();
        void UI_Viewport();
        void UI_Gizmos();
        void UI_DebugTools();
        void UI_ChildPanels();
        void ApplyDefault3DCameraPose();
        void TryInitialize3DMode();
        void ApplyPreferences();
        void SyncPrefsFromMembers();

        void SetEditorScene(const Ref<Scene>& scene);
        // Push the live RendererSettings (rendering path, culling, Forward+/depth-prepass
        // derivation) into the render graph / pass state — the same call the settings
        // panel and the MCP write tool make on every edit (#534). Nothing else calls it
        // at editor startup or scene-load, so without this the config/scene defaults
        // (depth-prepass, Forward+ auto-switch) silently never apply until a human toggles
        // a panel setting. Guarded on HasInitialized() and MUST run post-render-graph-build
        // on the main thread — see Renderer3D::ApplyRendererSettings.
        void ApplyRendererSettingsToGraph();
        void SyncWindowTitle() const;
        void BindContentBrowserSelectionCallback();

        // Unsaved-changes prompt: returns true if ok to proceed, false if cancelled
        bool ConfirmDiscardChanges();
        bool OnWindowClose(WindowCloseEvent const& e);

        // Terrain editing: screen-to-world raycast against heightmap
        bool TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const;

        // Unproject a viewport mouse position into a world-space picking ray
        // (normalized direction, unbounded TMax). False when the viewport is
        // degenerate or the camera matrix doesn't invert cleanly.
        bool BuildMouseRay(const glm::vec2& mousePos, const glm::vec2& viewportSize, Ray& outRay) const;

        // Async entity picking (PBO double-buffered readback)
        void InitEntityPicking();
        void ShutdownEntityPicking();

        // ---- MCP synthetic input injection (olo_input_inject, issue #607) -------
        // Accept a frame-quantized plan (called on the game thread from the MCP
        // server's MarshalRead job) and drain exactly ONE of its frames at the top of
        // each OnUpdate. Never inject from the HTTP worker: ImGuiIO is not
        // thread-safe and neither is the engine event dispatch.
        MCP::McpInputInjectResult QueueMcpInput(const MCP::McpInputPlan& plan);
        void DrainMcpInputQueue();
        // Feed one synthetic event into the editor's OWN input stream. See the .cpp
        // for why this goes through the ImGui GLFW backend callbacks rather than the
        // OS or ImGuiIO alone.
        void ApplyMcpInputEvent(const MCP::McpInputEvent& event);
        [[nodiscard]] MCP::McpInputViewportInfo GetMcpInputViewportInfo() const;
        [[nodiscard]] MCP::McpInputStateSnapshot GetMcpInputState() const;

        // ---- MCP editor selection (olo_editor_select_entity, issue #607) --------
        // Select/clear the Scene Hierarchy panel's selection so the Properties
        // inspector draws the requested entity's components — the write
        // EditorMcpContext::SelectEntityInEditor is wired to. `clear` true
        // deselects (entityUuid ignored); otherwise entityUuid is resolved in the
        // active scene. Main-thread-only (EnTT registry + ImGui panel selection
        // state), called from inside a MarshalRead job.
        [[nodiscard]] MCP::McpSelectEntityResult SelectEntityInEditor(u64 entityUuid, bool clear);

      private:
        OloEngine::OrthographicCameraController m_CameraController;

        Ref<Framebuffer> m_Framebuffer;

        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        std::filesystem::path m_EditorScenePath;

        Entity m_HoveredEntity;

        EditorCamera m_EditorCamera;

        bool m_ViewportFocused = false;
        bool m_ViewportHovered = false;

        glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
        glm::vec2 m_ViewportBounds[2] = {};

        int m_GizmoType = 0;    // Default to Translate (ImGuizmo::TRANSLATE) for immediate usability
        bool m_Is3DMode = true; // Toggle for 2D/3D rendering
        f32 m_GridSpacing = 1.0f;

        // Transform snapping
        f32 m_TranslateSnap = 0.5f;
        f32 m_RotateSnap = 45.0f;
        f32 m_ScaleSnap = 0.5f;

        // Entity clipboard (YAML)
        std::string m_EntityClipboard;

        // Debug windows
        bool m_ShowShaderDebugger = false;
        bool m_ShowGPUResourceInspector = false;
        bool m_ShowCommandBucketInspector = false;
        bool m_ShowRendererProfiler = false;
        // Off by default — the debugger sits in OnImGuiRender every frame and
        // tanks FPS while open. User opens it from the Window menu when needed.
        bool m_ShowRenderGraphDebugger = false;
        bool m_ShowAssetPackBuilder = false;
        bool m_ShowBuildGame = false;

        // Asset Pack Build Management
        AssetPackBuilder::BuildResult m_LastBuildResult{}; // Result from last build (accessed after m_BuildInProgress is false)
        std::atomic<bool> m_BuildInProgress{ false };
        std::future<void> m_BuildFuture; // Joinable handle so the destructor can block until the task finishes
        std::atomic<bool> m_BuildCancelRequested{ false };
        std::atomic<f32> m_BuildProgress{ 0.0f };

        enum class SceneState
        {
            Edit = 0,
            Play = 1,
            Simulate = 2
        };
        SceneState m_SceneState = SceneState::Edit;

        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
        Scope<ContentBrowserPanel> m_ContentBrowserPanel;
        Scope<AssetPackBuilderPanel> m_AssetPackBuilderPanel;
        Scope<BuildGamePanel> m_BuildGamePanel;
        AnimationPanel m_AnimationPanel;
        PostProcessSettingsPanel m_PostProcessSettingsPanel;
        RendererSettingsPanel m_RendererSettingsPanel;
        TerrainEditorPanel m_TerrainEditorPanel;
        InstanceScatterBrushPanel m_InstanceScatterBrushPanel;
        bool m_ShowInstanceScatterBrush = false;
        // Mesh-surface raycast for the scatter brush (§1.2) — owns the lazily
        // built per-mesh BVH cache, so it must outlive per-frame queries.
        SceneMeshRaycaster m_MeshRaycaster;
        StreamingPanel m_StreamingPanel;
        InputSettingsPanel m_InputSettingsPanel;
        NetworkDebugPanel m_NetworkDebugPanel;
        ThreadInspectorPanel m_ThreadInspectorPanel;
        ConsolePanel m_ConsolePanel;
        StatisticsPanel m_StatisticsPanel;
        DialogueEditorPanel m_DialogueEditorPanel;
        SkillTreeEditorPanel m_SkillTreeEditorPanel;
        bool m_ShowSkillTreeEditor = false;
        CinematicTimelinePanel m_CinematicTimelinePanel;
        bool m_ShowCinematicTimeline = false;
        EditorPreferencesPanel m_EditorPreferencesPanel;
        EditorPreferences m_Prefs;
        ShaderGraphEditorPanel m_ShaderGraphEditorPanel;
        SoundGraphEditorPanel m_SoundGraphEditorPanel;
        AnimationGraphEditorPanel m_AnimationGraphEditorPanel;
        bool m_ShowConsolePanel = true;
        bool m_ShowStatistics = true;
        bool m_ShowAnimationPanel = true;
        bool m_ShowPostProcessSettings = true;
        bool m_ShowRendererSettings = true;
        bool m_ShowTerrainEditor = false;
        bool m_ShowStreamingPanel = false;
        bool m_ShowInputSettings = false;
        bool m_ShowNetworkDebug = false;
        bool m_ShowThreadInspector = false;
        bool m_ShowDialogueEditor = false;
        NavMeshPanel m_NavMeshPanel;
        bool m_ShowNavMeshPanel = false;
        BehaviorTreeEditorPanel m_BehaviorTreeEditorPanel;
        bool m_ShowBehaviorTreeEditor = false;
        FSMEditorPanel m_FSMEditorPanel;
        bool m_ShowFSMEditor = false;
        bool m_ShowShaderGraphEditor = false;
        bool m_ShowSoundGraphEditor = false;
        bool m_ShowAnimationGraphEditor = false;
        SaveGamePanel m_SaveGamePanel;
        bool m_ShowSaveGamePanel = false;
        LocalizationPanel m_LocalizationPanel;
        bool m_ShowLocalizationPanel = false;
        GamepadDebugPanel m_GamepadDebugPanel;
        bool m_ShowGamepadDebug = false;
        ShaderEditorPanel m_ShaderEditorPanel;
        bool m_ShowShaderEditor = false;
        AudioEventsPanel m_AudioEventsPanel;
        bool m_ShowAudioEventsPanel = false;

        // Read-only MCP diagnostics server (#285). Constructed in OnAttach (off by
        // default), stopped in OnDetach. The user starts it from Window > MCP Server.
        Scope<MCP::McpServer> m_McpServer;
        bool m_ShowMcpPanel = false;
        // Tier-0 MCP viewport-size override (#316): when non-zero, UI_Viewport
        // uses this logical size instead of the ImGui panel size so captures
        // have a deterministic resolution. Set/cleared via olo_viewport_set_size.
        glm::uvec2 m_McpViewportSizeOverride{ 0, 0 };
        // Monotonic frame counter, exposed (with m_ViewportRenderSkipped and
        // m_LastViewportResizeFrame) to the MCP tools so a camera change can be
        // confirmed rendered before a capture is taken.
        u64 m_FrameIndex = 0;
        // Frame at which the viewport framebuffer was last resized. Freshly
        // resized render-graph framebuffers render black for a couple of
        // frames, so captures must not trust the first frames after a resize.
        u64 m_LastViewportResizeFrame = 0;
        // Set by OnWindowResized: Application::OnWindowResize (which runs before
        // layers see the event) resizes the Renderer3D render graph to the OS
        // window's framebuffer size, but in the editor the graph must track the
        // viewport panel / MCP override size. When the viewport-derived size
        // didn't change (typical with an olo_viewport_set_size override active),
        // the OnUpdate resize guard wouldn't fire and the graph would silently
        // keep rendering at window resolution — e.g. maximizing on a 4K monitor
        // inflated a "1920x1080" override's frame times ~6x (#316). This flag
        // forces one reassert of the viewport-derived size on the next update.
        bool m_ViewportSizeReassertNeeded = false;
        // Pending synthetic-input frames (olo_input_inject, #607). Front-popped one
        // per OnUpdate; game-thread-only (enqueued from a MarshalRead job, which also
        // runs on the game thread), so it needs no lock.
        std::deque<std::vector<MCP::McpInputEvent>> m_McpInputQueue;
        // Synthetic modifier keys currently held by an in-flight plan. Re-asserted
        // into ImGuiIO every drained frame because the ImGui GLFW backend recomputes
        // io.KeyMods from the REAL keyboard on every callback, which would otherwise
        // clear them the instant the click that needs them is dispatched.
        bool m_McpSyntheticCtrl = false;
        bool m_McpSyntheticShift = false;
        bool m_McpSyntheticAlt = false;

        // Undo/Redo
        CommandHistory m_CommandHistory;
        bool m_LastKnownDirtyState = false;
        bool m_GizmoWasUsing = false;
        glm::vec3 m_GizmoStartTranslation{};
        glm::vec3 m_GizmoStartRotation{};
        glm::vec3 m_GizmoStartScale{};

        // Terrain brush preview UBO (binding 11)
        Ref<UniformBuffer> m_BrushPreviewUBO;

        // Async entity picking via PBO double-buffering
        u32 m_PickingPBOs[2] = { 0, 0 };
        u32 m_PickingPBOIndex = 0; // Which PBO to write into this frame
        bool m_PickingPBOInitialized = false;
        bool m_PickingReadPending = false; // True after first frame's async read is issued

        // Viewport render throttling — skip expensive scene rendering when
        // frame time exceeds the budget so the editor UI stays responsive.
        bool m_ThrottleEditMode = true;
        bool m_ThrottlePlayMode = false;
        f32 m_RenderBudgetMs = 33.3f; // Skip if last frame > ~30 FPS
        f32 m_LastFrameTimeMs = 0.0f;
        bool m_ViewportRenderSkipped = false;

        // Auto-save
        f32 m_TimeSinceLastAutoSave = 0.0f;
        bool m_ShowAutoSaveRecovery = false;
        // Set when a direct scene load supersedes an armed recovery (issue
        // #607): the already-open modal must close WITHOUT loading, or its
        // deferred load would swap the just-opened scene back out. Cleared
        // whenever a fresh modal opens.
        bool m_CancelAutoSaveRecovery = false;
        std::filesystem::path m_PendingRecoveryScenePath;
        std::filesystem::path m_PendingRecoveryAutoPath;

        // Editor resources
        Ref<Texture2D> m_IconPlay;
        Ref<Texture2D> m_IconPause;
        Ref<Texture2D> m_IconSimulate;
        Ref<Texture2D> m_IconStep;
        Ref<Texture2D> m_IconStop;
    };

} // namespace OloEngine
