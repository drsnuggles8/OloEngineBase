#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/VisualScript/ComponentFieldRegistry.h"

#include <optional>
#include <string>

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        //======================================================================
        // Get / Set arbitrary component field (issue #793).
        //
        // The component and field are node PROPERTIES, not input pins, and that
        // is the whole design. A pin would make them runtime strings, which in
        // turn would force the Value pin to stay PinType::Any forever — the
        // compiler could never type-check a wire into or out of these nodes, and
        // a typo in a field name would surface as a runtime error in a shipped
        // build instead of a red wire in the editor. As properties they are known
        // at compile time, so m_ResolvePins can give Value the field's real type
        // and CheckLinkCompatibility does its normal job. This mirrors the
        // variable accessors, which resolve against the blackboard the same way.
        //======================================================================

        /// The registry entry this node addresses, or null when either property is
        /// unset or names something the registry does not carry.
        const ComponentFieldEntry* EntryOf(const VisualScriptNode& node)
        {
            static const std::string s_Empty;
            const std::string& component = node.GetProperty(NodeProps::kComponentName, s_Empty);
            const std::string& field = node.GetProperty(NodeProps::kFieldName, s_Empty);
            if (component.empty() || field.empty())
                return nullptr;
            return ComponentFieldRegistry::Find(component, field);
        }

        /// Unresolved falls back to Any rather than to a guess: an unset node is a
        /// half-authored one, and Any lets the author wire it up and pick the field
        /// afterwards without the compiler rejecting the graph first.
        PinType FieldTypeOf(const VisualScriptNode& node)
        {
            const ComponentFieldEntry* entry = EntryOf(node);
            return entry == nullptr ? PinType::Any : entry->m_Type;
        }

        /// Resolve the node's target entity and its registry entry together, since
        /// every body needs both and each failure has its own message. Returns
        /// nullopt after reporting.
        struct Resolved
        {
            Entity m_Entity;
            const ComponentFieldEntry* m_Entry;
        };

        std::optional<Resolved> Resolve(const NodeContext& ctx, UUID target, const char* what)
        {
            const std::string component = ctx.Property(NodeProps::kComponentName);
            const std::string field = ctx.Property(NodeProps::kFieldName);
            if (component.empty() || field.empty())
            {
                ctx.Error(std::string(what) + " has no component/field selected");
                return std::nullopt;
            }

            const ComponentFieldEntry* entry = ComponentFieldRegistry::Find(component, field);
            if (entry == nullptr)
            {
                // Naming both halves matters: the overwhelmingly common cause is a
                // field that exists but is not exposed (a container, a Ref<T>, a
                // runtime-only field marked OLO_SERIALIZE(Skip)), and "unknown
                // field" alone sends the author looking for a typo instead.
                ctx.Error(std::string(what) + ": no exposed field '" + field + "' on '" + component +
                          "' — check the field picker for what this component exposes");
                return std::nullopt;
            }

            Scene* scene = ctx.GetScene();
            if (scene == nullptr)
            {
                ctx.Error(std::string(what) + " needs a Scene; none is attached");
                return std::nullopt;
            }
            std::optional<Entity> entity = scene->TryGetEntityWithUUID(target);
            if (!entity.has_value())
            {
                ctx.Error(std::string(what) + " targets entity " + std::to_string(static_cast<u64>(target)) +
                          ", which does not exist");
                return std::nullopt;
            }
            if (!entry->Has(*entity))
            {
                ctx.Error(std::string(what) + ": entity " + std::to_string(static_cast<u64>(target)) + " has no " +
                          entry->m_Component);
                return std::nullopt;
            }
            return Resolved{ *entity, entry };
        }
    } // namespace

    void RegisterComponentFieldNodes(NodeRegistry& registry)
    {
        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kGetComponentField);
            descriptor.m_DisplayName = "Get Component Field";
            descriptor.m_Category = "Entity";
            descriptor.m_Tooltip = "Reads one field of one component on the target entity. Pick the component and "
                                   "field in the node's details panel; the Value pin then takes that field's type.";
            descriptor.m_Pins = { In("Target", PinType::Entity), Out("Value", PinType::Any) };
            descriptor.m_Flags = NodeFlags::Pure;
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset&)
            {
                return std::vector<PinDescriptor>{ In("Target", PinType::Entity), Out("Value", FieldTypeOf(node)) };
            };
            descriptor.m_Evaluate = [](NodeContext& ctx)
            {
                const std::optional<Resolved> resolved = Resolve(ctx, ctx.GetInputEntity(0), "Get Component Field");
                if (!resolved.has_value())
                    return;
                ctx.SetOutput(1, resolved->m_Entry->Read(resolved->m_Entity));
            };
            (void)registry.Register(std::move(descriptor));
        }

        {
            NodeTypeDescriptor descriptor;
            descriptor.m_TypeName = std::string(NodeTypes::kSetComponentField);
            descriptor.m_DisplayName = "Set Component Field";
            descriptor.m_Category = "Entity";
            descriptor.m_Tooltip = "Writes one field of one component on the target entity, clamped to the same "
                                   "range a scene load enforces. Pick the component and field in the node's "
                                   "details panel.";
            descriptor.m_Pins = { ExecIn(), In("Target", PinType::Entity), In("Value", PinType::Any), ExecOut() };
            descriptor.m_ResolvePins = [](const VisualScriptNode& node, const VisualScriptAsset&)
            {
                return std::vector<PinDescriptor>{ ExecIn(), In("Target", PinType::Entity),
                                                   In("Value", FieldTypeOf(node)), ExecOut() };
            };
            descriptor.m_Execute = [](NodeContext& ctx)
            {
                // Trigger unconditionally, including on every failure path: a
                // reported error should not also silently sever the exec chain
                // behind it, or one bad field name stops the rest of the graph
                // with no second message to explain why.
                if (const std::optional<Resolved> resolved = Resolve(ctx, ctx.GetInputEntity(1), "Set Component Field");
                    resolved.has_value())
                {
                    if (resolved->m_Entry->Write(resolved->m_Entity, ctx.GetInput(2)) == FieldWriteResult::Rejected)
                    {
                        ctx.Error("Set Component Field: refusing to write a non-finite value into " +
                                  resolved->m_Entry->m_Component + "." + resolved->m_Entry->m_Field);
                    }
                }
                ctx.Trigger(3);
            };
            (void)registry.Register(std::move(descriptor));
        }
    }

} // namespace OloEngine::VisualScript
