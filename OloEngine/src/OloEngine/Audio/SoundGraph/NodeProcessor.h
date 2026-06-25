#pragma once

#include "OloEngine/Audio/AudioCallback.h"
#include "OloEngine/Audio/SoundGraph/StreamRefs.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define LOG_DBG_MESSAGES 0

//==============================================================================
/// Simple flag utilities (from Hazel's Base.h)

namespace OloEngine::Audio::SoundGraph
{
    // Use Flag utilities from OloEngine namespace
    using OloEngine::AtomicFlag;
    using OloEngine::Flag;

} // namespace OloEngine::Audio::SoundGraph

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// NodeProcessor - Base class for all sound graph nodes
    struct NodeProcessor
    {
        explicit NodeProcessor(const char* dbgName, UUID id) noexcept
            : m_DebugName(dbgName), m_ID(id)
        {
        }

        virtual ~NodeProcessor() = default;

        //==============================================================================
        /// Compiled-plan dispatch (Phase 3 — docs/soundgraph-metasounds-refactor.md)
        ///
        /// A free-function thunk that invokes a node's Process *non-virtually*.
        /// SoundGraph::CompileExecutionPlan snapshots one per node into a flat
        /// m_CompiledOps array, so the audio-thread walk calls through a stored
        /// function pointer instead of dispatching through the NodeProcessor vtable.
        /// (At block rate the dispatch cost is tiny either way — see the doc's
        /// honest-framing notes — but storing the operator handles flat is the
        /// "compiled plan" the refactor specifies and the seam Phase 4's
        /// sample-offset trigger splitting will hang off of.)
        using ProcessFn = void (*)(NodeProcessor*, u32);

        /// Default thunk: dispatches through the vtable. Directly-constructed nodes
        /// (tests, the SoundGraph container itself) keep this; the factory overwrites
        /// it with a devirtualized ProcessThunk<T> when the concrete type is known.
        static void VtableProcessThunk(NodeProcessor* node, u32 numFrames)
        {
            node->Process(numFrames);
        }

        /// Operator handle used by the compiled plan. Set at construction (vtable
        /// fallback) and patched by Factory::MakeNode<T> to the concrete thunk.
        ProcessFn m_ProcessFn = &VtableProcessThunk;

        // Explicitly delete copy and move operations to prevent dangling pointers
        // (typed connection refs capture raw pointers into this object's members).
        NodeProcessor(const NodeProcessor&) = delete;
        NodeProcessor& operator=(const NodeProcessor&) = delete;
        NodeProcessor(NodeProcessor&&) = delete;
        NodeProcessor& operator=(NodeProcessor&&) = delete;

        std::string m_DebugName;
        UUID m_ID;

      protected:
        f32 m_SampleRate = 48000.0f;

      public:
        //==============================================================================
        /// Base Input/Output structures

        struct Input
        {
            Input() = delete;
            explicit Input(NodeProcessor& owner) noexcept : m_Node(&owner) {}
            NodeProcessor* m_Node = nullptr;
        };

        struct Output
        {
            Output() = delete;
            explicit Output(NodeProcessor& owner) noexcept : m_Node(&owner) {}
            NodeProcessor* m_Node = nullptr;
        };

        //==============================================================================
        /// Event handling system

        struct InputEvent : public Input
        {
            using EventFunction = std::function<void(f32)>;
            // Phase 4: sample-accurate handler — receives the trigger's frame
            // offset within the block (docs/soundgraph-metasounds-refactor.md).
            // Trigger-consuming nodes register one of these so they can Fire their
            // TriggerRef at the exact offset; value-only handlers ignore the offset.
            using OffsetEventFunction = std::function<void(f32, i32)>;

            explicit InputEvent(NodeProcessor& owner, EventFunction ev) noexcept
                : Input(owner), m_Event(std::move(ev))
            {
            }
            explicit InputEvent(NodeProcessor& owner, OffsetEventFunction ev) noexcept
                : Input(owner), m_OffsetEvent(std::move(ev))
            {
            }

            // sampleOffset defaults to 0 ("at the start of the block") so legacy
            // single-argument callers and routes keep their block-boundary timing.
            inline virtual void operator()(f32 value, i32 sampleOffset = 0) noexcept
            {
                OLO_PROFILE_FUNCTION();

                if (m_OffsetEvent)
                    m_OffsetEvent(value, sampleOffset);
                else if (m_Event)
                    m_Event(value);
            }

            // Exactly one is bound (the active ctor picks). m_OffsetEvent takes
            // precedence in operator() when set.
            EventFunction m_Event;
            OffsetEventFunction m_OffsetEvent;
        };

        struct OutputEvent : public Output
        {
            explicit OutputEvent(NodeProcessor& owner) noexcept
                : Output(owner)
            {
            }

            // sampleOffset (Phase 4) is forwarded to every destination so a node
            // firing a trigger mid-block (at frame `sampleOffset`) lets downstream
            // trigger consumers act at that exact frame. Defaults to 0 so existing
            // single-argument fires keep their block-boundary timing.
            inline void operator()(f32 value, i32 sampleOffset = 0) noexcept
            {
                OLO_PROFILE_FUNCTION();

                // Iterate through weak_ptr connections, cleaning up expired ones
                auto it = m_DestinationEvents.begin();
                while (it != m_DestinationEvents.end())
                {
                    if (auto dest = it->lock()) // Check if still valid
                    {
                        (*dest)(value, sampleOffset);
                        ++it;
                    }
                    else
                    {
                        it = m_DestinationEvents.erase(it);
                    }
                }
            }

            void AddDestination(std::shared_ptr<InputEvent> dest)
            {
                OLO_PROFILE_FUNCTION();

                if (dest)
                    m_DestinationEvents.push_back(std::weak_ptr<InputEvent>(dest));
            }

            // Safe connection management using weak_ptr to prevent dangling pointers
            // InputEvent instances are now owned by shared_ptr in InEvents
            std::vector<std::weak_ptr<InputEvent>> m_DestinationEvents;
        };

        //==============================================================================
        /// Event endpoint management

        InputEvent& AddInEvent(Identifier id, InputEvent::EventFunction function = nullptr)
        {
            OLO_PROFILE_FUNCTION();

            auto inputEvent = std::make_shared<InputEvent>(*this, std::move(function));
            const auto& [element, inserted] = InEvents.try_emplace(id, inputEvent);
            OLO_CORE_ASSERT(inserted, "Input event with this ID already exists");
            return *element->second;
        }

        // Phase 4 overload: registers a sample-offset-aware handler (a lambda taking
        // (f32 value, i32 sampleOffset)). Distinct from the value-only overload by
        // the handler's arity, so existing AddInEvent(id, [](f32){...}) call sites
        // keep resolving to the legacy overload above.
        InputEvent& AddInEvent(Identifier id, InputEvent::OffsetEventFunction function)
        {
            OLO_PROFILE_FUNCTION();

            auto inputEvent = std::make_shared<InputEvent>(*this, std::move(function));
            const auto& [element, inserted] = InEvents.try_emplace(id, inputEvent);
            OLO_CORE_ASSERT(inserted, "Input event with this ID already exists");
            return *element->second;
        }

        void AddOutEvent(Identifier id, OutputEvent& out)
        {
            OLO_PROFILE_FUNCTION();

            const auto& [element, inserted] = OutEvents.insert({ id, std::ref(out) });
            OLO_CORE_ASSERT(inserted, "Output event with this ID already exists");
        }

        //==============================================================================
        /// Typed stream endpoints (Phase 2 — docs/soundgraph-metasounds-refactor.md)
        ///
        /// Inputs register a pointer to the node's ref member (AudioBufferRef /
        /// ValueRef<T>); outputs register a pointer to the node's output storage
        /// (AudioBuffer / scalar member). Wiring patches the consumer's ref to point
        /// at the producer's storage — these maps are touched at construction and
        /// wire time only, never on the audio thread.

        struct InputStreamRef
        {
            EStreamType m_Type;
            void* m_Ref = nullptr; // AudioBufferRef* or ValueRef<T>* matching m_Type
        };

        struct OutputStreamSource
        {
            EStreamType m_Type;
            const void* m_Source = nullptr; // AudioBuffer* or const T* matching m_Type
        };

        std::unordered_map<Identifier, InputStreamRef> InputRefs;
        std::unordered_map<Identifier, OutputStreamSource> OutputSources;

        void AddAudioInRef(Identifier id, AudioBufferRef& ref)
        {
            [[maybe_unused]] const auto [it, inserted] =
                InputRefs.try_emplace(id, InputStreamRef{ EStreamType::Audio, &ref });
            OLO_CORE_ASSERT(inserted, "Input stream with this ID already exists");
        }

        template<StreamValue T>
        void AddValueInRef(Identifier id, ValueRef<T>& ref)
        {
            [[maybe_unused]] const auto [it, inserted] =
                InputRefs.try_emplace(id, InputStreamRef{ StreamTypeFor<T>, &ref });
            OLO_CORE_ASSERT(inserted, "Input stream with this ID already exists");
        }

        void AddAudioOutSource(Identifier id, const AudioBuffer& buffer)
        {
            [[maybe_unused]] const auto [it, inserted] =
                OutputSources.try_emplace(id, OutputStreamSource{ EStreamType::Audio, &buffer });
            OLO_CORE_ASSERT(inserted, "Output stream with this ID already exists");
        }

        template<StreamValue T>
        void AddValueOutSource(Identifier id, const T& member)
        {
            [[maybe_unused]] const auto [it, inserted] =
                OutputSources.try_emplace(id, OutputStreamSource{ StreamTypeFor<T>, &member });
            OLO_CORE_ASSERT(inserted, "Output stream with this ID already exists");
        }

        /// Patch a consumer input ref to read from a producer output. Returns false
        /// (with a warning) when the types are incompatible — replaces the silent
        /// mis-wiring the old ValueView re-aliasing allowed.
        ///
        /// Compatibility matrix:
        ///   Audio in  ← Audio out (per-frame) | Float out (scalar broadcast)
        ///   Float in  ← Float out             | Audio out (frame 0 of the block)
        ///   Int32/Int64/Bool in ← same-type out only
        static bool BindInputToOutput(InputStreamRef& dst, const OutputStreamSource& src)
        {
            switch (dst.m_Type)
            {
                case EStreamType::Audio:
                {
                    auto& ref = *static_cast<AudioBufferRef*>(dst.m_Ref);
                    if (src.m_Type == EStreamType::Audio)
                    {
                        ref.BindBuffer(static_cast<const AudioBuffer*>(src.m_Source)->Data());
                        return true;
                    }
                    if (src.m_Type == EStreamType::Float)
                    {
                        ref.BindScalar(static_cast<const f32*>(src.m_Source));
                        return true;
                    }
                    return false;
                }
                case EStreamType::Float:
                {
                    auto& ref = *static_cast<FloatRef*>(dst.m_Ref);
                    if (src.m_Type == EStreamType::Float)
                    {
                        ref.Bind(static_cast<const f32*>(src.m_Source));
                        return true;
                    }
                    if (src.m_Type == EStreamType::Audio)
                    {
                        // Control consumer of an audio producer: sample frame 0 of the
                        // block. AudioBuffer::Data() is stable (fixed capacity).
                        ref.Bind(static_cast<const AudioBuffer*>(src.m_Source)->Data());
                        return true;
                    }
                    return false;
                }
                case EStreamType::Int32:
                {
                    if (src.m_Type != EStreamType::Int32)
                        return false;
                    static_cast<IntRef*>(dst.m_Ref)->Bind(static_cast<const i32*>(src.m_Source));
                    return true;
                }
                case EStreamType::Int64:
                {
                    if (src.m_Type != EStreamType::Int64)
                        return false;
                    static_cast<Int64Ref*>(dst.m_Ref)->Bind(static_cast<const i64*>(src.m_Source));
                    return true;
                }
                case EStreamType::Bool:
                {
                    if (src.m_Type != EStreamType::Bool)
                        return false;
                    static_cast<BoolRef*>(dst.m_Ref)->Bind(static_cast<const bool*>(src.m_Source));
                    return true;
                }
            }
            return false;
        }

        /// Wire `srcNode.srcID` (output) into `dstNode.dstID` (input). Endpoint
        /// resolution + type check happen here, once, at wire time.
        static bool ConnectStreams(NodeProcessor& srcNode, Identifier srcID,
                                   NodeProcessor& dstNode, Identifier dstID)
        {
            auto srcIt = srcNode.OutputSources.find(srcID);
            if (srcIt == srcNode.OutputSources.end())
            {
                OLO_CORE_WARN("ConnectStreams: source node '{}' has no output stream {}",
                              srcNode.m_DebugName, static_cast<u32>(srcID));
                return false;
            }

            auto dstIt = dstNode.InputRefs.find(dstID);
            if (dstIt == dstNode.InputRefs.end())
            {
                OLO_CORE_WARN("ConnectStreams: destination node '{}' has no input stream {}",
                              dstNode.m_DebugName, static_cast<u32>(dstID));
                return false;
            }

            if (!BindInputToOutput(dstIt->second, srcIt->second))
            {
                OLO_CORE_WARN("ConnectStreams: incompatible stream types {} -> {} ('{}' -> '{}')",
                              static_cast<i32>(srcIt->second.m_Type), static_cast<i32>(dstIt->second.m_Type),
                              srcNode.m_DebugName, dstNode.m_DebugName);
                return false;
            }
            return true;
        }

        /// Write an asset-authored default into an input's inline default cell.
        /// Numeric values are converted to the endpoint's type (the editor stores
        /// e.g. Int plugs for Int endpoints, but a hand-edited asset shouldn't
        /// silently zero a parameter over a float-vs-int mismatch). Returns false
        /// if no input stream with this ID exists.
        bool SetInputDefault(Identifier id, const choc::value::Value& value)
        {
            auto it = InputRefs.find(id);
            if (it == InputRefs.end())
                return false;

            const auto& type = value.getType();

            // Saturating f64 -> i64: casting a NaN/Inf or out-of-range double to an
            // integer is undefined behavior, so clamp into representable range first.
            // (i64 max is not exactly representable as f64; the cast below relies on
            // the comparison bounds being the nearest representable doubles.)
            auto toSaturatedInt = [](f64 v) -> i64
            {
                constexpr f64 kMin = static_cast<f64>(std::numeric_limits<i64>::min());
                constexpr f64 kMax = static_cast<f64>(std::numeric_limits<i64>::max());
                if (v <= kMin)
                    return std::numeric_limits<i64>::min();
                if (v >= kMax)
                    return std::numeric_limits<i64>::max();
                return static_cast<i64>(v);
            };

            f64 numeric = 0.0;
            i64 integer = 0;
            bool boolean = false;
            if (type.isFloat32())
            {
                numeric = static_cast<f64>(value.getFloat32());
                if (!std::isfinite(numeric))
                    return false; // reject NaN/Inf — keep the compiled-in default
                integer = toSaturatedInt(numeric);
            }
            else if (type.isFloat64())
            {
                numeric = value.getFloat64();
                if (!std::isfinite(numeric))
                    return false; // reject NaN/Inf — keep the compiled-in default
                integer = toSaturatedInt(numeric);
            }
            else if (type.isInt32())
            {
                integer = static_cast<i64>(value.getInt32());
                numeric = static_cast<f64>(integer);
            }
            else if (type.isInt64())
            {
                integer = value.getInt64();
                numeric = static_cast<f64>(integer);
            }
            else if (type.isBool())
            {
                boolean = value.getBool();
                integer = boolean ? 1 : 0;
                numeric = static_cast<f64>(integer);
            }
            else
            {
                // Non-primitive plug (unsupported) — keep the compiled-in default.
                return false;
            }
            if (!type.isBool())
                boolean = (integer != 0) || (numeric != 0.0);

            switch (it->second.m_Type)
            {
                case EStreamType::Audio:
                    static_cast<AudioBufferRef*>(it->second.m_Ref)->SetDefault(static_cast<f32>(numeric));
                    return true;
                case EStreamType::Float:
                    static_cast<FloatRef*>(it->second.m_Ref)->SetDefault(static_cast<f32>(numeric));
                    return true;
                case EStreamType::Int32:
                    static_cast<IntRef*>(it->second.m_Ref)->SetDefault(static_cast<i32>(integer));
                    return true;
                case EStreamType::Int64:
                    static_cast<Int64Ref*>(it->second.m_Ref)->SetDefault(integer);
                    return true;
                case EStreamType::Bool:
                    static_cast<BoolRef*>(it->second.m_Ref)->SetDefault(boolean);
                    return true;
            }
            return false;
        }

        //==============================================================================
        /// Virtual interface - must be implemented by derived nodes

        virtual void SetSampleRate(f32 sampleRate)
        {
            m_SampleRate = sampleRate;
        }
        virtual void Init() {}
        // Block-rate process entry. Called once per block (Phase 2); derived nodes
        // produce `numFrames` samples per call. Audio-rate outputs write into their
        // AudioBuffer members; control-rate outputs update their scalar members once
        // per block. numFrames is guaranteed <= kMaxAudioBlockFrames by SoundGraph.
        virtual void Process(u32 numFrames)
        {
            (void)numFrames;
        }

        /// Large heap buffers this node owns beyond its own sizeof — e.g. a
        /// WavePlayer's decoded audio samples. Base nodes own none; a node type
        /// with a significant buffer overrides this so SoundGraphCache memory
        /// accounting stays type-agnostic (no per-type dynamic_cast) and counts
        /// any future buffer-owning node automatically.
        [[nodiscard]] virtual sizet GetHeapBytes() const
        {
            return 0;
        }

        //==============================================================================
        /// Endpoint access

        std::unordered_map<Identifier, std::shared_ptr<InputEvent>> InEvents;
        std::unordered_map<Identifier, std::reference_wrapper<OutputEvent>> OutEvents;

        //==============================================================================
        /// Convenience accessors

        inline InputEvent& InEvent(const Identifier& id)
        {
            return *InEvents.at(id);
        }
        inline OutputEvent& OutEvent(const Identifier& id)
        {
            return OutEvents.at(id).get();
        }

        // Additional helper methods for SoundGraph integration
        inline std::shared_ptr<InputEvent> GetInputEvent(const Identifier& id)
        {
            auto it = InEvents.find(id);
            return it != InEvents.end() ? it->second : nullptr;
        }

        inline OutputEvent* GetOutputEvent(const Identifier& id)
        {
            auto it = OutEvents.find(id);
            return it != OutEvents.end() ? &(it->second.get()) : nullptr;
        }

        inline const char* GetDisplayName() const
        {
            return m_DebugName.c_str();
        }
    };

    /// Devirtualized process thunk for a concrete node type. The `T::` qualification
    /// statically binds the call (no vtable load). Factory::MakeNode<T> assigns
    /// &ProcessThunk<T> to the node's m_ProcessFn so the compiled plan can invoke it
    /// through a plain function pointer. T must derive from NodeProcessor.
    template<typename T>
    inline void ProcessThunk(NodeProcessor* node, u32 numFrames)
    {
        static_cast<T*>(node)->T::Process(numFrames);
    }

} // namespace OloEngine::Audio::SoundGraph

#undef LOG_DBG_MESSAGES
#undef DBG
