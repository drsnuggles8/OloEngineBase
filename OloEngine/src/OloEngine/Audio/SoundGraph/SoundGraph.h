#pragma once

#include "NodeProcessor.h"
#include "WaveSource.h"
#include "Nodes/WavePlayer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Identifier.h"

#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <type_traits>

#define LOG_DBG_MESSAGES 0

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
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
			DECLARE_ID(OnFinished);
		private:
			IDs() = delete;
		};

		explicit SoundGraph(std::string_view debugName, UUID id)
			: NodeProcessor(debugName.data(), id), EndpointOutputStreams(*this)
		{
			AddInEvent(IDs::Play);
			
			// Create a dedicated input event for handling finish notifications
			auto& finishHandler = AddInEvent(Identifier("OnFinishHandler"), [&](float v)
				{
					(void)v;
					static constexpr float dummyValue = 1.0f;
					choc::value::ValueView value(choc::value::Type::createFloat32(), (void*)&dummyValue, nullptr);
					OutgoingEvents.push({ CurrentFrame, IDs::OnFinished, value });
				});
			
			out_OnFinish.AddDestination(&finishHandler);
			
			AddOutEvent(IDs::OnFinished, out_OnFinish);

			OutgoingEvents.reset(1024);
			OutgoingMessages.reset(1024);

			out_Channels.reserve(2);
		}

		//==============================================================================
		/// Core Components

		OutputEvent out_OnFinish{ *this };

		/// All nodes in the graph
		std::vector<Scope<NodeProcessor>> Nodes;

		/// Wave players for audio file playback (subset of nodes)
		std::vector<NodeProcessor*> WavePlayers; // Raw pointers to nodes in Nodes vector

		/// Input stream endpoints from external sources
		std::vector<Scope<StreamWriter>> EndpointInputStreams;

		/// Output stream endpoints (collects output from nodes)
		NodeProcessor EndpointOutputStreams;

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
				increment = (target - current) / numSteps;
				steps = numSteps;
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
		
		std::unordered_map<Identifier, InterpolatedValue> InterpInputs;

		/// Local variable streams (internal graph state)
		std::vector<Scope<StreamWriter>> LocalVariables;

		/// Output channel identifiers
		std::vector<Identifier> OutputChannelIDs;

		/// Output channel values
		std::vector<float> out_Channels;

		//==============================================================================
		/// Graph Construction Public API
		
		template<typename T>
		void AddGraphInputStream(Identifier id, T&& externalObjectOrDefaultValue)
		{
			EndpointInputStreams.emplace_back(CreateScope<StreamWriter>(AddInStream(id), std::forward<T>(externalObjectOrDefaultValue), id));

			if (std::is_same_v<std::remove_cvref_t<T>, float>)
			{
				// Add interpolation for float parameters
				InterpInputs.try_emplace(id, InterpolatedValue{ 0.0f, 0.0f, 0.0f, 0, EndpointInputStreams.back().get() });
			}
		}

		void AddGraphOutputStream(Identifier id)
		{
			AddOutStream<float>(id, out_Channels.emplace_back(0.0f));
			EndpointOutputStreams.AddInStream(id);

			AddConnection(OutValue(id), EndpointOutputStreams.InValue(id));
		}

		template<typename T>
		void AddLocalVariableStream(Identifier id, T&& externalObjectOrDefaultValue)
		{
			choc::value::Value dummy;
			LocalVariables.emplace_back(CreateScope<StreamWriter>(dummy.getViewReference(), std::forward<T>(externalObjectOrDefaultValue), id));
		}

		void AddNode(Scope<NodeProcessor>&& node)
		{
			OLO_CORE_ASSERT(node);
			Nodes.emplace_back(std::move(node));
		}

		void AddNode(NodeProcessor* node)
		{
			OLO_CORE_ASSERT(node);
			Nodes.emplace_back(Scope<NodeProcessor>(node));
		}

		//==============================================================================
		/// Node Discovery and Management

		NodeProcessor* FindNodeByID(UUID id)
		{
			auto it = std::find_if(Nodes.begin(), Nodes.end(),
				[id](const Scope<NodeProcessor>& nodePtr)
				{
					return nodePtr->ID == id;
				});

			if (it != Nodes.end())
				return it->get();
			else
				return nullptr;
		}

		const NodeProcessor* FindNodeByID(UUID id) const
		{
			auto it = std::find_if(Nodes.begin(), Nodes.end(),
				[id](const Scope<NodeProcessor>& nodePtr)
				{
					return nodePtr->ID == id;
				});

			if (it != Nodes.end())
				return it->get();
			else
				return nullptr;
		}

		//==============================================================================
		/// Graph Connections Internal Methods

		void AddConnection(choc::value::ValueView& source, choc::value::ValueView& destination) noexcept
		{
			destination = source;
		}

		void AddConnection(OutputEvent& source, InputEvent& destination) noexcept
		{
			source.AddDestination(&destination);
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
			static size_t routeCounter = 0;
			std::string routeIdStr = "Route_" + std::to_string(routeCounter++);
			Identifier routeId(routeIdStr);
			auto& routeHandler = AddInEvent(routeId, [dest](float v) { (*dest)(v); });
			source.AddDestination(&routeHandler);
		}

		//==============================================================================
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

		/// Graph Input Value -> Node Input Value
		bool AddInputValueRoute(Identifier graphInputEventID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
		{
			auto* destinationNode = FindNodeByID(destinationNodeID);
			auto endpoint = std::find_if(EndpointInputStreams.begin(), EndpointInputStreams.end(),
				[graphInputEventID](const Scope<StreamWriter>& endpoint) { return endpoint->DestinationID == graphInputEventID; });
			
			if (!destinationNode || endpoint == EndpointInputStreams.end())
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

			AddConnection(sourceNode->OutValue(sourceNodeEndpointID), EndpointOutputStreams.InValue(graphOutValueID));
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
			auto endpoint = std::find_if(LocalVariables.begin(), LocalVariables.end(),
				[graphLocalVariableID](const Scope<StreamWriter>& endpoint) { return endpoint->DestinationID == graphLocalVariableID; });

			if (!destinationNode || endpoint == LocalVariables.end())
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
			OutgoingEvents.reset(1024);
			OutgoingMessages.reset(1024);
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
			// Find wave players among nodes
			WavePlayers.clear();
			for (auto& node : Nodes)
			{
				if (auto* wavePlayer = dynamic_cast<WavePlayer*>(node.get()))
					WavePlayers.push_back(wavePlayer);
			}

			// Initialize all nodes in order, passing sample rate
			for (auto& node : Nodes)
			{
				node->SetSampleRate(m_SampleRate);
				node->Init();
			}

			bIsInitialized = true;
		}

		void BeginProcessBlock()
		{
			// Refill wave player buffers
			for (auto& wavePlayer : WavePlayers)
			{
				if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
					wp->ForceRefillBuffer();
			}
		}

		void Process() final
		{
			// Process parameter interpolations
			for (std::pair<const Identifier, InterpolatedValue>& interpValue : InterpInputs)
				interpValue.second.Process();

			// Process all nodes in graph
			for (auto& node : Nodes)
				node->Process();

			++CurrentFrame;
		}

		/// Reset nodes to their initial state
		void Reinit()
		{
			OutgoingEvents.reset();
			OutgoingMessages.reset();

			for (auto& node : Nodes)
				node->Init();
		}

		//==============================================================================
		/// Runtime Status

		bool IsPlayable() const { return bIsInitialized; }

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
			OutgoingEvent outEvent;
			while (OutgoingEvents.pop(outEvent))
				handleEvent(userContext, outEvent.FrameIndex, outEvent.EndpointID, outEvent.value);

			OutgoingMessage outMessage;
			while (OutgoingMessages.pop(outMessage))
				handleConsoleMessage(userContext, outMessage.FrameIndex, outMessage.Message);
		}

		//==============================================================================
		/// Parameter Interface

		bool SendInputValue(u32 endpointID, choc::value::ValueView value, bool interpolate)
		{
			auto endpoint = std::find_if(EndpointInputStreams.begin(), EndpointInputStreams.end(),
				[endpointID](const Scope<StreamWriter>& endpoint)
				{
					return (u32)endpoint->DestinationID == endpointID;
				});
			if (endpoint == EndpointInputStreams.end())
				return false;

			if (value.isFloat32())
			{
				// Handle interpolation for float values - use safe lookup
				auto interpIt = InterpInputs.find(endpoint->get()->DestinationID);
				if (interpIt != InterpInputs.end())
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

			if (endpoint == InEvents.end() || !endpoint->second.Event)
				return false;

			// Handle float values for events
			endpoint->second(value.isFloat32() ? value.getFloat32() : 1.0f);
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
			handles.reserve(EndpointInputStreams.size());

			for (const auto& endpoint : EndpointInputStreams)
				handles.push_back(endpoint->DestinationID);

			return handles;
		}

		//==============================================================================
		/// Wave Source Management (for future implementation)

		using RefillCallback = bool(*)(Audio::WaveSource&, void* userData, u32 numFrames);
		void SetRefillWavePlayerBufferCallback(RefillCallback callback, void* userData, u32 numFrames)
		{
			for (auto& wavePlayer : WavePlayers)
			{
				if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
				{
					// Store the original callback to chain with it
					auto originalCallback = wp->GetWaveSource().onRefill;
					
					// Set new callback that chains with the original
					wp->GetWaveSource().onRefill = [callback, userData, numFrames, originalCallback](Audio::WaveSource& source) -> bool {
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
		bool bIsInitialized = false;
		u64 CurrentFrame = 0;
		f32 m_SampleRate = 48000.0f;

		//==============================================================================
		/// Thread-safe Event/Message Queues

		struct OutgoingEvent
		{
			u64 FrameIndex;
			Identifier EndpointID;
			choc::value::ValueView value;
		};
		
		struct OutgoingMessage
		{
			u64 FrameIndex;
			const char* Message;
		};
		
		choc::fifo::SingleReaderSingleWriterFIFO<OutgoingEvent> OutgoingEvents;
		choc::fifo::SingleReaderSingleWriterFIFO<OutgoingMessage> OutgoingMessages;
	};

	//==============================================================================
	/// Template Specialization for choc::value::Value

	template<>
	inline void SoundGraph::AddGraphInputStream(Identifier id, choc::value::Value&& externalObjectOrDefaultValue)
	{
		const bool isFloat = externalObjectOrDefaultValue.isFloat32();

		EndpointInputStreams.emplace_back(CreateScope<StreamWriter>(AddInStream(id), std::forward<choc::value::Value>(externalObjectOrDefaultValue), id));

		if (isFloat)
		{
			InterpInputs.try_emplace(id, InterpolatedValue{ 0.0f, 0.0f, 0.0f, 0, EndpointInputStreams.back().get() });
		}
	}

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef LOG_DBG_MESSAGES
#undef DBG