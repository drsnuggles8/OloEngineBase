#include "OloEnginePCH.h"
#include "SoundGraphFactory.h"

// #include "Nodes/WavePlayerNode.h"
// Additional node includes will go here as we add them

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	using Registry = std::unordered_map<Identifier, std::function<NodeProcessor*(UUID nodeID)>>;

	// TODO(olbu): Expand this registry as we add more node types
	// Simple factory registration - we'll expand this as we add more nodes
	static const Registry NodeProcessors
	{
		// Wave player node - commented out until WavePlayerNode.h is available
		// { Identifier("WavePlayer"), [](UUID nodeID) { return new WavePlayerNode("WavePlayer", nodeID); } },
		
		// Math nodes (placeholder - will add when we create MathNodes.h)
		// { Identifier("Add"), [](UUID nodeID) { return new AddNode("Add", nodeID); } },
		// { Identifier("Multiply"), [](UUID nodeID) { return new MultiplyNode("Multiply", nodeID); } },
		
		// Generator nodes (placeholder - will add when we create GeneratorNodes.h)
		// { Identifier("Sine"), [](UUID nodeID) { return new SineNode("Sine", nodeID); } },
		// { Identifier("Noise"), [](UUID nodeID) { return new NoiseNode("Noise", nodeID); } },
	};

	//==============================================================================
	NodeProcessor* Factory::Create(Identifier nodeTypeID, UUID nodeID)
	{
		if (!NodeProcessors.count(nodeTypeID))
		{
			OLO_CORE_ERROR("SoundGraph::Factory::Create - Node with type ID '{}' is not in the registry", nodeTypeID.GetHash());
			return nullptr;
		}

		return NodeProcessors.at(nodeTypeID)(nodeID);
	}

	bool Factory::Contains(Identifier nodeTypeID)
	{
		return NodeProcessors.count(nodeTypeID);
	}

} // namespace OloEngine::Audio::SoundGraph