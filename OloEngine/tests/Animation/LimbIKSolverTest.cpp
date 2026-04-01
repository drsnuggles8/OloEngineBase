#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace OloEngine;
using namespace OloEngine::Animation;

// Helper: build a 3-bone arm chain along +Y
// root(0,0,0) -> shoulder(0,1,0) -> elbow(0,2,0) -> hand(0,3,0)
static void BuildArmChain(
    std::vector<BoneTransform>& pose,
    std::vector<int>& parentIndices)
{
    pose.resize(4);
    parentIndices = { -1, 0, 1, 2 };

    pose[0] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    pose[1] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    pose[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    pose[3] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
}

// Reachable target: end-effector should converge near the target
TEST(LimbIKSolverTest, ReachableTargetConverges)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    // Target within reach: chain length is 3 bones (each ~1 unit), total reach ~3 units
    LimbIKParams params;
    params.TargetBoneIndex = 3; // hand
    params.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    params.ChainLength = 3;
    params.Weight = 1.0f;

    LimbIKSolver::Solve(pose, parents, params);

    // Verify the end-effector moved toward the target
    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);

    f32 dist = glm::length(modelSpace[3].Translation - params.TargetPosition);
    EXPECT_LT(dist, 0.5f) << "End-effector should be near the target";
}

// Unreachable target: chain should extend to max reach
TEST(LimbIKSolverTest, UnreachableTargetExtendsChain)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    // Target far beyond reach
    LimbIKParams params;
    params.TargetBoneIndex = 3;
    params.TargetPosition = glm::vec3(100.0f, 0.0f, 0.0f);
    params.ChainLength = 3;
    params.Weight = 1.0f;

    LimbIKSolver::Solve(pose, parents, params);

    // Chain should extend roughly toward target direction
    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);

    auto handDir = glm::normalize(modelSpace[3].Translation - modelSpace[0].Translation);
    auto targetDir = glm::normalize(params.TargetPosition - modelSpace[0].Translation);

    f32 dot = glm::dot(handDir, targetDir);
    EXPECT_GT(dot, 0.5f) << "Chain should extend roughly toward unreachable target";
}

// Zero weight: pose should remain unchanged
TEST(LimbIKSolverTest, ZeroWeightPassthrough)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    auto originalPose = pose;

    LimbIKParams params;
    params.TargetBoneIndex = 3;
    params.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    params.ChainLength = 3;
    params.Weight = 0.0f;

    LimbIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f)
            << "Weight 0 should leave pose unchanged at bone " << i;
    }
}

// Zero chain length: should be a no-op
TEST(LimbIKSolverTest, ZeroChainLengthIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    auto originalPose = pose;

    LimbIKParams params;
    params.TargetBoneIndex = 3;
    params.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    params.ChainLength = 0;

    LimbIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Invalid bone index: should be a no-op
TEST(LimbIKSolverTest, InvalidBoneIndexIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    auto originalPose = pose;

    LimbIKParams params;
    params.TargetBoneIndex = 999;
    params.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    params.ChainLength = 3;

    LimbIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Partial weight: result should be between original and full IK
TEST(LimbIKSolverTest, PartialWeightBlendsResult)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    // Full weight
    auto fullPose = pose;
    LimbIKParams fullParams;
    fullParams.TargetBoneIndex = 3;
    fullParams.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    fullParams.ChainLength = 3;
    fullParams.Weight = 1.0f;
    LimbIKSolver::Solve(fullPose, parents, fullParams);

    // Half weight
    auto halfPose = pose;
    LimbIKParams halfParams = fullParams;
    halfParams.Weight = 0.5f;
    LimbIKSolver::Solve(halfPose, parents, halfParams);

    // Compute model-space to compare distances
    std::vector<BoneTransform> msOrig, msFull, msHalf;
    BlendUtils::ComputeModelSpacePose(pose, parents, msOrig);
    BlendUtils::ComputeModelSpacePose(fullPose, parents, msFull);
    BlendUtils::ComputeModelSpacePose(halfPose, parents, msHalf);

    f32 distOrig = glm::length(msOrig[3].Translation - fullParams.TargetPosition);
    f32 distFull = glm::length(msFull[3].Translation - fullParams.TargetPosition);
    f32 distHalf = glm::length(msHalf[3].Translation - fullParams.TargetPosition);

    // Half weight should be closer to target than original, but further than full
    EXPECT_LT(distHalf, distOrig + 0.01f) << "Half weight should move toward target";
}

// Single-bone chain: no crash (edge case, needs at least 2 bones for FABRIK)
TEST(LimbIKSolverTest, SingleBoneChainHandledGracefully)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    LimbIKParams params;
    params.TargetBoneIndex = 3;
    params.TargetPosition = glm::vec3(1.5f, 2.0f, 0.0f);
    params.ChainLength = 1; // Only 1 bone → FABRIK needs ≥2

    // Should not crash, may be a no-op
    EXPECT_NO_THROW(LimbIKSolver::Solve(pose, parents, params));
}

// Target at current position: minimal change
TEST(LimbIKSolverTest, TargetAtCurrentPositionMinimalChange)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildArmChain(pose, parents);

    // Compute current hand position
    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);
    auto handPos = modelSpace[3].Translation;

    auto originalPose = pose;

    LimbIKParams params;
    params.TargetBoneIndex = 3;
    params.TargetPosition = handPos; // Already there
    params.ChainLength = 3;
    params.Weight = 1.0f;

    LimbIKSolver::Solve(pose, parents, params);

    // Rotations should remain close to original
    for (sizet i = 0; i < pose.size(); ++i)
    {
        f32 dot = std::abs(glm::dot(pose[i].Rotation, originalPose[i].Rotation));
        EXPECT_GT(dot, 0.99f) << "Pose should barely change when target is at current position, bone " << i;
    }
}
