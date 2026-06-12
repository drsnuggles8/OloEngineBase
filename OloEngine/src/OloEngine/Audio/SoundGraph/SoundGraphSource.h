#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPatchPreset.h"
#include "OloEngine/Audio/LockFreeEventQueue.h"
#include <choc/containers/choc_Value.h>
#include "WaveSource.h"

#include <miniaudio.h>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include "OloEngine/Threading/Mutex.h"

namespace OloEngine::Audio::SoundGraph
{
    // Use Flag utilities from OloEngine namespace
    using OloEngine::AtomicFlag;

    class SoundGraphSource; // Forward decl for the custom miniaudio node.

    // Custom miniaudio node that bridges miniaudio's pull-based audio thread into
    // SoundGraphSource::ProcessSamples. `m_Base` MUST be the first member — miniaudio
    // casts ma_node* to/from this struct via reinterpret_cast. The owner back-pointer
    // is used by the static onProcess callback to dispatch into the right SoundGraphSource
    // instance.
    struct SoundGraphMiniaudioNode
    {
        ma_node_base m_Base;
        SoundGraphSource* m_Owner = nullptr;
    };

    //==============================================================================
    /// DataSourceContext - Manages wave asset readers for sound graph
    struct DataSourceContext
    {
        // Map of asset handle to audio buffer/reader data
        std::unordered_map<AssetHandle, Audio::WaveSource> m_WaveSources;

        bool InitializeWaveSource(AssetHandle handle, std::unordered_map<AssetHandle, std::shared_ptr<Audio::AudioData>>& audioDataCache);
        void UninitializeWaveSource(AssetHandle handle);
        void UninitializeAll();

        bool AreAllSourcesAtEnd() const;
        i32 GetSourceCount() const
        {
            return static_cast<i32>(m_WaveSources.size());
        }
    };

    //==============================================================================
    /// Data source interface between SoundGraphSound and SoundGraph instance.
    /// This is the critical audio callback that processes sound graphs in real-time.
    class SoundGraphSource
    {
      public:
        explicit SoundGraphSource();
        ~SoundGraphSource();

        //==============================================================================
        /// miniaudio node integration

        /** Get output node for routing through miniaudio engine */
        const ma_node_base* GetEngineNode() const
        {
            return &m_Node.m_Base;
        }
        ma_node_base* GetEngineNode()
        {
            return &m_Node.m_Base;
        }

        //==============================================================================
        /// Initialization and lifecycle

        bool Initialize(ma_engine* engine, u32 sampleRate, u32 maxBlockSize, u32 channelCount = 2);
        void Shutdown();

        void SuspendProcessing(bool shouldBeSuspended);
        bool IsSuspended() const
        {
            return m_Suspended.load(std::memory_order_relaxed);
        }
        bool IsFinished() const noexcept;
        bool IsPlaying() const;

        //==============================================================================
        /// Main thread update (processes events from audio thread)
        void Update(f64 deltaTime);

        //==============================================================================
        /// SoundGraph Interface

        bool InitializeDataSources(const std::vector<AssetHandle>& dataSources);
        void UninitializeDataSources();

        /** Replace current graph with new one. Called when a new graph has been compiled. */
        void ReplaceGraph(const Ref<SoundGraph>& newGraph);
        Ref<SoundGraph> GetGraph() const
        {
            return m_Graph;
        }

        /** Originating SoundGraphAsset handle this source was instantiated from. Set by the
            owner (e.g. Scene during InitAudioRuntime) so the asset reload dispatcher can
            identify which live sources to ReplaceGraph() when a graph asset changes on disk. */
        void SetSourceAssetHandle(AssetHandle handle)
        {
            m_SourceAssetHandle = handle;
        }
        AssetHandle GetSourceAssetHandle() const
        {
            return m_SourceAssetHandle;
        }

        //==============================================================================
        /// Parameter Interface

        /** Set graph parameter value by name (slower - hashes name) */
        bool SetParameter(std::string_view parameterName, const choc::value::Value& value);

        /** Set graph parameter value by ID (faster - pre-hashed) */
        bool SetParameter(u32 parameterID, const choc::value::Value& value);

        /** Apply parameter preset to the sound graph */
        bool ApplyParameterPreset(const SoundGraphPatchPreset& preset);

        //==============================================================================
        /// Playback Interface

        i32 GetNumDataSources() const;
        bool AreAllDataSourcesAtEnd() const;
        bool IsAnyDataSourceReading()
        {
            return !AreAllDataSourcesAtEnd();
        }

        bool SendPlayEvent();
        void ResetPlayback();
        u64 GetCurrentFrame() const
        {
            return m_CurrentFrame.load(std::memory_order_relaxed);
        }

        /** Get the maximum total frames from all data sources (longest audio duration) */
        u64 GetMaxTotalFrames() const;

        //==============================================================================
        /// Configuration

        /** Get the sample rate used by this source */
        u32 GetSampleRate() const
        {
            return m_SampleRate;
        }

        /** Get the channel count used by this source */
        u32 GetChannelCount() const
        {
            return m_ChannelCount;
        }

        //==============================================================================
        /// Event Callbacks (set by SoundGraphPlayer or other managers)

        using OnGraphMessageCallback = std::function<void(u64 frameIndex, const char* message)>;
        using OnGraphEventCallback = std::function<void(u64 frameIndex, u32 endpointID, const choc::value::Value& eventData)>;

        void SetMessageCallback(OnGraphMessageCallback callback)
        {
            m_OnGraphMessage = std::move(callback);
        }
        void SetEventCallback(OnGraphEventCallback callback)
        {
            m_OnGraphEvent = std::move(callback);
        }

        /** Process audio samples - called from the custom miniaudio node's onProcess
            callback on the audio thread. Public so the static vtable bridge in the
            translation unit can dispatch into it without friending the anonymous
            namespace. Thread-safe via the suspend/resume protocol; do not call directly
            from gameplay code. */
        void ProcessSamples(float** ppFramesOut, u32 frameCount);

      private:
        //==============================================================================
        /// Internal methods

        /** Called after SoundGraph has been reset to collect parameter handles */
        void UpdateParameterSet();

        /** Called from audio thread to apply preset changes */
        bool ApplyParameterPresetInternal() const;

        /** Called from audio thread to send updated parameters */
        void UpdateChangedParameters() const;

        /** SoundGraph event handlers (called from audio thread) */
        static void HandleGraphEvent(void* context, u64 frameIndex, Identifier endpointID, const choc::value::ValueView& eventData);
        static void HandleGraphMessage(void* context, u64 frameIndex, const char* message);

        /** Wave source refill callback for nodes */
        static bool RefillWaveSourceCallback(Audio::WaveSource& waveSource, void* userData);

      private:
        //============================================
        /// Helper methods

        /** Helper to silence output buffers (planar stereo) */
        void SilenceOutputBuffers(float** ppFramesOut, u32 frameCount) const;
        //============================================
        /// Audio engine and processing
        ma_engine* m_Engine = nullptr;
        SoundGraphMiniaudioNode m_Node{};
        bool m_IsInitialized = false;

        std::atomic<bool> m_Suspended{ false };
        AtomicFlag m_SuspendFlag;
        u32 m_SampleRate = 0;
        u32 m_BlockSize = 0;
        u32 m_ChannelCount = 2;

        //============================================
        /// Playback state
        std::atomic<bool> m_IsPlaying{ false };
        std::atomic<u64> m_CurrentFrame{ 0 };
        std::atomic<bool> m_IsFinished{ false };

        //============================================
        /// Sound graph and data sources
        Ref<SoundGraph> m_Graph = nullptr;
        AssetHandle m_SourceAssetHandle = 0;
        DataSourceContext m_DataSources;

        // Cached audio data for each wave source (keyed by AssetHandle)
        // Preloaded during initialization to avoid blocking file I/O in audio thread
        // Uses shared_ptr for automatic memory management and safe access
        std::unordered_map<AssetHandle, std::shared_ptr<Audio::AudioData>> m_CachedAudioDataMap;

        //============================================
        /// Parameter management
        struct ParameterInfo
        {
            u32 m_Handle;
            std::string m_Name; // For debugging

            ParameterInfo(u32 handle, std::string_view name = "")
                : m_Handle(handle), m_Name(name) {}
        };
        std::unordered_map<u32, ParameterInfo> m_ParameterHandles;

        //============================================
        /// Thread communication

        // Thread-safe parameter preset for communication between main and audio threads
        struct ThreadSafePreset
        {
            std::atomic<bool> m_HasChanges{ false };
            mutable FMutex m_PresetMutex;
            std::shared_ptr<SoundGraphPatchPreset> m_Preset;

            // Only one writer at a time expected - called from main thread
            void SetPreset(const SoundGraphPatchPreset& preset);

            // Read from audio thread - takes a stable snapshot for safe reading
            bool GetPresetIfChanged(SoundGraphPatchPreset& outPreset);
        } m_ThreadSafePreset;

        AtomicFlag m_PlayRequestFlag;
        bool m_PresetIsInitialized = false;
        // One-shot trace gate so the per-callback ProcessSamples log only fires once per
        // graph swap. Reset to false in ReplaceGraph.
        std::atomic<bool> m_HasLoggedFirstProcess{ false };

        // [SGSDiag] effective-rate telemetry. The window counters are audio-thread
        // local; every 100 callbacks ProcessSamples publishes a snapshot through the
        // atomics below and Update() does the (non-RT-safe) formatted logging on the
        // main thread. A snapshot the main thread hasn't consumed yet is simply
        // overwritten by the next window — losing a diagnostic line is fine.
        u32 m_DiagCallbackCount = 0;
        u64 m_DiagFrameCount = 0;
        std::chrono::steady_clock::time_point m_DiagWindowStart{};
        std::atomic<u32> m_DiagSnapshotCalls{ 0 };
        std::atomic<u64> m_DiagSnapshotFrames{ 0 };
        std::atomic<f64> m_DiagSnapshotElapsedMs{ 0.0 };
        std::atomic<bool> m_DiagSnapshotReady{ false };

        //============================================
        /// Event callbacks and thread-safe event handling
        OnGraphMessageCallback m_OnGraphMessage;
        OnGraphEventCallback m_OnGraphEvent;

        // Lock-free event queues for communicating from audio thread to main thread
        // These use pre-allocated storage to avoid any memory allocation in the audio callback
        Audio::AudioEventQueue<256> m_EventQueue;
        Audio::AudioMessageQueue<256> m_MessageQueue;
    };

} // namespace OloEngine::Audio::SoundGraph
