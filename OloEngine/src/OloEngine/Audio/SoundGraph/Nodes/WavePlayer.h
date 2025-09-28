#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/WaveSource.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioLoader.h"

#include <random>
#include <chrono>
#include <array>
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
	struct WavePlayer : public NodeProcessor
	{
		struct IDs
		{
			DECLARE_ID(Play);
			DECLARE_ID(Stop);
		private:
			IDs() = delete;
		};

		explicit WavePlayer(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
		{
			// Input events using Flag system like Hazel
			AddInEvent(IDs::Play, [this](float v) { (void)v; m_PlayFlag.SetDirty(); });
			AddInEvent(IDs::Stop, [this](float v) { (void)v; m_StopFlag.SetDirty(); });

			RegisterEndpoints();
		}

		// Input parameters
		int64_t* in_WaveAsset = nullptr;		// Asset handle for wave file
		float* in_StartTime = nullptr;			// Start time offset in seconds
		bool* in_Loop = nullptr;				// Enable looping playback
		int* in_NumberOfLoops = nullptr;		// Number of loops (-1 = infinite)

		// Output audio channels
		float out_OutLeft{ 0.0f };
		float out_OutRight{ 0.0f };

		// Output events
		OutputEvent out_OnPlay{ *this };
		OutputEvent out_OnStop{ *this };
		OutputEvent out_OnFinished{ *this };
		OutputEvent out_OnLooped{ *this };

		void RegisterEndpoints();
		void InitializeInputs();

		void Init() final
		{
			InitializeInputs();
			
			// Initialize state
			m_IsInitialized = false;
			m_IsPlaying = false;
			m_FrameNumber = 0;
			m_StartSample = 0;
			m_LoopCount = 0;
			m_TotalFrames = 0;

			// Initialize buffer and wave source
			m_WaveSource.Clear();
			m_AudioData.Clear();
			UpdateWaveSourceIfNeeded();

			m_IsInitialized = true;
		}

		void Process() final
		{
			// Handle events using Flag system like Hazel
			if (m_PlayFlag.CheckAndResetIfDirty())
				StartPlayback();

			if (m_StopFlag.CheckAndResetIfDirty())
				StopPlayback(true);

			if (m_IsPlaying)
			{
				// Check if we've reached the end
				if (m_FrameNumber >= m_TotalFrames)
				{
					if (*in_Loop)
					{
					++m_LoopCount;
					out_OnLooped(2.0f);						// Check if we've completed all loops
						if (*in_NumberOfLoops >= 0 && m_LoopCount > *in_NumberOfLoops)
						{
							StopPlayback(true);
							OutputSilence();
						}
						else
						{
							// Loop back to start
							ReadNextFrame();
							m_FrameNumber = m_StartSample;
							m_WaveSource.ReadPosition = m_FrameNumber + 1;
						}
					}
					else
					{
						// No looping - stop playback
						StopPlayback(true);
						OutputSilence();
					}
				}
				else
				{
					// Read next frame of audio data
					ReadNextFrame();
					m_FrameNumber++;
					m_WaveSource.ReadPosition = m_FrameNumber;
				}
			}
			else
			{
				OutputSilence();
			}
		}

	private:
		void StartPlayback()
		{
			// Update wave source if asset changed
			UpdateWaveSourceIfNeeded();

			// Check if we have a valid asset
			if (!m_WaveSource.WaveHandle)
			{
				DBG("WavePlayer: Invalid wave asset handle, cannot start playback");
				StopPlayback(true);
				return;
			}

			// If already playing, refill buffer from start
			if (m_IsPlaying)
				ForceRefillBuffer();

			m_IsPlaying = true;
			out_OnPlay(2.0f);
			DBG("WavePlayer: Started playing");
		}

		void StopPlayback(bool notifyOnFinish)
		{
			m_IsPlaying = false;
			m_LoopCount = 0;
			m_FrameNumber = m_StartSample;
			m_WaveSource.ReadPosition = m_FrameNumber;

			UpdateWaveSourceIfNeeded();

			if (notifyOnFinish)
				out_OnFinished(2.0f);

			DBG("WavePlayer: Stopped playing");
		}

		void UpdateWaveSourceIfNeeded()
		{
			u64 waveAsset = static_cast<u64>(*in_WaveAsset);

			if (m_WaveSource.WaveHandle != waveAsset)
			{
				m_WaveSource.WaveHandle = waveAsset;

				if (m_WaveSource.WaveHandle)
				{
					// Integrate with OloEngine's AssetManager
					AssetHandle assetHandle = static_cast<AssetHandle>(waveAsset);
					AssetMetadata metadata = AssetManager::GetAssetMetadata(assetHandle);
					
					if (metadata.IsValid() && !metadata.FilePath.empty())
					{
						// Load audio data using AudioLoader
						AudioData audioData;
						if (AudioLoader::LoadAudioFile(metadata.FilePath, audioData))
						{
							// Update wave source with audio data
							m_WaveSource.TotalFrames = audioData.numFrames;
							m_AudioData = std::move(audioData); // Store audio data
							
							// Set up refill callback to read from loaded audio data
							m_WaveSource.onRefill = [this](Audio::WaveSource& source) -> bool {
								return FillBufferFromAudioData(source);
							};
							
							m_TotalFrames = m_WaveSource.TotalFrames;
							m_IsInitialized = true;
							
							OLO_CORE_INFO("WavePlayer: Loaded audio asset - {} channels, {} Hz, {:.2f}s duration",
								m_AudioData.numChannels, m_AudioData.sampleRate, m_AudioData.duration);
						}
						else
						{
							OLO_CORE_ERROR("WavePlayer: Failed to load audio file: {}", metadata.FilePath.string());
							m_TotalFrames = 0;
							m_IsInitialized = false;
						}
					}
					else
					{
						OLO_CORE_ERROR("WavePlayer: Invalid asset metadata for handle {}", assetHandle);
						m_TotalFrames = 0;
						m_IsInitialized = false;
					}
				}
				else
				{
					m_TotalFrames = 0;
					m_AudioData.Clear();
				}

				// Apply start time offset
				if (in_StartTime && *in_StartTime > 0.0f)
				{
					f64 sampleRate = m_AudioData.IsValid() ? m_AudioData.sampleRate : 48000.0;
					m_StartSample = static_cast<i64>(*in_StartTime * sampleRate);
					m_StartSample = glm::min(m_StartSample, m_TotalFrames - 1);
				}
				else
				{
					m_StartSample = 0;
				}

				m_FrameNumber = m_StartSample;
			}
		}

	public:
		void ForceRefillBuffer()
		{
			if (m_WaveSource.WaveHandle && m_WaveSource.onRefill)
			{
				m_WaveSource.ReadPosition = m_FrameNumber;
				m_WaveSource.Refill();
			}
		}

		Audio::WaveSource& GetWaveSource() { return m_WaveSource; }
		const Audio::WaveSource& GetWaveSource() const { return m_WaveSource; }

	private:

		void ReadNextFrame()
		{
			if (m_WaveSource.Channels.Available() >= 2) // Stereo frame
			{
				// Read interleaved stereo data
				out_OutLeft = m_WaveSource.Channels.Get();
				out_OutRight = m_WaveSource.Channels.Get();
			}
			else if (m_WaveSource.Channels.Available() >= 1) // Mono frame
			{
				// Mono - duplicate to both channels
				float sample = m_WaveSource.Channels.Get();
				out_OutLeft = sample;
				out_OutRight = sample;
			}
			else
			{
				// No data available - try to refill buffer
				if (m_WaveSource.onRefill && m_WaveSource.Refill())
				{
					// Buffer refilled, try reading again
					ReadNextFrame();
				}
				else
				{
					// No data available
					OutputSilence();
				}
			}
		}

		void OutputSilence()
		{
			out_OutLeft = 0.0f;
			out_OutRight = 0.0f;
		}

		bool FillBufferFromAudioData(Audio::WaveSource& source)
		{
			if (!m_AudioData.IsValid()) return false;
			
			const u32 framesToRead = 1024; // Read chunk size
			u64 startFrame = source.ReadPosition;
			u64 endFrame = glm::min(startFrame + framesToRead, static_cast<u64>(m_AudioData.numFrames));
			
			if (startFrame >= m_AudioData.numFrames) return false;
			
			// Fill the circular buffer with interleaved audio data
			for (u64 frame = startFrame; frame < endFrame; ++frame)
			{
				for (u32 channel = 0; channel < m_AudioData.numChannels; ++channel)
				{
					f32 sample = m_AudioData.GetSample(frame, channel);
					source.Channels.Push(sample);
				}
			}
			
			return true;
		}

		// State variables
		bool m_IsInitialized{ false };
		bool m_IsPlaying{ false };
		i64 m_FrameNumber{ 0 };
		i64 m_StartSample{ 0 };
		int m_LoopCount{ 0 };
		i64 m_TotalFrames{ 0 };

		// Flag system for events (like Hazel)
		Flag m_PlayFlag;
		Flag m_StopFlag;

		// Wave source using OloEngine's system
		Audio::WaveSource m_WaveSource;
		
		// Audio data storage for loaded files
		AudioData m_AudioData;
	};
}

#undef DECLARE_ID
#undef DBG