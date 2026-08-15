#include "OloEnginePCH.h"
#include "VisualScriptEditorPanel.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSystem.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glm/gtc/type_ptr.hpp>

// =============================================================================
// The Visual Script editor's left column: blackboard variables, the selected
// node's properties and pin literals, and the live debugger.
//
// Split from VisualScriptEditorPanel.cpp purely for file size — the canvas half
// and the inspector half share no state beyond the panel's members, and one
// 1,600-line panel file is exactly the shape the repo's other seven graph
// editors already regret.
// =============================================================================

namespace OloEngine
{
    using namespace OloEngine::VisualScript;

    namespace
    {
        constexpr std::array<PinType, 9> kAuthorablePinTypes = {
            PinType::Bool, PinType::Int, PinType::Float, PinType::Vec2, PinType::Vec3,
            PinType::Vec4, PinType::String, PinType::Entity, PinType::Asset
        };

        /// What one frame of an editing widget did.
        struct EditOutcome
        {
            /// The widget took focus this frame — the caller's cue to snapshot
            /// the PRE-edit state for undo, before any write lands.
            bool m_Activated = false;
            /// `value` was written this frame. Drag widgets report this on every
            /// frame of the drag so the edit is visible while dragging.
            bool m_Changed = false;
            /// The edit finished (focus left the widget). One undo entry per
            /// commit, not one per frame.
            bool m_Committed = false;
        };

        /// One editable widget for a PinValue of any type.
        ///
        /// Drag widgets write through on EVERY change rather than only at
        /// deactivation: the local copy is re-read from `value` each frame, so
        /// deferring the write made a drag look completely frozen until release.
        EditOutcome EditPinValue(const char* id, PinValue& value)
        {
            ImGui::PushID(id);
            EditOutcome outcome;

            switch (value.GetType())
            {
                case PinType::Bool:
                {
                    bool v = value.AsBool();
                    if (ImGui::Checkbox("##b", &v))
                    {
                        // A checkbox has no drag phase: the click both starts and
                        // finishes the edit.
                        outcome.m_Activated = true;
                        value = PinValue::MakeBool(v);
                        outcome.m_Changed = true;
                        outcome.m_Committed = true;
                    }
                    break;
                }
                case PinType::Int:
                {
                    i32 v = static_cast<i32>(value.AsInt());
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool changed = ImGui::DragInt("##i", &v);
                    outcome.m_Activated = ImGui::IsItemActivated();
                    if (changed)
                    {
                        value = PinValue::MakeInt(v);
                        outcome.m_Changed = true;
                    }
                    outcome.m_Committed = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case PinType::Float:
                {
                    f32 v = value.AsFloat();
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool changed = ImGui::DragFloat("##f", &v, 0.05f);
                    outcome.m_Activated = ImGui::IsItemActivated();
                    if (changed)
                    {
                        value = PinValue::MakeFloat(v);
                        outcome.m_Changed = true;
                    }
                    outcome.m_Committed = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case PinType::Vec2:
                {
                    glm::vec2 v = value.AsVec2();
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool changed = ImGui::DragFloat2("##v2", glm::value_ptr(v), 0.05f);
                    outcome.m_Activated = ImGui::IsItemActivated();
                    if (changed)
                    {
                        value = PinValue::MakeVec2(v);
                        outcome.m_Changed = true;
                    }
                    outcome.m_Committed = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case PinType::Vec3:
                {
                    glm::vec3 v = value.AsVec3();
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool changed = ImGui::DragFloat3("##v3", glm::value_ptr(v), 0.05f);
                    outcome.m_Activated = ImGui::IsItemActivated();
                    if (changed)
                    {
                        value = PinValue::MakeVec3(v);
                        outcome.m_Changed = true;
                    }
                    outcome.m_Committed = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case PinType::Vec4:
                {
                    glm::vec4 v = value.AsVec4();
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool changed = ImGui::DragFloat4("##v4", glm::value_ptr(v), 0.05f);
                    outcome.m_Activated = ImGui::IsItemActivated();
                    if (changed)
                    {
                        value = PinValue::MakeVec4(v);
                        outcome.m_Changed = true;
                    }
                    outcome.m_Committed = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case PinType::String:
                {
                    std::string text = value.AsString();
                    char buffer[256] = {};
                    // Truncate rather than overflow; 255 chars is far past any
                    // sane literal and the alternative is a stack smash.
                    std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##s", buffer, sizeof(buffer));
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        outcome.m_Activated = true;
                        value = PinValue::MakeString(buffer);
                        outcome.m_Changed = true;
                        outcome.m_Committed = true;
                    }
                    break;
                }
                case PinType::Entity:
                case PinType::Asset:
                {
                    // Shown as a signed decimal because a UUID is a full-range
                    // u64 and ImGui's integer widgets are 32-bit; the text field
                    // round-trips the exact value either way.
                    std::string text = std::to_string(static_cast<u64>(value.AsEntity()));
                    char buffer[32] = {};
                    std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##id", buffer, sizeof(buffer), ImGuiInputTextFlags_CharsDecimal);
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        outcome.m_Activated = true;
                        value = PinValue::FromStorageString(value.GetType(), buffer);
                        outcome.m_Changed = true;
                        outcome.m_Committed = true;
                    }
                    break;
                }
                case PinType::Exec:
                case PinType::Any:
                    ImGui::TextDisabled("—");
                    break;
            }

            ImGui::PopID();
            return outcome;
        }
    } // namespace

    void VisualScriptEditorPanel::DrawSidebar()
    {
        ImGui::BeginChild("##vs_sidebar", ImVec2(s_SidebarWidth, 0.0f), ImGuiChildFlags_Borders);

        //-- Graph selector --------------------------------------------------------
        ImGui::SeparatorText("Graphs");
        if (ImGui::Selectable("Event Graph", m_ActiveFunction < 0))
        {
            m_ActiveFunction = -1;
            m_Selection.clear();
            m_DetailsNode = kInvalidNodeId;
        }
        for (sizet i = 0; i < m_Asset->m_Functions.size(); ++i)
        {
            ImGui::PushID(static_cast<i32>(i));
            if (ImGui::Selectable(m_Asset->m_Functions[i].m_Name.c_str(), m_ActiveFunction == static_cast<i32>(i)))
            {
                m_ActiveFunction = static_cast<i32>(i);
                m_Selection.clear();
                m_DetailsNode = kInvalidNodeId;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Function"))
        {
            PushUndo();
            VisualScriptGraph& function = m_Asset->m_Functions.emplace_back();
            function.m_Name = "Function" + std::to_string(m_Asset->m_Functions.size());
            // Both nodes up front: a function graph with no Entry cannot be
            // called, and the compiler rejects the Call rather than the function,
            // which reads as an error in the wrong place.
            function.AddNode(std::string(NodeTypes::kFunctionEntry), { 60.0f, 60.0f });
            function.AddNode(std::string(NodeTypes::kFunctionReturn), { 460.0f, 60.0f });
            m_ActiveFunction = static_cast<i32>(m_Asset->m_Functions.size()) - 1;
            Compile();
        }

        DrawVariablesSection();
        DrawNodeDetailsSection();
        DrawDebugSection();

        ImGui::EndChild();
    }

    void VisualScriptEditorPanel::DrawVariablesSection()
    {
        ImGui::SeparatorText("Variables");

        i32 removeIndex = -1;
        for (sizet i = 0; i < m_Asset->m_Variables.size(); ++i)
        {
            VisualScriptVariable& variable = m_Asset->m_Variables[i];
            ImGui::PushID(static_cast<i32>(i) + 1000);

            char name[64] = {};
            std::strncpy(name, variable.m_Name.c_str(), sizeof(name) - 1);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputText("##name", name, sizeof(name));
            if (ImGui::IsItemDeactivatedAfterEdit() && name[0] != '\0' && variable.m_Name != name)
            {
                PushUndo();
                // Rename every accessor that referenced the old name in the same
                // edit. Leaving them pointing at a name that no longer exists
                // would compile fine and fail at runtime with "Unknown variable".
                const std::string oldName = variable.m_Name;
                const std::string newName = name;
                const auto renameIn = [&](VisualScriptGraph& graph)
                {
                    for (VisualScriptNode& node : graph.m_Nodes)
                    {
                        if (node.m_TypeName != NodeTypes::kGetVariable && node.m_TypeName != NodeTypes::kSetVariable)
                            continue;
                        static const std::string s_Empty;
                        if (node.GetProperty(NodeProps::kVariableName, s_Empty) == oldName)
                            node.SetProperty(std::string(NodeProps::kVariableName), newName);
                    }
                };
                renameIn(m_Asset->m_EventGraph);
                for (VisualScriptGraph& function : m_Asset->m_Functions)
                    renameIn(function);

                m_Asset->m_Variables[i].m_Name = newName;
                Compile();
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::BeginCombo("##type", PinTypeToString(variable.m_Type)))
            {
                for (const PinType type : kAuthorablePinTypes)
                {
                    if (ImGui::Selectable(PinTypeToString(type), type == variable.m_Type))
                    {
                        PushUndo();
                        m_Asset->m_Variables[i].m_Type = type;
                        // Convert rather than reset: retyping Float->Int should
                        // keep 3.0 as 3, not silently zero the author's value.
                        m_Asset->m_Variables[i].m_DefaultValue = m_Asset->m_Variables[i].m_DefaultValue.ConvertTo(type);
                        Compile();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
            {
                removeIndex = static_cast<i32>(i);
            }

            ImGui::Indent();
            PinValue value = variable.m_DefaultValue;
            // Snapshot on ACTIVATION, write on every change. Pushing undo at
            // commit time would capture the post-edit state (the value has
            // already been written by then), so the undo would restore nothing.
            if (const EditOutcome outcome = EditPinValue("default", value); outcome.m_Activated || outcome.m_Changed)
            {
                if (outcome.m_Activated)
                {
                    PushUndo();
                }
                m_Asset->m_Variables[i].m_DefaultValue = value;
            }
            ImGui::Unindent();
            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
            PushUndo();
            m_Asset->m_Variables.erase(m_Asset->m_Variables.begin() + removeIndex);
            Compile();
        }

        if (ImGui::SmallButton("+ Variable"))
        {
            PushUndo();
            VisualScriptVariable& variable = m_Asset->m_Variables.emplace_back();
            variable.m_Name = "Variable" + std::to_string(m_Asset->m_Variables.size());
            variable.m_Type = PinType::Float;
            variable.m_DefaultValue = PinValue::MakeFloat(0.0f);
            Compile();
        }
    }

    void VisualScriptEditorPanel::DrawNodeDetailsSection()
    {
        ImGui::SeparatorText("Node");
        if (m_DetailsNode == kInvalidNodeId)
        {
            ImGui::TextDisabled("Select a node.");
            return;
        }

        VisualScriptGraph& graph = ActiveGraph();
        VisualScriptNode* node = graph.FindNode(m_DetailsNode);
        if (node == nullptr)
        {
            m_DetailsNode = kInvalidNodeId;
            return;
        }

        const NodeTypeDescriptor* type = NodeRegistry::Get().Find(node->m_TypeName);
        ImGui::TextUnformatted(type != nullptr ? type->m_DisplayName.c_str() : node->m_TypeName.c_str());
        if (type != nullptr && !type->m_Tooltip.empty())
        {
            ImGui::TextWrapped("%s", type->m_Tooltip.c_str());
        }
        ImGui::TextDisabled("%s  ·  id %u", node->m_TypeName.c_str(), node->m_Id);

        //-- Authoring properties --------------------------------------------------
        // Only the handful of node types whose SIGNATURE depends on authoring get
        // an editor; a generic string-map editor would let anyone type a key that
        // no resolver reads.
        static const std::string s_Empty;
        if (node->m_TypeName == NodeTypes::kCustomEvent || node->m_TypeName == NodeTypes::kOnGameplayEvent)
        {
            char buffer[96] = {};
            std::strncpy(buffer, node->GetProperty(NodeProps::kEventName, s_Empty).c_str(), sizeof(buffer) - 1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("Event", buffer, sizeof(buffer));
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                PushUndo();
                graph.FindNode(m_DetailsNode)->SetProperty(std::string(NodeProps::kEventName), buffer);
                Compile();
            }
        }
        else if (node->m_TypeName == NodeTypes::kGetVariable || node->m_TypeName == NodeTypes::kSetVariable)
        {
            const std::string current = node->GetProperty(NodeProps::kVariableName, s_Empty);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("Variable", current.c_str()))
            {
                for (const VisualScriptVariable& variable : m_Asset->m_Variables)
                {
                    if (ImGui::Selectable(variable.m_Name.c_str(), variable.m_Name == current))
                    {
                        PushUndo();
                        graph.FindNode(m_DetailsNode)->SetProperty(std::string(NodeProps::kVariableName), variable.m_Name);
                        Compile();
                    }
                }
                ImGui::EndCombo();
            }
        }
        else if (node->m_TypeName == NodeTypes::kSequence)
        {
            i32 count = std::atoi(node->GetProperty(NodeProps::kOutputCount, s_Empty).c_str());
            count = count <= 0 ? 2 : count;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("Outputs", &count, 2, 16))
            {
                PushUndo();
                // Changing the pin count can orphan wires on the removed pins.
                // The compiler reports them as dangling, which is the honest
                // outcome — silently deleting the author's wires is worse.
                graph.FindNode(m_DetailsNode)->SetProperty(std::string(NodeProps::kOutputCount), std::to_string(count));
                Compile();
            }
        }
        else if (node->m_TypeName == NodeTypes::kFunctionCall)
        {
            const std::string current = node->GetProperty(NodeProps::kFunctionName, s_Empty);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("Function", current.c_str()))
            {
                for (const VisualScriptGraph& function : m_Asset->m_Functions)
                {
                    if (ImGui::Selectable(function.m_Name.c_str(), function.m_Name == current))
                    {
                        PushUndo();
                        graph.FindNode(m_DetailsNode)->SetProperty(std::string(NodeProps::kFunctionName), function.m_Name);
                        Compile();
                    }
                }
                ImGui::EndCombo();
            }
        }

        //-- Unconnected input literals --------------------------------------------
        if (type == nullptr)
        {
            return;
        }
        const std::vector<PinDescriptor> pins = type->m_ResolvePins ? type->m_ResolvePins(*node, *m_Asset) : type->m_Pins;

        ImGui::SeparatorText("Inputs");
        bool anyEditable = false;
        for (const PinDescriptor& pin : pins)
        {
            if (pin.m_Direction != PinDirection::Input || IsExecPin(pin.m_Type))
            {
                continue;
            }
            // A connected pin's literal is dead data — showing an editable value
            // the graph will never read is actively misleading.
            const bool connected = std::ranges::any_of(graph.m_Links, [&](const VisualScriptLink& link)
                                                       { return link.m_TargetNode == node->m_Id && link.m_TargetPin == pin.m_Name; });
            anyEditable = true;
            ImGui::PushID(pin.m_Name.c_str());
            ImGui::TextUnformatted(pin.m_Name.c_str());
            if (connected)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(connected)");
            }
            else
            {
                const auto existing = node->m_PinDefaults.find(pin.m_Name);
                PinValue value = existing != node->m_PinDefaults.end() ? existing->second.ConvertTo(pin.m_Type)
                                                                       : pin.m_DefaultValue;
                if (const EditOutcome outcome = EditPinValue("v", value); outcome.m_Activated || outcome.m_Changed)
                {
                    if (outcome.m_Activated)
                    {
                        PushUndo();
                    }
                    graph.FindNode(m_DetailsNode)->m_PinDefaults[pin.m_Name] = value;
                }
            }
            ImGui::PopID();
        }
        if (!anyEditable)
        {
            ImGui::TextDisabled("No data inputs.");
        }
    }

    VisualScriptInstance* VisualScriptEditorPanel::ResolveDebugInstance()
    {
        if (!m_Scene || !m_DebugEntity)
        {
            return nullptr;
        }
        auto* system = m_Scene->GetVisualScripts();
        if (system == nullptr)
        {
            return nullptr;
        }
        VisualScriptInstance* instance = system->FindInstanceForDebug(m_DebugEntity.GetUUID());
        if (instance == nullptr)
        {
            return nullptr;
        }

        // The panel is the only thing that ever turns debugging on, and it pushes
        // the current state every frame — so closing the panel, deselecting the
        // entity or stopping play leaves no armed breakpoints behind.
        instance->Debug().m_TraceEnabled = m_TraceEnabled;
        instance->Debug().m_Breakpoints = m_Breakpoints;
        return instance;
    }

    void VisualScriptEditorPanel::DrawDebugSection()
    {
        ImGui::SeparatorText("Debugger");

        VisualScriptInstance* instance = ResolveDebugInstance();
        if (instance == nullptr)
        {
            ImGui::TextDisabled("Not running.");
            ImGui::TextWrapped("Press Play and select an entity with a VisualScriptComponent to watch it execute.");
            if (!m_Breakpoints.empty())
            {
                ImGui::Text("%d breakpoint(s) armed", static_cast<i32>(m_Breakpoints.size()));
                if (ImGui::SmallButton("Clear breakpoints"))
                    m_Breakpoints.clear();
            }
            ImGui::TextDisabled("Alt+click a node to toggle a breakpoint.");
            return;
        }

        const DebugState& debug = instance->Debug();
        if (debug.m_Paused)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "PAUSED at node %u",
                               static_cast<u32>(debug.m_PausedAt & 0xFFFFFFFFull));
            ImGui::TextWrapped("The node has NOT run yet — the pin values below are its inputs.");
            if (ImGui::Button("Resume"))
                instance->DebugResume();
            ImGui::SameLine();
            if (ImGui::Button("Step Tick"))
                instance->DebugStepOneTick();
            if (ImGui::IsItemHovered())
            {
                // Say plainly what this does, because "step" in a node editor
                // usually means one NODE. Exec descent has no continuation to
                // resume from, so node-granular stepping would mean re-running
                // the tick and repeating every side effect before the breakpoint.
                ImGui::SetTooltip("Runs exactly one more tick with breakpoints suppressed, then pauses again.\n"
                                  "Node-by-node stepping is not supported: the VM has no resumable continuation.");
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Running");
            ImGui::Text("%u node(s) last tick", instance->GetNodesExecutedThisTick());
            if (instance->DidExceedBudget())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Node budget exhausted!");
            }
        }

        ImGui::Checkbox("Trace", &m_TraceEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Pin values", &m_ShowPinValues);
        if (!m_Breakpoints.empty() && ImGui::SmallButton("Clear breakpoints"))
        {
            m_Breakpoints.clear();
        }

        //-- Live blackboard -------------------------------------------------------
        ImGui::SeparatorText("Live variables");
        for (const VisualScriptVariable& variable : m_Asset->m_Variables)
        {
            bool found = false;
            const PinValue value = instance->GetVariable(variable.m_Name, &found);
            if (found)
            {
                ImGui::Text("%s = %s", variable.m_Name.c_str(), value.AsString().c_str());
            }
            else
            {
                // The instance was compiled from an older version of the asset —
                // the author added this variable since Play started.
                ImGui::TextDisabled("%s (not in the running graph)", variable.m_Name.c_str());
            }
        }

        //-- Errors ----------------------------------------------------------------
        if (!instance->GetErrors().empty())
        {
            ImGui::SeparatorText("Runtime errors");
            for (const std::string& error : instance->GetErrors())
            {
                ImGui::TextWrapped("%s", error.c_str());
            }
        }
    }

} // namespace OloEngine
