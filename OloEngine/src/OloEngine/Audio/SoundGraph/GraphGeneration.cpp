#include "OloEnginePCH.h"
#include "GraphGeneration.h"

#include "NodeProcessor.h"
#include "SoundGraphFactory.h"
#include "SoundGraphPrototype.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	struct GraphGenerator
	{
		GraphGenerator(GraphGeneratorOptions& options, Ref<Prototype>& outPrototype)
			: Options(options), OutPrototype(outPrototype)
		{
			OLO_CORE_ASSERT(Options.GraphPrototype);
			OLO_CORE_ASSERT(OutPrototype);
		}

		//==============================================================================
		GraphGeneratorOptions& Options;
		Ref<Prototype> OutPrototype;
		std::vector<UUID> OutWaveAssets;

		//==============================================================================
		bool Run()
		{
			ConstructIO();
			ParseNodes();
			
			if (OutPrototype->Nodes.empty())
			{
				OLO_CORE_ERROR("GraphGenerator: No valid nodes found in prototype");
				return false;
			}

			ParseConnections();
			ParseWaveReferences();

			return true;
		}

		//==============================================================================
		void ConstructIO()
		{
			// Set up graph inputs and outputs based on channel count
			for (u32 i = 0; i < Options.NumInChannels; ++i)
			{
				std::string inputName = "In" + std::to_string(i);
				if (i == 0) inputName = "InLeft";
				else if (i == 1) inputName = "InRight";
				
				Prototype::Endpoint input(Identifier(inputName), Value(0.0f));
				OutPrototype->Inputs.push_back(input);
			}

			for (u32 i = 0; i < Options.NumOutChannels; ++i)
			{
				std::string outputName = "Out" + std::to_string(i);
				if (i == 0) outputName = "OutLeft";
				else if (i == 1) outputName = "OutRight";
				
				Prototype::Endpoint output(Identifier(outputName), Value(0.0f));
				OutPrototype->Outputs.push_back(output);
				OutPrototype->OutputChannelIDs.push_back(Identifier(outputName));
			}

			// Add standard graph events
			OutPrototype->Inputs.emplace_back(Identifier("Play"), Value(0.0f));
			OutPrototype->Outputs.emplace_back(Identifier("OnFinished"), Value(0.0f));
		}

		void ParseNodes()
		{
			// Copy nodes from the source prototype to the output prototype
			OutPrototype->Nodes = Options.GraphPrototype->Nodes;
			
			// Validate that all node types are supported by our factory
			for (const auto& node : OutPrototype->Nodes)
			{
				if (!Factory::Contains(node.NodeTypeID))
				{
					OLO_CORE_WARN("GraphGenerator: Unsupported node type: {}", node.NodeTypeID.GetString());
				}
			}
		}

		void ParseConnections()
		{
			// Copy connections from the source prototype
			OutPrototype->Connections = Options.GraphPrototype->Connections;
			
			// TODO: Validate connections are valid
			OLO_CORE_INFO("GraphGenerator: Parsed {} connections", OutPrototype->Connections.size());
		}

		void ParseWaveReferences()
		{
			// Scan through nodes and collect any wave asset references
			// For now, this is a placeholder - would need to examine node default values
			// for asset handles and collect them
			
			OutWaveAssets.clear();
			// TODO: Implement wave asset collection from node default values
		}
	};

	//==============================================================================
	Ref<Prototype> ConstructPrototype(GraphGeneratorOptions options, std::vector<UUID>& waveAssetsToLoad)
	{
		auto prototype = CreateRef<Prototype>();
		prototype->DebugName = options.Name;
		prototype->ID = UUID();

		GraphGenerator generator(options, prototype);
		
		if (!generator.Run())
		{
			OLO_CORE_ERROR("Failed to construct graph prototype: {}", options.Name);
			return nullptr;
		}

		waveAssetsToLoad = std::move(generator.OutWaveAssets);
		return prototype;
	}

	Ref<SoundGraph> CreateInstance(const Ref<Prototype>& prototype)
	{
		if (!prototype)
		{
			OLO_CORE_ERROR("Cannot create SoundGraph instance from null prototype");
			return nullptr;
		}

		auto graph = CreateRef<SoundGraph>(prototype->DebugName, prototype->ID);
		
		// Create all nodes
		for (const auto& nodeDesc : prototype->Nodes)
		{
			auto node = Factory::Create(nodeDesc.NodeTypeID, nodeDesc.ID);
			if (!node)
			{
				OLO_CORE_ERROR("Failed to create node of type: {}", nodeDesc.NodeTypeID.GetString());
				continue;
			}

			// TODO: Apply default value plugs to the node
			
			graph->AddNode(Scope<NodeProcessor>(node));
		}

		// TODO: Establish connections between nodes based on prototype->Connections
		// TODO: Set up graph inputs/outputs based on prototype->Inputs/Outputs
		
		OLO_CORE_INFO("Created SoundGraph instance '{}' with {} nodes", 
					  prototype->DebugName, prototype->Nodes.size());

		return graph;
	}

} // namespace OloEngine::Audio::SoundGraph