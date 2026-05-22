#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Physics3D/JoltShapes.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace OloEngine;

namespace
{
    // Build a 1x1x1 axis-aligned box centered on a chosen origin point.
    // Triangles wound CCW when viewed from outside (standard outward normals).
    void BuildUnitCube(const glm::vec3& center, std::vector<Vertex>& vertices, std::vector<u32>& indices)
    {
        const glm::vec3 h(0.5f);
        const glm::vec3 c = center;
        const std::array<glm::vec3, 8> corners = {
            c + glm::vec3(-h.x, -h.y, -h.z), // 0
            c + glm::vec3(+h.x, -h.y, -h.z), // 1
            c + glm::vec3(+h.x, +h.y, -h.z), // 2
            c + glm::vec3(-h.x, +h.y, -h.z), // 3
            c + glm::vec3(-h.x, -h.y, +h.z), // 4
            c + glm::vec3(+h.x, -h.y, +h.z), // 5
            c + glm::vec3(+h.x, +h.y, +h.z), // 6
            c + glm::vec3(-h.x, +h.y, +h.z), // 7
        };
        vertices.clear();
        vertices.reserve(8);
        for (const auto& p : corners)
        {
            vertices.push_back(Vertex{ p, glm::vec3(0.0f), glm::vec2(0.0f) });
        }
        indices = {
            // -Z face (normal -Z, CCW from -Z view): 0,3,2 and 0,2,1
            0, 3, 2, 0, 2, 1,
            // +Z face (CCW from +Z view): 4,5,6 and 4,6,7
            4, 5, 6, 4, 6, 7,
            // -Y face: 0,1,5 and 0,5,4
            0, 1, 5, 0, 5, 4,
            // +Y face: 3,7,6 and 3,6,2
            3, 7, 6, 3, 6, 2,
            // -X face: 0,4,7 and 0,7,3
            0, 4, 7, 0, 7, 3,
            // +X face: 1,2,6 and 1,6,5
            1, 2, 6, 1, 6, 5,
        };
    }

    // Approximate a unit sphere by a UV mesh; volume should approach (4/3)*pi.
    void BuildUVSphere(f32 radius, u32 latitudeBands, u32 longitudeBands,
                       std::vector<Vertex>& vertices, std::vector<u32>& indices)
    {
        vertices.clear();
        indices.clear();
        const f32 pi = glm::pi<f32>();
        for (u32 lat = 0; lat <= latitudeBands; ++lat)
        {
            const f32 theta = (static_cast<f32>(lat) / latitudeBands) * pi;
            const f32 sinTheta = std::sin(theta);
            const f32 cosTheta = std::cos(theta);
            for (u32 lon = 0; lon <= longitudeBands; ++lon)
            {
                const f32 phi = (static_cast<f32>(lon) / longitudeBands) * 2.0f * pi;
                const glm::vec3 p{
                    radius * sinTheta * std::cos(phi),
                    radius * cosTheta,
                    radius * sinTheta * std::sin(phi)
                };
                vertices.push_back(Vertex{ p, glm::normalize(p), glm::vec2(0.0f) });
            }
        }
        for (u32 lat = 0; lat < latitudeBands; ++lat)
        {
            for (u32 lon = 0; lon < longitudeBands; ++lon)
            {
                const u32 first = lat * (longitudeBands + 1) + lon;
                const u32 second = first + longitudeBands + 1;
                indices.insert(indices.end(), { first, second, first + 1 });
                indices.insert(indices.end(), { second, second + 1, first + 1 });
            }
        }
    }
} // namespace

// =============================================================================
// Unit cube — exact analytic result expected.
// =============================================================================

TEST(JoltShapesMeshMassProperties, UnitCubeAtOriginHasUnitVolumeAndZeroCentroid)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(glm::vec3(0.0f), v, i);

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i);

    EXPECT_TRUE(mass.IsValid);
    EXPECT_NEAR(mass.Volume, 1.0f, 1e-5f);
    EXPECT_NEAR(mass.Centroid.x, 0.0f, 1e-5f);
    EXPECT_NEAR(mass.Centroid.y, 0.0f, 1e-5f);
    EXPECT_NEAR(mass.Centroid.z, 0.0f, 1e-5f);
}

// Translation should not change volume; centroid should follow the translation.
// This validates that the signed-tetrahedron sum is genuinely translation-invariant
// for a closed mesh — the property the divergence theorem promises us.
TEST(JoltShapesMeshMassProperties, UnitCubeAtOffsetTracksCentroid)
{
    const glm::vec3 offset(7.5f, -3.25f, 11.0f);
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(offset, v, i);

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i);

    EXPECT_TRUE(mass.IsValid);
    EXPECT_NEAR(mass.Volume, 1.0f, 1e-4f);
    EXPECT_NEAR(mass.Centroid.x, offset.x, 1e-3f);
    EXPECT_NEAR(mass.Centroid.y, offset.y, 1e-3f);
    EXPECT_NEAR(mass.Centroid.z, offset.z, 1e-3f);
}

// Non-uniform scale: volume scales by sx*sy*sz, centroid scales component-wise.
TEST(JoltShapesMeshMassProperties, NonUniformScaleAppliesToVolumeAndCentroid)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(glm::vec3(0.5f, 0.0f, 0.0f), v, i);

    const glm::vec3 scale(2.0f, 3.0f, 0.5f);
    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i, scale);

    EXPECT_TRUE(mass.IsValid);
    EXPECT_NEAR(mass.Volume, 1.0f * scale.x * scale.y * scale.z, 1e-4f);
    EXPECT_NEAR(mass.Centroid.x, 0.5f * scale.x, 1e-4f);
    EXPECT_NEAR(mass.Centroid.y, 0.0f * scale.y, 1e-4f);
    EXPECT_NEAR(mass.Centroid.z, 0.0f * scale.z, 1e-4f);
}

// Negative scale mirrors the geometry but volume must remain positive.
TEST(JoltShapesMeshMassProperties, NegativeScaleProducesPositiveVolume)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(glm::vec3(0.0f), v, i);

    const glm::vec3 scale(-2.0f, 1.0f, 1.0f); // mirror across X
    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i, scale);

    EXPECT_TRUE(mass.IsValid);
    EXPECT_GT(mass.Volume, 0.0f);
    EXPECT_NEAR(mass.Volume, 2.0f, 1e-4f);
}

// =============================================================================
// UV sphere — converges to (4/3)*pi*r^3 as tessellation increases.
// =============================================================================

TEST(JoltShapesMeshMassProperties, UVSphereApproachesAnalyticVolume)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUVSphere(1.0f, 32, 32, v, i);

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i);

    const f32 analyticVolume = (4.0f / 3.0f) * glm::pi<f32>(); // r = 1
    EXPECT_TRUE(mass.IsValid);
    // 32x32 UV sphere is within ~1% of the analytic volume; leave headroom.
    EXPECT_NEAR(mass.Volume, analyticVolume, 0.05f);

    // Centroid should be at the origin by symmetry.
    EXPECT_NEAR(mass.Centroid.x, 0.0f, 1e-3f);
    EXPECT_NEAR(mass.Centroid.y, 0.0f, 1e-3f);
    EXPECT_NEAR(mass.Centroid.z, 0.0f, 1e-3f);
}

// =============================================================================
// Degenerate inputs — must not crash, must report IsValid=false.
// =============================================================================

TEST(JoltShapesMeshMassProperties, EmptyMeshReportsInvalid)
{
    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties({}, {});
    EXPECT_FALSE(mass.IsValid);
    EXPECT_FLOAT_EQ(mass.Volume, 0.0f);
}

TEST(JoltShapesMeshMassProperties, TrianglePastIndexRangeIsSkipped)
{
    // Single valid degenerate "mesh": a triangle that references vertex 99 (out of range).
    // The function should skip it and report invalid (no usable triangles).
    std::vector<Vertex> verts{
        Vertex{ glm::vec3(0.0f), glm::vec3(0.0f), glm::vec2(0.0f) },
        Vertex{ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f) },
        Vertex{ glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec2(0.0f) },
    };
    std::vector<u32> idx{ 0, 1, 99 };

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(verts, idx);
    EXPECT_FALSE(mass.IsValid);
}

// A flat (zero-volume) mesh — two triangles forming a quad in the XY plane.
// Sum of signed tetrahedra is zero; the helper should report invalid so callers
// fall back to AABB rather than feeding "0 volume" into mass-weighting.
TEST(JoltShapesMeshMassProperties, FlatQuadReportsInvalid)
{
    std::vector<Vertex> verts{
        Vertex{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f) },
        Vertex{ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f) },
        Vertex{ glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f) },
        Vertex{ glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f) },
    };
    std::vector<u32> idx{ 0, 1, 2, 0, 2, 3 };

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(verts, idx);
    EXPECT_FALSE(mass.IsValid);
}

// Indices length not a multiple of 3 — trailing partial triangle ignored,
// the valid prefix should still be processed.
TEST(JoltShapesMeshMassProperties, TrailingPartialTriangleIsIgnored)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(glm::vec3(0.0f), v, i);

    // Append one stray index — function should round down to full triangles only.
    i.push_back(0);

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i);
    EXPECT_TRUE(mass.IsValid);
    EXPECT_NEAR(mass.Volume, 1.0f, 1e-5f);
}

// =============================================================================
// Reversed winding — surface wound inwards still produces positive volume.
// =============================================================================

TEST(JoltShapesMeshMassProperties, ReversedWindingProducesPositiveVolume)
{
    std::vector<Vertex> v;
    std::vector<u32> i;
    BuildUnitCube(glm::vec3(0.0f), v, i);

    // Reverse each triangle's winding so all normals point inwards.
    for (sizet t = 0; t + 2 < i.size(); t += 3)
    {
        std::swap(i[t + 1], i[t + 2]);
    }

    const MeshMassProperties mass = JoltShapes::ComputeTriangleMeshMassProperties(v, i);
    EXPECT_TRUE(mass.IsValid);
    EXPECT_NEAR(mass.Volume, 1.0f, 1e-5f);
}
