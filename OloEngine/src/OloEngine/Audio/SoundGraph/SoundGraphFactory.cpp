#include "OloEnginePCH.h"
#include "SoundGraphFactory.h"

#include "Nodes/WavePlayer.h"
#include "Nodes/MathNodes.h"
#include "Nodes/GeneratorNodes.h"
#include "Nodes/EnvelopeNodes.h"
#include "Nodes/TriggerNodes.h"
#include "Nodes/ArrayNodes.h"
#include "Nodes/MusicNodes.h"

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// Construct a concrete node and patch its compiled-plan operator handle to the
    /// devirtualized ProcessThunk<T> (Phase 3). Every factory-created node therefore
    /// dispatches through a stored function pointer in the compiled plan; nodes built
    /// outside the factory (tests) keep the vtable fallback set in NodeProcessor.
    template<typename T>
    static std::unique_ptr<T> MakeNode(const char* dbgName, UUID nodeID)
    {
        auto node = std::make_unique<T>(dbgName, nodeID);
        node->m_ProcessFn = &ProcessThunk<T>;
        return node;
    }

    using Registry = std::unordered_map<Identifier, std::function<std::unique_ptr<NodeProcessor>(UUID nodeID)>>;

    // Factory registration for all node types
    static const Registry s_NodeProcessors{
        // Wave player node
        { Identifier("WavePlayer"), [](UUID nodeID)
          { return MakeNode<WavePlayer>("WavePlayer", nodeID); } },

        // Generator nodes
        { Identifier("SineOscillator"), [](UUID nodeID)
          { return MakeNode<SineOscillator>("SineOscillator", nodeID); } },
        { Identifier("SquareOscillator"), [](UUID nodeID)
          { return MakeNode<SquareOscillator>("SquareOscillator", nodeID); } },
        { Identifier("SawtoothOscillator"), [](UUID nodeID)
          { return MakeNode<SawtoothOscillator>("SawtoothOscillator", nodeID); } },
        { Identifier("TriangleOscillator"), [](UUID nodeID)
          { return MakeNode<TriangleOscillator>("TriangleOscillator", nodeID); } },
        { Identifier("Noise"), [](UUID nodeID)
          { return MakeNode<Noise>("Noise", nodeID); } },

        // Math nodes (float)
        { Identifier("Add<float>"), [](UUID nodeID)
          { return MakeNode<Add<float>>("Add<float>", nodeID); } },
        { Identifier("Subtract<float>"), [](UUID nodeID)
          { return MakeNode<Subtract<float>>("Subtract<float>", nodeID); } },
        { Identifier("Multiply<float>"), [](UUID nodeID)
          { return MakeNode<Multiply<float>>("Multiply<float>", nodeID); } },
        { Identifier("Divide<float>"), [](UUID nodeID)
          { return MakeNode<Divide<float>>("Divide<float>", nodeID); } },
        { Identifier("Min<float>"), [](UUID nodeID)
          { return MakeNode<Min<float>>("Min<float>", nodeID); } },
        { Identifier("Max<float>"), [](UUID nodeID)
          { return MakeNode<Max<float>>("Max<float>", nodeID); } },
        { Identifier("Clamp<float>"), [](UUID nodeID)
          { return MakeNode<Clamp<float>>("Clamp<float>", nodeID); } },
        { Identifier("MapRange<float>"), [](UUID nodeID)
          { return MakeNode<MapRange<float>>("MapRange<float>", nodeID); } },
        { Identifier("Power<float>"), [](UUID nodeID)
          { return MakeNode<Power<float>>("Power<float>", nodeID); } },
        { Identifier("Abs<float>"), [](UUID nodeID)
          { return MakeNode<Abs<float>>("Abs<float>", nodeID); } },

        // Math nodes (int)
        { Identifier("Add<int>"), [](UUID nodeID)
          { return MakeNode<Add<int>>("Add<int>", nodeID); } },
        { Identifier("Subtract<int>"), [](UUID nodeID)
          { return MakeNode<Subtract<int>>("Subtract<int>", nodeID); } },
        { Identifier("Multiply<int>"), [](UUID nodeID)
          { return MakeNode<Multiply<int>>("Multiply<int>", nodeID); } },

        // Envelope nodes
        { Identifier("ADEnvelope"), [](UUID nodeID)
          { return MakeNode<ADEnvelope>("ADEnvelope", nodeID); } },
        { Identifier("ADSREnvelope"), [](UUID nodeID)
          { return MakeNode<ADSREnvelope>("ADSREnvelope", nodeID); } },

        // Trigger nodes
        { Identifier("RepeatTrigger"), [](UUID nodeID)
          { return MakeNode<RepeatTrigger>("RepeatTrigger", nodeID); } },
        { Identifier("TriggerCounter"), [](UUID nodeID)
          { return MakeNode<TriggerCounter>("TriggerCounter", nodeID); } },
        { Identifier("DelayedTrigger"), [](UUID nodeID)
          { return MakeNode<DelayedTrigger>("DelayedTrigger", nodeID); } },

        // Array operation nodes
        { Identifier("GetRandom<float>"), [](UUID nodeID)
          { return MakeNode<GetRandom<float>>("GetRandom<float>", nodeID); } },
        { Identifier("GetRandom<int>"), [](UUID nodeID)
          { return MakeNode<GetRandom<int>>("GetRandom<int>", nodeID); } },
        { Identifier("Get<float>"), [](UUID nodeID)
          { return MakeNode<Get<float>>("Get<float>", nodeID); } },
        { Identifier("Get<int>"), [](UUID nodeID)
          { return MakeNode<Get<int>>("Get<int>", nodeID); } },
        { Identifier("Random<float>"), [](UUID nodeID)
          { return MakeNode<Random<float>>("Random<float>", nodeID); } },
        { Identifier("Random<int>"), [](UUID nodeID)
          { return MakeNode<Random<int>>("Random<int>", nodeID); } },

        // Music utility nodes
        { Identifier("BPMToSeconds"), [](UUID nodeID)
          { return MakeNode<BPMToSeconds>("BPMToSeconds", nodeID); } },
        { Identifier("NoteToFrequency<float>"), [](UUID nodeID)
          { return MakeNode<NoteToFrequency<float>>("NoteToFrequency<float>", nodeID); } },
        { Identifier("NoteToFrequency<int>"), [](UUID nodeID)
          { return MakeNode<NoteToFrequency<int>>("NoteToFrequency<int>", nodeID); } },
        { Identifier("FrequencyToNote"), [](UUID nodeID)
          { return MakeNode<FrequencyToNote>("FrequencyToNote", nodeID); } },
    };

    //==============================================================================
    std::unique_ptr<NodeProcessor> Factory::Create(Identifier nodeTypeID, UUID nodeID)
    {
        OLO_PROFILE_FUNCTION();

        auto it = s_NodeProcessors.find(nodeTypeID);
        if (it == s_NodeProcessors.end())
        {
            OLO_CORE_ERROR("SoundGraph::Factory::Create - Node with type ID is not in the registry");
            return nullptr;
        }

        return it->second(nodeID);
    }

    bool Factory::Contains(Identifier nodeTypeID)
    {
        OLO_PROFILE_FUNCTION();
        return s_NodeProcessors.contains(nodeTypeID);
    }

} // namespace OloEngine::Audio::SoundGraph
