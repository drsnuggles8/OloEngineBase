#include "OloEnginePCH.h"
#include "WavePlayerNode.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	WavePlayerNode::WavePlayerNode()
	{
		SetupEndpoints();
	}

	void WavePlayerNode::Process(f32** inputs, f32** outputs, u32 numSamples)
	{
		// Process events first
		ProcessEvents();

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
		if (!m_IsPlaying || m_IsPaused || !m_AudioData.IsValid())
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
		const f64 maxPosition = static_cast<f64>(m_AudioData.numFrames);

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

			if (m_AudioData.numChannels == 1)
			{
				// Mono - duplicate to both channels
				leftSample = rightSample = GetSampleAtPosition(m_PlaybackPosition, 0) * m_Volume;
			}
			else if (m_AudioData.numChannels >= 2)
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

	void WavePlayerNode::Update([[maybe_unused]] f64 deltaTime)
	{
		// Update any time-based parameters here if needed
		// For most audio processing, everything happens in Process()
	}

	void WavePlayerNode::Initialize(f64 sampleRate, u32 maxBufferSize)
	{
		m_SampleRate = sampleRate;
		
		// Reset playback state
		m_IsPlaying = false;
		m_IsPaused = false;
		m_PlaybackPosition = 0.0;
		m_CurrentLoopCount = 0;
		m_OutputLeft = 0.0f;
		m_OutputRight = 0.0f;
		m_PlaybackPositionOutput = 0.0f;
		
		OLO_CORE_TRACE("[WavePlayerNode] Initialized with sample rate {} and buffer size {}", sampleRate, maxBufferSize);
	}

	void WavePlayerNode::SetAudioFile(const std::string& filePath)
	{
		LoadAudioFile(filePath);
	}

	void WavePlayerNode::SetAudioFile(AssetHandle audioFileHandle)
	{
		m_AudioFileHandle = audioFileHandle;
		
		if (audioFileHandle == 0)
		{
			// Clear audio data if invalid handle
			m_AudioData.Clear();
			m_Duration = 0.0;
			return;
		}

		// Get the AudioFile asset
		auto audioFileAsset = AssetManager::GetAsset<AudioFile>(audioFileHandle);
		if (!audioFileAsset)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to get AudioFile asset for handle: {}", audioFileHandle);
			return;
		}

		// Get the file path for this asset  
		const auto& metadata = AssetManager::GetAssetMetadata(audioFileHandle);
		auto filePath = Project::GetAssetDirectory() / metadata.FilePath;
		
		// Load the actual audio data using AudioLoader
		if (!Audio::AudioLoader::LoadAudioFile(filePath, m_AudioData))
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to load audio file from asset: {}", filePath.string());
			return;
		}

		// Update duration from loaded data
		m_Duration = m_AudioData.duration;

		OLO_CORE_TRACE("[WavePlayerNode] Successfully loaded AudioFile asset {} from: {}", audioFileHandle, filePath.string());
	}

	void WavePlayerNode::SetAudioData(const f32* data, u32 numFrames, u32 numChannels)
	{
		if (!data || numFrames == 0 || numChannels == 0)
		{
			OLO_CORE_ERROR("[WavePlayerNode] Invalid audio data provided");
			return;
		}

		// Clear existing data
		m_AudioData.Clear();

		// Set up audio data structure
		m_AudioData.numFrames = numFrames;
		m_AudioData.numChannels = numChannels;
		m_AudioData.sampleRate = m_SampleRate; // Use node's sample rate
		m_AudioData.duration = static_cast<f64>(numFrames) / m_SampleRate;

		// Copy audio data
		const u64 totalSamples = static_cast<u64>(numFrames) * numChannels;
		m_AudioData.samples.resize(totalSamples);
		std::memcpy(m_AudioData.samples.data(), data, totalSamples * sizeof(f32));

		// Update duration
		m_Duration = m_AudioData.duration;

		OLO_CORE_TRACE("[WavePlayerNode] Set audio data: {} frames, {} channels, {:.2f}s duration", 
			numFrames, numChannels, m_Duration);
	}

	void WavePlayerNode::SetupEndpoints()
	{
		// Input parameters
		AddParameter<f32>(OLO_IDENTIFIER("Volume"), "Volume", 1.0f);
		AddParameter<f32>(OLO_IDENTIFIER("Pitch"), "Pitch", 1.0f);
		AddParameter<f64>(OLO_IDENTIFIER("StartTime"), "StartTime", 0.0);
		AddParameter<bool>(OLO_IDENTIFIER("Loop"), "Loop", false);
		AddParameter<i32>(OLO_IDENTIFIER("LoopCount"), "LoopCount", -1);

		// Input events with flags
		m_PlayEvent = AddInputEvent<f32>(OLO_IDENTIFIER("Play"), "Play", 
			[this](f32) { m_PlayFlag.SetDirty(); });
		m_StopEvent = AddInputEvent<f32>(OLO_IDENTIFIER("Stop"), "Stop", 
			[this](f32) { m_StopFlag.SetDirty(); });
		m_PauseEvent = AddInputEvent<f32>(OLO_IDENTIFIER("Pause"), "Pause", 
			[this](f32) { m_PauseFlag.SetDirty(); });

		// Output events
		m_OnPlayEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnPlay"), "OnPlay");
		m_OnStopEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnStop"), "OnStop");
		m_OnFinishEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnFinish"), "OnFinish");
		m_OnLoopEvent = AddOutputEvent<f32>(OLO_IDENTIFIER("OnLoop"), "OnLoop");

		// Output parameters
		AddParameter<f32>(OLO_IDENTIFIER("OutLeft"), "OutLeft", 0.0f);
		AddParameter<f32>(OLO_IDENTIFIER("OutRight"), "OutRight", 0.0f);
		AddParameter<f32>(OLO_IDENTIFIER("PlaybackPosition"), "PlaybackPosition", 0.0f);
	}

	void WavePlayerNode::ProcessEvents()
	{
		// Process play event
		if (m_PlayFlag.CheckAndResetIfDirty())
		{
			OnPlayEvent(1.0f);
		}

		// Process stop event
		if (m_StopFlag.CheckAndResetIfDirty())
		{
			OnStopEvent(1.0f);
		}

		// Process pause event
		if (m_PauseFlag.CheckAndResetIfDirty())
		{
			OnPauseEvent(1.0f);
		}
	}

	void WavePlayerNode::LoadAudioFile(const std::string& filePath)
	{
		// Use AudioLoader to load the file
		if (!Audio::AudioLoader::LoadAudioFile(filePath, m_AudioData))
		{
			OLO_CORE_ERROR("[WavePlayerNode] Failed to load audio file: {}", filePath);
			return;
		}

		// Update duration from loaded data
		m_Duration = m_AudioData.duration;

		OLO_CORE_TRACE("[WavePlayerNode] Successfully loaded audio file: {}", filePath);
	}

	f32 WavePlayerNode::GetSampleAtPosition(f64 position, u32 channel) const
	{
		return m_AudioData.GetSample(static_cast<u64>(position), channel);
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
}
