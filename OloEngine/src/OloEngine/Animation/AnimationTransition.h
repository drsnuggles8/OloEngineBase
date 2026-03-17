#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include <string>
#include <vector>

namespace OloEngine
{
	struct TransitionCondition
	{
		std::string ParameterName;

		enum class Comparison : u8
		{
			Greater,
			Less,
			Equal,
			NotEqual,
			TriggerSet
		};

		Comparison Op = Comparison::Greater;
		f32 FloatThreshold = 0.0f;
		i32 IntThreshold = 0;
		bool BoolValue = false;

		[[nodiscard]] bool Evaluate(const AnimationParameterSet& params) const;
	};

	struct AnimationTransition
	{
		std::string SourceState;       // "" = Any State transition
		std::string DestinationState;

		std::vector<TransitionCondition> Conditions; // ALL must be true (AND logic)

		f32 BlendDuration = 0.2f;      // Cross-fade duration in seconds
		f32 ExitTime = -1.0f;          // -1 = transition any time, 0-1 = wait until normalized time
		bool HasExitTime = false;
		bool CanTransitionToSelf = false;

		[[nodiscard]] bool Evaluate(const AnimationParameterSet& params) const;
	};
} // namespace OloEngine
