#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// FlockingViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::OnUpdateRuntime × SystemScheduler (BoidSteering / BoidMovement) ×
//   FlockingSystem × FlockSpatialHash × TransformComponent.
//
// FlockSpatialHashTest pins the neighbour query in isolation. This file pins
// what only a real Scene tick can show — that the two scheduler nodes issue
// #731 splits the work across actually compose into a flock, and that they do
// so identically whichever executor runs them.
//
// The three acceptance criteria map onto the tests below:
//
//   1. "Several hundred boids flock at stable frame time, with neighbour
//      queries through the spatial hash." — FlockConvergesAndStaysBounded runs
//      400 agents through a real tick and asserts the flock actually forms
//      (neighbour counts rise, spread contracts) and never diverges or emits a
//      NaN. Frame TIME is not asserted here on purpose: a wall-clock threshold
//      in the shared suite is a flake generator, and the pruning proof lives in
//      FlockSpatialHashTest where it can be made hardware-independent.
//
//   2. "Deterministic under the fixed-timestep loop (#452) — no frame-rate-
//      dependent drift." — RepeatedRunsAreBitIdentical and
//      SequentialAndParallelExecutorsAgreeBitForBit. The second is the load-
//      bearing one: it runs the SAME scene under
//      SystemScheduler::SetParallelExecutionEnabled(false) and (true) with real
//      workers started, and demands bit-identical transforms. That is the only
//      check that would catch the steering pass reading state another marked
//      system mutates, or the movement node racing the solve.
//
//   3. "Runs on a worker thread with the scheduler audit table updated." — the
//      ordering/independence half is pinned as dependency EDGES in
//      SystemSchedulerTest (a position check would pass via the tie-break even
//      with the edge missing); the behavioural half is criterion 2's executor
//      agreement above.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SystemScheduler.h"
#include "OloEngine/Task/Scheduler.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Fixed LCG — the flock's starting scatter must be reproducible without
    // depending on any engine RNG stream.
    class DeterministicScatter
    {
      public:
        explicit DeterministicScatter(u32 seed)
            : m_State(seed)
        {
        }

        // Uniform in [-1, 1).
        f32 Next()
        {
            m_State = m_State * 1664525u + 1013904223u;
            return (static_cast<f32>(m_State >> 8) / static_cast<f32>(1u << 24)) * 2.0f - 1.0f;
        }

        glm::vec3 NextVec3(f32 extent)
        {
            const f32 x = Next() * extent;
            const f32 y = Next() * extent;
            const f32 z = Next() * extent;
            return { x, y, z };
        }

      private:
        u32 m_State;
    };

    [[nodiscard]] bool IsFinite(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A flock of several hundred agents, scattered in a box, with a shared goal.
// ─────────────────────────────────────────────────────────────────────────────
class FlockingViaSceneTickTest : public FunctionalTest
{
  protected:
    static constexpr u32 kBoidCount = 400;
    static constexpr f32 kScatterExtent = 15.0f;
    static constexpr glm::vec3 kGoal{ 0.0f, 0.0f, 0.0f };

    void BuildScene() override
    {
        m_Boids.reserve(kBoidCount);
        for (u32 i = 0; i < kBoidCount; ++i)
        {
            Entity e = GetScene().CreateEntity("Boid");
            e.AddComponent<BoidComponent>();
            m_Boids.push_back(e);
        }
        ResetFlockState();
    }

    // Re-seed every agent's authored state WITHOUT touching the registry's
    // structure. Destroying and recreating the entities instead would look
    // equivalent but is not: EnTT recycles entity ids from a LIFO free list, so
    // the rebuilt flock iterates in a different order, the snapshot indices
    // shuffle, and the float accumulation order inside each neighbourhood
    // changes — a determinism test built that way would fail for a reason that
    // has nothing to do with the code under test.
    void ResetFlockState()
    {
        DeterministicScatter scatter(0xB01D5EEDu);
        for (Entity& e : m_Boids)
        {
            e.GetComponent<TransformComponent>().Translation = scatter.NextVec3(kScatterExtent);

            auto& boid = e.GetComponent<BoidComponent>();
            boid.m_Velocity = scatter.NextVec3(1.0f);
            boid.m_SteeringForce = glm::vec3(0.0f);
            boid.m_NeighborCount = 0;
            boid.m_MaxSpeed = 6.0f;
            boid.m_MaxForce = 12.0f;
            boid.m_NeighborRadius = 5.0f;
            boid.m_SeparationRadius = 1.5f;
            boid.m_GoalPosition = kGoal;
            boid.m_GoalWeight = 0.6f;
            // No obstacles in the base scene — the avoidance seam has its own
            // test below with its own scene.
            boid.m_ObstacleAvoidWeight = 0.0f;
        }
    }

    // Mean distance from the flock's centroid — the spread metric.
    [[nodiscard]] f32 MeanRadius() const
    {
        glm::vec3 centroid(0.0f);
        for (const Entity& e : m_Boids)
            centroid += e.GetComponent<TransformComponent>().Translation;
        centroid /= static_cast<f32>(m_Boids.size());

        f32 total = 0.0f;
        for (const Entity& e : m_Boids)
            total += glm::length(e.GetComponent<TransformComponent>().Translation - centroid);
        return total / static_cast<f32>(m_Boids.size());
    }

    [[nodiscard]] f32 MeanNeighborCount() const
    {
        u64 total = 0;
        for (const Entity& e : m_Boids)
            total += e.GetComponent<BoidComponent>().m_NeighborCount;
        return static_cast<f32>(total) / static_cast<f32>(m_Boids.size());
    }

    [[nodiscard]] std::vector<glm::vec3> SnapshotPositions() const
    {
        std::vector<glm::vec3> out;
        out.reserve(m_Boids.size());
        for (const Entity& e : m_Boids)
            out.push_back(e.GetComponent<TransformComponent>().Translation);
        return out;
    }

    std::vector<Entity> m_Boids;
};

TEST_F(FlockingViaSceneTickTest, FlockConvergesAndStaysBounded)
{
    const f32 initialSpread = MeanRadius();

    RunFrames(1);

    // One tick is enough to prove the spatial hash found anybody at all — if
    // the neighbour query silently returned nothing (wrong cell size, a grid
    // never rebuilt, the steering node never registered) the agents would still
    // seek the goal and the flock would still contract, so the spread test
    // alone cannot catch it. THIS is the check that the hash is live.
    EXPECT_GT(MeanNeighborCount(), 1.0f) << "no agent found neighbours — the spatial hash is not being consulted";

    RunFrames(300);

    const f32 finalSpread = MeanRadius();
    EXPECT_LT(finalSpread, initialSpread) << "the flock never contracted (spread " << initialSpread << " -> "
                                          << finalSpread << ")";

    // Bounded, finite, and respecting the authored speed limit — a steering
    // feedback loop that has gone unstable shows up as any of these.
    for (const Entity& e : m_Boids)
    {
        const auto& transform = e.GetComponent<TransformComponent>();
        const auto& boid = e.GetComponent<BoidComponent>();

        ASSERT_TRUE(IsFinite(transform.Translation)) << "a boid position went non-finite";
        ASSERT_TRUE(IsFinite(boid.m_Velocity)) << "a boid velocity went non-finite";
        EXPECT_LE(glm::length(boid.m_Velocity), boid.m_MaxSpeed * 1.001f) << "max speed was not enforced";
        EXPECT_LT(glm::length(transform.Translation - kGoal), 10.0f * kScatterExtent)
            << "the flock dispersed instead of settling";
    }
}

TEST_F(FlockingViaSceneTickTest, FlockTracksItsGoal)
{
    // Goal seeking is the steering term a script drives, so pin it: pull the
    // goal well off to one side and the flock's centroid must follow.
    //
    // The weight is raised above the scene default because goal seeking is what
    // is under test here. At the default 0.6 it is a minority term against
    // separation (1.5) + cohesion (1.0) + alignment (1.0), and since the summed
    // force is truncated to m_MaxForce the flock creeps toward the goal at a
    // fraction of its top speed — real behaviour, but it makes the assertion a
    // measurement of the weight ratio rather than of goal seeking working.
    const glm::vec3 movedGoal{ 30.0f, 0.0f, -30.0f };
    for (Entity& e : m_Boids)
    {
        auto& boid = e.GetComponent<BoidComponent>();
        boid.m_GoalPosition = movedGoal;
        boid.m_GoalWeight = 2.0f;
    }

    const auto centroid = [this]
    {
        glm::vec3 c(0.0f);
        for (const Entity& e : m_Boids)
            c += e.GetComponent<TransformComponent>().Translation;
        return c / static_cast<f32>(m_Boids.size());
    };

    const f32 before = glm::length(centroid() - movedGoal);
    RunFrames(400);
    const f32 after = glm::length(centroid() - movedGoal);

    EXPECT_LT(after, before * 0.5f) << "the flock did not move towards its goal (" << before << " -> " << after << ")";
}

TEST_F(FlockingViaSceneTickTest, RepeatedRunsAreBitIdentical)
{
    // Same entities, same starting state, same tick count, twice. Fixed
    // timestep in, bit-identical positions out.
    RunFrames(120);
    const std::vector<glm::vec3> first = SnapshotPositions();

    ResetFlockState();

    RunFrames(120);
    const std::vector<glm::vec3> second = SnapshotPositions();

    ASSERT_EQ(first.size(), second.size());
    for (sizet i = 0; i < first.size(); ++i)
    {
        // Bitwise, not approximate: any tolerance here would hide exactly the
        // ordering-dependent drift this test exists to catch.
        EXPECT_EQ(first[i].x, second[i].x) << "boid " << i << " x diverged between identical runs";
        EXPECT_EQ(first[i].y, second[i].y) << "boid " << i << " y diverged between identical runs";
        EXPECT_EQ(first[i].z, second[i].z) << "boid " << i << " z diverged between identical runs";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Obstacle avoidance gets its own scene: a wall of agents flying straight at a
// sphere they must not pass through.
// ─────────────────────────────────────────────────────────────────────────────
class FlockingObstacleAvoidanceTest : public FunctionalTest
{
  protected:
    static constexpr f32 kObstacleRadius = 6.0f;
    static constexpr glm::vec3 kObstacleCentre{ 0.0f, 0.0f, 0.0f };

    void BuildScene() override
    {
        Entity obstacle = GetScene().CreateEntity("Obstacle");
        obstacle.GetComponent<TransformComponent>().Translation = kObstacleCentre;
        obstacle.AddComponent<BoidObstacleComponent>().m_Radius = kObstacleRadius;

        // A 7x7 curtain of agents upstream of the obstacle, every one of them
        // aimed at its centre — with avoidance off, all 49 would fly straight
        // through. The spacing spreads them from dead-centre (which can only
        // brake, since the repulsion is exactly anti-parallel to its heading)
        // out to well clear of the sphere, so the test can distinguish
        // "deflected" from "stalled".
        for (i32 row = -3; row <= 3; ++row)
        {
            for (i32 col = -3; col <= 3; ++col)
            {
                Entity e = GetScene().CreateEntity("Boid");
                e.GetComponent<TransformComponent>().Translation = { static_cast<f32>(col) * 3.0f,
                                                                     static_cast<f32>(row) * 3.0f, 20.0f };

                auto& boid = e.AddComponent<BoidComponent>();
                boid.m_Velocity = { 0.0f, 0.0f, -6.0f };
                boid.m_MaxSpeed = 6.0f;
                boid.m_MaxForce = 20.0f;
                boid.m_GoalPosition = { 0.0f, 0.0f, -40.0f }; // straight through the obstacle
                boid.m_GoalWeight = 1.0f;
                boid.m_ObstacleAvoidWeight = 8.0f;
                boid.m_ObstacleLookahead = 6.0f;

                // Flock coupling OFF so this scene isolates obstacle
                // avoidance. With it on, cohesion pulls the whole curtain in
                // toward its own centroid — which sits on the obstacle axis —
                // so the agents that would otherwise have slipped past the
                // sides get funnelled into the one place they cannot go, and
                // the test measures cohesion-vs-avoidance rather than
                // avoidance. (That collapse is correct flocking behaviour, and
                // it is worth knowing about: a flock aimed at an obstacle
                // narrower than the flock will bunch rather than split.) The
                // flocking terms themselves are covered by the tests above.
                boid.m_SeparationWeight = 0.0f;
                boid.m_AlignmentWeight = 0.0f;
                boid.m_CohesionWeight = 0.0f;

                m_Boids.push_back(e);
            }
        }
    }

    std::vector<Entity> m_Boids;
};

TEST_F(FlockingObstacleAvoidanceTest, AgentsSteerAroundTheObstacleInsteadOfThroughIt)
{
    // Track the closest approach of every agent across the whole traversal —
    // a single end-of-run position check would miss an agent that cut straight
    // through the middle and came out the far side.
    f32 closestApproach = std::numeric_limits<f32>::max();
    for (u32 frame = 0; frame < 500; ++frame)
    {
        RunFrames(1);
        for (const Entity& e : m_Boids)
        {
            const glm::vec3 position = e.GetComponent<TransformComponent>().Translation;
            ASSERT_TRUE(IsFinite(position));
            closestApproach = std::min(closestApproach, glm::length(position - kObstacleCentre));
        }
    }

    // Avoidance is a soft steering force, not a hard constraint, so the bar is
    // "clearly deflected", not "never overlapped": agents aimed dead-centre
    // must not end up deep inside the sphere.
    EXPECT_GT(closestApproach, kObstacleRadius * 0.5f)
        << "an agent penetrated well inside the obstacle (closest approach " << closestApproach << " vs radius "
        << kObstacleRadius << ")";

    // And the flock must actually have gone somewhere — a test where everyone
    // stalls in front of the obstacle would pass the check above vacuously.
    f32 furthestProgress = std::numeric_limits<f32>::max();
    for (const Entity& e : m_Boids)
        furthestProgress = std::min(furthestProgress, e.GetComponent<TransformComponent>().Translation.z);
    EXPECT_LT(furthestProgress, -kObstacleRadius) << "no agent got past the obstacle — they stalled rather than steered";
}

// ─────────────────────────────────────────────────────────────────────────────
// Executor agreement. Real FScheduler workers are started for the suite,
// otherwise Tasks::Launch degrades to inline execution and the "parallel" run
// would be the sequential one under another name — passing vacuously.
// ─────────────────────────────────────────────────────────────────────────────
class FlockingExecutorAgreementTest : public FunctionalTest
{
  protected:
    static void SetUpTestSuite()
    {
        LowLevelTasks::FScheduler::Get().StartWorkers();
    }

    static void TearDownTestSuite()
    {
        LowLevelTasks::FScheduler::Get().StopWorkers();
    }

    void SetUp() override
    {
        m_PreviousParallel = SystemScheduler::IsParallelExecutionEnabled();
        FunctionalTest::SetUp();
    }

    void TearDown() override
    {
        SystemScheduler::SetParallelExecutionEnabled(m_PreviousParallel);
        FunctionalTest::TearDown();
    }

    void BuildScene() override
    {
        for (u32 i = 0; i < 200; ++i)
        {
            Entity e = GetScene().CreateEntity("Boid");
            e.AddComponent<BoidComponent>();
            m_Boids.push_back(e);
        }
        ResetFlockState();
    }

    // Same reasoning as FlockingViaSceneTickTest::ResetFlockState — re-seed in
    // place so the two runs share an identical registry structure and iteration
    // order, leaving the EXECUTOR as the only difference between them.
    void ResetFlockState()
    {
        DeterministicScatter scatter(0x5EEDF00Du);
        for (Entity& e : m_Boids)
        {
            e.GetComponent<TransformComponent>().Translation = scatter.NextVec3(12.0f);

            auto& boid = e.GetComponent<BoidComponent>();
            boid.m_Velocity = scatter.NextVec3(2.0f);
            boid.m_SteeringForce = glm::vec3(0.0f);
            boid.m_NeighborCount = 0;
            boid.m_NeighborRadius = 5.0f;
            boid.m_GoalPosition = { 0.0f, 5.0f, 0.0f };
            boid.m_GoalWeight = 0.8f;
            boid.m_ObstacleAvoidWeight = 0.0f;
        }
    }

    [[nodiscard]] std::vector<glm::vec3> RunAndSnapshot(bool parallel)
    {
        SystemScheduler::SetParallelExecutionEnabled(parallel);
        RunFrames(150);

        std::vector<glm::vec3> out;
        out.reserve(m_Boids.size());
        for (const Entity& e : m_Boids)
            out.push_back(e.GetComponent<TransformComponent>().Translation);
        return out;
    }

    std::vector<Entity> m_Boids;
    bool m_PreviousParallel = true;
};

TEST_F(FlockingExecutorAgreementTest, SequentialAndParallelExecutorsAgreeBitForBit)
{
    // This is acceptance criterion 2's real teeth. BoidSteering is dispatched
    // to a worker in the parallel run and inlined in the sequential one; if the
    // split leaked any shared mutable state — or if BoidMovement could start
    // before the solve finished — the two runs would drift apart here and
    // nowhere else.
    const std::vector<glm::vec3> sequential = RunAndSnapshot(false);

    ResetFlockState();
    const std::vector<glm::vec3> parallel = RunAndSnapshot(true);

    ASSERT_EQ(sequential.size(), parallel.size());
    for (sizet i = 0; i < sequential.size(); ++i)
    {
        EXPECT_EQ(sequential[i].x, parallel[i].x) << "boid " << i << " x differs between executors";
        EXPECT_EQ(sequential[i].y, parallel[i].y) << "boid " << i << " y differs between executors";
        EXPECT_EQ(sequential[i].z, parallel[i].z) << "boid " << i << " z differs between executors";
    }
}
