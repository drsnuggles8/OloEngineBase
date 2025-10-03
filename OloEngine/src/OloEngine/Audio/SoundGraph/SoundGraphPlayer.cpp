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
        OLO_PROFILE_FUNCTION();
        
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
        
        // Initialize real-time message queue
        m_LogQueue.reset(512);
        
        m_IsInitialized = true;

        OLO_CORE_TRACE("[SoundGraphPlayer] Initialized successfully");
        return true;
    }

    void SoundGraphPlayer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_IsInitialized)
            return;

        // Stop and remove all sources
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (auto& [id, source] : m_SoundGraphSources)
            {
                if (source)
                {
                    source->SuspendProcessing(true);
                    source->Shutdown();
                }
            }
            m_SoundGraphSources.clear();
        }

        m_Engine = nullptr;
        m_IsInitialized = false;
        m_NextSourceID.store(1, std::memory_order_relaxed);

        OLO_CORE_TRACE("[SoundGraphPlayer] Shutdown complete");
    }

    u32 SoundGraphPlayer::CreateSoundGraphSource(Ref<SoundGraph> soundGraph)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_IsInitialized || !soundGraph)
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Cannot create source - not initialized or invalid sound graph");
            return 0;
        }

        u32 sourceID = GetNextSourceID();
        auto source = CreateScope<SoundGraphSource>();

        // Initialize the source with our engine and sample rate
        u32 sampleRate = ma_engine_get_sample_rate(m_Engine);
        u32 blockSize = 512;
        
        if (!source->Initialize(m_Engine, sampleRate, blockSize))
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to initialize sound graph source");
            return 0;
        }

        // Set up the sound graph
        source->ReplaceGraph(soundGraph);

        // Set up event callbacks
        source->SetMessageCallback([this](u64 frameIndex, const char* message) {
            RtMsg msg;
            msg.m_Frame = frameIndex;
            msg.m_IsEvent = false;
            
            // Determine log level based on message prefix
            if (message[0] == '!')
            {
                msg.m_Level = RtMsg::Error;
            }
            else if (message[0] == '*')
            {
                msg.m_Level = RtMsg::Warn;
            }
            else
            {
                msg.m_Level = RtMsg::Trace;
            }
            
            // Copy message text (real-time safe)
            sizet len = strlen(message);
            if (len >= sizeof(msg.m_Text))
                len = sizeof(msg.m_Text) - 1;
            memcpy(msg.m_Text, message, len);
            msg.m_Text[len] = '\0';
            
            // Try to push to queue (drop if full to maintain real-time safety)
            m_LogQueue.push(std::move(msg));
        });

        source->SetEventCallback([this](u64 frameIndex, u32 endpointID, const choc::value::Value& eventData) {
            (void)eventData;
            RtMsg msg;
            msg.m_Frame = frameIndex;
            msg.m_Level = RtMsg::Trace;
            msg.m_EndpointID = endpointID;
            msg.m_IsEvent = true;
            
            // Format event message (real-time safe)
            const char* eventMsg = "Event";
            sizet len = strlen(eventMsg);
            if (len >= sizeof(msg.m_Text))
                len = sizeof(msg.m_Text) - 1;
            memcpy(msg.m_Text, eventMsg, len);
            msg.m_Text[len] = '\0';
            
            // Try to push to queue (drop if full to maintain real-time safety)
            m_LogQueue.push(std::move(msg));
        });

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_SoundGraphSources[sourceID] = std::move(source);
        }

        OLO_CORE_TRACE("[SoundGraphPlayer] Created sound graph source with ID {0}", sourceID);
        return sourceID;
    }

    bool SoundGraphPlayer::Play(u32 sourceID)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_SoundGraphSources.find(sourceID);
        if (it == m_SoundGraphSources.end())
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {0} not found", sourceID);
            return false;
        }

        // Resume processing before sending play event
        it->second->SuspendProcessing(false);

        bool result = it->second->SendPlayEvent();
        if (result)
        {
            OLO_CORE_TRACE("[SoundGraphPlayer] Started playback of source {0}", sourceID);
            return true;
        }
        else
        {
            // Re-suspend processing to restore previous state since play failed
            it->second->SuspendProcessing(true);
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to start playback of source {0}", sourceID);
            return false;
        }
    }

    bool SoundGraphPlayer::Stop(u32 sourceID)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
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
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
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
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_SoundGraphSources.find(sourceID);
        if (it == m_SoundGraphSources.end())
        {
            return false;
        }

        return it->second->IsPlaying();
    }

    bool SoundGraphPlayer::RemoveSoundGraphSource(u32 sourceID)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_SoundGraphSources.find(sourceID);
        if (it == m_SoundGraphSources.end())
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Source ID {0} not found", sourceID);
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
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_SoundGraphSources.find(sourceID);
        if (it == m_SoundGraphSources.end())
        {
            return nullptr;
        }

        return it->second->GetGraph();
    }

    void SoundGraphPlayer::SetMasterVolume(f32 volume)
    {
        OLO_PROFILE_FUNCTION();
        
        // Bail early if the audio engine is not initialized
        if (!m_IsInitialized || !m_Engine)
        {
            OLO_CORE_WARN("[SoundGraphPlayer] Cannot set master volume: audio engine not initialized");
            return;
        }

        // Clamp the volume value
        f32 clampedVolume = glm::clamp(volume, 0.0f, 2.0f);
        
        // Synchronize access to m_MasterVolume to prevent data races with GetMasterVolume()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_MasterVolume = clampedVolume;
        }
        
        // Apply the master volume to the underlying miniaudio engine
        ma_result result = ma_engine_set_volume(m_Engine, clampedVolume);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to set master volume on audio engine: {}", ma_result_description(result));
            return;
        }

        OLO_CORE_TRACE("[SoundGraphPlayer] Set master volume to {}", clampedVolume);
    }

    void SoundGraphPlayer::Update(f64 deltaTime)
    {
        OLO_PROFILE_FUNCTION();
        
        // Process real-time messages from audio thread (lock-free)
        RtMsg msg;
        while (m_LogQueue.pop(msg))
        {
            if (msg.m_IsEvent)
            {
                // Handle graph events
                OLO_CORE_TRACE("[SoundGraph] Event at frame {0}, endpoint {1}", msg.m_Frame, msg.m_EndpointID);
            }
            else
            {
                // Handle log messages
                switch (msg.m_Level)
                {
                    case RtMsg::Error:
                        OLO_CORE_ERROR("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                    case RtMsg::Warn:
                        OLO_CORE_WARN("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                    case RtMsg::Trace:
                    default:
                        OLO_CORE_TRACE("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                }
            }
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
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
        std::lock_guard<std::mutex> lock(m_Mutex);
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

}