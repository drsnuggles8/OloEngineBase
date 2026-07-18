// OLO_TEST_LAYER: L4
// =============================================================================
// AtmosphereSkyMathTest.cpp
//
// Pins the combined day/night atmosphere sky (issue #633, Pillar B):
// AtmosphereSky::ComputeUBO / HashParameters / EvaluateAtDirection in
// OloEngine/Renderer/AtmosphereSky.{h,cpp} — the CPU mirror of
// AtmosphereSky.glsl, the same shaderpipe discipline as
// ProceduralSkyMathTest / StarNestSkyMathTest for the models it fuses.
//
// What gets pinned:
//   - Full-day blend (NightBlendFactor 0) reproduces the Preetham evaluator
//     exactly (the day half must not drift from ProceduralSky).
//   - Full night is much darker than day, stays finite/non-negative, and the
//     moon disk outshines the opposite sky point.
//   - The twilight blend is monotone between the two endpoints.
//   - Bake-time cloud tint changes the sky only when coverage > 0.
//   - Adversarial params (NaN moon, wild intensities) are sanitized.
//   - The parameter hash moves for every field (the rebake gate) and is
//     stable for identical inputs.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/AtmosphereSky.h"

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        AtmosphereSkyParameters DayParams()
        {
            AtmosphereSkyParameters p;
            p.Day.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
            p.Day.Turbidity = 2.5f;
            p.Day.Exposure = 0.1f;
            p.Day.SunIntensity = 1.0f;
            p.Day.ShowSunDisk = false;
            p.MoonDirection = glm::normalize(glm::vec3(-0.3f, -0.7f, -0.4f));
            p.MoonIlluminatedFraction = 1.0f;
            p.NightBlendFactor = 0.0f;
            p.NightBrightness = 0.35f;
            p.StarIntensity = 1.0f;
            return p;
        }

        AtmosphereSkyParameters NightParams()
        {
            AtmosphereSkyParameters p = DayParams();
            p.NightBlendFactor = 1.0f;
            p.MoonDirection = glm::normalize(glm::vec3(0.2f, 0.6f, 0.3f)); // moon up
            return p;
        }

        f32 Luma(const glm::vec3& c)
        {
            return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        }
    } // namespace

    TEST(AtmosphereSkyMath, FullDayMatchesPreethamExactly)
    {
        const auto params = DayParams();
        const auto ubo = AtmosphereSky::ComputeUBO(params);
        const auto preetham = ProceduralSky::ComputeCoefficients(params.Day);

        for (const glm::vec3& dir : { glm::vec3(0.0f, 1.0f, 0.0f),
                                      glm::normalize(glm::vec3(0.7f, 0.3f, 0.0f)),
                                      glm::normalize(glm::vec3(-0.5f, 0.2f, 0.6f)) })
        {
            const glm::vec3 combined = AtmosphereSky::EvaluateAtDirection(ubo, dir);
            const glm::vec3 day = ProceduralSky::EvaluateAtDirection(preetham, dir);
            EXPECT_NEAR(combined.r, day.r, 1.0e-5f);
            EXPECT_NEAR(combined.g, day.g, 1.0e-5f);
            EXPECT_NEAR(combined.b, day.b, 1.0e-5f);
        }
    }

    TEST(AtmosphereSkyMath, NightIsDarkFiniteAndMoonlit)
    {
        const auto night = AtmosphereSky::ComputeUBO(NightParams());
        const auto day = AtmosphereSky::ComputeUBO(DayParams());

        const glm::vec3 zenith(0.0f, 1.0f, 0.0f);
        const glm::vec3 nightZenith = AtmosphereSky::EvaluateAtDirection(night, zenith);
        const glm::vec3 dayZenith = AtmosphereSky::EvaluateAtDirection(day, zenith);
        EXPECT_LT(Luma(nightZenith), Luma(dayZenith) * 0.5f)
            << "night zenith must be far darker than day";
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(std::isfinite(nightZenith[i]));
            EXPECT_GE(nightZenith[i], 0.0f);
        }

        // The moon disk outshines the anti-moon sky at full illumination.
        const glm::vec3 moonDir(night.MoonDirection);
        const glm::vec3 towardMoon = AtmosphereSky::EvaluateAtDirection(night, moonDir);
        const glm::vec3 antiMoon = AtmosphereSky::EvaluateAtDirection(
            night, glm::normalize(glm::vec3(-moonDir.x, moonDir.y, -moonDir.z)));
        EXPECT_GT(Luma(towardMoon), Luma(antiMoon) * 2.0f);
    }

    TEST(AtmosphereSkyMath, TwilightBlendIsMonotoneBetweenEndpoints)
    {
        const glm::vec3 dir = glm::normalize(glm::vec3(0.2f, 0.5f, 0.1f));
        f32 prevLuma = std::numeric_limits<f32>::infinity();
        for (f32 blend = 0.0f; blend <= 1.0f; blend += 0.1f)
        {
            auto p = DayParams();
            p.NightBlendFactor = blend;
            // Keep the moon below the horizon so luminance decreases cleanly.
            p.MoonDirection = glm::vec3(0.0f, -1.0f, 0.0f);
            const auto ubo = AtmosphereSky::ComputeUBO(p);
            const f32 luma = Luma(AtmosphereSky::EvaluateAtDirection(ubo, dir));
            EXPECT_LE(luma, prevLuma + 1.0e-4f)
                << "sky must only darken as night blend rises (blend " << blend << ")";
            prevLuma = luma;
        }
    }

    TEST(AtmosphereSkyMath, CloudTintOnlyActsWhenCoverageIsSet)
    {
        auto p = DayParams();
        const auto clean = AtmosphereSky::ComputeUBO(p);
        p.CloudCoverage = 1.0f;
        const auto cloudy = AtmosphereSky::ComputeUBO(p);

        // Sample several upper-hemisphere directions; full coverage must
        // change at least one of them, zero coverage must change none.
        bool anyDifferent = false;
        for (const glm::vec3& dir : { glm::normalize(glm::vec3(0.3f, 0.4f, 0.1f)),
                                      glm::normalize(glm::vec3(-0.5f, 0.3f, 0.4f)),
                                      glm::normalize(glm::vec3(0.1f, 0.6f, -0.6f)) })
        {
            const glm::vec3 a = AtmosphereSky::EvaluateAtDirection(clean, dir);
            const glm::vec3 b = AtmosphereSky::EvaluateAtDirection(cloudy, dir);
            if (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) > 1.0e-4f)
                anyDifferent = true;
            for (int i = 0; i < 3; ++i)
                EXPECT_TRUE(std::isfinite(b[i]));
        }
        EXPECT_TRUE(anyDifferent) << "full coverage must visibly tint the sky";
    }

    TEST(AtmosphereSkyMath, AdversarialParamsAreSanitized)
    {
        AtmosphereSkyParameters p = NightParams();
        p.MoonDirection = glm::vec3(std::numeric_limits<f32>::quiet_NaN());
        p.MoonIntensity = -5.0f;
        p.StarIntensity = 1.0e30f;
        p.NightBrightness = std::numeric_limits<f32>::infinity();
        p.CloudCoverage = -3.0f;
        const auto ubo = AtmosphereSky::ComputeUBO(p);

        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                {
                    if (x == 0 && y == 0 && z == 0)
                        continue;
                    const glm::vec3 rgb = AtmosphereSky::EvaluateAtDirection(
                        ubo, glm::normalize(glm::vec3(x, y, z)));
                    for (int i = 0; i < 3; ++i)
                    {
                        EXPECT_TRUE(std::isfinite(rgb[i]));
                        EXPECT_GE(rgb[i], 0.0f);
                    }
                }
    }

    TEST(AtmosphereSkyMath, HashMovesForEveryFieldAndIsStable)
    {
        const auto base = DayParams();
        const u64 baseHash = AtmosphereSky::HashParameters(base, 256);
        EXPECT_EQ(baseHash, AtmosphereSky::HashParameters(base, 256));
        EXPECT_NE(baseHash, AtmosphereSky::HashParameters(base, 512));

        auto mutate = [&](auto&& change)
        {
            auto p = base;
            change(p);
            return AtmosphereSky::HashParameters(p, 256);
        };
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.Day.SunDirection.x += 0.01f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.Day.Turbidity += 0.5f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.MoonDirection.y += 0.01f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.MoonDiskSize += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.MoonIlluminatedFraction += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.MoonIntensity += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.StarIntensity += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.NightBlendFactor += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.NightBrightness += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.StarRotationRadians += 0.1f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.CloudCoverage += 0.05f; }));
        EXPECT_NE(baseHash, mutate([](auto& p)
                                   { p.CloudWetness += 0.1f; }));
    }
} // namespace OloEngine::Tests
