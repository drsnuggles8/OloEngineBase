#include "OloEnginePCH.h"
#include "SoundGraphFactory.h"

#include "Nodes/WavePlayer.h"
#include "Nodes/MathNodes.h"
#include "Nodes/GeneratorNodes.h"
#include "Nodes/EnvelopeNodes.h"
#include "Nodes/TriggerNodes.h"
#include "Nodes/ArrayNodes.h"
#include "Nodes/MusicNodes.h"

#include <memory>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    using Registry = std::unordered_map<Identifier, std::function<std::unique_ptr<NodeProcessor>(UUID nodeID)>>;

    // Factory registration for all node types
    static const Registry s_NodeProcessors
    {
        // Wave player node
        { Identifier("WavePlayer"), [](UUID nodeID) { return std::make_unique<WavePlayer>("WavePlayer", nodeID); } },
        
        // Generator nodes
        { Identifier("SineOscillator"), [](UUID nodeID) { return std::make_unique<SineOscillator>("SineOscillator", nodeID); } },
        { Identifier("SquareOscillator"), [](UUID nodeID) { return std::make_unique<SquareOscillator>("SquareOscillator", nodeID); } },
        { Identifier("SawtoothOscillator"), [](UUID nodeID) { return std::make_unique<SawtoothOscillator>("SawtoothOscillator", nodeID); } },
        { Identifier("TriangleOscillator"), [](UUID nodeID) { return std::make_unique<TriangleOscillator>("TriangleOscillator", nodeID); } },
        { Identifier("Noise"), [](UUID nodeID) { return std::make_unique<Noise>("Noise", nodeID); } },
        
        // Math nodes (float)
        { Identifier("Add<float>"), [](UUID nodeID) { return std::make_unique<Add<float>>("Add<float>", nodeID); } },
        { Identifier("Subtract<float>"), [](UUID nodeID) { return std::make_unique<Subtract<float>>("Subtract<float>", nodeID); } },
        { Identifier("Multiply<float>"), [](UUID nodeID) { return std::make_unique<Multiply<float>>("Multiply<float>", nodeID); } },
        { Identifier("Divide<float>"), [](UUID nodeID) { return std::make_unique<Divide<float>>("Divide<float>", nodeID); } },
        { Identifier("Min<float>"), [](UUID nodeID) { return std::make_unique<Min<float>>("Min<float>", nodeID); } },
        { Identifier("Max<float>"), [](UUID nodeID) { return std::make_unique<Max<float>>("Max<float>", nodeID); } },
        { Identifier("Clamp<float>"), [](UUID nodeID) { return std::make_unique<Clamp<float>>("Clamp<float>", nodeID); } },
        { Identifier("MapRange<float>"), [](UUID nodeID) { return std::make_unique<MapRange<float>>("MapRange<float>", nodeID); } },
        { Identifier("Power<float>"), [](UUID nodeID) { return std::make_unique<Power<float>>("Power<float>", nodeID); } },
        { Identifier("Abs<float>"), [](UUID nodeID) { return std::make_unique<Abs<float>>("Abs<float>", nodeID); } },
        
        // Math nodes (int)
        { Identifier("Add<int>"), [](UUID nodeID) { return std::make_unique<Add<int>>("Add<int>", nodeID); } },
        { Identifier("Subtract<int>"), [](UUID nodeID) { return std::make_unique<Subtract<int>>("Subtract<int>", nodeID); } },
        { Identifier("Multiply<int>"), [](UUID nodeID) { return std::make_unique<Multiply<int>>("Multiply<int>", nodeID); } },
        
        // Envelope nodes
        { Identifier("ADEnvelope"), [](UUID nodeID) { return std::make_unique<ADEnvelope>("ADEnvelope", nodeID); } },
        { Identifier("ADSREnvelope"), [](UUID nodeID) { return std::make_unique<ADSREnvelope>("ADSREnvelope", nodeID); } },
        
        // Trigger nodes
        { Identifier("RepeatTrigger"), [](UUID nodeID) { return std::make_unique<RepeatTrigger>("RepeatTrigger", nodeID); } },
        { Identifier("TriggerCounter"), [](UUID nodeID) { return std::make_unique<TriggerCounter>("TriggerCounter", nodeID); } },
        { Identifier("DelayedTrigger"), [](UUID nodeID) { return std::make_unique<DelayedTrigger>("DelayedTrigger", nodeID); } },
        
        // Array operation nodes
        { Identifier("GetRandom<float>"), [](UUID nodeID) { return std::make_unique<GetRandom<float>>("GetRandom<float>", nodeID); } },
        { Identifier("GetRandom<int>"), [](UUID nodeID) { return std::make_unique<GetRandom<int>>("GetRandom<int>", nodeID); } },
        { Identifier("Get<float>"), [](UUID nodeID) { return std::make_unique<Get<float>>("Get<float>", nodeID); } },
        { Identifier("Get<int>"), [](UUID nodeID) { return std::make_unique<Get<int>>("Get<int>", nodeID); } },
        { Identifier("Random<float>"), [](UUID nodeID) { return std::make_unique<Random<float>>("Random<float>", nodeID); } },
        { Identifier("Random<int>"), [](UUID nodeID) { return std::make_unique<Random<int>>("Random<int>", nodeID); } },
        
        // Music utility nodes
        { Identifier("BPMToSeconds"), [](UUID nodeID) { return std::make_unique<BPMToSeconds>("BPMToSeconds", nodeID); } },
        { Identifier("NoteToFrequency<float>"), [](UUID nodeID) { return std::make_unique<NoteToFrequency<float>>("NoteToFrequency<float>", nodeID); } },
        { Identifier("NoteToFrequency<int>"), [](UUID nodeID) { return std::make_unique<NoteToFrequency<int>>("NoteToFrequency<int>", nodeID); } },
        { Identifier("FrequencyToNote"), [](UUID nodeID) { return std::make_unique<FrequencyToNote>("FrequencyToNote", nodeID); } },
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
        return s_NodeProcessors.count(nodeTypeID);
    }

} // namespace OloEngine::Audio::SoundGraph