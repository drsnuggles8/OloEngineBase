#include "OloEnginePCH.h"
#include "EditorLayer.h"
#include "Panels/AssetPackBuilderPanel.h"
#include "UndoRedo/EntityCommands.h"
#include "UndoRedo/ComponentCommands.h"
#include "OloEngine/Math/Math.h"
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
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <thread>

namespace OloEngine
{
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
            // Resolve against the startup working directory so the path is
            // stable even if the CWD changes at runtime.
            const auto defaultProject = Application::Get().GetStartupWorkingDirectory() / "SandboxProject" / "Sandbox.oloproj";
            if (std::filesystem::exists(defaultProject) && OpenProject(defaultProject))
            {
                OLO_CORE_INFO("Loaded default project: {0}", defaultProject.string());
            }
            else if (!OpenProject())
            {
                Application::Get().Close();
            }
        }
        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

        // Reapply preferences loaded by OpenProject() since the camera was just reconstructed
        m_EditorCamera.SetFlySpeed(m_Prefs.CameraFlySpeed);

        // Initialize Renderer3D early so 3D code paths in OnUpdate / UI_Viewport
        // never run against an uninitialized renderer when m_Is3DMode is true.
        TryInitialize3DMode();

        // Create brush preview UBO (binding 11, 32 bytes = 2 vec4s)
        m_BrushPreviewUBO = UniformBuffer::Create(ShaderBindingLayout::BrushPreviewUBO::GetSize(), ShaderBindingLayout::UBO_BRUSH_PREVIEW);

        // Initialize save game system
        SaveGameManager::Initialize();
    }

    void EditorLayer::OnDetach()
    {
        OLO_PROFILE_FUNCTION();
        SaveGameManager::Shutdown();
    }

    void EditorLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();

        m_LastFrameTimeMs = ts.GetMilliseconds();

        // Sync with async asset loading thread
        AssetManager::SyncWithAssetThread();

        m_ActiveScene->OnViewportResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));

        const f64 epsilon = 1e-5;

        // Scale framebuffer dimensions by HiDPI factor so we render at native pixel resolution.
        // Camera and scene use logical (unscaled) coordinates for correct aspect ratio.
        const f32 dpiScale = Window::s_HighDPIScaleFactor;
        const u32 fbWidth = std::max(1u, static_cast<u32>(m_ViewportSize.x * dpiScale));
        const u32 fbHeight = std::max(1u, static_cast<u32>(m_ViewportSize.y * dpiScale));

        // Resize
        if (FramebufferSpecification const spec = m_Framebuffer->GetSpecification();
            (m_ViewportSize.x > 0.0f) && (m_ViewportSize.y > 0.0f) && // zero sized framebuffer is invalid
            ((std::abs(static_cast<f32>(spec.Width) - static_cast<f32>(fbWidth)) > epsilon) || (std::abs(static_cast<f32>(spec.Height) - static_cast<f32>(fbHeight)) > epsilon)))
        {
            m_Framebuffer->Resize(fbWidth, fbHeight);
            m_CameraController.OnResize(m_ViewportSize.x, m_ViewportSize.y);
            m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);

            // Also resize Renderer3D's render graph for 3D mode
            if (m_Is3DMode)
            {
                Renderer3D::OnWindowResize(fbWidth, fbHeight);
            }
        }

        // In edit mode, skip expensive scene rendering when the previous frame
        // exceeded the time budget.  Camera input is still processed so the
        // editor stays responsive; the viewport simply shows the last rendered
        // framebuffer until the GPU catches up.  In Play/Simulate mode, simulation
        // (physics, scripts) always runs; only rendering is skipped when throttled.
        bool const overBudget = m_LastFrameTimeMs > m_RenderBudgetMs;
        // Decide whether to skip rendering based on the active mode's throttle toggle.
        bool skipRender = false;
        if (overBudget)
        {
            switch (m_SceneState)
            {
                case SceneState::Edit:
                    skipRender = m_ThrottleEditMode;
                    break;
                case SceneState::Play:
                case SceneState::Simulate:
                    skipRender = m_ThrottlePlayMode;
                    break;
            }
        }
        m_ViewportRenderSkipped = skipRender;

        // Tell the scene whether it should execute render calls.
        // Simulation (physics, scripts, etc.) always runs regardless of this flag.
        m_ActiveScene->SetRenderingEnabled(!skipRender);

        if (!skipRender)
        {
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

            // Upload brush preview UBO for terrain shader
            {
                ShaderBindingLayout::BrushPreviewUBO brushData{};
                if (m_ShowTerrainEditor && m_TerrainEditorPanel.IsActive() && m_TerrainEditorPanel.HasBrushHit())
                {
                    brushData.BrushPosAndRadius = glm::vec4(m_TerrainEditorPanel.GetBrushWorldPos(), m_TerrainEditorPanel.GetBrushRadius());
                    brushData.BrushParams.x = 1.0f; // active
                    brushData.BrushParams.y = m_TerrainEditorPanel.GetBrushFalloff();
                    brushData.BrushParams.z = m_TerrainEditorPanel.GetEditMode() == TerrainEditMode::Paint ? 1.0f : 0.0f;
                }
                m_BrushPreviewUBO->SetData(&brushData, sizeof(brushData));
            }
        }

        // Camera updates always run so the editor stays responsive even when
        // scene rendering is throttled.
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
                m_ActiveScene->SetGridVisible(m_ShowGrid);
                m_ActiveScene->SetGridSpacing(m_GridSpacing);
                m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
                break;
            }
            case SceneState::Simulate:
            {
                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                m_ActiveScene->SetGridVisible(m_ShowGrid);
                m_ActiveScene->SetGridSpacing(m_GridSpacing);
                m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
                break;
            }
            case SceneState::Play:
            {
                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                m_ActiveScene->OnUpdateRuntime(ts);
                SaveGameManager::Tick(ts, *m_ActiveScene);
                break;
            }
        }

        if (!skipRender)
        {
            auto [mx, my] = ImGui::GetMousePos();
            mx -= m_ViewportBounds[0].x;
            my -= m_ViewportBounds[0].y;
            glm::vec2 const viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
            my = viewportSize.y - my;

            // Scale logical mouse coords to framebuffer pixel coords for entity picking
            const f32 pickDpiScale = Window::s_HighDPIScaleFactor;
            const auto mouseX = static_cast<int>(mx * pickDpiScale);

            if (const auto mouseY = static_cast<int>(my * pickDpiScale); (mouseX >= 0) && (mouseY >= 0) && (mouseX < static_cast<int>(viewportSize.x * pickDpiScale)) && (mouseY < static_cast<int>(viewportSize.y * pickDpiScale)))
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

            // Terrain editor: raycast from mouse into heightmap and update brush
            if (m_ShowTerrainEditor && m_TerrainEditorPanel.IsActive() && m_ViewportHovered && m_SceneState == SceneState::Edit)
            {
                glm::vec3 terrainHitPos{};
                bool hasTerrainHit = TerrainRaycast({ mx, my }, viewportSize, terrainHitPos);
                bool mouseDown = Input::IsMouseButtonPressed(Mouse::ButtonLeft) && !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt);
                m_TerrainEditorPanel.OnUpdate(ts, terrainHitPos, hasTerrainHit, mouseDown);
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
    }

    void EditorLayer::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        // Update window title when dirty state changes (covers panel edits via CommandHistory)
        if (bool const dirty = m_CommandHistory.IsDirty(); dirty != m_LastKnownDirtyState)
        {
            m_LastKnownDirtyState = dirty;
            SyncWindowTitle();
        }

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
        UI_Viewport();
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

            bool playMode = m_SceneState == SceneState::Play;
            if (ImGui::MenuItem("Quick Save", "F5", false, playMode))
            {
                m_SaveGamePanel.TriggerQuickSave();
            }

            if (ImGui::MenuItem("Quick Load", "F9", false, playMode))
            {
                m_SaveGamePanel.TriggerQuickLoad();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit"))
            {
                if (ConfirmDiscardChanges())
                {
                    Application::Get().Close();
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            std::string undoLabel = "Undo";
            if (m_CommandHistory.CanUndo())
            {
                undoLabel += " (" + m_CommandHistory.GetUndoDescription() + ")";
            }
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, m_CommandHistory.CanUndo()))
            {
                m_CommandHistory.Undo();
                SyncWindowTitle();
            }

            std::string redoLabel = "Redo";
            if (m_CommandHistory.CanRedo())
            {
                redoLabel += " (" + m_CommandHistory.GetRedoDescription() + ")";
            }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, m_CommandHistory.CanRedo()))
            {
                m_CommandHistory.Redo();
                SyncWindowTitle();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Preferences..."))
            {
                m_EditorPreferencesPanel.Open(m_Prefs, &m_EditorCamera);
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
            ImGui::MenuItem("Console", nullptr, &m_ShowConsolePanel);
            ImGui::MenuItem("Scene Statistics", nullptr, &m_ShowSceneStatistics);
            ImGui::MenuItem("Animation Panel", nullptr, &m_ShowAnimationPanel);
            ImGui::MenuItem("Post Process Settings", nullptr, &m_ShowPostProcessSettings);
            ImGui::MenuItem("Terrain Editor", nullptr, &m_ShowTerrainEditor);
            ImGui::MenuItem("Scene Streaming", nullptr, &m_ShowStreamingPanel);
            ImGui::MenuItem("Input Settings", nullptr, &m_ShowInputSettings);
            ImGui::MenuItem("Network Debug", nullptr, &m_ShowNetworkDebug);
            ImGui::MenuItem("Dialogue Editor", nullptr, &m_ShowDialogueEditor);
            ImGui::MenuItem("Shader Graph Editor", nullptr, &m_ShowShaderGraphEditor);
            ImGui::MenuItem("Save Game Panel", nullptr, &m_ShowSaveGamePanel);

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
            // Use post-processed output (includes tone mapping for HDR→LDR)
            if (auto postProcessPass = Renderer3D::GetPostProcessPass(); postProcessPass)
            {
                if (auto target = postProcessPass->GetTarget(); target)
                {
                    textureID = target->GetColorAttachmentRendererID(0);
                }
            }
            // Fallback to scene pass if post-process pass is not available
            if (textureID == 0)
            {
                if (auto scenePass = Renderer3D::GetScenePass(); scenePass)
                {
                    if (auto target = scenePass->GetTarget(); target)
                    {
                        textureID = target->GetColorAttachmentRendererID(0);
                    }
                }
            }
        }
        else
        {
            textureID = m_Framebuffer->GetColorAttachmentRendererID(0);
        }
        ImGui::Image(textureID, ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });

        // Play-mode visual indicator: draw colored border around viewport
        if (m_SceneState != SceneState::Edit)
        {
            ImU32 borderColor = (m_SceneState == SceneState::Play) ? IM_COL32(220, 30, 30, 255) : IM_COL32(220, 200, 30, 255);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(pMin, pMax, borderColor, 0.0f, 0, 3.0f);
        }

        // Render-throttle indicator: small badge when viewport frames are being
        // skipped to keep the editor UI responsive.
        if (m_ViewportRenderSkipped)
        {
            ImVec2 const vpMin = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 const textPos = { vpMin.x + 6.0f, vpMin.y + 4.0f };
            dl->AddRectFilled({ textPos.x - 2.0f, textPos.y - 1.0f }, { textPos.x + 86.0f, textPos.y + 15.0f }, IM_COL32(30, 30, 30, 180), 3.0f);
            dl->AddText(textPos, IM_COL32(255, 200, 60, 220), "Throttled");
        }

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                std::filesystem::path path = (const wchar_t*)payload->Data;

                if (path.extension() == ".olo") // Load scene
                {
                    if (ConfirmDiscardChanges())
                    {
                        m_HoveredEntity = Entity();
                        OpenScene(path);
                    }
                }
                else if (m_SceneState == SceneState::Edit && ((path.extension() == ".png") || (path.extension() == ".jpeg")) && m_HoveredEntity && m_HoveredEntity.HasComponent<SpriteRendererComponent>()) // Load texture
                {
                    const Ref<Texture2D> texture = Texture2D::Create(path.string());
                    if (texture->IsLoaded())
                    {
                        auto oldComponent = m_HoveredEntity.GetComponent<SpriteRendererComponent>();
                        m_HoveredEntity.GetComponent<SpriteRendererComponent>().Texture = texture;
                        auto newComponent = m_HoveredEntity.GetComponent<SpriteRendererComponent>();
                        m_CommandHistory.PushAlreadyExecuted(
                            std::make_unique<ComponentChangeCommand<SpriteRendererComponent>>(
                                m_EditorScene, m_HoveredEntity.GetUUID(), oldComponent, newComponent));
                    }
                    else
                    {
                        OLO_WARN("Could not load texture {0}", path.filename().string());
                    }
                }
                else if (m_SceneState == SceneState::Edit && path.extension() == ".oloprefab") // Instantiate prefab
                {
                    auto* editorManager = static_cast<EditorAssetManager*>(
                        Project::GetActive()->GetAssetManager().get());
                    AssetHandle handle = editorManager->ImportAsset(path);
                    if (handle)
                    {
                        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(handle);
                        if (prefab)
                        {
                            Entity instance = prefab->Instantiate(*m_EditorScene);
                            if (instance)
                            {
                                m_SceneHierarchyPanel.SetSelectedEntity(instance);

                                // Record undo: the entity already exists, so wrap a DeleteEntityCommand
                                // with DuplicateUndoCommand (undo = delete, redo = restore).
                                m_CommandHistory.PushAlreadyExecuted(
                                    std::make_unique<DuplicateUndoCommand>(
                                        std::make_unique<DeleteEntityCommand>(
                                            m_EditorScene, instance,
                                            [this]()
                                            { m_SceneHierarchyPanel.ClearSelection(); },
                                            [this](Entity restored)
                                            { m_SceneHierarchyPanel.SetSelectedEntity(restored); })));
                            }
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        UI_Gizmos();

        // Toolbar overlay inside viewport
        UI_Toolbar();

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorLayer::UI_Gizmos()
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
            f32 snapValue = m_TranslateSnap;
            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
            {
                snapValue = m_RotateSnap;
            }
            else if (m_GizmoType == ImGuizmo::OPERATION::SCALE)
            {
                snapValue = m_ScaleSnap;
            }

            const std::array<f32, 3> snapValues = { snapValue, snapValue, snapValue };

            ImGuizmo::Manipulate(glm::value_ptr(cameraView),
                                 glm::value_ptr(cameraProjection),
                                 static_cast<ImGuizmo::OPERATION>(m_GizmoType),
                                 ImGuizmo::LOCAL,
                                 glm::value_ptr(transform),
                                 nullptr,
                                 snap ? snapValues.data() : nullptr);

            const bool isUsing = ImGuizmo::IsUsing();

            // Capture transform at the start of gizmo interaction
            if (isUsing && !m_GizmoWasUsing)
            {
                m_GizmoStartTranslation = tc.Translation;
                m_GizmoStartRotation = tc.Rotation;
                m_GizmoStartScale = tc.Scale;
            }

            if (isUsing)
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

            // Push undo command when gizmo interaction ends
            if (!isUsing && m_GizmoWasUsing && m_SceneState == SceneState::Edit)
            {
                m_CommandHistory.PushAlreadyExecuted(std::make_unique<TransformChangeCommand>(
                    m_EditorScene, selectedEntity.GetUUID(),
                    m_GizmoStartTranslation, m_GizmoStartRotation, m_GizmoStartScale,
                    tc.Translation, tc.Rotation, tc.Scale));
            }

            m_GizmoWasUsing = isUsing;
        }
    }

    void EditorLayer::UI_Toolbar()
    {
        const auto toolbarEnabled = static_cast<bool>(m_ActiveScene);

        // Determine which buttons to show
        bool const hasPlayButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Play;
        bool const hasSimulateButton = m_SceneState == SceneState::Edit || m_SceneState == SceneState::Simulate;
        bool const hasPauseButton = m_SceneState != SceneState::Edit;
        bool const isPaused = hasPauseButton && m_ActiveScene && m_ActiveScene->IsPaused();

        // Count visible buttons
        int buttonCount = 0;
        if (hasPlayButton)
        {
            buttonCount++;
        }
        if (hasSimulateButton)
        {
            buttonCount++;
        }
        if (hasPauseButton)
        {
            buttonCount++;
        }
        if (isPaused)
        {
            buttonCount++;
        }
        if (buttonCount == 0)
        {
            return;
        }

        constexpr f32 buttonSize = 24.0f;
        constexpr f32 buttonSpacing = 4.0f;
        constexpr f32 padding = 8.0f;
        f32 const toolbarWidth = buttonCount * buttonSize + (buttonCount - 1) * buttonSpacing + padding * 2.0f;
        constexpr f32 toolbarHeight = buttonSize + padding * 2.0f;

        // Position at top-center of viewport content area
        ImVec2 const viewportMin = ImGui::GetWindowContentRegionMin();
        ImVec2 const viewportMax = ImGui::GetWindowContentRegionMax();
        f32 const viewportWidth = viewportMax.x - viewportMin.x;
        f32 const toolbarX = viewportMin.x + (viewportWidth - toolbarWidth) * 0.5f;
        constexpr f32 topMargin = 6.0f;
        f32 const toolbarY = viewportMin.y + topMargin;

        // Draw semi-transparent background
        ImVec2 const windowPos = ImGui::GetWindowPos();
        ImVec2 const bgMin = { windowPos.x + toolbarX, windowPos.y + toolbarY };
        ImVec2 const bgMax = { bgMin.x + toolbarWidth, bgMin.y + toolbarHeight };
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(bgMin, bgMax, IM_COL32(30, 30, 30, 180), 6.0f);
        drawList->AddRect(bgMin, bgMax, IM_COL32(60, 60, 60, 200), 6.0f);

        // Set cursor for buttons
        ImGui::SetCursorPos({ toolbarX + padding, toolbarY + padding });

        // Style: transparent button background
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(buttonSpacing, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        auto const& colors = ImGui::GetStyle().Colors;
        auto const& buttonHovered = colors[ImGuiCol_ButtonHovered];
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
        auto const& buttonActive = colors[ImGuiCol_ButtonActive];
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

        auto tintColor = ImVec4(1, 1, 1, 1);
        if (!toolbarEnabled)
        {
            tintColor.w = 0.5f;
        }

        ImVec2 const btnSize(buttonSize, buttonSize);

        // Play / Stop button
        if (hasPlayButton)
        {
            using enum OloEngine::EditorLayer::SceneState;
            Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Simulate)) ? m_IconPlay : m_IconStop;
            if (ImGui::ImageButton("##play_stop_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
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
            ImGui::SameLine();
        }

        // Simulate / Stop button
        if (hasSimulateButton)
        {
            using enum OloEngine::EditorLayer::SceneState;
            Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Play)) ? m_IconSimulate : m_IconStop;
            if (ImGui::ImageButton("##simulate_stop_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
            {
                if ((m_SceneState == Edit) || (m_SceneState == Play))
                {
                    OnSceneSimulate();
                }
                else if (m_SceneState == Simulate)
                {
                    OnSceneStop();
                }
            }
            ImGui::SameLine();
        }

        // Pause button
        if (hasPauseButton)
        {
            Ref<Texture2D> const icon = m_IconPause;
            if (ImGui::ImageButton("##pause_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
            {
                m_ActiveScene->SetPaused(!isPaused);
            }
            ImGui::SameLine();
        }

        // Step button (only when paused)
        if (isPaused)
        {
            Ref<Texture2D> const icon = m_IconStep;
            if (ImGui::ImageButton("##step_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
            {
                m_ActiveScene->Step();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
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
            m_AnimationPanel.OnImGuiRender(&m_ShowAnimationPanel);
        }

        // Post Process Settings Panel
        if (m_ShowPostProcessSettings)
        {
            m_PostProcessSettingsPanel.OnImGuiRender(&m_ShowPostProcessSettings);
        }

        // Terrain Editor Panel
        if (m_ShowTerrainEditor)
        {
            m_TerrainEditorPanel.SetContext(m_ActiveScene);
            m_TerrainEditorPanel.OnImGuiRender();
            m_ShowTerrainEditor = m_TerrainEditorPanel.Visible;
        }

        // Streaming Panel
        if (m_ShowStreamingPanel)
        {
            m_StreamingPanel.OnImGuiRender(&m_ShowStreamingPanel);
        }

        // Input Settings Panel
        if (m_ShowInputSettings)
        {
            m_InputSettingsPanel.OnImGuiRender(&m_ShowInputSettings);
        }

        // Network Debug Panel
        if (m_ShowNetworkDebug)
        {
            m_NetworkDebugPanel.OnImGuiRender(&m_ShowNetworkDebug);
        }

        // Dialogue Editor Panel
        if (m_ShowDialogueEditor)
        {
            m_DialogueEditorPanel.OnImGuiRender();
            m_ShowDialogueEditor = m_DialogueEditorPanel.IsOpen();
        }

        // Shader Graph Editor Panel
        if (m_ShowShaderGraphEditor)
        {
            m_ShaderGraphEditorPanel.SetOpen(true);
            m_ShaderGraphEditorPanel.OnImGuiRender();
            m_ShowShaderGraphEditor = m_ShaderGraphEditorPanel.IsOpen();
        }

        // Save Game Panel
        if (m_ShowSaveGamePanel)
        {
            m_SaveGamePanel.OnImGuiRender(&m_ShowSaveGamePanel);
        }

        // Console Panel
        if (m_ShowConsolePanel)
        {
            m_ConsolePanel.OnImGuiRender(&m_ShowConsolePanel);
        }

        // Scene Statistics Panel
        if (m_ShowSceneStatistics)
        {
            m_SceneStatisticsPanel.SetHoveredEntity(m_HoveredEntity);
            m_SceneStatisticsPanel.OnImGuiRender(&m_ShowSceneStatistics);
        }

        // Editor Preferences Dialog
        if (m_EditorPreferencesPanel.OnImGuiRender(m_Prefs))
        {
            ApplyPreferences();
        }
    }

    void EditorLayer::ApplyDefault3DCameraPose()
    {
        OLO_PROFILE_FUNCTION();

        // Elevated and looking slightly down so the infinite grid on the XZ
        // plane is visible.  Without this the camera sits at Y=0 with zero
        // pitch, making every view ray parallel to the grid plane.
        m_EditorCamera.SetPosition({ 0.0f, 5.0f, 10.0f });
        m_EditorCamera.SetPitch(-0.4f);
        m_EditorCamera.SetYaw(0.0f);
    }

    void EditorLayer::TryInitialize3DMode()
    {
        if (!m_Is3DMode || Renderer3D::IsInitialized())
        {
            return;
        }

        OLO_PROFILE_SCOPE("EditorLayer::TryInitialize3DMode");
        OLO_PROFILE_RENDERER_SCOPE("3DInit");
        OLO_CORE_INFO("Initializing Renderer3D for 3D mode...");
        Renderer3D::Init();
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);

        // Resize to current viewport size
        if (m_ViewportSize.x > 0 && m_ViewportSize.y > 0)
        {
            const f32 dpi = Window::s_HighDPIScaleFactor;
            Renderer3D::OnWindowResize(
                std::max(1u, static_cast<u32>(m_ViewportSize.x * dpi)),
                std::max(1u, static_cast<u32>(m_ViewportSize.y * dpi)));
        }

        ApplyDefault3DCameraPose();
    }

    void EditorLayer::ApplyPreferences()
    {
        m_ShowGrid = m_Prefs.ShowGrid;
        m_GridSpacing = m_Prefs.GridSpacing;
        m_TranslateSnap = m_Prefs.TranslateSnap;
        m_RotateSnap = m_Prefs.RotateSnap;
        m_ScaleSnap = m_Prefs.ScaleSnap;
        m_ShowPhysicsColliders = m_Prefs.ShowPhysicsColliders;
        m_Is3DMode = m_Prefs.Is3DMode;
        m_EditorCamera.SetFlySpeed(m_Prefs.CameraFlySpeed);
        m_ThrottleEditMode = m_Prefs.ThrottleEditMode;
        m_ThrottlePlayMode = m_Prefs.ThrottlePlayMode;
        m_RenderBudgetMs = m_Prefs.RenderBudgetMs;

        auto& physicsSettings = Physics3DSystem::GetSettings();
        physicsSettings.m_CaptureOnPlay = m_Prefs.CapturePhysicsOnPlay;

        if (m_Is3DMode && !Renderer3D::IsInitialized())
        {
            TryInitialize3DMode();
        }
    }

    void EditorLayer::SyncPrefsFromMembers()
    {
        m_Prefs.ShowGrid = m_ShowGrid;
        m_Prefs.GridSpacing = m_GridSpacing;
        m_Prefs.TranslateSnap = m_TranslateSnap;
        m_Prefs.RotateSnap = m_RotateSnap;
        m_Prefs.ScaleSnap = m_ScaleSnap;
        m_Prefs.ShowPhysicsColliders = m_ShowPhysicsColliders;
        m_Prefs.Is3DMode = m_Is3DMode;
        m_Prefs.CameraFlySpeed = m_EditorCamera.GetFlySpeed();
        m_Prefs.CapturePhysicsOnPlay = Physics3DSystem::GetSettings().m_CaptureOnPlay;
        m_Prefs.ThrottleEditMode = m_ThrottleEditMode;
        m_Prefs.ThrottlePlayMode = m_ThrottlePlayMode;
        m_Prefs.RenderBudgetMs = m_RenderBudgetMs;
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
        // Forward events to input settings panel for rebinding capture
        m_InputSettingsPanel.OnEvent(e);
        if (e.Handled)
        {
            return;
        }

        m_CameraController.OnEvent(e);
        if ((m_SceneState != SceneState::Play) && m_ViewportHovered)
        {
            m_EditorCamera.OnEvent(e);
        }

        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
        dispatcher.Dispatch<MouseButtonPressedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
        dispatcher.Dispatch<AssetReloadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetReloaded));
        dispatcher.Dispatch<WindowCloseEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnWindowClose));
    }

    bool EditorLayer::OnKeyPressed(KeyPressedEvent const& e)
    {
        // Shortcuts
        if (e.IsRepeat())
        {
            return false;
        }

        // Don't intercept shortcuts while ImGui text widgets have focus
        if (ImGui::GetIO().WantTextInput)
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

            // Undo/Redo
            case Key::Z:
            {
                if (control && m_SceneState == SceneState::Edit)
                {
                    if (m_ShowShaderGraphEditor && m_ShaderGraphEditorPanel.IsOpen() && m_ShaderGraphEditorPanel.IsFocused())
                        m_ShaderGraphEditorPanel.Undo();
                    else
                        m_CommandHistory.Undo();
                    SyncWindowTitle();
                }
                break;
            }
            case Key::Y:
            {
                if (control && m_SceneState == SceneState::Edit)
                {
                    if (m_ShowShaderGraphEditor && m_ShaderGraphEditorPanel.IsOpen() && m_ShaderGraphEditorPanel.IsFocused())
                        m_ShaderGraphEditorPanel.Redo();
                    else
                        m_CommandHistory.Redo();
                    SyncWindowTitle();
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
            case Key::C:
            {
                if (control && m_SceneState == SceneState::Edit)
                {
                    OnCopyEntity();
                }
                break;
            }
            case Key::V:
            {
                if (control && m_SceneState == SceneState::Edit)
                {
                    OnPasteEntity();
                }
                break;
            }

            // Gizmos
            case Key::Q:
            {
                if ((!ImGuizmo::IsUsing()) && editing && !m_EditorCamera.IsFlying())
                {
                    m_GizmoType = -1;
                }
                break;
            }
            case Key::W:
            {
                if ((!ImGuizmo::IsUsing()) && editing && !m_EditorCamera.IsFlying())
                {
                    m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
                }
                break;
            }
            case Key::E:
            {
                if ((!ImGuizmo::IsUsing()) && editing && !m_EditorCamera.IsFlying())
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

            // Save Game shortcuts (only in Play mode)
            case Key::F5:
            {
                if (m_SceneState == SceneState::Play)
                {
                    m_SaveGamePanel.TriggerQuickSave();
                }
                break;
            }
            case Key::F9:
            {
                if (m_SceneState == SceneState::Play)
                {
                    m_SaveGamePanel.TriggerQuickLoad();
                }
                break;
            }

            // Entity deletion
            case Key::Delete:
            {
                if (m_SceneState == SceneState::Edit)
                {
                    const auto& selected = m_SceneHierarchyPanel.GetSelectedEntities();
                    if (selected.size() > 1)
                    {
                        auto compound = std::make_unique<CompoundCommand>("Delete " + std::to_string(selected.size()) + " Entities");
                        for (auto& entity : selected)
                        {
                            compound->Add(std::make_unique<DeleteEntityCommand>(
                                m_EditorScene, entity,
                                []() {},
                                [](Entity) {}));
                        }
                        m_CommandHistory.Execute(std::move(compound));
                        m_SceneHierarchyPanel.ClearSelection();
                    }
                    else if (!selected.empty())
                    {
                        Entity selectedEntity = selected[0];
                        m_CommandHistory.Execute(std::make_unique<DeleteEntityCommand>(
                            m_EditorScene, selectedEntity,
                            [this]()
                            { m_SceneHierarchyPanel.ClearSelection(); },
                            [this](Entity restored)
                            { m_SceneHierarchyPanel.SetSelectedEntity(restored); }));
                    }
                }
                break;
            }
        }
        return false;
    }

    bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent const& e)
    {
        // When terrain editor is active, consume left-click for brush application
        if (m_ShowTerrainEditor && m_TerrainEditorPanel.IsActive() && e.GetMouseButton() == Mouse::ButtonLeft && m_ViewportHovered && !Input::IsKeyPressed(Key::LeftAlt))
        {
            return true;
        }

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
        const auto& selectedEntities = m_SceneHierarchyPanel.GetSelectedEntities();
        if (!selectedEntities.empty())
        {
            Renderer2D::SetLineWidth(4.0f);

            for (const auto& selection : selectedEntities)
            {
                if (!selection || !selection.HasComponent<TransformComponent>())
                {
                    continue;
                }

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
                        glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(glm::quat(tc.Rotation)) * glm::scale(glm::mat4(1.0f), glm::vec3(cc.Camera.GetOrthographicSize(), cc.Camera.GetOrthographicSize(), 1.0f) + glm::vec3(0.03f));
                        Renderer2D::DrawRect(transform, glm::vec4(1, 1, 1, 1));
                    }
                    else if (cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                    {
                        // TODO(olbu): Draw the selected camera properly once the Renderer2D can draw triangles/points
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

    void EditorLayer::BindContentBrowserSelectionCallback()
    {
        m_ContentBrowserPanel->SetAssetSelectedCallback([this](const std::filesystem::path& path, ContentFileType type)
                                                        {
            if (type == ContentFileType::Dialogue)
            {
                m_DialogueEditorPanel.OpenDialogue(path);
                m_ShowDialogueEditor = true;
            }
            else if (type == ContentFileType::ShaderGraph)
            {
                if (m_ShaderGraphEditorPanel.HasUnsavedChanges())
                {
                    auto const result = MessagePrompt::YesNoCancel(
                        "Unsaved Shader Graph",
                        "The current shader graph has unsaved changes. Do you want to save before opening a new one?");

                    switch (result)
                    {
                        case MessagePromptResult::Yes:
                            m_ShaderGraphEditorPanel.SaveIfNeeded();
                            break;
                        case MessagePromptResult::Cancel:
                            return;
                        case MessagePromptResult::No:
                        default:
                            break;
                    }
                }
                m_ShaderGraphEditorPanel.OpenShaderGraph(path);
                m_ShowShaderGraphEditor = true;
            }
            else if (type == ContentFileType::Scene)
            {
                if (ConfirmDiscardChanges())
                {
                    OpenScene(path);
                }
            } });
    }

    void EditorLayer::NewProject()
    {
        if (!ConfirmDiscardChanges())
        {
            return;
        }

        if (m_SceneState != SceneState::Edit)
        {
            OnSceneStop();
        }

        Project::New();
        NewScene();
        m_DialogueEditorPanel.NewDialogue();
        m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
        BindContentBrowserSelectionCallback();
        m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();
    }

    bool EditorLayer::OpenProject()
    {
        if (!ConfirmDiscardChanges())
        {
            return false;
        }
        std::error_code ec;
        auto const cwd = std::filesystem::current_path(ec).string();
        const char* initialDir = ec ? nullptr : cwd.c_str();
        if (std::string filepath = FileDialogs::OpenFile("OloEngine Project (*.oloproj)\0*.oloproj\0", initialDir); !filepath.empty())
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
            m_DialogueEditorPanel.NewDialogue();
            m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
            BindContentBrowserSelectionCallback();
            m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();

            // Load input action map if one exists for this project
            auto inputMapPath = Project::GetInputActionMapPath();
            if (std::filesystem::exists(inputMapPath))
            {
                auto loadedMap = InputActionSerializer::Deserialize(inputMapPath);
                if (loadedMap)
                {
                    InputActionManager::SetActionMap(*loadedMap);
                }
            }

            // Load editor preferences
            m_EditorPreferencesPanel.Load(m_Prefs, Project::GetProjectDirectory());
            ApplyPreferences();

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

        if (!ConfirmDiscardChanges())
        {
            return;
        }

        Ref<Scene> newScene = Ref<Scene>::Create();
        SetEditorScene(newScene);
        m_EditorScenePath = std::filesystem::path();
    }

    void EditorLayer::OpenScene()
    {
        if (!ConfirmDiscardChanges())
        {
            return;
        }
        std::error_code ec;
        auto const dir = Project::GetActive()
                             ? Project::GetAssetDirectory().string()
                             : std::filesystem::current_path(ec).string();
        const char* initialDir = ec ? nullptr : dir.c_str();
        std::string const filepath = FileDialogs::OpenFile("OloEditor Scene (*.olo)\0*.olo\0", initialDir);
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
        Renderer3D::GetPostProcessSettings() = newScene->GetPostProcessSettings();
        Renderer3D::GetSnowSettings() = newScene->GetSnowSettings();
        Renderer3D::GetWindSettings() = newScene->GetWindSettings();
        Renderer3D::GetSnowAccumulationSettings() = newScene->GetSnowAccumulationSettings();
        Renderer3D::GetSnowEjectaSettings() = newScene->GetSnowEjectaSettings();
        Renderer3D::GetPrecipitationSettings() = newScene->GetPrecipitationSettings();
        Renderer3D::GetFogSettings() = newScene->GetFogSettings();
        return true;
    }

    bool EditorLayer::SaveScene()
    {
        if (!m_EditorScenePath.empty())
        {
            m_EditorScene->SetPostProcessSettings(Renderer3D::GetPostProcessSettings());
            m_EditorScene->SetSnowSettings(Renderer3D::GetSnowSettings());
            m_EditorScene->SetWindSettings(Renderer3D::GetWindSettings());
            m_EditorScene->SetSnowAccumulationSettings(Renderer3D::GetSnowAccumulationSettings());
            m_EditorScene->SetSnowEjectaSettings(Renderer3D::GetSnowEjectaSettings());
            m_EditorScene->SetPrecipitationSettings(Renderer3D::GetPrecipitationSettings());
            m_EditorScene->SetFogSettings(Renderer3D::GetFogSettings());
            SerializeScene(m_EditorScene, m_EditorScenePath);
            m_CommandHistory.MarkSaved();
            SyncWindowTitle();

            // Save editor preferences alongside scene
            SyncPrefsFromMembers();
            if (Project::GetActive())
            {
                m_EditorPreferencesPanel.Save(m_Prefs, Project::GetProjectDirectory());
            }
            return true;
        }

        return SaveSceneAs();
    }

    bool EditorLayer::SaveSceneAs()
    {
        std::error_code ec;
        auto const dir = Project::GetActive()
                             ? Project::GetAssetDirectory().string()
                             : std::filesystem::current_path(ec).string();
        const char* initialDir = ec ? nullptr : dir.c_str();
        const std::filesystem::path filepath = FileDialogs::SaveFile("OloEditor Scene (*.olo)\0*.olo\0", initialDir);
        if (filepath.empty())
        {
            return false;
        }

        m_EditorScene->SetName(filepath.stem().string());
        m_EditorScenePath = filepath;

        m_EditorScene->SetPostProcessSettings(Renderer3D::GetPostProcessSettings());
        m_EditorScene->SetSnowSettings(Renderer3D::GetSnowSettings());
        m_EditorScene->SetWindSettings(Renderer3D::GetWindSettings());
        m_EditorScene->SetSnowAccumulationSettings(Renderer3D::GetSnowAccumulationSettings());
        m_EditorScene->SetSnowEjectaSettings(Renderer3D::GetSnowEjectaSettings());
        m_EditorScene->SetPrecipitationSettings(Renderer3D::GetPrecipitationSettings());
        m_EditorScene->SetFogSettings(Renderer3D::GetFogSettings());
        SerializeScene(m_EditorScene, filepath);
        m_CommandHistory.MarkSaved();
        SyncWindowTitle();

        // Save editor preferences alongside scene
        SyncPrefsFromMembers();
        if (Project::GetActive())
        {
            m_EditorPreferencesPanel.Save(m_Prefs, Project::GetProjectDirectory());
        }
        return true;
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

        // Validate that the scene has a primary camera before starting runtime
        Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
        if (!cameraEntity)
        {
            OLO_CORE_ERROR("Cannot enter Play mode: no entity with a primary CameraComponent found in the scene. "
                           "Add an entity with a CameraComponent and set Primary = true.");
            m_ActiveScene = m_EditorScene;
            m_SceneState = SceneState::Edit;
            return;
        }

        // Warn about orthographic cameras in 3D mode (common misconfiguration)
        if (m_Is3DMode)
        {
            auto& cam = cameraEntity.GetComponent<CameraComponent>();
            if (cam.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
            {
                OLO_CORE_WARN("Primary camera '{}' uses Orthographic projection in 3D mode. "
                              "This may cause the viewport to appear empty. Consider switching to Perspective.",
                              cameraEntity.GetName());
            }
        }

        m_ActiveScene->OnRuntimeStart();

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_SceneHierarchyPanel.SetCommandHistory(nullptr);
        m_AnimationPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetCommandHistory(nullptr);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_SaveGamePanel.SetContext(m_ActiveScene, m_Framebuffer);
        m_StreamingPanel.SetCommandHistory(nullptr);
        m_SceneStatisticsPanel.SetContext(m_ActiveScene);
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
        m_SceneHierarchyPanel.SetCommandHistory(nullptr);
        m_AnimationPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetCommandHistory(nullptr);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_StreamingPanel.SetCommandHistory(nullptr);
        m_SceneStatisticsPanel.SetContext(m_ActiveScene);
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
        m_SceneHierarchyPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetCommandHistory(&m_CommandHistory);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_SaveGamePanel.SetContext(nullptr, nullptr);
        m_StreamingPanel.SetCommandHistory(&m_CommandHistory);
        m_SceneStatisticsPanel.SetContext(m_ActiveScene);
    }

    void EditorLayer::SetEditorScene(const Ref<Scene>& scene)
    {
        OLO_CORE_ASSERT(scene, "EditorLayer ActiveScene cannot be null");

        // Reset hovered entity before changing scenes to prevent accessing stale registry
        m_HoveredEntity = Entity();

        m_EditorScene = scene;
        m_SceneHierarchyPanel.SetContext(m_EditorScene);
        m_SceneHierarchyPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationPanel.SetContext(m_EditorScene);
        m_AnimationPanel.SetCommandHistory(&m_CommandHistory);
        m_PostProcessSettingsPanel.SetCommandHistory(&m_CommandHistory);
        m_TerrainEditorPanel.SetContext(m_EditorScene);
        m_TerrainEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_StreamingPanel.SetContext(m_EditorScene);
        m_StreamingPanel.SetCommandHistory(&m_CommandHistory);
        m_SceneStatisticsPanel.SetContext(m_EditorScene);
        m_DialogueEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_InputSettingsPanel.SetCommandHistory(&m_CommandHistory);

        m_ActiveScene = m_EditorScene;

        // Clear undo history when switching scenes
        m_CommandHistory.Clear();

        SyncWindowTitle();
    }

    void EditorLayer::SyncWindowTitle() const
    {
        std::string const& projectName = Project::GetActive()->GetConfig().Name;
        std::string title = projectName + " - " + m_ActiveScene->GetName() + " - OloEditor";
        if (m_CommandHistory.IsDirty())
        {
            title += " *";
        }
        Application::Get().GetWindow().SetTitle(title);
    }

    bool EditorLayer::ConfirmDiscardChanges()
    {
        if (!m_CommandHistory.IsDirty())
        {
            return true;
        }

        auto const result = MessagePrompt::YesNoCancel(
            "Unsaved Changes",
            "The current scene has unsaved changes. Do you want to save before continuing?");

        switch (result)
        {
            case MessagePromptResult::Yes:
                return SaveScene();
            case MessagePromptResult::No:
                return true;
            case MessagePromptResult::Cancel:
            default:
                return false;
        }
    }

    bool EditorLayer::OnWindowClose([[maybe_unused]] WindowCloseEvent const& e)
    {
        if (!ConfirmDiscardChanges())
        {
            Application::Get().CancelClose();
            return true;
        }
        return false;
    }

    bool EditorLayer::TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const
    {
        OLO_PROFILE_FUNCTION();

        if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
        {
            return false;
        }

        // Find a terrain entity in the active scene
        Entity terrainEntity;
        auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        for (auto entityID : view)
        {
            terrainEntity = Entity(entityID, m_ActiveScene.get());
            break;
        }
        if (!terrainEntity || !terrainEntity.GetComponent<TerrainComponent>().m_TerrainData)
        {
            return false;
        }

        auto const& tc = terrainEntity.GetComponent<TerrainComponent>();
        auto const& transform = terrainEntity.GetComponent<TransformComponent>();

        // Convert mouse position to NDC [-1, 1]
        f32 ndcX = (mousePos.x / viewportSize.x) * 2.0f - 1.0f;
        f32 ndcY = (mousePos.y / viewportSize.y) * 2.0f - 1.0f;

        // Unproject near and far points
        glm::mat4 invVP = glm::inverse(m_EditorCamera.GetViewProjection());
        glm::vec4 nearNDC(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farNDC(ndcX, ndcY, 1.0f, 1.0f);

        glm::vec4 nearWorld = invVP * nearNDC;
        glm::vec4 farWorld = invVP * farNDC;
        nearWorld /= nearWorld.w;
        farWorld /= farWorld.w;

        glm::vec3 rayOrigin(nearWorld);
        glm::vec3 rayDir = glm::normalize(glm::vec3(farWorld) - glm::vec3(nearWorld));

        // Step along ray to find heightmap intersection
        // Terrain origin is at entity transform position
        glm::vec3 terrainOrigin = transform.Translation;
        f32 worldSizeX = tc.m_WorldSizeX;
        f32 worldSizeZ = tc.m_WorldSizeZ;
        f32 heightScale = tc.m_HeightScale;

        if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        {
            return false;
        }

        // Coarse march along ray (step size = 1 world unit)
        constexpr f32 stepSize = 1.0f;
        constexpr f32 maxDist = 2000.0f;
        constexpr i32 refinementSteps = 8;

        f32 t = 0.0f;
        bool wasAbove = true;
        for (; t < maxDist; t += stepSize)
        {
            glm::vec3 p = rayOrigin + rayDir * t;

            // Convert world position to terrain normalized coords [0,1]
            f32 normX = (p.x - terrainOrigin.x) / worldSizeX;
            f32 normZ = (p.z - terrainOrigin.z) / worldSizeZ;

            // Outside terrain bounds
            if (normX < 0.0f || normX > 1.0f || normZ < 0.0f || normZ > 1.0f)
            {
                continue;
            }

            f32 terrainHeight = terrainOrigin.y + tc.m_TerrainData->GetHeightAt(normX, normZ) * heightScale;
            bool isAbove = p.y > terrainHeight;

            if (!isAbove && wasAbove)
            {
                // Binary search refinement between t-stepSize and t
                f32 lo = t - stepSize;
                f32 hi = t;
                for (int i = 0; i < refinementSteps; ++i)
                {
                    f32 mid = (lo + hi) * 0.5f;
                    glm::vec3 mp = rayOrigin + rayDir * mid;
                    f32 mnx = (mp.x - terrainOrigin.x) / worldSizeX;
                    f32 mnz = (mp.z - terrainOrigin.z) / worldSizeZ;
                    mnx = glm::clamp(mnx, 0.0f, 1.0f);
                    mnz = glm::clamp(mnz, 0.0f, 1.0f);
                    f32 th = terrainOrigin.y + tc.m_TerrainData->GetHeightAt(mnx, mnz) * heightScale;
                    if (mp.y > th)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                glm::vec3 hitP = rayOrigin + rayDir * ((lo + hi) * 0.5f);
                outHitPos = hitP;
                return true;
            }
            wasAbove = isAbove;
        }
        return false;
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
            Entity newEntity = m_EditorScene->DuplicateEntity(selectedEntity);

            // Snapshot the new entity so undo can delete it and redo can restore it
            auto deleteCmd = std::make_unique<DeleteEntityCommand>(
                m_EditorScene, newEntity,
                [this]()
                { m_SceneHierarchyPanel.SetSelectedEntity({}); },
                [this](Entity restored)
                { m_SceneHierarchyPanel.SetSelectedEntity(restored); });

            m_CommandHistory.PushAlreadyExecuted(
                std::make_unique<DuplicateUndoCommand>(std::move(deleteCmd)));

            m_SceneHierarchyPanel.SetSelectedEntity(newEntity);
        }
    }

    void EditorLayer::OnCopyEntity()
    {
        const auto& selected = m_SceneHierarchyPanel.GetSelectedEntities();
        if (selected.empty())
        {
            return;
        }

        YAML::Emitter out;
        out << YAML::BeginSeq;
        for (const auto& entity : selected)
        {
            SceneSerializer::SerializeEntity(out, entity);
        }
        out << YAML::EndSeq;
        m_EntityClipboard = out.c_str();
    }

    void EditorLayer::OnPasteEntity()
    {
        if (m_EntityClipboard.empty())
        {
            return;
        }

        YAML::Node entities;
        try
        {
            entities = YAML::Load(m_EntityClipboard);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("OnPasteEntity: failed to parse clipboard YAML: {}", e.what());
            return;
        }
        if (!entities || !entities.IsSequence())
        {
            return;
        }

        // Build old→new UUID map for all entities
        std::unordered_map<u64, u64> uuidMap;
        for (auto entityNode : entities)
        {
            if (entityNode["Entity"])
            {
                u64 oldUUID = entityNode["Entity"].as<u64>();
                uuidMap[oldUUID] = static_cast<u64>(UUID());
            }
        }

        // Recursively remap UUIDs in all entity data (hierarchy refs, component refs, etc.)
        std::function<void(YAML::Node)> remapUUIDs = [&](YAML::Node node)
        {
            if (node.IsScalar())
            {
                try
                {
                    u64 val = node.as<u64>();
                    if (auto it = uuidMap.find(val); it != uuidMap.end())
                    {
                        node = it->second;
                    }
                }
                catch (const YAML::BadConversion&)
                {
                }
            }
            else if (node.IsMap())
            {
                for (auto it = node.begin(); it != node.end(); ++it)
                {
                    remapUUIDs(it->second);
                }
            }
            else if (node.IsSequence())
            {
                for (auto elem : node)
                {
                    remapUUIDs(elem);
                }
            }
        };

        for (auto entityNode : entities)
        {
            remapUUIDs(entityNode);
        }

        SceneSerializer serializer(m_EditorScene);
        auto createdUUIDs = serializer.DeserializeAdditive(entities);

        if (!createdUUIDs.empty())
        {
            // Create undo: wrap compound delete in DuplicateUndoCommand
            // Undo (user presses Ctrl+Z) → inner.Execute → delete pasted entities
            // Redo (user presses Ctrl+Y) → inner.Undo → restore pasted entities
            if (createdUUIDs.size() == 1)
            {
                auto entityOpt = m_EditorScene->TryGetEntityWithUUID(createdUUIDs[0]);
                if (entityOpt)
                {
                    m_CommandHistory.PushAlreadyExecuted(
                        std::make_unique<DuplicateUndoCommand>(
                            std::make_unique<DeleteEntityCommand>(
                                m_EditorScene, *entityOpt,
                                [this]()
                                { m_SceneHierarchyPanel.ClearSelection(); },
                                [this](Entity restored)
                                { m_SceneHierarchyPanel.SetSelectedEntity(restored); })));
                    m_SceneHierarchyPanel.SetSelectedEntity(*entityOpt);
                }
            }
            else
            {
                auto compound = std::make_unique<CompoundCommand>("Delete Pasted Entities");
                for (auto& uuid : createdUUIDs)
                {
                    auto entityOpt = m_EditorScene->TryGetEntityWithUUID(uuid);
                    if (entityOpt)
                    {
                        compound->Add(std::make_unique<DeleteEntityCommand>(
                            m_EditorScene, *entityOpt,
                            []() {},
                            [](Entity) {}));
                    }
                }
                m_CommandHistory.PushAlreadyExecuted(
                    std::make_unique<InvertedCommand>(std::move(compound)));
            }
        }
    }

    bool EditorLayer::OnAssetReloaded(AssetReloadedEvent const& e)
    {
        // Notify the rendering system so it can log generation changes
        // and verify next-frame refresh is clean.
        Renderer3D::OnAssetReloaded(e);

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
            case AssetType::ShaderGraph:
            {
                OLO_TRACE("   → Shader graph asset reloaded - recompiling affected materials");
                auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(e.GetHandle());
                Ref<Shader> compiledShader;
                if (graphAsset)
                {
                    graphAsset->MarkDirty();
                    compiledShader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(static_cast<u64>(e.GetHandle())));
                }

                auto recompileInScene = [&e, &compiledShader](Ref<Scene>& scene)
                {
                    if (!scene || !compiledShader)
                        return;
                    auto view = scene->GetAllEntitiesWith<MaterialComponent>();
                    for (auto entityID : view)
                    {
                        auto& matComp = view.get<MaterialComponent>(entityID);
                        if (matComp.m_ShaderGraphHandle == e.GetHandle())
                            matComp.m_Material.SetShader(compiledShader);
                    }
                };
                recompileInScene(m_ActiveScene);
                if (m_EditorScene && m_EditorScene != m_ActiveScene)
                    recompileInScene(m_EditorScene);
                break;
            }
            case AssetType::LightProbeVolume:
            {
                OLO_TRACE("   → Light probe volume asset reloaded - marking volumes dirty");
                auto markDirtyInScene = [&e](Ref<Scene>& scene)
                {
                    if (!scene)
                    {
                        return;
                    }
                    auto view = scene->GetAllEntitiesWith<LightProbeVolumeComponent>();
                    for (auto entityID : view)
                    {
                        auto& vol = view.get<LightProbeVolumeComponent>(entityID);
                        if (vol.m_BakedDataAsset == e.GetHandle())
                        {
                            vol.m_Dirty = true;
                        }
                    }
                };
                markDirtyInScene(m_ActiveScene);
                if (m_EditorScene && m_EditorScene != m_ActiveScene)
                {
                    markDirtyInScene(m_EditorScene);
                }
                break;
            }
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
