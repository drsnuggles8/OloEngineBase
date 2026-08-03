#pragma once

#include "OloEngine/AI/Flocking/FlockSpatialHash.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Scene;

    // Per-Scene scratch for the flocking systems (issue #731). Held by value on
    // the Scene and rebuilt every steering tick; persistent purely so the hot
    // path doesn't allocate per tick (same reasoning as Scene's
    // PropagateWorldTransforms scratch buffers).
    //
    // Runtime-only: never serialized, never copied by Scene::Copy — it is fully
    // rederived from the components at the top of every steering pass.
    struct FlockingWorkspace
    {
        // Boid snapshot, index-parallel and in the steering view's iteration
        // order. Taken ONCE at the top of the steering pass so every agent
        // steers against the same frozen world state — an agent's answer can
        // never depend on how far through the flock the loop has got.
        std::vector<glm::vec3> BoidPositions;
        std::vector<glm::vec3> BoidVelocities;
        FlockSpatialHash BoidGrid;

        // Obstacle snapshot. Separate grid because an obstacle's useful cell
        // size (avoidance reach + the biggest obstacle) has nothing to do with
        // a boid's perception radius.
        std::vector<glm::vec3> ObstaclePositions;
        std::vector<f32> ObstacleRadii;
        FlockSpatialHash ObstacleGrid;
        f32 MaxObstacleRadius = 0.0f;

        void Clear();
    };

    // =========================================================================
    // FlockingSystem — Reynolds boids over a uniform spatial hash (issue #731).
    //
    // ── The split, and why it exists ─────────────────────────────────────────
    // Acceptance criterion 3 asks for this to run on a worker thread, but
    // moving a flock means writing TransformComponent, and a TransformComponent
    // write is precisely what pins a system to the game thread in this engine
    // (see the audit table in Scene::GetGameplayScheduler — "Navigation /
    // MorphEval are pinned main-thread: TransformComponent writes"). A single
    // UpdateBoids system therefore could not be marked Parallelizable without
    // lying about it.
    //
    // So the work splits along that exact line:
    //
    //   StepSteering  — the expensive part (snapshot, hash rebuild, O(k)
    //                   neighbour queries per agent, force accumulation). Reads
    //                   transforms, writes ONLY m_SteeringForce /
    //                   m_NeighborCount on each agent's own component plus the
    //                   Scene-owned workspace. No structural registry changes,
    //                   no bus publishes, no GL, no RNG. Registered as the
    //                   Parallelizable "BoidSteering" node.
    //
    //   ApplyMovement — integrate force into velocity, clamp to max speed,
    //                   advance TransformComponent (and optionally face the
    //                   velocity). Trivially cheap, game thread only.
    //                   Registered as the unmarked "BoidMovement" node.
    //
    // ── Determinism (acceptance criterion 2) ─────────────────────────────────
    // Nothing here draws from an RNG stream at all, so the #452 seeded-stream
    // rule has no work to do: steering is a pure function of the snapshot.
    // Every accumulation runs in the spatial hash's documented visit order, and
    // integration uses the scheduler's fixed timestep, so a run is reproducible
    // whatever the frame pacing and identical with
    // OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1 set or unset.
    // =========================================================================
    class FlockingSystem
    {
      public:
        // Worker-safe. Snapshots the flock, rebuilds both grids, and solves the
        // steering force for every BoidComponent.
        static void StepSteering(Scene* scene, FlockingWorkspace& workspace);

        // Game thread only — writes TransformComponent.
        static void ApplyMovement(Scene* scene, f32 dt);

        // ── Steering kernel, exposed for unit tests ──────────────────────────

        // Truncate `v` to at most `maxLength`. A zero/non-finite vector maps to
        // zero rather than producing a NaN direction.
        [[nodiscard]] static glm::vec3 ClampLength(const glm::vec3& v, f32 maxLength);

        // Classic Reynolds steering: the force that turns `velocity` towards
        // travelling at `maxSpeed` along `desiredDirection`, truncated to
        // `maxForce`. A degenerate direction yields no steering.
        [[nodiscard]] static glm::vec3 SteerTowards(const glm::vec3& desiredDirection, const glm::vec3& velocity,
                                                    f32 maxSpeed, f32 maxForce);
    };
} // namespace OloEngine
