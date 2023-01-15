#pragma once

#include "OloEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"

#include "OloEngine/Renderer/EditorCamera.h"

namespace OloEngine
{
	class EditorLayer : public Layer
	{
	public:
		EditorLayer();
		~EditorLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;

		void OnUpdate(Timestep ts) override;
		void OnImGuiRender() override;
		void OnEvent(Event& e) override;
	private:
		bool OnKeyPressed(KeyPressedEvent const& e);
		bool OnMouseButtonPressed(MouseButtonPressedEvent const& e);

		void OnOverlayRender() const;

		void NewProject();
		void OpenProject();
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
		void OnScenePause() const;

		void OnDuplicateEntity() const;

		// UI Panels
		void UI_MenuBar();
		void UI_Toolbar();
		void UI_Viewport();
		void UI_Gizmos() const;
		void UI_RendererStats();
		void UI_Settings();
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

		int m_GizmoType = -1;

		bool m_ShowPhysicsColliders = false;

		enum class SceneState
		{
			Edit = 0, Play = 1, Simulate = 2
		};
		SceneState m_SceneState = SceneState::Edit;

		// Panels
		SceneHierarchyPanel m_SceneHierarchyPanel;
		Scope<ContentBrowserPanel> m_ContentBrowserPanel;

		// Editor resources
		Ref<Texture2D> m_IconPlay;
		Ref<Texture2D> m_IconPause;
		Ref<Texture2D> m_IconSimulate;
		Ref<Texture2D> m_IconStep;
		Ref<Texture2D> m_IconStop;

		// Texture Array
		Ref<Texture2DArray> m_TextureArray;
	};

}
