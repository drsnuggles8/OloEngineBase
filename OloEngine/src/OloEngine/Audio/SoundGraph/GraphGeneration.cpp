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
				
				Prototype::Endpoint input(Identifier(inputName), choc::value::Value(0.0f));
				OutPrototype->Inputs.push_back(input);
			}

			for (u32 i = 0; i < Options.NumOutChannels; ++i)
			{
				std::string outputName = "Out" + std::to_string(i);
				if (i == 0) outputName = "OutLeft";
				else if (i == 1) outputName = "OutRight";
				
				Prototype::Endpoint output(Identifier(outputName), choc::value::Value(0.0f));
				OutPrototype->Outputs.push_back(output);
				OutPrototype->OutputChannelIDs.push_back(Identifier(outputName));
			}

			// Add standard graph events
			OutPrototype->Inputs.emplace_back(Identifier("Play"), choc::value::Value(0.0f));
			OutPrototype->Outputs.emplace_back(Identifier("OnFinished"), choc::value::Value(0.0f));
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
					OLO_CORE_WARN("GraphGenerator: Unsupported node type: {}", node.NodeTypeID.GetHash());
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
		auto prototype = Ref<Prototype>::Create();
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

		auto graph = Ref<SoundGraph>::Create(prototype->DebugName, prototype->ID);
		
		//==============================================================================
		// Step 1: Set up graph inputs and outputs
		
		for (const auto& input : prototype->Inputs)
		{
			choc::value::Value copyValue = input.DefaultValue;
			graph->AddGraphInputStream(input.EndpointID, std::move(copyValue));
		}

		for (const auto& output : prototype->Outputs)
		{
			graph->AddGraphOutputStream(output.EndpointID);
		}

		// Set output channel IDs
		graph->OutputChannelIDs = prototype->OutputChannelIDs;

		//==============================================================================
		// Step 2: Set up local variables
		
		for (const auto& localVar : prototype->LocalVariablePlugs)
		{
			choc::value::Value copyValue = localVar.DefaultValue;
			graph->AddLocalVariableStream(localVar.EndpointID, std::move(copyValue));
		}

		//==============================================================================
		// Step 3: Create all nodes
		
		for (const auto& nodeDesc : prototype->Nodes)
		{
			auto node = Factory::Create(nodeDesc.NodeTypeID, nodeDesc.ID);
			if (!node)
			{
				OLO_CORE_ERROR("Failed to create node of type: {}", nodeDesc.NodeTypeID.GetHash());
				continue;
			}

			// Apply default value plugs to the node
			for (const auto& defaultPlug : nodeDesc.DefaultValuePlugs)
			{
				// Find the corresponding input stream in the node and set default value
				auto inputIt = node->InputStreams.find(defaultPlug.EndpointID);
				if (inputIt != node->InputStreams.end())
				{
					// Create a default value plug for this input
					auto defaultValuePlug = CreateScope<StreamWriter>(
						inputIt->second, 
						defaultPlug.DefaultValue, 
						defaultPlug.EndpointID
					);
					
					node->DefaultValuePlugs.push_back(std::move(defaultValuePlug));
				}
			}
			
			graph->AddNode(Scope<NodeProcessor>(node));
		}

		//==============================================================================
		// Step 4: Establish all connections between nodes
		
		for (const auto& connection : prototype->Connections)
		{
			bool success = false;
			
			switch (connection.Type)
			{
				case Prototype::Connection::NodeValue_NodeValue:
					success = graph->AddValueConnection(
						connection.Source.NodeID, 
						connection.Source.EndpointID,
						connection.Destination.NodeID, 
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::NodeEvent_NodeEvent:
					success = graph->AddEventConnection(
						connection.Source.NodeID, 
						connection.Source.EndpointID,
						connection.Destination.NodeID, 
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::GraphValue_NodeValue:
					success = graph->AddInputValueRoute(
						connection.Source.EndpointID,
						connection.Destination.NodeID, 
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::GraphEvent_NodeEvent:
					success = graph->AddInputEventsRoute(
						connection.Source.EndpointID,
						connection.Destination.NodeID, 
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::NodeValue_GraphValue:
					success = graph->AddToGraphOutputConnection(
						connection.Source.NodeID, 
						connection.Source.EndpointID,
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::NodeEvent_GraphEvent:
					success = graph->AddToGraphOutEventConnection(
						connection.Source.NodeID, 
						connection.Source.EndpointID,
						connection.Destination.EndpointID
					);
					break;

				case Prototype::Connection::LocalVariable_NodeValue:
					success = graph->AddLocalVariableRoute(
						connection.Source.EndpointID,
						connection.Destination.NodeID, 
						connection.Destination.EndpointID
					);
					break;

				default:
					OLO_CORE_ERROR("Unknown connection type: {}", (int)connection.Type);
					break;
			}

			if (!success)
			{
				OLO_CORE_WARN("Failed to establish connection from {}:{} to {}:{}",
					connection.Source.NodeID, connection.Source.EndpointID.GetHash(),
					connection.Destination.NodeID, connection.Destination.EndpointID.GetHash());
			}
		}

		//==============================================================================
		// Step 5: Initialize the graph
		
		graph->Init();
		
		OLO_CORE_INFO("Created SoundGraph instance '{}' with {} nodes and {} connections", 
					  prototype->DebugName, prototype->Nodes.size(), prototype->Connections.size());

		return graph;
	}

} // namespace OloEngine::Audio::SoundGraph