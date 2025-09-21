#include "OloEnginePCH.h"
#include "MixerNode.h"

#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MixerNode Implementation

	MixerNode::MixerNode(std::string_view debugName, UUID id, u32 numInputs)
		: NodeProcessor(debugName, id), m_NumInputs(numInputs)
	{
		ResizeInputs(numInputs);
		InitializeEndpoints();
	}

	void MixerNode::Process(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		// Clear output buffers
		for (u32 i = 0; i < numSamples; ++i)
		{
			leftChannel[i] = 0.0f;
			rightChannel[i] = 0.0f;
		}

		// Mix all input channels
		for (u32 inputIdx = 0; inputIdx < m_NumInputs; ++inputIdx)
		{
			const auto& input = m_Inputs[inputIdx];
			
			// Skip muted inputs
			if (input.Muted)
				continue;

			const f32 inputGain = input.Volume;
			
			// For this simple implementation, we'll just use the current input values
			// In a full implementation, you'd have proper audio routing between nodes
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] += input.LeftInput * inputGain;
				rightChannel[i] += input.RightInput * inputGain;
			}
		}

		// Apply master volume
		for (u32 i = 0; i < numSamples; ++i)
		{
			leftChannel[i] *= m_MasterVolume;
			rightChannel[i] *= m_MasterVolume;
		}

		// Update output values (use last sample)
		if (numSamples > 0)
		{
			m_OutputLeft = leftChannel[numSamples - 1];
			m_OutputRight = rightChannel[numSamples - 1];
		}
	}

	void MixerNode::Update([[maybe_unused]] f32 deltaTime)
	{
		// No special update logic needed for mixer
	}

	void MixerNode::Initialize(f64 sampleRate)
	{
		NodeProcessor::Initialize(sampleRate);
		Reset();
		
		OLO_CORE_TRACE("[MixerNode] Initialized '{}' with {} inputs", m_DebugName, m_NumInputs);
	}

	void MixerNode::Reset()
	{
		NodeProcessor::Reset();
		
		// Reset all inputs
		for (auto& input : m_Inputs)
		{
			input.LeftInput = 0.0f;
			input.RightInput = 0.0f;
		}
		
		m_OutputLeft = 0.0f;
		m_OutputRight = 0.0f;
	}

	void MixerNode::SetNumInputs(u32 numInputs)
	{
		if (numInputs != m_NumInputs)
		{
			m_NumInputs = numInputs;
			ResizeInputs(numInputs);
			InitializeEndpoints(); // Reinitialize endpoints with new input count
		}
	}

	void MixerNode::SetInputVolume(u32 inputIndex, f32 volume)
	{
		if (inputIndex < m_Inputs.size())
		{
			m_Inputs[inputIndex].Volume = std::clamp(volume, 0.0f, 10.0f); // Allow up to 10x gain
		}
	}

	f32 MixerNode::GetInputVolume(u32 inputIndex) const
	{
		if (inputIndex < m_Inputs.size())
		{
			return m_Inputs[inputIndex].Volume;
		}
		return 0.0f;
	}

	void MixerNode::SetInputMute(u32 inputIndex, bool mute)
	{
		if (inputIndex < m_Inputs.size())
		{
			m_Inputs[inputIndex].Muted = mute;
		}
	}

	bool MixerNode::IsInputMuted(u32 inputIndex) const
	{
		if (inputIndex < m_Inputs.size())
		{
			return m_Inputs[inputIndex].Muted;
		}
		return true; // Treat out-of-bounds as muted
	}

	void MixerNode::InitializeEndpoints()
	{
		// Clear existing endpoints
		m_InputEndpoints.clear();
		m_OutputEndpoints.clear();

		// Add dynamic input endpoints
		for (u32 i = 0; i < m_NumInputs; ++i)
		{
			auto& input = m_Inputs[i];
			
			AddInputValue(EndpointIDs::GetInputLeftName(i), &input.LeftInput);
			AddInputValue(EndpointIDs::GetInputRightName(i), &input.RightInput);
			AddInputValue(EndpointIDs::GetInputVolumeName(i), &input.Volume);
			AddInputValue(EndpointIDs::GetInputMuteName(i), reinterpret_cast<f32*>(&input.Muted));
		}

		// Add master controls
		AddInputValue(EndpointIDs::MasterVolume, &m_MasterVolume);

		// Add outputs
		AddOutputValue(EndpointIDs::OutputLeft, &m_OutputLeft);
		AddOutputValue(EndpointIDs::OutputRight, &m_OutputRight);
	}

	void MixerNode::ResizeInputs(u32 newSize)
	{
		m_Inputs.resize(newSize);
		
		// Initialize new inputs with default values
		for (auto& input : m_Inputs)
		{
			if (input.Volume == 0.0f) // Uninitialized
			{
				input.Volume = 1.0f;
			}
		}
	}

	//==============================================================================
	/// GainNode Implementation

	GainNode::GainNode(std::string_view debugName, UUID id)
		: NodeProcessor(debugName, id)
	{
		InitializeEndpoints();
	}

	void GainNode::Process(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		const f32 appliedGain = m_Mute ? 0.0f : m_Gain;

		for (u32 i = 0; i < numSamples; ++i)
		{
			// For this simple implementation, use the current input values
			// In a full implementation, you'd have proper audio routing
			leftChannel[i] = m_InputLeft * appliedGain;
			rightChannel[i] = m_InputRight * appliedGain;
		}

		// Update output values
		if (numSamples > 0)
		{
			m_OutputLeft = leftChannel[numSamples - 1];
			m_OutputRight = rightChannel[numSamples - 1];
		}
	}

	void GainNode::Update([[maybe_unused]] f32 deltaTime)
	{
		// No special update logic needed for gain
	}

	void GainNode::Initialize(f64 sampleRate)
	{
		NodeProcessor::Initialize(sampleRate);
		Reset();
		
		OLO_CORE_TRACE("[GainNode] Initialized '{}'", m_DebugName);
	}

	void GainNode::Reset()
	{
		NodeProcessor::Reset();
		
		m_InputLeft = 0.0f;
		m_InputRight = 0.0f;
		m_OutputLeft = 0.0f;
		m_OutputRight = 0.0f;
	}

	void GainNode::InitializeEndpoints()
	{
		// Input values
		AddInputValue(EndpointIDs::InputLeft, &m_InputLeft);
		AddInputValue(EndpointIDs::InputRight, &m_InputRight);
		
		// Parameters
		AddInputValue(EndpointIDs::Gain, &m_Gain);
		AddInputValue(EndpointIDs::Mute, reinterpret_cast<f32*>(&m_Mute));

		// Output values
		AddOutputValue(EndpointIDs::OutputLeft, &m_OutputLeft);
		AddOutputValue(EndpointIDs::OutputRight, &m_OutputRight);
	}
}