#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCoatShadowDeformedPropertyTests — issue #1426.
//
// A coat bound to a moving body is baked from the strands the pass DRAWS, in
// groom object space, and rebuilt when those strands drift past a bound. This
// file pins the three things that decision rests on, on the CPU and without a
// GPU, so it runs in CI:
//
//   1. THE SPACE. A strand carried by the body is shadowed by the same
//      neighbours it had at rest — and a volume left at the rest pose is
//      measurably wrong for it, which is what makes the first claim mean
//      anything (the negative control).
//   2. THE BOUND. Drift is the largest centreline displacement, an
//      incomparable pose is infinitely far, and the rebake and stale bounds
//      are exclusive and fail towards "rebuild" and "do not sample".
//   3. THE CADENCE. Measured, not argued: a drift bound and a frame-count
//      cadence are run over the same swinging limb and compared on rebuilds
//      (cost) and on transmittance error against a fresh bake (lag). The
//      numbers docs/agent-rules/groom-deformed-coat-self-shadowing.md quotes are the
//      ones this prints.
//   4. THE SUBSET (#1445). A walking coat is baked from a hashed subset of its
//      segments, radii scaled; measured against the exact answer over strides
//      down past rule 4's floor, which is where the shipped target comes from.
//
// Everything is deterministic: GroomStrandFixture's integer hash, no <random>.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "Groom/GroomStrandFixture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomCoatShadow;

namespace
{
    // The scenes' authored width scale and the component's authored kappa, so
    // the transmittances compared here are in the range a real coat renders in.
    constexpr f32 kWidthScale = 25.0f;
    constexpr f32 kKappa = 4.0f;
    // The shipped march: three voxels a step (rule 5).
    constexpr f32 kStepVoxels = 3.0f;

    [[nodiscard]] std::vector<GroomStrandVertex> DrawnStream(const GroomAsset& groom)
    {
        GroomStrandBuildSettings settings;
        settings.MaxStrands = 1000000u;
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        (void)BuildGroomStrandMesh(groom, settings, vertices, indices);
        return vertices;
    }

    // Moves every segment whose REST midpoint satisfies `select` by `offset`,
    // all four corners and every position lane, the way a bound build moves a
    // strand with its root. Segment-granular rather than strand-granular: the
    // density a probe sees depends on where segments are, not on which strand
    // owns them.
    template<typename Select>
    [[nodiscard]] std::vector<GroomStrandVertex> Displaced(const std::vector<GroomStrandVertex>& rest,
                                                           const glm::vec3& offset, Select select)
    {
        std::vector<GroomStrandVertex> out = rest;
        for (sizet s = 0; s + 3 < out.size(); s += 4)
        {
            const glm::vec3 mid = (rest[s].Position + rest[s + 2].Position) * 0.5f;
            if (!select(mid))
            {
                continue;
            }
            for (sizet c = 0; c < 4; ++c)
            {
                out[s + c].Position += offset;
                out[s + c].Other += offset;
                out[s + c].PrevPosition += offset;
            }
        }
        return out;
    }

    [[nodiscard]] DensityVolume Bake(const std::vector<GroomStrandVertex>& stream, u32 resolution)
    {
        std::vector<CoatSegment> segments;
        (void)BuildCoatSegmentsFromStrandVertices(stream, kWidthScale, segments);
        DensityVolumeSettings settings;
        settings.Resolution = resolution;
        DensityVolume volume;
        (void)BuildDensityVolume(segments, settings, volume);
        return volume;
    }

    [[nodiscard]] f32 Transmittance(const DensityVolume& volume, const glm::vec3& origin, const glm::vec3& direction)
    {
        return CoatTransmittance(SampleDensityVolume(volume, origin, direction, true, kStepVoxels), kKappa);
    }

    [[nodiscard]] f32 SmallestVoxel(const DensityVolume& volume)
    {
        const glm::vec3 v = volume.VoxelSize();
        return std::min({ v.x, v.y, v.z });
    }

    struct Pelt
    {
        Ref<GroomAsset> Groom;
        std::vector<GroomStrandVertex> Rest;
        std::vector<CoatSegment> RestSegments;
    };

    [[nodiscard]] Pelt MakePelt(u32 strands)
    {
        Pelt pelt;
        const auto made = Tests::GroomStrandFixture::MakePelt(strands, 6u);
        pelt.Groom = made.Groom;
        if (pelt.Groom)
        {
            pelt.Rest = DrawnStream(*pelt.Groom);
            (void)BuildCoatSegmentsFromStrandVertices(pelt.Rest, kWidthScale, pelt.RestSegments);
        }
        return pelt;
    }

    struct ProbeError
    {
        f64 Mean = 0.0;
        f64 Max = 0.0;
        f64 MeanTruthDarkening = 0.0;
    };

    // Mean and max |T_candidate - T_truth| over probes that have moved by
    // `offset`, where T_truth is the transmittance the REST coat gives the
    // same probe at its rest position. MeanTruthDarkening is 1 - mean(T_truth):
    // the size of the effect being compared, so an error is always quoted
    // against something rather than against zero.
    [[nodiscard]] ProbeError CompareMoved(const DensityVolume& candidate, const DensityVolume& restVolume,
                                          const std::vector<CoatProbe>& probes, const glm::vec3& offset)
    {
        ProbeError e;
        if (probes.empty())
        {
            return e;
        }
        for (const CoatProbe& probe : probes)
        {
            const f32 truth = Transmittance(restVolume, probe.Position, probe.Direction);
            const f32 got = Transmittance(candidate, probe.Position + offset, probe.Direction);
            const f64 err = std::abs(static_cast<f64>(got) - static_cast<f64>(truth));
            e.Mean += err;
            e.Max = std::max(e.Max, err);
            e.MeanTruthDarkening += 1.0 - static_cast<f64>(truth);
        }
        e.Mean /= static_cast<f64>(probes.size());
        e.MeanTruthDarkening /= static_cast<f64>(probes.size());
        return e;
    }
} // namespace

// ── 1. The space ─────────────────────────────────────────────────────────────

TEST(GroomCoatShadowDeformed, TheDrawnStreamAtRestIsTheSameCoatAsTheRestCurves)
{
    // A bound coat at its bind pose and the same coat unbound must get the SAME
    // volume, or binding a groom would change its shadow while nothing moved.
    // Compared voxel by voxel rather than segment by segment, because the mesh
    // may walk curves in role order and the density is what the shader reads.
    const Pelt pelt = MakePelt(2000u);
    ASSERT_TRUE(pelt.Groom);

    CoatSampleSettings sample;
    sample.MaxStrands = 1000000u;
    sample.WidthScale = kWidthScale;
    std::vector<CoatSegment> fromCurves;
    ASSERT_GT(BuildCoatSegments(*pelt.Groom, glm::mat4(1.0f), sample, fromCurves), 0u);
    ASSERT_EQ(pelt.RestSegments.size(), fromCurves.size()) << "the drawn stream and the rest curves disagree on the "
                                                              "number of segments";

    DensityVolumeSettings settings;
    settings.Resolution = 48u;
    DensityVolume a;
    DensityVolume b;
    ASSERT_TRUE(BuildDensityVolume(pelt.RestSegments, settings, a));
    ASSERT_TRUE(BuildDensityVolume(fromCurves, settings, b));
    ASSERT_EQ(a.Dimensions, b.Dimensions);

    f64 worst = 0.0;
    f64 peak = 0.0;
    for (sizet i = 0; i < a.Density.size(); ++i)
    {
        worst = std::max(worst, std::abs(static_cast<f64>(a.Density[i]) - static_cast<f64>(b.Density[i])));
        peak = std::max(peak, static_cast<f64>(b.Density[i]));
    }
    ASSERT_GT(peak, 0.0);
    EXPECT_LE(worst, peak * 1.0e-5) << "the two sources bake different coats";
}

TEST(GroomCoatShadowDeformed, ACoatCarriedWholeByTheBodyKeepsItsShadowExactly)
{
    // Root motion that lives in the SKIN rather than in the entity transform
    // moves the whole coat inside groom object space. The bake's bounds move
    // with it, so every segment lands in the same voxel relative to them and
    // the volume is the same volume, shifted: a probe carried along reads the
    // transmittance it read at rest, to rounding.
    const Pelt pelt = MakePelt(3000u);
    ASSERT_TRUE(pelt.Groom);
    const glm::vec3 offset{ 0.37f, -0.11f, 0.23f };
    const auto moved = Displaced(pelt.Rest, offset, [](const glm::vec3&)
                                 { return true; });

    const DensityVolume rest = Bake(pelt.Rest, 48u);
    const DensityVolume posed = Bake(moved, 48u);
    ASSERT_TRUE(rest.IsValid());
    ASSERT_TRUE(posed.IsValid());

    std::vector<CoatProbe> probes;
    ASSERT_GT(BuildCoatProbes(pelt.RestSegments, glm::vec3(1.0f, 0.3f, 0.2f), 300u, 1426u, probes), 0u);

    const ProbeError carried = CompareMoved(posed, rest, probes, offset);
    const ProbeError stale = CompareMoved(rest, rest, probes, offset);
    std::printf("[groom-coat-deformed] rigid: carried mean %.5f max %.5f, stale mean %.4f, effect %.4f\n",
                carried.Mean, carried.Max, stale.Mean, carried.MeanTruthDarkening);

    ASSERT_GT(carried.MeanTruthDarkening, 0.1) << "the probes are not inside enough coat for this to measure anything";
    EXPECT_LT(carried.Max, 1.0e-3) << "a coat moved whole by the body is no longer shadowed by the same neighbours";
    // The negative control: the REST volume sampled where the probe now is. If
    // this were also small, the case would pass whatever space the bake used.
    EXPECT_GT(stale.Mean, 0.1) << "the rest-pose volume is not wrong for the moved coat, so this case cannot "
                                  "tell a pose bake from a rest bake";
}

TEST(GroomCoatShadowDeformed, ALimbThatMovesAgainstTheBodyIsShadowedByItsOwnNeighbours)
{
    // The non-rigid case, which is the one a walk actually produces: half the
    // coat (a "limb", every segment with x > 0) swings 4 cm along z, the rest
    // stays. Judged against the EXACT answer on the posed geometry -- the
    // ray-traced reference, not another volume -- because a volume compared
    // with a volume at a different grid alignment mostly measures the two
    // quantisations, which is what the first version of this case did.
    //
    // Three errors against that truth:
    //   * the rest coat's volume at REST, against the rest truth: what the
    //     representation costs anyway (rule 4: its error is a bias);
    //   * the POSE bake at the posed probes: must cost no more than that;
    //   * the REST bake at the posed probes -- the bind-pose volume #1248
    //     refused to ship: must be clearly worse, or this case cannot tell a
    //     pose bake from a rest bake.
    // The swing is PERPENDICULAR to the light on purpose. Moved along the light
    // axis, a probe slides along its own ray and the bind-pose answer is
    // nearly right by accident.
    const Pelt pelt = MakePelt(4000u);
    ASSERT_TRUE(pelt.Groom);
    const glm::vec3 swing{ 0.0f, 0.0f, 0.04f };
    const auto limbSelect = [](const glm::vec3& mid)
    { return mid.x > 0.0f; };
    const auto moved = Displaced(pelt.Rest, swing, limbSelect);
    std::vector<CoatSegment> movedSegments;
    ASSERT_GT(BuildCoatSegmentsFromStrandVertices(moved, kWidthScale, movedSegments), 0u);

    const DensityVolume rest = Bake(pelt.Rest, 48u);
    const DensityVolume posed = Bake(moved, 48u);
    ASSERT_TRUE(rest.IsValid());
    ASSERT_TRUE(posed.IsValid());

    // Probes on the limb, far enough from the cut that a ray towards +x
    // meets only limb fur in both poses.
    const glm::vec3 light{ 1.0f, 0.0f, 0.0f };
    std::vector<CoatProbe> all;
    (void)BuildCoatProbes(pelt.RestSegments, light, 1200u, 1426u, all);
    std::vector<CoatProbe> restProbes;
    std::vector<CoatProbe> posedProbes;
    for (const CoatProbe& p : all)
    {
        if (p.Position.x > 0.06f && restProbes.size() < 250u)
        {
            restProbes.push_back(p);
            posedProbes.push_back({ p.Position + swing, p.Direction });
        }
    }
    ASSERT_GE(restProbes.size(), 50u);

    // The reference's footprint spans FOUR strand spacings (the trap the
    // analysis tabulates: below one spacing it reports a confident zero).
    glm::vec3 lo{ 0.0f };
    glm::vec3 hi{ 0.0f };
    ASSERT_TRUE(CoatSegmentBounds(pelt.RestSegments, lo, hi));
    const f32 spacing = std::sqrt(((hi.x - lo.x) * (hi.z - lo.z)) / static_cast<f32>(pelt.Groom->GetCurveCount()));
    ReferenceSettings reference;
    reference.Rays = 64u;
    reference.FootprintRadius = spacing * 4.0f;

    CoatSegmentGrid restGrid;
    CoatSegmentGrid posedGrid;
    ASSERT_TRUE(BuildCoatSegmentGrid(pelt.RestSegments, 64u, restGrid));
    ASSERT_TRUE(BuildCoatSegmentGrid(movedSegments, 64u, posedGrid));
    const std::vector<f32> restTruth =
        EvaluateCoatShadowReference(pelt.RestSegments, restProbes, kKappa, reference, &restGrid);
    const std::vector<f32> posedTruth =
        EvaluateCoatShadowReference(movedSegments, posedProbes, kKappa, reference, &posedGrid);

    const auto errorOf = [](const DensityVolume& volume, const std::vector<CoatProbe>& probes,
                            const std::vector<f32>& truth)
    {
        std::vector<f32> got;
        got.reserve(probes.size());
        for (const CoatProbe& p : probes)
        {
            got.push_back(Transmittance(volume, p.Position, p.Direction));
        }
        return CompareCoatShadow(got, truth, 0.0);
    };
    const CoatShadowError atRest = errorOf(rest, restProbes, restTruth);
    const CoatShadowError poseBake = errorOf(posed, posedProbes, posedTruth);
    const CoatShadowError restBake = errorOf(rest, posedProbes, posedTruth);

    f64 darkening = 0.0;
    for (const f32 t : posedTruth)
    {
        darkening += 1.0 - static_cast<f64>(t);
    }
    darkening /= static_cast<f64>(posedTruth.size());

    std::printf("[groom-coat-deformed] limb: %zu probes, truth darkening %.4f; |dT| vs reference: at rest %.4f, "
                "pose bake %.4f, bind-pose bake %.4f\n",
                restProbes.size(), darkening, atRest.MeanAbs, poseBake.MeanAbs, restBake.MeanAbs);

    ASSERT_GT(darkening, 0.1) << "the limb probes are not inside enough coat to measure anything";
    EXPECT_LE(poseBake.MeanAbs, atRest.MeanAbs * 1.5 + 0.01)
        << "the pose bake costs more error than the representation does at rest: the moved limb is not shadowed "
           "by its own neighbours";
    // Judged as EXCESS over the representation's own error at rest, not as a
    // raw ratio: the volume is already ~0.07 off the reference at rest (rule
    // 4's bias), and that floor sits under both bakes equally. What the swing
    // adds is the comparison. Measured: +0.013 for the pose bake, +0.134 for
    // the bind-pose one.
    const f64 poseExcess = std::max(poseBake.MeanAbs - atRest.MeanAbs, 0.005);
    const f64 staleExcess = restBake.MeanAbs - atRest.MeanAbs;
    EXPECT_GT(staleExcess, poseExcess * 5.0)
        << "a bind-pose volume is not clearly wrong for the swung limb, so this case cannot tell the two bakes apart";
}

// ── 2. The bound ─────────────────────────────────────────────────────────────

TEST(GroomCoatShadowDeformed, DriftIsTheLargestCentrelineDisplacement)
{
    const Pelt pelt = MakePelt(200u);
    ASSERT_TRUE(pelt.Groom);
    std::vector<glm::vec3> baked;
    CaptureCoatPose(pelt.Rest, baked);
    ASSERT_EQ(baked.size(), pelt.Rest.size() / 4u);

    EXPECT_FLOAT_EQ(MaxCoatPoseDrift(baked, pelt.Rest), 0.0f) << "an unmoved coat must read as not having moved";

    // One segment moves 1 cm and everything else 1 mm: the answer is the
    // 1 cm, not an average that would hide the one strand that is visibly off.
    auto moved = Displaced(pelt.Rest, glm::vec3(0.0f, 0.001f, 0.0f), [](const glm::vec3&)
                           { return true; });
    for (sizet c = 0; c < 4; ++c)
    {
        moved[40 + c].Position += glm::vec3(0.0f, 0.009f, 0.0f);
    }
    EXPECT_NEAR(MaxCoatPoseDrift(baked, moved), 0.01f, 1.0e-5f);
}

TEST(GroomCoatShadowDeformed, AnIncomparablePoseReadsAsInfinitelyFar)
{
    const Pelt pelt = MakePelt(200u);
    ASSERT_TRUE(pelt.Groom);
    std::vector<glm::vec3> baked;
    CaptureCoatPose(pelt.Rest, baked);

    // A LOD hand-over rebuilt the curve set: fewer segments.
    const std::vector<GroomStrandVertex> fewer(pelt.Rest.begin(), pelt.Rest.end() - 4);
    EXPECT_TRUE(std::isinf(MaxCoatPoseDrift(baked, fewer)));

    // A corrupt point must not read as "did not move".
    auto corrupt = pelt.Rest;
    corrupt[8].Position.x = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_TRUE(std::isinf(MaxCoatPoseDrift(baked, corrupt)));

    EXPECT_TRUE(std::isinf(MaxCoatPoseDrift({}, pelt.Rest))) << "no snapshot is not a zero drift";

    // But a segment that was corrupt at the bake AND still is was dropped from
    // the volume, so it cannot have drifted from it. Counting it as infinite
    // would rebake a coat with one bad strand every single frame.
    std::vector<glm::vec3> corruptBaked;
    CaptureCoatPose(corrupt, corruptBaked);
    EXPECT_FLOAT_EQ(MaxCoatPoseDrift(corruptBaked, corrupt), 0.0f);

    // And a stream that is not a strand mesh is not one.
    const std::vector<GroomStrandVertex> ragged(pelt.Rest.begin(), pelt.Rest.begin() + 6);
    std::vector<CoatSegment> segments;
    EXPECT_EQ(BuildCoatSegmentsFromStrandVertices(ragged, 1.0f, segments), 0u);
}

TEST(GroomCoatShadowDeformed, TheBoundsAreExclusiveAndFailTowardsRebuildingAndNotSampling)
{
    const CoatRebakePolicy policy{};
    ASSERT_FLOAT_EQ(policy.MaxDriftVoxels, 0.5f);
    ASSERT_FLOAT_EQ(policy.StaleDriftVoxels, 2.0f);

    EXPECT_FALSE(CoatRebakeIsDue(0.0f, policy)) << "an idle coat must cost nothing";
    EXPECT_FALSE(CoatRebakeIsDue(0.5f, policy));
    EXPECT_TRUE(CoatRebakeIsDue(0.5001f, policy));
    EXPECT_TRUE(CoatRebakeIsDue(std::numeric_limits<f32>::quiet_NaN(), policy));
    EXPECT_TRUE(CoatRebakeIsDue(std::numeric_limits<f32>::infinity(), policy));

    EXPECT_FALSE(CoatBakeIsStale(2.0f, policy));
    EXPECT_TRUE(CoatBakeIsStale(2.0001f, policy));
    EXPECT_TRUE(CoatBakeIsStale(std::numeric_limits<f32>::quiet_NaN(), policy));

    CoatRebakePolicy frozen = policy;
    frozen.RebakeOnDrift = false;
    EXPECT_FALSE(CoatRebakeIsDue(100.0f, frozen));
    EXPECT_TRUE(CoatBakeIsStale(100.0f, frozen)) << "freezing the rebake must not also blind the detector";

    EXPECT_FLOAT_EQ(CoatDriftInVoxels(0.03f, 0.01f), 3.0f);
    EXPECT_TRUE(std::isinf(CoatDriftInVoxels(0.03f, 0.0f)));
    EXPECT_TRUE(std::isinf(CoatDriftInVoxels(std::numeric_limits<f32>::quiet_NaN(), 0.01f)));
}

TEST(GroomCoatShadowDeformed, ACorruptPolicyIsReplacedRatherThanObeyed)
{
    CoatRebakePolicy nan;
    nan.MaxDriftVoxels = std::numeric_limits<f32>::quiet_NaN();
    nan.StaleDriftVoxels = -1.0f;
    const CoatRebakePolicy fixed = SanitizeCoatRebakePolicy(nan);
    EXPECT_FLOAT_EQ(fixed.MaxDriftVoxels, CoatRebakePolicy{}.MaxDriftVoxels);
    EXPECT_FLOAT_EQ(fixed.StaleDriftVoxels, CoatRebakePolicy{}.StaleDriftVoxels);

    // The stale bound is never below the rebake bound: between the two, a
    // coat would be refused a volume the policy was about to keep.
    CoatRebakePolicy inverted;
    inverted.MaxDriftVoxels = 3.0f;
    inverted.StaleDriftVoxels = 1.0f;
    EXPECT_FLOAT_EQ(SanitizeCoatRebakePolicy(inverted).StaleDriftVoxels, 3.0f);
}

TEST(GroomCoatShadowDeformed, AStaleBakeAfterALargePoseChangeIsDetected)
{
    // THE NEGATIVE CONTROL for the bound: bake, move the coat five voxels, and
    // the drift must say so — both "rebuild" and, with the rebuild frozen,
    // "do not sample". A detector that read zero here would leave a walking
    // coat shadowed by where it was a second ago, which looks plausible.
    const Pelt pelt = MakePelt(1500u);
    ASSERT_TRUE(pelt.Groom);
    const DensityVolume rest = Bake(pelt.Rest, 48u);
    ASSERT_TRUE(rest.IsValid());
    const f32 voxel = SmallestVoxel(rest);

    std::vector<glm::vec3> baked;
    CaptureCoatPose(pelt.Rest, baked);
    const auto moved = Displaced(pelt.Rest, glm::vec3(0.0f, 0.0f, 5.0f * voxel),
                                 [](const glm::vec3& mid)
                                 { return mid.y > 0.0f; });

    const f32 drift = CoatDriftInVoxels(MaxCoatPoseDrift(baked, moved), voxel);
    EXPECT_NEAR(drift, 5.0f, 1.0e-3f);

    CoatRebakePolicy frozen;
    frozen.RebakeOnDrift = false;
    EXPECT_TRUE(CoatRebakeIsDue(drift, CoatRebakePolicy{}));
    EXPECT_TRUE(CoatBakeIsStale(drift, frozen));

    // And a rebake clears it.
    CaptureCoatPose(moved, baked);
    EXPECT_FLOAT_EQ(MaxCoatPoseDrift(baked, moved), 0.0f);
}

// ── 3. The cadence ───────────────────────────────────────────────────────────

TEST(GroomCoatShadowDeformed, ADriftBoundHoldsItsLagAndCostsNothingWhileStill)
{
    // One walk cycle of a swinging limb, then the animal standing still: half
    // the pelt oscillates 3 cm along z over 60 frames (one second at 60 Hz),
    // roughly a trotting foreleg at this coat's scale, then holds its last
    // pose for 30 frames. Every policy sees the same frames.
    //
    // THE LAG is measured as transmittance error on limb probes against a
    // FRESH bake of the same frame, which is what every-frame baking would
    // show. THE COST is the number of bakes. Both are printed; the table in
    // the rule doc is this output.
    const Pelt pelt = MakePelt(3000u);
    ASSERT_TRUE(pelt.Groom);
    constexpr u32 kMoving = 60;
    constexpr u32 kStill = 30;
    constexpr u32 kFrames = kMoving + kStill;
    constexpr u32 kResolution = 32;
    constexpr f32 kAmplitude = 0.03f;
    const auto limbSelect = [](const glm::vec3& mid)
    { return mid.x > 0.0f; };

    std::vector<std::vector<GroomStrandVertex>> frames;
    std::vector<DensityVolume> fresh;
    std::vector<glm::vec3> offsets;
    for (u32 f = 0; f < kFrames; ++f)
    {
        if (f >= kMoving)
        {
            // Standing still: the last moving pose, repeated.
            offsets.push_back(offsets.back());
            frames.push_back(frames.back());
            fresh.push_back(fresh.back());
            continue;
        }
        const f32 phase = 2.0f * std::numbers::pi_v<f32> * static_cast<f32>(f) / static_cast<f32>(kMoving);
        offsets.emplace_back(0.0f, 0.0f, kAmplitude * std::sin(phase));
        frames.push_back(Displaced(pelt.Rest, offsets.back(), limbSelect));
        fresh.push_back(Bake(frames.back(), kResolution));
        ASSERT_TRUE(fresh.back().IsValid());
    }

    const glm::vec3 light = glm::normalize(glm::vec3(1.0f, 0.4f, 0.0f));
    std::vector<CoatProbe> all;
    (void)BuildCoatProbes(pelt.RestSegments, light, 1500u, 1426u, all);
    std::vector<CoatProbe> limb;
    for (const CoatProbe& p : all)
    {
        if (p.Position.x > 0.04f)
        {
            limb.push_back(p);
        }
    }
    ASSERT_GE(limb.size(), 40u);

    struct Result
    {
        const char* Name;
        f32 Param;
        u32 Bakes = 0;
        u32 StillBakes = 0;
        f64 MeanError = 0.0;
        f64 MaxError = 0.0;
        f32 MaxDriftVoxels = 0.0f;
    };

    // Run one policy over the cycle. `due(f, driftVoxels)` decides a rebake.
    const auto run = [&](const char* name, f32 param, auto due)
    {
        Result r{ name, param };
        u32 bakedFrame = 0;
        std::vector<glm::vec3> pose;
        CaptureCoatPose(frames[0], pose);
        r.Bakes = 1; // frame 0 is every policy's first bake
        f64 errorSum = 0.0;
        u32 samples = 0;
        for (u32 f = 1; f < kFrames; ++f)
        {
            const f32 driftVoxels = CoatDriftInVoxels(MaxCoatPoseDrift(pose, frames[f]), SmallestVoxel(fresh[bakedFrame]));
            if (due(f, driftVoxels))
            {
                bakedFrame = f;
                CaptureCoatPose(frames[f], pose);
                ++r.Bakes;
                r.StillBakes += f >= kMoving ? 1u : 0u;
            }
            r.MaxDriftVoxels = std::max(r.MaxDriftVoxels, bakedFrame == f ? 0.0f : driftVoxels);
            for (const CoatProbe& probe : limb)
            {
                const glm::vec3 at = probe.Position + offsets[f];
                const f64 truth = Transmittance(fresh[f], at, probe.Direction);
                const f64 got = Transmittance(fresh[bakedFrame], at, probe.Direction);
                const f64 err = std::abs(got - truth);
                errorSum += err;
                r.MaxError = std::max(r.MaxError, err);
                ++samples;
            }
        }
        r.MeanError = errorSum / static_cast<f64>(samples);
        return r;
    };

    std::vector<Result> driftResults;
    for (const f32 bound : { 0.25f, 0.5f, 1.0f, 2.0f })
    {
        CoatRebakePolicy policy;
        policy.MaxDriftVoxels = bound;
        driftResults.push_back(run("drift", bound, [policy](u32, f32 d)
                                   { return CoatRebakeIsDue(d, policy); }));
    }
    std::vector<Result> countResults;
    for (const u32 n : { 1u, 2u, 4u, 8u })
    {
        countResults.push_back(
            run("everyN", static_cast<f32>(n), [n](u32 f, f32)
                { return (f % n) == 0u; }));
    }

    std::printf("[groom-coat-deformed] cadence over a %u-frame swing then %u still frames, %zu limb probes, %u^3:\n",
                kMoving, kStill, limb.size(), kResolution);
    for (const auto* set : { &driftResults, &countResults })
    {
        for (const Result& r : *set)
        {
            std::printf("[groom-coat-deformed]   %-6s %4.2f: %2u bakes (%2u while still), mean |dT| %.4f, max |dT| "
                        "%.4f, max drift %.2f vox\n",
                        r.Name, static_cast<f64>(r.Param), r.Bakes, r.StillBakes, r.MeanError, r.MaxError,
                        static_cast<f64>(r.MaxDriftVoxels));
        }
    }

    // PREDICTIONS, not tuned bands.
    //
    // WHAT THIS DOES NOT CLAIM, because the first version claimed it and the
    // numbers refuted it: that a drift bound has a smaller WORST transmittance
    // error than a frame count at the same cost. It does not, reliably -- the
    // worst |dT| is set by single probes whose march crosses a voxel boundary
    // as the grid re-quantises, which happens under either policy. Its mean
    // error is lower at matched cost (1.0 vox vs every 8 frames, 8 bakes each,
    // in the printed table), but that is one pair, not a law.
    //
    // (a) A drift bound holds its own bound, by construction -- the lag is a
    //     stated number whatever the animation does. A frame count's lag is
    //     whatever the limb covers in N frames.
    for (const Result& r : driftResults)
    {
        EXPECT_LE(r.MaxDriftVoxels, r.Param + 1.0e-4f) << "drift bound " << r.Param;
    }
    // (b) Every frame is exact, and costs a bake a frame.
    EXPECT_EQ(countResults[0].Bakes, kFrames);
    EXPECT_DOUBLE_EQ(countResults[0].MaxError, 0.0);
    // (c) The default bound costs fewer bakes than every frame on a moving
    //     limb -- otherwise the bound buys nothing over baking every frame.
    EXPECT_LT(driftResults[1].Bakes, kMoving);
    // (d) Looser is cheaper and less accurate on average, at both ends.
    EXPECT_GT(driftResults.front().Bakes, driftResults.back().Bakes);
    EXPECT_LT(driftResults.front().MeanError, driftResults.back().MeanError);
    // (e) THE REASON FOR THE CHOICE: a coat that stops costs nothing under a
    //     drift bound, and keeps paying under a frame count. Most bound coats
    //     in a scene are standing, idling or breathing most of the time.
    for (const Result& d : driftResults)
    {
        EXPECT_EQ(d.StillBakes, 0u) << "drift bound " << d.Param << " rebaked a coat that had stopped moving";
    }
    for (const Result& c : countResults)
    {
        EXPECT_GT(c.StillBakes, 0u) << "every " << c.Param << " stopped paying while still, which it cannot know to do";
    }
}

// ── 4. The subset (#1445) ────────────────────────────────────────────────────

TEST(GroomCoatShadowDeformed, ASubsetBakeKeepsTheShadowItReplaces)
{
    // A walking coat rebakes every frame, and every stage of the bake costs one
    // step per segment while the volume stays 64^3. So it bakes from a subset:
    // each segment kept with probability 1/stride, its radius scaled by the
    // achieved fraction's inverse. That keeps the EXPECTED density in every
    // voxel; what it costs is variance, and rule 4 says where that starts to
    // matter -- a voxel holding one strand or none stores a sample, not an
    // average.
    //
    // Measured here against the EXACT answer, over strides that leave from
    // dozens of segments per occupied voxel down to under one, and judged as
    // EXCESS over the full bake's own error (the representation's bias sits
    // under every arm equally). The shipped target is the stride
    // CoatBakeSubsetStride picks at CoatRebakePolicy's default.
    const Pelt pelt = MakePelt(40000u);
    ASSERT_TRUE(pelt.Groom);
    constexpr u32 kResolution = 32u;

    DensityVolumeSettings settings;
    settings.Resolution = kResolution;
    DensityVolume full;
    DensityVolumeBuildStats fullStats;
    ASSERT_TRUE(BuildDensityVolume(pelt.RestSegments, settings, full, &fullStats));
    const f64 perVoxel = static_cast<f64>(pelt.RestSegments.size()) / static_cast<f64>(fullStats.OccupiedVoxels);

    const glm::vec3 light = glm::normalize(glm::vec3(1.0f, 0.3f, 0.2f));
    std::vector<CoatProbe> probes;
    ASSERT_GT(BuildCoatProbes(pelt.RestSegments, light, 300u, 1445u, probes), 100u);

    glm::vec3 lo{ 0.0f };
    glm::vec3 hi{ 0.0f };
    ASSERT_TRUE(CoatSegmentBounds(pelt.RestSegments, lo, hi));
    const glm::vec3 extent = hi - lo;
    const f32 area = 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x) / 3.0f;
    const f32 spacing = std::sqrt(area / static_cast<f32>(pelt.Groom->GetCurveCount()));
    ReferenceSettings reference;
    reference.Rays = 64u;
    reference.FootprintRadius = spacing * 4.0f;
    CoatSegmentGrid grid;
    ASSERT_TRUE(BuildCoatSegmentGrid(pelt.RestSegments, 64u, grid));
    const std::vector<f32> truth = EvaluateCoatShadowReference(pelt.RestSegments, probes, kKappa, reference, &grid);

    f64 darkening = 0.0;
    for (const f32 t : truth)
    {
        darkening += 1.0 - static_cast<f64>(t);
    }
    darkening /= static_cast<f64>(truth.size());
    ASSERT_GT(darkening, 0.1) << "the probes are not inside enough coat to measure anything";

    const auto errorAt = [&](u32 stride, u32* outKept)
    {
        std::vector<CoatSegment> subset;
        (void)SubsampleCoatSegments(pelt.RestSegments, stride, subset);
        *outKept = static_cast<u32>(subset.size());
        DensityVolume volume;
        EXPECT_TRUE(BuildDensityVolume(subset, settings, volume));
        std::vector<f32> got;
        got.reserve(probes.size());
        for (const CoatProbe& p : probes)
        {
            got.push_back(Transmittance(volume, p.Position, p.Direction));
        }
        return CompareCoatShadow(got, truth, 0.0);
    };

    u32 kept = 0;
    const CoatShadowError fullError = errorAt(1u, &kept);
    ASSERT_EQ(kept, pelt.RestSegments.size());
    const u32 shipped = CoatBakeSubsetStride(pelt.RestSegments.size(), fullStats.OccupiedVoxels,
                                             CoatRebakePolicy{}.BakeSegmentsPerOccupiedVoxel);
    std::printf("[groom-coat-deformed] subset: %zu segments, %u occupied voxels (%.1f per voxel), truth darkening "
                "%.4f, shipped stride %u\n",
                pelt.RestSegments.size(), fullStats.OccupiedVoxels, perVoxel, darkening, shipped);
    std::printf("[groom-coat-deformed] subset stride 1: |dT| mean %.4f max %.4f bias %+.4f\n", fullError.MeanAbs,
                fullError.MaxAbs, fullError.Bias);

    f64 shippedExcess = 0.0;
    f64 sparseExcess = 0.0;
    const u32 sparseStride = static_cast<u32>(std::max(2.0, std::ceil(perVoxel * 2.0)));
    for (const u32 stride : { 2u, 4u, 8u, 16u, shipped, sparseStride })
    {
        const CoatShadowError e = errorAt(stride, &kept);
        const f64 excess = e.MeanAbs - fullError.MeanAbs;
        std::printf("[groom-coat-deformed] subset stride %u: %u kept (%.1f per voxel), |dT| mean %.4f max %.4f bias "
                    "%+.4f, excess %+.4f\n",
                    stride, kept, static_cast<f64>(kept) / fullStats.OccupiedVoxels, e.MeanAbs, e.MaxAbs, e.Bias,
                    excess);
        if (stride == shipped)
        {
            shippedExcess = excess;
        }
        if (stride == sparseStride)
        {
            sparseExcess = excess;
        }
    }

    ASSERT_GT(shipped, 1u) << "the pelt must be dense enough for the shipped target to thin it, or this case measures "
                              "the full bake twice";
    // The shipped subset costs a small fraction of the effect it shadows...
    EXPECT_LT(shippedExcess, 0.01) << "the shipped subset adds more error than the full bake has";
    // ...and the metric can see a subset that is too thin -- the negative
    // control, rule 4's regime: under one segment per occupied voxel.
    EXPECT_GT(sparseExcess, shippedExcess * 2.0 + 0.002)
        << "a subset leaving under a segment per voxel scored no worse than the shipped one, so this case cannot tell "
           "a safe stride from an unsafe one";
}

TEST(GroomCoatShadowDeformed, TheSubsetIsTheSameSegmentsEveryFrameAndKeepsTheCoatsMass)
{
    // A subset that changed per bake would re-dither the shadow every frame on
    // a coat that is only walking; the selection is a hash of the index.
    // And the scale is the ACHIEVED fraction's inverse, so the binned areal
    // mass -- what the volume stores -- is the full coat's.
    const Pelt pelt = MakePelt(4000u);
    ASSERT_TRUE(pelt.Groom);
    std::vector<CoatSegment> first;
    std::vector<CoatSegment> second;
    const f32 scale = SubsampleCoatSegments(pelt.RestSegments, 5u, first);
    (void)SubsampleCoatSegments(pelt.RestSegments, 5u, second);
    ASSERT_EQ(first.size(), second.size());
    for (sizet s = 0; s < first.size(); ++s)
    {
        ASSERT_EQ(std::memcmp(&first[s], &second[s], sizeof(CoatSegment)), 0) << "segment " << s;
    }
    EXPECT_NEAR(scale, static_cast<f32>(pelt.RestSegments.size()) / static_cast<f32>(first.size()), 1.0e-6f);
    EXPECT_NEAR(static_cast<f64>(first.size()) / pelt.RestSegments.size(), 0.2, 0.02)
        << "a stride of 5 keeps about a fifth";

    DensityVolumeSettings settings;
    settings.Resolution = 32u;
    DensityVolume full;
    DensityVolume thinned;
    DensityVolumeBuildStats fullStats;
    DensityVolumeBuildStats thinnedStats;
    ASSERT_TRUE(BuildDensityVolume(pelt.RestSegments, settings, full, &fullStats));
    ASSERT_TRUE(BuildDensityVolume(first, settings, thinned, &thinnedStats));
    EXPECT_NEAR(thinnedStats.TotalArealMass / fullStats.TotalArealMass, 1.0, 0.02)
        << "the subset lost fibre area, which is shadow";

    // Stride 1 is the identity; no target, or nothing measured yet, is stride 1.
    EXPECT_EQ(SubsampleCoatSegments(pelt.RestSegments, 1u, first), 1.0f);
    EXPECT_EQ(first.size(), pelt.RestSegments.size());
    EXPECT_EQ(CoatBakeSubsetStride(100000u, 0u, 8.0f), 1u);
    EXPECT_EQ(CoatBakeSubsetStride(100000u, 1000u, 0.0f), 1u);
    EXPECT_EQ(CoatBakeSubsetStride(100000u, 1000u, 8.0f), 12u);
    EXPECT_EQ(CoatBakeSubsetStride(1000u, 1000u, 8.0f), 1u) << "a coat already under the target is baked whole";
}
