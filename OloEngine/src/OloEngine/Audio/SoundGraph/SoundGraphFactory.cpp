#include "OloEnginePCH.h"
#include "SoundGraphFactory.h"

#include "Nodes/WavePlayer.h"
// Additional node includes will go here as we add them

#include <memory>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	using Registry = std::unordered_map<Identifier, std::function<std::unique_ptr<NodeProcessor>(UUID nodeID)>>;

	// Simple factory registration - we'll expand this as we add more nodes
	static const Registry NodeProcessors
	{
		// Wave player node
		{ Identifier("WavePlayer"), [](UUID nodeID) { return std::make_unique<WavePlayer>("WavePlayer", nodeID); } },
		
		// Math nodes (placeholder - will add when we create MathNodes.h)
		// { Identifier("Add"), [](UUID nodeID) { return std::make_unique<AddNode>("Add", nodeID); } },
		// { Identifier("Multiply"), [](UUID nodeID) { return std::make_unique<MultiplyNode>("Multiply", nodeID); } },
		
		// Generator nodes (placeholder - will add when we create GeneratorNodes.h)
		// { Identifier("Sine"), [](UUID nodeID) { return std::make_unique<SineNode>("Sine", nodeID); } },
		// { Identifier("Noise"), [](UUID nodeID) { return std::make_unique<NoiseNode>("Noise", nodeID); } },
	};

	//==============================================================================
	std::unique_ptr<NodeProcessor> Factory::Create(Identifier nodeTypeID, UUID nodeID)
	{
		auto it = NodeProcessors.find(nodeTypeID);
		if (it == NodeProcessors.end())
		{
			OLO_CORE_ERROR("SoundGraph::Factory::Create - Node with type ID is not in the registry");
			return nullptr;
		}

		return it->second(nodeID);
	}

	bool Factory::Contains(Identifier nodeTypeID)
	{
		return NodeProcessors.count(nodeTypeID);
	}

} // namespace OloEngine::Audio::SoundGraph