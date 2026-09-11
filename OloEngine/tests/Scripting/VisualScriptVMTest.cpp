#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// VisualScriptVMTest — the flow VM's contract (issue #634, AC#2 and AC#4).
//
// These run the VM with NO Scene attached (RuntimeContext::m_Scene == nullptr),
// which is deliberate: everything asserted here is about control flow, data
// pull, latency and the safety guards, none of which should need an ECS. The
// ECS-facing half is covered by the Functional tests.
//
// What each group would catch:
//   Compile*      — a graph that should be rejected being accepted, which turns
//                   an authoring mistake into a runtime hang or a silent no-op.
//   Flow*         — a per-node behaviour contract (AC#4's "each flow node").
//   Determinism*  — two instances of one plan diverging, or a second run of the
//                   same instance not reproducing. Without this, "it worked when
//                   I ran it" is the only evidence a graph is correct.
//   Guard*        — the runaway-loop budget and the recursion depth cap. Both
//                   are the difference between a bad graph logging once and a
//                   bad graph hanging the frame with no stack to look at.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptVM.h"

#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::VisualScript;

namespace
{
    class VisualScriptVMTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            NodeRegistry::EnsureStandardLibrary();
            m_Asset = Ref<VisualScriptAsset>::Create();
        }

        VisualScriptGraph& Graph()
        {
            return m_Asset->m_EventGraph;
        }

        VisualScriptVariable& AddVariable(std::string name, PinType type, PinValue defaultValue)
        {
            VisualScriptVariable& variable = m_Asset->m_Variables.emplace_back();
            variable.m_Name = std::move(name);
            variable.m_Type = type;
            variable.m_DefaultValue = std::move(defaultValue);
            return variable;
        }

        /// Compiles and instantiates. Fails the test (rather than crashing later)
        /// when the graph does not compile — a compile failure in a test that
        /// meant to exercise runtime behaviour is otherwise a confusing null
        /// dereference three lines down.
        bool Instantiate()
        {
            m_Errors.clear();
            m_Plan = VisualScriptPlan::Compile(*m_Asset, m_Errors);
            if (!m_Plan)
            {
                return false;
            }
            m_Instance = VisualScriptInstance(m_Plan, UUID(1234));
            return true;
        }

        RuntimeContext MakeRuntime(f32 dt = 1.0f / 60.0f)
        {
            RuntimeContext runtime;
            runtime.m_DeltaTime = dt;
            runtime.m_EventOutbox = &m_Outbox;
            runtime.m_LogSink = &m_Log;
            runtime.m_RandomState = &m_RandomState;
            return runtime;
        }

        Ref<VisualScriptAsset> m_Asset;
        /// Kept so tests that build extra instances share the compiled plan
        /// without const_cast-ing it back out of the instance.
        Ref<VisualScriptPlan> m_Plan;
        VisualScriptInstance m_Instance;
        std::vector<CompileDiagnostic> m_Errors;
        std::vector<EmittedEvent> m_Outbox;
        std::vector<std::string> m_Log;
        u64 m_RandomState = 1;
    };

    // A graph that counts: OnUpdate -> Set "Count" = Get "Count" + 1.
    // Reused by several cases because it exercises the whole loop — an event
    // entry, an exec node, a pure pull chain, and a blackboard write.
    void BuildCounterGraph(VisualScriptGraph& graph)
    {
        const NodeId update = graph.AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
        const NodeId get = graph.AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
        graph.FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
        const NodeId add = graph.AddNode("Math.Add").m_Id;
        graph.FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
        const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
        graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

        graph.AddLink(update, "Then", set, "Enter");
        graph.AddLink(get, "Value", add, "A");
        graph.AddLink(add, "Result", set, "Value");
    }
} // namespace

//==============================================================================
// Compilation
//==============================================================================

TEST_F(VisualScriptVMTest, CompileRejectsUnknownNodeType)
{
    Graph().AddNode("Flow.ThisNodeDoesNotExist");
    EXPECT_FALSE(Instantiate()) << "an unknown node type must fail the compile, not silently vanish";
    ASSERT_FALSE(m_Errors.empty());
    EXPECT_NE(m_Errors[0].m_Message.find("Unknown node type"), std::string::npos);
}

TEST_F(VisualScriptVMTest, CompileRejectsDanglingLinkEndpoint)
{
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId print = Graph().AddNode("Utility.Print").m_Id;
    // "Out" is not a pin on Utility.Print.
    Graph().AddLink(update, "Then", print, "Out");

    EXPECT_FALSE(Instantiate()) << "a link naming a pin that does not exist must fail the compile";
    ASSERT_FALSE(m_Errors.empty());
    EXPECT_NE(m_Errors[0].m_Message.find("Dangling"), std::string::npos);
}

TEST_F(VisualScriptVMTest, CompileRejectsLinkToMissingNode)
{
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    Graph().AddLink(update, "Then", 9999, "Enter");
    EXPECT_FALSE(Instantiate());
}

TEST_F(VisualScriptVMTest, CompileRejectsExecWiredToDataPin)
{
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId print = Graph().AddNode("Utility.Print").m_Id;
    // Exec output into a String input: the one crossing the pin-kind split must
    // never allow. If this ever passes, control flow and dataflow have merged.
    Graph().AddLink(update, "Then", print, "Message");

    EXPECT_FALSE(Instantiate());
    ASSERT_FALSE(m_Errors.empty());
    EXPECT_NE(m_Errors[0].m_Message.find("Incompatible"), std::string::npos);
}

TEST_F(VisualScriptVMTest, CompileRejectsTwoWritersIntoOneDataInput)
{
    const NodeId a = Graph().AddNode("Math.Add").m_Id;
    const NodeId b = Graph().AddNode("Math.Add").m_Id;
    const NodeId sink = Graph().AddNode("Math.Add").m_Id;
    Graph().AddLink(a, "Result", sink, "A");
    Graph().AddLink(b, "Result", sink, "A");

    EXPECT_FALSE(Instantiate()) << "two links into one input pin has no defined winner";
}

TEST_F(VisualScriptVMTest, CompileRejectsPureDataCycle)
{
    const NodeId a = Graph().AddNode("Math.Add").m_Id;
    const NodeId b = Graph().AddNode("Math.Add").m_Id;
    Graph().AddLink(a, "Result", b, "A");
    Graph().AddLink(b, "Result", a, "A");

    EXPECT_FALSE(Instantiate()) << "a cycle among pure nodes would recurse forever at pull time";
    ASSERT_FALSE(m_Errors.empty());
    EXPECT_NE(m_Errors[0].m_Message.find("Cycle"), std::string::npos);
}

TEST_F(VisualScriptVMTest, CompileAcceptsExecFeedbackLoop)
{
    // The mirror image of the case above: an exec-level loop is legal and
    // useful. Exec node outputs are READ from stored slots, so no recursion.
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId flip = Graph().AddNode("Flow.FlipFlop").m_Id;
    const NodeId once = Graph().AddNode("Flow.DoOnce").m_Id;
    Graph().AddLink(update, "Then", flip, "Enter");
    Graph().AddLink(flip, "A", once, "Enter");
    Graph().AddLink(flip, "B", once, "Reset");

    EXPECT_TRUE(Instantiate()) << "exec feedback must compile; only PURE cycles are rejected";
}

TEST_F(VisualScriptVMTest, CompileRejectsDuplicateVariableNames)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));
    AddVariable("Count", PinType::Int, PinValue::MakeInt(0));
    EXPECT_FALSE(Instantiate());
}

//==============================================================================
// Flow-node contracts
//==============================================================================

TEST_F(VisualScriptVMTest, FlowBranchTakesTheSelectedSide)
{
    AddVariable("Taken", PinType::String, PinValue::MakeString(""));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId branch = Graph().AddNode("Flow.Branch").m_Id;
    Graph().FindNode(branch)->m_PinDefaults["Condition"] = PinValue::MakeBool(true);
    const NodeId setTrue = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(setTrue)->SetProperty(std::string(NodeProps::kVariableName), "Taken");
    Graph().FindNode(setTrue)->m_PinDefaults["Value"] = PinValue::MakeString("true-side");
    const NodeId setFalse = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(setFalse)->SetProperty(std::string(NodeProps::kVariableName), "Taken");
    Graph().FindNode(setFalse)->m_PinDefaults["Value"] = PinValue::MakeString("false-side");

    Graph().AddLink(begin, "Then", branch, "Enter");
    Graph().AddLink(branch, "True", setTrue, "Enter");
    Graph().AddLink(branch, "False", setFalse, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_EQ(m_Instance.GetVariable("Taken").AsString(), "true-side");
}

TEST_F(VisualScriptVMTest, FlowSequenceRunsEveryOutputInOrder)
{
    AddVariable("Trace", PinType::String, PinValue::MakeString(""));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId sequence = Graph().AddNode(std::string(NodeTypes::kSequence)).m_Id;
    Graph().FindNode(sequence)->SetProperty(std::string(NodeProps::kOutputCount), "3");
    Graph().AddLink(begin, "Then", sequence, "Enter");

    // Each branch appends its own letter, so the ASSERTED string proves ORDER,
    // not just that all three ran.
    for (i32 i = 0; i < 3; ++i)
    {
        const NodeId append = Graph().AddNode("Utility.Format").m_Id;
        Graph().FindNode(append)->m_PinDefaults["Format"] = PinValue::MakeString("{0}" + std::to_string(i));
        const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
        Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Trace");
        const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
        Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Trace");

        Graph().AddLink(get, "Value", append, "A");
        Graph().AddLink(append, "Result", set, "Value");
        Graph().AddLink(sequence, "Then " + std::to_string(i), set, "Enter");
    }

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_EQ(m_Instance.GetVariable("Trace").AsString(), "012");
}

TEST_F(VisualScriptVMTest, FlowForLoopRunsTheBodyOncePerIndex)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.ForLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["First"] = PinValue::MakeInt(1);
    Graph().FindNode(loop)->m_PinDefaults["Last"] = PinValue::MakeInt(5);

    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

    Graph().AddLink(begin, "Then", loop, "Enter");
    Graph().AddLink(loop, "Loop Body", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    // Accumulating the INDEX (not a constant 1) also proves the loop republishes
    // its Index output every iteration rather than latching the first value.
    Graph().AddLink(loop, "Index", add, "B");
    Graph().AddLink(add, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 15.0f) << "1+2+3+4+5";
}

TEST_F(VisualScriptVMTest, FlowDoOnceBlocksAfterTheFirstEntryUntilReset)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    // OnUpdate -> DoOnce -> Set Count = Count + 1
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId once = Graph().AddNode("Flow.DoOnce").m_Id;
    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

    // A custom event resets the latch.
    const NodeId reset = Graph().AddNode(std::string(NodeTypes::kCustomEvent)).m_Id;
    Graph().FindNode(reset)->SetProperty(std::string(NodeProps::kEventName), "Reset");

    Graph().AddLink(update, "Then", once, "Enter");
    Graph().AddLink(once, "Then", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    Graph().AddLink(add, "Result", set, "Value");
    Graph().AddLink(reset, "Then", once, "Reset");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    for (i32 i = 0; i < 10; ++i)
    {
        m_Instance.Tick(runtime);
    }
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 1.0f)
        << "Do Once must pass exactly one entry through across ten ticks";

    IncomingEvent resetEvent;
    resetEvent.m_Key = VisualScriptPlan::MakeEventKey("Custom", "Reset");
    m_Instance.DispatchEvent(resetEvent, runtime);
    for (i32 i = 0; i < 10; ++i)
    {
        m_Instance.Tick(runtime);
    }
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 2.0f)
        << "Reset must re-arm the latch for exactly one more entry";
}

TEST_F(VisualScriptVMTest, FlowGateBlocksUntilOpened)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId gate = Graph().AddNode("Flow.Gate").m_Id;
    Graph().FindNode(gate)->m_PinDefaults["Start Closed"] = PinValue::MakeBool(true);
    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

    Graph().AddLink(update, "Then", gate, "Enter");
    Graph().AddLink(gate, "Exit", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    Graph().AddLink(add, "Result", set, "Value");

    // A custom event opens the gate.
    const NodeId custom = Graph().AddNode(std::string(NodeTypes::kCustomEvent)).m_Id;
    Graph().FindNode(custom)->SetProperty(std::string(NodeProps::kEventName), "Open");
    Graph().AddLink(custom, "Then", gate, "Open");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    for (i32 i = 0; i < 3; ++i)
    {
        m_Instance.Tick(runtime);
    }
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 0.0f) << "a closed gate must pass nothing";

    IncomingEvent open;
    open.m_Key = VisualScriptPlan::MakeEventKey("Custom", "Open");
    EXPECT_GT(m_Instance.DispatchEvent(open, runtime), 0u);

    for (i32 i = 0; i < 3; ++i)
    {
        m_Instance.Tick(runtime);
    }
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 3.0f) << "an opened gate must pass every entry";
}

TEST_F(VisualScriptVMTest, FlowDelaySuspendsAndResumesAcrossTicks)
{
    AddVariable("Fired", PinType::Bool, PinValue::MakeBool(false));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId delay = Graph().AddNode("Flow.Delay").m_Id;
    Graph().FindNode(delay)->m_PinDefaults["Duration"] = PinValue::MakeFloat(0.25f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Fired");
    Graph().FindNode(set)->m_PinDefaults["Value"] = PinValue::MakeBool(true);

    Graph().AddLink(begin, "Then", delay, "Enter");
    Graph().AddLink(delay, "Completed", set, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime(0.1f);
    m_Instance.BeginPlay(runtime);
    EXPECT_FALSE(m_Instance.GetVariable("Fired").AsBool()) << "Delay must not complete in the tick that entered it";
    EXPECT_EQ(m_Instance.GetPendingLatentCount(), 1u);

    m_Instance.Tick(runtime); // 0.1s elapsed
    EXPECT_FALSE(m_Instance.GetVariable("Fired").AsBool());
    m_Instance.Tick(runtime); // 0.2s
    EXPECT_FALSE(m_Instance.GetVariable("Fired").AsBool());
    m_Instance.Tick(runtime); // 0.3s >= 0.25s
    EXPECT_TRUE(m_Instance.GetVariable("Fired").AsBool()) << "Delay must resume once its duration has elapsed";
    EXPECT_EQ(m_Instance.GetPendingLatentCount(), 0u) << "a resumed latent must not stay parked";
}

TEST_F(VisualScriptVMTest, FlowWaitForEventSuspendsUntilThatEventArrives)
{
    AddVariable("Fired", PinType::Bool, PinValue::MakeBool(false));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId wait = Graph().AddNode("Flow.WaitForEvent").m_Id;
    Graph().FindNode(wait)->m_PinDefaults["Event Name"] = PinValue::MakeString("Go");
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Fired");
    Graph().FindNode(set)->m_PinDefaults["Value"] = PinValue::MakeBool(true);

    Graph().AddLink(begin, "Then", wait, "Enter");
    Graph().AddLink(wait, "Resumed", set, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    for (i32 i = 0; i < 20; ++i)
    {
        m_Instance.Tick(runtime);
    }
    EXPECT_FALSE(m_Instance.GetVariable("Fired").AsBool()) << "an event wait must not expire on time";

    IncomingEvent wrong;
    wrong.m_Key = VisualScriptPlan::MakeEventKey("Custom", "SomethingElse");
    m_Instance.DispatchEvent(wrong, runtime);
    EXPECT_FALSE(m_Instance.GetVariable("Fired").AsBool()) << "a different event must not resume the wait";

    IncomingEvent go;
    go.m_Key = VisualScriptPlan::MakeEventKey("Custom", "Go");
    m_Instance.DispatchEvent(go, runtime);
    EXPECT_TRUE(m_Instance.GetVariable("Fired").AsBool());
}

TEST_F(VisualScriptVMTest, FunctionCallPassesArgumentsAndReturnsResults)
{
    AddVariable("Result", PinType::Float, PinValue::MakeFloat(0.0f));

    // function Double(In) -> Out { Out = In * 2 }
    VisualScriptGraph& function = m_Asset->m_Functions.emplace_back();
    function.m_Name = "Double";
    function.m_Inputs.push_back({ "In", PinType::Float, PinValue::MakeFloat(0.0f) });
    function.m_Outputs.push_back({ "Out", PinType::Float, PinValue::MakeFloat(0.0f) });
    const NodeId entry = function.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
    const NodeId multiply = function.AddNode("Math.Multiply").m_Id;
    function.FindNode(multiply)->m_PinDefaults["B"] = PinValue::MakeFloat(2.0f);
    const NodeId ret = function.AddNode(std::string(NodeTypes::kFunctionReturn)).m_Id;
    function.AddLink(entry, "Then", ret, "Enter");
    function.AddLink(entry, "In", multiply, "A");
    function.AddLink(multiply, "Result", ret, "Out");

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId call = Graph().AddNode(std::string(NodeTypes::kFunctionCall)).m_Id;
    Graph().FindNode(call)->SetProperty(std::string(NodeProps::kFunctionName), "Double");
    Graph().FindNode(call)->m_PinDefaults["In"] = PinValue::MakeFloat(21.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Result");

    Graph().AddLink(begin, "Then", call, "Enter");
    Graph().AddLink(call, "Then", set, "Enter");
    Graph().AddLink(call, "Out", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Result").AsFloat(), 42.0f);
}

TEST_F(VisualScriptVMTest, FunctionCallRejectsAnUnknownTarget)
{
    Graph().AddNode(std::string(NodeTypes::kFunctionCall)).SetProperty(std::string(NodeProps::kFunctionName), "NoSuchFunction");
    EXPECT_FALSE(Instantiate()) << "a call to a function that does not exist must fail the compile";
}

TEST_F(VisualScriptVMTest, CompileRejectsALatentNodeInsideAFunction)
{
    // CallFunction runs the body synchronously and pops the return frame the
    // moment ExecuteFrom returns, so a Delay inside a function parks, the call
    // returns empty, and the later resume finds no frame to return into. None of
    // that is visible at runtime — the function just quietly yields nothing — so
    // the only honest place to catch it is the compile.
    VisualScriptGraph& function = m_Asset->m_Functions.emplace_back();
    function.m_Name = "Waits";
    const NodeId entry = function.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
    const NodeId delay = function.AddNode("Flow.Delay").m_Id;
    const NodeId ret = function.AddNode(std::string(NodeTypes::kFunctionReturn)).m_Id;
    function.AddLink(entry, "Then", delay, "Enter");
    function.AddLink(delay, "Completed", ret, "Enter");

    Graph().AddNode(std::string(NodeTypes::kOnBeginPlay));

    EXPECT_FALSE(Instantiate()) << "a latent node in a function must fail the compile, not fail silently at runtime";
    ASSERT_FALSE(m_Errors.empty());
    EXPECT_NE(m_Errors.front().m_Message.find("Latent"), std::string::npos)
        << "the diagnostic must name the actual problem; got: " << m_Errors.front().m_Message;
}

TEST_F(VisualScriptVMTest, LatentNodesStillCompileInTheEventGraph)
{
    // The guard above must not have outlawed latents outright — the event graph
    // is where they are designed to live.
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId delay = Graph().AddNode("Flow.Delay").m_Id;
    Graph().AddLink(begin, "Then", delay, "Enter");
    EXPECT_TRUE(Instantiate());
}

//==============================================================================
// Data pull
//==============================================================================

TEST_F(VisualScriptVMTest, UnconnectedInputPinUsesItsLiteralDefault)
{
    AddVariable("Result", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["A"] = PinValue::MakeFloat(3.5f);
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.5f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Result");

    Graph().AddLink(begin, "Then", set, "Enter");
    Graph().AddLink(add, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Result").AsFloat(), 5.0f);
}

TEST_F(VisualScriptVMTest, DataPullCoercesAcrossNumericPinTypes)
{
    AddVariable("Result", PinType::Int, PinValue::MakeInt(0));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["A"] = PinValue::MakeFloat(2.9f);
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(0.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Result");

    Graph().AddLink(begin, "Then", set, "Enter");
    // Float output -> Int variable: coerced, and truncation is the documented
    // behaviour (not rounding).
    Graph().AddLink(add, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_EQ(m_Instance.GetVariable("Result").AsInt(), 2);
}

TEST_F(VisualScriptVMTest, NonFiniteVariableWriteIsRejected)
{
    AddVariable("Value", PinType::Float, PinValue::MakeFloat(1.0f));
    ASSERT_TRUE(Instantiate());

    EXPECT_TRUE(m_Instance.SetVariable("Value", PinValue::MakeFloat(std::numeric_limits<f32>::quiet_NaN())));
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Value").AsFloat(), 0.0f)
        << "a NaN must be clamped at the blackboard, not propagated into every downstream node";
    EXPECT_FALSE(m_Instance.GetErrors().empty()) << "the clamp must be reported, not silent";
}

//==============================================================================
// Determinism
//==============================================================================

TEST_F(VisualScriptVMTest, DeterminismTwoInstancesOfOnePlanProduceIdenticalResults)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));
    BuildCounterGraph(Graph());
    ASSERT_TRUE(Instantiate());

    // Both instances share the compiled plan — this is the per-entity instancing
    // claim. If the plan carried mutable state, the second instance would start
    // wherever the first left off.
    VisualScriptInstance first(m_Plan, UUID(1));
    VisualScriptInstance second(m_Plan, UUID(2));

    RuntimeContext runtime = MakeRuntime();
    first.BeginPlay(runtime);
    second.BeginPlay(runtime);
    for (i32 i = 0; i < 7; ++i)
    {
        first.Tick(runtime);
    }
    for (i32 i = 0; i < 7; ++i)
    {
        second.Tick(runtime);
    }

    EXPECT_FLOAT_EQ(first.GetVariable("Count").AsFloat(), 7.0f);
    EXPECT_FLOAT_EQ(second.GetVariable("Count").AsFloat(), 7.0f);
}

TEST_F(VisualScriptVMTest, DeterminismRandomNodeReproducesFromTheSameSeed)
{
    AddVariable("Roll", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId random = Graph().AddNode("Math.RandomFloat").m_Id;
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Roll");
    Graph().AddLink(update, "Then", set, "Enter");
    Graph().AddLink(random, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());

    const auto run = [&](u64 seed)
    {
        VisualScriptInstance instance(m_Plan, UUID(1));
        u64 state = seed;
        RuntimeContext runtime = MakeRuntime();
        runtime.m_RandomState = &state;
        instance.BeginPlay(runtime);
        std::vector<f32> rolls;
        for (i32 i = 0; i < 5; ++i)
        {
            instance.Tick(runtime);
            rolls.push_back(instance.GetVariable("Roll").AsFloat());
        }
        return rolls;
    };

    const std::vector<f32> a = run(12345);
    const std::vector<f32> b = run(12345);
    const std::vector<f32> c = run(999);

    ASSERT_EQ(a.size(), 5u);
    EXPECT_EQ(a, b) << "the same seed must reproduce the same sequence — a replay depends on it";
    EXPECT_NE(a, c) << "a different seed must produce a different sequence, or the node is not random at all";
    for (const f32 roll : a)
    {
        EXPECT_GE(roll, 0.0f);
        EXPECT_LT(roll, 1.0f);
    }
}

//==============================================================================
// Guards
//==============================================================================

TEST_F(VisualScriptVMTest, GuardRunawayWhileLoopIsHaltedByTheNodeBudget)
{
    m_Asset->m_NodeBudgetPerTick = 500;

    // While(true) with an EMPTY body: the specific shape that would hang the
    // frame if BeginIteration did not charge the budget itself. If this test
    // ever hangs rather than fails, that guard is gone.
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.WhileLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["Condition"] = PinValue::MakeBool(true);
    Graph().AddLink(begin, "Then", loop, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    EXPECT_TRUE(m_Instance.DidExceedBudget()) << "an unbounded loop must trip the budget guard";
    EXPECT_LE(m_Instance.GetNodesExecutedThisTick(), 501u) << "the guard must stop AT the budget, not somewhere past it";
    EXPECT_FALSE(m_Instance.GetErrors().empty()) << "tripping the budget must be reported";
}

TEST_F(VisualScriptVMTest, GuardBudgetRefillsEachTick)
{
    m_Asset->m_NodeBudgetPerTick = 500;
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));
    BuildCounterGraph(Graph());
    ASSERT_TRUE(Instantiate());

    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    for (i32 i = 0; i < 100; ++i)
    {
        m_Instance.Tick(runtime);
    }
    // 100 ticks of a 4-node graph is 400 node executions total but only ~4 per
    // tick — a budget that did NOT refill would stop this at tick ~125.
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 100.0f);
    EXPECT_FALSE(m_Instance.DidExceedBudget());
}

TEST_F(VisualScriptVMTest, GuardDeepExecChainIsBoundedNotAStackOverflow)
{
    // 400 chained Print nodes — past the 128 depth cap. Since ADR 0023 an exec
    // chain costs no C++ stack, so this can no longer overflow one; the cap is
    // kept as policy, so that a graph that was a reported error before does not
    // silently become legal. The depth rides on the exec frame — drop it and
    // this runs all 400 quietly.
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    NodeId previous = begin;
    std::string previousPin = "Then";
    for (i32 i = 0; i < 400; ++i)
    {
        const NodeId print = Graph().AddNode("Utility.Print").m_Id;
        Graph().AddLink(previous, previousPin, print, "Enter");
        previous = print;
        previousPin = "Then";
    }

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_FALSE(m_Instance.GetErrors().empty()) << "exceeding the depth cap must be reported";
    EXPECT_LT(m_Log.size(), 400u) << "the chain must have been cut short, not run to completion";
}

TEST_F(VisualScriptVMTest, ResumingAfterABreakpointRunsTheGraphAgain)
{
    // The guard above early-returns from ExecuteFrom while paused; this proves it
    // is a pause and not a permanent kill switch.
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    // Built inline rather than via BuildCounterGraph so the setter's NodeId is in
    // hand for the breakpoint key.
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    Graph().AddLink(update, "Then", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    Graph().AddLink(add, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, set));
    m_Instance.Tick(runtime);
    ASSERT_TRUE(m_Instance.Debug().m_Paused);
    const f32 whilePaused = m_Instance.GetVariable("Count").AsFloat();

    m_Instance.Tick(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), whilePaused) << "a paused graph must not tick";

    m_Instance.Debug().m_Breakpoints.clear();
    m_Instance.DebugResume();
    m_Instance.Tick(runtime);
    EXPECT_GT(m_Instance.GetVariable("Count").AsFloat(), whilePaused) << "resuming must let the graph run again";
}

//==============================================================================
// Node-granular stepping (issue #1069, ADR 0023)
//
// The exec stack lives on the instance, so a pause has something to resume
// from. These are the properties that buys, and they are the ones a rewrite of
// the drain loop would break silently: a step that runs two nodes, a resume
// that drops the rest of the tick, a loop whose index restarts, and a loop left
// wedged shut after the budget abandoned it.
//==============================================================================

TEST_F(VisualScriptVMTest, StepAdvancesExactlyOneNodeAtATime)
{
    // OnUpdate -> Print A -> Print B -> Print C. Print is the cleanest step
    // observer there is: one visible side effect per node, in order.
    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    NodeId previous = update;
    std::string previousPin = "Then";
    std::vector<NodeId> prints;
    for (const char* message : { "A", "B", "C" })
    {
        const NodeId print = Graph().AddNode("Utility.Print").m_Id;
        Graph().FindNode(print)->m_PinDefaults["Message"] = PinValue::MakeString(message);
        Graph().AddLink(previous, previousPin, print, "Enter");
        previous = print;
        previousPin = "Then";
        prints.push_back(print);
    }

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, prints[0]));
    m_Instance.Tick(runtime);
    ASSERT_TRUE(m_Instance.Debug().m_Paused);
    ASSERT_TRUE(m_Log.empty()) << "the breakpoint stops BEFORE the node it is on";

    // One press, one node — and the breakpoint it is standing on does not eat
    // the press, or stepping off a breakpoint would be impossible.
    m_Instance.DebugStepNodes(1);
    m_Instance.Tick(runtime);
    EXPECT_TRUE(m_Instance.Debug().m_Paused) << "a step re-arms the pause";
    ASSERT_EQ(m_Log.size(), 1u);
    EXPECT_EQ(m_Log[0], "A");

    m_Instance.DebugStepNodes(1);
    m_Instance.Tick(runtime);
    ASSERT_EQ(m_Log.size(), 2u);
    EXPECT_EQ(m_Log[1], "B");

    m_Instance.DebugStepNodes(1);
    m_Instance.Tick(runtime);
    ASSERT_EQ(m_Log.size(), 3u);
    EXPECT_EQ(m_Log[2], "C");
}

TEST_F(VisualScriptVMTest, SteppingRunsEachSideEffectExactlyOnce)
{
    // The reason #1069 refused to build stepping on the old VM: the only way to
    // "step" a non-resumable VM was to re-run the tick and stop one node later,
    // which repeats everything before the breakpoint. If that ever comes back,
    // this counter reads 2, or 3, instead of 1.
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId after = Graph().AddNode("Utility.Print").m_Id;
    Graph().FindNode(after)->m_PinDefaults["Message"] = PinValue::MakeString("done");

    Graph().AddLink(update, "Then", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    Graph().AddLink(add, "Result", set, "Value");
    Graph().AddLink(set, "Then", after, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, set));
    m_Instance.Tick(runtime);
    ASSERT_TRUE(m_Instance.Debug().m_Paused);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 0.0f);

    m_Instance.DebugStepNodes(1);
    m_Instance.Tick(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 1.0f) << "the increment happened once";
    EXPECT_TRUE(m_Log.empty()) << "and only the ONE node ran";

    m_Instance.DebugStepNodes(1);
    m_Instance.Tick(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 1.0f) << "stepping on must not redo it";
    EXPECT_EQ(m_Log.size(), 1u);
}

TEST_F(VisualScriptVMTest, SteppingWalksALoopBodyOneIterationAtATime)
{
    // The case the issue opened with: "a graph with a loop body gives you no way
    // to watch the exec token move". Each step is one node, so a three-iteration
    // loop is a countable walk rather than one opaque jump.
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.ForLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["First"] = PinValue::MakeInt(1);
    Graph().FindNode(loop)->m_PinDefaults["Last"] = PinValue::MakeInt(3);
    const NodeId print = Graph().AddNode("Utility.Print").m_Id;
    const NodeId toString = Graph().AddNode("Utility.ToString").m_Id;

    Graph().AddLink(begin, "Then", loop, "Enter");
    Graph().AddLink(loop, "Loop Body", print, "Enter");
    Graph().AddLink(loop, "Index", toString, "Value");
    Graph().AddLink(toString, "Result", print, "Message");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();

    // Break on the body so BeginPlay stops in front of iteration 1.
    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, print));
    m_Instance.BeginPlay(runtime);
    ASSERT_TRUE(m_Instance.Debug().m_Paused);
    ASSERT_TRUE(m_Log.empty());

    // Each iteration is two frames — the Print, then the loop node re-entering
    // itself — so stepping two at a time lands on the next body node.
    for (i32 iteration = 1; iteration <= 3; ++iteration)
    {
        m_Instance.DebugStepNodes(2);
        m_Instance.Tick(runtime);
        ASSERT_EQ(m_Log.size(), static_cast<sizet>(iteration)) << "iteration " << iteration;
    }

    // The Index output has to move with the walk, or the body would see the same
    // value every time and the loop would be a step-shaped no-op.
    EXPECT_EQ(m_Instance.PeekOutput(0, loop, "Index").AsInt(), 3);

    m_Instance.Debug().m_Breakpoints.clear();
    m_Instance.DebugResume();
    m_Instance.Tick(runtime);
    EXPECT_EQ(m_Log.size(), 3u) << "the loop ran exactly its authored range, once";
}

TEST_F(VisualScriptVMTest, ResumingAfterABreakpointFinishesTheInterruptedTick)
{
    // Before ADR 0023 this was the debugger's quiet lie: pausing threw the rest
    // of the tick away and "Resume" started the NEXT one, so breaking anywhere
    // changed what the graph did. The Sequence here has a branch after the
    // breakpoint; it must not run while paused, and must run on resume.
    AddVariable("First", PinType::Bool, PinValue::MakeBool(false));
    AddVariable("Second", PinType::Bool, PinValue::MakeBool(false));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId sequence = Graph().AddNode(std::string(NodeTypes::kSequence)).m_Id;
    Graph().FindNode(sequence)->SetProperty(std::string(NodeProps::kOutputCount), "2");
    Graph().AddLink(begin, "Then", sequence, "Enter");

    const NodeId setFirst = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(setFirst)->SetProperty(std::string(NodeProps::kVariableName), "First");
    Graph().FindNode(setFirst)->m_PinDefaults["Value"] = PinValue::MakeBool(true);
    const NodeId setSecond = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(setSecond)->SetProperty(std::string(NodeProps::kVariableName), "Second");
    Graph().FindNode(setSecond)->m_PinDefaults["Value"] = PinValue::MakeBool(true);

    Graph().AddLink(sequence, "Then 0", setFirst, "Enter");
    Graph().AddLink(sequence, "Then 1", setSecond, "Enter");

    ASSERT_TRUE(Instantiate());
    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, setFirst));

    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    ASSERT_TRUE(m_Instance.Debug().m_Paused);
    EXPECT_FALSE(m_Instance.GetVariable("First").AsBool()) << "breaking happens BEFORE the node body runs";
    EXPECT_FALSE(m_Instance.GetVariable("Second").AsBool())
        << "a pause must hold the whole run, including branches the paused node never reached";
    EXPECT_GT(m_Instance.GetPendingExecCount(), 0u) << "the held work is queued, not discarded";

    m_Instance.Debug().m_Breakpoints.clear();
    m_Instance.DebugResume();
    m_Instance.Tick(runtime);
    EXPECT_TRUE(m_Instance.GetVariable("First").AsBool()) << "resume runs the node it stopped in front of";
    EXPECT_TRUE(m_Instance.GetVariable("Second").AsBool()) << "resume FINISHES the tick it interrupted";
    EXPECT_EQ(m_Instance.GetPendingExecCount(), 0u);
}

TEST_F(VisualScriptVMTest, ForLoopReadsFirstAndLastOnceOnEntry)
{
    // The index moved out of a C++ local and into NodeState when the loop became
    // re-entrant; the bounds had to move with it. Re-pulling Last per iteration
    // would look harmless and would let a loop body silently move its own end
    // point, because BeginIteration invalidates the pure memo on purpose.
    AddVariable("Limit", PinType::Int, PinValue::MakeInt(3));
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.ForLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["First"] = PinValue::MakeInt(1);
    const NodeId limit = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(limit)->SetProperty(std::string(NodeProps::kVariableName), "Limit");

    // First thing the body does is set Limit to 0 — a loop that re-read Last
    // would stop after one iteration.
    const NodeId clearLimit = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(clearLimit)->SetProperty(std::string(NodeProps::kVariableName), "Limit");
    Graph().FindNode(clearLimit)->m_PinDefaults["Value"] = PinValue::MakeInt(0);

    const NodeId get = Graph().AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    Graph().FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = Graph().AddNode("Math.Add").m_Id;
    Graph().FindNode(add)->m_PinDefaults["B"] = PinValue::MakeFloat(1.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

    Graph().AddLink(begin, "Then", loop, "Enter");
    Graph().AddLink(limit, "Value", loop, "Last");
    Graph().AddLink(loop, "Loop Body", clearLimit, "Enter");
    Graph().AddLink(clearLimit, "Then", set, "Enter");
    Graph().AddLink(get, "Value", add, "A");
    Graph().AddLink(add, "Result", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 3.0f)
        << "the range was fixed when the loop was entered, as the C++ locals used to fix it";
}

TEST_F(VisualScriptVMTest, LoopReenteredThroughAnExecCycleIsRefusedNotMiscounted)
{
    // The one behaviour ADR 0023 knowingly changes. Exec cycles are legal
    // (ADR 0014 section 4), so a body CAN wire back into its own loop's Enter.
    // Under descent that recursed with a fresh index; the index is in NodeState
    // now, so the re-entry would reset the iteration the outer entry is holding.
    // Refused loudly rather than silently miscounted.
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.ForLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["First"] = PinValue::MakeInt(1);
    Graph().FindNode(loop)->m_PinDefaults["Last"] = PinValue::MakeInt(3);
    const NodeId print = Graph().AddNode("Utility.Print").m_Id;
    Graph().FindNode(print)->m_PinDefaults["Message"] = PinValue::MakeString("body");

    Graph().AddLink(begin, "Then", loop, "Enter");
    Graph().AddLink(loop, "Loop Body", print, "Enter");
    Graph().AddLink(print, "Then", loop, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    EXPECT_FALSE(m_Instance.GetErrors().empty()) << "the re-entry must be reported, not silent";
    bool named = false;
    for (const std::string& error : m_Instance.GetErrors())
    {
        named = named || error.find("re-entered") != std::string::npos;
    }
    EXPECT_TRUE(named) << "and the error must say what happened";
    EXPECT_EQ(m_Log.size(), 3u) << "the outer loop still ran its own range exactly once";
}

TEST_F(VisualScriptVMTest, ALoopAbandonedByTheNodeBudgetStartsAgainNextTick)
{
    // A pending re-entry owns NodeState::m_Flag as its "still iterating" marker.
    // The budget running out inside the body throws that frame away, so the VM
    // has to release the marker on the way — otherwise the loop reads its own
    // leftover marker as a re-entry next tick and refuses itself for the rest of
    // play, which is a hang that looks like a logic bug in the graph.
    m_Asset->m_NodeBudgetPerTick = 50;

    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId loop = Graph().AddNode("Flow.ForLoop").m_Id;
    Graph().FindNode(loop)->m_PinDefaults["First"] = PinValue::MakeInt(1);
    Graph().FindNode(loop)->m_PinDefaults["Last"] = PinValue::MakeInt(1000);
    const NodeId print = Graph().AddNode("Utility.Print").m_Id;
    Graph().FindNode(print)->m_PinDefaults["Message"] = PinValue::MakeString("x");

    Graph().AddLink(update, "Then", loop, "Enter");
    Graph().AddLink(loop, "Loop Body", print, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    m_Instance.Tick(runtime);
    const sizet afterFirst = m_Log.size();
    ASSERT_GT(afterFirst, 0u);
    ASSERT_TRUE(m_Instance.DidExceedBudget()) << "the loop must have been cut short, or this proves nothing";
    EXPECT_EQ(m_Instance.GetPendingExecCount(), 0u) << "abandoned work is unwound, not left queued";

    m_Instance.ClearErrors();
    m_Instance.Tick(runtime);
    EXPECT_GT(m_Log.size(), afterFirst) << "the loop must run again, not stay wedged shut";
    for (const std::string& error : m_Instance.GetErrors())
    {
        EXPECT_EQ(error.find("re-entered"), std::string::npos) << "the leftover marker was not released: " << error;
    }
}

TEST_F(VisualScriptVMTest, SteppingThroughAFunctionCallStillMarshalsItsResults)
{
    // A call is queued now: the callee runs, and a bookkeeping frame afterwards
    // copies the results back and releases the re-entry guard. That frame is the
    // one piece of the old synchronous epilogue with nowhere obvious to live, so
    // step across it and check the value still lands.
    AddVariable("Result", PinType::Float, PinValue::MakeFloat(0.0f));

    VisualScriptGraph& function = m_Asset->m_Functions.emplace_back();
    function.m_Name = "Double";
    function.m_Inputs.push_back({ "In", PinType::Float, PinValue::MakeFloat(0.0f) });
    function.m_Outputs.push_back({ "Out", PinType::Float, PinValue::MakeFloat(0.0f) });
    const NodeId entry = function.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
    const NodeId multiply = function.AddNode("Math.Multiply").m_Id;
    function.FindNode(multiply)->m_PinDefaults["B"] = PinValue::MakeFloat(2.0f);
    const NodeId ret = function.AddNode(std::string(NodeTypes::kFunctionReturn)).m_Id;
    function.AddLink(entry, "Then", ret, "Enter");
    function.AddLink(entry, "In", multiply, "A");
    function.AddLink(multiply, "Result", ret, "Out");

    const NodeId update = Graph().AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
    const NodeId call = Graph().AddNode(std::string(NodeTypes::kFunctionCall)).m_Id;
    Graph().FindNode(call)->SetProperty(std::string(NodeProps::kFunctionName), "Double");
    Graph().FindNode(call)->m_PinDefaults["In"] = PinValue::MakeFloat(21.0f);
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Result");

    Graph().AddLink(update, "Then", call, "Enter");
    Graph().AddLink(call, "Then", set, "Enter");
    Graph().AddLink(call, "Out", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    m_Instance.Debug().m_Breakpoints.insert(DebugState::MakeKey(0, call));
    m_Instance.Tick(runtime);
    ASSERT_TRUE(m_Instance.Debug().m_Paused);

    // One node at a time, well past what this graph needs, so the walk crosses
    // the call, the callee and the bookkeeping frame without ever running two
    // nodes for one press.
    for (i32 i = 0; i < 12; ++i)
    {
        m_Instance.DebugStepNodes(1);
        m_Instance.Tick(runtime);
    }
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Result").AsFloat(), 42.0f);
    EXPECT_TRUE(m_Instance.GetErrors().empty()) << "the re-entry guard must have been released";
}

TEST_F(VisualScriptVMTest, ADelayInOneSequenceBranchStillLetsTheNextBranchRun)
{
    // Latent semantics under the drain loop: a suspending node queues nothing,
    // so the stack simply moves on to what was already queued — which is exactly
    // what returning-without-triggering did under descent. Worth pinning because
    // it is the one place the rewrite could have changed behaviour for free.
    AddVariable("Second", PinType::Bool, PinValue::MakeBool(false));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId sequence = Graph().AddNode(std::string(NodeTypes::kSequence)).m_Id;
    Graph().FindNode(sequence)->SetProperty(std::string(NodeProps::kOutputCount), "2");
    const NodeId delay = Graph().AddNode("Flow.Delay").m_Id;
    Graph().FindNode(delay)->m_PinDefaults["Duration"] = PinValue::MakeFloat(5.0f);
    const NodeId afterDelay = Graph().AddNode("Utility.Print").m_Id;
    Graph().FindNode(afterDelay)->m_PinDefaults["Message"] = PinValue::MakeString("late");
    const NodeId setSecond = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(setSecond)->SetProperty(std::string(NodeProps::kVariableName), "Second");
    Graph().FindNode(setSecond)->m_PinDefaults["Value"] = PinValue::MakeBool(true);

    Graph().AddLink(begin, "Then", sequence, "Enter");
    Graph().AddLink(sequence, "Then 0", delay, "Enter");
    Graph().AddLink(delay, "Completed", afterDelay, "Enter");
    Graph().AddLink(sequence, "Then 1", setSecond, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    EXPECT_TRUE(m_Instance.GetVariable("Second").AsBool()) << "branch 1 must not wait on branch 0's Delay";
    EXPECT_TRUE(m_Log.empty()) << "and branch 0 must genuinely be parked";
    EXPECT_EQ(m_Instance.GetPendingLatentCount(), 1u);
}

//==============================================================================
// Numeric narrowing
//==============================================================================

TEST_F(VisualScriptVMTest, OutOfRangeFloatToIntYieldsZeroRatherThanUndefinedBehaviour)
{
    // f32 reaches ~3.4e38; i64 stops at ~9.2e18. "It is finite" was the old guard,
    // and a finite-but-huge value made the cast undefined behaviour.
    EXPECT_EQ(PinValue::MakeFloat(3.0e38f).AsInt(), 0);
    EXPECT_EQ(PinValue::MakeFloat(-3.0e38f).AsInt(), 0);

    // The string path reaches the same helper only when the INTEGER parse fails
    // outright. std::from_chars' integer overload succeeds on a prefix, so "1e38"
    // reads as 1 and "3.7" as 3 without ever consulting the float fallback —
    // pinned here because it is surprising, and because a future rewrite of
    // IntFromStorage that "fixed" it would silently change stored values.
    EXPECT_EQ(PinValue::MakeString("1e38").ConvertTo(PinType::Int).AsInt(), 1);
    EXPECT_EQ(PinValue::MakeString("3.7").ConvertTo(PinType::Int).AsInt(), 3);
    // A leading '.' does fail the integer parse, so this one goes through the
    // float fallback with a value ~19 orders of magnitude past i64.
    EXPECT_EQ(PinValue::MakeString(".5e38").ConvertTo(PinType::Int).AsInt(), 0)
        << "the float fallback must refuse an out-of-range value rather than cast it";

    // In-range values still truncate toward zero exactly as before.
    EXPECT_EQ(PinValue::MakeFloat(3.7f).AsInt(), 3);
    EXPECT_EQ(PinValue::MakeFloat(-3.7f).AsInt(), -3);
    EXPECT_EQ(PinValue::MakeString(".5").ConvertTo(PinType::Int).AsInt(), 0);
}

TEST_F(VisualScriptVMTest, IsRepresentableAsFloatRejectsWhatNarrowingCannotHold)
{
    EXPECT_TRUE(IsRepresentableAsFloat(0.0));
    EXPECT_TRUE(IsRepresentableAsFloat(-1.5));
    EXPECT_TRUE(IsRepresentableAsFloat(static_cast<f64>(std::numeric_limits<f32>::max())));
    EXPECT_FALSE(IsRepresentableAsFloat(1.0e300)) << "finite, but narrowing it to f32 is undefined behaviour";
    EXPECT_FALSE(IsRepresentableAsFloat(-1.0e300));
    EXPECT_FALSE(IsRepresentableAsFloat(std::numeric_limits<f64>::infinity()));
    EXPECT_FALSE(IsRepresentableAsFloat(std::numeric_limits<f64>::quiet_NaN()));
}

TEST_F(VisualScriptVMTest, EventsOnlyFireAfterBeginPlay)
{
    AddVariable("Count", PinType::Float, PinValue::MakeFloat(0.0f));
    BuildCounterGraph(Graph());
    ASSERT_TRUE(Instantiate());

    RuntimeContext runtime = MakeRuntime();
    m_Instance.Tick(runtime); // before BeginPlay
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 0.0f)
        << "OnUpdate must not run before OnBeginPlay has";

    m_Instance.BeginPlay(runtime);
    m_Instance.Tick(runtime);
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Count").AsFloat(), 1.0f);
}

TEST_F(VisualScriptVMTest, PublishEventReachesTheOutboxRatherThanDispatchingInline)
{
    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId publish = Graph().AddNode("Utility.PublishEvent").m_Id;
    Graph().FindNode(publish)->m_PinDefaults["Event Name"] = PinValue::MakeString("Ping");
    Graph().AddLink(begin, "Then", publish, "Enter");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    // The outbox — not a synchronous dispatch — is what keeps a graph from
    // publishing to GameplayEventBus in the middle of an entity iteration.
    ASSERT_EQ(m_Outbox.size(), 1u);
    EXPECT_EQ(m_Outbox[0].m_Name, "Ping");
}

TEST_F(VisualScriptVMTest, WaitForEventPublishesTheArrivingPayload)
{
    // Regression: resuming a latent jumps straight to the resume pin's targets and
    // never re-runs the node body, so the node's own "Payload" output has exactly
    // one chance to be filled — at resume time. It used to stay empty forever,
    // which made the documented output silently dead.
    AddVariable("Heard", PinType::String, PinValue::MakeString(""));

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId wait = Graph().AddNode("Flow.WaitForEvent").m_Id;
    Graph().FindNode(wait)->m_PinDefaults["Event Name"] = PinValue::MakeString("Go");
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Heard");

    Graph().AddLink(begin, "Then", wait, "Enter");
    Graph().AddLink(wait, "Resumed", set, "Enter");
    Graph().AddLink(wait, "Payload", set, "Value");

    ASSERT_TRUE(Instantiate());
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);

    IncomingEvent go;
    go.m_Key = VisualScriptPlan::MakeEventKey("Custom", "Go");
    go.m_Payload = PinValue::MakeString("hello");
    m_Instance.DispatchEvent(go, runtime);

    EXPECT_EQ(m_Instance.GetVariable("Heard").AsString(), "hello")
        << "the payload the event carried must reach the waiting node's Payload output";
}

TEST_F(VisualScriptVMTest, TwoFunctionsResolveTheirOwnSignatures)
{
    // Regression: NodeIds are graph-LOCAL, so every function graph's Entry node has
    // id 1. Resolving Entry/Return pins by id therefore gave the SECOND function
    // the FIRST function's parameter list — dangling links that killed the whole
    // asset, or silent argument mis-marshalling when the arities matched.
    AddVariable("Result", PinType::String, PinValue::MakeString(""));

    VisualScriptGraph& first = m_Asset->m_Functions.emplace_back();
    first.m_Name = "TakesFloat";
    first.m_Inputs.push_back({ "Amount", PinType::Float, PinValue::MakeFloat(0.0f) });
    first.m_Outputs.push_back({ "Doubled", PinType::Float, PinValue::MakeFloat(0.0f) });
    {
        const NodeId entry = first.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
        const NodeId mul = first.AddNode("Math.Multiply").m_Id;
        first.FindNode(mul)->m_PinDefaults["B"] = PinValue::MakeFloat(2.0f);
        const NodeId ret = first.AddNode(std::string(NodeTypes::kFunctionReturn)).m_Id;
        first.AddLink(entry, "Then", ret, "Enter");
        first.AddLink(entry, "Amount", mul, "A");
        first.AddLink(mul, "Result", ret, "Doubled");
    }

    // Deliberately a DIFFERENT signature, with pin names the first function does
    // not have — an id-based lookup cannot resolve these against the first graph.
    VisualScriptGraph& second = m_Asset->m_Functions.emplace_back();
    second.m_Name = "TakesString";
    second.m_Inputs.push_back({ "Label", PinType::String, PinValue::MakeString("") });
    second.m_Outputs.push_back({ "Echo", PinType::String, PinValue::MakeString("") });
    {
        const NodeId entry = second.AddNode(std::string(NodeTypes::kFunctionEntry)).m_Id;
        const NodeId ret = second.AddNode(std::string(NodeTypes::kFunctionReturn)).m_Id;
        second.AddLink(entry, "Then", ret, "Enter");
        second.AddLink(entry, "Label", ret, "Echo");
    }

    const NodeId begin = Graph().AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
    const NodeId call = Graph().AddNode(std::string(NodeTypes::kFunctionCall)).m_Id;
    Graph().FindNode(call)->SetProperty(std::string(NodeProps::kFunctionName), "TakesString");
    Graph().FindNode(call)->m_PinDefaults["Label"] = PinValue::MakeString("second");
    const NodeId set = Graph().AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    Graph().FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Result");

    Graph().AddLink(begin, "Then", call, "Enter");
    Graph().AddLink(call, "Then", set, "Enter");
    Graph().AddLink(call, "Echo", set, "Value");

    ASSERT_TRUE(Instantiate()) << (m_Errors.empty() ? "compile failed with no diagnostics" : m_Errors[0].m_Message);
    RuntimeContext runtime = MakeRuntime();
    m_Instance.BeginPlay(runtime);
    EXPECT_EQ(m_Instance.GetVariable("Result").AsString(), "second")
        << "the second function must resolve its OWN parameters, not the first function's";
}

TEST_F(VisualScriptVMTest, VariableOverridesReplaceTheAssetDefaults)
{
    AddVariable("Speed", PinType::Float, PinValue::MakeFloat(1.0f));
    ASSERT_TRUE(Instantiate());
    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Speed").AsFloat(), 1.0f);

    std::map<std::string, PinValue> overrides;
    overrides["Speed"] = PinValue::MakeFloat(9.5f);
    // An override naming a variable that no longer exists must be tolerated —
    // it is what a rename leaves behind, and it should not break the entity.
    overrides["Gone"] = PinValue::MakeFloat(1.0f);
    m_Instance.ApplyVariableOverrides(overrides);

    EXPECT_FLOAT_EQ(m_Instance.GetVariable("Speed").AsFloat(), 9.5f);
}
