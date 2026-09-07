// =============================================================================
// WaterWakeShapeVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the boat-shaped water SURFACE (issue #968), from the
// five camera setups the issue's acceptance criteria name, written to
//   OloEditor/assets/tests/visual/WaterWakeShape_<pose>.png
//
// WHY THIS IS A SEPARATE FILE FROM WaterWakeVisualEvidenceTest
// -------------------------------------------------------------
// That one captures #967's FOAM field. This one captures the GEOMETRY, and it
// switches the foam OFF to do it. That is the whole point: with foam in the
// frame, a reviewer cannot tell a wake that displaces the surface from a decal
// painted on a flat one — which is precisely the "foam-only decal would look
// disconnected at close range" problem #968 exists to fix. With foam off, every
// bright and dark band in these captures is the sea's own shading responding to
// a surface that actually moved.
//
// WHAT THE GOLDENS CANNOT TELL YOU, AND WHAT IS ASSERTED INSTEAD
// --------------------------------------------------------------
// An RMSE-vs-golden check reports a mirrored V, a wake laid along the track
// instead of either side of it, a bow bump with no normal to go with it, and a
// new driver, all identically: "the frame changed". So every capture is taken
// TWICE — once with the wake enabled and once with it disabled, everything else
// held — and the assertions are about the DIFFERENCE between those two frames:
//
//   1. the wake CHANGES the frame at all (max per-pixel difference is real, not
//      driver noise);
//   2. the change is LOCAL — a box around the strongest difference carries far
//      more difference per pixel than the frame's average does. This is the
//      assertion that fails on a wake which lifted the whole water plane, which
//      is what an unbounded term or a bump with non-compact support produces
//      and which looks completely plausible on its own;
//   3. the change is a SURFACE change, not a shading change — the hull
//      footprint's luma standard deviation FALLS when the wake is on, because
//      the ocean displacement is suppressed there. That is the "no water
//      clipping through the deck" criterion, measured;
// The turning capture is included in the same pass, and the curved-trail
// criterion it exists for — that the wake follows the hull's HISTORY rather than
// its current heading — is asserted numerically and headlessly by
// WaterWakeShapeTest.ArmsFollowACurvedHistoryRatherThanTheCurrentHeading. Here
// it is a picture, and its golden is what a reviewer looks at.
//
// The A/B is also what makes the numbers meaningful at all: measured against an
// absolute threshold, "the water looks different here" is indistinguishable
// from the frame-to-frame noise floor (docs/agent-rules/
// live-verification-noise-floor.md). Both halves of each pair render the same
// scene at the same mocked instant, so the noise floor between them is zero by
// construction and any difference is the feature.
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

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Physics3D/BoatWakeSystem.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"
#include "OloEngine/Renderer/Water/WaterSpraySystem.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <iostream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        /// Frozen wall clock. Both halves of every A/B pair render at exactly
        /// this instant, so the sea is identical between them and the entire
        /// difference is the wake.
        constexpr f32 kCaptureTime = 30.0f;

        constexpr f64 kGoldenRmseThreshold = 6.0;

        /// The hull the captures are built around. Launch-sized, matching
        /// BoatWakeSystem's own fallbacks.
        constexpr f32 kHalfBeam = 1.3f;
        constexpr f32 kHalfLength = 3.2f;
        constexpr f32 kCruiseSpeed = 9.0f;

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

        /// Per-pixel mean absolute RGB difference between two readbacks.
        [[nodiscard]] std::vector<f32> DiffMap(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            std::vector<f32> out(static_cast<std::size_t>(kWidth) * kHeight, 0.0f);
            for (std::size_t p = 0; p < out.size(); ++p)
            {
                const std::size_t i = p * 4u;
                const f32 d = (std::abs(static_cast<f32>(a[i + 0]) - static_cast<f32>(b[i + 0])) +
                               std::abs(static_cast<f32>(a[i + 1]) - static_cast<f32>(b[i + 1])) +
                               std::abs(static_cast<f32>(a[i + 2]) - static_cast<f32>(b[i + 2]))) /
                              3.0f;
                out[p] = d;
            }
            return out;
        }

        struct DiffStats
        {
            f32 m_Max = 0.0f; ///< the single strongest pixel, ANYWHERE — a liveness signal only
            u32 m_PeakX = 0;  ///< centre of the strongest BOX (see AnalyseDiff)
            u32 m_PeakY = 0;
            f64 m_GlobalMean = 0.0;
            f64 m_LocalMean = 0.0; ///< mean difference inside that box
        };

        /// Where the difference is, and how concentrated it is.
        ///
        /// The concentration ratio is the load-bearing number: a wake that
        /// raised the whole water plane produces a large max AND a large global
        /// mean, so `local / global` is near 1 — while a real, local wake puts
        /// the difference in one place and the ratio is many times that.
        ///
        /// WHERE the difference is means the centre of the box with the highest
        /// MEAN difference, NOT the position of the single brightest pixel.
        ///
        /// This distinction is the whole of issue #1094, so it is worth stating
        /// plainly. The hull footprint's A/B difference is broad and faint —
        /// hundreds of pixels across, peaking at 3-8 out of 255 on a smoothly
        /// shaded sea. A single-pixel argmax over a field like that is not an
        /// estimator of where the footprint is; it is an estimator of where the
        /// brightest speck is, and any stray speck outranks the real signal.
        /// EIGHT pixels of foam were enough: with them the argmax sat at
        /// (644, 327) and the footprint measurement read 0.69 / 1.10 (a pass),
        /// without them it moved 63 px to (581, 305) and read 1.30 / 0.61 — an
        /// apparently INVERTED A/B, from frames that are otherwise identical to
        /// the byte. Both numbers were true; they were measured in different
        /// places, one of them off the footprint.
        ///
        /// A window mean is the estimator that matches what the caller then
        /// measures (a box of the same size), and eight outlier pixels move it
        /// by 8/(2*half)^2 of their value — nothing. Computed through a summed-area
        /// table so the exhaustive search stays O(W*H) rather than O(W*H*half^2).
        ///
        /// `m_Max` is still the true global maximum, but it is only ever a
        /// LIVENESS signal ("the wake changed some pixel"), never a location.
        [[nodiscard]] DiffStats AnalyseDiff(const std::vector<f32>& diff, u32 boxHalfSize = 60)
        {
            DiffStats s;
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const f32 d = diff[static_cast<std::size_t>(y) * kWidth + x];
                    sum += d;
                    s.m_Max = std::max(s.m_Max, d);
                }
            }
            s.m_GlobalMean = sum / static_cast<f64>(diff.size());

            // Summed-area table: sat[(y+1)*(W+1) + (x+1)] is the sum over
            // [0,x] x [0,y]. f64 because kWidth*kHeight*255 overflows f32's
            // integer-exact range long before the last row.
            std::vector<f64> sat(static_cast<std::size_t>(kWidth + 1u) * (kHeight + 1u), 0.0);
            const auto satAt = [&sat](u32 x, u32 y) -> f64&
            { return sat[static_cast<std::size_t>(y) * (kWidth + 1u) + x]; };
            for (u32 y = 0; y < kHeight; ++y)
            {
                f64 rowSum = 0.0;
                for (u32 x = 0; x < kWidth; ++x)
                {
                    rowSum += diff[static_cast<std::size_t>(y) * kWidth + x];
                    satAt(x + 1u, y + 1u) = satAt(x + 1u, y) + rowSum;
                }
            }
            const auto boxMean = [&satAt](u32 x0, u32 y0, u32 x1, u32 y1) -> f64
            {
                const f64 area = static_cast<f64>(x1 - x0) * static_cast<f64>(y1 - y0);
                if (area <= 0.0)
                    return 0.0;
                return (satAt(x1, y1) - satAt(x0, y1) - satAt(x1, y0) + satAt(x0, y0)) / area;
            };

            // Search every FULL-SIZE window, so every candidate is scored over
            // the same area — a partial window at the frame edge would win on a
            // smaller denominator rather than on more difference.
            const u32 half = std::min(boxHalfSize, std::min(kWidth, kHeight) / 2u);
            f64 bestMean = -1.0;
            for (u32 cy = half; cy + half <= kHeight; ++cy)
            {
                for (u32 cx = half; cx + half <= kWidth; ++cx)
                {
                    const f64 m = boxMean(cx - half, cy - half, cx + half, cy + half);
                    if (m > bestMean)
                    {
                        bestMean = m;
                        s.m_PeakX = cx;
                        s.m_PeakY = cy;
                    }
                }
            }
            s.m_LocalMean = (bestMean > 0.0) ? bestMean : 0.0;
            return s;
        }

        [[nodiscard]] f64 MeanDiffInRect(const std::vector<f32>& diff, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = y0; y < std::min(y1, kHeight); ++y)
            {
                for (u32 x = x0; x < std::min(x1, kWidth); ++x)
                {
                    sum += diff[static_cast<std::size_t>(y) * kWidth + x];
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        /// The path the hull has taken, as a function of age in seconds.
        enum class TrailShape
        {
            Straight,
            SCurve,
        };

        /// World XZ and unit heading of the hull `age` seconds ago.
        ///
        /// Closed-form rather than integrated, for the reason
        /// WaterWakeVisualEvidenceTest gives: an integrated path would depend on
        /// the frame count and the goldens would stop reproducing.
        void PoseAtAge(TrailShape shape, f32 age, glm::vec2& outXZ, glm::vec2& outForward)
        {
            // The hull is at the origin now, travelling +Z, so a pose `age`
            // seconds ago is behind it.
            const f32 run = kCruiseSpeed * age;
            if (shape == TrailShape::Straight)
            {
                outXZ = { 0.0f, -run };
                outForward = { 0.0f, 1.0f };
                return;
            }
            // One S: heading swings to port and back as we look further back, so
            // the trail leaves a curve rather than a bend.
            const f32 theta = 0.55f * std::sin(age * 1.15f);
            outForward = { std::sin(theta), std::cos(theta) };
            // Integrating the heading in closed form is unnecessary — placing
            // each pose along its own heading at its own run distance produces a
            // curved polyline, which is all the arms are laid from.
            outXZ = -outForward * run;
        }
    } // namespace

    class WaterWakeShapeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        /// Enter on clean water state, and reset ALL FOUR services, not the two
        /// this file happens to drive.
        ///
        /// docs/agent-rules/world-anchored-renderer-state-in-tests.md already
        /// states both halves of this rule, and this file followed neither: it
        /// reset WaterWakeSystem and WaterDisturbanceSystem only, and only on the
        /// way OUT. The other two are world-anchored too, and
        /// `Scene::OnRuntimeStart` resets the four together for that reason.
        /// WaterSpraySystem in particular retains particles across a Scene
        /// boundary, and eight of the sibling test's spray specks surviving into
        /// this one is what made issue #1094's two orderings disagree at all.
        ///
        /// On the way IN rather than only on the way out, because a test can only
        /// guarantee the state it starts from — what ran before it in the process
        /// is not its to control, and `--gtest_filter` reorders it freely.
        void SetUp() override
        {
            WaterDisturbanceSystem::Reset();
            WaterWakeSystem::Reset();
            WaterRainRippleSystem::Reset();
            WaterSpraySystem::Reset();
            RendererAttachedTest::SetUp();
        }

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
                m_Ocean = ocean;
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 400.0f;
                wc.m_WorldSizeZ = 400.0f;
                // A denser grid than the foam evidence test uses: this one is
                // capturing GEOMETRY, and a wake ridge narrower than the mesh
                // spacing is band-limited away by design (WaterWake.h section
                // 5). At 400 m / 320 the spacing is 1.25 m, which resolves the
                // arms (radius ~1 m and growing) and lets the band limit be a
                // fade at the far end rather than a wall at the near one.
                wc.m_GridResolutionX = 320;
                wc.m_GridResolutionZ = 320;
                // NEARLY CALM, and this is a measurement decision rather than an
                // art one. The wake is a bounded, modest contribution — a 45 cm
                // bow rise and ~25 cm arm ridges — and on a 35 cm sea it is one
                // more ripple among many: the first version of these captures
                // used 0.35 m and the wake was real (the A/B found it) but
                // simply not legible to a reviewer, which is the entire job of
                // an evidence PNG. On a near-flat sea every feature in frame is
                // the boat's.
                //
                // The hull-exclusion test raises this in its own body, because
                // that one measures the FALL in surface variance and needs
                // variance to remove.
                wc.m_WaveAmplitude = 0.08f;
                wc.m_FoamCoverage = 0.02f;
                wc.m_RenderFromBelow = true;

                // Foam OFF. See the header block: with foam in the frame these
                // captures cannot distinguish a displaced surface from a decal,
                // which is the exact confusion this feature exists to end.
                wc.m_WakeFoamEnabled = false;

                wc.m_WakeShapeEnabled = true;
                wc.m_WakeShapeAffectsPhysics = false; // nothing floats here
                wc.m_WakeShapeHeightScale = 1.0f;
                wc.m_WakeShapeFlattenStrength = 0.9f;
            }
        }

        /// Publish one hull with a full historical arm polyline, exactly as
        /// BoatWakeSystem would.
        ///
        /// Driven through WaterWakeSystem directly rather than through a
        /// BoatComponent for the same reason the #967 evidence test drives
        /// SubmitSplat directly: the service knows nothing about boats, and a
        /// capture that needed a physics-driven boat would be measuring
        /// BoatSystem's settling behaviour as much as this feature.
        /// NOTE the ASSERT_TRUE inside: a fatal gtest failure returns from THIS
        /// function only, so RenderInto wraps both calls in
        /// ASSERT_NO_FATAL_FAILURE — otherwise a rejected hull would be followed
        /// by a rendered, read-back and golden-compared frame containing no wake.
        static void PublishHull(TrailShape shape, f32 speed)
        {
            WaterWakeSystem::BeginFrame();

            WaterWakeHullDesc desc;
            glm::vec2 nowXZ{ 0.0f };
            glm::vec2 nowFwd{ 0.0f, 1.0f };
            PoseAtAge(shape, 0.0f, nowXZ, nowFwd);
            desc.m_CentreXZ = nowXZ;
            desc.m_ForwardXZ = nowFwd;
            desc.m_HalfBeam = kHalfBeam;
            desc.m_HalfLength = kHalfLength;
            desc.m_Speed = speed;
            // The gate is BoatWakeSystem's; reproduce it so a stopped capture
            // gates the bow bump and arms off exactly as the game would.
            const auto gateFor = [](f32 s)
            {
                return glm::clamp((std::abs(s) - BoatWakeSystem::kMinSpeedMetresPerSecond) /
                                      std::max(BoatWakeSystem::kFullSpeedMetresPerSecond -
                                                   BoatWakeSystem::kMinSpeedMetresPerSecond,
                                               1.0e-3f),
                                  0.0f, 1.0f);
            };
            desc.m_Gate = gateFor(speed);
            desc.m_ArmSampleCount = WaterWake::kMaxArmSamples;
            for (u32 i = 0; i < WaterWake::kMaxArmSamples; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(WaterWake::kMaxArmSamples - 1u);
                const f32 age = BoatWakeSystem::kArmAgeMinSeconds +
                                t * (BoatWakeSystem::kArmAgeMaxSeconds - BoatWakeSystem::kArmAgeMinSeconds);
                WaterWakeArmSample& s = desc.m_Arms[i];
                PoseAtAge(shape, age, s.m_CentreXZ, s.m_ForwardXZ);
                s.m_AgeSeconds = age;
                s.m_Speed = speed;
                s.m_Gate = gateFor(speed);
            }
            ASSERT_TRUE(WaterWakeSystem::SubmitHull(desc));
        }

        /// Render one frame with the wake in the requested state and read it
        /// back, right-side up.
        ///
        /// The hull is (re)published every frame, as BoatWakeSystem does — a
        /// record published once and left standing is a different thing from
        /// what the game runs, and testing the wrong one is how the BeginFrame
        /// contract would go unnoticed.
        void RenderInto(const EditorCamera& camera, TrailShape shape, f32 speed, bool wakeEnabled,
                        std::vector<u8>& outPixels)
        {
            m_Ocean.GetComponent<WaterComponent>().m_WakeShapeEnabled = wakeEnabled;
            Time::SetMockTime(kCaptureTime);
            ASSERT_NO_FATAL_FAILURE(PublishHull(shape, speed));
            RunEditorFrames(camera, 1);
            // A second frame at the same instant: the settings reach the shader
            // through Renderer3D's published block, which is filled DURING a
            // frame, so the first frame after a toggle can still carry the
            // previous state. Rendering twice makes the A/B measure the wake
            // rather than a one-frame lag in the plumbing.
            ASSERT_NO_FATAL_FAILURE(PublishHull(shape, speed));
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
            // sampling below both treat row 0 as the TOP.
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

        void GoldenCompare(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("WaterWakeShape_" + poseName + ".png")).string();

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
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight;

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }

        /// Write an AMPLIFIED difference image beside the golden, on rebase.
        ///
        /// The golden shows what ships, and what ships is deliberately modest —
        /// a 45 cm bow rise and ~25 cm arm ridges on an unfoamed surface, which
        /// from overhead is a faint V a reviewer has to hunt for. That is a true
        /// property of the feature and not something to fix by inflating the
        /// amplitude for the camera.
        ///
        /// So the evidence is two images: the golden, which is honest about the
        /// look, and this one, which is honest about the SHAPE. Scaling the
        /// per-pixel A/B difference by 8 turns "is there a V there?" into a
        /// picture of the V, its half-angle, its curve through an S-turn, and
        /// the hull-shaped hole at its head — none of which the golden can be
        /// read for.
        void WriteDiffImage(const std::string& poseName, const std::vector<f32>& diff)
        {
            if (!GoldenRebaseRequested())
                return;
            std::vector<u8> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4u, 255u);
            for (std::size_t i = 0; i < diff.size(); ++i)
            {
                const auto v = static_cast<u8>(std::clamp(diff[i] * 8.0f, 0.0f, 255.0f));
                rgba[i * 4u + 0] = v;
                rgba[i * 4u + 1] = v;
                rgba[i * 4u + 2] = v;
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("WaterWakeShape_" + poseName + "_Diff.png")).string();
            (void)::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                   rgba.data(), static_cast<int>(kWidth) * 4);
        }

        [[nodiscard]] static f64 LumaStdDev(const std::vector<u8>& px, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = y0; y < std::min(y1, kHeight); ++y)
                for (u32 x = x0; x < std::min(x1, kWidth); ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sum += (static_cast<f64>(px[i]) + px[i + 1] + px[i + 2]) / 3.0;
                    ++count;
                }
            if (count == 0)
                return 0.0;
            const f64 mean = sum / static_cast<f64>(count);
            f64 sumSq = 0.0;
            for (u32 y = y0; y < std::min(y1, kHeight); ++y)
                for (u32 x = x0; x < std::min(x1, kWidth); ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const f64 l = (static_cast<f64>(px[i]) + px[i + 1] + px[i + 2]) / 3.0;
                    sumSq += (l - mean) * (l - mean);
                }
            return std::sqrt(sumSq / static_cast<f64>(count));
        }

        /// A camera ORBITING the hull rather than posed by hand.
        ///
        /// EditorCamera::Focus keeps the focal point, so the hull is in frame by
        /// construction whatever the yaw convention turns out to be. Hand-posed
        /// eye+yaw+pitch triples are how the first version of this test ended up
        /// photographing the open sea with the boat behind the camera in four
        /// poses out of five, and the empty sky in the fifth — every capture
        /// looked like a perfectly good picture of water, and the A/B assertion
        /// (`the wake changes the frame at all`) was the only thing that said
        /// otherwise. Positive pitch tilts the view DOWN.
        [[nodiscard]] static EditorCamera OrbitHull(f32 distance, f32 yaw, f32 pitch,
                                                    glm::vec3 focus = glm::vec3(0.0f))
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(focus, distance, yaw, pitch);
            return camera;
        }

        Entity m_Ocean;
    };

    // The five acceptance views: close, overhead, grazing, stopped, turning.
    TEST_F(WaterWakeShapeVisualEvidenceTest, CaptureWakeShapeFromTheAcceptanceViews)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
                WaterDisturbanceSystem::Reset();
                WaterWakeSystem::Reset();
                WaterRainRippleSystem::Reset();
                WaterSpraySystem::Reset();
            }
        } scopedClock;

        struct Pose
        {
            const char* m_Name;
            f32 m_Distance;      ///< metres from the focal point
            f32 m_Yaw;           ///< orbit azimuth
            f32 m_Pitch;         ///< positive tilts DOWN
            glm::vec2 m_FocusXZ; ///< what the orbit is centred on
            TrailShape m_Shape;
            f32 m_Speed;
        };

        // The five acceptance views. Every one orbits a point on the trail, so
        // the subject is in frame by construction. "Close" is the criterion's
        // own view — the one where a foam-only decal gives itself away.
        //
        // The three trailing views focus BEHIND the hull rather than on it: the
        // arms are what those poses exist to show, and they live astern.
        const std::array<Pose, 5> kPoses = { {
            { "Close", 11.0f, 0.6f, 0.30f, { 0.0f, 0.0f }, TrailShape::Straight, kCruiseSpeed },
            // Not straight down: at 83 degrees the Fresnel term is almost all
            // refraction, the surface shades from the deep colour, and a
            // displaced-but-unfoamed surface is nearly invisible however
            // correct it is. 57 degrees still reads as an overhead view and
            // keeps enough grazing response to show the V.
            { "Overhead", 34.0f, 0.0f, 1.00f, { 0.0f, -12.0f }, TrailShape::Straight, kCruiseSpeed },
            { "Grazing", 30.0f, 1.9f, 0.10f, { 0.0f, -10.0f }, TrailShape::Straight, kCruiseSpeed },
            { "Stopped", 11.0f, 0.6f, 0.30f, { 0.0f, 0.0f }, TrailShape::Straight, 0.0f },
            { "Turning", 40.0f, 0.0f, 0.95f, { 0.0f, -14.0f }, TrailShape::SCurve, kCruiseSpeed },
        } };

        for (const Pose& pose : kPoses)
        {
            const EditorCamera camera =
                OrbitHull(pose.m_Distance, pose.m_Yaw, pose.m_Pitch,
                          glm::vec3(pose.m_FocusXZ.x, 0.0f, pose.m_FocusXZ.y));

            std::vector<u8> withWake;
            std::vector<u8> withoutWake;
            ASSERT_NO_FATAL_FAILURE(
                RenderInto(camera, pose.m_Shape, pose.m_Speed, /*wakeEnabled=*/true, withWake));
            ASSERT_NO_FATAL_FAILURE(
                RenderInto(camera, pose.m_Shape, pose.m_Speed, /*wakeEnabled=*/false, withoutWake));

            // The golden is of the WAKE-ON frame; that is the deliverable.
            GoldenCompare(pose.m_Name, withWake);

            const std::vector<f32> diff = DiffMap(withWake, withoutWake);
            WriteDiffImage(pose.m_Name, diff);
            const DiffStats stats = AnalyseDiff(diff);

            // Printed unconditionally: these are the numbers to read when a
            // capture looks wrong, and the bands any future assertion should be
            // measured from rather than guessed at (docs/agent-rules/
            // persistent-world-space-fields.md section 4).
            std::cout << "[wake-shape] pose=" << pose.m_Name << " maxDiff=" << stats.m_Max << " peak at ("
                      << stats.m_PeakX << ", " << stats.m_PeakY << ") globalMean=" << stats.m_GlobalMean
                      << " localMean=" << stats.m_LocalMean << " ratio="
                      << (stats.m_GlobalMean > 1e-6 ? stats.m_LocalMean / stats.m_GlobalMean : 0.0)
                      << std::endl;

            // 1. The wake changes the frame. On the window MEAN, for the reason
            //    the footprint case below uses it: `m_Max` is one pixel, and a
            //    single-pixel statistic against a fixed bar is a flake waiting
            //    for a quantisation step (issue #1094). Measured across these
            //    five poses (RTX 4090, 2026-09-07): 1.21 (Stopped, the weakest
            //    by design) to 8.89 (Close).
            EXPECT_GT(stats.m_LocalMean, 0.5)
                << "pose '" << pose.m_Name
                << "': enabling the wake changed no pixel by more than driver noise — the surface is "
                   "not being displaced at all";

            // 2. And it changes it LOCALLY. A wake that lifted the whole plane
            //    would score a big max AND a big global mean, so the ratio is
            //    what separates "a boat is sitting in the sea" from "the sea
            //    moved". Deliberately a modest factor: the arms, the bow and the
            //    footprint are spread over a fair fraction of these framings, so
            //    a large ratio would be asserting the wake is SMALL rather than
            //    that it is local.
            ASSERT_GT(stats.m_GlobalMean, 1e-6) << "pose '" << pose.m_Name << "': the frames are identical";
            EXPECT_GT(stats.m_LocalMean / stats.m_GlobalMean, 2.0)
                << "pose '" << pose.m_Name
                << "': the difference is spread evenly over the frame — the whole water plane moved "
                   "rather than a wake forming in it";
        }
    }

    // The hull-exclusion criterion, measured: "a close Drift shot shows no water
    // clipping through the boat's deck or hull interior".
    TEST_F(WaterWakeShapeVisualEvidenceTest, TheHullFootprintFlattensTheSeaBeneathIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
                WaterDisturbanceSystem::Reset();
                WaterWakeSystem::Reset();
                WaterRainRippleSystem::Reset();
                WaterSpraySystem::Reset();
            }
        } scopedClock;

        // Straight down on the hull, close, so the footprint fills an
        // unambiguous part of the frame. Orbit-framed at pitch ~90 degrees;
        // hand-posing this is what pointed the first version at the empty sky.
        // A choppy sea on purpose — the opposite of the acceptance captures.
        // This test measures the FALL in surface variance inside the hull
        // footprint, so there has to be variance to remove; a NEGATIVE CONTROL
        // below asserts the fixture actually has it.
        m_Ocean.GetComponent<WaterComponent>().m_WaveAmplitude = 0.55f;

        // Looking down on the hull from 12 m at ~63 degrees. Not straight down:
        // from directly above the water shades almost entirely from its deep
        // colour, so removing the waves changes nothing measurable however
        // correctly they were removed.
        const EditorCamera camera = OrbitHull(12.0f, 0.0f, 1.10f);

        std::vector<u8> withWake;
        std::vector<u8> withoutWake;
        ASSERT_NO_FATAL_FAILURE(
            RenderInto(camera, TrailShape::Straight, 0.0f, /*wakeEnabled=*/true, withWake));
        ASSERT_NO_FATAL_FAILURE(
            RenderInto(camera, TrailShape::Straight, 0.0f, /*wakeEnabled=*/false, withoutWake));

        GoldenCompare("HullFootprint", withWake);

        // WHERE to measure is taken FROM THE CAPTURE, not from a fraction of the
        // frame. The footprint's projected position depends on the FOV, the
        // orbit distance and the pitch, and a band derived from percentages is a
        // guess about that projection whose failure looks exactly like the bug
        // the test exists to catch — see docs/agent-rules/
        // persistent-world-space-fields.md section 4. The strongest A/B
        // difference IS the footprint (nothing else in this frame changed), so
        // the box follows it.
        //
        // "Strongest" means the strongest BOX, not the strongest pixel — see
        // AnalyseDiff, and issue #1094, which was nothing but this distinction.
        constexpr u32 kHalf = 45;
        const std::vector<f32> diff = DiffMap(withWake, withoutWake);
        WriteDiffImage("HullFootprint", diff);
        const DiffStats stats = AnalyseDiff(diff, kHalf);
        std::cout << "[wake-shape] footprint diff max=" << stats.m_Max << " peak at (" << stats.m_PeakX
                  << ", " << stats.m_PeakY << ") globalMean=" << stats.m_GlobalMean
                  << " localMean=" << stats.m_LocalMean << std::endl;

        // LIVENESS, on the local mean rather than on the global maximum.
        //
        // The old form was ASSERT_GT(m_Max, 4.0f), and m_Max is a single pixel:
        // measured 4.33 in the ordering issue #1094 reported, i.e. 8% above its
        // own bar, and 8.0 in the other — a bar that a quantisation step could
        // cross either way. The window mean over the footprint is the same
        // quantity the assertions below are about and it is stable to three
        // digits across orderings: measured 2.90 (RTX 4090, 2026-09-07) in both.
        ASSERT_GT(stats.m_LocalMean, 1.0)
            << "the hull footprint changes no pixel — the ocean is not being suppressed under the hull, "
               "so a crest can rise through the deck";

        const u32 x0 = (stats.m_PeakX > kHalf) ? stats.m_PeakX - kHalf : 0u;
        const u32 y0 = (stats.m_PeakY > kHalf) ? stats.m_PeakY - kHalf : 0u;
        const u32 x1 = std::min(kWidth, stats.m_PeakX + kHalf);
        const u32 y1 = std::min(kHeight, stats.m_PeakY + kHalf);
        const f64 insideOn = LumaStdDev(withWake, x0, y0, x1, y1);
        const f64 insideOff = LumaStdDev(withoutWake, x0, y0, x1, y1);

        // The control: the same-sized box mirrored to the far side of the frame,
        // where the footprint cannot reach. Its variance must be UNCHANGED, or
        // what the measurement above found was a global shading difference
        // rather than a locally flattened surface.
        const u32 cx = (stats.m_PeakX < kWidth / 2u) ? (kWidth - kHalf - 60u) : (kHalf + 60u);
        const f64 outsideOn = LumaStdDev(withWake, cx - kHalf, y0, cx + kHalf, y1);
        const f64 outsideOff = LumaStdDev(withoutWake, cx - kHalf, y0, cx + kHalf, y1);

        std::cout << "[wake-shape] footprint stddev inside on=" << insideOn << " off=" << insideOff
                  << " | control on=" << outsideOn << " off=" << outsideOff << std::endl;

        // NEGATIVE CONTROL: a flat-calm fixture would satisfy "variance fell"
        // trivially at 0 -> 0.
        // Measured on this fixture (RTX 4090, 2026-09-07, box placed by window
        // mean): inside 0.95 without the wake, 0.74 with it — a 22% fall —
        // against a control box that is bit-identical. The bar is set from that
        // measurement, not from a guess: water shades smoothly even at 0.55 m
        // amplitude, so a stddev of "tens" (which the #967 foam captures see,
        // because foam is near-white against blue) is not available here and a
        // threshold borrowed from that test would fail on a working feature.
        //
        // The earlier recorded figures (1.10 / 0.69, 2026-08-30) came from a box
        // the single-pixel argmax happened to place, which is why they moved when
        // the estimator was fixed for #1094. They described the same frames.
        //
        // TWO THINGS TO KNOW BEFORE RETUNING THIS, both measured rather than
        // assumed. The 0.85 bar has less headroom than it did: 0.744 against a
        // bar of 0.797, ~7%, where the old box gave ~26%. That is deliberate and
        // it is not a knife edge — the same box on the same frames reproduces to
        // three significant figures across every test ordering (0.744048 vs
        // 0.745533, 0.2%), so the margin is ~35x the observed spread. The old
        // box's larger headroom was not a better test; it was a box that had
        // landed on the strongest part of the difference by accident.
        //
        // And the ratio is NOT monotonic in `kHalf` — measured 0.71 / 0.87 /
        // 0.78 / 0.73 / 0.88 at half = 20 / 30 / 45 / 60 / 80, because each size
        // selects a different window and the stddev depends on how much
        // unflattened sea the window also contains. So kHalf is left at the 45
        // this test has always used. Do NOT tune it to buy margin: a size picked
        // because it produced a comfortable number is the same guess this whole
        // section exists to replace.
        ASSERT_GT(insideOff, 0.5)
            << "the sea has no structure inside the footprint — this test cannot detect flattening";

        EXPECT_LT(insideOn, insideOff * 0.85)
            << "the sea inside the hull footprint is as textured with the wake on as without it";
        EXPECT_NEAR(outsideOn, outsideOff, std::max(1.0, outsideOff * 0.20))
            << "the sea OUTSIDE the hull footprint also changed — the suppression is not confined to "
               "the hull";
    }
} // namespace OloEngine::Tests
