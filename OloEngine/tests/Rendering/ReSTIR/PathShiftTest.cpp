// OLO_TEST_LAYER: L1
// These tests exercise the production shift arithmetic with analytic charts and
// independent quadrature. They do NOT exercise runtime path construction, inverse
// tracing, sampler inversion, visibility rays, or the GPU pipeline.

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReSTIR/PathShift.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace
{
    using namespace OloEngine::ReSTIR;
    using namespace OloEngine::ReSTIR::PT;

    // Smooth bijection of the unit interval. Its inverse and derivative come
    // from this explicit coordinate map, independently of the production density
    // arithmetic. A unit interval can parameterize hemisphere cosine or azimuth.
    f64 Warp(f64 x)
    {
        return 0.5 * x * (1.0 + x);
    }

    f64 Unwarp(f64 y)
    {
        return 0.5 * (std::sqrt(1.0 + 8.0 * y) - 1.0);
    }

    f64 WarpDerivative(f64 x)
    {
        return 0.5 + x;
    }

    f64 Signal(f64 y)
    {
        return 1.0 + y * y;
    }

    template<typename Function>
    f64 Quadrature(Function function, u32 count = 16384u)
    {
        f64 sum = 0.0;
        for (u32 i = 0; i < count; ++i)
            sum += function((static_cast<f64>(i) + 0.5) / count);
        return sum / count;
    }

    TEST(PathShiftTest, ReconnectionPreservesAreaDensityAndReciprocityOnRandomGeometry)
    {
        std::mt19937 random(1211u);
        std::uniform_real_distribution<f32> coordinate(-1.0f, 1.0f);
        for (u32 iteration = 0; iteration < 512u; ++iteration)
        {
            const glm::vec3 vertex(coordinate(random), coordinate(random), coordinate(random));
            const glm::vec3 normal = glm::normalize(glm::vec3(0.4f + 0.1f * coordinate(random),
                                                              0.7f, 0.6f));
            const glm::vec3 tangent = glm::normalize(glm::cross(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
            const glm::vec3 source = vertex + normal * (2.0f + 0.4f * coordinate(random)) +
                                     tangent * coordinate(random);
            const glm::vec3 destination = vertex + normal * (2.0f + 0.4f * coordinate(random)) +
                                          tangent * coordinate(random);
            const ConnectionEndpoint held{ vertex, normal };
            const ConnectionEndpoint from{ source, glm::normalize(vertex - source) };
            const ConnectionEndpoint to{ destination, glm::normalize(vertex - destination) };
            ASSERT_EQ(ConnectionDomain(from, to, held, true, true), ShiftRejection::None);
            const f32 jacobian = ReconnectionJacobian(vertex, normal, destination, source);
            const f32 reverse = ReconnectionJacobian(vertex, normal, source, destination);
            const f32 sourcePdf = SolidAnglePdfFromAreaPdf(0.37f, vertex, normal, source);
            const f32 destinationPdf = SolidAnglePdfFromAreaPdf(0.37f, vertex, normal, destination);
            EXPECT_NEAR(destinationPdf, sourcePdf / jacobian, 2.0e-6f * destinationPdf);
            EXPECT_NEAR(jacobian * reverse, 1.0f, 1.0e-6f);

            // A replay prefix followed by this geometric connection must agree
            // with direct conversion of the joint density at the destination.
            const f64 x = (static_cast<f64>(iteration) + 0.5) / 512.0;
            ShiftJacobianProduct hybrid;
            ASSERT_TRUE(hybrid.AppendReplay(0.5, 0.5 / WarpDerivative(x)));
            ASSERT_TRUE(hybrid.Append(jacobian));
            EXPECT_NEAR(0.5 * sourcePdf / hybrid.Value(),
                        (0.5 / WarpDerivative(x)) * destinationPdf, 4.0e-6 * destinationPdf);
        }
    }

    TEST(PathShiftTest, ConnectionDomainRejectsEachSignedEndpointAndVisibilityIndependently)
    {
        const ConnectionEndpoint held{ { 0.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };
        const ConnectionEndpoint source{ { -0.7f, 0.0f, 0.2f }, { 0.0f, 1.0f, 0.0f } };
        const ConnectionEndpoint destination{ { 0.9f, 0.0f, -0.3f }, { 0.0f, 1.0f, 0.0f } };
        ASSERT_EQ(ConnectionDomain(source, destination, held, true, true), ShiftRejection::None);
        for (const bool changeSource : { false, true })
        {
            auto from = source;
            auto to = destination;
            auto& changed = changeSource ? from : to;
            changed.Normal = -changed.Normal;
            EXPECT_EQ(ConnectionDomain(from, to, held, true, true), ShiftRejection::Hemisphere);

            // The receiver still faces the held vertex, but now sees its back.
            changed.Position.y = 4.0f;
            EXPECT_EQ(ConnectionDomain(from, to, held, true, true), ShiftRejection::Hemisphere);
            changed.Position = held.Position + glm::vec3(0.0f, -0.01f, 0.0f);
            changed.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
            EXPECT_EQ(ConnectionDomain(from, to, held, true, true), ShiftRejection::Distance);
        }
        EXPECT_EQ(ConnectionDomain(source, destination, held, false, true), ShiftRejection::Occluded);
        EXPECT_EQ(ConnectionDomain(source, destination, held, true, false), ShiftRejection::Occluded);
        auto invalid = held;
        invalid.Normal = glm::vec3(0.0f);
        EXPECT_EQ(ConnectionDomain(source, destination, invalid, true, true), ShiftRejection::NonFinite);
        invalid = held;
        invalid.Position.x = std::numeric_limits<f32>::infinity();
        EXPECT_EQ(ConnectionDomain(source, destination, invalid, true, true), ShiftRejection::NonFinite);
    }

    TEST(PathShiftTest, ReconnectionMatchesAnalyticSolidAngleAndDroppingItDoesNot)
    {
        // Unit square at y=3. Its solid angle from the centered destination is
        // known analytically; the source is farther away and deliberately offset.
        const glm::vec3 source(0.4f, 0.0f, 0.2f);
        const glm::vec3 destination(0.0f, 1.0f, 0.0f);
        const glm::vec3 normal(0.0f, -1.0f, 0.0f);
        const f64 truth = 4.0 * std::atan(0.25 / (2.0 * std::sqrt(4.5)));
        const auto estimate = [&](bool omitConnection)
        {
            return Quadrature([&](f64 x)
                              { return Quadrature([&](f64 z)
                                                  {
                    const glm::vec3 held(static_cast<f32>(x - 0.5), 3.0f, static_cast<f32>(z - 0.5));
                    const f64 sourceWeight = 1.0 / SolidAnglePdfFromAreaPdf(1.0f, held, normal, source);
                    const f64 jacobian = ReconnectionJacobian(held, normal, destination, source);
                    return ShiftedCandidateWeight(sourceWeight, omitConnection ? 1.0 : jacobian, 1.0, 1.0); }, 256u); }, 256u);
        };
        EXPECT_NEAR(estimate(false), truth, 1.0e-6);
        EXPECT_GT(std::abs(estimate(true) - truth), 0.05);
    }

    TEST(PathShiftTest, ProductConditioningIsReciprocalAndRejectsCancellation)
    {
        std::mt19937 random(979u);
        std::uniform_real_distribution<f64> logFactor(-0.9, 0.9);
        for (u32 iteration = 0; iteration < 256u; ++iteration)
        {
            ShiftJacobianProduct forward;
            ShiftJacobianProduct reverse;
            for (u32 vertex = 0; vertex < 4u; ++vertex)
            {
                const f64 factor = std::exp(logFactor(random));
                const bool accepted = forward.Append(factor);
                EXPECT_EQ(reverse.Append(1.0 / factor), accepted);
            }
            EXPECT_EQ(forward.Rejection(), reverse.Rejection());
            if (forward.Rejection() == ShiftRejection::None)
                EXPECT_NEAR(forward.Value() * reverse.Value(), 1.0, 2.0e-15);
        }
        ShiftJacobianProduct cancellation;
        ASSERT_TRUE(cancellation.Append(4.0));
        EXPECT_FALSE(cancellation.Append(0.25)); // Product is one; intermediate distortion is not.
        EXPECT_EQ(cancellation.Rejection(), ShiftRejection::Conditioning);
        EXPECT_FALSE(cancellation.Append(1.0));
        EXPECT_DOUBLE_EQ(cancellation.Value(), 0.0);
        for (const f64 bad : { 0.0, -1.0, std::numeric_limits<f64>::infinity(),
                               std::numeric_limits<f64>::quiet_NaN() })
        {
            ShiftJacobianProduct invalid;
            EXPECT_FALSE(invalid.AppendReplay(1.0, bad));
            EXPECT_EQ(invalid.Rejection(), ShiftRejection::NonFinite);
        }
    }

    TEST(PathShiftTest, ReplayAndEveryProductFactorPreserveAnIndependentIntegral)
    {
        const f64 reference = Quadrature(Signal);
        EXPECT_NEAR(reference, 4.0 / 3.0, 1.0e-9);
        const auto estimate = [](bool dropFirst, bool dropSecond, bool dropMapping)
        {
            return Quadrature([&](f64 x)
                              { return Quadrature([&](f64 y)
                                                  {
                    ShiftJacobianProduct replay;
                    if (!dropFirst)
                        (void)replay.AppendReplay(0.5, 0.5 / WarpDerivative(x));
                    if (!dropSecond)
                        (void)replay.AppendReplay(0.5, 0.5 / WarpDerivative(y));
                    const f64 target = Signal(dropMapping ? x : Warp(x)) *
                                       Signal(dropMapping ? y : Warp(y));
                    return ShiftedCandidateWeight(1.0, replay.Value(), target, 1.0); }, 256u); }, 256u);
        };
        const f64 truth = reference * reference;
        EXPECT_NEAR(estimate(false, false, false), truth, 3.0e-5);
        EXPECT_GT(std::abs(estimate(true, false, false) - truth), 0.05);
        EXPECT_GT(std::abs(estimate(false, true, false) - truth), 0.05);
        EXPECT_GT(std::abs(estimate(true, true, false) - truth), 0.1);
        EXPECT_GT(std::abs(estimate(false, false, true) - truth), 0.1);
    }

    TEST(PathShiftTest, MixtureChartIntegralNeedsConditionalDensityRatherThanMixtureDensity)
    {
        // Source: two equiprobable uniform charts. Destination: chart zero uses
        // Warp, chart one identity. Their images overlap; extended integrands
        // must sum to the physical signal rather than count it twice.
        const auto estimate = [](bool useMixtureInsteadOfChart, bool omitJacobian)
        {
            f64 result = 0.0;
            for (u32 chart = 0; chart < 2u; ++chart)
            {
                result += Quadrature([&](f64 x)
                                     {
                    const f64 y = chart == 0u ? Warp(x) : x;
                    const f64 conditional = chart == 0u ? 1.0 / WarpDerivative(x) : 1.0;
                    const f64 mixture = 0.5 / WarpDerivative(Unwarp(y)) + 0.5;
                    const f64 extendedTarget = Signal(y) * (0.5 * conditional) / mixture;
                    ShiftJacobianProduct replay;
                    if (useMixtureInsteadOfChart)
                        (void)replay.AppendReplay(1.0, mixture);
                    else
                        (void)replay.AppendReplay(0.5, 0.5 * conditional);
                    // Averaging each stratum explicitly cancels its 1/2 draw
                    // probability against source W=2. Residual integrates to one.
                    return 0.5 * ShiftedCandidateWeight(2.0, omitJacobian ? 1.0 : replay.Value(),
                                                       extendedTarget, 1.0); });
            }
            return result;
        };
        const f64 truth = Quadrature(Signal);
        EXPECT_NEAR(estimate(false, false), truth, 1.0e-8);
        EXPECT_GT(std::abs(estimate(true, false) - truth), 0.005);
        EXPECT_GT(std::abs(estimate(false, true) - truth), 0.005);
    }

    TEST(PathShiftTest, FiniteWeightsSurviveIntermediateOverflowAndUnderflow)
    {
        EXPECT_NEAR(ShiftedCandidateWeight(1.0e308, 2.0, 1.0e-308, 1.0), 2.0, 1.0e-12);
        const f64 tiny = 2.0 * std::numeric_limits<f64>::denorm_min();
        const f64 expected = (tiny * 1.0e300) * 0.125;
        const f64 actual = ShiftedCandidateWeight(tiny, 0.125, 1.0e300, 1.0);
        EXPECT_GT(actual, 0.0);
        EXPECT_NEAR(actual, expected, expected * 1.0e-12);
        EXPECT_DOUBLE_EQ(ShiftedCandidateWeight(1.0, 1.0, 1.0, 1.1), 0.0);
    }

    TEST(PathShiftTest, MISOverDistinctMapsIntegratesOnceAndIncludesInverseSupport)
    {
        // Three deterministic strategies: identity, Warp, and x -> x/2 whose
        // image is only the lower half. All source densities are uniform. No
        // samples are rejected and redrawn; the third strategy has partial image.
        const auto strategiesAt = [](f64 y)
        {
            return std::array<StrategyDensity, 3>{ StrategyDensity{ 2.0, 1.0, 1.0, true },
                                                   StrategyDensity{ 3.0, 1.0, WarpDerivative(Unwarp(y)), true },
                                                   StrategyDensity{ 5.0, 1.0, 0.5, y < 0.5 } };
        };
        for (u32 i = 0; i < 128u; ++i)
        {
            const auto strategies = strategiesAt((static_cast<f64>(i) + 0.5) / 128.0);
            f64 sum = 0.0;
            for (sizet strategy = 0; strategy < strategies.size(); ++strategy)
                sum += ShiftMISWeight(strategies, strategy);
            EXPECT_NEAR(sum, 1.0, 3.0e-16);
        }
        const auto estimate = [&](bool omitJacobian, bool omitMIS, bool wrongDomain)
        {
            f64 result = 0.0;
            for (sizet strategy = 0; strategy < 3u; ++strategy)
            {
                result += Quadrature([&](f64 x)
                                     {
                    const f64 y = strategy == 0u ? x : (strategy == 1u ? Warp(x) : 0.5 * x);
                    const f64 jacobian = strategy == 0u ? 1.0 : (strategy == 1u ? WarpDerivative(x) : 0.5);
                    auto densities = strategiesAt(y);
                    if (wrongDomain)
                        densities[2].InDomain = true;
                    const f64 mis = omitMIS ? 1.0 : ShiftMISWeight(densities, strategy);
                    return ShiftedCandidateWeight(1.0, omitJacobian ? 1.0 : jacobian, Signal(y), mis); });
            }
            return result;
        };
        const f64 truth = Quadrature(Signal);
        // The inverse support boundary is crossed by the warped strategy's
        // quadrature cells; account for that deterministic discretization error.
        EXPECT_NEAR(estimate(false, false, false), truth, 8.0e-5);
        EXPECT_GT(std::abs(estimate(true, false, false) - truth), 0.1);
        EXPECT_GT(std::abs(estimate(false, true, false) - truth), 0.5);
        EXPECT_GT(std::abs(estimate(false, false, true) - truth), 0.1);
    }
} // namespace
