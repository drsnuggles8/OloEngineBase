#include "OloEnginePCH.h"
#include "OloEngine/Core/Base.h"
#include "NodeTypes.h"
#include "ArrayNodes.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
// MUST come before INIT_ENDPOINTS_FUNCS expansions below — the
// DESCRIBE_NODE specializations have to be visible at the point each
// EndpointUtilities::RegisterEndpoints<Node> is instantiated, otherwise
// IsDescribedNode_v<Node> short-circuits and endpoint registration silently
// no-ops (which manifests as a failed wire in EstablishConnections the
// moment any node->graph connection is resolved at runtime).
#include "OloEngine/Audio/SoundGraph/Nodes/NodeDescriptions.h"

namespace OloEngine::Audio::SoundGraph
{
    /// Moving all constructors and Init() function calls for non-template nodes here
    /// to avoid recursive includes nightmare caused by trying to inline as much as possible.

    /// This macro defines the out-of-line 'void RegisterEndpoints()' declared by node
    /// processor types that need custom constructor / Init() behavior. (The old
    /// InitializeInputs step is gone — Phase 2 typed refs are born pointing at their
    /// inline defaults and re-pointed by connections, so there is no second pass.)
    /// Registration returning false means the node has no visible DESCRIBE_NODE
    /// specialization — every endpoint silently missing. Fail fast at construction
    /// instead of surfacing later as inexplicable wire failures.
#define INIT_ENDPOINTS_FUNCS(TNodeProcessor)                                                               \
    void TNodeProcessor::RegisterEndpoints()                                                               \
    {                                                                                                      \
        [[maybe_unused]] const bool registered = EndpointUtilities::RegisterEndpoints(this);               \
        OLO_CORE_ASSERT(registered, #TNodeProcessor ": endpoint registration failed — node is not"         \
                                                    " described (missing DESCRIBE_NODE specialization?)"); \
    }

    // Generator nodes that need custom behavior use INIT_ENDPOINTS_FUNCS
    INIT_ENDPOINTS_FUNCS(Noise);
    INIT_ENDPOINTS_FUNCS(SineOscillator);
    INIT_ENDPOINTS_FUNCS(SquareOscillator);
    INIT_ENDPOINTS_FUNCS(SawtoothOscillator);
    INIT_ENDPOINTS_FUNCS(TriangleOscillator);

    // WavePlayer needs custom behavior for asset loading
    INIT_ENDPOINTS_FUNCS(WavePlayer);

    // Envelope nodes need custom behavior for state machines
    INIT_ENDPOINTS_FUNCS(ADEnvelope);
    INIT_ENDPOINTS_FUNCS(ADSREnvelope);

    // Trigger nodes need custom behavior for timing control
    INIT_ENDPOINTS_FUNCS(RepeatTrigger);
    INIT_ENDPOINTS_FUNCS(TriggerCounter);
    INIT_ENDPOINTS_FUNCS(DelayedTrigger);

    // Array nodes — template member definitions require template<> syntax
#define INIT_ENDPOINTS_FUNCS_TEMPLATE(TNodeProcessor)                                                      \
    template<>                                                                                             \
    void TNodeProcessor::RegisterEndpoints()                                                               \
    {                                                                                                      \
        [[maybe_unused]] const bool registered = EndpointUtilities::RegisterEndpoints(this);               \
        OLO_CORE_ASSERT(registered, #TNodeProcessor ": endpoint registration failed — node is not"         \
                                                    " described (missing DESCRIBE_NODE specialization?)"); \
    }

    INIT_ENDPOINTS_FUNCS_TEMPLATE(Get<int>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Get<float>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Random<int>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Random<float>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(GetRandom<int>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(GetRandom<float>);

    // Math nodes — one explicit specialization per instantiation the Factory
    // creates. These USED to be inline in MathNodes.h, which made registration
    // an ODR coin-flip: TUs without NodeDescriptions.h (e.g. SoundGraphFactory.cpp)
    // instantiated a no-op body (IsDescribedNode_v == false) and the linker was
    // free to keep either copy. Out-of-line in this TU, registration always sees
    // the DESCRIBE_NODE specializations.
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Add<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Subtract<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Multiply<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Divide<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Min<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Max<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Clamp<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(MapRange<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Power<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Abs<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Add<i32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Subtract<i32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(Multiply<i32>);

    // Music nodes — same ODR rationale as the math nodes above.
    INIT_ENDPOINTS_FUNCS(BPMToSeconds);
    INIT_ENDPOINTS_FUNCS(FrequencyToNote);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(NoteToFrequency<f32>);
    INIT_ENDPOINTS_FUNCS_TEMPLATE(NoteToFrequency<i32>);

#undef INIT_ENDPOINTS_FUNCS_TEMPLATE

} // namespace OloEngine::Audio::SoundGraph
