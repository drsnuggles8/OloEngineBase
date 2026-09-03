#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// WaterShoreWaveTest — L1 contract tests for shore wave deformation (#1033).
//
// Waves that shoal, refract and break against a seabed depth field are exactly
// the "tests green, screen wrong" category: every relation below is an analytic
// statement about water that can be asserted with no GPU at all, and none of
// them is visible in a frame until several of them are right at once. So they
// are pinned here, and the frame is checked separately by
// WaterShoreVisualEvidenceTest.
//
// Three things are under test and they are deliberately not merged:
//
//   * the WAVE TRANSFORM (WaterShore::TransformOctave and the relations it is
//     built from) — dispersion, Green's law, Snell's law, the breaker limit;
//   * the FIELD (WaterShoreDepthSystem::BakeField / SampleBaked) — that a known
//     seabed resamples to the depth and gradient it should, in the addressing
//     the shader uses;
//   * the MIRROR — that WaterShoreCommon.glsl still carries the same constants,
//     because a divergence there is a CPU/GPU surface split that no C++ test
//     could otherwise see.
//
// The single most important case is the DEEP-WATER LIMIT: with no field, or in
// deep water, every relation must reduce to what the engine did before this
// existed. That is what makes "the open ocean is unchanged" a property rather
// than a hope, and it is asserted first.
// =============================================================================

#include "OloEngine/Renderer/Water/WaterShoreDepth.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Renderer/WaterSurface.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr f32 kTwoPi = 6.28318530718f;

    [[nodiscard]] f32 WavenumberOf(f32 wavelength)
    {
        return kTwoPi / wavelength;
    }

    /// A flat seabed at a constant depth, as a Sample. `gradient` zero means
    /// "nothing to refract against", which is its own case below.
    [[nodiscard]] WaterShore::Sample FlatBed(f32 depth, glm::vec2 gradient = { 0.0f, 0.0f })
    {
        WaterShore::Sample s;
        s.Depth = depth;
        s.Gradient = gradient;
        s.Enabled = true;
        return s;
    }

    /// A square seabed ramp: depth `deep` at the +X edge falling linearly to
    /// `shallow` at the -X edge, expressed as a terrain height field so the bake
    /// sees the same shape a real island would present.
    struct RampBed
    {
        std::vector<f32> Heights;
        SeabedTerrain Tile;
    };

    [[nodiscard]] RampBed MakeRampBed(u32 resolution, f32 sizeMetres, f32 baseY, f32 heightScale)
    {
        RampBed bed;
        bed.Heights.resize(static_cast<sizet>(resolution) * resolution);
        for (u32 z = 0; z < resolution; ++z)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                // Normalised height rises linearly toward -X, i.e. the seabed
                // shallows that way. Independent of Z, so the depth gradient is
                // exactly (+something, 0) everywhere: the shore normal points
                // along -X and the "deeper water" direction along +X.
                const f32 t = 1.0f - static_cast<f32>(x) / static_cast<f32>(resolution - 1);
                bed.Heights[static_cast<sizet>(z) * resolution + x] = t;
            }
        }
        bed.Tile.OriginXZ = { -sizeMetres * 0.5f, -sizeMetres * 0.5f };
        bed.Tile.SizeXZ = { sizeMetres, sizeMetres };
        bed.Tile.BaseY = baseY;
        bed.Tile.HeightScale = heightScale;
        bed.Tile.Resolution = resolution;
        bed.Tile.Heights = &bed.Heights;
        return bed;
    }

    [[nodiscard]] std::string ReadShaderSource(const char* relative)
    {
        namespace fs = std::filesystem;
        const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / relative;
        std::ifstream file(path);
        if (!file)
            return {};
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
} // namespace

// =============================================================================
// 1. The deep-water limit
// =============================================================================

// With no seabed to speak of, every part of the transform is the identity. This
// is the property that lets the shore code sit unconditionally in the shared
// octave sum: an ocean tile with nothing under it renders exactly what it did
// before #1033, rather than something close to it.
TEST(WaterShoreWave, DisabledSampleLeavesTheOctaveExactlyAsAuthored)
{
    const glm::vec2 dir = glm::normalize(glm::vec2(0.6f, 0.8f));
    constexpr f32 wavelength = 18.2f;
    constexpr f32 steepness = 0.25f;

    const WaterShore::Sample off = WaterShore::DisabledSample();
    const auto octave = WaterShore::TransformOctave(dir, wavelength, steepness, 0.07f, off.Depth,
                                                    off.Gradient);

    EXPECT_FLOAT_EQ(octave.Wavelength, wavelength);
    EXPECT_FLOAT_EQ(octave.Direction.x, dir.x);
    EXPECT_FLOAT_EQ(octave.Direction.y, dir.y);
    EXPECT_EQ(octave.Breaking, 0.0f);
    // The phase speed a caller must get is exactly the one gerstnerWave()
    // derives internally from the wavelength, or the two chains disagree the
    // moment one of them takes the explicit-speed overload.
    EXPECT_FLOAT_EQ(octave.PhaseSpeed,
                    std::sqrt(WaterShore::kGravity / WavenumberOf(wavelength)));
    EXPECT_FLOAT_EQ(octave.Steepness, steepness);
}

// The sentinel is not merely "large": it has to be deep enough that tanh(kh) is
// exactly 1 in f32 at the wavelengths this engine's water uses, or the deep
// ocean would pick up a slow drift with nothing visibly wrong.
TEST(WaterShoreWave, DeepSentinelSaturatesTheDispersionRelationInFloat)
{
    // The longest wavelength the WaterComponent's clamp admits, which is the
    // hardest case: a longer wave feels a deeper bottom. This is the assertion
    // that caught the sentinel being 200 m, where a 500 m wave still had
    // tanh(kh) = 0.987 and the "unchanged" open ocean was quietly drifting.
    constexpr f32 longestWavelength = 500.0f;
    const f32 k0 = WavenumberOf(longestWavelength);
    EXPECT_FLOAT_EQ(std::tanh(k0 * WaterShore::kDeepSentinelMetres), 1.0f);
    EXPECT_FLOAT_EQ(WaterShore::LocalWavenumber(k0, WaterShore::kDeepSentinelMetres), k0);
}

// =============================================================================
// 2. Shoaling — the dispersion relation
// =============================================================================

// The whole transform hangs off inverting w^2 = g k tanh(k h) for k, so assert
// the root rather than the iteration: a wrong iteration count or a mistyped
// update would still converge somewhere, and only the residual shows it.
TEST(WaterShoreWave, LocalWavenumberSolvesTheDispersionRelation)
{
    for (const f32 wavelength : { 5.0f, 18.2f, 27.3f, 60.0f })
    {
        const f32 k0 = WavenumberOf(wavelength);
        const f32 omegaSquared = WaterShore::kGravity * k0;
        for (const f32 depth : { 0.2f, 0.5f, 1.5f, 4.0f, 12.0f, 30.0f })
        {
            const f32 k = WaterShore::LocalWavenumber(k0, depth);
            const f32 residual = WaterShore::kGravity * k * std::tanh(k * depth);
            EXPECT_NEAR(residual, omegaSquared, omegaSquared * 2e-3f)
                << "wavelength " << wavelength << " m, depth " << depth << " m";
        }
    }
}

// Shoaling, stated as the thing you can see: crests bunch up and slow down as
// the water shallows, monotonically, with no reversal anywhere in the range.
TEST(WaterShoreWave, WavesShortenAndSlowMonotonicallyAsDepthFalls)
{
    const glm::vec2 dir{ 1.0f, 0.0f };
    constexpr f32 wavelength = 18.2f;

    f32 previousWavelength = std::numeric_limits<f32>::max();
    f32 previousSpeed = std::numeric_limits<f32>::max();
    for (const f32 depth : { 40.0f, 20.0f, 12.0f, 8.0f, 5.0f, 3.0f, 2.0f, 1.0f, 0.5f, 0.25f })
    {
        const auto octave =
            WaterShore::TransformOctave(dir, wavelength, 0.25f, 0.07f, depth, glm::vec2(0.0f));
        EXPECT_LT(octave.Wavelength, previousWavelength) << "at depth " << depth;
        EXPECT_LT(octave.PhaseSpeed, previousSpeed) << "at depth " << depth;
        EXPECT_LE(octave.Wavelength, wavelength + 1e-3f);
        previousWavelength = octave.Wavelength;
        previousSpeed = octave.PhaseSpeed;
    }
}

// Green's law has a signature that a hand-rolled "amplitude grows in shallow
// water" fudge does not: the shoaling coefficient DIPS below 1 before it rises.
// A wave first gets slightly smaller as it starts to feel the bottom, because
// the group velocity briefly rises. Pinning the dip is what distinguishes the
// real relation from any monotone approximation of it.
TEST(WaterShoreWave, ShoalingCoefficientDipsBelowOneBeforeItGrows)
{
    constexpr f32 wavelength = 18.2f;
    const f32 k0 = WavenumberOf(wavelength);

    f32 minimum = std::numeric_limits<f32>::max();
    f32 shallowest = 0.0f;
    for (i32 step = 0; step < 400; ++step)
    {
        const f32 depth = 0.1f + static_cast<f32>(step) * 0.1f;
        const f32 k = WaterShore::LocalWavenumber(k0, depth);
        const f32 ks = WaterShore::ShoalingCoefficient(k0, k, depth);
        minimum = std::min(minimum, ks);
        if (step == 0)
            shallowest = ks;
    }

    // The classic value is 0.913 at h/L0 ~ 0.157; assert the dip exists and is
    // in the right neighbourhood rather than to four figures.
    EXPECT_LT(minimum, 0.99f);
    EXPECT_GT(minimum, 0.85f);
    // …and that it does climb well above 1 in genuinely shallow water.
    EXPECT_GT(shallowest, 1.3f);
    // Deep water is still exactly 1.
    EXPECT_FLOAT_EQ(WaterShore::ShoalingCoefficient(k0, k0, WaterShore::kDeepSentinelMetres), 1.0f);
}

// =============================================================================
// 3. Refraction — Snell's law
// =============================================================================

// The acceptance criterion in prose: waves arrive parallel to the beach whatever
// their offshore heading. In numbers: the angle between the wave and the shore
// normal must SHRINK as the water shallows, and never overshoot.
TEST(WaterShoreWave, ObliqueWavesTurnTowardTheShoreAndNeverPastIt)
{
    // Seabed deepens toward +X, so the shore normal (the shoreward direction) is
    // -X and a wave running shoreward has a negative X component.
    const glm::vec2 gradient{ 1.0f, 0.0f };
    constexpr f32 wavelength = 18.2f;

    // A wave arriving at 60 degrees off the shore normal.
    const f32 incidence = glm::radians(60.0f);
    const glm::vec2 deepDir{ -std::cos(incidence), std::sin(incidence) };

    f32 previousAngle = incidence;
    for (const f32 depth : { 30.0f, 12.0f, 6.0f, 3.0f, 1.5f, 0.6f, 0.25f })
    {
        const auto octave =
            WaterShore::TransformOctave(deepDir, wavelength, 0.25f, 0.07f, depth, gradient);

        EXPECT_NEAR(glm::length(octave.Direction), 1.0f, 1e-4f) << "at depth " << depth;
        // Still travelling shoreward — a turn that flipped the sign would send
        // the wave back out to sea, which is what a mishandled cosine does.
        EXPECT_LT(octave.Direction.x, 0.0f) << "at depth " << depth;

        const f32 angle = std::asin(std::clamp(octave.Direction.y, -1.0f, 1.0f));
        EXPECT_LT(angle, previousAngle + 1e-5f) << "at depth " << depth;
        EXPECT_GE(angle, 0.0f);
        previousAngle = angle;
    }

    // By the surf zone it is very nearly normal to the beach — this is the
    // "arrives parallel to the shoreline" the issue asks to see.
    EXPECT_LT(previousAngle, glm::radians(20.0f));
}

// A wave already running straight at the beach has nothing to turn, and a flat
// seabed has nothing to turn against. Both must be exact no-ops on direction:
// a normalise of a near-zero vector here is how a whole ocean picks up NaN.
TEST(WaterShoreWave, NormalIncidenceAndFlatSeabedLeaveTheHeadingAlone)
{
    constexpr f32 wavelength = 18.2f;
    const glm::vec2 shoreward{ -1.0f, 0.0f };

    const auto normalIncidence = WaterShore::TransformOctave(shoreward, wavelength, 0.25f, 0.07f,
                                                             2.0f, glm::vec2(1.0f, 0.0f));
    EXPECT_NEAR(normalIncidence.Direction.x, -1.0f, 1e-5f);
    EXPECT_NEAR(normalIncidence.Direction.y, 0.0f, 1e-5f);

    const glm::vec2 oblique = glm::normalize(glm::vec2(-0.5f, 0.87f));
    const auto flatBed =
        WaterShore::TransformOctave(oblique, wavelength, 0.25f, 0.07f, 2.0f, glm::vec2(0.0f));
    EXPECT_FLOAT_EQ(flatBed.Direction.x, oblique.x);
    EXPECT_FLOAT_EQ(flatBed.Direction.y, oblique.y);
    // …but it still shoals. Refraction and shoaling are independent, and a
    // guard that skipped both on a flat bed would be wrong.
    EXPECT_LT(flatBed.Wavelength, wavelength);
}

// A wave running OUT to sea must keep going out to sea. Snell is symmetric and
// the sign of the along-normal component is the only thing that says which way
// the wave is going; losing it turns the whole train around at a slope.
TEST(WaterShoreWave, OffshoreTravellingWavesKeepTravellingOffshore)
{
    const glm::vec2 gradient{ 1.0f, 0.0f }; // deeper toward +X
    const glm::vec2 seaward = glm::normalize(glm::vec2(0.7f, 0.7f));
    const auto octave =
        WaterShore::TransformOctave(seaward, 18.2f, 0.25f, 0.07f, 1.5f, gradient);
    EXPECT_GT(octave.Direction.x, 0.0f);
}

// =============================================================================
// 4. Breaking — the depth limit
// =============================================================================

// The breaker limit is a statement about metres of water: the rendered
// amplitude cannot exceed breakerIndex * depth. That is why the caller's
// amplitude scale is passed in, and this is the assertion that would fail if
// anyone "simplified" it back out.
TEST(WaterShoreWave, AmplitudeIsCappedAtTheBreakerLimitAndReportsTheExcess)
{
    const glm::vec2 dir{ -1.0f, 0.0f };
    constexpr f32 wavelength = 18.2f;
    constexpr f32 steepness = 0.25f;
    constexpr f32 amplitudeScale = 0.5f; // a deliberately energetic sea
    constexpr f32 breakerIndex = WaterShore::kBreakerIndex;

    bool sawBreaking = false;
    for (const f32 depth : { 20.0f, 8.0f, 3.0f, 1.5f, 0.8f, 0.4f, 0.2f, 0.1f })
    {
        const auto octave = WaterShore::TransformOctave(dir, wavelength, steepness, amplitudeScale,
                                                        depth, glm::vec2(1.0f, 0.0f), breakerIndex);
        // Reconstruct the amplitude the surface is actually displaced by.
        const f32 k = WavenumberOf(octave.Wavelength);
        const f32 renderedAmplitude = (octave.Steepness / k) * amplitudeScale;
        EXPECT_LE(renderedAmplitude, breakerIndex * depth + 1e-4f) << "at depth " << depth;
        if (octave.Breaking > 0.0f)
            sawBreaking = true;
        EXPECT_GE(octave.Breaking, 0.0f);
        EXPECT_LE(octave.Breaking, 1.0f);
    }
    EXPECT_TRUE(sawBreaking) << "an energetic sea run into 10 cm of water never broke";
}

// Offshore there is nothing to report, and reporting anything there would put a
// band of foam on the open sea.
TEST(WaterShoreWave, NothingBreaksInDeepWater)
{
    const auto octave = WaterShore::TransformOctave({ -1.0f, 0.0f }, 18.2f, 0.5f, 1.0f, 50.0f,
                                                    glm::vec2(1.0f, 0.0f));
    EXPECT_EQ(octave.Breaking, 0.0f);
}

// Past the breaker line the wave must DECAY toward the waterline, not keep
// growing. This is what the clamp buys, and without it a shoaling wave becomes
// a wall of water standing on the beach.
TEST(WaterShoreWave, AmplitudeFallsToZeroAtTheWaterline)
{
    const glm::vec2 dir{ -1.0f, 0.0f };
    f32 previousAmplitude = std::numeric_limits<f32>::max();
    for (const f32 depth : { 0.8f, 0.6f, 0.4f, 0.2f, 0.1f, WaterShore::kMinDepthMetres })
    {
        const auto octave =
            WaterShore::TransformOctave(dir, 18.2f, 0.5f, 0.5f, depth, glm::vec2(1.0f, 0.0f));
        const f32 amplitude = (octave.Steepness / WavenumberOf(octave.Wavelength)) * 0.5f;
        EXPECT_LT(amplitude, previousAmplitude) << "at depth " << depth;
        previousAmplitude = amplitude;
    }
    EXPECT_LT(previousAmplitude, WaterShore::kBreakerIndex * WaterShore::kMinDepthMetres + 1e-4f);
}

// A single Gerstner octave self-intersects at Q >= 1 whatever the depth says.
// The surface has to stay a function even where the breaker limit has not bitten.
TEST(WaterShoreWave, SteepnessNeverReachesTheGerstnerFold)
{
    for (const f32 depth : { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f })
    {
        for (const f32 scale : { 0.05f, 0.5f, 2.0f, 8.0f })
        {
            const auto octave =
                WaterShore::TransformOctave({ -1.0f, 0.0f }, 18.2f, 1.0f, scale, depth,
                                            glm::vec2(1.0f, 0.0f));
            EXPECT_LE(octave.Steepness * scale, WaterShore::kMaxSteepness + 1e-5f)
                << "depth " << depth << ", scale " << scale;
        }
    }
}

// =============================================================================
// 5. The field — bake and sampling
// =============================================================================

TEST(WaterShoreField, BakeResamplesTheSeabedIntoDepthAndGradient)
{
    // A 400 m ramp from 20 m of water at +X up to dry land at -X, under a water
    // plane at y = 0.
    constexpr f32 size = 400.0f;
    RampBed bed = MakeRampBed(129, size, -20.0f, 40.0f);

    WaterShoreBakeRequest request;
    request.CentreXZ = { 0.0f, 0.0f };
    request.ExtentMetres = size;
    request.WaterPlaneY = 0.0f;

    std::vector<glm::vec4> texels;
    const std::array<SeabedTerrain, 1> tiles{ bed.Tile };
    WaterShoreDepthSystem::BakeField(request, tiles, texels);
    ASSERT_EQ(texels.size(), static_cast<sizet>(WaterShore::kResolution) * WaterShore::kResolution);

    // The analytic depth: at world X the ramp's normalised height is
    // t = 1 - (X + size/2) / size, so seabed Y = -20 + 40 t and depth = -seabedY.
    auto analyticDepth = [&](f32 worldX)
    {
        const f32 t = 1.0f - (worldX + size * 0.5f) / size;
        return std::clamp(20.0f - 40.0f * t, WaterShore::kMinDepthMetres,
                          WaterShore::kDeepSentinelMetres);
    };

    // All four are WATER: the ramp crosses y = 0 at world X = 0, so anything at
    // or below that is dry land and its depth is clamped to the minimum. Testing
    // the gradient there would be asserting the slope of a clamp.
    for (const f32 worldX : { 150.0f, 110.0f, 70.0f, 30.0f })
    {
        const WaterShore::Sample sample =
            WaterShoreDepthSystem::SampleBaked(texels, request, { worldX, 12.0f });
        ASSERT_TRUE(sample.Enabled) << "at X " << worldX;
        EXPECT_NEAR(sample.Depth, analyticDepth(worldX), 0.35f) << "at X " << worldX;

        // The gradient points toward DEEPER water — here, +X — and the ramp is
        // constant in Z. This is the quantity refraction turns waves against; a
        // sign error would aim the whole train out to sea.
        EXPECT_GT(sample.Gradient.x, 0.0f) << "at X " << worldX;
        EXPECT_NEAR(sample.Gradient.x, 40.0f / size, 0.02f) << "at X " << worldX;
        EXPECT_NEAR(sample.Gradient.y, 0.0f, 1e-3f) << "at X " << worldX;
    }
}

// Where there is no terrain the field must read as open sea, and the SENTINEL
// is what makes every downstream relation reduce to its deep-water form there.
TEST(WaterShoreField, WaterWithNoTerrainUnderItReadsAsOpenSea)
{
    WaterShoreBakeRequest request;
    request.ExtentMetres = 800.0f;

    std::vector<glm::vec4> texels;
    WaterShoreDepthSystem::BakeField(request, {}, texels);
    ASSERT_FALSE(texels.empty());
    for (const glm::vec4& texel : texels)
    {
        EXPECT_FLOAT_EQ(texel.x, WaterShore::kDeepSentinelMetres);
        EXPECT_FLOAT_EQ(texel.y, 0.0f);
        EXPECT_FLOAT_EQ(texel.z, 0.0f);
    }
}

// Outside the window is not "the nearest edge texel", it is open sea. Relying on
// CLAMP_TO_EDGE instead would drag whatever the field's border happens to hold
// across the whole rest of the ocean.
TEST(WaterShoreField, OutsideTheWindowIsTheDisabledSample)
{
    WaterShoreBakeRequest request;
    request.CentreXZ = { 0.0f, 0.0f };
    request.ExtentMetres = 200.0f;

    std::vector<glm::vec4> texels;
    WaterShoreDepthSystem::BakeField(request, {}, texels);

    EXPECT_FALSE(WaterShoreDepthSystem::SampleBaked(texels, request, { 400.0f, 0.0f }).Enabled);
    EXPECT_FALSE(WaterShoreDepthSystem::SampleBaked(texels, request, { 0.0f, -101.0f }).Enabled);
    EXPECT_TRUE(WaterShoreDepthSystem::SampleBaked(texels, request, { 0.0f, 0.0f }).Enabled);
}

// Two tiles that overlap both describe solid ground; the water column ends at
// whichever is HIGHER. Taking the other one would drown an island under the
// seabed of its neighbour.
TEST(WaterShoreField, OverlappingTilesKeepTheShallowerSeabed)
{
    constexpr f32 size = 200.0f;
    RampBed deepBed = MakeRampBed(65, size, -30.0f, 0.0f);   // flat at -30
    RampBed shallowBed = MakeRampBed(65, size, -4.0f, 0.0f); // flat at -4

    WaterShoreBakeRequest request;
    request.ExtentMetres = size;

    std::vector<glm::vec4> texels;
    const std::array<SeabedTerrain, 2> tiles{ deepBed.Tile, shallowBed.Tile };
    WaterShoreDepthSystem::BakeField(request, tiles, texels);

    const WaterShore::Sample sample =
        WaterShoreDepthSystem::SampleBaked(texels, request, { 0.0f, 0.0f });
    EXPECT_NEAR(sample.Depth, 4.0f, 0.05f);
}

// =============================================================================
// 6. The CPU sampler physics floats on
// =============================================================================

// A boat in the surf has to float on the surface that is DRAWN there. Both
// halves matter: the shoaled sampler must differ from the deep one, and the
// sampler with no seabed must be bit-identical to the pre-#1033 one.
TEST(WaterShoreWave, TheBuoyancySamplerFollowsTheShoaledSurface)
{
    WaterSurface::Params params;
    params.m_WaveDir0 = { 1.0f, 0.15f, 0.25f, 10.0f };
    params.m_WaveDir1 = { 0.6f, 0.8f, 0.15f, 15.0f };
    params.m_WaveFrequency = 0.55f;
    params.m_WaveAmplitude = 0.12f;
    params.m_WaveSpeed = 1.0f;

    constexpr f32 time = 3.25f;
    const glm::vec2 queryXZ{ 12.0f, -7.0f };

    const glm::vec3 deepA =
        WaterSurface::SampleDisplacement(params, queryXZ, time, WaterShore::DisabledSample());
    const glm::vec3 deepB = WaterSurface::SampleDisplacement(params, queryXZ, time);
    EXPECT_FLOAT_EQ(deepA.x, deepB.x);
    EXPECT_FLOAT_EQ(deepA.y, deepB.y);
    EXPECT_FLOAT_EQ(deepA.z, deepB.z);

    const glm::vec3 shallow =
        WaterSurface::SampleDisplacement(params, queryXZ, time, FlatBed(1.0f, { 1.0f, 0.0f }));
    EXPECT_NE(shallow.y, deepA.y);
    // The surf-zone surface is bounded by the breaker limit summed over the
    // ladder, which is far below the deep-water crest bound at this amplitude.
    EXPECT_LT(std::abs(shallow.y), 8.0f * WaterShore::kBreakerIndex * 1.0f);
}

// =============================================================================
// 7. The GPU mirror
// =============================================================================

// WaterShoreCommon.glsl is a hand-written twin of WaterShoreDepth.h. Nothing in
// C++ can see it diverge, and a divergence is a CPU/GPU surface split — physics
// and rendering quietly disagreeing about where the water is. Pin the constants
// that would cause it, by value, from the shader source.
TEST(WaterShoreWave, TheShaderCarriesTheSameConstantsAsTheHeader)
{
    const std::string source = ReadShaderSource("include/WaterShoreCommon.glsl");
    ASSERT_FALSE(source.empty()) << "WaterShoreCommon.glsl not found or empty";

    struct Mirror
    {
        const char* Declaration;
        f32 Value;
    };
    const std::array<Mirror, 5> kMirrors{ {
        { "const float WATER_SHORE_GRAVITY = ", WaterShore::kGravity },
        { "const float WATER_SHORE_DEEP_SENTINEL = ", WaterShore::kDeepSentinelMetres },
        { "const float WATER_SHORE_MIN_DEPTH = ", WaterShore::kMinDepthMetres },
        { "const float WATER_SHORE_BREAKER_INDEX = ", WaterShore::kBreakerIndex },
        { "const float WATER_SHORE_MAX_STEEPNESS = ", WaterShore::kMaxSteepness },
    } };

    for (const Mirror& mirror : kMirrors)
    {
        const sizet at = source.find(mirror.Declaration);
        ASSERT_NE(at, std::string::npos)
            << mirror.Declaration << " is gone from WaterShoreCommon.glsl — if it moved, repoint "
                                     "this test rather than deleting the pin";
        const f32 shaderValue =
            std::stof(source.substr(at + std::strlen(mirror.Declaration), 32));
        EXPECT_FLOAT_EQ(shaderValue, mirror.Value)
            << mirror.Declaration << "in the shader is " << shaderValue << " but the C++ mirror is "
            << mirror.Value
            << ".\n  These are the same physical constant evaluated on two sides of the CPU/GPU "
               "seam. A divergence does not look wrong: it puts the buoyancy surface and the drawn "
               "surface at different heights near shore.";
    }

    // The iteration count is part of the contract too — the two sides must run
    // the SAME number or they differ by the residual, which is largest exactly
    // where the shore waves are.
    const std::string iterations = "const int WATER_SHORE_DISPERSION_ITERATIONS = " +
                                   std::to_string(WaterShore::kDispersionIterations) + ";";
    EXPECT_NE(source.find(iterations), std::string::npos)
        << "WaterShoreCommon.glsl does not run " << WaterShore::kDispersionIterations
        << " dispersion iterations";
}

// The shore transform is applied in the vertex stage AND the tess-eval stage,
// which is what makes the colour pass and the surface-depth capture agree — they
// replay one shared chain. A stage that quietly kept the deep-water entry point
// would show as depth artefacts at the waterline rather than as wrong waves.
TEST(WaterShoreWave, BothDisplacingStagesUseTheShoreAwareSum)
{
    for (const char* stage : { "include/WaterVertexStage.glsl", "include/WaterTessEvalStage.glsl" })
    {
        const std::string source = ReadShaderSource(stage);
        ASSERT_FALSE(source.empty()) << stage;
        EXPECT_NE(source.find("waterShoreSample("), std::string::npos)
            << stage << " never samples the seabed";
        EXPECT_NE(source.find("sumGerstnerWavesShore("), std::string::npos)
            << stage << " still calls the deep-water octave sum";
        EXPECT_EQ(source.find("= sumGerstnerWaves("), std::string::npos)
            << stage << " has a displacement site left on the deep-water entry point";
    }
}
