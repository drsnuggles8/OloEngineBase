// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include <OloEnginePCH.h>
#include "EditorLayer.h"
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "OloEngine/Scene/SceneSerializer.h"

#include "OloEngine/Utils/PlatformUtils.h"

#include "ImGuizmo.h"

#include "OloEngine/Math/Math.h"

namespace OloEngine {

	extern const std::filesystem::path g_AssetPath;

	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_CameraController(1280.0f / 720.0f), m_SquareColor({ 0.2f, 0.3f, 0.8f, 1.0f })
	{
	}

	void EditorLayer::OnAttach()
	{
		OLO_PROFILE_FUNCTION();

		m_CheckerboardTexture = Texture2D::Create("assets/textures/Checkerboard.png");
		m_IconPlay = Texture2D::Create("Resources/Icons/PlayButton.png");
		m_IconSimulate = Texture2D::Create("Resources/Icons/SimulateButton.png");
		m_IconStop = Texture2D::Create("Resources/Icons/StopButton.png");

		FramebufferSpecification fbSpec;
		fbSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
		fbSpec.Width = 1280;
		fbSpec.Height = 720;
		m_Framebuffer = Framebuffer::Create(fbSpec);

		m_EditorScene = CreateRef<Scene>();
		m_ActiveScene = m_EditorScene;

		if (const auto commandLineArgs = Application::Get().GetCommandLineArgs(); commandLineArgs.Count > 1)
		{
			const auto sceneFilePath = commandLineArgs[1];
			SceneSerializer const serializer(m_ActiveScene);
			serializer.Deserialize(sceneFilePath);
		}

		m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

#if 0
		// Entity
		auto square = m_ActiveScene->CreateEntity("Green Square");
		square.AddComponent<SpriteRendererComponent>(glm::vec4{ 0.0f, 1.0f, 0.0f, 1.0f });

		auto redSquare = m_ActiveScene->CreateEntity("Red Square");
		redSquare.AddComponent<SpriteRendererComponent>(glm::vec4{ 1.0f, 0.0f, 0.0f, 1.0f });

		m_SquareEntity = square;

		m_CameraEntity = m_ActiveScene->CreateEntity("Camera A");
		m_CameraEntity.AddComponent<CameraComponent>();

		m_SecondCamera = m_ActiveScene->CreateEntity("Camera B");
		auto& cc = m_SecondCamera.AddComponent<CameraComponent>();
		cc.Primary = false;

		class CameraController : public NativeScript
		{
		public:
			CameraController(Entity entity) : NativeScript(entity)
			{
				auto& translation = GetComponent<TransformComponent>().Translation;
				translation.x = rand_r() % 10 - 5.0f;
			}

			void OnUpdate(Timestep ts) override
			{
				auto& translation = GetComponent<TransformComponent>().Translation;

				float speed = 5.0f;

				if (Input::IsKeyPressed(Key::A))
					translation.x -= speed * ts;
				if (Input::IsKeyPressed(Key::D))
					translation.x += speed * ts;
				if (Input::IsKeyPressed(Key::W))
					translation.y += speed * ts;
				if (Input::IsKeyPressed(Key::S))
					translation.y -= speed * ts;
			}
		};

		m_CameraEntity.AddComponent<NativeScriptComponent>().Bind<CameraController>();
		m_SecondCamera.AddComponent<NativeScriptComponent>().Bind<CameraController>();
#endif
	}

	void EditorLayer::OnDetach()
	{
		OLO_PROFILE_FUNCTION();
	}

	void EditorLayer::OnUpdate(Timestep const ts)
	{
		OLO_PROFILE_FUNCTION();

		const double epsilon = 1e-5;

		// Resize
		if (FramebufferSpecification const spec = m_Framebuffer->GetSpecification();
			(m_ViewportSize.x > 0.0f) && (m_ViewportSize.y > 0.0f) && // zero sized framebuffer is invalid
			((std::abs(static_cast<float>(spec.Width) - m_ViewportSize.x) > epsilon) || (std::abs(static_cast<float>(spec.Height) - m_ViewportSize.y) > epsilon)))
		{
			m_Framebuffer->Resize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
			m_CameraController.OnResize(m_ViewportSize.x, m_ViewportSize.y);
			m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
			m_ActiveScene->OnViewportResize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
		}

		// Render
		Renderer2D::ResetStats();
		m_Framebuffer->Bind();
		RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		RenderCommand::Clear();

		// Clear our entity ID attachment to -1
		m_Framebuffer->ClearAttachment(1, -1);

		switch (m_SceneState)
		{
			case SceneState::Edit:
			{
				if (m_ViewportFocused)
				{
					m_CameraController.OnUpdate(ts);
				}

				m_EditorCamera.OnUpdate(ts);

				m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
				break;
			}
			case SceneState::Simulate:
			{
				m_EditorCamera.OnUpdate(ts);

				m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
				break;
			}
			case SceneState::Play:
			{
				m_ActiveScene->OnUpdateRuntime(ts);
				break;
			}
		}

		auto [mx, my] = ImGui::GetMousePos();
		mx -= m_ViewportBounds[0].x;
		my -= m_ViewportBounds[0].y;
		glm::vec2 const viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
		my = viewportSize.y - my;
		const auto mouseX = static_cast<int>(mx);

		if (const auto mouseY = static_cast<int>(my); (mouseX >= 0) && (mouseY >= 0) && (mouseX < static_cast<int>(viewportSize.x)) && (mouseY < static_cast<int>(viewportSize.y)))
		{
			const int pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
			m_HoveredEntity = pixelData == -1 ? Entity() : Entity(static_cast<entt::entity>(pixelData), m_ActiveScene.get());
		}

		OnOverlayRender();

		m_Framebuffer->Unbind();
	}

	void EditorLayer::OnImGuiRender()
	{
		OLO_PROFILE_FUNCTION();

		// Note: Switch this to true to enable dockspace
		static bool dockspaceOpen = true;
		const static bool opt_fullscreen_persistant = true;
		const bool opt_fullscreen = opt_fullscreen_persistant;
		static ImGuiDockNodeFlags const dockspace_flags = ImGuiDockNodeFlags_None;

		// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
		// because it would be confusing to have two docking targets within each others.
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
		if (opt_fullscreen)
		{
			ImGuiViewport const* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->Pos);
			ImGui::SetNextWindowSize(viewport->Size);
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
			window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
		}

		// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background and handle the pass-thru hole, so we ask Begin() to not render a background.
		if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
		{
			window_flags |= ImGuiWindowFlags_NoBackground;
		}

		// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
		// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
		// all active windows docked into it will lose their parent and become undocked.
		// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
		// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);
		ImGui::PopStyleVar();

		if (opt_fullscreen)
		{
			ImGui::PopStyleVar(2);
		}

		// DockSpace
		ImGuiIO const& io = ImGui::GetIO();
		ImGuiStyle& style = ImGui::GetStyle();
		const float minWinSizeX = style.WindowMinSize.x;
		style.WindowMinSize.x = 370.0f;
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			ImGuiID const dockspace_id = ImGui::GetID("MyDockSpace");
			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
		}

		style.WindowMinSize.x = minWinSizeX;

		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				// Disabling fullscreen would allow the window to be moved to the front of other windows,
				// which we can't undo at the moment without finer window depth/z control.
				//ImGui::MenuItem("Fullscreen", NULL, &opt_fullscreen_persistant);
				if (ImGui::MenuItem("New", "Ctrl+N"))
				{
					NewScene();
				}

				if (ImGui::MenuItem("Open...", "Ctrl+O"))
				{
					OpenScene();
				}

				if (ImGui::MenuItem("Save", "Ctrl+S", false, m_ActiveScene != nullptr))
				{
					SaveScene();
				}

				if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_ActiveScene != nullptr))
				{
					SaveSceneAs();
				}

				if (ImGui::MenuItem("Exit"))
				{
					Application::Get().Close();
				}
				ImGui::EndMenu();
			}

			ImGui::EndMenuBar();
		}

		m_SceneHierarchyPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();

		ImGui::Begin("Stats");

		std::string name = "None";
		if (m_HoveredEntity)
		{
			name = m_HoveredEntity.GetComponent<TagComponent>().Tag;
		}
		ImGui::Text("Hovered Entity: %s", name.c_str());

		const auto stats = Renderer2D::GetStats();
		ImGui::Text("Renderer2D Stats:");
		ImGui::Text("Draw Calls: %d", stats.DrawCalls);
		ImGui::Text("Quads: %d", stats.QuadCount);
		ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
		ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

		ImGui::End();

		ImGui::Begin("Settings");
		ImGui::Checkbox("Show physics colliders", &m_ShowPhysicsColliders);
		ImGui::End();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });
		ImGui::Begin("Viewport");
		const auto viewportMinRegion = ImGui::GetWindowContentRegionMin();
		const auto viewportMaxRegion = ImGui::GetWindowContentRegionMax();
		const auto viewportOffset = ImGui::GetWindowPos();
		m_ViewportBounds[0] = { viewportMinRegion.x + viewportOffset.x, viewportMinRegion.y + viewportOffset.y };
		m_ViewportBounds[1] = { viewportMaxRegion.x + viewportOffset.x, viewportMaxRegion.y + viewportOffset.y };

		m_ViewportFocused = ImGui::IsWindowFocused();
		m_ViewportHovered = ImGui::IsWindowHovered();
		Application::Get().GetImGuiLayer()->BlockEvents((!m_ViewportFocused) && (!m_ViewportHovered));

		ImVec2 const viewportPanelSize = ImGui::GetContentRegionAvail();
		m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };

		uint64_t const textureID = m_Framebuffer->GetColorAttachmentRendererID(0);
		ImGui::Image(reinterpret_cast<void*>(textureID), ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
			{
				auto* const path = static_cast<wchar_t* const>(payload->Data);

				const auto filePath = std::filesystem::path(path);
				if (filePath.extension().string() == ".olo")
				{
					OpenScene(std::filesystem::path(g_AssetPath) / path);
				}
				else if (filePath.extension().string() == ".png")
				{
					const std::filesystem::path texturePath = std::filesystem::path(g_AssetPath) / path;
					const Ref<Texture2D> texture = Texture2D::Create(texturePath.string());
					if (texture->IsLoaded())
					{
						if (m_HoveredEntity && m_HoveredEntity.HasComponent<SpriteRendererComponent>())
						{
							m_HoveredEntity.GetComponent<SpriteRendererComponent>().Texture = texture;
						}
					}
					else
					{
						OLO_WARN("Could not load texture {0}", texturePath.filename().string());
					}
				}
				else
				{
					OLO_WARN("Tried to load unknown filetype {0}", filePath);
				}
			}
			ImGui::EndDragDropTarget();
		}

		// Gizmos
		if (Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity(); selectedEntity && (m_GizmoType != -1))
		{
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist();

			ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y);

			// Camera
			// Runtime camera from entity
			// auto cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
			// const auto& camera = cameraEntity.GetComponent<CameraComponent>().Camera;
			// const glm::mat4& cameraProjection = camera.GetProjection();
			// glm::mat4 cameraView = glm::inverse(cameraEntity.GetComponent<TransformComponent>().GetTransform());

			// Editor camera
			const glm::mat4& cameraProjection = m_EditorCamera.GetProjection();
			glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();

			// Entity transform
			auto& tc = selectedEntity.GetComponent<TransformComponent>();
			glm::mat4 transform = tc.GetTransform();

			// Snapping
			const bool snap = Input::IsKeyPressed(Key::LeftControl);
			float snapValue = 0.5f; // Snap to 0.5m for translation/scale
			// Snap to 45 degrees for rotation
			if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
			{
				snapValue = 45.0f;
			}

			const std::array<float, 3> snapValues = { snapValue, snapValue, snapValue };

			ImGuizmo::Manipulate(glm::value_ptr(cameraView),
				glm::value_ptr(cameraProjection),
				static_cast<ImGuizmo::OPERATION>(m_GizmoType),
				ImGuizmo::LOCAL,
				glm::value_ptr(transform),
				nullptr,
				snap ? snapValues.data() : nullptr);

			if (ImGuizmo::IsUsing())
			{
				glm::vec3 translation;
				glm::vec3 rotation;
				glm::vec3 scale;
				Math::DecomposeTransform(transform, translation, rotation, scale);

				glm::vec3 const deltaRotation = rotation - tc.Rotation;
				tc.Translation = translation;
				tc.Rotation += deltaRotation;
				tc.Scale = scale;
			}
		}
		ImGui::End();
		ImGui::PopStyleVar();

		UI_Toolbar();

		ImGui::End();
	}

	void EditorLayer::UI_Toolbar()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		auto const& colors = ImGui::GetStyle().Colors;
		const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
		const auto& buttonActive = colors[ImGuiCol_ButtonActive];
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		const auto toolbarEnabled = static_cast<bool>(m_ActiveScene);

		auto tintColor = ImVec4(1, 1, 1, 1);
		if (!toolbarEnabled)
		{
			tintColor.w = 0.5f;
		}

		const float size = ImGui::GetWindowHeight() - 4.0f;
		{
			Ref<Texture2D> const icon = ((m_SceneState == SceneState::Edit) || (m_SceneState == SceneState::Simulate)) ? m_IconPlay : m_IconStop;
			ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x * 0.5f) - (size * 0.5f));
			if (ImGui::ImageButton(reinterpret_cast<ImTextureID>(icon->GetRendererID()), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), 0, ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor) && toolbarEnabled)
			{
				if ((m_SceneState == SceneState::Edit) || (m_SceneState == SceneState::Simulate))
				{
					OnScenePlay();
				}
				else if (m_SceneState == SceneState::Play)
				{
					OnSceneStop();
				}
			}
		}
		ImGui::SameLine();
		{
			Ref<Texture2D> const icon = ((m_SceneState == SceneState::Edit) || (m_SceneState == SceneState::Play)) ? m_IconSimulate : m_IconStop;		//ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x * 0.5f) - (size * 0.5f));
			if ((ImGui::ImageButton(reinterpret_cast<ImTextureID>(icon->GetRendererID()), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), 0, ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor)) && toolbarEnabled)
			{
				if ((m_SceneState == SceneState::Edit) || (m_SceneState == SceneState::Play))
				{
					OnSceneSimulate();
				}
				else if (m_SceneState == SceneState::Simulate)
				{
					OnSceneStop();
				}
			}
		}
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
		ImGui::End();
	}

	void EditorLayer::OnEvent(Event& e)
	{
		m_CameraController.OnEvent(e);
		m_EditorCamera.OnEvent(e);

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
	}

	bool EditorLayer::OnKeyPressed(KeyPressedEvent const& e)
	{
		// Shortcuts
		if (e.IsRepeat())
		{
			return false;
		}

		const bool control = Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl);
		const bool shift = Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift);

		switch (e.GetKeyCode())
		{
			case Key::N:
			{
				if (control)
				{
					NewScene();
				}

				break;
			}
			case Key::O:
			{
				if (control)
				{
					OpenScene();
				}

				break;
			}
			case Key::S:
			{
				if (control)
				{
					if (shift)
					{
						SaveSceneAs();
					}
					else
					{
						SaveScene();
					}
				}

				break;
			}

			// Scene Commands
			case Key::D:
			{
				if (control)
				{
					OnDuplicateEntity();
				}
				break;
			}

			// Gizmos
			case Key::Q:
			{
				if (!ImGuizmo::IsUsing())
				{
					m_GizmoType = -1;
				}
				break;
			}
			case Key::W:
			{
				if (!ImGuizmo::IsUsing())
				{
					m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
				}
				break;
			}
			case Key::E:
			{
				if (!ImGuizmo::IsUsing())
				{
					m_GizmoType = ImGuizmo::OPERATION::ROTATE;
				}
				break;
			}
			case Key::R:
			{
				if (!ImGuizmo::IsUsing())
				{
					m_GizmoType = ImGuizmo::OPERATION::SCALE;
				}
				break;
			}

			default:
			{
				break;
			}
		}
	}

	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent const& e)
	{
		if ((e.GetMouseButton() == Mouse::ButtonLeft) && m_ViewportHovered && (!ImGuizmo::IsOver()) && (!Input::IsKeyPressed(Key::LeftAlt)))
		{
			m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
		}
		return false;
	}

	void EditorLayer::OnOverlayRender() const
	{
		if (m_SceneState == SceneState::Play)
		{
			Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
			if (!camera)
			{
				return;
			}
			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
		}
		else
		{
			Renderer2D::BeginScene(m_EditorCamera);
		}

		if (m_ShowPhysicsColliders)
		{
			// Calculate z index for translation
			const float zIndex = 0.001f;
			glm::vec3 cameraForwardDirection = m_EditorCamera.GetForwardDirection();
			glm::vec3 projectionCollider = cameraForwardDirection * glm::vec3(zIndex);

			// Box Colliders
			{
				const auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (const auto entity : view)
				{
					const auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

					const glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, -projectionCollider.z);
					const glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					const glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
						* glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawRect(transform, glm::vec4(0, 1, 0, 1));
				}
			}

			// Circle Colliders
			{
				const auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
				for (const auto entity : view)
				{
					const auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

					const glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, -projectionCollider.z);
					const glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

					const glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation)
						* glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
				}
			}
		}

		Renderer2D::EndScene();
	}

	void EditorLayer::NewScene()
	{
		m_ActiveScene = CreateRef<Scene>();
		m_ActiveScene->OnViewportResize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		m_EditorScenePath = std::filesystem::path();
	}

	void EditorLayer::OpenScene()
	{
		std::string const filepath = FileDialogs::OpenFile("OloEditor Scene (*.olo)\0*.olo\0");
		if (!filepath.empty())
		{
			OpenScene(filepath);
		}
	}

	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		if (m_SceneState != SceneState::Edit)
		{
			OnSceneStop();
		}

		if (path.extension().string() != ".olo")
		{
			OLO_WARN("Could not load {0} - not a scene file", path.filename().string());
			return;
		}

		Ref<Scene> const newScene = CreateRef<Scene>();
		SceneSerializer const serializer(newScene);
		if (serializer.Deserialize(path.string()))
		{
			m_EditorScene = newScene;
			m_EditorScene->OnViewportResize(static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
			m_SceneHierarchyPanel.SetContext(m_EditorScene);

			m_ActiveScene = m_EditorScene;
			m_EditorScenePath = path;
		}
	}

	void EditorLayer::SaveScene()
	{
		if (!m_EditorScenePath.empty())
		{
			SerializeScene(m_ActiveScene, m_EditorScenePath);
		}
		else
		{
			SaveSceneAs();
		}
	}

	void EditorLayer::SaveSceneAs()
	{
		const std::string filepath = FileDialogs::SaveFile("OloEditor Scene (*.olo)\0*.olo\0");
		if (!filepath.empty())
		{
			SerializeScene(m_ActiveScene, filepath);
			m_EditorScenePath = filepath;
		}
	}

	void EditorLayer::SerializeScene(Ref<Scene> const scene, const std::filesystem::path& path) const
	{
		const SceneSerializer serializer(scene);
		serializer.Serialize(path.string());
	}

	void EditorLayer::OnScenePlay()
	{
		if (m_SceneState == SceneState::Simulate)
		{
			OnSceneStop();
		}

		m_SceneState = SceneState::Play;

		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnRuntimeStart();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnSceneSimulate()
	{
		if (m_SceneState == SceneState::Play)
		{
			OnSceneStop();
		}

		m_SceneState = SceneState::Simulate;

		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnSimulationStart();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}


	void EditorLayer::OnSceneStop()
	{
		OLO_CORE_ASSERT(m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)

		if (m_SceneState == SceneState::Play)
		{
			m_ActiveScene->OnRuntimeStop();
		}
		else if (m_SceneState == SceneState::Simulate)
		{
			m_ActiveScene->OnSimulationStop();
		}

		m_SceneState = SceneState::Edit;

		m_ActiveScene = m_EditorScene;

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnDuplicateEntity() const
	{
		if (m_SceneState != SceneState::Edit)
		{
			return;
		}

		const Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		if (selectedEntity)
		{
			m_EditorScene->DuplicateEntity(selectedEntity);
		}
	}

}
