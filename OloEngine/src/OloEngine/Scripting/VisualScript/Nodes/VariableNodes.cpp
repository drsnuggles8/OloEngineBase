#include "OloEnginePCH.h"
#include "NodeBuilders.h"

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // A variable accessor's pin TYPE is whatever the blackboard declares.
        // Resolving it here (rather than pinning everything to Any) is what makes
        // the compiler's link type-check meaningful for variable wires.
        PinType VariableTypeOf(const VisualScriptNode& node, const VisualScriptAsset& asset)
        {
            static const std::string s_Empty;
            const VisualScriptVariable* variable = asset.FindVariable(node.GetProperty(NodeProps::kVariableName, s_Empty));
            return variable == nullptr ? PinType::Any : variable->m_Type;
        }
    } // namespace

    void RegisterVariableNodes(NodeRegistry& registry)
    {
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kGetVariable);
            descriptor.m_DisplayName = "Get Variable";
            descriptor.m_Category = "Variables";
            descriptor.m_Tooltip = "Reads the blackboard variable named by this node's Variable property.";
            descriptor.m_Pins = { Out("Value", PinType::Any) };
            descriptor.m_Flags = NodeFlags::Pure;
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset& asset)
            {
                return std::vector<PinDescriptor>{ Out("Value", VariableTypeOf(node, asset)) };
            };
            descriptor.m_Evaluate = [](NodeContext& ctx)
            {
                bool found = false;
                const PinValue value = ctx.GetVariable(ctx.Property(NodeProps::kVariableName), &found);
                if (!found)
                {
                    ctx.Error("Unknown variable '" + ctx.Property(NodeProps::kVariableName) + "'");
                    return;
                }
                ctx.SetOutput(0, value);
            };
            (void)registry.Register(std::move(descriptor));
        }

        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kSetVariable);
            descriptor.m_DisplayName = "Set Variable";
            descriptor.m_Category = "Variables";
            descriptor.m_Tooltip = "Writes the blackboard variable named by this node's Variable property.";
            descriptor.m_Pins = { ExecIn(), In("Value", PinType::Any), ExecOut(), Out("Out", PinType::Any) };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset& asset)
            {
                const PinType type = VariableTypeOf(node, asset);
                return std::vector<PinDescriptor>{ ExecIn(), In("Value", type), ExecOut(), Out("Out", type) };
            };
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                const std::string name = ctx.Property(NodeProps::kVariableName);
                const PinValue value = ctx.GetInput(1);
                if (!ctx.SetVariable(name, value))
                {
                    ctx.Error("Unknown variable '" + name + "'");
                }
                // Pass-through output so a Set can be chained into further reads
                // without a second Get.
                ctx.SetOutput(3, value);
                ctx.Trigger(2);
            };
            (void)registry.Register(std::move(descriptor));
        }
    }

} // namespace OloEngine::VisualScript
