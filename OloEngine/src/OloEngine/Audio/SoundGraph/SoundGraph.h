#pragma once

#include "NodeProcessor.h"
#include "WaveSource.h"
#include "Nodes/WavePlayer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Audio/LockFreeEventQueue.h"

#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <type_traits>
#include <queue>
#include <algorithm>
#include <atomic>

#define LOG_DBG_MESSAGES 0

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

// Forward declarations
namespace OloEngine { class SoundGraphAsset; }

namespace OloEngine::Audio::SoundGraph
{
	// Forward declarations and utility structs
	struct GraphEvent
	{
		u64 m_FrameIndex = 0;
		Identifier m_EndpointID;
		choc::value::Value m_Value;
		std::string m_Message;
	};
	
	struct EndpointIDs
	{
		static inline const Identifier s_Play = Identifier("play");
		static inline const Identifier s_Stop = Identifier("stop");
	};
	
	//==============================================================================
	/// Raw Sound Graph containing Inputs, Outputs and Nodes
	/// This is the main executable graph that processes audio in real-time
	class SoundGraph final : public NodeProcessor, public RefCounted
	{
	public:
		struct IDs
		{
			DECLARE_ID(InLeft);
			DECLARE_ID(InRight);
			DECLARE_ID(OutLeft);
			DECLARE_ID(OutRight);

			DECLARE_ID(Play);
			DECLARE_ID(Stop);
			DECLARE_ID(OnFinished);
		private:
			IDs() = delete;
		};

	explicit SoundGraph(std::string_view debugName, UUID id)
		: NodeProcessor(debugName.data(), id), m_EndpointOutputStreams("Graph Output Endpoints", UUID())
		{
			AddInEvent(IDs::Play);
			
			// Create a dedicated input event for handling finish notifications
			AddInEvent(Identifier("OnFinishHandler"), [&](float v)
				{
					(void)v;
			// Push finish event using pre-allocated storage (real-time safe)
			Audio::AudioThreadEvent event;
			event.FrameIndex = m_CurrentFrame;
			event.EndpointID = static_cast<u32>(IDs::OnFinished);
			
			choc::value::Value value = choc::value::createFloat32(1.0f);
			event.ValueData.CopyFrom(value);
			m_OutgoingEvents.Push(event);
		});			// Connect using shared_ptr from InEvents - use the same identifier as registration
			if (auto finishHandlerPtr = InEvents.find(Identifier("OnFinishHandler")); finishHandlerPtr != InEvents.end())
				m_OutOnFinish.AddDestination(finishHandlerPtr->second);
			
			AddOutEvent(IDs::OnFinished, m_OutOnFinish);
			
			m_OutChannels.reserve(2);
		}

		//==============================================================================
		/// Core Components

		OutputEvent m_OutOnFinish{ *this };

		/// All nodes in the graph
		std::vector<Scope<NodeProcessor>> m_Nodes;

		/// Wave players for audio file playback (subset of nodes)
		std::vector<NodeProcessor*> m_WavePlayers; // Raw pointers to nodes in m_Nodes vector

		/// Input stream endpoints from external sources
		std::vector<Scope<StreamWriter>> m_EndpointInputStreams;

		/// Output stream endpoints (collects output from nodes)
		NodeProcessor m_EndpointOutputStreams;

		//==============================================================================
		/// Parameter Interpolation System

		struct InterpolatedValue
		{
			float current;
			float target;
			float increment;
			int steps = 0;
			StreamWriter* endpoint;
			
			void SetTarget(float newTarget, int numSteps) noexcept
			{
				target = newTarget;
				
				// Guard against division by zero and invalid step counts
				if (numSteps <= 0)
				{
					// No interpolation - set increment to 0 and steps to 0
					increment = 0.0f;
					steps = 0;
				}
				else
				{
					// Normal interpolation calculation
					increment = (target - current) / numSteps;
					steps = numSteps;
				}
			}
			
			void Reset(float initialValue) noexcept
			{
				current = initialValue;
				target = initialValue;
				increment = 0.0f;
				steps = 0;
			}

			inline void Process() noexcept
			{
				if (steps > 0)
				{
					current += increment;
					steps--;

					if (steps == 0)
						current = target;

					*endpoint << current;
				}
			}
		};
		
		std::unordered_map<Identifier, InterpolatedValue> m_InterpInputs;

		/// Local variable streams (internal graph state)
		std::vector<Scope<StreamWriter>> m_LocalVariables;

		/// Output channel identifiers
		std::vector<Identifier> m_OutputChannelIDs;

		/// Output channel values
		std::vector<float> m_OutChannels;

		//==============================================================================
		/// Graph Construction Public API
		
		template<typename T>
		void AddGraphInputStream(Identifier id, T&& externalObjectOrDefaultValue)
		{
			m_EndpointInputStreams.emplace_back(CreateScope<StreamWriter>(AddInStream(id), std::forward<T>(externalObjectOrDefaultValue), id));

			if (std::is_same_v<std::remove_cvref_t<T>, float>)
			{
				// Add interpolation for float parameters
				m_InterpInputs.try_emplace(id, InterpolatedValue{ 0.0f, 0.0f, 0.0f, 0, m_EndpointInputStreams.back().get() });
			}
		}

		void AddGraphOutputStream(Identifier id)
		{
			AddOutStream<float>(id, m_OutChannels.emplace_back(0.0f));
			m_EndpointOutputStreams.AddInStream(id);

			AddConnection(OutValue(id), m_EndpointOutputStreams.InValue(id));
		}

		template<typename T>
		void AddLocalVariableStream(Identifier id, T&& externalObjectOrDefaultValue)
		{
			choc::value::Value dummy;
			m_LocalVariables.emplace_back(CreateScope<StreamWriter>(dummy.getViewReference(), std::forward<T>(externalObjectOrDefaultValue), id));
		}

		void AddNode(Scope<NodeProcessor>&& node)
		{
			OLO_CORE_ASSERT(node);
			NodeProcessor* nodePtr = node.get();
			UUID nodeID = nodePtr->m_ID;
			
			m_Nodes.emplace_back(std::move(node));
			m_NodeLookup[nodeID] = nodePtr;
		}

		//==============================================================================
		/// Node Discovery and Management

		/// Fast O(1) node lookup by UUID using hash map
		NodeProcessor* FindNodeByID(UUID id)
		{
			auto it = m_NodeLookup.find(id);
			return (it != m_NodeLookup.end()) ? it->second : nullptr;
		}

		/// Fast O(1) node lookup by UUID using hash map (const version)
		const NodeProcessor* FindNodeByID(UUID id) const
		{
			auto it = m_NodeLookup.find(id);
			return (it != m_NodeLookup.end()) ? it->second : nullptr;
		}

		//==============================================================================
		/// Graph Connections Internal Methods

		void AddConnection(choc::value::ValueView& source, choc::value::ValueView& destination) noexcept
		{
			destination = source;
		}

		void AddConnection(OutputEvent& source, InputEvent& destination) noexcept
		{
			// Find the shared_ptr for this InputEvent in InEvents
			for (const auto& [id, inputEventPtr] : InEvents)
			{
				if (inputEventPtr.get() == &destination)
				{
					source.AddDestination(inputEventPtr);
					return;
				}
			}
			// If not found, this suggests the InputEvent isn't managed by this graph
			OLO_CORE_WARN("AddConnection: InputEvent not found in managed events");
		}

		/// Connect Input Event to Input Event
		void AddRoute(InputEvent& source, InputEvent& destination) noexcept
		{
			InputEvent* dest = &destination;
			source.Event = [dest](float v) { (*dest)(v); };
		}

		/// Connect Output Event to Output Event
	void AddRoute(OutputEvent& source, OutputEvent& destination) noexcept
	{
		// Create a dedicated input event for routing from OutputEvent to OutputEvent
		OutputEvent* dest = &destination;
		static std::atomic<sizet> routeCounter{0};
		sizet currentRouteId = routeCounter.fetch_add(1, std::memory_order_relaxed);
		std::string routeIdStr = "Route_" + std::to_string(currentRouteId);
		Identifier routeId(routeIdStr);
		AddInEvent(routeId, [dest](float v) { (*dest)(v); });
		// Use the shared_ptr from InEvents for the newly created routeHandler
		if (auto routeHandlerPtr = InEvents.find(routeId); routeHandlerPtr != InEvents.end())
			source.AddDestination(routeHandlerPtr->second);
	}		//==============================================================================
		/// Graph Connections Public API

		/// Node Output Value -> Node Input Value
		bool AddValueConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* sourceNode = FindNodeByID(sourceNodeID);
			auto* destinationNode = FindNodeByID(destinationNodeID);

			if (!sourceNode || !destinationNode)
			{
				OLO_CORE_ASSERT(false, "Failed to find source or destination node for value connection");
				return false;
			}

			AddConnection(sourceNode->OutValue(sourceNodeEndpointID), destinationNode->InValue(destinationNodeEndpointID));
			return true;
		}

		/// String-based overload for AddValueConnection
		bool AddValueConnection(UUID sourceNodeID, const std::string& sourceEndpoint, 
								UUID targetNodeID, const std::string& targetEndpoint);

		/// Node Output Event -> Node Input Event
		bool AddEventConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* sourceNode = FindNodeByID(sourceNodeID);
			auto* destinationNode = FindNodeByID(destinationNodeID);

			if (!sourceNode || !destinationNode)
			{
				OLO_CORE_ASSERT(false, "Failed to find source or destination node for event connection");
				return false;
			}

			AddConnection(sourceNode->OutEvent(sourceNodeEndpointID), destinationNode->InEvent(destinationNodeEndpointID));
			return true;
		}

		/// String-based overload for AddEventConnection
		bool AddEventConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
								UUID targetNodeID, const std::string& targetEndpoint);

		/// Graph Input Value -> Node Input Value
		bool AddInputValueRoute(Identifier graphInputEventID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* destinationNode = FindNodeByID(destinationNodeID);
			auto endpoint = std::find_if(m_EndpointInputStreams.begin(), m_EndpointInputStreams.end(),
				[graphInputEventID](const Scope<StreamWriter>& endpoint) { return endpoint->DestinationID == graphInputEventID; });
			
			if (!destinationNode || endpoint == m_EndpointInputStreams.end())
			{
				OLO_CORE_ASSERT(false, "Failed to find destination node or input endpoint");
				return false;
			}

			AddConnection((*endpoint)->OutputValue.getViewReference(), destinationNode->InValue(destinationNodeEndpointID));
			return true;
		}

		/// Graph Input Event -> Node Input Event
		bool AddInputEventsRoute(Identifier graphInputEventID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* destinationNode = FindNodeByID(destinationNodeID);

			if (!destinationNode)
			{
				OLO_CORE_ASSERT(false, "Failed to find destination node for input event route");
				return false;
			}

			AddRoute(InEvent(graphInputEventID), destinationNode->InEvent(destinationNodeEndpointID));
			return true;
		}

		/// Node Output Value -> Graph Output Value
		bool AddToGraphOutputConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, Identifier graphOutValueID)
		{
			auto* sourceNode = FindNodeByID(sourceNodeID);

			if (!sourceNode)
			{
				OLO_CORE_ASSERT(false, "Failed to find source node for graph output connection");
				return false;
			}

			AddConnection(sourceNode->OutValue(sourceNodeEndpointID), m_EndpointOutputStreams.InValue(graphOutValueID));
			return true;
		}

		/// Node Output Event -> Graph Output Event
		bool AddToGraphOutEventConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, Identifier graphOutEventID)
		{
			auto* sourceNode = FindNodeByID(sourceNodeID);
			if (!sourceNode)
			{
				OLO_CORE_ASSERT(false, "Failed to find source node for graph output event connection");
				return false;
			}

			AddRoute(sourceNode->OutEvent(sourceNodeEndpointID), OutEvent(graphOutEventID));
			return true;
		}

		/// Graph Local Variable (StreamWriter) -> Node Input Value
		bool AddLocalVariableRoute(Identifier graphLocalVariableID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* destinationNode = FindNodeByID(destinationNodeID);
			auto endpoint = std::find_if(m_LocalVariables.begin(), m_LocalVariables.end(),
				[graphLocalVariableID](const Scope<StreamWriter>& endpoint) { return endpoint->DestinationID == graphLocalVariableID; });

			if (!destinationNode || endpoint == m_LocalVariables.end())
			{
				OLO_CORE_ASSERT(false, "Failed to find destination node or local variable endpoint");
				return false;
			}

			AddConnection((*endpoint)->OutputValue.getViewReference(), destinationNode->InValue(destinationNodeEndpointID));
			return true;
		}

		//==============================================================================
		/// Graph Lifecycle

		/// Reset Graph to its initial state
		void Reset()
		{
			m_OutgoingEvents.Clear();
			m_OutgoingMessages.Clear();
		}

		void SetSampleRate(f32 sampleRate) 
		{ 
			m_SampleRate = sampleRate; 
		}
		
		f32 GetSampleRate() const 
		{ 
			return m_SampleRate; 
		}

		void Init() final
		{
			// Rebuild node lookup map and find wave players
			m_NodeLookup.clear();
			m_WavePlayers.clear();
			
			for (auto& node : m_Nodes)
			{
				// Rebuild lookup map
				m_NodeLookup[node->m_ID] = node.get();
				
				// Find wave players
				if (auto* wavePlayer = dynamic_cast<WavePlayer*>(node.get()))
					m_WavePlayers.push_back(wavePlayer);
			}

			// Initialize all nodes in order, passing sample rate
			for (auto& node : m_Nodes)
			{
				node->SetSampleRate(m_SampleRate);
			node->Init();
		}

		m_IsInitialized = true;
	}		void BeginProcessBlock()
		{
			// Refill wave player buffers
			for (auto& wavePlayer : m_WavePlayers)
			{
				if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
					wp->ForceRefillBuffer();
			}
		}

		void Process() final
		{
			// Process parameter interpolations
			for (std::pair<const Identifier, InterpolatedValue>& interpValue : m_InterpInputs)
				interpValue.second.Process();

		// Process all nodes in graph
		for (auto& node : m_Nodes)
			node->Process();

		++m_CurrentFrame;
	}		/// Reset nodes to their initial state
		void Reinit()
		{
			m_OutgoingEvents.Clear();
			m_OutgoingMessages.Clear();

			for (auto& node : m_Nodes)
				node->Init();
		}

		//==============================================================================
	/// Runtime Status

	bool IsPlayable() const { return m_IsInitialized; }

	/// Missing method declarations
	void InitializeEndpoints();
	void ProcessEvents();
	void OnPlay(f32 value);
	void OnStop(f32 value);
	void Play();
	void Stop();
	
	// Additional methods found in implementation
	std::queue<GraphEvent> GetPendingEvents();
	void TriggerGraphEvent(const std::string& eventName, f32 value);
	void ProcessConnections();
	void OnFinished(f32 value);
	SoundGraphAsset CreateAssetData() const;
	void UpdateFromAssetData(const SoundGraphAsset& asset);

	//==============================================================================
		/// Event and Message Handling

		/// Used in HandleOutgoingEvents
		using HandleOutgoingEventFn = void(void* userContext, u64 frameIndex, Identifier endpointName, const choc::value::ValueView&);
		/// Used in HandleOutgoingEvents
		using HandleConsoleMessageFn = void(void* userContext, u64 frameIndex, const char* message);

		/// Flushes any outgoing event and console messages that are currently queued
		/// This must be called periodically if the graph is generating events
		void HandleOutgoingEvents(void* userContext, HandleOutgoingEventFn* handleEvent, HandleConsoleMessageFn* handleConsoleMessage)
		{
			Audio::AudioThreadEvent outEvent;
			while (m_OutgoingEvents.Pop(outEvent))
			{
				// Convert EndpointID back to Identifier and get ValueView
				Identifier endpointID(outEvent.EndpointID);
				choc::value::ValueView valueView = outEvent.ValueData.GetView();
				handleEvent(userContext, outEvent.FrameIndex, endpointID, valueView);
			}

			Audio::AudioThreadMessage outMessage;
			while (m_OutgoingMessages.Pop(outMessage))
			{
				handleConsoleMessage(userContext, outMessage.FrameIndex, outMessage.Text);
			}
		}

		//==============================================================================
		/// Parameter Interface

		bool SendInputValue(u32 endpointID, choc::value::ValueView value, bool interpolate)
		{
			auto endpoint = std::find_if(m_EndpointInputStreams.begin(), m_EndpointInputStreams.end(),
				[endpointID](const Scope<StreamWriter>& endpoint)
				{
					return (u32)endpoint->DestinationID == endpointID;
				});
			if (endpoint == m_EndpointInputStreams.end())
				return false;

			if (value.isFloat32())
			{
				// Handle interpolation for float values - use safe lookup
				auto interpIt = m_InterpInputs.find(endpoint->get()->DestinationID);
				if (interpIt != m_InterpInputs.end())
				{
					auto& interpInput = interpIt->second;
					if (interpolate)
					{
						interpInput.SetTarget(value.getFloat32(), 480); // 10ms at 48kHz
					}
					else
					{
						interpInput.Reset(value.getFloat32());
						*(*endpoint) << value;
					}
				}
				else
				{
					// No interpolation registered, just set the value directly
					*(*endpoint) << value;
				}
			}
			else
			{
				*(*endpoint) << value;
			}
			
			return true;
		}

		bool SendInputEvent(Identifier endpointID, choc::value::ValueView value)
		{
			auto endpoint = InEvents.find(endpointID);

			if (endpoint == InEvents.end() || !endpoint->second || !endpoint->second->Event)
				return false;

			// Handle float values for events
			endpoint->second->Event(value.isFloat32() ? value.getFloat32() : 1.0f);
			return true;
		}

		std::vector<Identifier> GetInputEventEndpoints() const
		{
			std::vector<Identifier> handles;
			handles.reserve(InEvents.size());

			for (const auto& [handle, endpoint] : InEvents)
				handles.push_back(handle);

			return handles;
		}

		std::vector<Identifier> GetParameters() const
		{
			std::vector<Identifier> handles;
			handles.reserve(m_EndpointInputStreams.size());

			for (const auto& endpoint : m_EndpointInputStreams)
				handles.push_back(endpoint->DestinationID);

			return handles;
		}

		//==============================================================================
		/// Wave Source Management (for future implementation)

		using RefillCallback = bool(*)(Audio::WaveSource&, void* userData, u32 numFrames);
		void SetRefillWavePlayerBufferCallback(RefillCallback callback, void* userData, u32 numFrames)
		{
			for (auto& wavePlayer : m_WavePlayers)
			{
				if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
				{
					// Store the original callback to chain with it
					auto originalCallback = wp->GetWaveSource().m_OnRefill;
					
					// Set new callback that chains with the original
					wp->GetWaveSource().m_OnRefill = [callback, userData, numFrames, originalCallback](Audio::WaveSource& source) -> bool {
						bool result = true;
						if (originalCallback)
							result = originalCallback(source);
						if (callback)
							result &= callback(source, userData, numFrames);
						return result;
					};
				}
			}
		}

	private:
		bool m_IsInitialized = false;
		f32 m_SampleRate = 48000.0f;
		
		// Missing member variables from implementation
		bool m_HasFinished = false;
		bool m_IsPlaying = false;
		u64 m_CurrentFrame = 0;
		std::string m_DebugName;
		
		// Fast node lookup map (O(1) instead of O(n))
		std::unordered_map<UUID, NodeProcessor*> m_NodeLookup;

		//==============================================================================
		/// Thread-safe Event/Message Queues
		/// Using lock-free queues with pre-allocated storage to avoid heap allocations in audio thread
		
		Audio::AudioEventQueue<1024> m_OutgoingEvents;
		Audio::AudioMessageQueue<1024> m_OutgoingMessages;
	};

	//==============================================================================
	/// Template Specialization for choc::value::Value

	template<>
	inline void SoundGraph::AddGraphInputStream(Identifier id, choc::value::Value&& externalObjectOrDefaultValue)
	{
		const bool isFloat = externalObjectOrDefaultValue.isFloat32();

		m_EndpointInputStreams.emplace_back(CreateScope<StreamWriter>(AddInStream(id), std::forward<choc::value::Value>(externalObjectOrDefaultValue), id));

		if (isFloat)
		{
			m_InterpInputs.try_emplace(id, InterpolatedValue{ 0.0f, 0.0f, 0.0f, 0, m_EndpointInputStreams.back().get() });
		}
	}

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef LOG_DBG_MESSAGES
#undef DBG