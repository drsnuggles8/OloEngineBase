#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include "Events.h"
#include "Parameters.h"
#include "Flag.h"
#include <vector>
#include <memory>
#include <string>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Base class for all audio processing nodes
	struct NodeProcessor
	{
		NodeProcessor() = default;
		virtual ~NodeProcessor() = default;

		/// Process a block of audio samples
		virtual void Process(f32** inputs, f32** outputs, u32 numSamples) = 0;
		
		/// Update node state (called on main thread)
		virtual void Update(f64 deltaTime) {}
		
		/// Initialize node with sample rate and buffer size
		virtual void Initialize(f64 sampleRate, u32 maxBufferSize) = 0;

		/// Get node type identifier
		virtual Identifier GetTypeID() const = 0;
		
		/// Get node display name
		virtual const char* GetDisplayName() const = 0;

		//======================================================================
		// ENDPOINT REGISTRATION SYSTEM
		//======================================================================

		/// Add an input event endpoint
		template<typename T>
		std::shared_ptr<InputEvent> AddInputEvent(const Identifier& id, 
			const std::string& name,
			std::function<void(f32)> callback)
		{
			auto inputEvent = std::make_shared<InputEvent>(*this, std::move(callback));
			m_InputEvents[id] = inputEvent;
			m_InputNames[id] = name;
			return inputEvent;
		}

		/// Add an output event endpoint
		template<typename T>
		std::shared_ptr<OutputEvent> AddOutputEvent(const Identifier& id, const std::string& name)
		{
			auto outputEvent = std::make_shared<OutputEvent>(*this);
			m_OutputEvents[id] = outputEvent;
			m_OutputNames[id] = name;
			return outputEvent;
		}

		/// Add a parameter endpoint
		template<typename T>
		void AddParameter(const Identifier& id, const std::string& name, T initialValue)
		{
			m_Parameters.AddParameter(id, name, initialValue);
		}

		/// Get input event by ID
		std::shared_ptr<InputEvent> GetInputEvent(const Identifier& id) const
		{
			auto it = m_InputEvents.find(id);
			return (it != m_InputEvents.end()) ? it->second : nullptr;
		}

		/// Get output event by ID
		std::shared_ptr<OutputEvent> GetOutputEvent(const Identifier& id) const
		{
			auto it = m_OutputEvents.find(id);
			return (it != m_OutputEvents.end()) ? it->second : nullptr;
		}

		/// Get parameter value
		template<typename T>
		T GetParameterValue(const Identifier& id, T defaultValue = T{}) const
		{
			return m_Parameters.GetParameterValue(id, defaultValue);
		}

		/// Set parameter value
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			m_Parameters.SetParameterValue(id, value);
		}

		/// Get all input events
		const std::unordered_map<Identifier, std::shared_ptr<InputEvent>>& GetInputEvents() const
		{
			return m_InputEvents;
		}

		/// Get all output events
		const std::unordered_map<Identifier, std::shared_ptr<OutputEvent>>& GetOutputEvents() const
		{
			return m_OutputEvents;
		}

		/// Get parameter registry
		const ParameterRegistry& GetParameterRegistry() const
		{
			return m_Parameters;
		}

	protected:
		/// Sample rate for audio processing
		f64 m_SampleRate = 48000.0;

		/// Parameter registry for this node
		ParameterRegistry m_Parameters;

		/// Input event endpoints
		std::unordered_map<Identifier, std::shared_ptr<InputEvent>> m_InputEvents;
		std::unordered_map<Identifier, std::string> m_InputNames;

		/// Output event endpoints
		std::unordered_map<Identifier, std::shared_ptr<OutputEvent>> m_OutputEvents;
		std::unordered_map<Identifier, std::string> m_OutputNames;
	};

	//==============================================================================
	/// Connection between nodes
	struct Connection
	{
		Identifier SourceNodeID;
		std::string SourceEndpoint;
		Identifier TargetNodeID;
		std::string TargetEndpoint;
		bool IsEvent = false;
	};

} // namespace OloEngine::Audio::SoundGraph

//==============================================================================
// MACROS FOR CONVENIENT ENDPOINT DECLARATION
//==============================================================================

/// Declare an input parameter with type and name
#define DECLARE_INPUT(Type, Name) \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	AddParameter<Type>(Name##_ID, #Name, Type{})

/// Declare an output parameter with type and name
#define DECLARE_OUTPUT(Type, Name) \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	AddParameter<Type>(Name##_ID, #Name, Type{})

/// Declare an input event endpoint
#define DECLARE_INPUT_EVENT(Name, Callback) \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	auto Name##_Event = AddInputEvent(Name##_ID, #Name, Callback)

/// Declare an output event endpoint
#define DECLARE_OUTPUT_EVENT(Name) \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	auto Name##_Event = AddOutputEvent<f32>(Name##_ID, #Name)

/// Declare an input event that triggers a flag
#define DECLARE_INPUT_EVENT_FLAG(Name, FlagVar) \
	Flag FlagVar; \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	auto Name##_Event = AddInputEvent(Name##_ID, #Name, EventUtils::CreateFlagTrigger(FlagVar))

/// Declare an input event that sets a value and triggers a flag
#define DECLARE_INPUT_EVENT_VALUE(Name, ValueVar, FlagVar) \
	Flag FlagVar; \
	const auto Name##_ID = OLO_IDENTIFIER(#Name); \
	auto Name##_Event = AddInputEvent(Name##_ID, #Name, EventUtils::CreateValueSetter(ValueVar, FlagVar))

/// Connect two events in the constructor/initialization
#define CONNECT_EVENTS(SourceEvent, DestEvent) \
	EventUtils::ConnectEvents(SourceEvent, DestEvent)