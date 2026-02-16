#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <variant>

namespace OloEngine
{
    struct EmitPoint {};

    struct EmitSphere
    {
        f32 Radius = 1.0f;
    };

    struct EmitBox
    {
        glm::vec3 HalfExtents{ 0.5f, 0.5f, 0.5f };
    };

    struct EmitCone
    {
        f32 Angle = 25.0f;  // degrees
        f32 Radius = 0.5f;
    };

    struct EmitRing
    {
        f32 InnerRadius = 0.3f;
        f32 OuterRadius = 1.0f;
    };

    struct EmitEdge
    {
        f32 Length = 1.0f;
    };

    using EmissionShape = std::variant<EmitPoint, EmitSphere, EmitBox, EmitCone, EmitRing, EmitEdge>;

    // Sample a position offset from the emission shape
    inline glm::vec3 SampleEmissionShape(const EmissionShape& shape)
    {
        auto& rng = RandomUtils::GetGlobalRandom();

        return std::visit([&](auto&& s) -> glm::vec3
        {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, EmitPoint>)
            {
                return glm::vec3(0.0f);
            }
            else if constexpr (std::is_same_v<T, EmitSphere>)
            {
                // Uniform point in sphere via rejection sampling
                glm::vec3 p;
                do
                {
                    p = { rng.GetFloat32InRange(-1.0f, 1.0f), rng.GetFloat32InRange(-1.0f, 1.0f), rng.GetFloat32InRange(-1.0f, 1.0f) };
                } while (glm::dot(p, p) > 1.0f);
                return p * s.Radius;
            }
            else if constexpr (std::is_same_v<T, EmitBox>)
            {
                return {
                    rng.GetFloat32InRange(-s.HalfExtents.x, s.HalfExtents.x),
                    rng.GetFloat32InRange(-s.HalfExtents.y, s.HalfExtents.y),
                    rng.GetFloat32InRange(-s.HalfExtents.z, s.HalfExtents.z)
                };
            }
            else if constexpr (std::is_same_v<T, EmitCone>)
            {
                f32 angleRad = glm::radians(s.Angle);
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                f32 r = rng.GetFloat32InRange(0.0f, s.Radius);
                return {
                    r * std::cos(theta),
                    r * std::sin(theta),
                    r * std::tan(angleRad)
                };
            }
            else if constexpr (std::is_same_v<T, EmitRing>)
            {
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                f32 r = rng.GetFloat32InRange(s.InnerRadius, s.OuterRadius);
                return { r * std::cos(theta), r * std::sin(theta), 0.0f };
            }
            else if constexpr (std::is_same_v<T, EmitEdge>)
            {
                f32 half = s.Length * 0.5f;
                return { rng.GetFloat32InRange(-half, half), 0.0f, 0.0f };
            }
            else
            {
                return glm::vec3(0.0f);
            }
        }, shape);
    }

    // Get a direction from the emission shape for velocity initialization
    inline glm::vec3 SampleEmissionDirection(const EmissionShape& shape)
    {
        auto& rng = RandomUtils::GetGlobalRandom();

        return std::visit([&](auto&& s) -> glm::vec3
        {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, EmitCone>)
            {
                f32 angleRad = glm::radians(s.Angle);
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                f32 phi = rng.GetFloat32InRange(0.0f, angleRad);
                return glm::normalize(glm::vec3(
                    std::sin(phi) * std::cos(theta),
                    std::sin(phi) * std::sin(theta),
                    std::cos(phi)
                ));
            }
            else
            {
                // Default: emit upward (+Y)
                return glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }, shape);
    }
}
