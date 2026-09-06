// OLO_TEST_LAYER: L8
// =============================================================================
// VolumetricShadowVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for issue #723's volumetric shadow map — media that
// shadow THEMSELVES — rendered through the FULL Renderer3D pipeline and written
// to OloEditor/assets/tests/visual/VolumetricShadow_*.png. The PNGs are meant
// to be OPENED AND LOOKED AT (CLAUDE.md: rendering changes MUST be visually
// verified); the contracts below are what a headless run can still prove.
//
// EVIDENCE, NOT GOLDENS, on purpose. Every capture here is an A/B pair — the
// same frame with self-shadowing off and on — so the assertions are
// DIFFERENTIALS. A differential cancels the driver, the exposure, the phase
// function and the sun colour, none of which a fixed-image RMSE can separate
// from the effect under test. It also adds no new golden that could flicker,
// which is the acceptance criterion's other half.
//
//   1. CLOUD SELF-SHADOWING TRACKS THE SUN. Captured at four sun elevations
//      from two angles. The machine-checkable part is that the differential
//      GROWS BY AN ORDER OF MAGNITUDE as the sun sinks: at a high sun the
//      light path leaves the layer inside the raymarch's own 1400 m cone, so
//      the map has nothing to add, and at a low sun the path is many times
//      that and the map supplies all of it. A uniform veil — an absorption
//      floor integrated along the light path, a constant optical depth, a
//      strength knob applied without a transform — sits at a ratio of 1x and
//      fails, while still passing "the frame got darker".
//
//   2. A DENSE FOG VOLUME OCCLUDES LIGHT ALONG ITS OWN DEPTH. A near-horizontal
//      sun crosses a long fog volume, so the light path runs ACROSS the screen
//      while the view ray runs into it — which separates self-shadowing from
//      the view-ray extinction the fog already had. Enabling it must cost the
//      far (shadowed) end more brightness than the near (sunward) end.
//
// Both fixtures snapshot and restore the process-global FogSettings, the
// RendererSettings and the cloudscape render state: RendererAttachedTest's
// snapshot does NOT cover them, and a leaked dense fog / storm deck breaks
// unrelated goldens later in the suite
// (docs/agent-rules/volumetric-cloud-debugging.md, fifth cause). RendererSettings
// was the same trap one level up — the fog-volume fixture switches Path to
// ForwardPlus, and leaking that left ~100 later visual-evidence tests rendering
// down a pipeline where the feature they assert on does not run at all.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <stb_image/stb_image_write.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // 800x600, not the 1024x768 the sibling evidence fixtures use: this
        // test writes NINETEEN frames (four sun elevations x two poses x an
        // A/B pair, plus the fog set), and they are committed as the evidence
        // for issue #723's third acceptance criterion. At 1024x768 that is
        // 12 MB of PNG in one change; the contracts are band means, which do
        // not care, and the swing is just as legible smaller.
        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 600;

        // Frozen wall clock: pins wind advection and animated fog noise so an
        // A/B pair differs ONLY by the setting under test.
        constexpr f32 kCaptureTime = 100.0f;

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;
            ScopedMockTime(ScopedMockTime&&) = delete;
            ScopedMockTime& operator=(ScopedMockTime&&) = delete;
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        };

        struct Band
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB over a UV rect of a TOP-DOWN RGBA8 buffer.
        Band SampleBand(const std::vector<u8>& pixels, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const auto ix0 = static_cast<u32>(x0 * static_cast<f32>(kWidth));
            const auto ix1 = static_cast<u32>(x1 * static_cast<f32>(kWidth));
            const auto iy0 = static_cast<u32>(y0 * static_cast<f32>(kHeight));
            const auto iy1 = static_cast<u32>(y1 * static_cast<f32>(kHeight));
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= pixels.size())
                        continue;
                    sumR += pixels[idx + 0];
                    sumG += pixels[idx + 1];
                    sumB += pixels[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / static_cast<f64>(count),
                     static_cast<f64>(sumG) / static_cast<f64>(count),
                     static_cast<f64>(sumB) / static_cast<f64>(count) };
        }
    } // namespace

    // =========================================================================
    // Shared capture plumbing
    // =========================================================================
    class VolumetricShadowCaptureFixture : public RendererAttachedTest
    {
      protected:
        void SetUp() override
        {
            m_SavedFog = Renderer3D::GetFogSettings();
            m_SavedSettings = Renderer3D::GetRendererSettings();
            RendererAttachedTest::SetUp();
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            Renderer3D::GetFogSettings() = m_SavedFog;
            // RendererSettings is process-global too, and these fixtures write
            // it — the fog-volume one switches Path to ForwardPlus for its own
            // experiment. Restoring it is not optional in a single-process
            // suite: a leaked Path left every later visual-evidence test
            // rendering down the wrong pipeline, where the feature under test
            // is simply absent and its on/off captures come out identical.
            Renderer3D::GetRendererSettings() = m_SavedSettings;
            Renderer3D::ApplyRendererSettings();
            Renderer3D::SetCloudscapeState(CloudscapeRenderState{});
        }

        // Render from an explicitly posed editor camera, read back top-down,
        // and write the PNG. Returns the pixels for band sampling.
        void Capture(const std::string& tag, const glm::vec3& eye, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 8000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);

            // Several ticks: the cloud/froxel temporal resolves settle over
            // them. The shadow VOLUME itself needs none — it has no history —
            // but its CONSUMERS do.
            RunEditorFrames(camera, 5);

            auto resolveComposite = []
            {
                auto resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
                if (!resolved)
                    resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
                if (!resolved)
                    resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                return resolved;
            };
            auto fb = resolveComposite();
            if (!fb)
            {
                // A preceding GPU test can leave the pipeline mid-reconfigure;
                // the graph then needs a few more frames (the
                // AtmosphereVisualEvidenceTest retry).
                RunEditorFrames(camera, 6);
                fb = resolveComposite();
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("VolumetricShadow_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        FogSettings m_SavedFog{};
        RendererSettings m_SavedSettings{};
    };

    // =========================================================================
    // 1. Clouds — directional self-shadowing that tracks the sun
    //
    // THE SCENE IS AUTHORED TO ISOLATE THE TERM UNDER TEST, and that is not a
    // convenience — it is the difference between a test that measures the
    // feature and one that measures the cloudscape's shading balance. Both
    // knobs below were derived by sweeping the LIVE editor over MCP:
    //
    //   * AmbientScale is turned DOWN. Self-shadowing modulates the SUN term
    //     only (the ambient term is the sky estimate, which a shadow map fitted
    //     to the sun direction says nothing about). At the stock AmbientScale
    //     of 1.0 the ambient term dominates the composite and the same enabled/
    //     disabled pair differs by 0.3%; at 0.05 it differs by up to 33%. The
    //     shadowing is identical in both — only its visibility in the pixel
    //     changes.
    //   * Density is MODERATE, not heavy. At the stock 1.0-1.6 the raymarch's
    //     own 1400 m light cone already drives the primary Beer term to ~e^-21
    //     before the shadow map contributes anything, so a dense deck is the one
    //     regime where self-shadowing provably cannot change a pixel. The first
    //     version of this test used Density 1.6 and measured 0.4 grey levels.
    //
    // The contract is that the effect TRACKS THE SUN, which is criterion 1 of
    // issue #723 stated as something a headless run can prove: the light path
    // through the layer is short at a high sun (the cone covers all of it, the
    // map has nothing to add) and long at a low one (the map supplies what the
    // cone cannot reach), so the differential must grow by orders of magnitude
    // as the sun sinks. Live measurement across the same sweep, deck underside:
    // 0.16% at 60 degrees, 0.21% at 30, 17.0% at 12, 32.9% at 5.
    // =========================================================================
    class CloudSelfShadowVisualEvidenceTest : public VolumetricShadowCaptureFixture
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            auto& settings = Renderer3D::GetRendererSettings();
            settings.ShowGrid = false;
            settings.ShowLightGizmos = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            Renderer3D::ApplyRendererSettings();

            // Fog off: this fixture is about the CLOUD cascade only, and fog
            // in-scatter over a kilometre-scale sightline would swamp it.
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = false;
            fog.EnableVolumetric = false;

            // The sun is driven by this light's direction, set per capture.
            // No TimeOfDayComponent on purpose: the clock would overwrite the
            // direction every tick, and elevation is what the contract is about
            // — an hour is just a slower way to say the same thing.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-1.0f, -0.5f, 0.0f));
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = false; // isolate MEDIUM occlusion from geometry shadows
                m_Sun = light;
            }

            {
                Entity atmosphere = scene.CreateEntity("Cloudscape");
                // NO WeatherStateComponent, deliberately: the weather director
                // writes the process-global fog/wind/precipitation/snow
                // settings, and that leak has broken eight unrelated goldens
                // once already (volumetric-cloud-debugging.md, fifth cause).
                auto& clouds = atmosphere.AddComponent<CloudscapeComponent>();
                clouds.m_LayerBottom = 700.0f;
                clouds.m_LayerTop = 2600.0f;
                clouds.m_Coverage = 0.55f;
                clouds.m_Density = 0.4f;    // see the header: NOT a heavy deck
                clouds.m_TypeBlend = 0.85f; // cumulus: mass low in the layer
                clouds.m_WeatherMapScaleKm = 8.0f;
                clouds.m_MaxSteps = 48;
                clouds.m_AmbientScale = 0.05f; // see the header: isolate the sun term
                clouds.m_SunLightScale = 2.5f;
                clouds.m_TemporalBlend = 0.5f;         // few warm-up frames per capture
                clouds.m_CastCloudShadows = false;     // isolate SELF-shadowing from ground shadows
                clouds.m_VolumetricSelfShadow = false; // the "off" leg; toggled per capture

                m_Atmosphere = atmosphere;
            }

            // Ground + a prop so no frame reads as "subject on black"
            // (single-mesh-visual-test-lighting.md).
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
            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 240.0f, 1.0f, 240.0f },
                         { 0.36f, 0.4f, 0.3f });
            addPrimitive("Pillar", MeshPrimitive::Cube, { -8.0f, 8.0f, -22.0f }, { 3.0f, 16.0f, 3.0f },
                         { 0.75f, 0.72f, 0.68f });
        }

        // Point the sun at `elevationDegrees` above the horizon, in the +X half
        // of the sky, by setting the direction it TRAVELS.
        void SetSunElevation(f32 elevationDegrees)
        {
            const f32 elevation = glm::radians(elevationDegrees);
            const glm::vec3 towardSun(std::cos(elevation), std::sin(elevation), 0.0f);
            m_Sun.GetComponent<DirectionalLightComponent>().m_Direction = -towardSun;
        }

        void SetSelfShadow(bool enabled)
        {
            m_Atmosphere.GetComponent<CloudscapeComponent>().m_VolumetricSelfShadow = enabled;
        }

        Entity m_Atmosphere;
        Entity m_Sun;
    };

    TEST_F(CloudSelfShadowVisualEvidenceTest, DeckSelfShadowingTracksTheSunAcrossADayCycle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);

        // Two poses per elevation. UNDERSIDE looks steeply up at the deck's
        // base, which is where "dark underneath" lives; TOWARDSUN looks along
        // the light, where the lit faces are. Both are captured at every
        // elevation so the PNG set shows the swing, and both feed the contract.
        const glm::vec3 kUndersideEye(0.0f, 6.0f, 40.0f);
        constexpr f32 kUndersidePitch = -0.85f; // negative pitch tilts UP (EditorCamera, radians)
        const glm::vec3 kTowardSunEye(-900.0f, 300.0f, 0.0f);
        constexpr f32 kTowardSunYaw = 1.5708f; // +X, toward the sun
        constexpr f32 kTowardSunPitch = -0.35f;

        struct SunCase
        {
            const char* Name;
            f32 Elevation;
        };
        const std::array<SunCase, 4> suns{ {
            { "High60", 60.0f },
            { "Mid30", 30.0f },
            { "Low12", 12.0f },
            { "Dusk05", 5.0f },
        } };

        std::array<f64, suns.size()> undersideDeltas{};
        std::array<f64, suns.size()> towardSunDeltas{};

        for (u32 i = 0; i < suns.size(); ++i)
        {
            const SunCase& sun = suns[i];
            SetSunElevation(sun.Elevation);

            std::vector<u8> undersideOff, towardSunOff, undersideOn, towardSunOn;

            SetSelfShadow(false);
            Capture(std::string("Clouds_Underside_") + sun.Name + "_Off", kUndersideEye, 0.0f, kUndersidePitch,
                    undersideOff);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            Capture(std::string("Clouds_TowardSun_") + sun.Name + "_Off", kTowardSunEye, kTowardSunYaw,
                    kTowardSunPitch, towardSunOff);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            SetSelfShadow(true);
            Capture(std::string("Clouds_Underside_") + sun.Name + "_On", kUndersideEye, 0.0f, kUndersidePitch,
                    undersideOn);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            Capture(std::string("Clouds_TowardSun_") + sun.Name + "_On", kTowardSunEye, kTowardSunYaw,
                    kTowardSunPitch, towardSunOn);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            // Sky band only: the ground fills the bottom of the underside frame
            // and must not dilute the measurement.
            undersideDeltas[i] = SampleBand(undersideOff, 0.15f, 0.85f, 0.03f, 0.50f).Luma() -
                                 SampleBand(undersideOn, 0.15f, 0.85f, 0.03f, 0.50f).Luma();
            towardSunDeltas[i] = SampleBand(towardSunOff, 0.15f, 0.85f, 0.10f, 0.70f).Luma() -
                                 SampleBand(towardSunOn, 0.15f, 0.85f, 0.10f, 0.70f).Luma();

            // Optical depth only accumulates, so the transmittance the cascade
            // applies is <= 1 everywhere: enabling it can never ADD light.
            EXPECT_GE(undersideDeltas[i], -0.5) << "underside brightened at " << sun.Name;
            EXPECT_GE(towardSunDeltas[i], -0.5) << "sun side brightened at " << sun.Name;
        }

        const f64 highSun = undersideDeltas[0]; // 60 degrees
        const f64 duskSun = undersideDeltas[3]; // 5 degrees

        // 1. THE EFFECT EXISTS at a low sun. Measured here at ~7 grey levels;
        // 3 is a floor with better than 2x headroom, chosen so a driver
        // difference cannot turn a working map into a red.
        EXPECT_GT(duskSun, 3.0)
            << "no cloud self-shadowing at a 5-degree sun (delta " << duskSun << ") — the cloud cascade "
                                                                                 "never ran, or its optical depth is arriving as zero";

        // 2. IT TRACKS THE SUN — the assertion that separates a working shadow
        // map from a constant darkening, and criterion 1 of the issue stated as
        // something a headless run can prove. At a high sun the light path
        // leaves the layer within the raymarch's own 1400 m cone, so the map has
        // nothing left to contribute and the differential nearly vanishes; at a
        // low sun the path is many times the cone's reach and the map supplies
        // all of it. Measured ratio ~9x here (0.78 -> 7.11 grey levels) and
        // ~175x in the live editor with the same sweep; 3x is the floor, where
        // a veil would sit at 1x.
        // FLOORED AT ZERO, because a ratio against a negative baseline is not a
        // test. highSun is a measured difference and may land slightly below 0
        // (the -0.5 tolerance above allows it); `duskSun > highSun * 3.0` would
        // then be satisfied by ANY positive dusk value — including one far too
        // small to mean the map ran. Clamping makes the assertion "at least 3x
        // the high-sun differential, and never less than the absolute floor".
        const f64 highSunFloor = std::max(highSun, 1.0);
        EXPECT_GT(duskSun, highSunFloor * 3.0)
            << "the differential did not grow as the sun sank (60 deg: " << highSun << ", 5 deg: " << duskSun
            << ") — a shadow that does not track the light is a veil, not occlusion";

        // 3. IT SURVIVES A SECOND CAMERA AND A SECOND LOW SUN, so neither the
        // effect nor its magnitude is an artefact of one lucky pose.
        //
        // ⚠ NOT "the underside darkens more than the sun side". That was the
        // first version of this assertion and it is WRONG at a low sun, which
        // is exactly where the effect lives: a camera pointed at a 5-degree sun
        // is looking ALONG the light path, through the greatest thickness of
        // medium there is — the most shadowed direction in the frame, not the
        // least. The lit faces are the ones you see with the sun BEHIND the
        // camera. Measured: sun side 9.43 vs underside 6.48, i.e. the reverse
        // of the naive expectation, from a correct implementation.
        EXPECT_GT(towardSunDeltas[3], 3.0)
            << "the sun-facing view showed no self-shadowing (delta " << towardSunDeltas[3]
            << ") — the effect should be visible from every angle that sees the deck";
        EXPECT_GT(undersideDeltas[2], 1.0)
            << "the 12-degree sun showed no self-shadowing (delta " << undersideDeltas[2]
            << ") — a single working elevation is a coincidence, two is a trend";
    }
    // =========================================================================
    // 2. Fog volume — occlusion along the volume's own depth
    // =========================================================================
    class FogVolumeSelfShadowVisualEvidenceTest : public VolumetricShadowCaptureFixture
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::ForwardPlus;
            settings.ShowGrid = false;
            settings.ShowLightGizmos = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            Renderer3D::ApplyRendererSettings();

            // A NEAR-HORIZONTAL sun travelling along -X. That is the whole
            // experiment design: the light path runs ACROSS the frame while the
            // view ray runs INTO it, so the fog's pre-existing view-ray
            // extinction (which is symmetric left-to-right) cannot fake the
            // signal. The +X end of the volume is sunward; the -X end is the
            // one that must go dark.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-1.0f, -0.22f, 0.0f));
                dl.m_Intensity = 4.0f;
                dl.m_CastShadows = false; // isolate MEDIUM occlusion from geometry shadows
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
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            };
            addPrimitive("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 160.0f, 1.0f, 160.0f },
                         { 0.12f, 0.12f, 0.13f });

            // One long, dense fog volume laid out along X, so "along its own
            // depth" is measurable left-to-right in the frame.
            {
                Entity volume = scene.CreateEntity("DenseFogVolume");
                auto& tc = volume.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 6.0f, -18.0f };
                auto& fv = volume.AddComponent<FogVolumeComponent>();
                fv.m_Shape = FogVolumeShape::Box;
                // ~52 m along the light, which is what the measurement reads.
                fv.m_Extents = { 26.0f, 6.0f, 10.0f };
                fv.m_Color = { 0.72f, 0.74f, 0.78f };
                // Dense enough to be optically thick along the VIEW ray (0.06/m
                // over the volume's 20 m depth) but not so dense that the light
                // path SATURATES: at 0.9/m — the first value tried — the
                // sunward end already sat at exp(-3.6) and the far end at
                // exp(-58), so both were black and the differential this test
                // measures collapsed into the noise floor. A graded medium is
                // what makes "occludes along its own depth" observable at all.
                fv.m_Density = 0.06f;
                fv.m_FalloffDistance = 3.0f;
                fv.m_BlendWeight = 1.0f;
                fv.m_Enabled = true;
            }

            // Froxel fog with sun scattering on — that is the term the fog
            // cascade attenuates. Global height fog kept minimal so the VOLUME
            // dominates the measurement. Noise off for determinism.
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.EnableVolumetric = true;
            fog.Density = 0.002f;
            fog.Start = 0.0f;
            fog.End = 160.0f;
            fog.HeightFalloff = 0.05f;
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 0.97f;
            fog.Color = { 0.42f, 0.45f, 0.5f };
            fog.EnableScattering = true;
            fog.SunIntensity = 30.0f;
            fog.MieStrength = 0.02f;
            fog.MieDirectionality = 0.4f;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
            fog.AbsorptionCoefficient = 0.01f;
            fog.EnableVolumetricSelfShadow = false; // the "off" leg; toggled per capture
            fog.VolumetricSelfShadowExtent = 220.0f;
        }
    };

    TEST_F(FogVolumeSelfShadowVisualEvidenceTest, DenseVolumeOccludesLightAlongItsOwnDepth)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);

        // Looking down -Z at the volume broadside, slightly above it.
        const glm::vec3 eye(0.0f, 7.0f, 26.0f);

        std::vector<u8> off;
        std::vector<u8> on;
        Renderer3D::GetFogSettings().EnableVolumetricSelfShadow = false;
        Capture("Fog_Broadside_Off", eye, 0.0f, 0.02f, off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        Renderer3D::GetFogSettings().EnableVolumetricSelfShadow = true;
        Capture("Fog_Broadside_On", eye, 0.0f, 0.02f, on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // A second angle, per the multi-angle rule: three-quarter view from the
        // sunward side, where the lit face is nearest the camera and the
        // shadowed interior recedes.
        std::vector<u8> obliqueOn;
        Capture("Fog_Oblique_On", { 34.0f, 9.0f, 22.0f }, -0.85f, 0.06f, obliqueOn);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // The sun travels along -X, so screen-right (+X, the frame is not
        // mirrored at this yaw) is the SUNWARD end and screen-left is the far
        // end of the light path. Sample the volume's own band, above the floor.
        const f64 sunwardOff = SampleBand(off, 0.68f, 0.94f, 0.30f, 0.55f).Luma();
        const f64 sunwardOn = SampleBand(on, 0.68f, 0.94f, 0.30f, 0.55f).Luma();
        const f64 shadowedOff = SampleBand(off, 0.06f, 0.32f, 0.30f, 0.55f).Luma();
        const f64 shadowedOn = SampleBand(on, 0.06f, 0.32f, 0.30f, 0.55f).Luma();

        const f64 sunwardDrop = sunwardOff - sunwardOn;
        const f64 shadowedDrop = shadowedOff - shadowedOn;

        // The effect exists at all.
        EXPECT_GT(shadowedDrop, 1.0)
            << "the far end of the fog volume did not darken — the fog cascade never ran, or its "
               "optical depth is arriving as zero";

        // ...and it is DIRECTIONAL. This is the assertion that separates real
        // self-occlusion from every look-alike: a uniform veil (an absorption
        // floor integrated over the light path, a constant optical depth, a
        // strength knob applied without a transform) darkens both ends
        // equally and fails here.
        EXPECT_GT(shadowedDrop, sunwardDrop + 0.75)
            << "the fog volume darkened uniformly (sunward " << sunwardDrop << ", shadowed " << shadowedDrop
            << ") — that is a veil, not occlusion along the volume's depth";

        // Self-shadowing may never brighten the medium.
        EXPECT_GE(sunwardDrop, -1.0) << "the sunward end brightened — the cascade is adding light";
    }
} // namespace OloEngine::Tests
