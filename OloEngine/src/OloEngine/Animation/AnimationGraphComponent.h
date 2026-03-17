#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationParameter.h"

namespace OloEngine
{
	struct AnimationGraphComponent
	{
		AssetHandle AnimationGraphAssetHandle = 0;

		// Runtime
		Ref<AnimationGraph> RuntimeGraph;

		// Per-entity parameter instance (copied from graph at start)
		AnimationParameterSet Parameters;

		AnimationGraphComponent() = default;
	};
} // namespace OloEngine
