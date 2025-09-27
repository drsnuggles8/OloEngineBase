#include "OloEnginePCH.h"
#include "SoundGraphPlayer.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	// SoundGraphPlayer Implementation

	SoundGraphPlayer::~SoundGraphPlayer()
	{
		Shutdown();
	}

	bool SoundGraphPlayer::Initialize(ma_engine* engine)
	{
		if (m_IsInitialized)
		{
			OLO_CORE_WARN("[SoundGraphPlayer] Already initialized");
			return false;
		}

		if (!engine)
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Invalid engine");
			return false;
		}

		m_Engine = engine;
		m_IsInitialized = true;

		OLO_CORE_TRACE("[SoundGraphPlayer] Initialized successfully");
		return true;
	}

	void SoundGraphPlayer::Shutdown()
	{
		if (!m_IsInitialized)
			return;

		// Stop and remove all sources
		for (auto& [id, source] : m_SoundGraphSources)
		{
			if (source)
			{
				source->SuspendProcessing(true);
				source->Shutdown();
			}
		}
		m_SoundGraphSources.clear();

		m_Engine = nullptr;
		m_IsInitialized = false;
		m_NextSourceID = 1;

		OLO_CORE_TRACE("[SoundGraphPlayer] Shutdown complete");
	}

	u32 SoundGraphPlayer::CreateSoundGraphSource(Ref<SoundGraph> soundGraph)
	{
		if (!m_IsInitialized || !soundGraph)
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Cannot create source - not initialized or invalid sound graph");
			return 0;
		}

		u32 sourceID = GetNextSourceID();
		auto source = CreateScope<SoundGraphSource>();

		// Initialize the source with our engine and sample rate
		// TODO: Get actual sample rate from engine
		u32 sampleRate = 48000;
		u32 blockSize = 512;
		
		if (!source->Initialize(m_Engine, sampleRate, blockSize))
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Failed to initialize sound graph source");
			return 0;
		}

		// Set up the sound graph
		source->ReplaceGraph(soundGraph);

		// Set up event callbacks
		source->SetMessageCallback([](u64 frameIndex, const char* message) {
			if (message[0] == '!')
			{
				OLO_CORE_ERROR("[SoundGraph] Frame {0}: {1}", frameIndex, message);
			}
			else if (message[0] == '*')
			{
				OLO_CORE_WARN("[SoundGraph] Frame {0}: {1}", frameIndex, message);
			}
			else
			{
				OLO_CORE_TRACE("[SoundGraph] Frame {0}: {1}", frameIndex, message);
			}
		});

		source->SetEventCallback([](u64 frameIndex, u32 endpointID, const Value& eventData) {
			// Handle graph events - could check for specific endpoint IDs like "OnFinished"
			OLO_CORE_TRACE("[SoundGraph] Event at frame {0}, endpoint {1}", frameIndex, endpointID);
		});

		m_SoundGraphSources[sourceID] = std::move(source);

		OLO_CORE_TRACE("[SoundGraphPlayer] Created sound graph source with ID {0}", sourceID);
		return sourceID;
	}

	bool SoundGraphPlayer::Play(u32 sourceID)
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {0} not found", sourceID);
			return false;
		}

		bool result = it->second->SendPlayEvent();
		if (result)
		{
			OLO_CORE_TRACE("[SoundGraphPlayer] Started playback of source {0}", sourceID);
			return true;
		}
		else
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Failed to start playback of source {0}", sourceID);
			return false;
		}
	}

	bool SoundGraphPlayer::Stop(u32 sourceID)
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {0} not found", sourceID);
			return false;
		}

		it->second->SuspendProcessing(true);
		OLO_CORE_TRACE("[SoundGraphPlayer] Stopped playback of source {0}", sourceID);
		return true;
	}

	bool SoundGraphPlayer::Pause(u32 sourceID)
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {0} not found", sourceID);
			return false;
		}

		// Pause by suspending processing
		it->second->SuspendProcessing(true);
		OLO_CORE_TRACE("[SoundGraphPlayer] Paused playback of source {0}", sourceID);
		return true;
	}

	bool SoundGraphPlayer::IsPlaying(u32 sourceID) const
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			return false;
		}

		return it->second->IsPlaying();
	}

	bool SoundGraphPlayer::RemoveSoundGraphSource(u32 sourceID)
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {} not found", sourceID);
			return false;
		}

		// Stop playback and uninitialize before removing
		it->second->SuspendProcessing(true);
		it->second->Shutdown();
		m_SoundGraphSources.erase(it);

		OLO_CORE_TRACE("[SoundGraphPlayer] Removed sound graph source {}", sourceID);
		return true;
	}

	Ref<SoundGraph> SoundGraphPlayer::GetSoundGraph(u32 sourceID) const
	{
		auto it = m_SoundGraphSources.find(sourceID);
		if (it == m_SoundGraphSources.end())
		{
			return nullptr;
		}

		return it->second->GetGraph();
	}

	void SoundGraphPlayer::SetMasterVolume(f32 volume)
	{
		m_MasterVolume = glm::clamp(volume, 0.0f, 2.0f);
		
		// Apply master volume to all sources
		for (auto& [id, source] : m_SoundGraphSources)
		{
			// We don't have direct volume control on individual sources yet
			// This would need integration with the sound graph parameter system
			// For now, we just store the master volume for future use
		}

		OLO_CORE_TRACE("[SoundGraphPlayer] Set master volume to {}", m_MasterVolume);
	}

	void SoundGraphPlayer::Update(f64 deltaTime)
	{
		// Update all sound graphs on the main thread
		for (auto& [id, source] : m_SoundGraphSources)
		{
			if (source)
			{
				source->Update(deltaTime);
			}
		}

		// Remove any finished sources
		auto it = m_SoundGraphSources.begin();
		while (it != m_SoundGraphSources.end())
		{
			if (!it->second->IsPlaying())
			{
				// Check if the sound has actually finished (not just stopped)
				// For now, we'll keep all sources around until explicitly removed
				++it;
			}
			else
			{
				++it;
			}
		}
	}

	u32 SoundGraphPlayer::GetActiveSourceCount() const
	{
		u32 count = 0;
		for (const auto& [id, source] : m_SoundGraphSources)
		{
			if (source && source->IsPlaying())
			{
				count++;
			}
		}
		return count;
	}

	//==============================================================================
	// SoundGraphManager Implementation

	SoundGraphManager& SoundGraphManager::GetInstance()
	{
		static SoundGraphManager instance;
		return instance;
	}

	bool SoundGraphManager::Initialize(ma_engine* engine)
	{
		if (m_IsInitialized)
		{
			OLO_CORE_WARN("[SoundGraphManager] Already initialized");
			return false;
		}

		bool success = m_Player.Initialize(engine);
		if (success)
		{
			m_IsInitialized = true;
			OLO_CORE_TRACE("[SoundGraphManager] Initialized successfully");
		}
		else
		{
			OLO_CORE_ERROR("[SoundGraphManager] Failed to initialize");
		}

		return success;
	}

	void SoundGraphManager::Shutdown()
	{
		if (!m_IsInitialized)
			return;

		m_Player.Shutdown();
		m_IsInitialized = false;

		OLO_CORE_TRACE("[SoundGraphManager] Shutdown complete");
	}

	u32 SoundGraphManager::PlaySoundGraph(Ref<SoundGraph> soundGraph)
	{
		if (!m_IsInitialized || !soundGraph)
		{
			OLO_CORE_ERROR("[SoundGraphManager] Cannot play sound graph - not initialized or invalid graph");
			return 0;
		}

		u32 sourceID = m_Player.CreateSoundGraphSource(soundGraph);
		if (sourceID != 0)
		{
			if (m_Player.Play(sourceID))
			{
				OLO_CORE_TRACE("[SoundGraphManager] Playing sound graph with source ID {}", sourceID);
				return sourceID;
			}
			else
			{
				// Clean up the source if playback failed
				m_Player.RemoveSoundGraphSource(sourceID);
				return 0;
			}
		}

		return 0;
	}

	bool SoundGraphManager::StopSoundGraph(u32 sourceID)
	{
		if (!m_IsInitialized)
		{
			OLO_CORE_ERROR("[SoundGraphManager] Not initialized");
			return false;
		}

		return m_Player.Stop(sourceID);
	}

	bool SoundGraphManager::IsPlaying(u32 sourceID) const
	{
		if (!m_IsInitialized)
		{
			return false;
		}

		return m_Player.IsPlaying(sourceID);
	}

	void SoundGraphManager::Update(f64 deltaTime)
	{
		if (m_IsInitialized)
		{
			m_Player.Update(deltaTime);
		}
	}

}