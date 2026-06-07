// =============================================================================
// UnderwaterCausticsVisualTest.cpp
//
// Visual evidence (PNG) + driver-independent contracts for the underwater
// caustics (WATER_FUTURE_IMPROVEMENTS.md §7.1) and submerged refraction
// distortion (§7.2 bullet 2) added on top of the shipped underwater-fog base.
//
// Both effects live in the tone-map underwater stage (PostProcess_ToneMap.glsl,
// gated on the camera being below the water surface). They're hard to unit-test
// at the pixel level across GPUs, so instead of a brittle golden RMSE this test
// uses A/B differentials that are true on any conformant driver:
//
//   * Caustics: render the seabed from a submerged top-down pose with caustics
//     ON, then again with intensity 0. Caustics are purely ADDITIVE light with a
//     high-frequency spatial pattern, so the ON frame's seabed band must be both
//     BRIGHTER (higher mean) and more TEXTURED (higher variance) than OFF.
//   * Refraction: render a submerged side view of a high-contrast edge with the
//     refraction wobble ON, then with strength 0. The wobble displaces the
//     scene-colour sample, so the two frames must DIFFER (RMSE above noise).
//
// Each pose is also captured to OloEditor/assets/tests/visual/UnderwaterFx_*.png
// for a human / the authoring agent to eyeball (the caustic web + chromatic
// wobble are obvious there). These are evidence, not golden references — the test
// never compares against them, so the write is gated behind OLOENGINE_GOLDEN_REBASE
// to keep the committed PNGs churn-free on normal runs. The cheap CPU math
// contracts (offset bounds, pattern range, depth fade) live in WaterRenderingTest.cpp.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when no GL 4.6 context
// exists — same RendererAttachedTest mechanism as WaterVisualEvidenceTest.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
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

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // Freeze the wave/caustic/refraction clock so each capture is a
        // deterministic frame (the effects animate off Time::GetTime()).
        constexpr f32 kCaptureTime = 8.0f;

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

        // Mean + variance of luma (0..255 units) over a rectangular band of an
        // RGBA8 (row 0 == top) buffer. Variance is the caustic "texture" signal.
        struct BandStats
        {
            f64 Mean = 0.0;
            f64 Variance = 0.0;
        };

        [[nodiscard]] BandStats LumaBandStats(const std::vector<u8>& px, u32 x0, u32 x1, u32 y0, u32 y1)
        {
            f64 sum = 0.0;
            f64 sumSq = 0.0;
            u64 n = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const f64 luma = 0.299 * px[idx + 0] + 0.587 * px[idx + 1] + 0.114 * px[idx + 2];
                    sum += luma;
                    sumSq += luma * luma;
                    ++n;
                }
            }
            BandStats s;
            if (n > 0)
            {
                s.Mean = sum / static_cast<f64>(n);
                s.Variance = sumSq / static_cast<f64>(n) - s.Mean * s.Mean;
                if (s.Variance < 0.0)
                    s.Variance = 0.0; // guard tiny negative from float error
            }
            return s;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        // Mean RGB RMSE (0..255) over a rectangular band of two equal-size buffers.
        [[nodiscard]] f64 BandRmse(const std::vector<u8>& a, const std::vector<u8>& b, u32 x0, u32 x1, u32 y0, u32 y1)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            f64 sumSq = 0.0;
            u64 n = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    for (int c = 0; c < 3; ++c)
                    {
                        const f64 d = static_cast<f64>(a[idx + c]) - static_cast<f64>(b[idx + c]);
                        sumSq += d * d;
                        ++n;
                    }
                }
            }
            return n ? std::sqrt(sumSq / static_cast<f64>(n)) : 0.0;
        }
    } // namespace

    class UnderwaterCausticsVisualTest : public RendererAttachedTest
    {
      protected:
        Entity m_Ocean;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Sun pointing mostly straight down so caustics (faded by the sun's
            // overhead factor) read at full strength.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 30.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.15f, -0.95f, -0.2f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 1.6f;
            }

            // Ocean at y = 0 with caustics + refraction tuned for visibility.
            {
                m_Ocean = scene.CreateEntity("Ocean");
                auto& wc = m_Ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 200.0f;
                wc.m_WorldSizeZ = 200.0f;
                wc.m_GridResolutionX = 96;
                wc.m_GridResolutionZ = 96;
                wc.m_WaveAmplitude = 0.4f;
                wc.m_RenderFromBelow = true;
                wc.m_UnderwaterFogColor = glm::vec3(0.04f, 0.16f, 0.26f);
                wc.m_UnderwaterFogDensity = 0.04f; // light fog so the seabed stays readable
                // Caustics — strong & coarse so the filaments are unmistakable.
                wc.m_CausticsIntensity = 2.0f;
                wc.m_CausticsScale = 0.5f;
                wc.m_CausticsSpeed = 0.6f;
                wc.m_CausticsMaxDepth = 30.0f;
                wc.m_CausticsColor = glm::vec3(0.75f, 0.9f, 1.0f);
                // Refraction — a clearly visible wobble + chromatic split.
                wc.m_UnderwaterRefractionStrength = 0.03f;
                wc.m_UnderwaterRefractionScale = 16.0f;
                wc.m_UnderwaterRefractionSpeed = 1.2f;
                wc.m_UnderwaterChromaticStrength = 0.5f;
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

            // Mid-dark grey seabed 12 m down: flat + uniform so the additive
            // caustic light is the dominant source of brightness/texture, with
            // plenty of headroom before the tone-mapper saturates.
            addPrimitive("Seafloor", MeshPrimitive::Plane, { 0.0f, -12.0f, 0.0f }, { 80.0f, 1.0f, 80.0f },
                         { 0.28f, 0.28f, 0.30f });
            // A pillar breaking the surface — its sharp vertical edges are the
            // high-contrast feature the refraction wobble displaces.
            addPrimitive("Pillar", MeshPrimitive::Cube, { 3.0f, -6.0f, 4.0f }, { 1.2f, 16.0f, 1.2f },
                         { 0.9f, 0.25f, 0.15f });
        }

        // Render one pose through the full editor pipeline and read back the final
        // composited (post tone-map / underwater) frame, flipped so row 0 == top.
        // Always writes a PNG for human inspection.
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

            // Flip vertically (GL origin is bottom-left) so row 0 is the top.
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

            // Refresh the committed evidence PNG only when explicitly rebasing, so
            // a normal GPU run leaves the tracked images untouched (no working-tree
            // churn). The A/B contracts below run every time regardless.
            if (GoldenRebaseRequested())
            {
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string path = (dir / ("UnderwaterFx_" + poseName + ".png")).string();
                ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                 outPixels.data(), static_cast<int>(kWidth) * 4);
            }
        }

        [[nodiscard]] WaterComponent& Water()
        {
            return m_Ocean.GetComponent<WaterComponent>();
        }
    };

    // Caustics add high-frequency ADDITIVE light to upward-facing submerged
    // geometry: ON must be brighter AND more textured than OFF on the seabed.
    TEST_F(UnderwaterCausticsVisualTest, CausticsBrightenAndTextureTheSeabed)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Submerged, looking down at the seabed. Refraction off so the only
        // difference between the two captures is the caustic light.
        const glm::vec3 eye(0.0f, -3.0f, 6.0f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 1.15f; // tilt well down onto the floor

        Water().m_UnderwaterRefractionStrength = 0.0f;

        // Central band of pure seabed (avoid the pillar on the right).
        const u32 x0 = kWidth / 8u;
        const u32 x1 = (kWidth * 7u) / 16u;
        const u32 y0 = kHeight / 3u;
        const u32 y1 = (kHeight * 5u) / 6u;

        Water().m_CausticsIntensity = 2.0f;
        std::vector<u8> on;
        Capture("Caustics_On", eye, yaw, pitch, on);
        if (::testing::Test::HasFatalFailure())
            return;
        const BandStats statsOn = LumaBandStats(on, x0, x1, y0, y1);

        Water().m_CausticsIntensity = 0.0f;
        std::vector<u8> off;
        Capture("Caustics_Off", eye, yaw, pitch, off);
        if (::testing::Test::HasFatalFailure())
            return;
        const BandStats statsOff = LumaBandStats(off, x0, x1, y0, y1);

        // Both frames must actually render the seabed (not black).
        EXPECT_GT(statsOff.Mean, 4.0) << "Seabed band rendered (near-)black with caustics off";

        // Additive light → brighter mean with caustics on.
        EXPECT_GT(statsOn.Mean, statsOff.Mean + 1.0)
            << "Caustics did not brighten the seabed (meanOn=" << statsOn.Mean
            << " meanOff=" << statsOff.Mean << "). See UnderwaterFx_Caustics_On/Off.png";

        // High-frequency pattern → more spatial variance with caustics on.
        EXPECT_GT(statsOn.Variance, statsOff.Variance * 1.15 + 1.0)
            << "Caustics added no visible texture (varOn=" << statsOn.Variance
            << " varOff=" << statsOff.Variance << "). See UnderwaterFx_Caustics_On/Off.png";
    }

    // The submerged refraction wobble displaces the scene-colour sample, so a
    // frame with it ON must differ from the same frame with strength 0.
    TEST_F(UnderwaterCausticsVisualTest, RefractionDistortsTheSubmergedImage)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Submerged, looking across at the pillar's vertical edges. Caustics off
        // so the only difference between captures is the refraction wobble.
        const glm::vec3 eye(0.0f, -3.5f, 14.0f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.06f;

        Water().m_CausticsIntensity = 0.0f;

        Water().m_UnderwaterRefractionStrength = 0.03f;
        Water().m_UnderwaterChromaticStrength = 0.5f;
        std::vector<u8> on;
        Capture("Refraction_On", eye, yaw, pitch, on);
        if (::testing::Test::HasFatalFailure())
            return;

        Water().m_UnderwaterRefractionStrength = 0.0f;
        std::vector<u8> off;
        Capture("Refraction_Off", eye, yaw, pitch, off);
        if (::testing::Test::HasFatalFailure())
            return;

        // Sanity: the frame is not black.
        const BandStats whole = LumaBandStats(off, 0u, kWidth, 0u, kHeight);
        EXPECT_GT(whole.Mean, 4.0) << "Submerged frame rendered (near-)black";

        // The wobble displaces pixels across the full frame; measure over the
        // central region where the pillar edge + seabed contrast live.
        const f64 rmse = BandRmse(on, off, kWidth / 4u, (kWidth * 3u) / 4u, kHeight / 4u, (kHeight * 3u) / 4u);
        EXPECT_GT(rmse, 2.0) << "Refraction wobble produced no visible distortion (RMSE " << rmse
                             << "). See UnderwaterFx_Refraction_On/Off.png";
    }
} // namespace OloEngine::Tests
