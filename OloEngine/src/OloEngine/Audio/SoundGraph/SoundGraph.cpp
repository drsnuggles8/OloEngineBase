#include "OloEnginePCH.h"
#include "SoundGraph.h"
#include "Nodes/WavePlayerNode.h"
#include "Nodes/MixerNode.h"

namespace OloEngine::Audio::SoundGraph
{
	SoundGraph::SoundGraph(std::string_view debugName, UUID id)
		: NodeProcessor(debugName, id)
	{
		InitializeEndpoints();
	}

	void SoundGraph::Process(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		if (!m_IsPlaying)
		{
			// Fill with silence if not playing
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] = 0.0f;
				rightChannel[i] = 0.0f;
			}
			return;
		}

		// Process all nodes in the graph
		// Note: In a real implementation, you'd need to process nodes in topological order
		// based on their connections to ensure proper data flow
		for (auto& node : m_Nodes)
		{
			// Update frame counter for each node
			node->SetCurrentFrame(m_CurrentFrame);
			
			// Process the node
			// For now, we'll use temporary buffers - in a real implementation
			// you'd have a more sophisticated buffer management system
			static thread_local std::vector<f32> tempLeft(512);
			static thread_local std::vector<f32> tempRight(512);
			
			if (tempLeft.size() < numSamples)
			{
				tempLeft.resize(numSamples);
				tempRight.resize(numSamples);
			}

			node->Process(tempLeft.data(), tempRight.data(), numSamples);
		}

		// For now, just output the graph's output values
		// In a real implementation, this would come from the final node in the graph
		for (u32 i = 0; i < numSamples; ++i)
		{
			leftChannel[i] = m_OutputLeft;
			rightChannel[i] = m_OutputRight;
		}

		// Update frame counter
		m_CurrentFrame += numSamples;

		// Process any pending events
		ProcessEvents();
	}

	void SoundGraph::Update(f32 deltaTime)
	{
		// Update all nodes
		for (auto& node : m_Nodes)
		{
			node->Update(deltaTime);
		}
	}

	void SoundGraph::Initialize(f64 sampleRate)
	{
		NodeProcessor::Initialize(sampleRate);

		// Initialize all nodes
		for (auto& node : m_Nodes)
		{
			node->Initialize(sampleRate);
		}

		OLO_CORE_TRACE("[SoundGraph] Initialized sound graph '{}' with sample rate {}", m_DebugName, sampleRate);
	}

	void SoundGraph::Reset()
	{
		NodeProcessor::Reset();

		m_IsPlaying = false;
		m_HasFinished = false;
		m_CurrentFrame = 0;

		// Reset all nodes
		for (auto& node : m_Nodes)
		{
			node->Reset();
		}

		// Clear output values
		m_OutputLeft = 0.0f;
		m_OutputRight = 0.0f;

		// Clear event queue
		std::queue<GraphEvent> empty;
		std::swap(m_OutgoingEvents, empty);

		OLO_CORE_TRACE("[SoundGraph] Reset sound graph '{}'", m_DebugName);
	}

	void SoundGraph::AddNode(Scope<NodeProcessor>&& node)
	{
		if (node)
		{
			OLO_CORE_TRACE("[SoundGraph] Adding node '{}' to graph '{}'", node->m_DebugName, m_DebugName);
			node->Initialize(m_SampleRate);
			m_Nodes.push_back(std::move(node));
		}
	}

	NodeProcessor* SoundGraph::FindNodeByID(UUID id)
	{
		auto it = std::find_if(m_Nodes.begin(), m_Nodes.end(),
			[id](const Scope<NodeProcessor>& nodePtr)
			{
				return nodePtr->m_ID == id;
			});

		return (it != m_Nodes.end()) ? it->get() : nullptr;
	}

	const NodeProcessor* SoundGraph::FindNodeByID(UUID id) const
	{
		auto it = std::find_if(m_Nodes.begin(), m_Nodes.end(),
			[id](const Scope<NodeProcessor>& nodePtr)
			{
				return nodePtr->m_ID == id;
			});

		return (it != m_Nodes.end()) ? it->get() : nullptr;
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
			connection.SourceNodeID = sourceNodeID;
			connection.SourceEndpoint = sourceEndpoint;
			connection.TargetNodeID = targetNodeID;
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

		// For now, this is a simplified implementation
		// In a real system, you'd properly route the graph input to the node input
		OLO_CORE_TRACE("[SoundGraph] Connected graph input '{}' to node '{}' input '{}'", 
			graphInput, node->m_DebugName, nodeInput);

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

		// For now, this is a simplified implementation
		// In a real system, you'd properly route the node output to the graph output
		OLO_CORE_TRACE("[SoundGraph] Connected node '{}' output '{}' to graph output '{}'", 
			node->m_DebugName, nodeOutput, graphOutput);

		return true;
	}

	void SoundGraph::Play()
	{
		if (!m_IsPlaying)
		{
			m_IsPlaying = true;
			m_HasFinished = false;
			OnPlay();
			OLO_CORE_TRACE("[SoundGraph] Started playing sound graph '{}'", m_DebugName);
		}
	}

	void SoundGraph::Stop()
	{
		if (m_IsPlaying)
		{
			m_IsPlaying = false;
			OnStop();
			OLO_CORE_TRACE("[SoundGraph] Stopped sound graph '{}'", m_DebugName);
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
		// Set up graph input/output endpoints
		AddInputValue(EndpointIDs::InputLeft, &m_InputLeft);
		AddInputValue(EndpointIDs::InputRight, &m_InputRight);
		AddOutputValue(EndpointIDs::OutputLeft, &m_OutputLeft);
		AddOutputValue(EndpointIDs::OutputRight, &m_OutputRight);

		// Set up event endpoints
		AddInputEvent(EndpointIDs::Play, [this](f32 value) { OnPlay(value); });
		AddInputEvent(EndpointIDs::Stop, [this](f32 value) { OnStop(value); });
		AddOutputEvent(EndpointIDs::OnFinished, [this](f32 value) { OnFinished(value); });
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
		// Trigger play on all relevant nodes
		for (auto& node : m_Nodes)
		{
			node->TriggerEvent("Play", value);
		}
	}

	void SoundGraph::OnStop(f32 value)
	{
		// Trigger stop on all relevant nodes
		for (auto& node : m_Nodes)
		{
			node->TriggerEvent("Stop", value);
		}
	}

	void SoundGraph::OnFinished(f32 value)
	{
		m_HasFinished = true;
		m_IsPlaying = false;

		// Add finished event to queue
		GraphEvent event;
		event.Frame = m_CurrentFrame;
		event.EventName = EndpointIDs::OnFinished;
		event.Value = value;
		m_OutgoingEvents.push(event);
	}

	//==============================================================================
	/// Asset Serialization Implementation

	SoundGraphAsset SoundGraph::CreateAssetData() const
	{
		SoundGraphAsset asset;
		asset.Name = GetName();
		asset.ID = GetID();

		// Serialize all nodes
		for (const auto& node : m_Nodes)
		{
			SoundGraphAsset::NodeData nodeData;
			nodeData.ID = node->GetID();
			nodeData.Name = node->GetName();

			// Determine node type and extract properties
			if (auto wavePlayer = dynamic_cast<const WavePlayerNode*>(node.get()))
			{
				nodeData.Type = "WavePlayer";
				nodeData.Properties["Volume"] = std::to_string(wavePlayer->GetVolume());
				nodeData.Properties["Pitch"] = std::to_string(wavePlayer->GetPitch());
				nodeData.Properties["Loop"] = wavePlayer->IsLooping() ? "true" : "false";
				// Note: AudioFilePath would need to be stored when loading the file
			}
			else if (auto gainNode = dynamic_cast<const GainNode*>(node.get()))
			{
				nodeData.Type = "Gain";
				nodeData.Properties["Volume"] = std::to_string(gainNode->GetVolume());
			}
			else if (auto mixerNode = dynamic_cast<const MixerNode*>(node.get()))
			{
				nodeData.Type = "Mixer";
				nodeData.Properties["InputCount"] = std::to_string(mixerNode->GetInputEndpoints().size());
			}
			else
			{
				nodeData.Type = "Unknown";
				OLO_CORE_WARN("Unknown node type for serialization: {}", node->GetName());
			}

			asset.Nodes.push_back(nodeData);
		}

		// Copy connections directly
		asset.Connections = m_Connections;

		OLO_CORE_TRACE("Created asset data for sound graph '{}' with {} nodes and {} connections",
			asset.Name, asset.Nodes.size(), asset.Connections.size());

		return std::move(asset);
	}

	void SoundGraph::UpdateFromAssetData([[maybe_unused]] const SoundGraphAsset& asset)
	{
		// Clear existing state
		m_Nodes.clear();
		m_Connections.clear();
		m_IsPlaying = false;
		m_HasFinished = false;

		// This method could be used to update an existing graph
		// For now, we'll just clear and note that a full reconstruction 
		// should use SoundGraphFactory::CreateFromAsset() instead

		OLO_CORE_INFO("Updated sound graph '{}' from asset data - use SoundGraphFactory for full reconstruction", GetName());
	}
}