#include "OloEnginePCH.h"

// =============================================================================
// AnimationBlendCompletesViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AnimationSystem × clip blending. The unit-level
//   AnimationSystemTest exercises bone interpolation but not the
//   blend-completion handoff: when m_BlendTime exceeds m_BlendDuration,
//   AnimationSystem swaps m_CurrentClip to m_NextClip and clears the
//   blend flags. A regression there is "character starts a new animation
//   but never finishes the transition, leaving them stuck in the blend
//   state" — only visible when the system runs under a real Scene tick
//   over multiple frames.
//
// Scenario: an entity carries two clips. We start a blend from A→B with
// a 0.3s duration. After ticking 0.5s through Scene::OnUpdateRuntime, we
// expect m_Blending == false and m_CurrentClip == B.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AnimationBlendCompletesViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        m_ClipA = Fixtures::MakeTranslationClip(/*duration=*/1.0f);
        m_ClipA->Name = "ClipA";

        m_ClipB = Fixtures::MakeTranslationClip(/*duration=*/1.0f, /*boneName=*/"Root",
                                                /*fromPos=*/glm::vec3(1.0f, 0.0f, 0.0f),
                                                /*toPos=*/glm::vec3(2.0f, 0.0f, 0.0f));
        m_ClipB->Name = "ClipB";

        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = m_ClipA;
        animState.m_NextClip = m_ClipB;
        animState.m_IsPlaying = true;
        animState.m_Blending = true;
        animState.m_BlendDuration = kBlendDuration;
        animState.m_BlendTime = 0.0f;
        animState.m_CurrentTime = 0.0f;
        animState.m_NextTime = 0.0f;
    }

    static constexpr f32 kBlendDuration = 0.3f;
    Entity m_Animated;
    Ref<AnimationClip> m_ClipA;
    Ref<AnimationClip> m_ClipB;
};

TEST_F(AnimationBlendCompletesViaSceneTickTest, BlendCompletesAfterDurationAndCurrentClipSwapsToNext)
{
    // Sanity: start state is mid-blend.
    {
        const auto& s = m_Animated.GetComponent<AnimationStateComponent>();
        ASSERT_TRUE(s.m_Blending);
        ASSERT_EQ(s.m_CurrentClip, m_ClipA);
        ASSERT_EQ(s.m_NextClip, m_ClipB);
    }

    // Tick well past blend duration.
    TickFor(/*seconds=*/0.5f);

    const auto& s = m_Animated.GetComponent<AnimationStateComponent>();
    EXPECT_FALSE(s.m_Blending)
        << "blend did not complete after " << 0.5f << "s of ticking with duration "
        << kBlendDuration << " — AnimationSystem's blend-completion branch never fired";

    EXPECT_EQ(s.m_CurrentClip, m_ClipB)
        << "blend completed but m_CurrentClip was not swapped to m_NextClip";

    // After completion, m_NextClip should be cleared so the system doesn't
    // re-enter the blend on the next frame.
    EXPECT_FALSE(s.m_NextClip)
        << "m_NextClip was not cleared after blend completion — AnimationSystem will think we're still blending";
}
