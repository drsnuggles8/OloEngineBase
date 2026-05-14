#include "OloEnginePCH.h"

// =============================================================================
// SceneStepAdvancesOneFrameTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::Step(N) × m_StepFrames decrement × OnUpdateRuntime gating.
//   The editor's "step frame" button (and any debugger-style scrubbing
//   workflow) calls Scene::Step(N) on a paused scene to advance exactly N
//   frames before the pause re-engages. The relevant gate is the
//   `if (!m_IsPaused || m_StepFrames-- > 0)` branch in OnUpdateRuntime.
//   A regression — Step doesn't decrement, or m_StepFrames goes negative
//   without re-pausing, or Step ignores m_IsPaused — silently breaks the
//   editor's frame-by-frame debugging path.
//
// Scenario: dynamic body falling. Pause the scene; tick — body must NOT
// fall. Call Step(3); tick 3 frames — body advances. Tick more — body
// must once again be frozen because Step's budget should be consumed.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class SceneStepAdvancesOneFrameTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Body = GetScene().CreateEntity("FallingBody");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 50.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    [[nodiscard]] f32 BodyY() const
    {
        return m_Body.GetComponent<TransformComponent>().Translation.y;
    }

    Entity m_Body;
};

TEST_F(SceneStepAdvancesOneFrameTest, StepAdvancesExactlyTheRequestedFrameCountThenRePauses)
{
    // Phase 1: pause and verify a tick is a no-op.
    GetScene().SetPaused(true);
    const f32 startY = BodyY();
    RunFrames(/*count=*/15); // 0.25s of "wall time"
    EXPECT_NEAR(BodyY(), startY, 1e-4f)
        << "paused scene advanced its body during a tick — Scene::SetPaused does not gate physics";

    // Phase 2: Step(3). Tick a single frame at a time so the per-frame
    // gating is honoured (Step decrements m_StepFrames inside OnUpdateRuntime).
    GetScene().Step(/*frames=*/3);
    const f32 yBeforeSteps = BodyY();
    RunFrames(/*count=*/3); // exactly 3 frames

    const f32 yAfterSteps = BodyY();
    EXPECT_LT(yAfterSteps, yBeforeSteps - 0.001f)
        << "Step(3) did not advance physics; y=" << yAfterSteps
        << " vs " << yBeforeSteps << " — m_StepFrames decrement is missing.";

    // Phase 3: continue ticking. Pause should re-engage now that the
    // step budget is consumed.
    const f32 yAtPauseRestore = BodyY();
    RunFrames(/*count=*/30); // 0.5s of wall time
    const f32 yAfterMorePausedTicks = BodyY();

    EXPECT_NEAR(yAfterMorePausedTicks, yAtPauseRestore, 1e-3f)
        << "scene continued to advance after Step's budget should have been consumed; "
           "y went from " << yAtPauseRestore << " to " << yAfterMorePausedTicks
           << " — Step did not re-pause once m_StepFrames hit zero.";
}
