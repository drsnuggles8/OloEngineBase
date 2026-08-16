#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        constexpr i64 kMinSequenceOutputs = 2;
        constexpr i64 kMaxSequenceOutputs = 16;

        i64 ParseCount(const std::string& text, i64 fallback)
        {
            i64 value = fallback;
            if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{})
            {
                return fallback;
            }
            return value;
        }
    } // namespace

    void RegisterFlowNodes(NodeRegistry& registry)
    {
        //-- Branch ---------------------------------------------------------------
        RegisterExecNode(registry, "Flow.Branch", "Branch", "Flow",
                         "Takes True or False depending on the Condition input.",
                         { ExecIn(), In("Condition", PinValue::MakeBool(false)), ExecOut("True"), ExecOut("False") },
                         [](NodeContext& ctx)
                         { ctx.Trigger(ctx.GetInputBool(1) ? 2 : 3); });

        //-- Sequence -------------------------------------------------------------
        // The one node whose pin COUNT is authored. Its resolver reads the
        // "Outputs" property; the compiler calls it, so a saved graph's wires
        // resolve against exactly the pins the author saw.
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kSequence);
            descriptor.m_DisplayName = "Sequence";
            descriptor.m_Category = "Flow";
            descriptor.m_Tooltip = "Runs each output in order; each branch completes before the next begins.";
            descriptor.m_Pins = { ExecIn(), ExecOut("Then 0"), ExecOut("Then 1") };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset&)
            {
                static const std::string s_Two = "2";
                const i64 count = std::clamp(ParseCount(node.GetProperty(NodeProps::kOutputCount, s_Two), kMinSequenceOutputs),
                                             kMinSequenceOutputs, kMaxSequenceOutputs);
                std::vector<PinDescriptor> pins;
                pins.reserve(static_cast<sizet>(count) + 1);
                pins.push_back(ExecIn());
                for (i64 i = 0; i < count; ++i)
                {
                    pins.push_back(ExecOut("Then " + std::to_string(i)));
                }
                return pins;
            };
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                const sizet count = ctx.GetCompiledNode().m_Pins.size();
                for (sizet pin = 1; pin < count; ++pin)
                {
                    if (ctx.IsBudgetExhausted())
                    {
                        return;
                    }
                    ctx.Trigger(pin);
                }
            };
            (void)registry.Register(std::move(descriptor));
        }

        //-- For loop -------------------------------------------------------------
        RegisterExecNode(registry, "Flow.ForLoop", "For Loop", "Flow",
                         "Runs Loop Body once for each integer from First to Last inclusive, then Completed.",
                         { ExecIn(), In("First", PinValue::MakeInt(0)), In("Last", PinValue::MakeInt(0)),
                           ExecOut("Loop Body"), Out("Index", PinType::Int), ExecOut("Completed") },
                         [](NodeContext& ctx)
                         {
                             const i64 first = ctx.GetInputInt(1);
                             const i64 last = ctx.GetInputInt(2);
                             for (i64 i = first; i <= last; ++i)
                             {
                                 // Charge the iteration BEFORE running the body:
                                 // an empty body consumes nothing, so without
                                 // this a huge range would spin for free.
                                 if (!ctx.BeginIteration())
                                 {
                                     return;
                                 }
                                 ctx.SetOutput(4, PinValue::MakeInt(i));
                                 ctx.Trigger(3);
                             }
                             ctx.Trigger(5);
                         });

        //-- While loop -----------------------------------------------------------
        RegisterExecNode(registry, "Flow.WhileLoop", "While Loop", "Flow",
                         "Runs Loop Body while Condition is true. Bounded by the graph's per-tick node budget.",
                         { ExecIn(), In("Condition", PinValue::MakeBool(false)), ExecOut("Loop Body"), ExecOut("Completed") },
                         [](NodeContext& ctx)
                         {
                             while (ctx.GetInputBool(1))
                             {
                                 if (!ctx.BeginIteration())
                                 {
                                     return;
                                 }
                                 ctx.Trigger(2);
                             }
                             ctx.Trigger(3);
                         });

        //-- Gate -----------------------------------------------------------------
        RegisterExecNode(registry, "Flow.Gate", "Gate", "Flow",
                         "Passes Enter through to Exit only while open. Open/Close/Toggle change the state.",
                         { ExecIn("Enter"), ExecIn("Open"), ExecIn("Close"), ExecIn("Toggle"),
                           In("Start Closed", PinValue::MakeBool(true)), ExecOut("Exit") },
                         [](NodeContext& ctx)
                         {
                             NodeState& state = ctx.State();
                             if (state.m_Counter == 0)
                             {
                                 // m_Counter doubles as "have I read Start Closed
                                 // yet" — the pin is only meaningful once.
                                 state.m_Counter = 1;
                                 state.m_Flag = !ctx.GetInputBool(4);
                             }
                             switch (ctx.GetEntryPin())
                             {
                                 case 1:
                                     state.m_Flag = true;
                                     return;
                                 case 2:
                                     state.m_Flag = false;
                                     return;
                                 case 3:
                                     state.m_Flag = !state.m_Flag;
                                     return;
                                 default:
                                     break;
                             }
                             if (state.m_Flag)
                             {
                                 ctx.Trigger(5);
                             }
                         });

        //-- Do Once --------------------------------------------------------------
        RegisterExecNode(registry, "Flow.DoOnce", "Do Once", "Flow",
                         "Passes the first Enter through, then blocks until Reset.",
                         { ExecIn("Enter"), ExecIn("Reset"), ExecOut() },
                         [](NodeContext& ctx)
                         {
                             NodeState& state = ctx.State();
                             if (ctx.GetEntryPin() == 1)
                             {
                                 state.m_Flag = false;
                                 return;
                             }
                             if (state.m_Flag)
                             {
                                 return;
                             }
                             state.m_Flag = true;
                             ctx.Trigger(2);
                         });

        //-- Do N -----------------------------------------------------------------
        RegisterExecNode(registry, "Flow.DoN", "Do N", "Flow",
                         "Passes the first N Enters through, then blocks until Reset.",
                         { ExecIn("Enter"), ExecIn("Reset"), In("N", PinValue::MakeInt(1)),
                           ExecOut(), Out("Counter", PinType::Int) },
                         [](NodeContext& ctx)
                         {
                             NodeState& state = ctx.State();
                             if (ctx.GetEntryPin() == 1)
                             {
                                 state.m_Counter = 0;
                                 return;
                             }
                             if (state.m_Counter >= ctx.GetInputInt(2))
                             {
                                 return;
                             }
                             ++state.m_Counter;
                             ctx.SetOutput(4, PinValue::MakeInt(state.m_Counter));
                             ctx.Trigger(3);
                         });

        //-- Flip Flop ------------------------------------------------------------
        RegisterExecNode(registry, "Flow.FlipFlop", "Flip Flop", "Flow",
                         "Alternates between A and B on successive entries.",
                         { ExecIn(), ExecOut("A"), ExecOut("B"), Out("Is A", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             NodeState& state = ctx.State();
                             state.m_Flag = !state.m_Flag;
                             ctx.SetOutput(3, PinValue::MakeBool(state.m_Flag));
                             ctx.Trigger(state.m_Flag ? 1 : 2);
                         });

        //-- Delay (latent) -------------------------------------------------------
        RegisterExecNode(registry, "Flow.Delay", "Delay", "Flow", "Suspends this branch for Duration seconds of simulation time, then continues.", { ExecIn(), In("Duration", PinValue::MakeFloat(1.0f)), ExecOut("Completed") }, [](NodeContext& ctx)
                         {
                             const f32 duration = ctx.GetInputFloat(1);
                             if (duration <= 0.0f)
                             {
                                 // A zero/negative delay continues immediately
                                 // rather than parking a latent that expires on
                                 // the very next tick — a graph that Delays(0) in
                                 // a loop would otherwise advance one node a tick.
                                 ctx.Trigger(2);
                                 return;
                             }
                             ctx.SuspendForSeconds(2, duration); }, NodeFlags::Latent);

        //-- Wait For Event (latent) ----------------------------------------------
        RegisterExecNode(registry, "Flow.WaitForEvent", "Wait For Event", "Flow", "Suspends this branch until the named custom event is published to this entity.", { ExecIn(), In("Event Name", PinValue::MakeString({})), ExecOut("Resumed"), Out("Payload", PinType::Any) }, [](NodeContext& ctx)
                         {
                             const std::string name = ctx.GetInputString(1);
                             if (name.empty())
                             {
                                 ctx.Error("Wait For Event with an empty Event Name would never resume");
                                 return;
                             }
                             ctx.SetOutput(3, PinValue{});
                             ctx.SuspendForEvent(2, VisualScriptPlan::MakeEventKey("Custom", name)); }, NodeFlags::Latent);
    }

} // namespace OloEngine::VisualScript
