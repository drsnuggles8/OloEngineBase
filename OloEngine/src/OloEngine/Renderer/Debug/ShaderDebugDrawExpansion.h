#pragma once

#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <vector>

// =============================================================================
// CPU mirror of the draw-side primitive -> line-segment expansion (issue #725).
// =============================================================================
//
// `DebugDrawPrimitives.glsl` turns one entry into `SegmentCount(primitive)` line
// segments, then turns each segment into a screen-space quad. The FIRST half of
// that — entry to segment endpoints, in the entry's own coordinate space — is
// pure math with no GL in it, and is duplicated here so it can be tested without
// a context (`ShaderDebugDrawExpansionTest`, L1/shaderpipe).
//
// The duplication is deliberate and is the same bargain
// `Rendering/GPUFrustumCullParityTest.cpp` makes for the cull compute: the GLSL
// cannot be executed headlessly, so the alternative to a mirrored
// implementation is no coverage at all. What keeps the two honest is that the
// test also asserts the shared constants (`ShaderDebugDrawContract::k*`) appear
// verbatim in the GLSL source, so a change to one side that is not made to the
// other fails rather than silently drifting.
//
// The second half (segment -> screen quad) is NOT mirrored: it depends on the
// viewport, the projection and the near-plane clip, and testing it against a
// re-implementation would only pin the re-implementation. It is covered by the
// visual-evidence test instead.
// =============================================================================

namespace OloEngine::ShaderDebugDrawExpansion
{
    struct Segment
    {
        glm::vec3 A{ 0.0f };
        glm::vec3 B{ 0.0f };
    };

    // Build an orthonormal basis (tangent, bitangent) for a plane with the given
    // normal. MUST match `OloDebugBasis` in DebugDrawPrimitives.glsl — a
    // different-but-valid basis rotates every ring by an arbitrary angle, which
    // is invisible on a circle but very visible on a cone's spokes.
    inline void OrthonormalBasis(const glm::vec3& normal, glm::vec3& outTangent, glm::vec3& outBitangent)
    {
        const glm::vec3 n = glm::normalize(normal);
        // Pick the world axis least aligned with n so the cross product is
        // well conditioned. Comparing |n.x| against 0.9 (rather than picking the
        // component-wise minimum) is what the GLSL does; keep them identical.
        const glm::vec3 reference = (glm::abs(n.x) < 0.9f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        outTangent = glm::normalize(glm::cross(reference, n));
        outBitangent = glm::cross(n, outTangent);
    }

    // The eight corners of an AABB, in the bit order the Box primitive uses:
    // bit0 = +X, bit1 = +Y, bit2 = +Z.
    [[nodiscard]] inline std::array<glm::vec3, 8> AABBCorners(const glm::vec3& mn, const glm::vec3& mx)
    {
        std::array<glm::vec3, 8> corners{};
        for (u32 i = 0; i < 8; ++i)
        {
            corners[i] = glm::vec3((i & 1u) ? mx.x : mn.x,
                                   (i & 2u) ? mx.y : mn.y,
                                   (i & 4u) ? mx.z : mn.z);
        }
        return corners;
    }

    // The 12 edges of a corner-indexed box: every pair of corner indices
    // differing in exactly one bit. Enumerated in a fixed order so the CPU and
    // GPU agree on which segment index is which edge.
    [[nodiscard]] inline std::array<glm::uvec2, 12> BoxEdges()
    {
        return { glm::uvec2{ 0, 1 }, glm::uvec2{ 2, 3 }, glm::uvec2{ 4, 5 }, glm::uvec2{ 6, 7 },   // along X (bit0)
                 glm::uvec2{ 0, 2 }, glm::uvec2{ 1, 3 }, glm::uvec2{ 4, 6 }, glm::uvec2{ 5, 7 },   // along Y (bit1)
                 glm::uvec2{ 0, 4 }, glm::uvec2{ 1, 5 }, glm::uvec2{ 2, 6 }, glm::uvec2{ 3, 7 } }; // along Z (bit2)
    }

    // ---- per-primitive expansion ------------------------------------------
    // Each writes exactly `ShaderDebugDrawContract::SegmentCount(type)` segments
    // and returns that count. `out` must be at least that long.

    u32 Expand(const ShaderDebugDrawLine& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawCircle& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawRectangle& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawAABB& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawBox& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawCone& entry, std::span<Segment> out);
    u32 Expand(const ShaderDebugDrawSphere& entry, std::span<Segment> out);

    // Which primitive an entry type belongs to — lets the allocating helper
    // below size its buffer from the contract instead of a magic number.
    template<typename TEntry>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf = ShaderDebugDrawPrimitive::Line;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawLine> = ShaderDebugDrawPrimitive::Line;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawCircle> = ShaderDebugDrawPrimitive::Circle;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawRectangle> = ShaderDebugDrawPrimitive::Rectangle;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawAABB> = ShaderDebugDrawPrimitive::AABB;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawBox> = ShaderDebugDrawPrimitive::Box;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawCone> = ShaderDebugDrawPrimitive::Cone;
    template<>
    inline constexpr ShaderDebugDrawPrimitive kPrimitiveOf<ShaderDebugDrawSphere> = ShaderDebugDrawPrimitive::Sphere;

    // Convenience allocating overload for tests / tools.
    template<typename TEntry>
    [[nodiscard]] std::vector<Segment> ExpandToVector(const TEntry& entry)
    {
        std::vector<Segment> segments(ShaderDebugDrawContract::SegmentCount(kPrimitiveOf<TEntry>));
        const u32 written = Expand(entry, std::span<Segment>(segments));
        segments.resize(written);
        return segments;
    }
} // namespace OloEngine::ShaderDebugDrawExpansion
