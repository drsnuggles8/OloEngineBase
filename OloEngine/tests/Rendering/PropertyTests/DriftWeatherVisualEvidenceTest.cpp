// OLO_TEST_LAYER: L8
// =============================================================================
// DriftWeatherVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for Drift's per-leg weather and time of day (issue
// #882, part of the #878 Drift epic): the three weather states the game
// actually uses — Clear / Overcast / Storm — over an open sea, across the four
// times of day the acceptance criteria name, from a pose close to the game's
// own chase camera. Written to
//     OloEditor/assets/tests/visual/Drift_<Time><Weather>.png
//
// WHY THIS IS A SECOND SET AND NOT A REBAKE OF Atmosphere_*.
//   AtmosphereVisualEvidenceTest already renders {dawn, noon, dusk, night} x
//   {Clear, Overcast, Storm}, and at first glance this is the same grid. It is
//   not the same PICTURE, and the difference is the whole point of #882: that
//   test's subject is a lit ground plane with props, and it exists to pin the
//   #633 atmosphere engine. Drift's subject is WATER — the sea state, the
//   whitecaps, the cloud shadows crossing the surface, the rain on it. None of
//   those appear in a single Atmosphere_* pixel, so rebaking that set would
//   have thrown away the engine-level regression net without gaining any
//   coverage of the thing this issue changes. The Atmosphere_* goldens are
//   therefore UNTOUCHED by this PR
//   (docs/agent-rules/procedural-generator-golden-coupling.md: a rebake is a
//   deliberate act with a stated reason, and there is no reason here).
//
// WHAT THIS TEST DOES AND DOES NOT COVER.
//   It renders the sea-state anchors, it does not derive them. The wind→sea
//   COUPLING — that a Storm leg's 14 m/s wind is what raises the swell, eased
//   rather than cut — lives in the shipped DriftWeatherDirector.lua and is
//   tested end-to-end, through a real Scene tick, by
//   Functional/Atmosphere/DriftLegWeatherAndSeaStateTest. Here the anchors are
//   applied directly per state so each capture is deterministic; keeping the
//   two in step is the reason both quote the same three numbers.
//
// Driver-independent contracts on the captured bands (goldens catch drift;
// these pin the physics, and each one is the frame's answer to one acceptance
// bullet):
//   1. The noon clear sky is bright and blue.
//   2. Night is far darker than noon, on the sky AND the sea band (the two
//      carry different claims — see the assertion for why the sea's threshold
//      is looser and why that is not slack).
//   3. Storm darkens the noon sea (sun dimming + cloud shadow + fog).
//   4. The dusk horizon is warmer (higher R/B) than the noon horizon.
//   5. THE SEA STATE READS. The storm sea band has visibly more contrast than
//      the clear one at the same hour — a built sea is chop and whitecaps, not
//      a flat plane in a different colour. This is the contract that would fail
//      if the weather only ever reached the sky, which is exactly the outcome
//      #882 exists to avoid, and it is measured as spatial variance so it
//      cannot be satisfied by the frame merely getting darker.
//
// Golden model mirrors AtmosphereVisualEvidenceTest: deterministic frame (mock
// time pinned, the clock paused and set per capture), normal runs COMPARE
// (RMSE) against the committed PNGs; --olo-golden-rebase (re)writes them.
// Runs in the normal suite; SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "DriftScenePresets.h"

#include "OloEngine/Atmosphere/WeatherSystem.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall clock: pins the Gerstner phase, the cloud advection and
        // the rain particle stream so every capture is deterministic.
        constexpr f32 kCaptureTime = 100.0f;

        // Matches AtmosphereVisualEvidenceTest. The cloud raymarch's jittered
        // start and the precipitation particles both carry a few grey levels of
        // run-order variance.
        constexpr f64 kGoldenRmseThreshold = 8.0;

        // ── The sea-state anchors ────────────────────────────────────────────
        // The same three sets DriftWeatherDirector.lua interpolates between,
        // which are in turn the calm / moderate / rough sets #879 authored and
        // play-tested by hand. Quoted rather than derived so a change to the
        // curve has to be made in both places DELIBERATELY — the pair is small
        // and the alternative (this test computing the curve itself) would make
        // the golden agree with a bug in the curve.
        struct SeaState
        {
            f32 WaveAmplitude;
            f32 WaveSpeed;
            f32 FoamHeightStart;
            f32 FoamBrightness;
            f32 FoamFadeDistance;
            f32 SpecularIntensity;
            f32 NoiseIntensity;
            glm::vec3 WaterColor;
        };

        const SeaState kSeaCalm{ 0.05f, 0.80f, 0.26f, 0.85f, 0.45f, 1.70f, 0.45f, { 0.075f, 0.360f, 0.480f } };
        const SeaState kSeaModerate{ 0.12f, 1.00f, 0.16f, 1.10f, 0.35f, 1.40f, 0.65f, { 0.090f, 0.330f, 0.440f } };
        const SeaState kSeaRough{ 0.22f, 1.30f, 0.075f, 1.65f, 0.22f, 0.85f, 0.95f, { 0.125f, 0.300f, 0.360f } };

        struct BandStats
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            f64 LumaStdDev = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB and luma standard deviation over a horizontal band
        // [rowBegin, rowEnd) of a TOP-DOWN RGBA8 buffer. The standard deviation
        // is what contract 5 reads: it is a measure of STRUCTURE, so a frame
        // that merely got darker or bluer cannot satisfy it.
        [[nodiscard]] BandStats MeanBand(const std::vector<u8>& pixels, u32 rowBegin, u32 rowEnd)
        {
            BandStats s;
            f64 lumaSum = 0.0;
            f64 lumaSqSum = 0.0;
            u64 count = 0;
            for (u32 y = rowBegin; y < rowEnd; ++y)
            {
                const u8* row = pixels.data() + static_cast<std::size_t>(y) * kWidth * 4u;
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const f64 r = row[x * 4 + 0];
                    const f64 g = row[x * 4 + 1];
                    const f64 b = row[x * 4 + 2];
                    s.R += r;
                    s.G += g;
                    s.B += b;
                    const f64 luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    lumaSum += luma;
                    lumaSqSum += luma * luma;
                    ++count;
                }
            }
            if (count > 0)
            {
                const f64 n = static_cast<f64>(count);
                s.R /= n;
                s.G /= n;
                s.B /= n;
                const f64 mean = lumaSum / n;
                s.LumaStdDev = std::sqrt(std::max(lumaSqSum / n - mean * mean, 0.0));
            }
            return s;
        }

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // Per-vendor baselines, same rules and the same reasons as
        // AtmosphereVisualEvidenceTest::GoldenBaselineDir — the night captures
        // are where procedural-sky float precision diverges most between
        // vendors, and this set has three of them.
        [[nodiscard]] fs::path GoldenBaselineDir()
        {
            fs::path base = fs::path("assets") / "tests" / "visual";
            if (const std::string& vendor = OloEngine::Tests::Options().GoldenVendor; !vendor.empty())
            {
                const std::string_view name(vendor);
                const fs::path vendorPath(name);
                const bool safe = name.find('/') == std::string_view::npos &&
                                  name.find('\\') == std::string_view::npos &&
                                  name != "." && name != ".." &&
                                  !vendorPath.has_root_name() && !vendorPath.has_root_directory();
                if (!safe)
                {
                    ADD_FAILURE() << "--olo-golden-vendor must be a single directory name "
                                     "(no separators, '.', '..', drive letter or root) — got '"
                                  << name << "'";
                    return base;
                }
                base /= name;
            }
            return base;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class DriftWeatherVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // The weather director writes the PROCESS-GLOBAL Renderer3D
        // fog/wind/precipitation/snow settings. RendererAttachedTest's snapshot
        // does not cover those, so without this restore the final Storm state
        // leaks into every later visual test in the suite — the cross-test
        // failure AtmosphereVisualEvidenceTest documents having caused.
        void SetUp() override
        {
            m_SavedFog = Renderer3D::GetFogSettings();
            m_SavedWind = Renderer3D::GetWindSettings();
            m_SavedPrecipitation = Renderer3D::GetPrecipitationSettings();
            m_SavedSnowAccumulation = Renderer3D::GetSnowAccumulationSettings();
            RendererAttachedTest::SetUp();
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            Renderer3D::GetFogSettings() = m_SavedFog;
            Renderer3D::GetWindSettings() = m_SavedWind;
            Renderer3D::GetPrecipitationSettings() = m_SavedPrecipitation;
            Renderer3D::GetSnowAccumulationSettings() = m_SavedSnowAccumulation;
            Renderer3D::SetCloudscapeState(CloudscapeRenderState{});
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Derive the live renderer flags from the settings exactly as the
            // editor does at scene load, so the captures are order-independent
            // (see the same call in AtmosphereVisualEvidenceTest).
            Renderer3D::ApplyRendererSettings();

            // The sun/moon light. Driven by TimeOfDaySystem every frame; these
            // are placeholders. Shadows ON here, unlike Drift.olo — the scene
            // has no thin sail geometry to alias, and the headland's shadow on
            // the water is part of what the captures are for.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.32f, -0.78f, 0.54f));
                dl.m_CastShadows = true;
                dl.m_MaxShadowDistance = 250.0f;
            }

            // The atmosphere, mirroring Drift.olo's Atmosphere entity.
            {
                Entity atmosphere = scene.CreateEntity("Atmosphere");
                auto& sky = atmosphere.AddComponent<ProceduralSkyComponent>();
                sky.m_Turbidity = 2.6f;
                sky.m_SunDiskSize = 1.1f;
                sky.m_IBLIntensity = 0.8f;
                sky.m_CubemapResolution = 128; // 13 rebakes in this test — keep each cheap

                auto& tod = atmosphere.AddComponent<TimeOfDayComponent>();
                tod.m_Paused = true; // captures set the hour explicitly
                tod.m_DayOfYear = 172;
                tod.m_LatitudeDegrees = 34.0f;
                // Drift.olo authors -60, which puts the low sun almost dead
                // ahead of the boat's +Z heading. This capture pose looks down
                // -Z instead (so the fixture's default yaw of 0 faces the
                // headland), so the offset is turned through 180 to put the sun
                // in front of THIS camera. Same art direction, mirrored frame:
                // without it the dawn and dusk cells render as flat grey, with
                // the warm half of the sky behind the camera and nothing bright
                // for the water to reflect — which is exactly how the first
                // bake of this golden set came out.
                tod.m_NorthOffsetDegrees = 120.0f;
                tod.m_SunIntensityMax = 3.6f;
                tod.m_MoonIntensityMax = 0.22f;
                tod.m_MoonPhase = 0.5f;
                tod.m_MoonDiskSize = 1.4f;
                tod.m_StarIntensity = 1.2f;
                tod.m_RebakeQuantumGameMinutes = 0.25f;

                auto& weather = atmosphere.AddComponent<WeatherStateComponent>();
                weather.m_TransitionDuration = 0.0f; // captures snap states
                ApplyDriftWeatherPresets(weather);

                auto& clouds = atmosphere.AddComponent<CloudscapeComponent>();
                clouds.m_LayerBottom = 900.0f;
                clouds.m_LayerTop = 2600.0f;
                clouds.m_ErosionStrength = 0.55f;
                clouds.m_WeatherMapScaleKm = 10.0f;
                clouds.m_CastCloudShadows = true;
                clouds.m_ShadowStrength = 0.8f;
                clouds.m_ShadowMapWorldSize = 6000.0f;
                clouds.m_MaxSteps = 48;
                clouds.m_TemporalBlend = 0.5f; // few warm-up frames per capture

                m_Atmosphere = atmosphere;
            }

            // The sea. Drift.olo runs the Gerstner path (UseFFT false — see the
            // long note there and issue #898), so this does too: capturing the
            // FFT surface would be evidence for a frame the game never draws.
            {
                Entity sea = scene.CreateEntity("Sea");
                auto& wc = sea.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 1000.0f;
                wc.m_WorldSizeZ = 1000.0f;
                wc.m_GridResolutionX = 384;
                wc.m_GridResolutionZ = 384;
                wc.m_WaveFrequency = 0.55f;
                wc.m_WaveDir0 = glm::vec2(1.0f, 0.15f);
                wc.m_WaveSteepness0 = 0.25f;
                wc.m_WaveDir1 = glm::vec2(0.6f, 0.8f);
                wc.m_WaveSteepness1 = 0.15f;
                wc.m_DeepColor = glm::vec3(0.015f, 0.075f, 0.14f);
                wc.m_Transparency = 0.45f;
                wc.m_Reflectivity = 0.85f;
                wc.m_NormalMapTiling = 5.0f;
                wc.m_RefractionDistortion = 0.018f;
                wc.m_RefractionHeightFactor = 0.25f;
                wc.m_UseFFT = false;
                wc.m_RenderFromBelow = true;
                m_Sea = sea;
                ApplySeaState(kSeaModerate);
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
                mat.m_Material.SetRoughnessFactor(0.92f);
            };

            // Seafloor: what stops the sea reading as an infinite void through
            // the transparency, exactly as in Drift.olo.
            addPrimitive("Seafloor", MeshPrimitive::Plane, { 0.0f, -22.0f, 0.0f },
                         { 1200.0f, 1.0f, 1200.0f }, { 0.3f, 0.29f, 0.24f });

            // A headland standing in for Drift's procedural island: the
            // steering target, and the thing the fog acts on. Blocks rather
            // than a TerrainComponent because a procedural heightfield rebuild
            // is not something a 13-capture test should pay for 13 times, and
            // none of the contracts read the island's own surface.
            addPrimitive("Headland", MeshPrimitive::Cube, { -18.0f, 6.0f, -300.0f },
                         { 190.0f, 46.0f, 120.0f }, { 0.44f, 0.42f, 0.36f });
            addPrimitive("Headland Peak", MeshPrimitive::Cube, { 30.0f, 22.0f, -330.0f },
                         { 90.0f, 78.0f, 90.0f }, { 0.4f, 0.39f, 0.35f });

            // Foreground: a boat-shaped stand-in — hull, mast and a sail. Not
            // the real Kenney hull (loading a model here would make this test
            // depend on the asset pipeline for no gain), but three primitives
            // rather than one, because the hero capture below needs a
            // SILHOUETTE. A lone cube gives the sea a scale reference and
            // nothing else; the mast and sail are what make the frame read as
            // a boat at sea rather than a crate on it.
            addPrimitive("Boat Hull", MeshPrimitive::Cube, { -6.5f, 0.1f, -9.0f },
                         { 2.2f, 1.2f, 5.2f }, { 0.62f, 0.34f, 0.22f });
            addPrimitive("Boat Mast", MeshPrimitive::Cube, { -6.5f, 2.6f, -9.0f },
                         { 0.22f, 4.4f, 0.22f }, { 0.45f, 0.3f, 0.2f });
            addPrimitive("Boat Sail", MeshPrimitive::Cube, { -6.5f, 2.6f, -9.3f },
                         { 2.6f, 3.0f, 0.08f }, { 0.86f, 0.84f, 0.78f });
        }

        void ApplySeaState(const SeaState& s)
        {
            auto& wc = m_Sea.GetComponent<WaterComponent>();
            wc.m_WaveAmplitude = s.WaveAmplitude;
            wc.m_WaveSpeed = s.WaveSpeed;
            wc.m_FoamHeightStart = s.FoamHeightStart;
            wc.m_FoamBrightness = s.FoamBrightness;
            wc.m_FoamFadeDistance = s.FoamFadeDistance;
            wc.m_SpecularIntensity = s.SpecularIntensity;
            wc.m_NoiseIntensity = s.NoiseIntensity;
            wc.m_WaterColor = s.WaterColor;
        }

        void Capture(const std::string& name, f32 hours, WeatherStateId weatherState,
                     const SeaState& sea, const glm::vec3& cameraEye = { 0.0f, 4.2f, 6.0f },
                     f32 cameraPitch = -0.06f)
        {
            auto& tod = m_Atmosphere.GetComponent<TimeOfDayComponent>();
            tod.m_TimeOfDayHours = hours;

            auto& weather = m_Atmosphere.GetComponent<WeatherStateComponent>();
            weather.m_CurrentState = weatherState;
            weather.m_TargetState = weatherState;
            weather.m_TransitionProgress = 1.0f;
            weather.m_PrevTargetSeen = weatherState;
            weather.m_BlendedValid = false; // re-settle on the new state
            WeatherSystem::ApplyImmediate(GetScene());

            ApplySeaState(sea);

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.15f, 2500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Yaw 0 looks down -Z, which is where the headland is. The eye sits
            // 4.2 m up — roughly where Drift's chase camera rides — so the
            // horizon lands near the vertical centre and the sea fills the
            // lower half at a grazing angle, which is the angle a sea state is
            // actually legible at.
            camera.SetPose(cameraEye, 0.0f, cameraPitch);

            RunEditorFrames(camera, 4);

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
                RunEditorFrames(camera, 6);
                fb = resolveComposite();
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << name << "'";

            std::vector<u8> pixels;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // Flip to top-down for both the PNG and the band sampling.
            {
                const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                    u8* bot = pixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            // Bands: sky = top 20%, horizon = 42-50%, sea = bottom 40%. The sea
            // band deliberately starts BELOW the horizon line so the headland
            // and the sky cannot contribute to contract 5's variance.
            m_Sky[name] = MeanBand(pixels, 0, kHeight * 20u / 100u);
            m_Horizon[name] = MeanBand(pixels, kHeight * 42u / 100u, kHeight * 50u / 100u);
            m_SeaBand[name] = MeanBand(pixels, kHeight * 60u / 100u, kHeight);

            const fs::path dir = GoldenBaselineDir();
            const std::string path = (dir / ("Drift_" + name + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': "
                                 << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr) << "Missing golden '" << path
                                       << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh
                                     << " — rerun with --olo-golden-rebase.";
            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Capture '" << name << "' drifted from its golden (RMSE " << rmse << ")";
        }

        Entity m_Atmosphere;
        Entity m_Sea;
        std::map<std::string, BandStats> m_Sky;
        std::map<std::string, BandStats> m_Horizon;
        std::map<std::string, BandStats> m_SeaBand;

        FogSettings m_SavedFog;
        WindSettings m_SavedWind;
        PrecipitationSettings m_SavedPrecipitation;
        SnowAccumulationSettings m_SavedSnowAccumulation;
    };

    TEST_F(DriftWeatherVisualEvidenceTest, LegMatrixRendersAndHoldsContracts)
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

        // The four hours the acceptance criteria name, which are also the four
        // legs DriftWeatherDirector.lua schedules.
        // DERIVED from the ephemeris at the latitude/day above, not chosen —
        // and identical to the four leg hours in DriftWeatherDirector.lua:
        //   5.2h -> sun +3.7 deg    12.0h -> +79.4 deg
        //  18.9h -> +2.6 deg        22.6h -> -29.2 deg (night)
        const std::pair<const char*, f32> times[] = {
            { "Dawn", 5.2f }, { "Noon", 12.0f }, { "Dusk", 18.9f }, { "Night", 22.6f }
        };
        // Each weather state carries the sea state its wind produces — the
        // pairing DriftWeatherDirector.lua derives at runtime.
        const struct
        {
            const char* Name;
            WeatherStateId State;
            const SeaState& Sea;
        } weathers[] = {
            { "Clear", WeatherStateId::Clear, kSeaCalm },
            { "Overcast", WeatherStateId::Overcast, kSeaModerate },
            { "Storm", WeatherStateId::Storm, kSeaRough },
        };

        for (const auto& [timeName, hours] : times)
        {
            for (const auto& w : weathers)
            {
                Capture(std::string(timeName) + w.Name, hours, w.State, w.Sea);
                if (::testing::Test::HasFatalFailure())
                    return;
            }
        }

        // The 13th capture: the frame the issue asks for as a store page, and
        // the only cell in this file composed rather than measured.
        //
        // DAWN, not the dusk squall, and that choice is evidence-led. The
        // squall's whole character is a veil, and a veil flattens a photograph
        // — the storm cells above are the right frames for showing what a
        // squall does, and the wrong frame for selling anything. The dawn cell
        // is where this engine's ocean actually shows off: a low sun lays a
        // gold specular track down the swell and the headland goes to
        // silhouette against it.
        //
        // Pose: eye barely above the crests (2.4 m), boat in the near left,
        // headland to the right, sun behind the ridge. A built sea reads as
        // SIZE only from near its own surface; from 4 m up it is texture.
        //
        // The first attempt put the eye at { 26, 2.4, 34 } — far enough that
        // the boat was a matchbox and the storm deck sat in a corner, so the
        // frame had no subject at all. A hero shot of weather still needs
        // something for the weather to be happening TO.
        //
        // No contract reads this cell; the golden pins it.
        Capture("DawnHero", 5.2f, WeatherStateId::Clear, kSeaModerate,
                { -2.5f, 2.4f, 2.0f }, -0.02f);
        if (::testing::Test::HasFatalFailure())
            return;

        // ── Cross-capture physical contracts ──
        // 1. Noon clear sky: bright and blue.
        const BandStats& noonClearSky = m_Sky["NoonClear"];
        EXPECT_GT(noonClearSky.Luma(), 60.0) << "noon clear sky must be bright";
        EXPECT_GE(noonClearSky.B, noonClearSky.R) << "noon clear sky must read blue";

        // 2. Night is far darker than noon — asserted on BOTH bands, because
        // they carry different claims and only one of them is about the sun.
        //
        // The SKY ratio is the physical one and uses the same 0.4 as
        // AtmosphereVisualEvidenceTest.
        EXPECT_LT(m_Sky["NightClear"].Luma(), m_Sky["NoonClear"].Luma() * 0.4)
            << "the night sky must be much darker than the day sky";
        // The SEA ratio is deliberately looser, and the reason is not slack: at
        // this grazing angle the water is mostly a mirror, so the night sea's
        // brightness is set by the night SKY — which TimeOfDayComponent
        // brightens on purpose (m_SkyExposureNight 0.35 against
        // m_SkyExposureDay 0.1) so that stars read at all. A sea that mirrors a
        // deliberately-lifted night sky lands around half of noon, not a
        // fifth. Measured on this scene: 84.5 against 162.8, a ratio of 0.52 —
        // so 0.65 still fails on any regression that stops night from being a
        // distinct time of day, while 0.4 was simply the wrong number for
        // water and failed on a correct frame.
        EXPECT_LT(m_SeaBand["NightClear"].Luma(), m_SeaBand["NoonClear"].Luma() * 0.65)
            << "the night sea must be clearly darker than the noon sea";

        // 3. Storm darkens the noon sea.
        EXPECT_LT(m_SeaBand["NoonStorm"].Luma(), m_SeaBand["NoonClear"].Luma())
            << "a storm must darken the sea at noon";

        // 4. Dusk horizon is warmer than noon horizon.
        const f64 duskWarmth = m_Horizon["DuskClear"].R / std::max(m_Horizon["DuskClear"].B, 1.0);
        const f64 noonWarmth = m_Horizon["NoonClear"].R / std::max(m_Horizon["NoonClear"].B, 1.0);
        EXPECT_GT(duskWarmth, noonWarmth) << "dusk horizon must be warmer than noon";

        // 5. The sea state reads. Chop and whitecaps are STRUCTURE, so the
        // storm sea band must carry more contrast than the clear one at the
        // same hour. Measured as variance rather than mean precisely so that
        // contract 3 (the frame got darker) cannot satisfy this one too.
        EXPECT_GT(m_SeaBand["NoonStorm"].LumaStdDev, m_SeaBand["NoonClear"].LumaStdDev)
            << "the storm sea is not visibly rougher than the clear sea — the "
               "weather reached the sky but not the water (storm stddev "
            << m_SeaBand["NoonStorm"].LumaStdDev << " vs clear "
            << m_SeaBand["NoonClear"].LumaStdDev << ")";
    }
} // namespace OloEngine::Tests
