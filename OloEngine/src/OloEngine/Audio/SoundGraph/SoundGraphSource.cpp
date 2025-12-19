#include "OloEnginePCH.h"
#include "SoundGraphSource.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Project/Project.h"
#include <chrono>
#include <thread>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// DataSourceContext Implementation

    bool DataSourceContext::InitializeWaveSource(AssetHandle handle, std::unordered_map<AssetHandle, std::shared_ptr<Audio::AudioData>>& audioDataCache)
    {
        OLO_PROFILE_FUNCTION();

        if (m_WaveSources.find(handle) != m_WaveSources.end())
            return true; // Already initialized

        // Load the audio asset metadata
        auto audioAsset = AssetManager::GetAsset<AudioFile>(handle);
        if (!audioAsset)
        {
            OLO_CORE_ERROR("[SoundGraphSource] Failed to load audio asset: {0}", handle);
            return false;
        }

        Audio::WaveSource waveSource{};
        waveSource.m_WaveHandle = handle;
        waveSource.m_WaveName = ""; // audioAsset->GetPath().c_str(); // For debugging - needs proper API

        // Extract metadata from the audio asset to set up the wave source properly
        // This prevents AreAllSourcesAtEnd() from immediately reporting finished due to TotalFrames = 0
        const double duration = audioAsset->GetDuration();
        const u32 sampleRate = audioAsset->GetSamplingRate();

        // Calculate total frames from duration and sample rate with overflow protection
        // Use long double to avoid overflow during multiplication before casting to i64
        if (duration < 0.0)
        {
            // Negative duration is invalid - clamp to 0
            waveSource.m_TotalFrames = 0;
        }
        else
        {
            const long double framesLD = static_cast<long double>(duration) * static_cast<long double>(sampleRate);
            constexpr i64 maxFrames = std::numeric_limits<i64>::max();

            // Saturate to i64::max if overflow would occur
            if (framesLD >= static_cast<long double>(maxFrames))
            {
                waveSource.m_TotalFrames = maxFrames;
                OLO_CORE_WARN("[SoundGraphSource] Audio duration {} seconds at {} Hz exceeds i64::max frames, saturating to max",
                              duration, sampleRate);
            }
            else
            {
                waveSource.m_TotalFrames = static_cast<i64>(framesLD);
            }
        }

        // Preload audio data to avoid blocking file I/O in audio thread
        // This is done during initialization on the main thread
        AssetMetadata metadata = AssetManager::GetAssetMetadata(handle);
        if (metadata.IsValid())
        {
            std::filesystem::path filePath = Project::GetAssetDirectory() / metadata.FilePath;
            if (std::filesystem::exists(filePath))
            {
                auto cachedAudioData = std::make_shared<Audio::AudioData>();
                if (Audio::AudioLoader::LoadAudioFile(filePath, *cachedAudioData))
                {
                    // Store in cache for use by audio thread
                    audioDataCache[handle] = cachedAudioData;

                    // Set atomic pointer for lock-free access in audio thread callback
                    waveSource.m_CachedAudioData.store(cachedAudioData.get(), std::memory_order_release);

                    OLO_CORE_TRACE("[SoundGraphSource] Preloaded audio data for handle {}: {} frames, {} channels, {} Hz",
                                   handle, cachedAudioData->m_NumFrames, cachedAudioData->m_NumChannels, cachedAudioData->m_SampleRate);
                }
                else
                {
                    OLO_CORE_ERROR("[SoundGraphSource] Failed to preload audio file: {}", filePath.string());
                }
            }
            else
            {
                OLO_CORE_ERROR("[SoundGraphSource] Audio file does not exist: {}", filePath.string());
            }
        }

        // Insert with try_emplace to construct WaveSource in-place (atomic member prevents move/copy)
        auto [it, inserted] = m_WaveSources.try_emplace(handle);
        if (inserted)
        {
            // Manually initialize the in-place constructed WaveSource
            it->second.m_WaveHandle = waveSource.m_WaveHandle;
            it->second.m_WaveName = waveSource.m_WaveName;
            it->second.m_TotalFrames = waveSource.m_TotalFrames;
            it->second.m_StartPosition = waveSource.m_StartPosition;
            it->second.m_ReadPosition = waveSource.m_ReadPosition;
            it->second.m_CachedAudioData.store(waveSource.m_CachedAudioData.load(std::memory_order_acquire), std::memory_order_release);
        }

        return true;
    }

    void DataSourceContext::UninitializeWaveSource(AssetHandle handle)
    {
        auto it = m_WaveSources.find(handle);
        if (it != m_WaveSources.end())
        {
            it->second.Clear();
            m_WaveSources.erase(it);
        }
    }

    void DataSourceContext::UninitializeAll()
    {
        for (auto& [handle, source] : m_WaveSources)
        {
            source.Clear();
        }
        m_WaveSources.clear();
    }

    bool DataSourceContext::AreAllSourcesAtEnd() const
    {
        for (const auto& [handle, source] : m_WaveSources)
        {
            if (source.m_ReadPosition < source.m_TotalFrames)
                return false;
        }
        return true;
    }

    //==============================================================================
    /// SoundGraphSource::ThreadSafePreset Implementation

    void SoundGraphSource::ThreadSafePreset::SetPreset(const SoundGraphPatchPreset& preset)
    {
        // Create a new shared_ptr copy using deep copying
        auto newPreset = std::make_shared<SoundGraphPatchPreset>();

        // Copy preset metadata
        newPreset->SetName(preset.GetName());
        newPreset->SetDescription(preset.GetDescription());
        newPreset->SetVersion(preset.GetVersion());
        newPreset->SetAuthor(preset.GetAuthor());

        // Copy parameter descriptors
        auto descriptors = preset.GetAllParameterDescriptors();
        for (const auto& descriptor : descriptors)
        {
            newPreset->RegisterParameter(descriptor);
        }

        // Copy patches
        auto patchNames = preset.GetPatchNames();
        for (const auto& patchName : patchNames)
        {
            const auto* sourcePatch = preset.GetPatch(patchName);
            if (sourcePatch)
            {
                if (newPreset->CreatePatch(patchName, "Copied patch"))
                {
                    auto* destPatch = newPreset->GetPatch(patchName);
                    if (destPatch)
                    {
                        // Copy all parameter values from source patch
                        *destPatch = *sourcePatch; // Use assignment operator if available
                    }
                }
            }
        }

        // Atomically swap in the new preset while holding the lock
        {
            std::lock_guard<std::mutex> lock(m_PresetMutex);
            m_Preset = newPreset;
        }

        // Signal changes after publishing the new preset
        m_HasChanges.store(true, std::memory_order_release);
    }

    bool SoundGraphSource::ThreadSafePreset::GetPresetIfChanged(SoundGraphPatchPreset& outPreset)
    {
        if (m_HasChanges.exchange(false, std::memory_order_acq_rel))
        {
            // Take a local copy of the shared_ptr while holding the lock briefly
            std::shared_ptr<SoundGraphPatchPreset> localPreset;
            {
                std::lock_guard<std::mutex> lock(m_PresetMutex);
                localPreset = m_Preset;
            }

            // Now work with the stable snapshot without holding the lock
            if (localPreset)
            {
                outPreset.Clear();
                // Copy preset data to output
                outPreset.SetName(localPreset->GetName());
                outPreset.SetDescription(localPreset->GetDescription());
                outPreset.SetVersion(localPreset->GetVersion());
                outPreset.SetAuthor(localPreset->GetAuthor());

                // Copy parameter descriptors
                auto descriptors = localPreset->GetAllParameterDescriptors();
                for (const auto& descriptor : descriptors)
                {
                    outPreset.RegisterParameter(descriptor);
                }

                // Copy patches
                auto patchNames = localPreset->GetPatchNames();
                for (const auto& patchName : patchNames)
                {
                    const auto* sourcePatch = localPreset->GetPatch(patchName);
                    if (sourcePatch)
                    {
                        if (outPreset.CreatePatch(patchName, "Copied patch"))
                        {
                            auto* destPatch = outPreset.GetPatch(patchName);
                            if (destPatch)
                            {
                                *destPatch = *sourcePatch;
                            }
                        }
                    }
                }

                return true;
            }
        }
        return false;
    }

    //==============================================================================
    /// SoundGraphSource Implementation

    u64 SoundGraphSource::GetMaxTotalFrames() const
    {
        u64 maxFrames = 0;
        for (const auto& [handle, source] : m_DataSources.m_WaveSources)
        {
            // Only consider non-negative frame counts to avoid signed/unsigned comparison issues
            if (source.m_TotalFrames > 0)
            {
                const u64 totalFramesU64 = static_cast<u64>(source.m_TotalFrames);
                if (totalFramesU64 > maxFrames)
                    maxFrames = totalFramesU64;
            }
        }
        return maxFrames;
    }

    SoundGraphSource::SoundGraphSource()
    {
        // Event queues are pre-allocated in their constructors - no initialization needed
        // The lock-free queues have no dynamic allocation
    }

    SoundGraphSource::~SoundGraphSource()
    {
        Shutdown();
    }

    bool SoundGraphSource::Initialize(ma_engine* engine, u32 sampleRate, u32 maxBlockSize, u32 channelCount)
    {
        OLO_PROFILE_FUNCTION();

        if (m_IsInitialized)
        {
            OLO_CORE_WARN("[SoundGraphSource] Already initialized");
            return false;
        }

        if (!engine)
        {
            OLO_CORE_ERROR("[SoundGraphSource] Invalid audio engine");
            return false;
        }

        // Validate channel count (miniaudio supports 1-254 channels, but use practical limits)
        // Reference: MA_MAX_CHANNELS in miniaudio.h is typically 254
        if (channelCount < 1 || channelCount > 254)
        {
            OLO_CORE_ERROR("[SoundGraphSource] Invalid channel count {0}. Must be between 1 and 254", channelCount);
            return false;
        }

        m_Engine = engine;
        m_SampleRate = sampleRate;
        m_BlockSize = maxBlockSize;
        m_ChannelCount = channelCount;

        // Set up miniaudio engine node
        ma_engine_node_config nodeConfig = ma_engine_node_config_init(
            engine,
            ma_engine_node_type_group,
            MA_SOUND_FLAG_NO_SPATIALIZATION);

        // Configure channels based on parameter (default is stereo)
        nodeConfig.channelsIn = channelCount;
        nodeConfig.channelsOut = channelCount;

        ma_result result = ma_engine_node_init(&nodeConfig, nullptr, &m_EngineNode);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize engine node: {0}", (int)result);
            return false;
        }

        // Store reference to this instance in the node for callback access
        // Note: This approach needs to be integrated with a proper miniaudio data source
        // For now, we'll handle processing through external calls

        // Attach to the engine's endpoint for output
        result = ma_node_attach_output_bus(&m_EngineNode, 0, &engine->nodeGraph.endpoint, 0);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[SoundGraphSource] Failed to attach output bus: {0}", (int)result);
            ma_engine_node_uninit(&m_EngineNode, nullptr);
            return false;
        }

        m_IsInitialized = true;

        OLO_CORE_INFO("[SoundGraphSource] Initialized with sample rate: {0}, block size: {1}, channels: {2}",
                      sampleRate, maxBlockSize, channelCount);
        return true;
    }

    void SoundGraphSource::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_IsInitialized)
            return;

        SuspendProcessing(true);

        // Wait for audio thread to acknowledge suspension before tearing down state
        // This prevents race conditions during destruction
        constexpr auto timeout = std::chrono::milliseconds(100);
        auto startTime = std::chrono::steady_clock::now();

        while (!m_Suspended.load() &&
               (std::chrono::steady_clock::now() - startTime) < timeout)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // If timeout occurred, log a warning but continue with shutdown
        if (!m_Suspended.load())
        {
            OLO_CORE_WARN("[SoundGraphSource] Timeout waiting for audio thread suspension acknowledgment");
        }

        UninitializeDataSources();

        ma_engine_node_uninit(&m_EngineNode, nullptr);

        m_Engine = nullptr;
        m_Graph = nullptr;
        m_IsInitialized = false;
        m_PresetIsInitialized = false;

        OLO_CORE_INFO("[SoundGraphSource] Shutdown complete");
    }

    void SoundGraphSource::SuspendProcessing(bool shouldBeSuspended)
    {
        OLO_PROFILE_FUNCTION();

        if (shouldBeSuspended)
        {
            m_SuspendFlag.SetDirty();
        }
        else
        {
            // Resuming - reset state
            m_IsPlaying.store(false, std::memory_order_relaxed);
            m_CurrentFrame.store(0, std::memory_order_relaxed);
            m_IsFinished.store(false, std::memory_order_relaxed);

            // Clear any pending events by consuming them all
            Audio::AudioThreadEvent event;
            while (m_EventQueue.Pop(event))
                ; // Consume all pending events

            Audio::AudioThreadMessage msg;
            while (m_MessageQueue.Pop(msg))
                ; // Consume all pending messages

            // Reset the suspend flag
            m_SuspendFlag.CheckAndResetIfDirty();
            m_Suspended.store(false, std::memory_order_relaxed);
        }
    }

    bool SoundGraphSource::IsFinished() const noexcept
    {
        return m_IsFinished.load(std::memory_order_relaxed) && !m_IsPlaying.load(std::memory_order_relaxed);
    }

    bool SoundGraphSource::IsPlaying() const
    {
        return m_IsPlaying.load(std::memory_order_relaxed) && !IsSuspended();
    }

    void SoundGraphSource::Update(f64 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        (void)deltaTime;
        // Process events from the audio thread
        Audio::AudioThreadEvent event;
        while (m_EventQueue.Pop(event))
        {
            // Call the event callback with the pre-allocated value data
            if (m_OnGraphEvent)
            {
                // We're on the main thread now - safe to allocate
                // Convert ValueView to Value for the callback
                choc::value::Value value = event.m_ValueData.GetValue();
                m_OnGraphEvent(event.m_FrameIndex, event.m_EndpointID, value);
            }
        }

        // Process messages from the audio thread
        Audio::AudioThreadMessage msg;
        while (m_MessageQueue.Pop(msg))
        {
            // Call the message callback with the pre-allocated text
            if (m_OnGraphMessage)
            {
                m_OnGraphMessage(msg.m_FrameIndex, msg.m_Text);
            }
        }

        // Handle automatic suspension when finished
        if (m_IsFinished.load(std::memory_order_relaxed) && m_IsPlaying.load(std::memory_order_relaxed))
        {
            SuspendProcessing(true);
        }
    }

    //==============================================================================
    /// SoundGraph Interface

    bool SoundGraphSource::InitializeDataSources(const std::vector<AssetHandle>& dataSources)
    {
        OLO_PROFILE_FUNCTION();

        bool success = true;

        for (AssetHandle handle : dataSources)
        {
            if (!m_DataSources.InitializeWaveSource(handle, m_CachedAudioDataMap))
            {
                OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize data source: {0}", handle);
                success = false;
            }
        }

        if (success)
        {
            OLO_CORE_INFO("[SoundGraphSource] Initialized {0} data sources", dataSources.size());
        }

        return success;
    }

    void SoundGraphSource::UninitializeDataSources()
    {
        m_DataSources.UninitializeAll();
    }

    void SoundGraphSource::ReplaceGraph(const Ref<SoundGraph>& newGraph)
    {
        if (newGraph == m_Graph)
            return;

        m_Graph = newGraph;

        if (m_Graph)
        {
            // Set up wave source refill callback
            // Note: This is a simplified approach - in a full implementation you'd need
            // to properly integrate with the sound graph's wave player system

            // Initialize the sound graph with our sample rate
            m_Graph->SetSampleRate(static_cast<f32>(m_SampleRate));

            // Update parameter mappings
            UpdateParameterSet();

            OLO_CORE_INFO("[SoundGraphSource] Replaced sound graph");
        }
    }

    //==============================================================================
    /// Parameter Interface

    bool SoundGraphSource::SetParameter(std::string_view parameterName, const choc::value::Value& value)
    {
        if (!m_Graph || parameterName.empty())
            return false;

        // Hash the parameter name for faster lookup
        u32 parameterID = OloEngine::Hash::GenerateFNVHash(parameterName);
        return SetParameter(parameterID, value);
    }

    bool SoundGraphSource::SetParameter(u32 parameterID, const choc::value::Value& value)
    {
        (void)parameterID;
        (void)value;
        auto it = m_ParameterHandles.find(parameterID);
        if (it == m_ParameterHandles.end())
        {
            OLO_CORE_WARN("[SoundGraphSource] Parameter ID {0} not found", parameterID);
            return false;
        }

        // For now, we'll apply parameters immediately
        // In a full implementation, you'd want thread-safe parameter updates
        if (m_Graph)
        {
            // This would need to be adapted to OloEngine's parameter system
            // return m_Graph->SetParameterValue(it->second.m_Handle, value);

            // Placeholder - needs integration with sound graph parameter system
            OLO_CORE_TRACE("[SoundGraphSource] Setting parameter {0} to value", it->second.m_Name);
            return true;
        }

        return false;
    }

    bool SoundGraphSource::ApplyParameterPreset(const SoundGraphPatchPreset& preset)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Graph)
        {
            OLO_CORE_ERROR("[SoundGraphSource] No sound graph loaded");
            return false;
        }

        // Set the preset for thread-safe access from audio thread
        m_ThreadSafePreset.SetPreset(preset);

        OLO_CORE_TRACE("[SoundGraphSource] Applied parameter preset with {0} parameters",
                       preset.GetParameterCount());
        return true;
    }

    //==============================================================================
    /// Playback Interface

    int SoundGraphSource::GetNumDataSources() const
    {
        OLO_PROFILE_FUNCTION();

        return m_DataSources.GetSourceCount();
    }

    bool SoundGraphSource::AreAllDataSourcesAtEnd()
    {
        OLO_PROFILE_FUNCTION();

        return m_DataSources.AreAllSourcesAtEnd();
    }

    bool SoundGraphSource::SendPlayEvent()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Graph)
            return false;

        m_PlayRequestFlag.SetDirty();
        return true;
    }

    void SoundGraphSource::ResetPlayback()
    {
        OLO_PROFILE_FUNCTION();

        // Reset playback state atomics
        m_CurrentFrame.store(0, std::memory_order_relaxed);
        m_IsPlaying.store(false, std::memory_order_relaxed);
        m_IsFinished.store(false, std::memory_order_relaxed);

        // Reset all wave source read positions to start
        for (auto& [handle, waveSource] : m_DataSources.m_WaveSources)
        {
            waveSource.m_ReadPosition = waveSource.m_StartPosition;
            // Clear any buffered data to ensure clean restart
            waveSource.m_Channels.Clear();
        }
    }

    //==============================================================================
    /// Internal Methods

    void SoundGraphSource::UpdateParameterSet()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Graph)
            return;

        m_ParameterHandles.clear();

        // Get parameter endpoints from the sound graph
        // This would need to be adapted to OloEngine's sound graph parameter system
        // auto parameters = m_Graph->GetParameterEndpoints();

        // For now, create placeholder parameter mapping
        // In a real implementation, you'd iterate through the sound graph's exposed parameters

        OLO_CORE_TRACE("[SoundGraphSource] Updated parameter set with {0} parameters",
                       m_ParameterHandles.size());
    }

    bool SoundGraphSource::ApplyParameterPresetInternal()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Graph)
            return false;

        // For now, we'll skip the preset application since it needs proper integration
        // In a real implementation, you'd:
        // 1. Get the preset from the thread-safe container
        // 2. Apply each parameter to the sound graph
        // 3. Track which parameters were successfully set
        // TODO(implement preset application)

        // Placeholder - just mark as initialized
        return true;
    }

    void SoundGraphSource::UpdateChangedParameters()
    {
        // TODO(implement parameter change detection)
        // Handle any parameter changes that occurred since last audio block
        // This would integrate with OloEngine's parameter change detection system
    }

    void SoundGraphSource::SilenceOutputBuffers(float** ppFramesOut, u32 frameCount)
    {
        // Silence each channel independently for planar buffers
        for (u32 channel = 0; channel < m_ChannelCount; ++channel)
        {
            if (ppFramesOut[channel])
            {
                ma_silence_pcm_frames(ppFramesOut[channel], frameCount, ma_format_f32, 1);
            }
        }
    }

    //==============================================================================
    /// Audio Processing

    void SoundGraphSource::ProcessSamples(float** ppFramesOut, u32 frameCount)
    {
        OLO_PROFILE_FUNCTION();

        // Handle suspension
        if (m_SuspendFlag.CheckAndResetIfDirty())
        {
            m_Suspended.store(true, std::memory_order_release);
            m_IsPlaying.store(false, std::memory_order_relaxed);
        }

        if (!m_Graph || IsSuspended())
        {
            SilenceOutputBuffers(ppFramesOut, frameCount);
            return;
        }

        // Apply parameter presets if needed
        if (!m_PresetIsInitialized)
        {
            if (ApplyParameterPresetInternal())
            {
                m_PresetIsInitialized = true;
            }
            else
            {
                // If preset application failed, output silence
                SilenceOutputBuffers(ppFramesOut, frameCount);
                return;
            }
        }

        // Handle play requests
        if (m_PlayRequestFlag.CheckAndResetIfDirty())
        {
            // Send play event to sound graph
            if (m_Graph->SendInputEvent(SoundGraph::IDs::Play, choc::value::createFloat32(1.0f)))
            {
                m_CurrentFrame.store(0);
                m_IsPlaying.store(true);
                m_IsFinished.store(false, std::memory_order_relaxed);
            }
        }

        // Process the sound graph
        if (m_PresetIsInitialized && m_Graph->IsPlayable())
        {
            // Begin processing block (refill wave player buffers)
            m_Graph->BeginProcessBlock();

            // Process each frame
            for (u32 frame = 0; frame < frameCount; ++frame)
            {
                // Process the graph for this frame
                m_Graph->Process();

                // Copy output channels to the output buffer
                u32 outputChannels = std::min(m_ChannelCount, static_cast<u32>(m_Graph->m_OutChannels.size()));
                for (u32 channel = 0; channel < outputChannels; ++channel)
                {
                    ppFramesOut[channel][frame] = m_Graph->m_OutChannels[channel];
                }

                // Handle mono to stereo conversion if needed
                if (outputChannels == 1 && m_ChannelCount > 1)
                {
                    for (u32 channel = 1; channel < m_ChannelCount; ++channel)
                    {
                        if (ppFramesOut[channel])
                        {
                            ppFramesOut[channel][frame] = ppFramesOut[0][frame];
                        }
                    }
                }
            }

            // Handle outgoing events and messages
            m_Graph->HandleOutgoingEvents(this, HandleGraphEvent, HandleGraphMessage);

            // Update frame counter
            m_CurrentFrame.fetch_add(frameCount, std::memory_order_relaxed);

            // Check if playback should finish
            if (AreAllDataSourcesAtEnd())
            {
                m_IsFinished.store(true, std::memory_order_relaxed);
            }
        }
        else
        {
            SilenceOutputBuffers(ppFramesOut, frameCount);
        }
    }

    //==============================================================================
    /// Static Callbacks

    void SoundGraphSource::HandleGraphEvent(void* context, u64 frameIndex, Identifier endpointID, const choc::value::ValueView& eventData)
    {
        auto* source = static_cast<SoundGraphSource*>(context);
        if (!source)
            return;

        // Queue the event for processing on the main thread using lock-free pre-allocated storage
        // This is real-time safe - no allocations, no locks, no blocking
        Audio::AudioThreadEvent event;
        event.m_FrameIndex = frameIndex;
        event.m_EndpointID = static_cast<u32>(endpointID);

        // Copy the event data into pre-allocated inline storage
        // This avoids heap allocation while remaining thread-safe
        if (!event.m_ValueData.CopyFrom(eventData))
        {
            // Value was too large for inline storage - this is rare for audio events
            // Log a warning but don't allocate (keep it real-time safe)
            // The event will be dropped, which is better than causing audio glitches
            return;
        }

        // Push to the lock-free queue (wait-free operation)
        if (!source->m_EventQueue.Push(event))
        {
            // Queue is full - event dropped
            // This is preferable to blocking or allocating in the audio thread
        }
    }

    void SoundGraphSource::HandleGraphMessage(void* context, u64 frameIndex, const char* message)
    {
        auto* source = static_cast<SoundGraphSource*>(context);
        if (!source || !message)
            return;

        // Queue the message for processing on the main thread using lock-free pre-allocated storage
        // This is real-time safe - no allocations, no locks, no blocking
        Audio::AudioThreadMessage msg;
        msg.m_FrameIndex = frameIndex;
        msg.SetText(message); // Copies into pre-allocated buffer

        // Push to the lock-free queue (wait-free operation)
        if (!source->m_MessageQueue.Push(msg))
        {
            // Queue is full - message dropped
            // This is preferable to blocking or allocating in the audio thread
        }
    }

    bool SoundGraphSource::RefillWaveSourceCallback(Audio::WaveSource& waveSource, void* userData)
    {
        OLO_PROFILE_FUNCTION();

        auto* source = static_cast<SoundGraphSource*>(userData);
        if (!source)
            return false;

        if (waveSource.m_WaveHandle == 0)
            return false;

        // Load cached audio data pointer atomically (lock-free, realtime-safe)
        // This pointer was set during initialization on the main thread
        const Audio::AudioData* audioData = waveSource.m_CachedAudioData.load(std::memory_order_acquire);

        // Handle missing or invalid cached data gracefully
        if (!audioData || !audioData->IsValid())
        {
            // Audio data not preloaded or corrupted - this is a critical error but don't block
            // Log once per wave source to avoid spam using thread-safe atomic flag
            bool expectedFalse = false;
            if (waveSource.m_MissingDataLogged.compare_exchange_strong(expectedFalse, true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR("[SoundGraphSource] No preloaded audio data for handle: {} - Audio will underrun",
                               waveSource.m_WaveHandle);
            }

            // Return false to signal underflow - gracefully handle by not pushing data
            // This prevents audio glitches from blocking operations
            return false;
        }

        // Calculate how many frames we can refill
        const i64 remainingFrames = waveSource.m_TotalFrames - waveSource.m_ReadPosition;
        if (remainingFrames <= 0)
            return false; // Already at end of audio

        // Determine how many frames to push into the circular buffer
        // We'll push up to the buffer capacity or what's remaining, whichever is smaller
        constexpr sizet bufferFrameCapacity = 1920; // MonoCircularBuffer<f32, 1920 * 2> / 2 channels
        const i64 framesToPush = std::min(static_cast<i64>(bufferFrameCapacity), remainingFrames);

        // Validate we don't exceed the audio data bounds
        if (waveSource.m_ReadPosition + framesToPush > static_cast<i64>(audioData->m_NumFrames))
        {
            // Read position out of bounds - this should not happen if TotalFrames was set correctly
            // Handle gracefully by clamping instead of erroring
            const i64 validFrames = static_cast<i64>(audioData->m_NumFrames) - waveSource.m_ReadPosition;
            if (validFrames <= 0)
                return false; // No valid frames to read

            // Clamp to valid range
            const i64 clampedFramesToPush = std::min(framesToPush, validFrames);

            // Copy samples from cached audio data into the circular buffer
            // Audio data is interleaved (L,R,L,R...), same as the circular buffer expects
            const i64 startSampleIndex = waveSource.m_ReadPosition * audioData->m_NumChannels;
            const i64 numSamplesToPush = clampedFramesToPush * audioData->m_NumChannels;

            // Push samples into the circular buffer (lock-free operation)
            waveSource.m_Channels.PushMultiple(&audioData->m_Samples[startSampleIndex], static_cast<int>(numSamplesToPush));

            // Update read position
            waveSource.m_ReadPosition += clampedFramesToPush;

            return true;
        }

        // Copy samples from cached audio data into the circular buffer
        // Audio data is interleaved (L,R,L,R...), same as the circular buffer expects
        const i64 startSampleIndex = waveSource.m_ReadPosition * audioData->m_NumChannels;
        const i64 numSamplesToPush = framesToPush * audioData->m_NumChannels;

        // Push samples into the circular buffer (lock-free operation)
        // No blocking, no allocations, no file I/O - fully realtime-safe
        waveSource.m_Channels.PushMultiple(&audioData->m_Samples[startSampleIndex], static_cast<int>(numSamplesToPush));

        // Update read position
        waveSource.m_ReadPosition += framesToPush;

        return true;
    }

} // namespace OloEngine::Audio::SoundGraph
