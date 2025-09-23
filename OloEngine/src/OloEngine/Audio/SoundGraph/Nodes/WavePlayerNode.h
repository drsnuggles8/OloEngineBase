#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioLoader.h"

#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Wave Player Node - plays audio files
	class WavePlayerNode : public NodeProcessor
	{
	public:
		explicit WavePlayerNode();
		virtual ~WavePlayerNode() = default;

		// NodeProcessor overrides
		void Process(f32** inputs, f32** outputs, u32 numSamples) override;
		void Update(f64 deltaTime) override;
		void Initialize(f64 sampleRate, u32 maxBufferSize) override;
		
		Identifier GetTypeID() const override { return OLO_IDENTIFIER("WavePlayer"); }
		const char* GetDisplayName() const override { return "Wave Player"; }

		//==============================================================================
		/// Configuration

		// Set the audio file to play (uses file path for now)
		void SetAudioFile(const std::string& filePath);
		
		// Set audio data directly (for streamed/generated audio)
		void SetAudioData(const f32* data, u32 numFrames, u32 numChannels);
		
		// Set AudioFile asset handle (future integration)
		void SetAudioFile(AssetHandle audioFileHandle);

		// Get current playback state
		bool IsPlaying() const { return m_IsPlaying; }
		bool IsPaused() const { return m_IsPaused; }
		f64 GetPlaybackPosition() const { return m_PlaybackPosition; }
		f64 GetDuration() const { return m_Duration; }
		bool IsLooping() const { return GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), false); }
		i32 GetCurrentLoopCount() const { return m_CurrentLoopCount; }
		i32 GetMaxLoopCount() const { return GetParameterValue<i32>(OLO_IDENTIFIER("LoopCount"), -1); }
		f64 GetLoopStart() const { return GetParameterValue<f64>(OLO_IDENTIFIER("LoopStart"), 0.0); }
		f64 GetLoopEnd() const { return GetParameterValue<f64>(OLO_IDENTIFIER("LoopEnd"), -1.0); }

		// Parameter setters (for serialization)
		void SetVolume(f32 volume) { 
			m_Volume = volume; 
			SetParameterValue(OLO_IDENTIFIER("Volume"), volume); 
		}
		void SetPitch(f32 pitch) { 
			m_Pitch = pitch; 
			SetParameterValue(OLO_IDENTIFIER("Pitch"), pitch); 
		}
		void SetLoop(bool loop) { 
			m_Loop = loop; 
			SetParameterValue(OLO_IDENTIFIER("Loop"), loop); 
		}
		void SetMaxLoopCount(i32 maxLoops) { 
			m_MaxLoopCount = maxLoops; 
			SetParameterValue(OLO_IDENTIFIER("LoopCount"), maxLoops); 
		}
		void SetStartTime(f64 startTime) { 
			m_StartTime = startTime; 
			SetParameterValue(OLO_IDENTIFIER("StartTime"), startTime); 
		}
		void SetLoopStart(f64 loopStart) { 
			m_LoopStart = loopStart; 
			SetParameterValue(OLO_IDENTIFIER("LoopStart"), loopStart); 
		}
		void SetLoopEnd(f64 loopEnd) { 
			m_LoopEnd = loopEnd; 
			SetParameterValue(OLO_IDENTIFIER("LoopEnd"), loopEnd); 
		}

		// Parameter getters
		f32 GetVolume() const { return GetParameterValue<f32>(OLO_IDENTIFIER("Volume"), 1.0f); }
		f32 GetPitch() const { return GetParameterValue<f32>(OLO_IDENTIFIER("Pitch"), 1.0f); }
		bool GetLoop() const { return GetParameterValue<bool>(OLO_IDENTIFIER("Loop"), false); }
		f64 GetStartTime() const { return GetParameterValue<f64>(OLO_IDENTIFIER("StartTime"), 0.0); }

		// File loading (for serialization)
		void LoadAudioFile(const std::string& filePath);

	private:
		//==============================================================================
		/// Setup and Internal Methods
		void SetupEndpoints();
		void ProcessEvents();
		f32 GetSampleAtPosition(f64 position, u32 channel) const;
		void TriggerFinish();

		// Event handlers
		void OnPlayEvent(f32 value);
		void OnStopEvent(f32 value);
		void OnPauseEvent(f32 value);

		//==============================================================================
		/// Audio Data and State
		Audio::AudioData m_AudioData;  // Audio data loaded from file or asset
		f64 m_Duration = 0.0;           // Duration in seconds (derived from AudioData)

		// Playback state
		bool m_IsPlaying = false;
		bool m_IsPaused = false;
		f64 m_PlaybackPosition = 0.0; // in samples
		i32 m_CurrentLoopCount = 0;

		// Input parameters (temporary member variables for compatibility)
		f32 m_Volume = 1.0f;
		f32 m_Pitch = 1.0f;
		f64 m_StartTime = 0.0;
		bool m_Loop = false;
		i32 m_MaxLoopCount = -1;
		f64 m_LoopStart = 0.0;
		f64 m_LoopEnd = -1.0;

		// Output values
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;
		f32 m_PlaybackPositionOutput = 0.0f;

		//==============================================================================
		/// Event System
		
		// Event flags
		Flag m_PlayFlag;
		Flag m_StopFlag;
		Flag m_PauseFlag;

		// Event endpoints (stored as members)
		std::shared_ptr<InputEvent> m_PlayEvent;
		std::shared_ptr<InputEvent> m_StopEvent;
		std::shared_ptr<InputEvent> m_PauseEvent;
		std::shared_ptr<OutputEvent> m_OnPlayEvent;
		std::shared_ptr<OutputEvent> m_OnStopEvent;
		std::shared_ptr<OutputEvent> m_OnFinishEvent;
		std::shared_ptr<OutputEvent> m_OnLoopEvent;

		// Asset handle for AudioFile integration (future)
		AssetHandle m_AudioFileHandle = 0;
	};

}