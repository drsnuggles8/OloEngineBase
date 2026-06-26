#pragma once
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/WaveSource.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Task/Task.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <type_traits>
#include <vector>

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

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
            // Phase 4: sample-accurate Play/Stop. The handlers record the trigger's
            // frame offset within the block (docs/soundgraph-metasounds-refactor.md);
            // Process() applies Start/StopPlayback at that exact frame. A
            // block-boundary event (no offset) fires at frame 0 — the old behavior.
            AddInEvent(IDs::Play, [this](float v, i32 sampleOffset)
                       { (void)v; m_PlayTrigger.Fire(sampleOffset); });
            AddInEvent(IDs::Stop, [this](float v, i32 sampleOffset)
                       { (void)v; m_StopTrigger.Fire(sampleOffset); });

            RegisterEndpoints();
        }

        ~WavePlayer()
        {
            // Mark as shutting down so the launched lambdas early-return at their
            // next gate check. CancelAsyncLoad bumps the generation counter, which
            // achieves the same thing for any task that hasn't reached its first
            // gate yet.
            m_ShuttingDown.store(true, std::memory_order_release);
            CancelAsyncLoad();

            // Wait for any in-flight load lambda to finish before our members
            // (m_LoadResultMutex, m_LoadResult, the atomics it reads) are
            // destroyed. The gate checks above don't close the window between
            // gate-check and member access; only joining the task does.
            //
            // We block unconditionally rather than time-bounding the wait:
            // AudioLoader::LoadAudioFile is the only slow step and it's bounded
            // by disk I/O. A timeout that fires would re-open the UAF window
            // this whole hook exists to close.
            std::vector<Tasks::TTask<void>> tasksToJoin;
            {
                TUniqueLock<FMutex> lock(m_LoadTasksMutex);
                tasksToJoin = std::move(m_LoadTasks);
            }
            for (const auto& task : tasksToJoin)
            {
                task.Wait();
            }
        }

        // Input parameters. The `m_<Name>` convention (no `In` prefix) is the engine-wide
        // pattern for reflection-cleaned endpoint names; RemovePrefixAndSuffix strips only
        // the `m_`, so the runtime endpoint identifier ends up bare (e.g. "WaveAsset").
        // Schema property keys in NodeSchema.cpp and connection endpoints in saved
        // .olosoundgraph files must match these cleaned names.
        Int64Ref m_WaveAsset;   // Asset handle for wave file
        FloatRef m_StartTime;   // Start time offset in seconds
        BoolRef m_Loop;         // Enable looping playback
        IntRef m_NumberOfLoops; // Number of loops (-1 = infinite)

        // Output audio channels. Endpoint identifiers become "OutLeft" / "OutRight".
        AudioBuffer m_OutLeft;
        AudioBuffer m_OutRight;

        // Output events. Endpoint identifiers become "OnPlay" / "OnStop" / etc., matching
        // common event-naming conventions for hooks the user wires consumers into.
        OutputEvent m_OnPlay{ *this };
        OutputEvent m_OnStop{ *this };
        OutputEvent m_OnFinished{ *this };
        OutputEvent m_OnLooped{ *this };

        void RegisterEndpoints();

        void Init() final
        {
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

        void Process(u32 numFrames) final
        {
            // Phase 2: called once per block. The per-block setup below (async-load
            // polling, event-flag checks) amortises across numFrames — this is the
            // amortisation Phase 1A put in place and Phase 2's typed wiring unlocked.
            OLO_PROFILE_FUNCTION();

            CheckAsyncLoadCompletion();

            // Phase 4: Play/Stop carry a frame offset within the block. Consume the
            // pending offsets and apply the state change at the exact frame inside
            // the per-sample loop below, instead of quantising to the block boundary.
            // kNotFired (-1) never matches a frame index, so an un-fired trigger is a
            // no-op. An offset at/after numFrames is clamped to the last frame so it
            // still takes effect this block.
            i32 playOffset = m_PlayTrigger.Consume();
            i32 stopOffset = m_StopTrigger.Consume();
            if (numFrames > 0)
            {
                const i32 lastFrame = static_cast<i32>(numFrames) - 1;
                if (playOffset > lastFrame)
                    playOffset = lastFrame;
                if (stopOffset > lastFrame)
                    stopOffset = lastFrame;
            }

            f32* outLeft = m_OutLeft.Data();
            f32* outRight = m_OutRight.Data();

            // Per-sample loop writing straight into the block output buffers.
            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                const i32 frameIdx = static_cast<i32>(frame);
                // Sample-accurate Play/Stop: apply at the trigger's frame. Play
                // before Stop so a same-frame play+stop ends stopped (matches the
                // old block-boundary order where Play was checked first).
                if (playOffset == frameIdx)
                    StartPlayback(frameIdx);
                if (stopOffset == frameIdx)
                    StopPlayback(false, frameIdx);

                if (!m_IsPlaying)
                {
                    outLeft[frame] = 0.0f;
                    outRight[frame] = 0.0f;
                    continue;
                }

                if (m_FrameNumber >= m_TotalFrames)
                {
                    if (m_Loop.Get())
                    {
                        ++m_LoopCount;
                        m_OnLooped(2.0f, frameIdx);

                        if (m_NumberOfLoops.Get() >= 0 && m_LoopCount > m_NumberOfLoops.Get())
                        {
                            StopPlayback(true, frameIdx);
                            outLeft[frame] = 0.0f;
                            outRight[frame] = 0.0f;
                        }
                        else
                        {
                            // Loop back to start.
                            m_FrameNumber = m_StartSample;
                            m_WaveSource.m_ReadPosition = m_FrameNumber;
                            m_NextRefillFrame = m_StartSample;
                            m_WaveSource.m_Channels.Clear();
                            ReadNextFrame(outLeft[frame], outRight[frame]);
                            ++m_FrameNumber;
                            m_WaveSource.m_ReadPosition = m_FrameNumber;
                        }
                    }
                    else
                    {
                        StopPlayback(true, frameIdx);
                        outLeft[frame] = 0.0f;
                        outRight[frame] = 0.0f;
                    }
                }
                else
                {
                    ReadNextFrame(outLeft[frame], outRight[frame]);
                    ++m_FrameNumber;
                    m_WaveSource.m_ReadPosition = m_FrameNumber;
                }
            }
        }

      private:
        // triggerOffset (Phase 4) is the frame within the current block at which
        // the Play fired; it is forwarded to the OnPlay output event so chained
        // trigger consumers stay sample-accurate. Defaults to 0 (block boundary)
        // for the pending-playback path in CheckAsyncLoadCompletion.
        void StartPlayback(i32 triggerOffset = 0)
        {
            // Check for completed async loads first (non-blocking)
            CheckAsyncLoadCompletion();

            // Update wave source if asset changed (async)
            UpdateWaveSourceIfNeeded();

            // Check if we have a valid asset
            if (!m_WaveSource.m_WaveHandle)
            {
                OLO_CORE_WARN("[WavePlayer] StartPlayback: no wave asset handle bound — bailing");
                StopPlayback(false, triggerOffset);
                return;
            }

            // Only start if audio data is ready
            if (!m_AudioData.IsValid())
            {
                m_PendingPlayback.store(true, std::memory_order_relaxed); // Will start when load completes
                return;
            }

            // Recompute m_StartSample from the current StartTime each Play so runtime
            // changes to the StartTime input plug are respected. m_StartSample is cached
            // at load time (CheckAsyncLoadCompletion) but the plug can be written
            // afterwards via parameter automation or the editor's property panel; without
            // this each Play would replay from whatever StartTime was at load.
            UpdateStartSampleFromStartTime();

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
            m_OnPlay(2.0f, triggerOffset);
            DBG("WavePlayer: Started playing");
        }

        // triggerOffset (Phase 4): the frame at which the Stop / natural-finish
        // occurred, forwarded to OnStop / OnFinished. Defaults to 0 (block boundary).
        void StopPlayback(bool notifyOnFinish, i32 triggerOffset = 0)
        {
            m_IsPlaying = false;
            m_PendingPlayback.store(false, std::memory_order_relaxed); // Cancel any pending playback
            m_LoopCount = 0;
            m_FrameNumber = m_StartSample;
            m_WaveSource.m_ReadPosition = m_FrameNumber;

            // Drain the circular buffer and reset the source refill cursor. Otherwise
            // ForceRefillBuffer on the next Play would see leftover samples sitting at
            // or above the low-watermark and short-circuit before topping up — replay
            // would emit the tail of the previous run from the stale buffer instead
            // of priming fresh from m_StartSample.
            m_WaveSource.m_Channels.Clear();
            m_NextRefillFrame = m_StartSample;

            // Check for completed async loads (in case asset changed while playing)
            CheckAsyncLoadCompletion();

            if (notifyOnFinish)
                m_OnFinished(2.0f, triggerOffset); // Natural completion
            else
                m_OnStop(2.0f, triggerOffset); // Manual stop or error

            DBG("WavePlayer: Stopped playing");
        }

        void UpdateWaveSourceIfNeeded()
        {
            // m_WaveAsset reads the wired producer's handle when connected, or the
            // asset-authored default plug value otherwise (0 = no asset configured;
            // the node stays silent in that case via the m_WaveHandle guards below).
            u64 waveAsset = static_cast<u64>(m_WaveAsset.Get());

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
            // Fast-path early-out for the common case (graph hot-reload after
            // shutdown began). The mutex-protected re-check below closes the
            // window between this load and Tasks::Launch.
            if (m_ShuttingDown.load(std::memory_order_acquire))
            {
                return;
            }

            // Mark as loading
            m_LoadState.store(LoadState::Loading, std::memory_order_release);
            m_LoadGeneration.fetch_add(1, std::memory_order_relaxed);
            u32 currentGeneration = m_LoadGeneration.load(std::memory_order_relaxed);

            // Hold m_LoadTasksMutex across Tasks::Launch + push so that any
            // concurrent ~WavePlayer either (a) hasn't drained m_LoadTasks yet
            // — it will block on the mutex until we push, then see and join
            // this task; or (b) has already set m_ShuttingDown and we bail out
            // before launching, so the destructor can't miss a task that
            // would otherwise touch dead this.
            TUniqueLock<FMutex> lock(m_LoadTasksMutex);
            if (m_ShuttingDown.load(std::memory_order_acquire))
            {
                return;
            }

            // Start async load using Task System. We keep the returned handle so
            // ~WavePlayer can join the lambda; otherwise the lambda's post-gate
            // member accesses can land after our atomics/mutex are destroyed.
            Tasks::TTask<void> loadTask = Tasks::Launch("WavePlayerAudioLoad", [this, waveAsset, currentGeneration]()
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

            // Park the handle so the destructor can join. Opportunistically
            // prune completed handles so the vector doesn't grow unbounded
            // across repeated SetWaveAsset calls. Mutex is already held from
            // the launch-guard above.
            m_LoadTasks.erase(std::remove_if(m_LoadTasks.begin(), m_LoadTasks.end(),
                                             [](const Tasks::TTask<void>& t)
                                             { return t.IsCompleted(); }),
                              m_LoadTasks.end());
            m_LoadTasks.push_back(std::move(loadTask));
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
                UpdateStartSampleFromStartTime();

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
            constexpr i32 kLowWatermarkSamples = 1920; // half of the 3840-sample buffer (= 960 stereo frames)
            if (const i32 currentAvail = m_WaveSource.m_Channels.Available(); currentAvail >= kLowWatermarkSamples)
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

        /// Heap bytes currently held by this node's decoded audio samples
        /// (m_AudioData.m_Samples). Zero until an asset finishes loading. Reports
        /// capacity() — the real allocation still resident in RAM — so a node whose
        /// clip was Clear()'d (which empties but does not shrink the vector) is still
        /// counted until its buffer is actually released. WavePlayer is the only node
        /// type that owns a large per-node buffer today; it surfaces it through the
        /// NodeProcessor::GetHeapBytes() hook the cache accounting consumes.
        [[nodiscard("size in bytes must be used")]] sizet GetAudioDataSizeBytes() const
        {
            return m_AudioData.m_Samples.capacity() * sizeof(f32);
        }

        [[nodiscard("heap byte count must be used")]] sizet GetHeapBytes() const override
        {
            return GetAudioDataSizeBytes();
        }

      private:
        /// Resolve the StartTime input into a clamped m_StartSample. Shared by
        /// StartPlayback (per-Play recompute) and CheckAsyncLoadCompletion (initial
        /// apply once the data is loaded). The input is user/graph-controlled, so a
        /// non-finite or huge value must not reach the f32->i64 cast (UB) — compute
        /// in f64 and clamp into [0, maxSample] first.
        void UpdateStartSampleFromStartTime()
        {
            const f32 startTime = m_StartTime.Get();
            if (!std::isfinite(startTime) || startTime <= 0.0f || m_AudioData.m_SampleRate == 0)
            {
                m_StartSample = 0;
                return;
            }

            const i64 maxSample = (m_TotalFrames > 0 ? m_TotalFrames - 1 : 0);
            const f64 startFrame = static_cast<f64>(startTime) * static_cast<f64>(m_AudioData.m_SampleRate);
            m_StartSample = (startFrame >= static_cast<f64>(maxSample)) ? maxSample : static_cast<i64>(startFrame);
        }

        void ReadNextFrame(f32& outLeft, f32& outRight)
        {
            // Iterative approach to avoid stack overflow from recursive refill attempts
            constexpr i32 maxRefillRetries = 5; // Reasonable limit to prevent infinite loops

            // Pick the read shape from the SOURCE's channel count, not from how many
            // samples happen to be queued: with the old Available()>=2 heuristic a
            // mono stream was consumed two source frames per output frame (double
            // speed) whenever the buffer held more than one sample.
            const bool stereoSource = !m_AudioData.IsValid() || m_AudioData.m_NumChannels >= 2;
            const i32 samplesPerFrame = stereoSource ? 2 : 1;

            for (i32 retryCount = 0; retryCount <= maxRefillRetries; ++retryCount)
            {
                if (m_WaveSource.m_Channels.Available() >= samplesPerFrame)
                {
                    if (stereoSource)
                    {
                        // Read interleaved stereo data
                        outLeft = m_WaveSource.m_Channels.Get();
                        outRight = m_WaveSource.m_Channels.Get();
                    }
                    else
                    {
                        // Mono - duplicate to both channels
                        const float sample = m_WaveSource.m_Channels.Get();
                        outLeft = sample;
                        outRight = sample;
                    }
                    return; // Successfully read data
                }

                // No data available - try to refill buffer (only if we haven't exceeded retry limit)
                if (retryCount < maxRefillRetries && m_WaveSource.m_OnRefill && m_WaveSource.Refill())
                {
                    // Buffer refilled, continue loop to try reading again
                    continue;
                }

                // No data available or max retries exceeded
                outLeft = 0.0f;
                outRight = 0.0f;
                return;
            }
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

        // Outstanding load tasks. The destructor joins these so the lambda
        // body always observes a live object — m_ShuttingDown and
        // m_LoadGeneration gate the lambda but don't close the window between
        // gate-check and member access. Access serialised on m_LoadTasksMutex
        // because StartAsyncLoad can run on the audio thread (via
        // UpdateWaveSourceIfNeeded → StartPlayback → Process) while another
        // caller is in destruction; contention is rare (just push + prune).
        FMutex m_LoadTasksMutex;
        std::vector<Tasks::TTask<void>> m_LoadTasks;

        // Phase 4: sample-accurate Play/Stop triggers. The InputEvent handlers
        // Fire() these with the event's frame offset; Process() Consume()s them and
        // splits its per-sample loop at that frame. (Replaces the old block-boundary
        // Flag dirty-bits.)
        TriggerRef m_PlayTrigger;
        TriggerRef m_StopTrigger;

        // Wave source using OloEngine's system
        Audio::WaveSource m_WaveSource;

        // Audio data storage for loaded files
        AudioData m_AudioData;
    };
} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef DBG
