#pragma once

#include "SoundGraphPrototype.h"
#include "SoundGraph.h"
#include "OloEngine/Core/Ref.h"

#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	struct GraphGeneratorOptions
	{
		std::string Name;
		u32 NumInChannels;
		u32 NumOutChannels;
		Ref<Prototype> GraphPrototype;
		// Note: Editor model and cache dependencies removed for now
	};

	Ref<Prototype> ConstructPrototype(GraphGeneratorOptions options, std::vector<UUID>& waveAssetsToLoad);

	/** Create instance of SoundGraph from Prototype for playback */
	Ref<SoundGraph> CreateInstance(const Ref<Prototype>& prototype);

} // namespace OloEngine::Audio::SoundGraph