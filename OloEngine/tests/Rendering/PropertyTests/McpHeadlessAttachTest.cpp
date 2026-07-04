// =============================================================================
// McpHeadlessAttachTest.cpp
//
// Headless-attach stretch of issue #316: prove the read-only MCP diagnostics
// tools (olo_screenshot, olo_shader_errors, olo_render_capture_target) work
// against the TEST harness's OFFSCREEN render-graph framebuffer — the same
// RunEditorFrames / visual-evidence path renderer worktrees verify in — rather
// than only a live editor viewport.
//
// Why this matters
// ----------------
// CLAUDE.md mandates "Rendering changes MUST be visually verified". That loop
// lives in the editor today (the MCP server reads the live viewport). But the
// real renderer verification happens HEADLESS (WaterVisualEvidenceTest et al.
// render into an offscreen FB and read it back). McpHeadlessHost stands the
// read-only MCP server up inside this test binary, pointed at that offscreen FB,
// so an agent gets olo_screenshot / olo_shader_errors against exactly what the
// headless pipeline drew — closing the visual-verify loop where renderer work
// actually lives.
//
// The two tests
// -------------
//   * ScreenshotAndShaderErrorsOverMcp — the CI regression guard. Renders a lit
//     cube into the offscreen FB, hosts the MCP server in-process, issues real
//     JSON-RPC tools/call dispatches through the httplib-free seam (on a worker
//     thread while this thread pumps the GameThread queue, preserving the
//     MarshalRead contract), and asserts olo_screenshot returns a decodable PNG
//     of the offscreen frame + olo_shader_errors returns a well-formed report.
//     SKIPs cleanly (not fails) when no GL 4.6 context — same gate as the other
//     visual-evidence tests.
//   * HostUntilDetached — the INTERACTIVE headless-attach entry point. SKIPs
//     unless OLO_MCP_HEADLESS_ATTACH=1; when set, it renders + hosts the server
//     (honouring OLO_MCP_PORT / OLO_MCP_DISCOVERY_FILE) and pumps live until a
//     stop signal, so an external agent attached via `claude mcp add` can
//     screenshot the offscreen FB from a renderer worktree's headless loop.
//
// Classification: integration (full Renderer3D bring-up + RGBA8 readback +
// in-process MCP dispatch). Sibling of RendererAttachedSmokeTest.
// =============================================================================

// OLO_TEST_LAYER: integration

#include "OloEnginePCH.h"

#include "McpHeadlessHost.h"
#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "MCP/McpServer.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using Json = OloEngine::MCP::Json;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // Minimal base64 decoder (RFC 4648) so the test can hand the PNG bytes to
        // stb_image and confirm the screenshot is a real, decodable image of the
        // requested size — not just a non-empty string.
        [[nodiscard]] std::vector<u8> Base64Decode(const std::string& in)
        {
            auto value = [](char c) -> int
            {
                if (c >= 'A' && c <= 'Z')
                    return c - 'A';
                if (c >= 'a' && c <= 'z')
                    return c - 'a' + 26;
                if (c >= '0' && c <= '9')
                    return c - '0' + 52;
                if (c == '+')
                    return 62;
                if (c == '/')
                    return 63;
                return -1; // padding '=' or whitespace
            };
            std::vector<u8> out;
            // Unsigned accumulator: signed left-shift overflows after a few sextets
            // (>32 bits) and that is UB. After each emitted byte, mask off the
            // consumed high bits so `buffer` only ever retains the `bits` leftover
            // low bits of base64 state.
            u32 buffer = 0;
            int bits = 0;
            for (const char c : in)
            {
                const int v = value(c);
                if (v < 0)
                    continue;
                buffer = (buffer << 6) | static_cast<u32>(v);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<u8>((buffer >> bits) & 0xFFu));
                    buffer &= (1u << bits) - 1u;
                }
            }
            return out;
        }

        // Pull the first image content block (type:"image") out of a tools/call
        // JSON-RPC response, returning its base64 data, or "" if none.
        [[nodiscard]] std::string FindImageData(const Json& response)
        {
            if (!response.contains("result") || !response["result"].contains("content"))
                return {};
            for (const auto& block : response["result"]["content"])
            {
                if (block.value("type", std::string{}) == "image" && block.contains("data"))
                    return block["data"].get<std::string>();
            }
            return {};
        }

        // Find the first text content block of a tools/call response.
        [[nodiscard]] std::string FindTextData(const Json& response)
        {
            if (!response.contains("result") || !response["result"].contains("content"))
                return {};
            for (const auto& block : response["result"]["content"])
            {
                if (block.value("type", std::string{}) == "text" && block.contains("text"))
                    return block["text"].get<std::string>();
            }
            return {};
        }

        [[nodiscard]] bool EnvFlagSet(const char* name)
        {
            const char* v = std::getenv(name);
            return v != nullptr && v[0] != '\0' && v[0] != '0';
        }
    } // namespace

    // A minimal lit scene rendered through the full editor render path into the
    // fixture's offscreen render-graph FB — the surface the headless MCP host
    // screenshots.
    class McpHeadlessAttachTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // A directional light so the cube is actually shaded (a black frame
            // would make the screenshot evidence meaningless).
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 2.0f, 4.0f, 3.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.9f);
                dl.m_Intensity = 3.0f;
            }

            // A unit cube at the origin with a bright, unmistakably non-grey
            // material so the captured PNG is obviously a rendered object.
            {
                Entity cube = scene.CreateEntity("Cube");
                auto& tc = cube.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, 0.0f };
                tc.Scale = { 1.5f, 1.5f, 1.5f };
                auto& mc = cube.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.15f, 0.65f, 0.95f, 1.0f));
            }
        }

        // Pose an EditorCamera so the cube fills the frame.
        [[nodiscard]] EditorCamera MakeCamera() const
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 1.2f, 4.5f), /*yaw=*/0.0f, /*pitch=*/0.18f);
            return camera;
        }
    };

    // The CI regression guard: olo_screenshot + olo_shader_errors against the
    // offscreen FB, end to end through real JSON-RPC dispatch. SKIPs without GL.
    TEST_F(McpHeadlessAttachTest, ScreenshotAndShaderErrorsOverMcp)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera = MakeCamera();

        // Render a couple of frames so the offscreen composite FB has content
        // before the host comes up.
        RunEditorFrames(camera, 2);

        // Independent sanity check that the offscreen frame is NOT black — so a
        // passing screenshot can't be a captured black buffer.
        {
            std::vector<u8> pixels;
            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, w, h)) << "offscreen composite FB unavailable";
            ASSERT_EQ(w, kWidth);
            ASSERT_EQ(h, kHeight);
            u64 lumaSum = 0;
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            ASSERT_GT(meanChannel, 2.0) << "offscreen frame rendered (near-)black; screenshot evidence would be meaningless";
        }

        McpHeadlessHost host(McpHeadlessHost::Hooks{
            .GetScene = [this]() -> Ref<Scene>
            { return GetSceneRef(); },
            .GetCompositeFramebuffer = []() -> Ref<Framebuffer>
            { return Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); },
            .Camera = &camera,
            .RenderWidth = kWidth,
            .RenderHeight = kHeight,
            .RenderOneFrame = [this, &camera]()
            { RunEditorFrames(camera, 1); },
            .SetViewportSize = [this](u32 w, u32 h)
            { ResizeRenderTarget(w, h); },
        });

        const u16 port = host.Start();
        ASSERT_NE(port, 0) << "McpHeadlessHost failed to bind any port for the MCP server";

        // ---- olo_screenshot (no pose): capture the current offscreen frame -----
        {
            const Json resp = host.CallTool("olo_screenshot", Json{ { "maxWidth", 256 } });
            ASSERT_TRUE(resp.contains("result")) << "olo_screenshot returned an error: " << resp.dump(2);
            EXPECT_FALSE(resp["result"].value("isError", false)) << resp.dump(2);

            const std::string b64 = FindImageData(resp);
            ASSERT_FALSE(b64.empty()) << "olo_screenshot returned no image block: " << resp.dump(2);

            const std::vector<u8> png = Base64Decode(b64);
            ASSERT_GT(png.size(), 8u);
            // PNG signature.
            const std::array<u8, 8> sig = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
            for (std::size_t i = 0; i < sig.size(); ++i)
                ASSERT_EQ(png[i], sig[i]) << "screenshot is not a PNG (byte " << i << ")";

            int dw = 0, dh = 0, dch = 0;
            stbi_uc* decoded = ::stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &dw, &dh, &dch, 4);
            ASSERT_NE(decoded, nullptr) << "screenshot PNG did not decode";
            ::stbi_image_free(decoded);
            EXPECT_GT(dw, 0);
            EXPECT_GT(dh, 0);
            EXPECT_LE(dw, 256) << "screenshot was not downscaled to maxWidth";
            // Aspect ratio of the offscreen FB is preserved on downscale.
            EXPECT_EQ(dh, std::max(1, (256 * static_cast<int>(kHeight)) / static_cast<int>(kWidth)));
        }

        // ---- olo_screenshot (posed): exercises camera control + frame-await ----
        {
            const Json resp = host.CallTool(
                "olo_screenshot",
                Json{ { "maxWidth", 200 },
                      { "camera", { { "position", { 3.0, 2.0, 3.0 } }, { "target", { 0.0, 0.0, 0.0 } } } },
                      { "settleFrames", 2 } });
            ASSERT_TRUE(resp.contains("result")) << "posed olo_screenshot errored: " << resp.dump(2);
            EXPECT_FALSE(resp["result"].value("isError", false)) << resp.dump(2);
            const std::string b64 = FindImageData(resp);
            ASSERT_FALSE(b64.empty()) << "posed olo_screenshot returned no image: " << resp.dump(2);
            const std::vector<u8> png = Base64Decode(b64);
            int dw = 0, dh = 0, dch = 0;
            stbi_uc* decoded = ::stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &dw, &dh, &dch, 4);
            ASSERT_NE(decoded, nullptr) << "posed screenshot PNG did not decode";
            ::stbi_image_free(decoded);
            EXPECT_GT(dw, 0);
            EXPECT_LE(dw, 200);

            // The camera must be restored to the pre-call pose (the host wired the
            // save/restore contract). The MakeCamera pose sits on +Z; the posed
            // capture moved it to (3,2,3). After the call it should be back.
            EXPECT_NEAR(camera.GetPosition().z, 4.5f, 1.0f)
                << "posed screenshot did not restore the prior camera";
        }

        // ---- olo_shader_errors: queries the live ShaderDebugger ----------------
        {
            const Json resp = host.CallTool("olo_shader_errors");
            ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
            EXPECT_FALSE(resp["result"].value("isError", false)) << resp.dump(2);

            const std::string text = FindTextData(resp);
            ASSERT_FALSE(text.empty()) << "olo_shader_errors returned no text: " << resp.dump(2);
            const Json report = Json::parse(text, nullptr, /*allow_exceptions=*/false);
            ASSERT_TRUE(report.is_object()) << "olo_shader_errors text was not JSON: " << text;
            ASSERT_TRUE(report.contains("count") && report["count"].is_number_integer()) << text;
            ASSERT_TRUE(report.contains("errors") && report["errors"].is_array()) << text;
            EXPECT_EQ(static_cast<std::size_t>(report["count"].get<int>()), report["errors"].size());
            // The production shader set compiled during Renderer::Init; any error
            // here is a real shader regression, not an MCP-plumbing failure.
            EXPECT_EQ(report["count"].get<int>(), 0)
                << "olo_shader_errors reported broken shaders: " << text;
        }

        // ---- olo_render_capture_target: best-effort intermediate-buffer read ---
        // A nice-to-have for the slice (the handover says don't block on it). We
        // discover a live target via olo_render_list_targets, then capture it and
        // assert the dispatch produced a well-formed result (image or a clean tool
        // error) — never a crash. Skips quietly if the graph exposed no targets.
        {
            const Json listResp = host.CallTool("olo_render_list_targets");
            ASSERT_TRUE(listResp.contains("result")) << listResp.dump(2);
            const std::string listText = FindTextData(listResp);
            const Json targets = Json::parse(listText, nullptr, false);
            std::string targetName;
            if (targets.is_object() && targets.contains("targets") && targets["targets"].is_array())
            {
                for (const auto& t : targets["targets"])
                {
                    if (t.contains("name") && t["name"].is_string())
                    {
                        targetName = t["name"].get<std::string>();
                        break;
                    }
                }
            }
            if (!targetName.empty())
            {
                const Json capResp = host.CallTool("olo_render_capture_target",
                                                   Json{ { "name", targetName }, { "maxWidth", 128 } });
                ASSERT_TRUE(capResp.contains("result")) << capResp.dump(2);
                // Either a real image came back, or a clean tool-level error — both
                // prove the tool ran end-to-end against the headless graph.
                const bool isError = capResp["result"].value("isError", false);
                const std::string capImg = FindImageData(capResp);
                EXPECT_TRUE(isError || !capImg.empty())
                    << "olo_render_capture_target('" << targetName << "') produced neither image nor error: "
                    << capResp.dump(2);
            }
        }

        // ---- olo_camera_frame_entity: no longer "not available" (#316 follow-on) -
        // Proves the headless host actually moves the camera to frame the Cube
        // entity, sharing OloEngine::FrameCameraOnEntity with the editor's wiring.
        {
            Entity cube = GetScene().FindEntityByName("Cube");
            ASSERT_TRUE(static_cast<bool>(cube)) << "BuildScene's 'Cube' entity is missing";
            const u64 cubeUuid = static_cast<u64>(cube.GetUUID());

            // MakeCamera() posed the camera with SetPose, which collapses the
            // orbit to distance 0 — so ANY successful frame moves distance off
            // zero. That makes "did framing actually run" unambiguous without
            // parsing the response JSON.
            ASSERT_NEAR(camera.GetDistance(), 0.0f, 1e-4f) << "test precondition: MakeCamera should start at distance 0";

            const Json resp = host.CallTool("olo_camera_frame_entity", Json{ { "id", std::to_string(cubeUuid) } });
            ASSERT_TRUE(resp.contains("result")) << "olo_camera_frame_entity returned an error: " << resp.dump(2);
            EXPECT_FALSE(resp["result"].value("isError", false))
                << "olo_camera_frame_entity is still reporting unavailable: " << resp.dump(2);

            const std::string text = FindTextData(resp);
            ASSERT_FALSE(text.empty()) << "olo_camera_frame_entity returned no text: " << resp.dump(2);
            const Json pose = Json::parse(text, nullptr, /*allow_exceptions=*/false);
            ASSERT_TRUE(pose.is_object()) << "olo_camera_frame_entity text was not JSON: " << text;
            EXPECT_EQ(pose.value("framedEntity", std::string{}), std::to_string(cubeUuid)) << text;

            // The camera (shared with the host via the Hooks::Camera pointer) must
            // have actually moved: pivoted onto the cube (world-space origin) at a
            // non-zero fit distance, not left at the pre-call pose.
            EXPECT_GT(camera.GetDistance(), 0.5f) << "camera was not re-pivoted to a real orbit distance";
            EXPECT_NEAR(camera.GetFocalPoint().x, 0.0f, 0.5f) << "camera did not focus on the cube (at the origin)";
            EXPECT_NEAR(camera.GetFocalPoint().y, 0.0f, 0.5f) << "camera did not focus on the cube (at the origin)";
            EXPECT_NEAR(camera.GetFocalPoint().z, 0.0f, 0.5f) << "camera did not focus on the cube (at the origin)";
        }

        // ---- olo_viewport_set_size: no longer "not available" (#316 follow-on) -
        // Proves a resize actually changes the offscreen composite framebuffer's
        // dimensions (not just that the tool call succeeds), then proves `reset`
        // restores the fixture's original resolution.
        {
            constexpr int kOverrideWidth = 320;
            constexpr int kOverrideHeight = 200;
            const Json setResp = host.CallTool(
                "olo_viewport_set_size", Json{ { "width", kOverrideWidth }, { "height", kOverrideHeight } });
            ASSERT_TRUE(setResp.contains("result")) << "olo_viewport_set_size returned an error: " << setResp.dump(2);
            EXPECT_FALSE(setResp["result"].value("isError", false))
                << "olo_viewport_set_size is still reporting unavailable: " << setResp.dump(2);

            const Json shotResp = host.CallTool("olo_screenshot", Json{ { "maxWidth", 1000 } });
            ASSERT_TRUE(shotResp.contains("result")) << shotResp.dump(2);
            const std::string b64 = FindImageData(shotResp);
            ASSERT_FALSE(b64.empty()) << "olo_screenshot returned no image after resize: " << shotResp.dump(2);
            const std::vector<u8> png = Base64Decode(b64);
            int dw = 0, dh = 0, dch = 0;
            stbi_uc* decoded = ::stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &dw, &dh, &dch, 4);
            ASSERT_NE(decoded, nullptr) << "resized screenshot PNG did not decode";
            ::stbi_image_free(decoded);
            EXPECT_EQ(dw, kOverrideWidth) << "olo_viewport_set_size did not resize the composite framebuffer's width";
            EXPECT_EQ(dh, kOverrideHeight) << "olo_viewport_set_size did not resize the composite framebuffer's height";

            // reset: true restores the fixture's original kWidth x kHeight.
            const Json resetResp = host.CallTool("olo_viewport_set_size", Json{ { "reset", true } });
            ASSERT_TRUE(resetResp.contains("result")) << resetResp.dump(2);
            EXPECT_FALSE(resetResp["result"].value("isError", false)) << resetResp.dump(2);

            const Json shotResp2 = host.CallTool("olo_screenshot", Json{ { "maxWidth", 1000 } });
            ASSERT_TRUE(shotResp2.contains("result")) << shotResp2.dump(2);
            const std::string b64_2 = FindImageData(shotResp2);
            ASSERT_FALSE(b64_2.empty()) << "olo_screenshot returned no image after reset: " << shotResp2.dump(2);
            const std::vector<u8> png2 = Base64Decode(b64_2);
            int dw2 = 0, dh2 = 0, dch2 = 0;
            stbi_uc* decoded2 = ::stbi_load_from_memory(png2.data(), static_cast<int>(png2.size()), &dw2, &dh2, &dch2, 4);
            ASSERT_NE(decoded2, nullptr) << "post-reset screenshot PNG did not decode";
            ::stbi_image_free(decoded2);
            EXPECT_EQ(dw2, static_cast<int>(kWidth)) << "olo_viewport_set_size{reset:true} did not restore the original width";
            EXPECT_EQ(dh2, static_cast<int>(kHeight)) << "olo_viewport_set_size{reset:true} did not restore the original height";
        }

        host.Stop();
    }

    // Interactive headless-attach: render + host the MCP server live so an
    // external agent (attached over the bound socket) can screenshot the
    // offscreen FB. SKIPs in normal CI; opt in with OLO_MCP_HEADLESS_ATTACH=1.
    // The run-oloengine driver sets OLO_MCP_PORT / OLO_MCP_DISCOVERY_FILE so the
    // bound server + discovery file land where `claude mcp add` expects them.
    TEST_F(McpHeadlessAttachTest, HostUntilDetached)
    {
        if (!EnvFlagSet("OLO_MCP_HEADLESS_ATTACH"))
            GTEST_SKIP() << "Set OLO_MCP_HEADLESS_ATTACH=1 to host the offscreen FB over MCP for an external agent "
                            "(interactive headless-attach mode).";
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera = MakeCamera();
        RunEditorFrames(camera, 2);

        McpHeadlessHost host(McpHeadlessHost::Hooks{
            .GetScene = [this]() -> Ref<Scene>
            { return GetSceneRef(); },
            .GetCompositeFramebuffer = []() -> Ref<Framebuffer>
            { return Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); },
            .Camera = &camera,
            .RenderWidth = kWidth,
            .RenderHeight = kHeight,
            .RenderOneFrame = [this, &camera]()
            { RunEditorFrames(camera, 1); },
            .SetViewportSize = [this](u32 w, u32 h)
            { ResizeRenderTarget(w, h); },
        });

        // Honour the per-worktree port the driver picked (OLO_MCP_PORT); the
        // discovery file path comes from OLO_MCP_DISCOVERY_FILE inside Start().
        u16 basePort = 0;
        if (const char* portEnv = std::getenv("OLO_MCP_PORT"); portEnv != nullptr)
        {
            if (const unsigned long parsed = std::strtoul(portEnv, nullptr, 10); parsed >= 1024 && parsed <= 65535)
                basePort = static_cast<u16>(parsed);
        }
        const u16 port = host.Start(basePort);
        ASSERT_NE(port, 0) << "McpHeadlessHost failed to bind a port for the MCP server";

        // How long to stay up (seconds); generous default for an interactive
        // session, capped so a forgotten run can't wedge a CI machine forever.
        int seconds = 600;
        if (const char* secEnv = std::getenv("OLO_MCP_ATTACH_SECONDS"); secEnv != nullptr)
        {
            if (const long parsed = std::strtol(secEnv, nullptr, 10); parsed > 0 && parsed <= 7200)
                seconds = static_cast<int>(parsed);
        }

        // A stop sentinel: delete this file (or let the timeout fire) to detach.
        const std::string stopFile = MCP::McpServer::DiscoveryFilePath(port) + ".stop";
        OLO_CORE_INFO("[MCP headless attach] hosting offscreen FB on http://127.0.0.1:{}/mcp for up to {}s. "
                      "Touch '{}' then delete it to stop early.",
                      port, seconds, stopFile);

        bool sawSentinel = false;
        host.HostUntil(
            [&]() -> bool
            {
                // Stop once the sentinel has appeared and then been removed, so an
                // operator can deterministically end the session.
                const bool exists = std::filesystem::exists(stopFile);
                if (exists)
                    sawSentinel = true;
                return sawSentinel && !exists;
            },
            std::chrono::seconds(seconds));

        host.Stop();
        SUCCEED() << "headless MCP attach session ended (port " << port << ").";
    }
} // namespace OloEngine::Tests
