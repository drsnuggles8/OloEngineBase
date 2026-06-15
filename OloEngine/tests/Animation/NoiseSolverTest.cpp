#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/Procedural/NoiseSolver.h"
#include "OloEngine/Animation/Procedural/NoisePostPass.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Animation;

namespace
{
    NoiseParams DefaultParams()
    {
        NoiseParams params;
        params.EndBoneIndex = 3;
        params.ChainLength = 4;
        params.Frequency = 1.0f;
        params.RotationAmplitude = glm::vec3(0.1f, 0.2f, 0.3f);
        params.TranslationAmplitude = glm::vec3(0.05f, 0.1f, 0.15f);
        params.Octaves = 3;
        params.Lacunarity = 2.0f;
        params.Gain = 0.5f;
        params.Seed = 7;
        params.Weight = 1.0f;
        return params;
    }

    // 4-bone chain along +Y: root(0) -> b1 -> b2 -> b3(tip). Mirrors the
    // SpringBoneSolverTest chain so the two suites stay comparable.
    void BuildChain(std::vector<BoneTransform>& pose, std::vector<int>& parentIndices)
    {
        pose.resize(4);
        parentIndices = { -1, 0, 1, 2 };
        pose[0] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[1] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
        pose[3] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
    }

    bool IsFiniteVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
} // namespace

// The per-axis rotation/translation offset must never exceed its amplitude
// (scaled by clamped weight) — the core "additive offsets stay bounded" claim.
TEST(NoiseSolverTest, OffsetBoundedByAmplitude)
{
    auto params = DefaultParams();
    constexpr f32 kEps = 1e-5f;

    for (int step = 0; step < 2000; ++step)
    {
        const f32 time = static_cast<f32>(step) * 0.013f;
        for (u32 chainIdx = 0; chainIdx < 4; ++chainIdx)
        {
            const NoiseBoneOffset o = NoiseSolver::SampleBoneOffset(params, chainIdx, chainIdx, time);
            EXPECT_LE(std::abs(o.EulerRadians.x), params.RotationAmplitude.x + kEps);
            EXPECT_LE(std::abs(o.EulerRadians.y), params.RotationAmplitude.y + kEps);
            EXPECT_LE(std::abs(o.EulerRadians.z), params.RotationAmplitude.z + kEps);
            EXPECT_LE(std::abs(o.Translation.x), params.TranslationAmplitude.x + kEps);
            EXPECT_LE(std::abs(o.Translation.y), params.TranslationAmplitude.y + kEps);
            EXPECT_LE(std::abs(o.Translation.z), params.TranslationAmplitude.z + kEps);
        }
    }
}

// Weight linearly scales the bound: at weight 0.5 the offset must stay within
// half the amplitude.
TEST(NoiseSolverTest, WeightScalesBound)
{
    auto params = DefaultParams();
    params.Weight = 0.5f;
    constexpr f32 kEps = 1e-5f;

    for (int step = 0; step < 500; ++step)
    {
        const f32 time = static_cast<f32>(step) * 0.021f;
        const NoiseBoneOffset o = NoiseSolver::SampleBoneOffset(params, 1, 2, time);
        EXPECT_LE(std::abs(o.EulerRadians.x), 0.5f * params.RotationAmplitude.x + kEps);
        EXPECT_LE(std::abs(o.EulerRadians.y), 0.5f * params.RotationAmplitude.y + kEps);
        EXPECT_LE(std::abs(o.EulerRadians.z), 0.5f * params.RotationAmplitude.z + kEps);
    }
}

// Weight 0 must produce an exactly-zero offset (full passthrough).
TEST(NoiseSolverTest, ZeroWeightIsZeroOffset)
{
    auto params = DefaultParams();
    params.Weight = 0.0f;
    const NoiseBoneOffset o = NoiseSolver::SampleBoneOffset(params, 0, 0, 1.234f);
    EXPECT_TRUE(Math::BitwiseEqual(o.EulerRadians, glm::vec3(0.0f)));
    EXPECT_TRUE(Math::BitwiseEqual(o.Translation, glm::vec3(0.0f)));
}

// Identical inputs must produce a bitwise-identical offset (deterministic).
TEST(NoiseSolverTest, DeterministicGivenParamsAndTime)
{
    auto params = DefaultParams();
    for (int step = 0; step < 50; ++step)
    {
        const f32 time = static_cast<f32>(step) * 0.1f;
        const NoiseBoneOffset a = NoiseSolver::SampleBoneOffset(params, 2, 3, time);
        const NoiseBoneOffset b = NoiseSolver::SampleBoneOffset(params, 2, 3, time);
        EXPECT_TRUE(Math::BitwiseEqual(a.EulerRadians, b.EulerRadians));
        EXPECT_TRUE(Math::BitwiseEqual(a.Translation, b.Translation));
    }
}

// Different bones must get de-correlated motion (not a rigid all-bones-together
// wobble) — adjacent chain positions sample distinct noise coordinates.
TEST(NoiseSolverTest, BonesAreDecorrelated)
{
    auto params = DefaultParams();
    bool anyDifference = false;
    for (int step = 0; step < 20 && !anyDifference; ++step)
    {
        const f32 time = static_cast<f32>(step) * 0.25f;
        const NoiseBoneOffset a = NoiseSolver::SampleBoneOffset(params, 0, 3, time);
        const NoiseBoneOffset b = NoiseSolver::SampleBoneOffset(params, 1, 2, time);
        if (!Math::BitwiseEqual(a.EulerRadians, b.EulerRadians))
        {
            anyDifference = true;
        }
    }
    EXPECT_TRUE(anyDifference) << "Different chain bones produced identical noise — not de-correlated";
}

// Changing the seed must change the motion (instances de-correlate).
TEST(NoiseSolverTest, SeedChangesMotion)
{
    auto a = DefaultParams();
    auto b = DefaultParams();
    b.Seed = a.Seed + 1;
    bool anyDifference = false;
    for (int step = 0; step < 20 && !anyDifference; ++step)
    {
        const f32 time = static_cast<f32>(step) * 0.25f;
        if (!Math::BitwiseEqual(NoiseSolver::SampleBoneOffset(a, 0, 3, time).EulerRadians,
                                NoiseSolver::SampleBoneOffset(b, 0, 3, time).EulerRadians))
        {
            anyDifference = true;
        }
    }
    EXPECT_TRUE(anyDifference) << "Different seeds produced identical noise";
}

// The noise is a smooth function of time — between two close samples the offset
// changes by only a small amount (no per-frame jitter / discontinuity).
TEST(NoiseSolverTest, MotionIsSmoothOverTime)
{
    auto params = DefaultParams();
    constexpr f32 kDt = 1.0f / 120.0f;
    f32 prevX = NoiseSolver::SampleBoneOffset(params, 1, 1, 0.0f).EulerRadians.x;
    for (int step = 1; step < 1000; ++step)
    {
        const f32 time = static_cast<f32>(step) * kDt;
        const f32 x = NoiseSolver::SampleBoneOffset(params, 1, 1, time).EulerRadians.x;
        // A small time step must yield a small change relative to the amplitude.
        // A genuine discontinuity would jump a large fraction of the amplitude;
        // smooth fbm advances only a few percent per 1/120 s step.
        EXPECT_LT(std::abs(x - prevX), 0.4f * params.RotationAmplitude.x + 1e-4f)
            << "Discontinuity at step " << step;
        prevX = x;
    }
}

// Non-finite parameters must produce a zero offset rather than poisoning the
// skeleton with NaN/Inf.
TEST(NoiseSolverTest, NonFiniteParamsProduceZeroOffset)
{
    const f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
    const f32 kInf = std::numeric_limits<f32>::infinity();

    auto badFreq = DefaultParams();
    badFreq.Frequency = kNaN;
    auto badAmp = DefaultParams();
    badAmp.RotationAmplitude.y = kInf;
    auto badGain = DefaultParams();
    badGain.Gain = kNaN;

    for (const auto& bad : { badFreq, badAmp, badGain })
    {
        const NoiseBoneOffset o = NoiseSolver::SampleBoneOffset(bad, 0, 0, 1.0f);
        EXPECT_TRUE(Math::BitwiseEqual(o.EulerRadians, glm::vec3(0.0f)));
        EXPECT_TRUE(Math::BitwiseEqual(o.Translation, glm::vec3(0.0f)));
    }

    // Non-finite time too.
    const NoiseBoneOffset oTime = NoiseSolver::SampleBoneOffset(DefaultParams(), 0, 0, kNaN);
    EXPECT_TRUE(Math::BitwiseEqual(oTime.EulerRadians, glm::vec3(0.0f)));
}

// Apply must modify only the chain bones and leave bones outside the chain
// untouched.
TEST(NoiseSolverTest, ApplyTouchesOnlyChainBones)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    auto original = pose;

    auto params = DefaultParams();
    params.EndBoneIndex = 3;
    params.ChainLength = 2; // bones 3 and 2 only
    NoiseSolver::Apply(pose, parents, params, 0.5f);

    // Bones 0 and 1 are outside the chain — untouched.
    EXPECT_TRUE(Math::BitwiseEqual(pose[0].Rotation, original[0].Rotation));
    EXPECT_TRUE(Math::BitwiseEqual(pose[1].Rotation, original[1].Rotation));
    // Bones 2 and 3 are in the chain — moved (with non-zero amplitude/time).
    const bool moved2 = !Math::BitwiseEqual(pose[2].Rotation, original[2].Rotation);
    const bool moved3 = !Math::BitwiseEqual(pose[3].Rotation, original[3].Rotation);
    EXPECT_TRUE(moved2 || moved3) << "Chain bones should have been displaced by the noise";
    for (const auto& bt : pose)
    {
        EXPECT_TRUE(std::isfinite(bt.Rotation.w) && IsFiniteVec3(bt.Translation));
    }
}

// Out-of-range end bone / zero chain length / zero weight must all be no-ops.
TEST(NoiseSolverTest, DegenerateConfigsAreNoOps)
{
    std::vector<BoneTransform> pose;
    std::vector<int> parents;
    BuildChain(pose, parents);
    const auto original = pose;

    auto outOfRange = DefaultParams();
    outOfRange.EndBoneIndex = 99;
    auto zeroChain = DefaultParams();
    zeroChain.ChainLength = 0;
    auto zeroWeight = DefaultParams();
    zeroWeight.Weight = 0.0f;
    auto nanTime = DefaultParams();

    NoiseSolver::Apply(pose, parents, outOfRange, 0.5f);
    NoiseSolver::Apply(pose, parents, zeroChain, 0.5f);
    NoiseSolver::Apply(pose, parents, zeroWeight, 0.5f);
    NoiseSolver::Apply(pose, parents, nanTime, std::numeric_limits<f32>::quiet_NaN());

    for (sizet i = 0; i < pose.size(); ++i)
    {
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Rotation, original[i].Rotation)) << "bone " << i;
        EXPECT_TRUE(Math::BitwiseEqual(pose[i].Translation, original[i].Translation)) << "bone " << i;
    }
}

// -----------------------------------------------------------------------------
// Post-pass tests (still unit-level — a hand-built Skeleton, no Scene).
// -----------------------------------------------------------------------------

namespace
{
    Ref<Skeleton> MakeChainSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(4);
        skeleton->m_BoneNames = { "Root", "Seg1", "Seg2", "Tip" };
        skeleton->m_ParentIndices = { -1, 0, 1, 2 };
        const auto offset = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        skeleton->m_LocalTransforms = { glm::mat4(1.0f), offset, offset, offset };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        return skeleton;
    }

    NoiseAnimationComponent DefaultComponent()
    {
        NoiseAnimationComponent c;
        c.Enabled = true;
        c.EndBoneIndex = 3;
        c.ChainLength = 3;
        c.Frequency = 1.5f;
        c.RotationAmplitude = glm::vec3(0.15f);
        c.TranslationAmplitude = glm::vec3(0.0f);
        c.Octaves = 2;
        c.Seed = 3;
        c.Weight = 1.0f;
        return c;
    }
} // namespace

// The post-pass actually displaces the chain when run on a fresh pose.
TEST(NoiseSolverTest, PostPassDisplacesChain)
{
    auto skeleton = MakeChainSkeleton();
    const auto basePose = skeleton->m_LocalTransforms;
    const auto comp = DefaultComponent();
    NoiseAnimationState state;

    ApplyNoisePostPass(*skeleton, comp, state, 0.5f);

    bool anyMoved = false;
    for (sizet i = 0; i < basePose.size(); ++i)
    {
        if (skeleton->m_LocalTransforms[i] != basePose[i])
        {
            anyMoved = true;
        }
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                EXPECT_TRUE(std::isfinite(skeleton->m_LocalTransforms[i][c][r]));
            }
        }
    }
    EXPECT_TRUE(anyMoved) << "Noise post-pass left the whole chain at rest";
}

// Frame-rate independence: the pose at a given accumulated time is the same
// whether reached in one big step or many small ones. The Scene resets the
// skeleton to the sampled animation pose each frame, so the test resets the
// base pose before every post-pass call (as the real pipeline does).
TEST(NoiseSolverTest, PostPassIsFrameRateIndependent)
{
    const auto comp = DefaultComponent();

    auto skeletonCoarse = MakeChainSkeleton();
    const auto basePose = skeletonCoarse->m_LocalTransforms;
    NoiseAnimationState coarseState;
    skeletonCoarse->m_LocalTransforms = basePose;
    ApplyNoisePostPass(*skeletonCoarse, comp, coarseState, 0.2f); // one 0.2s step

    auto skeletonFine = MakeChainSkeleton();
    NoiseAnimationState fineState;
    for (int i = 0; i < 4; ++i)
    {
        skeletonFine->m_LocalTransforms = basePose; // Scene re-samples the pose each frame
        ApplyNoisePostPass(*skeletonFine, comp, fineState, 0.05f); // four 0.05s steps
    }

    ASSERT_NEAR(coarseState.Time, fineState.Time, 1e-4f);
    for (sizet i = 0; i < basePose.size(); ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                EXPECT_NEAR(skeletonCoarse->m_LocalTransforms[i][c][r],
                            skeletonFine->m_LocalTransforms[i][c][r], 1e-4f)
                    << "Frame-rate dependence at bone " << i << " element [" << c << "][" << r << "]";
            }
        }
    }
}

// A disabled component is a strict passthrough.
TEST(NoiseSolverTest, PostPassDisabledIsPassthrough)
{
    auto skeleton = MakeChainSkeleton();
    const auto basePose = skeleton->m_LocalTransforms;
    auto comp = DefaultComponent();
    comp.Enabled = false;
    NoiseAnimationState state;

    ApplyNoisePostPass(*skeleton, comp, state, 0.5f);

    for (sizet i = 0; i < basePose.size(); ++i)
    {
        EXPECT_EQ(skeleton->m_LocalTransforms[i], basePose[i]) << "bone " << i;
    }
}
