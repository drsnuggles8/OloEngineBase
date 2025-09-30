#pragma once

#include "NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/UUID.h"

#include <memory>

namespace OloEngine::Audio::SoundGraph
{
	class Factory
	{
	public:
		[[nodiscard]] static std::unique_ptr<NodeProcessor> Create(Identifier nodeTypeID, UUID nodeID);
		static bool Contains(Identifier nodeTypeID);

	private:
		Factory() = delete;
	};

} // namespace OloEngine::Audio::SoundGraph