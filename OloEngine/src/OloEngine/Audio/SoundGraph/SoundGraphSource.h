#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPatchPreset.h"
#include <choc/containers/choc_Value.h>
#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>
#include "WaveSource.h"

#include <miniaudio.h>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
    // Use Flag utilities from Base.h
    using ::AtomicFlag;

    //==============================================================================
    /// DataSourceContext - Manages wave asset readers for sound graph
    struct DataSourceContext
    {
        // Map of asset handle to audio buffer/reader data
        std::unordered_map<AssetHandle, Audio::WaveSource> m_WaveSources;
        
        bool InitializeWaveSource(AssetHandle handle);
        void UninitializeWaveSource(AssetHandle handle);
        void UninitializeAll();
        
        bool AreAllSourcesAtEnd() const;
        int GetSourceCount() const { return static_cast<int>(m_WaveSources.size()); }
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
        const ma_engine_node* GetEngineNode() const { return &m_EngineNode; }
        ma_engine_node* GetEngineNode() { return &m_EngineNode; }

        //==============================================================================
        /// Initialization and lifecycle
        
        bool Initialize(ma_engine* engine, u32 sampleRate, u32 maxBlockSize);
        void Shutdown();
        
        void SuspendProcessing(bool shouldBeSuspended);
        bool IsSuspended() const { return m_Suspended.load(); }
        bool IsFinished() const noexcept { return m_IsFinished.load(std::memory_order_relaxed) && !m_IsPlaying.load(std::memory_order_relaxed); }
        bool IsPlaying() const { return m_IsPlaying.load() && !IsSuspended(); }

        //==============================================================================
        /// Main thread update (processes events from audio thread)
        void Update(f64 deltaTime);

        //==============================================================================
        /// SoundGraph Interface
        
        bool InitializeDataSources(const std::vector<AssetHandle>& dataSources);
        void UninitializeDataSources();
        
        /** Replace current graph with new one. Called when a new graph has been compiled. */
        void ReplaceGraph(const Ref<SoundGraph>& newGraph);
        Ref<SoundGraph> GetGraph() const { return m_Graph; }

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
        
        int GetNumDataSources() const;
        bool AreAllDataSourcesAtEnd();
        bool IsAnyDataSourceReading() { return !AreAllDataSourcesAtEnd(); }
        
        bool SendPlayEvent();
        u64 GetCurrentFrame() const { return m_CurrentFrame.load(); }

        //==============================================================================
        /// Configuration

        /** Get the sample rate used by this source */
        u32 GetSampleRate() const { return m_SampleRate; }

        //==============================================================================
        /// Event Callbacks (set by SoundGraphPlayer or other managers)
        
        using OnGraphMessageCallback = std::function<void(u64 frameIndex, const char* message)>;
        using OnGraphEventCallback = std::function<void(u64 frameIndex, u32 endpointID, const choc::value::Value& eventData)>;
        
        void SetMessageCallback(OnGraphMessageCallback callback) { m_OnGraphMessage = std::move(callback); }
        void SetEventCallback(OnGraphEventCallback callback) { m_OnGraphEvent = std::move(callback); }

    private:
        //==============================================================================
        /// Internal methods
        
        /** Called after SoundGraph has been reset to collect parameter handles */
        void UpdateParameterSet();
        
        /** Called from audio thread to apply preset changes */
        bool ApplyParameterPresetInternal();
        
        /** Called from audio thread to send updated parameters */
        void UpdateChangedParameters();
        
        /** Process audio samples - called by external audio system */
        void ProcessSamples(float** ppFramesOut, u32 frameCount);
        
        /** SoundGraph event handlers (called from audio thread) */
        static void HandleGraphEvent(void* context, u64 frameIndex, Identifier endpointID, const choc::value::ValueView& eventData);
        static void HandleGraphMessage(void* context, u64 frameIndex, const char* message);

        /** Wave source refill callback for nodes */
        static bool RefillWaveSourceCallback(Audio::WaveSource& waveSource, void* userData);

    private:
        //============================================
        /// Audio engine and processing
        ma_engine* m_Engine = nullptr;
        ma_engine_node m_EngineNode{};
        bool m_IsInitialized = false;
        
        std::atomic<bool> m_Suspended{ false };
        AtomicFlag m_SuspendFlag;
        u32 m_SampleRate = 0;
        u32 m_BlockSize = 0;

        //============================================
        /// Playback state
        std::atomic<bool> m_IsPlaying{ false };
        std::atomic<u64> m_CurrentFrame{ 0 };
        std::atomic<bool> m_IsFinished{ false };

        //============================================
        /// Sound graph and data sources
        Ref<SoundGraph> m_Graph = nullptr;
        DataSourceContext m_DataSources;

        //============================================
        /// Parameter management
        struct ParameterInfo
        {
            u32 Handle;
            std::string Name; // For debugging
            
            ParameterInfo(u32 handle, std::string_view name = "") 
                : Handle(handle), Name(name) {}
        };
        std::unordered_map<u32, ParameterInfo> m_ParameterHandles;

        //============================================
        /// Thread communication
        
        // Thread-safe parameter preset for communication between main and audio threads
        struct ThreadSafePreset
        {
            std::atomic<bool> m_HasChanges{ false };
            mutable std::mutex m_PresetMutex;
            std::shared_ptr<SoundGraphPatchPreset> m_Preset;
            
            // Only one writer at a time expected - called from main thread
            void SetPreset(const SoundGraphPatchPreset& preset)
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
            
            // Read from audio thread - takes a stable snapshot for safe reading
            bool GetPresetIfChanged(SoundGraphPatchPreset& outPreset)
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
                        // Copy preset data to output
                        outPreset.SetName(localPreset->GetName());
                        outPreset.SetDescription(localPreset->GetDescription());
                        outPreset.SetVersion(localPreset->GetVersion());
                        outPreset.SetAuthor(localPreset->GetAuthor());
                        
                        // Clear existing parameter descriptors and patches in output to prevent stale data
                        outPreset.Clear();
                        
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
        }
        
        m_ThreadSafePreset;
        AtomicFlag m_PlayRequestFlag;
        bool m_PresetIsInitialized = false;

        //============================================
        /// Event callbacks and thread-safe event handling
        OnGraphMessageCallback m_OnGraphMessage;
        OnGraphEventCallback m_OnGraphEvent;
        
        // Lock-free event queues for communicating from audio thread to main thread
        struct EventMessage
        {
            u64 FrameIndex;
            std::function<void()> Callback;
        };
        
        choc::fifo::SingleReaderSingleWriterFIFO<EventMessage> m_EventQueue;
        choc::fifo::SingleReaderSingleWriterFIFO<EventMessage> m_MessageQueue;
    };

} // namespace OloEngine::Audio::SoundGraph