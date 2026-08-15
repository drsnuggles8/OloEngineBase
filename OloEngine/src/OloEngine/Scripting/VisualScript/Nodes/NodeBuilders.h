#pragma once

#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptTypes.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptVM.h"

#include <string>
#include <utility>
#include <vector>

namespace OloEngine::VisualScript::Builders
{
    //==============================================================================
    // Small helpers so a node declaration reads as its pin list. The registration
    // TUs are the bulk of the node library, and a descriptor spelled out longhand
    // is where a wrong PinDirection hides.
    //==============================================================================

    [[nodiscard]] inline PinDescriptor ExecIn(std::string name = "Enter")
    {
        return { std::move(name), PinType::Exec, PinDirection::Input };
    }

    [[nodiscard]] inline PinDescriptor ExecOut(std::string name = "Then")
    {
        return { std::move(name), PinType::Exec, PinDirection::Output };
    }

    [[nodiscard]] inline PinDescriptor In(std::string name, PinType type)
    {
        return { std::move(name), type, PinDirection::Input };
    }

    [[nodiscard]] inline PinDescriptor In(std::string name, PinValue defaultValue)
    {
        const PinType type = defaultValue.GetType();
        return { std::move(name), type, PinDirection::Input, std::move(defaultValue) };
    }

    [[nodiscard]] inline PinDescriptor Out(std::string name, PinType type)
    {
        return { std::move(name), type, PinDirection::Output };
    }

    /// An exec node: one Enter, one Then, plus whatever `pins` adds in between.
    inline void RegisterExecNode(NodeRegistry& registry, std::string typeName, std::string displayName,
                                 std::string category, std::string tooltip,
                                 std::vector<PinDescriptor> pins,
                                 std::function<void(NodeContext&)> execute,
                                 NodeFlags flags = NodeFlags::None)
    {
        NodeTypeDescriptor descriptor;
        descriptor.m_TypeName = std::move(typeName);
        descriptor.m_DisplayName = std::move(displayName);
        descriptor.m_Category = std::move(category);
        descriptor.m_Tooltip = std::move(tooltip);
        descriptor.m_Pins = std::move(pins);
        descriptor.m_Flags = flags;
        descriptor.m_Execute = std::move(execute);
        (void)registry.Register(std::move(descriptor));
    }

    /// A pure node: data pins only, pull-evaluated, no side effects.
    inline void RegisterPureNode(NodeRegistry& registry, std::string typeName, std::string displayName,
                                 std::string category, std::string tooltip,
                                 std::vector<PinDescriptor> pins,
                                 std::function<void(NodeContext&)> evaluate)
    {
        NodeTypeDescriptor descriptor;
        descriptor.m_TypeName = std::move(typeName);
        descriptor.m_DisplayName = std::move(displayName);
        descriptor.m_Category = std::move(category);
        descriptor.m_Tooltip = std::move(tooltip);
        descriptor.m_Pins = std::move(pins);
        descriptor.m_Flags = NodeFlags::Pure;
        descriptor.m_Evaluate = std::move(evaluate);
        (void)registry.Register(std::move(descriptor));
    }

    /// Binary float -> float. Covers most of the Math category.
    inline void RegisterFloatBinary(NodeRegistry& registry, std::string typeName, std::string displayName,
                                    std::string tooltip, f32 (*op)(f32, f32))
    {
        RegisterPureNode(registry, std::move(typeName), std::move(displayName), "Math", std::move(tooltip),
                         { In("A", PinValue::MakeFloat(0.0f)), In("B", PinValue::MakeFloat(0.0f)), Out("Result", PinType::Float) },
                         [op](NodeContext& ctx)
                         { ctx.SetOutput(2, PinValue::MakeFloat(op(ctx.GetInputFloat(0), ctx.GetInputFloat(1)))); });
    }

} // namespace OloEngine::VisualScript::Builders
