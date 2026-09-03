// OLO_TEST_LAYER: L8
// =============================================================================
// WaterShoreVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for shore wave deformation — shoaling, refraction and
// breaking against a seabed depth field (issue #1033). Renders a conical island
// sitting in an ocean through the FULL Renderer3D pipeline and writes each pose
// to OloEditor/assets/tests/visual/WaterShore_<pose>.png.
//
// The camera poses are the point, and they are chosen rather than sampled. The
// three effects this issue is about are invisible from directly above:
//
//   * Approach / Grazing — low, near the waterline, looking along and into the
//     beach. This is where shoaling reads: the crest spacing visibly compresses
//     as the water shallows, and the breaker band appears.
//   * ObliqueSwell — the swell set 60 degrees off the beach normal and viewed
//     from high enough to see whole wavefronts. Refraction is the ONLY thing
//     that makes those crests swing round to meet the shoreline square, so this
//     is the pose where a broken Snell's law shows up as crests that stay
//     stubbornly diagonal all the way in.
//   * TopDown — deliberately included as the NEGATIVE control. It is the shot
//     that looks reasonable whether or not any of this works, which is exactly
//     why it must not be the one anybody judges by.
//   * OpenOcean — looking away from the island, out over water the field reports
//     as deep. This has to be indistinguishable from the pre-#1033 sea; it is
//     the visual half of the deep-water-limit contract WaterShoreWaveTest pins
//     numerically.
//
// The seabed is built by hand rather than generated: a cone with a known radius
// and slope, pushed straight into TerrainData. Procedural terrain would make
// every one of these PNGs a hostage to the noise generator, and the shape of the
// seabed is not what is under test — the waves on it are.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// The analytic contracts live in WaterShoreWaveTest.cpp; this file exists
// because CLAUDE.md's rendering rule is that neither one substitutes for the
// other.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Water/WaterShoreDepth.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainData.h"
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

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall clock, for the reason WaterVisualEvidenceTest freezes it:
        // the wave phase comes from Time::GetTime(), so pinning it is what makes
        // a capture reproducible enough to look at twice and compare.
        constexpr f32 kCaptureTime = 12.0f;

        // The island. A cone centred in its tile, breaking the surface, with a
        // long shallow apron — the apron is the whole point, because shoaling
        // happens over the approach and not at the waterline.
        constexpr f32 kTileSize = 400.0f;    // metres
        constexpr u32 kTileRes = 257;        // height-field samples per axis
        constexpr f32 kSeabedBaseY = -24.0f; // matches Drift's island base
        constexpr f32 kHeightScale = 60.0f;  // so the peak stands +36 m
        constexpr f32 kIslandRadius = 0.42f; // as a fraction of the tile half-size

        /// A cone height field, normalised [0, 1], flat at 0 outside the island
        /// radius so the tile border is sea floor exactly like Drift's
        /// island-falloff mask leaves it.
        [[nodiscard]] std::vector<f32> MakeConeHeights()
        {
            std::vector<f32> heights(static_cast<sizet>(kTileRes) * kTileRes, 0.0f);
            for (u32 z = 0; z < kTileRes; ++z)
            {
                for (u32 x = 0; x < kTileRes; ++x)
                {
                    const f32 u = static_cast<f32>(x) / static_cast<f32>(kTileRes - 1) - 0.5f;
                    const f32 v = static_cast<f32>(z) / static_cast<f32>(kTileRes - 1) - 0.5f;
                    const f32 r = std::sqrt(u * u + v * v) / kIslandRadius;
                    heights[static_cast<sizet>(z) * kTileRes + x] =
                        (r >= 1.0f) ? 0.0f : (1.0f - r);
                }
            }
            return heights;
        }
    } // namespace

    class WaterShoreVisualEvidenceTest : public RendererAttachedTest
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
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.55f, -0.75f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
                dl.m_Intensity = 2.5f;
            }

            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.35f;
            }

            // The seabed. A terrain entity's translation is its tile's CORNER
            // (see SeabedTerrain / the Drift island notes), so the cone's axis
            // ends up on the world origin.
            {
                Entity island = scene.CreateEntity("Island");
                auto& tc = island.GetComponent<TransformComponent>();
                tc.Translation = { -kTileSize * 0.5f, kSeabedBaseY, -kTileSize * 0.5f };

                auto& terrain = island.AddComponent<TerrainComponent>();
                terrain.m_WorldSizeX = kTileSize;
                terrain.m_WorldSizeZ = kTileSize;
                terrain.m_HeightScale = kHeightScale;
                terrain.m_ProceduralEnabled = false;
                terrain.m_CollisionEnabled = false;
                // Pushed in directly rather than generated — see the file header
                // for why the shape is authored and not sampled.
                terrain.m_TerrainData = Ref<TerrainData>::Create();
                terrain.m_TerrainData->SetHeights(kTileRes, MakeConeHeights());
            }

            // The sea. Drift's wave configuration, so what these captures show is
            // the sea the game actually runs, not a demo tuned to flatter the
            // feature. The swell heads roughly +X, which puts it 60-odd degrees
            // off the beach normal on the island's north-west shoulder — the
            // ObliqueSwell pose.
            {
                Entity ocean = scene.CreateEntity("Ocean");
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 800.0f;
                wc.m_WorldSizeZ = 800.0f;
                wc.m_GridResolutionX = 320;
                wc.m_GridResolutionZ = 320;
                wc.m_WaveAmplitude = 0.12f;
                wc.m_WaveFrequency = 0.55f;
                wc.m_WaveSpeed = 1.0f;
                wc.m_WaveDir0 = { 1.0f, 0.15f };
                wc.m_WaveSteepness0 = 0.25f;
                wc.m_WaveDir1 = { 0.6f, 0.8f };
                wc.m_WaveSteepness1 = 0.15f;
                wc.m_WaterColor = { 0.09f, 0.33f, 0.44f };
                wc.m_DeepColor = { 0.015f, 0.075f, 0.14f };
                wc.m_Transparency = 0.45f;
                wc.m_Reflectivity = 0.02f;
                wc.m_RenderFromBelow = true;
                wc.m_TessellationEnabled = true;

                // The feature under test.
                wc.m_ShoreWavesEnabled = true;
                // Below the physical 0.39 on purpose, and this is the knob the
                // issue's "visibly break" criterion actually turns. Drift's sea is
                // ~0.35 m crest-to-trough — small for mesh-Nyquist reasons rather
                // than physical ones (see the WaveAmplitude note in Drift.olo) —
                // and a physically-correct breaker index puts its surf zone in the
                // last ~15 cm of water, which is a couple of pixels. Widening the
                // band is an art decision, and it is made HERE rather than by
                // fudging the relation, so the physics stays the physics.
                wc.m_ShoreBreakerIndex = 0.06f;
                wc.m_ShoreFoamGain = 1.0f;
                wc.m_ShoreFoamFadeStart = 150.0f;
                wc.m_ShoreFoamFadeEnd = 420.0f;
            }
        }

        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // Three ticks, not two: the seabed bake runs on the first frame the
            // water is submitted, and the frame that TRIGGERS the bake is drawn
            // with whatever the field held before it. Capturing that frame would
            // photograph the deep-water sea and call it evidence.
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            std::vector<u8> pixels;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            // GL rows come back bottom-up; stbi_write_png treats row 0 as the top.
            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = pixels.data() + static_cast<sizet>(y) * rowBytes;
                    u8* bot = pixels.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("WaterShore_" + poseName + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, pixels.data(),
                                               static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "failed to write '" << path << "'";
        }
    };

    // Evidence, not a golden comparison. These PNGs exist to be LOOKED AT — the
    // question they answer ("do the waves turn and break?") is not one an RMSE
    // against a committed reference can ask, and pinning them as goldens would
    // make every future water change fail here for reasons that have nothing to
    // do with the shore. The numeric contracts are WaterShoreWaveTest's job.
    TEST_F(WaterShoreVisualEvidenceTest, CaptureShoreWavesFromGrazingAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            ScopedMockTime()
            {
                Time::SetMockTime(kCaptureTime);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } mockTime;

        // The island's waterline sits where the cone crosses y = 0: the cone
        // reaches kSeabedBaseY + kHeightScale * (1 - r) = 0 at r = 0.6, i.e. a
        // radius of 0.6 * kIslandRadius * kTileSize = ~101 m. Everything below is
        // positioned against that number rather than eyeballed.
        constexpr f32 kShoreRadius = 0.6f * kIslandRadius * kTileSize;

        // EditorCamera pose convention, worth stating because getting it wrong
        // costs a whole capture run: yaw 0 looks toward -Z and +90 toward +X, and
        // POSITIVE pitch tilts the view DOWN (EditorCamera::Focus says so — the
        // sign is the opposite of what the older water capture's comment claims,
        // and the first run of this file photographed the sky for four of five
        // poses because of it). Every pitch below is positive for that reason.

        // 1. Approach — 60 m off the beach, eye 3 m up, looking straight in.
        //    The breaker band along the whole waterline is what this shot is for.
        Capture("Approach", { kShoreRadius + 60.0f, 3.0f, 0.0f }, glm::radians(-90.0f),
                glm::radians(5.0f));

        // 2. ShoalingProfile — up the approach from 140 m out and 60 m up, so a
        //    whole run of crests is in frame at once, from deep water to the
        //    beach. This is the shot where SHOALING reads: the crest spacing
        //    compresses toward the shore because the phase speed falls with the
        //    depth. On the old sea it is even all the way in.
        Capture("ShoalingProfile", { kShoreRadius + 140.0f, 60.0f, 0.0f }, glm::radians(-90.0f),
                glm::radians(22.0f));

        // 3. Grazing — 5 m above the surface, just outside the break, looking
        //    ALONG the shoreline. The lowest useful eye height: this is the shot
        //    that shows whether the surf band has any vertical relief or is just
        //    a painted stripe.
        Capture("Grazing", { kShoreRadius + 26.0f, 5.0f, -34.0f }, glm::radians(-28.0f),
                glm::radians(7.0f));

        // 4. ObliqueSwell — the refraction shot. High enough to see whole
        //    wavefronts, aimed across the island's north-west shoulder where the
        //    +X swell meets the shore at a steep angle. Refraction is the ONLY
        //    thing that swings those crests round to meet the shoreline square,
        //    so this is the pose where a broken Snell's law shows up as crests
        //    that stay stubbornly diagonal all the way in.
        Capture("ObliqueSwell", { -kShoreRadius - 120.0f, 45.0f, -110.0f }, glm::radians(105.0f),
                glm::radians(16.0f));

        // 5. TopDown — the negative control (see the file header). Kept so the
        //    difference between it and the poses above is on the record.
        Capture("TopDown", { 0.0f, 260.0f, 0.0f }, glm::radians(-90.0f), glm::radians(88.0f));

        // 6. OpenOcean — looking away from the island over water the field
        //    reports as deep. Must read as the pre-#1033 sea.
        Capture("OpenOcean", { 320.0f, 6.0f, 0.0f }, glm::radians(90.0f), glm::radians(4.0f));
    }

    // The bake is what turns a scene's terrain into the field the shader reads,
    // and it runs inside the render tick. If it silently did not run, every PNG
    // above would still be produced and would simply show the old sea — so assert
    // the field exists and reports the island, in the same tick the captures use.
    TEST_F(WaterShoreVisualEvidenceTest, TheSceneTickBakesASeabedThatKnowsAboutTheIsland)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            ScopedMockTime()
            {
                Time::SetMockTime(kCaptureTime);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } mockTime;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                            2000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 200.0f, 20.0f, 0.0f }, glm::radians(-90.0f), glm::radians(-10.0f));
        RunEditorFrames(camera, 3);

        // Just outside the waterline: shallow, and shallowing toward the island.
        const WaterShore::Sample nearShore = WaterShoreDepthSystem::SampleWorld({ 115.0f, 0.0f });
        ASSERT_TRUE(nearShore.Enabled) << "the scene tick never baked a seabed field";
        EXPECT_GT(nearShore.Depth, 0.0f);
        EXPECT_LT(nearShore.Depth, 12.0f) << "the water just off the beach is not shallow";
        // Deeper toward +X, away from the island at the origin.
        EXPECT_GT(nearShore.Gradient.x, 0.0f) << "the seabed does not fall away from the island";

        // Well offshore but still inside the water tile: the field must report
        // the open sea, or the whole ocean would shoal.
        const WaterShore::Sample offshore = WaterShoreDepthSystem::SampleWorld({ 380.0f, 0.0f });
        ASSERT_TRUE(offshore.Enabled);
        EXPECT_FLOAT_EQ(offshore.Depth, WaterShore::kDeepSentinelMetres);
    }
} // namespace OloEngine::Tests
