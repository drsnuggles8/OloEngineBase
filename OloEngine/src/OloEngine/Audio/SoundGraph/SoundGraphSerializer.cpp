#include "OloEngine/Core/Base.h"
#include "SoundGraphSerializer.h"
#include "SoundGraphPrototype.h"
#include "Nodes/WavePlayer.h"
#include "Factory.h"

#include "OloEngine/Core/Log.h"
#include <yaml-cpp/yaml.h>
#include <fstream>

// Forward declarations
namespace OloEngine { struct SoundGraphNodeData; }

namespace OloEngine::Audio::SoundGraph
{
	// SoundGraphFactory class declaration
	class SoundGraphFactory
	{
	public:
		using NodeCreatorFunc = std::function<Scope<NodeProcessor>(const std::string&, Identifier)>;
		
		static Ref<SoundGraph> CreateFromAsset(const SoundGraphAsset& asset);
		static void InitializeDefaultNodeTypes();
		static Scope<NodeProcessor> CreateNode(const std::string& typeName, const std::string& name, Identifier id);
		static void ApplyNodeProperties(NodeProcessor* node, const SoundGraphNodeData& nodeData);
		
		template<typename NodeType>
		static void RegisterNodeType(const std::string& typeName)
		{
			s_NodeCreators[typeName] = [](const std::string& name, Identifier id) -> Scope<NodeProcessor>
			{
				auto node = Scope<NodeType>::Create(name, id);
				return std::static_pointer_cast<NodeProcessor>(node);
			};
		}
		
	private:
		static std::unordered_map<std::string, NodeCreatorFunc> s_NodeCreators;
	};
}

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	// SoundGraphSerializer Implementation

	void SoundGraphSerializer::Serialize(const SoundGraphAsset& asset, const std::filesystem::path& filepath)
	{
		std::string yamlString = SerializeToString(asset);
		
		std::ofstream fout(filepath);
		if (fout.is_open())
		{
			fout << yamlString;
			fout.close();
			OLO_CORE_INFO("Successfully serialized sound graph to {}", filepath.string());
		}
		else
		{
			OLO_CORE_ERROR("Failed to open file for writing: {}", filepath.string());
		}
	}

	std::string SoundGraphSerializer::SerializeToString(const SoundGraphAsset& asset)
	{
		YAML::Emitter out;
		
		out << YAML::BeginMap;
		
		// Asset metadata
		out << YAML::Key << "SoundGraph" << YAML::Value;
		out << YAML::BeginMap;
		
		out << YAML::Key << "Name" << YAML::Value << asset.Name;
		out << YAML::Key << "ID" << YAML::Value << asset.ID;
		
		// Serialize nodes
		out << YAML::Key << "Nodes" << YAML::Value;
		out << YAML::BeginSeq;
		
		for (const auto& nodeData : asset.Nodes)
		{
			SerializeNodeData(out, nodeData);
		}
		
		out << YAML::EndSeq;
		
		// Serialize connections
		out << YAML::Key << "Connections" << YAML::Value;
		out << YAML::BeginSeq;
		
		for (const auto& connection : asset.Connections)
		{
			SerializeConnection(out, connection);
		}
		
		out << YAML::EndSeq;
		
		out << YAML::EndMap; // SoundGraph
		out << YAML::EndMap; // Root
		
		return out.c_str();
	}

	bool SoundGraphSerializer::Deserialize(SoundGraphAsset& asset, const std::filesystem::path& filepath)
	{
		std::ifstream fin(filepath);
		if (!fin.is_open())
		{
			OLO_CORE_ERROR("Failed to open sound graph file: {}", filepath.string());
			return false;
		}
		
		std::stringstream buffer;
		buffer << fin.rdbuf();
		fin.close();
		
		return DeserializeFromString(asset, buffer.str());
	}

	bool SoundGraphSerializer::DeserializeFromString(SoundGraphAsset& asset, const std::string& yamlString)
	{
		try
		{
			YAML::Node data = YAML::Load(yamlString);
			
			if (!data["SoundGraph"])
			{
				OLO_CORE_ERROR("Invalid sound graph file format - missing SoundGraph root node");
				return false;
			}
			
			auto soundGraphNode = data["SoundGraph"];
			
			// Load metadata
			if (soundGraphNode["Name"])
				asset.Name = soundGraphNode["Name"].as<std::string>();
			
			if (soundGraphNode["ID"])
				asset.ID = UUID(soundGraphNode["ID"].as<u64>());
			
			// Clear existing data
			asset.Nodes.clear();
			asset.Connections.clear();
			
			// Load nodes
			if (soundGraphNode["Nodes"])
			{
				auto nodesNode = soundGraphNode["Nodes"];
				sizet nodeCount = 0;
				sizet validNodeCount = 0;
				
				for (const auto& nodeYaml : nodesNode)
				{
					nodeCount++;
					SoundGraphAsset::NodeData nodeData;
					if (DeserializeNodeData(nodeYaml, nodeData))
					{
						asset.Nodes.push_back(nodeData);
						validNodeCount++;
					}
				}
				
				// If nodes were present but none were valid, fail
				if (nodeCount > 0 && validNodeCount == 0)
				{
					OLO_CORE_ERROR("Sound graph contains nodes but none could be deserialized");
					return false;
				}
			}
			
			// Load connections
			if (soundGraphNode["Connections"])
			{
				auto connectionsNode = soundGraphNode["Connections"];
				sizet connectionCount = 0;
				sizet validConnectionCount = 0;
				
				for (const auto& connectionYaml : connectionsNode)
				{
					connectionCount++;
					Prototype::Connection connection;
					if (DeserializeConnection(connectionYaml, connection))
					{
						asset.Connections.push_back(connection);
						validConnectionCount++;
					}
				}
				
				// If connections were present but none were valid, fail
				if (connectionCount > 0 && validConnectionCount == 0)
				{
					OLO_CORE_ERROR("Sound graph contains connections but none could be deserialized");
					return false;
				}
			}
			
			OLO_CORE_INFO("Successfully deserialized sound graph: {}", asset.Name);
			return true;
		}
		catch (const YAML::Exception& e)
		{
			OLO_CORE_ERROR("Failed to parse YAML: Invalid format");
			return false;
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("Failed to deserialize sound graph: {}", e.what());
			return false;
		}
		catch (...)
		{
			OLO_CORE_ERROR("Unknown error occurred during sound graph deserialization");
			return false;
		}
	}

	YAML::Emitter& SoundGraphSerializer::SerializeNodeData(YAML::Emitter& out, const SoundGraphAsset::NodeData& nodeData)
	{
		out << YAML::BeginMap;
		
		out << YAML::Key << "ID" << YAML::Value << nodeData.ID;
		out << YAML::Key << "Name" << YAML::Value << nodeData.Name;
		out << YAML::Key << "Type" << YAML::Value << nodeData.Type;
		
		// Serialize properties
		if (!nodeData.Properties.empty())
		{
			out << YAML::Key << "Properties" << YAML::Value;
			out << YAML::BeginMap;
			
			for (const auto& [key, value] : nodeData.Properties)
			{
				out << YAML::Key << key << YAML::Value << value;
			}
			
			out << YAML::EndMap;
		}
		
		out << YAML::EndMap;
		
		return out;
	}

	YAML::Emitter& SoundGraphSerializer::SerializeConnection(YAML::Emitter& out, const Prototype::Connection& connection)
	{
		out << YAML::BeginMap;
		
		out << YAML::Key << "SourceNodeID" << YAML::Value << connection.Source.NodeID;
		out << YAML::Key << "SourceEndpoint" << YAML::Value << connection.Source.EndpointID;
		out << YAML::Key << "TargetNodeID" << YAML::Value << connection.Destination.NodeID;
		out << YAML::Key << "TargetEndpoint" << YAML::Value << connection.Destination.EndpointID;
		out << YAML::Key << "Type" << YAML::Value << connection.Type;
		
		out << YAML::EndMap;
		
		return out;
	}

	bool SoundGraphSerializer::DeserializeNodeData(const YAML::Node& node, SoundGraphAsset::NodeData& nodeData)
	{
		try
		{
			if (!node["ID"] || !node["Name"] || !node["Type"])
			{
				OLO_CORE_ERROR("Invalid node data - missing required fields");
				return false;
			}
			
			nodeData.ID = UUID(node["ID"].as<u64>());
			nodeData.Name = node["Name"].as<std::string>();
			nodeData.Type = node["Type"].as<std::string>();
			
			// Load properties if they exist
			if (node["Properties"])
			{
				auto propsNode = node["Properties"];
				
				for (const auto& prop : propsNode)
				{
					std::string key = prop.first.as<std::string>();
					std::string value = prop.second.as<std::string>();
					nodeData.Properties[key] = value;
				}
			}
			
			return true;
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("Failed to deserialize node data: {}", e.what());
			return false;
		}
	}

	bool SoundGraphSerializer::DeserializeConnection(const YAML::Node& node, Prototype::Connection& connection)
	{
		try
		{
			if (!node["SourceNodeID"] || !node["SourceEndpoint"] || 
				!node["TargetNodeID"] || !node["TargetEndpoint"])
			{
				OLO_CORE_ERROR("Invalid connection data - missing required fields");
				return false;
			}
			
			connection.Source.NodeID = UUID(node["SourceNodeID"].as<u64>());
			connection.Source.EndpointID = Identifier(node["SourceEndpoint"].as<u32>());
			connection.Destination.NodeID = UUID(node["TargetNodeID"].as<u64>());
			connection.Destination.EndpointID = Identifier(node["TargetEndpoint"].as<u32>());
			
			if (node["Type"])
				connection.Type = static_cast<Prototype::Connection::EType>(node["Type"].as<i32>());
			else
				connection.Type = Prototype::Connection::NodeValue_NodeValue; // Default value
			
			return true;
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("Failed to deserialize connection: {}", e.what());
			return false;
		}
	}

	//==============================================================================
	// SoundGraphFactory Implementation

	std::unordered_map<std::string, SoundGraphFactory::NodeCreatorFunc> SoundGraphFactory::s_NodeCreators;

	Ref<SoundGraph> SoundGraphFactory::CreateFromAsset(const SoundGraphAsset& asset)
	{
		// Initialize node types if not done already
		if (s_NodeCreators.empty())
			InitializeDefaultNodeTypes();
		
		auto soundGraph = Ref<SoundGraph>::Create();
		// TODO: Set name and ID if SoundGraph supports them
		
		// Create all nodes first
		std::unordered_map<UUID, NodeProcessor*> nodeMap;
		
		for (const auto& nodeData : asset.Nodes)
		{
			// Convert UUID from NodeData to Identifier for CreateNode
			Identifier nodeId = Identifier(static_cast<u64>(nodeData.ID));
			auto node = CreateNode(nodeData.Type, nodeData.Name, nodeId);
			if (node)
			{
				// Apply properties
				ApplyNodeProperties(node.get(), nodeData);
				
				// Store reference for connection phase using original UUID
				nodeMap[nodeData.ID] = node.get();
				
				// Add to sound graph
				soundGraph->AddNode(std::move(node));
			}
			else
			{
				OLO_CORE_ERROR("Failed to create node of type '{}' with name '{}'", nodeData.Type, nodeData.Name);
			}
		}
		
		// Connect nodes
		for (const auto& connection : asset.Connections)
		{
			auto outputNodeIt = nodeMap.find(connection.SourceNodeID);
			auto inputNodeIt = nodeMap.find(connection.TargetNodeID);
			
			if (outputNodeIt != nodeMap.end() && inputNodeIt != nodeMap.end())
			{
				NodeProcessor* outputNode = outputNodeIt->second;
				NodeProcessor* inputNode = inputNodeIt->second;
				
				// TODO: Implement connection logic for ValueView system
				// Connect the nodes
				/*
				if (!outputNode->ConnectTo(connection.Source.EndpointID, inputNode, connection.Destination.EndpointID))
				{
					OLO_CORE_WARN("Failed to connect {} -> {} ({} -> {})", 
						outputNode->GetDisplayName(), inputNode->GetDisplayName(),
						connection.Source.EndpointID, connection.Destination.EndpointID);
				}
				*/
				OLO_CORE_WARN("TODO: Connection logic not implemented for ValueView system");
			}
			else
			{
				OLO_CORE_ERROR("Connection references unknown nodes: {} -> {}", 
					static_cast<u64>(connection.SourceNodeID), static_cast<u64>(connection.TargetNodeID));
			}
		}
		
		OLO_CORE_INFO("Successfully created sound graph '{}' with {} nodes and {} connections", 
			asset.Name, asset.Nodes.size(), asset.Connections.size());
		
		return soundGraph;
	}

	void SoundGraphFactory::InitializeDefaultNodeTypes()
	{
		// Register built-in node types
		RegisterNodeType<WavePlayer>("WavePlayer");
		
		OLO_CORE_INFO("Registered {} default sound graph node types", s_NodeCreators.size());
	}

	Scope<NodeProcessor> SoundGraphFactory::CreateNode(const std::string& typeName, const std::string& name, Identifier id)
	{
		auto it = s_NodeCreators.find(typeName);
		if (it != s_NodeCreators.end())
		{
			return it->second(name, id);
		}
		
		OLO_CORE_ERROR("Unknown node type: {}", typeName);
		return nullptr;
	}

	void SoundGraphFactory::ApplyNodeProperties(NodeProcessor* node, const SoundGraphNodeData& nodeData)
	{
		// Apply type-specific properties
		if (auto wavePlayer = dynamic_cast<WavePlayer*>(node))
		{
			auto it = nodeData.Properties.find("WaveAsset");
			if (it != nodeData.Properties.end())
			{
				// Try to parse the asset handle from string
				try 
				{
					AssetHandle handle = std::stoull(it->second);
					wavePlayer->SetWaveAsset(handle);
				}
				catch (...)
				{
					OLO_CORE_WARN("SoundGraphSerializer: Failed to parse WaveAsset handle: {}", it->second);
				}
			}
		}
		// TODO: Add more node types as they become available
		
		// OLO_CORE_TRACE("Applied properties to node '{}' of type '{}'", node->GetDisplayName(), nodeData.Type); // TODO: GetDisplayName() method missing
	}
}