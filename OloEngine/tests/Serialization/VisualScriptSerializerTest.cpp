#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// VisualScriptSerializerTest — the `.olovs` round-trip contract (issue #634,
// AC#1: "save -> load -> save produces an identical file").
//
// Byte identity is the assertion, not "the values look the same", because the
// two ways this silently breaks both survive a value comparison:
//   * a std::unordered_map somewhere in the graph, which reorders between runs;
//   * a float written at fixed precision, which loses the last digit and then
//     converges to a DIFFERENT number on the third save.
// Both were designed against (std::map everywhere, std::to_chars shortest
// round-trip) and both are only actually pinned by comparing the text.
//
// Modelled on SoundGraphSerializerTest: the serializer is a standalone static
// class, so this needs no AssetManager and no project mount.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSerializer.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptVM.h"
#include "TestTempDir.h"

#include <filesystem>
#include <string>

using namespace OloEngine;
using namespace OloEngine::VisualScript;

namespace
{
    /// A graph that touches every persisted feature: variables of several pin
    /// types, node properties, per-pin literal defaults, exec and data links, a
    /// sub-graph function with its own signature, and an awkward float.
    Ref<VisualScriptAsset> BuildRichAsset()
    {
        auto asset = Ref<VisualScriptAsset>::Create();
        asset->m_NodeBudgetPerTick = 4242;

        asset->m_Variables.push_back({ "Count", PinType::Int, PinValue::MakeInt(7) });
        asset->m_Variables.push_back({ "Speed", PinType::Float, PinValue::MakeFloat(0.1f) });
        asset->m_Variables.push_back({ "Label", PinType::String, PinValue::MakeString("hello world") });
        asset->m_Variables.push_back({ "Home", PinType::Vec3, PinValue::MakeVec3({ 1.25f, -3.5f, 0.0f }) });
        asset->m_Variables.push_back({ "Target", PinType::Entity, PinValue::MakeEntity(UUID(1234567890123456789ull)) });

        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId begin = graph.AddNode(std::string(NodeTypes::kOnBeginPlay), { 10.5f, -20.25f }).m_Id;
        const NodeId branch = graph.AddNode("Flow.Branch", { 200.0f, 0.0f }).m_Id;
        // 0.1f is not exactly representable in binary — a fixed-precision writer
        // loses it, which is exactly what the byte-identity check catches.
        graph.FindNode(branch)->m_PinDefaults["Condition"] = PinValue::MakeBool(true);
        const NodeId print = graph.AddNode("Utility.Print", { 400.0f, -50.0f }).m_Id;
        graph.FindNode(print)->m_PinDefaults["Message"] = PinValue::MakeString("started");
        const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable), { 400.0f, 50.0f }).m_Id;
        graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Speed");
        graph.FindNode(set)->m_PinDefaults["Value"] = PinValue::MakeFloat(0.30000001192092896f);
        const NodeId call = graph.AddNode(std::string(NodeTypes::kFunctionCall), { 600.0f, 0.0f }).m_Id;
        graph.FindNode(call)->SetProperty(std::string(NodeProps::kFunctionName), "Double");
        graph.FindNode(call)->m_PinDefaults["In"] = PinValue::MakeFloat(21.0f);

        graph.AddLink(begin, "Then", branch, "Enter");
        graph.AddLink(branch, "True", print, "Enter");
        graph.AddLink(branch, "False", set, "Enter");
        graph.AddLink(print, "Then", call, "Enter");

        VisualScriptGraph& function = asset->m_Functions.emplace_back();
        function.m_Name = "Double";
        function.m_Inputs.push_back({ "In", PinType::Float, PinValue::MakeFloat(0.0f) });
        function.m_Outputs.push_back({ "Out", PinType::Float, PinValue::MakeFloat(0.0f) });
        const NodeId entry = function.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
        const NodeId multiply = function.AddNode("Math.Multiply", { 150.0f, 0.0f }).m_Id;
        function.FindNode(multiply)->m_PinDefaults["B"] = PinValue::MakeFloat(2.0f);
        const NodeId ret = function.AddNode(std::string(NodeTypes::kFunctionReturn), { 300.0f, 0.0f }).m_Id;
        function.AddLink(entry, "Then", ret, "Enter");
        function.AddLink(entry, "In", multiply, "A");
        function.AddLink(multiply, "Result", ret, "Out");

        return asset;
    }
} // namespace

class VisualScriptSerializerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        NodeRegistry::EnsureStandardLibrary();
    }
};

TEST_F(VisualScriptSerializerTest, RoundTripThroughStringIsByteIdentical)
{
    const auto original = BuildRichAsset();
    const std::string first = VisualScriptSerializer::SerializeToString(*original);
    ASSERT_FALSE(first.empty());

    auto reloaded = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*reloaded, first));

    const std::string second = VisualScriptSerializer::SerializeToString(*reloaded);
    EXPECT_EQ(first, second)
        << "save -> load -> save must be byte-identical. A difference here means either an "
           "unordered container leaked into the graph (reorders between runs) or a float lost "
           "precision on the way out (converges to a different value on the third save).";

    // And a third pass, because a lossy float typically converges rather than
    // oscillating: two saves can agree while both differ from the original.
    auto third = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*third, second));
    EXPECT_EQ(second, VisualScriptSerializer::SerializeToString(*third));
}

TEST_F(VisualScriptSerializerTest, RoundTripPreservesEveryValue)
{
    const auto original = BuildRichAsset();
    auto reloaded = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*reloaded, VisualScriptSerializer::SerializeToString(*original)));

    EXPECT_TRUE(original->EqualsForTest(*reloaded)) << "the deep value comparison must also hold, not just the text";
    EXPECT_EQ(reloaded->m_NodeBudgetPerTick, 4242u);
    ASSERT_EQ(reloaded->m_Variables.size(), original->m_Variables.size());
    EXPECT_EQ(static_cast<u64>(reloaded->FindVariable("Target")->m_DefaultValue.AsEntity()), 1234567890123456789ull)
        << "a full-range u64 entity id must survive; this is the value class that a double round trip would round";
    ASSERT_EQ(reloaded->m_Functions.size(), 1u);
    EXPECT_EQ(reloaded->m_Functions[0].m_Name, "Double");
    EXPECT_EQ(reloaded->m_Functions[0].m_Inputs.size(), 1u);
    EXPECT_EQ(reloaded->m_Functions[0].m_Outputs.size(), 1u);
}

TEST_F(VisualScriptSerializerTest, RoundTrippedGraphStillCompilesAndRuns)
{
    // A file that reloads into the right values but no longer compiles is a
    // real failure mode: link endpoints are stored by pin NAME, so a rename in
    // the serializer would pass the value check and break here.
    const auto original = BuildRichAsset();
    auto reloaded = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*reloaded, VisualScriptSerializer::SerializeToString(*original)));

    std::vector<CompileDiagnostic> errors;
    Ref<VisualScriptPlan> plan = VisualScriptPlan::Compile(*reloaded, errors);
    ASSERT_TRUE(plan) << (errors.empty() ? "compile failed with no diagnostics" : errors[0].m_Message);

    std::vector<std::string> log;
    std::vector<EmittedEvent> outbox;
    u64 random = 1;
    RuntimeContext runtime;
    runtime.m_DeltaTime = 1.0f / 60.0f;
    runtime.m_LogSink = &log;
    runtime.m_EventOutbox = &outbox;
    runtime.m_RandomState = &random;

    VisualScriptInstance instance(plan, UUID(1));
    instance.BeginPlay(runtime);
    ASSERT_EQ(log.size(), 1u) << "the reloaded graph's Branch(true) -> Print must still run";
    EXPECT_EQ(log[0], "started");
}

TEST_F(VisualScriptSerializerTest, RoundTripThroughAFileMatchesTheStringPath)
{
    const auto original = BuildRichAsset();
    const auto path = OloEngine::Tests::TempFile("visual_script_roundtrip.olovs");

    ASSERT_TRUE(VisualScriptSerializer::Serialize(*original, path));
    auto reloaded = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::Deserialize(*reloaded, path));

    EXPECT_EQ(VisualScriptSerializer::SerializeToString(*original), VisualScriptSerializer::SerializeToString(*reloaded));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_F(VisualScriptSerializerTest, RejectsYamlThatIsNotAGraph)
{
    auto asset = Ref<VisualScriptAsset>::Create();
    EXPECT_FALSE(VisualScriptSerializer::DeserializeFromString(*asset, "SoundGraph: 1\nNodes: []\n"))
        << "a file for a DIFFERENT asset type must be refused, not partially absorbed";
    EXPECT_FALSE(VisualScriptSerializer::DeserializeFromString(*asset, "{ this is not: [valid, yaml"));
    EXPECT_FALSE(VisualScriptSerializer::DeserializeFromString(*asset, ""));
}

TEST_F(VisualScriptSerializerTest, RejectsAFutureFormatVersion)
{
    auto asset = Ref<VisualScriptAsset>::Create();
    EXPECT_FALSE(VisualScriptSerializer::DeserializeFromString(*asset, "VisualScript: 9999\nEventGraph:\n  Name: EventGraph\n"))
        << "reading a newer format as if it were the current one silently drops whatever was added";
}

TEST_F(VisualScriptSerializerTest, NonFiniteValuesAreClampedOnLoad)
{
    // The hostile-file case: a hand-edited or corrupted graph carrying NaN.
    // Without the clamp it reaches a transform through the first Get Variable.
    const std::string yaml = R"(VisualScript: 1
NodeBudgetPerTick: 10000
Variables:
  - Name: Speed
    Type: Float
    Default: nan
EventGraph:
  Name: EventGraph
  NextNodeId: 1
  NextLinkId: 1
  Inputs: []
  Outputs: []
  Nodes: []
  Links: []
Functions: []
)";
    auto asset = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*asset, yaml));
    ASSERT_EQ(asset->m_Variables.size(), 1u);
    EXPECT_TRUE(asset->m_Variables[0].m_DefaultValue.IsFinite());
    EXPECT_FLOAT_EQ(asset->m_Variables[0].m_DefaultValue.AsFloat(), 0.0f);
}

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

TEST_F(VisualScriptSerializerTest, ShippedSampleGraphLoadsAndCompiles)
{
    // The sample under SandboxProject is the only hand-authored `.olovs` in the
    // repo, and until the editor panel exists it is the only worked example of
    // the file format. A format change that breaks it would otherwise go
    // unnoticed until someone opened it.
    const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets" / "VisualScripts" / "PatrolAndAnnounce.olovs";
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();

    auto asset = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::Deserialize(*asset, path));

    std::vector<CompileDiagnostic> errors;
    Ref<VisualScriptPlan> plan = VisualScriptPlan::Compile(*asset, errors);
    ASSERT_TRUE(plan) << (errors.empty() ? std::string("compile failed with no diagnostics")
                                         : errors[0].m_Graph + " node " + std::to_string(errors[0].m_Node) + ": " + errors[0].m_Message);
    EXPECT_EQ(asset->m_Functions.size(), 1u) << "the sample demonstrates a sub-graph function";
    EXPECT_GE(asset->m_Variables.size(), 3u);
}

TEST_F(VisualScriptSerializerTest, NodeIdCountersSurviveAHandEditedFloor)
{
    // A file whose NextNodeId is below an existing node id would make the next
    // AddNode collide with a live node — the reader must take the max, not the
    // stored value.
    const std::string yaml = R"(VisualScript: 1
Variables: []
EventGraph:
  Name: EventGraph
  NextNodeId: 1
  NextLinkId: 1
  Inputs: []
  Outputs: []
  Nodes:
    - Id: 42
      Type: Event.OnBeginPlay
      Position: 0 0
  Links: []
Functions: []
)";
    auto asset = Ref<VisualScriptAsset>::Create();
    ASSERT_TRUE(VisualScriptSerializer::DeserializeFromString(*asset, yaml));
    EXPECT_GT(asset->m_EventGraph.m_NextNodeId, 42u);
    EXPECT_EQ(asset->m_EventGraph.AddNode("Utility.Print").m_Id, 43u);
}
