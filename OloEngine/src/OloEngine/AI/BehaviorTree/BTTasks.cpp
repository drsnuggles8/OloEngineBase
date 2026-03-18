#include "OloEnginePCH.h"
#include "BTTasks.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"

namespace OloEngine
{
    // --- BTWait ---

    BTStatus BTWait::Tick(f32 dt, [[maybe_unused]] BTBlackboard& blackboard, [[maybe_unused]] Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        m_Elapsed += dt;
        if (m_Elapsed >= Duration)
        {
            m_Elapsed = 0.0f;
            return BTStatus::Success;
        }
        return BTStatus::Running;
    }

    void BTWait::Reset()
    {
        m_Elapsed = 0.0f;
    }

    // --- BTSetBlackboardValue ---

    BTStatus BTSetBlackboardValue::Tick([[maybe_unused]] f32 dt, BTBlackboard& blackboard, [[maybe_unused]] Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        blackboard.Set(Key, ValueToSet);
        return BTStatus::Success;
    }

    // --- BTLog ---

    BTStatus BTLog::Tick([[maybe_unused]] f32 dt, [[maybe_unused]] BTBlackboard& blackboard, [[maybe_unused]] Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("[BehaviorTree] {}", Message);
        return BTStatus::Success;
    }

    // --- BTCheckBlackboardKey ---

    BTStatus BTCheckBlackboardKey::Tick([[maybe_unused]] f32 dt, BTBlackboard& blackboard, [[maybe_unused]] Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (!blackboard.Has(Key))
            return BTStatus::Failure;

        if (ExpectedValue.has_value())
        {
            auto actual = blackboard.GetRaw(Key);
            if (!actual.has_value())
            {
                return BTStatus::Failure;
            }
            return (actual.value() == ExpectedValue.value()) ? BTStatus::Success : BTStatus::Failure;
        }

        return BTStatus::Success;
    }
    // --- BTMoveTo ---

    BTStatus BTMoveTo::Tick([[maybe_unused]] f32 dt, BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<NavAgentComponent>())
        {
            OLO_CORE_WARN("[BehaviorTree] BTMoveTo: Entity has no NavAgentComponent");
            return BTStatus::Failure;
        }

        auto& nav = entity.GetComponent<NavAgentComponent>();

        // Set target from blackboard if we don't have one yet
        if (!nav.m_HasTarget)
        {
            if (!blackboard.Has(TargetBlackboardKey))
            {
                return BTStatus::Failure;
            }

            auto target = blackboard.Get<glm::vec3>(TargetBlackboardKey);
            nav.m_TargetPosition = target;
            nav.m_HasTarget = true;
        }

        // Check if agent reached destination
        auto& transform = entity.GetComponent<TransformComponent>();
        f32 dist = glm::length(transform.Translation - nav.m_TargetPosition);
        if (dist <= nav.m_StoppingDistance)
        {
            nav.m_HasTarget = false;
            return BTStatus::Success;
        }

        return BTStatus::Running;
    }

    // --- BTPlayAnimation ---

    BTStatus BTPlayAnimation::Tick([[maybe_unused]] f32 dt, [[maybe_unused]] BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<AnimationGraphComponent>())
        {
            OLO_CORE_WARN("[BehaviorTree] BTPlayAnimation: Entity has no AnimationGraphComponent");
            return BTStatus::Failure;
        }

        auto& animGraph = entity.GetComponent<AnimationGraphComponent>();
        animGraph.Parameters.SetTrigger(AnimationName);
        return BTStatus::Success;
    }
} // namespace OloEngine
