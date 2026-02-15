#pragma once

#include "OloEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/AnimationPanel.h"
#include "Panels/EnvironmentSettingsPanel.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Asset/AssetPackBuilder.h"

#include <atomic>
#include <mutex>
#include <thread> // For std::this_thread::yield()

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
        void UI_Gizmos() const;
        void UI_RendererStats();
        void UI_Settings();
        void UI_DebugTools();
        void UI_ChildPanels();

        void SetEditorScene(const Ref<Scene>& scene);
        void SyncWindowTitle() const;

      private:
        OloEngine::OrthographicCameraController m_CameraController;

        // Temp
        Ref<VertexArray> m_SquareVA;
        Ref<Shader> m_FlatColorShader;
        Ref<Framebuffer> m_Framebuffer;

        Ref<Scene> m_ActiveScene;
        Ref<Scene> m_EditorScene;
        std::filesystem::path m_EditorScenePath;
        Entity m_SquareEntity;
        Entity m_CameraEntity;
        Entity m_SecondCamera;

        Entity m_HoveredEntity;

        bool m_PrimaryCamera = true;

        EditorCamera m_EditorCamera;

        Ref<Texture2D> m_CheckerboardTexture;

        bool m_ViewportFocused = false;
        bool m_ViewportHovered = false;

        glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
        glm::vec2 m_ViewportBounds[2] = {};

        glm::vec4 m_SquareColor = { 0.2f, 0.3f, 0.8f, 1.0f };

        int m_GizmoType = 0; // Default to Translate (ImGuizmo::TRANSLATE) for immediate usability
        bool m_ShowPhysicsColliders = false;
        bool m_Is3DMode = false; // Toggle for 2D/3D rendering

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
        EnvironmentSettingsPanel m_EnvironmentSettingsPanel;
        bool m_ShowAnimationPanel = true;
        bool m_ShowEnvironmentSettings = false;

        // Editor resources
        Ref<Texture2D> m_IconPlay;
        Ref<Texture2D> m_IconPause;
        Ref<Texture2D> m_IconSimulate;
        Ref<Texture2D> m_IconStep;
        Ref<Texture2D> m_IconStop;
    };

} // namespace OloEngine
