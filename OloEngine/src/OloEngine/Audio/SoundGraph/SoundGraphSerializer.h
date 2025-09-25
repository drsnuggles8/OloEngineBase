#pragma once

#include "SoundGraph.h"

#include <yaml-cpp/yaml.h>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Sound Graph Serializer - handles saving/loading sound graphs to/from YAML
	class SoundGraphSerializer
	{
	public:
		//==============================================================================
		/// Serialization

		// Serialize a sound graph asset to YAML
		static void Serialize(const SoundGraphAsset& asset, const std::filesystem::path& filepath);

		// Serialize a sound graph asset to YAML string
		static std::string SerializeToString(const SoundGraphAsset& asset);

		//==============================================================================
		/// Deserialization

		// Deserialize a sound graph asset from YAML file
		static bool Deserialize(SoundGraphAsset& asset, const std::filesystem::path& filepath);

		// Deserialize a sound graph asset from YAML string
		static bool DeserializeFromString(SoundGraphAsset& asset, const std::string& yamlString);

	private:
		//==============================================================================
		/// Internal serialization methods

		static YAML::Emitter& SerializeNodeData(YAML::Emitter& out, const SoundGraphAsset::NodeData& nodeData);
		static YAML::Emitter& SerializeConnection(YAML::Emitter& out, const Prototype::Connection& connection);

		static bool DeserializeNodeData(const YAML::Node& node, SoundGraphAsset::NodeData& nodeData);
		static bool DeserializeConnection(const YAML::Node& node, Prototype::Connection& connection);
	};

	//==============================================================================
	/// Sound Graph Factory - creates runtime instances from serialized data
	class SoundGraphFactory
	{
	public:
		// Create a sound graph from asset data
		static Ref<SoundGraph> CreateFromAsset(const SoundGraphAsset& asset);

		// Register a node type creator function
		template<typename NodeType>
		static void RegisterNodeType(const std::string& typeName)
		{
			s_NodeCreators[typeName] = [](const std::string& name, Identifier id) -> Scope<NodeProcessor>
			{
				// Create node with default constructor, name and ID are not used in current implementation
				return CreateScope<NodeType>();
			};
		}

	private:
		using NodeCreatorFunc = std::function<Scope<NodeProcessor>(const std::string&, Identifier)>;
		static std::unordered_map<std::string, NodeCreatorFunc> s_NodeCreators;

		static void InitializeDefaultNodeTypes();
		static Scope<NodeProcessor> CreateNode(const std::string& typeName, const std::string& name, Identifier id);
		static void ApplyNodeProperties(NodeProcessor* node, const SoundGraphAsset::NodeData& nodeData);
	};
}