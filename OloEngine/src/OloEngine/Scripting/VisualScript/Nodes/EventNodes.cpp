#include "OloEnginePCH.h"
#include "NodeBuilders.h"

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Every event node has the same shape: publish whatever the dispatcher
        // carried into its data outputs, then hand control to its exec output.
        // The dispatcher (VisualScriptInstance::FireEntries) has already matched
        // the key, so the node itself never filters.
        void RegisterSimpleEvent(NodeRegistry& registry, std::string typeName, std::string displayName, std::string tooltip)
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::move(typeName);
            descriptor.m_DisplayName = std::move(displayName);
            descriptor.m_Category = "Events";
            descriptor.m_Tooltip = std::move(tooltip);
            descriptor.m_Pins = { ExecOut() };
            descriptor.m_Flags = NodeFlags::Event;
            descriptor.m_Execute = [](NodeContext& ctx)
            { ctx.Trigger(0); };
            (void)registry.Register(std::move(descriptor));
        }

        void RegisterOtherEntityEvent(NodeRegistry& registry, std::string typeName, std::string displayName, std::string tooltip)
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::move(typeName);
            descriptor.m_DisplayName = std::move(displayName);
            descriptor.m_Category = "Events";
            descriptor.m_Tooltip = std::move(tooltip);
            descriptor.m_Pins = { ExecOut(), Out("Other", PinType::Entity) };
            descriptor.m_Flags = NodeFlags::Event;
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                ctx.SetOutput(1, PinValue::MakeEntity(ctx.GetCurrentEventOther()));
                ctx.Trigger(0);
            };
            (void)registry.Register(std::move(descriptor));
        }
    } // namespace

    void RegisterEventNodes(NodeRegistry& registry)
    {
        RegisterSimpleEvent(registry, std::string(NodeTypes::kOnBeginPlay), "On Begin Play",
                            "Fires once, the first tick this entity's graph runs.");
        RegisterSimpleEvent(registry, std::string(NodeTypes::kOnEndPlay), "On End Play",
                            "Fires when the runtime stops or the component is removed.");

        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kOnUpdate);
            descriptor.m_DisplayName = "On Update";
            descriptor.m_Category = "Events";
            descriptor.m_Tooltip = "Fires every simulation tick, after On Begin Play has run.";
            descriptor.m_Pins = { ExecOut(), Out("Delta Seconds", PinType::Float) };
            descriptor.m_Flags = NodeFlags::Event;
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                ctx.SetOutput(1, PinValue::MakeFloat(ctx.GetDeltaTime()));
                ctx.Trigger(0);
            };
            (void)registry.Register(std::move(descriptor));
        }

        RegisterOtherEntityEvent(registry, std::string(NodeTypes::kOnCollisionEnter), "On Collision Enter",
                                 "Fires when this entity's rigid body begins touching another.");
        RegisterOtherEntityEvent(registry, std::string(NodeTypes::kOnTriggerEnter), "On Trigger Enter",
                                 "Fires when another entity enters this entity's trigger volume.");

        // Custom + bus events carry a payload and a sender. Their event KEY comes
        // from the node's "Event" property, which is why they need no filtering
        // here — the compiler put each one under its own entry key.
        const auto registerNamedEvent = [&registry](std::string typeName, std::string displayName, std::string tooltip)
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::move(typeName);
            descriptor.m_DisplayName = std::move(displayName);
            descriptor.m_Category = "Events";
            descriptor.m_Tooltip = std::move(tooltip);
            descriptor.m_Pins = { ExecOut(), Out("Payload", PinType::Any), Out("Sender", PinType::Entity) };
            descriptor.m_Flags = NodeFlags::Event;
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                ctx.SetOutput(1, ctx.GetCurrentEventPayload());
                ctx.SetOutput(2, PinValue::MakeEntity(ctx.GetCurrentEventOther()));
                ctx.Trigger(0);
            };
            (void)registry.Register(std::move(descriptor));
        };

        registerNamedEvent(std::string(NodeTypes::kCustomEvent), "Custom Event",
                           "Fires when Publish Event (or a C#/Lua script) sends the name in this node's Event property.");
        registerNamedEvent(std::string(NodeTypes::kOnGameplayEvent), "On Gameplay Event",
                           "Fires when the scene's GameplayEventBus publishes the named event (quest, inventory, progression).");
    }

} // namespace OloEngine::VisualScript
