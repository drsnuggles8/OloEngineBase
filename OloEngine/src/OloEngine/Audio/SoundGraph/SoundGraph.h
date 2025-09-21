#pragma once

#include "NodeProcessor.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"

#include <vector>
#include <memory>
#include <string>
#include <queue>
#include <unordered_map>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Sound Graph Asset - serializable representation  
	struct SoundGraphAsset
	{
		std::string Name;
		UUID ID;

		struct NodeData
		{
			UUID ID;
			std::string Name;
			std::string Type;
			std::unordered_map<std::string, std::string> Properties;
		};

		std::vector<NodeData> Nodes;
		std::vector<Connection> Connections;

		static AssetType GetStaticType() { return AssetType::SoundGraph; }
		AssetType GetAssetType() const { return GetStaticType(); }
	};

	//==============================================================================
	/// Event that gets triggered by the sound graph
	struct GraphEvent
	{
		u64 Frame;
		std::string EventName;
		f32 Value;
	};

	//==============================================================================
	/// Main sound graph containing inputs, outputs, and nodes
	class SoundGraph : public NodeProcessor, public RefCounted
	{
	public:
		// Predefined endpoint identifiers
		struct EndpointIDs
		{
			static constexpr const char* InputLeft = "InLeft";
			static constexpr const char* InputRight = "InRight";
			static constexpr const char* OutputLeft = "OutLeft";
			static constexpr const char* OutputRight = "OutRight";
			static constexpr const char* Play = "Play";
			static constexpr const char* Stop = "Stop";
			static constexpr const char* OnFinished = "OnFinished";
		};

		explicit SoundGraph(std::string_view debugName, UUID id);
		virtual ~SoundGraph() = default;

		// NodeProcessor overrides
		void Process(f32* leftChannel, f32* rightChannel, u32 numSamples) override;
		void Update(f32 deltaTime) override;
		void Initialize(f64 sampleRate) override;
		void Reset() override;

		//==============================================================================
		/// Graph Construction API

		// Add a node to the graph
		void AddNode(Scope<NodeProcessor>&& node);
		
		// Find a node by ID
		NodeProcessor* FindNodeByID(UUID id);
		const NodeProcessor* FindNodeByID(UUID id) const;

		// Connect two nodes in the graph
		bool ConnectNodes(UUID sourceNodeID, const std::string& sourceEndpoint, 
						  UUID targetNodeID, const std::string& targetEndpoint);

		// Connect graph input to node input
		bool ConnectGraphInputToNode(const std::string& graphInput, UUID nodeID, const std::string& nodeInput);

		// Connect node output to graph output
		bool ConnectNodeToGraphOutput(UUID nodeID, const std::string& nodeOutput, const std::string& graphOutput);

		//==============================================================================
		/// Graph State Management

		// Play the sound graph
		void Play();

		// Stop the sound graph
		void Stop();

		// Check if the graph is currently playing
		[[nodiscard]] bool IsPlaying() const { return m_IsPlaying; }

		//==============================================================================
		/// Event System

		// Get pending events (called by audio system)
		std::queue<GraphEvent> GetPendingEvents();

		// Trigger an event on the graph
		void TriggerGraphEvent(const std::string& eventName, f32 value = 1.0f);

		//==============================================================================
		/// Graph Properties

		// Get all nodes in the graph
		[[nodiscard]] const std::vector<Scope<NodeProcessor>>& GetNodes() const { return m_Nodes; }

		// Get all connections in the graph
		[[nodiscard]] const std::vector<Connection>& GetConnections() const { return m_Connections; }

		//==============================================================================
		/// Asset Serialization

		// Create asset data from this runtime graph
		SoundGraphAsset CreateAssetData() const;

		// Update this graph from asset data  
		void UpdateFromAssetData(const SoundGraphAsset& asset);

	private:
		// Internal state
		bool m_IsPlaying = false;
		bool m_HasFinished = false;
		
		// Graph endpoints for input/output
		f32 m_InputLeft = 0.0f;
		f32 m_InputRight = 0.0f;
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;

		// Nodes and connections
		std::vector<Scope<NodeProcessor>> m_Nodes;
		std::vector<Connection> m_Connections;

		// Event queue for communicating with the main thread
		std::queue<GraphEvent> m_OutgoingEvents;

		// Graph input/output streams
		std::vector<Scope<StreamWriter>> m_GraphInputStreams;
		std::vector<Scope<StreamWriter>> m_GraphOutputStreams;

		//==============================================================================
		/// Internal methods

		void InitializeEndpoints();
		void ProcessEvents();
		void ProcessConnections();

		// Event handlers
		void OnPlay(f32 value = 1.0f);
		void OnStop(f32 value = 1.0f);
		void OnFinished(f32 value = 1.0f);
	};
}