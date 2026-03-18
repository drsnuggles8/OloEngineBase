#include "OloEnginePCH.h"
#include "State.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	void FSMState::OnEnter([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard) {}
	void FSMState::OnUpdate([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard, [[maybe_unused]] f32 dt) {}
	void FSMState::OnExit([[maybe_unused]] Entity entity, [[maybe_unused]] BTBlackboard& blackboard) {}
} // namespace OloEngine
