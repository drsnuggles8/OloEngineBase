#pragma once

#include "NodeProcessor.h"
#include "WaveSource.h"
#include "Nodes/WavePlayer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Audio/LockFreeEventQueue.h"

#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>

#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <type_traits>
#include <queue>
#include <algorithm>
#include <atomic>
#include <string_view>

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

// Forward declarations
namespace OloEngine
{
    class SoundGraphAsset;
}

namespace OloEngine::Audio::SoundGraph
{
    // Forward declarations and utility structs
    struct GraphEvent
    {
        u64 m_FrameIndex = 0;
        Identifier m_EndpointID;
        choc::value::Value m_Value;
        std::string m_Message;
    };

    struct EndpointIDs
    {
        static inline const Identifier s_Play = Identifier("play");
        static inline const Identifier s_Stop = Identifier("stop");
    };

    //==============================================================================
    /// Raw Sound Graph containing Inputs, Outputs and Nodes
    /// This is the main executable graph that processes audio in real-time
    class SoundGraph final : public NodeProcessor, public RefCounted
    {
      public:
        struct IDs
        {
            DECLARE_ID(InLeft);
            DECLARE_ID(InRight);
            DECLARE_ID(OutLeft);
            DECLARE_ID(OutRight);

            DECLARE_ID(Play);
            DECLARE_ID(Stop);
            DECLARE_ID(OnFinished);

          private:
            IDs() = delete;
        };

        explicit SoundGraph(std::string_view debugName, UUID id)
            : NodeProcessor(debugName.data(), id), m_EndpointOutputStreams("Graph Output Endpoints", UUID())
        {
            AddInEvent(IDs::Play);

            // Create a dedicated input event for handling finish notifications
            AddInEvent(Identifier("OnFinishHandler"), [this](float v)
                       {
                    (void)v;
            // Push finish event using pre-allocated storage (real-time safe)
            Audio::AudioThreadEvent event;
            event.m_FrameIndex = m_CurrentFrame;
            event.m_EndpointID = static_cast<u32>(IDs::OnFinished);

            choc::value::Value value = choc::value::createFloat32(1.0f);
            event.m_ValueData.CopyFrom(value);
            m_OutgoingEvents.Push(event); }); // Connect using shared_ptr from InEvents - use the same identifier as registration
            if (auto finishHandlerPtr = InEvents.find(Identifier("OnFinishHandler")); finishHandlerPtr != InEvents.end())
                m_OutOnFinish.AddDestination(finishHandlerPtr->second);

            AddOutEvent(IDs::OnFinished, m_OutOnFinish);

            m_OutChannels.reserve(2);
        }

        //==============================================================================
        /// Core Components

        OutputEvent m_OutOnFinish{ *this };

        /// All nodes in the graph
        std::vector<Scope<NodeProcessor>> m_Nodes;

        /// Wave players for audio file playback (subset of nodes)
        std::vector<NodeProcessor*> m_WavePlayers; // Raw pointers to nodes in m_Nodes vector

        /// Input stream endpoints from external sources (O(1) lookup by Identifier)
        std::unordered_map<Identifier, Scope<StreamWriter>> m_EndpointInputStreams;

        /// Output stream endpoints (collects output from nodes)
        NodeProcessor m_EndpointOutputStreams;

        //==============================================================================
        /// Parameter Interpolation System

        struct InterpolatedValue
        {
            f32 m_Current;
            f32 m_Target;
            f32 m_Increment;
            i32 m_Steps = 0;
            StreamWriter* m_Endpoint;

            void SetTarget(f32 newTarget, i32 numSteps) noexcept
            {
                m_Target = newTarget;

                // Guard against division by zero and invalid step counts
                if (numSteps <= 0)
                {
                    // No interpolation - set increment to 0 and steps to 0
                    m_Increment = 0.0f;
                    m_Steps = 0;
                }
                else
                {
                    // Normal interpolation calculation
                    m_Increment = (m_Target - m_Current) / numSteps;
                    m_Steps = numSteps;
                }
            }

            void Reset(f32 initialValue) noexcept
            {
                m_Current = initialValue;
                m_Target = initialValue;
                m_Increment = 0.0f;
                m_Steps = 0;
            }

            inline void Process() noexcept
            {
                if (m_Steps > 0)
                {
                    m_Current += m_Increment;
                    --m_Steps;

                    if (m_Steps == 0)
                        m_Current = m_Target;

                    *m_Endpoint << m_Current;
                }
            }
        };

        std::unordered_map<Identifier, InterpolatedValue> m_InterpInputs;

        /// Local variable streams (internal graph state) - O(1) lookup by Identifier
        std::unordered_map<Identifier, Scope<StreamWriter>> m_LocalVariables;

        /// Output channel identifiers
        std::vector<Identifier> m_OutputChannelIDs;

        /// Output channel values (scalar, last-sample of the block)
        std::vector<float> m_OutChannels;

        /// Per-channel block-output buffers, parallel to m_OutChannels / m_OutputChannelIDs.
        /// Capacity is reserved off the audio thread via SetMaxBlockSize so that
        /// Process(numFrames) on the audio thread only adjusts the logical size (no heap
        /// allocation). Each entry [c][i] is the i-th sample of channel c produced during
        /// this block. SoundGraphSource bulk-copies these into the miniaudio output bus.
        std::vector<std::vector<f32>> m_OutputBuffers;

        /// Cached pointers to the endpoint output ValueViews, parallel to m_OutChannels /
        /// m_OutputChannelIDs. Rebuilt lazily on first Process() after Init() so the
        /// per-frame sync loop can read each output without doing an unordered_map::find
        /// — which in Debug (_ITERATOR_DEBUG_LEVEL=2) cost ~1µs per call and at 96 000
        /// lookups/sec (2 channels × 48 kHz) was a primary cause of the audio thread
        /// failing to keep real-time.
        std::vector<const choc::value::ValueView*> m_OutputChannelViews;

        //==============================================================================
        /// Graph Construction Public API

        template<typename T>
        void AddGraphInputStream(Identifier id, T&& externalObjectOrDefaultValue)
        {
            auto [it, inserted] = m_EndpointInputStreams.try_emplace(id, CreateScope<StreamWriter>(AddInStream(id), std::forward<T>(externalObjectOrDefaultValue), id));

            if (std::is_same_v<std::remove_cvref_t<T>, float>)
            {
                // Add interpolation for float parameters
                m_InterpInputs.try_emplace(id, InterpolatedValue{
                                                   .m_Current = 0.0f,
                                                   .m_Target = 0.0f,
                                                   .m_Increment = 0.0f,
                                                   .m_Steps = 0,
                                                   .m_Endpoint = it->second.get() });
            }
        }

        void AddGraphOutputStream(Identifier id)
        {
            AddOutStream<float>(id, m_OutChannels.emplace_back(0.0f));
            m_EndpointOutputStreams.AddInStream(id);

            AddConnection(OutValue(id), m_EndpointOutputStreams.InValue(id));
        }

        template<typename T>
        void AddLocalVariableStream(Identifier id, T&& externalObjectOrDefaultValue)
        {
            // StreamWriter requires a ValueView for the destination, but local variables don't
            // write to external storage. We provide an empty ValueView that will never be written to.
            // The actual storage is managed internally by StreamWriter's OutputValue member.
            m_LocalVariables.try_emplace(id, CreateScope<StreamWriter>(
                                                 choc::value::Value{}.getViewReference(),
                                                 std::forward<T>(externalObjectOrDefaultValue),
                                                 id));
        }

        void AddNode(Scope<NodeProcessor>&& node)
        {
            OLO_CORE_ASSERT(node);
            NodeProcessor* nodePtr = node.get();
            UUID nodeID = nodePtr->m_ID;

            m_Nodes.emplace_back(std::move(node));
            m_NodeLookup[nodeID] = nodePtr;
        }

        //==============================================================================
        /// Node Discovery and Management

        /// Fast O(1) node lookup by UUID using hash map
        NodeProcessor* FindNodeByID(UUID id)
        {
            auto it = m_NodeLookup.find(id);
            return (it != m_NodeLookup.end()) ? it->second : nullptr;
        }

        /// Fast O(1) node lookup by UUID using hash map (const version)
        const NodeProcessor* FindNodeByID(UUID id) const
        {
            auto it = m_NodeLookup.find(id);
            return (it != m_NodeLookup.end()) ? it->second : nullptr;
        }

        //==============================================================================
        /// Graph Connections Internal Methods

        void AddConnection(choc::value::ValueView& source, choc::value::ValueView& destination) const noexcept
        {
            destination = source;
        }

        void AddConnection(OutputEvent& source, InputEvent& destination) const noexcept
        {
            // Find the shared_ptr for this InputEvent in InEvents
            for (const auto& [id, inputEventPtr] : InEvents)
            {
                if (inputEventPtr.get() == &destination)
                {
                    source.AddDestination(inputEventPtr);
                    return;
                }
            }
            // If not found, this suggests the InputEvent isn't managed by this graph
            OLO_CORE_WARN("AddConnection: InputEvent not found in managed events");
        }

        /// Connect Input Event to Input Event
        void AddRoute(InputEvent& source, InputEvent& destination) const noexcept
        {
            InputEvent* dest = &destination;
            source.m_Event = [dest](float v)
            { (*dest)(v); };
        }

        /// Connect Output Event to Output Event
        void AddRoute(OutputEvent& source, OutputEvent& destination) noexcept
        {
            // Create a dedicated input event for routing from OutputEvent to OutputEvent
            OutputEvent* dest = &destination;
            static std::atomic<sizet> routeCounter{ 0 };
            sizet currentRouteId = routeCounter.fetch_add(1, std::memory_order_relaxed);
            std::string routeIdStr = "Route_" + std::to_string(currentRouteId);
            Identifier routeId(routeIdStr);
            AddInEvent(routeId, [dest](float v)
                       { (*dest)(v); });
            // Use the shared_ptr from InEvents for the newly created routeHandler
            if (auto routeHandlerPtr = InEvents.find(routeId); routeHandlerPtr != InEvents.end())
                source.AddDestination(routeHandlerPtr->second);
        } //==============================================================================
        /// Graph Connections Public API

        /// Node Output Value -> Node Input Value
        bool AddValueConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* sourceNode = FindNodeByID(sourceNodeID);
            auto* destinationNode = FindNodeByID(destinationNodeID);

            if (!sourceNode || !destinationNode)
            {
                OLO_CORE_ASSERT(false, "Failed to find source or destination node for value connection");
                return false;
            }

            AddConnection(sourceNode->OutValue(sourceNodeEndpointID), destinationNode->InValue(destinationNodeEndpointID));
            return true;
        }

        /// String-based overload for AddValueConnection
        bool AddValueConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
                                UUID targetNodeID, const std::string& targetEndpoint);

        /// Node Output Event -> Node Input Event
        bool AddEventConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* sourceNode = FindNodeByID(sourceNodeID);
            auto* destinationNode = FindNodeByID(destinationNodeID);

            if (!sourceNode || !destinationNode)
            {
                OLO_CORE_ASSERT(false, "Failed to find source or destination node for event connection");
                return false;
            }

            AddConnection(sourceNode->OutEvent(sourceNodeEndpointID), destinationNode->InEvent(destinationNodeEndpointID));
            return true;
        }

        /// String-based overload for AddEventConnection
        bool AddEventConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
                                UUID targetNodeID, const std::string& targetEndpoint);

        /// Graph Input Value -> Node Input Value
        bool AddInputValueRoute(Identifier graphInputEventID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* destinationNode = FindNodeByID(destinationNodeID);
            auto endpointIt = m_EndpointInputStreams.find(graphInputEventID);

            if (!destinationNode || endpointIt == m_EndpointInputStreams.end())
            {
                OLO_CORE_ASSERT(false, "Failed to find destination node or input endpoint");
                return false;
            }

            AddConnection(endpointIt->second->m_OutputValue.getViewReference(), destinationNode->InValue(destinationNodeEndpointID));
            return true;
        }

        /// Graph Input Event -> Node Input Event
        bool AddInputEventsRoute(Identifier graphInputEventID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* destinationNode = FindNodeByID(destinationNodeID);

            if (!destinationNode)
            {
                OLO_CORE_ASSERT(false, "Failed to find destination node for input event route");
                return false;
            }

            AddRoute(InEvent(graphInputEventID), destinationNode->InEvent(destinationNodeEndpointID));
            return true;
        }

        /// Node Output Value -> Graph Output Value
        bool AddToGraphOutputConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, Identifier graphOutValueID)
        {
            auto* sourceNode = FindNodeByID(sourceNodeID);

            if (!sourceNode)
            {
                OLO_CORE_ASSERT(false, "Failed to find source node for graph output connection");
                return false;
            }

            AddConnection(sourceNode->OutValue(sourceNodeEndpointID), m_EndpointOutputStreams.InValue(graphOutValueID));
            return true;
        }

        /// Node Output Event -> Graph Output Event
        bool AddToGraphOutEventConnection(UUID sourceNodeID, Identifier sourceNodeEndpointID, Identifier graphOutEventID)
        {
            auto* sourceNode = FindNodeByID(sourceNodeID);
            if (!sourceNode)
            {
                OLO_CORE_ASSERT(false, "Failed to find source node for graph output event connection");
                return false;
            }

            AddRoute(sourceNode->OutEvent(sourceNodeEndpointID), OutEvent(graphOutEventID));
            return true;
        }

        /// Graph Local Variable (StreamWriter) -> Node Input Value
        bool AddLocalVariableRoute(Identifier graphLocalVariableID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* destinationNode = FindNodeByID(destinationNodeID);
            auto endpointIt = m_LocalVariables.find(graphLocalVariableID);

            if (!destinationNode || endpointIt == m_LocalVariables.end())
            {
                OLO_CORE_ASSERT(false, "Failed to find destination node or local variable endpoint");
                return false;
            }

            AddConnection(endpointIt->second->m_OutputValue.getViewReference(), destinationNode->InValue(destinationNodeEndpointID));
            return true;
        }

        //==============================================================================
        /// Graph Lifecycle

        /// Reset Graph to its initial state
        void Reset()
        {
            m_OutgoingEvents.Clear();
            m_OutgoingMessages.Clear();
        }

        void SetSampleRate(f32 sampleRate)
        {
            m_SampleRate = sampleRate;
        }

        f32 GetSampleRate() const
        {
            return m_SampleRate;
        }

        void Init() final
        {
            OLO_PROFILE_FUNCTION();
            // Rebuild node lookup map and find wave players
            m_NodeLookup.clear();
            m_WavePlayers.clear();
            // Drop the cached endpoint-view pointers; Process() will rebuild them on
            // its next call once the freshly-(re)wired endpoint map is stable.
            m_OutputChannelViews.clear();

            for (const auto& node : m_Nodes)
            {
                // Rebuild lookup map
                m_NodeLookup[node->m_ID] = node.get();

                // Find wave players
                if (auto* wavePlayer = dynamic_cast<WavePlayer*>(node.get()))
                    m_WavePlayers.push_back(wavePlayer);
            }

            // Initialize all nodes in order, passing sample rate
            for (const auto& node : m_Nodes)
            {
                node->SetSampleRate(m_SampleRate);
                node->Init();
            }

            // Pre-allocate per-channel block buffers to a sensible default block size so
            // even consumers that drive Process() directly (instantiation tests, future
            // offline render paths) don't trigger heap allocations on the audio thread.
            // SoundGraphSource overrides this via SetMaxBlockSize() with the real miniaudio
            // block size before the audio thread starts pulling samples.
            EnsureOutputBuffersCapacity(kDefaultMaxBlockSize);

            // Pre-build the endpoint-view cache off the audio thread (was previously rebuilt
            // lazily inside Process() — that path is allocation-prone on the audio thread).
            RebuildOutputChannelViewsCache();

            m_IsInitialized = true;
        }

        /// Pre-allocates per-channel block-buffer capacity *off* the audio thread. Call
        /// after graph construction (all AddGraphOutputStream calls complete) and before
        /// any audio-thread Process(numFrames) call. Safe to call again with a larger
        /// value; never shrinks. Idempotent for sizes already covered.
        void SetMaxBlockSize(u32 maxBlockSize)
        {
            EnsureOutputBuffersCapacity(maxBlockSize);
            // Endpoint wiring may have completed *after* Init() in some construction paths
            // (e.g. SoundGraphSource::ReplaceGraph rewires before resuming). Refresh the
            // cache here too so Process() can always observe a valid cache without doing
            // the rebuild itself.
            RebuildOutputChannelViewsCache();
        }

        void BeginProcessBlock() const
        {
            // Refill wave player buffers
            for (auto& wavePlayer : m_WavePlayers)
            {
                if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
                    wp->ForceRefillBuffer();
            }
        }

        // Phase 1 block-rate entry point. Replaces the old per-sample Process() (which was
        // called 48 000 times/sec from SoundGraphSource::ProcessSamples). The per-sample
        // iteration is now hoisted INTO the graph so SoundGraphSource can hand us a whole
        // block in a single call.
        //
        // Phase 1 scope: SoundGraphSource overhead (function dispatch, OLO_PROFILE_FUNCTION
        // entry, ppFramesOut bounds-check) collapses from per-sample to per-block. The
        // per-sample iteration inside this method still walks all nodes once per sample,
        // because the ValueView wiring between nodes is scalar — moving the loop one level
        // out without changing wiring would only deliver the LAST sample of the block to
        // downstream consumers. Phase 2 introduces typed AudioBufferRef wiring; once that
        // lands, this loop can collapse to a single per-node Process(numFrames) call and
        // we get the real perf win documented in docs/soundgraph-metasounds-refactor.md.
        void Process(u32 numFrames) final
        {
            // Block-rate entry point (~100 Hz at 48 kHz / 480-frame block) — profile macro
            // is fine here. The matching note in WavePlayer::Process explains why the
            // per-sample hot path called from this method still avoids OLO_PROFILE_FUNCTION.
            OLO_PROFILE_FUNCTION();

            // The endpoint-view cache and per-channel block buffers were both pre-built off
            // the audio thread (in Init() and SetMaxBlockSize()). The asserts below are the
            // RT-safety contract: if any of them fire we'd be a missed off-thread setup
            // call away from a heap allocation in the audio callback.
            OLO_CORE_ASSERT(m_OutputChannelViews.size() == m_OutputChannelIDs.size(),
                            "SoundGraph::Process: view cache is stale; call SetMaxBlockSize/Init off-thread after wiring");
            OLO_CORE_ASSERT(m_OutputBuffers.size() >= m_OutChannels.size(),
                            "SoundGraph::Process: output buffer count is short of channel count; call SetMaxBlockSize off-thread");
            for ([[maybe_unused]] const auto& buf : m_OutputBuffers)
            {
                // Check size(), not capacity() — operator[] writes below are bounds-checked
                // against size() by MSVC Debug iterators (and are simply UB past size() in
                // Release). EnsureOutputBuffersCapacity uses resize() so size == capacity in
                // practice, but checking the dimension that actually gates operator[] keeps
                // the contract honest.
                OLO_CORE_ASSERT(buf.size() >= numFrames,
                                "SoundGraph::Process: block buffer size insufficient for numFrames; call SetMaxBlockSize off-thread");
            }

            const sizet channelCount = std::min(m_OutChannels.size(), m_OutputChannelViews.size());

            for (u32 frame = 0; frame < numFrames; ++frame)
            {
                // Per-sample parameter interpolation (10 ms ramps in SendInputValue).
                for (auto& [id, interpValue] : m_InterpInputs)
                    interpValue.Process();

                // Walk the node graph. Phase 1 passes numFrames=1 here: each node produces
                // one sample, downstream consumers read the scalar via ValueView, the chain
                // works. Phase 2 swaps in typed buffer connections and lifts this call out
                // of the per-sample loop.
                for (const auto& node : m_Nodes)
                    node->Process(1);

                // Sample each graph output channel from its (possibly re-aliased) endpoint
                // view into the per-channel block buffer. The view points at the producing
                // node's storage — see the original comment preserved on m_OutputChannelViews.
                for (sizet c = 0; c < channelCount; ++c)
                {
                    const auto* view = m_OutputChannelViews[c];
                    if (view && view->getType().isFloat32())
                        m_OutputBuffers[c][frame] = view->getFloat32();
                    else
                        m_OutputBuffers[c][frame] = 0.0f;
                }

                ++m_CurrentFrame;
            }

            // Mirror the final sample into the scalar m_OutChannels for any consumer that
            // reads it directly (debug UIs, telemetry). The block buffer is the real output.
            for (sizet c = 0; c < channelCount; ++c)
            {
                if (numFrames > 0)
                    m_OutChannels[c] = m_OutputBuffers[c][numFrames - 1];
            }
        }

        // Reset nodes to their initial state
        void Reinit()
        {
            OLO_PROFILE_FUNCTION();

            m_OutgoingEvents.Clear();
            m_OutgoingMessages.Clear();

            for (const auto& node : m_Nodes)
                node->Init();
        }

        //==============================================================================
        /// Runtime Status

        bool IsPlayable() const
        {
            return m_IsInitialized;
        }

        void InitializeEndpoints();
        void OnPlay(f32 value);
        void OnStop(f32 value);
        void OnFinished(f32 value);
        void Play();
        void Stop();
        void TriggerGraphEvent(std::string_view eventName, f32 value);

        //==============================================================================
        /// Event and Message Handling

        /// Used in HandleOutgoingEvents
        using HandleOutgoingEventFn = void(void* userContext, u64 frameIndex, Identifier endpointName, const choc::value::ValueView&);
        /// Used in HandleOutgoingEvents
        using HandleConsoleMessageFn = void(void* userContext, u64 frameIndex, const char* message);

        /// Flushes any outgoing event and console messages that are currently queued
        /// This must be called periodically if the graph is generating events
        void HandleOutgoingEvents(void* userContext, HandleOutgoingEventFn* handleEvent, HandleConsoleMessageFn* handleConsoleMessage)
        {
            Audio::AudioThreadEvent outEvent;
            while (m_OutgoingEvents.Pop(outEvent))
            {
                // Convert EndpointID back to Identifier and get ValueView
                Identifier endpointID(outEvent.m_EndpointID);
                choc::value::ValueView valueView = outEvent.m_ValueData.GetView();
                handleEvent(userContext, outEvent.m_FrameIndex, endpointID, valueView);
            }

            Audio::AudioThreadMessage outMessage;
            while (m_OutgoingMessages.Pop(outMessage))
            {
                handleConsoleMessage(userContext, outMessage.m_FrameIndex, outMessage.m_Text);
            }
        }

        //==============================================================================
        /// Parameter Interface

        bool SendInputValue(u32 endpointID, choc::value::ValueView value, bool interpolate)
        {
            // Find endpoint by searching through the map (still O(n) due to u32 lookup, but rare case)
            auto endpointIt = std::ranges::find_if(m_EndpointInputStreams,
                                                   [endpointID](const auto& pair)
                                                   {
                                                       return (u32)pair.second->m_DestinationID == endpointID;
                                                   });
            if (endpointIt == m_EndpointInputStreams.end())
                return false;

            const auto& endpoint = endpointIt->second;

            if (value.isFloat32())
            {
                // Handle interpolation for float values - use safe lookup
                auto interpIt = m_InterpInputs.find(endpoint->m_DestinationID);
                if (interpIt != m_InterpInputs.end())
                {
                    auto& interpInput = interpIt->second;
                    if (interpolate)
                    {
                        interpInput.SetTarget(value.getFloat32(), 480); // 10ms at 48kHz
                    }
                    else
                    {
                        interpInput.Reset(value.getFloat32());
                        *endpoint << value;
                    }
                }
                else
                {
                    // No interpolation registered, just set the value directly
                    *endpoint << value;
                }
            }
            else
            {
                *endpoint << value;
            }

            return true;
        }

        bool SendInputEvent(Identifier endpointID, choc::value::ValueView value)
        {
            auto endpoint = InEvents.find(endpointID);

            if (endpoint == InEvents.end() || !endpoint->second || !endpoint->second->m_Event)
                return false;

            // Handle float values for events
            endpoint->second->m_Event(value.isFloat32() ? value.getFloat32() : 1.0f);
            return true;
        }

        std::vector<Identifier> GetInputEventEndpoints() const
        {
            std::vector<Identifier> handles;
            handles.reserve(InEvents.size());

            for (const auto& [handle, endpoint] : InEvents)
                handles.push_back(handle);

            return handles;
        }

        std::vector<Identifier> GetParameters() const
        {
            std::vector<Identifier> handles;
            handles.reserve(m_EndpointInputStreams.size());

            for (const auto& [id, endpoint] : m_EndpointInputStreams)
                handles.push_back(id);

            return handles;
        }

        //==============================================================================
        /// Wave Source Management (for future implementation)

        using RefillCallback = bool (*)(Audio::WaveSource&, void* userData, u32 numFrames);
        void SetRefillWavePlayerBufferCallback(RefillCallback callback, void* userData, u32 numFrames) const
        {
            for (auto& wavePlayer : m_WavePlayers)
            {
                if (auto* wp = static_cast<WavePlayer*>(wavePlayer))
                {
                    // Store the original callback to chain with it
                    auto originalCallback = wp->GetWaveSource().m_OnRefill;

                    // Set new callback that chains with the original
                    wp->GetWaveSource().m_OnRefill = [callback, userData, numFrames, originalCallback](Audio::WaveSource& source) -> bool
                    {
                        bool result = true;
                        if (originalCallback)
                            result = originalCallback(source);
                        if (callback)
                            result &= callback(source, userData, numFrames);
                        return result;
                    };
                }
            }
        }

      private:
        // Default per-channel block-buffer capacity used by Init() when SoundGraphSource
        // hasn't yet called SetMaxBlockSize. Sized generously so direct-Process callers
        // (tests, offline render) don't trip the capacity assert in Process.
        static constexpr u32 kDefaultMaxBlockSize = 4096;

        /// Ensure m_OutputBuffers has one entry per output channel and each entry has
        /// `capacity` valid storage slots. Both .size() and .capacity() end up >= capacity.
        /// MUST be called off the audio thread.
        void EnsureOutputBuffersCapacity(u32 capacity)
        {
            if (m_OutputBuffers.size() < m_OutChannels.size())
                m_OutputBuffers.resize(m_OutChannels.size());
            for (auto& buf : m_OutputBuffers)
            {
                if (buf.size() < capacity)
                    buf.resize(capacity);
            }
        }

        /// (Re-)build the endpoint-view pointer cache used by Process() to read the
        /// graph's output channel values without an unordered_map::find per sample.
        /// MUST be called off the audio thread (clear + reserve + push_back can allocate).
        /// Safe to call repeatedly when wiring changes.
        void RebuildOutputChannelViewsCache()
        {
            m_OutputChannelViews.clear();
            m_OutputChannelViews.reserve(m_OutputChannelIDs.size());
            for (const auto& id : m_OutputChannelIDs)
            {
                auto it = m_EndpointOutputStreams.InputStreams.find(id);
                m_OutputChannelViews.push_back(it != m_EndpointOutputStreams.InputStreams.end() ? &it->second : nullptr);
            }
        }

        bool m_IsInitialized = false;
        f32 m_SampleRate = 48000.0f;

        bool m_HasFinished = false;
        bool m_IsPlaying = false;
        u64 m_CurrentFrame = 0;
        std::string m_DebugName;

        // Fast node lookup map (O(1) instead of O(n))
        std::unordered_map<UUID, NodeProcessor*> m_NodeLookup;

        //==============================================================================
        /// Thread-safe Event/Message Queues
        /// Using lock-free queues with pre-allocated storage to avoid heap allocations in audio thread

        Audio::AudioEventQueue<1024> m_OutgoingEvents;
        Audio::AudioMessageQueue<1024> m_OutgoingMessages;
    };

    //==============================================================================
    /// Template Specialization for choc::value::Value

    template<>
    inline void SoundGraph::AddGraphInputStream(Identifier id, choc::value::Value&& externalObjectOrDefaultValue)
    {
        const bool isFloat = externalObjectOrDefaultValue.isFloat32();

        auto [it, inserted] = m_EndpointInputStreams.try_emplace(id, CreateScope<StreamWriter>(AddInStream(id), std::move(externalObjectOrDefaultValue), id));

        if (isFloat)
        {
            m_InterpInputs.try_emplace(id, InterpolatedValue{
                                               .m_Current = 0.0f,
                                               .m_Target = 0.0f,
                                               .m_Increment = 0.0f,
                                               .m_Steps = 0,
                                               .m_Endpoint = it->second.get() });
        }
    }

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef LOG_DBG_MESSAGES
#undef DBG
