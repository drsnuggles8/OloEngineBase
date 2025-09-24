#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "SoundGraph.h"
#include "SoundGraphPatchPreset.h"
#include "Value.h"

#include <atomic>
#include <functional>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /** Data source interface between SoundGraphSound and SoundGraph instance.
        Handles real-time audio processing and parameter management.
    */
    class SoundGraphSource
    {
    public:
        explicit SoundGraphSource();
        ~SoundGraphSource();

        //==============================================================================
        /// Audio Processing Interface
        bool Init(u32 sampleRate, u32 maxBlockSize);
        void ProcessBlock(const f32** inputFrames, f32** outputFrames, u32 frameCount);
        void ReleaseResources();

        void SuspendProcessing(bool shouldBeSuspended);
        bool IsSuspended() const { return m_Suspended.load(); }

        bool IsFinished() const { return m_IsFinished && !m_IsPlaying; }

        //==============================================================================
        /// Audio Update (called from main thread)
        void Update(f32 deltaTime);

        //==============================================================================
        /// SoundGraph Interface
        bool InitializeDataSources(const std::vector<AssetHandle>& dataSources);
        void UninitializeDataSources();
        
        /** Replace current graph with new one */
        void ReplaceGraph(const Ref<SoundGraph>& newGraph);
        Ref<SoundGraph> GetGraph() const { return m_Graph; }

        //==============================================================================
        /// Parameter Interface
        
        /** Set graph parameter value by name */
        bool SetParameter(const std::string& parameterName, const ValueView& value);

        /** Set graph parameter value by ID */
        bool SetParameter(u32 parameterID, const ValueView& value);

        /** Apply parameter preset */
        bool ApplyParameterPreset(const SoundGraphPatchPreset& preset);

        //==============================================================================
        /// Playback Interface
        bool SendPlayEvent();
        bool SendStopEvent();
        
        bool IsPlaying() const { return m_IsPlaying.load(); }
        u64 GetCurrentFrame() const { return m_CurrentFrame.load(); }

        //==============================================================================
        /// Event Handling
        using OnGraphEventCallback = std::function<void(u64 frameIndex, Identifier endpointID, const ValueView& eventData)>;
        using OnGraphMessageCallback = std::function<void(u64 frameIndex, const char* message)>;

        void SetEventCallback(OnGraphEventCallback callback) { m_OnGraphEvent = callback; }
        void SetMessageCallback(OnGraphMessageCallback callback) { m_OnGraphMessage = callback; }

    private:
        void UpdateParameterSet();
        void UpdateChangedParameters();
        void HandleOutgoingEvents();
        
    private:
        //==============================================================================
        /// Audio Processing State
        std::atomic<bool> m_Suspended = false;
        std::atomic<bool> m_IsPlaying = false;
        std::atomic<bool> m_IsFinished = false;
        std::atomic<u64> m_CurrentFrame = 0;

        u32 m_SampleRate = 0;
        u32 m_BlockSize = 0;

        //==============================================================================
        /// SoundGraph Instance
        Ref<SoundGraph> m_Graph;

        //==============================================================================
        /// Parameter Management
        struct ParameterInfo
        {
            u32 Handle;
            std::string Name;
            
            ParameterInfo(u32 handle, const std::string& name)
                : Handle(handle), Name(name) {}
        };
        std::unordered_map<u32, ParameterInfo> m_ParameterHandles;
        std::unordered_map<std::string, u32> m_ParameterNameToHandle;

        // Thread-safe parameter updates
        mutable std::mutex m_PresetMutex;
        SoundGraphPatchPreset m_PendingPreset;
        std::atomic<bool> m_PresetUpdatePending = false;
        bool m_PresetInitialized = false;

        //==============================================================================
        /// Event System
        OnGraphEventCallback m_OnGraphEvent;
        OnGraphMessageCallback m_OnGraphMessage;

        // Pending events to process on main thread
        std::mutex m_EventQueueMutex;
        std::vector<std::function<void()>> m_PendingEvents;

        //==============================================================================
        /// Playback Control
        std::atomic<bool> m_PlayRequested = false;
        std::atomic<bool> m_StopRequested = false;
    };

} // namespace OloEngine::Audio::SoundGraph