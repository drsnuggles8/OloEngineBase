// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// VRCSClassifierGpuTest.cpp — the real VRCSClassify.comp, on the real driver.
//
// Issue #683. The classifier decides where the engine is allowed to shade one
// pixel in four. Its failure mode is not a crash and not a wrong number: it is a
// tile that SHOULD have stayed full rate being coarsened, which shows up as a
// blocky silhouette or a smeared crease in one scene, at one camera angle, in
// one build. So the properties below are all of the shape "this tile must NOT
// coarsen", driven with synthetic depth and normal buffers where the right
// answer is known by construction rather than measured from a frame.
//
// EVERY ASSERTION IS A CONSERVATISM CLAIM. The test never demands that a tile DO
// coarsen except in the one flat-wall case that proves the feature is not simply
// inert — because "refused to coarsen" costs time and "coarsened wrongly" costs
// image quality, and only one of those is a bug worth failing a build over.
//
// Synthetic inputs, not a rendered scene, on purpose: a rendered scene's depth
// and normals are correct-but-unknown, so an assertion against them can only
// restate whatever the classifier produced. Here a step edge is a step edge.
//
// Classification: shaderpipe (a real compute shader dispatched on the GPU).
// SKIPs cleanly with no GL 4.6 context, so headless CI is a no-op.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/VRCS/ShadingRateClassifier.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 256;
        constexpr u32 kTile = ShadingRateClassifier::kTileSize;
        constexpr u32 kTilesX = kWidth / kTile;
        constexpr u32 kTilesY = kHeight / kTile;

        // A standard GL perspective projection's two depth-linearisation
        // coefficients for near = 1, far = 101:
        //     A = proj[2][2] = -(f + n) / (f - n) = -1.02
        //     B = proj[3][2] = -2fn   / (f - n)   = -2.02
        // and the shader recovers linear view distance as B / (ndc + A), with
        // ndc = 2 * deviceZ - 1. Named here so the depths chosen below can be
        // reasoned about as distances rather than as opaque constants.
        constexpr f32 kDepthLinearizeA = -1.02f;
        constexpr f32 kDepthLinearizeB = -2.02f;

        [[nodiscard]] f32 LinearizeDepth(f32 deviceZ) noexcept
        {
            const f32 ndc = deviceZ * 2.0f - 1.0f;
            return kDepthLinearizeB / (ndc + kDepthLinearizeA);
        }

        // The inverse of the above, so a test can specify the INVERSE DEPTH the
        // classifier actually fits its plane through and let the fixture work
        // back to a device Z. Specifying device Z directly is what made the
        // first version of the sweep below useless: a linear ramp in device Z
        // is also linear in inverse depth, so its plane-fit residual is zero
        // everywhere and every tile answered the same.
        [[nodiscard]] f32 DeviceZFromInverseDepth(f32 invDepth) noexcept
        {
            const f32 ndc = invDepth * kDepthLinearizeB - kDepthLinearizeA;
            return (ndc + 1.0f) * 0.5f;
        }

        // Inverse depth of the reference plane the sweep ripples around.
        const f32 kBaseInverseDepth = 1.0f / LinearizeDepth(0.5f);

        // Octahedral encoding, matching OctDecode in the shader (and every other
        // engine consumer of the normal RT). Only the +Z hemisphere is needed
        // here, where the encode is a plain divide by the L1 norm.
        struct EncodedNormal
        {
            f32 X = 0.0f;
            f32 Y = 0.0f;
        };

        [[nodiscard]] EncodedNormal OctEncodeUpperHemisphere(f32 nx, f32 ny, f32 nz) noexcept
        {
            const f32 l1 = std::abs(nx) + std::abs(ny) + std::abs(nz);
            return { nx / l1, ny / l1 };
        }

        // The engine-wide "no meaningful surface normal here" sentinel, written
        // by the grid / particle / Renderer2D shaders. The classifier must treat
        // a tile containing one as unclassifiable.
        constexpr EncodedNormal kNormalSentinel{ -2.0f, -2.0f };

        // A synthetic frame: one device-Z per pixel and one encoded normal per
        // pixel, uploaded as the two textures the classifier reads.
        struct SyntheticFrame
        {
            std::vector<f32> Depth = std::vector<f32>(static_cast<std::size_t>(kWidth) * kHeight, 0.5f);
            std::vector<f32> Normals = std::vector<f32>(static_cast<std::size_t>(kWidth) * kHeight * 2u, 0.0f);

            void SetDepth(u32 x, u32 y, f32 deviceZ)
            {
                Depth[static_cast<std::size_t>(y) * kWidth + x] = deviceZ;
            }
            void SetNormal(u32 x, u32 y, EncodedNormal n)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 2u;
                Normals[i] = n.X;
                Normals[i + 1] = n.Y;
            }
            void Fill(f32 deviceZ, EncodedNormal n)
            {
                for (u32 y = 0; y < kHeight; ++y)
                    for (u32 x = 0; x < kWidth; ++x)
                    {
                        SetDepth(x, y, deviceZ);
                        SetNormal(x, y, n);
                    }
            }
        };

        // Everything a single classification run needs, torn down together. The
        // classifier owns its rate image, so nothing here outlives the test.
        struct ClassifyRun
        {
            ShadingRateClassifier Classifier;
            Ref<Texture2D> DepthTexture;
            Ref<Texture2D> NormalTexture;

            bool Setup(SyntheticFrame& frame)
            {
                TextureSpecification depthSpec;
                depthSpec.Width = kWidth;
                depthSpec.Height = kHeight;
                depthSpec.Format = ImageFormat::R32F;
                depthSpec.GenerateMips = false;
                DepthTexture = Texture2D::Create(depthSpec);

                TextureSpecification normalSpec;
                normalSpec.Width = kWidth;
                normalSpec.Height = kHeight;
                normalSpec.Format = ImageFormat::RG16F;
                normalSpec.GenerateMips = false;
                NormalTexture = Texture2D::Create(normalSpec);

                if (!DepthTexture || !NormalTexture)
                    return false;

                DepthTexture->SetData(frame.Depth.data(),
                                      static_cast<u32>(frame.Depth.size() * sizeof(f32)));
                // RG16F uploads as GL_FLOAT (8 bytes/texel) and the driver
                // converts — see OpenGLTexture's SetData format table.
                NormalTexture->SetData(frame.Normals.data(),
                                       static_cast<u32>(frame.Normals.size() * sizeof(f32)));

                Classifier.Initialize();
                Classifier.Resize(kWidth, kHeight);
                return Classifier.IsValid();
            }

            // Classify and read the rates back. `frameIndex` must differ between
            // calls on the same instance or the once-per-frame memoisation
            // returns the previous result — which is the behaviour, not a bug,
            // and is itself asserted below.
            std::vector<u8> Run(u64 frameIndex, const ShadingRateClassifier::Thresholds& thresholds)
            {
                ShadingRateClassifier::Inputs inputs;
                inputs.SceneDepth = DepthTexture->GetRHIHandle();
                inputs.ViewNormals = NormalTexture->GetRHIHandle();
                inputs.SceneDepthLifetime = RHI::HeapSlotLifetime::Persistent;
                inputs.ViewNormalsLifetime = RHI::HeapSlotLifetime::Persistent;
                inputs.DepthLinearizeA = kDepthLinearizeA;
                inputs.DepthLinearizeB = kDepthLinearizeB;

                if (!Classifier.Classify(frameIndex, inputs, thresholds))
                    return {};

                ::glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                std::vector<u8> rates(static_cast<std::size_t>(kTilesX) * kTilesY, 0u);
                ::glGetTextureImage(Classifier.GetRateTextureResource()->GetRendererID(), 0, GL_RED_INTEGER,
                                    GL_UNSIGNED_BYTE, static_cast<GLsizei>(rates.size()), rates.data());
                return rates;
            }
        };

        [[nodiscard]] ShadingRateClassifier::Thresholds DefaultThresholds() noexcept
        {
            // The shipped PostProcessSettings defaults, restated rather than
            // pulled from a settings object so a defaults change is a visible
            // test edit rather than a silent change of what is being tested.
            ShadingRateClassifier::Thresholds t;
            t.Depth = 0.01f;
            t.Normal = 0.02f;
            t.Luma = 0.25f;
            t.Coarse4x4Scale = 0.25f;
            t.Allow4x4 = false;
            return t;
        }

        [[nodiscard]] u8 RateAt(const std::vector<u8>& rates, u32 tileX, u32 tileY)
        {
            return rates[static_cast<std::size_t>(tileY) * kTilesX + tileX];
        }

        [[nodiscard]] u32 CountCoarse(const std::vector<u8>& rates)
        {
            return static_cast<u32>(std::count_if(rates.begin(), rates.end(), [](u8 r)
                                                  { return r > 1u; }));
        }

        // A frame whose NON-PLANARITY sweeps from none to plenty down the
        // screen: inverse depth ripples sinusoidally across x with an amplitude
        // that grows with y, so tile rows near the top fit a plane almost
        // exactly and rows near the bottom do not. Normals are held constant
        // throughout so the normal term contributes nothing and the tests below
        // are about the depth term alone.
        //
        // A ripple rather than a ramp or a step, because the two threshold
        // tests need a CONTINUUM of metric values — a step gives two answers
        // and a ramp gives one (a ramp is planar, so its residual is zero).
        [[nodiscard]] SyntheticFrame MakeNonPlanaritySweep()
        {
            SyntheticFrame frame;
            const EncodedNormal facing = OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f);
            constexpr f32 kRipplePeriodPixels = 16.0f;
            constexpr f32 kMaxAmplitude = 0.02f; // ~4% of the base inverse depth
            for (u32 y = 0; y < kHeight; ++y)
            {
                const f32 amplitude = kMaxAmplitude * (static_cast<f32>(y) / static_cast<f32>(kHeight));
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const f32 phase = 6.283185307f * static_cast<f32>(x) / kRipplePeriodPixels;
                    const f32 w = kBaseInverseDepth + amplitude * std::sin(phase);
                    frame.SetDepth(x, y, DeviceZFromInverseDepth(w));
                    frame.SetNormal(x, y, facing);
                }
            }
            return frame;
        }
    } // namespace

    // =========================================================================
    // The feature is not inert: a genuinely flat, uniformly-oriented surface
    // coarsens. Without this every other assertion here is satisfied by a
    // classifier that returns 1x1 unconditionally.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, AFlatUniformlyOrientedSurfaceCoarsens)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame;
        frame.Fill(0.5f, OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f));

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame)) << "classifier failed to initialise (shader compile?)";

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_EQ(rates.size(), static_cast<std::size_t>(kTilesX) * kTilesY);

        EXPECT_EQ(CountCoarse(rates), kTilesX * kTilesY)
            << "a constant-depth, constant-normal frame has nothing for the thresholds to reject";
        for (u8 r : rates)
            EXPECT_EQ(r, ShadingRateClassifier::kRate2x2) << "4x4 was not permitted, so 2x2 is the ceiling";
    }

    // =========================================================================
    // A STEEPLY RECEDING PLANE COARSENS. This is the regression test for the
    // defect the rate heatmap exposed on the first working build.
    //
    // The original metric was the tile's depth RANGE relative to its nearest
    // surface. A ground plane viewed at a grazing angle genuinely spans a large
    // depth range across 8 pixels, so that metric rejected every ground tile in
    // the scene — the heatmap showed the sky and the cube's camera-facing side
    // coarsening while the entire floor, and the cube's grazing top face,
    // stayed at full rate. Nothing looked wrong: the image was correct, the
    // image-diff and tile-seam assertions all passed, and the feature simply
    // did almost nothing on the surfaces that cover most of an outdoor frame.
    //
    // The fix was to measure DEPARTURE FROM A PLANE instead of depth range. A
    // plane has a zero residual at any angle; only a discontinuity has a large
    // one. This test is the shape the old metric fails on and the new one
    // passes: no step, no curvature, just a lot of slope.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, ASteeplyRecedingPlaneStillCoarsens)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame;
        const EncodedNormal facing = OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f);
        // Inverse depth falls linearly with y from the base plane to a tenth of
        // it — the screen-space signature of a floor running to the horizon.
        // Linear in INVERSE depth is what "planar" means, so this is a plane.
        for (u32 y = 0; y < kHeight; ++y)
        {
            const f32 t = static_cast<f32>(y) / static_cast<f32>(kHeight - 1u);
            const f32 w = kBaseInverseDepth * (1.0f - 0.9f * t);
            for (u32 x = 0; x < kWidth; ++x)
            {
                frame.SetDepth(x, y, DeviceZFromInverseDepth(w));
                frame.SetNormal(x, y, facing);
            }
        }

        // Confirm the fixture really is the hard case: the depth RANGE inside a
        // single tile is far past the 1% tolerance, so a range-based classifier
        // would reject these tiles. Only a planarity test can accept them.
        {
            const f32 wTop = kBaseInverseDepth;
            const f32 wEighthRow = kBaseInverseDepth * (1.0f - 0.9f * (8.0f / static_cast<f32>(kHeight - 1u)));
            const f32 nearestZ = 1.0f / wTop;
            const f32 rangeZ = (1.0f / wEighthRow) - nearestZ;
            ASSERT_GT(rangeZ / nearestZ, 0.01f)
                << "the synthetic plane is not steep enough to have failed the old range metric, so this "
                   "test would pass without the fix";
        }

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_FALSE(rates.empty());

        // Every tile except the last row (partial tiles never coarsen, and the
        // last row here is full, so in fact every tile).
        EXPECT_EQ(CountCoarse(rates), kTilesX * kTilesY)
            << "a flat plane was rejected because it recedes — the range-metric regression";
    }

    // =========================================================================
    // A depth step inside a tile is a silhouette. Those tiles must stay full
    // rate; the tiles either side of it, being flat, must not be punished for
    // the step's existence — a classifier that answers 1x1 everywhere as soon as
    // the frame contains any edge is safe and useless.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, ADepthStepInsideATileKeepsThatTileFullRate)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The step lands at x = 100, which is 4 pixels into tile column 12 —
        // deliberately NOT on a tile boundary, so exactly one column of tiles
        // straddles it and the neighbours are uniform.
        constexpr u32 kStepX = 100u;
        constexpr u32 kStepTile = kStepX / kTile; // 12
        static_assert(kStepX % kTile != 0u, "the step must be inside a tile, not on its boundary");

        SyntheticFrame frame;
        const EncodedNormal facing = OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f);
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                frame.SetDepth(x, y, x < kStepX ? 0.5f : 0.6f);
                frame.SetNormal(x, y, facing);
            }
        }
        // Sanity on the fixture itself: the step has to be far bigger than the
        // tolerance, or this test would pass for the wrong reason. Stated in
        // inverse depth because that is the space the plane fit works in.
        const f32 nearW = 1.0f / LinearizeDepth(0.5f);
        const f32 farW = 1.0f / LinearizeDepth(0.6f);
        ASSERT_GT(std::abs(farW - nearW) / nearW, 0.05f)
            << "the synthetic step is too small to read as a discontinuity";

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_FALSE(rates.empty());

        for (u32 ty = 0; ty < kTilesY; ++ty)
        {
            EXPECT_EQ(RateAt(rates, kStepTile, ty), ShadingRateClassifier::kRate1x1)
                << "tile column " << kStepTile << " straddles the depth step and must not coarsen (row " << ty
                << ")";
            EXPECT_GT(RateAt(rates, kStepTile - 2u, ty), 1u)
                << "a uniform tile two columns from the step must still coarsen (row " << ty << ")";
            EXPECT_GT(RateAt(rates, kStepTile + 2u, ty), 1u)
                << "a uniform tile two columns past the step must still coarsen (row " << ty << ")";
        }
    }

    // =========================================================================
    // Normals catch what depth cannot. A crease is continuous in depth and
    // discontinuous in shading, so a depth-only classifier would coarsen
    // straight across it.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, ANormalCreaseAtConstantDepthKeepsThatTileFullRate)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kCreaseX = 100u;
        constexpr u32 kCreaseTile = kCreaseX / kTile;

        SyntheticFrame frame;
        const EncodedNormal facing = OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f);
        const EncodedNormal sideways = OctEncodeUpperHemisphere(1.0f, 0.0f, 0.0f);
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                frame.SetDepth(x, y, 0.5f); // depth is CONSTANT across the whole frame
                frame.SetNormal(x, y, x < kCreaseX ? facing : sideways);
            }
        }

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_FALSE(rates.empty());

        for (u32 ty = 0; ty < kTilesY; ++ty)
        {
            EXPECT_EQ(RateAt(rates, kCreaseTile, ty), ShadingRateClassifier::kRate1x1)
                << "the 90-degree normal crease is invisible to depth; only the normal term can see it (row "
                << ty << ")";
        }
    }

    // =========================================================================
    // A tile that mixes sky and geometry IS a silhouette, and its linearised
    // depth range is not reliably large (the far plane linearises to a finite
    // distance). The classifier rejects it on the mix itself, not on the range.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, ATileMixingSkyAndGeometryStaysFullRate)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kHorizonY = 100u;
        constexpr u32 kHorizonTile = kHorizonY / kTile;

        SyntheticFrame frame;
        const EncodedNormal facing = OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f);
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                frame.SetDepth(x, y, y < kHorizonY ? 1.0f : 0.5f); // 1.0 = far plane = sky
                frame.SetNormal(x, y, facing);
            }
        }

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_FALSE(rates.empty());

        for (u32 tx = 0; tx < kTilesX; ++tx)
        {
            EXPECT_EQ(RateAt(rates, tx, kHorizonTile), ShadingRateClassifier::kRate1x1)
                << "tile row " << kHorizonTile << " straddles the horizon (column " << tx << ")";
        }
        // Pure sky above and pure geometry below are both uniform, and both
        // coarsen — the rejection above is about the MIX, not about sky.
        EXPECT_GT(RateAt(rates, 0u, kHorizonTile - 2u), 1u) << "pure sky is uniform";
        EXPECT_GT(RateAt(rates, 0u, kHorizonTile + 2u), 1u) << "pure geometry is uniform";
    }

    // =========================================================================
    // The (-2,-2) sentinel means "no normal here". A tile containing one cannot
    // reason about normal agreement, so it must not coarsen — this is the case
    // that used to bake the editor grid into GTAO's AO buffer before GTAO.comp
    // learned the same sentinel.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, ASentinelNormalPoisonsItsTile)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame;
        frame.Fill(0.5f, OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f));
        // ONE pixel, in the middle of one tile. Everything else is flat.
        constexpr u32 kPoisonTileX = 7u;
        constexpr u32 kPoisonTileY = 9u;
        frame.SetNormal(kPoisonTileX * kTile + 3u, kPoisonTileY * kTile + 4u, kNormalSentinel);

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        const std::vector<u8> rates = run.Run(1u, DefaultThresholds());
        ASSERT_FALSE(rates.empty());

        EXPECT_EQ(RateAt(rates, kPoisonTileX, kPoisonTileY), ShadingRateClassifier::kRate1x1)
            << "a single sentinel normal makes the tile's mean normal meaningless";
        EXPECT_GT(RateAt(rates, kPoisonTileX + 1u, kPoisonTileY), 1u)
            << "the poison must not spread to a neighbouring tile";
    }

    // =========================================================================
    // 4x4 is a strict subset of 2x2. The shader tests 4x4 only after 2x2 has
    // passed and at a tighter tolerance, so a tile can never be 4x4-eligible
    // while being 2x2-ineligible — which is what makes "turn off 4x4" a safe,
    // strictly-more-conservative lever rather than a different classification.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, FourByFourIsAStrictSubsetOfTwoByTwo)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame = MakeNonPlanaritySweep();

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        auto thresholds = DefaultThresholds();
        thresholds.Allow4x4 = false;
        const std::vector<u8> without4x4 = run.Run(1u, thresholds);
        ASSERT_FALSE(without4x4.empty());

        thresholds.Allow4x4 = true;
        const std::vector<u8> with4x4 = run.Run(2u, thresholds);
        ASSERT_EQ(with4x4.size(), without4x4.size());

        u32 sawFourByFour = 0;
        for (std::size_t i = 0; i < with4x4.size(); ++i)
        {
            if (with4x4[i] == ShadingRateClassifier::kRate4x4)
            {
                ++sawFourByFour;
                EXPECT_EQ(without4x4[i], ShadingRateClassifier::kRate2x2)
                    << "tile " << i << " went 4x4 but is not even 2x2-eligible";
            }
            else
            {
                EXPECT_EQ(with4x4[i], without4x4[i])
                    << "tile " << i << ": permitting 4x4 changed a non-4x4 tile's rate";
            }
        }
        EXPECT_GT(sawFourByFour, 0u) << "the ramp produced no 4x4 tiles, so the subset claim is vacuous";
    }

    // =========================================================================
    // Tightening a threshold never produces MORE coarse tiles. The thresholds
    // are the operator's safety lever — "if VRCS is smearing something, turn
    // this down" only works if turning it down is monotone.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, TighteningTheDepthThresholdIsMonotone)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame = MakeNonPlanaritySweep();

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        auto loose = DefaultThresholds();
        loose.Depth = 0.05f;
        const std::vector<u8> looseRates = run.Run(1u, loose);
        ASSERT_FALSE(looseRates.empty());

        auto tight = DefaultThresholds();
        tight.Depth = 0.0005f;
        const std::vector<u8> tightRates = run.Run(2u, tight);
        ASSERT_EQ(tightRates.size(), looseRates.size());

        for (std::size_t i = 0; i < tightRates.size(); ++i)
        {
            EXPECT_LE(tightRates[i], looseRates[i])
                << "tile " << i << " coarsened MORE at a tighter depth threshold";
        }
        EXPECT_LT(CountCoarse(tightRates), CountCoarse(looseRates))
            << "the two thresholds produced identical results, so monotonicity is untested here";
    }

    // =========================================================================
    // One classification per frame. A second consumer asking for the same frame
    // index gets the first one's rates rather than a redundant dispatch — the
    // reason this is one shared utility instead of a classify pass per consumer.
    // =========================================================================
    TEST(VRCSClassifierGpuTest, TheSameFrameIndexReusesTheExistingClassification)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SyntheticFrame frame;
        frame.Fill(0.5f, OctEncodeUpperHemisphere(0.0f, 0.0f, 1.0f));

        ClassifyRun run;
        ASSERT_TRUE(run.Setup(frame));

        auto thresholds = DefaultThresholds();
        const std::vector<u8> first = run.Run(7u, thresholds);
        ASSERT_FALSE(first.empty());
        ASSERT_EQ(first.front(), ShadingRateClassifier::kRate2x2) << "flat frame, 4x4 not permitted";

        // Permitting 4x4 on this frame WOULD change every tile from 2x2 to 4x4
        // — it is flat, so it clears the tighter tolerance too. Asking again for
        // the SAME frame index must therefore return the old 2x2 rates: the
        // change is decisive, so an unchanged result can only mean the dispatch
        // was skipped, not that the two runs happened to agree.
        thresholds.Allow4x4 = true;
        const std::vector<u8> sameFrame = run.Run(7u, thresholds);
        EXPECT_EQ(sameFrame, first) << "a second Classify() for the same frame re-dispatched";

        const std::vector<u8> nextFrame = run.Run(8u, thresholds);
        EXPECT_NE(nextFrame, first) << "a new frame index must re-classify with the new thresholds";
        EXPECT_EQ(nextFrame.front(), ShadingRateClassifier::kRate4x4);
    }
} // namespace OloEngine::Tests
