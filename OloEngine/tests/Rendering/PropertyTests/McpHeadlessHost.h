#pragma once

// =============================================================================
// McpHeadlessHost — stand up the read-only MCP diagnostics server inside the
// TEST binary, pointed at the test fixture's OFFSCREEN render-graph framebuffer
// (the RunEditorFrames / visual-evidence path) instead of a live editor
// viewport.
//
// This is the "headless attach" stretch of issue #316. The visual-verify loop
// CLAUDE.md mandates ("Rendering changes MUST be visually verified") lives in
// the editor today: the MCP server reads the editor's live viewport. Renderer
// worktrees, though, do their real verification in the HEADLESS test loop
// (WaterVisualEvidenceTest / SceneRenderEvidenceTest render into an offscreen
// FB). This host makes olo_screenshot / olo_shader_errors / olo_render_capture_target
// work against that offscreen FB, so a renderer agent can verify visually
// without a running editor.
//
// The seam
// --------
// McpServer is already decoupled from the editor: it only reads an
// EditorMcpContext (a struct of std::function hooks) and marshals main-thread
// work onto the engine GameThread queue via EnqueueGameThreadTask. In the
// editor, Application::Run drains that queue every frame; a test has no such
// loop, so this host pumps the GameThread queue ITSELF
// (FNamedThreadManager::ProcessTasks(GameThread)) from the GL-context thread
// while an MCP tools/call runs on a worker thread.
//
// The MarshalRead contract is preserved exactly: tool handlers run on a NON-game
// thread (the worker) and block on a job the pumping (GL-context) thread
// services — never a deadlock, because the pumping thread is not the one calling
// MarshalRead. Every context hook (CaptureViewportPng, camera get/set, frame
// index) therefore runs on the GL-context thread, where readback / EditorCamera
// access is valid.
//
// Usage
// -----
//   McpHeadlessHost host(McpHeadlessHost::Hooks{
//       .GetScene              = [&]{ return GetSceneRef(); },
//       .GetCompositeFramebuffer = [&]{ return Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); },
//       .Camera                = &camera,
//       .RenderWidth = w, .RenderHeight = h,
//       .RenderOneFrame        = [&]{ RunEditorFrames(camera, 1); },
//   });
//   const u16 port = host.Start(/*basePort=*/0);   // 0 => derive a free port
//   const Json result = host.CallTool("olo_screenshot", { {"maxWidth", 256} });
//   ...
//   host.Stop();
//
// All of Start/Stop/PumpOnce/CallTool/HostUntil MUST be called from the
// GL-context thread (the test's main thread). CallTool dispatches on a worker
// thread internally and pumps this thread until the call completes.
// =============================================================================

#include "OloEnginePCH.h"

#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Task/NamedThreads.h"

#include <glad/gl.h>
#include <nlohmann/json.hpp>
#include <stb_image/stb_image_write.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    class McpHeadlessHost
    {
      public:
        using Json = OloEngine::MCP::Json;

        // Hooks the host needs from the test. Every callback runs on the pumping
        // (GL-context) thread inside a MarshalRead job, so GL readback / EnTT /
        // EditorCamera access is valid there.
        struct Hooks
        {
            // The active scene the diagnostics tools (olo_scene_*) read.
            std::function<Ref<Scene>()> GetScene;
            // Resolve the offscreen composite framebuffer olo_screenshot reads
            // back — the same UIComposite RT0 the editor viewport samples and the
            // visual-evidence tests read via ReadbackComposite.
            std::function<Ref<Framebuffer>()> GetCompositeFramebuffer;
            // The EditorCamera the olo_camera_* / posed-olo_screenshot tools drive.
            // RenderOneFrame must render THIS camera so a pose change is visible in
            // the next captured frame.
            EditorCamera* Camera = nullptr;
            u32 RenderWidth = 0;
            u32 RenderHeight = 0;
            // Render one frame at the current camera pose into the offscreen FB.
            // Called by PumpOnce before draining the GameThread queue, so a capture
            // job always sees a fresh frame and AwaitRenderedFrames sees the frame
            // counter advance.
            std::function<void()> RenderOneFrame;
        };

        explicit McpHeadlessHost(Hooks hooks)
            : m_Hooks(std::move(hooks))
        {
            m_Server = CreateScope<MCP::McpServer>(BuildContext());
            MCP::RegisterBuiltinTools(*m_Server);
        }

        ~McpHeadlessHost()
        {
            Stop();
        }

        McpHeadlessHost(const McpHeadlessHost&) = delete;
        McpHeadlessHost& operator=(const McpHeadlessHost&) = delete;

        // Start the server, binding the first free port at or after `basePort`.
        // basePort == 0 derives a per-process base (from the PID) so parallel
        // ctest processes don't collide. Returns the bound port, or 0 on failure.
        [[nodiscard]] u16 Start(u16 basePort = 0, int attempts = 24)
        {
            u16 port = basePort != 0 ? basePort : DerivePidPort();
            for (int i = 0; i < attempts; ++i)
            {
                const u16 candidate = static_cast<u16>(port + static_cast<u16>(i));
                if (candidate < 1024)
                    continue;
                if (m_Server->Start(candidate))
                    return candidate;
            }
            return 0;
        }

        void Stop()
        {
            if (m_Server && m_Server->IsRunning())
                m_Server->Stop();
        }

        [[nodiscard]] MCP::McpServer& Server()
        {
            return *m_Server;
        }
        [[nodiscard]] u64 FrameIndex() const
        {
            return m_FrameIndex.load(std::memory_order_relaxed);
        }

        // Pump one iteration on the CALLING (GL-context) thread: render one frame
        // (advancing the frame counter the capture/await tools observe) then drain
        // the GameThread queue so any pending MarshalRead job runs to completion.
        void PumpOnce()
        {
            if (m_Hooks.RenderOneFrame)
                m_Hooks.RenderOneFrame();
            m_FrameIndex.fetch_add(1, std::memory_order_relaxed);
            // Explicit-thread overload: drains the GameThread queue regardless of
            // named-thread attachment (we don't AttachToThread — the pumping thread
            // already holds the GL context the jobs need).
            Tasks::FNamedThreadManager::Get().ProcessTasks(Tasks::ENamedThread::GameThread, /*bIncludeLocalQueue=*/true);
        }

        // Run a JSON-RPC tools/call and return the raw JSON-RPC response object.
        // The handler runs on a worker thread (so its MarshalRead is legal); this
        // thread pumps until it completes. MUST be called from the GL-context
        // thread. Returns the full { jsonrpc, id, result|error } envelope.
        [[nodiscard]] Json CallTool(const std::string& name, const Json& arguments = Json::object(),
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30))
        {
            const Json message = { { "jsonrpc", "2.0" },
                                   { "id", ++m_NextId },
                                   { "method", "tools/call" },
                                   { "params", { { "name", name }, { "arguments", arguments } } } };
            return CallRaw(message, timeout);
        }

        // Run any JSON-RPC message (tools/list, initialize, …) with the same
        // pump-while-dispatching machinery.
        [[nodiscard]] Json CallRaw(const Json& message, std::chrono::milliseconds timeout = std::chrono::seconds(30))
        {
            std::future<Json> future =
                std::async(std::launch::async, [this, message]() -> Json
                           { return m_Server->HandleMessage(message); });

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                PumpOnce();
                if (std::chrono::steady_clock::now() >= deadline)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            // Even on the deadline path the worker self-bounds: MarshalRead times
            // out (~5 s) and the handler returns a tool error, so get() completes.
            return future.get();
        }

        // Interactive headless-attach mode: render + pump continuously so an
        // EXTERNAL agent (attached over the bound socket via `claude mcp add`) can
        // screenshot the offscreen FB live. Returns when `shouldStop` reports true
        // or `maxDuration` elapses. The discovery file was already written by
        // Start() (honouring OLO_MCP_DISCOVERY_FILE / OLO_MCP_PORT).
        void HostUntil(const std::function<bool()>& shouldStop, std::chrono::seconds maxDuration)
        {
            const auto deadline = std::chrono::steady_clock::now() + maxDuration;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (shouldStop && shouldStop())
                    break;
                PumpOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }

        // ---- PNG capture (mirrors EditorLayer::CaptureFramebufferPng) -----------
        // Read back colour attachment 0 of `framebuffer` (RGBA8), flip to PNG
        // top-down orientation, optionally downscale so the width is <= maxWidth,
        // and encode a PNG in memory. MUST run on the GL thread. Empty on failure.
        [[nodiscard]] static std::vector<u8> CaptureFramebufferPng(const Ref<Framebuffer>& framebuffer, int maxWidth)
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
            const auto append = [](void* context, void* data, int size)
            {
                auto* out = static_cast<std::vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                out->insert(out->end(), bytes, bytes + size);
            };
            if (::stbi_write_png_to_func(append, &png, static_cast<int>(outW), static_cast<int>(outH),
                                         4, src->data(), static_cast<int>(outW * 4)) == 0)
                return {};
            return png;
        }

      private:
        // Build the EditorMcpContext that backs the server with the OFFSCREEN FB
        // and the test's EditorCamera. Mirrors EditorLayer's wiring (#316) but
        // sources its frame from the fixture's render-graph composite rather than
        // a live viewport. GetCommandHistory / FrameEntity / SetViewportSizeOverride
        // are intentionally null (no project writes in the headless host; the
        // remaining two degrade to a clean "not available" from their tools).
        MCP::EditorMcpContext BuildContext()
        {
            MCP::EditorMcpContext ctx;
            ctx.GetActiveScene = [this]() -> Ref<Scene>
            { return m_Hooks.GetScene ? m_Hooks.GetScene() : nullptr; };
            ctx.IsPlaying = []() -> bool
            { return false; };
            ctx.CaptureViewportPng = [this](int maxWidth) -> std::vector<u8>
            {
                const Ref<Framebuffer> fb = m_Hooks.GetCompositeFramebuffer ? m_Hooks.GetCompositeFramebuffer() : nullptr;
                return CaptureFramebufferPng(fb, maxWidth);
            };

            ctx.GetCameraPose = [this]() -> MCP::McpCameraPose
            {
                MCP::McpCameraPose pose;
                if (m_Hooks.Camera)
                {
                    EditorCamera& cam = *m_Hooks.Camera;
                    pose.Position = cam.GetPosition();
                    pose.FocalPoint = cam.GetFocalPoint();
                    pose.Forward = cam.GetForwardDirection();
                    pose.Distance = cam.GetDistance();
                    pose.YawRadians = cam.GetYaw();
                    pose.PitchRadians = cam.GetPitch();
                    pose.FovDegrees = cam.GetFOV();
                    pose.NearClip = cam.GetNearClip();
                    pose.FarClip = cam.GetFarClip();
                }
                pose.ViewportWidth = m_Hooks.RenderWidth;
                pose.ViewportHeight = m_Hooks.RenderHeight;
                return pose;
            };
            ctx.SetCameraPose = [this](const glm::vec3& eye, f32 yaw, f32 pitch, f32 fovDegrees)
            {
                if (!m_Hooks.Camera)
                    return;
                if (fovDegrees > 0.0f)
                    m_Hooks.Camera->SetFOV(fovDegrees);
                m_Hooks.Camera->SetPose(eye, yaw, pitch);
            };
            ctx.OrbitCamera = [this](const glm::vec3& target, f32 yaw, f32 pitch, f32 distance)
            {
                if (m_Hooks.Camera)
                    m_Hooks.Camera->Focus(target, distance, yaw, pitch);
            };
            ctx.RestoreCameraPose = [this](const MCP::McpCameraPose& pose)
            {
                if (!m_Hooks.Camera)
                    return;
                m_Hooks.Camera->SetFOV(pose.FovDegrees);
                m_Hooks.Camera->Focus(pose.FocalPoint, pose.Distance, pose.YawRadians, pose.PitchRadians);
            };

            ctx.GetFrameIndex = [this]() -> u64
            { return m_FrameIndex.load(std::memory_order_relaxed); };
            ctx.IsCaptureUnready = [this]() -> bool
            { return m_FrameIndex.load(std::memory_order_relaxed) == 0; };
            return ctx;
        }

        // A per-process base port in [20000, 60000) so two MCP-hosting test
        // processes under `ctest --parallel` start their bind sweep at different
        // ports. The address of a static varies across processes (ASLR) and is
        // header-free; even if two processes collide, Start()'s ascending bind
        // sweep moves the loser forward (bind is atomic), so this only shortens
        // the sweep, it isn't relied on for correctness.
        [[nodiscard]] static u16 DerivePidPort()
        {
            static int anchor = 0;
            const auto bits = reinterpret_cast<std::uintptr_t>(&anchor);
            return static_cast<u16>(20000 + static_cast<u32>(bits % 40000u));
        }

        Hooks m_Hooks;
        Scope<MCP::McpServer> m_Server;
        std::atomic<u64> m_FrameIndex{ 0 };
        std::atomic<int> m_NextId{ 0 };
    };
} // namespace OloEngine::Tests
