#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/WaveSource.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Task/Task.h"

#include <chrono>
#include <array>
#include <type_traits>
#include <thread>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <atomic>

#define LOG_DBG_MESSAGES 0

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

#define DECLARE_ID(name)             \
    static constexpr Identifier name \
    {                                \
        #name                        \
    }

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
            AddInEvent(IDs::Play, [this](float v)
                       { (void)v; m_PlayFlag.SetDirty(); });
            AddInEvent(IDs::Stop, [this](float v)
                       { (void)v; m_StopFlag.SetDirty(); });

            RegisterEndpoints();
        }

        ~WavePlayer()
        {
            // Mark as shutting down to prevent new loads from completing
            m_ShuttingDown.store(true, std::memory_order_release);

            // Cancel any pending load
            CancelAsyncLoad();

            // Note: Task System manages task lifecycle - no explicit wait needed
            // Tasks will complete naturally but results will be ignored due to m_ShuttingDown flag
        }

        // Input parameters. The `m_<Name>` convention (no `In` prefix) is the engine-wide
        // pattern for reflection-cleaned endpoint names; RemovePrefixAndSuffix strips only
        // the `m_`, so the runtime endpoint identifier ends up bare (e.g. "WaveAsset").
        // Schema property keys in NodeSchema.cpp and connection endpoints in saved
        // .olosoundgraph files must match these cleaned names.
        i64* m_WaveAsset = nullptr;     // Asset handle for wave file
        f32* m_StartTime = nullptr;     // Start time offset in seconds
        bool* m_Loop = nullptr;         // Enable looping playback
        i32* m_NumberOfLoops = nullptr; // Number of loops (-1 = infinite)

        // Output audio channels. Endpoint identifiers become "OutLeft" / "OutRight".
        f32 m_OutLeft{ 0.0f };
        f32 m_OutRight{ 0.0f };

        // Output events. Endpoint identifiers become "OnPlay" / "OnStop" / etc., matching
        // common event-naming conventions for hooks the user wires consumers into.
        OutputEvent m_OnPlay{ *this };
        OutputEvent m_OnStop{ *this };
        OutputEvent m_OnFinished{ *this };
        OutputEvent m_OnLooped{ *this };

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
            m_NextRefillFrame = 0;

            // Initialize buffer and wave source
            m_WaveSource.Clear();
            m_AudioData.Clear();
            UpdateWaveSourceIfNeeded();

            m_IsInitialized = true;
        }

        void Process() final
        {
            // Intentionally no OLO_PROFILE_FUNCTION: called once per audio frame from
            // SoundGraph::Process. The Debug-mode InstrumentationTimer overhead is on the
            // order of a microsecond per call, which is fatal at 48 kHz. See the matching
            // comment in SoundGraph::Process for the full reasoning.

            // If parameter wiring is incomplete, stay silent rather than crash. This can
            // happen for a node added by the editor that hasn't had its inputs configured
            // and whose endpoint registration didn't establish the expected parameter map.
            if (!m_WaveAsset || !m_Loop || !m_NumberOfLoops)
            {
                OutputSilence();
                return;
            }

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
                    if (*m_Loop)
                    {
                        ++m_LoopCount;
                        m_OnLooped(2.0f);

                        // Check if we've completed all loops
                        if (*m_NumberOfLoops >= 0 && m_LoopCount > *m_NumberOfLoops)
                        {
                            StopPlayback(true);
                            OutputSilence();
                        }
                        else
                        {
                            // Loop back to start - reset play head before fetching next frame.
                            // Also rewind m_NextRefillFrame so the post-loop refills start
                            // reading from the file beginning instead of past-the-end.
                            m_FrameNumber = m_StartSample;
                            m_WaveSource.m_ReadPosition = m_FrameNumber;
                            m_NextRefillFrame = m_StartSample;
                            m_WaveSource.m_Channels.Clear();
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
                OLO_CORE_WARN("[WavePlayer] StartPlayback: no wave asset handle bound — bailing");
                StopPlayback(false);
                return;
            }

            // Only start if audio data is ready
            if (!m_AudioData.IsValid())
            {
                m_PendingPlayback.store(true, std::memory_order_relaxed); // Will start when load completes
                return;
            }

            // Set playback frame counters to start sample (respects start-time offset).
            m_FrameNumber = m_StartSample;
            m_WaveSource.m_ReadPosition = m_FrameNumber;
            m_NextRefillFrame = m_StartSample;

            // Prime the buffer unconditionally when transitioning into playback. The
            // first call always fills (buffer is empty after StopPlayback / Clear),
            // and m_NextRefillFrame advances accordingly inside ForceRefillBuffer.
            ForceRefillBuffer();

            m_IsPlaying = true;
            m_PendingPlayback.store(false, std::memory_order_relaxed);
            m_OnPlay(2.0f);
            DBG("WavePlayer: Started playing");
        }

        void StopPlayback(bool notifyOnFinish)
        {
            m_IsPlaying = false;
            m_PendingPlayback.store(false, std::memory_order_relaxed); // Cancel any pending playback
            m_LoopCount = 0;
            m_FrameNumber = m_StartSample;
            m_WaveSource.m_ReadPosition = m_FrameNumber;

            // Check for completed async loads (in case asset changed while playing)
            CheckAsyncLoadCompletion();

            if (notifyOnFinish)
                m_OnFinished(2.0f); // Natural completion
            else
                m_OnStop(2.0f); // Manual stop or error

            DBG("WavePlayer: Stopped playing");
        }

        void UpdateWaveSourceIfNeeded()
        {
            // m_WaveAsset is wired up by InitializeInputs against the node's parameter
            // table; if registration failed (parameter name mismatch, missing description,
            // etc.) it stays at its default nullptr and dereferencing it crashes. Treat
            // that case as "no asset configured" so the node just stays silent.
            if (!m_WaveAsset)
            {
                if (m_WaveSource.m_WaveHandle != 0)
                {
                    CancelAsyncLoad();
                    m_WaveSource.m_WaveHandle = 0;
                    m_TotalFrames = 0;
                    m_AudioData.Clear();
                }
                m_IsInitialized = false;
                return;
            }

            u64 waveAsset = static_cast<u64>(*m_WaveAsset);

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
            m_LoadState.store(LoadState::Loading, std::memory_order_release);
            m_LoadGeneration.fetch_add(1, std::memory_order_relaxed);
            u32 currentGeneration = m_LoadGeneration.load(std::memory_order_relaxed);

            // Start async load using Task System
            Tasks::Launch("WavePlayerAudioLoad", [this, waveAsset, currentGeneration]()
                          {
                // Check if this load is still relevant
                if (m_ShuttingDown.load(std::memory_order_acquire) ||
                    currentGeneration != m_LoadGeneration.load(std::memory_order_relaxed))
                {
                    return; // Load was cancelled or superseded
                }

                // Integrate with OloEngine's AssetManager. AssetMetadata::FilePath stores
                // the path *relative to the project root* — see
                // EditorAssetManager::GetRelativePath which calls
                // std::filesystem::relative against m_ProjectPath. So the stored value for
                // ding.wav is "Assets/Audio/ding.wav" (with the Assets/ prefix). Joining
                // that against GetAssetDirectory() doubles the Assets segment; the correct
                // base is GetProjectDirectory().
                AssetHandle assetHandle = static_cast<AssetHandle>(waveAsset);
                AssetMetadata metadata = AssetManager::GetAssetMetadata(assetHandle);

                std::optional<AudioData> result;
                if (metadata.IsValid() && !metadata.FilePath.empty())
                {
                    const std::filesystem::path absolutePath = Project::GetProjectDirectory() / metadata.FilePath;
                    // Load audio data using AudioLoader (on background thread)
                    AudioData audioData;
                    if (AudioLoader::LoadAudioFile(absolutePath, audioData))
                    {
                        OLO_CORE_INFO("WavePlayer: Loaded audio asset - {} channels, {} Hz, {:.2f}s duration",
                                     audioData.m_NumChannels, audioData.m_SampleRate, audioData.m_Duration);
                        result = std::move(audioData);
                    }
                    else
                    {
                        OLO_CORE_ERROR("WavePlayer: Failed to load audio file: {}", absolutePath.string());
                    }
                }
                else
                {
                    OLO_CORE_ERROR("WavePlayer: Invalid asset metadata for handle {}", assetHandle);
                }

                // Check again if still relevant before storing result
                if (m_ShuttingDown.load(std::memory_order_acquire) ||
                    currentGeneration != m_LoadGeneration.load(std::memory_order_relaxed))
                {
                    return; // Load was cancelled or superseded
                }

                // Store result for pickup by audio thread
                {
                    TUniqueLock<FMutex> lock(m_LoadResultMutex);
                    m_LoadResult = std::move(result);
                    m_LoadResultReady.store(true, std::memory_order_release);
                } }, Tasks::ETaskPriority::BackgroundNormal);
        }

        void CheckAsyncLoadCompletion()
        {
            // Check if a load result is ready (non-blocking, audio thread safe)
            if (!m_LoadResultReady.load(std::memory_order_acquire))
            {
                return; // No result ready
            }

            // Retrieve the result
            std::optional<AudioData> result;
            {
                TUniqueLock<FMutex> lock(m_LoadResultMutex);
                result = std::move(m_LoadResult);
                m_LoadResult.reset();
                m_LoadResultReady.store(false, std::memory_order_release);
            }

            if (result.has_value())
            {
                // Success - swap in the loaded data on audio thread
                m_AudioData = std::move(result.value());
                m_WaveSource.m_TotalFrames = m_AudioData.m_NumFrames;

                // Set up refill callback to read from loaded audio data
                m_WaveSource.m_OnRefill = [this](Audio::WaveSource& source) -> bool
                {
                    return FillBufferFromAudioData(source);
                };

                m_TotalFrames = m_WaveSource.m_TotalFrames;
                m_IsInitialized = true;
                m_LoadState.store(LoadState::Ready, std::memory_order_relaxed);

                // Apply start time offset now that we have the data
                if (m_StartTime && *m_StartTime > 0.0f)
                {
                    f64 sampleRate = m_AudioData.m_SampleRate;
                    m_StartSample = static_cast<i64>(*m_StartTime * sampleRate);
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

        void CancelAsyncLoad()
        {
            // Increment generation to invalidate any in-flight load
            m_LoadGeneration.fetch_add(1, std::memory_order_relaxed);

            // Mark as cancelled
            if (m_LoadState.load(std::memory_order_relaxed) == LoadState::Loading)
            {
                m_LoadState.store(LoadState::Cancelled, std::memory_order_relaxed);
            }

            // Clear any pending result
            {
                TUniqueLock<FMutex> lock(m_LoadResultMutex);
                m_LoadResult.reset();
                m_LoadResultReady.store(false, std::memory_order_release);
            }
        }

        // Note: CleanupStaleLoads removed - Task System manages task lifecycle

      public:
        void ForceRefillBuffer()
        {
            if (!m_WaveSource.m_WaveHandle || !m_WaveSource.m_OnRefill)
                return;

            // Per-block topup. The naive version re-pushed 1024 frames starting at the
            // reader's current frame number every single block — but only ~480 frames of
            // buffered data drains per block, so most of those pushes land on samples that
            // are still sitting in the circular buffer from the previous push. The buffer
            // accumulates duplicate samples (same frames pushed multiple times), the reader
            // consumes them in arrival order, and the perceived playback rate ends up
            // ~12× slower than real time.
            //
            // Two-part fix:
            //  1. Only push more data when the buffer is actually running low. Once it has
            //     at least one block's worth of read-ahead (we use the buffer capacity / 2
            //     as the threshold), there's no need to re-fill yet.
            //  2. When we do refill, start *past* the highest sample index we've already
            //     pushed (m_NextRefillFrame), not at the reader position. That keeps each
            //     sample from m_AudioData entering the buffer exactly once.
            const i32 currentAvail = m_WaveSource.m_Channels.Available();
            constexpr i32 kLowWatermarkSamples = 1920; // half of the 3840-sample buffer (= 960 stereo frames)
            if (currentAvail >= kLowWatermarkSamples)
                return;

            // FillBufferFromAudioData reads from m_NextRefillFrame and advances it on
            // success, so we don't need to touch source.m_ReadPosition for the source
            // index any more.
            [[maybe_unused]] bool refillSuccess = m_WaveSource.Refill();
        }

        Audio::WaveSource& GetWaveSource()
        {
            return m_WaveSource;
        }
        const Audio::WaveSource& GetWaveSource() const
        {
            return m_WaveSource;
        }

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
                    m_OutLeft = m_WaveSource.m_Channels.Get();
                    m_OutRight = m_WaveSource.m_Channels.Get();
                    return; // Successfully read data
                }
                else if (m_WaveSource.m_Channels.Available() >= 1) // Mono frame
                {
                    // Mono - duplicate to both channels
                    float sample = m_WaveSource.m_Channels.Get();
                    m_OutLeft = sample;
                    m_OutRight = sample;
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
            m_OutLeft = 0.0f;
            m_OutRight = 0.0f;
        }

        bool FillBufferFromAudioData(Audio::WaveSource& source)
        {
            if (!m_AudioData.IsValid())
                return false;

            // Use m_NextRefillFrame as the source-of-truth read pointer rather than
            // source.m_ReadPosition, so both refill paths (ForceRefillBuffer at block
            // boundaries and the lazy refill from ReadNextFrame when the buffer empties)
            // produce a single non-overlapping stream of pushes.
            const u32 framesToRead = 1024; // Read chunk size
            const u64 startFrame = static_cast<u64>(m_NextRefillFrame);
            const u64 endFrame = glm::min(startFrame + framesToRead, static_cast<u64>(m_AudioData.m_NumFrames));

            if (startFrame >= m_AudioData.m_NumFrames)
                return false;

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

            // Advance the "next frame to push" so subsequent refills pick up where we left off.
            m_NextRefillFrame = static_cast<i64>(endFrame);

            return true;
        }

        // State variables
        bool m_IsInitialized{ false };
        bool m_IsPlaying{ false };
        i64 m_FrameNumber{ 0 };
        i64 m_StartSample{ 0 };
        i64 m_LoopCount{ 0 };
        i64 m_TotalFrames{ 0 };
        // Next frame index in m_AudioData that ForceRefillBuffer will push from. Each
        // successful refill advances this so we never re-push samples that are still in
        // the circular buffer. Reset on Init / StartPlayback (and on loop wrap).
        i64 m_NextRefillFrame{ 0 };

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
        std::atomic<bool> m_ShuttingDown{ false };    // Prevents new loads during destruction
        std::atomic<u32> m_LoadGeneration{ 0 };       // Incremented to invalidate in-flight loads

        // Thread-safe result delivery from Task to audio thread
        FMutex m_LoadResultMutex;
        std::optional<AudioData> m_LoadResult;
        std::atomic<bool> m_LoadResultReady{ false };

        // Flag system for events (like Hazel)
        Flag m_PlayFlag;
        Flag m_StopFlag;

        // Wave source using OloEngine's system
        Audio::WaveSource m_WaveSource;

        // Audio data storage for loaded files
        AudioData m_AudioData;
    };
} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef DBG
