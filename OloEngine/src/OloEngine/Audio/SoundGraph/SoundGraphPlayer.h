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

        u64 frame = 0;
        Level level = Trace;
        char text[256] = {};
        u32 endpointID = 0; // For event messages
        bool isEvent = false; // true for events, false for log messages
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
        u32 GetNextSourceID() 
        { 
            u32 id = m_NextSourceID.fetch_add(1, std::memory_order_relaxed);
            // Ensure we never return 0 (if wraparound occurs)
            if (id == 0)
            {
                id = m_NextSourceID.fetch_add(1, std::memory_order_relaxed);
            }
            return id;
        }
    };

}