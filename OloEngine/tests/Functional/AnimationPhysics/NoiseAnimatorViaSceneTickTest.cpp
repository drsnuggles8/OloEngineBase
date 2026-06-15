#include "OloEnginePCH.h"

// =============================================================================
// NoiseAnimatorViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AnimationSystem × procedural noise post-pass × runtime state
//   lifecycle. The unit-level NoiseSolverTest pins the noise math; this test
//   pins the wiring only visible under a real Scene tick:
//     - Scene::ResolveNoiseAnimation creates the runtime
//       NoiseAnimationStateComponent on demand (the user never adds it).
//     - The post-pass actually runs inside AnimationSystem::Update and adds
//       bounded, organic motion to the chain.
//     - A disabled component is a passthrough: pose identical to a pure
//       animation tick, and no state component gets created.
//     - Removing the component drops the cached runtime state.
//
// Scenario: a 4-bone chain skeleton playing a (looping) clip so the animation
// path runs. A NoiseAnimationComponent covers the chain below the root. After
// ticking, the chain bones must wobble (non-identity local rotations) while
// staying finite and bounded by the configured amplitude.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // 4-bone chain along +Y: Root -> Seg1 -> Seg2 -> Tip, one unit apart.
    Ref<Skeleton> MakeChainSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(4);
        skeleton->m_BoneNames = { "Root", "Seg1", "Seg2", "Tip" };
        skeleton->m_ParentIndices = { -1, 0, 1, 2 };
        const auto offset = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        skeleton->m_LocalTransforms = { glm::mat4(1.0f), offset, offset, offset };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        for (sizet i = 0; i < 4; ++i)
        {
            const auto parent = skeleton->m_ParentIndices[i];
            skeleton->m_GlobalTransforms[i] = (parent >= 0)
                                                  ? skeleton->m_GlobalTransforms[parent] * skeleton->m_LocalTransforms[i]
                                                  : skeleton->m_LocalTransforms[i];
        }
        skeleton->SetBindPose();
        return skeleton;
    }

    // Minimal clip that keeps the animation path "playing" without itself
    // touching the chain bones, so any chain motion is the noise pass.
    Ref<AnimationClip> MakeIdleClip(f32 duration)
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "Functional_NoiseIdleClip";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Root";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
        boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(std::move(boneAnim));
        clip->InitializeBoneCache();
        return clip;
    }

    bool IsFiniteMat4(const glm::mat4& m)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (!std::isfinite(m[c][r]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Largest local-rotation deflection (radians) across the chain bones — zero
    // when the noise pass never fired.
    f32 MaxChainDeflection(const Skeleton& skeleton)
    {
        f32 maxAngle = 0.0f;
        for (sizet i = 1; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            const auto rotation = glm::normalize(glm::quat_cast(glm::mat3(skeleton.m_LocalTransforms[i])));
            maxAngle = std::max(maxAngle, glm::angle(rotation));
        }
        return maxAngle;
    }
} // namespace

class NoiseAnimatorViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("NoiseAnimated");
        m_Animated.AddComponent<SkeletonComponent>(MakeChainSkeleton());

        m_Clip = MakeIdleClip(/*duration=*/1.0f);
        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = m_Clip;
        animState.m_IsPlaying = true;
        animState.m_CurrentTime = 0.0f;

        auto& noise = m_Animated.AddComponent<NoiseAnimationComponent>();
        noise.Enabled = true;
        noise.EndBoneIndex = 3; // Tip
        noise.ChainLength = 3;   // Seg1 -> Seg2 -> Tip
        noise.Frequency = 2.0f;
        noise.RotationAmplitude = glm::vec3(0.2f);
        noise.TranslationAmplitude = glm::vec3(0.0f);
        noise.Octaves = 2;
        noise.Seed = 11;
        noise.Weight = 1.0f;
    }

    Entity m_Animated;
    Ref<AnimationClip> m_Clip;
};

TEST_F(NoiseAnimatorViaSceneTickTest, ChainWobblesAndStateComponentIsCreated)
{
    ASSERT_FALSE(m_Animated.HasComponent<NoiseAnimationStateComponent>())
        << "Runtime state must not exist before the first tick";

    const auto& skeleton = *m_Animated.GetComponent<SkeletonComponent>().m_Skeleton;

    // Noise crosses zero, so wait until the chain has visibly deflected rather
    // than sampling a single (possibly near-zero) frame.
    const bool deflected = TickUntil(
        [&]() { return MaxChainDeflection(skeleton) > 0.01f; },
        /*timeoutSeconds=*/2.0f);
    EXPECT_TRUE(deflected)
        << "Noise chain never deflected — the noise post-pass never fired through the Scene tick";

    ASSERT_TRUE(m_Animated.HasComponent<NoiseAnimationStateComponent>())
        << "Scene tick must create the runtime state component on demand";

    // The offset must stay bounded by the amplitude (0.2 rad), with a small
    // margin for the chained per-bone composition.
    EXPECT_LT(MaxChainDeflection(skeleton), 0.6f)
        << "Noise deflection must stay bounded by the configured amplitude";

    for (sizet i = 0; i < skeleton.m_FinalBoneMatrices.size(); ++i)
    {
        EXPECT_TRUE(IsFiniteMat4(skeleton.m_FinalBoneMatrices[i]))
            << "Non-finite bone matrix at " << i;
    }

    const auto& state = m_Animated.GetComponent<NoiseAnimationStateComponent>().State;
    EXPECT_GT(state.Time, 0.0f) << "Noise phase accumulator should have advanced";
}

TEST_F(NoiseAnimatorViaSceneTickTest, DisabledComponentIsPassthroughAndCreatesNoState)
{
    m_Animated.GetComponent<NoiseAnimationComponent>().Enabled = false;

    TickFor(/*seconds=*/0.5f);

    EXPECT_FALSE(m_Animated.HasComponent<NoiseAnimationStateComponent>())
        << "Disabled noise animator must not allocate runtime state";

    const auto& skeleton = *m_Animated.GetComponent<SkeletonComponent>().m_Skeleton;
    EXPECT_LT(MaxChainDeflection(skeleton), 1e-4f)
        << "Disabled noise animator must leave the animated pose untouched";
}

TEST_F(NoiseAnimatorViaSceneTickTest, RemovingComponentDropsRuntimeState)
{
    TickFor(/*seconds=*/0.2f);
    ASSERT_TRUE(m_Animated.HasComponent<NoiseAnimationStateComponent>());

    m_Animated.RemoveComponent<NoiseAnimationComponent>();

    EXPECT_FALSE(m_Animated.HasComponent<NoiseAnimationStateComponent>())
        << "Removing NoiseAnimationComponent must drop the cached phase accumulator "
           "so a re-added component restarts its noise phase from zero";
}
