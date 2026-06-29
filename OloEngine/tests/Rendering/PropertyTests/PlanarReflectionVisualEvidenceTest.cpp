// =============================================================================
// PlanarReflectionVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent differential contract for the
// planar-reflection pass (PlanarReflectionRenderPass + Water.glsl). Renders a
// reflective water plane with a tall, vivid RED pillar standing above it, from
// several camera angles, through the FULL Renderer3D pipeline, and writes each
// frame to OloEditor/assets/tests/visual/PlanarReflection_<pose>.png.
//
// A planar reflection can pass every CPU/contract test (PlanarReflectionMathTest)
// and still look wrong on screen — the mirror could be flipped, offset, clipped
// at the wrong side, or simply never sampled. So the real proof is a DIFFERENTIAL
// contract that no other reflection tech can fake: render the identical frozen
// frame twice, once with planar reflections ON and once OFF, and assert that the
// water in front of the red pillar gains red ONLY when the mirror is on. The red
// can only have arrived by the pillar being reflected down into the water — the
// cubemap/SSR fallbacks reflect the sky, not a red pillar that isn't on screen
// where the water samples.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// Runs in the normal suite; SKIPs cleanly without a GL 4.6 context (issue #258).
// =============================================================================

// OLO_TEST_LAYER: L8

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

#include <array>
#include <cmath>
#include <cstdlib>
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
        constexpr f32 kCaptureTime = 9.0f; // frozen wall clock → deterministic waves

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        // Mean per-channel value over a rectangular band of an RGBA8 frame
        // (rows are top-down after the vertical flip in Capture()).
        struct BandMean
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
        };
        [[nodiscard]] BandMean MeanOfBand(const std::vector<u8>& px, u32 x0, u32 x1, u32 y0, u32 y1)
        {
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sumR += px[i + 0];
                    sumG += px[i + 1];
                    sumB += px[i + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count, static_cast<f64>(sumB) / count };
        }
    } // namespace

    class PlanarReflectionVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Sun.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            // Skybox / IBL so unreflected water still has a sky to mirror (and so
            // the reflection-OFF baseline isn't pitch black).
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.4f;
            }

            // Reflective water plane at y = 0 (Gerstner, not FFT, so the surface is
            // a calm near-mirror — the clearest test of the reflection).
            {
                Entity ocean = scene.CreateEntity("ReflectiveWater");
                m_WaterEntity = ocean;
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 120.0f;
                wc.m_WorldSizeZ = 120.0f;
                wc.m_GridResolutionX = 96;
                wc.m_GridResolutionZ = 96;
                wc.m_WaveAmplitude = 0.12f; // gentle ripple — keep the mirror legible
                wc.m_SSREnabled = false;    // isolate the planar reflection
                wc.m_PlanarReflectionsEnabled = true;
                wc.m_PlanarReflectionIntensity = 1.0f;
                wc.m_PlanarReflectionDistortion = 0.015f;
            }

            // A tall, vivid RED pillar standing well ABOVE the water. Red appears
            // nowhere else in the scene (sky is blue, water teal, sun warm-white),
            // so any red that shows up IN the water can only be its reflection.
            {
                Entity pillar = scene.CreateEntity("RedPillar");
                auto& tc = pillar.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 6.0f, -16.0f };
                tc.Scale = { 3.0f, 12.0f, 3.0f };
                auto& mc = pillar.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = cube->GetMeshSource();
                auto& mat = pillar.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.03f, 0.03f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(0.6f, 0.0f, 0.0f, 1.0f)); // glow so it reads through lighting
            }
        }

        void SetPlanarReflections(bool enabled)
        {
            if (m_WaterEntity)
                m_WaterEntity.GetComponent<WaterComponent>().m_PlanarReflectionsEnabled = enabled;
        }

        // Render the scene from an EditorCamera pose and read back the final
        // composited (post-tone-map) frame, vertically flipped to top-down rows.
        void Capture(const glm::vec3& position, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
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
            ASSERT_TRUE(fb) << "No composited framebuffer";

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
        }

        void SavePng(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("PlanarReflection_" + poseName + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             pixels.data(), static_cast<int>(kWidth) * 4);
        }

        Entity m_WaterEntity;
    };

    // The reflection of the red pillar must appear in the water ONLY when planar
    // reflections are on. Render the identical frozen frame twice and diff the
    // water band in front of the pillar.
    TEST_F(PlanarReflectionVisualEvidenceTest, RedPillarReflectsIntoWaterOnlyWhenEnabled)
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

        // Camera above the water looking forward-down at the pillar — the pillar
        // breaks the top of the frame, its reflection streams down the water below.
        const glm::vec3 pos{ 0.0f, 5.0f, 6.0f };
        const f32 yaw = 0.0f;
        const f32 pitch = 0.32f; // tilt down toward the water + pillar base

        std::vector<u8> on, off;

        SetPlanarReflections(true);
        Capture(pos, yaw, pitch, on);
        if (::testing::Test::HasFatalFailure())
            return;

        SetPlanarReflections(false);
        Capture(pos, yaw, pitch, off);
        if (::testing::Test::HasFatalFailure())
            return;

        // The committed On/Off PNGs are eyeball evidence, refreshed only on an
        // explicit rebase so a normal run leaves no working-tree churn (the
        // differential contract below is the actual CI assertion).
        if (GoldenRebaseRequested())
        {
            SavePng("On", on);
            SavePng("Off", off);
            return;
        }

        // Sample a water band directly below the pillar (centre column, lower-mid
        // of the frame — water, not the pillar itself which is up top). The pillar
        // sits at screen centre; its reflection falls straight below it.
        const u32 x0 = (kWidth * 3u) / 8u; // centre-ish column around the pillar
        const u32 x1 = (kWidth * 5u) / 8u;
        const u32 y0 = (kHeight * 11u) / 20u; // just below the waterline
        const u32 y1 = (kHeight * 17u) / 20u; // foreground water
        const BandMean mOn = MeanOfBand(on, x0, x1, y0, y1);
        const BandMean mOff = MeanOfBand(off, x0, x1, y0, y1);

        // Both frames must render something (not black).
        ASSERT_GT(mOff.R + mOff.G + mOff.B, 6.0) << "reflection-OFF water band rendered near-black";

        // The reflection adds the pillar's red to the water: red rises with the
        // mirror on, and rises MORE than the other channels (a red pillar, not a
        // brightness change). These are the discriminating signals.
        EXPECT_GT(mOn.R, mOff.R + 8.0)
            << "Planar reflection ON did not add the red pillar to the water "
            << "(R on=" << mOn.R << " off=" << mOff.R << "). See PlanarReflection_On/Off.png";
        EXPECT_GT(mOn.R - mOff.R, mOn.B - mOff.B)
            << "Water gained more blue than red — that's sky, not the red pillar "
            << "(dR=" << (mOn.R - mOff.R) << " dB=" << (mOn.B - mOff.B) << ").";
    }
} // namespace OloEngine::Tests
