// OLO_TEST_LAYER: L8
// =============================================================================
// OceanCascadeVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the band-limited three-cascade FFT ocean preset
// (issue #969; water-ocean.md §1.3), captured at the NEAR, MID
// and HORIZON distances the issue's acceptance criteria name, on a sea built
// from Drift's authored water settings.
//
// WHY THE ASSERTIONS ARE A/B RATHER THAN GOLDEN. The subject here is a CHANGE —
// "the same sea, with three bands instead of one" — and
// docs/agent-rules/cpu-gpu-surface-parity.md §5b is the postmortem of five
// acceptance captures that were happily rebased as goldens while four of them
// pointed away from the subject and the fifth at empty sky. A golden is whatever
// the camera saw; a difference between two renders that differ ONLY in the thing
// under test is the assertion that knows the subject was in shot. So every pose
// renders twice, one cascade and three, and the frames must differ.
//
// The PNGs are evidence artifacts, not committed goldens (spectral detail is
// GPU-float sensitive), so this never fails on cross-GPU pixel drift. Written to
// OloEditor/assets/tests/visual/OceanCascade_<pose>_<mode>.png so the two can be
// flipped between by eye — which is the check unit tests cannot make and
// CLAUDE.md requires for a rendering change.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// SKIPs cleanly when no GL 4.6 context exists.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Ocean/OceanCascades.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
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

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        constexpr f32 kCaptureTime = 12.0f;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            f64 sumSq = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }
    } // namespace

    class OceanCascadeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 40.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.6f, -0.7f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
                dl.m_Intensity = 2.0f;
            }
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.3f;
            }
            {
                // Drift's authored sea, verbatim where it matters: the patch
                // size, wind, amplitude and choppiness the scene ships with, so
                // the captures answer "what would Drift look like" rather than
                // "what does a sea tuned for this test look like". Big enough
                // that the horizon pose has open water to reach.
                Entity ocean = scene.CreateEntity("Sea");
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 1600.0f;
                wc.m_WorldSizeZ = 1600.0f;
                wc.m_GridResolutionX = 640;
                wc.m_GridResolutionZ = 640;
                wc.m_WaterColor = glm::vec3(0.09f, 0.33f, 0.44f);
                wc.m_DeepColor = glm::vec3(0.015f, 0.075f, 0.14f);
                wc.m_Transparency = 0.45f;
                wc.m_Reflectivity = 0.02f;
                wc.m_FresnelPower = 5.0f;
                wc.m_SpecularIntensity = 1.4f;
                wc.m_RenderFromBelow = true;
                wc.m_UseFFT = true;
                wc.m_FFTResolution = 128;
                wc.m_FFTPatchSize = 140.0f;
                wc.m_FFTWindSpeed = 8.0f;
                wc.m_FFTWindDirection = glm::vec2(1.0f, 0.2f);
                wc.m_FFTAmplitude = 0.55f;
                wc.m_FFTChoppiness = 0.9f;
                wc.m_FFTHeightScale = 1.0f;
                wc.m_FFTCascades = Ocean::kSingleCascadeCount; // per-capture below
                m_OceanEntity = ocean;
            }
            {
                // A seafloor, so a see-through surface reads as an obvious
                // failure rather than as deep water.
                Entity e = scene.CreateEntity("Seafloor");
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -26.0f, 0.0f };
                tc.Scale = { 400.0f, 1.0f, 400.0f };
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.08f, 1.0f));
            }
        }

        void SetCascades(u32 count)
        {
            ASSERT_TRUE(m_OceanEntity);
            m_OceanEntity.GetComponent<WaterComponent>().m_FFTCascades = count;
        }

        void Capture(const std::string& fileName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << fileName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            // Flip vertically so the PNG is right-side up (GL origin is bottom-left).
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<sizet>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("OceanCascade_" + fileName + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             outPixels.data(), static_cast<int>(kWidth) * 4);
        }

        Entity m_OceanEntity;
    };

    TEST_F(OceanCascadeVisualEvidenceTest, CaptureNearMidAndHorizonForBothCascadeModes)
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

        // The three distances the issue's acceptance criteria name. NEAR is a
        // chase-camera height above the surface — where "close chop" has to
        // read; MID looks out over a few hundred metres; HORIZON is the pose
        // the issue's opening symptom lives at, where one tile's repetition is
        // most visible and a long swell is the only thing that can carry the
        // frame.
        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        const std::array<Pose, 3> poses = { {
            { "Near", { 0.0f, 2.5f, 0.0f }, 0.0f, 0.06f },
            { "Mid", { 0.0f, 14.0f, 0.0f }, 0.0f, 0.10f },
            { "Horizon", { 0.0f, 45.0f, 0.0f }, 0.0f, 0.03f },
        } };

        for (const auto& pose : poses)
        {
            std::vector<u8> single;
            std::vector<u8> three;

            SetCascades(Ocean::kSingleCascadeCount);
            if (::testing::Test::HasFatalFailure())
                return;
            Capture(std::string(pose.Name) + "_Single", pose.Position, pose.Yaw, pose.Pitch, single);
            if (::testing::Test::HasFatalFailure())
                return;

            SetCascades(Ocean::kThreeBandCascadeCount);
            Capture(std::string(pose.Name) + "_ThreeBand", pose.Position, pose.Yaw, pose.Pitch, three);
            if (::testing::Test::HasFatalFailure())
                return;

            // 1. Both frames exist at all.
            for (const auto* pair : { &single, &three })
            {
                u64 lumaSum = 0;
                for (sizet i = 0; i < pair->size(); i += 4)
                    lumaSum += (*pair)[i] + (*pair)[i + 1] + (*pair)[i + 2];
                const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
                EXPECT_GT(meanChannel, 5.0) << "Pose '" << pose.Name << "' rendered (near-)black";
            }

            // 2. THE ASSERTION THAT KNOWS THE SUBJECT WAS IN SHOT. The only
            // difference between the two renders is the cascade count, so a
            // zero here means the preset reached no pixel — the sea was out of
            // frame, the FFT path never engaged, or the extra layers were never
            // sampled. An RMSE against a rebased golden cannot say any of that.
            const f64 rmse = Rgba8Rmse(single, three);
            std::cout << "[ DIAG ] pose '" << pose.Name << "' single-vs-three-band RMSE = " << rmse << "\n";
            EXPECT_GT(rmse, 0.5) << "Pose '" << pose.Name
                                 << "': the three-band preset changed nothing on screen";
        }
    }

    TEST_F(OceanCascadeVisualEvidenceTest, HorizonSeaIsOpaqueWaterUnderBothModes)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The failure the added bands could plausibly cause and the A/B above
        // cannot see: a summed displacement that overshoots, tears the surface
        // open, and lets the seafloor through. Checked at the horizon pose,
        // where the grazing angle makes a hole in the sea most visible.
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

        for (u32 cascades : { Ocean::kSingleCascadeCount, Ocean::kThreeBandCascadeCount })
        {
            SetCascades(cascades);
            if (::testing::Test::HasFatalFailure())
                return;
            std::vector<u8> pixels;
            Capture("HorizonWaterBand_" + std::to_string(cascades), { 0.0f, 45.0f, 0.0f }, 0.0f, 0.03f, pixels);
            if (::testing::Test::HasFatalFailure())
                return;

            // The foreground band, well below the horizon line.
            const u32 bandY0 = (kHeight * 3u) / 4u;
            const u32 bandY1 = (kHeight * 7u) / 8u;
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = bandY0; y < bandY1; ++y)
            {
                for (u32 x = kWidth / 4u; x < (kWidth * 3u) / 4u; ++x)
                {
                    const sizet idx = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    sumR += pixels[idx + 0];
                    sumG += pixels[idx + 1];
                    sumB += pixels[idx + 2];
                    ++count;
                }
            }
            ASSERT_GT(count, 0u);
            const f64 r = static_cast<f64>(sumR) / static_cast<f64>(count);
            const f64 g = static_cast<f64>(sumG) / static_cast<f64>(count);
            const f64 b = static_cast<f64>(sumB) / static_cast<f64>(count);
            std::cout << "[ DIAG ] cascades=" << cascades << " foreground band mean RGB = (" << r << ", " << g
                      << ", " << b << ")\n";
            EXPECT_GT(b, r) << "cascades=" << cascades
                            << ": the foreground sea is not reading as water — the seafloor is showing through";
        }
    }
} // namespace OloEngine::Tests
