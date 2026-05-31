// =============================================================================
// AutoExposureEvidenceTest.cpp
//
// GPU evidence for the histogram automatic-exposure / eye-adaptation feature
// (Renderer/AutoExposure.h + the AutoExposure*.comp compute passes +
// ToneMapRenderPass metering). The math itself is pinned on the CPU by
// AutoExposureMathTest; this test proves the *GPU plumbing* actually runs and
// applies the metered exposure to the frame — exactly the "tests green, screen
// wrong" gap the CLAUDE.md rendering rules warn about.
//
// It drives the REAL Scene pipeline (Scene::OnUpdateRuntime -> Renderer3D ->
// post-process -> tone-map) through the RendererAttachedTest fixture, rendering
// a deliberately over-bright, frame-filling surface and reading the final
// composited frame back. Two renders of the same scene are compared:
//
//   * Manual exposure (auto OFF, Exposure = 1.0): the bright scene tone-maps
//     near white.
//   * Auto exposure (auto ON): the histogram pass meters the high scene
//     luminance and the average pass drives the exposure DOWN, so the same
//     scene tone-maps to a much darker, non-degenerate frame.
//
// Contracts (driver-independent):
//   1. Both renders read back at the requested resolution.
//   2. The manual render is genuinely bright (the premise of the test).
//   3. Auto-exposure pulls the bright frame measurably darker than manual.
//   4. The auto-exposed frame is still a real image (not crushed to black).
//
// If the compute shaders fail to compile/link, EnsureAutoExposureResources()
// bails and the exposure buffer keeps its manual sentinel, so the auto render
// equals the manual render and contract 3 fails — i.e. this test also guards
// the compute shaders compiling and running.
//
// Both frames are written to OloEditor/assets/tests/visual/ for review.
//
// Classification: L8 (full GL pipeline through the real Scene render path +
// the auto-exposure compute passes, RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 256;

        [[nodiscard("mean luminance is this pure helper's only result")]] f32 MeanLuminance(const std::vector<u8>& px)
        {
            if (px.empty())
                return 0.0f;
            f64 sum = 0.0;
            const std::size_t byteCount = px.size();
            const std::size_t pixels = byteCount / 4;
            for (std::size_t i = 0; i < byteCount; i += 4)
            {
                const f32 r = static_cast<f32>(px[i + 0]) / 255.0f;
                const f32 g = static_cast<f32>(px[i + 1]) / 255.0f;
                const f32 b = static_cast<f32>(px[i + 2]) / 255.0f;
                sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            }
            return static_cast<f32>(sum / static_cast<f64>(pixels));
        }

        void WritePng(const std::vector<u8>& px, u32 w, u32 h, std::string_view name)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path out = dir / name;
            ::stbi_write_png(out.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                             px.data(), static_cast<int>(w) * 4);
        }
    } // namespace

    // A deliberately over-bright, frame-filling scene: a large cube lit by a
    // very strong point light right at the camera, so most of the viewport is
    // high-radiance HDR that tone-maps near white at exposure 1.0.
    class AutoExposureBrightScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity light = GetScene().CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.0f };
            auto& pointLight = light.AddComponent<PointLightComponent>();
            pointLight.m_Color = { 1.0f, 1.0f, 1.0f };
            pointLight.m_Intensity = 4000.0f; // extreme: drives surface radiance well above 1.0
            pointLight.m_Range = 80.0f;

            // A cube whose front face (z = +2) sits just in front of the camera
            // (z = 3) and is large enough (scale 4) to completely fill the
            // viewport — so the metered average is the bright lit surface, with
            // no dark background dragging it down. Axis-aligned (no rotation) so
            // the fill is uniform.
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Entity cubeEntity = GetScene().CreateEntity("Cube");
            cubeEntity.AddComponent<MeshComponent>(cube->GetMeshSource());
            cubeEntity.GetComponent<TransformComponent>().Scale = { 4.0f, 4.0f, 4.0f };

            EnableRendering(kSize, kSize);
        }

        f32 RenderAndMeasure(std::string_view pngName)
        {
            // 3 frames: seed TAA/velocity history + let the auto-exposure
            // metering run (the first metered frame snaps adaptation to target).
            RunFrames(3);
            std::vector<u8> px;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(px, w, h))
                return -1.0f;
            EXPECT_EQ(w, kSize);
            EXPECT_EQ(h, kSize);
            WritePng(px, w, h, pngName);
            return MeanLuminance(px);
        }
    };

    TEST_F(AutoExposureBrightScene, AutoExposureDarkensAnOverBrightScene)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The renderer reads its own PostProcessSettings copy (s_Data.PostProcess),
        // not the Scene's serialized copy, so drive that one directly.
        auto& pp = Renderer3D::GetPostProcessSettings();

        // Snapshot + RAII-restore ALL post-process settings so a failing ASSERT
        // (which returns early) can't leak this test's mutations — auto-exposure
        // ON, widened clamps, etc. — into later GPU tests sharing this process's
        // static Renderer3D state.
        struct PostProcessRestore
        {
            PostProcessSettings& Target;
            PostProcessSettings Saved;
            explicit PostProcessRestore(PostProcessSettings& t) : Target(t), Saved(t) {}
            ~PostProcessRestore()
            {
                Target = Saved;
            }
        } ppRestore(pp);

        // --- Manual exposure baseline (auto OFF) ---
        pp.AutoExposureEnabled = false;
        pp.Exposure = 1.0f;
        const f32 meanManual = RenderAndMeasure("AutoExposure_ManualBright.png");
        ASSERT_GE(meanManual, 0.0f) << "ReadbackComposite failed for the manual render";

        // --- Auto exposure (ON) ---
        pp.AutoExposureEnabled = true;
        pp.AutoExposureCompensation = 0.0f;
        pp.AutoExposureSpeedUp = 20.0f; // converge fast for the test
        pp.AutoExposureSpeedDown = 20.0f;
        // This is a deliberately extreme scene (surface radiance ~2^12), so widen
        // the metering window + clamps to actually capture it; a real scene fits
        // the defaults. (Demonstrates the range params doing their job.)
        pp.AutoExposureMaxLogLuminance = 14.0f;
        pp.AutoExposureMinExposure = 1e-6f;
        pp.AutoExposureMaxExposure = 1000.0f;
        const f32 meanAuto = RenderAndMeasure("AutoExposure_AutoBright.png");
        ASSERT_GE(meanAuto, 0.0f) << "ReadbackComposite failed for the auto render";

        // (The measured means appear in the EXPECT messages below on failure;
        // the written PNGs are the success-case evidence artifact.)

        // (2) The scene is saturated-bright at exposure 1.0 (premise).
        EXPECT_GT(meanManual, 0.9f)
            << "manual-exposure frame is not saturated-bright (mean=" << meanManual << ")";
        // (3) Auto-exposure meters the high luminance and pulls the frame down
        //     strongly, into the mid-tones (not just a token reduction).
        EXPECT_LT(meanAuto, meanManual - 0.3f)
            << "auto-exposure did not darken the over-bright scene (manual=" << meanManual
            << ", auto=" << meanAuto << ") — the compute metering may not have run";
        EXPECT_GT(meanAuto, 0.05f)
            << "auto-exposed frame collapsed to black (mean=" << meanAuto << ")";
        EXPECT_LT(meanAuto, 0.8f)
            << "auto-exposed frame is still near-white (mean=" << meanAuto
            << ") — metering did not bring the exposure down";

        // (Renderer settings are restored by ppRestore on scope exit.)
    }
} // namespace OloEngine::Tests
