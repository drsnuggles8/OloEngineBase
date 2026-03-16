#pragma once

#include "OloEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/AnimationPanel.h"
#include "Panels/PostProcessSettingsPanel.h"
#include "Panels/TerrainEditorPanel.h"
#include "Panels/StreamingPanel.h"
#include "Panels/InputSettingsPanel.h"
#include "Panels/NetworkDebugPanel.h"
#include "Panels/ConsolePanel.h"
#include "Panels/DialogueEditorPanel.h"
#include "Panels/SceneStatisticsPanel.h"
#include "Panels/EditorPreferencesPanel.h"

#include "UndoRedo/EditorCommand.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread> // For std::this_thread::yield()
#include <vector>

namespace OloEngine
{
    class AssetReloadedEvent;
    class AssetPackBuilderPanel;

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
        bool OnAssetReloaded(AssetReloadedEvent const& e);

        void OnOverlayRender() const;
        void OnOverlayRender3D() const;

        void NewProject();
        bool OpenProject();
        bool OpenProject(const std::filesystem::path& path);
        void SaveProject();

        void NewScene();
        void OpenScene();
        bool OpenScene(const std::filesystem::path& path);
        void SaveScene();
        void SaveSceneAs();

        void SerializeScene(Ref<Scene> const scene, const std::filesystem::path& path) const;

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
        void UI_RendererStats();
        void UI_Settings();
        void UI_DebugTools();
        void UI_ChildPanels();
        void ApplyDefault3DCameraPose();

        void SetEditorScene(const Ref<Scene>& scene);
        void SyncWindowTitle() const;
        void BindContentBrowserSelectionCallback();

        // Unsaved-changes prompt: returns true if ok to proceed, false if cancelled
        bool ConfirmDiscardChanges();
        bool OnWindowClose(WindowCloseEvent const& e);

        // Terrain editing: screen-to-world raycast against heightmap
        bool TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const;

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

        int m_GizmoType = 0; // Default to Translate (ImGuizmo::TRANSLATE) for immediate usability
        bool m_ShowPhysicsColliders = false;
        bool m_Is3DMode = true; // Toggle for 2D/3D rendering
        bool m_ShowGrid = true;
        f32 m_GridSpacing = 1.0f;

        // Transform snapping
        f32 m_TranslateSnap = 0.5f;
        f32 m_RotateSnap = 45.0f;
        f32 m_ScaleSnap = 0.5f;

        // Entity clipboard (YAML)
        std::string m_EntityClipboard;

        // Camera bookmarks
        struct CameraBookmark
        {
            std::string Name;
            glm::vec3 Position{};
            f32 Pitch = 0.0f;
            f32 Yaw = 0.0f;
            f32 Distance = 10.0f;
        };
        std::vector<CameraBookmark> m_CameraBookmarks;
        char m_BookmarkNameBuffer[64] = {};

        // Debug windows
        bool m_ShowShaderDebugger = false;
        bool m_ShowGPUResourceInspector = false;
        bool m_ShowCommandBucketInspector = false;
        bool m_ShowRendererProfiler = false;
        bool m_ShowRenderGraphDebugger = false;
        bool m_ShowAssetPackBuilder = false;

        // Asset Pack Build Management
        AssetPackBuilder::BuildResult m_LastBuildResult{}; // Result from last build (accessed after m_BuildInProgress is false)
        std::atomic<bool> m_BuildInProgress{ false };
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
        AnimationPanel m_AnimationPanel;
        PostProcessSettingsPanel m_PostProcessSettingsPanel;
        TerrainEditorPanel m_TerrainEditorPanel;
        StreamingPanel m_StreamingPanel;
        InputSettingsPanel m_InputSettingsPanel;
        NetworkDebugPanel m_NetworkDebugPanel;
        ConsolePanel m_ConsolePanel;
        SceneStatisticsPanel m_SceneStatisticsPanel;
        DialogueEditorPanel m_DialogueEditorPanel;
        EditorPreferencesPanel m_EditorPreferencesPanel;
        EditorPreferences m_Prefs;
        bool m_ShowConsolePanel = true;
        bool m_ShowSceneStatistics = true;
        bool m_ShowAnimationPanel = true;
        bool m_ShowPostProcessSettings = true;
        bool m_ShowTerrainEditor = false;
        bool m_ShowStreamingPanel = false;
        bool m_ShowInputSettings = false;
        bool m_ShowNetworkDebug = false;
        bool m_ShowDialogueEditor = false;
        bool m_ShowEditorPreferences = false;

        // Undo/Redo
        CommandHistory m_CommandHistory;
        bool m_LastKnownDirtyState = false;
        bool m_GizmoWasUsing = false;
        glm::vec3 m_GizmoStartTranslation{};
        glm::vec3 m_GizmoStartRotation{};
        glm::vec3 m_GizmoStartScale{};

        // Terrain brush preview UBO (binding 11)
        Ref<UniformBuffer> m_BrushPreviewUBO;

        // Editor resources
        Ref<Texture2D> m_IconPlay;
        Ref<Texture2D> m_IconPause;
        Ref<Texture2D> m_IconSimulate;
        Ref<Texture2D> m_IconStep;
        Ref<Texture2D> m_IconStop;
    };

} // namespace OloEngine
