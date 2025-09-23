#include "OloEnginePCH.h"
#include "MixerNode.h"
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MixerNode Implementation

	MixerNode::MixerNode()
	{
		// Master controls
		AddParameter<f32>(MasterVolume_ID, "Master Volume", 1.0f);
		AddParameter<u32>(NumInputs_ID, "Num Inputs", m_NumInputs);
		AddParameter<f32>(Output_ID, "Output", 0.0f);

		// Initialize with default number of inputs
		UpdateParameterCount();
	}

	void MixerNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		ProcessBeforeAudio();

		// Check if number of inputs changed
		const u32 requestedInputs = GetParameterValue<u32>(NumInputs_ID);
		if (requestedInputs != m_NumInputs)
		{
			SetNumInputs(requestedInputs);
		}

		const f32 masterVolume = GetParameterValue<f32>(MasterVolume_ID);

		if (outputs && outputs[0])
		{
			// Clear output buffer
			std::fill_n(outputs[0], numSamples, 0.0f);

			// Mix all active inputs
			for (u32 inputIdx = 0; inputIdx < m_NumInputs; ++inputIdx)
			{
				const f32 inputVolume = GetParameterValue<f32>(GetVolumeID(inputIdx));
				const bool isMuted = GetParameterValue<f32>(GetMuteID(inputIdx)) > 0.5f;

				if (isMuted || inputVolume == 0.0f)
					continue;

				// Get input signal (from parameter if no audio connection)
				const f32 inputSignal = GetParameterValue<f32>(GetInputID(inputIdx));

				// If we have an audio input connection, use that instead
				if (inputs && inputs[inputIdx])
				{
					for (u32 i = 0; i < numSamples; ++i)
					{
						outputs[0][i] += inputs[inputIdx][i] * inputVolume * masterVolume;
					}
				}
				else
				{
					// Use parameter value as constant signal
					const f32 scaledSignal = inputSignal * inputVolume * masterVolume;
					for (u32 i = 0; i < numSamples; ++i)
					{
						outputs[0][i] += scaledSignal;
					}
				}
			}

			// Update output parameter with final sample value
			SetParameterValue(Output_ID, numSamples > 0 ? outputs[0][numSamples - 1] : 0.0f);
		}
		else
		{
			// No output buffer - just update output parameter to 0
			SetParameterValue(Output_ID, 0.0f);
		}
	}

	void MixerNode::Initialize(f64 sampleRate, u32 samplesPerBlock)
	{
		NodeProcessor::Initialize(sampleRate, samplesPerBlock);
		m_SampleRate = sampleRate;
		Reset();
	}

	void MixerNode::Reset()
	{
		SetParameterValue(Output_ID, 0.0f, false);
	}

	void MixerNode::SetNumInputs(u32 numInputs)
	{
		numInputs = std::clamp(numInputs, 1u, 16u); // Reasonable limits
		
		if (numInputs != m_NumInputs)
		{
			m_NumInputs = numInputs;
			SetParameterValue(NumInputs_ID, numInputs, false);
			UpdateParameterCount();
		}
	}

	void MixerNode::SetInputVolume(u32 inputIndex, f32 volume)
	{
		if (inputIndex < m_NumInputs)
		{
			SetParameterValue(GetVolumeID(inputIndex), std::clamp(volume, 0.0f, 10.0f));
		}
	}

	f32 MixerNode::GetInputVolume(u32 inputIndex) const
	{
		if (inputIndex < m_NumInputs)
		{
			return GetParameterValue<f32>(GetVolumeID(inputIndex));
		}
		return 0.0f;
	}

	void MixerNode::SetInputMute(u32 inputIndex, bool mute)
	{
		if (inputIndex < m_NumInputs)
		{
			SetParameterValue(GetMuteID(inputIndex), mute ? 1.0f : 0.0f);
		}
	}

	bool MixerNode::IsInputMuted(u32 inputIndex) const
	{
		if (inputIndex < m_NumInputs)
		{
			return GetParameterValue<f32>(GetMuteID(inputIndex)) > 0.5f;
		}
		return true;
	}

	void MixerNode::SetMasterVolume(f32 volume)
	{
		SetParameterValue(MasterVolume_ID, std::clamp(volume, 0.0f, 10.0f));
	}

	f32 MixerNode::GetMasterVolume() const
	{
		return GetParameterValue<f32>(MasterVolume_ID);
	}

	Identifier MixerNode::GetInputID(u32 index) const
	{
		return OLO_IDENTIFIER((std::string(INPUT_PREFIX) + std::to_string(index + 1)).c_str());
	}

	Identifier MixerNode::GetVolumeID(u32 index) const
	{
		return OLO_IDENTIFIER((std::string(VOLUME_PREFIX) + std::to_string(index + 1)).c_str());
	}

	Identifier MixerNode::GetMuteID(u32 index) const
	{
		return OLO_IDENTIFIER((std::string(MUTE_PREFIX) + std::to_string(index + 1)).c_str());
	}

	void MixerNode::UpdateParameterCount()
	{
		// Add parameters for each input
		for (u32 i = 0; i < m_NumInputs; ++i)
		{
			const std::string inputName = std::string("Input ") + std::to_string(i + 1);
			const std::string volumeName = std::string("Volume ") + std::to_string(i + 1);
			const std::string muteName = std::string("Mute ") + std::to_string(i + 1);

			// Add input parameter (will be overridden by audio connections)
			if (!HasParameter(GetInputID(i)))
			{
				AddParameter<f32>(GetInputID(i), inputName, 0.0f);
			}

			// Add volume control
			if (!HasParameter(GetVolumeID(i)))
			{
				AddParameter<f32>(GetVolumeID(i), volumeName, 1.0f);
			}

			// Add mute control
			if (!HasParameter(GetMuteID(i)))
			{
				AddParameter<f32>(GetMuteID(i), muteName, 0.0f);
			}
		}
	}

	//==============================================================================
	/// GainNode Implementation

	GainNode::GainNode()
	{
		// Register parameters
		AddParameter<f32>(Input_ID, "Input", 0.0f);
		AddParameter<f32>(Gain_ID, "Gain", 1.0f);
		AddParameter<f32>(Mute_ID, "Mute", 0.0f);
		AddParameter<f32>(Output_ID, "Output", 0.0f);
	}

	void GainNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		ProcessBeforeAudio();

		const f32 gain = GetParameterValue<f32>(Gain_ID);
		const bool isMuted = GetParameterValue<f32>(Mute_ID) > 0.5f;
		const f32 appliedGain = isMuted ? 0.0f : gain;

		if (outputs && outputs[0])
		{
			if (inputs && inputs[0])
			{
				// Process audio input
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = inputs[0][i] * appliedGain;
				}
			}
			else
			{
				// Use parameter value as constant signal
				const f32 inputSignal = GetParameterValue<f32>(Input_ID);
				const f32 outputSignal = inputSignal * appliedGain;
				std::fill_n(outputs[0], numSamples, outputSignal);
			}

			// Update output parameter
			SetParameterValue(Output_ID, numSamples > 0 ? outputs[0][numSamples - 1] : 0.0f);
		}
		else
		{
			// No output buffer
			SetParameterValue(Output_ID, 0.0f);
		}
	}

	void GainNode::Initialize(f64 sampleRate, u32 samplesPerBlock)
	{
		NodeProcessor::Initialize(sampleRate, samplesPerBlock);
		m_SampleRate = sampleRate;
		Reset();
	}

	void GainNode::Reset()
	{
		SetParameterValue(Output_ID, 0.0f, false);
	}

	void GainNode::SetGain(f32 gain)
	{
		SetParameterValue(Gain_ID, std::clamp(gain, 0.0f, 10.0f));
	}

	f32 GainNode::GetGain() const
	{
		return GetParameterValue<f32>(Gain_ID);
	}

	void GainNode::SetMute(bool mute)
	{
		SetParameterValue(Mute_ID, mute ? 1.0f : 0.0f);
	}

	bool GainNode::IsMuted() const
	{
		return GetParameterValue<f32>(Mute_ID) > 0.5f;
	}

} // namespace OloEngine::Audio::SoundGraph