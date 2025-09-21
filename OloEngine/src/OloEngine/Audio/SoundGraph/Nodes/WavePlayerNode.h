#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Asset/Asset.h"

#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Wave Player Node - plays audio files
	class WavePlayerNode : public NodeProcessor
	{
	public:
		// Endpoint identifiers
		struct EndpointIDs
		{
			static constexpr const char* Play = "Play";
			static constexpr const char* Stop = "Stop";
			static constexpr const char* Pause = "Pause";
			static constexpr const char* OutputLeft = "OutLeft";
			static constexpr const char* OutputRight = "OutRight";
			static constexpr const char* PlaybackPosition = "PlaybackPosition";
			static constexpr const char* OnPlay = "OnPlay";
			static constexpr const char* OnStop = "OnStop";
			static constexpr const char* OnFinish = "OnFinish";
			static constexpr const char* OnLoop = "OnLoop";

			// Input parameters
			static constexpr const char* Volume = "Volume";
			static constexpr const char* Pitch = "Pitch";
			static constexpr const char* StartTime = "StartTime";
			static constexpr const char* Loop = "Loop";
			static constexpr const char* LoopCount = "LoopCount";
		};

		explicit WavePlayerNode(std::string_view debugName, UUID id);
		virtual ~WavePlayerNode();

		// NodeProcessor overrides
		void Process(f32* leftChannel, f32* rightChannel, u32 numSamples) override;
		void Update(f32 deltaTime) override;
		void Initialize(f64 sampleRate) override;
		void Reset() override;

		//==============================================================================
		/// Configuration

		// Set the audio file to play
		void SetAudioFile(const std::string& filePath);
		
		// Set audio data directly (for streamed/generated audio)
		void SetAudioData(const f32* data, u32 numFrames, u32 numChannels);

		// Get current playback state
		bool IsPlaying() const { return m_IsPlaying; }
		bool IsPaused() const { return m_IsPaused; }
		f64 GetPlaybackPosition() const { return m_PlaybackPosition; }
		f64 GetDuration() const { return m_Duration; }
		bool IsLooping() const { return m_Loop; }

		// Parameter setters (for serialization)
		void SetVolume(f32 volume) { m_Volume = volume; }
		void SetPitch(f32 pitch) { m_Pitch = pitch; }
		void SetLoop(bool loop) { m_Loop = loop; }
		void SetStartTime(f64 startTime) { m_StartTime = startTime; }
		void SetMaxLoopCount(i32 count) { m_MaxLoopCount = count; }

		// Parameter getters
		f32 GetVolume() const { return m_Volume; }
		f32 GetPitch() const { return m_Pitch; }
		bool GetLoop() const { return m_Loop; }
		f64 GetStartTime() const { return m_StartTime; }
		i32 GetMaxLoopCount() const { return m_MaxLoopCount; }

		// File loading (for serialization)
		void LoadAudioFile(const std::string& filePath);

	private:
		// Audio data
		std::vector<f32> m_AudioData;
		u32 m_NumChannels = 0;
		u32 m_NumFrames = 0;
		f64 m_Duration = 0.0; // in seconds

		// Playback state
		bool m_IsPlaying = false;
		bool m_IsPaused = false;
		f64 m_PlaybackPosition = 0.0; // in samples
		i32 m_CurrentLoopCount = 0;

		// Input parameters (controlled by other nodes or external input)
		f32 m_Volume = 1.0f;
		f32 m_Pitch = 1.0f;
		f64 m_StartTime = 0.0; // in seconds
		bool m_Loop = false;
		i32 m_MaxLoopCount = -1; // -1 = infinite loops

		// Output values
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;
		f32 m_PlaybackPositionOutput = 0.0f;

		// Event outputs
		OutputEvent m_OnPlay;
		OutputEvent m_OnStop;
		OutputEvent m_OnFinish;
		OutputEvent m_OnLoop;

		//==============================================================================
		/// Internal methods

		void InitializeEndpoints();
		f32 GetSampleAtPosition(f64 position, u32 channel) const;
		void TriggerFinish();

		// Event handlers
		void OnPlayEvent(f32 value);
		void OnStopEvent(f32 value);
		void OnPauseEvent(f32 value);
	};

	//==============================================================================
	/// Audio File Asset - represents a loaded audio file
	struct AudioFileAsset
	{
		// Asset type
		static AssetType GetStaticType() { return AssetType::Audio; }
		AssetType GetAssetType() const { return GetStaticType(); }

		// Audio data
		std::vector<f32> Data;
		u32 NumChannels = 0;
		u32 NumFrames = 0;
		f64 SampleRate = 44100.0;
		f64 Duration = 0.0;

		// Load from file
		bool LoadFromFile(const std::string& filePath);

		// Get audio data
		const f32* GetData() const { return Data.data(); }
		u32 GetNumFrames() const { return NumFrames; }
		u32 GetNumChannels() const { return NumChannels; }
		f64 GetSampleRate() const { return SampleRate; }
		f64 GetDuration() const { return Duration; }
	};
}