// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphNodeSchemaCoverageTest — every factory-constructible node type has
// a NodePinSchema entry (issue #591).
//
// NodeSchema.h/.cpp is the hand-maintained editor-metadata mirror of
// SoundGraphFactory.cpp's s_NodeProcessors registry (see that file's own
// "Hardcoded palette mirroring..." comment for the same pattern used by
// SoundGraphEditorPanel.cpp's node-creation palette). Nothing enforced the two
// stay in sync, so a node type could be constructible via the factory yet
// invisible to the editor's pin display (falling back to slower
// connection-derived discovery) or the property panel. This test hardcodes
// the same type-string list the factory registers and asserts every one
// resolves a pin schema; update BOTH this list and NodeSchema.cpp when adding
// a node type to SoundGraphFactory.cpp.
//
// Param schema (NodeParamSchema) is NOT asserted here: wired-only nodes (pure
// math/trigger nodes with no editable defaults worth exposing) legitimately
// have no entry — see NodeSchema.cpp's own comment.
// =============================================================================

#include "OloEngine/Audio/SoundGraph/NodeSchema.h"

using namespace OloEngine::Audio::SoundGraph;

namespace
{
    // Mirrors SoundGraphFactory.cpp's s_NodeProcessors identifier strings exactly.
    const char* const kAllRegisteredNodeTypes[] = {
        "WavePlayer",
        "SineOscillator",
        "SquareOscillator",
        "SawtoothOscillator",
        "TriangleOscillator",
        "Noise",
        "Add<float>",
        "Subtract<float>",
        "Multiply<float>",
        "Divide<float>",
        "Min<float>",
        "Max<float>",
        "Clamp<float>",
        "MapRange<float>",
        "Power<float>",
        "Abs<float>",
        "Add<int>",
        "Subtract<int>",
        "Multiply<int>",
        "ADEnvelope",
        "ADSREnvelope",
        "RepeatTrigger",
        "TriggerCounter",
        "DelayedTrigger",
        "GetRandom<float>",
        "GetRandom<int>",
        "Get<float>",
        "Get<int>",
        "Random<float>",
        "Random<int>",
        "BPMToSeconds",
        "NoteToFrequency<float>",
        "NoteToFrequency<int>",
        "FrequencyToNote",
    };
} // namespace

TEST(SoundGraphNodeSchemaCoverage, EveryRegisteredNodeTypeHasAPinSchema)
{
    for (const char* typeName : kAllRegisteredNodeTypes)
    {
        const NodePinSchema* schema = GetNodePinSchema(typeName);
        EXPECT_NE(schema, nullptr) << "No NodePinSchema entry for node type '" << typeName
                                   << "' — add one in NodeSchema.cpp so the editor shows its pins immediately.";
        if (schema != nullptr)
        {
            EXPECT_FALSE(schema->Inputs.empty() && schema->Outputs.empty())
                << "NodePinSchema for '" << typeName << "' has no pins at all — likely a copy-paste mistake.";
        }
    }
}

TEST(SoundGraphNodeSchemaCoverage, UnknownNodeTypeReturnsNullptr)
{
    EXPECT_EQ(GetNodePinSchema("DefinitelyNotARealNodeType"), nullptr);
    EXPECT_EQ(GetNodeSchema("DefinitelyNotARealNodeType"), nullptr);
}
