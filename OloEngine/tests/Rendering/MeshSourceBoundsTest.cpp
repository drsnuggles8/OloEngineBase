#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Containers/Array.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// MeshSource bounds contract
//
// Regression suite for a long-standing bug: every MeshSource constructor
// pre-allocates `m_BoneInfluences` to the vertex count so callers can later
// SetVertexBoneData(index, ...). The old `HasBoneInfluences()` read the array
// size and reported `true` for every mesh, even static primitives. The 200%
// expansion in CalculateBounds (intended to absorb skeletal limb extension
// past the rest-pose silhouette) then fired for every static mesh, inflating
// a unit cube's local AABB from 1×1×1 to 5×5×5 and breaking frustum culling,
// occlusion culling, navmesh footprints, and save-game bounds across the
// engine. Pinning the contract here so the bug can't come back unnoticed.
// =============================================================================

namespace
{
    // A unit cube identical in extent to MeshPrimitives::CreateCube(): all
    // vertices at ±0.5 on every axis. We construct manually because using
    // MeshPrimitives directly drags in MeshSource::Build() which creates
    // GPU resources, and these tests run without a GL context.
    Ref<MeshSource> MakeUnitCubeSource()
    {
        TArray<Vertex> vertices;
        // 8 cube corners, normal/UV irrelevant for the bounds path.
        const glm::vec3 corners[8] = {
            { -0.5f, -0.5f, -0.5f },
            { 0.5f, -0.5f, -0.5f },
            { -0.5f, 0.5f, -0.5f },
            { 0.5f, 0.5f, -0.5f },
            { -0.5f, -0.5f, 0.5f },
            { 0.5f, -0.5f, 0.5f },
            { -0.5f, 0.5f, 0.5f },
            { 0.5f, 0.5f, 0.5f },
        };
        for (const auto& c : corners)
            vertices.Add(Vertex(c, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f)));

        TArray<u32> indices;
        for (u32 i = 0; i < 8; ++i)
            indices.Add(i);

        return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    }
} // namespace

TEST(MeshSourceBounds, StaticMeshReportsNoBoneInfluences)
{
    // Constructor pre-allocates m_BoneInfluences to vertices.Num() with
    // every weight defaulted to 0. HasBoneInfluences() must look past the
    // allocation and report based on real weight data — otherwise every
    // static mesh ends up on the "this is a skinned mesh, expand the AABB"
    // path.
    auto source = MakeUnitCubeSource();
    ASSERT_TRUE(source);
    EXPECT_FALSE(source->HasBoneInfluences())
        << "Static mesh should not report HasBoneInfluences() == true. "
           "The pre-allocated zero-weight storage is an internal detail.";
}

TEST(MeshSourceBounds, StaticMeshLocalAABBIsTightAroundVertices)
{
    // The core regression: a unit-cube's local AABB must be exactly the
    // vertex extents, not the 5×5×5 box that fell out of the spurious
    // skeletal expansion.
    auto source = MakeUnitCubeSource();
    ASSERT_TRUE(source);

    const auto& box = source->GetBoundingBox();
    EXPECT_FLOAT_EQ(box.Min.x, -0.5f);
    EXPECT_FLOAT_EQ(box.Min.y, -0.5f);
    EXPECT_FLOAT_EQ(box.Min.z, -0.5f);
    EXPECT_FLOAT_EQ(box.Max.x, 0.5f);
    EXPECT_FLOAT_EQ(box.Max.y, 0.5f);
    EXPECT_FLOAT_EQ(box.Max.z, 0.5f);

    const glm::vec3 size = box.GetSize();
    EXPECT_FLOAT_EQ(size.x, 1.0f);
    EXPECT_FLOAT_EQ(size.y, 1.0f);
    EXPECT_FLOAT_EQ(size.z, 1.0f);
}

TEST(MeshSourceBounds, RiggedMeshExpandsAABBAfterRealBoneDataSet)
{
    // After actual non-zero bone weights are written, the mesh genuinely
    // is skinnable and the expansion should kick in. We trigger a
    // re-calculation by adding a submesh (which calls CalculateBounds()
    // internally) — that's the same code path used during asset import
    // when bone weights are populated before Build().
    auto source = MakeUnitCubeSource();
    ASSERT_TRUE(source);

    // Pre-condition: no expansion, AABB matches vertex extents.
    EXPECT_FALSE(source->HasBoneInfluences());

    BoneInfluence influence;
    influence.SetBoneData(0, /*boneId=*/0, /*weight=*/1.0f);
    source->SetVertexBoneData(0, influence);
    EXPECT_TRUE(source->HasBoneInfluences())
        << "HasBoneInfluences() should report true once any vertex has a "
           "non-zero weight set.";

    // Trigger re-bounds through the same public path import uses.
    Submesh sm;
    sm.m_BaseVertex = 0;
    sm.m_BaseIndex = 0;
    sm.m_IndexCount = 8;
    sm.m_VertexCount = 8;
    sm.m_MaterialIndex = 0;
    sm.m_IsRigged = true;
    source->AddSubmesh(sm);

    const auto& box = source->GetBoundingBox();
    const glm::vec3 size = box.GetSize();
    EXPECT_GT(size.x, 1.0f) << "Skinned-mesh expansion must enlarge the AABB.";
    EXPECT_GT(size.y, 1.0f);
    EXPECT_GT(size.z, 1.0f);
}

TEST(MeshSourceBounds, ZeroWeightSetVertexBoneDataDoesNotFlagAsSkinned)
{
    // Defensive check: if someone calls SetVertexBoneData with a weight of
    // 0 (e.g. clearing a vertex), HasBoneInfluences() must still report
    // false. Otherwise the regression sneaks back in via "we touched the
    // storage, so it must be real data" reasoning.
    auto source = MakeUnitCubeSource();
    ASSERT_TRUE(source);

    BoneInfluence emptyInfluence; // default-constructed: weights all 0
    source->SetVertexBoneData(0, emptyInfluence);

    EXPECT_FALSE(source->HasBoneInfluences())
        << "SetVertexBoneData with a zero-weight payload should be "
           "indistinguishable from leaving the storage at its default.";
}
