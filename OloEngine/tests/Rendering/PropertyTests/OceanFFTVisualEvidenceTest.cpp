// =============================================================================
// OceanFFTVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the Tessendorf FFT ocean
// (WATER_FUTURE_IMPROVEMENTS.md §1). Renders an FFT-enabled water surface
// through the FULL Renderer3D pipeline from several camera poses and writes each
// frame to OloEditor/assets/tests/visual/OceanFFT_<pose>.png so a human (or the
// agent) can eyeball the spectral ocean from the side, straddling the waterline,
// submerged, and top-down — the angles where water work historically broke.
//
// Unlike WaterVisualEvidenceTest (Gerstner, golden-PNG RMSE), this test asserts
// only DRIVER-INDEPENDENT contracts so it is portable across GPUs:
//   1. Every pose renders non-black.
//   2. The open-ocean band reads as opaque water (blue/teal dominant), not the
//      magenta seafloor showing through.
//   3. Toggling the FFT path on vs off VISIBLY changes the surface — proof the
//      whole chain works end to end (OceanFFTField CPU evaluation → GPU
//      displacement/normal textures → Water.glsl sampling), not just that the
//      flag is wired.
//
// The PNGs are evidence artifacts, not committed goldens (the spectral detail is
// GPU-float sensitive), so this test never fails on cross-GPU pixel drift.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// SKIPs cleanly when no GL 4.6 context exists.
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
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }
    } // namespace

    class OceanFFTVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.5f));
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
                Entity ocean = scene.CreateEntity("Ocean");
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 200.0f;
                wc.m_WorldSizeZ = 200.0f;
                wc.m_GridResolutionX = 200;
                wc.m_GridResolutionZ = 200;
                wc.m_RenderFromBelow = true;
                wc.m_UnderwaterFogColor = glm::vec3(0.04f, 0.18f, 0.3f);
                wc.m_UnderwaterFogDensity = 0.08f;
                // FFT ocean enabled.
                wc.m_UseFFT = true;
                wc.m_FFTResolution = 128;
                wc.m_FFTPatchSize = 64.0f;
                wc.m_FFTWindSpeed = 18.0f;
                wc.m_FFTWindDirection = glm::vec2(1.0f, 0.3f);
                wc.m_FFTAmplitude = 3.0f;
                wc.m_FFTChoppiness = 1.4f;
                wc.m_FFTHeightScale = 1.0f;
                m_OceanEntity = ocean;
            }

            auto addPrimitive = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                         const glm::vec3& scale, const glm::vec3& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            };
            addPrimitive("Seafloor", MeshPrimitive::Plane, { 0.0f, -20.0f, 0.0f }, { 60.0f, 1.0f, 60.0f },
                         { 1.0f, 0.0f, 1.0f });
            addPrimitive("Pillar", MeshPrimitive::Cube, { 8.0f, -9.0f, 4.0f }, { 1.5f, 22.0f, 1.5f },
                         { 0.6f, 0.6f, 0.62f });
        }

        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // Flip vertically so the PNG is right-side up (GL origin is bottom-left).
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
            const std::string path = (dir / ("OceanFFT_" + poseName + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             outPixels.data(), static_cast<int>(kWidth) * 4);
        }

        Entity m_OceanEntity;
    };

    TEST_F(OceanFFTVisualEvidenceTest, CaptureFFTOceanFromMultipleAngles)
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

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        const std::array<Pose, 4> poses = { {
            { "Overview", { 0.0f, 16.0f, 38.0f }, 0.0f, 0.40f },
            { "GrazingAcross", { 0.0f, 3.0f, 42.0f }, 0.0f, 0.05f },
            { "Submerged", { 0.0f, -4.0f, 18.0f }, 0.0f, 0.10f },
            { "TopDown", { 0.0f, 18.0f, 7.0f }, 0.0f, 1.30f },
        } };

        std::vector<u8> grazingPixels;
        for (const auto& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Position, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
                return;

            u64 lumaSum = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 5.0) << "Pose '" << pose.Name << "' rendered (near-)black";

            if (std::string(pose.Name) == "GrazingAcross")
                grazingPixels = pixels;
        }

        ASSERT_FALSE(grazingPixels.empty());

        // The foreground water band must read as opaque water (blue/teal), not the
        // magenta seafloor showing through a see-through / un-displaced surface.
        const u32 bandY0 = (kHeight * 3u) / 4u;
        const u32 bandY1 = (kHeight * 7u) / 8u;
        u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
        for (u32 y = bandY0; y < bandY1; ++y)
            for (u32 x = kWidth / 4u; x < (kWidth * 3u) / 4u; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                sumR += grazingPixels[idx + 0];
                sumG += grazingPixels[idx + 1];
                sumB += grazingPixels[idx + 2];
                ++count;
            }
        ASSERT_GT(count, 0u);
        const f64 meanR = static_cast<f64>(sumR) / count;
        const f64 meanG = static_cast<f64>(sumG) / count;
        const f64 meanB = static_cast<f64>(sumB) / count;
        const bool looksLikeMagentaSeafloor =
            (meanR > 110.0) && (meanB > 110.0) && (meanG < meanR * 0.55) && (meanG < meanB * 0.55);
        EXPECT_FALSE(looksLikeMagentaSeafloor)
            << "Foreground band reads as the magenta seafloor (R=" << meanR << " G=" << meanG
            << " B=" << meanB << ") — FFT surface see-through. See OceanFFT_GrazingAcross.png";
        EXPECT_GE(meanB, meanR)
            << "Foreground band is not water-blue (R=" << meanR << " G=" << meanG << " B=" << meanB << ")";
    }

    // Toggling the FFT path must visibly change the surface vs the analytic
    // Gerstner path from the same pose — end-to-end proof the spectral field is
    // generated, uploaded, and sampled (not just that the flag is plumbed).
    TEST_F(OceanFFTVisualEvidenceTest, FFTToggleVisiblyChangesSurface)
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

        // A pose looking down the surface so wave shape dominates the frame.
        const glm::vec3 pos(0.0f, 6.0f, 40.0f);
        const f32 yaw = 0.0f, pitch = 0.20f;

        std::vector<u8> fftOn;
        Capture("ToggleOn", pos, yaw, pitch, fftOn);
        if (::testing::Test::HasFatalFailure())
            return;

        // Flip the ocean to the Gerstner path and re-capture the same pose.
        ASSERT_TRUE(static_cast<bool>(m_OceanEntity));
        m_OceanEntity.GetComponent<WaterComponent>().m_UseFFT = false;
        std::vector<u8> fftOff;
        Capture("ToggleOff", pos, yaw, pitch, fftOff);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 rmse = Rgba8Rmse(fftOn, fftOff);
        EXPECT_GT(rmse, 3.0)
            << "FFT-on and FFT-off frames are nearly identical (RMSE " << rmse
            << ") — the FFT surface is not actually being applied. Compare "
               "OceanFFT_ToggleOn.png vs OceanFFT_ToggleOff.png";
    }

    // The GPU compute butterfly and the CPU reference must render the SAME
    // ocean (§1.2 — same h0, same math, different producer): toggling
    // m_FFTUseGpuCompute from the same pose must leave the frame essentially
    // unchanged. This is the inverse assertion of the FFT on/off test above,
    // and it goes through the full real pipeline (Scene update → compute
    // dispatch / CPU upload → Water.glsl sampling → tonemap).
    TEST_F(OceanFFTVisualEvidenceTest, GpuComputeToggleLeavesSurfaceUnchanged)
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

        const glm::vec3 pos(0.0f, 6.0f, 40.0f);
        const f32 yaw = 0.0f, pitch = 0.20f;

        ASSERT_TRUE(static_cast<bool>(m_OceanEntity));
        auto& wc = m_OceanEntity.GetComponent<WaterComponent>();
        ASSERT_TRUE(wc.m_UseFFT);

        wc.m_FFTUseGpuCompute = true;
        std::vector<u8> gpuFrame;
        Capture("GpuCompute", pos, yaw, pitch, gpuFrame);
        if (::testing::Test::HasFatalFailure())
            return;

        wc.m_FFTUseGpuCompute = false;
        std::vector<u8> cpuFrame;
        Capture("CpuReference", pos, yaw, pitch, cpuFrame);
        if (::testing::Test::HasFatalFailure())
            return;

        // Non-black sanity so "both frames empty" can't pass as "identical".
        u64 lumaSum = 0;
        for (std::size_t i = 0; i < gpuFrame.size(); i += 4)
            lumaSum += gpuFrame[i] + gpuFrame[i + 1] + gpuFrame[i + 2];
        const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
        ASSERT_GT(meanChannel, 5.0) << "GPU-compute frame rendered (near-)black";

        const f64 rmse = Rgba8Rmse(gpuFrame, cpuFrame);
        EXPECT_LT(rmse, 2.0)
            << "GPU-compute and CPU-reference frames differ visibly (RMSE " << rmse
            << ") — the compute pipeline is not producing the reference ocean. Compare "
               "OceanFFT_GpuCompute.png vs OceanFFT_CpuReference.png";
    }
} // namespace OloEngine::Tests
