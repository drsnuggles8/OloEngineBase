#include "OloEnginePCH.h"
#include "NodeBuilders.h"

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Which graph a Function.Entry / Function.Return belongs to is not
        // something a node can ask — the resolver receives the ASSET, not the
        // graph. Both nodes therefore find their own graph by locating the one
        // that physically contains this node.
        //
        // Matched by ADDRESS, not by node id: NodeIds are graph-LOCAL (every
        // graph's m_NextNodeId starts at 1), so an id comparison makes the second
        // function's Entry resolve against the FIRST function's signature — pins
        // named for the wrong parameters, and either a dangling-link compile error
        // that kills the whole asset or, when the arities happen to match, silent
        // argument mis-marshalling. The compiler hands the resolver a reference to
        // the node living inside the graph's own vector, so identity is exact.
        const VisualScriptGraph* OwningFunctionGraph(const VisualScriptNode& node, const VisualScriptAsset& asset)
        {
            for (const VisualScriptGraph& graph : asset.m_Functions)
            {
                for (const VisualScriptNode& candidate : graph.m_Nodes)
                {
                    if (&candidate == &node)
                    {
                        return &graph;
                    }
                }
            }
            return nullptr;
        }

        const VisualScriptGraph* NamedFunctionGraph(const VisualScriptNode& node, const VisualScriptAsset& asset)
        {
            static const std::string s_Empty;
            return asset.FindFunction(node.GetProperty(NodeProps::kFunctionName, s_Empty));
        }
    } // namespace

    void RegisterFunctionNodes(NodeRegistry& registry)
    {
        //-- Entry ----------------------------------------------------------------
        // Pin layout contract, mirrored by Function.Call and NodeContext::
        // CallFunction: [0] Then, [1 + i] one output per function input.
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kFunctionEntry);
            descriptor.m_DisplayName = "Function Entry";
            descriptor.m_Category = "Functions";
            descriptor.m_Tooltip = "Where a sub-graph function starts. Its outputs are the function's parameters.";
            descriptor.m_Pins = { ExecOut() };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset& asset)
            {
                std::vector<PinDescriptor> pins{ ExecOut() };
                if (const VisualScriptGraph* graph = OwningFunctionGraph(node, asset); graph != nullptr)
                {
                    for (const VisualScriptVariable& parameter : graph->m_Inputs)
                    {
                        pins.push_back(Out(parameter.m_Name, parameter.m_Type));
                    }
                }
                return pins;
            };
            // The caller has already written the parameter values into this
            // node's output slots, so the body only has to continue.
            descriptor.m_Execute = [](NodeContext& ctx)
            { ctx.Trigger(0); };
            (void)registry.Register(std::move(descriptor));
        }

        //-- Return ---------------------------------------------------------------
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kFunctionReturn);
            descriptor.m_DisplayName = "Function Return";
            descriptor.m_Category = "Functions";
            descriptor.m_Tooltip = "Ends a sub-graph function. Its inputs are the function's results.";
            descriptor.m_Pins = { ExecIn() };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset& asset)
            {
                std::vector<PinDescriptor> pins{ ExecIn() };
                if (const VisualScriptGraph* graph = OwningFunctionGraph(node, asset); graph != nullptr)
                {
                    for (const VisualScriptVariable& result : graph->m_Outputs)
                    {
                        pins.push_back(In(result.m_Name, result.m_Type));
                    }
                }
                return pins;
            };
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                const sizet pinCount = ctx.GetCompiledNode().m_Pins.size();
                std::vector<PinValue> results;
                results.reserve(pinCount > 0 ? pinCount - 1 : 0);
                for (sizet pin = 1; pin < pinCount; ++pin)
                {
                    results.push_back(ctx.GetInput(pin));
                }
                ctx.PublishReturnValues(results);
            };
            (void)registry.Register(std::move(descriptor));
        }

        //-- Call -----------------------------------------------------------------
        // [0] Enter, [1 + i] parameter inputs, [1 + n] Then, [2 + n + j] results.
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kFunctionCall);
            descriptor.m_DisplayName = "Call Function";
            descriptor.m_Category = "Functions";
            descriptor.m_Tooltip = "Runs the sub-graph function named by this node's Function property.";
            descriptor.m_Pins = { ExecIn(), ExecOut() };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset& asset)
            {
                std::vector<PinDescriptor> pins{ ExecIn() };
                const VisualScriptGraph* graph = NamedFunctionGraph(node, asset);
                if (graph != nullptr)
                {
                    for (const VisualScriptVariable& parameter : graph->m_Inputs)
                    {
                        pins.push_back(In(parameter.m_Name, parameter.m_Type));
                    }
                }
                pins.push_back(ExecOut());
                if (graph != nullptr)
                {
                    for (const VisualScriptVariable& result : graph->m_Outputs)
                    {
                        pins.push_back(Out(result.m_Name, result.m_Type));
                    }
                }
                return pins;
            };
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                const bool called = ctx.CallFunction();
                if (!called)
                {
                    // Still continue: a graph whose author mistyped a function
                    // name should log and keep running, not silently stop the
                    // whole branch (which reads as an unrelated bug elsewhere).
                    ctx.Error("Function call did not run; continuing past it");
                }
                // The "Then" pin sits after Enter plus one pin per parameter.
                sizet thenPin = 0;
                const auto& pins = ctx.GetCompiledNode().m_Pins;
                for (sizet i = 1; i < pins.size(); ++i)
                {
                    if (IsExecPin(pins[i].m_Type) && pins[i].m_Direction == PinDirection::Output)
                    {
                        thenPin = i;
                        break;
                    }
                }
                if (thenPin != 0)
                {
                    ctx.Trigger(thenPin);
                }
            };
            (void)registry.Register(std::move(descriptor));
        }
    }

} // namespace OloEngine::VisualScript
