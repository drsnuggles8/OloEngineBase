#include "OloEnginePCH.h"
#include "GraphGeneration.h"

#include "NodeProcessor.h"
#include "SoundGraphFactory.h"
#include "SoundGraphPrototype.h"
#include "SoundGraph.h"

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
			GenerateChannelIdentifiers();
		}

		//==============================================================================
		GraphGeneratorOptions& Options;
		Ref<Prototype> OutPrototype;
		std::vector<UUID> OutWaveAssets;
		std::vector<Identifier> m_OutputChannelIdentifiers; // Generated channel IDs for reuse

		void GenerateChannelIdentifiers()
		{
			m_OutputChannelIdentifiers.reserve(Options.NumOutChannels);
			for (u32 i = 0; i < Options.NumOutChannels; ++i)
			{
				if (i == 0)
					m_OutputChannelIdentifiers.push_back(SoundGraph::IDs::OutLeft);
				else if (i == 1)
					m_OutputChannelIdentifiers.push_back(SoundGraph::IDs::OutRight);
				else
				{
					// Generate unique channel identifier for channels beyond stereo
					std::string channelName = "Out" + std::to_string(i);
					m_OutputChannelIdentifiers.push_back(Identifier(channelName));
				}
			}
		}

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
			Identifier outputID = m_OutputChannelIdentifiers[i];
			
			Prototype::Endpoint output(outputID, choc::value::Value(0.0f));
			OutPrototype->Outputs.push_back(output);
		}			// Add standard graph events
			OutPrototype->Inputs.emplace_back(Identifier("Play"), choc::value::Value(0.0f));
			OutPrototype->Outputs.emplace_back(Identifier("OnFinished"), choc::value::Value(0.0f));
		}

		void ParseNodes()
		{
			// Copy nodes from the source prototype to the output prototype
			OutPrototype->Nodes = Options.GraphPrototype->Nodes;
			
			// Copy local variable plugs so they are available for CreateInstance step 2
			OutPrototype->LocalVariablePlugs = Options.GraphPrototype->LocalVariablePlugs;
			
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
			// Validate and copy connections from the source prototype
			OutPrototype->Connections.clear();
			
			if (!Options.GraphPrototype)
			{
				OLO_CORE_WARN("GraphGenerator: No graph prototype provided for connection parsing");
				return;
			}
			
			size_t validConnections = 0;
			size_t invalidConnections = 0;
			
			for (const auto& connection : Options.GraphPrototype->Connections)
			{
				// Validate connection endpoints are not empty
				if (!connection.Source.EndpointID.IsValid())
				{
					OLO_CORE_WARN("GraphGenerator: Connection has empty source endpoint");
					invalidConnections++;
					continue;
				}
				
				if (!connection.Destination.EndpointID.IsValid())
				{
					OLO_CORE_WARN("GraphGenerator: Connection has empty destination endpoint");
					invalidConnections++;
					continue;
				}
				
				// Validate source and destination nodes exist in prototype
				bool sourceNodeExists = false;
				bool destinationNodeExists = false;
				
				for (const auto& node : Options.GraphPrototype->Nodes)
				{
					if (node.ID == connection.Source.NodeID)
						sourceNodeExists = true;
					if (node.ID == connection.Destination.NodeID)
						destinationNodeExists = true;
				}
				
				if (!sourceNodeExists)
				{
					OLO_CORE_WARN("GraphGenerator: Connection references non-existent source node {}", static_cast<u64>(connection.Source.NodeID));
					invalidConnections++;
					continue;
				}
				
				if (!destinationNodeExists)
				{
					OLO_CORE_WARN("GraphGenerator: Connection references non-existent destination node {}", static_cast<u64>(connection.Destination.NodeID));
					invalidConnections++;
					continue;
				}
				
				// Validate connection type is valid
				if (connection.Type < Prototype::Connection::NodeValue_NodeValue || 
					connection.Type > Prototype::Connection::LocalVariable_NodeValue)
				{
					OLO_CORE_WARN("GraphGenerator: Connection has invalid connection type {}", static_cast<int>(connection.Type));
					invalidConnections++;
					continue;
				}
				
				// Validate that event connections only connect to event endpoints
				bool isEventConnection = (connection.Type == Prototype::Connection::NodeEvent_NodeEvent ||
										connection.Type == Prototype::Connection::GraphEvent_NodeEvent ||
										connection.Type == Prototype::Connection::NodeEvent_GraphEvent);
				
				bool isValueConnection = (connection.Type == Prototype::Connection::NodeValue_NodeValue ||
										connection.Type == Prototype::Connection::GraphValue_NodeValue ||
										connection.Type == Prototype::Connection::NodeValue_GraphValue ||
										connection.Type == Prototype::Connection::LocalVariable_NodeValue);
				
				if (!isEventConnection && !isValueConnection)
				{
					OLO_CORE_WARN("GraphGenerator: Connection type {} does not match event or value pattern", static_cast<int>(connection.Type));
					invalidConnections++;
					continue;
				}
				
				// Basic endpoint compatibility validation
				// For now, we assume same-type connections are compatible
				// More sophisticated type checking would require endpoint type information
				if (isEventConnection)
				{
					OLO_CORE_TRACE("GraphGenerator: Validated event connection from endpoint {} to {}", 
						static_cast<u64>(connection.Source.EndpointID), static_cast<u64>(connection.Destination.EndpointID));
				}
				else if (isValueConnection)
				{
					OLO_CORE_TRACE("GraphGenerator: Validated value connection from endpoint {} to {}", 
						static_cast<u64>(connection.Source.EndpointID), static_cast<u64>(connection.Destination.EndpointID));
				}
				
				// Connection is valid, add to output prototype
				OutPrototype->Connections.push_back(connection);
				validConnections++;
			}
			
			OLO_CORE_INFO("GraphGenerator: Validated {} connections ({} valid, {} invalid)", 
				Options.GraphPrototype->Connections.size(), validConnections, invalidConnections);
		}

		void ParseWaveReferences()
		{
			// Scan through nodes and collect any wave asset references
			OutWaveAssets.clear();
			
			for (const auto& node : Options.GraphPrototype->Nodes)
			{
				// Check each default value plug for potential asset handles
				for (const auto& plug : node.DefaultValuePlugs)
				{
					// Asset handles are stored as int64 values
					if (plug.DefaultValue.getType().isInt64())
					{
						int64_t assetHandle = plug.DefaultValue.getInt64();
						if (assetHandle != 0) // Non-zero indicates a valid asset handle
						{
							UUID assetUUID = static_cast<UUID>(assetHandle);
							if (std::find(OutWaveAssets.begin(), OutWaveAssets.end(), assetUUID) == OutWaveAssets.end())
							{
								OutWaveAssets.push_back(assetUUID);
								OLO_CORE_TRACE("GraphGenerator: Found wave asset reference: {}", assetUUID);
							}
						}
					}
				}
			}
			
			OLO_CORE_INFO("GraphGenerator: Collected {} wave asset references", OutWaveAssets.size());
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

		// Set output channel IDs from the prototype outputs
		graph->OutputChannelIDs.clear();
		graph->OutputChannelIDs.reserve(prototype->Outputs.size());
		
		// Filter outputs to only include channel outputs (exclude events like "OnFinished")
		for (const auto& output : prototype->Outputs)
		{
			// Skip event outputs - only include audio channel outputs
			if (output.EndpointID != Identifier("OnFinished"))
			{
				graph->OutputChannelIDs.push_back(output.EndpointID);
			}
		}

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