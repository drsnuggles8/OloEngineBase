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

	void WavePlayerNode::Process(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		// Clear output if not playing
		if (!m_IsPlaying || m_IsPaused || m_AudioData.empty())
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] = 0.0f;
				rightChannel[i] = 0.0f;
			}
			m_OutputLeft = 0.0f;
			m_OutputRight = 0.0f;
			return;
		}

		const f64 sampleIncrement = m_Pitch; // Pitch affects playback speed
		const f64 maxPosition = static_cast<f64>(m_NumFrames);

		for (u32 i = 0; i < numSamples; ++i)
		{
			// Check if we've reached the end
			if (m_PlaybackPosition >= maxPosition)
			{
				if (m_Loop && (m_MaxLoopCount < 0 || m_CurrentLoopCount < m_MaxLoopCount))
				{
					// Loop back to start time
					m_PlaybackPosition = m_StartTime * m_SampleRate;
					m_CurrentLoopCount++;
					
					// Trigger loop event
					m_OnLoop.Trigger(static_cast<f32>(m_CurrentLoopCount));
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

			// Write to output buffers
			leftChannel[i] = leftSample;
			rightChannel[i] = rightSample;

			// Update output values (last sample in the buffer)
			m_OutputLeft = leftSample;
			m_OutputRight = rightSample;

			// Advance playback position
			m_PlaybackPosition += sampleIncrement;
		}

		// Update playback position output (normalized 0-1)
		if (m_Duration > 0.0)
		{
			m_PlaybackPositionOutput = static_cast<f32>((m_PlaybackPosition / m_SampleRate) / m_Duration);
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
		AddInputEvent(EndpointIDs::Play, [this](f32 value) { OnPlayEvent(value); });
		AddInputEvent(EndpointIDs::Stop, [this](f32 value) { OnStopEvent(value); });
		AddInputEvent(EndpointIDs::Pause, [this](f32 value) { OnPauseEvent(value); });

		// Input parameters
		AddInputValue(EndpointIDs::Volume, &m_Volume);
		AddInputValue(EndpointIDs::Pitch, &m_Pitch);
		AddInputValue(EndpointIDs::StartTime, reinterpret_cast<f32*>(&m_StartTime));
		AddInputValue(EndpointIDs::Loop, reinterpret_cast<f32*>(&m_Loop));
		AddInputValue(EndpointIDs::LoopCount, reinterpret_cast<f32*>(&m_MaxLoopCount));

		// Output values
		AddOutputValue(EndpointIDs::OutputLeft, &m_OutputLeft);
		AddOutputValue(EndpointIDs::OutputRight, &m_OutputRight);
		AddOutputValue(EndpointIDs::PlaybackPosition, &m_PlaybackPositionOutput);

		// Output events
		AddOutputEvent(EndpointIDs::OnPlay, [this](f32 value) { m_OnPlay.Trigger(value); });
		AddOutputEvent(EndpointIDs::OnStop, [this](f32 value) { m_OnStop.Trigger(value); });
		AddOutputEvent(EndpointIDs::OnFinish, [this](f32 value) { m_OnFinish.Trigger(value); });
		AddOutputEvent(EndpointIDs::OnLoop, [this](f32 value) { m_OnLoop.Trigger(value); });
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
		m_OnFinish.Trigger(1.0f);
		
		OLO_CORE_TRACE("[WavePlayerNode] '{}' finished playing", m_DebugName);
	}

	void WavePlayerNode::OnPlayEvent(f32 value)
	{
		if (!m_IsPlaying)
		{
			m_IsPlaying = true;
			m_IsPaused = false;
			m_PlaybackPosition = m_StartTime * m_SampleRate;
			m_CurrentLoopCount = 0;
			m_OnPlay.Trigger(value);
			
			OLO_CORE_TRACE("[WavePlayerNode] '{}' started playing", m_DebugName);
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
			m_OnStop.Trigger(value);
			
			OLO_CORE_TRACE("[WavePlayerNode] '{}' stopped", m_DebugName);
		}
	}

	void WavePlayerNode::OnPauseEvent([[maybe_unused]] f32 value)
	{
		if (m_IsPlaying)
		{
			m_IsPaused = !m_IsPaused;
			OLO_CORE_TRACE("[WavePlayerNode] '{}' {}", m_DebugName, m_IsPaused ? "paused" : "resumed");
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