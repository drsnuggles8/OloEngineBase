#include "OloEnginePCH.h"

// OLO_TEST_LAYER: cullinglod
// =============================================================================
// FoliageLodTransitionContractTest — issue #1237.
//
// Pins the math in OloEngine/src/OloEngine/Terrain/Foliage/FoliageLodTransition.h
// and, through it, the GLSL twin every foliage stage and the cull kernel
// compile (OloEditor/assets/shaders/include/FoliageLodTransition.glsl). The two
// carry the same functions with the same names; nothing makes a drift between
// them a compile error, so the contracts here are what the evidence captures
// are checked against when one moves.
//
// The four things worth asserting, and why each is the one that would break:
//
//   1. **Identity when unauthored.** Every field defaults to the value that
//      makes the whole file a no-op. A scene, a save or a test that predates
//      #1237 has to render EXACTLY as it did — the feature must be asked for.
//   2. **Coverage invariance.** `effectiveKeep * compensation^2 == 1` is the
//      whole claim of "coverage-preserving". The subtle half is that the
//      EFFECTIVE keep counts a partially-faded plant at its fade value, which
//      is the integral of the fade ramp and not `k`. Compensating by `k` alone
//      over-grows the survivors by the whole fade band — a half-right that
//      survives a screenshot.
//   3. **The hand-over still happens where the author said.** The per-instance
//      spread is centred, so its MEAN over a layer is the authored threshold.
//      An off-centre spread would move every authored ladder the moment the
//      feature was switched on, which is the one thing a transition-smoothing
//      change must not do.
//   4. **The anti-oscillation bound, as it actually is.** Hysteresis here is
//      one frame of memory (the sign of the distance derivative), not a latch,
//      so a two-frame camera oscillation still flips plants. What decorrelation
//      buys is that it flips a BOUNDED FRACTION rather than the whole layer.
//      The test asserts that bound and not an absolute guarantee the design
//      does not have — see the header.
// =============================================================================

#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageLodTransition.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // A layer of plants laid out on a line, each with the hash it would get
        // in the shader. Positions rather than indices: the hash is keyed on
        // POSITION everywhere, which is what makes it survive compaction.
        std::vector<f32> MakeLayerHashes(u32 count)
        {
            std::vector<f32> hashes;
            hashes.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                const auto fi = static_cast<f32>(i);
                hashes.push_back(FoliageLod::InstanceHash(glm::vec3(fi * 0.37f, 1.5f, fi * -0.91f)));
            }
            return hashes;
        }
    } // namespace

    // ── 1. Identity ─────────────────────────────────────────────────────────

    TEST(FoliageLodTransitionContract, AnUnauthoredLayerIsTheIdentityAtEveryDistance)
    {
        const FoliageLayer layer;
        EXPECT_FLOAT_EQ(layer.LodTransitionSpread, 0.0f);
        EXPECT_FLOAT_EQ(layer.LodHysteresis, 0.0f);
        EXPECT_FALSE(layer.LodStochasticCoverage);
        EXPECT_FALSE(layer.UseDensityLod);

        const auto params = FoliageLod::Params::Identity();
        ASSERT_FALSE(params.Enabled);
        ASSERT_FALSE(params.StochasticCoverage);
        EXPECT_FLOAT_EQ(FoliageLod::PackFlags(params), 0.0f);

        for (f32 dist = 0.0f; dist <= 500.0f; dist += 2.5f)
        {
            for (const f32 hash : { 0.0f, 0.25f, 0.5f, 0.999f })
            {
                const auto d = FoliageLod::EvaluateDensity(params, hash, dist);
                EXPECT_FLOAT_EQ(d.Alpha, 1.0f) << "dist " << dist << " hash " << hash;
                EXPECT_FLOAT_EQ(d.ScaleCompensation, 1.0f) << "dist " << dist << " hash " << hash;
            }
        }

        // A zero spread puts every plant on the authored number, which is what
        // makes the pre-#1237 hand-over bit-identical rather than merely close.
        for (const f32 hash : { 0.0f, 0.3f, 0.7f, 0.999f })
        {
            EXPECT_FLOAT_EQ(FoliageLod::TransitionDistance(30.0f, hash, 0.0f, 0.0f), 30.0f);
            EXPECT_FLOAT_EQ(FoliageLod::HysteresisOffset(30.0f, true, 0.0f), 0.0f);
            EXPECT_FLOAT_EQ(FoliageLod::HysteresisOffset(30.0f, false, 0.0f), 0.0f);
        }
    }

    TEST(FoliageLodTransitionContract, TheFlagBitfieldPacksTheTwoSwitchesIndependently)
    {
        FoliageLod::Params p;
        EXPECT_FLOAT_EQ(FoliageLod::PackFlags(p), 0.0f);
        p.Enabled = true;
        EXPECT_FLOAT_EQ(FoliageLod::PackFlags(p), 1.0f);
        p.Enabled = false;
        p.StochasticCoverage = true;
        EXPECT_FLOAT_EQ(FoliageLod::PackFlags(p), 2.0f);
        p.Enabled = true;
        EXPECT_FLOAT_EQ(FoliageLod::PackFlags(p), 3.0f);
    }

    // ── 2. Coverage preservation ────────────────────────────────────────────

    TEST(FoliageLodTransitionContract, CompensationInvertsTheEffectiveKeepFractionExactly)
    {
        // The relation the whole feature rests on, over a sweep rather than at
        // one point. maxScale is far above anything the clamp would reach so
        // the identity is tested, not the cap.
        constexpr f32 kUncapped = 1000.0f;
        for (f32 keep = 1.0f; keep > 0.02f; keep -= 0.01f)
        {
            for (const f32 fade : { 0.0f, 0.05f, 0.2f, 0.5f })
            {
                const f32 effective = FoliageLod::EffectiveKeepFraction(keep, fade);
                const f32 comp = FoliageLod::CoverageCompensation(keep, fade, kUncapped);
                EXPECT_NEAR(effective * comp * comp, 1.0f, 1e-4f)
                    << "keep " << keep << " fade " << fade;
            }
        }
    }

    TEST(FoliageLodTransitionContract, ThinningALayerPreservesItsSummedApparentCoverage)
    {
        // The relation above, but measured the way the image is: over a real
        // population of hashes, summing each survivor's fade times the area it
        // covers. This is what "apparent coverage is preserved" means, and it
        // is the assertion that would fail if the half-fade-band correction
        // were dropped — the closed-form one above would still pass.
        const std::vector<f32> hashes = MakeLayerHashes(20000);

        FoliageLod::Params params;
        params.Enabled = true;
        params.Start = 20.0f;
        params.End = 120.0f;
        params.MinFraction = 0.2f;
        params.FadeFraction = 0.12f;
        params.MaxScale = 8.0f; // well above 1/sqrt(effective keep), so never capped
        params = FoliageLod::Sanitise(params);

        const auto coverageAt = [&](f32 dist)
        {
            f64 sum = 0.0;
            for (const f32 h : hashes)
            {
                const auto d = FoliageLod::EvaluateDensity(params, h, dist);
                // Area scales with the SQUARE of the linear compensation.
                sum += static_cast<f64>(d.Alpha) * static_cast<f64>(d.ScaleCompensation) *
                       static_cast<f64>(d.ScaleCompensation);
            }
            return sum;
        };

        const f64 unthinned = coverageAt(0.0f);
        ASSERT_DOUBLE_EQ(unthinned, static_cast<f64>(hashes.size()))
            << "inside the start distance nothing is thinned and nothing is grown";

        // 1% is the sampling error of 20 000 uniform hashes against the
        // continuum the closed form describes, not a slack tolerance — the
        // uncompensated arm below misses by ~60%, three orders of magnitude out.
        for (const f32 dist : { 30.0f, 50.0f, 80.0f, 120.0f, 400.0f })
        {
            EXPECT_NEAR(coverageAt(dist) / unthinned, 1.0, 0.01) << "dist " << dist;
        }

        // The control: without the compensation the layer visibly thins. If
        // this passed too, the assertion above would be measuring nothing.
        f64 uncompensated = 0.0;
        for (const f32 h : hashes)
            uncompensated += FoliageLod::EvaluateDensity(params, h, 400.0f).Alpha;
        EXPECT_LT(uncompensated / unthinned, 0.3) << "the un-grown survivors cover far less";
    }

    TEST(FoliageLodTransitionContract, TheDensityFloorIsAFloorAndTheKeepFractionIsMonotone)
    {
        constexpr f32 kStart = 25.0f;
        constexpr f32 kEnd = 90.0f;
        constexpr f32 kFloor = 0.3f;

        f32 previous = 2.0f;
        for (f32 dist = 0.0f; dist <= 500.0f; dist += 1.0f)
        {
            const f32 keep = FoliageLod::DensityKeepFraction(dist, kStart, kEnd, kFloor);
            EXPECT_LE(keep, previous + 1e-6f) << "keep fraction must never rise with distance, at " << dist;
            EXPECT_GE(keep, kFloor - 1e-6f) << "never thins past the floor, at " << dist;
            EXPECT_LE(keep, 1.0f);
            previous = keep;
        }
        EXPECT_FLOAT_EQ(FoliageLod::DensityKeepFraction(0.0f, kStart, kEnd, kFloor), 1.0f);
        EXPECT_FLOAT_EQ(FoliageLod::DensityKeepFraction(kStart, kStart, kEnd, kFloor), 1.0f);
        EXPECT_NEAR(FoliageLod::DensityKeepFraction(kEnd, kStart, kEnd, kFloor), kFloor, 1e-6f);
    }

    TEST(FoliageLodTransitionContract, AThinningPlantFadesRatherThanVanishing)
    {
        // The per-plant pop the fade band exists to remove: walk one plant out
        // and assert its alpha never steps by more than a little between
        // adjacent metres. With a zero fade band the same walk steps by 1.
        FoliageLod::Params params;
        params.Enabled = true;
        params.Start = 10.0f;
        params.End = 60.0f;
        params.MinFraction = 0.1f;
        params.FadeFraction = 0.2f;
        params = FoliageLod::Sanitise(params);

        FoliageLod::Params hard = params;
        hard.FadeFraction = 0.0f;

        // A hash comfortably inside the range the threshold sweeps through.
        constexpr f32 kHash = 0.55f;
        f32 worstSmooth = 0.0f;
        f32 worstHard = 0.0f;
        f32 prevSmooth = FoliageLod::EvaluateDensity(params, kHash, 0.0f).Alpha;
        f32 prevHard = FoliageLod::EvaluateDensity(hard, kHash, 0.0f).Alpha;
        for (f32 dist = 0.05f; dist <= 80.0f; dist += 0.05f)
        {
            const f32 s = FoliageLod::EvaluateDensity(params, kHash, dist).Alpha;
            const f32 h = FoliageLod::EvaluateDensity(hard, kHash, dist).Alpha;
            worstSmooth = std::max(worstSmooth, std::abs(s - prevSmooth));
            worstHard = std::max(worstHard, std::abs(h - prevHard));
            prevSmooth = s;
            prevHard = h;
        }
        EXPECT_FLOAT_EQ(worstHard, 1.0f) << "a zero fade band IS the pop — the control must show it";
        EXPECT_LT(worstSmooth, 0.05f) << "with a fade band the plant dims out over metres";
    }

    // ── 3. The ladder does not move ─────────────────────────────────────────

    TEST(FoliageLodTransitionContract, TheSpreadIsCentredSoTheMeanHandoverIsTheAuthoredDistance)
    {
        const std::vector<f32> hashes = MakeLayerHashes(20000);
        constexpr f32 kNominal = 30.0f;
        constexpr f32 kSpread = 12.0f;

        f64 sum = 0.0;
        f32 lo = 1e9f;
        f32 hi = -1e9f;
        for (const f32 h : hashes)
        {
            const f32 d = FoliageLod::TransitionDistance(kNominal, h, kSpread, 0.0f);
            sum += d;
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        EXPECT_NEAR(sum / static_cast<f64>(hashes.size()), kNominal, 0.1)
            << "raising the spread must not move where the layer hands over on average";
        EXPECT_NEAR(hi - lo, kSpread, 0.05) << "the offsets span exactly the authored spread";
        EXPECT_GE(lo, kNominal - 0.5f * kSpread - 0.05f);
        EXPECT_LE(hi, kNominal + 0.5f * kSpread + 0.05f);
    }

    TEST(FoliageLodTransitionContract, TheInstanceHashIsUniformAndDoesNotCollapseOntoAGrid)
    {
        // #1254 measured the neighbouring bit-reinterpreting hash collapsing an
        // 80x80 grid onto 32 distinct values. A thinning mask built on 32
        // values deletes plants in diagonal stripes, so this hash is asserted
        // both for SPREAD and for uniformity.
        std::vector<f32> values;
        values.reserve(80 * 80);
        for (i32 x = 0; x < 80; ++x)
            for (i32 z = 0; z < 80; ++z)
                values.push_back(FoliageLod::InstanceHash(
                    glm::vec3(static_cast<f32>(x) * 0.5f, 3.25f, static_cast<f32>(z) * 0.5f)));

        std::array<u32, 16> buckets{};
        for (const f32 v : values)
        {
            ASSERT_GE(v, 0.0f);
            ASSERT_LT(v, 1.0f);
            ++buckets[std::min<sizet>(15, static_cast<sizet>(v * 16.0f))];
        }
        const auto expected = static_cast<f32>(values.size()) / 16.0f;
        for (sizet i = 0; i < buckets.size(); ++i)
        {
            EXPECT_GT(static_cast<f32>(buckets[i]), expected * 0.75f) << "bucket " << i;
            EXPECT_LT(static_cast<f32>(buckets[i]), expected * 1.25f) << "bucket " << i;
        }

        // Stability: the same position hashes the same, which is what survives
        // the GPU cull's compaction reordering the rows every frame.
        EXPECT_FLOAT_EQ(FoliageLod::InstanceHash(glm::vec3(12.5f, 3.0f, -7.25f)),
                        FoliageLod::InstanceHash(glm::vec3(12.5f, 3.0f, -7.25f)));
        EXPECT_NE(FoliageLod::InstanceHash(glm::vec3(12.5f, 3.0f, -7.25f)),
                  FoliageLod::InstanceHash(glm::vec3(12.5f, 3.0f, -7.24f)));
    }

    // ── 4. Hysteresis, and the bound it actually gives ──────────────────────

    TEST(FoliageLodTransitionContract, HysteresisMovesTheBandInTheDirectionOfTravel)
    {
        constexpr f32 kNominal = 40.0f;
        constexpr f32 kH = 0.1f;
        const f32 out = FoliageLod::HysteresisOffset(kNominal, /*receding*/ true, kH);
        const f32 in = FoliageLod::HysteresisOffset(kNominal, /*receding*/ false, kH);
        EXPECT_FLOAT_EQ(out, kNominal * kH);
        EXPECT_FLOAT_EQ(in, -kNominal * kH);
        EXPECT_GT(FoliageLod::TransitionDistance(kNominal, 0.5f, 0.0f, out),
                  FoliageLod::TransitionDistance(kNominal, 0.5f, 0.0f, in))
            << "a retreating viewer holds the near representation longer";
    }

    TEST(FoliageLodTransitionContract, HysteresisSlidesTheBandWithoutChangingItsWidth)
    {
        // The property the callers rely on, and the one a per-edge FACTOR
        // silently breaks: a factor scales both edges, so the band's WIDTH
        // scales with it too and a plant's hand-over takes measurably longer
        // walking away than walking in. This was the first implementation and
        // it disagreed with its own comment — caught in review, not by a test,
        // which is why there is now a test.
        constexpr f32 kStart = 40.0f;
        constexpr f32 kEnd = 55.0f;
        constexpr f32 kSpread = 12.0f;
        constexpr f32 kH = 0.08f;

        for (const f32 hash : { 0.0f, 0.13f, 0.5f, 0.87f, 0.999f })
        {
            const f32 outShift = FoliageLod::HysteresisOffset(kStart, true, kH);
            const f32 inShift = FoliageLod::HysteresisOffset(kStart, false, kH);

            const f32 outWidth = FoliageLod::TransitionDistance(kEnd, hash, kSpread, outShift) -
                                 FoliageLod::TransitionDistance(kStart, hash, kSpread, outShift);
            const f32 inWidth = FoliageLod::TransitionDistance(kEnd, hash, kSpread, inShift) -
                                FoliageLod::TransitionDistance(kStart, hash, kSpread, inShift);
            EXPECT_NEAR(outWidth, kEnd - kStart, 1e-3f) << "hash " << hash;
            EXPECT_NEAR(inWidth, kEnd - kStart, 1e-3f) << "hash " << hash;
        }

        // And the coverage it produces is the same curve, shifted: a plant
        // crossing outward and the same plant crossing inward spend the same
        // distance mid-hand-over.
        const auto bandTravel = [&](bool receding)
        {
            f32 entered = -1.0f;
            f32 left = -1.0f;
            for (f32 d = 0.0f; d < 120.0f; d += 0.01f)
            {
                const f32 prev = receding ? d - 1.0f : d + 1.0f;
                const f32 c = FoliageLod::MeshCoverageLod(d, prev, kStart, kEnd, 0.5f, kSpread, kH);
                if (c < 1.0f && entered < 0.0f)
                    entered = d;
                if (c <= 0.0f && entered >= 0.0f && left < 0.0f)
                    left = d;
            }
            return left - entered;
        };
        EXPECT_NEAR(bandTravel(true), bandTravel(false), 0.05f)
            << "the hand-over takes a different distance depending on which way the viewer is moving";
    }

    TEST(FoliageLodTransitionContract, DecorrelationBoundsHowMuchOfALayerACameraWobbleCanFlip)
    {
        // The honest statement of the anti-oscillation property. A camera
        // oscillating across the threshold flips the plants whose OWN threshold
        // lies inside its travel — with no spread that is the whole layer (the
        // ring), and with a spread it is roughly amplitude/spread of it.
        const std::vector<f32> hashes = MakeLayerHashes(20000);
        constexpr f32 kNominal = 40.0f;
        constexpr f32 kAmplitude = 0.5f; // metres of camera wobble

        const auto flippedFraction = [&](f32 spread)
        {
            u32 flipped = 0;
            for (const f32 h : hashes)
            {
                // Two frames of a wobble: out to +amplitude (receding), then
                // back to -amplitude (approaching). A plant "flips" when the
                // two frames disagree about which side of its own threshold it
                // is on.
                const f32 outward = FoliageLod::TransitionDistance(
                    kNominal, h, spread, FoliageLod::HysteresisOffset(kNominal, true, 0.05f));
                const f32 inward = FoliageLod::TransitionDistance(
                    kNominal, h, spread, FoliageLod::HysteresisOffset(kNominal, false, 0.05f));
                const bool nearOnTheWayOut = (kNominal + kAmplitude) < outward;
                const bool nearOnTheWayIn = (kNominal - kAmplitude) < inward;
                if (nearOnTheWayOut != nearOnTheWayIn)
                    ++flipped;
            }
            return static_cast<f32>(flipped) / static_cast<f32>(hashes.size());
        };

        const f32 withoutSpread = flippedFraction(0.0f);
        const f32 withSpread = flippedFraction(20.0f);

        EXPECT_GT(withoutSpread, 0.99f)
            << "with every plant on one threshold a wobble flips the WHOLE layer — the ring";
        EXPECT_LT(withSpread, 0.25f) << "decorrelated, only the plants near their own threshold flip";
        // The bound the header claims. The hysteresis widens the interval the
        // two frames disagree over beyond the raw 2*amplitude, so the bound is
        // stated against the hysteretic width rather than the wobble alone.
        constexpr f32 kHystereticWidth = 2.0f * kAmplitude + 2.0f * 0.05f * kNominal;
        EXPECT_LE(withSpread, kHystereticWidth / 20.0f + 0.02f)
            << "the flipping fraction is bounded by the disagreement width over the spread";
    }

    // ── Sanitisation ────────────────────────────────────────────────────────

    TEST(FoliageLodTransitionContract, SanitiseNeverLetsANonFiniteAuthoredValueReachTheShaders)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        FoliageLod::Params hostile;
        hostile.Enabled = true;
        hostile.StochasticCoverage = true;
        hostile.Start = nan;
        hostile.End = -inf;
        hostile.MinFraction = nan;
        hostile.FadeFraction = inf;
        hostile.MaxScale = nan;
        hostile.TransitionSpread = -inf;
        hostile.Hysteresis = nan;

        const auto clean = FoliageLod::Sanitise(hostile);
        for (const f32 v : { clean.Start, clean.End, clean.MinFraction, clean.FadeFraction, clean.MaxScale,
                             clean.TransitionSpread, clean.Hysteresis })
        {
            EXPECT_TRUE(std::isfinite(v));
        }
        EXPECT_GE(clean.End, clean.Start) << "an inverted band makes a smoothstep undefined";
        EXPECT_GE(clean.MinFraction, FoliageLod::kMinKeepFraction) << "guards the reciprocal square root";
        EXPECT_GE(clean.MaxScale, 1.0f) << "the compensation may never shrink a plant";
        EXPECT_TRUE(clean.Enabled);
        EXPECT_TRUE(clean.StochasticCoverage) << "sanitising must not silently switch the feature off";

        // And the sanitised numbers produce finite results everywhere.
        for (f32 dist = 0.0f; dist <= 500.0f; dist += 5.0f)
        {
            const auto d = FoliageLod::EvaluateDensity(clean, 0.5f, dist);
            EXPECT_TRUE(std::isfinite(d.Alpha));
            EXPECT_TRUE(std::isfinite(d.ScaleCompensation));
            EXPECT_GE(d.ScaleCompensation, 1.0f);
        }
    }

    TEST(FoliageLodTransitionContract, SanitiseKeepsAnInvertedAuthoredBandUsable)
    {
        FoliageLod::Params inverted;
        inverted.Enabled = true;
        inverted.Start = 90.0f;
        inverted.End = 20.0f; // author dragged End below Start
        const auto clean = FoliageLod::Sanitise(inverted);
        EXPECT_FLOAT_EQ(clean.Start, 90.0f);
        EXPECT_FLOAT_EQ(clean.End, 90.0f) << "collapses to a hard step at Start rather than inverting";

        // The keep fraction is still monotone across the collapsed band.
        EXPECT_FLOAT_EQ(FoliageLod::DensityKeepFraction(0.0f, clean.Start, clean.End, clean.MinFraction), 1.0f);
        EXPECT_FLOAT_EQ(FoliageLod::DensityKeepFraction(200.0f, clean.Start, clean.End, clean.MinFraction),
                        clean.MinFraction);
    }
} // namespace OloEngine
