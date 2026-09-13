// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReSTIR/PathReplayCharts.h"

#include <limits>
#include <random>

namespace OloEngine::Tests
{
    namespace Charts = ReSTIR::ReplayCharts;

    TEST(PathReplayCharts, ExistingOracleSamplersInvertOnNonAxisAlignedFrames)
    {
        std::mt19937 rng(1211u);
        std::uniform_real_distribution<f32> uniform(0.025f, 0.975f);
        for (const auto chart : { Charts::Chart::Cosine, Charts::Chart::LegacyGGX, Charts::Chart::VisibleGGX })
        {
            u32 accepted = 0;
            for (u32 i = 0; i < 1000u; ++i)
            {
                const glm::vec3 normal = glm::normalize(glm::vec3(uniform(rng) - 0.5f, uniform(rng) - 0.5f,
                                                                  (i % 2u == 0u ? 1.0f : -1.0f) * uniform(rng)));
                glm::vec3 tangent, bitangent;
                PathTracing::OrthonormalBasis(normal, tangent, bitangent);
                const glm::vec3 view = glm::normalize(normal + tangent * (uniform(rng) - 0.5f) * 2.0f +
                                                      bitangent * (uniform(rng) - 0.5f));
                const Charts::Frame frame{ normal, view, 0.2f + 0.7f * uniform(rng) };
                const glm::vec2 xi(uniform(rng), uniform(rng));
                glm::vec3 oracle;
                switch (chart)
                {
                    case Charts::Chart::Cosine:
                        oracle = PathTracing::CosineSampleHemisphere(xi, normal);
                        break;
                    case Charts::Chart::LegacyGGX:
                        oracle = glm::reflect(-view, PathTracing::ImportanceSampleGGX(xi, normal, frame.Roughness));
                        break;
                    case Charts::Chart::VisibleGGX:
                        oracle = glm::reflect(-view, PathTracing::SampleGGXVNDF(normal, view, frame.Roughness, xi));
                        break;
                }
                if (!(glm::dot(normal, oracle) > 0.0f))
                {
                    EXPECT_FALSE(Charts::Forward(chart, frame, xi));
                    continue;
                }
                const auto inverse = Charts::Inverse(chart, frame, oracle);
                ASSERT_TRUE(inverse) << "chart=" << static_cast<u32>(chart) << " sample=" << i;
                EXPECT_NEAR(inverse->x, xi.x, 1.0e-4f);
                EXPECT_NEAR(inverse->y, xi.y, 1.0e-4f);
                const auto replay = Charts::Forward(chart, frame, *inverse);
                ASSERT_TRUE(replay);
                EXPECT_LT(glm::length(*replay - oracle), 2.0e-5f);
                ++accepted;
            }
            EXPECT_GT(accepted, 500u);
        }
    }

    TEST(PathReplayCharts, AuthoredZeroRoughnessUsesTheOracleFiniteWidthFloor)
    {
        const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, -0.2f, 1.0f));
        const Charts::Frame frame{ normal, glm::normalize(normal + glm::vec3(0.1f, 0.1f, 0.0f)), 0.0f };
        for (const auto chart : { Charts::Chart::LegacyGGX, Charts::Chart::VisibleGGX })
        {
            const auto sample = Charts::Forward(chart, frame, glm::vec2(0.45f, 0.62f));
            ASSERT_TRUE(sample);
            Charts::Frame floored = frame;
            floored.Roughness = PathTracing::kMinRoughness;
            const auto reference = Charts::Forward(chart, floored, glm::vec2(0.45f, 0.62f));
            ASSERT_TRUE(reference);
            EXPECT_LT(glm::length(*sample - *reference), 1.0e-7f);
            // Replay retains these original uniforms. It must not recover
            // near-specular uniforms by inverting the rounded direction.
            const auto replay = Charts::Forward(chart, frame, glm::vec2(0.45f, 0.62f));
            ASSERT_TRUE(replay);
            EXPECT_LT(glm::length(*replay - *sample), 1.0e-7f);
        }
    }

    TEST(PathReplayCharts, LegacyRoundedPoleCannotRecoverTheOriginalUniforms)
    {
        Charts::Frame frame;
        frame.Roughness = 0.0f;
        // Two different azimuths collapse to the same float cos(theta)==1
        // inside the existing oracle. There is no unique directional inverse.
        const auto first = Charts::Forward(Charts::Chart::LegacyGGX, frame, glm::vec2(0.2f, 0.001f));
        const auto second = Charts::Forward(Charts::Chart::LegacyGGX, frame, glm::vec2(0.7f, 0.001f));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_LT(glm::length(*first - *second), 1.0e-7f);
        EXPECT_FALSE(Charts::Inverse(Charts::Chart::LegacyGGX, frame, *first));
        EXPECT_FALSE(Charts::Inverse(Charts::Chart::LegacyGGX, frame, *second));
    }

    TEST(PathReplayCharts, ChangedLobeProbabilityPreservesRawUniformAndChangesResidual)
    {
        for (const f32 raw : { 0.12f, 0.88f })
        {
            const auto source = Charts::DecodeLobe(raw, 0.3f);
            ASSERT_TRUE(source);
            const auto destination = Charts::ReplayLobe(raw, 0.3f, 0.6f);
            ASSERT_TRUE(destination);
            const auto recovered = Charts::EncodeLobe(*destination, 0.6f);
            ASSERT_TRUE(recovered);
            EXPECT_NEAR(*recovered, raw, 1.0e-7f);
            const auto reverse = Charts::ReplayLobe(raw, 0.6f, 0.3f);
            ASSERT_TRUE(reverse);
            EXPECT_NEAR(reverse->Residual, source->Residual, 2.0e-7f);
            // Negative control: retaining normalized residual changes the
            // underlying uniform and therefore implements a different shift.
            const auto wrong = Charts::EncodeLobe(*source, 0.6f);
            ASSERT_TRUE(wrong);
            EXPECT_GT(std::abs(*wrong - raw), 0.05f);
        }
        const auto changesBranch = Charts::DecodeLobe(0.45f, 0.3f);
        ASSERT_TRUE(changesBranch);
        EXPECT_FALSE(Charts::ReplayLobe(0.45f, 0.3f, 0.6f));
    }

    TEST(PathReplayCharts, ResidualChangeHasTheBranchProbabilityJacobian)
    {
        constexpr f32 step = 0.001f;
        for (const bool specular : { false, true })
        {
            const auto rawA = Charts::EncodeLobe({ specular, 0.7f - step }, 0.3f);
            const auto rawB = Charts::EncodeLobe({ specular, 0.7f + step }, 0.3f);
            ASSERT_TRUE(rawA);
            ASSERT_TRUE(rawB);
            const auto a = Charts::ReplayLobe(*rawA, 0.3f, 0.6f);
            const auto b = Charts::ReplayLobe(*rawB, 0.3f, 0.6f);
            ASSERT_TRUE(a);
            ASSERT_TRUE(b);
            const f32 derivative = (b->Residual - a->Residual) / (2.0f * step);
            const f32 ratio = specular ? 0.3f / 0.6f : 0.7f / 0.4f;
            EXPECT_NEAR(derivative, ratio, 1.0e-4f);
            EXPECT_GT(std::abs(derivative - 1.0f), 0.4f);
        }
    }

    TEST(PathReplayCharts, InvalidDomainsDoNotProduceCoordinates)
    {
        const Charts::Frame frame{};
        for (const auto chart : { Charts::Chart::Cosine, Charts::Chart::LegacyGGX, Charts::Chart::VisibleGGX })
        {
            EXPECT_FALSE(Charts::Forward(chart, frame, glm::vec2(0.0f, 0.3f)));
            EXPECT_FALSE(Charts::Forward(chart, frame, glm::vec2(0.3f, 1.0f)));
            EXPECT_FALSE(Charts::Inverse(chart, frame, -frame.Normal));
            EXPECT_FALSE(Charts::Inverse(chart, frame, glm::vec3(1.0f, 0.0f, 0.0f)));
            EXPECT_FALSE(Charts::Inverse(chart, frame, glm::vec3(0.0f)));
            Charts::Frame invalid = frame;
            invalid.View = -frame.Normal;
            EXPECT_FALSE(Charts::Forward(chart, invalid, glm::vec2(0.3f)));
            invalid = frame;
            invalid.Roughness = std::numeric_limits<f32>::quiet_NaN();
            EXPECT_FALSE(Charts::Forward(chart, invalid, glm::vec2(0.3f)));
        }
        EXPECT_FALSE(Charts::DecodeLobe(0.3f, 0.3f));
        EXPECT_FALSE(Charts::DecodeLobe(0.0f, 0.3f));
        EXPECT_FALSE(Charts::DecodeLobe(0.5f, 1.0f));
        EXPECT_FALSE(Charts::EncodeLobe({ true, 1.0f }, 0.3f));
    }
} // namespace OloEngine::Tests
