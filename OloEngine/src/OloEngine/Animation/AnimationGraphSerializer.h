#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphAsset.h"
#include <string>

namespace OloEngine
{
	class AnimationGraphSerializer
	{
	public:
		static bool Serialize(const Ref<AnimationGraph>& graph, const std::string& filepath);
		static Ref<AnimationGraph> Deserialize(const std::string& filepath);

		static bool SerializeAsset(const Ref<AnimationGraphAsset>& asset, const std::string& filepath);
		static Ref<AnimationGraphAsset> DeserializeAsset(const std::string& filepath);
	};
} // namespace OloEngine
