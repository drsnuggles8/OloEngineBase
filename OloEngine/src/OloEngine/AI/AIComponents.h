#pragma once

#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h" // OLO_SERIALIZE(Skip/Key) to mark runtime fields / rename

#include <glm/glm.hpp>

namespace OloEngine
{
    struct BehaviorTreeComponent
    {
        OLO_SERIALIZE(Key, "BehaviorTreeAsset")
        AssetHandle BehaviorTreeAssetHandle = 0;
        OLO_SERIALIZE(Skip)
        BTBlackboard Blackboard;

        // Runtime (not serialized)
        OLO_SERIALIZE(Skip)
        Ref<BehaviorTree> RuntimeTree = nullptr;
        OLO_SERIALIZE(Skip)
        bool IsRunning = false;

        BehaviorTreeComponent() = default;

        // Copy = duplicate the static asset reference only; runtime state is
        // rebuilt from the asset later. (Used by entt's emplace_or_replace and
        // by serialization round-trips.)
        BehaviorTreeComponent(const BehaviorTreeComponent& other)
            : BehaviorTreeAssetHandle(other.BehaviorTreeAssetHandle)
        {
        }
        BehaviorTreeComponent& operator=(const BehaviorTreeComponent& other)
        {
            if (this != &other)
            {
                BehaviorTreeAssetHandle = other.BehaviorTreeAssetHandle;
                Blackboard.Clear();
                RuntimeTree = nullptr;
                IsRunning = false;
            }
            return *this;
        }

        // Move = transfer ownership including runtime state. Without these,
        // user-defined copy ops disable the implicit move and `std::move(...)`
        // silently falls back to copy-which-clears-runtime — surprising
        // callers that just built a programmatic tree and expected it to
        // survive into the registry.
        BehaviorTreeComponent(BehaviorTreeComponent&&) noexcept = default;
        BehaviorTreeComponent& operator=(BehaviorTreeComponent&&) noexcept = default;
    };

    struct StateMachineComponent
    {
        OLO_SERIALIZE(Key, "StateMachineAsset")
        AssetHandle StateMachineAssetHandle = 0;
        OLO_SERIALIZE(Skip)
        BTBlackboard Blackboard;

        // Runtime (not serialized)
        OLO_SERIALIZE(Skip)
        Ref<StateMachine> RuntimeFSM = nullptr;

        StateMachineComponent() = default;

        // Copy = duplicate static asset reference only (rebuilt from asset
        // later). See BehaviorTreeComponent above for the rationale.
        StateMachineComponent(const StateMachineComponent& other)
            : StateMachineAssetHandle(other.StateMachineAssetHandle)
        {
        }
        StateMachineComponent& operator=(const StateMachineComponent& other)
        {
            if (this != &other)
            {
                StateMachineAssetHandle = other.StateMachineAssetHandle;
                Blackboard.Clear();
                RuntimeFSM = nullptr;
            }
            return *this;
        }

        // Move = transfer ownership including runtime state.
        StateMachineComponent(StateMachineComponent&&) noexcept = default;
        StateMachineComponent& operator=(StateMachineComponent&&) noexcept = default;
    };

    // Deliberative GOAP planner attached to an entity. Like the BT/FSM
    // components, the heavy runtime brain (RuntimeAgent: its actions, goals and
    // world state) is built programmatically by gameplay/scripting code and is
    // NOT serialized — only the authored Enabled flag and a script-facing
    // Blackboard persist. AISystem ticks RuntimeAgent each frame when Enabled.
    struct GoapAgentComponent
    {
        bool Enabled = true;
        OLO_SERIALIZE(Skip)
        BTBlackboard Blackboard; // sensor/script bridge, mirrors BT/FSM

        // Runtime (not serialized); rebuilt after load by gameplay code.
        OLO_SERIALIZE(Skip)
        Ref<GoapAgent> RuntimeAgent = nullptr;

        GoapAgentComponent() = default;

        // Copy = duplicate the authored config only; the runtime brain is rebuilt
        // later. See BehaviorTreeComponent above for the rationale.
        GoapAgentComponent(const GoapAgentComponent& other)
            : Enabled(other.Enabled)
        {
        }
        GoapAgentComponent& operator=(const GoapAgentComponent& other)
        {
            if (this != &other)
            {
                Enabled = other.Enabled;
                Blackboard.Clear();
                RuntimeAgent = nullptr;
            }
            return *this;
        }

        // Move = transfer ownership including runtime state.
        GoapAgentComponent(GoapAgentComponent&&) noexcept = default;
        GoapAgentComponent& operator=(GoapAgentComponent&&) noexcept = default;
    };

    // Marks an entity as something a PerceptionComponent can sense (a stimulus
    // source). A perceiver only ever considers entities carrying this marker, so
    // it doubles as the target filter: Team gates which factions notice each
    // other (see PerceptionComponent::PerceiverTeam / DetectSameTeam) and
    // IsPerceptible can be toggled off for stealth / cloaking without removing
    // the component. Pure authored data — no runtime state.
    struct PerceptibleComponent
    {
        i32 Team = 0;              // faction id used by perceiver team-filtering
        bool IsPerceptible = true; // when false the entity is invisible to sight

        PerceptibleComponent() = default;
        PerceptibleComponent(const PerceptibleComponent&) = default;
        PerceptibleComponent& operator=(const PerceptibleComponent&) = default;

        auto operator==(const PerceptibleComponent& other) const -> bool = default;
    };

    // Sight sensor: lets an NPC "see" PerceptibleComponent-tagged entities that
    // fall inside a forward cone (range + field of view), optionally gated by a
    // physics line-of-sight raycast. PerceptionSystem refreshes the runtime
    // result fields each tick from inside Scene::OnUpdateRuntime and mirrors
    // them into the entity's AI Blackboard under PerceptionKeys::*, so behavior
    // trees, FSMs, GOAP and scripts can all react. The look direction is the
    // entity's local -Z forward (engine convention; see EditorCamera/camera fly).
    struct PerceptionComponent
    {
        // --- Authored / serialized ---
        f32 SightRange = 15.0f;                     // max distance a target can be seen (metres)
        f32 FovDegrees = 90.0f;                     // full angular width of the sight cone
        glm::vec3 EyeOffset = { 0.0f, 1.7f, 0.0f }; // local-space eye position (eye height)
        bool RequireLineOfSight = true;             // when true, an occluded target is not seen
        i32 PerceiverTeam = 0;                      // this sensor's faction id
        bool DetectSameTeam = false;                // when false, same-team perceptibles are ignored

        // --- Runtime result (not serialized; recomputed every tick) ---
        OLO_SERIALIZE(Skip)
        bool HasVisibleTarget = false; // a target is currently visible this tick
        OLO_SERIALIZE(Skip)
        UUID VisibleTarget = 0; // UUID of the nearest visible target (0 = none)
        OLO_SERIALIZE(Skip)
        glm::vec3 LastKnownPosition = { 0.0f, 0.0f, 0.0f }; // where the target was last seen
        OLO_SERIALIZE(Skip)
        bool HasLastKnownPosition = false; // true once any target has been seen
        OLO_SERIALIZE(Skip)
        f32 TimeSinceLastSeen = 0.0f; // seconds since a target was last visible

        PerceptionComponent() = default;

        // Copy / move duplicate the authored config only; the runtime sensor
        // result is rebuilt by PerceptionSystem each tick. Mirrors
        // NavAgentComponent's authored-only copy semantics.
        PerceptionComponent(const PerceptionComponent& other)
            : SightRange(other.SightRange), FovDegrees(other.FovDegrees), EyeOffset(other.EyeOffset),
              RequireLineOfSight(other.RequireLineOfSight), PerceiverTeam(other.PerceiverTeam),
              DetectSameTeam(other.DetectSameTeam)
        {
        }
        PerceptionComponent(PerceptionComponent&&) noexcept = default;

        PerceptionComponent& operator=(const PerceptionComponent& other)
        {
            if (this != &other)
            {
                SightRange = other.SightRange;
                FovDegrees = other.FovDegrees;
                EyeOffset = other.EyeOffset;
                RequireLineOfSight = other.RequireLineOfSight;
                PerceiverTeam = other.PerceiverTeam;
                DetectSameTeam = other.DetectSameTeam;
                HasVisibleTarget = false;
                VisibleTarget = 0;
                LastKnownPosition = { 0.0f, 0.0f, 0.0f };
                HasLastKnownPosition = false;
                TimeSinceLastSeen = 0.0f;
            }
            return *this;
        }
        PerceptionComponent& operator=(PerceptionComponent&&) noexcept = default;

        ~PerceptionComponent() = default;

        // Compares serialized fields only — runtime sensor results are excluded
        // (they are tick-derived, not authoring-visible). Mirrors NavAgentComponent.
        auto operator==(const PerceptionComponent& other) const -> bool
        {
            const bool sameCone = Math::BitwiseEqual(SightRange, other.SightRange) &&
                                  Math::BitwiseEqual(FovDegrees, other.FovDegrees) &&
                                  Math::BitwiseEqual(EyeOffset, other.EyeOffset);
            const bool sameFilter = (RequireLineOfSight == other.RequireLineOfSight) &&
                                    (PerceiverTeam == other.PerceiverTeam) &&
                                    (DetectSameTeam == other.DetectSameTeam);
            return sameCone && sameFilter;
        }
    };

    // =========================================================================
    // BoidComponent — Reynolds flocking steering (issue #731).
    //
    // Separation / alignment / cohesion over the agent's neighbours, plus goal
    // seeking and obstacle avoidance. Neighbour queries go through the per-tick
    // FlockSpatialHash the steering system rebuilds (AI/Flocking/), never an
    // O(n^2) scan.
    //
    // The work is split across TWO scheduler nodes and that split is the whole
    // design (see FlockingSystem.h and the audit table in
    // Scene::GetGameplayScheduler):
    //   * "BoidSteering" is Parallelizable — it reads neighbour positions and
    //     writes only m_SteeringForce / m_NeighborCount on the agent's OWN
    //     component.
    //   * "BoidMovement" runs on the game thread — it integrates the force into
    //     m_Velocity and writes TransformComponent, which is exactly the thing
    //     that may not happen on a worker.
    //
    // m_Velocity is authored state, not runtime state: a scene can start a
    // flock already in motion, so it round-trips through scene YAML and the
    // save game (the "silently zeroed initial velocity still simulates
    // perfectly" trap from docs/agent-rules/force-model-vehicles.md).
    //
    // Deliberately a plain trivially-copyable aggregate of public fields:
    // OloHeaderTool generates the scene (de)serializer, the AllComponents entry
    // and the MCP field registry from it with no hand-written blocks. It does
    // define an operator== (see the bottom of the struct) — not because the
    // codegen needs one, but because the editor's byte-level undo comparison is
    // wrong for this component; the trait specialization in
    // SceneHierarchyPanel opts it onto the value path.
    // =========================================================================
    struct BoidComponent
    {
        // ── Motion limits ────────────────────────────────────────────────────
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_MaxSpeed = 6.0f;
        // Cap on the magnitude of the combined steering force (units/s^2).
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 10000.0f)
        f32 m_MaxForce = 12.0f;

        // ── Neighbourhood ────────────────────────────────────────────────────
        // Perception radius for alignment + cohesion. Also drives the spatial
        // hash's cell size (the flock's largest radius becomes the cell size),
        // so keeping this uniform across a flock keeps queries at 27 cells.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.01f, Max = 1000.0f)
        f32 m_NeighborRadius = 4.0f;
        // Personal-space radius; separation only acts inside it.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.01f, Max = 1000.0f)
        f32 m_SeparationRadius = 1.5f;
        // Hard cap on neighbours folded into one agent's steering. Bounds the
        // worst case when a flock stacks up in one cell. Note this keeps the
        // first N in the hash's (spatial) visit order, NOT the nearest N —
        // sorting by distance would cost more than the cap saves. That order is
        // deterministic, so the retained set is reproducible; it is simply not
        // the closest set, which only matters if you set this very low.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1u, Max = 4096u)
        u32 m_MaxNeighbors = 24;

        // ── Behaviour weights ────────────────────────────────────────────────
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 m_SeparationWeight = 1.5f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 m_AlignmentWeight = 1.0f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 m_CohesionWeight = 1.0f;

        // ── Goal seeking ─────────────────────────────────────────────────────
        // World-space point the flock is drawn towards. Weight 0 disables it;
        // scripts move m_GoalPosition to steer a flock around.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 m_GoalWeight = 0.5f;
        OLO_PROPERTY()
        glm::vec3 m_GoalPosition = { 0.0f, 0.0f, 0.0f };

        // ── Obstacle avoidance ───────────────────────────────────────────────
        // Agents steer away from every BoidObstacleComponent whose sphere comes
        // within m_ObstacleAvoidRadius of them, ramped by how deep the overlap
        // is.
        //
        // This is ISOTROPIC repulsion: strength depends on distance only, so an
        // obstacle directly behind an agent pushes exactly as hard as one dead
        // ahead. It is deliberately not a forward-projected probe — the field
        // is a radius, not a lookahead distance, and is named accordingly.
        // Velocity-aware avoidance (project along the heading, only react to
        // what is actually in the way) is a reasonable follow-up and would need
        // its own field rather than a silent reinterpretation of this one.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 100.0f)
        f32 m_ObstacleAvoidWeight = 2.0f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 m_ObstacleAvoidRadius = 3.0f;

        // ── Movement options ─────────────────────────────────────────────────
        // Confine steering + motion to the XZ plane (a ground herd / a 2D
        // shoal), mirroring NavAgentComponent::m_LockYAxis.
        OLO_PROPERTY()
        bool m_LockYAxis = false;
        // Rotate the entity to face its velocity. Off for agents whose
        // orientation is authored or driven by something else.
        OLO_PROPERTY()
        bool m_FaceVelocity = true;

        // ── Authored motion state ────────────────────────────────────────────
        // Serialized on purpose: a scene may author a flock already in motion.
        OLO_PROPERTY()
        glm::vec3 m_Velocity = { 0.0f, 0.0f, 0.0f };

        // ── Runtime (recomputed every tick, never persisted) ─────────────────
        // Written by BoidSteering on a worker, consumed by BoidMovement.
        OLO_SERIALIZE(Skip)
        glm::vec3 m_SteeringForce = { 0.0f, 0.0f, 0.0f };
        // Neighbours folded into the last steering solve — introspection hook
        // for tests and the editor ("is this agent actually flocking?").
        OLO_SERIALIZE(Skip)
        u32 m_NeighborCount = 0;

        // Authored fields only — the two Skip-tagged runtime fields above are
        // rewritten by BoidSteering every tick. The editor's undo tracking
        // snapshots a component on the idle→edit transition and compares it
        // FRAMES LATER, so a whole-struct compare would report "changed" on
        // every frame of a running simulation, latch its is-editing flag on,
        // and never push the undo entry at all. Same lesson as
        // TimeOfDayComponent / WeatherStateComponent; PreferValueComparison in
        // SceneHierarchyPanel opts this component into using this operator.
        //
        // This also keeps the editor off the byte-level memcmp path, which
        // would compare the two indeterminate padding bytes between
        // m_FaceVelocity and m_Velocity (SonarCloud cpp:S5000 — the
        // PerceptibleComponent / CloudscapeComponent case).
        //
        // m_Velocity IS compared: it is authored state the inspector exposes.
        // Unlike TimeOfDayComponent's clock it needs no drift gate, because the
        // flocking systems run only under SimulateRuntimeStep — never in edit
        // mode, which is where undo actually matters.
        auto operator==(const BoidComponent& other) const -> bool
        {
            const bool sameLimits = Math::BitwiseEqual(m_MaxSpeed, other.m_MaxSpeed) &&
                                    Math::BitwiseEqual(m_MaxForce, other.m_MaxForce);
            const bool sameNeighbourhood = Math::BitwiseEqual(m_NeighborRadius, other.m_NeighborRadius) &&
                                           Math::BitwiseEqual(m_SeparationRadius, other.m_SeparationRadius) &&
                                           (m_MaxNeighbors == other.m_MaxNeighbors);
            const bool sameWeights = Math::BitwiseEqual(m_SeparationWeight, other.m_SeparationWeight) &&
                                     Math::BitwiseEqual(m_AlignmentWeight, other.m_AlignmentWeight) &&
                                     Math::BitwiseEqual(m_CohesionWeight, other.m_CohesionWeight);
            const bool sameGoal = Math::BitwiseEqual(m_GoalWeight, other.m_GoalWeight) &&
                                  Math::BitwiseEqual(m_GoalPosition, other.m_GoalPosition);
            const bool sameObstacles = Math::BitwiseEqual(m_ObstacleAvoidWeight, other.m_ObstacleAvoidWeight) &&
                                       Math::BitwiseEqual(m_ObstacleAvoidRadius, other.m_ObstacleAvoidRadius);
            const bool sameMotion = (m_LockYAxis == other.m_LockYAxis) &&
                                    (m_FaceVelocity == other.m_FaceVelocity) &&
                                    Math::BitwiseEqual(m_Velocity, other.m_Velocity);
            return sameLimits && sameNeighbourhood && sameWeights && sameGoal && sameObstacles && sameMotion;
        }
    };

    // Spherical obstacle the flock steers around. Position comes from the
    // entity's TransformComponent; this only carries the radius. Deliberately
    // NOT tied to a physics collider — a flock's avoidance volume is usually
    // larger and softer than its collision shape, and a scene should be able to
    // place one without a rigid body.
    struct BoidObstacleComponent
    {
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.01f, Max = 10000.0f)
        f32 m_Radius = 1.0f;
    };
} // namespace OloEngine
