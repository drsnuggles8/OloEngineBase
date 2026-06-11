#include "OloEnginePCH.h"

// =============================================================================
// SpringBoneJiggleViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AnimationSystem × spring-bone post-pass × runtime state
//   lifecycle. The unit-level SpringBoneSolverTest pins the simulation math;
//   this test pins the wiring only visible under a real Scene tick:
//     - Scene::ResolveSpringBone creates the runtime SpringBoneStateComponent
//       on demand (the user never adds it by hand).
//     - The post-pass actually runs inside AnimationSystem::Update and
//       deflects the chain when the animated root moves.
//     - A disabled component is a passthrough: pose identical to a pure
//       animation tick, and no state component gets created.
//
// Scenario: a 4-bone chain skeleton whose root is animated side-to-side by a
// translation clip. A SpringBoneComponent covers the chain below the root.
// After ticking, the simulated bones must lag the animated pose (non-identity
// local rotations on the chain) while staying finite.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/SpringBoneComponent.h"

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

    // Clip that swings the root bone side-to-side so the chain has motion to lag.
    Ref<AnimationClip> MakeRootSwingClip(f32 duration)
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "Functional_RootSwingClip";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Root";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
        boneAnim.PositionKeys.push_back({ static_cast<f64>(duration) * 0.5, glm::vec3(2.0f, 0.0f, 0.0f) });
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

    // Largest local-rotation deflection (radians) across the simulated chain
    // bones — zero when the spring pass never fired.
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

class SpringBoneJiggleViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("SpringBoneAnimated");
        m_Animated.AddComponent<SkeletonComponent>(MakeChainSkeleton());

        m_Clip = MakeRootSwingClip(/*duration=*/1.0f);
        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = m_Clip;
        animState.m_IsPlaying = true;
        animState.m_CurrentTime = 0.0f;

        auto& spring = m_Animated.AddComponent<SpringBoneComponent>();
        spring.Enabled = true;
        spring.EndBoneIndex = 3; // Tip
        spring.ChainLength = 3;  // Seg1 -> Seg2 -> Tip
        spring.Stiffness = 20.0f;
        spring.Damping = 4.0f;
        spring.Gravity = glm::vec3(0.0f);
        spring.Weight = 1.0f;
    }

    Entity m_Animated;
    Ref<AnimationClip> m_Clip;
};

TEST_F(SpringBoneJiggleViaSceneTickTest, ChainLagsAnimatedRootAndStateComponentIsCreated)
{
    ASSERT_FALSE(m_Animated.HasComponent<SpringBoneStateComponent>())
        << "Runtime state must not exist before the first tick";

    // Half a swing: the root has moved fast sideways, the chain must lag.
    TickFor(/*seconds=*/0.4f);

    ASSERT_TRUE(m_Animated.HasComponent<SpringBoneStateComponent>())
        << "Scene tick must create the runtime state component on demand";

    const auto& skeleton = *m_Animated.GetComponent<SkeletonComponent>().m_Skeleton;
    EXPECT_GT(MaxChainDeflection(skeleton), 0.01f)
        << "Spring chain should deflect while the animated root is moving — "
           "the spring-bone post-pass never fired through the Scene tick";

    for (sizet i = 0; i < skeleton.m_FinalBoneMatrices.size(); ++i)
    {
        EXPECT_TRUE(IsFiniteMat4(skeleton.m_FinalBoneMatrices[i]))
            << "Non-finite bone matrix at " << i;
    }

    const auto& state = m_Animated.GetComponent<SpringBoneStateComponent>().State;
    EXPECT_TRUE(state.Initialized);
    EXPECT_EQ(state.CurrPositions.size(), 2u) << "ChainLength 3 -> 2 simulated joints";
}

TEST_F(SpringBoneJiggleViaSceneTickTest, DisabledComponentIsPassthroughAndCreatesNoState)
{
    m_Animated.GetComponent<SpringBoneComponent>().Enabled = false;

    TickFor(/*seconds=*/0.4f);

    EXPECT_FALSE(m_Animated.HasComponent<SpringBoneStateComponent>())
        << "Disabled spring bones must not allocate runtime state";

    const auto& skeleton = *m_Animated.GetComponent<SkeletonComponent>().m_Skeleton;
    EXPECT_LT(MaxChainDeflection(skeleton), 1e-4f)
        << "Disabled spring bones must leave the animated pose untouched";
}

TEST_F(SpringBoneJiggleViaSceneTickTest, RemovingComponentDropsRuntimeState)
{
    TickFor(/*seconds=*/0.2f);
    ASSERT_TRUE(m_Animated.HasComponent<SpringBoneStateComponent>());

    m_Animated.RemoveComponent<SpringBoneComponent>();

    EXPECT_FALSE(m_Animated.HasComponent<SpringBoneStateComponent>())
        << "Removing SpringBoneComponent must drop the cached simulation state "
           "so a re-added component starts fresh from the animated pose";
}
