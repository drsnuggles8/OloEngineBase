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
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

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
            : NodeProcessor(debugName.data(), id)
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
        }

        //==============================================================================
        /// Core Components

        OutputEvent m_OutOnFinish{ *this };

        /// All nodes in the graph
        std::vector<Scope<NodeProcessor>> m_Nodes;

        /// Wave players for audio file playback (subset of nodes)
        std::vector<NodeProcessor*> m_WavePlayers; // Raw pointers to nodes in m_Nodes vector

        //==============================================================================
        /// Graph value cells — typed storage behind graph input parameters and local
        /// variables. Consumers' input refs bind directly to a cell's scalar member
        /// (control-rate) or to its ramp buffer (audio-rate float consumers). The map
        /// is node-based, so cell addresses are stable across inserts.

        struct GraphValueCell
        {
            EStreamType m_Type = EStreamType::Float;

            f32 m_Float = 0.0f;
            i32 m_Int32 = 0;
            i64 m_Int64 = 0;
            bool m_Bool = false;

            /// Float cells carry a block buffer so audio-rate consumers see smooth
            /// per-sample parameter ramps (SendInputValue's 10 ms interpolation).
            /// Refilled lazily: a broadcast fill when the scalar changed
            /// (m_BufferDirty), per-frame writes while a ramp is active.
            Scope<AudioBuffer> m_RampBuffer;
            bool m_BufferDirty = true;

            void InitFloat(f32 v)
            {
                m_Type = EStreamType::Float;
                m_Float = v;
                m_RampBuffer = CreateScope<AudioBuffer>();
                m_BufferDirty = true;
            }
            void InitInt32(i32 v)
            {
                m_Type = EStreamType::Int32;
                m_Int32 = v;
            }
            void InitInt64(i64 v)
            {
                m_Type = EStreamType::Int64;
                m_Int64 = v;
            }
            void InitBool(bool v)
            {
                m_Type = EStreamType::Bool;
                m_Bool = v;
            }
            void InitFromValue(const choc::value::ValueView& v)
            {
                if (v.isFloat32())
                    InitFloat(std::isfinite(v.getFloat32()) ? v.getFloat32() : 0.0f);
                else if (v.isFloat64())
                {
                    const f64 value = v.getFloat64();
                    InitFloat(std::isfinite(value) ? static_cast<f32>(value) : 0.0f);
                }
                else if (v.isInt32())
                    InitInt32(v.getInt32());
                else if (v.isInt64())
                    InitInt64(v.getInt64());
                else if (v.isBool())
                    InitBool(v.getBool());
                else
                    InitFloat(0.0f); // unsupported plug type — default-silent float
            }

            /// Saturating f64 -> i64: casting NaN/Inf or out-of-range doubles to an
            /// integer is undefined behavior, so clamp into representable range first.
            static i64 ToSaturatedInt(f64 v) noexcept
            {
                constexpr f64 kMin = static_cast<f64>(std::numeric_limits<i64>::min());
                constexpr f64 kMax = static_cast<f64>(std::numeric_limits<i64>::max());
                if (v <= kMin)
                    return std::numeric_limits<i64>::min();
                if (v >= kMax)
                    return std::numeric_limits<i64>::max();
                return static_cast<i64>(v);
            }

            /// Typed scalar write with tolerant numeric conversion. Marks the ramp
            /// buffer dirty so audio-rate consumers pick the change up next block.
            /// Non-finite floats are rejected (cell keeps its previous value).
            bool SetScalarFromValue(const choc::value::ValueView& v)
            {
                f64 numeric = 0.0;
                i64 integer = 0;
                if (v.isFloat32())
                {
                    numeric = static_cast<f64>(v.getFloat32());
                    if (!std::isfinite(numeric))
                        return false;
                    integer = ToSaturatedInt(numeric);
                }
                else if (v.isFloat64())
                {
                    numeric = v.getFloat64();
                    if (!std::isfinite(numeric))
                        return false;
                    integer = ToSaturatedInt(numeric);
                }
                else if (v.isInt32())
                {
                    integer = static_cast<i64>(v.getInt32());
                    numeric = static_cast<f64>(integer);
                }
                else if (v.isInt64())
                {
                    integer = v.getInt64();
                    numeric = static_cast<f64>(integer);
                }
                else if (v.isBool())
                {
                    integer = v.getBool() ? 1 : 0;
                    numeric = static_cast<f64>(integer);
                }
                else
                {
                    return false;
                }

                switch (m_Type)
                {
                    case EStreamType::Float:
                        m_Float = static_cast<f32>(numeric);
                        break;
                    case EStreamType::Int32:
                        m_Int32 = static_cast<i32>(integer);
                        break;
                    case EStreamType::Int64:
                        m_Int64 = integer;
                        break;
                    case EStreamType::Bool:
                        m_Bool = (integer != 0) || (numeric != 0.0);
                        break;
                    case EStreamType::Audio:
                        return false; // cells are never audio-typed
                }
                m_BufferDirty = true;
                return true;
            }
        };

        /// Input parameter cells from external sources (O(1) lookup by Identifier)
        std::unordered_map<Identifier, GraphValueCell> m_EndpointInputStreams;

        /// Local variable cells (internal graph state) - O(1) lookup by Identifier
        std::unordered_map<Identifier, GraphValueCell> m_LocalVariables;

        /// Graph output endpoints. Each is an AudioBufferRef bound (by a
        /// NodeValue_GraphValue connection) to the producing node's output buffer or
        /// scalar; Process copies through them into m_OutputBuffers. Includes event
        /// endpoints like OnFinished, which simply stay unbound.
        std::unordered_map<Identifier, AudioBufferRef> m_GraphOutputRefs;

        //==============================================================================
        /// Parameter Interpolation System

        struct InterpolatedValue
        {
            f32 m_Current = 0.0f;
            f32 m_Target = 0.0f;
            f32 m_Increment = 0.0f;
            i32 m_Steps = 0;
            GraphValueCell* m_Cell = nullptr;

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

            /// Advance the ramp by one block, writing per-frame values into the
            /// cell's ramp buffer (audio-rate consumers) and the final value into
            /// the scalar (control-rate consumers). No-op when idle.
            inline void ProcessBlock(u32 numFrames) noexcept
            {
                if (m_Steps <= 0 || !m_Cell)
                    return;

                f32* buffer = m_Cell->m_RampBuffer ? m_Cell->m_RampBuffer->Data() : nullptr;
                for (u32 frame = 0; frame < numFrames; ++frame)
                {
                    if (m_Steps > 0)
                    {
                        m_Current += m_Increment;
                        --m_Steps;
                        if (m_Steps == 0)
                            m_Current = m_Target;
                    }
                    if (buffer)
                        buffer[frame] = m_Current;
                }

                m_Cell->m_Float = m_Current;
                // While ramping we own the buffer contents; once the ramp lands,
                // request a full broadcast fill so frames beyond this block's
                // numFrames hold the settled value too.
                m_Cell->m_BufferDirty = (m_Steps == 0);
            }
        };

        std::unordered_map<Identifier, InterpolatedValue> m_InterpInputs;

        /// Output channel identifiers
        std::vector<Identifier> m_OutputChannelIDs;

        /// Output channel values (scalar, last-sample of the block)
        std::vector<float> m_OutChannels;

        /// Per-channel block-output buffers, parallel to m_OutChannels / m_OutputChannelIDs.
        /// Capacity is reserved off the audio thread via SetMaxBlockSize so that
        /// Process(numFrames) on the audio thread only adjusts the logical size (no heap
        /// allocation). Each entry [c][i] is the i-th sample of channel c produced during
        /// this block. SoundGraphSource bulk-copies these into the miniaudio bus.
        std::vector<std::vector<f32>> m_OutputBuffers;

        //==============================================================================
        /// Graph Construction Public API

        template<typename T>
        void AddGraphInputStream(Identifier id, T&& externalObjectOrDefaultValue)
        {
            using U = std::remove_cvref_t<T>;

            auto [it, inserted] = m_EndpointInputStreams.try_emplace(id);
            if (!inserted)
                return;
            GraphValueCell& cell = it->second;

            if constexpr (std::is_same_v<U, choc::value::Value>)
                cell.InitFromValue(externalObjectOrDefaultValue.getViewReference());
            else if constexpr (std::is_same_v<U, f32>)
                cell.InitFloat(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, i32>)
                cell.InitInt32(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, i64>)
                cell.InitInt64(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, bool>)
                cell.InitBool(externalObjectOrDefaultValue);
            else
                static_assert(std::is_same_v<U, choc::value::Value>,
                              "Graph input default must be a stream value type or choc::value::Value");

            if (cell.m_Type == EStreamType::Float)
            {
                // Add interpolation for float parameters
                m_InterpInputs.try_emplace(id, InterpolatedValue{
                                                   .m_Current = cell.m_Float,
                                                   .m_Target = cell.m_Float,
                                                   .m_Increment = 0.0f,
                                                   .m_Steps = 0,
                                                   .m_Cell = &cell });
            }
        }

        void AddGraphOutputStream(Identifier id)
        {
            // m_OutChannels and m_GraphOutputRefs must stay parallel (the output copy
            // in ProcessChunk sizes off both) — only grow the scalar mirror when the
            // ref actually inserted, so a duplicate id can't skew the channel count.
            auto [it, inserted] = m_GraphOutputRefs.try_emplace(id); // default: silent constant until wired
            if (!inserted)
            {
                OLO_CORE_WARN("AddGraphOutputStream: duplicate graph output endpoint {} ignored", static_cast<u32>(id));
                return;
            }
            m_OutChannels.emplace_back(0.0f);
        }

        template<typename T>
        void AddLocalVariableStream(Identifier id, T&& externalObjectOrDefaultValue)
        {
            using U = std::remove_cvref_t<T>;

            auto [it, inserted] = m_LocalVariables.try_emplace(id);
            if (!inserted)
                return;
            GraphValueCell& cell = it->second;

            if constexpr (std::is_same_v<U, choc::value::Value>)
                cell.InitFromValue(externalObjectOrDefaultValue.getViewReference());
            else if constexpr (std::is_same_v<U, f32>)
                cell.InitFloat(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, i32>)
                cell.InitInt32(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, i64>)
                cell.InitInt64(externalObjectOrDefaultValue);
            else if constexpr (std::is_same_v<U, bool>)
                cell.InitBool(externalObjectOrDefaultValue);
            else
                static_assert(std::is_same_v<U, choc::value::Value>,
                              "Local variable default must be a stream value type or choc::value::Value");
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

        /// Compile-time-typed connection helpers. The Identifier-based public API
        /// below resolves endpoints from runtime (asset) data and validates types at
        /// bind time; these overloads serve code-constructed graphs where both ends
        /// are known statically — a type mismatch fails to compile.

        /// Audio producer -> audio consumer (per-frame samples)
        static void AddConnection(const AudioBuffer& source, AudioBufferRef& destination) noexcept
        {
            destination.BindBuffer(source.Data());
        }

        /// Scalar f32 producer -> audio consumer (broadcast across the block)
        static void AddConnection(const f32& source, AudioBufferRef& destination) noexcept
        {
            destination.BindScalar(&source);
        }

        /// Scalar producer -> scalar consumer of the same value type
        template<StreamValue T>
        static void AddConnection(const T& source, ValueRef<T>& destination) noexcept
        {
            destination.Bind(&source);
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
        }

        /// Bind a graph value cell (parameter / local variable) to a node input ref.
        static bool ConnectCellToNodeInput(GraphValueCell& cell, NodeProcessor& node, Identifier endpointID)
        {
            auto it = node.InputRefs.find(endpointID);
            if (it == node.InputRefs.end())
            {
                OLO_CORE_WARN("ConnectCellToNodeInput: node '{}' has no input stream {}",
                              node.m_DebugName, static_cast<u32>(endpointID));
                return false;
            }

            InputStreamRef& dst = it->second;
            switch (dst.m_Type)
            {
                case EStreamType::Audio:
                    if (cell.m_Type == EStreamType::Float)
                    {
                        // Audio-rate consumer reads the cell's ramp buffer so
                        // interpolated parameter changes stay per-sample smooth.
                        static_cast<AudioBufferRef*>(dst.m_Ref)->BindBuffer(cell.m_RampBuffer->Data());
                        return true;
                    }
                    break;
                case EStreamType::Float:
                    if (cell.m_Type == EStreamType::Float)
                    {
                        static_cast<FloatRef*>(dst.m_Ref)->Bind(&cell.m_Float);
                        return true;
                    }
                    break;
                case EStreamType::Int32:
                    if (cell.m_Type == EStreamType::Int32)
                    {
                        static_cast<IntRef*>(dst.m_Ref)->Bind(&cell.m_Int32);
                        return true;
                    }
                    break;
                case EStreamType::Int64:
                    if (cell.m_Type == EStreamType::Int64)
                    {
                        static_cast<Int64Ref*>(dst.m_Ref)->Bind(&cell.m_Int64);
                        return true;
                    }
                    break;
                case EStreamType::Bool:
                    if (cell.m_Type == EStreamType::Bool)
                    {
                        static_cast<BoolRef*>(dst.m_Ref)->Bind(&cell.m_Bool);
                        return true;
                    }
                    break;
            }

            OLO_CORE_WARN("ConnectCellToNodeInput: incompatible types {} -> {} (node '{}', endpoint {})",
                          static_cast<i32>(cell.m_Type), static_cast<i32>(dst.m_Type),
                          node.m_DebugName, static_cast<u32>(endpointID));
            return false;
        }

        //==============================================================================
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

            if (!NodeProcessor::ConnectStreams(*sourceNode, sourceNodeEndpointID, *destinationNode, destinationNodeEndpointID))
                return false;

            // Record the dependency edge so Init() can order producers before
            // consumers for the once-per-block Process walk.
            m_ConnectionEdges.emplace_back(sourceNodeID, destinationNodeID);
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

            // Event producers should also run before their consumers so triggers
            // fired mid-block apply within the same block, matching the old
            // per-sample propagation latency as closely as possible.
            m_ConnectionEdges.emplace_back(sourceNodeID, destinationNodeID);
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

            return ConnectCellToNodeInput(endpointIt->second, *destinationNode, destinationNodeEndpointID);
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

            auto refIt = m_GraphOutputRefs.find(graphOutValueID);
            if (refIt == m_GraphOutputRefs.end())
            {
                OLO_CORE_ASSERT(false, "Failed to find graph output endpoint");
                return false;
            }

            auto srcIt = sourceNode->OutputSources.find(sourceNodeEndpointID);
            if (srcIt == sourceNode->OutputSources.end())
            {
                OLO_CORE_WARN("AddToGraphOutputConnection: source node '{}' has no output stream {}",
                              sourceNode->m_DebugName, static_cast<u32>(sourceNodeEndpointID));
                return false;
            }

            InputStreamRef dst{ EStreamType::Audio, &refIt->second };
            if (!NodeProcessor::BindInputToOutput(dst, srcIt->second))
            {
                OLO_CORE_WARN("AddToGraphOutputConnection: incompatible source type for graph output (node '{}')",
                              sourceNode->m_DebugName);
                return false;
            }
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

        /// Graph Local Variable -> Node Input Value
        bool AddLocalVariableRoute(Identifier graphLocalVariableID, UUID destinationNodeID, Identifier destinationNodeEndpointID) noexcept
        {
            auto* destinationNode = FindNodeByID(destinationNodeID);
            auto endpointIt = m_LocalVariables.find(graphLocalVariableID);

            if (!destinationNode || endpointIt == m_LocalVariables.end())
            {
                OLO_CORE_ASSERT(false, "Failed to find destination node or local variable endpoint");
                return false;
            }

            return ConnectCellToNodeInput(endpointIt->second, *destinationNode, destinationNodeEndpointID);
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

            // Pre-build the per-block runtime caches off the audio thread (process
            // order, output-ref pointers, ramp-buffer cell list).
            RebuildRuntimeCaches();

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
            // caches here too so Process() can always observe valid caches without doing
            // the rebuild itself.
            RebuildRuntimeCaches();
        }

        /// Phase 3 contiguous output-buffer pool (docs/soundgraph-metasounds-refactor.md).
        /// Gathers every node's audio-output AudioBuffer into one contiguous allocation
        /// so the per-node output buffers live in a single block (cache locality, one
        /// allocation) instead of N scattered kMaxAudioBlockFrames vectors.
        ///
        /// MUST be called off the audio thread, exactly once, AFTER all nodes are
        /// created but BEFORE any connection captures a producer's Data() pointer (the
        /// StreamRefs.h pointer-stability contract). CreateInstance calls it between
        /// node creation and wiring. Graphs built without it (direct/test construction)
        /// simply keep each buffer's self-owned storage — behaviour is identical.
        ///
        /// Stride is fixed at kMaxAudioBlockFrames (not the runtime block size) so the
        /// pool is sized once and never resized: SetMaxBlockSize, which runs *after*
        /// wiring, must never reallocate it or it would dangle every captured pointer.
        void AllocateNodeOutputPool()
        {
            if (!m_NodeOutputPool.empty())
                return; // already pooled (idempotent — never resize after wiring)

            std::vector<AudioBuffer*> outputs;
            for (const auto& node : m_Nodes)
            {
                for (auto& [id, source] : node->OutputSources)
                {
                    if (source.m_Type == EStreamType::Audio)
                    {
                        // OutputSources stores const void* into the node's own (mutable)
                        // AudioBuffer member; the const_cast targets a non-const object.
                        outputs.push_back(const_cast<AudioBuffer*>(static_cast<const AudioBuffer*>(source.m_Source)));
                    }
                }
            }

            if (outputs.empty())
                return;

            constexpr sizet stride = static_cast<sizet>(kMaxAudioBlockFrames);
            m_NodeOutputPool.assign(outputs.size() * stride, 0.0f);
            for (sizet i = 0; i < outputs.size(); ++i)
                outputs[i]->AdoptPoolStorage(m_NodeOutputPool.data() + i * stride);
        }

        /// [begin, end) of the contiguous node-output pool, or {nullptr, nullptr} when
        /// the graph isn't pooled (direct/test construction). Diagnostic / test hook:
        /// lets callers confirm a node's output buffer was relocated into the pool.
        std::pair<const f32*, const f32*> GetNodeOutputPoolRange() const
        {
            if (m_NodeOutputPool.empty())
                return { nullptr, nullptr };
            return { m_NodeOutputPool.data(), m_NodeOutputPool.data() + m_NodeOutputPool.size() };
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

        // Phase 2 block-rate entry point: each node's Process(numFrames) is called
        // exactly once per chunk, in producer-before-consumer order, with typed
        // connections carrying whole blocks between nodes. This replaces the Phase 1
        // per-sample node walk (numFrames=1, 48 000 node sweeps/sec) that scalar
        // ValueView wiring forced, and with it the Debug-build real-time deficit
        // documented in docs/soundgraph-metasounds-refactor.md.
        void Process(u32 numFrames) final
        {
            // Block-rate entry point (~100 Hz at 48 kHz / 480-frame block).
            OLO_PROFILE_FUNCTION();

            // Per-node output buffers are fixed at kMaxAudioBlockFrames capacity
            // (pointer-stability contract in StreamRefs.h), so larger requests are
            // processed in chunks. The normal miniaudio block is far below the cap —
            // this loop runs exactly once.
            u32 done = 0;
            while (done < numFrames)
            {
                const u32 chunk = std::min(numFrames - done, kMaxAudioBlockFrames);
                ProcessChunk(chunk, done);
                done += chunk;
            }

            // Mirror the final sample into the scalar m_OutChannels for any consumer that
            // reads it directly (debug UIs, telemetry). The block buffer is the real output.
            const sizet channelCount = std::min(m_OutChannels.size(), m_OutputBuffers.size());
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
                                                       return static_cast<u32>(pair.first) == endpointID;
                                                   });
            if (endpointIt == m_EndpointInputStreams.end())
                return false;

            GraphValueCell& cell = endpointIt->second;

            if (value.isFloat32() && cell.m_Type == EStreamType::Float)
            {
                // Handle interpolation for float values - use safe lookup
                auto interpIt = m_InterpInputs.find(endpointIt->first);
                if (interpIt != m_InterpInputs.end())
                {
                    auto& interpInput = interpIt->second;
                    if (interpolate)
                    {
                        interpInput.SetTarget(value.getFloat32(), 480); // 10ms at 48kHz
                        return true;
                    }
                    interpInput.Reset(value.getFloat32());
                }
            }

            return cell.SetScalarFromValue(value);
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
        static constexpr u32 kDefaultMaxBlockSize = kMaxAudioBlockFrames;

        /// One chunk of block processing (numFrames <= kMaxAudioBlockFrames).
        /// outOffset is the write offset into m_OutputBuffers for multi-chunk calls.
        void ProcessChunk(u32 numFrames, u32 outOffset)
        {
            // The caches and per-channel block buffers were all pre-built off the
            // audio thread (Init() / SetMaxBlockSize()). The asserts below are the
            // RT-safety contract: if any of them fire we'd be a missed off-thread
            // setup call away from a heap allocation in the audio callback.
            OLO_CORE_ASSERT(m_OutputChannelRefCache.size() == m_OutputChannelIDs.size(),
                            "SoundGraph::Process: ref cache is stale; call SetMaxBlockSize/Init off-thread after wiring");
            OLO_CORE_ASSERT(m_CompiledOps.size() == m_ProcessOrder.size(),
                            "SoundGraph::Process: compiled plan is stale; call SetMaxBlockSize/Init off-thread after wiring");
            OLO_CORE_ASSERT(m_OutputBuffers.size() >= m_OutChannels.size(),
                            "SoundGraph::Process: output buffer count is short of channel count; call SetMaxBlockSize off-thread");
            for ([[maybe_unused]] const auto& buf : m_OutputBuffers)
            {
                OLO_CORE_ASSERT(buf.size() >= static_cast<sizet>(outOffset) + numFrames,
                                "SoundGraph::Process: block buffer size insufficient for numFrames; call SetMaxBlockSize off-thread");
            }

            // 1. Parameter maintenance: broadcast-fill any ramp buffer whose scalar
            //    changed since the last block, THEN advance active ramps (per-frame
            //    writes). Fill-before-ramp matters: a ramp that lands inside this
            //    block sets the dirty flag for the *next* block's broadcast — running
            //    the fill afterwards would clobber the ramp samples it just wrote.
            for (auto* cell : m_RampBufferCells)
            {
                if (cell->m_BufferDirty)
                {
                    std::fill_n(cell->m_RampBuffer->Data(), kMaxAudioBlockFrames, cell->m_Float);
                    cell->m_BufferDirty = false;
                }
            }

            for (auto& [id, interpValue] : m_InterpInputs)
                interpValue.ProcessBlock(numFrames);

            // 2. Walk the compiled execution plan once per chunk, producers before
            //    consumers. Each op is a {devirtualized thunk, node} pair; the call
            //    goes through a stored function pointer — no hash lookups, no vtable
            //    load, no choc::value reads. Typed connections hand whole blocks (or
            //    stable scalars) downstream.
            for (const CompiledOp& op : m_CompiledOps)
                op.m_Fn(op.m_State, numFrames);

            // 3. Copy each graph output endpoint into its per-channel block buffer.
            const sizet channelCount = std::min(m_OutChannels.size(), m_OutputChannelRefCache.size());
            for (sizet c = 0; c < channelCount; ++c)
            {
                f32* dst = m_OutputBuffers[c].data() + outOffset;
                const AudioBufferRef* ref = m_OutputChannelRefCache[c];
                if (ref && ref->IsBuffer())
                    std::memcpy(dst, ref->m_Data, static_cast<sizet>(numFrames) * sizeof(f32));
                else if (ref)
                    std::fill_n(dst, numFrames, *ref->m_Data);
                else
                    std::fill_n(dst, numFrames, 0.0f);
            }

            m_CurrentFrame += numFrames;
        }

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

        /// (Re-)build the per-block runtime caches: graph-output ref pointers,
        /// ramp-buffer cell list, and the producer-before-consumer process order.
        /// MUST be called off the audio thread (allocates). Safe to call repeatedly
        /// when wiring changes.
        void RebuildRuntimeCaches()
        {
            // Output channel refs, parallel to m_OutputChannelIDs.
            m_OutputChannelRefCache.clear();
            m_OutputChannelRefCache.reserve(m_OutputChannelIDs.size());
            for (const auto& id : m_OutputChannelIDs)
            {
                auto it = m_GraphOutputRefs.find(id);
                m_OutputChannelRefCache.push_back(it != m_GraphOutputRefs.end() ? &it->second : nullptr);
            }

            // Float cells whose ramp buffers need per-block maintenance.
            m_RampBufferCells.clear();
            for (auto& [id, cell] : m_EndpointInputStreams)
            {
                if (cell.m_RampBuffer)
                    m_RampBufferCells.push_back(&cell);
            }
            for (auto& [id, cell] : m_LocalVariables)
            {
                if (cell.m_RampBuffer)
                    m_RampBufferCells.push_back(&cell);
            }

            BuildProcessOrder();
            CompileExecutionPlan();
        }

        /// Lower the topological node order (m_ProcessOrder) to a flat array of
        /// operator handles — the "compiled execution plan" (Phase 3,
        /// docs/soundgraph-metasounds-refactor.md). Each op pairs a node's
        /// devirtualized ProcessFn (set by the factory; vtable fallback otherwise)
        /// with the node instance, so the audio-thread walk in ProcessChunk is a
        /// straight-line sweep over a contiguous vector of {fn, state} pairs — no
        /// hash lookups, no per-node vtable load. MUST run off the audio thread
        /// (allocates); rebuilt whenever the process order changes.
        void CompileExecutionPlan()
        {
            m_CompiledOps.clear();
            m_CompiledOps.reserve(m_ProcessOrder.size());
            for (auto* node : m_ProcessOrder)
                m_CompiledOps.push_back(CompiledOp{ node->m_ProcessFn, node });
        }

        /// Topologically order m_Nodes using the recorded connection edges (Kahn's
        /// algorithm, stable w.r.t. authoring order). With once-per-block node calls,
        /// a consumer running before its producer would read the *previous* block —
        /// a full block of added latency per mis-ordered hop (the per-sample walk
        /// capped that error at one sample). Cycles get a warning and keep authoring
        /// order for the nodes involved.
        void BuildProcessOrder()
        {
            const sizet count = m_Nodes.size();
            m_ProcessOrder.clear();
            m_ProcessOrder.reserve(count);

            std::unordered_map<UUID, sizet> indexOf;
            indexOf.reserve(count);
            for (sizet i = 0; i < count; ++i)
                indexOf[m_Nodes[i]->m_ID] = i;

            std::vector<std::vector<sizet>> adjacency(count);
            std::vector<u32> inDegree(count, 0);
            for (const auto& [srcID, dstID] : m_ConnectionEdges)
            {
                auto srcIt = indexOf.find(srcID);
                auto dstIt = indexOf.find(dstID);
                if (srcIt == indexOf.end() || dstIt == indexOf.end() || srcIt->second == dstIt->second)
                    continue;
                adjacency[srcIt->second].push_back(dstIt->second);
                ++inDegree[dstIt->second];
            }

            std::vector<bool> emitted(count, false);
            sizet emittedCount = 0;
            bool progress = true;
            while (emittedCount < count && progress)
            {
                progress = false;
                for (sizet i = 0; i < count; ++i)
                {
                    if (emitted[i] || inDegree[i] != 0)
                        continue;
                    emitted[i] = true;
                    ++emittedCount;
                    progress = true;
                    m_ProcessOrder.push_back(m_Nodes[i].get());
                    for (sizet dependent : adjacency[i])
                        --inDegree[dependent];
                }
            }

            if (emittedCount < count)
            {
                OLO_CORE_WARN("SoundGraph: connection graph contains a cycle; {} node(s) keep authoring order",
                              count - emittedCount);
                for (sizet i = 0; i < count; ++i)
                {
                    if (!emitted[i])
                        m_ProcessOrder.push_back(m_Nodes[i].get());
                }
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

        /// Value/event dependency edges (source node -> destination node) recorded at
        /// wire time; input to BuildProcessOrder.
        std::vector<std::pair<UUID, UUID>> m_ConnectionEdges;

        /// Node pointers in producer-before-consumer order; input to
        /// CompileExecutionPlan. Rebuilt off-thread by RebuildRuntimeCaches.
        std::vector<NodeProcessor*> m_ProcessOrder;

        /// One entry per node: a devirtualized process thunk paired with the node
        /// instance to pass it. The audio thread walks this flat vector directly.
        struct CompiledOp
        {
            NodeProcessor::ProcessFn m_Fn; // devirtualized Process thunk (or vtable fallback)
            NodeProcessor* m_State;        // node instance passed to m_Fn
        };

        /// The compiled execution plan: m_ProcessOrder lowered to flat {fn, state}
        /// pairs (Phase 3). The audio thread's per-chunk node sweep. Rebuilt
        /// off-thread by CompileExecutionPlan.
        std::vector<CompiledOp> m_CompiledOps;

        /// Cached pointers into m_GraphOutputRefs, parallel to m_OutputChannelIDs, so
        /// the per-chunk output copy does no hash lookups on the audio thread.
        std::vector<const AudioBufferRef*> m_OutputChannelRefCache;

        /// Float cells (parameters + local variables) owning ramp buffers that need
        /// per-block maintenance. Rebuilt off-thread by RebuildRuntimeCaches.
        std::vector<GraphValueCell*> m_RampBufferCells;

        /// Contiguous backing store for all node audio-output buffers (Phase 3). Sized
        /// once by AllocateNodeOutputPool (before wiring) and never resized — each
        /// node's AudioBuffer points into a fixed kMaxAudioBlockFrames slot here, so
        /// the pointers consumers capture at wire time stay valid for the graph's life.
        /// Empty for graphs that never call AllocateNodeOutputPool (buffers self-owned).
        std::vector<f32> m_NodeOutputPool;

        //==============================================================================
        /// Thread-safe Event/Message Queues
        /// Using lock-free queues with pre-allocated storage to avoid heap allocations in audio thread

        Audio::AudioEventQueue<1024> m_OutgoingEvents;
        Audio::AudioMessageQueue<1024> m_OutgoingMessages;
    };

} // namespace OloEngine::Audio::SoundGraph

#undef DECLARE_ID
#undef LOG_DBG_MESSAGES
#undef DBG
