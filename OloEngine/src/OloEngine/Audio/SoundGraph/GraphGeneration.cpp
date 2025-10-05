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
        GraphGenerator(const GraphGeneratorOptions& options, Ref<Prototype>& outPrototype)
            : m_Options(options), m_OutPrototype(outPrototype)
        {
            OLO_CORE_ASSERT(m_Options.m_GraphPrototype);
            OLO_CORE_ASSERT(m_OutPrototype);
            GenerateChannelIdentifiers();
        }

        //==============================================================================
        const GraphGeneratorOptions& m_Options;
        Ref<Prototype> m_OutPrototype;
        std::vector<UUID> m_OutWaveAssets;
        std::vector<Identifier> m_OutputChannelIdentifiers; // Generated channel IDs for reuse

        void GenerateChannelIdentifiers()
        {
            m_OutputChannelIdentifiers.reserve(m_Options.m_NumOutChannels);
            for (u32 i = 0; i < m_Options.m_NumOutChannels; ++i)
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
            OLO_PROFILE_FUNCTION();

            ConstructIO();
            ParseNodes();
            
            if (m_OutPrototype->m_Nodes.empty())
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
            for (u32 i = 0; i < m_Options.m_NumInChannels; ++i)
            {
                std::string inputName;
                if (i == 0)
                    inputName = "InLeft";
                else if (i == 1)
                    inputName = "InRight";
                else
                    inputName = "In" + std::to_string(i);
                
                Prototype::Endpoint input(Identifier(inputName), choc::value::Value(0.0f));
                m_OutPrototype->m_Inputs.push_back(input);
            }

            for (u32 i = 0; i < m_Options.m_NumOutChannels; ++i)
            {
                Identifier outputID = m_OutputChannelIdentifiers[i];
                
                Prototype::Endpoint output(outputID, choc::value::Value(0.0f));
                m_OutPrototype->m_Outputs.push_back(output);
            }
            
            // Add standard graph events
            m_OutPrototype->m_Inputs.emplace_back(Identifier("Play"), choc::value::Value(0.0f));
            m_OutPrototype->m_Outputs.emplace_back(SoundGraph::IDs::OnFinished, choc::value::Value(0.0f));
        }

        void ParseNodes()
        {
            OLO_PROFILE_FUNCTION();
            
            // Copy nodes from the source prototype to the output prototype
            m_OutPrototype->m_Nodes = m_Options.m_GraphPrototype->m_Nodes;
            
            // Copy local variable plugs so they are available for CreateInstance step 2
            m_OutPrototype->m_LocalVariablePlugs = m_Options.m_GraphPrototype->m_LocalVariablePlugs;
            
            // Validate that all node types are supported by our factory
            for (const auto& node : m_OutPrototype->m_Nodes)
            {
                if (!Factory::Contains(node.m_NodeTypeID))
                {
                    OLO_CORE_WARN("GraphGenerator: Unsupported node type: {}", node.m_NodeTypeID.GetHash());
                }
            }
        }

        void ParseConnections()
        {
            OLO_PROFILE_FUNCTION();
            
            // Validate and copy connections from the source prototype
            m_OutPrototype->m_Connections.clear();
            
            if (!m_Options.m_GraphPrototype)
            {
                OLO_CORE_WARN("GraphGenerator: No graph prototype provided for connection parsing");
                return;
            }
            
            sizet validConnections = 0;
            sizet invalidConnections = 0;
            
            // Build a hash set of all node IDs for O(1) lookup during connection validation
            std::unordered_set<UUID> nodeIDs;
            nodeIDs.reserve(m_Options.m_GraphPrototype->m_Nodes.size());
            for (const auto& node : m_Options.m_GraphPrototype->m_Nodes)
            {
                nodeIDs.insert(node.m_ID);
            }
            
            for (const auto& connection : m_Options.m_GraphPrototype->m_Connections)
            {
                // Validate connection endpoints are not empty
                if (!connection.m_Source.m_EndpointID.IsValid())
                {
                    OLO_CORE_WARN("GraphGenerator: Connection has empty source endpoint");
                    invalidConnections++;
                    continue;
                }
                
                if (!connection.m_Destination.m_EndpointID.IsValid())
                {
                    OLO_CORE_WARN("GraphGenerator: Connection has empty destination endpoint");
                    invalidConnections++;
                    continue;
                }
                
                // Validate source and destination nodes exist in prototype (only for node-targeting connections)
                
                // Determine which endpoints require real nodes based on connection type
                bool sourceRequiresNode = (connection.m_Type == Prototype::Connection::NodeValue_NodeValue ||
                                        connection.m_Type == Prototype::Connection::NodeEvent_NodeEvent ||
                                        connection.m_Type == Prototype::Connection::NodeValue_GraphValue ||
                                        connection.m_Type == Prototype::Connection::NodeEvent_GraphEvent);
                
                bool destinationRequiresNode = (connection.m_Type == Prototype::Connection::NodeValue_NodeValue ||
                                                connection.m_Type == Prototype::Connection::NodeEvent_NodeEvent ||
                                                connection.m_Type == Prototype::Connection::GraphValue_NodeValue ||
                                                connection.m_Type == Prototype::Connection::GraphEvent_NodeEvent);
            
                // Only validate node existence for endpoints that actually require nodes
                bool sourceNodeExists = !sourceRequiresNode;  // Default to true for non-node endpoints
                bool destinationNodeExists = !destinationRequiresNode;  // Default to true for non-node endpoints
            
                // Use O(1) hash set lookup instead of O(n) linear search
                if (sourceRequiresNode)
                {
                    sourceNodeExists = nodeIDs.contains(connection.m_Source.m_NodeID);
                }
                
                if (destinationRequiresNode)
                {
                    destinationNodeExists = nodeIDs.contains(connection.m_Destination.m_NodeID);
                }
                
                if (sourceRequiresNode && !sourceNodeExists)
                {
                    OLO_CORE_WARN("GraphGenerator: Connection references non-existent source node {}", static_cast<u64>(connection.m_Source.m_NodeID));
                    invalidConnections++;
                    continue;
                }
                
                if (destinationRequiresNode && !destinationNodeExists)
                {
                    OLO_CORE_WARN("GraphGenerator: Connection references non-existent destination node {}", static_cast<u64>(connection.m_Destination.m_NodeID));
                    invalidConnections++;
                    continue;
                }
            
                // Validate connection type is valid using explicit allow-list
                bool isValidConnectionType = false;
                switch (connection.m_Type)
                {
                    case Prototype::Connection::NodeValue_NodeValue:
                    case Prototype::Connection::NodeEvent_NodeEvent:
                    case Prototype::Connection::GraphValue_NodeValue:
                    case Prototype::Connection::GraphEvent_NodeEvent:
                    case Prototype::Connection::NodeValue_GraphValue:
                    case Prototype::Connection::NodeEvent_GraphEvent:
                    case Prototype::Connection::LocalVariable_NodeValue:
                        isValidConnectionType = true;
                        break;
                    default:
                        isValidConnectionType = false;
                        break;
                }
                
                if (!isValidConnectionType)
                {
                    OLO_CORE_WARN("GraphGenerator: Connection has invalid connection type {}", static_cast<i32>(connection.m_Type));
                    invalidConnections++;
                    continue;
                }
                
                // Validate that event connections only connect to event endpoints
                bool isEventConnection = (connection.m_Type == Prototype::Connection::NodeEvent_NodeEvent ||
                                        connection.m_Type == Prototype::Connection::GraphEvent_NodeEvent ||
                                        connection.m_Type == Prototype::Connection::NodeEvent_GraphEvent);
                
                bool isValueConnection = (connection.m_Type == Prototype::Connection::NodeValue_NodeValue ||
                                        connection.m_Type == Prototype::Connection::GraphValue_NodeValue ||
                                        connection.m_Type == Prototype::Connection::NodeValue_GraphValue ||
                                        connection.m_Type == Prototype::Connection::LocalVariable_NodeValue);
                
                // Basic endpoint compatibility validation
                // For now, we assume same-type connections are compatible
                // More sophisticated type checking would require endpoint type information
                if (isEventConnection)
                {
                    OLO_CORE_TRACE("GraphGenerator: Validated event connection from endpoint {} to {}", 
                        static_cast<u64>(connection.m_Source.m_EndpointID), static_cast<u64>(connection.m_Destination.m_EndpointID));
                }
                else if (isValueConnection)
                {
                    OLO_CORE_TRACE("GraphGenerator: Validated value connection from endpoint {} to {}", 
                        static_cast<u64>(connection.m_Source.m_EndpointID), static_cast<u64>(connection.m_Destination.m_EndpointID));
                }
                
                // Connection is valid, add to output prototype
                m_OutPrototype->m_Connections.push_back(connection);
                validConnections++;
            }
                        
            OLO_CORE_INFO("GraphGenerator: Validated {} connections ({} valid, {} invalid)", 
                m_Options.m_GraphPrototype->m_Connections.size(), validConnections, invalidConnections);
        }

        void ParseWaveReferences()
        {
            OLO_PROFILE_FUNCTION();

            // Scan through nodes and collect any wave asset references
            m_OutWaveAssets.clear();
            std::unordered_set<UUID> seenAssets;
            
            for (const auto& node : m_Options.m_GraphPrototype->m_Nodes)
            {
                // Check each default value plug for potential asset handles
                for (const auto& plug : node.m_DefaultValuePlugs)
                {
                    // Asset handles are stored as int64 values
                    if (plug.m_DefaultValue.getType().isInt64())
                    {
                        i64 assetHandle = plug.m_DefaultValue.getInt64();
                        if (assetHandle != 0) // Non-zero indicates a valid asset handle
                        {
                            UUID assetUUID = static_cast<UUID>(assetHandle);
                            if (seenAssets.insert(assetUUID).second)
                            {
                                m_OutWaveAssets.push_back(assetUUID);
                                OLO_CORE_TRACE("GraphGenerator: Found wave asset reference: {}", assetUUID);
                            }
                        }
                    }
                }
            }
            
            OLO_CORE_INFO("GraphGenerator: Collected {} wave asset references", m_OutWaveAssets.size());
        }
    };

    //==============================================================================
    Ref<Prototype> ConstructPrototype(const GraphGeneratorOptions& options, std::vector<UUID>& waveAssetsToLoad)
    {
        OLO_PROFILE_FUNCTION();
        
        auto prototype = Ref<Prototype>::Create();
        prototype->m_DebugName = options.m_Name;
        prototype->m_ID = UUID();

        GraphGenerator generator(options, prototype);
        
        if (!generator.Run())
        {
            OLO_CORE_ERROR("Failed to construct graph prototype: {}", options.m_Name);
            return nullptr;
        }

        waveAssetsToLoad = std::move(generator.m_OutWaveAssets);
        return prototype;
    }

    //==============================================================================
    // Helper functions for CreateInstance

    namespace
    {
        void SetupGraphIO(Ref<SoundGraph>& graph, const Ref<Prototype>& prototype)
        {
            // Set up graph inputs
            for (const auto& input : prototype->m_Inputs)
            {
                choc::value::Value copyValue = input.m_DefaultValue;
                graph->AddGraphInputStream(input.m_EndpointID, std::move(copyValue));
            }

            // Set up graph outputs
            for (const auto& output : prototype->m_Outputs)
            {
                graph->AddGraphOutputStream(output.m_EndpointID);
            }

            // Set output channel IDs from the prototype outputs
            graph->m_OutputChannelIDs.clear();
            graph->m_OutputChannelIDs.reserve(prototype->m_Outputs.size());
            
            // Filter outputs to only include channel outputs (exclude events like "OnFinished")
            for (const auto& output : prototype->m_Outputs)
            {
                // Skip event outputs - only include audio channel outputs
                if (output.m_EndpointID != SoundGraph::IDs::OnFinished)
                {
                    graph->m_OutputChannelIDs.push_back(output.m_EndpointID);
                }
            }
        }

        void SetupLocalVariables(Ref<SoundGraph>& graph, const Ref<Prototype>& prototype)
        {
            for (const auto& localVar : prototype->m_LocalVariablePlugs)
            {
                choc::value::Value copyValue = localVar.m_DefaultValue;
                graph->AddLocalVariableStream(localVar.m_EndpointID, std::move(copyValue));
            }
        }

        void CreateGraphNodes(Ref<SoundGraph>& graph, const Ref<Prototype>& prototype)
        {
            for (const auto& nodeDesc : prototype->m_Nodes)
            {
                auto node = Factory::Create(nodeDesc.m_NodeTypeID, nodeDesc.m_ID);
                if (!node)
                {
                    OLO_CORE_ERROR("Failed to create node of type: {}", nodeDesc.m_NodeTypeID.GetHash());
                    continue;
                }

                // Apply default value plugs to the node
                for (const auto& defaultPlug : nodeDesc.m_DefaultValuePlugs)
                {
                    // Find the corresponding input stream in the node and set default value
                    auto inputIt = node->InputStreams.find(defaultPlug.m_EndpointID);
                    if (inputIt != node->InputStreams.end())
                    {
                        // Create a default value plug for this input
                        auto defaultValuePlug = CreateScope<StreamWriter>(
                            inputIt->second, 
                            defaultPlug.m_DefaultValue, 
                            defaultPlug.m_EndpointID
                        );
                        
                        node->DefaultValuePlugs.push_back(std::move(defaultValuePlug));
                    }
                }
                
                graph->AddNode(std::move(node));
            }
        }

        void EstablishConnections(Ref<SoundGraph>& graph, const Ref<Prototype>& prototype)
        {
            for (const auto& connection : prototype->m_Connections)
            {
                bool success = false;
                
                switch (connection.m_Type)
                {
                    case Prototype::Connection::NodeValue_NodeValue:
                        success = graph->AddValueConnection(
                            connection.m_Source.m_NodeID, 
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_NodeID, 
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::NodeEvent_NodeEvent:
                        success = graph->AddEventConnection(
                            connection.m_Source.m_NodeID, 
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_NodeID, 
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::GraphValue_NodeValue:
                        success = graph->AddInputValueRoute(
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_NodeID, 
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::GraphEvent_NodeEvent:
                        success = graph->AddInputEventsRoute(
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_NodeID, 
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::NodeValue_GraphValue:
                        success = graph->AddToGraphOutputConnection(
                            connection.m_Source.m_NodeID, 
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::NodeEvent_GraphEvent:
                        success = graph->AddToGraphOutEventConnection(
                            connection.m_Source.m_NodeID, 
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    case Prototype::Connection::LocalVariable_NodeValue:
                        success = graph->AddLocalVariableRoute(
                            connection.m_Source.m_EndpointID,
                            connection.m_Destination.m_NodeID, 
                            connection.m_Destination.m_EndpointID
                        );
                        break;

                    default:
                        OLO_CORE_ERROR("Unknown connection type: {}", static_cast<i32>(connection.m_Type));
                        break;
                }

                if (!success)
                {
                    OLO_CORE_WARN("Failed to establish connection from {}:{} to {}:{}",
                        connection.m_Source.m_NodeID, connection.m_Source.m_EndpointID.GetHash(),
                        connection.m_Destination.m_NodeID, connection.m_Destination.m_EndpointID.GetHash());
                }
            }
        }

        void InitializeGraph(Ref<SoundGraph>& graph, const Ref<Prototype>& prototype)
        {
            graph->Init();
            
            OLO_CORE_INFO("Created SoundGraph instance '{}' with {} nodes and {} connections", 
                          prototype->m_DebugName, prototype->m_Nodes.size(), prototype->m_Connections.size());
        }
    } // anonymous namespace

    Ref<SoundGraph> CreateInstance(const Ref<Prototype>& prototype)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!prototype)
        {
            OLO_CORE_ERROR("Cannot create SoundGraph instance from null prototype");
            return nullptr;
        }

        auto graph = Ref<SoundGraph>::Create(prototype->m_DebugName, prototype->m_ID);
        
        // Step 1: Set up graph inputs and outputs
        SetupGraphIO(graph, prototype);
        
        // Step 2: Set up local variables
        SetupLocalVariables(graph, prototype);
        
        // Step 3: Create all nodes
        CreateGraphNodes(graph, prototype);
        
        // Step 4: Establish all connections between nodes
        EstablishConnections(graph, prototype);
        
        // Step 5: Initialize the graph
        InitializeGraph(graph, prototype);

        return graph;
    }

} // namespace OloEngine::Audio::SoundGraph