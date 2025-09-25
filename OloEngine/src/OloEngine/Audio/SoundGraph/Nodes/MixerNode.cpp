#include "OloEnginePCH.h"
#include "MixerNode.h"
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MixerNode Implementation

	MixerNode::MixerNode(NodeDatabase& database, NodeID nodeID)
		: NodeProcessor(database, nodeID)
		, m_MasterVolumeView("Master Volume", 1.0f)
		, m_NumInputsView("Num Inputs", 4.0f)
		, m_OutputView("Output", 0.0f)
	{
		// Initialize dynamic arrays based on default NumInputs
		SetNumInputs(m_NumInputs);
		
		// Setup Input/Output events
		RegisterInputEvent<f32>("Master Volume", [this](const f32& value) { m_CurrentMasterVolume = value; });
		RegisterInputEvent<f32>("Num Inputs", [this](const f32& value) { 
			u32 newInputs = static_cast<u32>(std::clamp(value, 1.0f, 16.0f));
			if (newInputs != m_NumInputs) {
				SetNumInputs(newInputs);
			}
			m_CurrentNumInputs = value;
		});
		
		RegisterOutputEvent<f32>("Output");
	}

	void MixerNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		// Update ValueView streams from inputs
		m_MasterVolumeView.UpdateFromConnections(inputs, numSamples);
		m_NumInputsView.UpdateFromConnections(inputs, numSamples);
		
		// Update all input streams
		for (u32 i = 0; i < m_NumInputs; ++i)
		{
			m_InputViews[i].UpdateFromConnections(inputs, numSamples);
			m_VolumeViews[i].UpdateFromConnections(inputs, numSamples);
			m_MuteViews[i].UpdateFromConnections(inputs, numSamples);
		}
		
		for (u32 sample = 0; sample < numSamples; ++sample)
		{
			// Get current master volume
			f32 masterVolume = m_MasterVolumeView.GetValue(sample);
			if (masterVolume != m_CurrentMasterVolume) m_CurrentMasterVolume = masterVolume;
			
			// Check if number of inputs changed
			f32 numInputsFloat = m_NumInputsView.GetValue(sample);
			if (numInputsFloat != m_CurrentNumInputs)
			{
				m_CurrentNumInputs = numInputsFloat;
				u32 requestedInputs = static_cast<u32>(std::clamp(numInputsFloat, 1.0f, 16.0f));
				if (requestedInputs != m_NumInputs)
				{
					SetNumInputs(requestedInputs);
				}
			}

			f32 outputSample = 0.0f;

			// Mix all active inputs
			for (u32 inputIdx = 0; inputIdx < m_NumInputs; ++inputIdx)
			{
				f32 inputVolume = m_VolumeViews[inputIdx].GetValue(sample);
				f32 muteValue = m_MuteViews[inputIdx].GetValue(sample);
				bool isMuted = muteValue > 0.5f;

				// Update cached values
				if (inputVolume != m_CurrentVolumes[inputIdx]) m_CurrentVolumes[inputIdx] = inputVolume;
				if (muteValue != m_CurrentMutes[inputIdx]) m_CurrentMutes[inputIdx] = muteValue;

				if (isMuted || inputVolume == 0.0f)
					continue;

				// Get input signal
				f32 inputSample = 0.0f;
				
				// If we have an audio input connection, use that instead of parameter
				if (inputs && inputs[inputIdx])
				{
					inputSample = inputs[inputIdx][sample];
				}
				else
				{
					// Use parameter value as constant signal
					inputSample = m_InputViews[inputIdx].GetValue(sample);
					if (inputSample != m_CurrentInputs[inputIdx]) m_CurrentInputs[inputIdx] = inputSample;
				}
				
				// Apply volume scaling
				outputSample += inputSample * inputVolume * masterVolume;
			}

			// Set output value
			m_OutputView.SetValue(sample, outputSample);
		}
		
		// Update output streams
		m_OutputView.UpdateOutputConnections(outputs, numSamples);
	}

	void MixerNode::Initialize(f64 sampleRate, u32 maxBufferSize)
	{
		NodeProcessor::Initialize(sampleRate, maxBufferSize);
		
		// Initialize ValueView streams
		m_MasterVolumeView.Initialize(maxBufferSize);
		m_NumInputsView.Initialize(maxBufferSize);
		m_OutputView.Initialize(maxBufferSize);
		
		// Initialize all input streams
		for (u32 i = 0; i < m_InputViews.size(); ++i)
		{
			m_InputViews[i].Initialize(maxBufferSize);
			m_VolumeViews[i].Initialize(maxBufferSize);
			m_MuteViews[i].Initialize(maxBufferSize);
		}
		
		Reset();
	}

	void MixerNode::Reset()
	{
		// Reset cached values to defaults
		m_CurrentMasterVolume = 1.0f;
		m_CurrentNumInputs = static_cast<f32>(m_NumInputs);
		
		for (u32 i = 0; i < m_CurrentInputs.size(); ++i)
		{
			m_CurrentInputs[i] = 0.0f;
			m_CurrentVolumes[i] = 1.0f;
			m_CurrentMutes[i] = 0.0f;
		}
	}

	void MixerNode::SetNumInputs(u32 numInputs)
	{
		numInputs = std::clamp(numInputs, 1u, 16u); // Reasonable limits
		
		if (numInputs != m_NumInputs)
		{
			m_NumInputs = numInputs;
			
			// Resize all dynamic arrays
			m_InputViews.resize(m_NumInputs);
			m_VolumeViews.resize(m_NumInputs);
			m_MuteViews.resize(m_NumInputs);
			m_CurrentInputs.resize(m_NumInputs, 0.0f);
			m_CurrentVolumes.resize(m_NumInputs, 1.0f);
			m_CurrentMutes.resize(m_NumInputs, 0.0f);
			
			// Initialize new ValueView streams
			for (u32 i = 0; i < m_NumInputs; ++i)
			{
				const std::string inputName = std::string("Input ") + std::to_string(i + 1);
				const std::string volumeName = std::string("Volume ") + std::to_string(i + 1);
				const std::string muteName = std::string("Mute ") + std::to_string(i + 1);
				
				m_InputViews[i] = ValueView<f32>(inputName, 0.0f);
				m_VolumeViews[i] = ValueView<f32>(volumeName, 1.0f);
				m_MuteViews[i] = ValueView<f32>(muteName, 0.0f);
				
				// Create Input/Output events for dynamic inputs
				RegisterInputEvent<f32>(inputName, [this, i](const f32& value) { 
					if (i < m_CurrentInputs.size()) m_CurrentInputs[i] = value; 
				});
				RegisterInputEvent<f32>(volumeName, [this, i](const f32& value) { 
					if (i < m_CurrentVolumes.size()) m_CurrentVolumes[i] = value; 
				});
				RegisterInputEvent<f32>(muteName, [this, i](const f32& value) { 
					if (i < m_CurrentMutes.size()) m_CurrentMutes[i] = value; 
				});
			}
		}
	}

	void MixerNode::SetInputVolume(u32 inputIndex, f32 volume)
	{
		if (inputIndex < m_NumInputs)
		{
			volume = std::clamp(volume, 0.0f, 10.0f);
			m_CurrentVolumes[inputIndex] = volume;
		}
	}

	f32 MixerNode::GetInputVolume(u32 inputIndex) const
	{
		if (inputIndex < m_NumInputs)
		{
			return m_CurrentVolumes[inputIndex];
		}
		return 0.0f;
	}

	void MixerNode::SetInputMute(u32 inputIndex, bool mute)
	{
		if (inputIndex < m_NumInputs)
		{
			m_CurrentMutes[inputIndex] = mute ? 1.0f : 0.0f;
		}
	}

	bool MixerNode::IsInputMuted(u32 inputIndex) const
	{
		if (inputIndex < m_NumInputs)
		{
			return m_CurrentMutes[inputIndex] > 0.5f;
		}
		return false;
	}

	void MixerNode::SetMasterVolume(f32 volume)
	{
		m_CurrentMasterVolume = std::clamp(volume, 0.0f, 10.0f);
	}

	f32 MixerNode::GetMasterVolume() const
	{
		return m_CurrentMasterVolume;
	}

	//==============================================================================
	/// GainNode Implementation

	GainNode::GainNode(NodeDatabase& database, NodeID nodeID)
		: NodeProcessor(database, nodeID)
		, m_InputView("Input", 0.0f)
		, m_GainView("Gain", 1.0f)
		, m_MuteView("Mute", 0.0f)
		, m_OutputView("Output", 0.0f)
	{
		// Setup Input/Output events
		RegisterInputEvent<f32>("Input", [this](const f32& value) { m_CurrentInput = value; });
		RegisterInputEvent<f32>("Gain", [this](const f32& value) { m_CurrentGain = value; });
		RegisterInputEvent<f32>("Mute", [this](const f32& value) { m_CurrentMute = value; });
		
		RegisterOutputEvent<f32>("Output");
	}

	void GainNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		// Update ValueView streams from inputs
		m_InputView.UpdateFromConnections(inputs, numSamples);
		m_GainView.UpdateFromConnections(inputs, numSamples);
		m_MuteView.UpdateFromConnections(inputs, numSamples);
		
		for (u32 sample = 0; sample < numSamples; ++sample)
		{
			// Get current parameter values from streams
			f32 gain = m_GainView.GetValue(sample);
			f32 muteValue = m_MuteView.GetValue(sample);
			bool isMuted = muteValue > 0.5f;
			
			// Update internal state if changed
			if (gain != m_CurrentGain) m_CurrentGain = gain;
			if (muteValue != m_CurrentMute) m_CurrentMute = muteValue;
			
			f32 appliedGain = isMuted ? 0.0f : gain;
			f32 inputSample = 0.0f;
			
			// Get input sample - prioritize audio connection over parameter
			if (inputs && inputs[0])
			{
				inputSample = inputs[0][sample];
			}
			else
			{
				// Use parameter value as constant signal
				inputSample = m_InputView.GetValue(sample);
				if (inputSample != m_CurrentInput) m_CurrentInput = inputSample;
			}
			
			// Apply gain
			f32 outputSample = inputSample * appliedGain;
			
			// Set output value
			m_OutputView.SetValue(sample, outputSample);
		}
		
		// Update output streams
		m_OutputView.UpdateOutputConnections(outputs, numSamples);
	}

	void GainNode::Initialize(f64 sampleRate, u32 maxBufferSize)
	{
		NodeProcessor::Initialize(sampleRate, maxBufferSize);
		
		// Initialize ValueView streams
		m_InputView.Initialize(maxBufferSize);
		m_GainView.Initialize(maxBufferSize);
		m_MuteView.Initialize(maxBufferSize);
		m_OutputView.Initialize(maxBufferSize);
		
		Reset();
	}

	void GainNode::Reset()
	{
		// Reset cached values to defaults
		m_CurrentInput = 0.0f;
		m_CurrentGain = 1.0f;
		m_CurrentMute = 0.0f;
	}

	void GainNode::SetGain(f32 gain)
	{
		m_CurrentGain = std::clamp(gain, 0.0f, 10.0f);
	}

	f32 GainNode::GetGain() const
	{
		return m_CurrentGain;
	}

	void GainNode::SetMute(bool mute)
	{
		m_CurrentMute = mute ? 1.0f : 0.0f;
	}

	bool GainNode::IsMuted() const
	{
		return m_CurrentMute > 0.5f;
	}

} // namespace OloEngine::Audio::SoundGraph