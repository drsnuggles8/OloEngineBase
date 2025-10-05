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
        
        // Channel count configuration:
        // Currently defaults to stereo (2). In the future, this could be:
        // - Read from soundGraph metadata (if Prototype/SoundGraph stores channel info)
        // - Passed as a parameter to CreateSoundGraphSource()
        // - Inferred from graph output endpoint count
        // For now, use stereo as a sensible default for most use cases
        u32 channelCount = 2;
        
        if (!source->Initialize(m_Engine, sampleRate, blockSize, channelCount))
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to initialize sound graph source");
            return 0;
        }

        // Set up the sound graph
        source->ReplaceGraph(soundGraph);

        // Set up event callbacks
        source->SetMessageCallback([this](u64 frameIndex, const char* message) {
            // Early return for null or empty messages (realtime-safe)
            if (!message || message[0] == '\0')
                return;
            
            RealtimeMessage msg;
            msg.m_Frame = frameIndex;
            msg.m_IsEvent = false;
            
            // Determine log level based on first character (safe now that we've checked for null/empty)
            if (message[0] == '!')
            {
                msg.m_Level = RealtimeMessage::Error;
            }
            else if (message[0] == '*')
            {
                msg.m_Level = RealtimeMessage::Warn;
            }
            else
            {
                msg.m_Level = RealtimeMessage::Trace;
            }
            
            // Copy message text (realtime-safe bounded length computation)
            // Compute bounded length without strlen (avoid unbounded scan)
            constexpr sizet maxLen = sizeof(msg.m_Text) - 1;
            sizet len = 0;
            while (len < maxLen && message[len] != '\0')
            {
                ++len;
            }
            
            memcpy(msg.m_Text, message, len);
            msg.m_Text[len] = '\0';
            
            // Try to push to queue (drop if full to maintain real-time safety)
            m_LogQueue.push(std::move(msg));
        });

        source->SetEventCallback([this](u64 frameIndex, u32 endpointID, const choc::value::Value& eventData) {
            (void)eventData;
            RealtimeMessage msg;
            msg.m_Frame = frameIndex;
            msg.m_Level = RealtimeMessage::Trace;
            msg.m_EndpointID = endpointID;
            msg.m_IsEvent = true;
            
            // Format event message (real-time safe - use bounded length computation)
            const char* eventMsg = "Event";
            // Compute bounded length without strlen (avoid unbounded memory scan)
            constexpr sizet maxLen = sizeof(msg.m_Text) - 1;
            sizet len = 0;
            while (len < maxLen && eventMsg[len] != '\0')
            {
                ++len;
            }
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

        // Capture the current processing state before modifying it
        bool wasSuspended = it->second->IsSuspended();

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
            // Restore the original processing state since play failed
            it->second->SuspendProcessing(wasSuspended);
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

        // Stop playback: suspend processing and reset playback position
        it->second->SuspendProcessing(true);
        it->second->ResetPlayback();
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

        // Pause by suspending processing while preserving playback position
        // This allows resuming from the same position with Play()
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
        
        // Apply the master volume to the underlying miniaudio engine first
        ma_result result = ma_engine_set_volume(m_Engine, clampedVolume);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to set master volume on audio engine: {}", ma_result_description(result));
            return;
        }
        
        // Only update cached value after successful engine call to maintain consistency
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_MasterVolume = clampedVolume;
        }

        OLO_CORE_TRACE("[SoundGraphPlayer] Set master volume to {}", clampedVolume);
    }

    void SoundGraphPlayer::Update(f64 deltaTime)
    {
        OLO_PROFILE_FUNCTION();
        
        // Process real-time messages from audio thread (lock-free)
        RealtimeMessage msg;
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
                    case RealtimeMessage::Error:
                        OLO_CORE_ERROR("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                    case RealtimeMessage::Warn:
                        OLO_CORE_WARN("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                    case RealtimeMessage::Trace:
                    default:
                        OLO_CORE_TRACE("[SoundGraph] Frame {0}: {1}", msg.m_Frame, msg.m_Text);
                        break;
                }
            }
        }

        // Update all sound graphs on the main thread
        // CRITICAL FIX: Hold mutex during updates to prevent use-after-free.
        // Previously copied raw pointers then released mutex, creating race where
        // RemoveSource() could delete a source between unlock and Update() call.
        // Trade-off: Holding lock during updates reduces concurrency but ensures safety.
        // Future optimization: Could use shared_ptr if SoundGraphSource inherits RefCounted.
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& [id, source] : m_SoundGraphSources)
        {
            if (source)
            {
                source->Update(deltaTime);
            }
        }

        // Note: Sources are intentionally retained until explicitly removed via RemoveSource()
        // This allows sources to be reused and provides manual control over lifecycle management
    }

    u32 SoundGraphPlayer::GetActiveSourceCount() const
    {
        OLO_PROFILE_FUNCTION();
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

    u32 SoundGraphPlayer::GetNextSourceID()
    {
        OLO_PROFILE_FUNCTION();
        
        constexpr u32 MaxAttempts = 1000; // Prevent infinite loop if many IDs are in use
        constexpr u32 BatchSize = 16;     // Check this many candidate IDs per lock acquisition
        
        {
            OLO_PROFILE_SCOPE("GetNextSourceID - ID Allocation Loop");
            
            u32 attempt = 0;
            while (attempt < MaxAttempts)
            {
                // Generate a batch of candidate IDs using atomic operations (lock-free)
                std::array<u32, BatchSize> candidates;
                u32 validCandidateCount = 0;
                
                for (u32 i = 0; i < BatchSize && attempt < MaxAttempts; ++i, ++attempt)
                {
                    u32 id = m_NextSourceID.fetch_add(1, std::memory_order_relaxed);
                    
                    // Skip 0 as it's reserved for error/invalid ID
                    if (id != 0)
                    {
                        candidates[validCandidateCount++] = id;
                    }
                }
                
                // Now check all candidates under a single lock
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    for (u32 i = 0; i < validCandidateCount; ++i)
                    {
                        if (m_SoundGraphSources.find(candidates[i]) == m_SoundGraphSources.end())
                        {
                            // ID is available
                            return candidates[i];
                        }
                        // ID collision detected - continue checking next candidate in batch
                    }
                }
                
                // All candidates in this batch were in use, continue to next batch
            }
        }
        
        // All attempts exhausted - this should be extremely rare
        // Only happens if ~1000+ consecutive IDs are all in use
        OLO_CORE_ERROR("[SoundGraphPlayer] Failed to allocate unique source ID after {} attempts", MaxAttempts);
        return 0; // Return 0 to indicate failure
    }

}