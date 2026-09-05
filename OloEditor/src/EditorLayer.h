#pragma once

#include "OloEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/AnimationPanel.h"
#include "Panels/PostProcessSettingsPanel.h"
#include "Panels/RendererSettingsPanel.h"
#include "Panels/TerrainEditorPanel.h"
#include "Panels/InstanceScatterBrushPanel.h"
#include "Panels/TilemapPainterPanel.h"
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
#include "Panels/VisualScriptEditorPanel.h"
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
#include "OloEngine/Renderer/Baking/LightmapBaker.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/SceneMeshRaycast.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace OloEngine
{
    // (MCP::McpServer and the input-injection payload structs come from MCP/McpServer.h.)
    namespace MCP::EditorPanels
    {
        enum class PanelId : u32;
    }

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

        // Point every scene-observing panel at `scene`, with `history` as its
        // undo/redo target (nullptr while playing/simulating — a runtime scene
        // has no editable history). Panels hold raw Scene* / entity handles, so
        // every scene swap must go through here or a panel is left pointing at
        // a destroyed registry.
        void BindPanelsToScene(const Ref<Scene>& scene, CommandHistory* history);

        // Bring `m_ActiveScene` up as the running Play-mode scene: render
        // preference, OnRuntimeStart, gameplay-event logger, panel binding.
        // Shared by OnScenePlay and SwitchPlayScene so the two ways Play mode
        // acquires a scene cannot drift apart.
        void StartActiveRuntimeScene();

        // Service a script-requested scene switch during Play (issue #642).
        // Loads `request` (resolved against the project asset directory) as a
        // fresh runtime scene and swaps it in, leaving the EDITOR scene — and
        // any unsaved edits in it — untouched, so Stop still returns the user to
        // what they were working on. Returns false and keeps the current scene
        // running if the target can't be resolved or loaded.
        bool SwitchPlayScene(const std::string& request, const std::string& saveSlot = {});

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

        // Baked-GI lightmap bake (issue #439). BakeLightmaps runs the unwrap +
        // rasterization + reference-scene capture on the game thread (they
        // touch meshes the editor renders / the ECS), then hands the texel bake
        // to a background task; ProcessLightmapBakeCompletion (called from
        // OnUpdate) consumes the result mailbox on the game thread — asset
        // save, scene settings update, runtime re-resolve.
        void BakeLightmaps();
        [[nodiscard]] MCP::LightmapBake::Snapshot LightmapBakeFromMcp(const MCP::LightmapBake::Request& request, bool start);
        void ProcessLightmapBakeCompletion();

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

        // Terrain editing: screen-to-world raycast against the heightmap.
        //
        // Prefers the GPU picker (issue #717) and falls back to the CPU march
        // only while the GPU answer has not come back yet, or when the GPU path
        // is unusable at all. Both halves are below.
        bool TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const;

        // How far the GPU picker has got with the ray for this frame.
        enum class TerrainPickState : u8
        {
            Unavailable, // no terrain, no GPU tree, or the A/B lever is off
            Pending,     // ray submitted, answer still in flight
            Answered,    // `outHit` / `outHitPos` are this pick's real result
        };

        // Submit this frame's ray to the GPU picker and read whatever the ring
        // has already retired. Never stalls: the answer is one or two frames
        // old by construction, which is what a hovering brush cursor can afford
        // and a synchronous readback is not.
        TerrainPickState TerrainRaycastGPU(const glm::vec2& mousePos, const glm::vec2& viewportSize,
                                           glm::vec3& outHitPos, bool& outHit) const;
        MCP::TerrainPick::Snapshot TerrainPickFromMcp(const MCP::TerrainPick::Request& request, bool submit) const;

        // The pre-#717 path: march the CPU-side heightmap mirror in 1-unit
        // steps and bisect. Still the fallback, and still the reference the GPU
        // path's accuracy is measured against.
        bool TerrainRaycastCPU(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const;

        // Process-wide A/B lever for GPU terrain picking, the twin of
        // TerrainChunkManager::IsGpuDrivenLODEnabled. Defaults to enabled unless
        // OLO_TERRAIN_CPU_PICK=1 — when the brush cursor sits in the wrong
        // place, the first question is which of the two paths put it there, and
        // that has to be answerable without a rebuild.
        [[nodiscard]] static bool IsGpuTerrainPickingEnabled();

        // Monotonic id stamped onto each submitted pick ray, so a returned
        // answer can be told apart from the one before it. Mutable because the
        // raycast entry points are const — submitting a ray is a query from the
        // caller's point of view.
        mutable u32 m_TerrainPickRayId = 0;
        mutable u32 m_McpTerrainPickRayId = 0;
        mutable MCP::TerrainPick::Request m_McpTerrainPickInput;
        mutable MCP::TerrainPick::WorldRay m_McpTerrainPickWorldRay;
        mutable glm::mat4 m_McpTerrainPickTerrainTransform{ 1.0f };

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
        // Hold the ImGui GLFW backend's "the cursor is over this window" latch for the
        // duration of a plan, and hand it back to physical reality when the plan ends
        // (issue #854). Without the first, every injected position is overwritten by
        // the hardware one; without the second, the editor is left hovering a point
        // the physical mouse is nowhere near.
        void AssertSyntheticCursorOverWindow();
        void RestoreCursorOverWindowState();
        // Release any mouse button an in-flight plan pressed but never released.
        void ReleaseSyntheticMouseButtons();
        // Read back where the last injected cursor position ACTUALLY landed in ImGui,
        // one frame after it was fed in (issue #854). Called at the top of every
        // drain, including the ones after a plan has finished.
        void SampleMcpCursorLanding();
        [[nodiscard]] MCP::McpInputViewportInfo GetMcpInputViewportInfo() const;
        [[nodiscard]] MCP::McpInputStateSnapshot GetMcpInputState() const;

        // ---- MCP editor liveness (issue #607) ----------------------------------
        // Frame counter + wall-clock gap since the last completed frame + window
        // state, so a tool can say "the editor is asleep" instead of quietly
        // returning a stale frame. Main-thread-only (queries GLFW), called from
        // inside a MarshalRead job.
        [[nodiscard]] MCP::McpEditorLiveness GetMcpEditorLiveness() const;
        // The single definition of "the viewport framebuffer must not be captured
        // yet" — throttle-skipped, or still inside the post-resize black-frame
        // window. Shared by the IsCaptureUnready context hook and the liveness
        // snapshot so the two can never disagree.
        [[nodiscard]] bool IsViewportCaptureUnready() const;

        // ---- MCP editor selection (olo_editor_select_entity, issue #607) --------
        // Select/clear the Scene Hierarchy panel's selection so the Properties
        // inspector draws the requested entity's components — the write
        // EditorMcpContext::SelectEntityInEditor is wired to. `clear` true
        // deselects (entityUuid ignored); otherwise entityUuid is resolved in the
        // active scene. Main-thread-only (EnTT registry + ImGui panel selection
        // state), called from inside a MarshalRead job.
        [[nodiscard]] MCP::McpSelectEntityResult SelectEntityInEditor(u64 entityUuid, bool clear);

        [[nodiscard]] std::vector<MCP::McpEditorPanelState> GetMcpEditorPanels() const;
        [[nodiscard]] MCP::McpEditorPanelSetResult SetMcpEditorPanel(const std::string& panel, bool open);
        [[nodiscard]] MCP::McpEditorDebugDrawState GetMcpEditorDebugDrawState() const;
        [[nodiscard]] MCP::McpEditorDebugDrawSetResult SetMcpEditorDebugDraw(const std::string& category, bool enabled);
        [[nodiscard]] bool& McpPanelVisibility(MCP::EditorPanels::PanelId panel);
        [[nodiscard]] const bool& McpPanelVisibility(MCP::EditorPanels::PanelId panel) const;

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

        // Lightmap bake state (issue #439) — same shape as the asset-pack build:
        // atomics for progress/cancel, a future the destructor can join, and a
        // mutex-guarded result mailbox the game thread drains in OnUpdate.
        std::atomic<bool> m_LightmapBakeInProgress{ false };
        std::atomic<bool> m_LightmapBakeCancel{ false };
        std::atomic<f32> m_LightmapBakeProgress{ 0.0f };
        std::future<void> m_LightmapBakeFuture;
        std::mutex m_LightmapBakeResultMutex;
        LightmapBakeResult m_LightmapBakeResult; // guarded by m_LightmapBakeResultMutex
        bool m_LightmapBakeResultReady = false;  // guarded by m_LightmapBakeResultMutex
        // Which scene the running bake belongs to, pinned at bake start (game
        // thread only): completion runs frames later, and the active scene may
        // have been switched — or swapped for the Play copy — by then. The
        // result must attach to THIS scene or be reported as orphaned, never
        // silently attached to whatever is active at completion.
        Ref<Scene> m_LightmapBakeScene;
        std::filesystem::path m_LightmapBakeScenePath;
        u64 m_McpLightmapBakeSequence = 0;
        bool m_McpLightmapSaveRequested = false;
        MCP::LightmapBake::Snapshot m_McpLightmapBakeSnapshot;

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
        TilemapPainterPanel m_TilemapPainterPanel;
        bool m_ShowTilemapPainter = false;
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
        VisualScriptEditorPanel m_VisualScriptEditorPanel;
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
        bool m_ShowVisualScriptEditor = false;
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
        // Wall clock at the top of the most recent OnUpdate. The frame counter alone
        // cannot distinguish "stalled" from "slow" without two samples, so an agent
        // would have to poll twice and reason about the interval; a timestamp turns
        // "is the loop actually ticking?" into ONE call (issue #607). Zero until the
        // first frame runs, which GetMcpEditorLiveness reports as "not stalled"
        // rather than inventing a stall during startup.
        std::chrono::steady_clock::time_point m_LastFrameTick{};
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
        // True while a plan holds the backend's cursor-enter latch (issue #854), so
        // the teardown knows it has something to hand back.
        bool m_McpSyntheticCursorEntered = false;
        // Quiet frames to wait after a plan ends before handing hover back to the
        // physical mouse. Long enough that the caller's post-plan state read still
        // sees what the injection did (it reads one frame past the plan, and the
        // editor's entity picking is two frames latent on top of that), short enough
        // that no human could notice. 0 = nothing pending.
        static constexpr int s_McpCursorRestoreDelayFrames = 8;
        int m_McpCursorRestoreCountdown = 0;
        // Mouse buttons an in-flight plan has pressed and not yet released. A plan's
        // teardown releases whatever is left here: unlike keyAction "press", no mouse
        // action deliberately leaves a button held, so anything still down at the end
        // of a plan is a half-applied plan, and a stuck button freezes ImGui's ActiveId
        // for the rest of the session.
        std::array<bool, 8> m_McpSyntheticButtonsDown{};
        // The last injected cursor position (window-client logical px) and where ImGui
        // actually put it, sampled one frame later. See McpInputStateSnapshot.
        bool m_McpCursorProbeArmed = false;
        bool m_McpCursorLandingValid = false;
        f32 m_McpCursorAskedX = 0.0f;
        f32 m_McpCursorAskedY = 0.0f;
        f32 m_McpCursorLandedX = 0.0f;
        f32 m_McpCursorLandedY = 0.0f;
        // What ImGui's hit-test resolved on the LAST frame an injected plan actually
        // ran (issue #921), captured at the end of OnImGuiRender (after every panel
        // has run its Begin/widgets for the frame) while a plan is in flight. A live
        // read taken when the tool reports back — after the plan has drained and the
        // cursor may already have been handed back to "nowhere" — is stale by
        // construction: g.HoveredWindow answers "what's under the cursor NOW", and by
        // then there may BE no cursor. Latching it during the plan is what makes the
        // diagnostic answer the question it's meant to answer.
        std::string m_McpHoveredWindowName;
        u32 m_McpHoveredId = 0;
        u32 m_McpActiveId = 0;

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
