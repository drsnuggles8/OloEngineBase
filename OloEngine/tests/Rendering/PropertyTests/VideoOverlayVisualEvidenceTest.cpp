// =============================================================================
// VideoOverlayVisualEvidenceTest.cpp
//
// Visual-evidence test for the fullscreen video-overlay compositing path
// (issue #176). It drives the REAL path the runtime/editor use:
//
//   VideoSystem::ShowFullscreenImage(...)  // a static RGBA frame via the
//                                          // same overlay player as video
//     -> Scene::OnUpdateRuntime
//        -> VideoSystem::OnUpdate (uploads the frame to a streaming
//           VideoTexture via the PBO path on real GL)
//        -> render graph -> UICompositePass -> Scene::RenderUIOverlay
//           (draws an opaque black backdrop + the letterboxed video quad
//            on top of the scene/UI)
//   -> ReadbackComposite (the same UIComposite output the editor shows)
//
// A real .mpg fixture is not needed: a synthetic magenta frame exercises the
// upload + composite faithfully and gives deterministic, driver-independent
// pixel contracts.
//
// Frame: 64x32 (2:1) solid magenta. In the 256x256 viewport it fits to
// 256x128 centred vertically, so the magenta band sits at y in [64,192] with
// opaque black letterbox bars above and below. Contracts:
//   1. The composite reads back at the requested resolution.
//   2. The centre pixel is the magenta video frame.
//   3. Both letterbox bars are pure black (the overlay backdrop), which also
//      proves the overlay actually covered the scene.
//
// The readback PNG is written to
//   OloEditor/assets/tests/visual/VideoOverlay_VisualEvidence.png
//
// Classification: L7 (smoke / sanity readback through the real GL pipeline).
// SKIPs cleanly when no GL 4.6 context is available.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Video/VideoSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <cstddef>
#include <filesystem>
#include <tuple>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 256;

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "VideoOverlay_VisualEvidence.png";
        }
    } // namespace

    class VideoOverlayScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // A primary camera so the render graph executes and produces a UIComposite
            // output (the overlay is drawn into that pass, on top of whatever the scene
            // rendered — here just the clear).
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            EnableRendering(kSize, kSize);
        }
    };

    TEST_F(VideoOverlayScene, FullscreenImageCompositesLetterboxed)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // 64x32 solid-magenta synthetic frame.
        constexpr u32 frameW = 64;
        constexpr u32 frameH = 32;
        std::vector<u8> frame(static_cast<std::size_t>(frameW) * frameH * 4u);
        for (std::size_t i = 0; i < frame.size(); i += 4)
        {
            frame[i + 0] = 255; // R
            frame[i + 1] = 0;   // G
            frame[i + 2] = 255; // B
            frame[i + 3] = 255; // A
        }

        VideoSystem::ShowFullscreenImage(frame.data(), frameW, frameH);
        // Tear the global fullscreen player down on every exit path (incl. ASSERT_* early
        // returns) so it can't leak into later tests.
        struct AutoStopFullscreen
        {
            ~AutoStopFullscreen()
            {
                VideoSystem::StopFullscreen();
            }
        } autoStop;

        // Two frames: first seeds render-graph history, second produces the stable image.
        RunFrames(2);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        // Always write the artifact first so it's available even if an assert fails.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(width), static_cast<int>(height),
                                           4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        const auto pixel = [&](u32 x, u32 y) -> std::tuple<u8, u8, u8>
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4u;
            return { px[idx + 0], px[idx + 1], px[idx + 2] };
        };
        const auto isMagenta = [](std::tuple<u8, u8, u8> t)
        {
            const auto [r, g, b] = t;
            return r > 150 && g < 100 && b > 150;
        };
        const auto isBlack = [](std::tuple<u8, u8, u8> t)
        {
            const auto [r, g, b] = t;
            return r < 40 && g < 40 && b < 40;
        };

        // Centre shows the magenta video frame.
        EXPECT_TRUE(isMagenta(pixel(width / 2, height / 2)))
            << "centre pixel should show the magenta video frame";
        // Both letterbox bars are pure black (overlay backdrop) — flip-robust.
        EXPECT_TRUE(isBlack(pixel(width / 2, 8u)))
            << "top letterbox bar should be black";
        EXPECT_TRUE(isBlack(pixel(width / 2, height - 8u)))
            << "bottom letterbox bar should be black";
        // autoStop tears down the fullscreen player on scope exit.
    }
} // namespace OloEngine::Tests
