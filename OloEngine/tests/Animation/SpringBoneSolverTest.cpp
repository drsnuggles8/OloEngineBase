#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/Procedural/SpringBoneSolver.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Animation;

namespace
{
    constexpr f32 kDt = 1.0f / 60.0f;

    // 4-bone chain along +Y: root(0,0,0) -> b1(0,1,0) -> b2(0,2,0) -> b3(0,3,0).
    // Mirrors the BuildArmChain helper in LimbIKSolverTest.
    void BuildChain(std::vector<BoneTransform>& pose, std::vector<int>& parentIndices)
    {
        pose.resize(4);
        parentIndices = { -1, 0, 1, 2 };

        pose[0] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[1] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[3] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    }

    SpringBoneParams DefaultParams()
    {
        SpringBoneParams params;
        params.EndBoneIndex = 3;
        params.ChainLength = 4;
        params.Stiffness = 80.0f;
        params.Damping = 12.0f;
        params.Gravity = glm::vec3(0.0f);
        params.Weight = 1.0f;
        return params;
    }

    std::vector<glm::vec3> ModelSpacePositions(std::span<const BoneTransform> pose, std::span<const int> parents)
    {
        std::vector<BoneTransform> modelSpace;
        BlendUtils::ComputeModelSpacePose(pose, parents, modelSpace);
        std::vector<glm::vec3> positions(modelSpace.size());
        for (sizet i = 0; i < modelSpace.size(); ++i)
        {
            positions[i] = modelSpace[i].Translation;
        }
        return positions;
    }
} // namespace

// First Solve initializes the state from the animated pose and must not move anything.
TEST(SpringBoneSolverTest, FirstSolveInitializesStateWithoutModifyingPose)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    auto originalPose = pose;

    SpringBoneState state;
    SpringBoneSolver::Solve(pose, parents, DefaultParams(), state, kDt);

    EXPECT_TRUE(state.Initialized);
    EXPECT_EQ(state.CurrPositions.size(), 3u); // 4 chain bones -> 3 simulated joints
    for (sizet i = 0; i < pose.size(); ++i)
    {
        // The init frame returns before any write-back, so this is bit-exact.
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation))
            << "First solve (state init) must leave bone " << i << " untouched";
    }
}

// A chain at rest with zero gravity must stay at rest indefinitely.
TEST(SpringBoneSolverTest, RestPoseIsStableOverManySteps)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    auto originalPose = pose;

    SpringBoneState state;
    auto params = DefaultParams();
    for (int frame = 0; frame < 300; ++frame)
    {
        SpringBoneSolver::Solve(pose, parents, params, state, kDt);
    }

    for (sizet i = 0; i < pose.size(); ++i)
    {
        // Sign-invariant rotation comparison: q and -q encode the same
        // rotation, so compare via |dot| -> 1 instead of component deltas.
        EXPECT_NEAR(1.0f - std::abs(glm::dot(pose[i].Rotation, originalPose[i].Rotation)), 0.0f, 1e-6f)
            << "Rest pose drifted at bone " << i << " after 300 frames";
    }
}

// With gravity the chain tip must sag below its animated rest position.
TEST(SpringBoneSolverTest, GravitySagsChainBelowRestPose)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    params.Stiffness = 5.0f; // weak spring so gravity visibly wins
    params.Gravity = glm::vec3(0.0f, -9.81f, 0.0f);

    // The chain points straight up (+Y) and gravity pulls straight down, a
    // perfectly singular equilibrium — give it a slight sideways rest tilt
    // so gravity has a lever arm, as any real animated pose would.
    pose[1].Translation = glm::vec3(0.2f, 1.0f, 0.0f);

    std::vector<BoneTransform> workPose;
    for (int frame = 0; frame < 600; ++frame)
    {
        workPose = pose; // animated pose is re-evaluated every frame
        SpringBoneSolver::Solve(workPose, parents, params, state, kDt);
    }

    auto restPositions = ModelSpacePositions(pose, parents);
    auto simPositions = ModelSpacePositions(workPose, parents);
    EXPECT_LT(simPositions[3].y, restPositions[3].y - 0.05f)
        << "Tip should sag below the rest pose under gravity";
}

// Segment lengths must be preserved by the simulation (length constraint).
TEST(SpringBoneSolverTest, SegmentLengthsArePreserved)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    params.Stiffness = 5.0f;
    params.Gravity = glm::vec3(3.0f, -9.81f, 1.0f); // off-axis so the chain bends

    std::vector<BoneTransform> workPose;
    for (int frame = 0; frame < 120; ++frame)
    {
        workPose = pose;
        SpringBoneSolver::Solve(workPose, parents, params, state, kDt);
    }

    auto positions = ModelSpacePositions(workPose, parents);
    for (sizet i = 1; i < positions.size(); ++i)
    {
        EXPECT_NEAR(glm::length(positions[i] - positions[i - 1]), 1.0f, 1e-3f)
            << "Segment " << i << " length not preserved";
    }
}

// After a transient disturbance, a damped spring must converge back to the
// animated pose (error decreases over time).
TEST(SpringBoneSolverTest, ConvergesBackToRestAfterDisturbance)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();

    // Settle, then disturb the simulated state directly (as a sudden
    // animated-pose jump would).
    std::vector<BoneTransform> workPose = pose;
    SpringBoneSolver::Solve(workPose, parents, params, state, kDt);
    for (auto& p : state.CurrPositions)
    {
        p += glm::vec3(0.3f, 0.0f, 0.0f);
    }

    auto tipError = [&](const std::vector<BoneTransform>& solved)
    {
        auto rest = ModelSpacePositions(pose, parents);
        auto sim = ModelSpacePositions(solved, parents);
        return glm::length(sim[3] - rest[3]);
    };

    workPose = pose;
    SpringBoneSolver::Solve(workPose, parents, params, state, kDt);
    f32 earlyError = tipError(workPose);
    EXPECT_GT(earlyError, 0.01f) << "Disturbance should visibly deflect the chain";

    for (int frame = 0; frame < 600; ++frame)
    {
        workPose = pose;
        SpringBoneSolver::Solve(workPose, parents, params, state, kDt);
    }
    f32 lateError = tipError(workPose);
    EXPECT_LT(lateError, earlyError * 0.1f)
        << "Damped spring must converge back toward the animated pose";
    EXPECT_LT(lateError, 0.01f);
}

// Identical inputs must produce bitwise-identical state trajectories.
TEST(SpringBoneSolverTest, DeterministicGivenPoseAndDeltaTime)
{
    std::vector<BoneTransform> poseA;
    std::vector<BoneTransform> poseB;
    std::vector<int> parents;
    BuildChain(poseA, parents);
    BuildChain(poseB, parents);

    SpringBoneState stateA;
    SpringBoneState stateB;
    auto params = DefaultParams();
    params.Gravity = glm::vec3(1.0f, -9.81f, 0.5f);

    for (int frame = 0; frame < 120; ++frame)
    {
        SpringBoneSolver::Solve(poseA, parents, params, stateA, kDt);
        SpringBoneSolver::Solve(poseB, parents, params, stateB, kDt);
    }

    ASSERT_EQ(stateA.CurrPositions.size(), stateB.CurrPositions.size());
    for (sizet i = 0; i < stateA.CurrPositions.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(stateA.CurrPositions[i], stateB.CurrPositions[i]))
            << "Simulation diverged at joint " << i;
    }
    for (sizet i = 0; i < poseA.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(poseA[i].Rotation, poseB[i].Rotation))
            << "Output pose diverged at bone " << i;
    }
}

// Non-finite parameters must leave both the pose and the state untouched.
TEST(SpringBoneSolverTest, NonFiniteParamsAreRejected)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    SpringBoneSolver::Solve(pose, parents, params, state, kDt); // init
    auto originalPose = pose;
    auto originalState = state.CurrPositions;

    const f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
    const f32 kInf = std::numeric_limits<f32>::infinity();

    auto badStiffness = params;
    badStiffness.Stiffness = kNaN;
    auto badGravity = params;
    badGravity.Gravity = glm::vec3(0.0f, kInf, 0.0f);
    auto badWeight = params;
    badWeight.Weight = kNaN;

    for (const auto& bad : { badStiffness, badGravity, badWeight })
    {
        SpringBoneSolver::Solve(pose, parents, bad, state, kDt);
    }
    SpringBoneSolver::Solve(pose, parents, params, state, kNaN); // bad dt

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation))
            << "Non-finite params must not modify the pose (bone " << i << ")";
    }
    for (sizet i = 0; i < state.CurrPositions.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(state.CurrPositions[i], originalState[i]))
            << "Non-finite params must not corrupt the state (joint " << i << ")";
    }
}

// Weight 0 must be a passthrough even with the simulation deflected.
TEST(SpringBoneSolverTest, ZeroWeightPassthrough)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    SpringBoneSolver::Solve(pose, parents, params, state, kDt); // init
    for (auto& p : state.CurrPositions)
    {
        p += glm::vec3(0.5f, 0.0f, 0.0f);
    }

    auto originalPose = pose;
    params.Weight = 0.0f;
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation))
            << "Weight 0 must leave bone " << i << " unchanged";
    }
}

// Chain length below 2 cannot simulate anything and must be a no-op.
TEST(SpringBoneSolverTest, ChainLengthOneIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    auto originalPose = pose;

    SpringBoneState state;
    auto params = DefaultParams();
    params.ChainLength = 1;
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);

    EXPECT_FALSE(state.Initialized);
    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation));
    }
}

// Out-of-range end bone index must be a no-op.
TEST(SpringBoneSolverTest, OutOfRangeEndBoneIsNoOp)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    auto originalPose = pose;

    SpringBoneState state;
    auto params = DefaultParams();
    params.EndBoneIndex = 99;
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);

    EXPECT_FALSE(state.Initialized);
    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation));
    }
}

// Changing the chain shape re-initializes the state instead of indexing stale data.
TEST(SpringBoneSolverTest, StateReinitializesWhenChainShapeChanges)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);
    ASSERT_EQ(state.CurrPositions.size(), 3u);

    // Shrink the chain: 3 bones -> 2 simulated joints; first call re-inits
    params.ChainLength = 3;
    auto originalPose = pose;
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);
    EXPECT_EQ(state.CurrPositions.size(), 2u);
    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, originalPose[i].Rotation))
            << "Re-init frame must not modify the pose";
    }
}

// A corrupted (non-finite) state must self-heal by re-initializing.
TEST(SpringBoneSolverTest, NonFiniteStateSelfHeals)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);

    SpringBoneState state;
    auto params = DefaultParams();
    SpringBoneSolver::Solve(pose, parents, params, state, kDt);
    state.CurrPositions[1].y = std::numeric_limits<f32>::quiet_NaN();

    SpringBoneSolver::Solve(pose, parents, params, state, kDt);

    for (const auto& p : state.CurrPositions)
    {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
            << "State must be re-initialized to finite values";
    }
}
