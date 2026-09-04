#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1

// =============================================================================
// WaterRainRippleTest — L1 property tests for the rain-impact ripple contract
// (issue #1034, §7.3).
//
// The contract lives in Renderer/Water/WaterRainRipples.h and is mirrored by
// include/WaterRainCommon.glsl. Everything about this feature renders: a ripple
// field that is on when it should be off, that pulses instead of expanding, or
// that drifts with the camera all produce a perfectly plausible frame. So the
// pins here are the ones a picture cannot give:
//
//   1. the OFF state is exactly off — not "small", zero, and reached before the
//      neighbourhood walk, because that early-out IS the acceptance criterion
//      "no cost when rain is off";
//   2. the intensity tail SNAPS to off. PrecipitationSystem's smoothed
//      intensity decays exponentially and never reaches zero, so a naive gate
//      stops firing after the first shower and the criterion silently becomes
//      "no cost until it has rained once". This one is negative-controlled
//      against a raw exponential tail;
//   3. rings EXPAND — the crest radius is strictly increasing in age — which is
//      the difference between rain and a stationary sparkle, and which a still
//      frame cannot show;
//   4. the two halves of the gate (the water tile wants ripples, the sky is
//      actually raining) both have to be true, and snow is not raining;
//   5. the GLSL twin's constants are the header's. Two mirrors drift, and the
//      drift renders.
//
// The hash is INTEGER precisely so it can be pinned this way: u32 wrapping
// arithmetic is bit-identical in C++ and GLSL, so the text check below compares
// the actual magic numbers rather than hoping two float expressions agree.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"
#include "OloEngine/Renderer/Water/WaterRainRipples.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace WR = OloEngine::WaterRain;
    namespace fs = std::filesystem;

    // f32/i32/u32 are GLOBAL typedefs (Core/Base.h declares them after the
    // OloEngine namespace closes), so there is nothing to import here.

    constexpr f32 kFullDensity = WR::kMaxDensity;

    /// Largest ripple slope magnitude over a `steps` x `steps` sample grid
    /// covering `extent` metres from the origin.
    [[nodiscard]] f32 PeakSlope(f32 timeSeconds, f32 strength, f32 density, i32 steps, f32 extent)
    {
        f32 peak = 0.0f;
        for (i32 z = 0; z < steps; ++z)
        {
            for (i32 x = 0; x < steps; ++x)
            {
                const glm::vec2 p{ extent * static_cast<f32>(x) / static_cast<f32>(steps),
                                   extent * static_cast<f32>(z) / static_cast<f32>(steps) };
                const glm::vec2 s =
                    WR::RippleSlope(p, timeSeconds, strength, density, WR::kCellSizeMetres);
                peak = std::max(peak, std::sqrt(glm::dot(s, s)));
            }
        }
        return peak;
    }

    /// Number of grid samples with a non-zero ripple slope.
    [[nodiscard]] i32 ActiveSamples(f32 timeSeconds, f32 strength, f32 density, i32 steps, f32 extent)
    {
        i32 active = 0;
        for (i32 z = 0; z < steps; ++z)
        {
            for (i32 x = 0; x < steps; ++x)
            {
                const glm::vec2 p{ extent * static_cast<f32>(x) / static_cast<f32>(steps),
                                   extent * static_cast<f32>(z) / static_cast<f32>(steps) };
                const glm::vec2 s =
                    WR::RippleSlope(p, timeSeconds, strength, density, WR::kCellSizeMetres);
                if (s.x != 0.0f || s.y != 0.0f)
                    ++active;
            }
        }
        return active;
    }

    [[nodiscard]] std::string ReadRippleGlsl()
    {
        const fs::path path =
            fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" / "WaterRainCommon.glsl";
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

namespace OloEngine::Tests
{
    class WaterRainRippleSystemTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            WaterRainRippleSystem::Reset();
        }
        void TearDown() override
        {
            WaterRainRippleSystem::Reset();
        }
    };

    // -------------------------------------------------------------------------
    // 1. The OFF state is exactly off
    // -------------------------------------------------------------------------

    TEST(WaterRainRippleTest, ZeroStrengthProducesExactlyZeroSlope)
    {
        // Negative control FIRST: this fixture has to be in the regime where a
        // non-zero strength actually produces ripples, or the assertion below
        // passes on an empty field and pins nothing.
        ASSERT_GT(PeakSlope(3.0f, 1.0f, kFullDensity, 24, 6.0f), 0.0f)
            << "fixture produced no ripples at full strength — the OFF assertion below is vacuous";

        for (i32 z = 0; z < 24; ++z)
        {
            for (i32 x = 0; x < 24; ++x)
            {
                const glm::vec2 p{ 0.25f * static_cast<f32>(x), 0.25f * static_cast<f32>(z) };
                const glm::vec2 s = WR::RippleSlope(p, 3.0f, 0.0f, kFullDensity, WR::kCellSizeMetres);
                // EXACTLY zero, not "small": the shader's twin returns before
                // the cell walk on this branch, and a test that tolerated 1e-6
                // would keep passing if someone replaced the early-out with a
                // multiply by zero — which costs the full 9-cell walk every
                // fragment on a dry sea.
                EXPECT_EQ(s.x, 0.0f);
                EXPECT_EQ(s.y, 0.0f);
            }
        }
    }

    TEST(WaterRainRippleTest, NonFiniteInputsProduceZeroSlope)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        for (const glm::vec2 p : { glm::vec2(nan, 0.0f), glm::vec2(0.0f, inf) })
        {
            const glm::vec2 s = WR::RippleSlope(p, 3.0f, 1.0f, kFullDensity, WR::kCellSizeMetres);
            EXPECT_EQ(s.x, 0.0f);
            EXPECT_EQ(s.y, 0.0f);
        }

        const glm::vec2 badTime = WR::RippleSlope({ 1.0f, 1.0f }, nan, 1.0f, kFullDensity,
                                                  WR::kCellSizeMetres);
        EXPECT_EQ(badTime.x, 0.0f);
        EXPECT_EQ(badTime.y, 0.0f);
    }

    TEST(WaterRainRippleTest, SlopeStaysBoundedAtFullStrength)
    {
        // A ring whose slope blew up would tip the shading normal past
        // horizontal and read as black speckle, not as rain.
        //
        // THIS CAUGHT A REAL BUG, which is why the bound is physical rather
        // than generous: the profile is dimensionless and the slope divides by
        // a width in metres, so the first version — with no kRingHeightMetres
        // to multiply back in — described a one-metre-tall raindrop ripple and
        // peaked at 40.7, a surface tilted 88 degrees. A slope of 1.0 is 45
        // degrees, which no raindrop makes; the real peak is near 0.07.
        const f32 peak = PeakSlope(7.25f, 1.0f, kFullDensity, 96, 12.0f);
        EXPECT_TRUE(std::isfinite(peak));
        EXPECT_GT(peak, 0.0f) << "no ripple in the sampled area — the bound below is vacuous";
        EXPECT_LT(peak, 1.0f) << "peak ripple slope " << peak
                              << " tilts the surface past 45 degrees; a raindrop ring is "
                              << WR::kRingHeightMetres << " m tall over a "
                              << WR::kRingWidthMetres << " m wavelet";
    }

    // -------------------------------------------------------------------------
    // 2. Rings expand rather than pulsing
    // -------------------------------------------------------------------------

    TEST(WaterRainRippleTest, RingCrestRadiusStrictlyIncreasesWithAge)
    {
        f32 previous = -1.0f;
        for (i32 i = 0; i <= 20; ++i)
        {
            const f32 age = static_cast<f32>(i) / 20.0f;
            const f32 radius = WR::RingRadius(age);
            EXPECT_GT(radius, previous) << "at age " << age;
            previous = radius;
        }
        EXPECT_FLOAT_EQ(WR::RingRadius(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(WR::RingRadius(1.0f), WR::kRingMaxRadiusMetres);
    }

    TEST(WaterRainRippleTest, RingAmplitudeRampsInAndDecaysToZero)
    {
        // Both ends matter. A ring that starts at full amplitude POPS; one that
        // is still non-zero at age 1 FLICKERS, because the next cycle re-jitters
        // the centre and the ring teleports.
        EXPECT_FLOAT_EQ(WR::RingAmplitude(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(WR::RingAmplitude(1.0f), 0.0f);
        EXPECT_GT(WR::RingAmplitude(0.2f), WR::RingAmplitude(0.02f));
        EXPECT_GT(WR::RingAmplitude(0.3f), WR::RingAmplitude(0.9f));
    }

    TEST(WaterRainRippleTest, RingWidthGrowsSoTheCrestNeverOutrunsThePixel)
    {
        // The anti-alias term. An old ring is faint but far from its centre, so
        // its screen footprint shrinks; widening the wavelet is what stops it
        // becoming sub-pixel detail that has to be filtered instead of dropped
        // (docs/agent-rules/water-shading-nyquist.md §3).
        EXPECT_FLOAT_EQ(WR::RingWidth(0.0f), WR::kRingWidthMetres);
        EXPECT_GT(WR::RingWidth(1.0f), WR::RingWidth(0.0f));
    }

    TEST(WaterRainRippleTest, ProfileSlopeIsZeroBeyondTheCutoff)
    {
        EXPECT_EQ(WR::RingProfileSlope(WR::kRingCutoffWidths), 0.0f);
        EXPECT_EQ(WR::RingProfileSlope(-WR::kRingCutoffWidths), 0.0f);
        // ...and non-zero inside it, so the cutoff is not simply eating the
        // whole function.
        EXPECT_NE(WR::RingProfileSlope(0.0f), 0.0f);
    }

    TEST(WaterRainRippleTest, TheFieldEvolvesInTimeRatherThanStandingStill)
    {
        // "Rain visibly stipples the surface" is a claim about MOTION. Sampling
        // one fixed point across a ring lifetime has to see the field change;
        // a field that was constant in time would render as a static bumpy
        // texture and pass any single-frame check.
        const glm::vec2 probe{ 3.17f, -2.44f };
        std::vector<glm::vec2> samples;
        for (i32 i = 0; i < 12; ++i)
        {
            const f32 t = 10.0f + static_cast<f32>(i) * (WR::kRingLifetimeSeconds / 12.0f);
            samples.push_back(
                WR::RippleSlope(probe, t, 1.0f, kFullDensity, WR::kCellSizeMetres));
        }
        i32 distinct = 0;
        for (sizet i = 1; i < samples.size(); ++i)
        {
            if (samples[i] != samples[0])
                ++distinct;
        }
        EXPECT_GT(distinct, 0) << "the ripple field did not change over a whole ring lifetime";
    }

    // -------------------------------------------------------------------------
    // 3. Density
    // -------------------------------------------------------------------------

    TEST(WaterRainRippleTest, DensityRisesWithIntensityAndIsZeroWhenDry)
    {
        EXPECT_EQ(WR::DensityForIntensity(0.0f), 0.0f);
        EXPECT_EQ(WR::DensityForIntensity(-1.0f), 0.0f);
        EXPECT_EQ(WR::DensityForIntensity(std::numeric_limits<f32>::quiet_NaN()), 0.0f);
        EXPECT_GT(WR::DensityForIntensity(0.5f), WR::DensityForIntensity(0.1f));
        EXPECT_LE(WR::DensityForIntensity(1.0f), WR::kMaxDensity);
        EXPECT_LE(WR::DensityForIntensity(4.0f), WR::kMaxDensity) << "intensity is not clamped";
    }

    TEST(WaterRainRippleTest, MoreCellsFireAtHigherDensity)
    {
        const i32 sparse = ActiveSamples(11.0f, 1.0f, WR::DensityForIntensity(0.15f), 64, 10.0f);
        const i32 heavy = ActiveSamples(11.0f, 1.0f, WR::DensityForIntensity(1.0f), 64, 10.0f);
        EXPECT_GT(sparse, 0) << "even light rain must produce some rings";
        EXPECT_GT(heavy, sparse);
    }

    // -------------------------------------------------------------------------
    // 4. The two halves of the gate
    // -------------------------------------------------------------------------

    TEST_F(WaterRainRippleSystemTest, BothHalvesOfTheGateAreRequired)
    {
        WaterRain::WaterRainSettings on;
        on.m_Enabled = true;
        on.m_Strength = 1.0f;

        // Water half only.
        WaterRainRippleSystem::SetSettings(on);
        WaterRainRippleSystem::SetPrecipitation(false, 0.0f);
        EXPECT_EQ(WaterRainRippleSystem::GetShaderParams().x, 0.0f);

        // Sky half only.
        WaterRainRippleSystem::SetSettings(WaterRain::WaterRainSettings{});
        WaterRainRippleSystem::SetPrecipitation(true, 1.0f);
        EXPECT_EQ(WaterRainRippleSystem::GetShaderParams().x, 0.0f);

        // Both.
        WaterRainRippleSystem::SetSettings(on);
        WaterRainRippleSystem::SetPrecipitation(true, 1.0f);
        EXPECT_GT(WaterRainRippleSystem::GetShaderParams().x, 0.0f);
        EXPECT_GT(WaterRainRippleSystem::GetShaderParams().y, 0.0f) << "density must be non-zero too";
    }

    TEST_F(WaterRainRippleSystemTest, SnowProducesNoRipples)
    {
        WaterRain::WaterRainSettings on;
        on.m_Enabled = true;
        WaterRainRippleSystem::SetSettings(on);

        // `rippleCapable == false` is how RenderPipeline reports snow. A
        // snowflake melts on water; it does not ring.
        WaterRainRippleSystem::SetPrecipitation(false, 1.0f);
        EXPECT_EQ(WaterRainRippleSystem::GetShaderParams().x, 0.0f);

        // Negative control: the same intensity through the capable path DOES
        // ripple, so the assertion above is about the type and not about the
        // fixture being dry.
        WaterRainRippleSystem::SetPrecipitation(true, 1.0f);
        EXPECT_GT(WaterRainRippleSystem::GetShaderParams().x, 0.0f);
    }

    TEST_F(WaterRainRippleSystemTest, TheSmoothedIntensityTailSnapsToOff)
    {
        WaterRain::WaterRainSettings on;
        on.m_Enabled = true;
        WaterRainRippleSystem::SetSettings(on);

        // Negative control: this IS the regime the snap exists for. Model
        // PrecipitationSystem's lerp-toward-zero and confirm it never reaches
        // zero on its own, so a bare `intensity > 0` gate would keep the shader
        // walking 9 cells per fragment on a sea that stopped raining minutes
        // ago.
        f32 raw = 1.0f;
        for (i32 i = 0; i < 600; ++i)
            raw = std::lerp(raw, 0.0f, 0.05f);
        ASSERT_GT(raw, 0.0f) << "the exponential tail reached exactly zero — this test is vacuous";
        ASSERT_LT(raw, WaterRain::kMinIntensity);

        WaterRainRippleSystem::SetPrecipitation(true, raw);
        EXPECT_EQ(WaterRainRippleSystem::GetShaderParams().x, 0.0f)
            << "a decayed intensity tail left the ripple field switched on";

        // And the snap does not eat an intensity anyone can see.
        WaterRainRippleSystem::SetPrecipitation(true, WaterRain::kMinIntensity);
        EXPECT_GT(WaterRainRippleSystem::GetShaderParams().x, 0.0f);
    }

    TEST_F(WaterRainRippleSystemTest, StrengthScalesWithIntensityRatherThanOnlyGatingDensity)
    {
        WaterRain::WaterRainSettings on;
        on.m_Enabled = true;
        on.m_Strength = 1.0f;
        WaterRainRippleSystem::SetSettings(on);

        WaterRainRippleSystem::SetPrecipitation(true, 1.0f);
        const f32 heavy = WaterRainRippleSystem::GetShaderParams().x;
        WaterRainRippleSystem::SetPrecipitation(true, 0.25f);
        const f32 light = WaterRainRippleSystem::GetShaderParams().x;

        EXPECT_GT(heavy, light) << "drizzle rings are as deep as a downpour's";
    }

    TEST_F(WaterRainRippleSystemTest, FadeEndpointsAreOrdered)
    {
        WaterRain::WaterRainSettings inverted;
        inverted.m_Enabled = true;
        inverted.m_FadeStartMetres = 90.0f;
        inverted.m_FadeEndMetres = 10.0f;
        WaterRainRippleSystem::SetSettings(inverted);

        const glm::vec4 p2 = WaterRainRippleSystem::GetShaderParams2();
        // smoothstep(edge0, edge1, x) is undefined for edge0 >= edge1, so an
        // inverted authored pair must not reach the shader as authored.
        EXPECT_GE(p2.y, p2.x + 1.0f);
    }

    TEST_F(WaterRainRippleSystemTest, NonFiniteSettingsFallBackRatherThanPropagate)
    {
        WaterRain::WaterRainSettings bad;
        bad.m_Enabled = true;
        bad.m_Strength = std::numeric_limits<f32>::quiet_NaN();
        bad.m_FadeStartMetres = std::numeric_limits<f32>::infinity();
        bad.m_FadeEndMetres = std::numeric_limits<f32>::quiet_NaN();
        WaterRainRippleSystem::SetSettings(bad);
        WaterRainRippleSystem::SetPrecipitation(true, std::numeric_limits<f32>::quiet_NaN());

        const glm::vec4 p = WaterRainRippleSystem::GetShaderParams();
        const glm::vec4 p2 = WaterRainRippleSystem::GetShaderParams2();
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        EXPECT_TRUE(std::isfinite(p2.x) && std::isfinite(p2.y));
        // A NaN intensity is not rain.
        EXPECT_EQ(p.x, 0.0f);
    }

    TEST_F(WaterRainRippleSystemTest, ResetClearsBothHalves)
    {
        WaterRain::WaterRainSettings on;
        on.m_Enabled = true;
        WaterRainRippleSystem::SetSettings(on);
        WaterRainRippleSystem::SetPrecipitation(true, 1.0f);
        ASSERT_GT(WaterRainRippleSystem::GetShaderParams().x, 0.0f);

        // A runtime scene switch must not leave the previous scene's weather on
        // the new scene's sea — the cross-frame-history defect
        // docs/agent-rules/runtime-scene-switching.md is about.
        WaterRainRippleSystem::Reset();
        EXPECT_EQ(WaterRainRippleSystem::GetShaderParams().x, 0.0f);
        EXPECT_FALSE(WaterRainRippleSystem::GetSettings().m_Enabled);
    }

    // -------------------------------------------------------------------------
    // 5. The GLSL twin
    // -------------------------------------------------------------------------

    TEST(WaterRainRippleTest, GlslTwinCarriesTheSameContractConstants)
    {
        const std::string glsl = ReadRippleGlsl();
        ASSERT_FALSE(glsl.empty()) << "could not read include/WaterRainCommon.glsl";

        // The hash magic numbers. These are why the hash is integer at all: they
        // are exact on both sides, so this compares the actual arithmetic rather
        // than hoping two float expressions round the same way.
        for (const char* magic : { "0x7feb352du", "0x846ca68bu", "0x9e3779b9u", "0x85ebca6bu",
                                   "0xc2b2ae35u", "0x27d4eb2fu" })
        {
            EXPECT_NE(glsl.find(magic), std::string::npos)
                << "WaterRainCommon.glsl is missing hash constant " << magic
                << " — the GLSL ripple field no longer matches WaterRainRipples.h";
        }

        // The ring constants, as literals. A drift here does not error; it moves
        // every ring by a few centimetres on the GPU only, which no CPU test and
        // no single screenshot can see.
        struct NamedConstant
        {
            const char* m_Token;
            f32 m_Expected;
        };
        const NamedConstant constants[] = {
            { "WATER_RAIN_RING_LIFETIME = ", WR::kRingLifetimeSeconds },
            { "WATER_RAIN_RING_MAX_RADIUS = ", WR::kRingMaxRadiusMetres },
            { "WATER_RAIN_RING_HEIGHT = ", WR::kRingHeightMetres },
            { "WATER_RAIN_RING_WIDTH = ", WR::kRingWidthMetres },
            { "WATER_RAIN_RING_CUTOFF = ", WR::kRingCutoffWidths },
            { "WATER_RAIN_JITTER_ORIGIN = ", WR::kJitterOrigin },
            { "WATER_RAIN_JITTER_EXTENT = ", WR::kJitterExtent },
        };
        for (const NamedConstant& c : constants)
        {
            const sizet at = glsl.find(c.m_Token);
            ASSERT_NE(at, std::string::npos) << "WaterRainCommon.glsl is missing " << c.m_Token;
            const sizet valueStart = at + std::string(c.m_Token).size();
            const sizet valueEnd = glsl.find(';', valueStart);
            ASSERT_NE(valueEnd, std::string::npos);
            const std::string text = glsl.substr(valueStart, valueEnd - valueStart);
            EXPECT_NEAR(std::stof(text), c.m_Expected, 1.0e-6f)
                << c.m_Token << "is " << text << " in GLSL but " << c.m_Expected << " in C++";
        }

        // The early-out itself, not merely its effect. This is the line that
        // makes "no cost when rain is off" true, and it is one edit away from
        // becoming a multiply by zero that still passes every value assertion in
        // this file.
        // Asserted as an ORDERING rather than as a text match, so it survives a
        // reformat but not a semantic change: the guard has to come before the
        // loop, in the same function.
        const sizet fnAt = glsl.find("vec2 waterRainRippleSlope(");
        ASSERT_NE(fnAt, std::string::npos) << "waterRainRippleSlope is gone from WaterRainCommon.glsl";
        const sizet guardAt = glsl.find("params.x <= 0.0", fnAt);
        const sizet loopAt = glsl.find("for (int dz", fnAt);
        ASSERT_NE(guardAt, std::string::npos)
            << "waterRainRippleSlope has no `params.x <= 0.0` guard at all";
        ASSERT_NE(loopAt, std::string::npos) << "waterRainRippleSlope has no cell walk";
        EXPECT_LT(guardAt, loopAt)
            << "waterRainRippleSlope no longer returns BEFORE the cell walk when rain is off — "
               "the 'no cost when rain is off' criterion is a multiply by zero now";
    }
} // namespace OloEngine::Tests
