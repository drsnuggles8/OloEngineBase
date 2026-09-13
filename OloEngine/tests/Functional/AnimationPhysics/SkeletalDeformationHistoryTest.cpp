#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// SkeletalDeformationHistoryTest — Functional Test (issue #1226).
//
// Cross-subsystem seam under test:
//   Scene frame boundary × SkeletonData's previous-pose palette. The shared
//   deformation output has two halves — the current pose in
//   m_FinalBoneMatrices and the previous pose in m_PrevFinalBoneMatrices — and
//   every velocity-emitting consumer subtracts one from the other to get a
//   per-pixel motion vector. So the contract these tests pin is not "the
//   history is updated" but "the difference between the two halves is the
//   motion that actually happened".
//
//   The failure this guards against is silent by construction. Before #1226 the
//   history advanced from inside AnimationSystem::Update, which Scene only
//   calls for an entity that is playing. Pause a character and the advance
//   stopped with prev and current one frame apart — so the shaders kept
//   emitting that final frame's bone delta, unchanged, for as long as the pause
//   lasted. Nothing asserts, nothing logs, and the only symptom is that a
//   perfectly still character smears under TAA and motion blur. It is exactly
//   the class of bug that survives three issues and then gets blamed on the
//   temporal filter.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    /// Largest per-component difference between the current and previous pose,
    /// across the whole palette. This is the quantity the skinned shaders turn
    /// into a motion vector: zero here means zero bone motion on screen.
    [[nodiscard]] f32 MaxBoneMotion(const SkeletonData& skeleton)
    {
        const sizet boneCount = skeleton.m_FinalBoneMatrices.size();
        if (boneCount == 0 || skeleton.m_PrevFinalBoneMatrices.size() != boneCount)
            return 0.0f;

        f32 maxDelta = 0.0f;
        for (sizet bone = 0; bone < boneCount; ++bone)
        {
            const glm::mat4& curr = skeleton.m_FinalBoneMatrices[bone];
            const glm::mat4& prev = skeleton.m_PrevFinalBoneMatrices[bone];
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    maxDelta = glm::max(maxDelta, glm::abs(curr[column][row] - prev[column][row]));
                }
            }
        }
        return maxDelta;
    }
} // namespace

class SkeletalDeformationHistoryTest : public FunctionalTest
{
  protected:
    static constexpr f32 kClipDuration = 10.0f; // long enough never to wrap here

    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("DeformingCharacter");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        auto& anim = m_Animated.AddComponent<AnimationStateComponent>();
        // A translation clip gives a bone delta that is large and unambiguous
        // in the palette, so "no motion" and "some motion" cannot be confused
        // for float noise.
        anim.m_CurrentClip = Fixtures::MakeTranslationClip(kClipDuration);
        anim.m_IsPlaying = true;
        anim.m_CurrentTime = 0.0f;
    }

    [[nodiscard]] SkeletonData& Skeleton()
    {
        return *m_Animated.GetComponent<SkeletonComponent>().m_Skeleton;
    }

    Entity m_Animated;
};

TEST_F(SkeletalDeformationHistoryTest, AnimatingProducesNonZeroBoneMotion)
{
    RunFrames(15);

    EXPECT_EQ(Skeleton().m_PrevFinalBoneMatrices.size(), Skeleton().m_FinalBoneMatrices.size())
        << "previous-pose palette is not sized to the current pose — every consumer "
           "would fall back to zero bone motion";
    EXPECT_TRUE(Skeleton().HasBoneHistory())
        << "history never became valid while the clip was playing";
    EXPECT_GT(MaxBoneMotion(Skeleton()), 1e-5f)
        << "an animating skeleton produced zero bone motion — velocity is dead, "
           "TAA and motion blur would treat a moving character as static";
}

TEST_F(SkeletalDeformationHistoryTest, PausedSkeletonProducesZeroBoneMotion)
{
    // Drive the clip far enough that the per-frame bone delta is unmistakably
    // non-zero, so the assertion after the pause is about the pause and not
    // about a clip that happened to be still.
    RunFrames(15);
    ASSERT_GT(MaxBoneMotion(Skeleton()), 1e-5f) << "clip did not move the bone before the pause";

    GetScene().SetPaused(true);

    // One frame of pause is enough to expose the bug: the history advance has
    // to run on a frame that runs no gameplay tick at all. Several frames then
    // confirm it stays zero rather than oscillating.
    for (u32 frame = 0; frame < 10; ++frame)
    {
        RunFrames(1);
        EXPECT_NEAR(MaxBoneMotion(Skeleton()), 0.0f, 1e-6f)
            << "paused skeleton still reports bone motion on pause frame " << frame
            << " — the previous pose froze one frame behind the current one, so every "
               "velocity-emitting consumer smears a character that is standing still";
    }
}

TEST_F(SkeletalDeformationHistoryTest, ResumeRestoresMotionWithoutAPauseSizedJump)
{
    RunFrames(15);
    const f32 movingDelta = MaxBoneMotion(Skeleton());
    ASSERT_GT(movingDelta, 1e-5f);

    GetScene().SetPaused(true);
    RunFrames(30);
    ASSERT_NEAR(MaxBoneMotion(Skeleton()), 0.0f, 1e-6f);

    GetScene().SetPaused(false);
    RunFrames(1);

    const f32 resumedDelta = MaxBoneMotion(Skeleton());
    EXPECT_GT(resumedDelta, 1e-5f) << "motion did not come back after resume";
    // The resumed frame must carry ONE frame of motion, not the pause's worth.
    // A history that had frozen across the pause would hand the first resumed
    // frame a delta spanning every paused frame, which reads as a teleport to
    // any temporal consumer.
    EXPECT_LT(resumedDelta, movingDelta * 3.0f)
        << "first resumed frame carried " << resumedDelta << " against a steady-state "
        << movingDelta << " — the pause leaked into the motion vector";
}

TEST_F(SkeletalDeformationHistoryTest, ExplicitResetDropsHistoryAndZeroesMotion)
{
    RunFrames(15);
    ASSERT_GT(MaxBoneMotion(Skeleton()), 1e-5f);

    const u32 resetCount = Animation::SkeletalDeformationSystem::ResetHistory(
        &GetScene(), Animation::DeformationHistoryResetCause::Teleport);

    EXPECT_EQ(resetCount, 1u) << "the scene's one skinned entity was not reset";
    EXPECT_FALSE(Skeleton().HasBoneHistory())
        << "history still claims to hold a genuine previous pose after an explicit reset";
    EXPECT_NEAR(MaxBoneMotion(Skeleton()), 0.0f, 1e-6f)
        << "a reset must leave prev == current so the next frame emits zero motion "
           "rather than a velocity measured across the discontinuity";
    EXPECT_EQ(Animation::SkeletalDeformationSystem::GetStats().LastResetCause,
              Animation::DeformationHistoryResetCause::Teleport)
        << "the reset was not attributed — an unattributed history drop is exactly the "
           "silent failure this system exists to make countable";
}

TEST_F(SkeletalDeformationHistoryTest, BoneCountChangeDropsHistoryLoudly)
{
    RunFrames(15);
    ASSERT_TRUE(Skeleton().HasBoneHistory());

    // Stand in for a skeleton swap: the palette grows under the entity, so the
    // previous pose describes a skeleton that no longer exists. Holding it
    // would produce a motion vector between two unrelated rigs.
    Skeleton().m_FinalBoneMatrices.push_back(glm::mat4(1.0f));

    RunFrames(1);

    EXPECT_GT(Animation::SkeletalDeformationSystem::GetStats().HistoryResets, 0u)
        << "the bone-count change was not counted as a history drop — an unattributed "
           "resize is indistinguishable from a genuine previous pose downstream";

    // Whatever size the animation systems settle the palette at, the two halves
    // must agree again: every consumer indexes them together, per bone.
    RunFrames(2);
    EXPECT_EQ(Skeleton().m_PrevFinalBoneMatrices.size(), Skeleton().m_FinalBoneMatrices.size())
        << "previous palette never resynchronised with the current pose";
}

TEST_F(SkeletalDeformationHistoryTest, EverySkinnedEntityIsAdvancedNotJustPlayingOnes)
{
    // A second skinned entity that never plays anything. The old
    // advance-inside-the-animation-update placement skipped it entirely, which
    // is the same defect the pause test exercises, reached from the other side.
    Entity idle = GetScene().CreateEntity("IdleCharacter");
    idle.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

    RunFrames(5);

    const auto& stats = Animation::SkeletalDeformationSystem::GetStats();
    EXPECT_EQ(stats.SkeletonsAdvanced, 2u)
        << "the non-playing skinned entity was not advanced; a skeleton that is never "
           "advanced keeps whatever previous pose it was created with";

    SkeletonData& idleSkeleton = *idle.GetComponent<SkeletonComponent>().m_Skeleton;
    EXPECT_NEAR(MaxBoneMotion(idleSkeleton), 0.0f, 1e-6f)
        << "an entity that never animated reported bone motion";
}
