#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    // Well-known AI Blackboard keys the PerceptionSystem writes each tick into
    // the perceiver's BehaviorTreeComponent / StateMachineComponent /
    // GoapAgentComponent blackboard. Behavior-tree query nodes (e.g.
    // BTCanSeeTarget), FSM transition predicates, GOAP sensors and Lua scripts
    // read these to react to what the NPC currently sees. The values mirror the
    // runtime result fields on PerceptionComponent.
    namespace PerceptionKeys
    {
        inline constexpr const char* CanSeeTarget = "Perception.CanSeeTarget";           // bool
        inline constexpr const char* Target = "Perception.Target";                       // UUID (present only while visible)
        inline constexpr const char* LastKnownPosition = "Perception.LastKnownPosition"; // glm::vec3
    } // namespace PerceptionKeys

    // Sight-perception sensor pass. For every entity carrying a
    // PerceptionComponent it scans all PerceptibleComponent entities and marks
    // the nearest one that falls inside the perceiver's range + field-of-view
    // cone and (optionally) has clear physics line-of-sight. The result is
    // stored on the PerceptionComponent and mirrored into the entity's AI
    // blackboard(s). Driven from Scene::OnUpdateRuntime ahead of AISystem so the
    // behavior tree / FSM / GOAP tick sees fresh sensor data the same frame.
    class PerceptionSystem
    {
      public:
        static void OnUpdate(Scene* scene, f32 dt);
    };
} // namespace OloEngine
