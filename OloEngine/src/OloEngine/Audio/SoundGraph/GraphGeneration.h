#pragma once

#include "SoundGraphPrototype.h"
#include "SoundGraph.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	struct GraphGeneratorOptions
	{
		std::string m_Name;
		u32 m_NumInChannels;
		u32 m_NumOutChannels;
		Ref<Prototype> m_GraphPrototype;
		// Note: Editor model and cache dependencies removed for now
	};

	Ref<Prototype> ConstructPrototype(GraphGeneratorOptions options, std::vector<UUID>& waveAssetsToLoad);

	/** Create instance of SoundGraph from Prototype for playback */
	Ref<SoundGraph> CreateInstance(const Ref<Prototype>& prototype);

} // namespace OloEngine::Audio::SoundGraph