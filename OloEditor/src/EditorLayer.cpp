#include "OloEnginePCH.h"
#include <optional>
#include "OloEngine/Core/Interactivity.h"
#include "OloEngine/Core/Environment.h"
#include "EditorLayer.h"
#include "Panels/AssetPackBuilderPanel.h"
#include "Panels/BuildGamePanel.h"
#include "MCP/McpScriptTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpServerPanel.h"
#include "MCP/McpTools.h"
#include "OloEngine/Renderer/Preview/AssetPreviewRenderer.h"
#include "OloEngine/Core/SyntheticInput.h"

#include <backends/imgui_impl_glfw.h>
#include <imgui_internal.h> // GImGui->HoveredWindow/HoveredId/ActiveId for GetMcpInputState (issue #921)
#include <GLFW/glfw3.h>
#include <stb_image/stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <vector>
#include "UndoRedo/EntityCommands.h"
#include "UndoRedo/ComponentCommands.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainGPUPicker.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
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
#include "OloEngine/Scene/SceneCameraFraming.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSystem.h"
#include "OloEngine/Scene/SceneTransition.h"
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
#include "OloEngine/Asset/Interchange/MeshExporterRegistry.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/SceneLightmap.h"
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

    // How to answer the "Recover Auto-Save?" prompt when OpenScene finds a newer
    // .auto file. Prompt = show the modal (the interactive default).
    enum class AutoSaveRecoveryChoice
    {
        Prompt,
        Autosave, // load the .auto (the recovered work)
        Original, // load the saved scene, leaving the .auto in place
        Discard,  // delete the .auto, then load the saved scene
    };

    // A headless / agent session can't click the recovery modal — synthetic Win32
    // clicks don't reach ImGui — so a newer auto-save would wedge the editor at a
    // modal it can never dismiss (issue #316). OLO_EDITOR_AUTOSAVE_RECOVERY
    // lets an automated launch pre-answer it: 'autosave'/'recover', 'original'/'keep',
    // or 'discard'/'delete' (case-insensitive). Unset / empty / unrecognized keeps
    // the interactive modal, so this never changes a human's editor.
    [[nodiscard]] AutoSaveRecoveryChoice ResolveAutoSaveRecoveryChoice()
    {
        const std::optional<std::string> env = OloEngine::Env::Get("OLO_EDITOR_AUTOSAVE_RECOVERY");
        if (!env)
        {
            // No explicit answer. A non-interactive process must not block on
            // the modal, so take the least destructive option: keep what is on
            // disk and leave the recovery file alone.
            if (OloEngine::IsNonInteractive())
            {
                OLO_CORE_WARN("Non-interactive: auto-save recovery prompt answered 'original' "
                              "(set OLO_EDITOR_AUTOSAVE_RECOVERY to choose)");
                return AutoSaveRecoveryChoice::Original;
            }
            return AutoSaveRecoveryChoice::Prompt;
        }
        std::string value(*env);
        std::ranges::transform(value, value.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
        if (value == "autosave" || value == "recover" || value == "auto")
            return AutoSaveRecoveryChoice::Autosave;
        if (value == "original" || value == "keep" || value == "saved")
            return AutoSaveRecoveryChoice::Original;
        if (value == "discard" || value == "delete")
            return AutoSaveRecoveryChoice::Discard;
        return AutoSaveRecoveryChoice::Prompt; // unrecognized -> safe interactive default
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
        // Same contract for an in-flight lightmap bake (issue #439).
        m_LightmapBakeCancel.store(true);
        if (m_LightmapBakeFuture.valid())
        {
            m_LightmapBakeFuture.wait();
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
        //
        // `region` (issue #607) reads back only a sub-rect, in top-left-origin
        // pixel coordinates of the attachment, so a small enough rect never hits
        // the maxWidth downscale and comes back at 1:1 — the only way to measure a
        // pixel-scale artifact on a 4K viewport. An out-of-bounds region is a
        // failure (empty bytes), never a silent clamp: a quietly shrunk rect would
        // report the wrong spatial period without saying so.
        std::vector<u8> CaptureFramebufferPng(const Ref<Framebuffer>& framebuffer, int maxWidth,
                                              MCP::McpCaptureRegion region = {})
        {
            if (!framebuffer)
                return {};
            const auto& spec = framebuffer->GetSpecification();
            const u32 fullWidth = spec.Width;
            const u32 fullHeight = spec.Height;
            if (fullWidth == 0 || fullHeight == 0)
                return {};
            const RHI::ResourceHandle attachment = framebuffer->GetColorAttachmentHandle(0);
            if (!attachment.IsValid())
                return {};

            if (region.IsWholeImage())
                region = MCP::McpCaptureRegion{ 0, 0, fullWidth, fullHeight };
            // `extent > remaining`, not `offset + extent > size`, so a huge pair
            // cannot wrap the u32 addition and slip past the check.
            else if (region.X >= fullWidth || region.Y >= fullHeight ||
                     region.Width > fullWidth - region.X || region.Height > fullHeight - region.Y)
                return {};

            const u32 width = region.Width;
            const u32 height = region.Height;
            // The rect arrives top-left-origin. Row order is ONE per backend
            // for every off-screen target (ADR 0011 amendment (85)): GL
            // stores bottom-up, Vulkan top-down. The shared predicate matches
            // the viewport widget's uv choice so capture and screen can never
            // disagree.
            const bool bottomUpRows = ImGuiLayer::RenderTargetRowsAreBottomUp();
            const u32 readY = bottomUpRows ? fullHeight - region.Y - region.Height : region.Y;

            std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
            if (!RenderCommand::ReadTextureSubImage(attachment, 0, static_cast<i32>(region.X),
                                                    static_cast<i32>(readY), 0, width, height, 1u,
                                                    RHI::Format::RGBA8UNorm, pixels.size(), pixels.data()))
                return {};

            // PNG rows are top-down; only bottom-up (GL) rows need the flip.
            const u32 rowBytes = width * 4;
            std::vector<u8> flipped(pixels.size());
            for (u32 y = 0; y < height; ++y)
            {
                const u32 srcRow = bottomUpRows ? height - 1 - y : y;
                std::memcpy(flipped.data() + static_cast<sizet>(y) * rowBytes,
                            pixels.data() + static_cast<sizet>(srcRow) * rowBytes, rowBytes);
            }

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

        // The project path is the first POSITIONAL argument. Skipping the
        // `--flag` family matters as soon as the app takes any flag of its own
        // (`--rhi=vulkan`, `--smoke-test`): argv[1] was read unconditionally, so
        // a leading flag was opened as a project file and the editor came up
        // with no project at all ("Failed to load project file '--rhi=vulkan'").
        const char* projectFilePath = nullptr;
        if (const auto commandLineArgs = Application::Get().GetSpecification().CommandLineArgs; commandLineArgs.Count > 1)
        {
            for (int i = 1; i < commandLineArgs.Count; ++i)
            {
                if (const char* arg = commandLineArgs[i]; arg != nullptr && arg[0] != '-')
                {
                    projectFilePath = arg;
                    break;
                }
            }
        }
        if (projectFilePath != nullptr)
        {
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

        // Apply the config's renderer settings to the freshly-built render graph
        // (#534). TryInitialize3DMode ran Renderer3D::Init, which builds the graph
        // from the real window size, so the graph exists here — but nothing had yet
        // pushed the RendererSettings defaults (depth-prepass / Forward+ auto-switch)
        // into it, so the editor booted with those silently off until a panel toggle.
        ApplyRendererSettingsToGraph();

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
            mcpContext.CaptureViewportPng = [this](int maxWidth, MCP::McpCaptureRegion region) -> std::vector<u8>
            {
                // Capture what the viewport actually displays (see UI_Viewport): in 3D
                // mode that's the UICompositePass output (fallback SceneColor), not
                // m_Framebuffer, which would otherwise yield a stale/blank image.
                Ref<Framebuffer> target = m_Framebuffer;
                if (m_Is3DMode)
                {
                    // ColorBlindColor first (issue #458): the accessibility stage runs
                    // AFTER UICompositePass, so resolving UIComposite here would hand
                    // back the pre-adaptation image and an MCP screenshot would show a
                    // frame the player never sees.
                    if (auto cb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor); cb)
                        target = cb;
                    else if (auto ui = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); ui)
                        target = ui;
                    else if (auto scene = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); scene)
                        target = scene;
                }
                return CaptureFramebufferPng(target, maxWidth, region);
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
            // Consented, undoable project writes (#306). The write tools run
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
            // olo_scene_open (#316): open/switch the active scene. Resolves the
            // path against the project asset directory when relative, stops Play mode
            // if running, and loads the scene DIRECTLY via LoadEditorSceneFile — never
            // raising the auto-save recovery modal (a remote agent can't click it).
            // Main-thread-only (EnTT registry / renderer settings), so the MCP server
            // calls it from a MarshalRead job.
            mcpContext.OpenSceneFromMcp = [this](const std::string& path) -> MCP::McpSceneOpenResult
            {
                MCP::McpSceneOpenResult result;
                result.Available = true;

                std::filesystem::path scenePath(path);
                if (scenePath.is_relative() && Project::GetActive())
                    scenePath = Project::GetAssetFileSystemPath(scenePath);
                result.Path = scenePath.string();

                if (auto const ext = LowercaseExtension(scenePath); ext != ".olo" && ext != ".scene")
                {
                    result.Message = "Not a scene file (expected .olo or .scene): " + scenePath.string();
                    return result;
                }
                std::error_code ec;
                if (!std::filesystem::exists(scenePath, ec))
                {
                    result.Message = "Scene file not found: " + scenePath.string();
                    return result;
                }

                // The runtime scene in Play/Simulate is a transient copy; stop first so
                // the load replaces the authored (Edit-mode) scene cleanly.
                if (m_SceneState != SceneState::Edit)
                    OnSceneStop();

                if (!LoadEditorSceneFile(scenePath, scenePath))
                {
                    result.Message = "Failed to load scene (deserialize error — see the engine log): " + scenePath.string();
                    return result;
                }

                result.Ok = true;
                result.SceneName = m_ActiveScene ? m_ActiveScene->GetName() : std::string{};
                result.EntityCount = m_ActiveScene
                                         ? static_cast<u32>(m_ActiveScene->GetAllEntitiesWith<IDComponent>().size())
                                         : 0;
                result.Message = "Opened scene '" + result.SceneName + "' (" +
                                 std::to_string(result.EntityCount) + " entities).";
                return result;
            };
            // olo_scene_play / olo_scene_stop (#316): toggle Play mode — the
            // same OnScenePlay / OnSceneStop the editor's Play/Stop buttons drive.
            // Idempotent: a redundant call reports changed:false. Entering Play can
            // fail when the scene has no primary camera (OnScenePlay reverts to Edit),
            // reported as ok:false. Main-thread-only (mutates scene state / runs the
            // runtime start-stop), so it runs inside a MarshalRead job.
            mcpContext.SetScenePlayState = [this](bool play) -> MCP::McpScenePlayResult
            {
                MCP::McpScenePlayResult result;
                result.Available = true;

                if (play)
                {
                    if (m_SceneState == SceneState::Play)
                    {
                        result.Ok = true;
                        result.Playing = true;
                        result.Message = "Already in Play mode.";
                    }
                    else
                    {
                        // From Simulate, OnScenePlay stops it first; from Edit it enters
                        // Play. It reverts to Edit if the scene has no primary camera.
                        OnScenePlay();
                        const bool nowPlaying = m_SceneState == SceneState::Play;
                        result.Ok = nowPlaying;
                        result.Playing = nowPlaying;
                        result.Changed = nowPlaying;
                        result.Message = nowPlaying
                                             ? "Entered Play mode."
                                             : "Could not enter Play mode: the scene has no primary CameraComponent "
                                               "(see the engine log). The editor stayed in Edit mode.";
                    }
                }
                else
                {
                    if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
                    {
                        OnSceneStop();
                        result.Ok = true;
                        result.Changed = true;
                        result.Message = "Stopped Play mode; restored the authored scene.";
                    }
                    else
                    {
                        result.Ok = true;
                        result.Message = "Already stopped (Edit mode).";
                    }
                    result.Playing = false;
                }

                result.SceneName = m_ActiveScene ? m_ActiveScene->GetName() : std::string{};
                return result;
            };
            // olo_input_inject (#607): synthetic mouse/keyboard input. All three run on
            // the game thread (inside a MarshalRead job) — ImGuiIO and the engine event
            // dispatch are not thread-safe, so nothing here may be called from the HTTP
            // worker. QueueMcpInput only ENQUEUES; the events are applied one frame at a
            // time by DrainMcpInputQueue at the top of OnUpdate.
            mcpContext.GetInputViewportInfo = [this]() -> MCP::McpInputViewportInfo
            { return GetMcpInputViewportInfo(); };
            mcpContext.InjectInput = [this](const MCP::McpInputPlan& plan) -> MCP::McpInputInjectResult
            { return QueueMcpInput(plan); };
            mcpContext.GetInputState = [this]() -> MCP::McpInputStateSnapshot
            { return GetMcpInputState(); };
            // olo_editor_select_entity (#607): select/clear the Scene Hierarchy
            // panel's selection so the Properties inspector draws the requested
            // entity — see SelectEntityInEditor for the UUID resolution + the
            // "leave the current selection untouched on a bad uuid" contract.
            mcpContext.SelectEntityInEditor = [this](u64 entityUuid, bool clear) -> MCP::McpSelectEntityResult
            { return SelectEntityInEditor(entityUuid, clear); };
            mcpContext.GetFrameIndex = [this]() -> u64
            { return m_FrameIndex; };
            mcpContext.IsCaptureUnready = [this]() -> bool
            { return IsViewportCaptureUnready(); };
            // The superset of the two hooks above plus the window state (#607): the
            // one call that can say "the editor is minimized, so nothing you are
            // about to read or inject can work" instead of quietly succeeding.
            mcpContext.GetEditorLiveness = [this]() -> MCP::McpEditorLiveness
            { return GetMcpEditorLiveness(); };

            m_McpServer = CreateScope<MCP::McpServer>(std::move(mcpContext));
            MCP::RegisterBuiltinTools(*m_McpServer);
            // Project-authored Lua script tools (issue #357 / ADR 0005): scan
            // <project assets>/McpTools so the tool set is populated before the
            // server starts. Since issue #607 the scan is also safe to repeat
            // LIVE — olo_script_tools_reload (registered here) and the MCP panel's
            // "Reload script tools" button republish the set through an atomic
            // copy-on-write swap and notify connected agents, so a script edit
            // needs neither a server restart nor an editor restart.
            if (const auto scriptDir = MCP::DefaultScriptToolsDirectory(); !scriptDir.empty())
            {
                MCP::RegisterScriptToolsReloadTool(*m_McpServer, scriptDir);
                (void)MCP::LoadScriptTools(*m_McpServer, scriptDir);
            }
            // Apply the persisted redaction preference (loaded by OpenProject above).
            m_McpServer->SetRedactPaths(m_Prefs.McpRedactPaths);

            // Auto-start is opt-in and explicit: either the persisted preference
            // (Window > MCP Server > "Start automatically") or the OLO_MCP_AUTOSTART
            // env var (for headless attach / the smoke test). Default stays off.
            if (Env::IsTruthy("OLO_MCP_AUTOSTART") || m_Prefs.McpAutoStart)
            {
                auto port = static_cast<u16>(std::clamp(m_Prefs.McpPort, 1024, 65535));
                if (const std::optional<i64> parsed = Env::GetInt("OLO_MCP_PORT");
                    parsed && *parsed >= 1024 && *parsed <= 65535)
                {
                    port = static_cast<u16>(*parsed);
                }
                if (m_McpServer->Start(port))
                {
                    m_ShowMcpPanel = true; // surface the panel so the token is visible

                    // Session write consent (issue #306) defaults to Disabled
                    // and is never persisted, so a headless launch would otherwise
                    // refuse every olo_scene_open/olo_scene_play/olo_scene_stop call
                    // with no way to click the MCP panel's radio buttons.
                    // OLO_MCP_ALLOW_WRITES is the explicit, opt-in escape hatch for a
                    // deliberately-launched automated session (e.g.
                    // scripts/perf/run-perf-battery.ps1) — same spirit as
                    // OLO_MCP_AUTOSTART, and still off by default for an interactive
                    // user. Gated on Start() succeeding: arming write consent on a
                    // server that never actually started listening is meaningless at
                    // best and misleading state to carry if it's started later.
                    if (Env::IsTruthy("OLO_MCP_ALLOW_WRITES"))
                    {
                        OLO_CORE_INFO("OLO_MCP_ALLOW_WRITES set - MCP write consent = AllowSession");
                        m_McpServer->SetAllowWrites(true);
                    }
                }
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

        // Drop any synthetic input a half-drained plan left held (olo_input_inject,
        // #607) — the overlay is process-wide static state, so a key left "down" here
        // would outlive the editor layer.
        m_McpInputQueue.clear();
        ReleaseSyntheticMouseButtons();
        RestoreCursorOverWindowState();
        SyntheticInput::Reset();

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
        // Raw GL: PBO-backed async readback of the entity-ID attachment. The
        // whole path (init, per-frame read, teardown) is glad calls, so under
        // any non-GL backend it is skipped entirely and m_PickingPBOInitialized
        // stays false — which is already the "no picking this frame" gate in
        // OnUpdate. Hover-picking on Vulkan needs the RHI readback path
        // (#691), not a port of this one.
        if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
        {
            OLO_CORE_INFO("[RHI] Editor entity picking disabled: the PBO readback path is OpenGL-only "
                          "(#691)");
            return;
        }
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

    // ---- MCP synthetic input injection (olo_input_inject, issue #607) ------------
    //
    // WHICH LAYER, AND WHY. There are four candidate seams and only one is right:
    //
    //  1. The OS (Win32 SendInput / SetCursorPos) — REJECTED. When an agent drives the
    //     editor it is a background window (Windows will not let a background process
    //     take foreground), so OS-level input lands in whatever window IS focused. That
    //     is not merely useless: it would type into the user's real foreground app.
    //  2. ImGuiIO alone (io.AddMousePosEvent / AddMouseButtonEvent) — INSUFFICIENT. It
    //     drives ImGui widgets but produces no engine Event, so EditorLayer's viewport
    //     picking (OnMouseButtonPressed) never fires. A click would select nothing.
    //  3. The engine Event dispatch alone (Application::OnEvent) — INSUFFICIENT, and the
    //     mirror image: viewport picking fires, but ImGui never sees the click, so no
    //     button, menu, or hierarchy row responds. It would also break the picking it
    //     appears to serve, since the hovered entity is read at ImGui's mouse position.
    //  4. The ImGui GLFW BACKEND CALLBACKS — CHOSEN. ImGui_ImplGlfw_CursorPosCallback /
    //     MouseButtonCallback / KeyCallback / CharCallback are the exact functions GLFW
    //     invokes for real input. Each feeds ImGuiIO *and* chains to the engine's own
    //     previously-installed GLFW callback (the backend was initialised with
    //     install_callbacks=true, so it saved them), which raises the corresponding
    //     engine Event through Application::OnEvent -> the layer stack. One call, both
    //     consumers, on exactly the path real input takes.
    //
    // One gap remains at seam 4: the engine's Input:: API answers from the CURRENT
    // HARDWARE state (glfwGetKey / glfwGetMouseButton / glfwGetCursorPos), which a
    // synthetic event cannot move — and the editor mixes styles (OnMouseButtonPressed
    // reacts to the event but reads its modifier via Input::IsKeyPressed(LeftControl)).
    // OloEngine::SyntheticInput is the overlay that closes it: the platform Input
    // implementations OR this state over the hardware state.

    MCP::McpInputInjectResult EditorLayer::QueueMcpInput(const MCP::McpInputPlan& plan)
    {
        MCP::McpInputInjectResult result;
        result.Available = true;

        if (plan.Frames.empty())
        {
            result.Message = "Empty input plan (nothing to inject).";
            return result;
        }
        // Refuse to interleave two plans: the second one's press would land in the
        // middle of the first one's drag and neither would mean anything.
        if (!m_McpInputQueue.empty())
        {
            result.Message = "An injected input sequence is still in flight (" +
                             std::to_string(m_McpInputQueue.size()) + " frame(s) left). Retry once it drains.";
            return result;
        }

        for (const auto& frame : plan.Frames)
            m_McpInputQueue.push_back(frame);

        result.Ok = true;
        result.BaseFrame = m_FrameIndex;
        result.FrameCount = static_cast<u32>(plan.Frames.size());
        result.Message = "Queued " + std::to_string(result.FrameCount) + " frame(s) of synthetic input.";
        return result;
    }

    void EditorLayer::DrainMcpInputQueue()
    {
        // ImGui applies its event queue in NewFrame, which runs AFTER this drain, so
        // the position fed in last tick is only observable now. Sampling here — on
        // every tick, in flight or not — is what lets the tool answer "did the
        // injected position actually take?" instead of guessing (issue #854).
        SampleMcpCursorLanding();

        if (m_McpInputQueue.empty())
        {
            // Nothing in flight: make sure a previous plan left no key stuck down.
            if (m_McpSyntheticCtrl || m_McpSyntheticShift || m_McpSyntheticAlt)
            {
                m_McpSyntheticCtrl = false;
                m_McpSyntheticShift = false;
                m_McpSyntheticAlt = false;
            }
            // ...and hand the cursor back a few quiet frames after the plan ended, not
            // on the frame it ended. Restoring immediately would blank ImGui's cursor
            // (the physical mouse is elsewhere) BEFORE the tool reads the state change
            // the injection caused, so every reply's `after.viewportHovered` /
            // `hoveredEntity` would come back empty — trading one wrong answer for
            // another. The countdown is short and unconditional: the plan always ends
            // with trailing settle frames, the caller reads one frame past the plan,
            // and this fires shortly after that. OnDetach restores without waiting.
            if (m_McpCursorRestoreCountdown > 0 && --m_McpCursorRestoreCountdown == 0)
                RestoreCursorOverWindowState();
            return;
        }
        // A new plan supersedes any pending hand-back: it is about to re-assert the
        // latch anyway, and letting the countdown fire mid-plan would drop it.
        m_McpCursorRestoreCountdown = 0;

        // Hold the backend's cursor-enter latch for as long as the plan runs, and
        // re-assert it every frame: a real cursor-LEAVE arriving mid-plan (the human
        // moves the physical mouse off the window) would otherwise drop it and hand
        // the rest of the plan back to the hardware cursor half way through.
        AssertSyntheticCursorOverWindow();

        const std::vector<MCP::McpInputEvent> frame = std::move(m_McpInputQueue.front());
        m_McpInputQueue.pop_front();

        for (const MCP::McpInputEvent& event : frame)
            ApplyMcpInputEvent(event);

        // Re-assert the synthetic modifiers into ImGui LAST. Every ImGui_ImplGlfw_*
        // callback calls ImGui_ImplGlfw_UpdateKeyModifiers, which recomputes io.KeyMods
        // by polling the REAL keyboard — so the synthetic Ctrl we pressed a frame ago
        // is cleared by the very mouse-button callback that needs it. Queueing the mod
        // events after the frame's events makes ours the last word (ImGui applies its
        // input queue in order at NewFrame). Only ever forced TRUE: a synthetic
        // modifier must not mask one a human is genuinely holding.
        ImGuiIO& io = ImGui::GetIO();
        if (m_McpSyntheticCtrl)
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (m_McpSyntheticShift)
            io.AddKeyEvent(ImGuiMod_Shift, true);
        if (m_McpSyntheticAlt)
            io.AddKeyEvent(ImGuiMod_Alt, true);

        // The last frame of the plan has drained: release the cursor override so the
        // real mouse takes over again. Any KEY the plan left held stays held — that is
        // the documented contract of keyAction "press". A mouse BUTTON is different:
        // no action leaves one held on purpose, so one still down here means the plan
        // was half applied, and a button ImGui believes is held pins its ActiveId and
        // makes IsWindowHovered() false for every panel — i.e. it silently swallows
        // every later click in the session (issue #854). Teardown restores it.
        if (m_McpInputQueue.empty())
        {
            SyntheticInput::ClearMousePosition();
            ReleaseSyntheticMouseButtons();
            // Arm the hand-back rather than doing it here: see the countdown at the
            // top of this function for why it must not land before the caller reads
            // the state the plan produced.
            m_McpCursorRestoreCountdown = s_McpCursorRestoreDelayFrames;
        }
    }

    // Assert to the ImGui GLFW backend that the cursor is inside this window.
    //
    // Why this is needed at all (issue #854). ImGui_ImplGlfw_UpdateMouseData(), called
    // from ImGui_ImplGlfw_NewFrame(), contains a fallback: when the window is FOCUSED
    // and bd->MouseWindow is null — i.e. the physical mouse is not over any editor
    // window, the normal state for an agent-driven session — it polls glfwGetCursorPos
    // and queues the HARDWARE position. The drain above runs before ImGuiLayer::Begin,
    // so our synthetic position is queued FIRST and the hardware one LAST, and ImGui
    // applies its queue in order. The injected position was therefore discarded on
    // every single frame, and the click landed wherever the physical mouse happened to
    // be — while the tool reported ok. Both halves of #854 are that one fact: the drag
    // never reached the pins, and every LATER injection was equally inert, because the
    // condition is a property of where the physical mouse is sitting, not of the drag.
    //
    // Asserting the enter is not a fiction: InputInject::ResolvePoint has already
    // refused any point outside this window's client area, so the synthetic cursor
    // genuinely is inside it. The callback also queues bd->LastValidMousePos, which the
    // position event we send immediately afterwards supersedes within the same frame.
    void EditorLayer::AssertSyntheticCursorOverWindow()
    {
        auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        if (!window)
            return;
        ::ImGui_ImplGlfw_CursorEnterCallback(window, GLFW_TRUE);
        m_McpSyntheticCursorEntered = true;
    }

    // ...and hand it back when the plan ends, so an injected plan leaves the backend
    // exactly as it found it. Left asserted, the editor would keep hovering the last
    // injected point long after the plan finished, which is the same genre of latch
    // this issue is about. GLFW_HOVERED is the ground truth we defer to; when it says
    // the mouse really is outside, the leave callback's sentinel position is picked up
    // and replaced by the fallback above on the very next frame, which IS the
    // pre-injection behaviour.
    void EditorLayer::RestoreCursorOverWindowState()
    {
        if (!m_McpSyntheticCursorEntered)
            return;
        m_McpSyntheticCursorEntered = false;

        auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        if (!window)
            return;
        if (::glfwGetWindowAttrib(window, GLFW_HOVERED) == 0)
            ::ImGui_ImplGlfw_CursorEnterCallback(window, GLFW_FALSE);
    }

    // Release every mouse button an in-flight plan pressed and did not release.
    // Mirrors the press into BOTH sinks the press went to, so neither the poll-based
    // Input:: overlay nor ImGui is left believing a button is down.
    void EditorLayer::ReleaseSyntheticMouseButtons()
    {
        // The two tables index the same GLFW button space; if the overlay ever grows a
        // button this one has not, a plan could press one we never track and therefore
        // never release.
        static_assert(std::tuple_size_v<decltype(m_McpSyntheticButtonsDown)> == SyntheticInput::s_MouseButtonCount,
                      "the tracked-button table must cover exactly the buttons SyntheticInput accepts");

        auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        for (sizet button = 0; button < m_McpSyntheticButtonsDown.size(); ++button)
        {
            if (!m_McpSyntheticButtonsDown[button])
                continue;
            m_McpSyntheticButtonsDown[button] = false;
            SyntheticInput::SetMouseButton(static_cast<MouseCode>(button), false);
            if (window)
            {
                ::ImGui_ImplGlfw_MouseButtonCallback(window, static_cast<i32>(button), GLFW_RELEASE, 0);
            }
            OLO_CORE_WARN("[MCP] Injected plan ended with mouse button {} still down; released it.", button);
        }
    }

    // Read back where the previous tick's injected position actually landed. ImGui's
    // NewFrame has run since, so io.MousePos now reflects it — or does not, which is
    // the whole point.
    void EditorLayer::SampleMcpCursorLanding()
    {
        if (!m_McpCursorProbeArmed)
            return;
        m_McpCursorProbeArmed = false;

        // Same ImGui-screen -> window-client correction GetMcpInputState applies, so
        // asked and landed are directly comparable.
        const MCP::McpInputViewportInfo info = GetMcpInputViewportInfo();
        const ImVec2 mouse = ImGui::GetMousePos();
        m_McpCursorLandedX = mouse.x - info.WindowX;
        m_McpCursorLandedY = mouse.y - info.WindowY;
        m_McpCursorLandingValid = true;
    }

    void EditorLayer::ApplyMcpInputEvent(const MCP::McpInputEvent& event)
    {
        auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        if (!window)
            return;

        // The GLFW modifier bitmask the callbacks would have carried. ImGui's backend
        // ignores it (it re-polls the hardware), but the engine's own callbacks are
        // handed it, and keeping it honest costs nothing.
        i32 mods = 0;
        if (m_McpSyntheticCtrl)
            mods |= GLFW_MOD_CONTROL;
        if (m_McpSyntheticShift)
            mods |= GLFW_MOD_SHIFT;
        if (m_McpSyntheticAlt)
            mods |= GLFW_MOD_ALT;

        switch (event.Type)
        {
            case MCP::McpInputEvent::Kind::MousePos:
            {
                // Window-client logical coordinates — exactly what GLFW hands the real
                // cursor-pos callback. The backend adds the window position back itself
                // when multi-viewport is enabled, so ImGui ends up with the right screen
                // coordinate without us duplicating that math.
                SyntheticInput::SetMousePosition({ event.X, event.Y });
                ::ImGui_ImplGlfw_CursorPosCallback(window, static_cast<f64>(event.X), static_cast<f64>(event.Y));
                // Arm the landing probe: next tick, once ImGui's NewFrame has applied
                // its queue, SampleMcpCursorLanding checks whether this position is
                // where the cursor actually ended up (issue #854).
                m_McpCursorAskedX = event.X;
                m_McpCursorAskedY = event.Y;
                m_McpCursorProbeArmed = true;
                m_McpCursorLandingValid = false;
                break;
            }
            case MCP::McpInputEvent::Kind::MouseDelta:
            {
                // Relative injection (issue #607) targets the POLL-based Input:: API
                // — the mouse-look rigs and camera controllers that integrate
                // `Input::GetMousePosition() - lastPos` across frames, and which an
                // absolute override provably cannot drive (see
                // SyntheticInput::AddMouseDelta).
                //
                // Deliberately NOT forwarded to ImGui. ImGui's cursor is absolute and
                // is fed by the real GLFW callbacks; synthesising a position for it
                // here would move the ImGui pointer as a side effect of a call that
                // asked for a displacement, and would make relative injection
                // non-additive with respect to widget/gizmo behaviour. `move` and
                // `drag` remain the actions for anything ImGui-facing.
                SyntheticInput::AddMouseDelta({ event.X, event.Y });
                break;
            }
            case MCP::McpInputEvent::Kind::MouseOffsetReset:
            {
                // The counterpart to the above: puts the virtual cursor back where the
                // hardware one actually is. A delta consumer registers this as one
                // jump of -(accumulated offset), which is honest — it IS a jump — and
                // is why it lives in its own plan frame rather than being folded into
                // the same frame as a following displacement.
                SyntheticInput::ClearMouseOffset();
                break;
            }
            case MCP::McpInputEvent::Kind::MouseButton:
            {
                SyntheticInput::SetMouseButton(static_cast<MouseCode>(event.Code), event.Down);
                ::ImGui_ImplGlfw_MouseButtonCallback(window, event.Code, event.Down ? GLFW_PRESS : GLFW_RELEASE, mods);
                // Remember what this plan is holding, so its teardown can put back
                // anything the plan itself failed to release.
                if (event.Code >= 0 && static_cast<sizet>(event.Code) < m_McpSyntheticButtonsDown.size())
                    m_McpSyntheticButtonsDown[static_cast<sizet>(event.Code)] = event.Down;
                // Re-arm the landing probe WITHOUT changing what was asked for. A press
                // trails its cursor move by several frames, so measuring only after the
                // move would miss a cursor stolen in between — and a press that lands
                // somewhere else is precisely the "reported ok, did nothing" this issue
                // is about. Re-arming here makes the last measurement the one taken
                // after the button actually acted.
                m_McpCursorProbeArmed = true;
                break;
            }
            case MCP::McpInputEvent::Kind::Key:
            {
                const auto key = static_cast<KeyCode>(event.Code);
                SyntheticInput::SetKey(key, event.Down);
                if (key == Key::LeftControl || key == Key::RightControl)
                    m_McpSyntheticCtrl = event.Down;
                else if (key == Key::LeftShift || key == Key::RightShift)
                    m_McpSyntheticShift = event.Down;
                else if (key == Key::LeftAlt || key == Key::RightAlt)
                    m_McpSyntheticAlt = event.Down;

                // Recompute the bitmask so the modifier key event itself carries its own
                // state (GLFW sets the bit on the press that establishes it).
                i32 keyMods = 0;
                if (m_McpSyntheticCtrl)
                    keyMods |= GLFW_MOD_CONTROL;
                if (m_McpSyntheticShift)
                    keyMods |= GLFW_MOD_SHIFT;
                if (m_McpSyntheticAlt)
                    keyMods |= GLFW_MOD_ALT;

                const i32 scancode = ::glfwGetKeyScancode(event.Code);
                ::ImGui_ImplGlfw_KeyCallback(window, event.Code, scancode, event.Down ? GLFW_PRESS : GLFW_RELEASE, keyMods);
                break;
            }
            case MCP::McpInputEvent::Kind::Char:
            {
                ::ImGui_ImplGlfw_CharCallback(window, static_cast<unsigned int>(event.Code));
                break;
            }
        }
    }

    MCP::McpInputViewportInfo EditorLayer::GetMcpInputViewportInfo() const
    {
        MCP::McpInputViewportInfo info;

        // The viewport panel bounds are only meaningful once UI_Viewport has run at
        // least once and the render target has a size.
        const glm::vec2 panelSize = m_ViewportBounds[1] - m_ViewportBounds[0];
        if (m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f || panelSize.x < 1.0f || panelSize.y < 1.0f)
            return info;

        info.Available = true;
        info.PanelX = m_ViewportBounds[0].x;
        info.PanelY = m_ViewportBounds[0].y;
        info.LogicalWidth = m_ViewportSize.x;
        info.LogicalHeight = m_ViewportSize.y;
        info.DpiScale = Window::s_HighDPIScaleFactor;

        // ImGui screen coordinates are DESKTOP coordinates while multi-viewport is on
        // (this editor enables it), so the window's own client-area origin must be
        // subtracted to get back to the window-client space GLFW's callbacks speak.
        // Without multi-viewport the two spaces coincide and the origin is (0, 0).
        const ImGuiIO& io = ImGui::GetIO();
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
        {
            if (auto* const window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow()); window)
            {
                int windowX = 0;
                int windowY = 0;
                ::glfwGetWindowPos(window, &windowX, &windowY);
                info.WindowX = static_cast<f32>(windowX);
                info.WindowY = static_cast<f32>(windowY);
            }
        }

        const Window& appWindow = Application::Get().GetWindow();
        info.WindowWidth = appWindow.GetWidth();
        info.WindowHeight = appWindow.GetHeight();
        return info;
    }

    MCP::McpInputStateSnapshot EditorLayer::GetMcpInputState() const
    {
        MCP::McpInputStateSnapshot state;
        state.Available = true;
        state.Pending = !m_McpInputQueue.empty();
        state.ViewportHovered = m_ViewportHovered;

        const ImVec2 mouse = ImGui::GetMousePos();
        // ImGui screen -> window client, the same correction GetMcpInputViewportInfo
        // applies, so the reported cursor is in the space the caller injected in.
        const MCP::McpInputViewportInfo info = GetMcpInputViewportInfo();
        state.MouseX = mouse.x - info.WindowX;
        state.MouseY = mouse.y - info.WindowY;

        // Entity::GetName() is non-const, so take copies (an Entity is a handle pair —
        // copying it is free) rather than widening this method's constness.
        if (Entity selected = m_SceneHierarchyPanel.GetSelectedEntity(); selected)
        {
            state.SelectedEntityId = static_cast<u64>(selected.GetUUID());
            state.SelectedEntityName = selected.GetName();
        }
        if (Entity hovered = m_HoveredEntity; hovered)
        {
            state.HoveredEntityId = static_cast<u64>(hovered.GetUUID());
            state.HoveredEntityName = hovered.GetName();
        }

        // The accumulated relative displacement of the "mouseDelta" action. Unlike the
        // absolute override this OUTLIVES the plan (that is the entire point — see
        // McpInputInject.h), so every reply reports it and a drift is visible.
        const glm::vec2 offset = SyntheticInput::GetMouseOffset();
        state.MouseOffsetX = offset.x;
        state.MouseOffsetY = offset.y;

        // Where the last injected position was aimed and where it actually landed
        // (issue #854). Reported separately from MouseX/MouseY above, which is a
        // live read and by this point may legitimately have moved back to the
        // hardware cursor as the plan's teardown handed hover back.
        state.CursorLandingValid = m_McpCursorLandingValid;
        state.CursorAskedX = m_McpCursorAskedX;
        state.CursorAskedY = m_McpCursorAskedY;
        state.CursorLandedX = m_McpCursorLandedX;
        state.CursorLandedY = m_McpCursorLandedY;

        // What ImGui's hit-test resolved on the last frame the plan actually ran
        // (issue #921) — latched by DrainMcpInputQueue every tick, not read live
        // here. A live read at THIS point (after the plan drained, and possibly
        // after the cursor has already been handed back to "nowhere") answers a
        // different question than the one this field exists to answer.
        state.HoveredWindowName = m_McpHoveredWindowName;
        state.HoveredId = m_McpHoveredId;
        state.ActiveId = m_McpActiveId;
        return state;
    }

    MCP::McpEditorLiveness EditorLayer::GetMcpEditorLiveness() const
    {
        MCP::McpEditorLiveness liveness;
        liveness.Available = true;
        liveness.FrameIndex = m_FrameIndex;

        // Zero until the first OnUpdate has run. Report 0 ms rather than the epoch
        // gap, so a tool started against a just-launched editor does not diagnose a
        // multi-decade stall (EditorLiveness::IsTicking would otherwise refuse every
        // call during startup).
        liveness.MsSinceLastFrame =
            m_LastFrameTick.time_since_epoch().count() == 0
                ? 0.0
                : std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - m_LastFrameTick).count();

        // Application::IsIconified is the AUTHORITATIVE flag here, not GLFW's window
        // attribute: it is the exact condition Application::Run guards the layer
        // update with, so it is what decides whether OnUpdate (and therefore the
        // injection drain and the frame counter) runs at all.
        //
        // The window is reached through the SAME guarded pointer: Application::Get()
        // dereferences its singleton unconditionally, so pairing it with a TryGet()
        // null check above would be a null dereference in exactly the case the check
        // exists to cover.
        // Not `const Application*`: GetWindow() is a non-const accessor, so a const
        // pointer cannot reach the native window at all.
        if (Application* const app = Application::TryGet())
        {
            liveness.Iconified = app->IsIconified();
            if (auto* const window = static_cast<GLFWwindow*>(app->GetWindow().GetNativeWindow()))
            {
                liveness.Focused = ::glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
                liveness.Visible = ::glfwGetWindowAttrib(window, GLFW_VISIBLE) != 0;
                // Belt and braces: a window can be iconified without Application having
                // seen the 0x0 resize event yet (the event arrives on the next poll).
                liveness.Iconified = liveness.Iconified || (::glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0);
            }
        }

        liveness.CaptureUnready = IsViewportCaptureUnready();
        return liveness;
    }

    bool EditorLayer::IsViewportCaptureUnready() const
    {
        // Unready while throttled, or within a few frames of a viewport resize:
        // freshly resized render-graph framebuffers render black for the first
        // couple of frames (verified against the live editor — 2 frames after a
        // resize still captured black, 6 were clean), so wait out a conservative
        // window.
        //
        // ONE definition, two consumers (the IsCaptureUnready context hook and the
        // liveness snapshot). They must agree by construction: a capture tool that
        // waits on one rule while the liveness block reports the other would send a
        // reader hunting a contradiction that is purely an artifact of the split.
        constexpr u64 kResizeSettleFrames = 6;
        return m_ViewportRenderSkipped || (m_FrameIndex < m_LastViewportResizeFrame + kResizeSettleFrames);
    }

    MCP::McpSelectEntityResult EditorLayer::SelectEntityInEditor(u64 entityUuid, bool clear)
    {
        MCP::McpSelectEntityResult result;
        result.Available = true;

        // Snapshot the selection BEFORE mutating, so Changed can be derived by
        // comparison below instead of tracked ad hoc per branch.
        const Entity previouslySelected = m_SceneHierarchyPanel.GetSelectedEntity();
        const u64 previousEntityId = previouslySelected ? static_cast<u64>(previouslySelected.GetUUID()) : 0;

        if (clear)
        {
            m_SceneHierarchyPanel.ClearSelection();
            result.Ok = true;
            result.Message = "Cleared the Scene Hierarchy selection.";
        }
        else if (!m_ActiveScene)
        {
            result.Ok = false;
            result.Message = "No active scene.";
        }
        else if (auto entityOpt = m_ActiveScene->TryGetEntityWithUUID(UUID(entityUuid)); entityOpt)
        {
            m_SceneHierarchyPanel.SetSelectedEntity(*entityOpt);
            result.Ok = true;
            result.Message = "Selected '" + entityOpt->GetName() + "'.";
        }
        else
        {
            // Bad uuid: leave the current selection untouched — a typo'd id
            // should never look like a clear.
            result.Ok = false;
            result.Message = "No entity with UUID " + std::to_string(entityUuid) + " in the active scene.";
        }

        // Single source of truth for the resulting selection state: read it
        // back from the panel rather than tracking it through locals above, so
        // the success and failure paths above can't drift out of sync with what
        // the panel actually holds.
        if (Entity current = m_SceneHierarchyPanel.GetSelectedEntity(); current)
        {
            result.Selected = true;
            result.EntityId = static_cast<u64>(current.GetUUID());
            result.EntityName = current.GetName();
        }
        result.Changed = result.Ok && (result.EntityId != previousEntityId);
        return result;
    }

    void EditorLayer::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE("EditorLayer::OnUpdate", Application::Get().GetPerformanceProfiler());

        ++m_FrameIndex; // MCP capture tools key off this to await a rendered frame
        // Stamped in the same breath as the counter so the two can never disagree:
        // together they let one MCP call answer "is the editor running frames?"
        // (issue #607). While the window is iconified Application::Run never reaches
        // this function at all, so BOTH freeze — which is exactly the signal.
        m_LastFrameTick = std::chrono::steady_clock::now();

        // Drain the lightmap-bake result mailbox (issue #439) — asset save and
        // scene-settings update must happen on the game thread.
        ProcessLightmapBakeCompletion();

        // Apply at most ONE frame of queued synthetic input (olo_input_inject, #607).
        // Deliberately before every early-return below (a scene-less editor must still
        // drain, or an injected plan would hang the caller waiting on frames that never
        // consume it) and before ImGuiLayer::Begin, so the events are picked up by this
        // frame's ImGui::NewFrame.
        DrainMcpInputQueue();

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

        // Resize. Also fires when a window-resize event was seen
        // (m_ViewportSizeReassertNeeded): Application::OnWindowResize resized the
        // render graph to the OS window size behind our back, and with an
        // unchanged viewport size (e.g. an active MCP viewport override) the
        // spec comparison alone would never correct it — the scene would keep
        // rendering at window resolution (#316: ~6x inflated frame times with a
        // maximized window on a 4K monitor).
        if (FramebufferSpecification const spec = m_Framebuffer->GetSpecification();
            (m_ViewportSize.x > 0.0f) && (m_ViewportSize.y > 0.0f) && // zero sized framebuffer is invalid
            ((std::abs(static_cast<f32>(spec.Width) - static_cast<f32>(fbWidth)) > epsilon) || (std::abs(static_cast<f32>(spec.Height) - static_cast<f32>(fbHeight)) > epsilon) || m_ViewportSizeReassertNeeded))
        {
            m_ViewportSizeReassertNeeded = false;
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

                // Drive networking after the simulation step, so an editor Play
                // session is a first-class client or listen server — the same loop
                // the dedicated server and the shipped runtime run. Game thread
                // only; see NetworkManager's threading contract.
                NetworkManager::Tick(ts);

                // Handle script-triggered scene transitions (issue #642).
                // A load and a reload are mutually exclusive by construction
                // (Scene's setters clear each other), so this is a check order,
                // not a precedence rule.
                if (m_ActiveScene->HasPendingSceneLoad())
                {
                    const std::string request = m_ActiveScene->GetPendingSceneLoad();
                    m_ActiveScene->ClearPendingSceneLoad();
                    // A failed switch is deliberately not fatal — the error is
                    // logged and Play carries on with the current scene, which
                    // is what an author needs to see to fix the scene name.
                    (void)SwitchPlayScene(request);
                }
                else if (m_ActiveScene->GetPendingReload())
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
            // Mouse y is top-down; the EntityID attachment is bottom-up on GL
            // only (ADR 0011 amendment (85)), so the origin conversion is a
            // GL-arm concern: glReadPixels / OpenGLFramebuffer::ReadPixel
            // address bottom-up rows, while VulkanFramebuffer::ReadPixel reads
            // the top-down image verbatim and wants the top-down coordinate.
            if (ImGuiLayer::RenderTargetRowsAreBottomUp())
            {
                my = viewportSize.y - my;
            }

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
                    // 3D without the GL PBO ring (any non-GL backend — the
                    // ring's init is GL-guarded): synchronous 1x1 read of the
                    // EntityID attachment through the framebuffer virtual,
                    // which lowers onto ReadTextureSubImage (#691b).
                    // One texel through a blocking one-shot per hovered frame
                    // is measurable but small; promote to an async ring if it
                    // ever shows up in a profile.
                    if (auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                        framebuffer != nullptr)
                    {
                        pixelData = framebuffer->ReadPixel(1, mouseX, mouseY);
                    }
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

            // Export the selected entity's mesh to glTF/glb (issue #655). Enabled only
            // when the selection carries a MeshComponent with a built MeshSource.
            {
                Entity meshSel = m_SceneHierarchyPanel.GetSelectedEntity();
                const bool canExportMesh = meshSel && meshSel.HasComponent<MeshComponent>() &&
                                           meshSel.GetComponent<MeshComponent>().m_MeshSource != nullptr;
                if (ImGui::MenuItem("Export Mesh to glTF...", nullptr, false, canExportMesh))
                {
                    if (std::string chosen = FileDialogs::SaveFile("glTF (*.gltf)\0*.gltf\0glTF Binary (*.glb)\0*.glb\0");
                        !chosen.empty())
                    {
                        std::filesystem::path outPath(chosen);
                        if (!outPath.has_extension())
                            outPath.replace_extension(".gltf");
                        const Ref<MeshSource>& meshSource = meshSel.GetComponent<MeshComponent>().m_MeshSource;
                        if (MeshExportResult exportResult = MeshExporterRegistry::Get().Export(*meshSource, outPath);
                            exportResult.Success)
                            OLO_INFO("Exported mesh to {}", outPath.string());
                        else
                            OLO_ERROR("Mesh export failed: {}", exportResult.Error);
                    }
                }
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

            // Baked GI (issue #439): bakes indirect lighting for every
            // lightmap-static entity into the scene lightmap asset.
            if (m_LightmapBakeInProgress.load())
            {
                char label[64];
                snprintf(label, sizeof(label), "Baking Lightmaps... %.0f%%",
                         static_cast<f64>(m_LightmapBakeProgress.load()) * 100.0);
                ImGui::MenuItem(label, nullptr, false, false);
                if (ImGui::MenuItem("Cancel Lightmap Bake"))
                {
                    m_LightmapBakeCancel.store(true);
                }
            }
            else if (ImGui::MenuItem("Bake Lightmaps"))
            {
                BakeLightmaps();
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
            ImGui::MenuItem("Skill Tree Editor", nullptr, &m_ShowSkillTreeEditor);
            ImGui::MenuItem("Cinematic Timeline", nullptr, &m_ShowCinematicTimeline);
            ImGui::MenuItem("NavMesh Panel", nullptr, &m_ShowNavMeshPanel);
            ImGui::MenuItem("Behavior Tree Editor", nullptr, &m_ShowBehaviorTreeEditor);
            ImGui::MenuItem("State Machine Editor", nullptr, &m_ShowFSMEditor);
            ImGui::MenuItem("Shader Graph Editor", nullptr, &m_ShowShaderGraphEditor);
            ImGui::MenuItem("Visual Script Editor", nullptr, &m_ShowVisualScriptEditor);
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

        // Display appropriate framebuffer based on mode. The ImTextureID
        // comes from ImGuiLayer::GetFramebufferTextureID (#691):
        // on GL it is the raw GL texture name as before, on Vulkan an
        // imgui_impl_vulkan descriptor set for the attachment.
        u64 textureID = 0;
        if (m_Is3DMode)
        {
            // Colour-vision adaptation (issue #458) is the LAST stage before the
            // backbuffer, so it outranks UIComposite here. This viewport is an
            // ImGui image of a graph resource, not the presented backbuffer —
            // without this branch the accessibility remap would be correct on the
            // swapchain and invisible in the editor, which is where it gets looked at.
            if (auto colorBlindFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor); colorBlindFramebuffer)
            {
                // ImGuiLayer::GetFramebufferTextureID, not GetColorAttachmentRendererID
                // (#691): on GL it is still the raw texture name, but on
                // Vulkan it is an imgui_impl_vulkan descriptor set. Passing the raw
                // name would draw garbage there.
                textureID = ImGuiLayer::GetFramebufferTextureID(*colorBlindFramebuffer, 0);
            }
            // Otherwise the UICompositePass output (post-processed scene + 2D overlays + UI)
            if (textureID == 0)
            {
                if (auto uiFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); uiFramebuffer)
                {
                    textureID = ImGuiLayer::GetFramebufferTextureID(*uiFramebuffer, 0);
                }
            }
            // Fallback to scene pass if post-process pass is not available
            if (textureID == 0)
            {
                if (auto sceneFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); sceneFramebuffer)
                {
                    textureID = ImGuiLayer::GetFramebufferTextureID(*sceneFramebuffer, 0);
                }
            }
        }
        else
        {
            textureID = ImGuiLayer::GetFramebufferTextureID(*m_Framebuffer, 0);
        }
        if (textureID != 0)
        {
            // ONE row order per backend (ADR 0011 amendment (85), retiring
            // (79)'s per-target regime): GL render targets are bottom-up
            // (flip V), Vulkan's are top-down (identity). The shared predicate
            // keeps this widget, CaptureFramebufferPng, and every thumbnail
            // panel agreeing about which way up the frame is.
            const bool bottomUpRows = ImGuiLayer::RenderTargetRowsAreBottomUp();
            const ImVec2 uv0 = bottomUpRows ? ImVec2{ 0, 1 } : ImVec2{ 0, 0 };
            const ImVec2 uv1 = bottomUpRows ? ImVec2{ 1, 0 } : ImVec2{ 1, 1 };
            ImGui::Image(textureID, ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, uv0, uv1);
        }
        else
        {
            // No presentable attachment (e.g. the Vulkan backend before the
            // first rendered frame). A Dummy keeps the layout, the border /
            // badge draws below (GetItemRectMin) and the drag-drop target
            // anchored to a real item; passing 0 to ImGui::Image would bind a
            // null descriptor set on Vulkan.
            ImGui::Dummy(ImVec2{ m_ViewportSize.x, m_ViewportSize.y });
        }

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

    namespace
    {
        // #691: toolbar icon button that stays clickable when the
        // icon has no ImTextureID on this backend (ImGuiLayer::GetTextureID
        // returns 0 — e.g. an unresolvable texture under Vulkan). Falls back
        // to a plain same-size button; passing 0 to ImGui::ImageButton would
        // bind a null descriptor set in imgui_impl_vulkan.
        [[nodiscard]] bool ToolbarIconButton(const char* strId, const Texture2D& icon, const ImVec2& size,
                                             const ImVec4& tint)
        {
            if (const u64 texId = ImGuiLayer::GetTextureID(icon); texId != 0)
            {
                return ImGui::ImageButton(strId, static_cast<ImTextureID>(texId), size, ImVec2(0, 0), ImVec2(1, 1),
                                          ImVec4(0, 0, 0, 0), tint);
            }
            return ImGui::Button(strId, size);
        }
    } // namespace

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
            if (Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Simulate)) ? m_IconPlay : m_IconStop; ToolbarIconButton("##play_stop_icon", *icon, btnSize, tintColor) && toolbarEnabled)
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
            if (Ref<Texture2D> const icon = ((m_SceneState == Edit) || (m_SceneState == Play)) ? m_IconSimulate : m_IconStop; ToolbarIconButton("##simulate_stop_icon", *icon, btnSize, tintColor) && toolbarEnabled)
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
            if (Ref<Texture2D> const icon = m_IconPause; ToolbarIconButton("##pause_icon", *icon, btnSize, tintColor) && toolbarEnabled)
            {
                m_ActiveScene->SetPaused(!isPaused);
            }
            ImGui::SameLine();
        }

        // Step button (only when paused)
        if (isPaused)
        {
            Ref<Texture2D> const icon = m_IconStep;
            if (ToolbarIconButton("##step_icon", *icon, btnSize, tintColor) && toolbarEnabled)
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
            // Only follow the hierarchy selection when it can actually be animated.
            // A bone click drives hierarchy selection to the bone's own entity (via
            // SetSelectBoneEntityCallback), which normally has neither component - so
            // blindly re-syncing here would overwrite the panel's animated entity and
            // flip it to the empty state on the very next frame.
            if (Entity hierarchySelection = m_SceneHierarchyPanel.GetSelectedEntity();
                hierarchySelection && (hierarchySelection.HasComponent<AnimationStateComponent>() || hierarchySelection.HasComponent<SkeletonComponent>()))
            {
                m_AnimationPanel.SetSelectedEntity(hierarchySelection);
            }
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

        // MCP per-action write-consent modal (issue #306). Rendered every frame
        // regardless of the panel's visibility so an agent's write in Prompt mode is
        // never left blocked because the user closed the panel. A no-op when idle.
        if (m_McpServer)
        {
            MCP::RenderMcpConsentModal(*m_McpServer);
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

        // Skill Tree Editor Panel — SetOpen(true) before rendering (the
        // ShaderGraph/SoundGraph variant of the panel-internal open flag
        // pattern) so re-checking the Window menu item after the user closed
        // the window with its X actually reopens it.
        if (m_ShowSkillTreeEditor)
        {
            m_SkillTreeEditorPanel.SetOpen(true);
            m_SkillTreeEditorPanel.OnImGuiRender();
            m_ShowSkillTreeEditor = m_SkillTreeEditorPanel.IsOpen();
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

        // Visual Script Editor Panel (issue #634)
        if (m_ShowVisualScriptEditor)
        {
            m_VisualScriptEditorPanel.SetOpen(true);
            // The debugger reads the LIVE scene (m_ActiveScene, which is the play
            // copy while running) and follows the hierarchy selection, so
            // clicking an entity while playing shows that entity's graph state.
            m_VisualScriptEditorPanel.SetContext(m_ActiveScene);
            m_VisualScriptEditorPanel.SetSelectedEntity(m_SceneHierarchyPanel.GetSelectedEntity());
            m_VisualScriptEditorPanel.OnImGuiRender();
            m_ShowVisualScriptEditor = m_VisualScriptEditorPanel.IsOpen();
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

        // Latch what ImGui's hit-test resolved THIS frame (issue #921), while an
        // injected plan is in flight (or just drained — one extra frame past empty
        // so the plan's LAST tick, where a press/release actually lands, is never
        // missed). g.HoveredWindow/HoveredId/ActiveId are computed once per frame in
        // NewFrame and then USED by every widget's ButtonBehavior during THIS same
        // frame's Begin/End calls above — so reading them here, after every panel
        // has run, is what makes the value match what ImGui itself actually decided.
        // Reading them from DrainMcpInputQueue (tried first) reads the PREVIOUS
        // frame's resolution, since that runs before NewFrame; reading them from the
        // tool's post-hoc GetInputState() call reads whatever is hovered at report
        // time, which can be long after the plan (and its cursor) are gone. Neither
        // answers "what did ImGui do with the click" — this does.
        if (::GImGui != nullptr && (!m_McpInputQueue.empty() || m_McpCursorRestoreCountdown > 0))
        {
            m_McpHoveredWindowName = ::GImGui->HoveredWindow != nullptr ? ::GImGui->HoveredWindow->Name : std::string{};
            m_McpHoveredId = ::GImGui->HoveredId;
            m_McpActiveId = ::GImGui->ActiveId;
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
        if (!m_Is3DMode || Renderer3D::HasInitialized())
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

        // Render interpolation (#502): apply live so toggling it in Preferences
        // takes effect on the running Play session without a restart.
        if (m_ActiveScene)
        {
            m_ActiveScene->SetRenderInterpolationEnabled(m_Prefs.RenderInterpolation);
        }

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
            ApplyTieringToRendererSettings(cfg.QualityTiering, Renderer3D::GetRendererSettings());
        }

        if (m_Is3DMode && !Renderer3D::HasInitialized())
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
        dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnWindowResized));
        dispatcher.Dispatch<AssetLoadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetLoaded));
        dispatcher.Dispatch<AssetReloadedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetReloaded));
        dispatcher.Dispatch<AssetImportedEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnAssetImported));
        dispatcher.Dispatch<WindowCloseEvent>(OLO_BIND_EVENT_FN(EditorLayer::OnWindowClose));
    }

    bool EditorLayer::OnWindowResized(WindowResizeEvent const& e)
    {
        // Application::OnWindowResize already ran (it dispatches before the layer
        // stack) and resized the Renderer3D render graph to the OS window's
        // framebuffer size. In the editor the scene renders into the viewport
        // panel (or the MCP olo_viewport_set_size override), not the window, so
        // mark the viewport-derived size for one reassert on the next OnUpdate —
        // the resize guard there won't fire on its own when the viewport size is
        // unchanged (see m_ViewportSizeReassertNeeded). Ignore minimize-sized
        // events (width/height == 0) since there's no real render-graph work.
        if (e.GetWidth() > 0 && e.GetHeight() > 0)
        {
            m_ViewportSizeReassertNeeded = true;
        }
        return false; // other layers may care about the resize too
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
                    // Ctrl+Shift+Z is the other conventional Redo binding; without
                    // this it fell through to Undo, so the two shortcuts disagreed.
                    const bool redo = shift;
                    if (m_ShowVisualScriptEditor && m_VisualScriptEditorPanel.IsOpen() && m_VisualScriptEditorPanel.IsFocused())
                        redo ? m_VisualScriptEditorPanel.Redo() : m_VisualScriptEditorPanel.Undo();
                    else if (m_ShowShaderGraphEditor && m_ShaderGraphEditorPanel.IsOpen() && m_ShaderGraphEditorPanel.IsFocused())
                        redo ? m_ShaderGraphEditorPanel.Redo() : m_ShaderGraphEditorPanel.Undo();
                    else
                        redo ? m_CommandHistory.Redo() : m_CommandHistory.Undo();
                    SyncWindowTitle();
                }
                break;
            }
            case Key::Y:
            {
                if (control && m_SceneState == SceneState::Edit)
                {
                    if (m_ShowVisualScriptEditor && m_VisualScriptEditorPanel.IsOpen() && m_VisualScriptEditorPanel.IsFocused())
                        m_VisualScriptEditorPanel.Redo();
                    else if (m_ShowShaderGraphEditor && m_ShaderGraphEditorPanel.IsOpen() && m_ShaderGraphEditorPanel.IsFocused())
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
            else if (type == ContentFileType::VisualScript)
            {
                if (m_VisualScriptEditorPanel.HasUnsavedChanges())
                {
                    auto const result = MessagePrompt::YesNoCancel(
                        "Unsaved Visual Script",
                        "The current visual script has unsaved changes. Do you want to save before opening a new one?");

                    switch (result)
                    {
                        case MessagePromptResult::Yes:
                            if (!m_VisualScriptEditorPanel.SaveIfNeeded())
                                return;
                            break;
                        case MessagePromptResult::Cancel:
                            return;
                        case MessagePromptResult::No:
                        default:
                            break;
                    }
                }
                m_VisualScriptEditorPanel.OpenGraph(path);
                m_ShowVisualScriptEditor = true;
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
            else if (type == ContentFileType::SkillTree)
            {
                if (m_SkillTreeEditorPanel.HasUnsavedChanges())
                {
                    auto const result = MessagePrompt::YesNoCancel(
                        "Unsaved Skill Tree",
                        "The current skill tree has unsaved changes. Do you want to save before opening a new one?");

                    switch (result)
                    {
                        case MessagePromptResult::Yes:
                            if (!m_SkillTreeEditorPanel.SaveIfNeeded())
                                return;
                            break;
                        case MessagePromptResult::Cancel:
                            return;
                        case MessagePromptResult::No:
                        default:
                            break;
                    }
                }
                m_SkillTreeEditorPanel.OpenSkillTree(path);
                m_ShowSkillTreeEditor = true;
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

        // "Edit Skill Tree" on the ProgressionComponent inspector opens the
        // referenced tree in the skill tree editor panel.
        m_SceneHierarchyPanel.SetOpenSkillTreeEditorCallback([this](AssetHandle handle)
                                                             {
            m_SkillTreeEditorPanel.OpenSkillTree(handle);
            m_ShowSkillTreeEditor = true; });

        // Clicking a bone in the Animation panel's Bone Hierarchy selects its entity.
        m_AnimationPanel.SetSelectBoneEntityCallback([this](Entity boneEntity)
                                                     { m_SceneHierarchyPanel.SetSelectedEntity(boneEntity); });
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
        m_SkillTreeEditorPanel.NewTree();
        m_ShowSkillTreeEditor = false;
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
            m_SkillTreeEditorPanel.NewTree();
            m_ShowSkillTreeEditor = false;
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

            // MCP script tools are project-scoped (they live under
            // <project assets>/McpTools, issue #357 / ADR 0005) and are loaded once at
            // attach / server start. Switching projects must not leave the previous
            // project's tools serving. The tool vector is immutable while the server
            // runs (the ADR's load-at-start rule), so stop it first, then rescan the
            // NEW project's directory (LoadScriptTools unregisters the old set even
            // when the new project has no McpTools dir). The server is left stopped:
            // Start() rotates a fresh bearer token, so the user restarts it from the
            // panel — where the new token is shown — rather than silently breaking the
            // old agent's credentials. On the initial-attach OpenProject call
            // m_McpServer does not exist yet (created later in OnAttach), so this only
            // runs on a genuine project switch.
            if (m_McpServer)
            {
                if (m_McpServer->IsRunning())
                    m_McpServer->Stop();
                if (const auto scriptDir = MCP::DefaultScriptToolsDirectory(); !scriptDir.empty())
                    (void)MCP::LoadScriptTools(*m_McpServer, scriptDir);
                else
                    m_McpServer->UnregisterScriptTools();
            }

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

        // Re-apply renderer settings after the scene swap so the graph reflects the
        // current config (#534) — mirrors the scene-load finalizers below.
        ApplyRendererSettingsToGraph();
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
            // A headless / agent session can't click the recovery modal, so an
            // automated launch can pre-answer it via OLO_EDITOR_AUTOSAVE_RECOVERY
            // (issue #316). Unset -> show the modal as before.
            switch (ResolveAutoSaveRecoveryChoice())
            {
                case AutoSaveRecoveryChoice::Autosave:
                    OLO_CORE_INFO("Auto-save recovery pre-answered (autosave): loading '{}'", autoPath.string());
                    return LoadEditorSceneFile(autoPath, path);
                case AutoSaveRecoveryChoice::Original:
                    OLO_CORE_INFO("Auto-save recovery pre-answered (original): loading '{}'", path.string());
                    return LoadEditorSceneFile(path, path);
                case AutoSaveRecoveryChoice::Discard:
                {
                    OLO_CORE_INFO("Auto-save recovery pre-answered (discard): deleting '{}' and loading '{}'",
                                  autoPath.string(), path.string());
                    std::error_code ec;
                    std::filesystem::remove(autoPath, ec);
                    return LoadEditorSceneFile(path, path);
                }
                case AutoSaveRecoveryChoice::Prompt:
                default:
                    m_PendingRecoveryScenePath = path;
                    m_PendingRecoveryAutoPath = autoPath;
                    m_ShowAutoSaveRecovery = true;
                    return true; // The modal will handle loading
            }
        }

        return LoadEditorSceneFile(path, path);
    }

    // Deserialize `loadPath` and install it as the editor scene, recording `scenePath`
    // as the scene's on-disk identity (they differ only for auto-save recovery, where
    // we load the .auto but keep the real scene's path). The full "Open Scene"
    // finalization: SetEditorScene, camera framing, the unified-timeline SceneLoad
    // event, and syncing the scene's renderer settings + quality tiering. Shared by
    // OpenScene (the non-modal path) and the MCP olo_scene_open hook. Returns false
    // (leaving the current scene untouched) if the deserialize fails.
    bool EditorLayer::LoadEditorSceneFile(const std::filesystem::path& loadPath, const std::filesystem::path& scenePath)
    {
        Ref<Scene> const newScene = Ref<Scene>::Create();
        if (SceneSerializer serializer(newScene); !serializer.Deserialize(loadPath.string()))
        {
            return false;
        }
        SetEditorScene(newScene);
        m_EditorScenePath = scenePath;
        FrameEditorCameraOnTerrain(newScene);

        // One unified-timeline event for the whole load (#306). The per-entity
        // EntitySpawn flood is suppressed during deserialize (SceneSerializer), so this
        // SceneLoad line stands in for it with the resulting entity count.
        DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::SceneLoad,
            "Loaded scene '" + newScene->GetName() + "' (" +
                std::to_string(static_cast<u64>(newScene->GetAllEntitiesWith<IDComponent>().size())) + " entities)",
            0, scenePath.filename().string());

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
            ApplyTieringToRendererSettings(project->GetConfig().QualityTiering, Renderer3D::GetRendererSettings());
        }

        // Push the (now scene-loaded + quality-tiered) settings into the render graph
        // (#534). Runs AFTER the PostProcessSettings copy above because
        // ApplyRendererSettings rebuilds the graph on an AO-technique change, so the
        // scene's ActiveAOTechnique must already be live. Covers OpenScene, MCP
        // olo_scene_open, and the auto-save pre-answered paths that share this finalizer.
        ApplyRendererSettingsToGraph();

        // A direct scene load supersedes any ARMED auto-save recovery (issue
        // #607): an olo_scene_open (or File > Open) while the recovery modal
        // is up would otherwise leave m_PendingRecovery* pointing at the
        // previous scene, and a later modal button click would silently swap
        // the just-opened scene back out — the "scene randomly reverted" race.
        // Clearing m_ShowAutoSaveRecovery alone is not enough once the popup
        // has opened; the cancel flag makes UI_AutoSaveRecoveryModal close it
        // without loading.
        m_ShowAutoSaveRecovery = false;
        m_CancelAutoSaveRecovery = true;
        m_PendingRecoveryScenePath.clear();
        m_PendingRecoveryAutoPath.clear();

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
        // Shared with the headless MCP test host (McpHeadlessHost) — see
        // SceneCameraFraming.h. Keep the bounds-computation + fit logic there,
        // not here, so the editor and the headless host can't drift apart.
        return FrameCameraOnEntity(m_ActiveScene, entityUuid, m_EditorCamera);
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
            ApplyTieringToRendererSettings(project->GetConfig().QualityTiering, Renderer3D::GetRendererSettings());
        }

        // Same rationale as LoadEditorSceneFile (#534): apply after the scene's
        // settings are live so the graph reflects them. This finalizer backs the
        // auto-save recovery modal's load paths.
        ApplyRendererSettingsToGraph();

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
            // A fresh modal must not be closed by a cancel raised against an
            // EARLIER recovery (issue #607) — consume any stale flag now.
            m_CancelAutoSaveRecovery = false;
        }

        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Recover Auto-Save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            // A direct scene load (olo_scene_open / File > Open) superseded
            // this recovery while the modal was up: close WITHOUT loading —
            // the pending paths now reference a scene the user already
            // navigated away from (issue #607).
            if (m_CancelAutoSaveRecovery)
            {
                m_CancelAutoSaveRecovery = false;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
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

        StartActiveRuntimeScene();
    }

    // Bring m_ActiveScene up as the running Play-mode scene. Shared by the two
    // ways Play mode acquires one — pressing Play, and a script switching scenes
    // mid-session (issue #642) — so neither can drift from the other on what
    // "started" means. The order matters: preference, then start, then the
    // observers that a start resets.
    void EditorLayer::StartActiveRuntimeScene()
    {
        // Apply the render-interpolation preference to the fresh runtime scene
        // (#502): Play renders interpolated poses between fixed ticks unless the
        // user disabled it.
        m_ActiveScene->SetRenderInterpolationEnabled(m_Prefs.RenderInterpolation);

        m_ActiveScene->OnRuntimeStart();

        // Stream quest/inventory gameplay events to the Console panel for this
        // Play session (subscriptions are dropped on OnRuntimeStop).
        AttachGameplayEventLogger(*m_ActiveScene);

        BindPanelsToScene(m_ActiveScene, nullptr);
        m_SaveGamePanel.SetContext(m_ActiveScene, m_Framebuffer);

        // Register the Play scene with networking so an editor session participates
        // in the same server-authoritative loop as OloRuntime and OloServer. This
        // is the editor half of the acceptance criterion; the dedicated server's
        // half lives in OloServerApp.
        //
        // NOTE the asymmetry with m_EditorScene: only the runtime scene is ever
        // registered. Replicating the authored scene would let a live connection
        // write into the document the user is editing.
        NetworkManager::SetActiveScene(m_ActiveScene.get());
    }

    // Every scene-observing panel, in one place. Introduced when script-driven
    // scene switching (issue #642) added a fourth site that swaps the active
    // scene — a panel missing from any one of them keeps a raw pointer into a
    // destroyed registry.
    void EditorLayer::BindPanelsToScene(const Ref<Scene>& scene, CommandHistory* history)
    {
        m_SceneHierarchyPanel.SetContext(scene);
        m_SceneHierarchyPanel.SetCommandHistory(history);
        m_AnimationPanel.SetContext(scene);
        m_AnimationPanel.SetCommandHistory(history);
        m_AnimationGraphEditorPanel.SetContext(scene);
        m_AnimationGraphEditorPanel.SetCommandHistory(history);
        m_StreamingPanel.SetContext(scene);
        m_StreamingPanel.SetCommandHistory(history);
        m_StatisticsPanel.SetContext(scene);
        m_NavMeshPanel.SetContext(scene);
        m_BehaviorTreeEditorPanel.SetContext(scene);
        m_FSMEditorPanel.SetContext(scene);
        m_AudioEventsPanel.SetActiveScene(scene);
    }

    bool EditorLayer::SwitchPlayScene(const std::string& request)
    {
        // Resolve against the project's asset directory — that is where the
        // editor's Scenes/ live. In a shipped game the same request resolves
        // against the game directory instead (OloRuntimeApp).
        const auto searchRoot = Project::GetActive() ? Project::GetAssetDirectory() : std::filesystem::path{};
        const auto resolved = SceneTransition::ResolveScenePath(request, searchRoot);
        if (resolved.empty())
        {
            OLO_CORE_ERROR("[Editor] Scene switch to '{}' failed: no matching scene file under '{}'. "
                           "Staying on the current scene.",
                           request, searchRoot.string());
            return false;
        }

        // Load and validate BEFORE tearing the running scene down, so a bad
        // target leaves Play mode running rather than half-stopped.
        auto loaded = SceneTransition::LoadSceneFile(resolved, /*requirePrimaryCamera=*/true);
        if (!loaded)
        {
            OLO_CORE_ERROR("[Editor] Scene switch failed: {}", loaded.Error);
            return false;
        }

        OLO_CORE_INFO("[Editor] Switching play scene to '{}'", resolved.string());

        // Stale entity references into the outgoing scene.
        m_HoveredEntity = Entity();
        m_PickingReadPending = false;

        // Detach from replication before the outgoing scene dies — the drivers hold
        // a raw Scene*. StartActiveRuntimeScene re-registers the incoming one.
        NetworkManager::SetActiveScene(nullptr);
        m_ActiveScene->OnRuntimeStop();

        m_ActiveScene = loaded.LoadedScene;
        // Unlike OnScenePlay's Scene::Copy, this scene was just deserialized and
        // has never seen the viewport, so size it before the runtime starts.
        if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
        {
            m_ActiveScene->OnViewportResize(static_cast<u32>(m_ViewportSize.x), static_cast<u32>(m_ViewportSize.y));
        }
        StartActiveRuntimeScene();
        return true;
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

        BindPanelsToScene(m_ActiveScene, nullptr);
    }

    void EditorLayer::OnSceneStop()
    {
        using enum OloEngine::EditorLayer::SceneState;
        OLO_CORE_ASSERT(m_SceneState == Play || m_SceneState == Simulate,
                        "OnSceneStop called with unexpected SceneState: {}", static_cast<int>(m_SceneState));

        if (m_SceneState == Play)
        {
            // Leaving Play means leaving the loop: the runtime scene is about to be
            // replaced by the authored one, which must never be replicated.
            NetworkManager::SetActiveScene(nullptr);
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

        BindPanelsToScene(m_ActiveScene, &m_CommandHistory);
        m_SaveGamePanel.SetContext(nullptr, nullptr);
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
        m_SkillTreeEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_AnimationGraphEditorPanel.SetContext(m_EditorScene);
        m_AnimationGraphEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_SoundGraphEditorPanel.SetCommandHistory(&m_CommandHistory);
        m_InputSettingsPanel.SetCommandHistory(&m_CommandHistory);
        m_AudioEventsPanel.SetActiveScene(m_EditorScene);

        m_ActiveScene = m_EditorScene;

        // Clear undo history when switching scenes
        m_CommandHistory.Clear();

        // (The ephemeral MCP sun-direction override clear that used to live here
        // was retired by issue #633 — the MCP time-of-day tools now edit the
        // serialized TimeOfDayComponent, which scene swaps reload normally.)

        SyncWindowTitle();
    }

    void EditorLayer::ApplyRendererSettingsToGraph()
    {
        // No renderer to configure until 3D mode has brought Renderer3D up (the
        // render graph is built inside Renderer3D::Init). In 2D mode this is a
        // no-op; the call re-runs when 3D mode initializes.
        if (!Renderer3D::HasInitialized())
            return;

        // Mirror the settings-panel / MCP-write call sites: this pushes the config's
        // rendering path, culling toggles, and the Forward+/depth-prepass derivation
        // into the live graph. Safe to re-run (idempotent); must be on the main thread
        // after the graph exists — both guaranteed here (post-TryInitialize3DMode at
        // startup, and inside the main-thread scene-load finalizers).
        Renderer3D::ApplyRendererSettings();
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
        if (m_VisualScriptEditorPanel.HasUnsavedChanges())
        {
            auto const result = MessagePrompt::YesNoCancel(
                "Unsaved Visual Script",
                "The current visual script has unsaved changes. Do you want to save before closing?");

            switch (result)
            {
                case MessagePromptResult::Yes:
                    if (!m_VisualScriptEditorPanel.SaveIfNeeded())
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

        // Check skill tree editor unsaved changes — mirrors the graph editor flows above
        if (m_SkillTreeEditorPanel.HasUnsavedChanges())
        {
            auto const result = MessagePrompt::YesNoCancel(
                "Unsaved Skill Tree",
                "The current skill tree has unsaved changes. Do you want to save before closing?");

            switch (result)
            {
                case MessagePromptResult::Yes:
                    if (!m_SkillTreeEditorPanel.SaveIfNeeded())
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

    bool EditorLayer::IsGpuTerrainPickingEnabled()
    {
        // Through the lever registry, not a raw getenv. The engine has exactly
        // one getenv (Core/Environment.cpp) and everything on top of it is
        // enumerable — see Core/DebugLevers.inl. The sibling site
        // (TerrainChunkManager::IsGpuDrivenLODEnabled) records what happens
        // otherwise: SonarCloud failed the quality gate on a raw getenv there.
        //
        // Read once. A per-call read on a path the brush cursor hits every
        // frame is not free, and a lever that changed mid-session would make
        // "which path answered this" unanswerable after the fact — the exact
        // question the lever exists to settle.
        static const bool s_Enabled = !Levers::TerrainCpuPick();
        return s_Enabled;
    }

    EditorLayer::TerrainPickState EditorLayer::TerrainRaycastGPU(const glm::vec2& mousePos,
                                                                 const glm::vec2& viewportSize,
                                                                 glm::vec3& outHitPos, bool& outHit) const
    {
        OLO_PROFILE_FUNCTION();

        outHit = false;
        if (!IsGpuTerrainPickingEnabled() || !m_ActiveScene)
        {
            return TerrainPickState::Unavailable;
        }

        Entity terrainEntity;
        auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        if (auto it = view.begin(); it != view.end())
        {
            terrainEntity = Entity(*it, m_ActiveScene.get());
        }
        if (!terrainEntity)
        {
            return TerrainPickState::Unavailable;
        }

        auto& tc = terrainEntity.GetComponent<TerrainComponent>();
        const auto& transform = terrainEntity.GetComponent<TransformComponent>();
        if (!tc.m_ChunkManager || !tc.m_ChunkManager->IsBuilt())
        {
            return TerrainPickState::Unavailable;
        }
        const auto& quadtree = tc.m_ChunkManager->GetGPUQuadtree();
        if (!quadtree || !quadtree->IsBuilt())
        {
            return TerrainPickState::Unavailable;
        }

        Ray mouseRay;
        if (!BuildMouseRay(mousePos, viewportSize, mouseRay))
        {
            return TerrainPickState::Unavailable;
        }

        // Terrain-LOCAL, because that is the space the node pyramid and the
        // heightmap live in — the same reason the LOD descent evaluates there
        // (MakeTerrainLocalCullInputs, #429). The full inverse transform rather
        // than a translation subtraction: it reduces to the same thing for an
        // unrotated, unscaled terrain (which is every terrain the CPU path ever
        // handled correctly) and is right for the ones it did not.
        const glm::mat4 invModel = glm::inverse(transform.GetTransform());
        const glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(mouseRay.Origin, 1.0f));
        const glm::vec3 localDir = glm::vec3(invModel * glm::vec4(mouseRay.Direction, 0.0f));

        auto& picker = tc.m_ChunkManager->GetOrCreateGPUPicker();
        if (!picker)
        {
            return TerrainPickState::Unavailable;
        }

        TerrainGPUPicker::RayRequest request;
        request.OriginLocal = localOrigin;
        request.DirectionLocal = localDir;
        // The CPU march's 2000-unit reach, converted to LOCAL units. SubmitRay
        // normalizes the direction, so `t` — and therefore MaxDistance — is
        // measured in terrain-local units, and `localDir` is exactly the local
        // displacement one world unit along the ray produces. Passing 2000
        // straight through would silently give the two paths different
        // world-space reaches under any non-unit terrain scale, which is a
        // disagreement about what counts as OUT OF RANGE rather than about
        // where the ground is — the harder kind to notice.
        const f32 localUnitsPerWorldUnit = glm::length(localDir);
        request.MaxDistance = 2000.0f * (localUnitsPerWorldUnit > 0.0f ? localUnitsPerWorldUnit : 1.0f);
        request.RayId = ++m_TerrainPickRayId;
        if (!picker->SubmitRay(request))
        {
            return TerrainPickState::Unavailable;
        }

        const auto& result = picker->GetLatest();
        if (!result.Valid)
        {
            // Nothing has come back yet — the first frames of a hover, or the
            // frames right after the ring was drained. The caller marches on the
            // CPU for those, which is what keeps the cursor from popping into
            // existence two frames late.
            return TerrainPickState::Pending;
        }

        outHit = result.Hit;
        if (result.Hit)
        {
            outHitPos = glm::vec3(transform.GetTransform() * glm::vec4(result.PositionLocal, 1.0f));
        }
        return TerrainPickState::Answered;
    }

    bool EditorLayer::TerrainRaycast(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const
    {
        OLO_PROFILE_FUNCTION();

        bool gpuHit = false;
        if (TerrainRaycastGPU(mousePos, viewportSize, outHitPos, gpuHit) == TerrainPickState::Answered)
        {
            return gpuHit;
        }
        return TerrainRaycastCPU(mousePos, viewportSize, outHitPos);
    }

    bool EditorLayer::TerrainRaycastCPU(const glm::vec2& mousePos, const glm::vec2& viewportSize, glm::vec3& outHitPos) const
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
        // Unified diagnostics timeline (#306): a hot-reload is exactly the kind of
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
            case AssetType::Lightmap:
            {
                OLO_TRACE("   → Lightmap asset reloaded - invalidating scene lightmap runtimes");
                // The lightmap runtime caches a GPU atlas built from the OLD
                // asset data, and its cheap resolve path (same handle, same
                // bake key) never re-reads the asset — an externally replaced
                // .olmap would keep rendering from the stale texture forever
                // without this.
                auto invalidateIfUsing = [&e](Ref<Scene> scene)
                {
                    if (scene && scene->GetLightmapSettings().LightmapAsset == e.GetHandle())
                    {
                        scene->GetLightmapRuntime()->Invalidate();
                    }
                };
                invalidateIfUsing(m_EditorScene);
                if (m_ActiveScene != m_EditorScene)
                {
                    invalidateIfUsing(m_ActiveScene);
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
            case AssetType::VisualScript:
            {
                // AC#1's hot-reload: the panel reloads unless the author has
                // unsaved edits to the same graph, and the running scene's
                // VisualScriptSystem rebuilds every live instance from it.
                m_VisualScriptEditorPanel.NotifyAssetReloaded(e.GetHandle(), e.GetPath());
                if (m_ActiveScene)
                {
                    if (auto* system = m_ActiveScene->GetVisualScripts(); system != nullptr)
                        system->NotifyGraphReloaded(e.GetHandle());
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

    void EditorLayer::BakeLightmaps()
    {
        if (m_LightmapBakeInProgress.load())
        {
            OLO_CORE_WARN("Lightmap bake already in progress, ignoring request");
            return;
        }
        if (m_SceneState != SceneState::Edit)
        {
            OLO_CORE_WARN("Lightmap bake requires Edit mode — stop Play/Simulate first");
            return;
        }
        Ref<Scene> scene = m_ActiveScene;
        if (!scene)
        {
            return;
        }

        // ── Gather every lightmap-static entity (game thread — reads the ECS) ──
        std::vector<LightmapBakeInput> inputs;
        {
            auto view = scene->GetAllEntitiesWith<IDComponent, MeshComponent>();
            for (auto entity : view)
            {
                const auto& mesh = view.get<MeshComponent>(entity);
                if (!mesh.m_LightmapStatic || !mesh.m_MeshSource || mesh.m_MeshSource->GetVertices().IsEmpty())
                {
                    continue;
                }
                LightmapBakeInput input;
                input.EntityUUID = static_cast<u64>(view.get<IDComponent>(entity).ID);
                input.Mesh = mesh.m_MeshSource;
                input.WorldTransform = scene->GetWorldTransform(entity);
                inputs.push_back(std::move(input));
            }
        }
        if (inputs.empty())
        {
            OLO_CORE_WARN("Lightmap bake: no entities are marked Lightmap Static — nothing to bake");
            return;
        }

        auto& lmSettings = scene->GetLightmapSettings();
        LightmapBakeSettings bakeSettings;
        bakeSettings.AtlasSize = lmSettings.AtlasSize;
        bakeSettings.SamplesPerTexel = lmSettings.SamplesPerTexel;
        bakeSettings.MaxBounces = lmSettings.MaxBounces;
        bakeSettings.TexelsPerMeter = lmSettings.TexelsPerMeter;

        // ── Stage 1 on the game thread: unwrap (mutates meshes the editor is
        // rendering), atlas layout, texel rasterization ──
        auto prepared = std::make_shared<LightmapBakePrepared>();
        if (std::string error; !LightmapBaker::Prepare(inputs, bakeSettings, *prepared, error))
        {
            OLO_CORE_ERROR("Lightmap bake failed to prepare: {}", error);
            return;
        }

        // Re-Build the unwrapped meshes right away (GL thread) so rendering
        // picks up the seam-split vertex data while the bake runs. Non-const
        // iteration on purpose: Ref<T> propagates const through operator->,
        // and Build() mutates the mesh.
        for (auto& input : inputs)
        {
            input.Mesh->Build();
        }

        // The bake key is computed AFTER the unwrap on purpose: the unwrap
        // changes vertex counts, and the key must describe the state the
        // runtime will re-derive at load (the resolve re-unwraps missing UV2
        // streams deterministically before ITS key check).
        bakeSettings.BakeKey = SceneLightmapRuntime::ComputeBakeKey(*scene, lmSettings);

        // ── Capture the reference world (game thread — reads ECS + mesh data) ──
        PathTracing::ReferenceSceneBuilder builder;
        builder.AddScene(*scene, [](Entity entity)
                         { return entity.HasComponent<MeshComponent>() &&
                                  entity.GetComponent<MeshComponent>().m_LightmapStatic; });
        auto world = std::make_shared<PathTracing::ReferenceScene>(builder.Build(PathTracing::ReferenceSceneBuildOptions{}));

        // ── Stage 2 in the background: the texel bake reads only `prepared`
        // and `world`, both frozen from here on ──
        m_LightmapBakeProgress.store(0.0f);
        m_LightmapBakeCancel.store(false);
        m_LightmapBakeInProgress.store(true);

        auto bakeDone = std::make_shared<std::promise<void>>();
        m_LightmapBakeFuture = bakeDone->get_future();

        // Pin the bake to THIS scene (we are in Edit mode, so scene ==
        // m_EditorScene). Completion attaches the result to the pinned scene.
        m_LightmapBakeScene = scene;
        m_LightmapBakeScenePath = m_EditorScenePath;

        OLO_CORE_INFO("Lightmap bake started: {} entities, {} texels, {} spp",
                      inputs.size(), prepared->Jobs.size(), bakeSettings.SamplesPerTexel);

        Tasks::Launch("BakeLightmaps", [this, prepared, world, bakeSettings, bakeDone]()
                      {
            LightmapBakeResult result = LightmapBaker::BakeTexels(*prepared, *world, bakeSettings,
                                                                  &m_LightmapBakeProgress, &m_LightmapBakeCancel);
            {
                std::scoped_lock lock(m_LightmapBakeResultMutex);
                m_LightmapBakeResult = std::move(result);
                m_LightmapBakeResultReady = true;
            }
            m_LightmapBakeInProgress.store(false);
            bakeDone->set_value(); }, Tasks::ETaskPriority::BackgroundNormal);
    }

    void EditorLayer::ProcessLightmapBakeCompletion()
    {
        LightmapBakeResult result;
        {
            std::scoped_lock lock(m_LightmapBakeResultMutex);
            if (!m_LightmapBakeResultReady)
            {
                return;
            }
            result = std::move(m_LightmapBakeResult);
            m_LightmapBakeResult = {};
            m_LightmapBakeResultReady = false;
        }

        // The bake belongs to the scene pinned at start — NOT whatever is
        // active now (the user may have opened another scene, or entered Play,
        // mid-bake). Release the pin regardless of outcome. Non-const binding
        // on purpose: Ref<T> propagates const through operator->, and the
        // settings write + Invalidate() below mutate the scene.
        Ref<Scene> bakedScene = m_LightmapBakeScene;
        const std::filesystem::path bakedScenePath = m_LightmapBakeScenePath;
        m_LightmapBakeScene = nullptr;
        m_LightmapBakeScenePath.clear();

        if (!result.Success)
        {
            OLO_CORE_ERROR("Lightmap bake failed: {}", result.Error);
            return;
        }

        // Persist as a REAL registered asset (never AddMemoryOnlyAsset — a
        // memory-only asset has no file and no registry entry, so the asset
        // pack would silently omit it).
        Ref<EditorAssetManager> editorAssetManager;
        if (Project::GetActive() && Project::HasAssetManager())
        {
            editorAssetManager = Project::GetAssetManager().As<EditorAssetManager>();
        }
        if (!editorAssetManager)
        {
            OLO_CORE_ERROR("Lightmap bake finished but no editor asset manager is active — result discarded");
            return;
        }

        const std::string sceneName = bakedScene ? bakedScene->GetName() : "Scene";
        // Disambiguate scenes that share a display name: suffix the file with
        // a hash of the scene's asset path, so two "Level.olo" in different
        // folders don't overwrite each other's bake. Deterministic per path —
        // a re-bake of the same scene replaces its own file. An unsaved scene
        // has no path and falls back to the bare name.
        std::string fileStem = sceneName;
        if (!bakedScenePath.empty())
        {
            const std::string scenePathUtf8 = bakedScenePath.generic_string();
            const u64 pathHash = Hash::FNV1a64(scenePathUtf8.data(), scenePathUtf8.size());
            char pathTag[12];
            (void)snprintf(pathTag, sizeof(pathTag), "%08x", static_cast<unsigned>(pathHash ^ (pathHash >> 32)));
            fileStem += '-';
            fileStem += pathTag;
        }
        const std::filesystem::path assetPath = std::filesystem::path("assets") / "lightmaps" / (fileStem + ".olmap");
        std::filesystem::create_directories(Project::GetAssetDirectory() / "lightmaps");

        Ref<LightmapAsset> stored = editorAssetManager->CreateOrReplaceAsset<LightmapAsset>(
            Project::GetAssetDirectory() / "lightmaps" / (fileStem + ".olmap"),
            result.Asset->GetWidth(), result.Asset->GetHeight(), result.Asset->GetPageCount(),
            result.Asset->GetBakeKey(),
            std::move(result.Asset->GetTexelData()),
            std::move(result.Asset->GetEntries()));
        if (!stored)
        {
            OLO_CORE_ERROR("Lightmap bake finished but the asset could not be stored at {}", assetPath.string());
            return;
        }

        if (bakedScene)
        {
            auto& lmSettings = bakedScene->GetLightmapSettings();
            lmSettings.LightmapAsset = stored->GetHandle();
            bakedScene->GetLightmapRuntime()->Invalidate();
        }
        if (bakedScene != m_EditorScene)
        {
            OLO_CORE_WARN("Lightmap bake completed for scene '{}', which is no longer the open scene — the .olmap "
                          "was saved, but the open scene was not touched. Reopen '{}' and re-bake (or assign {} "
                          "manually) to use it.",
                          sceneName, sceneName, assetPath.string());
            return;
        }

        OLO_CORE_INFO("Lightmap bake complete: {} entities baked, {} skipped — saved to {} (save the scene to keep the reference)",
                      result.BakedEntityCount, result.SkippedEntityCount, assetPath.string());
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
