#pragma once

#include "SoundGraph.h"
#include "../AudioSource.h"

#include <thread>
#include <atomic>
#include <mutex>

namespace OloEngine::Audio
{
	//==============================================================================
	/// Sound Graph Player - integrates sound graphs with the audio system
	class SoundGraphPlayer : public RefCounted
	{
	public:
		explicit SoundGraphPlayer(Ref<SoundGraph::SoundGraph> soundGraph);
		virtual ~SoundGraphPlayer();

		//==============================================================================
		/// Playback Control

		void Play();
		void Stop();
		void Pause();
		void Resume();

		[[nodiscard]] bool IsPlaying() const { return m_IsPlaying.load(); }
		[[nodiscard]] bool IsPaused() const { return m_IsPaused.load(); }

		//==============================================================================
		/// Configuration

		void SetVolume(f32 volume) { m_Volume = volume; }
		[[nodiscard]] f32 GetVolume() const { return m_Volume; }

		void SetLoop(bool loop) { m_Loop = loop; }
		[[nodiscard]] bool IsLooping() const { return m_Loop; }

		// Get the underlying sound graph
		[[nodiscard]] Ref<SoundGraph::SoundGraph> GetSoundGraph() const { return m_SoundGraph; }

		//==============================================================================
		/// Audio Callback Integration

		// This would be called by the audio system's callback
		void ProcessAudio(f32* leftChannel, f32* rightChannel, u32 numSamples);

		// Update the sound graph (called from main thread)
		void Update(f32 deltaTime);

	private:
		Ref<SoundGraph::SoundGraph> m_SoundGraph;

		// Playback state
		std::atomic<bool> m_IsPlaying{ false };
		std::atomic<bool> m_IsPaused{ false };
		bool m_Loop = false;
		f32 m_Volume = 1.0f;

		// Thread safety
		mutable std::mutex m_Mutex;

		//==============================================================================
		/// Internal methods

		void OnSoundGraphFinished();
	};

	//==============================================================================
	/// Sound Graph Manager - manages all active sound graph players
	class SoundGraphManager
	{
	public:
		static SoundGraphManager& GetInstance();

		//==============================================================================
		/// Player Management

		// Create and register a new sound graph player
		Ref<SoundGraphPlayer> CreatePlayer(Ref<SoundGraph::SoundGraph> soundGraph);

		// Remove a player from the manager
		void RemovePlayer(Ref<SoundGraphPlayer> player);

		// Process audio for all active players (called by audio thread)
		void ProcessAudio(f32* leftChannel, f32* rightChannel, u32 numSamples);

		// Update all players (called from main thread)
		void Update(f32 deltaTime);

		//==============================================================================
		/// Global Settings

		void SetMasterVolume(f32 volume) { m_MasterVolume = volume; }
		[[nodiscard]] f32 GetMasterVolume() const { return m_MasterVolume; }

		// Initialize with the audio system sample rate
		void Initialize(f64 sampleRate);

		// Shutdown the manager
		void Shutdown();

	private:
		SoundGraphManager() = default;
		~SoundGraphManager() = default;

		std::vector<Ref<SoundGraphPlayer>> m_Players;
		f32 m_MasterVolume = 1.0f;
		f64 m_SampleRate = 48000.0;
		mutable std::mutex m_PlayersMutex;

		// Temporary buffers for mixing
		std::vector<f32> m_TempLeftBuffer;
		std::vector<f32> m_TempRightBuffer;
	};
}