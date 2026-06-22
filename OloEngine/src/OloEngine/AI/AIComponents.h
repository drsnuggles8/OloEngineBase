#pragma once

#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    struct BehaviorTreeComponent
    {
        AssetHandle BehaviorTreeAssetHandle = 0;
        BTBlackboard Blackboard;

        // Runtime (not serialized)
        Ref<BehaviorTree> RuntimeTree = nullptr;
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
        AssetHandle StateMachineAssetHandle = 0;
        BTBlackboard Blackboard;

        // Runtime (not serialized)
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
        BTBlackboard Blackboard; // sensor/script bridge, mirrors BT/FSM

        // Runtime (not serialized); rebuilt after load by gameplay code.
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

        auto operator==(const PerceptibleComponent& other) const -> bool
        {
            return Team == other.Team && IsPerceptible == other.IsPerceptible;
        }
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
        bool HasVisibleTarget = false;                      // a target is currently visible this tick
        UUID VisibleTarget = 0;                             // UUID of the nearest visible target (0 = none)
        glm::vec3 LastKnownPosition = { 0.0f, 0.0f, 0.0f }; // where the target was last seen
        bool HasLastKnownPosition = false;                  // true once any target has been seen
        f32 TimeSinceLastSeen = 0.0f;                       // seconds since a target was last visible

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
        PerceptionComponent(PerceptionComponent&&) noexcept = default;
        PerceptionComponent& operator=(PerceptionComponent&&) noexcept = default;

        // Compares serialized fields only — runtime sensor results are excluded
        // (they are tick-derived, not authoring-visible). Mirrors NavAgentComponent.
        auto operator==(const PerceptionComponent& other) const -> bool
        {
            return Math::BitwiseEqual(SightRange, other.SightRange) && Math::BitwiseEqual(FovDegrees, other.FovDegrees) && Math::BitwiseEqual(EyeOffset, other.EyeOffset) && RequireLineOfSight == other.RequireLineOfSight && PerceiverTeam == other.PerceiverTeam && DetectSameTeam == other.DetectSameTeam;
        }
    };
} // namespace OloEngine
