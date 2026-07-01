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
} // namespace OloEngine::Tests
