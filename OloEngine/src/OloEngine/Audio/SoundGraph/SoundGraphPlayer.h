#pragma once

#include "OloEngine/Core/Base.h"
#include "SoundGraph.h"
#include "SoundGraphSource.h"
#include <miniaudio.h>
#include <unordered_map>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SoundGraphPlayer - Manages playback of sound graphs through the audio engine
	class SoundGraphPlayer
	{
	public:
		SoundGraphPlayer() = default;
		~SoundGraphPlayer();

		// Initialize with the audio engine
		bool Initialize(ma_engine* engine);
		void Shutdown();

		//==============================================================================
		/// Playback Management

		// Create a new sound graph source for playback
		u32 CreateSoundGraphSource(Ref<SoundGraph> soundGraph);

		// Play a sound graph source
		bool Play(u32 sourceID);

		// Stop a sound graph source
		bool Stop(u32 sourceID);

		// Pause a sound graph source
		bool Pause(u32 sourceID);

		// Check if a source is playing
		bool IsPlaying(u32 sourceID) const;

		// Remove a sound graph source
		bool RemoveSoundGraphSource(u32 sourceID);

		// Get the sound graph from a source ID
		Ref<SoundGraph> GetSoundGraph(u32 sourceID) const;

		//==============================================================================
		/// Global Controls

		// Set master volume for all sound graph sources
		void SetMasterVolume(f32 volume);
		f32 GetMasterVolume() const { return m_MasterVolume; }

		// Update all sound graphs (called from main thread)
		void Update(f64 deltaTime);

		//==============================================================================
		/// Debug and Statistics

		// Get number of active sound graph sources
		u32 GetActiveSourceCount() const;

		// Get total number of managed sources
		u32 GetTotalSourceCount() const { return static_cast<u32>(m_SoundGraphSources.size()); }

	private:
		// Audio engine reference
		ma_engine* m_Engine = nullptr;
		bool m_IsInitialized = false;

		// Master volume control
		f32 m_MasterVolume = 1.0f;

		// Sound graph sources
		std::unordered_map<u32, Scope<SoundGraphSource>> m_SoundGraphSources;
		u32 m_NextSourceID = 1;

		// Get next available source ID
		u32 GetNextSourceID() { return m_NextSourceID++; }
	};

	//==============================================================================
	/// SoundGraphManager - Singleton for global sound graph management
	class SoundGraphManager
	{
	public:
		static SoundGraphManager& GetInstance();

		// Initialize with the audio engine
		bool Initialize(ma_engine* engine);
		void Shutdown();

		// Get the sound graph player
		SoundGraphPlayer& GetPlayer() { return m_Player; }

		// Convenience methods that delegate to the player
		u32 PlaySoundGraph(Ref<SoundGraph> soundGraph);
		bool StopSoundGraph(u32 sourceID);
		bool IsPlaying(u32 sourceID) const;

		// Update (called from main thread)
		void Update(f64 deltaTime);

	private:
		SoundGraphManager() = default;
		~SoundGraphManager() = default;

		SoundGraphPlayer m_Player;
		bool m_IsInitialized = false;
	};

}