// OLO_TEST_LAYER: L8
// =============================================================================
// FSR2VisualEvidenceTest.cpp
//
// Visual evidence (PNG) + driver-independent contracts for FSR2 temporal
// upscaling (#684) — FSR2RenderPass + Platform/OpenGL/OpenGLTemporalUpscaler,
// driven through the FULL Renderer3D pipeline.
//
// WHY THIS TEST LOOKS DIFFERENT FROM EASUVisualEvidenceTest. A spatial upscaler
// is a function of one frame, so a still comparison is a complete test of it. A
// temporal upscaler is a function of the frames BEFORE this one, and every way it
// goes wrong — a motion-vector sign flip, a jitter scaled against the wrong
// extent, a history that never invalidates — produces a still frame that looks
// entirely reasonable. So the contracts here are about what happens ACROSS
// frames:
//
//   * ReconstructsDisplayResolution — settle, then compare against native. Proves
//     the reduced-res + FSR2 path ran and did not collapse into a blurry
//     bilinear upscale (the exact symptom of jitter divided by the display
//     extent instead of the render extent).
//   * ConvergesAfterCameraMotion — the ghosting test, and the reason this file
//     exists. Render a settled reference pose; move the camera away and back;
//     let it settle again. If reprojection is inverted or the history never
//     releases, the returned frame keeps a trail and stays measurably different
//     from the reference. A still screenshot cannot see this at all.
//   * MSAAFallsBackToSpatial — the acceptance criterion that MSAA is guarded
//     rather than silently broken: with MSAA on, the frame must still render.
//
// All contracts are GOLDEN-FREE and differential, so they survive a GPU change.
// PNGs land in OloEditor/assets/tests/visual/FSR2_*.png and are meant to be
// LOOKED AT — the numbers here catch collapse, not subtle quality.
//
// Runs in the normal suite and SKIPs (not fails) when there is no GL 4.6 context
// OR when FSR2 is unavailable on this build/backend (non-Windows, Vulkan,
// OLO_WITH_FSR2=0) — an unsupported configuration is not a failure, and the
// fallback itself is covered by FSR2PolicyTest without a GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 2.0f;

        // FSR2 needs several jittered frames before its history is meaningful. Its
        // jitter phase count at a 0.667 scale is ~16-20, so a handful of frames is
        // partial convergence and the numbers below are chosen for that, not for a
        // fully converged image. More frames would be a better image and a slower
        // test; this is the point where the contracts stop being noisy.
        constexpr u32 kSettleFrames = 12;

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
        }

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += Luma(px, x, y);
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }

        [[nodiscard]] f64 GradientEnergy(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y + 1u < kHeight; ++y)
                for (u32 x = 0; x + 1u < kWidth; ++x)
                {
                    const f64 c = Luma(px, x, y);
                    sum += std::abs(Luma(px, x + 1u, y) - c);
                    sum += std::abs(Luma(px, x, y + 1u) - c);
                }
            return sum;
        }

        [[nodiscard]] f64 MeanAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += std::abs(Luma(a, x, y) - Luma(b, x, y));
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }
    } // namespace

    class FSR2VisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // A ground plane plus two directional lights, because a sparse scene
            // renders the subject near-black and every luma threshold below would
            // then be measuring nothing — see
            // docs/agent-rules/single-mesh-visual-test-lighting.md.
            {
                Entity key = scene.CreateEntity("Key");
                key.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 2.0f;
                dl.m_CastShadows = false;
            }
            {
                Entity fill = scene.CreateEntity("Fill");
                fill.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.6f, -0.4f, 0.4f));
                dl.m_Color = glm::vec3(0.5f, 0.55f, 0.7f);
                dl.m_Intensity = 1.0f;
                dl.m_CastShadows = false;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale, const glm::vec4& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh;
                switch (prim)
                {
                    case MeshPrimitive::Plane:
                        mesh = MeshPrimitives::CreatePlane();
                        break;
                    case MeshPrimitive::Sphere:
                        mesh = MeshPrimitives::CreateSphere();
                        break;
                    default:
                        mesh = MeshPrimitives::CreateCube();
                        break;
                }
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(albedo);
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.85f);
                return e;
            };

            addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 80.0f, 1.0f, 80.0f },
                    glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));

            // A high-contrast checker of blocks: dense sub-pixel edges are what a
            // temporal upscaler reconstructs and what a broken one smears.
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 5; ++col)
                {
                    const bool bright = ((row + col) & 1) == 0;
                    const glm::vec4 albedo = bright ? glm::vec4(0.9f, 0.88f, 0.85f, 1.0f)
                                                    : glm::vec4(0.12f, 0.13f, 0.15f, 1.0f);
                    const glm::vec3 pos = { static_cast<f32>(col - 2) * 3.0f, 1.0f,
                                            static_cast<f32>(row - 2) * 3.0f };
                    addMesh("Cube", MeshPrimitive::Cube, pos, { 1.6f, 2.0f, 1.6f }, albedo);
                }

            addMesh("SphereL", MeshPrimitive::Sphere, { -4.5f, 2.0f, 5.0f }, { 2.0f, 2.0f, 2.0f },
                    glm::vec4(0.8f, 0.3f, 0.25f, 1.0f));
            addMesh("SphereR", MeshPrimitive::Sphere, { 4.5f, 2.0f, 5.0f }, { 2.0f, 2.0f, 2.0f },
                    glm::vec4(0.25f, 0.45f, 0.8f, 1.0f));
        }

        // Is FSR2 actually usable in this process? Asked through the same factory
        // the pass uses, so the answer cannot disagree with what the pipeline
        // decides. Deliberately NOT a compile-time check: a Windows GL build can
        // still fail at context creation, and that is a skip, not a failure.
        [[nodiscard]] static bool TemporalUpscalerUsable()
        {
            const Ref<TemporalUpscaler> upscaler = TemporalUpscaler::Create();
            return upscaler && upscaler->IsAvailable();
        }

        void RunFramesAndCapture(const EditorCamera& camera, u32 frames, const std::string& tag,
                                 std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, frames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for FSR2 capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL reads bottom-up; flip so the PNG is the right way round when a
            // human opens it. (The metrics are orientation-independent, but the
            // evidence is meant to be looked at.)
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            if (tag.empty())
                return;

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("FSR2_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        [[nodiscard]] static EditorCamera MakeCamera(const glm::vec3& pos, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pos, yaw, pitch);
            return camera;
        }

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        };
    };

    // Native vs FSR2 Quality (0.667x render scale). After settling, the temporal
    // upscale must render, differ from native, preserve brightness, and — the
    // load-bearing one — retain most of native's high-frequency energy. That last
    // assertion is what fails if the jitter is scaled against the display extent
    // instead of the render extent: FSR2 keeps running, keeps accumulating, and
    // quietly produces a bilinear-grade blur.
    TEST_F(FSR2VisualEvidenceTest, ReconstructsDisplayResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TemporalUpscalerUsable())
            GTEST_SKIP() << "FSR2 is not available on this build/backend — the spatial upscaler is used instead";

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = MakeCamera({ 0.0f, 7.0f, 16.0f }, 0.0f, 0.32f);
        auto& pp = Renderer3D::GetPostProcessSettings();

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        std::vector<u8> nativePixels;
        RunFramesAndCapture(camera, 3, "Native", nativePixels);
        if (::testing::Test::HasFatalFailure())
            return;

        pp.Upscale = UpscaleMode::Quality; // 0.667x — the acceptance criterion's scale
        pp.Technique = UpscalerTechnique::Temporal;
        pp.FSR2SharpeningEnabled = true;
        pp.FSR2Sharpness = 0.5f;
        std::vector<u8> fsr2Pixels;
        RunFramesAndCapture(camera, kSettleFrames, "Quality", fsr2Pixels);

        pp.Upscale = UpscaleMode::Off; // restore for later tests in the process
        pp.Technique = UpscalerTechnique::Spatial;
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 nativeMean = MeanLuma(nativePixels);
        const f64 fsr2Mean = MeanLuma(fsr2Pixels);
        EXPECT_GT(nativeMean, 20.0) << "native frame rendered (near-)black";
        EXPECT_GT(fsr2Mean, 20.0)
            << "FSR2 frame rendered (near-)black — the upscaler may have produced no output while the "
               "pipeline still routed the post chain through FSR2Color. See FSR2_Quality.png";

        EXPECT_GT(MeanAbsDiff(nativePixels, fsr2Pixels), 0.5)
            << "the FSR2 frame is essentially identical to native — the reduced-res temporal path may "
               "not be running at all. See FSR2_Native.png / FSR2_Quality.png";

        EXPECT_LT(std::abs(fsr2Mean - nativeMean), nativeMean * 0.15)
            << "FSR2 shifted overall brightness (native=" << nativeMean << " fsr2=" << fsr2Mean
            << "). Exposure is FSR2's own (FFX_FSR2_ENABLE_AUTO_EXPOSURE) — a large shift here means "
               "its metering disagrees with the un-exposed HDR input it is supposed to receive.";

        const f64 nativeEnergy = GradientEnergy(nativePixels);
        const f64 fsr2Energy = GradientEnergy(fsr2Pixels);
        EXPECT_GT(fsr2Energy, nativeEnergy * 0.60)
            << "FSR2 lost too much detail (native energy=" << nativeEnergy << " fsr2=" << fsr2Energy
            << ", ratio=" << (fsr2Energy / nativeEnergy)
            << "). A temporal upscaler that reconstructs nothing degrades to a bilinear blur while still "
               "running — the usual cause is the projection jitter being scaled against the DISPLAY "
               "extent instead of the RENDER extent. See FSR2_Quality.png";
    }

    // THE ghosting test. Settle at pose A, drive the camera away and back, settle
    // at pose A again. A correct reprojection reproduces the first settled frame;
    // an inverted motion-vector scale, or a history that never releases, leaves a
    // trail that keeps the two measurably apart.
    //
    // The comparison is FSR2-to-FSR2 at the same pose, so it needs no golden and
    // no cross-GPU tolerance: both frames come from the same driver, the same
    // scene and the same settings, and differ only in what happened before them.
    TEST_F(FSR2VisualEvidenceTest, ConvergesAfterCameraMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TemporalUpscalerUsable())
            GTEST_SKIP() << "FSR2 is not available on this build/backend — the spatial upscaler is used instead";

        const ScopedMockTime scopedMockTime(kCaptureTime);
        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;
        pp.FSR2SharpeningEnabled = true;
        pp.FSR2Sharpness = 0.5f;

        const glm::vec3 poseA = { 0.0f, 7.0f, 16.0f };
        constexpr f32 yawA = 0.0f;
        constexpr f32 pitchA = 0.32f;

        // Settle at A and keep the reference.
        std::vector<u8> settledA;
        RunFramesAndCapture(MakeCamera(poseA, yawA, pitchA), kSettleFrames, "MotionRef", settledA);
        if (::testing::Test::HasFatalFailure())
        {
            pp.Upscale = UpscaleMode::Off;
            pp.Technique = UpscalerTechnique::Spatial;
            return;
        }

        // Move. Small per-frame steps on purpose: a large jump would be a camera
        // cut, which FSR2 is entitled to resolve by discarding its history — and
        // discarding history is exactly what would HIDE a reprojection bug. This
        // is continuous motion the motion vectors must actually describe.
        std::vector<u8> scratch;
        for (u32 step = 1; step <= 10u; ++step)
        {
            const f32 t = static_cast<f32>(step);
            const glm::vec3 pos = poseA + glm::vec3(0.35f * t, 0.10f * t, -0.25f * t);
            RunFramesAndCapture(MakeCamera(pos, yawA + 0.012f * t, pitchA), 1, "", scratch);
            if (::testing::Test::HasFatalFailure())
                break;
        }
        // ...and back along the same path, so the final frames re-see exactly the
        // geometry the outbound frames left behind.
        for (u32 step = 10u; step >= 1u; --step)
        {
            const f32 t = static_cast<f32>(step);
            const glm::vec3 pos = poseA + glm::vec3(0.35f * t, 0.10f * t, -0.25f * t);
            const std::string tag = (step == 5u) ? std::string("MotionMid") : std::string();
            RunFramesAndCapture(MakeCamera(pos, yawA + 0.012f * t, pitchA), 1, tag, scratch);
            if (::testing::Test::HasFatalFailure())
                break;
        }

        std::vector<u8> settledB;
        RunFramesAndCapture(MakeCamera(poseA, yawA, pitchA), kSettleFrames, "MotionSettled", settledB);

        pp.Upscale = UpscaleMode::Off; // restore for later tests in the process
        pp.Technique = UpscalerTechnique::Spatial;
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_GT(MeanLuma(settledB), 20.0)
            << "the frame after camera motion is (near-)black. See FSR2_MotionSettled.png";

        // The threshold is loose on purpose. FSR2 is stochastic across phases, so
        // two settled frames at the same pose are never bit-identical and a tight
        // bound would be a flake. What it DOES catch is the failure this test
        // exists for: real ghosting leaves whole silhouettes duplicated across the
        // frame, which is a mean absolute luma difference an order of magnitude
        // above this, not a fraction of a level.
        const f64 residual = MeanAbsDiff(settledA, settledB);
        EXPECT_LT(residual, 8.0)
            << "FSR2 did not converge back to its settled image after the camera moved away and "
               "returned (mean abs luma diff="
            << residual
            << "). That is ghosting: the history is being reprojected to the wrong place, or never "
               "released. Check the motion-vector scale sign in TemporalUpscalePolicy::MotionVectorScale "
               "first — an inverted one produces exactly this while every still frame looks correct. "
               "Compare FSR2_MotionRef.png with FSR2_MotionSettled.png.";
    }

    // The MSAA acceptance criterion: unsupported must mean GUARDED, not broken.
    // With MSAA on and the temporal technique requested, the pipeline falls back
    // to the spatial upscaler and the frame must still render normally.
    TEST_F(FSR2VisualEvidenceTest, MSAAFallsBackToSpatialAndStillRenders)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        auto& settings = Renderer3D::GetRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();

        const RenderingPath savedPath = settings.Path;
        const u32 savedSamples = settings.Deferred.MSAASampleCount;

        settings.Path = RenderingPath::Deferred;
        settings.Deferred.MSAASampleCount = 4u;
        Renderer3D::ApplyRendererSettings();

        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;

        std::vector<u8> px;
        RunFramesAndCapture(MakeCamera({ 0.0f, 7.0f, 16.0f }, 0.0f, 0.32f), 4, "MSAAFallback", px);

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        settings.Deferred.MSAASampleCount = savedSamples;
        settings.Path = savedPath;
        Renderer3D::ApplyRendererSettings();

        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_GT(MeanLuma(px), 20.0)
            << "requesting FSR2 with MSAA active produced a (near-)black frame. The guard is supposed to "
               "fall back to the FSR1 spatial upscaler, keeping the render scale — a black frame means "
               "neither upscaler declared its output while the post chain still expected one. "
               "See FSR2_MSAAFallback.png";
    }

    // FSR2's runtime is a third-party GL client that binds its own UBOs,
    // samplers and images to FIXED slot indices lifted from the DirectX
    // register numbers, and never restores them. Those indices share a
    // namespace with the engine's: the luminance-pyramid pass's constant
    // buffer lands on UBO binding 5, which is `MultiLightBuffer`.
    //
    // The engine binds that UBO once rather than per frame, so the damage is
    // not to FSR2's own frame at all — it is to EVERY LATER FRAME, whichever
    // technique renders it. That is what makes it so easy to misread as a
    // temporal-accumulation bug: the first frame is pixel-correct (nothing has
    // dispatched yet) and every frame after is uniformly dark, exactly the
    // shape of a bad history blend. Confirmed to fail with the restore scope
    // removed: 115.7 before FSR2 ran, 69.9 after.
    //
    // So the assertion deliberately renders NO FSR2 frame at the point it
    // measures: it compares native-before against native-after, which is the
    // only framing that pins the blame on the dispatch rather than the
    // upscale. Any future GL state FSR2 leaks — a sampler left on a unit, a
    // stale image binding — lands here too.
    TEST_F(FSR2VisualEvidenceTest, DispatchLeavesEngineBindingsIntactForLaterFrames)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TemporalUpscalerUsable())
            GTEST_SKIP() << "FSR2 is not available on this build/backend — nothing dispatches, nothing to leak";

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = MakeCamera({ 0.0f, 7.0f, 16.0f }, 0.0f, 0.32f);
        auto& pp = Renderer3D::GetPostProcessSettings();

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        std::vector<u8> before;
        RunFramesAndCapture(camera, 3, "BindingsBefore", before);
        if (::testing::Test::HasFatalFailure())
            return;

        // Let FSR2 own a few frames, then hand the frame back to the native path.
        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;
        std::vector<u8> ignored;
        RunFramesAndCapture(camera, kSettleFrames, "", ignored);

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        std::vector<u8> after;
        RunFramesAndCapture(camera, 3, "BindingsAfter", after);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 meanBefore = MeanLuma(before);
        const f64 meanAfter = MeanLuma(after);
        ASSERT_GT(meanBefore, 20.0) << "the native reference did not render";

        // Same scene, same camera, same technique — the only thing between the
        // two captures is that FSR2 dispatched. 2% is well inside the frame's
        // own repeatability and an order of magnitude under the 36% the leak
        // produced.
        EXPECT_LT(std::abs(meanAfter - meanBefore), meanBefore * 0.02)
            << "a native frame rendered AFTER an FSR2 dispatch differs from the identical frame rendered "
               "before it (before="
            << meanBefore << " after=" << meanAfter
            << "). FSR2 left GL binding state behind and the engine, which binds its light and shadow UBOs "
               "once rather than per frame, is now reading someone else's buffer. See FSR2BindingScope in "
               "OpenGLTemporalUpscaler.cpp, and FSR2_BindingsBefore.png vs FSR2_BindingsAfter.png";
    }

    // A CONVERGED temporal upscaler on a static scene must be stable FRAME TO
    // FRAME. Every other test in this file compares two SETTLED captures — same
    // pose, many frames apart — which is blind to the frame-to-frame shake a
    // user actually sees, because both samples are equally "settled" whatever
    // the jitter is doing. Reported from the editor as the whole picture
    // jittering constantly.
    //
    // The native path is measured in the same run as the control. FSR2 renders
    // with a sub-pixel jitter baked into the projection and is supposed to
    // RESOLVE it; if the resolve is wrong the jitter passes straight through and
    // the image swims by up to a render pixel every frame. Comparing against
    // native separates "the upscaler is shaking" from "something in the scene
    // moves", which would shake both.
    TEST_F(FSR2VisualEvidenceTest, SettledTemporalFrameIsStableFrameToFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TemporalUpscalerUsable())
            GTEST_SKIP() << "FSR2 is not available on this build/backend";

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = MakeCamera({ 0.0f, 7.0f, 16.0f }, 0.0f, 0.32f);
        auto& pp = Renderer3D::GetPostProcessSettings();

        const auto worstConsecutiveDiff = [&](u32 frames)
        {
            std::vector<u8> prev;
            RunFramesAndCapture(camera, kSettleFrames, "", prev);
            f64 worst = 0.0;
            for (u32 i = 0; i < frames; ++i)
            {
                std::vector<u8> next;
                RunFramesAndCapture(camera, 1, "", next);
                worst = std::max(worst, MeanAbsDiff(prev, next));
                prev.swap(next);
            }
            return worst;
        };

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        const f64 nativeWorst = worstConsecutiveDiff(6);

        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Temporal;
        const f64 temporalWorst = worstConsecutiveDiff(6);

        pp.Upscale = UpscaleMode::Off;
        pp.Technique = UpscalerTechnique::Spatial;
        if (::testing::Test::HasFatalFailure())
            return;

        std::cout << "[FSR2 frame-to-frame stability] native worst mean|d| = " << nativeWorst
                  << "   temporal worst mean|d| = " << temporalWorst << std::endl;

        EXPECT_LT(temporalWorst, 1.0)
            << "a settled FSR2 frame changes by mean|d|=" << temporalWorst
            << " between CONSECUTIVE frames on a static camera (native control=" << nativeWorst
            << "). The upscaler is passing its own projection jitter through instead of resolving it, "
               "which reads on screen as the whole image swimming. Check that the jitter handed to FSR2 "
               "matches the SIGN and SCALE of the offset baked into the projection matrix.";
    }

} // namespace OloEngine::Tests
