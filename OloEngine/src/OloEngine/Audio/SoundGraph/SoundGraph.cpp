#include "OloEnginePCH.h"
#include "SoundGraph.h"
#include "Nodes/WavePlayer.h"
// #include "Nodes/MixerNode.h"  // Temporarily disabled due to API incompatibility

namespace OloEngine::Audio::SoundGraph
{
	// Type aliases for convenience
	using InputEvent = NodeProcessor::InputEvent;
	using OutputEvent = NodeProcessor::OutputEvent;
	SoundGraph::SoundGraph()
	{
		// Ensure parameters are properly initialized
		m_Parameters.SetInterpolationConfig({});
		InitializeEndpoints();
	}

	void SoundGraph::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		// Ensure parameter connections are updated first
		ProcessBeforeAudio();

		if (!m_IsPlaying)
		{
			// Fill with silence if not playing
			if (outputs[0] && outputs[1])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = 0.0f;
					outputs[1][i] = 0.0f;
				}
			}
			return;
		}

		// Process all nodes in the graph
		// Note: In a real implementation, you'd need to process nodes in topological order
		// based on their connections to ensure proper data flow
		for (auto& node : m_Nodes)
		{
			// Process the node with temporary buffers
			// In a real implementation you'd have proper buffer management
			static thread_local std::vector<f32*> tempInputs(2);
			static thread_local std::vector<f32*> tempOutputs(2);
			static thread_local std::vector<f32> tempLeftBuffer(512);
			static thread_local std::vector<f32> tempRightBuffer(512);
			
			if (tempLeftBuffer.size() < numSamples)
			{
				tempLeftBuffer.resize(numSamples);
				tempRightBuffer.resize(numSamples);
			}

			tempInputs[0] = tempLeftBuffer.data();
			tempInputs[1] = tempRightBuffer.data();
			tempOutputs[0] = tempLeftBuffer.data();
			tempOutputs[1] = tempRightBuffer.data();

			node->Process(tempInputs.data(), tempOutputs.data(), numSamples);
		}

		// For now, just output the graph's internal values
		// In a real implementation, this would come from the final node in the graph
		if (outputs[0] && outputs[1])
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				outputs[0][i] = m_OutputLeft;
				outputs[1][i] = m_OutputRight;
			}
		}

		// Process any pending events
		ProcessEvents();
		
		// Update frame counter
		m_CurrentFrame += numSamples;
	}

	void SoundGraph::Update(f64 deltaTime)
	{
		// Update all nodes
		for (auto& node : m_Nodes)
		{
			node->Update(deltaTime);
		}
	}

	void SoundGraph::Initialize(f64 sampleRate, u32 maxBufferSize)
	{
		m_SampleRate = sampleRate;

		// Initialize interpolation system for this graph
		InitializeInterpolation(sampleRate, 0.01);

		// Initialize all nodes
		for (auto& node : m_Nodes)
		{
			node->Initialize(sampleRate, maxBufferSize);
		}

		OLO_CORE_TRACE("[SoundGraph] Initialized sound graph with sample rate {}", sampleRate);
	}

	// Note: Reset method removed as it doesn't exist in current NodeProcessor API

	void SoundGraph::AddNode(Scope<NodeProcessor>&& node)
	{
		if (node)
		{
			OLO_CORE_TRACE("[SoundGraph] Adding node to sound graph");
			node->Initialize(m_SampleRate, 512); // Use default buffer size
			m_Nodes.push_back(std::move(node));
		}
	}

	NodeProcessor* SoundGraph::FindNodeByID(UUID id)
	{
		// TODO: Implement ID-based node lookup once NodeProcessor API supports IDs
		OLO_CORE_WARN("[SoundGraph] FindNodeByID not implemented - NodeProcessor API needs ID support");
		return nullptr;
	}

	const NodeProcessor* SoundGraph::FindNodeByID(UUID id) const
	{
		// TODO: Implement ID-based node lookup once NodeProcessor API supports IDs
		OLO_CORE_WARN("[SoundGraph] FindNodeByID not implemented - NodeProcessor API needs ID support");
		return nullptr;
	}

	bool SoundGraph::ConnectNodes(UUID sourceNodeID, const std::string& sourceEndpoint, 
								  UUID targetNodeID, const std::string& targetEndpoint)
	{
		NodeProcessor* sourceNode = FindNodeByID(sourceNodeID);
		NodeProcessor* targetNode = FindNodeByID(targetNodeID);

		if (!sourceNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Source node with ID {} not found", static_cast<u64>(sourceNodeID));
			return false;
		}

		if (!targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Target node with ID {} not found", static_cast<u64>(targetNodeID));
			return false;
		}

		// Attempt to connect the nodes
		bool success = sourceNode->ConnectTo(sourceEndpoint, targetNode, targetEndpoint);

		if (success)
		{
			// Store the connection for serialization/debugging
			Connection connection;
			connection.SourceNodeID = Identifier(static_cast<u32>(static_cast<u64>(sourceNodeID)));
			connection.SourceEndpoint = sourceEndpoint;
			connection.TargetNodeID = Identifier(static_cast<u32>(static_cast<u64>(targetNodeID)));
			connection.TargetEndpoint = targetEndpoint;
			// Determine if it's an event connection by checking the endpoints
			// (This is a simplified check - you'd want a more robust method)
			connection.IsEvent = (sourceEndpoint.find("Event") != std::string::npos) ||
								(targetEndpoint.find("Event") != std::string::npos);
			
			m_Connections.push_back(connection);
		}

		return success;
	}

	bool SoundGraph::ConnectGraphInputToNode(const std::string& graphInput, UUID nodeID, const std::string& nodeInput)
	{
		NodeProcessor* node = FindNodeByID(nodeID);
		if (!node)
		{
			OLO_CORE_ERROR("[SoundGraph] Node with ID {} not found", static_cast<u64>(nodeID));
			return false;
		}

		// TODO: Implement proper graph input routing once NodeProcessor API supports connection management
		OLO_CORE_TRACE("[SoundGraph] Connected graph input '{}' to node input '{}'", 
			graphInput, nodeInput);

		return true;
	}

	bool SoundGraph::ConnectNodeToGraphOutput(UUID nodeID, const std::string& nodeOutput, const std::string& graphOutput)
	{
		NodeProcessor* node = FindNodeByID(nodeID);
		if (!node)
		{
			OLO_CORE_ERROR("[SoundGraph] Node with ID {} not found", static_cast<u64>(nodeID));
			return false;
		}

		// TODO: Implement proper graph output routing once NodeProcessor API supports connection management
		OLO_CORE_TRACE("[SoundGraph] Connected node output '{}' to graph output '{}'", 
			nodeOutput, graphOutput);

		return true;
	}

	void SoundGraph::Play()
	{
		if (!m_IsPlaying)
		{
			m_IsPlaying = true;
			m_HasFinished = false;
			OnPlay(1.0f);
			OLO_CORE_TRACE("[SoundGraph] Started playing sound graph '{}'", m_DebugName);
		}
	}

	void SoundGraph::Stop()
	{
		if (m_IsPlaying)
		{
			m_IsPlaying = false;
			OnStop(0.0f);
			OLO_CORE_TRACE("[SoundGraph] Stopped playing sound graph '{}'", m_DebugName);
		}
	}

	std::queue<GraphEvent> SoundGraph::GetPendingEvents()
	{
		std::queue<GraphEvent> events;
		std::swap(events, m_OutgoingEvents);
		return events;
	}

	void SoundGraph::TriggerGraphEvent(const std::string& eventName, f32 value)
	{
		// Add event to queue for processing in audio thread
		GraphEvent event;
		event.Frame = m_CurrentFrame;
		event.EventName = eventName;
		event.Value = value;
		m_OutgoingEvents.push(event);

		// Also trigger immediately if it's a graph-level event
		if (eventName == EndpointIDs::Play)
		{
			Play();
		}
		else if (eventName == EndpointIDs::Stop)
		{
			Stop();
		}
	}

	void SoundGraph::InitializeEndpoints()
	{
	// Set up graph input/output parameters using current NodeProcessor API
	this->AddParameter<f32>(OLO_IDENTIFIER("InputLeft"), "InputLeft", 0.0f);
	this->AddParameter<f32>(OLO_IDENTIFIER("InputRight"), "InputRight", 0.0f);
	this->AddParameter<f32>(OLO_IDENTIFIER("OutputLeft"), "OutputLeft", 0.0f);
	this->AddParameter<f32>(OLO_IDENTIFIER("OutputRight"), "OutputRight", 0.0f);

	// Set up event endpoints with current API
	this->AddInEvent(OLO_IDENTIFIER("Play"), [this](f32 value) { this->OnPlay(value); });
	this->AddInEvent(OLO_IDENTIFIER("Stop"), [this](f32 value) { this->OnStop(value); });
	}

	void SoundGraph::ProcessEvents()
	{
		// Process any internal events that need to be handled during audio processing
		// This is where you'd handle things like triggering other nodes, etc.
	}

	void SoundGraph::ProcessConnections()
	{
		// Process all connections in the graph
		// This would handle the actual data flow between nodes
		// For now, this is a placeholder for the full implementation
	}

	void SoundGraph::OnPlay(f32 value)
	{
		m_IsPlaying = true;
		
		// TODO: Trigger play on all relevant nodes once event system is properly integrated
		// For now, just mark as playing
		OLO_CORE_TRACE("[SoundGraph] Started playing sound graph");
	}

	void SoundGraph::OnStop(f32 value)
	{
		m_IsPlaying = false;
		m_HasFinished = false;
		
		// TODO: Trigger stop on all relevant nodes once event system is properly integrated  
		// For now, just mark as stopped
		OLO_CORE_TRACE("[SoundGraph] Stopped sound graph");
	}

	void SoundGraph::OnFinished(f32 value)
	{
		m_HasFinished = true;
		m_IsPlaying = false;

		// TODO: Add finished event to queue once frame tracking is properly implemented
		// For now, just mark as finished
		OLO_CORE_TRACE("[SoundGraph] Sound graph finished");
	}

	//==============================================================================
	/// Asset Serialization Implementation - TEMPORARILY DISABLED

	SoundGraphAsset SoundGraph::CreateAssetData() const
	{
		SoundGraphAsset asset;
		asset.Name = "SoundGraph"; // TODO: GetName() when NodeProcessor API supports it
		asset.ID = UUID(); // TODO: GetID() when NodeProcessor API supports it

		// TODO: Serialize all nodes once NodeProcessor API provides GetID/GetName methods
		OLO_CORE_WARN("[SoundGraph] CreateAssetData partially implemented - waiting for NodeProcessor API updates");

		return asset;
	}

	void SoundGraph::UpdateFromAssetData([[maybe_unused]] const SoundGraphAsset& asset)
	{
		// Clear existing state
		m_Nodes.clear();
		m_Connections.clear();
		m_IsPlaying = false;
		m_HasFinished = false;

		// TODO: Implement full asset reconstruction once NodeProcessor API supports ID/Name methods
		OLO_CORE_INFO("Updated sound graph from asset data - TODO: implement full reconstruction");
	}

	//==============================================================================
	/// Graph-Level Routing Implementation

	bool SoundGraph::AddValueConnection(UUID sourceNodeID, const std::string& sourceEndpoint, 
										UUID targetNodeID, const std::string& targetEndpoint)
	{
		NodeProcessor* sourceNode = FindNodeByID(sourceNodeID);
		NodeProcessor* targetNode = FindNodeByID(targetNodeID);

		if (!sourceNode || !targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Node not found for value connection");
			return false;
		}

		// Try different parameter types (f32 most common in audio)
		if (AddConnection<f32>(sourceNode, sourceEndpoint, targetNode, targetEndpoint))
		{
			OLO_CORE_TRACE("[SoundGraph] Created f32 value connection: '{}:{}' -> '{}:{}'",
				sourceNode->GetDisplayName(), sourceEndpoint, targetNode->GetDisplayName(), targetEndpoint);
			return true;
		}

		if (AddConnection<i32>(sourceNode, sourceEndpoint, targetNode, targetEndpoint))
		{
			OLO_CORE_TRACE("[SoundGraph] Created i32 value connection: '{}:{}' -> '{}:{}'",
				sourceNode->GetDisplayName(), sourceEndpoint, targetNode->GetDisplayName(), targetEndpoint);
			return true;
		}

		if (AddConnection<bool>(sourceNode, sourceEndpoint, targetNode, targetEndpoint))
		{
			OLO_CORE_TRACE("[SoundGraph] Created bool value connection: '{}:{}' -> '{}:{}'",
				sourceNode->GetDisplayName(), sourceEndpoint, targetNode->GetDisplayName(), targetEndpoint);
			return true;
		}

		OLO_CORE_ERROR("[SoundGraph] No compatible parameter types found for value connection");
		return false;
	}

	bool SoundGraph::AddEventConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
										UUID targetNodeID, const std::string& targetEndpoint)
	{
		NodeProcessor* sourceNode = FindNodeByID(sourceNodeID);
		NodeProcessor* targetNode = FindNodeByID(targetNodeID);

		if (!sourceNode || !targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Node not found for event connection");
			return false;
		}

		Identifier sourceID = OLO_IDENTIFIER(sourceEndpoint.c_str());
		Identifier targetID = OLO_IDENTIFIER(targetEndpoint.c_str());

		auto sourceEvent = sourceNode->GetOutputEvent(sourceID);
		auto targetEvent = targetNode->GetInputEvent(targetID);

		if (!sourceEvent || !targetEvent)
		{
			OLO_CORE_ERROR("[SoundGraph] Event endpoints not found for connection");
			return false;
		}

		AddConnection(sourceEvent, targetEvent);
		OLO_CORE_TRACE("[SoundGraph] Created event connection: '{}:{}' -> '{}:{}'",
			sourceNode->GetDisplayName(), sourceEndpoint, targetNode->GetDisplayName(), targetEndpoint);
		return true;
	}

	bool SoundGraph::AddInputValueRoute(const std::string& graphInput, UUID targetNodeID, const std::string& targetEndpoint)
	{
		NodeProcessor* targetNode = FindNodeByID(targetNodeID);
		if (!targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Target node not found for input value route");
			return false;
		}

		// Create or get graph input parameter
		Identifier inputID = OLO_IDENTIFIER(graphInput.c_str());
		
		// Add graph input parameter if it doesn't exist
		if (!HasParameter(inputID))
		{
			AddParameter<f32>(inputID, graphInput, 0.0f);
		}

		// Create parameter connection from graph to node
		if (AddConnection<f32>(this, graphInput, targetNode, targetEndpoint))
		{
			OLO_CORE_TRACE("[SoundGraph] Created input value route: '{}' -> '{}:{}'",
				graphInput, targetNode->GetDisplayName(), targetEndpoint);
			return true;
		}

		return false;
	}

	bool SoundGraph::AddInputEventRoute(const std::string& graphInput, UUID targetNodeID, const std::string& targetEndpoint)
	{
		NodeProcessor* targetNode = FindNodeByID(targetNodeID);
		if (!targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Target node not found for input event route");
			return false;
		}

		auto graphInputEvent = GetOrCreateGraphInputEvent(graphInput);
		
		Identifier targetID = OLO_IDENTIFIER(targetEndpoint.c_str());
		auto targetEvent = targetNode->GetInputEvent(targetID);

		if (!targetEvent)
		{
			OLO_CORE_ERROR("[SoundGraph] Target event endpoint '{}' not found", targetEndpoint);
			return false;
		}

		AddRoute(graphInputEvent, targetEvent);
		OLO_CORE_TRACE("[SoundGraph] Created input event route: '{}' -> '{}:{}'",
			graphInput, targetNode->GetDisplayName(), targetEndpoint);
		return true;
	}

	bool SoundGraph::AddOutputValueRoute(UUID sourceNodeID, const std::string& sourceEndpoint, const std::string& graphOutput)
	{
		NodeProcessor* sourceNode = FindNodeByID(sourceNodeID);
		if (!sourceNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Source node not found for output value route");
			return false;
		}

		// Create or get graph output parameter
		Identifier outputID = OLO_IDENTIFIER(graphOutput.c_str());
		
		// Add graph output parameter if it doesn't exist
		if (!HasParameter(outputID))
		{
			AddParameter<f32>(outputID, graphOutput, 0.0f);
		}

		// Create parameter connection from node to graph
		if (AddConnection<f32>(sourceNode, sourceEndpoint, this, graphOutput))
		{
			OLO_CORE_TRACE("[SoundGraph] Created output value route: '{}:{}' -> '{}'",
				sourceNode->GetDisplayName(), sourceEndpoint, graphOutput);
			return true;
		}

		return false;
	}

	bool SoundGraph::AddOutputEventRoute(UUID sourceNodeID, const std::string& sourceEndpoint, const std::string& graphOutput)
	{
		NodeProcessor* sourceNode = FindNodeByID(sourceNodeID);
		if (!sourceNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Source node not found for output event route");
			return false;
		}

		Identifier sourceID = OLO_IDENTIFIER(sourceEndpoint.c_str());
		auto sourceEvent = sourceNode->GetOutputEvent(sourceID);
		
		if (!sourceEvent)
		{
			OLO_CORE_ERROR("[SoundGraph] Source event endpoint '{}' not found", sourceEndpoint);
			return false;
		}

		auto graphOutputEvent = GetOrCreateGraphOutputEvent(graphOutput);
		AddRoute(sourceEvent, graphOutputEvent);
		
		OLO_CORE_TRACE("[SoundGraph] Created output event route: '{}:{}' -> '{}'",
			sourceNode->GetDisplayName(), sourceEndpoint, graphOutput);
		return true;
	}

	bool SoundGraph::AddRoute(const std::string& sourceEventName, const std::string& targetEventName)
	{
		auto sourceEvent = GetOrCreateGraphInputEvent(sourceEventName);
		auto targetEvent = GetOrCreateGraphInputEvent(targetEventName);
		
		AddRoute(sourceEvent, targetEvent);
		OLO_CORE_TRACE("[SoundGraph] Created event route: '{}' -> '{}'", sourceEventName, targetEventName);
		return true;
	}

	bool SoundGraph::AddEventRoute(const std::string& sourceEventName, const std::string& targetEventName)
	{
		auto sourceEvent = GetOrCreateGraphOutputEvent(sourceEventName);
		auto targetEvent = GetOrCreateGraphOutputEvent(targetEventName);
		
		AddRoute(sourceEvent, targetEvent);
		OLO_CORE_TRACE("[SoundGraph] Created output event route: '{}' -> '{}'", sourceEventName, targetEventName);
		return true;
	}

	//==============================================================================
	/// Internal Routing Utilities

	void SoundGraph::AddConnection(std::shared_ptr<OutputEvent> source, std::shared_ptr<InputEvent> destination)
	{
		if (source && destination)
		{
			source->AddDestination(destination);
		}
	}

	void SoundGraph::AddRoute(std::shared_ptr<InputEvent> source, std::shared_ptr<InputEvent> destination)
	{
		if (source && destination)
		{
			// Route input event to input event (event chaining)
			source->Event = [destination](f32 value) 
			{
				if (destination) (*destination)(value);
			};
		}
	}

	void SoundGraph::AddRoute(std::shared_ptr<OutputEvent> source, std::shared_ptr<OutputEvent> destination)
	{
		if (source && destination)
		{
			// Create intermediate input event for routing
			auto intermediateEvent = std::make_shared<InputEvent>(*this, 
				[destination](f32 value) 
				{
					if (destination) (*destination)(value);
				});
			
			source->ConnectTo(intermediateEvent);
		}
	}

	std::shared_ptr<InputEvent> SoundGraph::GetOrCreateGraphInputEvent(const std::string& name)
	{
		Identifier eventID = OLO_IDENTIFIER(name.c_str());
		
		auto existingEvent = GetInputEvent(eventID);
		if (existingEvent)
			return existingEvent;

		// Create new graph input event
		return AddInputEvent<f32>(eventID, name, [this, name](f32 value)
		{
			// Default behavior: trigger corresponding graph event
			TriggerGraphEvent(name, value);
		});
	}

	std::shared_ptr<OutputEvent> SoundGraph::GetOrCreateGraphOutputEvent(const std::string& name)
	{
		Identifier eventID = OLO_IDENTIFIER(name.c_str());
		
		auto existingEvent = GetOutputEvent(eventID);
		if (existingEvent)
			return existingEvent;

		// Create new graph output event
		return AddOutputEvent<f32>(eventID, name);
	}
}