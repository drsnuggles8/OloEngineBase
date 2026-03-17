#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include <string>
#include <vector>

namespace OloEngine
{
	struct AnimationLayer
	{
		std::string Name;
		Ref<AnimationStateMachine> StateMachine;

		enum class BlendMode : u8
		{
			Override,
			Additive
		};

		BlendMode Mode = BlendMode::Override;
		f32 Weight = 1.0f;

		// Bone mask: which bones this layer affects (empty = all bones)
		std::vector<std::string> AffectedBones;

		// Avatar mask name
		std::string AvatarMask;
	};
} // namespace OloEngine
