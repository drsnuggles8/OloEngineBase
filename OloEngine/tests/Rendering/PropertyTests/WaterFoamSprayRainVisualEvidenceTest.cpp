// =============================================================================
// WaterFoamSprayRainVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the three halves of issue #1034 — advected foam
// (§2.2), crest spray (§2.3) and rain-impact ripples (§7.3) — rendered through
// the FULL Renderer3D pipeline over a built FFT ocean, written to
//   OloEditor/assets/tests/visual/WaterFoamSprayRain_<pose>.png
//
// WHY A SEQUENCE AND NOT A SINGLE FRAME
// -------------------------------------
// "Whitecaps drift with the surface rather than pulsing in place" is a
// MULTI-FRAME claim, and a still frame cannot show it: advected foam and
// instantaneous Jacobian foam both render as white patches on crests. So the
// drift poses are a numbered sequence from ONE fixed camera at fixed clock
// intervals, meant to be flipped through, and the drift itself is pinned
// numerically and headlessly in WaterFoamAdvectionTest (the patch's centre of
// mass travels at the advecting velocity, negative-controlled against a zero
// velocity). A golden image cannot make that assertion; a golden image is here
// so a human can see the thing the numbers describe.
//
// WHAT IS ASSERTED FROM THE READBACK
// ----------------------------------
// Beyond the goldens — and this is the part that fails on a real regression
// rather than on a driver difference:
//
//   1. ADVECTED FOAM COVERS MORE WATER THAN INSTANTANEOUS FOAM does, from the
//      same camera at the same instant on the same sea. That is the direct
//      consequence of foam PERSISTING after the crest that made it has passed,
//      and it is false for any implementation that merely re-colours the same
//      folding crests;
//   2. SPRAY EMITS ON A BUILT SEA AND NOT ON A CALM ONE, read from the emitter's
//      own counter through the live pipeline rather than from a substituted
//      sampler — the headless test already covers the criterion, this covers
//      the wiring;
//   3. RAIN VISIBLY CHANGES THE FRAME WHILE IT IS RAINING, and the ripple field
//      reports EXACTLY disabled when it is not. The second half is asserted on
//      the gate value rather than on the frame deliberately: "no cost when rain
//      is off" is about the shader returning BEFORE its cell walk, which is a
//      statement about `params.x <= 0` and not one a pixel tolerance can make.
//
// DETERMINISM
// -----------
// Time::SetMockTime freezes the wave phase, the field decay AND the spray
// emitter's time bucket, so every capture reproduces exactly. See
// WaterWakeVisualEvidenceTest's header for why the clock has to be advanced by
// ticking rather than by jumping.
//
// The committed PNGs are golden references: a normal run COMPARES (RMSE) and
// writes nothing; pass --olo-golden-rebase to (re)write them. Run from
// OloEditor/.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

// OLO_TEST_LAYER: L8

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"
#include "OloEngine/Renderer/Water/WaterSpraySystem.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        /// Wall-clock the field is built up to before any capture. Long enough
        /// for a 3.5 s half-life foam field to reach a steady state.
        constexpr f32 kSettleStartTime = 40.0f;
        /// Under RenderPipeline's 0.25 s dt clamp, so the field advances by the
        /// whole interval rather than by the clamp.
        constexpr f32 kTickSeconds = 0.05f;
        /// Ticks used to settle the field before capturing.
        constexpr u32 kSettleTicks = 120; // 6 s

        constexpr f64 kGoldenRmseThreshold = 6.0;

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
            return OloEngine::Tests::Options().GoldenRebase;
        }

        /// RAII so a mocked clock cannot leak into the tests that follow this
        /// one in the same process, on any exit path including ASSERT returns.
        /// Copied in shape from WaterWakeVisualEvidenceTest, deliberately: a
        /// leaked mock clock freezes every later visual test's animation and
        /// the resulting golden mismatches point everywhere except here.
        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        };
    } // namespace

    class WaterFoamSprayRainVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Issue #1086. The disturbance field these four systems write is
            // WORLD-anchored and lives in Renderer3D's process statics, not in
            // the Scene — so the fresh Scene this fixture builds per test does
            // NOT clear it, and each test inherits whatever foam the previous
            // one deposited at the same world coordinates. Production is not
            // affected: Scene::OnRuntimeStart and Scene::OnSimulationStart both
            // reset exactly these four (Scene.cpp, "Drop any wake foam and hull
            // history left by a previous scene"), and RendererAttachedTest
            // drives OnUpdate* directly without passing through either.
            //
            // Left unreset, the spray poses were captured with the foam field
            // FoamDriftSequenceFromAFixedCamera had already built up, so they
            // matched only while that test ran first in the same process and
            // diverged by RMSE 12.1 / 8.7 whenever the spray test ran alone —
            // which is what a --gtest_filter or a ctest shard does. Resetting
            // here makes every pose in this file self-contained, so the goldens
            // encode the same frame in any execution order.
            WaterDisturbanceSystem::Reset();
            WaterWakeSystem::Reset();
            WaterRainRippleSystem::Reset();
            WaterSpraySystem::Reset();

            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 30.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.6f, -0.7f));
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
                env.m_IBLIntensity = 0.3f;
            }

            {
                Entity ocean = scene.CreateEntity("Ocean");
                m_Ocean = ocean;
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 400.0f;
                wc.m_WorldSizeZ = 400.0f;
                wc.m_GridResolutionX = 192;
                wc.m_GridResolutionZ = 192;
                wc.m_RenderFromBelow = true;

                // A BUILT sea, unlike the wake test's deliberately calm one:
                // every feature under test here is driven by the surface
                // FOLDING, so a flat sea would make each capture a picture of
                // nothing and each assertion vacuous.
                wc.m_UseFFT = true;
                wc.m_FFTResolution = 128;
                wc.m_FFTPatchSize = 90.0f;
                wc.m_FFTWindSpeed = 24.0f;
                wc.m_FFTWindDirection = { 1.0f, 0.25f };
                wc.m_FFTAmplitude = 2.0f;
                wc.m_FFTChoppiness = 1.6f;
                wc.m_FFTHeightScale = 1.0f;
                // The DEFAULT coverage. Raising it floods the near water with
                // procedural crest foam, which is not what is under test and
                // buries the advected field's own contribution in it.
                wc.m_FoamCoverage = 0.12f;

                wc.m_FoamAdvectionEnabled = true;
                wc.m_FoamAdvectionIntensity = 1.0f;
                wc.m_FoamAdvectionHalfLife = 3.5f;
                wc.m_FoamAdvectionThreshold = 0.10f;
                wc.m_FoamAdvectionDrift = 0.06f;

                wc.m_SprayEnabled = true;
                wc.m_SprayThreshold = 0.20f;
                wc.m_SprayRate = 60.0f;
                // EXAGGERATED for the capture, and deliberately so: the default
                // 0.25 m puff is a few pixels at any pose that also shows the
                // sea it comes off, which makes a screenshot useless as
                // evidence. The emission criterion — the part under test — is
                // unaffected by size.
                wc.m_SprayParticleSize = 0.6f;
                wc.m_SprayRadius = 40.0f;

                wc.m_RainRipplesEnabled = true;
                wc.m_RainRippleStrength = 1.4f;
                wc.m_RainRippleFadeStart = 22.0f;
                wc.m_RainRippleFadeEnd = 55.0f;
            }
        }

        [[nodiscard]] WaterComponent& Water()
        {
            return m_Ocean.GetComponent<WaterComponent>();
        }

        [[nodiscard]] static EditorCamera LowCamera()
        {
            // Low and near the surface: the ripple fade ends at 55 m and the
            // spray billboards are 12 cm across, so a high overview shot would
            // show neither. Grazing the water is where all three read.
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f,
                                600.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // SetPose (eye, yaw, pitch), the same form WaterWakeVisualEvidenceTest
            // uses — EditorCamera has no focal-point setter, and an orbit
            // distance would put the eye somewhere the yaw decides rather than
            // somewhere the test does.
            camera.SetPose({ 0.0f, 3.5f, 30.0f }, 0.0f, 0.12f);
            return camera;
        }

        /// Close in and looking DOWN, for the poses whose subject is
        /// centimetres across. A 0.25 m spray puff is ~4 px at the 30 m
        /// overview pose — indistinguishable from the foam speckle it sits on —
        /// so a capture meant as evidence that spray is on screen has to be
        /// taken from somewhere it can be seen.
        ///
        /// High and steeply DOWN, and both halves of that are load-bearing:
        ///
        ///   * an eye at 1.4 m spends most frames UNDERWATER on this sea (it is
        ///     RMS-normalised to ~0.3 x its 2 m amplitude and crests reach a few
        ///     times that), and the capture is then the flat tinted underside
        ///     path with no spray, no foam and no waves in it;
        ///   * a SHALLOW look drops the sightline below crest height within ~20 m,
        ///     so the near crests occlude every droplet beyond them and the frame
        ///     shows two specks out of several thousand live particles.
        ///
        /// 16 m up at 40 degrees clears the crests, which is what a capture whose
        /// job is "show that spray is on screen" has to do.
        [[nodiscard]] static EditorCamera CloseCamera()
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f,
                                600.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 16.0f, 16.0f }, 0.0f, 0.72f);
            return camera;
        }

        /// Advance the mocked clock by `ticks` steps, running one full pipeline
        /// tick each. It has to be TICKED, not fast-forwarded: RenderPipeline
        /// clamps the field's per-frame dt to 0.25 s, so jumping the clock and
        /// rendering once advances the field by 0.25 s and no further.
        void Tick(const EditorCamera& camera, u32 ticks, f32 startTime)
        {
            for (u32 i = 1; i <= ticks; ++i)
            {
                Time::SetMockTime(startTime + static_cast<f32>(i) * kTickSeconds);
                RunEditorFrames(camera, 1);
            }
        }

        [[nodiscard]] static f32 EndTime(f32 startTime, u32 ticks)
        {
            return startTime + static_cast<f32>(ticks) * kTickSeconds;
        }

        /// Render + read back the composited frame, flipped right-side up.
        void Readback(const EditorCamera& camera, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 1);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // glGetTextureImage returns rows bottom-up; stbi_write_png and the
            // band sampling below both treat row 0 as the TOP.
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

        void WriteOrCompareGolden(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("WaterFoamSprayRain_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string()
                                 << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh
                                     << ", expected " << kWidth << "x" << kHeight;

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }

        /// Fraction of pixels in the lower half of the frame — which is water,
        /// from this camera — bright enough to be foam rather than sea.
        ///
        /// A COVERAGE measure rather than a mean: advected foam is the same
        /// white as instantaneous foam, so brightness alone cannot tell them
        /// apart. What persistence changes is HOW MUCH of the surface is white
        /// at any instant.
        [[nodiscard]] static f64 FoamCoverage(const std::vector<u8>& px)
        {
            u64 bright = 0;
            u64 total = 0;
            for (u32 y = kHeight / 2u; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; x += 2u)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const u32 luma = (static_cast<u32>(px[idx + 0]) + px[idx + 1] + px[idx + 2]) / 3u;
                    if (luma > 150u)
                        ++bright;
                    ++total;
                }
            }
            return total ? static_cast<f64>(bright) / static_cast<f64>(total) : 0.0;
        }

        Entity m_Ocean;
    };

    // -------------------------------------------------------------------------
    // §2.2 — advected foam
    // -------------------------------------------------------------------------

    TEST_F(WaterFoamSprayRainVisualEvidenceTest, AdvectedFoamCoversMoreWaterThanInstantaneousFoam)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // RAII so the mocked clock cannot leak into the tests that follow
        // this one in the same process, on any exit path including ASSERT
        // returns.
        ScopedMockTime mockClock(kSettleStartTime);

        const EditorCamera camera = LowCamera();
        Water().m_SprayEnabled = false;
        Water().m_RainRipplesEnabled = false;

        // ONE continuous timeline, and the control is a SINGLE tick after the
        // treatment. That ordering is the whole design of this comparison:
        // Scene's animation clock ACCUMULATES across ticks (it is scene-owned,
        // not derived from the mocked wall clock — water-shading-nyquist.md §6),
        // so running the two settle sequences separately would put the two
        // captures at different wave phases and the coverage difference would be
        // partly the sea and partly the feature. Flipping one uniform and
        // stepping 0.05 s leaves the sea where it was.
        Water().m_FoamAdvectionEnabled = true;
        Tick(camera, kSettleTicks, kSettleStartTime);
        const f32 clock = EndTime(kSettleStartTime, kSettleTicks);

        std::vector<u8> advected;
        Readback(camera, advected);
        ASSERT_FALSE(advected.empty());
        const f64 advectedCoverage = FoamCoverage(advected);
        WriteOrCompareGolden("FoamAdvectOn", advected);

        // Control: advection OFF. The shader falls back to the instantaneous
        // Jacobian term, which is what the sea looked like before #1034.
        Water().m_FoamAdvectionEnabled = false;
        Tick(camera, 1u, clock);

        std::vector<u8> instantaneous;
        Readback(camera, instantaneous);
        ASSERT_FALSE(instantaneous.empty());
        const f64 instantaneousCoverage = FoamCoverage(instantaneous);
        // Negative control: the fixture must actually be foaming, or the
        // comparison below is between two empty seas.
        ASSERT_GT(instantaneousCoverage, 0.0)
            << "the control sea shows no foam at all — this comparison is vacuous";
        WriteOrCompareGolden("FoamAdvectOff", instantaneous);

        // Foam that PERSISTS after the crest has passed covers more of the
        // surface at any instant than foam that exists only where the surface
        // is folding right now. An implementation that merely re-coloured the
        // same folding crests would score the same here.
        EXPECT_GT(advectedCoverage, instantaneousCoverage * 1.05)
            << "advected foam coverage " << advectedCoverage << " is not meaningfully above the "
            << "instantaneous " << instantaneousCoverage << " — the field is not persisting";
    }

    TEST_F(WaterFoamSprayRainVisualEvidenceTest, FoamDriftSequenceFromAFixedCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // RAII so the mocked clock cannot leak into the tests that follow
        // this one in the same process, on any exit path including ASSERT
        // returns.
        ScopedMockTime mockClock(kSettleStartTime);

        // The multi-frame evidence. One camera, four instants half a second
        // apart, so flipping between the PNGs shows the foam patches TRAVELLING
        // rather than blinking. The numeric proof of that is
        // WaterFoamAdvectionTest — a golden cannot assert motion, only record
        // it for a human.
        const EditorCamera camera = LowCamera();
        Water().m_SprayEnabled = false;
        Water().m_RainRipplesEnabled = false;

        Tick(camera, kSettleTicks, kSettleStartTime);
        f32 clock = EndTime(kSettleStartTime, kSettleTicks);

        for (u32 frame = 0; frame < 4u; ++frame)
        {
            std::vector<u8> pixels;
            Readback(camera, pixels);
            ASSERT_FALSE(pixels.empty());
            EXPECT_GT(FoamCoverage(pixels), 0.0) << "frame " << frame << " shows no foam";
            WriteOrCompareGolden("FoamDrift_" + std::to_string(frame), pixels);

            Tick(camera, 10u, clock); // half a second
            clock = EndTime(clock, 10u);
        }
    }

    // -------------------------------------------------------------------------
    // §2.3 — crest spray
    // -------------------------------------------------------------------------

    TEST_F(WaterFoamSprayRainVisualEvidenceTest, SprayEmitsOnABuiltSeaAndNotOnACalmOne)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // RAII so the mocked clock cannot leak into the tests that follow
        // this one in the same process, on any exit path including ASSERT
        // returns.
        ScopedMockTime mockClock(kSettleStartTime);

        // Close in: the subject is 25 cm across, and at the overview pose it is
        // a handful of pixels lost in the foam.
        const EditorCamera camera = CloseCamera();
        Water().m_RainRipplesEnabled = false;
        Water().m_SprayEnabled = true;

        Tick(camera, 40u, kSettleStartTime);
        const u32 builtSeaEmissions = WaterSpraySystem::GetLastEmitCount();
        const u32 alive = WaterSpraySystem::GetAliveParticleCount();

        std::vector<u8> spray;
        Readback(camera, spray);
        ASSERT_FALSE(spray.empty());
        WriteOrCompareGolden("SprayOn", spray);

        EXPECT_GT(builtSeaEmissions, 0u)
            << "a sea built by a 24 m/s wind emitted no spray — the emitter is not wired to the "
            << "live ocean field";
        // The emit count alone only proves the CPU emitter ran. This proves the
        // particles survived the upload, the simulate and the compaction and
        // are in the pool to be drawn — which is the half a screenshot of a
        // foamy sea genuinely cannot distinguish.
        EXPECT_GT(alive, 0u)
            << builtSeaEmissions << " particles were emitted but none are alive in the pool";

        // Flatten the sea. The spectrum is regenerated from the new wind, so
        // the fold signal collapses and the SAME emitter must fall silent.
        // This is the acceptance criterion, through the live pipeline.
        Water().m_FFTWindSpeed = 0.5f;
        Water().m_FFTAmplitude = 0.02f;
        Water().m_FFTChoppiness = 0.0f;
        Tick(camera, 40u, EndTime(kSettleStartTime, 40u));

        EXPECT_EQ(WaterSpraySystem::GetLastEmitCount(), 0u)
            << "spray emitted on a calm sea";

        std::vector<u8> calm;
        Readback(camera, calm);
        ASSERT_FALSE(calm.empty());
        WriteOrCompareGolden("SprayCalmSea", calm);
    }

    // -------------------------------------------------------------------------
    // §7.3 — rain-impact ripples
    // -------------------------------------------------------------------------

    TEST_F(WaterFoamSprayRainVisualEvidenceTest, RainStipplesTheSurfaceAndStopsWhenItIsNotRaining)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // RAII so the mocked clock cannot leak into the tests that follow
        // this one in the same process, on any exit path including ASSERT
        // returns.
        ScopedMockTime mockClock(kSettleStartTime);

        const EditorCamera camera = LowCamera();
        Water().m_SprayEnabled = false;
        Water().m_FoamAdvectionEnabled = false;

        // Rain OFF — with the water tile's ripple flag still ON, so this
        // isolates the SKY half of the gate. Precipitation is a renderer-global
        // setting rather than a component, so it is driven directly here.
        auto& precipitation = Renderer3D::GetPrecipitationSettings();
        precipitation.Enabled = false;
        precipitation.Type = PrecipitationType::Rain;
        precipitation.Intensity = 0.0f;
        PrecipitationSystem::SetIntensityImmediate(0.0f);
        Tick(camera, 20u, kSettleStartTime);

        std::vector<u8> dry;
        Readback(camera, dry);
        ASSERT_FALSE(dry.empty());
        WriteOrCompareGolden("RainOff", dry);

        // "No cost when rain is off" is an acceptance criterion, and the honest
        // place to assert it is EXACTLY here, on the value the shader gates on
        // — not on the frame. A frame comparison across two ticked sequences
        // carries the temporal pipeline's history with it, so it could only ever
        // be a tolerance, and a tolerance is precisely what this criterion is
        // not about: the shader must return BEFORE its cell walk, and it does
        // that on `x <= 0`.
        EXPECT_LE(WaterRainRippleSystem::GetShaderParams().x, 0.0f)
            << "the ripple field was live while it was not raining — the water tile's flag alone "
            << "switched it on, so the shader is walking 9 cells per fragment on a dry sea";

        // Rain ON.
        precipitation.Enabled = true;
        precipitation.Intensity = 1.0f;
        PrecipitationSystem::SetIntensityImmediate(1.0f);
        Tick(camera, 20u, kSettleStartTime);

        std::vector<u8> wet;
        Readback(camera, wet);
        ASSERT_FALSE(wet.empty());
        WriteOrCompareGolden("RainOn", wet);

        // The other half of the gate: with BOTH halves true the field is live.
        EXPECT_GT(WaterRainRippleSystem::GetShaderParams().x, 0.0f)
            << "it is raining on a tile that asked for ripples and the field is still disabled";

        // The stipple has to be VISIBLE, not merely present. The threshold is
        // well above the frame-to-frame noise a driver difference produces (the
        // golden threshold above is 6.0) and well below what a bug that
        // whitened the whole surface would score.
        const f64 difference = Rgba8Rmse(dry, wet);
        EXPECT_GT(difference, 1.5) << "rain did not visibly change the water surface";
        EXPECT_LT(difference, 90.0)
            << "rain changed the frame far more than a normal perturbation should — "
            << "the ripple slope is probably tipping the shading normal past horizontal";
    }
} // namespace OloEngine::Tests
