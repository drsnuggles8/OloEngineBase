#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawExpansion.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine::ShaderDebugDrawExpansion
{
    namespace
    {
        // Ring vertex `i` of `count` in the plane (tangent, bitangent) around
        // `center`. Shared by circle / sphere / cone so the three cannot pick
        // different winding or phase.
        [[nodiscard]] glm::vec3 RingPoint(const glm::vec3& center, const glm::vec3& tangent,
                                          const glm::vec3& bitangent, f32 radius, u32 i, u32 count)
        {
            const f32 angle = glm::two_pi<f32>() * (static_cast<f32>(i) / static_cast<f32>(count));
            return center + (tangent * (radius * std::cos(angle))) + (bitangent * (radius * std::sin(angle)));
        }

        void WriteRing(std::span<Segment> out, u32 base, const glm::vec3& center, const glm::vec3& normal,
                       f32 radius, u32 segments)
        {
            glm::vec3 tangent{ 0.0f };
            glm::vec3 bitangent{ 0.0f };
            OrthonormalBasis(normal, tangent, bitangent);
            for (u32 i = 0; i < segments; ++i)
            {
                out[base + i].A = RingPoint(center, tangent, bitangent, radius, i, segments);
                out[base + i].B = RingPoint(center, tangent, bitangent, radius, i + 1, segments);
            }
        }

        void WriteBoxEdges(std::span<Segment> out, const std::array<glm::vec3, 8>& corners)
        {
            const auto edges = BoxEdges();
            for (u32 i = 0; i < 12; ++i)
            {
                out[i].A = corners[edges[i].x];
                out[i].B = corners[edges[i].y];
            }
        }
    } // namespace

    u32 Expand(const ShaderDebugDrawLine& entry, std::span<Segment> out)
    {
        OLO_CORE_ASSERT(out.size() >= 1, "Segment span too small for a Line");
        out[0].A = entry.Start;
        out[0].B = entry.End;
        return 1;
    }

    u32 Expand(const ShaderDebugDrawCircle& entry, std::span<Segment> out)
    {
        constexpr u32 segments = ShaderDebugDrawContract::kCircleSegments;
        OLO_CORE_ASSERT(out.size() >= segments, "Segment span too small for a Circle");
        WriteRing(out, 0, entry.Center, entry.Normal, entry.Radius, segments);
        return segments;
    }

    u32 Expand(const ShaderDebugDrawRectangle& entry, std::span<Segment> out)
    {
        OLO_CORE_ASSERT(out.size() >= 4, "Segment span too small for a Rectangle");
        // Corners in a consistent winding so the four edges close the loop.
        const glm::vec3 c0 = entry.Center - entry.AxisU - entry.AxisV;
        const glm::vec3 c1 = entry.Center + entry.AxisU - entry.AxisV;
        const glm::vec3 c2 = entry.Center + entry.AxisU + entry.AxisV;
        const glm::vec3 c3 = entry.Center - entry.AxisU + entry.AxisV;
        out[0] = { c0, c1 };
        out[1] = { c1, c2 };
        out[2] = { c2, c3 };
        out[3] = { c3, c0 };
        return 4;
    }

    u32 Expand(const ShaderDebugDrawAABB& entry, std::span<Segment> out)
    {
        OLO_CORE_ASSERT(out.size() >= 12, "Segment span too small for an AABB");
        WriteBoxEdges(out, AABBCorners(entry.Min, entry.Max));
        return 12;
    }

    u32 Expand(const ShaderDebugDrawBox& entry, std::span<Segment> out)
    {
        OLO_CORE_ASSERT(out.size() >= 12, "Segment span too small for a Box");
        std::array<glm::vec3, 8> corners{};
        for (u32 i = 0; i < 8; ++i)
            corners[i] = glm::vec3(entry.Corners[i]);
        WriteBoxEdges(out, corners);
        return 12;
    }

    u32 Expand(const ShaderDebugDrawCone& entry, std::span<Segment> out)
    {
        constexpr u32 ringSegments = ShaderDebugDrawContract::kConeRingSegments;
        constexpr u32 sideLines = ShaderDebugDrawContract::kConeSideLines;
        OLO_CORE_ASSERT(out.size() >= ringSegments + sideLines, "Segment span too small for a Cone");

        const glm::vec3 baseCenter = entry.Apex + entry.Axis;
        WriteRing(out, 0, baseCenter, entry.Axis, entry.Radius, ringSegments);

        // Spokes from the apex to evenly spaced points on the base ring. Taken
        // from the SAME ring parameterisation as the ring itself (every
        // ringSegments/sideLines-th ring vertex) so a spoke always lands exactly
        // on a ring vertex rather than a chord midpoint.
        glm::vec3 tangent{ 0.0f };
        glm::vec3 bitangent{ 0.0f };
        OrthonormalBasis(entry.Axis, tangent, bitangent);
        for (u32 i = 0; i < sideLines; ++i)
        {
            const u32 ringIndex = (i * ringSegments) / sideLines;
            out[ringSegments + i].A = entry.Apex;
            out[ringSegments + i].B =
                RingPoint(baseCenter, tangent, bitangent, entry.Radius, ringIndex, ringSegments);
        }
        return ringSegments + sideLines;
    }

    u32 Expand(const ShaderDebugDrawSphere& entry, std::span<Segment> out)
    {
        constexpr u32 ringSegments = ShaderDebugDrawContract::kSphereRingSegments;
        OLO_CORE_ASSERT(out.size() >= 3 * ringSegments, "Segment span too small for a Sphere");
        // Three axis-aligned great circles. Axis order X, Y, Z — the same order
        // the GLSL derives from `ring = segment / kSphereRingSegments`.
        WriteRing(out, 0 * ringSegments, entry.Center, glm::vec3(1.0f, 0.0f, 0.0f), entry.Radius, ringSegments);
        WriteRing(out, 1 * ringSegments, entry.Center, glm::vec3(0.0f, 1.0f, 0.0f), entry.Radius, ringSegments);
        WriteRing(out, 2 * ringSegments, entry.Center, glm::vec3(0.0f, 0.0f, 1.0f), entry.Radius, ringSegments);
        return 3 * ringSegments;
    }
} // namespace OloEngine::ShaderDebugDrawExpansion
