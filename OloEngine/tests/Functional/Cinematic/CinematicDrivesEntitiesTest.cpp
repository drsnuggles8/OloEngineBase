#include "OloEnginePCH.h"

// =============================================================================
// CinematicDrivesEntitiesTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Cinematic × Scene::OnUpdateRuntime × Transform/Camera/Model components.
//   A CinematicComponent with an in-memory sequence is driven through real
//   runtime ticks; the test asserts the sequence actually poses the targeted
//   entities (translation, camera FOV, visibility), fires its timed event, and
//   transitions to a finished/looping state correctly. This is the contract a
//   cutscene relies on — math-level curve tests can't prove the system wires
//   the playhead to the scene.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Core/Ref.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kFovStart = glm::radians(45.0f);
    constexpr f32 kFovEnd = glm::radians(60.0f);
} // namespace

class CinematicDrivesEntitiesTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Hero = GetScene().CreateEntity("Hero");
        m_Cam = GetScene().CreateEntity("Cam");
        m_Cam.AddComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(kFovStart);
        m_Prop = GetScene().CreateEntity("Prop");
        m_Prop.AddComponent<ModelComponent>().m_Visible = true;

        // Build a 1-second sequence that drives all three entities.
        auto seq = Ref<CinematicSequence>::Create();
        seq->Name = "FunctionalCutscene";
        seq->Duration = 1.0f;

        CinematicTransformTrack tt;
        tt.Target = m_Hero.GetUUID();
        tt.Translation.Keys.push_back({ 0.0f, glm::vec3(0.0f), CinematicInterp::Linear });
        tt.Translation.Keys.push_back({ 1.0f, glm::vec3(0.0f, 10.0f, 0.0f), CinematicInterp::Linear });
        seq->TransformTracks.push_back(std::move(tt));

        CinematicCameraTrack ct;
        ct.Target = m_Cam.GetUUID();
        ct.VerticalFovRadians.Keys.push_back({ 0.0f, kFovStart, CinematicInterp::Linear });
        ct.VerticalFovRadians.Keys.push_back({ 1.0f, kFovEnd, CinematicInterp::Linear });
        seq->CameraTracks.push_back(std::move(ct));

        CinematicVisibilityTrack vt;
        vt.Target = m_Prop.GetUUID();
        vt.Keys.push_back({ 0.0f, false });
        vt.Keys.push_back({ 0.5f, true });
        seq->VisibilityTracks.push_back(std::move(vt));

        CinematicEventTrack et;
        et.Name = "cues";
        et.Keys.push_back({ 0.5f, "boom" });
        seq->EventTracks.push_back(std::move(et));

        m_Director = GetScene().CreateEntity("Director");
        auto& cine = m_Director.AddComponent<CinematicComponent>();
        cine.RuntimeSequence = seq; // in-memory sequence; no asset file needed
        cine.PlayFromStart();
    }

    // Run frame-by-frame so per-frame event firing can be observed (the
    // component clears EventsFiredThisFrame every tick).
    bool RunAndWatchForEvent(u32 frames, const std::string& eventName, f32 dt = 1.0f / 60.0f)
    {
        bool seen = false;
        for (u32 i = 0; i < frames; ++i)
        {
            RunFrames(1, dt);
            const auto& cine = m_Director.GetComponent<CinematicComponent>();
            for (const auto& e : cine.EventsFiredThisFrame)
            {
                if (e == eventName)
                {
                    seen = true;
                }
            }
        }
        return seen;
    }

    Entity m_Hero, m_Cam, m_Prop, m_Director;
};

TEST_F(CinematicDrivesEntitiesTest, PosesEntitiesFiresEventAndFinishes)
{
    // The visibility track flips Prop hidden at t=0 immediately on the first
    // applied frame.
    RunFrames(1);
    EXPECT_FALSE(m_Prop.GetComponent<ModelComponent>().m_Visible)
        << "VisibilityTrack should hide Prop at t=0";

    // Drive ~0.9s worth of frames while watching for the timed event at t=0.5.
    const bool boomFired = RunAndWatchForEvent(53, "boom");
    EXPECT_TRUE(boomFired) << "EventTrack 'boom' at t=0.5 never fired";

    // Prop became visible once the playhead passed t=0.5.
    EXPECT_TRUE(m_Prop.GetComponent<ModelComponent>().m_Visible)
        << "VisibilityTrack should reveal Prop after t=0.5";

    // Finish the sequence (push the playhead past the 1s duration).
    RunFrames(20);

    const auto& cine = m_Director.GetComponent<CinematicComponent>();
    EXPECT_TRUE(cine.Finished) << "non-looping sequence should be finished after its duration";
    EXPECT_FALSE(cine.Playing) << "finished sequence should stop playing";

    // Hero ended at the final translation key.
    const glm::vec3 heroPos = m_Hero.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(heroPos.x, 0.0f, 1e-3f);
    EXPECT_NEAR(heroPos.y, 10.0f, 1e-3f);
    EXPECT_NEAR(heroPos.z, 0.0f, 1e-3f);

    // Camera FOV ramped to its final key value.
    const f32 fov = m_Cam.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV();
    EXPECT_NEAR(fov, kFovEnd, 1e-3f);
}

TEST_F(CinematicDrivesEntitiesTest, MidwayTranslationIsInterpolated)
{
    // ~0.5s in, Hero should be roughly halfway up (linear 0 -> 10 over 1s).
    TickFor(0.5f);
    const glm::vec3 heroPos = m_Hero.GetComponent<TransformComponent>().Translation;
    EXPECT_GT(heroPos.y, 3.0f) << "Hero should have moved partway by t~=0.5";
    EXPECT_LT(heroPos.y, 7.0f) << "Hero should not yet be at the end by t~=0.5";

    const auto& cine = m_Director.GetComponent<CinematicComponent>();
    EXPECT_TRUE(cine.Playing) << "sequence should still be playing mid-timeline";
    EXPECT_FALSE(cine.Finished);
}

class CinematicLoopTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Target = GetScene().CreateEntity("LoopTarget");

        auto seq = Ref<CinematicSequence>::Create();
        seq->Duration = 0.5f;
        CinematicEventTrack et;
        et.Name = "tick";
        et.Keys.push_back({ 0.0f, "loopstart" });
        seq->EventTracks.push_back(std::move(et));

        m_Director = GetScene().CreateEntity("LoopDirector");
        auto& cine = m_Director.AddComponent<CinematicComponent>();
        cine.RuntimeSequence = seq;
        cine.Loop = true;
        cine.PlayFromStart();
    }

    Entity m_Target, m_Director;
};

TEST_F(CinematicLoopTest, LoopingSequenceNeverFinishesAndRefiresZeroEvent)
{
    u32 loopStartCount = 0;
    for (u32 i = 0; i < 120; ++i) // 2 seconds at 60fps over a 0.5s loop -> ~4 laps
    {
        RunFrames(1);
        const auto& cine = m_Director.GetComponent<CinematicComponent>();
        for (const auto& e : cine.EventsFiredThisFrame)
        {
            if (e == "loopstart")
            {
                ++loopStartCount;
            }
        }
    }

    const auto& cine = m_Director.GetComponent<CinematicComponent>();
    EXPECT_TRUE(cine.Playing) << "looping sequence should keep playing";
    EXPECT_FALSE(cine.Finished) << "looping sequence should never report finished";
    // t==0 event fires on the first tick and again on each wrap; expect several.
    EXPECT_GE(loopStartCount, 3u) << "t==0 event should re-fire on each loop";
}

// Reverse playback: a negative PlaybackSpeed drives the playhead from the end
// back to 0 through the real runtime, poses entities backward, fires events in
// descending order, and finishes at 0 (the mirror of the forward case above).
class CinematicReversePlaybackTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Hero = GetScene().CreateEntity("Hero");

        auto seq = Ref<CinematicSequence>::Create();
        seq->Name = "ReverseCutscene";
        seq->Duration = 1.0f;

        CinematicTransformTrack tt;
        tt.Target = m_Hero.GetUUID();
        tt.Translation.Keys.push_back({ 0.0f, glm::vec3(0.0f), CinematicInterp::Linear });
        tt.Translation.Keys.push_back({ 1.0f, glm::vec3(0.0f, 10.0f, 0.0f), CinematicInterp::Linear });
        seq->TransformTracks.push_back(std::move(tt));

        CinematicEventTrack et;
        et.Name = "cues";
        et.Keys.push_back({ 0.25f, "low" });
        et.Keys.push_back({ 0.75f, "high" });
        seq->EventTracks.push_back(std::move(et));

        m_Director = GetScene().CreateEntity("Director");
        auto& cine = m_Director.AddComponent<CinematicComponent>();
        cine.RuntimeSequence = seq;
        // Reverse playback through the *normal* start path. PlayFromStart()
        // rewinds to Time=0 (the forward start), and CinematicSystem::Advance
        // seeds the playhead to the end for a negative speed so the sequence
        // actually plays backward instead of finishing instantly at 0. This is
        // the exact path PlayOnStart / the editor Play button / a Lua
        // PlayFromStart() all take — so the test proves the seed, not a manual
        // Time=duration workaround.
        cine.PlaybackSpeed = -1.0f;
        cine.PlayFromStart();
    }

    Entity m_Hero, m_Director;
};

TEST_F(CinematicReversePlaybackTest, PlaysBackwardFiresDescendingEventsAndFinishesAtZero)
{
    // First frame: the seed must move the playhead to (near) the end and keep
    // playing — the bug this guards is a reverse PlayFromStart() finishing
    // instantly at 0 having played nothing.
    RunFrames(1);
    {
        const auto& cine = m_Director.GetComponent<CinematicComponent>();
        EXPECT_TRUE(cine.Playing) << "reverse sequence must not finish on its first step";
        EXPECT_FALSE(cine.Finished);
        EXPECT_GT(cine.Time, 0.5f) << "playhead should be seeded near the end, not stuck at 0";
        const f32 heroY = m_Hero.GetComponent<TransformComponent>().Translation.y;
        EXPECT_GT(heroY, 5.0f) << "Hero should start posed near the end key (y=10), not the start (y=0)";
    }

    // Capture the event firing order across the rest of the backward run.
    std::vector<std::string> firedOrder;
    bool reachedZero = false;
    for (u32 i = 0; i < 120 && !reachedZero; ++i) // up to 2s; a 1s reverse run finishes well within
    {
        RunFrames(1);
        const auto& cine = m_Director.GetComponent<CinematicComponent>();
        for (const auto& e : cine.EventsFiredThisFrame)
        {
            firedOrder.push_back(e);
        }
        reachedZero = cine.Finished;
    }

    const auto& cine = m_Director.GetComponent<CinematicComponent>();
    EXPECT_TRUE(cine.Finished) << "reverse sequence should finish when the playhead reaches 0";
    EXPECT_FALSE(cine.Playing) << "finished sequence should stop playing";
    EXPECT_NEAR(cine.Time, 0.0f, 1e-3f) << "reverse playhead should land on 0";

    // Hero ended at the first translation key (it started at the last).
    const glm::vec3 heroPos = m_Hero.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(heroPos.y, 0.0f, 1e-2f) << "Hero should be back at the start pose";

    // Events fired in descending time order: "high" (t=0.75) before "low"
    // (t=0.25) as the playhead receded from the end.
    ASSERT_EQ(firedOrder.size(), 2u);
    EXPECT_EQ(firedOrder[0], "high");
    EXPECT_EQ(firedOrder[1], "low");
}
