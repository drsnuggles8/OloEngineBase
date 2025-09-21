#include "OloEnginePCH.h"
#include "SoundGraphPlayer.h"

#include <algorithm>

namespace OloEngine::Audio
{
	//==============================================================================
	/// SoundGraphPlayer Implementation

	SoundGraphPlayer::SoundGraphPlayer(Ref<SoundGraph::SoundGraph> soundGraph)
		: m_SoundGraph(soundGraph)
	{
		if (!m_SoundGraph)
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Cannot create player with null sound graph");
			return;
		}

		OLO_CORE_TRACE("[SoundGraphPlayer] Created player for sound graph '{}'", m_SoundGraph->m_DebugName);
	}

	SoundGraphPlayer::~SoundGraphPlayer()
	{
		Stop();
	}

	void SoundGraphPlayer::Play()
	{
		if (!m_SoundGraph)
			return;

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (!m_IsPlaying.load())
		{
			m_IsPlaying.store(true);
			m_IsPaused.store(false);
			m_SoundGraph->Play();

			OLO_CORE_TRACE("[SoundGraphPlayer] Started playing sound graph '{}'", m_SoundGraph->m_DebugName);
		}
	}

	void SoundGraphPlayer::Stop()
	{
		if (!m_SoundGraph)
			return;

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (m_IsPlaying.load())
		{
			m_IsPlaying.store(false);
			m_IsPaused.store(false);
			m_SoundGraph->Stop();

			OLO_CORE_TRACE("[SoundGraphPlayer] Stopped sound graph '{}'", m_SoundGraph->m_DebugName);
		}
	}

	void SoundGraphPlayer::Pause()
	{
		if (!m_SoundGraph)
			return;

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (m_IsPlaying.load() && !m_IsPaused.load())
		{
			m_IsPaused.store(true);
			OLO_CORE_TRACE("[SoundGraphPlayer] Paused sound graph '{}'", m_SoundGraph->m_DebugName);
		}
	}

	void SoundGraphPlayer::Resume()
	{
		if (!m_SoundGraph)
			return;

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (m_IsPlaying.load() && m_IsPaused.load())
		{
			m_IsPaused.store(false);
			OLO_CORE_TRACE("[SoundGraphPlayer] Resumed sound graph '{}'", m_SoundGraph->m_DebugName);
		}
	}

	void SoundGraphPlayer::ProcessAudio(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		if (!m_SoundGraph || !m_IsPlaying.load() || m_IsPaused.load())
		{
			// Fill with silence
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] = 0.0f;
				rightChannel[i] = 0.0f;
			}
			return;
		}

		// Process the sound graph
		m_SoundGraph->Process(leftChannel, rightChannel, numSamples);

		// Apply volume
		if (m_Volume != 1.0f)
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] *= m_Volume;
				rightChannel[i] *= m_Volume;
			}
		}

		// Check if the sound graph has finished
		if (!m_SoundGraph->IsPlaying() && m_IsPlaying.load())
		{
			if (m_Loop)
			{
				// Restart the sound graph
				m_SoundGraph->Reset();
				m_SoundGraph->Play();
			}
			else
			{
				OnSoundGraphFinished();
			}
		}
	}

	void SoundGraphPlayer::Update(f32 deltaTime)
	{
		if (m_SoundGraph)
		{
			m_SoundGraph->Update(deltaTime);
		}
	}

	void SoundGraphPlayer::OnSoundGraphFinished()
	{
		m_IsPlaying.store(false);
		m_IsPaused.store(false);
		OLO_CORE_TRACE("[SoundGraphPlayer] Sound graph '{}' finished playing", m_SoundGraph->m_DebugName);
	}

	//==============================================================================
	/// SoundGraphManager Implementation

	SoundGraphManager& SoundGraphManager::GetInstance()
	{
		static SoundGraphManager instance;
		return instance;
	}

	Ref<SoundGraphPlayer> SoundGraphManager::CreatePlayer(Ref<SoundGraph::SoundGraph> soundGraph)
	{
		if (!soundGraph)
		{
			OLO_CORE_ERROR("[SoundGraphManager] Cannot create player with null sound graph");
			return nullptr;
		}

		// Initialize the sound graph with our sample rate
		soundGraph->Initialize(m_SampleRate);

		auto player = Ref<SoundGraphPlayer>::Create(soundGraph);

		// Add to our list of players
		{
			std::lock_guard<std::mutex> lock(m_PlayersMutex);
			m_Players.push_back(player);
		}

		OLO_CORE_TRACE("[SoundGraphManager] Created sound graph player (total: {})", m_Players.size());
		return player;
	}

	void SoundGraphManager::RemovePlayer(Ref<SoundGraphPlayer> player)
	{
		if (!player)
			return;

		std::lock_guard<std::mutex> lock(m_PlayersMutex);

		auto it = std::find(m_Players.begin(), m_Players.end(), player);
		if (it != m_Players.end())
		{
			m_Players.erase(it);
			OLO_CORE_TRACE("[SoundGraphManager] Removed sound graph player (total: {})", m_Players.size());
		}
	}

	void SoundGraphManager::ProcessAudio(f32* leftChannel, f32* rightChannel, u32 numSamples)
	{
		// Clear output buffers
		for (u32 i = 0; i < numSamples; ++i)
		{
			leftChannel[i] = 0.0f;
			rightChannel[i] = 0.0f;
		}

		// Ensure temp buffers are large enough
		if (m_TempLeftBuffer.size() < numSamples)
		{
			m_TempLeftBuffer.resize(numSamples);
			m_TempRightBuffer.resize(numSamples);
		}

		// Process all active players
		{
			std::lock_guard<std::mutex> lock(m_PlayersMutex);

			for (auto& player : m_Players)
			{
				if (!player || !player->IsPlaying())
					continue;

				// Process this player into temp buffers
				player->ProcessAudio(m_TempLeftBuffer.data(), m_TempRightBuffer.data(), numSamples);

				// Mix into output buffers
				for (u32 i = 0; i < numSamples; ++i)
				{
					leftChannel[i] += m_TempLeftBuffer[i];
					rightChannel[i] += m_TempRightBuffer[i];
				}
			}
		}

		// Apply master volume
		if (m_MasterVolume != 1.0f)
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				leftChannel[i] *= m_MasterVolume;
				rightChannel[i] *= m_MasterVolume;
			}
		}

		// Clean up finished players
		{
			std::lock_guard<std::mutex> lock(m_PlayersMutex);
			
			m_Players.erase(
				std::remove_if(m_Players.begin(), m_Players.end(),
					[](const Ref<SoundGraphPlayer>& player)
					{
						return !player || (!player->IsPlaying() && !player->IsLooping());
					}),
				m_Players.end()
			);
		}
	}

	void SoundGraphManager::Update(f32 deltaTime)
	{
		std::lock_guard<std::mutex> lock(m_PlayersMutex);

		for (auto& player : m_Players)
		{
			if (player)
			{
				player->Update(deltaTime);
			}
		}
	}

	void SoundGraphManager::Initialize(f64 sampleRate)
	{
		m_SampleRate = sampleRate;
		
		// Reserve space for temp buffers (reasonable default)
		m_TempLeftBuffer.reserve(1024);
		m_TempRightBuffer.reserve(1024);

		OLO_CORE_TRACE("[SoundGraphManager] Initialized with sample rate {}", sampleRate);
	}

	void SoundGraphManager::Shutdown()
	{
		std::lock_guard<std::mutex> lock(m_PlayersMutex);

		// Stop all players
		for (auto& player : m_Players)
		{
			if (player)
			{
				player->Stop();
			}
		}

		m_Players.clear();
		m_TempLeftBuffer.clear();
		m_TempRightBuffer.clear();

		OLO_CORE_TRACE("[SoundGraphManager] Shutdown complete");
	}
}