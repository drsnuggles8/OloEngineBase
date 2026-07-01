#include "OloEnginePCH.h"
#include "EditorLayer.h"
#include "Panels/AssetPackBuilderPanel.h"
#include "Panels/BuildGamePanel.h"
#include "MCP/McpServer.h"
#include "MCP/McpServerPanel.h"
#include "MCP/McpTools.h"
#include "OloEngine/Renderer/Preview/AssetPreviewRenderer.h"

#include <stb_image/stb_image_write.h>

#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <vector>
#include "UndoRedo/EntityCommands.h"
#include "UndoRedo/ComponentCommands.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/QualityTiering.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugger.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Localization/LocalizationManager.h"
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
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "GameplayEventLogger.h"
#include "OloEngine/Core/PerformanceProfiler.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <thread>

namespace
{
    // Interprets ImGui drag-drop payload bytes as a UTF-8 path.
    [[nodiscard]] std::filesystem::path PathFromUtf8Payload(const ImGuiPayload& payload)
    {
        auto const* data = static_cast<char const*>(payload.Data);
        auto const* u8data = reinterpret_cast<char8_t const*>(data);
        // Strip trailing NUL if the sender included it in DataSize
        size_t len = static_cast<size_t>(payload.DataSize);
        if (len > 0 && data[len - 1] == '\0')
            --len;
        return std::filesystem::path(std::u8string_view(u8data, len));
    }

    // Returns the file extension lowercased for case-insensitive comparison.
    [[nodiscard]] std::string LowercaseExtension(const std::filesystem::path& p)
    {
        std::string ext = p.extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
        return ext;
    }
} // namespace

namespace OloEngine
{
    EditorLayer::EditorLayer()
        : Layer("EditorLayer"), m_CameraController(1280.0f / 720.0f)
    {
    }

    EditorLayer::~EditorLayer()
    {
        // Cancel any ongoing build and wait for the task to finish so
        // the background lambda cannot access member state after destruction.
        m_BuildCancelRequested.store(true);
        if (m_BuildFuture.valid())
        {
            m_BuildFuture.wait();
        }
    }

    namespace
    {
        // stbi write callback that appends the encoded bytes to a std::vector.
        void StbiAppendToVector(void* context, void* data, int size)
        {
            auto* out = static_cast<std::vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->insert(out->end(), bytes, bytes + size);
        }

        // Read back the framebuffer's color attachment 0 (RGBA8), flip it to PNG
        // top-down orientation, optionally downscale so the width is <= maxWidth,
        // and encode a PNG in memory. Mirrors SaveGame/ThumbnailCapture. MUST run on
        // the main (GL) thread. Returns empty bytes on any failure.
        std::vector<u8> CaptureFramebufferPng(const Ref<Framebuffer>& framebuffer, int maxWidth)
        {
            if (!framebuffer)
                return {};
            const auto& spec = framebuffer->GetSpecification();
            const u32 width = spec.Width;
            const u32 height = spec.Height;
            if (width == 0 || height == 0)
                return {};
            const u32 textureId = framebuffer->GetColorAttachmentRendererID(0);
            if (textureId == 0)
                return {};

            std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
            ::glGetTextureImage(textureId, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                static_cast<GLsizei>(pixels.size()), pixels.data());

            // glGetTextureImage returns rows bottom-up; flip for PNG (top-down).
            const u32 rowBytes = width * 4;
            std::vector<u8> flipped(pixels.size());
            for (u32 y = 0; y < height; ++y)
                std::memcpy(flipped.data() + static_cast<sizet>(y) * rowBytes,
                            pixels.data() + static_cast<sizet>(height - 1 - y) * rowBytes, rowBytes);

            u32 outW = width;
            u32 outH = height;
            const std::vector<u8>* src = &flipped;
            std::vector<u8> scaled;
            if (maxWidth > 0 && width > static_cast<u32>(maxWidth))
            {
                outW = static_cast<u32>(maxWidth);
                outH = std::max<u32>(1, static_cast<u32>((static_cast<u64>(height) * outW) / width));
                scaled.assign(static_cast<sizet>(outW) * outH * 4, 0);
                // Nearest-neighbour downscale — cheap and adequate for a debug frame.
                for (u32 y = 0; y < outH; ++y)
                {
                    const u32 sy = std::min(height - 1, static_cast<u32>((static_cast<u64>(y) * height) / outH));
                    for (u32 x = 0; x < outW; ++x)
                    {
                        const u32 sx = std::min(width - 1, static_cast<u32>((static_cast<u64>(x) * width) / outW));
                        std::memcpy(&scaled[(static_cast<sizet>(y) * outW + x) * 4],
                                    &flipped[(static_cast<sizet>(sy) * width + sx) * 4], 4);
                    }
                }
                src = &scaled;
            }

            std::vector<u8> png;
            if (::stbi_write_png_to_func(StbiAppendToVector, &png, static_cast<int>(outW), static_cast<int>(outH),
                                         4, src->data(), static_cast<int>(outW * 4)) == 0)
                return {};
            return png;
        }
    } // namespace

    void EditorLayer::OnAttach()
    {
        OLO_PROFILE_FUNCTION();

        // First-run ImGui layout. The live `imgui.ini` is per-user state and is
        // gitignored, so a fresh checkout / new git worktree has none — ImGui would
        // then open with its default floating-window mess. Seed it once from the
        // committed `imgui_default.ini` so the editor comes up with the curated
        // docked layout. We only ever copy when `imgui.ini` is absent, so a user's
        // own arrangement (written back to the gitignored `imgui.ini`) always wins
        // and is never overwritten. Paths are CWD-relative to mirror ImGui's own
        // IniFilename resolution (the editor runs with CWD = OloEditor/), and this
        // runs before the first ImGui::NewFrame loads the ini (EditorLayer::OnAttach
        // is invoked during construction, ahead of Application::Run()).
        {
            std::error_code seedEc;
            if (!std::filesystem::exists("imgui.ini", seedEc) && std::filesystem::exists("imgui_default.ini", seedEc))
            {
                std::filesystem::copy_file("imgui_default.ini", "imgui.ini", seedEc);
                if (seedEc)
                {
                    OLO_CORE_WARN("EditorLayer: failed to seed imgui.ini from imgui_default.ini: {0}", seedEc.message());
                }
                else
                {
                    OLO_CORE_INFO("EditorLayer: seeded imgui.ini from committed default layout (imgui_default.ini)");
                }
            }
        }

        Application::Get().GetWindow().SetTitle("Test");

        // Toolbar icons are authored colour bitmaps — load as sRGB so the GPU
        // linearises them on sample, matching how every other colour texture
        // in the engine is treated. Each load gets a 1x1 spec-based fallback
        // so toolbar code paths that later dereference m_Icon*->GetRendererID()
        // never see a null Ref, mirroring ContentBrowserPanel's pattern.
        auto loadToolbarIcon = [](const char* path) -> Ref<Texture2D>
        {
            auto tex = Texture2D::Create(path, /*srgb=*/true);
            if (!tex || !tex->IsLoaded())
            {
                OLO_CORE_ERROR("EditorLayer: Failed to load toolbar icon '{}' — using 1x1 fallback", path);
                return Texture2D::Create(TextureSpecification{});
            }
            return tex;
        };
        m_IconPlay = loadToolbarIcon("Resources/Icons/PlayButton.png");
        m_IconPause = loadToolbarIcon("Resources/Icons/PauseButton.png");
        m_IconSimulate = loadToolbarIcon("Resources/Icons/SimulateButton.png");
        m_IconStep = loadToolbarIcon("Resources/Icons/StepButton.png");
        m_IconStop = loadToolbarIcon("Resources/Icons/StopButton.png");

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
            else
            {
                // No additional handling required.
            }
        }
        m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

        // Reapply preferences loaded by OpenProject() since the camera was just reconstructed
        m_EditorCamera.SetFlySpeed(m_Prefs.CameraFlySpeed);

        // Initialize Renderer3D early so 3D code paths in OnUpdate / UI_Viewport
        // never run against an uninitialized renderer when m_Is3DMode is true.
        TryInitialize3DMode();

        // Frame the start scene's terrain AFTER TryInitialize3DMode: it calls
        // ApplyDefault3DCameraPose, which would otherwise clobber the framing.
        // (OpenProject() opened the start scene before the camera reconstruction
        // above, so its earlier framing was already lost.)
        FrameEditorCameraOnTerrain(m_EditorScene);

        // Create brush preview UBO (binding 11, 32 bytes = 2 vec4s)
        m_BrushPreviewUBO = UniformBuffer::Create(ShaderBindingLayout::BrushPreviewUBO::GetSize(), ShaderBindingLayout::UBO_BRUSH_PREVIEW);

        // Create PBOs for async entity picking
        InitEntityPicking();

        // Initialize save game system
        SaveGameManager::Initialize();

        // Read-only MCP diagnostics server (#285). Construct it (off by default)
        // and register the tools now; the user starts it from Window > MCP Server.
        // The context lambdas are only invoked on the game thread (inside the
        // server's MarshalRead jobs), so reading m_ActiveScene / m_SceneState here
        // is safe against the non-thread-safe EnTT registry.
        {
            MCP::EditorMcpContext mcpContext;
            mcpContext.GetActiveScene = [this]() -> Ref<Scene>
            { return m_ActiveScene; };
            mcpContext.IsPlaying = [this]() -> bool
            { return m_SceneState == SceneState::Play; };
            mcpContext.CaptureViewportPng = [this](int maxWidth) -> std::vector<u8>
            {
                // Capture what the viewport actually displays (see UI_Viewport): in 3D
                // mode that's the UICompositePass output (fallback SceneColor), not
                // m_Framebuffer, which would otherwise yield a stale/blank image.
                Ref<Framebuffer> target = m_Framebuffer;
                if (m_Is3DMode)
                {
                    if (auto ui = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); ui)
                        target = ui;
                    else if (auto scene = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); scene)
                        target = scene;
                }
                return CaptureFramebufferPng(target, maxWidth);
            };

            // Tier-0 camera / viewport control (#316). Editor-only inspection
            // state; nothing here touches the project.
            mcpContext.GetCameraPose = [this]() -> MCP::McpCameraPose
            {
                MCP::McpCameraPose pose;
                pose.Position = m_EditorCamera.GetPosition();
                pose.FocalPoint = m_EditorCamera.GetFocalPoint();
                pose.Forward = m_EditorCamera.GetForwardDirection();
                pose.Distance = m_EditorCamera.GetDistance();
                pose.YawRadians = m_EditorCamera.GetYaw();
                pose.PitchRadians = m_EditorCamera.GetPitch();
                pose.FovDegrees = m_EditorCamera.GetFOV();
                pose.NearClip = m_EditorCamera.GetNearClip();
                pose.FarClip = m_EditorCamera.GetFarClip();
                pose.ViewportWidth = static_cast<u32>(m_ViewportSize.x);
                pose.ViewportHeight = static_cast<u32>(m_ViewportSize.y);
                return pose;
            };
            mcpContext.SetCameraPose = [this](const glm::vec3& eye, f32 yawRadians, f32 pitchRadians, f32 fovDegrees)
            {
                if (fovDegrees > 0.0f)
                    m_EditorCamera.SetFOV(fovDegrees);
                m_EditorCamera.SetPose(eye, yawRadians, pitchRadians);
            };
            mcpContext.OrbitCamera = [this](const glm::vec3& target, f32 yawRadians, f32 pitchRadians, f32 distance)
            {
                m_EditorCamera.Focus(target, distance, yawRadians, pitchRadians);
            };
            mcpContext.RestoreCameraPose = [this](const MCP::McpCameraPose& pose)
            {
                m_EditorCamera.SetFOV(pose.FovDegrees);
                m_EditorCamera.Focus(pose.FocalPoint, pose.Distance, pose.YawRadians, pose.PitchRadians);
            };
            mcpContext.FrameEntity = [this](u64 entityUuid) -> bool
            { return FrameEditorCameraOnEntity(entityUuid); };
            mcpContext.SetViewportSizeOverride = [this](u32 width, u32 height)
            {
                // The MCP tool layer already clamps to [64, 8192]; re-clamp here so
                // the framebuffer/render-graph resize in OnUpdate can never see an
                // oversized request even if another caller appears. 0,0 = clear.
                if (width != 0 || height != 0)
                {
                    width = std::clamp(width, 64u, 8192u);
                    height = std::clamp(height, 64u, 8192u);
                }
                m_McpViewportSizeOverride = { width, height };
            };
            // Consented, undoable project writes (#306 item C). The write tools run
            // their mutation through the same undo stack as the editor's own edits,
            // so an agent's change is a single Ctrl-Z. Main-thread-only, like the
            // readers above (the MCP server calls it from a MarshalRead job).
            mcpContext.GetCommandHistory = [this]() -> CommandHistory*
            {
                // Only expose the undo stack in Edit mode. In Play / Simulate the
                // runtime scene is a transient copy the editor undo stack does not
                // track, so a write there would be unsound (and discarded on stop) —
                // hand out nullptr so the write handler refuses ("no editor command
                // history available") rather than mutating the runtime scene.
                return m_SceneState == SceneState::Edit ? &m_CommandHistory : nullptr;
            };
            // olo_reload_script: reload the C# app assembly — the same path as the
            // Script ▸ Reload assembly menu (Ctrl+R). Main-thread-only (Mono domain),
            // so the MCP server calls it from a MarshalRead job. Reports honestly when
            // C# scripting is disabled in this build or not yet initialized, rather
            // than pretending a reload happened.
            mcpContext.ReloadScriptAssembly = []() -> MCP::McpScriptReloadResult
            {
                MCP::McpScriptReloadResult result;
                result.Language = "csharp";
#if OLO_ENABLE_CSHARP_SCRIPTING
                if (ScriptEngine::GetCoreAssemblyImage() == nullptr)
                {
                    result.Message = "C# scripting is not initialized (no core assembly loaded).";
                    return result;
                }
                // Available: scripting is initialized so a reload could be attempted.
                // Ok: whether the assembly actually loaded (false when the freshly-built
                // app assembly fails to load — e.g. a compile error — leaving the entity
                // classes stale). Report both distinctly rather than always claiming success.
                const bool reloaded = ScriptEngine::ReloadAssembly();
                result.Available = true;
                result.Ok = reloaded;
                result.ScriptClassCount = static_cast<u32>(ScriptEngine::GetEntityClasses().size());
                result.Message = reloaded
                                     ? "Reloaded the C# app assembly (" + std::to_string(result.ScriptClassCount) +
                                           " script class(es) registered)."
                                     : "Reload failed: the C# app assembly did not load (see the engine log). "
                                       "Rebuild the game assembly and retry.";
#else
                result.Message = "C# scripting is disabled in this build (Mono not available on this platform).";
#endif
                return result;
            };
            mcpContext.GetFrameIndex = [this]() -> u64
            { return m_FrameIndex; };
            mcpContext.IsCaptureUnready = [this]() -> bool
            {
                // Unready while throttled, or within a few frames of a viewport
                // resize: freshly resized render-graph framebuffers render black
                // for the first couple of frames (verified against the live
                // editor — 2 frames after a resize still captured black, 6 were
                // clean), so wait out a conservative window.
                constexpr u64 kResizeSettleFrames = 6;
                return m_ViewportRenderSkipped ||
                       (m_FrameIndex < m_LastViewportResizeFrame + kResizeSettleFrames);
            };

            m_McpServer = CreateScope<MCP::McpServer>(std::move(mcpContext));
            MCP::RegisterBuiltinTools(*m_McpServer);
            // Apply the persisted redaction preference (loaded by OpenProject above).
            m_McpServer->SetRedactPaths(m_Prefs.McpRedactPaths);

            // Auto-start is opt-in and explicit: either the persisted preference
            // (Window > MCP Server > "Start automatically") or the OLO_MCP_AUTOSTART
            // env var (for headless attach / the smoke test). Default stays off.
            const char* autostartEnv = std::getenv("OLO_MCP_AUTOSTART");
            const bool envAutoStart = autostartEnv != nullptr && std::string_view(autostartEnv) != "0" && *autostartEnv != '\0';
            if (envAutoStart || m_Prefs.McpAutoStart)
            {
                auto port = static_cast<u16>(std::clamp(m_Prefs.McpPort, 1024, 65535));
                if (const char* portEnv = std::getenv("OLO_MCP_PORT"); portEnv != nullptr)
                {
                    if (const unsigned long parsed = std::strtoul(portEnv, nullptr, 10);
                        parsed >= 1024 && parsed <= 65535)
                        port = static_cast<u16>(parsed);
                }
                if (m_McpServer->Start(port))
                    m_ShowMcpPanel = true; // surface the panel so the token is visible
            }
        }
    }

    void EditorLayer::OnDetach()
    {
        OLO_PROFILE_FUNCTION();

        // Stop the MCP server first so no in-flight marshaled read touches editor
        // state while the rest of OnDetach tears it down.
        if (m_McpServer)
            m_McpServer->Stop();

        // Properly stop the scene if still in play/simulate mode
        // (e.g., user closed the window while playing)
        if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
        {
            OnSceneStop();
        }

        AssetPreviewRenderer::Shutdown();
        ShutdownEntityPicking();
        SaveGameManager::Shutdown();

        // Persist the active locale across editor sessions. Best-effort —
        // failures are logged inside SaveActiveLocaleToFile and don't block
        // shutdown, since the next launch will fall back to OS negotiation.
        if (!LocalizationManager::GetCurrentLocale().empty())
            (void)LocalizationManager::SaveActiveLocaleToFile("userprefs/locale.yaml");
    }

    void EditorLayer::InitEntityPicking()
    {
        glCreateBuffers(2, m_PickingPBOs);
        for (auto pbo : m_PickingPBOs)
        {
            glNamedBufferStorage(pbo, sizeof(int), nullptr, GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
        }
        m_PickingPBOIndex = 0;
        m_PickingReadPending = false;
        m_PickingPBOInitialized = true;
    }

    void EditorLayer::ShutdownEntityPicking()
    {
        if (m_PickingPBOInitialized)
        {
            glDeleteBuffers(2, m_PickingPBOs);
            m_PickingPBOs[0] = 0;
            m_PickingPBOs[1] = 0;
            m_PickingPBOInitialized = false;
            m_PickingReadPending = false;
        }
    }

    void EditorLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE("EditorLayer::OnUpdate", Application::Get().GetPerformanceProfiler());

        ++m_FrameIndex; // MCP capture tools key off this to await a rendered frame

        m_LastFrameTimeMs = ts.GetMilliseconds();

        // Sync with async asset loading thread
        AssetManager::SyncWithAssetThread();

        if (!m_ActiveScene)
        {
            return;
        }

        m_ActiveScene->OnViewportResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));
        m_ActiveScene->SetViewportOffset(m_ViewportBounds[0]);

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
            m_LastViewportResizeFrame = m_FrameIndex; // MCP captures must wait out the resize transient
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

        // Feed selected entity IDs to the selection outline pass (editor-only, 3D Edit mode)
        if (m_Is3DMode && m_SceneState == SceneState::Edit)
        {
            auto& selectedEntities = m_SceneHierarchyPanel.GetSelectedEntities();
            std::vector<i32> ids;
            ids.reserve(selectedEntities.size());
            for (auto& entity : selectedEntities)
            {
                if (entity)
                {
                    ids.push_back(static_cast<i32>(static_cast<u32>(entity)));
                }
            }

            Renderer3D::SetSelectionOutlineEntityIDs(ids);
        }
        else
        {
            Renderer3D::SetSelectionOutlineEntityIDs({});
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
                m_ActiveScene->SetGridVisible(Renderer3D::GetRendererSettings().ShowGrid);
                m_ActiveScene->SetGridSpacing(m_GridSpacing);
                m_ActiveScene->SetLightGizmosVisible(Renderer3D::GetRendererSettings().ShowLightGizmos);
                m_ActiveScene->SetWorldAxisHelperVisible(Renderer3D::GetRendererSettings().ShowWorldAxisHelper);
                m_ActiveScene->SetCameraFrustumsVisible(Renderer3D::GetRendererSettings().ShowCameraFrustums);
                m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);

                // Auto-save timer
                if (auto const project = Project::GetActive(); project && project->GetConfig().EnableAutoSave && !m_EditorScenePath.empty())
                {
                    m_TimeSinceLastAutoSave += ts;
                    if (m_TimeSinceLastAutoSave >= static_cast<f32>(project->GetConfig().AutoSaveIntervalSeconds))
                    {
                        AutoSaveScene();
                    }
                }
                break;
            }
            case SceneState::Simulate:
            {
                m_EditorCamera.OnUpdate(ts);

                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                m_ActiveScene->SetGridVisible(Renderer3D::GetRendererSettings().ShowGrid);
                m_ActiveScene->SetGridSpacing(m_GridSpacing);
                m_ActiveScene->SetLightGizmosVisible(Renderer3D::GetRendererSettings().ShowLightGizmos);
                m_ActiveScene->SetWorldAxisHelperVisible(Renderer3D::GetRendererSettings().ShowWorldAxisHelper);
                m_ActiveScene->SetCameraFrustumsVisible(Renderer3D::GetRendererSettings().ShowCameraFrustums);
                m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
                break;
            }
            case SceneState::Play:
            {
                m_ActiveScene->SetIs3DModeEnabled(m_Is3DMode);
                // Deterministic fixed-timestep tick (issue #452): advance gameplay
                // in fixed dt steps from the variable frame delta, render once.
                m_ActiveScene->OnUpdateRuntimeFixed(ts, Application::Get().GetFixedTimeStep());
                SaveGameManager::Tick(ts, *m_ActiveScene);

                // Handle script-triggered scene reload
                if (m_ActiveScene->GetPendingReload())
                {
                    m_ActiveScene->SetPendingReload(false);
                    OnSceneStop();
                    OnScenePlay();
                }
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
                // Async entity picking via PBO double-buffering:
                // 1) Read back previous frame's result (no stall — data is ready)
                // 2) Issue new async read into the other PBO
                int pixelData = -1;
                if (m_Is3DMode && m_PickingPBOInitialized)
                {
                    const u32 readPBO = m_PickingPBOs[1 - m_PickingPBOIndex]; // Previous frame's PBO
                    const u32 writePBO = m_PickingPBOs[m_PickingPBOIndex];    // This frame's PBO

                    // Step 1: Read back previous frame's result (only if we issued a read last frame)
                    if (m_PickingReadPending)
                    {
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, readPBO);
                        if (const auto* mapped = static_cast<const int*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)); mapped)
                        {
                            pixelData = *mapped;
                            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                        }
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    }

                    // Step 2: Issue async read for this frame into the write PBO
                    if (auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); framebuffer)
                    {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer->GetRendererID());
                        glReadBuffer(GL_COLOR_ATTACHMENT0 + 1); // Entity ID attachment
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, writePBO);
                        glReadPixels(mouseX, mouseY, 1, 1, GL_RED_INTEGER, GL_INT, nullptr);
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                        m_PickingReadPending = true;
                    }

                    // Swap PBO index for next frame
                    m_PickingPBOIndex = 1 - m_PickingPBOIndex;
                }
                else if (!m_Is3DMode)
                {
                    // 2D mode: synchronous read (not performance critical)
                    pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
                }
                else
                {
                    // No additional handling required.
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

            // Instance scatter brush: raycasts the terrain heightmap AND the
            // scene's mesh surfaces (§1.2 — BVH closest hit), and feeds the
            // brush whichever hit is closer along the ray. Terrain normals
            // come from the CPU heightmap via finite differences, mesh
            // normals from the struck triangle — `vec3(0, 1, 0)` fallback
            // when nothing is hit.
            if (m_ShowInstanceScatterBrush && m_InstanceScatterBrushPanel.IsActive() &&
                m_ViewportHovered && m_SceneState == SceneState::Edit)
            {
                // Sync the target from the SceneHierarchy selection. The
                // brush panel refuses to paint when the selected entity
                // doesn't have an InstancedMeshComponent, so an empty
                // selection here is harmless.
                m_InstanceScatterBrushPanel.SetTargetEntity(m_SceneHierarchyPanel.GetSelectedEntity());

                glm::vec3 hitPos{};
                glm::vec3 surfaceNormal{ 0.0f, 1.0f, 0.0f };
                bool hasHit = false;
                {
                    OLO_PROFILE_SCOPE("EditorLayer::ScatterBrushRaycast");

                    hasHit = TerrainRaycast({ mx, my }, viewportSize, hitPos);
                    if (hasHit && m_ActiveScene)
                    {
                        // Pull the surface normal from the same terrain entity
                        // TerrainRaycast hit. The normal is needed for slope
                        // filtering (§1.4) and align-to-normal (§1.5 style).
                        auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
                        if (auto it = view.begin(); it != view.end())
                        {
                            Entity terrainEntity(*it, m_ActiveScene.get());
                            const auto& tc = terrainEntity.GetComponent<TerrainComponent>();
                            const auto& tx = terrainEntity.GetComponent<TransformComponent>();
                            if (tc.m_TerrainData && tc.m_WorldSizeX > 0.0f && tc.m_WorldSizeZ > 0.0f)
                            {
                                const f32 normX = (hitPos.x - tx.Translation.x) / tc.m_WorldSizeX;
                                const f32 normZ = (hitPos.z - tx.Translation.z) / tc.m_WorldSizeZ;
                                surfaceNormal = tc.m_TerrainData->GetNormalAt(
                                    glm::clamp(normX, 0.0f, 1.0f),
                                    glm::clamp(normZ, 0.0f, 1.0f),
                                    tc.m_WorldSizeX, tc.m_WorldSizeZ, tc.m_HeightScale);
                            }
                        }
                    }

                    // Mesh-surface pass: capping the ray at the terrain hit
                    // means only a mesh in front of the terrain can win, so a
                    // single CastRay both finds the mesh hit and resolves the
                    // terrain-vs-mesh precedence.
                    if (m_ActiveScene)
                    {
                        if (Ray mouseRay; BuildMouseRay({ mx, my }, viewportSize, mouseRay))
                        {
                            if (hasHit)
                            {
                                mouseRay.TMax = glm::dot(hitPos - mouseRay.Origin, mouseRay.Direction);
                            }
                            SceneMeshRayHit meshHit;
                            if (m_MeshRaycaster.CastRay(*m_ActiveScene, mouseRay, meshHit))
                            {
                                hitPos = meshHit.Point;
                                surfaceNormal = meshHit.Normal;
                                hasHit = true;
                            }
                        }
                    }
                }

                const bool mouseDown = Input::IsMouseButtonPressed(Mouse::ButtonLeft) &&
                                       !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt);
                m_InstanceScatterBrushPanel.OnUpdate(ts, hitPos, surfaceNormal, hasHit, mouseDown);
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
        OLO_PERF_SCOPE("EditorLayer::OnImGuiRender", Application::Get().GetPerformanceProfiler());

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
        UI_AutoSaveRecoveryModal();

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
                SyncPrefsFromMembers();
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

            if (ImGui::MenuItem("Build Shader Pack"))
            {
                BuildShaderPack();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Validate Asset References"))
            {
                ValidateAssetReferences();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Asset Pack Builder", nullptr, &m_ShowAssetPackBuilder))
            {
                // Toggle panel visibility
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Build Game...", nullptr, &m_ShowBuildGame))
            {
                // Toggle Build Game panel
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
            ImGui::MenuItem("Statistics", nullptr, &m_ShowStatistics);
            ImGui::MenuItem("Animation Panel", nullptr, &m_ShowAnimationPanel);
            ImGui::MenuItem("Post Process Settings", nullptr, &m_ShowPostProcessSettings);
            ImGui::MenuItem("Renderer Settings", nullptr, &m_ShowRendererSettings);
            ImGui::MenuItem("Terrain Editor", nullptr, &m_ShowTerrainEditor);
            ImGui::MenuItem("Instance Scatter Brush", nullptr, &m_ShowInstanceScatterBrush);
            ImGui::MenuItem("Scene Streaming", nullptr, &m_ShowStreamingPanel);
            ImGui::MenuItem("Input Settings", nullptr, &m_ShowInputSettings);
            ImGui::MenuItem("Network Debug", nullptr, &m_ShowNetworkDebug);
            ImGui::MenuItem("Thread Inspector", nullptr, &m_ShowThreadInspector);
            ImGui::MenuItem("Dialogue Editor", nullptr, &m_ShowDialogueEditor);
            ImGui::MenuItem("Cinematic Timeline", nullptr, &m_ShowCinematicTimeline);
            ImGui::MenuItem("NavMesh Panel", nullptr, &m_ShowNavMeshPanel);
            ImGui::MenuItem("Behavior Tree Editor", nullptr, &m_ShowBehaviorTreeEditor);
            ImGui::MenuItem("State Machine Editor", nullptr, &m_ShowFSMEditor);
            ImGui::MenuItem("Shader Graph Editor", nullptr, &m_ShowShaderGraphEditor);
            ImGui::MenuItem("Animation Graph Editor", nullptr, &m_ShowAnimationGraphEditor);
            ImGui::MenuItem("Save Game Panel", nullptr, &m_ShowSaveGamePanel);
            ImGui::MenuItem("Localization", nullptr, &m_ShowLocalizationPanel);
            ImGui::MenuItem("Gamepad Debug", nullptr, &m_ShowGamepadDebug);
            ImGui::MenuItem("Shader Editor", nullptr, &m_ShowShaderEditor);
            ImGui::MenuItem("Audio Events", nullptr, &m_ShowAudioEventsPanel);
            ImGui::MenuItem("MCP Server", nullptr, &m_ShowMcpPanel);

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
        // MCP viewport override (#316): pin the render size for deterministic
        // captures regardless of the panel's layout. The image is still drawn
        // into the panel rect below (clipped/letterboxed as needed).
        if (m_McpViewportSizeOverride.x > 0 && m_McpViewportSizeOverride.y > 0)
            m_ViewportSize = { static_cast<f32>(m_McpViewportSizeOverride.x), static_cast<f32>(m_McpViewportSizeOverride.y) };
        else
            m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };

        // Display appropriate framebuffer based on mode
        u64 textureID = 0;
        if (m_Is3DMode)
        {
            // Use UICompositePass output (post-processed scene + 2D overlays + UI)
            if (auto uiFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); uiFramebuffer)
            {
                textureID = uiFramebuffer->GetColorAttachmentRendererID(0);
            }
            // Fallback to scene pass if post-process pass is not available
            if (textureID == 0)
            {
                if (auto sceneFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); sceneFramebuffer)
                {
                    textureID = sceneFramebuffer->GetColorAttachmentRendererID(0);
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
            // Accept scene files (typed payload from content browser)
            if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_SCENE"))
            {
                std::filesystem::path path = PathFromUtf8Payload(*payload);
                if (ConfirmDiscardChanges())
                {
                    m_HoveredEntity = Entity();
                    OpenScene(path);
                }
            }

            // Accept generic items (images, prefabs, and legacy scene drops)
            if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                std::filesystem::path path = PathFromUtf8Payload(*payload);

                auto const ext = LowercaseExtension(path);
                if (ext == ".olo" || ext == ".scene") // Legacy: scene via generic payload
                {
                    if (ConfirmDiscardChanges())
                    {
                        m_HoveredEntity = Entity();
                        OpenScene(path);
                    }
                }
                else if (m_SceneState == SceneState::Edit && [&ext]
                         { static constexpr std::string_view kImageExts[] = {".png", ".jpeg", ".jpg"}; return std::ranges::find(kImageExts, ext) != std::ranges::end(kImageExts); }() && m_HoveredEntity && m_HoveredEntity.HasComponent<SpriteRendererComponent>()) // Load texture
                {
                    // Sprite art is colour content, treat the dropped image as sRGB.
                    const Ref<Texture2D> texture = Texture2D::Create(path.string(), /*srgb=*/true);
                    if (texture && texture->IsLoaded())
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
                else if (m_SceneState == SceneState::Edit && LowercaseExtension(path) == ".oloprefab") // Instantiate prefab
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
                else if (m_SceneState == SceneState::Edit && [&ext]
                         { static constexpr std::string_view kModelExts[] = {".gltf", ".glb", ".fbx", ".obj"}; return std::ranges::find(kModelExts, ext) != std::ranges::end(kModelExts); }()) // Import a 3D model into the scene
                {
                    std::string filepath = path.string();
                    std::string entityName = path.stem().string();
                    if (entityName.empty())
                    {
                        entityName = "Model";
                    }

                    // Wire the entity inside the create callback so undo/redo recreate it cleanly.
                    // Models carrying a skeleton and/or animation clips get the full animation
                    // component set (mesh + skeleton + animation state + material); everything
                    // else is imported as a single combined static mesh.
                    m_CommandHistory.Execute(std::make_unique<CreateEntityCommand>(
                        m_EditorScene, entityName,
                        [this, filepath](Entity created)
                        {
                            bool wired = false;
                            auto animatedModel = Ref<AnimatedModel>::Create(filepath);
                            if (animatedModel && !animatedModel->GetMeshes().empty() &&
                                (animatedModel->HasSkeleton() || animatedModel->HasAnimations()))
                            {
                                ModelImporter::PopulateAnimatedEntity(created, animatedModel, filepath);
                                wired = true;
                            }
                            else
                            {
                                auto model = Ref<Model>::Create(filepath);
                                wired = ModelImporter::PopulateStaticEntity(created, model);
                            }

                            if (!wired)
                            {
                                OLO_WARN("Could not import model into scene: {0}", filepath);
                            }
                            m_SceneHierarchyPanel.SetSelectedEntity(created);
                        },
                        [this]()
                        { m_SceneHierarchyPanel.ClearSelection(); }));
                }
                else
                {
                    // No additional handling required.
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
            else
            {
                // No additional handling required.
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
                m_GizmoStartRotation = tc.GetRotationEuler();
                m_GizmoStartScale = tc.Scale;
            }

            if (isUsing)
            {
                tc.SetTransform(transform);
            }

            // Push undo command when gizmo interaction ends
            if (!isUsing && m_GizmoWasUsing && m_SceneState == SceneState::Edit)
            {
                m_CommandHistory.PushAlreadyExecuted(std::make_unique<TransformChangeCommand>(
                    m_EditorScene, selectedEntity.GetUUID(),
                    m_GizmoStartTranslation, m_GizmoStartRotation, m_GizmoStartScale,
                    tc.Translation, tc.GetRotationEuler(), tc.Scale));
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
            ++buttonCount;
        }
        if (hasSimulateButton)
        {
            ++buttonCount;
        }
        if (hasPauseButton)
        {
            ++buttonCount;
        }
        if (isPaused)
        {
            ++buttonCount;
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
            if (Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Simulate)) ? m_IconPlay : m_IconStop; ImGui::ImageButton("##play_stop_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
            {
                if ((m_SceneState == Edit) || (m_SceneState == Simulate))
                {
                    OnScenePlay();
                }
                else if (m_SceneState == Play)
                {
                    OnSceneStop();
                }
                else
                {
                    // No additional handling required.
                }
            }
            ImGui::SameLine();
        }

        // Simulate / Stop button
        if (hasSimulateButton)
        {
            using enum OloEngine::EditorLayer::SceneState;
            if (Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Play)) ? m_IconSimulate : m_IconStop; ImGui::ImageButton("##simulate_stop_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
            {
                if ((m_SceneState == Edit) || (m_SceneState == Play))
                {
                    OnSceneSimulate();
                }
                else if (m_SceneState == Simulate)
                {
                    OnSceneStop();
                }
                else
                {
                    // No additional handling required.
                }
            }
            ImGui::SameLine();
        }

        // Pause button
        if (hasPauseButton)
        {
            if (Ref<Texture2D> const icon = m_IconPause; ImGui::ImageButton("##pause_icon", static_cast<u64>(icon->GetRendererID()), btnSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tintColor) && toolbarEnabled)
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

        // Build Game Panel
        if (m_ShowBuildGame && m_BuildGamePanel)
        {
            m_BuildGamePanel->SetEditorScenePath(m_EditorScenePath);
            m_BuildGamePanel->SetIs3DMode(m_Is3DMode);
            m_BuildGamePanel->OnImGuiRender(m_ShowBuildGame);
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

        // Renderer Settings Panel
        if (m_ShowRendererSettings)
        {
            m_RendererSettingsPanel.OnImGuiRender(&m_ShowRendererSettings);
            if (m_RendererSettingsPanel.ConsumeDebugSettingsChanged())
            {
                SyncPrefsFromMembers();
            }
        }

        // Terrain Editor Panel
        if (m_ShowInstanceScatterBrush)
        {
            m_InstanceScatterBrushPanel.SetContext(m_ActiveScene);
            m_InstanceScatterBrushPanel.OnImGuiRender();
            m_ShowInstanceScatterBrush = m_InstanceScatterBrushPanel.Visible;
        }

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

        // MCP Diagnostics Server Panel
        if (m_ShowMcpPanel && m_McpServer)
        {
            MCP::RenderMcpServerPanel(*m_McpServer, m_Prefs.McpPort, m_Prefs.McpAutoStart, &m_ShowMcpPanel);
        }

        // Thread Inspector Panel
        if (m_ShowThreadInspector)
        {
            m_ThreadInspectorPanel.OnImGuiRender(&m_ShowThreadInspector);
        }

        // Dialogue Editor Panel
        if (m_ShowDialogueEditor)
        {
            m_DialogueEditorPanel.OnImGuiRender();
            m_ShowDialogueEditor = m_DialogueEditorPanel.IsOpen();
        }

        // Cinematic Timeline Panel — context is set per-frame so scrubbing /
        // preview always poses the current active scene (edit mode == editor
        // scene; play/sim == the runtime copy, where ApplyAtTime is a no-op).
        if (m_ShowCinematicTimeline)
        {
            m_CinematicTimelinePanel.SetContext(m_ActiveScene);
            m_CinematicTimelinePanel.OnImGuiRender(&m_ShowCinematicTimeline);
        }

        // Shader Graph Editor Panel
        if (m_ShowShaderGraphEditor)
        {
            m_ShaderGraphEditorPanel.SetOpen(true);
            m_ShaderGraphEditorPanel.OnImGuiRender();
            m_ShowShaderGraphEditor = m_ShaderGraphEditorPanel.IsOpen();
        }

        // Sound Graph Editor Panel
        if (m_ShowSoundGraphEditor)
        {
            m_SoundGraphEditorPanel.SetOpen(true);
            m_SoundGraphEditorPanel.OnImGuiRender();
            m_ShowSoundGraphEditor = m_SoundGraphEditorPanel.IsOpen();
        }

        // Animation Graph Editor Panel
        if (m_ShowAnimationGraphEditor)
        {
            m_AnimationGraphEditorPanel.SetSelectedEntity(m_SceneHierarchyPanel.GetSelectedEntity());
            m_AnimationGraphEditorPanel.OnImGuiRender(&m_ShowAnimationGraphEditor);
        }

        // Save Game Panel
        if (m_ShowSaveGamePanel)
        {
            m_SaveGamePanel.OnImGuiRender(&m_ShowSaveGamePanel);
        }

        // Localization Panel — lazy-init the locale set on first open, and
        // re-init whenever the active project changes (so opening a second
        // project picks up its own assets/localization/ tree rather than
        // sticking with the first project's locales). The gate is the
        // identity of Project::GetActive(); a process-lifetime `static bool`
        // would silently break the second-project case.
        if (m_ShowLocalizationPanel)
        {
            static const Project* s_LastLocalizationProject = reinterpret_cast<const Project*>(0x1);
            if (const Project* activeProject = Project::GetActive().Raw(); activeProject != s_LastLocalizationProject)
            {
                // Resolve the project's asset root + "localization" subdir
                // into an absolute path. The hard-coded "assets/localization"
                // only worked when the editor was launched from a CWD that
                // happened to sit one level above the project's assets/
                // directory — opening a project at a different path silently
                // failed to find any locale files.
                const std::filesystem::path localizationDir = activeProject
                                                                  ? Project::GetAssetFileSystemPath("localization")
                                                                  : std::filesystem::path{ "assets/localization" };
                m_LocalizationPanel.SetDirectory(localizationDir);
                s_LastLocalizationProject = activeProject;

                // Restore the previously-selected locale or, on first launch,
                // negotiate against the OS preference list. Persistence file
                // lives under userprefs/ — separate from save games so it
                // survives Save → New Game.
                const std::filesystem::path prefsPath = "userprefs/locale.yaml";
                if (!LocalizationManager::LoadActiveLocaleFromFile(prefsPath))
                {
                    const std::string negotiated = LocalizationManager::NegotiateLocale();
                    if (!negotiated.empty())
                        (void)LocalizationManager::SetCurrentLocale(negotiated);
                }
            }
            m_LocalizationPanel.OnImGuiRender(&m_ShowLocalizationPanel);
        }

        // NavMesh Panel
        if (m_ShowNavMeshPanel)
        {
            m_NavMeshPanel.OnImGuiRender();
        }

        // Behavior Tree Editor Panel
        if (m_ShowBehaviorTreeEditor)
        {
            m_BehaviorTreeEditorPanel.OnImGuiRender();
        }

        // FSM Editor Panel
        if (m_ShowFSMEditor)
        {
            m_FSMEditorPanel.OnImGuiRender();
        }

        // Gamepad Debug Panel
        if (m_ShowGamepadDebug)
        {
            m_GamepadDebugPanel.OnImGuiRender(&m_ShowGamepadDebug);
        }

        // Shader Editor Panel
        if (m_ShowShaderEditor)
        {
            m_ShaderEditorPanel.OnImGuiRender(&m_ShowShaderEditor);
        }

        // Audio Events Panel
        if (m_ShowAudioEventsPanel)
        {
            m_AudioEventsPanel.OnImGuiRender(&m_ShowAudioEventsPanel);
        }

        // Console Panel
        if (m_ShowConsolePanel)
        {
            m_ConsolePanel.OnImGuiRender(&m_ShowConsolePanel);
        }

        // Scene Statistics Panel
        if (m_ShowStatistics)
        {
            m_StatisticsPanel.SetHoveredEntity(m_HoveredEntity);
            m_StatisticsPanel.OnImGuiRender(&m_ShowStatistics);
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
        Renderer3D::SetSelectionOutlineEnabled(true);
        Renderer3D::Init(&Application::Get().GetWindow());
        AssetPreviewRenderer::Initialize();
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
        auto& debugSettings = Renderer3D::GetRendererSettings();
        debugSettings.ShowGrid = m_Prefs.ShowGrid;
        m_GridSpacing = m_Prefs.GridSpacing;
        m_TranslateSnap = m_Prefs.TranslateSnap;
        m_RotateSnap = m_Prefs.RotateSnap;
        m_ScaleSnap = m_Prefs.ScaleSnap;
        debugSettings.ShowPhysicsColliders = m_Prefs.ShowPhysicsColliders;
        debugSettings.ShowLightGizmos = m_Prefs.ShowLightGizmos;
        debugSettings.ShowBoundingBoxes = m_Prefs.ShowBoundingBoxes;
        m_Is3DMode = m_Prefs.Is3DMode;
        m_EditorCamera.SetFlySpeed(m_Prefs.CameraFlySpeed);
        m_ThrottleEditMode = m_Prefs.ThrottleEditMode;
        m_ThrottlePlayMode = m_Prefs.ThrottlePlayMode;
        m_RenderBudgetMs = m_Prefs.RenderBudgetMs;

        // Push frame pacing to the main loop (#456).
        Application::Get().SetFrameRateCap(m_Prefs.FrameRateCap);
        Application::Get().SetFrameTimeSmoothing(m_Prefs.FrameTimeSmoothing);

        auto& physicsSettings = Physics3DSystem::GetSettings();
        physicsSettings.m_CaptureOnPlay = m_Prefs.CapturePhysicsOnPlay;

        // Keep the live MCP server's redaction policy in sync with prefs whenever
        // they're (re)applied — e.g. after OpenProject reloads m_Prefs. Guarded
        // because the server is constructed later in OnAttach (#285).
        if (m_McpServer)
            m_McpServer->SetRedactPaths(m_Prefs.McpRedactPaths);

        if (auto project = Project::GetActive())
        {
            auto& cfg = project->GetConfig();
            cfg.EnableAutoSave = m_Prefs.EnableAutoSave;
            cfg.AutoSaveIntervalSeconds = std::clamp(m_Prefs.AutoSaveIntervalSeconds, 10, 7200);

            // Apply quality tiering to renderer settings
            ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
            ApplyTieringToSettings(cfg.QualityTiering, Renderer3D::GetPostProcessSettings(), shadowCopy);
            Renderer3D::GetShadowMap().SetSettings(shadowCopy);
        }

        if (m_Is3DMode && !Renderer3D::IsInitialized())
        {
            TryInitialize3DMode();
        }
    }

    void EditorLayer::SyncPrefsFromMembers()
    {
        auto const& debugSettings = Renderer3D::GetRendererSettings();
        m_Prefs.ShowGrid = debugSettings.ShowGrid;
        m_Prefs.GridSpacing = m_GridSpacing;
        m_Prefs.TranslateSnap = m_TranslateSnap;
        m_Prefs.RotateSnap = m_RotateSnap;
        m_Prefs.ScaleSnap = m_ScaleSnap;
        m_Prefs.ShowPhysicsColliders = debugSettings.ShowPhysicsColliders;
        m_Prefs.ShowLightGizmos = debugSettings.ShowLightGizmos;
        m_Prefs.ShowBoundingBoxes = debugSettings.ShowBoundingBoxes;
        m_Prefs.Is3DMode = m_Is3DMode;
        m_Prefs.CameraFlySpeed = m_EditorCamera.GetFlySpeed();
        m_Prefs.CapturePhysicsOnPlay = Physics3DSystem::GetSettings().m_CaptureOnPlay;
        m_Prefs.ThrottleEditMode = m_ThrottleEditMode;
        m_Prefs.ThrottlePlayMode = m_ThrottlePlayMode;
        m_Prefs.RenderBudgetMs = m_RenderBudgetMs;

        // Frame pacing lives on the Application; read the live values back (#456).
        m_Prefs.FrameRateCap = Application::Get().GetFrameRateCap();
        m_Prefs.FrameTimeSmoothing = Application::Get().GetFrameTimeSmoothing();

        // MCP: port + auto-start are edited in place by the panel; sync redaction
        // (which lives on the server) back so it persists. (#285)
        if (m_McpServer)
            m_Prefs.McpRedactPaths = m_McpServer->RedactPaths();

        if (auto const project = Project::GetActive())
        {
            auto const& cfg = project->GetConfig();
            m_Prefs.EnableAutoSave = cfg.EnableAutoSave;
            m_Prefs.AutoSaveIntervalSeconds = std::clamp(cfg.AutoSaveIntervalSeconds, 10, 7200);
        }
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
                RenderGraphDebugRuntime::GetActiveGraph().Raw(), &m_ShowCommandBucketInspector, "Command Bucket Inspector");
        }

        if (m_ShowRendererProfiler)
        {
            RendererProfiler::GetInstance().RenderUI(&m_ShowRendererProfiler);
        }

        if (m_ShowRenderGraphDebugger)
        {
            static RenderGraphDebugger s_RenderGraphDebugger;
            s_RenderGraphDebugger.RenderDebugView(RenderGraphDebugRuntime::GetActiveGraph(), &m_ShowRenderGraphDebugger, "Render Graph Debugger");
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
        dispatcher.Dispatch<AssetLoadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetLoaded));
        dispatcher.Dispatch<AssetReloadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetReloaded));
        dispatcher.Dispatch<AssetImportedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetImported));
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
                    else
                    {
                        // No additional handling required.
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
        // Same pattern for the instance scatter brush — when in Paint mode,
        // left-click is a stroke deposit, not entity-picking.
        if (m_ShowInstanceScatterBrush && m_InstanceScatterBrushPanel.IsActive() &&
            e.GetMouseButton() == Mouse::ButtonLeft && m_ViewportHovered &&
            !Input::IsKeyPressed(Key::LeftAlt))
        {
            return true;
        }

        if ((m_SceneState != SceneState::Play) && (e.GetMouseButton() == Mouse::ButtonLeft) && m_ViewportHovered && (!ImGuizmo::IsOver()) && (!Input::IsKeyPressed(Key::LeftAlt)))
        {
            if (Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl))
            {
                m_SceneHierarchyPanel.ToggleEntitySelection(m_HoveredEntity);
            }
            else
            {
                m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
            }
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
        if (const auto& selectedEntities = m_SceneHierarchyPanel.GetSelectedEntities(); !selectedEntities.empty())
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
                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(tc.GetRotation()) * glm::scale(glm::mat4(1.0f), tc.Scale + 0.03f);
                    Renderer2D::DrawCircle(transform, glm::vec4(1, 1, 1, 1), 0.03f);
                }

                if (selection.HasComponent<CameraComponent>())
                {
                    auto const& cc = selection.GetComponent<CameraComponent>();

                    if (cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
                    {
                        glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::toMat4(tc.GetRotation()) * glm::scale(glm::mat4(1.0f), glm::vec3(cc.Camera.GetOrthographicSize(), cc.Camera.GetOrthographicSize(), 1.0f) + glm::vec3(0.03f));
                        Renderer2D::DrawRect(transform, glm::vec4(1, 1, 1, 1));
                    }
                    else if (cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                    {
                        // TODO(olbu): Draw the selected camera properly once the Renderer2D can draw triangles/points
                    }
                    else
                    {
                        // No additional handling required.
                    }
                }
            }
        }

        if (Renderer3D::GetRendererSettings().ShowPhysicsColliders)
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

                    glm::mat4 transform = glm::translate(glm::mat4(1.0f), tc.Translation) * glm::rotate(glm::mat4(1.0f), tc.GetRotationEuler().z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), glm::vec3(bc2d.Offset, 0.001f)) * glm::scale(glm::mat4(1.0f), scale);

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
            else if (type == ContentFileType::Cinematic)
            {
                m_CinematicTimelinePanel.OpenSequence(path);
                m_ShowCinematicTimeline = true;
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
                            if (!m_ShaderGraphEditorPanel.SaveIfNeeded())
                                return;
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
            else if (type == ContentFileType::SoundGraph)
            {
                if (m_SoundGraphEditorPanel.HasUnsavedChanges())
                {
                    auto const result = MessagePrompt::YesNoCancel(
                        "Unsaved Sound Graph",
                        "The current sound graph has unsaved changes. Do you want to save before opening a new one?");
                    switch (result)
                    {
                        case MessagePromptResult::Yes:
                            if (!m_SoundGraphEditorPanel.SaveIfNeeded())
                                return;
                            break;
                        case MessagePromptResult::Cancel:
                            return;
                        case MessagePromptResult::No:
                        default:
                            break;
                    }
                }
                m_SoundGraphEditorPanel.OpenSoundGraph(path);
                m_ShowSoundGraphEditor = true;
            }
            else if (type == ContentFileType::Shader)
            {
                if (m_ShaderEditorPanel.HasUnsavedChanges())
                {
                    auto const result = MessagePrompt::YesNoCancel(
                        "Unsaved Shader",
                        "The current shader has unsaved changes. Do you want to save before opening a new one?");

                    switch (result)
                    {
                        case MessagePromptResult::Yes:
                            if (!m_ShaderEditorPanel.Save())
                                return;
                            break;
                        case MessagePromptResult::Cancel:
                            return;
                        case MessagePromptResult::No:
                        default:
                            break;
                    }
                }
                m_ShaderEditorPanel.OpenFile(path);
                m_ShowShaderEditor = true;
            }
            else if (type == ContentFileType::Scene)
            {
                if (ConfirmDiscardChanges())
                {
                    OpenScene(path);
                }
            }
            else
            {
                // No additional handling required.
            } });

        // "Edit in Timeline" on the CinematicComponent inspector opens the
        // referenced sequence in the timeline panel.
        m_SceneHierarchyPanel.SetOpenCinematicTimelineCallback([this](AssetHandle handle)
                                                               {
            m_CinematicTimelinePanel.OpenSequence(handle);
            m_ShowCinematicTimeline = true; });
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
        m_CinematicTimelinePanel.Reset();
        m_ShowCinematicTimeline = false;
        m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
        BindContentBrowserSelectionCallback();
        m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();
        m_BuildGamePanel = CreateScope<BuildGamePanel>();
        m_BuildGamePanel->SetSaveSceneCallback([this]()
                                               { return SaveScene(); });
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

            // Load item definitions before opening scene so deserialization can resolve items
            ItemDatabase::Clear();
            if (auto itemsDir = Project::GetAssetFileSystemPath("Items"); std::filesystem::exists(itemsDir))
            {
                ItemDatabase::LoadFromDirectory(itemsDir.string());
            }

            // Load quest definitions (.oloquest) so QuestSystem::AcceptQuest and
            // scene deserialization can resolve them by ID.
            QuestDatabase::Clear();
            if (auto questsDir = Project::GetAssetFileSystemPath("Quests"); std::filesystem::exists(questsDir))
            {
                QuestDatabase::LoadFromDirectory(questsDir.string());
            }

            auto startScenePath = Project::GetAssetFileSystemPath(Project::GetActive()->GetConfig().StartScene);
            OLO_ASSERT(std::filesystem::exists(startScenePath));
            OpenScene(startScenePath);

            m_DialogueEditorPanel.NewDialogue();
            m_CinematicTimelinePanel.Reset();
            m_ShowCinematicTimeline = false;
            m_ContentBrowserPanel = CreateScope<ContentBrowserPanel>();
            BindContentBrowserSelectionCallback();
            m_AssetPackBuilderPanel = CreateScope<AssetPackBuilderPanel>();
            m_BuildGamePanel = CreateScope<BuildGamePanel>();
            m_BuildGamePanel->SetSaveSceneCallback([this]()
                                                   { return SaveScene(); });

            // Load this project's per-context input action maps, replacing any maps left
            // over from a previously-open project so they can't leak across projects
            // (InputActionManager is init'd once at app startup, not per project). A
            // project with no input config resets to empty. DeserializeContexts also
            // accepts the legacy single-map format (restored as the Gameplay context).
            InputActionSerializer::ContextMaps projectInputContexts;
            if (auto inputMapPath = Project::GetInputActionMapPath(); std::filesystem::exists(inputMapPath))
            {
                if (auto loadedContexts = InputActionSerializer::DeserializeContexts(inputMapPath))
                {
                    projectInputContexts = std::move(*loadedContexts);
                }
            }
            InputActionManager::ReplaceAllContextMaps(projectInputContexts);

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
        std::string const filepath = FileDialogs::OpenFile("OloEditor Scene (*.olo;*.scene)\0*.olo;*.scene\0", initialDir);
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

        if (auto const ext = LowercaseExtension(path); ext != ".olo" && ext != ".scene")
        {
            OLO_WARN("Could not load {0} - not a scene file", path.filename().string());
            return false;
        }

        // Check for a newer auto-save file
        auto autoPath = path;
        autoPath += ".auto";
        if (FileSystem::IsNewer(autoPath, path))
        {
            m_PendingRecoveryScenePath = path;
            m_PendingRecoveryAutoPath = autoPath;
            m_ShowAutoSaveRecovery = true;
            return true; // The modal will handle loading
        }

        Ref<Scene> const newScene = Ref<Scene>::Create();
        if (SceneSerializer serializer(newScene); !serializer.Deserialize(path.string()))
        {
            return false;
        }
        SetEditorScene(newScene);
        m_EditorScenePath = path;
        FrameEditorCameraOnTerrain(newScene);

        // One unified-timeline event for the whole load (#306 item B). The per-entity
        // EntitySpawn flood is suppressed during deserialize (SceneSerializer), so this
        // SceneLoad line stands in for it with the resulting entity count.
        DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::SceneLoad,
            "Loaded scene '" + newScene->GetName() + "' (" +
                std::to_string(static_cast<u64>(newScene->GetAllEntitiesWith<IDComponent>().size())) + " entities)",
            0, path.filename().string());

        Renderer3D::GetPostProcessSettings() = newScene->GetPostProcessSettings();
        Renderer3D::GetSnowSettings() = newScene->GetSnowSettings();
        Renderer3D::GetWindSettings() = newScene->GetWindSettings();
        Renderer3D::GetSnowAccumulationSettings() = newScene->GetSnowAccumulationSettings();
        Renderer3D::GetSnowEjectaSettings() = newScene->GetSnowEjectaSettings();
        Renderer3D::GetPrecipitationSettings() = newScene->GetPrecipitationSettings();
        Renderer3D::GetFogSettings() = newScene->GetFogSettings();

        // Reapply quality tiering over scene-loaded settings
        if (auto project = Project::GetActive())
        {
            ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
            ApplyTieringToSettings(project->GetConfig().QualityTiering, Renderer3D::GetPostProcessSettings(), shadowCopy);
            Renderer3D::GetShadowMap().SetSettings(shadowCopy);
        }

        m_TimeSinceLastAutoSave = 0.0f;
        return true;
    }

    void EditorLayer::FrameEditorCameraOnTerrain(const Ref<Scene>& scene)
    {
        if (!scene)
            return;

        // Terrain geometry spans world [0, worldSize] from its transform origin,
        // so the default origin-focused editor camera looks straight past it and
        // the scene appears empty. Orbit the terrain centre at a distance that
        // fits its footprint, tilted down (positive pitch = look down).
        auto terrainFrameView = scene->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        bool framed = false;
        for (auto terrainEntity : terrainFrameView)
        {
            if (framed)
                continue;
            framed = true;
            const auto& tf = terrainFrameView.get<TransformComponent>(terrainEntity);
            const auto& tc = terrainFrameView.get<TerrainComponent>(terrainEntity);
            const glm::vec3 center = tf.Translation +
                                     glm::vec3(tc.m_WorldSizeX * 0.5f, tc.m_HeightScale * 0.4f, tc.m_WorldSizeZ * 0.5f);
            const f32 distance = std::max(tc.m_WorldSizeX, tc.m_WorldSizeZ) * 1.1f;
            // ~37° downward: a top-down-ish overview that shows the terrain's flat
            // (grass) tops rather than a grazing angle full of steep rock faces.
            m_EditorCamera.Focus(center, distance, 0.0f, 0.65f);
        }
    }

    bool EditorLayer::FrameEditorCameraOnEntity(u64 entityUuid)
    {
        const Ref<Scene> scene = m_ActiveScene;
        if (!scene)
            return false;
        const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
        if (!entityOpt)
            return false;
        Entity entity = *entityOpt;
        if (!entity.HasComponent<TransformComponent>())
            return false;

        // World transform: compose local transforms up the parent chain (the
        // scene stores parent-relative transforms; gizmos and rendering do the
        // same composition).
        glm::mat4 world = entity.GetComponent<TransformComponent>().GetTransform();
        for (UUID parentId = entity.GetParentUUID(); static_cast<u64>(parentId) != 0;)
        {
            const auto parentOpt = scene->TryGetEntityWithUUID(parentId);
            if (!parentOpt)
                break;
            Entity parent = *parentOpt;
            if (parent.HasComponent<TransformComponent>())
                world = parent.GetComponent<TransformComponent>().GetTransform() * world;
            parentId = parent.GetParentUUID();
        }

        // Bounds: prefer real mesh bounds, fall back to a scale-derived radius.
        glm::vec3 center = glm::vec3(world[3]);
        f32 radius = 0.0f;
        if (entity.HasComponent<ModelComponent>())
        {
            if (const auto& model = entity.GetComponent<ModelComponent>().m_Model; model)
            {
                const BoundingBox worldBox = model->GetTransformedBoundingBox(world);
                center = worldBox.GetCenter();
                radius = glm::length(worldBox.GetExtents());
            }
        }
        else if (entity.HasComponent<MeshComponent>())
        {
            if (const auto& meshSource = entity.GetComponent<MeshComponent>().m_MeshSource; meshSource)
            {
                const BoundingBox worldBox = meshSource->GetBoundingBox().Transform(world);
                center = worldBox.GetCenter();
                radius = glm::length(worldBox.GetExtents());
            }
        }
        else if (entity.HasComponent<TerrainComponent>())
        {
            const auto& tc = entity.GetComponent<TerrainComponent>();
            center += glm::vec3(tc.m_WorldSizeX * 0.5f, tc.m_HeightScale * 0.4f, tc.m_WorldSizeZ * 0.5f);
            radius = std::max(tc.m_WorldSizeX, tc.m_WorldSizeZ) * 0.5f;
        }
        if (radius < 0.5f)
        {
            // Scale-derived fallback: length of the world matrix's basis columns.
            const f32 sx = glm::length(glm::vec3(world[0]));
            const f32 sy = glm::length(glm::vec3(world[1]));
            const f32 sz = glm::length(glm::vec3(world[2]));
            radius = std::max({ sx, sy, sz, 0.5f });
        }

        // Keep the current view direction; just re-pivot and fit. Derive the
        // distance from the camera's vertical FOV so the bounding sphere fills
        // the frame at any FOV (a fixed multiplier underfits at the editor's
        // default 30 degrees), with a small margin so silhouettes don't touch
        // the frame edge.
        const f32 fovRadians = glm::radians(m_EditorCamera.GetFOV());
        const f32 fitDistance = (radius / std::tan(fovRadians * 0.5f)) * 1.05f;
        m_EditorCamera.Focus(center, fitDistance, m_EditorCamera.GetYaw(), m_EditorCamera.GetPitch());
        return true;
    }

    bool EditorLayer::SaveScene()
    {
        if (!m_EditorScenePath.empty())
        {
            // Strip tiering overlay so scene stores un-tiered base settings
            m_EditorScene->SetPostProcessSettings(
                StripTieringOverlay(Renderer3D::GetPostProcessSettings(), m_EditorScene->GetPostProcessSettings()));
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

            // Clean up auto-save file on manual save
            DeleteAutoSaveFile();
            m_TimeSinceLastAutoSave = 0.0f;
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

        // Strip tiering overlay so scene stores un-tiered base settings
        m_EditorScene->SetPostProcessSettings(
            StripTieringOverlay(Renderer3D::GetPostProcessSettings(), m_EditorScene->GetPostProcessSettings()));
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

        // Clean up auto-save file on manual save
        DeleteAutoSaveFile();
        m_TimeSinceLastAutoSave = 0.0f;
        return true;
    }

    void EditorLayer::SerializeScene(Ref<Scene> const scene, const std::filesystem::path& path) const
    {
        const SceneSerializer serializer(scene);
        serializer.Serialize(path);
    }

    bool EditorLayer::LoadSceneInternal(const std::filesystem::path& scenePath)
    {
        Ref<Scene> const newScene = Ref<Scene>::Create();
        if (SceneSerializer serializer(newScene); !serializer.Deserialize(scenePath.string()))
        {
            OLO_CORE_ERROR("Failed to deserialize scene '{}'", scenePath.string());
            return false;
        }
        SetEditorScene(newScene);
        Renderer3D::GetPostProcessSettings() = newScene->GetPostProcessSettings();
        Renderer3D::GetSnowSettings() = newScene->GetSnowSettings();
        Renderer3D::GetWindSettings() = newScene->GetWindSettings();
        Renderer3D::GetSnowAccumulationSettings() = newScene->GetSnowAccumulationSettings();
        Renderer3D::GetSnowEjectaSettings() = newScene->GetSnowEjectaSettings();
        Renderer3D::GetPrecipitationSettings() = newScene->GetPrecipitationSettings();
        Renderer3D::GetFogSettings() = newScene->GetFogSettings();

        // Reapply quality tiering over scene-loaded settings
        if (auto project = Project::GetActive())
        {
            ShadowSettings shadowCopy = Renderer3D::GetShadowMap().GetSettings();
            ApplyTieringToSettings(project->GetConfig().QualityTiering, Renderer3D::GetPostProcessSettings(), shadowCopy);
            Renderer3D::GetShadowMap().SetSettings(shadowCopy);
        }

        return true;
    }

    void EditorLayer::AutoSaveScene()
    {
        if (m_EditorScenePath.empty())
        {
            return;
        }

        auto autoPath = m_EditorScenePath;
        autoPath += ".auto";

        // Sync renderer settings into scene before saving
        m_EditorScene->SetPostProcessSettings(
            StripTieringOverlay(Renderer3D::GetPostProcessSettings(), m_EditorScene->GetPostProcessSettings()));
        m_EditorScene->SetSnowSettings(Renderer3D::GetSnowSettings());
        m_EditorScene->SetWindSettings(Renderer3D::GetWindSettings());
        m_EditorScene->SetSnowAccumulationSettings(Renderer3D::GetSnowAccumulationSettings());
        m_EditorScene->SetSnowEjectaSettings(Renderer3D::GetSnowEjectaSettings());
        m_EditorScene->SetPrecipitationSettings(Renderer3D::GetPrecipitationSettings());
        m_EditorScene->SetFogSettings(Renderer3D::GetFogSettings());
        SerializeScene(m_EditorScene, autoPath);

        // Also save editor preferences alongside
        SyncPrefsFromMembers();
        if (Project::GetActive())
        {
            m_EditorPreferencesPanel.Save(m_Prefs, Project::GetProjectDirectory());
        }

        m_TimeSinceLastAutoSave = 0.0f;
        OLO_CORE_INFO("Auto-saved scene to '{0}'", autoPath.string());
    }

    void EditorLayer::DeleteAutoSaveFile() const
    {
        std::error_code ec;
        if (!m_EditorScenePath.empty())
        {
            auto autoPath = m_EditorScenePath;
            autoPath += ".auto";
            std::filesystem::remove(autoPath, ec);
        }
    }

    void EditorLayer::UI_AutoSaveRecoveryModal()
    {
        if (m_ShowAutoSaveRecovery)
        {
            ImGui::OpenPopup("Recover Auto-Save?");
            m_ShowAutoSaveRecovery = false; // Only open once; the popup stays open until user picks
        }

        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Recover Auto-Save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped("An auto-save file was found that is newer than the saved scene:");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", m_PendingRecoveryAutoPath.filename().string().c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Load Auto-Save", ImVec2(140, 0)))
            {
                if (LoadSceneInternal(m_PendingRecoveryAutoPath))
                {
                    m_EditorScenePath = m_PendingRecoveryScenePath;
                    m_TimeSinceLastAutoSave = 0.0f;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    OLO_CORE_ERROR("Auto-save recovery failed: could not deserialize '{}'", m_PendingRecoveryAutoPath.string());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load Original", ImVec2(140, 0)))
            {
                if (LoadSceneInternal(m_PendingRecoveryScenePath))
                {
                    m_EditorScenePath = m_PendingRecoveryScenePath;
                    m_TimeSinceLastAutoSave = 0.0f;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    OLO_CORE_ERROR("Auto-save recovery failed: could not deserialize '{}'", m_PendingRecoveryScenePath.string());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard Auto-Save", ImVec2(140, 0)))
            {
                std::error_code ec;
                std::filesystem::remove(m_PendingRecoveryAutoPath, ec);

                if (LoadSceneInternal(m_PendingRecoveryScenePath))
                {
                    m_EditorScenePath = m_PendingRecoveryScenePath;
                    m_TimeSinceLastAutoSave = 0.0f;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    OLO_CORE_ERROR("Auto-save recovery failed: could not deserialize '{}'", m_PendingRecoveryScenePath.string());
                }
            }

            ImGui::EndPopup();
        }
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
            const auto& cam = cameraEntity.GetComponent<CameraComponent>();
            if (cam.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
            {
                OLO_CORE_WARN("Primary camera '{}' uses Orthographic projection in 3D mode. "
                              "This may cause the viewport to appear empty. Consider switching to Perspective.",
                              cameraEntity.GetName());
            }
        }

        m_ActiveScene->OnRuntimeStart();

        // Stream quest/inventory gameplay events to the Console panel for this
        // Play session (subscriptions are dropped on OnRuntimeStop).
        AttachGameplayEventLogger(*m_ActiveScene);

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_SceneHierarchyPanel.SetCommandHistory(nullptr);
        m_AnimationPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetCommandHistory(nullptr);
        m_AnimationGraphEditorPanel.SetContext(m_ActiveScene);
        m_AnimationGraphEditorPanel.SetCommandHistory(nullptr);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_SaveGamePanel.SetContext(m_ActiveScene, m_Framebuffer);
        m_StreamingPanel.SetCommandHistory(nullptr);
        m_StatisticsPanel.SetContext(m_ActiveScene);
        m_NavMeshPanel.SetContext(m_ActiveScene);
        m_BehaviorTreeEditorPanel.SetContext(m_ActiveScene);
        m_FSMEditorPanel.SetContext(m_ActiveScene);
        m_AudioEventsPanel.SetActiveScene(m_ActiveScene);
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
        m_AnimationGraphEditorPanel.SetContext(m_ActiveScene);
        m_AnimationGraphEditorPanel.SetCommandHistory(nullptr);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_StreamingPanel.SetCommandHistory(nullptr);
        m_StatisticsPanel.SetContext(m_ActiveScene);
        m_NavMeshPanel.SetContext(m_ActiveScene);
        m_BehaviorTreeEditorPanel.SetContext(m_ActiveScene);
        m_FSMEditorPanel.SetContext(m_ActiveScene);
        m_AudioEventsPanel.SetActiveScene(m_ActiveScene);
    }

    void EditorLayer::OnSceneStop()
    {
        using enum OloEngine::EditorLayer::SceneState;
        OLO_CORE_ASSERT(m_SceneState == Play || m_SceneState == Simulate,
                        "OnSceneStop called with unexpected SceneState: {}", static_cast<int>(m_SceneState));

        if (m_SceneState == Play)
        {
            m_ActiveScene->OnRuntimeStop();
        }
        else if (m_SceneState == Simulate)
        {
            m_ActiveScene->OnSimulationStop();
        }
        else
        {
            // No additional handling required.
        }

        m_SceneState = Edit;

        // Reset hovered entity before changing scenes to prevent accessing stale registry
        m_HoveredEntity = Entity();
        m_PickingReadPending = false; // Discard stale PBO data from the old scene

        m_ActiveScene = m_EditorScene;

        m_SceneHierarchyPanel.SetContext(m_ActiveScene);
        m_SceneHierarchyPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationPanel.SetContext(m_ActiveScene);
        m_AnimationPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationGraphEditorPanel.SetContext(m_ActiveScene);
        m_AnimationGraphEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_StreamingPanel.SetContext(m_ActiveScene);
        m_SaveGamePanel.SetContext(nullptr, nullptr);
        m_StreamingPanel.SetCommandHistory(&m_CommandHistory);
        m_StatisticsPanel.SetContext(m_ActiveScene);
        m_NavMeshPanel.SetContext(m_ActiveScene);
        m_BehaviorTreeEditorPanel.SetContext(m_ActiveScene);
        m_FSMEditorPanel.SetContext(m_ActiveScene);
        m_AudioEventsPanel.SetActiveScene(m_ActiveScene);
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
        m_InstanceScatterBrushPanel.SetContext(m_EditorScene);
        m_InstanceScatterBrushPanel.SetCommandHistory(&m_CommandHistory);
        m_StreamingPanel.SetContext(m_EditorScene);
        m_StreamingPanel.SetCommandHistory(&m_CommandHistory);
        m_StatisticsPanel.SetContext(m_EditorScene);
        m_NavMeshPanel.SetContext(m_EditorScene);
        m_BehaviorTreeEditorPanel.SetContext(m_EditorScene);
        m_FSMEditorPanel.SetContext(m_EditorScene);
        m_DialogueEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationGraphEditorPanel.SetContext(m_EditorScene);
        m_AnimationGraphEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_SoundGraphEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_InputSettingsPanel.SetCommandHistory(&m_CommandHistory);
        m_AudioEventsPanel.SetActiveScene(m_EditorScene);

        m_ActiveScene = m_EditorScene;

        // Clear undo history when switching scenes
        m_CommandHistory.Clear();

        // Drop any ephemeral MCP sun-direction override (#316 Part 4) so loading
        // or creating a scene restores its authored procedural-sky sun. This is the
        // single choke point for every editor scene swap (NewScene / OpenScene /
        // auto-save recovery), so clearing here honours the documented "resets on
        // scene reload" contract; a no-op when no override is active.
        Renderer3D::ClearSunDirectionOverride();

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
        // Check shader graph unsaved changes first
        if (m_ShaderGraphEditorPanel.HasUnsavedChanges())
        {
            auto const result = MessagePrompt::YesNoCancel(
                "Unsaved Shader Graph",
                "The current shader graph has unsaved changes. Do you want to save before closing?");

            switch (result)
            {
                case MessagePromptResult::Yes:
                    if (!m_ShaderGraphEditorPanel.SaveIfNeeded())
                    {
                        Application::Get().CancelClose();
                        return true;
                    }
                    break;
                case MessagePromptResult::Cancel:
                    Application::Get().CancelClose();
                    return true;
                case MessagePromptResult::No:
                default:
                    break;
            }
        }

        // Check sound graph unsaved changes — mirrors the shader graph flow above
        if (m_SoundGraphEditorPanel.HasUnsavedChanges())
        {
            auto const result = MessagePrompt::YesNoCancel(
                "Unsaved Sound Graph",
                "The current sound graph has unsaved changes. Do you want to save before closing?");

            switch (result)
            {
                case MessagePromptResult::Yes:
                    if (!m_SoundGraphEditorPanel.SaveIfNeeded())
                    {
                        Application::Get().CancelClose();
                        return true;
                    }
                    break;
                case MessagePromptResult::Cancel:
                    Application::Get().CancelClose();
                    return true;
                case MessagePromptResult::No:
                default:
                    break;
            }
        }

        // Check shader editor unsaved changes
        if (m_ShaderEditorPanel.HasUnsavedChanges())
        {
            auto const result = MessagePrompt::YesNoCancel(
                "Unsaved Shader",
                "The current shader has unsaved changes. Do you want to save before closing?");

            switch (result)
            {
                case MessagePromptResult::Yes:
                    if (!m_ShaderEditorPanel.Save())
                    {
                        Application::Get().CancelClose();
                        return true;
                    }
                    break;
                case MessagePromptResult::Cancel:
                    Application::Get().CancelClose();
                    return true;
                case MessagePromptResult::No:
                default:
                    break;
            }
        }

        if (!ConfirmDiscardChanges())
        {
            Application::Get().CancelClose();
            return true;
        }
        return false;
    }

    bool EditorLayer::BuildMouseRay(const glm::vec2& mousePos, const glm::vec2& viewportSize, Ray& outRay) const
    {
        if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
        {
            return false;
        }

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

        outRay = Ray(glm::vec3(nearWorld), glm::normalize(glm::vec3(farWorld) - glm::vec3(nearWorld)));
        // A degenerate view-projection (uninitialized camera, zero-size
        // viewport mid-resize) yields NaNs through the inverse/normalize.
        return Math::IsFinite(outRay.Origin) && Math::IsFinite(outRay.Direction);
    }

    bool EditorLayer::TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const
    {
        OLO_PROFILE_FUNCTION();

        // Find a terrain entity in the active scene
        Entity terrainEntity;
        auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        if (auto it = view.begin(); it != view.end())
        {
            terrainEntity = Entity(*it, m_ActiveScene.get());
        }
        if (!terrainEntity || !terrainEntity.GetComponent<TerrainComponent>().m_TerrainData)
        {
            return false;
        }

        auto const& tc = terrainEntity.GetComponent<TerrainComponent>();
        auto const& transform = terrainEntity.GetComponent<TransformComponent>();

        Ray mouseRay;
        if (!BuildMouseRay(mousePos, viewportSize, mouseRay))
        {
            return false;
        }
        glm::vec3 rayOrigin = mouseRay.Origin;
        glm::vec3 rayDir = mouseRay.Direction;

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
        std::function<void(YAML::Node)> remapUUIDs = [&uuidMap, &remapUUIDs](YAML::Node node)
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
            else
            {
                // No additional handling required.
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
                for (const auto& uuid : createdUUIDs)
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

    bool EditorLayer::OnAssetLoaded(AssetLoadedEvent const& e)
    {
        OLO_PROFILE_FUNCTION();

        // First-time async-load completion. Unlike OnAssetReloaded we do NOT
        // patch in-scene references: a newly loaded asset wasn't present in
        // any cache yet, so the very next frame's normal resolution path will
        // pick it up. The only thing worth doing here is dropping any
        // placeholder thumbnail the Content Browser may have shown while the
        // asset was still streaming in — the panel will then re-resolve and
        // render the real preview on its next paint.
        if (m_ContentBrowserPanel)
        {
            const AssetType type = e.GetAssetType();
            if (type == AssetType::Material || type == AssetType::Mesh)
            {
                m_ContentBrowserPanel->InvalidateThumbnail(e.GetHandle(), e.GetPath());
            }
            else if (type == AssetType::Texture2D)
            {
                // Same reasoning as OnAssetReloaded: without a per-material
                // dependency graph, a newly available texture might be
                // referenced by any cached material thumbnail. Cheapest fix
                // is to drop them all and re-render lazily on next paint.
                m_ContentBrowserPanel->ClearThumbnails();
            }
            else
            {
                // No additional handling required.
            }
        }

        OLO_TRACE("📦 Asset Loaded Event Received!");
        OLO_TRACE("   Handle: {}", static_cast<u64>(e.GetHandle()));
        OLO_TRACE("   Type: {}", (int)e.GetAssetType());
        OLO_TRACE("   Path: {}", e.GetPath().string());

        return false; // Don't consume — other listeners may want this too.
    }

    bool EditorLayer::OnAssetImported(AssetImportedEvent const& e)
    {
        OLO_PROFILE_FUNCTION();

        // A brand-new file was auto-imported from disk by the asset manager's
        // filesystem watcher. Surface it in the Content Browser so it appears
        // without a manual import or F5: mark the directory that now contains it
        // dirty so the panel rescans it on the next paint. (The Content Browser
        // grid is built from a cached filesystem tree, not the asset registry,
        // so the registry import alone wouldn't make it show up.)
        if (m_ContentBrowserPanel)
        {
            m_ContentBrowserPanel->OnAssetImported(e.GetPath());
        }

        DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::AssetReload,
            std::string("Auto-imported ") + AssetUtils::AssetTypeToString(e.GetAssetType()) + " '" +
                e.GetPath().filename().string() + "'",
            static_cast<u64>(e.GetHandle()), e.GetPath().string());

        OLO_TRACE("✨ Asset Imported Event Received!");
        OLO_TRACE("   Handle: {}", static_cast<u64>(e.GetHandle()));
        OLO_TRACE("   Type: {}", (int)e.GetAssetType());
        OLO_TRACE("   Path: {}", e.GetPath().string());

        return false; // Don't consume — other listeners may want this too.
    }

    bool EditorLayer::OnAssetReloaded(AssetReloadedEvent const& e)
    {
        // Unified diagnostics timeline (#306 item B): a hot-reload is exactly the kind of
        // "what just happened" an agent wants to correlate with a visual/behaviour change.
        DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::AssetReload,
            std::string("Reloaded ") + AssetUtils::AssetTypeToString(e.GetAssetType()) + " '" +
                e.GetPath().filename().string() + "'",
            0, e.GetPath().string());

        // Notify the rendering system so it can log generation changes
        // and verify next-frame refresh is clean.
        Renderer3D::OnAssetReloaded(e);

        // Invalidate any cached Content Browser thumbnail for the
        // reloaded asset. Materials get re-rendered with their new
        // factors / textures on the next panel paint; meshes after a
        // re-import similarly re-fetch a fresh icosphere render. We
        // also speculatively invalidate when a *Texture2D* changes,
        // because a material thumbnail may depend on it — without a
        // per-material dependency graph the cheap fix is "if a texture
        // reloads, blow away material previews too." Materials live
        // bounded (256 entries max) so the re-render cost is small.
        if (m_ContentBrowserPanel)
        {
            const AssetType type = e.GetAssetType();
            if (type == AssetType::Material || type == AssetType::Mesh)
            {
                m_ContentBrowserPanel->InvalidateThumbnail(e.GetHandle(), e.GetPath());
            }
            else if (type == AssetType::Texture2D)
            {
                m_ContentBrowserPanel->ClearThumbnails();
            }
            else
            {
                // No additional handling required.
            }
        }

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
            case AssetType::AnimationGraph:
            {
                OLO_TRACE("   → Animation graph asset reloaded - refreshing runtime graphs");
                auto reloadInScene = [&e](Ref<Scene>& scene)
                {
                    if (!scene)
                        return;
                    auto view = scene->GetAllEntitiesWith<AnimationGraphComponent>();
                    for (auto entityID : view)
                    {
                        auto& graphComp = view.get<AnimationGraphComponent>(entityID);
                        if (graphComp.AnimationGraphAssetHandle == e.GetHandle())
                        {
                            // Clear runtime graph so it gets re-loaded next frame
                            graphComp.RuntimeGraph = nullptr;
                        }
                    }
                };
                reloadInScene(m_ActiveScene);
                if (m_EditorScene && m_EditorScene != m_ActiveScene)
                    reloadInScene(m_EditorScene);
                break;
            }
            case AssetType::SoundGraph:
            {
                // Sound graph reloaded on disk: re-fetch the updated SoundGraphAsset, cook a
                // fresh prototype + instance, and ReplaceGraph() on every live source that was
                // instantiated from this asset handle. The source-asset-handle field is set
                // when Scene::InitAudioRuntime creates the SoundGraphSound; sources created
                // some other way (e.g. tests) without that field set are skipped.
                OLO_TRACE("   → Sound graph asset reloaded - refreshing live audio sources");
                auto graphAsset = AssetManager::GetAsset<SoundGraphAsset>(e.GetHandle());
                if (!graphAsset)
                {
                    OLO_WARN("SoundGraph reload: failed to fetch updated asset {}", static_cast<u64>(e.GetHandle()));
                    break;
                }
                const Ref<Audio::SoundGraph::Prototype>& prototype = graphAsset->GetCompiledPrototype();
                if (!prototype)
                {
                    OLO_WARN("SoundGraph reload: asset {} has no compiled prototype after reload", static_cast<u64>(e.GetHandle()));
                    break;
                }

                auto refreshInScene = [&e, &prototype](Ref<Scene>& scene)
                {
                    if (!scene)
                        return;
                    auto view = scene->GetAllEntitiesWith<AudioSoundGraphComponent>();
                    for (auto entityID : view)
                    {
                        auto& sgc = view.get<AudioSoundGraphComponent>(entityID);
                        if (sgc.SoundGraphHandle != e.GetHandle() || !sgc.Sound)
                            continue;

                        auto* source = sgc.Sound->GetSource();
                        if (!source || source->GetSourceAssetHandle() != e.GetHandle())
                            continue;

                        Ref<Audio::SoundGraph::SoundGraph> newInstance = Audio::SoundGraph::CreateInstance(prototype);
                        if (!newInstance)
                        {
                            OLO_WARN("SoundGraph reload: CreateInstance returned null for asset {}", static_cast<u64>(e.GetHandle()));
                            continue;
                        }
                        source->ReplaceGraph(newInstance);
                        OLO_TRACE("SoundGraph reload: replaced graph on entity {} from asset {}",
                                  static_cast<u64>(static_cast<entt::entity>(entityID)),
                                  static_cast<u64>(e.GetHandle()));
                    }
                };
                refreshInScene(m_ActiveScene);
                if (m_EditorScene && m_EditorScene != m_ActiveScene)
                    refreshInScene(m_EditorScene);

                // Let the visual editor panel reconcile against the new on-disk version.
                // If the user is in the middle of editing the same graph it'll prompt
                // before clobbering their work; otherwise it just reloads.
                m_SoundGraphEditorPanel.NotifyAssetReloaded(e.GetHandle(), e.GetPath());
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

        // Create a promise/future pair so the destructor can join on the
        // background task and guarantee the lambda (which captures `this`)
        // finishes before member state is destroyed.
        auto buildDone = std::make_shared<std::promise<void>>();
        m_BuildFuture = buildDone->get_future();

        // Start async build task using Task System
        Tasks::Launch("BuildAssetPack", [this, settings, buildDone]()
                      {
            try
            {
                auto result = AssetPackBuilder::BuildFromActiveProject(settings, m_BuildProgress, &m_BuildCancelRequested);

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
                m_BuildInProgress.store(false);
                buildDone->set_value();
            }
            catch (const std::exception& ex)
            {
                OLO_CORE_ERROR("Asset Pack build exception: {}", ex.what());
                AssetPackBuilder::BuildResult errorResult{};
                errorResult.m_Success = false;
                errorResult.m_ErrorMessage = ex.what();
                errorResult.m_OutputPath.clear();
                errorResult.m_AssetCount = 0;
                errorResult.m_SceneCount = 0;
                m_LastBuildResult = errorResult;
                m_BuildInProgress.store(false);
                buildDone->set_value();
            } }, Tasks::ETaskPriority::BackgroundNormal);

        OLO_CORE_INFO("Asset Pack build started asynchronously...");
    }

    void EditorLayer::BuildShaderPack() const
    {
        OLO_PROFILE_FUNCTION();

        const std::filesystem::path outputPath = "assets/ShaderPack.osp";
        OLO_CORE_INFO("Building Shader Pack to '{}'...", outputPath.string());

        bool success = ShaderPack::CreateFromLibraries(
            Renderer2D::GetShaderLibrary(),
            Renderer3D::GetShaderLibrary(),
            outputPath);

        if (success)
        {
            OLO_CORE_INFO("Shader Pack built successfully: {}", outputPath.string());
        }
        else
        {
            OLO_CORE_ERROR("Shader Pack build failed");
        }
    }

    void EditorLayer::ValidateAssetReferences() const
    {
        OLO_PROFILE_FUNCTION();

        auto project = Project::GetActive();
        if (!project)
        {
            OLO_CORE_WARN("Validate Asset References: no active project");
            return;
        }

        Ref<AssetManagerBase> assetManager = project->GetAssetManager();
        if (!assetManager)
        {
            OLO_CORE_WARN("Validate Asset References: no active asset manager");
            return;
        }

        const AssetReferenceValidationReport report = assetManager->ValidateReferences();

        if (report.IsValid())
        {
            OLO_CORE_INFO("Validate Asset References: checked {} reference(s); no dangling references found.",
                          report.CheckedReferenceCount);
            return;
        }

        OLO_CORE_WARN("Validate Asset References: checked {} reference(s); found {} dangling reference(s):",
                      report.CheckedReferenceCount, report.DanglingCount());
        for (const auto& dangling : report.DanglingReferences)
        {
            const AssetMetadata referencerMeta = assetManager->GetAssetMetadata(dangling.Referencer);
            const std::string referencerLabel = referencerMeta.FilePath.empty()
                                                    ? std::to_string(static_cast<u64>(dangling.Referencer))
                                                    : referencerMeta.FilePath.string();
            OLO_CORE_WARN("  - '{}' references missing asset {} ({})",
                          referencerLabel,
                          static_cast<u64>(dangling.Reference),
                          AssetUtils::AssetTypeToString(dangling.ReferenceType));
        }
    }

} // namespace OloEngine
