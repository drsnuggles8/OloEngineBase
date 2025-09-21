#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Log.h"

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>

namespace OloEngine::Audio::SoundGraph
{
	// Forward declarations
	struct StreamWriter;
	struct InputEvent;
	struct OutputEvent;

	//==============================================================================
	/// Base class for all sound graph nodes
	struct NodeProcessor
	{
		explicit NodeProcessor(std::string_view debugName, UUID id) noexcept
			: m_DebugName(debugName), m_ID(id)
		{
		}

		virtual ~NodeProcessor() = default;

		std::string m_DebugName;
		UUID m_ID;

		// Audio processing callback - called by the audio thread
		virtual void Process([[maybe_unused]] f32* leftChannel, [[maybe_unused]] f32* rightChannel, [[maybe_unused]] u32 numSamples) {}

		// Update any internal state before processing - called by main thread
		virtual void Update([[maybe_unused]] f32 deltaTime) {}

		// Initialize the node with the given sample rate
		virtual void Initialize(f64 sampleRate) { m_SampleRate = sampleRate; }

		// Reset the node to its initial state
		virtual void Reset() {}

		// Get the current frame number (for timing)
		u64 GetCurrentFrame() const { return m_CurrentFrame; }

		// Set the current frame (called by the sound graph)
		void SetCurrentFrame(u64 frame) { m_CurrentFrame = frame; }

		// Get the debug name of the node
		const std::string& GetName() const { return m_DebugName; }

		// Get the ID of the node
		UUID GetID() const { return m_ID; }

	protected:
		f64 m_SampleRate = 48000.0;
		u64 m_CurrentFrame = 0;

		// Input/Output management
		struct InputEndpoint
		{
			std::string Name;
			f32* ValuePtr = nullptr;
			std::function<void(f32)> EventCallback;
			bool IsEvent = false;
		};

		struct OutputEndpoint
		{
			std::string Name;
			f32* ValuePtr = nullptr;
			std::function<void(f32)> EventCallback;
			bool IsEvent = false;
		};

		std::unordered_map<std::string, InputEndpoint> m_InputEndpoints;
		std::unordered_map<std::string, OutputEndpoint> m_OutputEndpoints;

		// Helper functions for adding inputs/outputs
		void AddInputValue(const std::string& name, f32* valuePtr)
		{
			InputEndpoint endpoint;
			endpoint.Name = name;
			endpoint.ValuePtr = valuePtr;
			endpoint.IsEvent = false;
			m_InputEndpoints[name] = endpoint;
		}

		void AddInputEvent(const std::string& name, std::function<void(f32)> callback)
		{
			InputEndpoint endpoint;
			endpoint.Name = name;
			endpoint.EventCallback = callback;
			endpoint.IsEvent = true;
			m_InputEndpoints[name] = endpoint;
		}

		void AddOutputValue(const std::string& name, f32* valuePtr)
		{
			OutputEndpoint endpoint;
			endpoint.Name = name;
			endpoint.ValuePtr = valuePtr;
			endpoint.IsEvent = false;
			m_OutputEndpoints[name] = endpoint;
		}

		void AddOutputEvent(const std::string& name, std::function<void(f32)> callback)
		{
			OutputEndpoint endpoint;
			endpoint.Name = name;
			endpoint.EventCallback = callback;
			endpoint.IsEvent = true;
			m_OutputEndpoints[name] = endpoint;
		}

	public:
		// Public interface for connections
		const std::unordered_map<std::string, InputEndpoint>& GetInputEndpoints() const { return m_InputEndpoints; }
		const std::unordered_map<std::string, OutputEndpoint>& GetOutputEndpoints() const { return m_OutputEndpoints; }

		// Connect this node's output to another node's input
		bool ConnectTo(const std::string& outputName, NodeProcessor* targetNode, const std::string& inputName);

		// Trigger an event on this node
		void TriggerEvent(const std::string& eventName, f32 value = 1.0f);
	};

	//==============================================================================
	/// Event handling structures
	struct InputEvent
	{
		NodeProcessor& Node;
		std::function<void(f32)> Callback;

		InputEvent(NodeProcessor& node, std::function<void(f32)> callback)
			: Node(node), Callback(callback) {}

		void operator()(f32 value) { Callback(value); }
	};

	struct OutputEvent
	{
		NodeProcessor& Node;
		std::function<void(f32)> Callback;

		OutputEvent(NodeProcessor& node)
			: Node(node) {}

		void AddCallback(std::function<void(f32)> callback)
		{
			Callback = callback;
		}

		void Trigger(f32 value = 1.0f)
		{
			if (Callback)
				Callback(value);
		}
	};

	//==============================================================================
	/// Stream writer for value interpolation
	struct StreamWriter
	{
		f32 m_Value = 0.0f;
		std::string m_Name;

		StreamWriter(f32 initialValue, const std::string& name)
			: m_Value(initialValue), m_Name(name) {}

		StreamWriter& operator<<(f32 value)
		{
			m_Value = value;
			return *this;
		}

		operator f32() const { return m_Value; }
	};

	//==============================================================================
	/// Connection between nodes
	struct Connection
	{
		UUID SourceNodeID;
		std::string SourceEndpoint;
		UUID TargetNodeID;
		std::string TargetEndpoint;
		bool IsEvent = false;
	};
}