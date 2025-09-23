#include "OloEnginePCH.h"
#include "WavePlayerNode.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	WavePlayerNode::WavePlayerNode(std::string_view debugName, UUID id)
		: NodeProcessor(debugName, id), m_OnPlay(*this), m_OnStop(*this), m_OnFinish(*this), m_OnLoop(*this)
	{
		InitializeEndpoints();
	}

	WavePlayerNode::~WavePlayerNode() = default;

	void WavePlayerNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		// For WavePlayer, we typically don't use inputs, but generate our own audio output
		// outputs[0] = left channel, outputs[1] = right channel
		f32* leftChannel = outputs[0];
		f32* rightChannel = outputs[1];
		// Read current parameter values
		f32 volume = GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 1.0f);
		f32 pitch = GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 1.0f);
		f64 startTime = GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), 0.0);
		bool loop = GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), false);
		i32 maxLoopCount = GetParameterValue<i32>(OLO_IDENTIFIER("LoopCount"), -1);

		// Clear output if not playing
		if (!m_IsPlaying || m_IsPaused || m_AudioData.empty())
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] = 0.0f;
				rightChannel[i] = 0.0f;
			}
			
			// Set output parameters
			SetParameterValue(OLO_IDENTIFIER("OutLeft"), 0.0f);
			SetParameterValue(OLO_IDENTIFIER("OutRight"), 0.0f);
			return;
		}

		const f64 sampleIncrement = pitch; // Pitch affects playback speed
		const f64 maxPosition = static_cast<f64>(m_NumFrames);

		for (u32 i = 0; i < numSamples; ++i)
		{
			// Check if we've reached the end
			if (m_PlaybackPosition >= maxPosition)
			{
				if (loop && (maxLoopCount < 0 || m_CurrentLoopCount < maxLoopCount))
				{
					// Loop back to start time
					m_PlaybackPosition = startTime * m_SampleRate;
					m_CurrentLoopCount++;
					
					// Trigger loop event
					TriggerOutputEvent("OnLoop", static_cast<f32>(m_CurrentLoopCount));
				}
				else
				{
					// Finished playing
					TriggerFinish();
					
					// Fill remaining samples with silence
					for (u32 j = i; j < numSamples; ++j)
					{
						leftChannel[j] = 0.0f;
						rightChannel[j] = 0.0f;
					}
					return;
				}
			}

			// Get samples from audio data
			f32 leftSample = 0.0f;
			f32 rightSample = 0.0f;

			if (m_NumChannels == 1)
			{
				// Mono - duplicate to both channels
				leftSample = rightSample = GetSampleAtPosition(m_PlaybackPosition, 0) * m_Volume;
			}
			else if (m_NumChannels >= 2)
			{
				// Stereo or more - use first two channels
				leftSample = GetSampleAtPosition(m_PlaybackPosition, 0) * m_Volume;
				rightSample = GetSampleAtPosition(m_PlaybackPosition, 1) * m_Volume;
			}

			// Apply volume
			leftSample *= volume;
			rightSample *= volume;

			// Write to output buffers
			leftChannel[i] = leftSample;
			rightChannel[i] = rightSample;

			// Advance playback position
			m_PlaybackPosition += sampleIncrement;
		}

		// Update output parameters (last sample in the buffer for continuous values)
		SetParameterValue(OLO_IDENTIFIER("OutLeft"), leftChannel[numSamples - 1]);
		SetParameterValue(OLO_IDENTIFIER("OutRight"), rightChannel[numSamples - 1]);

		// Update playback position parameter (normalized 0-1)
		if (m_Duration > 0.0)
		{
			f32 normalizedPosition = static_cast<f32>((m_PlaybackPosition / m_SampleRate) / m_Duration);
			SetParameterValue(OLO_IDENTIFIER("PlaybackPosition"), normalizedPosition);
		}
	}

	void WavePlayerNode::Update([[maybe_unused]] f32 deltaTime)
	{
		// Update any time-based parameters here if needed
		// For most audio processing, everything happens in Process()
	}

	void WavePlayerNode::Initialize(f64 sampleRate)
	{
		NodeProcessor::Initialize(sampleRate);
		
		// Reset playback state
		Reset();
		
		OLO_CORE_TRACE("[WavePlayerNode] Initialized '{}' with sample rate {}", m_DebugName, sampleRate);
	}

	void WavePlayerNode::Reset()
	{
		NodeProcessor::Reset();
		
		m_IsPlaying = false;
		m_IsPaused = false;
		m_PlaybackPosition = 0.0;
		m_CurrentLoopCount = 0;
		m_OutputLeft = 0.0f;
		m_OutputRight = 0.0f;
		m_PlaybackPositionOutput = 0.0f;
	}

	void WavePlayerNode::SetAudioFile(const std::string& filePath)
	{
		LoadAudioFile(filePath);
	}

	void WavePlayerNode::SetAudioData(const f32* data, u32 numFrames, u32 numChannels)
	{
		if (!data || numFrames == 0 || numChannels == 0)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Invalid audio data provided");
			return;
		}

		m_NumFrames = numFrames;
		m_NumChannels = numChannels;
		m_Duration = static_cast<f64>(numFrames) / m_SampleRate;

		// Copy audio data
		const u32 totalSamples = numFrames * numChannels;
		m_AudioData.resize(totalSamples);
		std::memcpy(m_AudioData.data(), data, totalSamples * sizeof(f32));

		OLO_CORE_TRACE("[WavePlayerNode] Set audio data: {} frames, {} channels, {:.2f}s duration", 
			numFrames, numChannels, m_Duration);
	}

	void WavePlayerNode::InitializeEndpoints()
	{
		// Input events
		AddInputEvent<f32>(OLO_IDENTIFIER("Play"), "Play", [this](f32 value) { OnPlayEvent(value); });
		AddInputEvent<f32>(OLO_IDENTIFIER("Stop"), "Stop", [this](f32 value) { OnStopEvent(value); });
		AddInputEvent<f32>(OLO_IDENTIFIER("Pause"), "Pause", [this](f32 value) { OnPauseEvent(value); });

		// Input parameters
		AddParameter<f32>(OLO_IDENTIFIER("Volume"), "Volume", 1.0f);
		AddParameter<f32>(OLO_IDENTIFIER("Pitch"), "Pitch", 1.0f);
		AddParameter<f64>(OLO_IDENTIFIER("StartTime"), "StartTime", 0.0);
		AddParameter<bool>(OLO_IDENTIFIER("Loop"), "Loop", false);
		AddParameter<i32>(OLO_IDENTIFIER("LoopCount"), "LoopCount", -1);

		// Output parameters
		AddParameter<f32>(OLO_IDENTIFIER("OutLeft"), "OutLeft", 0.0f);
		AddParameter<f32>(OLO_IDENTIFIER("OutRight"), "OutRight", 0.0f);
		AddParameter<f32>(OLO_IDENTIFIER("PlaybackPosition"), "PlaybackPosition", 0.0f);

		// Output events
		AddOutputEvent<f32>(OLO_IDENTIFIER("OnPlay"), "OnPlay");
		AddOutputEvent<f32>(OLO_IDENTIFIER("OnStop"), "OnStop");
		AddOutputEvent<f32>(OLO_IDENTIFIER("OnFinish"), "OnFinish");
		AddOutputEvent<f32>(OLO_IDENTIFIER("OnLoop"), "OnLoop");
	}

	void WavePlayerNode::LoadAudioFile(const std::string& filePath)
	{
		// Use miniaudio to load the file
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0); // Let miniaudio determine channels and sample rate
		
		ma_result result = ma_decoder_init_file(filePath.c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to initialize decoder for file: {}", filePath);
			return;
		}

		// Get audio file info
		ma_uint64 totalFrames;
		result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to get frame count for file: {}", filePath);
			ma_decoder_uninit(&decoder);
			return;
		}

		m_NumChannels = decoder.outputChannels;
		m_NumFrames = static_cast<u32>(totalFrames);
		m_Duration = static_cast<f64>(totalFrames) / static_cast<f64>(decoder.outputSampleRate);

		// Allocate buffer and read audio data
		const u32 totalSamples = m_NumFrames * m_NumChannels;
		m_AudioData.resize(totalSamples);

		ma_uint64 framesRead;
		result = ma_decoder_read_pcm_frames(&decoder, m_AudioData.data(), totalFrames, &framesRead);
		
		ma_decoder_uninit(&decoder);

		if (result != MA_SUCCESS || framesRead != totalFrames)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to read audio data from file: {}", filePath);
			m_AudioData.clear();
			m_NumFrames = 0;
			m_NumChannels = 0;
			m_Duration = 0.0;
			return;
		}

		OLO_CORE_TRACE("[WavePlayerNode] Loaded audio file '{}': {} frames, {} channels, {:.2f}s duration", 
			filePath, m_NumFrames, m_NumChannels, m_Duration);
	}

	f32 WavePlayerNode::GetSampleAtPosition(f64 position, u32 channel) const
	{
		if (m_AudioData.empty() || channel >= m_NumChannels)
			return 0.0f;

		// Convert to integer position
		const u64 intPosition = static_cast<u64>(position);
		
		// Boundary check
		if (intPosition >= m_NumFrames)
			return 0.0f;

		// Calculate sample index
		const u64 sampleIndex = intPosition * m_NumChannels + channel;
		
		if (sampleIndex >= m_AudioData.size())
			return 0.0f;

		// For now, use nearest neighbor sampling
		// In a more advanced implementation, you'd use linear interpolation
		return m_AudioData[sampleIndex];
	}

	void WavePlayerNode::TriggerFinish()
	{
		m_IsPlaying = false;
		m_PlaybackPosition = 0.0;
		m_CurrentLoopCount = 0;
		TriggerOutputEvent("OnFinish", 1.0f);
		
		OLO_CORE_TRACE("[WavePlayerNode] '{}' finished playing", GetDisplayName());
	}

	void WavePlayerNode::OnPlayEvent(f32 value)
	{
		if (!m_IsPlaying)
		{
			f64 startTime = GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), 0.0);
			
			m_IsPlaying = true;
			m_IsPaused = false;
			m_PlaybackPosition = startTime * m_SampleRate;
			m_CurrentLoopCount = 0;
			TriggerOutputEvent("OnPlay", value);
			
			OLO_CORE_TRACE("[WavePlayerNode] '{}' started playing", GetDisplayName());
		}
	}

	void WavePlayerNode::OnStopEvent(f32 value)
	{
		if (m_IsPlaying)
		{
			m_IsPlaying = false;
			m_IsPaused = false;
			m_PlaybackPosition = 0.0;
			m_CurrentLoopCount = 0;
			TriggerOutputEvent("OnStop", value);
			
			OLO_CORE_TRACE("[WavePlayerNode] '{}' stopped", GetDisplayName());
		}
	}

	void WavePlayerNode::OnPauseEvent([[maybe_unused]] f32 value)
	{
		if (m_IsPlaying)
		{
			m_IsPaused = !m_IsPaused;
			OLO_CORE_TRACE("[WavePlayerNode] '{}' {}", GetDisplayName(), m_IsPaused ? "paused" : "resumed");
		}
	}

	//==============================================================================
	/// AudioFileAsset Implementation

	bool AudioFileAsset::LoadFromFile(const std::string& filePath)
	{
		// Use miniaudio to load the file
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
		
		ma_result result = ma_decoder_init_file(filePath.c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioFileAsset] Failed to initialize decoder for file: {}", filePath);
			return false;
		}

		// Get audio file info
		ma_uint64 totalFrames;
		result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioFileAsset] Failed to get frame count for file: {}", filePath);
			ma_decoder_uninit(&decoder);
			return false;
		}

		NumChannels = decoder.outputChannels;
		NumFrames = static_cast<u32>(totalFrames);
		SampleRate = static_cast<f64>(decoder.outputSampleRate);
		Duration = static_cast<f64>(totalFrames) / SampleRate;

		// Allocate buffer and read audio data
		const u32 totalSamples = NumFrames * NumChannels;
		Data.resize(totalSamples);

		ma_uint64 framesRead;
		result = ma_decoder_read_pcm_frames(&decoder, Data.data(), totalFrames, &framesRead);
		
		ma_decoder_uninit(&decoder);

		if (result != MA_SUCCESS || framesRead != totalFrames)
		{
			OLO_CORE_ERROR("[AudioFileAsset] Failed to read audio data from file: {}", filePath);
			Data.clear();
			NumFrames = 0;
			NumChannels = 0;
			SampleRate = 0.0;
			Duration = 0.0;
			return false;
		}

		OLO_CORE_TRACE("[AudioFileAsset] Loaded audio file '{}': {} frames, {} channels, {:.2f}s duration", 
			filePath, NumFrames, NumChannels, Duration);

		return true;
	}
}