#pragma once

// Path-suffix shift arithmetic in the extended solid-angle/chart measure of
// docs/design/restir-pt-shift-mappings.md sections 2-7. NEE terminal points use
// area measure (or counting measure for punctual endpoints) explicitly.
// Geometry, visibility, chart inversion and scene epochs establish the domain
// BEFORE these factors are admitted. This module alone is not a path mapper.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirCore.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

namespace OloEngine::ReSTIR::PT
{
    inline constexpr f64 kMaximumLogCondition = 2.079441541679835928; // log(8)
    inline constexpr f32 kMinimumConnectionDistance = 0.05f;

    enum class ShiftMapping : u32
    {
        Reconnection,
        RandomReplay,
        Hybrid
    };

    enum class ShiftRejection : u32
    {
        None,
        NonFinite,
        UnsupportedChart,
        Hemisphere,
        Distance,
        Occluded,
        InverseMismatch,
        Conditioning
    };

    // Accumulating absolute log factors prevents cancellation from hiding an
    // ill-conditioned intermediate map. A rejected product stays rejected.
    // J_reverse uses negated logs, so it has the same conditioning domain.
    class ShiftJacobianProduct
    {
      public:
        [[nodiscard("Use the shift result and reject invalid reuse")]] bool Append(f64 factor)
        {
            if (m_Rejection != ShiftRejection::None)
                return false;
            if (!std::isfinite(factor) || !(factor > 0.0))
            {
                m_Rejection = ShiftRejection::NonFinite;
                return false;
            }
            const f64 logFactor = std::log(factor);
            m_LogJacobian += logFactor;
            m_AbsoluteLogSum += std::abs(logFactor);
            if (m_AbsoluteLogSum > kMaximumLogCondition)
            {
                m_Rejection = ShiftRejection::Conditioning;
                return false;
            }
            return true;
        }

        [[nodiscard("Use the shift result and reject invalid reuse")]] bool AppendReplay(f64 sourceChartDensity, f64 destinationChartDensity)
        {
            if (!std::isfinite(sourceChartDensity) || !std::isfinite(destinationChartDensity) ||
                !(sourceChartDensity > 0.0) || !(destinationChartDensity > 0.0))
            {
                if (m_Rejection == ShiftRejection::None)
                    m_Rejection = ShiftRejection::NonFinite;
                return false;
            }
            // du = c_source dOmega_source dr_source = c_dest dOmega_dest dr_dest.
            // The c values are q_l * p_l, NOT the full mixture PDF.
            return Append(sourceChartDensity / destinationChartDensity);
        }

        [[nodiscard("Use the shift result and reject invalid reuse")]] f64 Value() const
        {
            return m_Rejection == ShiftRejection::None ? std::exp(m_LogJacobian) : 0.0;
        }
        [[nodiscard("Use the shift result and reject invalid reuse")]] f64 LogValue() const noexcept
        {
            return m_LogJacobian;
        }
        [[nodiscard("Use the shift result and reject invalid reuse")]] f64 AbsoluteLogSum() const noexcept
        {
            return m_AbsoluteLogSum;
        }
        [[nodiscard("Use the shift result and reject invalid reuse")]] ShiftRejection Rejection() const noexcept
        {
            return m_Rejection;
        }

      private:
        f64 m_LogJacobian = 0.0;
        f64 m_AbsoluteLogSum = 0.0;
        ShiftRejection m_Rejection = ShiftRejection::None;
    };

    struct ConnectionEndpoint
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };

    [[nodiscard("Use the shift result and reject invalid reuse")]] inline bool FiniteVector(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // Check BOTH receiver/held-vertex hemispheres, at BOTH source and
    // destination. The core Jacobian's absolute cosine cannot do this job.
    // Visibility is supplied from traced segments, never inferred from geometry.
    [[nodiscard("Use the shift result and reject invalid reuse")]] inline ShiftRejection ConnectionDomain(const ConnectionEndpoint& source,
                                                                                                          const ConnectionEndpoint& destination,
                                                                                                          const ConnectionEndpoint& held,
                                                                                                          bool sourceVisible, bool destinationVisible)
    {
        for (const auto* endpoint : { &source, &destination, &held })
        {
            if (!FiniteVector(endpoint->Position) || !FiniteVector(endpoint->Normal) ||
                !(glm::dot(endpoint->Normal, endpoint->Normal) > 0.0f))
                return ShiftRejection::NonFinite;
        }
        for (const auto* receiver : { &source, &destination })
        {
            const glm::vec3 segment = held.Position - receiver->Position;
            const f32 distanceSq = glm::dot(segment, segment);
            if (!std::isfinite(distanceSq))
                return ShiftRejection::NonFinite;
            if (!(distanceSq > kMinimumConnectionDistance * kMinimumConnectionDistance))
                return ShiftRejection::Distance;
            const glm::vec3 direction = segment / std::sqrt(distanceSq);
            if (!(glm::dot(glm::normalize(receiver->Normal), direction) > kMinimumShiftCosine) ||
                !(glm::dot(glm::normalize(held.Normal), -direction) > kMinimumShiftCosine))
                return ShiftRejection::Hemisphere;
        }
        return sourceVisible && destinationVisible ? ShiftRejection::None : ShiftRejection::Occluded;
    }

    // One entry per (source, mapping), evaluated at the SAME destination y.
    // InverseTarget and ForwardJacobian must come from that strategy's inverse
    // image of y, never from the unrelated survivor it originally proposed.
    struct StrategyDensity
    {
        f64 Confidence = 0.0;
        f64 InverseTarget = 0.0;
        f64 ForwardJacobian = 1.0;
        bool InDomain = false;
    };

    [[nodiscard("Use the shift result and reject invalid reuse")]] inline f64 LogStrategyDensity(const StrategyDensity& strategy)
    {
        return std::log(strategy.Confidence) + std::log(strategy.InverseTarget) -
               std::log(strategy.ForwardJacobian);
    }

    [[nodiscard("Invalid densities must not enter shift arithmetic")]] inline bool FinitePositive(f64 value)
    {
        return std::isfinite(value) && value > 0.0;
    }

    [[nodiscard("Use the shift result and reject invalid reuse")]] inline bool HasStrategySupport(const StrategyDensity& strategy)
    {
        return strategy.InDomain && FinitePositive(strategy.Confidence) &&
               FinitePositive(strategy.InverseTarget) && FinitePositive(strategy.ForwardJacobian);
    }

    // Generalized balance in a common destination measure, section 7.
    // Log-sum-exp avoids overflow in M * pHat / J even for finite inputs.
    [[nodiscard("Use the shift result and reject invalid reuse")]] inline f64 ShiftMISWeight(std::span<const StrategyDensity> strategies, sizet selected)
    {
        if (selected >= strategies.size() || !HasStrategySupport(strategies[selected]))
            return 0.0;
        f64 maximumLog = LogStrategyDensity(strategies[selected]);
        for (const auto& strategy : strategies)
        {
            if (HasStrategySupport(strategy))
                maximumLog = std::max(maximumLog, LogStrategyDensity(strategy));
        }
        f64 denominator = 0.0;
        for (const auto& strategy : strategies)
        {
            if (HasStrategySupport(strategy))
                denominator += std::exp(LogStrategyDensity(strategy) - maximumLog);
        }
        return std::exp(LogStrategyDensity(strategies[selected]) - maximumLog) / denominator;
    }

    // W behaves as reciprocal density, so p_dst=p_src/J gives W_dst=W_src*J.
    // m already includes confidence. Final combined W is sum(w)/pHat, with no
    // additional M factor or strategy-count division (initial RIS differs).
    [[nodiscard("Use the shift result and reject invalid reuse")]] inline f64 ShiftedCandidateWeight(f64 sourceWeight, f64 jacobian,
                                                                                                     f64 destinationTarget, f64 misWeight)
    {
        const bool validMIS = FinitePositive(misWeight) && misWeight <= 1.0;
        const bool validTransport = FinitePositive(sourceWeight) && FinitePositive(jacobian) &&
                                    FinitePositive(destinationTarget);
        if (!validMIS || !validTransport)
            return 0.0;
        // Small later factors can bring an overflowing intermediate product
        // back into range. Evaluate the whole product before testing its range.
        const f64 weight = std::exp(std::log(sourceWeight) + std::log(jacobian) +
                                    std::log(destinationTarget) + std::log(misWeight));
        return std::isfinite(weight) ? weight : 0.0;
    }
} // namespace OloEngine::ReSTIR::PT
