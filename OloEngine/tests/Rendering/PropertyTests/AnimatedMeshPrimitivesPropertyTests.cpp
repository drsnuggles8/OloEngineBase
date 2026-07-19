// OLO_TEST_LAYER: L1
// =============================================================================
// AnimatedMeshPrimitivesPropertyTests.cpp
//
// Correctness proof for MeshPrimitives::CreateAnimatedCube() and
// CreateMultiBoneAnimatedCube() (issue #592): the procedurally generated
// skinned primitives are built through MeshSource::Build(), which needs a
// live GL context to create GPU buffers, so this lives in the L1 property
// suite (gated on OLO_ENSURE_GPU_OR_SKIP) even though every assertion below
// is pure CPU math over the MeshSource / Skeleton contract.
//
// The linear-blend-skinning formula pinned here — skinnedPos = sum(weight_k
// * (finalBoneMatrix[boneId_k] * localPos)), with finalBoneMatrix = poseGlobal
// * inverseBindPose — mirrors AnimationSystem::Update (AnimationSystem.cpp)
// and PBR_MultiLight_Skinned.glsl's vertex-shader skinning step.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Recomputes global transforms + final skinning matrices from the
    // skeleton's current local transforms, mirroring the hierarchy walk in
    // AnimationSystem::Update (skeleton.m_GlobalTransforms[i] = parentGlobal
    // * preTransform * localTransform, then finalBoneMatrices[i] = global *
    // inverseBindPose). Both test skeletons here have identity pre-transforms.
    void RecomputePose(Skeleton& skeleton)
    {
        const sizet boneCount = skeleton.m_LocalTransforms.size();
        for (sizet i = 0; i < boneCount; ++i)
        {
            const i32 parent = skeleton.m_ParentIndices[i];
            skeleton.m_GlobalTransforms[i] = (parent >= 0)
                                                 ? skeleton.m_GlobalTransforms[static_cast<sizet>(parent)] * skeleton.m_LocalTransforms[i]
                                                 : skeleton.m_LocalTransforms[i];
        }
        for (sizet i = 0; i < boneCount; ++i)
        {
            skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i] * skeleton.m_InverseBindPoses[i];
        }
    }

    // Applies linear-blend skinning to one vertex using the mesh's stored
    // BoneInfluence, exactly as PBR_MultiLight_Skinned.glsl does.
    glm::vec3 SkinVertex(const Skeleton& skeleton, const BoneInfluence& influence, const glm::vec3& localPos)
    {
        glm::vec4 blended(0.0f);
        for (int k = 0; k < 4; ++k)
        {
            if (influence.m_Weights[k] > 0.0f)
            {
                blended += influence.m_Weights[k] * (skeleton.m_FinalBoneMatrices[influence.m_BoneIDs[k]] * glm::vec4(localPos, 1.0f));
            }
        }
        return glm::vec3(blended);
    }
} // namespace

TEST(AnimatedMeshPrimitives, AnimatedCubeHasRootBoneAndFullWeights)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto mesh = MeshPrimitives::CreateAnimatedCube();
    ASSERT_TRUE(mesh);
    auto meshSource = mesh->GetMeshSource();
    ASSERT_TRUE(meshSource);

    ASSERT_TRUE(meshSource->HasSkeleton());
    EXPECT_TRUE(meshSource->HasBoneInfluences());
    ASSERT_EQ(meshSource->GetBoneInfo().Num(), 1);
    EXPECT_EQ(meshSource->GetBoneInfo(0).m_BoneIndex, 0u);

    const auto& vertices = meshSource->GetVertices();
    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        const BoneInfluence& influence = meshSource->GetVertexBoneData(static_cast<u32>(i));
        EXPECT_EQ(influence.m_BoneIDs[0], 0u);
        EXPECT_FLOAT_EQ(influence.m_Weights[0], 1.0f);
        EXPECT_FLOAT_EQ(influence.m_Weights[1], 0.0f);
        EXPECT_FLOAT_EQ(influence.m_Weights[2], 0.0f);
        EXPECT_FLOAT_EQ(influence.m_Weights[3], 0.0f);
    }
}

TEST(AnimatedMeshPrimitives, AnimatedCubeRotatesRigidlyWithRootBone)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto mesh = MeshPrimitives::CreateAnimatedCube();
    ASSERT_TRUE(mesh);
    auto meshSource = mesh->GetMeshSource();
    ASSERT_TRUE(meshSource);

    Skeleton* skeleton = meshSource->GetSkeleton();
    ASSERT_NE(skeleton, nullptr);

    // Rotate the root bone 90 degrees about Y and recompute the pose — with
    // every vertex weighted 100% to bone 0, the whole mesh must rotate as a
    // rigid body, i.e. skinning must reproduce the same rotation applied
    // directly to the original vertex positions.
    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    skeleton->m_LocalTransforms[0] = rotation;
    RecomputePose(*skeleton);

    const auto& vertices = meshSource->GetVertices();
    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        const BoneInfluence& influence = meshSource->GetVertexBoneData(static_cast<u32>(i));
        const glm::vec3 localPos = vertices[i].Position;
        const glm::vec3 skinned = SkinVertex(*skeleton, influence, localPos);
        const glm::vec3 expected = glm::vec3(rotation * glm::vec4(localPos, 1.0f));

        EXPECT_NEAR(skinned.x, expected.x, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(skinned.y, expected.y, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(skinned.z, expected.z, 1e-4f) << "vertex " << i;
    }
}

TEST(AnimatedMeshPrimitives, MultiBoneCubeHasTwoBonesAndSeamBlendWeights)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto mesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
    ASSERT_TRUE(mesh);
    auto meshSource = mesh->GetMeshSource();
    ASSERT_TRUE(meshSource);

    ASSERT_TRUE(meshSource->HasSkeleton());
    EXPECT_TRUE(meshSource->HasBoneInfluences());
    ASSERT_EQ(meshSource->GetBoneInfo().Num(), 2);

    const auto& vertices = meshSource->GetVertices();
    bool sawPureLower = false;
    bool sawPureUpper = false;
    bool sawSeamBlend = false;

    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        const BoneInfluence& influence = meshSource->GetVertexBoneData(static_cast<u32>(i));
        const f32 totalWeight = influence.m_Weights[0] + influence.m_Weights[1] + influence.m_Weights[2] + influence.m_Weights[3];
        EXPECT_NEAR(totalWeight, 1.0f, 1e-4f) << "vertex " << i << " weights must sum to 1";

        const f32 y = vertices[i].Position.y;
        if (y < -1e-3f)
        {
            EXPECT_EQ(influence.m_BoneIDs[0], 0u);
            EXPECT_FLOAT_EQ(influence.m_Weights[0], 1.0f);
            sawPureLower = true;
        }
        else if (y > 1e-3f)
        {
            EXPECT_EQ(influence.m_BoneIDs[0], 1u);
            EXPECT_FLOAT_EQ(influence.m_Weights[0], 1.0f);
            sawPureUpper = true;
        }
        else
        {
            // Seam vertex: blended 50/50 between the lower and upper bone.
            EXPECT_FLOAT_EQ(influence.m_Weights[0], 0.5f);
            EXPECT_FLOAT_EQ(influence.m_Weights[1], 0.5f);
            EXPECT_EQ(influence.m_BoneIDs[0], 0u);
            EXPECT_EQ(influence.m_BoneIDs[1], 1u);
            sawSeamBlend = true;
        }
    }

    EXPECT_TRUE(sawPureLower) << "expected at least one vertex rigidly bound to the lower bone";
    EXPECT_TRUE(sawPureUpper) << "expected at least one vertex rigidly bound to the upper bone";
    EXPECT_TRUE(sawSeamBlend) << "expected at least one blended seam vertex — the case that exercises blend weights";
}

TEST(AnimatedMeshPrimitives, MultiBoneCubeBendsAtSeamWhenUpperBoneRotates)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto mesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
    ASSERT_TRUE(mesh);
    auto meshSource = mesh->GetMeshSource();
    ASSERT_TRUE(meshSource);

    Skeleton* skeleton = meshSource->GetSkeleton();
    ASSERT_NE(skeleton, nullptr);
    ASSERT_EQ(skeleton->m_LocalTransforms.size(), 2u);

    // Rotate only the upper bone 90 degrees about X, pivoting at the shared
    // bind-pose origin (the seam, y = 0). The lower bone stays at bind pose.
    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    skeleton->m_LocalTransforms[1] = rotation;
    RecomputePose(*skeleton);

    const auto& vertices = meshSource->GetVertices();
    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        const BoneInfluence& influence = meshSource->GetVertexBoneData(static_cast<u32>(i));
        const glm::vec3 localPos = vertices[i].Position;
        const glm::vec3 skinned = SkinVertex(*skeleton, influence, localPos);

        const f32 y = localPos.y;
        glm::vec3 expected;
        if (y < -1e-3f)
        {
            // Pure lower-bone vertices are unaffected by the upper rotation.
            expected = localPos;
        }
        else if (y > 1e-3f)
        {
            // Pure upper-bone vertices follow the rotation exactly.
            expected = glm::vec3(rotation * glm::vec4(localPos, 1.0f));
        }
        else
        {
            // Seam vertices blend the un-rotated and rotated positions 50/50.
            expected = 0.5f * localPos + 0.5f * glm::vec3(rotation * glm::vec4(localPos, 1.0f));
        }

        EXPECT_NEAR(skinned.x, expected.x, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(skinned.y, expected.y, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(skinned.z, expected.z, 1e-4f) << "vertex " << i;
    }

    // Sanity: the mesh actually bent — a pure-upper vertex must have moved
    // away from its bind-pose position once rotated 90 degrees about X.
    bool sawMovedUpperVertex = false;
    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        const glm::vec3 localPos = vertices[i].Position;
        if (localPos.y > 1e-3f)
        {
            const BoneInfluence& influence = meshSource->GetVertexBoneData(static_cast<u32>(i));
            const glm::vec3 skinned = SkinVertex(*skeleton, influence, localPos);
            if (glm::length(skinned - localPos) > 1e-3f)
            {
                sawMovedUpperVertex = true;
                break;
            }
        }
    }
    EXPECT_TRUE(sawMovedUpperVertex) << "rotating the upper bone should visibly move the upper half of the mesh";
}
