// =============================================================================
// WaterWakeVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the persistent boat-driven water disturbance field
// (issue #967). Renders an ocean through the FULL Renderer3D pipeline with a
// wake laid into the field, from the five camera setups the issue's acceptance
// criteria name, and writes each frame to
//   OloEditor/assets/tests/visual/WaterWake_<pose>.png
//
// WHY THIS TEST DRIVES THE SPLAT API DIRECTLY, AND NOT A BOAT
// ------------------------------------------------------------
// It submits through WaterDisturbanceSystem::SubmitSplat with no BoatComponent,
// no rigidbody and no physics step anywhere in the fixture. That is not a
// shortcut around a hard-to-build boat — it is the acceptance criterion
// "propeller/contact splats can be submitted independently of BoatComponent"
// being exercised as the ONLY path this test has. If someone later couples the
// service to boats, this file stops compiling rather than quietly still passing.
// The boat-side geometry (hull sweep, V-arms, propeller wash, the speed gate)
// is BoatWakeSystem's, and its bounded history is pinned separately and
// headlessly in WaterDisturbanceFieldTest.
//
// DETERMINISM
// -----------
// Two clocks matter and both are frozen through Time::SetMockTime:
//   * the wave phase / normal-map scroll (as in WaterVisualEvidenceTest), and
//   * the disturbance field's own decay, which RenderPipeline derives from
//     Time::GetTime() precisely so a mocked clock makes it reproducible.
// The "stopped" pose then advances the mock clock by an exact number of seconds
// rather than by however long the previous frame happened to take, which is
// what lets a DECAY capture be a golden at all.
//
// The committed WaterWake_<pose>.png files are golden references: a normal run
// COMPARES (RMSE) and writes nothing; pass --olo-golden-rebase to (re)write
// them after a deliberate visual change. Run from OloEditor/.
//
// Beyond the PNGs — and this is the part that fails on a real regression rather
// than on a driver difference — three driver-independent contracts are asserted
// from the readback itself:
//   1. the trail is BRIGHTER than untouched open water beside it (there is a
//      wake at all);
//   2. it is in the RIGHT PLACE — the bright band sits where the splats were
//      laid, not offset, mirrored about the origin, or wrapped to the far side
//      of the field, which are the three ways the toroidal addressing fails;
//   3. it DECAYS — the same pixels are dimmer after the clock advances with no
//      further splats.
// A golden alone cannot distinguish any of those from "the frame changed".
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

// OLO_TEST_LAYER: L8

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
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
        namespace WD = OloEngine::WaterDisturbance;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        /// Wall-clock at which the trail finishes being laid. Every capture that
        /// is not testing decay renders at exactly this instant.
        constexpr f32 kLayStartTime = 30.0f;
        /// Seconds of simulated time per laid segment.
        constexpr f32 kLayStep = 1.0f / 30.0f;
        /// Segments in each trail. 90 at 0.6 m per segment is a ~54 m trail —
        /// long enough to read as a wake from the overhead pose and short enough
        /// to stay well inside the 256 m field.
        constexpr u32 kSegments = 90;

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

        /// The path a "boat" takes for a capture.
        enum class TrailShape
        {
            Straight,
            SCurve,
        };

        /// World XZ of the trail at parameter t in [0, 1].
        ///
        /// Deliberately closed-form rather than integrated: an integrated path
        /// would depend on the frame count, and the whole point of the goldens
        /// is that the same capture reproduces exactly.
        [[nodiscard]] glm::vec2 TrailPoint(TrailShape shape, f32 t)
        {
            // Runs along +Z toward the camera, starting well behind the origin.
            const f32 z = -28.0f + t * 54.0f;
            if (shape == TrailShape::Straight)
                return { 0.0f, z };
            // One full S: two opposite-signed lobes, so the capture actually
            // shows the trail changing direction rather than just curving.
            return { 9.0f * std::sin(t * 2.0f * 3.14159265f), z };
        }
    } // namespace

    class WaterWakeVisualEvidenceTest : public RendererAttachedTest
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
                wc.m_WorldSizeX = 400.0f;
                wc.m_WorldSizeZ = 400.0f;
                wc.m_GridResolutionX = 192;
                wc.m_GridResolutionZ = 192;
                // A calm sea on purpose. Whitecaps are the loudest thing in the
                // frame, and a built sea would bury the wake in exactly the
                // signal these captures need to separate it from.
                wc.m_WaveAmplitude = 0.25f;
                wc.m_FoamCoverage = 0.02f;
                wc.m_RenderFromBelow = true;

                wc.m_WakeFoamEnabled = true;
                wc.m_WakeFoamIntensity = 1.0f;
                wc.m_WakeFoamHalfLife = 6.0f;
                wc.m_WakeFoamFadeStart = 90.0f;
                wc.m_WakeFoamFadeEnd = 260.0f;
            }
        }

        /// Lay a trail into the field by submitting one capsule per simulated
        /// frame, advancing the mocked clock between frames so the field decays
        /// exactly as it would in a real run.
        ///
        /// Each RunEditorFrames(camera, 1) drives one full pipeline tick, which
        /// is what consumes the queued splat — the queue is per-dispatch, so a
        /// batch submitted without ticking between them would all land at once
        /// and paint a single blob rather than a trail.
        void LayTrail(TrailShape shape, const EditorCamera& camera)
        {
            for (u32 i = 0; i < kSegments; ++i)
            {
                const f32 t0 = static_cast<f32>(i) / static_cast<f32>(kSegments);
                const f32 t1 = static_cast<f32>(i + 1u) / static_cast<f32>(kSegments);

                WaterDisturbanceSplat splat;
                splat.m_From = TrailPoint(shape, t0);
                splat.m_To = TrailPoint(shape, t1);
                splat.m_Radius = 1.6f;
                splat.m_Strength = 0.85f;
                splat.m_Softness = 1.4f;
                ASSERT_TRUE(WaterDisturbanceSystem::SubmitSplat(splat))
                    << "segment " << i << " was rejected — the field will have a gap";

                Time::SetMockTime(kLayStartTime + static_cast<f32>(i) * kLayStep);
                RunEditorFrames(camera, 1);
            }
        }

        /// Advance the field by `seconds` of decay, submitting nothing.
        ///
        /// It has to be TICKED, not fast-forwarded. RenderPipeline clamps the
        /// wake's per-frame dt to 0.25 s — correct engine behaviour, so a
        /// breakpoint or a hitch cannot wipe the field in one frame — which
        /// means jumping the mocked clock 12 s and rendering once decays it by
        /// 0.25 s, not 12. The first version of this test did exactly that and
        /// measured no decay at all, which looked like a broken decay rather
        /// than a broken test.
        void DecayFor(f32 seconds, const EditorCamera& camera, f32 startTime)
        {
            constexpr f32 kStep = 0.2f; // under RenderPipeline's 0.25 s dt clamp
            const u32 steps = static_cast<u32>(std::ceil(seconds / kStep));
            for (u32 i = 1; i <= steps; ++i)
            {
                Time::SetMockTime(startTime + static_cast<f32>(i) * kStep);
                RunEditorFrames(camera, 1);
            }
        }

        /// Render + read back the composited frame, flip it right-side up, and
        /// golden-compare (or rebase) it. Mirrors WaterVisualEvidenceTest's
        /// Capture so the two agree on which buffer is "the frame".
        void CaptureAndCompare(const std::string& poseName, const EditorCamera& camera,
                               std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 1);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // glGetTextureImage returns rows bottom-up; both stbi_write_png and
            // the band sampling below treat row 0 as the TOP.
            {
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

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("WaterWake_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string()
                                 << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, outPixels.data(),
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
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }

        /// Mean RGB of an axis-aligned pixel rectangle (row 0 == top).
        [[nodiscard]] static f64 MeanLuma(const std::vector<u8>& px, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            u64 sum = 0;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sum += px[idx + 0];
                    sum += px[idx + 1];
                    sum += px[idx + 2];
                    count += 3;
                }
            }
            return count ? static_cast<f64>(sum) / static_cast<f64>(count) : 0.0;
        }

        /// Standard deviation of per-pixel luma over a rectangle. A rendered
        /// water surface has real structure (waves, foam, glitter) and scores
        /// tens; a flat fogged slab scores ~1.
        [[nodiscard]] static f64 LumaStdDev(const std::vector<u8>& px, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            const f64 mean = MeanLuma(px, x0, y0, x1, y1);
            f64 sumSq = 0.0;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const f64 luma =
                        (static_cast<f64>(px[idx]) + px[idx + 1] + px[idx + 2]) / 3.0;
                    sumSq += (luma - mean) * (luma - mean);
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        [[nodiscard]] static EditorCamera MakeCamera(const glm::vec3& position, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            return camera;
        }
    };

    // The five acceptance views (issue #967): straight travel, an S-turn,
    // stopped-and-decaying, overhead, and grazing.
    TEST_F(WaterWakeVisualEvidenceTest, CaptureWakeFromTheAcceptanceViews)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // RAII so the mocked clock cannot leak into the tests that follow this
        // one in the same process, on any exit path including ASSERT returns.
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
        } scopedMockTime(kLayStartTime);

        WaterDisturbanceSystem::Reset();

        // --- 1. Straight travel, seen from astern and slightly above ---------
        const EditorCamera chase = MakeCamera({ 0.0f, 9.0f, 46.0f }, 0.0f, 0.30f);
        LayTrail(TrailShape::Straight, chase);
        if (::testing::Test::HasFatalFailure())
            return;

        std::vector<u8> straightPixels;
        CaptureAndCompare("StraightTravel", chase, straightPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // CONTRACT 1 + 2: there is a trail, and it is WHERE IT WAS LAID.
        //
        // The trail runs along world X = 0, i.e. straight down the middle of a
        // frame whose camera sits on the +Z axis looking at the origin. So the
        // centre column is the trail and the columns either side are untouched
        // open water. Comparing regions of the SAME frame is what makes this
        // driver-independent, and comparing them HORIZONTALLY is what catches
        // the addressing failures: a field offset laterally, or wrapped to the
        // far side of the torus, moves the bright band off the centre while
        // leaving a perfectly plausible wake elsewhere in frame — which a
        // brightness-only check accepts and a golden reports as merely
        // "different".
        //
        // The bands are the MEASURED trail geometry for this pose, not guessed
        // fractions: rows 320-440 are where the laid trail actually projects,
        // and rows below ~460 are foreground water in front of its near end.
        // Sampling those instead is what the first version of this test did, and
        // it reported no wake in a frame that plainly has one.
        {
            constexpr u32 bandY0 = 320;
            constexpr u32 bandY1 = 440;
            const f64 onTrail = MeanLuma(straightPixels, 600, bandY0, 680, bandY1);
            const f64 offTrailLeft = MeanLuma(straightPixels, 430, bandY0, 510, bandY1);
            const f64 offTrailRight = MeanLuma(straightPixels, 770, bandY0, 850, bandY1);

            // Measured on this scene: centre 155.9, left 129.7, right 115.3 —
            // margins of +26 and +41. The threshold is set well under both so
            // it reports a wake that has WEAKENED or MOVED, not GPU noise.
            //
            // Compared against each side SEPARATELY rather than their mean: the
            // scene's lighting is genuinely asymmetric (the sun glitters off the
            // port bow), so an averaged reference would let a trail that had
            // slid onto the bright side still pass.
            EXPECT_GT(onTrail, offTrailLeft + 10.0)
                << "No wake above the open water to its left (trail " << onTrail << " vs "
                << offTrailLeft << "). Either no foam was written, or the field is addressed "
                                   "somewhere else. See WaterWake_StraightTravel.png";
            EXPECT_GT(onTrail, offTrailRight + 10.0)
                << "No wake above the open water to its right (trail " << onTrail << " vs "
                << offTrailRight << "). See WaterWake_StraightTravel.png";
        }

        // --- 2. The S-turn ---------------------------------------------------
        // A fresh field: the straight trail is still in there at full strength,
        // and leaving it would make the S-turn capture a picture of both.
        WaterDisturbanceSystem::Reset();
        Time::SetMockTime(kLayStartTime);
        LayTrail(TrailShape::SCurve, chase);
        if (::testing::Test::HasFatalFailure())
            return;

        std::vector<u8> sTurnPixels;
        CaptureAndCompare("STurn", chase, sTurnPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // CONTRACT 2, the stronger form: the S-turn trail must NOT look like the
        // straight one. This is what actually shows the field follows the path
        // rather than being a fixed decoration — and it is asserted on the same
        // camera, so the only difference between the two frames is the trail.
        {
            const f64 rmse = Rgba8Rmse(straightPixels, sTurnPixels);
            EXPECT_GT(rmse, 3.0)
                << "The S-turn frame is nearly identical to the straight-travel frame (RMSE " << rmse
                << ") — the trail does not follow the submitted path. Compare "
                   "WaterWake_StraightTravel.png and WaterWake_STurn.png";
        }

        // --- 3. Stopped: the trail decays ------------------------------------
        // Same camera, same trail, no further splats — only the clock moves.
        // Two half-lives, so the trail must lose most of its excess brightness.
        //
        // Measured as trail MINUS nearby open water in the same frame, not as
        // the trail's absolute brightness. Advancing the clock to decay the
        // field also advances the WAVE PHASE, so the whole sea changes between
        // the two captures; an absolute comparison measures the sea moving at
        // least as much as the wake fading, and the first version of this test
        // read the decayed frame as marginally BRIGHTER for exactly that reason.
        // The trail's excess over its own surroundings is the part that is about
        // the field.
        auto trailContrast = [this](const std::vector<u8>& px)
        {
            return MeanLuma(px, 440, 270, 760, 460) - MeanLuma(px, 860, 270, 1180, 460);
        };
        const f64 beforeDecay = trailContrast(sTurnPixels);

        DecayFor(12.0f, chase, kLayStartTime + static_cast<f32>(kSegments) * kLayStep);
        std::vector<u8> decayedPixels;
        CaptureAndCompare("StoppedDecay", chase, decayedPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // CONTRACT 3: it fades. The R8 storage bug this feature nearly shipped
        // with produced a wake that rendered perfectly and NEVER decayed, with
        // every math test green — so "the frame changed" is not enough, the
        // trail specifically has to lose contrast against the water around it.
        {
            const f64 afterDecay = trailContrast(decayedPixels);
            // NEGATIVE CONTROL: there has to be a trail to fade in the first
            // place, or "it faded" is satisfied by there never having been one.
            ASSERT_GT(beforeDecay, 6.0)
                << "No measurable trail before the decay step — the rest of this assertion is "
                   "vacuous. See WaterWake_STurn.png";
            // 12 s at a 6 s half-life is two half-lives: a quarter left. The 0.6
            // threshold leaves plenty of room for the wave phase moving under it.
            EXPECT_LT(afterDecay, beforeDecay * 0.6)
                << "The wake did not fade after 12 s at a 6 s half-life (contrast " << beforeDecay
                << " -> " << afterDecay
                << "). See WaterWake_STurn.png vs WaterWake_StoppedDecay.png";
        }

        // --- 4 + 5. Overhead and grazing -------------------------------------
        // Re-laid, because the field has just been decayed to nearly nothing.
        WaterDisturbanceSystem::Reset();
        Time::SetMockTime(kLayStartTime);

        // Overhead: the pose that shows the trail's SHAPE, and the one where a
        // wake that slid with the mesh or wrapped across the torus is obvious.
        const EditorCamera overhead = MakeCamera({ 0.0f, 62.0f, 6.0f }, 0.0f, 1.45f);
        LayTrail(TrailShape::SCurve, overhead);
        if (::testing::Test::HasFatalFailure())
            return;
        std::vector<u8> overheadPixels;
        CaptureAndCompare("Overhead", overhead, overheadPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // Grazing: just above the surface looking along it. The pose where the
        // crest-foam distance fade would have deleted the wake had it been
        // folded in before that fade instead of after it.
        //
        // 3.2 m, not the 1.6 m this was first written at. The underwater stage
        // activates whenever the eye is within a wave's reach of the surface —
        // `gap > -waveReach` with waveReach floored at 2 m (Scene.cpp) — so at
        // 1.6 m the capture came back as a flat blue slab with the underwater
        // fog, blur and chromatic split over it, and no water surface at all.
        // The test still passed: the only assertion on this pose was
        // "not near-black", and a fogged slab is not near-black. The variance
        // check below is what now fails on it.
        const EditorCamera grazing = MakeCamera({ 0.0f, 3.2f, 34.0f }, 0.0f, 0.03f);
        std::vector<u8> grazingPixels;
        CaptureAndCompare("Grazing", grazing, grazingPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // Every pose must have produced a real frame, not a black one.
        for (const auto* named : { &straightPixels, &sTurnPixels, &decayedPixels, &overheadPixels,
                                   &grazingPixels })
        {
            EXPECT_GT(MeanLuma(*named, 0, 0, kWidth, kHeight), 5.0) << "a pose rendered (near-)black";
        }

        // ...and a real WATER SURFACE, which "not black" does not establish.
        // The grazing pose is the one that fails this: put the eye inside the
        // underwater stage's activation band and the whole lower frame comes
        // back as a uniformly fogged slab — bright, plausible at a glance, and
        // showing no surface, no waves and no wake. Structure is what separates
        // "rendered the sea" from "rendered a colour".
        {
            const f64 grazingStdDev = LumaStdDev(grazingPixels, 200, 380, 1080, 620);
            EXPECT_GT(grazingStdDev, 6.0)
                << "The grazing frame's water region is nearly uniform (std dev " << grazingStdDev
                << ") — no surface detail. The usual cause is the camera sitting inside the "
                   "underwater stage's activation band (within waveReach of the surface), which "
                   "replaces the whole lower frame with fog. See WaterWake_Grazing.png";
        }

        WaterDisturbanceSystem::Reset();
    }
} // namespace OloEngine::Tests
