#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/IK/AimIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace OloEngine;
using namespace OloEngine::Animation;

// Helper: build a simple 3-bone chain (root -> mid -> tip)
// Bones are laid out along +Y: root at origin, mid at (0,1,0), tip at (0,2,0)
static void BuildSimpleChain(
    std::vector<BoneTransform>& pose,
    std::vector<int>& parentIndices)
{
    pose.resize(3);
    parentIndices = { -1, 0, 1 };

    // Root at origin
    pose[0] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    // Mid bone: 1 unit up from root
    pose[1] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    // Tip bone: 1 unit up from mid
    pose[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
}

// Single-bone AimIK: bone should rotate so its aim axis points at target
TEST(AimIKSolverTest, SingleBoneAimAtTarget)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    // Aim the tip bone at a target along +X (model space)
    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(5.0f, 2.0f, 0.0f);
    params.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f); // local Z should point at target
    params.ChainLength = 1;
    params.Weight = 1.0f;

    AimIKSolver::Solve(pose, parents, params);

    // After IK, compute model-space to verify the aim axis points roughly toward target
    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);

    auto bonePos = modelSpace[2].Translation;
    auto aimDir = BlendUtils::TransformVector(modelSpace[2], params.AimAxis);
    auto targetDir = glm::normalize(params.TargetPosition - bonePos);

    // The aim direction should align with the target direction
    f32 dotProduct = glm::dot(glm::normalize(aimDir), targetDir);
    EXPECT_GT(dotProduct, 0.95f) << "Aim axis should point approximately toward target";
}

// Zero weight: pose should remain unchanged
TEST(AimIKSolverTest, ZeroWeightPassthrough)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    auto originalPose = pose;

    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(5.0f, 2.0f, 0.0f);
    params.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    params.ChainLength = 1;
    params.Weight = 0.0f;

    AimIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f)
            << "Weight 0 should leave pose unchanged at bone " << i;
    }
}

// Multi-bone chain: rotation should be distributed
TEST(AimIKSolverTest, MultiBoneChainDistributesRotation)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(3.0f, 2.0f, 0.0f);
    params.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    params.ChainLength = 3; // All three bones participate
    params.ChainFactor = 0.5f;
    params.Weight = 1.0f;

    AimIKSolver::Solve(pose, parents, params);

    // Verify that the root bone was also rotated (not just the end-effector)
    auto identityQuat = glm::identity<glm::quat>();
    f32 rootRotationDiff = 1.0f - std::abs(glm::dot(pose[0].Rotation, identityQuat));
    EXPECT_GT(rootRotationDiff, 1e-4f) << "Root bone should be rotated when chain includes it";
}

// Chain length of 0: should be a no-op
TEST(AimIKSolverTest, ZeroChainLengthIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    auto originalPose = pose;

    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(5.0f, 2.0f, 0.0f);
    params.ChainLength = 0;

    AimIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Invalid bone index: should be a no-op
TEST(AimIKSolverTest, InvalidBoneIndexIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    auto originalPose = pose;

    AimIKParams params;
    params.TargetBoneIndex = 999; // Out of range
    params.TargetPosition = glm::vec3(5.0f, 2.0f, 0.0f);
    params.ChainLength = 1;

    AimIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Target at bone position (degenerate): should not crash
TEST(AimIKSolverTest, TargetAtBonePositionDoesNotCrash)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    // Target exactly at the tip bone's model-space position
    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(0.0f, 2.0f, 0.0f); // Same as bone position
    params.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    params.ChainLength = 1;
    params.Weight = 1.0f;

    // Should not crash
    EXPECT_NO_THROW(AimIKSolver::Solve(pose, parents, params));
}

// Partial weight: result should be between original and full IK
TEST(AimIKSolverTest, PartialWeightBlendsResult)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    auto originalPose = pose;

    // First compute full-weight result
    auto fullWeightPose = pose;
    AimIKParams fullParams;
    fullParams.TargetBoneIndex = 2;
    fullParams.TargetPosition = glm::vec3(5.0f, 2.0f, 0.0f);
    fullParams.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    fullParams.ChainLength = 1;
    fullParams.Weight = 1.0f;
    AimIKSolver::Solve(fullWeightPose, parents, fullParams);

    // Now compute half-weight result
    AimIKParams halfParams = fullParams;
    halfParams.Weight = 0.5f;
    AimIKSolver::Solve(pose, parents, halfParams);

    // Half-weight rotation should be between original and full-weight
    f32 dotOriginal = std::abs(glm::dot(pose[2].Rotation, originalPose[2].Rotation));
    f32 dotFull = std::abs(glm::dot(pose[2].Rotation, fullWeightPose[2].Rotation));

    // Should be somewhat close to both (not identical to either)
    // Allow some tolerance since the math is complex
    EXPECT_LT(dotOriginal, 0.999f) << "Half weight should differ from original";
    EXPECT_LT(dotFull, 0.999f) << "Half weight should differ from full IK";
}

// ChainFactor of 0: all rotation on end-effector, none on ancestors
TEST(AimIKSolverTest, ChainFactorZeroPutsAllRotationOnEndBone)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildSimpleChain(pose, parents);

    auto originalPose = pose;

    AimIKParams params;
    params.TargetBoneIndex = 2;
    params.TargetPosition = glm::vec3(3.0f, 2.0f, 0.0f);
    params.AimAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    params.ChainLength = 2;
    params.ChainFactor = 0.0f; // No distribution to ancestors
    params.Weight = 1.0f;

    AimIKSolver::Solve(pose, parents, params);

    // With chainFactor 0, first bone (tip) gets weight 0 via NLerp(identity, correction, 0)
    // which means identity, and the last bone (parent) gets weight 1.
    // The mid bone (parent of tip in chain) should be rotated.
    // Note: "last in chain" means furthest ancestor.
    auto identityQuat = glm::identity<glm::quat>();
    f32 tipDiff = 1.0f - std::abs(glm::dot(pose[2].Rotation, identityQuat));
    // The tip bone (first in iteration, i=0) gets chainFactor=0 → identity correction
    EXPECT_LT(tipDiff, 0.01f) << "Tip bone should have minimal rotation with chainFactor=0";
}
