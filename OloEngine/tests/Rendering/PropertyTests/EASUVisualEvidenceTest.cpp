// OLO_TEST_LAYER: L8
// =============================================================================
// EASUVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the FSR1 EASU/RCAS
// spatial upscale (PostProcess_EASU.glsl + PostProcess_RCAS.glsl, EASURenderPass
// + UpscalerRenderPass, reduced-size scene band). A busy scene is rendered twice
// through the FULL Renderer3D pipeline from the same pose — once at NATIVE
// resolution (Upscale == Off) and once with an FSR1 preset (the scene renders
// below display res and EASU upscales it back) — and both composited frames are
// written to OloEditor/assets/tests/visual/EASU_<state>.png.
//
// The contract is GOLDEN-FREE and differential, robust across GPUs:
//   * Both frames render (not near-black).
//   * The upscaled frame actually DIFFERS from native (proves the reduced-res +
//     EASU path ran, not a silent pass-through at full res).
//   * Overall brightness is preserved (upscaling reconstructs the same content,
//     it does not darken/brighten the frame).
//   * The upscaled frame retains most of the native frame's high-frequency
//     energy (EASU + RCAS reconstruct edges — it must NOT collapse to a blurry
//     low-detail image the way a naive bilinear upscale would).
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists.
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "Platform/OpenGL/OpenGLDebug.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
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

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
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

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += Luma(px, x, y);
            return sum / (static_cast<f64>(kWidth) * kHeight);
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

    class EASUVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

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

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for EASU capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

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

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("EASU_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        // Render the scene at NATIVE (Upscale == Off) and at FSR1 Performance
        // (0.5x scene band + EASU upscale), write PNG evidence tagged with the
        // given prefix, and assert the golden-free contract. Shared by the
        // forward + deferred tests. The render path must already be configured by
        // the caller.
        void RunUpscaleContract(const std::string& tagPrefix)
        {
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
            } scopedMockTime(kCaptureTime);

            const glm::vec3 pos = { 0.0f, 7.0f, 16.0f };
            constexpr f32 yaw = 0.0f;
            constexpr f32 pitch = 0.32f;

            auto& pp = Renderer3D::GetPostProcessSettings();

            pp.Upscale = UpscaleMode::Off;
            std::vector<u8> nativePixels;
            Capture(tagPrefix + "Native", pos, yaw, pitch, nativePixels);
            if (::testing::Test::HasFatalFailure())
                return;

            pp.Upscale = UpscaleMode::Performance; // 0.5x render scale (2x area win)
            pp.RCASSharpness = 0.6f;
            std::vector<u8> upscaledPixels;
            Capture(tagPrefix + "Performance", pos, yaw, pitch, upscaledPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            pp.Upscale = UpscaleMode::Off; // restore for later tests

            const f64 nativeMean = MeanLuma(nativePixels);
            const f64 upMean = MeanLuma(upscaledPixels);
            EXPECT_GT(nativeMean, 20.0) << tagPrefix << "native frame rendered (near-)black";
            EXPECT_GT(upMean, 20.0) << tagPrefix << "upscaled frame rendered (near-)black";

            // The reduced-res + EASU path must actually change the frame.
            const f64 diff = MeanAbsDiff(nativePixels, upscaledPixels);
            EXPECT_GT(diff, 0.5)
                << tagPrefix << "FSR1 frame is essentially identical to native (mean abs luma diff=" << diff
                << ") — the reduced-res upscale path may not be running. See EASU_" << tagPrefix << "*.png";

            // Upscaling reconstructs the same scene: overall brightness must stay
            // close (within ~15%).
            EXPECT_LT(std::abs(upMean - nativeMean), nativeMean * 0.15)
                << tagPrefix << "FSR1 shifted overall brightness too much (native=" << nativeMean
                << " up=" << upMean << ")";

            // EASU + RCAS must reconstruct edges: the upscaled frame keeps most of
            // native's high-frequency energy. A naive half-res bilinear upscale
            // would collapse well below this; require >= 60% of native. (It can
            // even exceed native because RCAS sharpens.)
            const f64 nativeEnergy = GradientEnergy(nativePixels);
            const f64 upEnergy = GradientEnergy(upscaledPixels);
            EXPECT_GT(upEnergy, nativeEnergy * 0.60)
                << tagPrefix << "FSR1 upscale lost too much detail (native energy=" << nativeEnergy
                << " up=" << upEnergy << ", ratio=" << (upEnergy / nativeEnergy)
                << ") — EASU/RCAS reconstruction may be failing. See EASU_" << tagPrefix << "*.png";
        }
    };

    // Native vs FSR1 Performance: the scene renders below display res and EASU
    // upscales it. The upscaled frame must render, differ from native, preserve
    // brightness, and keep most of native's high-frequency energy (EASU/RCAS
    // reconstruct edges — no bilinear-blur collapse). SKIPs without GL 4.6.
    TEST_F(EASUVisualEvidenceTest, UpscaleReconstructsDisplayResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // Default forward path (BuildScene leaves it unchanged).
        RunUpscaleContract("");
    }

    // Same contract on the DEFERRED path, where the reduced scene band means the
    // G-buffer + deferred lighting run at reduced res before EASU upscales. This
    // is the path with the more complex sizing interaction (separate G-buffer +
    // DeferredLightingPass), so it gets its own evidence capture. SKIPs w/o GL 4.6.
    TEST_F(EASUVisualEvidenceTest, UpscaleReconstructsDisplayResolutionDeferred)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        const RenderingPath savedPath = settings.Path;
        settings.Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        RunUpscaleContract("Deferred_");

        settings.Path = savedPath; // restore for later tests
        Renderer3D::ApplyRendererSettings();
    }

    // Depth-of-field (a DEPTH-driven post effect) with FSR1 upscale active. This
    // exercises DepthVelocityUpscalePass: DOF runs at full display res and reads
    // the nearest-upscaled full-res depth. The frame must render and DOF must
    // actually alter it (depth-based blur) vs the no-DOF upscaled frame — if the
    // full-res depth were black/broken, DOF would blur uniformly or not at all.
    // The PNG (EASU_DOF_*.png) is inspected for a correct near-focus/far-blur look.
    TEST_F(EASUVisualEvidenceTest, DepthOfFieldConsumesUpscaledDepth)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        const glm::vec3 pos = { 0.0f, 7.0f, 16.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f;

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Upscale = UpscaleMode::Performance;
        pp.RCASSharpness = 0.6f;

        pp.DOFEnabled = false;
        std::vector<u8> noDofPixels;
        Capture("DOF_Off", pos, yaw, pitch, noDofPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // Focus on the near cluster (~10-14 units), heavily blur the far floor.
        pp.DOFEnabled = true;
        pp.DOFFocusDistance = 12.0f;
        pp.DOFFocusRange = 4.0f;
        pp.DOFBokehRadius = 6.0f;
        std::vector<u8> dofPixels;
        Capture("DOF_On", pos, yaw, pitch, dofPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        pp.DOFEnabled = false;
        pp.Upscale = UpscaleMode::Off; // restore

        EXPECT_GT(MeanLuma(dofPixels), 20.0) << "DOF+upscale frame rendered (near-)black";

        // DOF must change the frame (depth-driven blur read the upscaled depth).
        const f64 diff = MeanAbsDiff(noDofPixels, dofPixels);
        EXPECT_GT(diff, 0.5)
            << "DOF did not alter the upscaled frame (mean abs luma diff=" << diff
            << ") — the full-res depth may not be reaching DOF. See EASU_DOF_*.png";

        // Depth-based blur removes high-frequency detail from the defocused
        // region, so the DOF frame must have LESS gradient energy than the sharp
        // no-DOF frame (a broken all-near/all-far depth would blur uniformly or
        // not at all — this still catches a no-op).
        const f64 sharpEnergy = GradientEnergy(noDofPixels);
        const f64 dofEnergy = GradientEnergy(dofPixels);
        EXPECT_LT(dofEnergy, sharpEnergy)
            << "DOF did not reduce high-frequency detail (sharp=" << sharpEnergy
            << " dof=" << dofEnergy << ") — depth-driven defocus may be failing. See EASU_DOF_*.png";
    }

    // Regression test for issue #504: GTAO's scratch textures (GTAODenoisePing/
    // Pong/Edge/HZB, declared in PopulateBlackboard) must track the scene-band
    // resolution FSR1 renders at, same as AOBuffer — not the full display
    // resolution the post chain runs at. When they didn't, GTAORenderPass::
    // Execute's final glCopyImageSubData copied a display-sized region into the
    // smaller (scene-band-sized) AOBuffer and overran its Y bound
    // (GL_INVALID_VALUE id 1281) the instant Upscale left Off at runtime — the
    // two sizes are identical (and the bug invisible) only when Upscale == Off,
    // which is why boot-time-Off scenes never surfaced it. SSAO went through
    // the same runtime switch in RunUpscaleContract above without issue (its
    // AOBuffer + scratch targets both derive from sceneSpec directly), so this
    // needs its own GTAO-enabled repro rather than reusing that helper.
    // GTAO is not the default AOTechnique (SSAO is), so this must explicitly
    // opt in — every other test in this file runs with the SSAO/GTAO defaults
    // and never touched this path.
    TEST_F(EASUVisualEvidenceTest, GTAOSurvivesRuntimeUpscaleSwitch)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        // GTAO is not the default AOTechnique (SSAO is) — this restores it
        // unconditionally via RAII so a fatal ASSERT/EXPECT failure inside
        // Capture() below (which returns early) can't leave global renderer
        // state mutated for later tests in the suite.
        struct ScopedAOTechnique
        {
            PostProcessSettings& Settings;
            AOTechnique SavedTechnique;
            bool SavedGtaoEnabled;

            explicit ScopedAOTechnique(PostProcessSettings& settings)
                : Settings(settings), SavedTechnique(settings.ActiveAOTechnique), SavedGtaoEnabled(settings.GTAOEnabled)
            {
                Settings.ActiveAOTechnique = AOTechnique::GTAO;
                Settings.GTAOEnabled = true;
            }
            ~ScopedAOTechnique()
            {
                Settings.ActiveAOTechnique = SavedTechnique;
                Settings.GTAOEnabled = SavedGtaoEnabled;
            }
        } scopedAOTechnique(Renderer3D::GetPostProcessSettings());

        const glm::vec3 pos = { 0.0f, 7.0f, 16.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f;

        auto& pp = Renderer3D::GetPostProcessSettings();

        // Reset the GL debug callback's error counter so only errors from this
        // test's captures (not earlier tests in the suite) count below.
        ResetGLErrorCount();

        pp.Upscale = UpscaleMode::Off;
        std::vector<u8> nativePixels;
        Capture("GTAONative", pos, yaw, pitch, nativePixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // The runtime switch itself is the regression: GTAO's scratch textures
        // were declared once (at display res) and never resized to the reduced
        // scene band this transition introduces.
        pp.Upscale = UpscaleMode::Performance;
        pp.RCASSharpness = 0.6f;
        std::vector<u8> upscaledPixels;
        Capture("GTAOPerformance", pos, yaw, pitch, upscaledPixels);

        pp.Upscale = UpscaleMode::Off; // restore

        if (::testing::Test::HasFatalFailure())
            return;

        // The statistical contract in RunUpscaleContract can pass even with
        // GL_INVALID_VALUE spam (a corrupted corner of the frame doesn't
        // necessarily fail loose mean-luma/energy thresholds), so check the
        // GL debug callback's own error counter directly instead of parsing
        // log text.
        EXPECT_EQ(GetGLErrorCount(), 0u)
            << "GL error(s) after switching Upscale to Performance with GTAO active — see OloEngine.log for detail";

        // Deliberately NOT asserting on absolute brightness here (unlike
        // RunUpscaleContract's native-vs-upscaled checks above): GTAO's
        // AOApplyPass reuses PostProcess_SSAOApply.glsl, which is written for
        // SSAO's HALF-resolution AOBuffer (see its "This texture is at HALF
        // resolution" comment) even though GTAO's AOBuffer is FULL scene-band
        // resolution — a separate, pre-existing issue (reproduces even at
        // Upscale == Off, so it predates and is unrelated to this fix) that
        // makes GTAO-lit scenes render too dark to use as a brightness oracle.
        // Tracked as #533; out of scope here. The switch-safety contract
        // this test exists for is the GL-error-counter check above and the
        // "upscaled isn't darker than native" check below, which both hold
        // regardless.
        const f64 nativeMean = MeanLuma(nativePixels);
        const f64 upMean = MeanLuma(upscaledPixels);
        EXPECT_GE(upMean, nativeMean * 0.5)
            << "runtime Upscale switch made the GTAO-lit frame substantially darker than native (native="
            << nativeMean << " up=" << upMean << ") — see EASU_GTAONative.png / EASU_GTAOPerformance.png";
    }

    // Guard for issue #563 (the render-graph half of #549): a forward FSR1
    // Upscale -> Off transition must NOT leave the scene black for the short
    // warm-up a temporal-sensitive visual test reads back.
    //
    // An FSR1 render shrinks the ScenePass / AO scene-band framebuffers below
    // display res and pools reduced-size SceneColor / SceneDepth / SceneNormals /
    // Velocity transients. When the band is later restored to full res by a
    // viewport-resize event whose DISPLAY size is UNCHANGED (exactly what happens
    // when the next full-size scene is entered in one process — e.g. a following
    // visual test's EnableRendering -> OnWindowResize -> RenderGraph::Resize),
    // the old display-size-gated pool eviction did not fire. The pool kept the
    // stale reduced-size transients and the alias-group resolver handed one to
    // the scene chain for the first frames after the transition — a black scene.
    // This models the real order-dependent CAS-after-EASU shuffle failure (which
    // enters CAS's full-size scene right after an FSR1 render) deterministically
    // in one fixture, via the same-display-size ResizeRenderTarget below.
    //
    // Frame 0 after any scene/config change is a legitimate warm-up frame (black
    // even in isolation, which the pipeline's ~1-frame temporal latency explains);
    // the regression was that frame 1 was ALSO black. So warm exactly two frames
    // (the minimum a temporal-sensitive victim like CASVisualEvidenceTest uses)
    // after the transition and assert the composited frame is not black. SKIPs
    // without a GL 4.6 context.
    TEST_F(EASUVisualEvidenceTest, ForwardUpscaleOffTransitionKeepsSceneVisible)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 7.0f, 16.0f }, 0.0f, 0.32f);

        auto& pp = Renderer3D::GetPostProcessSettings();

        // Mimic the producer's full render history so the transient pool ends up
        // in the same stale state the real cross-test ordering produces: a native
        // (full-res) render pools full-size SceneColor transients, then an FSR1
        // Performance render shrinks the band and pools reduced-size ones.
        pp.Upscale = UpscaleMode::Off;
        pp.RCASSharpness = 0.6f;
        RunEditorFrames(camera, 3);

        pp.Upscale = UpscaleMode::Performance;
        RunEditorFrames(camera, 3);

        // Back to native, then re-enter the full-size scene via a same-display-
        // size viewport resize (mimicking the next visual test's EnableRendering
        // -> OnWindowResize). This restores the reduced band to full res through
        // RenderGraph::Resize WITHOUT a display-dimension change — the arm that
        // the old display-only pool eviction missed and that leaves stale
        // reduced-size transients in the pool.
        pp.Upscale = UpscaleMode::Off;
        ResizeRenderTarget(kWidth, kHeight);

        // Warm only two frames after the transition. Frame 1 must carry scene
        // content, not the stale-transient black.
        RunEditorFrames(camera, 2);

        auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
        if (!fb)
            fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
        ASSERT_TRUE(fb) << "No composited framebuffer after the upscale-off transition";

        std::vector<u8> px;
        ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, px);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

        const f64 meanLuma = MeanLuma(px);
        EXPECT_GT(meanLuma, 20.0)
            << "scene rendered (near-)black on the 2-frame warm-up after a forward FSR1 "
               "Upscale->Off transition (#563): mean luma="
            << meanLuma
            << ". The render-graph transient pool likely kept a stale reduced-size "
               "framebuffer across the scene-band resize.";
    }
} // namespace OloEngine::Tests
