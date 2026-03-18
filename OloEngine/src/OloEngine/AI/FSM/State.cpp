#include "OloEnginePCH.h"
#include "State.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    void FSMState::OnEnter([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard)
    {
        OLO_PROFILE_FUNCTION();
    }

    void FSMState::OnUpdate([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard, [[maybe_unused]] f32 dt)
    {
        OLO_PROFILE_FUNCTION();
    }

    void FSMState::OnExit([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard)
    {
        OLO_PROFILE_FUNCTION();
    }
} // namespace OloEngine
