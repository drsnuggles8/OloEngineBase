// OLO_TEST_LAYER: L8
// =============================================================================
// AtmosphereVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the atmosphere & sky epic (issue #633): the
// time-of-day driven sun/moon + AtmosphereSky bake, the weather director's
// blended fog/precipitation/sun-dimming, and the volumetric cloudscape —
// rendered through the FULL Renderer3D pipeline across the acceptance
// matrix
//     {dawn, noon, dusk, night} x {Clear, Overcast, Storm}
// from a fixed ground-level pose, written to
//     OloEditor/assets/tests/visual/Atmosphere_<Time><Weather>.png
//
// Scene follows docs/agent-rules/single-mesh-visual-test-lighting.md: a
// static ground plane + props so the frame never reads as "subject on
// black", and the PNGs are meant to be OPENED AND LOOKED AT (CLAUDE.md:
// rendering changes MUST be visually verified), not just pass the numbers.
//
// Driver-independent contracts asserted on the captured bands (goldens are
// the regression net; these pin the physics):
//   1. The noon clear sky is bright and blue (B >= R in the sky band).
//   2. The clear night sky is far darker than the clear noon sky.
//   3. Noon storm ground is darker than noon clear ground (sun dimming +
//      cloud shadows attenuate the directional light).
//   4. The dawn horizon is warmer (higher R/B) than the noon horizon.
//
// Golden model mirrors WaterVisualEvidenceTest: deterministic frame (mock
// time pinned; the time-of-day clock is paused — each capture sets the hour
// explicitly), normal runs COMPARE (RMSE) against the committed PNGs; set
// OLOENGINE_GOLDEN_REBASE=1 to (re)write them after a deliberate change.
// Runs in the normal suite; SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

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
#include <cstdlib>
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

        // Frozen wall clock: pins the cloud wind advection + any Time-driven
        // animation so every capture is deterministic (the golden
        // prerequisite). The time-of-day HOUR is a component field set per
        // capture — the clock itself is paused.
        constexpr f32 kCaptureTime = 100.0f;

        // Per-channel RMSE threshold (0..255). Slightly looser than the water
        // test's 6.0: the cloud raymarch keeps a jittered start whose temporal
        // accumulation depends on the global frame counter, contributing a
        // few grey levels of run-order variance in cloudy captures.
        constexpr f64 kGoldenRmseThreshold = 8.0;

        struct BandStats
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB over a horizontal band [rowBegin, rowEnd) of a TOP-DOWN
        // RGBA8 buffer.
        [[nodiscard]] BandStats MeanBand(const std::vector<u8>& pixels, u32 rowBegin, u32 rowEnd)
        {
            BandStats s;
            u64 count = 0;
            for (u32 y = rowBegin; y < rowEnd; ++y)
            {
                const u8* row = pixels.data() + static_cast<std::size_t>(y) * kWidth * 4u;
                for (u32 x = 0; x < kWidth; ++x)
                {
                    s.R += row[x * 4 + 0];
                    s.G += row[x * 4 + 1];
                    s.B += row[x * 4 + 2];
                    ++count;
                }
            }
            if (count > 0)
            {
                s.R /= static_cast<f64>(count);
                s.G /= static_cast<f64>(count);
                s.B /= static_cast<f64>(count);
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

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }
    } // namespace

    class AtmosphereVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // The weather director writes the PROCESS-GLOBAL Renderer3D
        // fog/wind/precipitation/snow settings (that is its job), and the
        // RendererAttachedTest snapshot does NOT cover those structs — the
        // same trap VolumetricFogVisualEvidenceTest documents for FogSettings.
        // Without this restore, the matrix's final Storm state leaked into
        // every later visual test in the suite (storm fog over the water /
        // bloom / planar-reflection goldens — 8 cross-test failures).
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

            // Derive the live renderer flags (depth prepass, Forward+ mode,
            // culling toggles) from the settings, exactly as EditorLayer does
            // at boot/scene-load since issue #534. The pristine test process
            // runs with those flags UN-derived, but any earlier test that
            // calls ApplyRendererSettings (e.g. RendererSettingsBootstrapTest's
            // TearDown) flips them process-wide — and the derived state shifts
            // the textured ground enough to break the night goldens (RMSE ~22
            // on the grid lines; the applied state is also the one that
            // renders them stably instead of z-precision-dashed). Applying
            // here makes the captures order-independent and pins the goldens
            // to the editor-equivalent state.
            Renderer3D::ApplyRendererSettings();

            // The sun/moon light — DRIVEN by the time-of-day system each
            // frame; authored values here are placeholders.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_CastShadows = true;
            }

            // The atmosphere director: procedural sky (AtmosphereSky bake
            // inputs), the clock, the weather state machine and the cloud
            // layer, all on one entity for clarity.
            {
                Entity atmosphere = scene.CreateEntity("Atmosphere");
                auto& sky = atmosphere.AddComponent<ProceduralSkyComponent>();
                sky.m_CubemapResolution = 128; // 12 rebakes in this test — keep each cheap

                auto& tod = atmosphere.AddComponent<TimeOfDayComponent>();
                tod.m_Paused = true; // captures set the hour explicitly
                tod.m_DayOfYear = 80;
                tod.m_LatitudeDegrees = 48.0f;
                tod.m_MoonPhase = 0.5f; // full moon for the night captures
                tod.m_RebakeQuantumGameMinutes = 0.25f;

                auto& weather = atmosphere.AddComponent<WeatherStateComponent>();
                weather.m_TransitionDuration = 0.0f; // captures snap states

                auto& clouds = atmosphere.AddComponent<CloudscapeComponent>();
                // Scales tuned live in the AtmosphereTest editor scene: a low,
                // thin layer with ~2 km weather features keeps visible cloud
                // STRUCTURE (shapes + blue gaps) inside the capture frustum.
                clouds.m_LayerBottom = 700.0f;
                clouds.m_LayerTop = 1600.0f;
                clouds.m_WeatherMapScaleKm = 8.0f;
                clouds.m_MaxSteps = 48;
                clouds.m_TemporalBlend = 0.5f; // few warm-up frames per capture

                m_Atmosphere = atmosphere;
            }

            // Ground + props (single-mesh-visual-test-lighting rule: never a
            // lone subject on nothing).
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

            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                         { 120.0f, 1.0f, 120.0f }, { 0.36f, 0.4f, 0.3f });
            addPrimitive("Pillar", MeshPrimitive::Cube, { -6.0f, 6.0f, -14.0f },
                         { 2.0f, 12.0f, 2.0f }, { 0.75f, 0.72f, 0.68f });
            addPrimitive("Cube", MeshPrimitive::Cube, { 6.0f, 1.5f, -10.0f },
                         { 3.0f, 3.0f, 3.0f }, { 0.7f, 0.35f, 0.25f });
        }

        // Set the matrix cell, render, read back (top-down), save/compare the
        // PNG, and record the band stats for the cross-capture contracts. The
        // default pose is the ground-level horizon view every matrix cell
        // uses; the aerial capture below passes its own (positive pitch tilts
        // the view down, per EditorCamera).
        void Capture(const std::string& name, f32 hours, WeatherStateId weatherState,
                     const glm::vec3& cameraEye = { 0.0f, 3.0f, 14.0f }, f32 cameraPitch = -0.08f)
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

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Ground-level pose looking over the props toward the horizon —
            // sky (with clouds) fills the top half, lit ground the bottom.
            camera.SetPose(cameraEye, 0.0f, cameraPitch);

            // Several ticks: the sky rebake happens on the first, the cloud
            // temporal accumulation settles over the rest.
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
                // A preceding GPU test can leave the pipeline mid-reconfigure
                // (path switch / topology rebuild); the graph then needs an
                // extra frame or two before the composite chain resolves.
                // Settle more and retry once before declaring failure.
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

            // Bands: sky = top 18%, horizon = 38-46%, ground = bottom 25%.
            m_Sky[name] = MeanBand(pixels, 0, kHeight * 18u / 100u);
            m_Horizon[name] = MeanBand(pixels, kHeight * 38u / 100u, kHeight * 46u / 100u);
            m_Ground[name] = MeanBand(pixels, kHeight * 75u / 100u, kHeight);

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("Atmosphere_" + name + ".png")).string();

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
                                       << "' — rerun with OLOENGINE_GOLDEN_REBASE=1 to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh
                                     << " — rerun with OLOENGINE_GOLDEN_REBASE=1.";
            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Capture '" << name << "' drifted from its golden (RMSE " << rmse << ")";
        }

        Entity m_Atmosphere;
        std::map<std::string, BandStats> m_Sky;
        std::map<std::string, BandStats> m_Horizon;
        std::map<std::string, BandStats> m_Ground;

        FogSettings m_SavedFog;
        WindSettings m_SavedWind;
        PrecipitationSettings m_SavedPrecipitation;
        SnowAccumulationSettings m_SavedSnowAccumulation;
    };

    TEST_F(AtmosphereVisualEvidenceTest, DayNightWeatherMatrixRendersAndHoldsContracts)
    {
        // Pin the wall clock so wind advection / animated noise are identical
        // every run (mirrors WaterVisualEvidenceTest's mock-time scope).
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

        const std::pair<const char*, f32> times[] = {
            { "Dawn", 7.0f }, { "Noon", 12.0f }, { "Dusk", 17.5f }, { "Night", 0.0f }
        };
        const std::pair<const char*, WeatherStateId> weathers[] = {
            { "Clear", WeatherStateId::Clear },
            { "Overcast", WeatherStateId::Overcast },
            { "Storm", WeatherStateId::Storm },
        };

        for (const auto& [timeName, hours] : times)
        {
            for (const auto& [weatherName, state] : weathers)
            {
                Capture(std::string(timeName) + weatherName, hours, state);
                if (::testing::Test::HasFatalFailure())
                    return;
            }
        }

        // 13th capture — a substantially different angle, per the repo's
        // multi-angle visual-verification rule: an elevated vantage over the
        // props pitched ~22° down (the 120 m ground plane fills the lower
        // half; the top-of-frame ray still clears the horizon, keeping the
        // 700 m cloud deck in the top band). Exposes what the horizon pose
        // can't: the props/ground depth-compositing seen from above, the
        // scattered noon cumulus casting ground shadows, and the cloud
        // field's structure from a second vantage. First attempt used
        // { 0, 380, 60 } pitch 0.85 — at 60° FOV that puts the WHOLE frustum
        // below the horizon and shrinks the plane to a sliver; the frame was
        // a featureless haze. No named contract reads this cell; the golden
        // pins it.
        Capture("NoonClearAerial", 12.0f, WeatherStateId::Clear,
                { 0.0f, 45.0f, 55.0f }, 0.38f);
        if (::testing::Test::HasFatalFailure())
            return;

        // ── Cross-capture physical contracts ──
        // 1. Noon clear sky: bright and blue.
        const BandStats& noonClearSky = m_Sky["NoonClear"];
        EXPECT_GT(noonClearSky.Luma(), 60.0) << "noon clear sky must be bright";
        EXPECT_GE(noonClearSky.B, noonClearSky.R) << "noon clear sky must read blue";

        // 2. Clear night sky is far darker than clear noon.
        EXPECT_LT(m_Sky["NightClear"].Luma(), noonClearSky.Luma() * 0.4)
            << "night sky must be much darker than day";

        // 3. Storm dims the noon ground (sun dimming + cloud shadows).
        EXPECT_LT(m_Ground["NoonStorm"].Luma(), m_Ground["NoonClear"].Luma())
            << "storm must darken the ground at noon";

        // 4. Dawn horizon is warmer than noon horizon.
        const f64 dawnWarmth = m_Horizon["DawnClear"].R / std::max(m_Horizon["DawnClear"].B, 1.0);
        const f64 noonWarmth = m_Horizon["NoonClear"].R / std::max(m_Horizon["NoonClear"].B, 1.0);
        EXPECT_GT(dawnWarmth, noonWarmth) << "dawn horizon must be warmer than noon";
    }
} // namespace OloEngine::Tests
