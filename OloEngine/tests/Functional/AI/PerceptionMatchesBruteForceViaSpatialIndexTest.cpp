#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// PerceptionMatchesBruteForceViaSpatialIndexTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × Scene::UpdateSpatialIndex (SceneSpatialIndex) ×
//   PerceptionSystem × PerceptionComponent / PerceptibleComponent.
//
// PerceptionSystem was refactored (issue #430) from an O(n) linear scan over
// every perceptible entity to a radius query against the per-tick spatial
// acceleration structure that Scene::OnUpdateRuntime rebuilds immediately
// before PerceptionSystem runs. This test is the proof-of-value *and* the
// safety net: it pins that the spatially-accelerated result is byte-for-byte
// the same target the brute-force scan would have chosen.
//
// Rather than hand-pick a few positions, it scatters many perceptibles of
// mixed team / perceptibility, ticks one frame, then recomputes the expected
// "nearest visible target" with an independent O(n) reference using the exact
// same filters (self, perceptibility, team, range+FOV cone). If the spatial
// query ever drops a candidate it should have returned — wrong cell range,
// off-by-one bounding cell, a stale index — the chosen target diverges and
// this fails. LOS raycasting is off so the case is physics-independent; the
// occlusion seam is pinned separately.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/Perception/PerceptionMath.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PerceptionMatchesBruteForceViaSpatialIndexTest : public FunctionalTest
{
  protected:
    // The watcher's authored sight parameters, shared by the scene build and
    // the brute-force reference so the two can't drift.
    static constexpr f32 kSightRange = 20.0f;
    static constexpr f32 kFovDegrees = 120.0f;
    static constexpr i32 kWatcherTeam = 0;

    struct TargetSpec
    {
        glm::vec3 Pos;
        i32 Team;
        bool Perceptible;
    };

    void BuildScene() override
    {
        // Watcher at the origin looking down -Z (identity rotation).
        m_Watcher = GetScene().CreateEntity("Watcher");
        m_Watcher.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        auto& pc = m_Watcher.AddComponent<PerceptionComponent>();
        pc.SightRange = kSightRange;
        pc.FovDegrees = kFovDegrees;
        pc.EyeOffset = { 0.0f, 0.0f, 0.0f }; // eye at entity origin → simple geometry
        pc.RequireLineOfSight = false;       // physics-independent
        pc.PerceiverTeam = kWatcherTeam;
        pc.DetectSameTeam = false;

        // A spread of perceptibles: in-cone at various ranges, out of range,
        // behind the watcher, same-team (filtered), cloaked (filtered), and a
        // dense near cluster to stress multiple grid cells. Positions are fixed
        // (not random) so the expectation is reproducible and reviewable.
        const TargetSpec specs[] = {
            { { 0.0f, 0.0f, -3.0f }, 1, true },   // straight ahead, near
            { { 2.0f, 0.0f, -4.0f }, 1, true },   // ahead, off-axis, near
            { { -1.5f, 1.0f, -6.0f }, 2, true },  // ahead
            { { 5.0f, 0.0f, -5.0f }, 1, true },   // ahead, wider angle
            { { 0.5f, -0.5f, -2.0f }, 3, true },  // closest in-cone candidate
            { { 0.0f, 0.0f, -25.0f }, 1, true },  // out of range (>20)
            { { 0.0f, 0.0f, 8.0f }, 1, true },    // behind
            { { -3.0f, 0.0f, 4.0f }, 1, true },   // behind/side
            { { 1.0f, 0.0f, -3.0f }, 0, true },   // SAME team → filtered
            { { 0.2f, 0.0f, -1.5f }, 4, false },  // cloaked (nearest of all) → filtered
            { { 12.0f, 2.0f, -12.0f }, 2, true }, // far but in-cone
            { { -8.0f, 0.0f, -9.0f }, 5, true },  // left, in-cone-ish
            { { 18.0f, 0.0f, -2.0f }, 1, true },  // far to the side, likely outside cone
            { { 0.0f, 10.0f, -1.0f }, 1, true },  // steeply above → outside cone
        };

        for (const TargetSpec& spec : specs)
        {
            Entity e = GetScene().CreateEntity("Target");
            e.GetComponent<TransformComponent>().Translation = spec.Pos;
            auto& perc = e.AddComponent<PerceptibleComponent>();
            perc.Team = spec.Team;
            perc.IsPerceptible = spec.Perceptible;
            m_Targets.push_back(e);
        }
    }

    // Independent O(n) reference: the nearest perceptible-and-visible target by
    // distance from the eye, applying the same filters PerceptionSystem does.
    // Returns 0 if nothing should be visible.
    [[nodiscard]] UUID ExpectedVisibleTarget() const
    {
        const auto& pc = m_Watcher.GetComponent<PerceptionComponent>();
        const auto& wt = m_Watcher.GetComponent<TransformComponent>();
        const glm::quat orientation = wt.GetRotation();
        const glm::vec3 eye = wt.Translation + (orientation * pc.EyeOffset);
        const glm::vec3 forward = glm::normalize(orientation * glm::vec3(0.0f, 0.0f, -1.0f));

        UUID best = 0;
        f32 bestDistSq = std::numeric_limits<f32>::max();
        for (const Entity& target : m_Targets)
        {
            const auto& perceptible = target.GetComponent<PerceptibleComponent>();
            if (!perceptible.IsPerceptible)
                continue;
            if (!pc.DetectSameTeam && perceptible.Team == pc.PerceiverTeam)
                continue;

            const glm::vec3 pos = target.GetComponent<TransformComponent>().Translation;
            const f32 distSq = glm::dot(pos - eye, pos - eye);
            if (distSq >= bestDistSq)
                continue;
            if (!PerceptionMath::IsInSightCone(eye, forward, pos, pc.SightRange, pc.FovDegrees))
                continue;

            bestDistSq = distSq;
            best = target.GetUUID();
        }
        return best;
    }

    Entity m_Watcher;
    std::vector<Entity> m_Targets;
};

TEST_F(PerceptionMatchesBruteForceViaSpatialIndexTest, ChoosesSameTargetAsBruteForceScan)
{
    const UUID expected = ExpectedVisibleTarget();
    ASSERT_NE(static_cast<u64>(expected), 0u)
        << "test scene is degenerate — the reference expected at least one visible target";

    RunFrames(1);

    const auto& pc = m_Watcher.GetComponent<PerceptionComponent>();
    EXPECT_TRUE(pc.HasVisibleTarget)
        << "spatial-index perception saw nothing where the brute-force scan saw a target";
    EXPECT_EQ(static_cast<u64>(pc.VisibleTarget), static_cast<u64>(expected))
        << "spatial-index perception chose a different target than the brute-force scan";
}

TEST_F(PerceptionMatchesBruteForceViaSpatialIndexTest, TracksBruteForceAsTargetsMove)
{
    // Walk the whole field of targets through the watcher's view over several
    // frames; every frame the accelerated choice must equal the brute force.
    for (u32 frame = 0; frame < 12; ++frame)
    {
        // Slide every target along +Z toward/through the watcher so the nearest
        // visible one keeps changing (and the spatial index re-bins them).
        for (Entity& target : m_Targets)
        {
            target.GetComponent<TransformComponent>().Translation.z += 2.0f;
        }

        const UUID expected = ExpectedVisibleTarget();
        RunFrames(1);

        const auto& pc = m_Watcher.GetComponent<PerceptionComponent>();
        EXPECT_EQ(pc.HasVisibleTarget, static_cast<u64>(expected) != 0u)
            << "visibility disagreement at frame " << frame;
        if (static_cast<u64>(expected) != 0u)
        {
            EXPECT_EQ(static_cast<u64>(pc.VisibleTarget), static_cast<u64>(expected))
                << "target disagreement at frame " << frame;
        }
    }
}

TEST_F(PerceptionMatchesBruteForceViaSpatialIndexTest, SeesNothingWhenAllTargetsLeaveRange)
{
    // Shove every target far past the sight range; the accelerated query must
    // agree with the reference that nothing is visible.
    for (Entity& target : m_Targets)
    {
        target.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -1000.0f };
    }
    ASSERT_EQ(static_cast<u64>(ExpectedVisibleTarget()), 0u);

    RunFrames(1);

    EXPECT_FALSE(m_Watcher.GetComponent<PerceptionComponent>().HasVisibleTarget);
}
