// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SyntheticInputTest — unit test (headless, no window, no Application).
//
// Pins the synthetic-input overlay behind olo_input_inject, and specifically the
// ABSOLUTE-vs-RELATIVE distinction added for issue #607.
//
// The bug, stated precisely. `Input::GetMousePosition()` returns the synthetic
// cursor only while an injected plan is IN FLIGHT, then falls back to the hardware
// cursor. That is exactly right for an absolute consumer (an ImGui widget, a gizmo
// drag). But a mouse-look rig integrates `mouse - lastMousePos` ACROSS frames, so it
// sees the override arrive as a one-frame spike and, the frame the plan drains, that
// spike's exact mirror. The two cancel. Measured on the #645 player rig: four equal
// horizontal moves each produced dYaw = 0.000, with pitch pinned at MaxPitchDeg —
// which reads like a saturating rig, and is not.
//
// The test that matters is therefore not "does the override work" but "what does an
// INTEGRATING consumer accumulate across the whole plan". SimulatedLookDelta below
// is that consumer, in three lines, and the two tests differ only in which injection
// mode drives it: the absolute path must sum to zero (documenting the limitation,
// not asserting it is fine), the relative path must sum to the requested
// displacement and stay there.
//
// This is also why `holdFrames` — latching the absolute override for longer — was
// NOT the fix: the mirror is produced by REVERTING the override, not by its brevity,
// so the sum is exactly zero however many frames it is held. AbsoluteOverride-
// SumsToZeroHoweverLongItIsHeld pins that, so nobody re-proposes it.
//
// No window is needed: with no Application, Input::GetMousePosition falls back to a
// zero hardware position, which makes the arithmetic exact rather than approximate.
// =============================================================================

#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/SyntheticInput.h"

#include <glm/glm.hpp>

#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    // A minimal stand-in for a mouse-look rig: sample the poll API once per "frame"
    // and accumulate `current - previous`, exactly as PlayerRigSystem does.
    class IntegratingConsumer
    {
      public:
        void Sample()
        {
            const glm::vec2 mouse = Input::GetMousePosition();
            if (m_HasPrevious)
            {
                m_AccumulatedX += mouse.x - m_PreviousX;
                m_AccumulatedY += mouse.y - m_PreviousY;
            }
            m_PreviousX = mouse.x;
            m_PreviousY = mouse.y;
            m_HasPrevious = true;
        }

        [[nodiscard]] f32 AccumulatedX() const
        {
            return m_AccumulatedX;
        }
        [[nodiscard]] f32 AccumulatedY() const
        {
            return m_AccumulatedY;
        }

      private:
        bool m_HasPrevious = false;
        f32 m_PreviousX = 0.0f;
        f32 m_PreviousY = 0.0f;
        f32 m_AccumulatedX = 0.0f;
        f32 m_AccumulatedY = 0.0f;
    };

    // SyntheticInput is process-global state, so every test must leave it clean or
    // it poisons whatever runs next (including unrelated Input:: assertions).
    class SyntheticInputTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            SyntheticInput::Reset();
        }
        void TearDown() override
        {
            SyntheticInput::Reset();
        }
    };
} // namespace

TEST_F(SyntheticInputTest, AbsoluteOverrideSumsToZeroHoweverLongItIsHeld)
{
    // The documented limitation, pinned so the "just hold it longer" non-fix cannot
    // be re-proposed: whether the override lives for 1 frame or 20, an integrating
    // consumer nets exactly zero, because the mirror comes from REVERTING it.
    for (const int holdFrames : { 1, 5, 20 })
    {
        SyntheticInput::Reset();
        IntegratingConsumer consumer;
        consumer.Sample(); // baseline at the hardware position

        SyntheticInput::SetMousePosition({ 400.0f, 300.0f });
        for (int frame = 0; frame < holdFrames; ++frame)
            consumer.Sample();

        SyntheticInput::ClearMousePosition(); // the plan drains
        consumer.Sample();

        EXPECT_FLOAT_EQ(consumer.AccumulatedX(), 0.0f) << "held for " << holdFrames << " frame(s)";
        EXPECT_FLOAT_EQ(consumer.AccumulatedY(), 0.0f) << "held for " << holdFrames << " frame(s)";
    }
}

TEST_F(SyntheticInputTest, RelativeDeltaIsRegisteredOnceAndNeverTakenBack)
{
    IntegratingConsumer consumer;
    consumer.Sample();

    SyntheticInput::AddMouseDelta({ 40.0f, -10.0f });
    consumer.Sample();
    EXPECT_FLOAT_EQ(consumer.AccumulatedX(), 40.0f);
    EXPECT_FLOAT_EQ(consumer.AccumulatedY(), -10.0f);

    // Every subsequent frame contributes nothing — no mirror, ever. This is the
    // whole difference from the absolute path above.
    for (int frame = 0; frame < 10; ++frame)
        consumer.Sample();
    EXPECT_FLOAT_EQ(consumer.AccumulatedX(), 40.0f);
    EXPECT_FLOAT_EQ(consumer.AccumulatedY(), -10.0f);
}

TEST_F(SyntheticInputTest, RepeatedDeltasAccumulate)
{
    // The reported symptom was four equal moves producing dYaw = 0.000 each. Four
    // equal DELTAS must produce four equal non-zero contributions that add up.
    IntegratingConsumer consumer;
    consumer.Sample();

    f32 previous = 0.0f;
    for (int move = 0; move < 4; ++move)
    {
        SyntheticInput::AddMouseDelta({ 40.0f, 0.0f });
        consumer.Sample();
        EXPECT_FLOAT_EQ(consumer.AccumulatedX() - previous, 40.0f) << "move " << move << " registered nothing";
        previous = consumer.AccumulatedX();
    }
    EXPECT_FLOAT_EQ(consumer.AccumulatedX(), 160.0f);
}

TEST_F(SyntheticInputTest, AbsoluteOverrideStillWinsOutrightWhileAnOffsetIsHeld)
{
    // The trap the whole slice had to avoid: the one-frame override behaviour is
    // CORRECT for ImGui widgets and gizmo drags, so relative injection had to be
    // strictly additive. An injected click names an exact pixel and must resolve to
    // it — adding the accumulated offset on top would land the click somewhere the
    // tool never asked for, silently, and only on sessions that had used mouseDelta.
    SyntheticInput::AddMouseDelta({ 40.0f, -10.0f });
    SyntheticInput::SetMousePosition({ 640.0f, 360.0f });

    const glm::vec2 position = Input::GetMousePosition();
    EXPECT_FLOAT_EQ(position.x, 640.0f);
    EXPECT_FLOAT_EQ(position.y, 360.0f);

    // ...and the offset is still there once the plan drains.
    SyntheticInput::ClearMousePosition();
    const glm::vec2 after = Input::GetMousePosition();
    EXPECT_FLOAT_EQ(after.x, 40.0f);
    EXPECT_FLOAT_EQ(after.y, -10.0f);
}

TEST_F(SyntheticInputTest, ResetAndClearOffsetPutTheCursorBack)
{
    SyntheticInput::AddMouseDelta({ 25.0f, 5.0f });
    ASSERT_FLOAT_EQ(SyntheticInput::GetMouseOffset().x, 25.0f);

    SyntheticInput::ClearMouseOffset();
    EXPECT_FLOAT_EQ(SyntheticInput::GetMouseOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(Input::GetMousePosition().x, 0.0f);

    // Reset() is the teardown/panic path and must clear the offset too, or an
    // interrupted session leaves the cursor permanently displaced.
    SyntheticInput::AddMouseDelta({ -7.0f, 3.0f });
    SyntheticInput::Reset();
    EXPECT_FLOAT_EQ(SyntheticInput::GetMouseOffset().y, 0.0f);
}

TEST_F(SyntheticInputTest, NoInjectionMeansNoOffsetAtAll)
{
    // The fast path: an untouched overlay must not perturb the reported position by
    // so much as a float epsilon, since every Input::GetMousePosition caller in the
    // engine goes through it.
    glm::vec2 offset{ 1.0f, 1.0f };
    EXPECT_FALSE(SyntheticInput::TryGetMouseOffset(offset));
    EXPECT_FLOAT_EQ(Input::GetMousePosition().x, 0.0f);
    EXPECT_FLOAT_EQ(Input::GetMousePosition().y, 0.0f);
}

// ---- issue #854: a drag must leave nothing behind ---------------------------
//
// #854 reported that after a `drag`, every later `click` in the session was inert
// while still reporting ok, and named this overlay as the prime suspect: it carries
// both an absolute cursor override and a persistent relative offset, and the override
// wins outright while set, so an override left latched by a drag would explain both
// halves at once.
//
// It was not the cause. The real one is a layer up, in the ImGui GLFW backend, which
// re-injects the HARDWARE cursor position after the injected one whenever the window
// is focused and the physical mouse is not over it — see McpInputInjectTest and
// EditorLayer::AssertSyntheticCursorOverWindow. This test exists anyway, and says so
// out loud, for two reasons: the suspicion is a natural one that will be raised again,
// and the property it describes is one this overlay genuinely owes its callers. A
// drag's whole event sequence — position, press, interpolated moves, release — must
// leave the overlay bit-for-bit as it found it, or the next injection starts from a
// state nobody asked for.

TEST_F(SyntheticInputTest, ADragLeavesTheOverlayExactlyAsItFoundIt)
{
    // Snapshot the pre-plan state. With no Application the hardware fallback is a zero
    // position, so this is exact rather than approximate.
    const glm::vec2 before = Input::GetMousePosition();
    ASSERT_FALSE(SyntheticInput::IsMouseButtonDown(Mouse::ButtonLeft));
    ASSERT_FALSE(SyntheticInput::AnyKeyDown());

    // Replay what a `drag` plan feeds the overlay: move to the start, press, walk the
    // interpolated steps with the button held, release, then the plan drains.
    SyntheticInput::SetMousePosition({ 100.0f, 200.0f });
    SyntheticInput::SetMouseButton(Mouse::ButtonLeft, true);
    for (int step = 1; step <= 8; ++step)
    {
        const auto t = static_cast<f32>(step) / 8.0f;
        SyntheticInput::SetMousePosition({ 100.0f + (400.0f * t), 200.0f + (150.0f * t) });
    }
    // Mid-drag the overlay must report the button as held, or the drag is not a drag.
    EXPECT_TRUE(SyntheticInput::IsMouseButtonDown(Mouse::ButtonLeft));
    SyntheticInput::SetMouseButton(Mouse::ButtonLeft, false);
    SyntheticInput::ClearMousePosition();

    // State after the drag == state before it. A latched override here would make
    // every later injected click resolve to the drag's end point instead of its own.
    const glm::vec2 after = Input::GetMousePosition();
    EXPECT_FLOAT_EQ(after.x, before.x);
    EXPECT_FLOAT_EQ(after.y, before.y);
    EXPECT_FALSE(SyntheticInput::IsMouseButtonDown(Mouse::ButtonLeft));
    glm::vec2 position{ 0.0f };
    EXPECT_FALSE(SyntheticInput::TryGetMousePosition(position))
        << "the absolute override outlived the plan that set it";
    // A drag emits no relative displacement at all, so the one piece of state that is
    // ALLOWED to outlive a plan must not have moved either.
    EXPECT_FLOAT_EQ(SyntheticInput::GetMouseOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(SyntheticInput::GetMouseOffset().y, 0.0f);
}

TEST_F(SyntheticInputTest, AnAbandonedDragDoesNotLeaveAButtonHeld)
{
    // The teardown path, which is the one that actually needed a guard. A plan that is
    // cut short between its press and its release leaves the button logically down,
    // and a mouse button nothing will ever release is not a cosmetic leak: ImGui pins
    // its ActiveId while a button is held, which makes IsWindowHovered() false for
    // every panel and swallows every subsequent click. Reset() is what the editor
    // calls to guarantee that cannot outlive the plan.
    SyntheticInput::SetMousePosition({ 640.0f, 360.0f });
    SyntheticInput::SetMouseButton(Mouse::ButtonLeft, true);
    ASSERT_TRUE(SyntheticInput::IsMouseButtonDown(Mouse::ButtonLeft));

    SyntheticInput::Reset(); // the plan is abandoned mid-flight

    EXPECT_FALSE(SyntheticInput::IsMouseButtonDown(Mouse::ButtonLeft));
    glm::vec2 position{ 0.0f };
    EXPECT_FALSE(SyntheticInput::TryGetMousePosition(position));
    EXPECT_FLOAT_EQ(Input::GetMousePosition().x, 0.0f);
}
