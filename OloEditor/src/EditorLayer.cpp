#include "OloEnginePCH.h"
#include "EditorLayer.h"
#include "Panels/AssetPackBuilderPanel.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugger.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Task/Task.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <thread>

namespace OloEngine
{
    static std::unique_ptr<Font> s_Font;

    EditorLayer::EditorLayer()
        : Layer("EditorLayer"), m_CameraController(1280.0f / 720.0f)
    {
    }

    EditorLayer::~EditorLayer()
    {
        // Cancel any ongoing build
        m_BuildCancelRequested.store(true);

        // Wait for build to complete if running (spin-wait with yield)
        while (m_BuildInProgress.load())
        {
            // Keep requesting cancellation during wait
            m_BuildCancelRequested.store(true);
            std::this_thread::yield();
        }
    }

    void EditorLayer::OnAttach()
    {
        OLO_PROFILE_FUNCTION();

        Application::Get().GetWindow().SetTitle("Test");

        m_IconPlay = Texture2D::Create("Resources/Icons/PlayButton.png");
        m_IconPause = Texture2D::Create("Resources/Icons/PauseButton.png");
        m_IconSimulate = Texture2D::Create("Resources/Icons/SimulateButton.png");
        m_IconStep = Texture2D::Create("Resources/Icons/StepButton.png");
        m_IconStop = Texture2D::Create("Resources/Icons/StopButton.png");

        FramebufferSpecification fbSpec;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
        fbSpec.Width = 1280;
        fbSpec.Height = 720;
        m_Framebuffer = Framebuffer::Create(fbSpec);

        if (const auto commandLineArgs = Application::Get().GetSpecification().CommandLineArgs; commandLineArgs.Count > 1)
        {
            auto* projectFilePath = commandLineArgs[1];
            OpenProject(projectFilePath);
        }
        else
        {
            if (!OpenProject())
            {
                Application::Get().Close();
            }
        }
        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);
    }

    void EditorLayer::OnDetach()
    {
        OLO_PROFILE_FUNCTION();
    }

    void EditorLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();

        // Sync with async asset loading thread
        AssetManager::SyncWithAssetThread();

        m_ActiveScene->OnViewportResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));

        const f64 epsilon = 1e-5;

        // Resize
        if (FramebufferSpecification const spec = m_Framebuffer->GetSpecification();
            (m_ViewportSize.x > 0.0f) && (m_ViewportSize.y > 0.0f) && // zero sized framebuffer is invalid
            ((std::abs(static_cast<f32>(spec.Width) - m_ViewportSize.x) > epsilon) || (std::abs(static_cast<f32>(spec.Height) - m_ViewportSize.y) > epsilon)))
        {
            m_Framebuffer->Resize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));
            m_CameraController.OnResize(m_ViewportSize.x, m_ViewportSize.y);
            m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);

            // Also resize Renderer3D's render graph for 3D mode
            if (m_Is3DMode)
            {
                Renderer3D::OnWindowResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));
            }
        }

        // Render
        Renderer2D::ResetStats();

        // In 3D mode, Renderer3D manages its own framebuffer via RenderGraph
        // In 2D mode, we use the editor's framebuffer
        if (!m_Is3DMode)
        {
            m_Framebuffer->Bind();
            RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
            RenderCommand::Clear();
            // Clear our entity ID attachment to -1
            m_Framebuffer->ClearAttachment(1, -1);
        }

        switch (m_SceneState)
        {
            case SceneState::Edit:
            {
                if (m_ViewportFocused)
                {
                    m_CameraController.OnUpdate(ts);
                }

                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
                break;
            }
            case SceneState::Simulate:
            {
                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
                break;
            }
            case SceneState::Play:
            {
                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
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
            // Read entity ID from appropriate framebuffer based on mode
            int pixelData = -1;
            if (m_Is3DMode)
            {
                pixelData = Renderer3D::ReadEntityIDFromFramebuffer(mouseX, mouseY);
            }
            else
            {
                pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
            }
            m_HoveredEntity = pixelData == -1 ? Entity() : Entity(static_cast<entt::entity>(pixelData), m_ActiveScene.get());
        }

        if (m_Is3DMode)
        {
            OnOverlayRender3D();
        }
        else
        {
            OnOverlayRender();
            m_Framebuffer->Unbind();
        }
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
#pragma warning(push)
#pragma warning(disable : 4127)
        if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
        {
            window_flags |= ImGuiWindowFlags_NoBackground;
        }
#pragma warning(pop)

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
        const f32 minWinSizeX = style.WindowMinSize.x;
        style.WindowMinSize.x = 370.0f;
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
        {
            ImGuiID const dockspace_id = ImGui::GetID("MyDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        style.WindowMinSize.x = minWinSizeX;
        UI_MenuBar();
        UI_Toolbar();
        UI_Viewport();
        UI_RendererStats();
        UI_Settings();
        UI_DebugTools();
        UI_ChildPanels();

        ImGui::End();
    }

    void EditorLayer::UI_MenuBar()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::BeginMainMenuBar();
        ImGui::PopStyleVar();

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::BeginMenu("New"))
            {
                if (ImGui::MenuItem("Project"))
                {
                    NewProject();
                }
                if (ImGui::MenuItem("Scene", "Ctrl+N"))
                {
                    NewScene();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Open..."))
            {
                if (ImGui::MenuItem("Project"))
                {
                    OpenProject();
                }
                if (ImGui::MenuItem("Scene", "Ctrl+O"))
                {
                    OpenScene();
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, m_ActiveScene != nullptr))
            {
                SaveScene();
            }

            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, m_ActiveScene != nullptr))
            {
                SaveSceneAs();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit"))
            {
                Application::Get().Close();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Script"))
        {
            if (ImGui::MenuItem("Reload assembly", "Ctrl+R"))
            {
                ScriptEngine::ReloadAssembly();
            }

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Shaders"))
        {
            if (ImGui::MenuItem("Reload shader", "Ctrl+Shift+R"))
            {
                OLO_INFO("Reloading shaders...");
                Renderer2D::GetShaderLibrary().ReloadShaders();
                OLO_INFO("Shaders reloaded!");
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Build"))
        {
            if (ImGui::MenuItem("Build Asset Pack..."))
            {
                BuildAssetPack();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Asset Pack Builder", nullptr, &m_ShowAssetPackBuilder))
            {
                // Toggle panel visibility
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Shader Debugger", nullptr, &m_ShowShaderDebugger);
            ImGui::MenuItem("GPU Resource Inspector", nullptr, &m_ShowGPUResourceInspector);
            ImGui::MenuItem("Command Bucket Inspector", nullptr, &m_ShowCommandBucketInspector);
            ImGui::MenuItem("Renderer Profiler", nullptr, &m_ShowRendererProfiler);
            ImGui::MenuItem("Render Graph Debugger", nullptr, &m_ShowRenderGraphDebugger);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window"))
        {
            ImGui::MenuItem("Animation Panel", nullptr, &m_ShowAnimationPanel);
            ImGui::MenuItem("Environment Settings", nullptr, &m_ShowEnvironmentSettings);

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    void EditorLayer::UI_Viewport()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });
        ImGui::Begin("Viewport");
        const auto viewportMinRegion = ImGui::GetWindowContentRegionMin();
        const auto viewportMaxRegion = ImGui::GetWindowContentRegionMax();
        const auto viewportOffset = ImGui::GetWindowPos();
        m_ViewportBounds[0] = { viewportMinRegion.x + viewportOffset.x, viewportMinRegion.y + viewportOffset.y };
        m_ViewportBounds[1] = { viewportMaxRegion.x + viewportOffset.x, viewportMaxRegion.y + viewportOffset.y };

        m_ViewportFocused = ImGui::IsWindowFocused();
        m_ViewportHovered = ImGui::IsWindowHovered();
        Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportHovered);

        ImVec2 const viewportPanelSize = ImGui::GetContentRegionAvail();
        m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };

        // Display appropriate framebuffer based on mode
        u64 textureID = 0;
        if (m_Is3DMode)
        {
            // Get Renderer3D's scene pass output (the FinalPass renders to screen, not a target)
            if (auto scenePass = Renderer3D::GetScenePass(); scenePass)
            {
                if (auto target = scenePass->GetTarget(); target)
                {
                    textureID = target->GetColorAttachmentRendererID(0);
                }
            }
        }
        else
        {
            textureID = m_Framebuffer->GetColorAttachmentRendererID(0);
        }
        ImGui::Image(textureID, ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                std::filesystem::path path = (const wchar_t*)payload->Data;

                if (path.extension() == ".olo") // Load scene
                {
                    m_HoveredEntity = Entity();
                    OpenScene(path);
                }
                else if (((path.extension() == ".png") || (path.extension() == ".jpeg")) && m_HoveredEntity && m_HoveredEntity.HasComponent<SpriteRendererComponent>()) // Load texture
                {
                    const Ref<Texture2D> texture = Texture2D::Create(path.string());
                    if (texture->IsLoaded())
                    {
                        m_HoveredEntity.GetComponent<SpriteRendererComponent>().Texture = texture;
                    }
                    else
                    {
                        OLO_WARN("Could not load texture {0}", path.filename().string());
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        UI_Gizmos();

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorLayer::UI_Gizmos() const
    {
        Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
        if (!selectedEntity || !selectedEntity.HasComponent<TransformComponent>())
        {
            return;
        }

        if ((m_GizmoType != -1) && (!Input::IsKeyPressed(Key::LeftAlt)))
        {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();

            ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y);

            // Editor camera
            const glm::mat4& cameraProjection = m_EditorCamera.GetProjection();
            glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();

            // Entity transform
            auto& tc = selectedEntity.GetComponent<TransformComponent>();
            glm::mat4 transform = tc.GetTransform();

            // Snapping
            const bool snap = Input::IsKeyPressed(Key::LeftControl);
            f32 snapValue = 0.5f;
            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
            {
                snapValue = 45.0f;
            }

            const std::array<f32, 3> snapValues = { snapValue, snapValue, snapValue };

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

        const f32 size = ImGui::GetWindowHeight() - 4.0f;

        ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x * 0.5f) - (size * 0.5f));

        bool hasPlayButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play;
        bool hasSimulateButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Simulate;
        bool hasPauseButton = m_SceneState != SceneState::Edit;

        if (hasPlayButton)
        {
            using enum OloEngine::EditorLayer::SceneState;
            Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Simulate)) ? m_IconPlay : m_IconStop;
            if (ImGui::ImageButton("##play_stop_icon", (u64)icon->GetRendererID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor) && toolbarEnabled)
            {
                if ((m_SceneState == Edit) || (m_SceneState == Simulate))
                {
                    OnScenePlay();
                }
                else if (m_SceneState == Play)
                {
                    OnSceneStop();
                }
            }
        }
        if (hasSimulateButton)
        {
            if (hasPlayButton)
            {
                ImGui::SameLine();
            }

            Ref<Texture2D> icon = ((m_SceneState == SceneState::Edit) || (m_SceneState == SceneState::Play)) ? m_IconSimulate : m_IconStop;
            if (ImGui::ImageButton("##simulate_stop_icon", (u64)icon->GetRendererID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor) && toolbarEnabled)
            {
                using enum OloEngine::EditorLayer::SceneState;
                if ((m_SceneState == Edit) || (m_SceneState == Play))
                {
                    OnSceneSimulate();
                }
                else if (m_SceneState == Simulate)
                {
                    OnSceneStop();
                }
            }
            if (hasPauseButton)
            {
                bool isPaused = m_ActiveScene->IsPaused();
                ImGui::SameLine();
                {
                    icon = m_IconPause;
                    if (ImGui::ImageButton("##pause_icon", (u64)icon->GetRendererID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor) && toolbarEnabled)
                    {
                        m_ActiveScene->SetPaused(!isPaused);
                    }
                }

                // Step button
                if (isPaused)
                {
                    ImGui::SameLine();
                    {
                        icon = m_IconStep;
                        isPaused = m_ActiveScene->IsPaused();
                        if (ImGui::ImageButton("##step_icon", (u64)icon->GetRendererID(), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), tintColor) && toolbarEnabled)
                        {
                            m_ActiveScene->Step();
                        }
                    }
                }
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        ImGui::End();
    }

    void EditorLayer::UI_ChildPanels()
    {
        m_SceneHierarchyPanel.OnImGuiRender();
        m_ContentBrowserPanel->OnImGuiRender();

        // Asset Pack Builder Panel
        if (m_ShowAssetPackBuilder && m_AssetPackBuilderPanel)
        {
            m_AssetPackBuilderPanel->OnImGuiRender(m_ShowAssetPackBuilder);
        }

        // Animation Panel
        if (m_ShowAnimationPanel)
        {
            m_AnimationPanel.SetSelectedEntity(m_SceneHierarchyPanel.GetSelectedEntity());
            m_AnimationPanel.OnImGuiRender();
        }

        // Environment Settings Panel
        if (m_ShowEnvironmentSettings)
        {
            m_EnvironmentSettingsPanel.SetContext(m_ActiveScene);
            m_EnvironmentSettingsPanel.OnImGuiRender();
        }
    }

    void EditorLayer::UI_Settings()
    {
        ImGui::Begin("Settings");
        ImGui::Checkbox("Show physics colliders", &m_ShowPhysicsColliders);

        // 3D Mode toggle with lazy initialization
        bool was3DMode = m_Is3DMode;
        ImGui::Checkbox("3D Mode", &m_Is3DMode);
        if (m_Is3DMode && !was3DMode)
        {
            // Lazily initialize Renderer3D when 3D mode is first enabled
            if (!Renderer3D::IsInitialized())
            {
                OLO_CORE_INFO("Initializing Renderer3D for 3D mode...");
                Renderer3D::Init();
                // Resize to current viewport size
                if (m_ViewportSize.x > 0 && m_ViewportSize.y > 0)
                {
                    Renderer3D::OnWindowResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));
                }
            }
        }

        ImGui::Separator();

        // Physics Debug Settings
        ImGui::Text("Physics Debug");
        auto& physicsSettings = Physics3DSystem::GetSettings();
        if (ImGui::Checkbox("Capture physics on play", &physicsSettings.m_CaptureOnPlay))
        {
            // Setting changed - could save project here if needed
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Enable expensive physics debug capture during play mode.\nOff by default for production performance.");
        }

        if (!s_Font)
        {
            s_Font = std::make_unique<Font>("assets/fonts/opensans/OpenSans-Regular.ttf");
        }
        ImGui::Image((ImTextureID)s_Font->GetAtlasTexture()->GetRendererID(), { 512, 512 }, { 0, 1 }, { 1, 0 });

        ImGui::End();
    }

    void EditorLayer::UI_RendererStats()
    {
        ImGui::Begin("Stats");
        std::string name = "None";
        // Validate entity belongs to current active scene before accessing components
        if (m_HoveredEntity && m_HoveredEntity.GetScene() == m_ActiveScene.get() && m_HoveredEntity.HasComponent<TagComponent>())
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
        ImGui::Text("Frame Rate: %.1f FPS", ImGui::GetIO().Framerate);
        ImGui::Text("Frame Time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
        ImGui::End();
    }

    void EditorLayer::UI_DebugTools()
    {
// Render debug tool windows if enabled
#ifdef OLO_DEBUG
        if (m_ShowShaderDebugger)
        {
            ShaderDebugger::GetInstance().RenderDebugView(&m_ShowShaderDebugger, "Shader Debugger");
        }

        if (m_ShowGPUResourceInspector)
        {
            GPUResourceInspector::GetInstance().RenderDebugView(&m_ShowGPUResourceInspector, "GPU Resource Inspector");
        }

        if (m_ShowCommandBucketInspector)
        {
            CommandPacketDebugger::GetInstance().RenderDebugView(
                Renderer3D::GetCommandBucket(), &m_ShowCommandBucketInspector, "Command Bucket Inspector");
        }

        if (m_ShowRendererProfiler)
        {
            RendererProfiler::GetInstance().RenderUI(&m_ShowRendererProfiler);
        }

        if (m_ShowRenderGraphDebugger)
        {
            static RenderGraphDebugger s_RenderGraphDebugger;
            s_RenderGraphDebugger.RenderDebugView(Renderer3D::GetRenderGraph(), &m_ShowRenderGraphDebugger, "Render Graph Debugger");
        }
#endif
    }

    void EditorLayer::OnEvent(Event& e)
    {
        m_CameraController.OnEvent(e);
        if ((m_SceneState != SceneState::Play) && m_ViewportHovered)
        {
            m_EditorCamera.OnEvent(e);
        }

        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        dispatcher.Dispatch<MouseButtonPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
        dispatcher.Dispatch<AssetReloadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetReloaded));
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
        bool editing = m_ViewportHovered && (m_SceneState == SceneState::Edit);

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
                if (control && editing)
                {
                    OnDuplicateEntity();
                }
                break;
            }

            // Gizmos
            case Key::Q:
            {
                if ((!ImGuizmo::IsUsing()) && editing)
                {
                    m_GizmoType = -1;
                }
                break;
            }
            case Key::W:
            {
                if ((!ImGuizmo::IsUsing()) && editing)
                {
                    m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
                }
                break;
            }
            case Key::E:
            {
                if ((!ImGuizmo::IsUsing()) && editing)
                {
                    m_GizmoType = ImGuizmo::OPERATION::ROTATE;
                }
                break;
            }
            case Key::R:
            {
                if (control)
                {
                    ScriptEngine::ReloadAssembly();
                }
                else
                {
                    if ((!ImGuizmo::IsUsing()) && editing)
                    {
                        m_GizmoType = ImGuizmo::OPERATION::SCALE;
                    }
                }
                break;
            }
        }
        return false;
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

        // Entity outline
        if (Entity selection = m_SceneHierarchyPanel.GetSelectedEntity(); selection)
        {
            Renderer2D::SetLineWidth(4.0f);

            if (selection.HasComponent<TransformComponent>())
            {
                auto const& tc = selection.GetComponent<TransformComponent>();

                if (selection.HasComponent<SpriteRendererComponent>())
                {
                    Renderer2D::DrawRect(tc.GetTransform(), glm::vec4(1, 1, 1, 1));
                }

                if (selection.HasComponent<CircleRendererComponent>())
                {
                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(glm::quat(tc.Rotation)) * glm::scale(glm::mat4(1.0f), tc.Scale + 0.03f);
                    Renderer2D::DrawCircle(transform, glm::vec4(1, 1, 1, 1), 0.03f);
                }

                if (selection.HasComponent<CameraComponent>())
                {
                    auto const& cc = selection.GetComponent<CameraComponent>();

                    if (cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
                    {
                        // For orthographic cameras, we can still use a rectangle as an indicator
                        glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(glm::quat(tc.Rotation)) * glm::scale(glm::mat4(1.0f), glm::vec3(cc.Camera.GetOrthographicSize(), cc.Camera.GetOrthographicSize(), 1.0f) + glm::vec3(0.03f));
                        Renderer2D::DrawRect(transform, glm::vec4(1, 1, 1, 1));
                    }
                    else if (cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                    {
                        // auto position = glm::vec3(tc.Translation.x, tc.Translation.y, 0.0f);
                        // auto size = glm::vec2(0.5f); // adjust as needed
                        //  TODO(olbu): Draw the selected camera properly once the Renderer2D can draw triangles/points
                    }
                }
            }
        }

        if (m_ShowPhysicsColliders)
        {
            if (const f64 epsilon = 1e-5; std::abs(Renderer2D::GetLineWidth() - -2.0f) > static_cast<f32>(epsilon))
            {
                Renderer2D::Flush();
                Renderer2D::SetLineWidth(2.0f);
            }

            // Calculate z index for translation
            const f32 zIndex = 0.001f;
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

                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f)) * glm::scale(glm::mat4(1.0f), scale);

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

                    const glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), glm::vec3(scale.x, scale.x, scale.z));

                    Renderer2D::DrawCircle(transform, glm::vec4(0, 1, 0, 1), 0.01f);
                }
            }
        }

        Renderer2D::EndScene();
    }

    void EditorLayer::OnOverlayRender3D() const
    {
        // In 3D mode, overlays (grid, light gizmos) are rendered as part of Scene::RenderScene3D
        // to avoid calling BeginScene/EndScene multiple times which would reset the frame.
        //
        // This function is kept for any future 3D overlay rendering that needs to happen
        // AFTER the scene has been rendered (e.g., UI overlays, debug info).
        //
        // Currently, all 3D overlays are integrated into RenderScene3D in Scene.cpp.

        // Note: Selection highlight could be done here if needed, but currently
        // we're keeping it simple by integrating everything into the scene render.
    }

    void EditorLayer::NewProject()
    {
        Project::New();
        NewScene();
        m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
        m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();
    }

    bool EditorLayer::OpenProject()
    {
        auto const cwd = std::filesystem::current_path().string();
        if (std::string filepath = FileDialogs::OpenFile("OloEngine Project (*.oloproj)\0*.oloproj\0", cwd.c_str()); !filepath.empty())
        {
            OpenProject(filepath);
            return true;
        }
        return false;
    }

    bool EditorLayer::OpenProject(const std::filesystem::path& path)
    {
        if (Project::Load(path))
        {
            auto editorAssetManager = Ref<EditorAssetManager>::Create();
            editorAssetManager->Initialize();
            Project::SetAssetManager(editorAssetManager);

            auto startScenePath = Project::GetAssetFileSystemPath(Project::GetActive()->GetConfig().StartScene);
            OLO_ASSERT(std::filesystem::exists(startScenePath));
            OpenScene(startScenePath);
            m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
            m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();
            return true;
        }
        return false;
    }

    void EditorLayer::SaveProject()
    {
        // Project::SaveActive();
    }

    void EditorLayer::NewScene()
    {
        if (m_SceneState != SceneState::Edit)
        {
            return;
        }

        Ref<Scene> newScene = Ref<Scene>::Create();
        SetEditorScene(newScene);
        m_EditorScenePath = std::filesystem::path();
    }

    void EditorLayer::OpenScene()
    {
        auto const dir = Project::GetActive()
                             ? Project::GetAssetDirectory().string()
                             : std::filesystem::current_path().string();
        std::string const filepath = FileDialogs::OpenFile("OloEditor Scene (*.olo)\0*.olo\0", dir.c_str());
        if (!filepath.empty())
        {
            OpenScene(filepath);
        }
    }

    bool EditorLayer::OpenScene(const std::filesystem::path& path)
    {
        if (m_SceneState != SceneState::Edit)
        {
            OnSceneStop();
        }

        if (path.extension().string() != ".olo")
        {
            OLO_WARN("Could not load {0} - not a scene file", path.filename().string());
            return false;
        }

        Ref<Scene> const newScene = Ref<Scene>::Create();
        if (SceneSerializer serializer(newScene); !serializer.Deserialize(path.string()))
        {
            return false;
        }
        SetEditorScene(newScene);
        m_EditorScenePath = path;
        return true;
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
        auto const dir = Project::GetActive()
                             ? Project::GetAssetDirectory().string()
                             : std::filesystem::current_path().string();
        const std::filesystem::path filepath = FileDialogs::SaveFile("OloEditor Scene (*.olo)\0*.olo\0", dir.c_str());
        if (!filepath.empty())
        {
            m_EditorScene->SetName(filepath.stem().string());
            m_EditorScenePath = filepath;

            SerializeScene(m_EditorScene, filepath);
            SyncWindowTitle();
        }
    }

    void EditorLayer::SerializeScene(Ref<Scene> const scene, const std::filesystem::path& path) const
    {
        const SceneSerializer serializer(scene);
        serializer.Serialize(path);
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
        m_AnimationPanel.SetContext(m_ActiveScene);
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
        m_AnimationPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::OnSceneStop()
    {
        using enum OloEngine::EditorLayer::SceneState;
        OLO_CORE_ASSERT(m_SceneState == Play || m_SceneState == Simulate);

        if (m_SceneState == Play)
        {
            m_ActiveScene->OnRuntimeStop();
        }
        else if (m_SceneState == Simulate)
        {
            m_ActiveScene->OnSimulationStop();
        }

        m_SceneState = Edit;

        // Reset hovered entity before changing scenes to prevent accessing stale registry
        m_HoveredEntity = Entity();

        m_ActiveScene = m_EditorScene;

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::SetEditorScene(const Ref<Scene>& scene)
    {
        OLO_CORE_ASSERT(scene, "EditorLayer ActiveScene cannot be null");

        // Reset hovered entity before changing scenes to prevent accessing stale registry
        m_HoveredEntity = Entity();

        m_EditorScene = scene;
        m_SceneHierarchyPanel.SetContext(m_EditorScene);
        m_AnimationPanel.SetContext(m_EditorScene);

        m_ActiveScene = m_EditorScene;

        SyncWindowTitle();
    }

    void EditorLayer::SyncWindowTitle() const
    {
        std::string const& projectName = Project::GetActive()->GetConfig().Name;
        std::string title = projectName + " - " + m_ActiveScene->GetName() + " - OloEditor";
        Application::Get().GetWindow().SetTitle(title);
    }

    void EditorLayer::OnScenePause()
    {
        if (m_SceneState == SceneState::Edit)
        {
            return;
        }

        m_ActiveScene->SetPaused(true);
    }

    void EditorLayer::OnDuplicateEntity()
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

    bool EditorLayer::OnAssetReloaded(AssetReloadedEvent const& e)
    {
        OLO_TRACE("🔄 Asset Reloaded Event Received!");
        OLO_TRACE("   Handle: {}", static_cast<u64>(e.GetHandle()));
        OLO_TRACE("   Type: {}", (int)e.GetAssetType());
        OLO_TRACE("   Path: {}", e.GetPath().string());

        // TODO(olbu) Add specific handling based on asset type
        switch (e.GetAssetType())
        {
            case AssetType::Texture2D:
                OLO_TRACE("   → Texture asset reloaded - visual updates may be needed");
                break;
            case AssetType::Scene:
                OLO_TRACE("   → Scene asset reloaded - consider refreshing scene hierarchy");
                break;
            case AssetType::Script:
                OLO_TRACE("   → Script asset reloaded - C# assemblies updated");
                break;
            default:
                OLO_TRACE("   → Asset type {} reloaded", (int)e.GetAssetType());
                break;
        }

        return false; // Don't consume the event, let other listeners handle it too
    }

    void EditorLayer::BuildAssetPack()
    {
        // Prevent concurrent builds
        if (m_BuildInProgress.load())
        {
            OLO_CORE_WARN("Asset Pack build already in progress, ignoring request");
            return;
        }

        OLO_CORE_INFO("Building Asset Pack...");

        // Configure build settings
        AssetPackBuilder::BuildSettings settings;
        settings.m_OutputPath = "Assets/AssetPack.olopack";
        settings.m_CompressAssets = true;
        settings.m_IncludeScriptModule = true;
        settings.m_ValidateAssets = true;

        // Reset progress and flags
        m_BuildProgress.store(0.0f);
        m_BuildCancelRequested.store(false);
        m_BuildInProgress.store(true);

        // Start async build task using Task System
        Tasks::Launch("BuildAssetPack", [this, settings]()
                      {
            try
            {
                auto result = AssetPackBuilder::BuildFromActiveProject(settings, m_BuildProgress, &m_BuildCancelRequested);

                // Mark build as complete
                m_BuildInProgress.store(false);

                if (result.m_Success && !m_BuildCancelRequested.load())
                {
                    OLO_CORE_INFO("Asset Pack built successfully!");
                    OLO_CORE_INFO("  Output: {}", result.m_OutputPath.string());
                    OLO_CORE_INFO("  Assets: {}", result.m_AssetCount);
                    OLO_CORE_INFO("  Scenes: {}", result.m_SceneCount);
                }
                else if (m_BuildCancelRequested.load())
                {
                    OLO_CORE_INFO("Asset Pack build was cancelled");
                }
                else
                {
                    OLO_CORE_ERROR("Asset Pack build failed: {}", result.m_ErrorMessage);
                }

                // Store result for potential later access
                m_LastBuildResult = result;
            }
            catch (const std::exception& ex)
            {
                m_BuildInProgress.store(false);
                OLO_CORE_ERROR("Asset Pack build exception: {}", ex.what());
                AssetPackBuilder::BuildResult errorResult{};
                errorResult.m_Success = false;
                errorResult.m_ErrorMessage = ex.what();
                errorResult.m_OutputPath.clear();
                errorResult.m_AssetCount = 0;
                errorResult.m_SceneCount = 0;
                m_LastBuildResult = errorResult;
            } }, Tasks::ETaskPriority::BackgroundNormal);

        OLO_CORE_INFO("Asset Pack build started asynchronously...");
    }

} // namespace OloEngine
