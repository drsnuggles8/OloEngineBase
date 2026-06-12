#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/IK/FABRIKSolver.h"
#include "OloEngine/Animation/BlendUtils.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <span>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Animation;

// Helper: build a 5-bone chain along +Y (4 segments of length 1)
// root(0,0,0) -> j1(0,1,0) -> j2(0,2,0) -> j3(0,3,0) -> tip(0,4,0)
static void BuildChain(
    std::vector<BoneTransform>& pose,
    std::vector<int>& parentIndices)
{
    pose.resize(5);
    parentIndices = { -1, 0, 1, 2, 3 };

    pose[0] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    for (sizet i = 1; i < 5; ++i)
    {
        pose[i] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    }
}

static std::vector<BoneTransform> ToModelSpace(
    std::span<const BoneTransform> pose,
    std::span<const int> parents)
{
    std::vector<BoneTransform> modelSpace;
    BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);
    return modelSpace;
}

// Reachable target: end-effector should converge within tolerance
TEST(FABRIKSolverTest, ReachableTargetConverges)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    FABRIKParams params;
    params.TargetBoneIndex = 4;                          // tip
    params.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f); // within reach (total length 4)
    params.ChainLength = 5;
    params.MaxIterations = 20;
    params.Tolerance = 0.001f;
    params.Weight = 1.0f;

    FABRIKSolver::Solve(pose, parents, params);

    auto modelSpace = ToModelSpace(pose, parents);
    f32 dist = glm::length(modelSpace[4].Translation - params.TargetPosition);
    EXPECT_LT(dist, 0.01f) << "End-effector should converge to the reachable target";
}

// Unreachable target: chain should fully straighten toward the target
TEST(FABRIKSolverTest, UnreachableTargetStraightensChain)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(100.0f, 0.0f, 0.0f);
    params.ChainLength = 5;
    params.Weight = 1.0f;

    FABRIKSolver::Solve(pose, parents, params);

    auto modelSpace = ToModelSpace(pose, parents);
    auto targetDir = glm::normalize(params.TargetPosition - modelSpace[0].Translation);

    // Every segment should point straight at the target — full extension
    for (sizet i = 1; i < 5; ++i)
    {
        auto segDir = glm::normalize(modelSpace[i].Translation - modelSpace[i - 1].Translation);
        EXPECT_GT(glm::dot(segDir, targetDir), 0.999f)
            << "Segment " << i << " should be collinear with the target direction";
    }

    // Tip should sit at exactly max reach (total chain length 4)
    f32 tipDist = glm::length(modelSpace[4].Translation - modelSpace[0].Translation);
    EXPECT_NEAR(tipDist, 4.0f, 0.01f);
}

// Bone lengths must be preserved by the solve
TEST(FABRIKSolverTest, BoneLengthsPreserved)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(1.5f, 1.0f, 1.0f);
    params.ChainLength = 5;
    params.MaxIterations = 20;
    params.Weight = 1.0f;

    FABRIKSolver::Solve(pose, parents, params);

    auto modelSpace = ToModelSpace(pose, parents);
    for (sizet i = 1; i < 5; ++i)
    {
        f32 segLen = glm::length(modelSpace[i].Translation - modelSpace[i - 1].Translation);
        EXPECT_NEAR(segLen, 1.0f, 0.01f) << "Segment " << i << " length should be preserved";
    }
}

// Pole vector should pull intermediate joints toward its side of the chain
TEST(FABRIKSolverTest, PoleVectorBiasesBendDirection)
{
    std::vector<BoneTransform> poseTowardPole;
    std::vector<BoneTransform> poseAwayFromPole;
    std::vector<int> parents;
    BuildChain(poseTowardPole, parents);
    poseAwayFromPole = poseTowardPole;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(1.0f, 2.0f, 0.0f); // closer than full reach — chain must bend
    params.ChainLength = 5;
    params.MaxIterations = 20;
    params.Weight = 1.0f;

    params.PoleVector = glm::vec3(0.0f, 1.0f, 5.0f); // far on +Z
    FABRIKSolver::Solve(poseTowardPole, parents, params);

    params.PoleVector = glm::vec3(0.0f, 1.0f, -5.0f); // far on -Z
    FABRIKSolver::Solve(poseAwayFromPole, parents, params);

    auto msToward = ToModelSpace(poseTowardPole, parents);
    auto msAway = ToModelSpace(poseAwayFromPole, parents);

    // Middle joints should land on opposite Z sides for opposite poles
    EXPECT_GT(msToward[2].Translation.z, msAway[2].Translation.z + 0.01f)
        << "Opposite pole vectors should bend the chain to opposite sides";

    // Pole must not break convergence or bone lengths
    EXPECT_LT(glm::length(msToward[4].Translation - params.TargetPosition), 0.05f);
    for (sizet i = 1; i < 5; ++i)
    {
        f32 segLen = glm::length(msToward[i].Translation - msToward[i - 1].Translation);
        EXPECT_NEAR(segLen, 1.0f, 0.01f) << "Pole constraint must preserve segment " << i << " length";
    }
}

// Zero weight: pose should remain unchanged
TEST(FABRIKSolverTest, ZeroWeightPassthrough)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f);
    params.ChainLength = 5;
    params.Weight = 0.0f;

    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f)
            << "Weight 0 should leave pose unchanged at bone " << i;
    }
}

// Partial weight: result should be between original and full IK
TEST(FABRIKSolverTest, PartialWeightBlendsResult)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    FABRIKParams fullParams;
    fullParams.TargetBoneIndex = 4;
    fullParams.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f);
    fullParams.ChainLength = 5;
    fullParams.Weight = 1.0f;

    auto fullPose = pose;
    FABRIKSolver::Solve(fullPose, parents, fullParams);

    auto halfPose = pose;
    FABRIKParams halfParams = fullParams;
    halfParams.Weight = 0.5f;
    FABRIKSolver::Solve(halfPose, parents, halfParams);

    auto msOrig = ToModelSpace(pose, parents);
    auto msFull = ToModelSpace(fullPose, parents);
    auto msHalf = ToModelSpace(halfPose, parents);

    f32 distOrig = glm::length(msOrig[4].Translation - fullParams.TargetPosition);
    f32 distFull = glm::length(msFull[4].Translation - fullParams.TargetPosition);
    f32 distHalf = glm::length(msHalf[4].Translation - fullParams.TargetPosition);

    EXPECT_LT(distHalf, distOrig + 0.01f) << "Half weight should move toward target";
    EXPECT_GT(distHalf, distFull + 0.01f) << "Half weight should not reach as far as full weight";
}

// Chain length below 2: should be a no-op (FABRIK needs at least 2 bones)
TEST(FABRIKSolverTest, ChainLengthBelowTwoIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f);

    params.ChainLength = 0;
    FABRIKSolver::Solve(pose, parents, params);
    params.ChainLength = 1;
    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Invalid bone index: should be a no-op
TEST(FABRIKSolverTest, InvalidBoneIndexIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 999;
    params.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f);
    params.ChainLength = 5;

    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Non-finite parameters: should be a no-op
TEST(FABRIKSolverTest, NonFiniteTargetIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f);
    params.ChainLength = 5;

    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f);
    }
}

// Degenerate chain with all joints coincident: no crash, no-op
TEST(FABRIKSolverTest, ZeroLengthChainHandledGracefully)
{
    std::vector<BoneTransform> pose(5, { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) });
    std::vector<int> parents = { -1, 0, 1, 2, 3 };

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(2.0f, 2.0f, 0.0f);
    params.ChainLength = 5;

    EXPECT_NO_THROW(FABRIKSolver::Solve(pose, parents, params));

    // All joints coincident — solver must bail out without modifying the pose
    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - glm::identity<glm::quat>()), 0.0f, 1e-5f);
    }
}

// Partial chain (shorter than the full skeleton): only chain bones move
TEST(FABRIKSolverTest, PartialChainLeavesRootBonesUntouched)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = glm::vec3(1.0f, 3.0f, 0.0f);
    params.ChainLength = 3; // bones 2,3,4 — bones 0,1 stay fixed
    params.Weight = 1.0f;

    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < 2; ++i)
    {
        EXPECT_NEAR(glm::length(pose[i].Rotation - originalPose[i].Rotation), 0.0f, 1e-5f)
            << "Bone " << i << " is outside the chain and must not move";
    }

    auto modelSpace = ToModelSpace(pose, parents);
    EXPECT_NEAR(glm::length(modelSpace[2].Translation - glm::vec3(0.0f, 2.0f, 0.0f)), 0.0f, 1e-4f)
        << "Chain root joint must stay pinned";
    EXPECT_LT(glm::length(modelSpace[4].Translation - params.TargetPosition), 0.01f);
}

// Target at current tip position: pose should barely change
TEST(FABRIKSolverTest, TargetAtCurrentPositionMinimalChange)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    auto modelSpace = ToModelSpace(pose, parents);
    auto tipPos = modelSpace[4].Translation;

    auto originalPose = pose;

    FABRIKParams params;
    params.TargetBoneIndex = 4;
    params.TargetPosition = tipPos;
    params.ChainLength = 5;
    params.Weight = 1.0f;

    FABRIKSolver::Solve(pose, parents, params);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        f32 dot = std::abs(glm::dot(pose[i].Rotation, originalPose[i].Rotation));
        EXPECT_GT(dot, 0.99f) << "Pose should barely change when target is at current tip, bone " << i;
    }
}
