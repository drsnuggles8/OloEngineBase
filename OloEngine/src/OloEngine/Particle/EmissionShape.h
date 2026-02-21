#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <variant>
#include <vector>

namespace OloEngine
{
    // Explicit enum for emission shape serialization (decoupled from variant index order)
    enum class EmissionShapeType : i32
    {
        Point = 0,
        Sphere = 1,
        Box = 2,
        Cone = 3,
        Ring = 4,
        Edge = 5,
        Mesh = 6,
    };

    // NOTE: Emit* structs use PascalCase fields intentionally — they are simple PODs mirroring shader/config data.
    struct EmitPoint
    {
    };

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
        f32 Angle = 25.0f; // degrees
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

    struct EmitMesh
    {
        struct Triangle
        {
            glm::vec3 V0, V1, V2;
            glm::vec3 Normal;
        };

        std::vector<Triangle> Triangles;
        std::vector<f32> CumulativeAreas;
        f32 TotalArea = 0.0f;
        i32 PrimitiveType = 0; // For serialization: index into primitive mesh list

        bool IsValid() const
        {
            return !Triangles.empty();
        }

        void Build(const glm::vec3* positions, u32 vertexCount, const u32* indices, u32 indexCount)
        {
            Triangles.clear();
            CumulativeAreas.clear();
            TotalArea = 0.0f;

            if (!positions || vertexCount == 0 || !indices || indexCount < 3)
            {
                return;
            }

            u32 triCount = indexCount / 3;
            Triangles.reserve(triCount);
            CumulativeAreas.reserve(triCount);

            for (u32 i = 0; i < triCount; ++i)
            {
                u32 i0 = indices[i * 3 + 0];
                u32 i1 = indices[i * 3 + 1];
                u32 i2 = indices[i * 3 + 2];

                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                {
                    continue;
                }

                const glm::vec3& v0 = positions[i0];
                const glm::vec3& v1 = positions[i1];
                const glm::vec3& v2 = positions[i2];

                glm::vec3 edge1 = v1 - v0;
                glm::vec3 edge2 = v2 - v0;
                glm::vec3 crossProd = glm::cross(edge1, edge2);
                f32 area = glm::length(crossProd) * 0.5f;

                if (area < 1e-8f)
                {
                    continue;
                }

                TotalArea += area;
                Triangles.push_back({ v0, v1, v2, glm::normalize(crossProd) });
                CumulativeAreas.push_back(TotalArea);
            }
        }
    };

    using EmissionShape = std::variant<EmitPoint, EmitSphere, EmitBox, EmitCone, EmitRing, EmitEdge, EmitMesh>;

    // Convert EmissionShape variant to EmissionShapeType enum (for serialization)
    inline EmissionShapeType GetEmissionShapeType(const EmissionShape& shape)
    {
        return std::visit([](auto&& s) -> EmissionShapeType
                          {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, EmitPoint>)   return EmissionShapeType::Point;
            if constexpr (std::is_same_v<T, EmitSphere>)  return EmissionShapeType::Sphere;
            if constexpr (std::is_same_v<T, EmitBox>)     return EmissionShapeType::Box;
            if constexpr (std::is_same_v<T, EmitCone>)    return EmissionShapeType::Cone;
            if constexpr (std::is_same_v<T, EmitRing>)    return EmissionShapeType::Ring;
            if constexpr (std::is_same_v<T, EmitEdge>)    return EmissionShapeType::Edge;
            if constexpr (std::is_same_v<T, EmitMesh>)    return EmissionShapeType::Mesh; }, shape);
    }

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
                f32 angleRad = std::clamp(glm::radians(s.Angle), 0.0f, glm::half_pi<f32>() - 0.001f);
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                f32 u = rng.GetFloat32InRange(0.0f, 1.0f);
                f32 r = std::sqrt(u) * s.Radius; // sqrt for uniform disk distribution
                f32 tanAngle = std::tan(angleRad);
                return {
                    r * std::cos(theta),
                    r * tanAngle,
                    r * std::sin(theta)
                };
            }
            else if constexpr (std::is_same_v<T, EmitRing>)
            {
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                // Area-uniform sampling: sqrt maps linear u to uniform area distribution
                f32 u = rng.GetFloat32InRange(0.0f, 1.0f);
                f32 inner2 = s.InnerRadius * s.InnerRadius;
                f32 outer2 = s.OuterRadius * s.OuterRadius;
                f32 r = std::sqrt(u * (outer2 - inner2) + inner2);
                return { r * std::cos(theta), r * std::sin(theta), 0.0f };
            }
            else if constexpr (std::is_same_v<T, EmitEdge>)
            {
                f32 half = s.Length * 0.5f;
                return { rng.GetFloat32InRange(-half, half), 0.0f, 0.0f };
            }
            else if constexpr (std::is_same_v<T, EmitMesh>)
            {
                if (!s.IsValid())
                    return glm::vec3(0.0f);

                // Weighted random triangle selection via CDF
                f32 r = rng.GetFloat32InRange(0.0f, s.TotalArea);
                auto it = std::lower_bound(s.CumulativeAreas.begin(), s.CumulativeAreas.end(), r);
                u32 triIdx = static_cast<u32>(std::distance(s.CumulativeAreas.begin(), it));
                if (triIdx >= static_cast<u32>(s.Triangles.size()))
                    triIdx = static_cast<u32>(s.Triangles.size()) - 1;

                const auto& tri = s.Triangles[triIdx];

                // Uniform random point on triangle via barycentric coordinates
                f32 r1 = rng.GetFloat32InRange(0.0f, 1.0f);
                f32 r2 = rng.GetFloat32InRange(0.0f, 1.0f);
                f32 sqrtR1 = std::sqrt(r1);
                return (1.0f - sqrtR1) * tri.V0 + sqrtR1 * (1.0f - r2) * tri.V1 + sqrtR1 * r2 * tri.V2;
            }
            else
            {
                return glm::vec3(0.0f);
            } }, shape);
    }

    // Helper: generate a random direction uniformly distributed on the unit sphere
    template<typename Algorithm>
    inline glm::vec3 RandomUnitDirection(FastRandom<Algorithm>& rng)
    {
        glm::vec3 dir;
        f32 lenSq;
        do
        {
            dir = { rng.GetFloat32InRange(-1.0f, 1.0f), rng.GetFloat32InRange(-1.0f, 1.0f), rng.GetFloat32InRange(-1.0f, 1.0f) };
            lenSq = glm::dot(dir, dir);
        } while (lenSq < 0.0001f || lenSq > 1.0f);
        return glm::normalize(dir);
    }

    // Get a direction from the emission shape for velocity initialization
    inline glm::vec3 SampleEmissionDirection(const EmissionShape& shape)
    {
        auto& rng = RandomUtils::GetGlobalRandom();

        return std::visit([&](auto&& s) -> glm::vec3
                          {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, EmitPoint>)
            {
                // Random direction on unit sphere (uniform)
                return RandomUnitDirection(rng);
            }
            else if constexpr (std::is_same_v<T, EmitSphere>)
            {
                // Random direction on unit sphere (uniform)
                return RandomUnitDirection(rng);
            }
            else if constexpr (std::is_same_v<T, EmitBox>)
            {
                // Random direction on unit sphere
                return RandomUnitDirection(rng);
            }
            else if constexpr (std::is_same_v<T, EmitCone>)
            {
                f32 angleRad = std::clamp(glm::radians(s.Angle), 0.0f, glm::half_pi<f32>() - 0.001f);
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                // Uniform cone distribution: sample cos(phi) uniformly in [cos(angleRad), 1]
                f32 cosPhiMin = std::cos(angleRad);
                f32 cosPhi = rng.GetFloat32InRange(cosPhiMin, 1.0f);
                f32 sinPhi = std::sqrt(1.0f - cosPhi * cosPhi);
                return glm::normalize(glm::vec3(
                    sinPhi * std::cos(theta),
                    cosPhi,
                    sinPhi * std::sin(theta)
                ));
            }
            else if constexpr (std::is_same_v<T, EmitRing>)
            {
                // Outward from center in XY plane (matching SampleEmissionShape)
                f32 theta = rng.GetFloat32InRange(0.0f, glm::two_pi<f32>());
                return glm::vec3(std::cos(theta), std::sin(theta), 0.0f);
            }
            else if constexpr (std::is_same_v<T, EmitEdge>)
            {
                // Emit upward (+Y) from edge
                return glm::vec3(0.0f, 1.0f, 0.0f);
            }
            else if constexpr (std::is_same_v<T, EmitMesh>)
            {
                if (!s.IsValid())
                    return glm::vec3(0.0f, 1.0f, 0.0f);

                // Pick a random triangle weighted by area and use its face normal
                f32 r = rng.GetFloat32InRange(0.0f, s.TotalArea);
                auto it = std::lower_bound(s.CumulativeAreas.begin(), s.CumulativeAreas.end(), r);
                u32 triIdx = static_cast<u32>(std::distance(s.CumulativeAreas.begin(), it));
                if (triIdx >= static_cast<u32>(s.Triangles.size()))
                    triIdx = static_cast<u32>(s.Triangles.size()) - 1;

                return s.Triangles[triIdx].Normal;
            }
            else
            {
                return glm::vec3(0.0f, 1.0f, 0.0f);
            } }, shape);
    }

    // Combined position + direction sampler for mesh shapes.
    // Guarantees that both are sampled from the same triangle (avoids position/direction mismatch).
    struct EmissionSample
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Direction{ 0.0f, 1.0f, 0.0f };
    };

    inline EmissionSample SampleEmissionCombined(const EmissionShape& shape)
    {
        EmissionSample sample;

        // For mesh shapes, sample position and direction from the same triangle.
        // Skip the generic SampleEmissionShape call to avoid wasteful double-sampling.
        if (auto* mesh = std::get_if<EmitMesh>(&shape); mesh && mesh->IsValid())
        {
            auto& rng = RandomUtils::GetGlobalRandom();
            f32 r = rng.GetFloat32InRange(0.0f, mesh->TotalArea);
            auto it = std::lower_bound(mesh->CumulativeAreas.begin(), mesh->CumulativeAreas.end(), r);
            u32 triIdx = static_cast<u32>(std::distance(mesh->CumulativeAreas.begin(), it));
            if (triIdx >= static_cast<u32>(mesh->Triangles.size()))
                triIdx = static_cast<u32>(mesh->Triangles.size()) - 1;

            const auto& tri = mesh->Triangles[triIdx];

            // Uniform random point on triangle via barycentric coordinates
            f32 r1 = rng.GetFloat32InRange(0.0f, 1.0f);
            f32 r2 = rng.GetFloat32InRange(0.0f, 1.0f);
            f32 sqrtR1 = std::sqrt(r1);
            sample.Position = (1.0f - sqrtR1) * tri.V0 + sqrtR1 * (1.0f - r2) * tri.V1 + sqrtR1 * r2 * tri.V2;
            sample.Direction = tri.Normal;
        }
        else
        {
            sample.Position = SampleEmissionShape(shape);
            sample.Direction = SampleEmissionDirection(shape);
        }

        return sample;
    }
} // namespace OloEngine
