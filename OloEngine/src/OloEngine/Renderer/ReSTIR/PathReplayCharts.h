#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <glm/glm.hpp>

#include <cmath>
#include <optional>

namespace OloEngine::ReSTIR::ReplayCharts
{
    // These are sampler charts, not BRDF lobes to evaluate in isolation. The
    // estimator still evaluates the full closure and its mixture density.
    enum class Chart : u32
    {
        Cosine,
        LegacyGGX,
        VisibleGGX
    };

    struct Frame
    {
        glm::vec3 Normal{ 0.0f, 0.0f, 1.0f };
        glm::vec3 View{ 0.0f, 0.0f, 1.0f };
        f32 Roughness = 0.5f;
    };

    [[nodiscard("Check the sampler chart result before replay")]] inline bool Interior(f32 x) noexcept
    {
        return std::isfinite(x) && x > 0.0f && x < 1.0f;
    }

    [[nodiscard("Check the sampler chart result before replay")]] inline bool UnitVector(const glm::vec3& v) noexcept
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::abs(glm::dot(v, v) - 1.0f) <= 2.0e-5f;
    }

    [[nodiscard("Check the sampler chart result before replay")]] inline bool Valid(const Frame& frame) noexcept
    {
        const bool roughnessValid = std::isfinite(frame.Roughness) &&
                                    frame.Roughness >= 0.0f && frame.Roughness <= 1.0f;
        return UnitVector(frame.Normal) && UnitVector(frame.View) &&
               glm::dot(frame.Normal, frame.View) > 0.0f && roughnessValid;
    }

    [[nodiscard("Check the sampler chart result before replay")]] inline std::optional<glm::vec3> Forward(Chart chart, const Frame& frame,
                                                                                                          const glm::vec2& xi) noexcept
    {
        if (!Valid(frame) || !Interior(xi.x) || !Interior(xi.y))
            return std::nullopt;
        const f32 roughness = PathTracing::ClosureV2Roughness(frame.Roughness);
        glm::vec3 direction;
        switch (chart)
        {
            case Chart::Cosine:
                direction = PathTracing::CosineSampleHemisphere(xi, frame.Normal);
                break;
            case Chart::LegacyGGX:
                direction = glm::reflect(-frame.View, PathTracing::ImportanceSampleGGX(xi, frame.Normal, roughness));
                break;
            case Chart::VisibleGGX:
                direction = glm::reflect(-frame.View, PathTracing::SampleGGXVNDF(frame.Normal, frame.View, roughness, xi));
                break;
            default:
                return std::nullopt;
        }
        if (!UnitVector(direction) || !(glm::dot(frame.Normal, direction) > 0.0f))
            return std::nullopt;
        return direction;
    }

    [[nodiscard("Check the sampler chart result before replay")]] inline f32 Azimuth(f32 x, f32 y) noexcept
    {
        const f32 angle = std::atan2(y, x) / PathTracing::kTwoPi;
        return angle < 0.0f ? angle + 1.0f : angle;
    }

    // Reject boundaries and numerical non-invertibility; never clamp a failed
    // coordinate into the chart. Raw replay uses retained original uniforms,
    // never this inverse: the Legacy float sampler can collapse multiple
    // near-pole uniforms onto one direction. Reconnection uses rough charts.
    // A direction round trip is a numerical check,
    // not evidence that a scene-level path shift has the required inverse.
    [[nodiscard("Check the sampler chart result before replay")]] inline std::optional<glm::vec2> Inverse(Chart chart, const Frame& frame,
                                                                                                          const glm::vec3& direction) noexcept
    {
        if (!Valid(frame) || !UnitVector(direction) || !(glm::dot(frame.Normal, direction) > 0.0f))
            return std::nullopt;
        glm::vec3 tangent, bitangent;
        PathTracing::OrthonormalBasis(frame.Normal, tangent, bitangent);
        glm::vec2 xi;
        if (chart == Chart::Cosine)
        {
            const f32 x = glm::dot(direction, tangent);
            const f32 y = glm::dot(direction, bitangent);
            xi = { x * x + y * y, Azimuth(x, y) };
        }
        else
        {
            const glm::vec3 half = glm::normalize(frame.View + direction);
            const glm::vec3 h(glm::dot(half, tangent), glm::dot(half, bitangent), glm::dot(half, frame.Normal));
            if (!(h.z > 0.0f) || !(glm::dot(frame.View, half) > 0.0f))
                return std::nullopt;
            const f32 roughness = PathTracing::ClosureV2Roughness(frame.Roughness);
            const f32 alpha = roughness * roughness;
            if (chart == Chart::LegacyGGX)
            {
                // sin^2 / (sin^2 + alpha^2 cos^2) avoids subtracting
                // two nearly equal numbers at the near-specular pole.
                const f32 sinSq = h.x * h.x + h.y * h.y;
                xi = { Azimuth(h.x, h.y), sinSq / (sinSq + alpha * alpha * h.z * h.z) };
            }
            else if (chart == Chart::VisibleGGX)
            {
                const glm::vec3 ve(glm::dot(frame.View, tangent), glm::dot(frame.View, bitangent),
                                   glm::dot(frame.View, frame.Normal));
                const glm::vec3 vh = glm::normalize(glm::vec3(alpha * ve.x, alpha * ve.y, ve.z));
                const f32 lenSq = vh.x * vh.x + vh.y * vh.y;
                const glm::vec3 t1 = lenSq > 0.0f ? glm::vec3(-vh.y, vh.x, 0.0f) / std::sqrt(lenSq)
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 t2 = glm::cross(vh, t1);
                const glm::vec3 nh = glm::normalize(glm::vec3(h.x / alpha, h.y / alpha, h.z));
                const f32 p1 = glm::dot(nh, t1);
                const f32 radicand = 1.0f - p1 * p1;
                if (!(radicand > 0.0f))
                    return std::nullopt;
                const f32 s = 0.5f * (1.0f + vh.z);
                const f32 p2 = (glm::dot(nh, t2) - (1.0f - s) * std::sqrt(radicand)) / s;
                xi = { p1 * p1 + p2 * p2, Azimuth(p1, p2) };
            }
            else
                return std::nullopt;
        }
        if (!Interior(xi.x) || !Interior(xi.y))
            return std::nullopt;
        if (const auto reconstructed = Forward(chart, frame, xi);
            !reconstructed || glm::length(*reconstructed - direction) > 2.0e-5f)
            return std::nullopt;
        return xi;
    }

    struct LobeCoordinate
    {
        bool Specular = false;
        f32 Residual = 0.0f;
    };

    [[nodiscard("Check the sampler chart result before replay")]] inline std::optional<LobeCoordinate> DecodeLobe(f32 raw, f32 specularProbability) noexcept
    {
        if (!Interior(raw) || !Interior(specularProbability))
            return std::nullopt;
        const bool specular = raw < specularProbability;
        const f32 residual = specular ? raw / specularProbability
                                      : (raw - specularProbability) / (1.0f - specularProbability);
        if (!Interior(residual))
            return std::nullopt;
        return LobeCoordinate{ specular, residual };
    }

    [[nodiscard("Check the sampler chart result before replay")]] inline std::optional<f32> EncodeLobe(LobeCoordinate coordinate, f32 specularProbability) noexcept
    {
        if (!Interior(coordinate.Residual) || !Interior(specularProbability))
            return std::nullopt;
        const f32 raw = coordinate.Specular ? specularProbability * coordinate.Residual
                                            : specularProbability + (1.0f - specularProbability) * coordinate.Residual;
        if (const auto decoded = DecodeLobe(raw, specularProbability);
            !decoded || decoded->Specular != coordinate.Specular)
            return std::nullopt;
        return raw;
    }

    // Same raw uniform, same branch, changed residual. The caller incorporates
    // q_src/q_dst in the extended chart Jacobian, not the mixture PDF ratio.
    [[nodiscard("Check the sampler chart result before replay")]] inline std::optional<LobeCoordinate> ReplayLobe(f32 raw, f32 sourceProbability,
                                                                                                                  f32 destinationProbability) noexcept
    {
        const auto source = DecodeLobe(raw, sourceProbability);
        if (!source)
            return std::nullopt;
        const auto destination = DecodeLobe(raw, destinationProbability);
        if (!destination || destination->Specular != source->Specular)
            return std::nullopt;
        return destination;
    }
} // namespace OloEngine::ReSTIR::ReplayCharts
