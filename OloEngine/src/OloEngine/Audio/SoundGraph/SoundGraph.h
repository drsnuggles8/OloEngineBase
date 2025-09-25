#pragma once

#include "NodeProcessor.h"
#include "SoundGraphPrototype.h"
#include "Value.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
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
		std::vector<Prototype::Connection> Connections;

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
	class SoundGraph : public RefCounted
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

		explicit SoundGraph();
		virtual ~SoundGraph() = default;

		// Audio processing interface (not NodeProcessor inheritance)
		void Process(f32** inputs, f32** outputs, u32 numSamples);
		void Initialize(f64 sampleRate, u32 maxBufferSize);
		
		//==============================================================================
		/// Graph Construction API

		// Add a node to the graph
		void AddNode(Scope<NodeProcessor>&& node);
		
		// Find a node by ID
		NodeProcessor* FindNodeByID(UUID id);
		const NodeProcessor* FindNodeByID(UUID id) const;

		//==============================================================================
		/// Graph-Level Routing API (Enhanced from Hazel patterns)

		/** Node Output Value -> Node Input Value */
		bool AddValueConnection(UUID sourceNodeID, const std::string& sourceEndpoint, 
							   UUID targetNodeID, const std::string& targetEndpoint);

		/** Node Output Event -> Node Input Event */
		bool AddEventConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
							   UUID targetNodeID, const std::string& targetEndpoint);

		/** Graph Input Value -> Node Input Value */
		bool AddInputValueRoute(const std::string& graphInput, UUID targetNodeID, const std::string& targetEndpoint);

		/** Graph Input Event -> Node Input Event */
		bool AddInputEventRoute(const std::string& graphInput, UUID targetNodeID, const std::string& targetEndpoint);

		/** Node Output Value -> Graph Output Value */
		bool AddOutputValueRoute(UUID sourceNodeID, const std::string& sourceEndpoint, const std::string& graphOutput);

		/** Node Output Event -> Graph Output Event */
		bool AddOutputEventRoute(UUID sourceNodeID, const std::string& sourceEndpoint, const std::string& graphOutput);

		/** Connect Input Event to Input Event (for event chaining) */
		bool AddRoute(const std::string& sourceEventName, const std::string& targetEventName);

		/** Connect Output Event to Output Event (for event forwarding) */
		bool AddEventRoute(const std::string& sourceEventName, const std::string& targetEventName);

		//==============================================================================
		/// Legacy Connection API (maintained for backward compatibility)

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
		[[nodiscard]] const std::vector<Prototype::Connection>& GetConnections() const { return m_Connections; }

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
		std::string m_DebugName = "SoundGraph";
		u64 m_CurrentFrame = 0;
		
		// Graph endpoints for input/output
		f32 m_InputLeft = 0.0f;
		f32 m_InputRight = 0.0f;
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;

		// Nodes and connections
		std::vector<Scope<NodeProcessor>> m_Nodes;
		std::vector<Prototype::Connection> m_Connections;

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

		//==============================================================================
		/// Internal routing utilities

		/** Low-level event connection helper */
		void AddConnection(std::shared_ptr<NodeProcessor::OutputEvent> source, std::shared_ptr<NodeProcessor::InputEvent> destination);

		/** Low-level parameter connection helper */
		template<typename T>
		bool AddConnection(NodeProcessor* sourceNode, const std::string& sourceParam,
						  NodeProcessor* targetNode, const std::string& targetParam)
		{
			if (!sourceNode || !targetNode)
				return false;

			Identifier sourceID = OLO_IDENTIFIER(sourceParam.c_str());
			Identifier targetID = OLO_IDENTIFIER(targetParam.c_str());

			// Verify parameters exist
			if (!sourceNode->HasParameter(sourceID) || !targetNode->HasParameter(targetID))
				return false;

			// Create parameter connection
			auto connection = CreateParameterConnection<T>(sourceNode, sourceID, targetNode, targetID);
			if (connection)
			{
				// Store connection for processing during audio callback
				// TODO: Add connection storage system once ParameterConnection integration is complete
				return true;
			}

			return false;
		}

		/** Route InputEvent to InputEvent (for event chaining) */
		void AddRoute(std::shared_ptr<NodeProcessor::InputEvent> source, std::shared_ptr<NodeProcessor::InputEvent> destination);

		/** Route OutputEvent to OutputEvent (for event forwarding) */
		void AddRoute(std::shared_ptr<NodeProcessor::OutputEvent> source, std::shared_ptr<NodeProcessor::OutputEvent> destination);

		/** Get or create graph input event */
		std::shared_ptr<NodeProcessor::InputEvent> GetOrCreateGraphInputEvent(const std::string& name);

		/** Get or create graph output event */
		std::shared_ptr<NodeProcessor::OutputEvent> GetOrCreateGraphOutputEvent(const std::string& name);

		//==============================================================================
		/// Event handlers
		void OnPlay(f32 value = 1.0f);
		void OnStop(f32 value = 1.0f);
		void OnFinished(f32 value = 1.0f);
	};
}