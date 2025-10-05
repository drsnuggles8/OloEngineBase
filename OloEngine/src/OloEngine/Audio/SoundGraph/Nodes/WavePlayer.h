#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/WaveSource.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioLoader.h"

#include <chrono>
#include <array>
#include <type_traits>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>

#define LOG_DBG_MESSAGES 0

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

#define DECLARE_ID(name) static constexpr Identifier name{ #name }

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    struct WavePlayer : public NodeProcessor
    {
        struct IDs
        {
            DECLARE_ID(Play);
            DECLARE_ID(Stop);
        private:
            IDs() = delete;
        };

        explicit WavePlayer(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
        {
            // Input events using Flag system like Hazel
            AddInEvent(IDs::Play, [this](float v) { (void)v; m_PlayFlag.SetDirty(); });
            AddInEvent(IDs::Stop, [this](float v) { (void)v; m_StopFlag.SetDirty(); });

            RegisterEndpoints();
        }

        ~WavePlayer()
        {
            // Ensure any ongoing async load is moved to stale container
            CancelAsyncLoad();
            
            // Wait for all stale loads to complete before destruction
            // This is safe to do in destructor since it's not on audio thread
            for (auto& future : m_StaleLoads)
            {
                if (future.valid())
                {
                    try { future.get(); } catch (...) { /* Ignore exceptions during cleanup */ }
                }
            }
            m_StaleLoads.clear();
        }

        // Input parameters
        i64* m_InWaveAsset = nullptr;		// Asset handle for wave file
        f32* m_InStartTime = nullptr;			// Start time offset in seconds
        bool* m_InLoop = nullptr;				// Enable looping playback
        i32* m_InNumberOfLoops = nullptr;		// Number of loops (-1 = infinite)

        // Output audio channels
        f32 m_OutOutLeft{ 0.0f };
        f32 m_OutOutRight{ 0.0f };

        // Output events
        OutputEvent m_OutOnPlay{ *this };
        OutputEvent m_OutOnStop{ *this };
        OutputEvent m_OutOnFinished{ *this };
        OutputEvent m_OutOnLooped{ *this };

        void RegisterEndpoints();
        void InitializeInputs();

        void Init() final
        {
            InitializeInputs();
            
            // Initialize state
            m_IsInitialized = false;
            m_IsPlaying = false;
            m_FrameNumber = 0;
            m_StartSample = 0;
            m_LoopCount = 0;
            m_TotalFrames = 0;

            // Initialize buffer and wave source
            m_WaveSource.Clear();
            m_AudioData.Clear();
            UpdateWaveSourceIfNeeded();

            m_IsInitialized = true;
        }

        void Process() final
        {
            OLO_PROFILE_FUNCTION();

            // Check for completed async loads (non-blocking, audio thread safe)
            CheckAsyncLoadCompletion();

            // Handle events using Flag system like Hazel
            if (m_PlayFlag.CheckAndResetIfDirty())
                StartPlayback();

            if (m_StopFlag.CheckAndResetIfDirty())
                StopPlayback(false);

            if (m_IsPlaying)
            {
                // Check if we've reached the end
                if (m_FrameNumber >= m_TotalFrames)
                {
                    if (*m_InLoop)
                    {
                        ++m_LoopCount;
                        m_OutOnLooped(2.0f);
                        
                        // Check if we've completed all loops
                        if (*m_InNumberOfLoops >= 0 && m_LoopCount > *m_InNumberOfLoops)
                        {
                            StopPlayback(true);
                            OutputSilence();
                        }
                        else
                        {
                            // Loop back to start - reset play head before fetching next frame
                            m_FrameNumber = m_StartSample;
                            m_WaveSource.m_ReadPosition = m_FrameNumber;
                            ReadNextFrame();
                            m_FrameNumber++;
                            m_WaveSource.m_ReadPosition = m_FrameNumber;
                        }
                    }
                    else
                    {
                        // No looping - stop playback
                        StopPlayback(true);
                        OutputSilence();
                    }
                }
                else
                {
                    // Read next frame of audio data
                    ReadNextFrame();
                    m_FrameNumber++;
                    m_WaveSource.m_ReadPosition = m_FrameNumber;
                }
            }
            else
            {
                OutputSilence();
            }
        }

    private:
        void StartPlayback()
        {
            // Check for completed async loads first (non-blocking)
            CheckAsyncLoadCompletion();

            // Update wave source if asset changed (async)
            UpdateWaveSourceIfNeeded();

            // Check if we have a valid asset
            if (!m_WaveSource.m_WaveHandle)
            {
                DBG("WavePlayer: Invalid wave asset handle, cannot start playback");
                StopPlayback(false);
                return;
            }

            // Only start if audio data is ready
            if (!m_AudioData.IsValid())
            {
                DBG("WavePlayer: Audio data not ready yet, playback delayed");
                m_PendingPlayback.store(true, std::memory_order_relaxed); // Will start when load completes
                return;
            }

            // Set playback frame counters to start sample (respects start-time offset)
            m_FrameNumber = m_StartSample;
            m_WaveSource.m_ReadPosition = m_FrameNumber;

            // Prime the buffer unconditionally when transitioning into playback
            ForceRefillBuffer();

            m_IsPlaying = true;
            m_PendingPlayback.store(false, std::memory_order_relaxed);
            m_OutOnPlay(2.0f);
            DBG("WavePlayer: Started playing");
        }

        void StopPlayback(bool notifyOnFinish)
        {
            m_IsPlaying = false;
            m_PendingPlayback.store(false, std::memory_order_relaxed);  // Cancel any pending playback
            m_LoopCount = 0;
            m_FrameNumber = m_StartSample;
            m_WaveSource.m_ReadPosition = m_FrameNumber;

            // Check for completed async loads (in case asset changed while playing)
            CheckAsyncLoadCompletion();

            if (notifyOnFinish)
                m_OutOnFinished(2.0f);  // Natural completion
            else
                m_OutOnStop(2.0f);     // Manual stop or error

            DBG("WavePlayer: Stopped playing");
        }

        void UpdateWaveSourceIfNeeded()
        {
            u64 waveAsset = static_cast<u64>(*m_InWaveAsset);

            if (m_WaveSource.m_WaveHandle != waveAsset)
            {
                // Cancel any pending load for the old asset
                CancelAsyncLoad();

                m_WaveSource.m_WaveHandle = waveAsset;

                if (m_WaveSource.m_WaveHandle)
                {
                    // Start async loading
                    StartAsyncLoad(waveAsset);
                }
                else
                {
                    // Clear data for null asset
                    m_TotalFrames = 0;
                    m_AudioData.Clear();
                    m_IsInitialized = false;
                }

                // Reset playback position (will be updated when async load completes)
                m_StartSample = 0;
                m_FrameNumber = m_StartSample;
            }
        }

        void StartAsyncLoad(u64 waveAsset)
        {
            // Mark as loading
            m_LoadState.store(LoadState::Loading, std::memory_order_relaxed);
            
            // Start async load on background thread
            m_AsyncLoadFuture = std::async(std::launch::async, [this, waveAsset]() -> std::optional<AudioData> {
                // Integrate with OloEngine's AssetManager
                AssetHandle assetHandle = static_cast<AssetHandle>(waveAsset);
                AssetMetadata metadata = AssetManager::GetAssetMetadata(assetHandle);
                
                if (metadata.IsValid() && !metadata.FilePath.empty())
                {
                    // Load audio data using AudioLoader (on background thread)
                    AudioData audioData;
                    if (AudioLoader::LoadAudioFile(metadata.FilePath, audioData))
                    {
                        OLO_CORE_INFO("WavePlayer: Loaded audio asset - {} channels, {} Hz, {:.2f}s duration",
                            audioData.m_NumChannels, audioData.m_SampleRate, audioData.m_Duration);
                        return audioData;
                    }
                    else
                    {
                        OLO_CORE_ERROR("WavePlayer: Failed to load audio file: {}", metadata.FilePath.string());
                    }
                }
                else
                {
                    OLO_CORE_ERROR("WavePlayer: Invalid asset metadata for handle {}", assetHandle);
                }
                
                return std::nullopt; // Failed to load
            });
        }

        void CheckAsyncLoadCompletion()
        {
            // Clean up any completed stale futures first (non-blocking)
            CleanupStaleLoads();
            
            if (m_LoadState.load(std::memory_order_relaxed) == LoadState::Loading && m_AsyncLoadFuture.valid())
            {
                // Check if async load completed (non-blocking)
                auto status = m_AsyncLoadFuture.wait_for(std::chrono::seconds(0));
                if (status == std::future_status::ready)
                {
                    // Load completed - get result
                    auto result = m_AsyncLoadFuture.get();
                    if (result.has_value())
                    {
                        // Success - swap in the loaded data on audio thread
                        m_AudioData = std::move(result.value());
                        m_WaveSource.m_TotalFrames = m_AudioData.m_NumFrames;
                        
                        // Set up refill callback to read from loaded audio data
                        m_WaveSource.m_OnRefill = [this](Audio::WaveSource& source) -> bool {
                            return FillBufferFromAudioData(source);
                        };
                        
                        m_TotalFrames = m_WaveSource.m_TotalFrames;
                        m_IsInitialized = true;
                        m_LoadState.store(LoadState::Ready, std::memory_order_relaxed);

                        // Apply start time offset now that we have the data
                        if (m_InStartTime && *m_InStartTime > 0.0f)
                        {
                            f64 sampleRate = m_AudioData.m_SampleRate;
                            m_StartSample = static_cast<i64>(*m_InStartTime * sampleRate);
                            i64 maxSample = (m_TotalFrames > 0 ? m_TotalFrames - 1 : 0);
                            m_StartSample = glm::min(m_StartSample, maxSample);
                        }
                        else
                        {
                            m_StartSample = 0;
                        }

                        m_FrameNumber = m_StartSample;

                        // If playback was pending, start it now
                        if (m_PendingPlayback.load(std::memory_order_relaxed))
                        {
                            StartPlayback();
                        }
                    }
                    else
                    {
                        // Failed to load
                        m_TotalFrames = 0;
                        m_IsInitialized = false;
                        m_LoadState.store(LoadState::Failed, std::memory_order_relaxed);
                        m_PendingPlayback.store(false, std::memory_order_relaxed);
                    }
                }
            }
        }

        void CancelAsyncLoad()
        {
            if (m_AsyncLoadFuture.valid())
            {
                // Mark as cancelled
                m_LoadState.store(LoadState::Cancelled, std::memory_order_relaxed);
                
                // Move future to stale container to avoid blocking destructor
                m_StaleLoads.push_back(std::move(m_AsyncLoadFuture));
                // m_AsyncLoadFuture is now invalid and safe to reassign
            }
        }

        void CleanupStaleLoads()
        {
            // Use C++20 std::erase_if to remove completed or invalid futures
            // Avoids const_cast code smell by taking non-const reference
            std::erase_if(m_StaleLoads, 
                [](std::future<std::optional<AudioData>>& future) -> bool {
                    if (!future.valid())
                        return true; // Remove invalid futures
                    
                    // Check if completed (non-blocking)
                    auto status = future.wait_for(std::chrono::seconds(0));
                    if (status == std::future_status::ready)
                    {
                        // Future is complete, safe to remove
                        try { 
                            // Get result to properly clean up the future
                            future.get();
                        } catch (...) { 
                            // Ignore exceptions during cleanup 
                        }
                        return true;
                    }
                    return false; // Keep pending futures
                });
        }

    public:
        void ForceRefillBuffer()
        {
            if (m_WaveSource.m_WaveHandle && m_WaveSource.m_OnRefill)
            {
                m_WaveSource.m_ReadPosition = m_FrameNumber;
                [[maybe_unused]] bool refillSuccess = m_WaveSource.Refill();
            }
        }

        Audio::WaveSource& GetWaveSource() { return m_WaveSource; }
        const Audio::WaveSource& GetWaveSource() const { return m_WaveSource; }

    private:

        void ReadNextFrame()
        {
            // Iterative approach to avoid stack overflow from recursive refill attempts
            constexpr i32 maxRefillRetries = 5; // Reasonable limit to prevent infinite loops
            
            for (i32 retryCount = 0; retryCount <= maxRefillRetries; ++retryCount)
            {
                if (m_WaveSource.m_Channels.Available() >= 2) // Stereo frame
                {
                    // Read interleaved stereo data
                    m_OutOutLeft = m_WaveSource.m_Channels.Get();
                    m_OutOutRight = m_WaveSource.m_Channels.Get();
                    return; // Successfully read data
                }
                else if (m_WaveSource.m_Channels.Available() >= 1) // Mono frame
                {
                    // Mono - duplicate to both channels
                    float sample = m_WaveSource.m_Channels.Get();
                    m_OutOutLeft = sample;
                    m_OutOutRight = sample;
                    return; // Successfully read data
                }
                else
                {
                    // No data available - try to refill buffer (only if we haven't exceeded retry limit)
                    if (retryCount < maxRefillRetries && m_WaveSource.m_OnRefill && m_WaveSource.Refill())
                    {
                        // Buffer refilled, continue loop to try reading again
                        continue;
                    }
                    else
                    {
                        // No data available or max retries exceeded
                        OutputSilence();
                        return;
                    }
                }
            }
        }

        void OutputSilence()
        {
            m_OutOutLeft = 0.0f;
            m_OutOutRight = 0.0f;
        }

        bool FillBufferFromAudioData(Audio::WaveSource& source)
        {
            if (!m_AudioData.IsValid()) return false;
            
            const u32 framesToRead = 1024; // Read chunk size
            u64 startFrame = source.m_ReadPosition;
            u64 endFrame = glm::min(startFrame + framesToRead, static_cast<u64>(m_AudioData.m_NumFrames));
            
            if (startFrame >= m_AudioData.m_NumFrames) return false;
            
            // Fill the circular buffer with interleaved audio data
            for (u64 frame = startFrame; frame < endFrame; ++frame)
            {
                for (u32 channel = 0; channel < m_AudioData.m_NumChannels; ++channel)
                {
                    // Safe cast: endFrame is already bounded by m_NumFrames (u32)
                    f32 sample = m_AudioData.GetSample(static_cast<u32>(frame), channel);
                    source.m_Channels.Push(sample);
                }
            }
            
            return true;
        }

        // State variables
        bool m_IsInitialized{ false };
        bool m_IsPlaying{ false };
        i64 m_FrameNumber{ 0 };
        i64 m_StartSample{ 0 };
        i64 m_LoopCount{ 0 };
        i64 m_TotalFrames{ 0 };

        // Async loading state
        enum class LoadState
        {
            Idle,
            Loading,
            Ready,
            Failed,
            Cancelled
        };
        
        std::atomic<LoadState> m_LoadState{ LoadState::Idle };
        std::atomic<bool> m_PendingPlayback{ false }; // Start playback when async load completes
        std::future<std::optional<AudioData>> m_AsyncLoadFuture;
        
        // Container for stale futures to avoid blocking destructor on audio thread
        std::vector<std::future<std::optional<AudioData>>> m_StaleLoads;

        // Flag system for events (like Hazel)
        Flag m_PlayFlag;
        Flag m_StopFlag;

        // Wave source using OloEngine's system
        Audio::WaveSource m_WaveSource;
        
        // Audio data storage for loaded files
        AudioData m_AudioData;
    };
}

#undef DECLARE_ID
#undef DBG