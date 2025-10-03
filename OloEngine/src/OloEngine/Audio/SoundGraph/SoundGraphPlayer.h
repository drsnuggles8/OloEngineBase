#pragma once

#include "OloEngine/Core/Base.h"
#include "SoundGraph.h"
#include "SoundGraphSource.h"
#include <miniaudio.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// Real-time safe message for audio thread logging
    struct RtMsg
    {
        enum Level : u8
        {
            Trace = 0,
            Warn = 1,
            Error = 2
        };

        u64 m_Frame = 0;
        Level m_Level = Trace;
        char m_Text[256] = {};
        u32 m_EndpointID = 0; // For event messages
        bool m_IsEvent = false; // true for events, false for log messages
    };

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

        void SetMasterVolume(f32 volume);
        f32 GetMasterVolume() const { return m_MasterVolume; }

        void Update(f64 deltaTime);

        //==============================================================================
        /// Debug and Statistics

        u32 GetActiveSourceCount() const;
        u32 GetTotalSourceCount() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return static_cast<u32>(m_SoundGraphSources.size()); 
        }

    private:
        // Audio engine reference
        ma_engine* m_Engine = nullptr;
        bool m_IsInitialized = false;

        // Master volume control
        f32 m_MasterVolume = 1.0f;

        // Thread synchronization
        mutable std::mutex m_Mutex;

        // Sound graph sources (protected by m_Mutex)
        std::unordered_map<u32, Scope<SoundGraphSource>> m_SoundGraphSources;
        std::atomic<u32> m_NextSourceID{1};

        // Real-time safe logging system
        choc::fifo::SingleReaderSingleWriterFIFO<RtMsg> m_LogQueue;

        // Get next available source ID (thread-safe)
        // Returns a unique ID that is not currently in use, or 0 if all IDs are exhausted
        u32 GetNextSourceID() 
        { 
            constexpr u32 MaxAttempts = 1000; // Prevent infinite loop if many IDs are in use
            
            for (u32 attempt = 0; attempt < MaxAttempts; ++attempt)
            {
                u32 id = m_NextSourceID.fetch_add(1, std::memory_order_relaxed);
                
                // Skip 0 as it's reserved for error/invalid ID
                if (id == 0)
                    continue;
                
                // Thread-safe check: is this ID already in use?
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_SoundGraphSources.find(id) == m_SoundGraphSources.end())
                {
                    // ID is available
                    return id;
                }
                
                // ID collision detected (very rare unless wraparound occurred)
                // Continue to next candidate
            }
            
            // All attempts exhausted - this should be extremely rare
            // Only happens if ~1000+ consecutive IDs are all in use
            OLO_CORE_ERROR("[SoundGraphPlayer] Failed to allocate unique source ID after {} attempts", MaxAttempts);
            return 0; // Return 0 to indicate failure
        }
    };

}