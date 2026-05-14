#include "OloEnginePCH.h"

// =============================================================================
// AnimationContinuesAfterPauseResumeTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene pause × AnimationStateComponent.m_CurrentTime preservation. The
//   `ScenePauseFreezesAllSubsystems` test already verifies the pause flag
//   freezes animation. The complementary contract: when unpaused, the
//   animation must resume from EXACTLY where it left off, not restart
//   from time=0 and not silently drift forward by the pause duration.
//   A regression that resets m_CurrentTime on resume looks like "every
//   pause/unpause restarts the character animation," and a regression
//   that advances m_CurrentTime during pause manifests as "characters
//   teleport in their animations after a pause." Both are very visible
//   in production.
//
// Scenario: drive a clip for ~0.25s, pause, idle for several frames,
// unpause, drive for another 0.25s. Final m_CurrentTime should equal
// pre-pause + post-resume duration (within fixed-step precision) — NOT
// pre-pause alone (failed resume), NOT 2× the pause+post-resume
// duration (clock-leaked through pause).
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class AnimationContinuesAfterPauseResumeTest : public FunctionalTest
{
  protected:
    static constexpr f32 kClipDuration = 10.0f; // long so we don't wrap during the test

    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        auto& anim = m_Animated.AddComponent<AnimationStateComponent>();
        anim.m_CurrentClip = Fixtures::MakeTranslationClip(/*duration=*/kClipDuration);
        anim.m_IsPlaying = true;
        anim.m_CurrentTime = 0.0f;
    }

    [[nodiscard]] f32 ClipTime() const
    {
        return m_Animated.GetComponent<AnimationStateComponent>().m_CurrentTime;
    }

    Entity m_Animated;
};

TEST_F(AnimationContinuesAfterPauseResumeTest, ClipTimeResumesFromPauseSnapshot)
{
    constexpr u32 kPrePauseFrames  = 15; // ~0.25s
    constexpr u32 kPausedFrames    = 30; // ~0.5s of "wall time" — must NOT advance the clip
    constexpr u32 kPostResumeFrames = 15; // ~0.25s

    // Phase 1: tick before pause. Snapshot the clip time.
    RunFrames(kPrePauseFrames);
    const f32 prePauseTime = ClipTime();
    ASSERT_GT(prePauseTime, 0.0f) << "animation didn't tick at all before pause";

    // Phase 2: pause. Tick — clip time MUST NOT change.
    GetScene().SetPaused(true);
    RunFrames(kPausedFrames);
    const f32 pausedTime = ClipTime();
    EXPECT_NEAR(pausedTime, prePauseTime, 1e-5f)
        << "animation advanced during pause; pre=" << prePauseTime
        << " mid-pause=" << pausedTime;

    // Phase 3: resume. Tick. Clip time should advance from the snapshot
    // by ~kPostResumeFrames * dt, NOT restart from 0 and NOT include the
    // paused duration.
    GetScene().SetPaused(false);
    RunFrames(kPostResumeFrames);
    const f32 postResumeTime = ClipTime();

    constexpr f32 dt = 1.0f / 60.0f;
    const f32 expectedDelta = kPostResumeFrames * dt;
    const f32 expectedTime  = prePauseTime + expectedDelta;

    EXPECT_NEAR(postResumeTime, expectedTime, dt * 0.6f) // tolerate <1 frame
        << "animation did not resume from the pause-snapshot; pre=" << prePauseTime
        << " expected=" << expectedTime << " got=" << postResumeTime
        << " (clock leaked through pause if too high, restarted from 0 if too low)";

    EXPECT_GT(postResumeTime, prePauseTime + 1e-3f)
        << "animation did not advance after resume — pause flag stayed sticky";
}
