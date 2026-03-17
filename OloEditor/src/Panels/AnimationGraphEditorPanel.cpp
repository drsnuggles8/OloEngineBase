#include "AnimationGraphEditorPanel.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>
#include <string>

namespace OloEngine
{
    AnimationGraphEditorPanel::AnimationGraphEditorPanel(const Ref<Scene>& context)
        : m_Context(context)
    {
    }

    void AnimationGraphEditorPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
    }

    void AnimationGraphEditorPanel::SetSelectedEntity(Entity entity)
    {
        m_SelectedEntity = entity;
    }

    void AnimationGraphEditorPanel::OnImGuiRender(bool* p_open)
    {
        if (!ImGui::Begin("Animation Graph Editor", p_open))
        {
            ImGui::End();
            return;
        }

        if (!m_SelectedEntity || !m_Context)
        {
            ImGui::TextDisabled("Select an entity with AnimationGraphComponent");
            ImGui::End();
            return;
        }

        if (!m_SelectedEntity.HasComponent<AnimationGraphComponent>())
        {
            if (ImGui::Button("Add Animation Graph Component"))
            {
                m_SelectedEntity.AddComponent<AnimationGraphComponent>();
            }
            ImGui::End();
            return;
        }

        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();

        // Initialize runtime graph if missing
        if (!graphComp.RuntimeGraph)
        {
            if (ImGui::Button("Create New Animation Graph"))
            {
                graphComp.RuntimeGraph = Ref<AnimationGraph>::Create();
                // Add a default base layer with an empty state machine
                AnimationLayer baseLayer;
                baseLayer.Name = "Base Layer";
                baseLayer.StateMachine = Ref<AnimationStateMachine>::Create();
                graphComp.RuntimeGraph->Layers.push_back(baseLayer);
            }
            ImGui::End();
            return;
        }

        auto& graph = graphComp.RuntimeGraph;

        // Tabs for different sections
        if (ImGui::BeginTabBar("AnimGraphTabs"))
        {
            if (ImGui::BeginTabItem("Parameters"))
            {
                DrawParameterEditor();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("States"))
            {
                DrawStateGraph();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Transitions"))
            {
                DrawTransitionEditor();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Blend Trees"))
            {
                DrawBlendTreeEditor();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Layers"))
            {
                DrawLayerManager();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Preview"))
            {
                DrawLivePreview();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void AnimationGraphEditorPanel::DrawParameterEditor()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;

        ImGui::Text("Parameters");
        ImGui::Separator();

        // List existing parameters
        std::string paramToRemove;
        for (auto& [name, param] : graph->Parameters.GetAll())
        {
            ImGui::PushID(name.c_str());

            // Parameter type label
            const char* typeLabel = "?";
            switch (param.ParamType)
            {
                case AnimationParameterType::Float:
                    typeLabel = "Float";
                    break;
                case AnimationParameterType::Int:
                    typeLabel = "Int";
                    break;
                case AnimationParameterType::Bool:
                    typeLabel = "Bool";
                    break;
                case AnimationParameterType::Trigger:
                    typeLabel = "Trigger";
                    break;
            }

            ImGui::Text("[%s] %s", typeLabel, name.c_str());
            ImGui::SameLine();

            // Editable value
            switch (param.ParamType)
            {
                case AnimationParameterType::Float:
                {
                    f32 val = param.FloatValue;
                    if (ImGui::DragFloat("##val", &val, 0.01f))
                    {
                        graph->Parameters.SetFloat(name, val);
                        graphComp.Parameters.SetFloat(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Int:
                {
                    i32 val = param.IntValue;
                    if (ImGui::DragInt("##val", &val))
                    {
                        graph->Parameters.SetInt(name, val);
                        graphComp.Parameters.SetInt(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Bool:
                {
                    bool val = param.BoolValue;
                    if (ImGui::Checkbox("##val", &val))
                    {
                        graph->Parameters.SetBool(name, val);
                        graphComp.Parameters.SetBool(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Trigger:
                {
                    if (ImGui::Button("Fire"))
                    {
                        graph->Parameters.SetTrigger(name);
                        graphComp.Parameters.SetTrigger(name);
                    }
                    break;
                }
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                paramToRemove = name;
            }

            ImGui::PopID();
        }

        if (!paramToRemove.empty())
        {
            graph->Parameters.RemoveParameter(paramToRemove);
            graphComp.Parameters.RemoveParameter(paramToRemove);
        }

        ImGui::Separator();

        // Add new parameter
        if (ImGui::Button("Add Parameter"))
        {
            m_ShowNewParamDialog = true;
            memset(m_NewParamName, 0, sizeof(m_NewParamName));
        }

        if (m_ShowNewParamDialog)
        {
            ImGui::InputText("Name", m_NewParamName, sizeof(m_NewParamName));
            const char* typeNames[] = { "Float", "Int", "Bool", "Trigger" };
            ImGui::Combo("Type", &m_NewParamType, typeNames, 4);

            if (m_NewParamType == 0)
                ImGui::DragFloat("Default", &m_NewParamDefaultFloat, 0.01f);
            else if (m_NewParamType == 1)
                ImGui::DragInt("Default", &m_NewParamDefaultInt);
            else if (m_NewParamType == 2)
                ImGui::Checkbox("Default", &m_NewParamDefaultBool);

            if (ImGui::Button("Create") && strlen(m_NewParamName) > 0)
            {
                std::string name = m_NewParamName;
                switch (m_NewParamType)
                {
                    case 0:
                        graph->Parameters.DefineFloat(name, m_NewParamDefaultFloat);
                        graphComp.Parameters.DefineFloat(name, m_NewParamDefaultFloat);
                        break;
                    case 1:
                        graph->Parameters.DefineInt(name, m_NewParamDefaultInt);
                        graphComp.Parameters.DefineInt(name, m_NewParamDefaultInt);
                        break;
                    case 2:
                        graph->Parameters.DefineBool(name, m_NewParamDefaultBool);
                        graphComp.Parameters.DefineBool(name, m_NewParamDefaultBool);
                        break;
                    case 3:
                        graph->Parameters.DefineTrigger(name);
                        graphComp.Parameters.DefineTrigger(name);
                        break;
                }
                m_ShowNewParamDialog = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_ShowNewParamDialog = false;
            }
        }
    }

    void AnimationGraphEditorPanel::DrawStateGraph()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;

        if (graph->Layers.empty())
        {
            ImGui::TextDisabled("No layers defined");
            return;
        }

        // Layer selector
        if (graph->Layers.size() > 1)
        {
            std::vector<const char*> layerNames;
            for (auto const& layer : graph->Layers)
            {
                layerNames.push_back(layer.Name.c_str());
            }
            ImGui::Combo("Layer", &m_SelectedLayerIndex, layerNames.data(), static_cast<int>(layerNames.size()));
        }

        if (m_SelectedLayerIndex < 0 || m_SelectedLayerIndex >= static_cast<i32>(graph->Layers.size()))
        {
            return;
        }

        auto& layer = graph->Layers[m_SelectedLayerIndex];
        auto& sm = layer.StateMachine;
        if (!sm)
        {
            return;
        }

        ImGui::Text("States (Default: %s)", sm->GetDefaultState().c_str());
        ImGui::Text("Current: %s %s", sm->GetCurrentStateName().c_str(),
                    sm->IsInTransition() ? "(transitioning)" : "");
        ImGui::Separator();

        // Draw states as selectable items
        for (auto const& [name, state] : sm->GetStates())
        {
            bool isSelected = (m_SelectedStateName == name);
            bool isCurrent = (sm->GetCurrentStateName() == name);
            bool isDefault = (sm->GetDefaultState() == name);

            std::string label = name;
            if (isCurrent)
                label += " [CURRENT]";
            if (isDefault)
                label += " [DEFAULT]";

            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_SelectedStateName = name;
            }
        }

        ImGui::Separator();

        // Add new state
        if (ImGui::Button("Add State"))
        {
            m_ShowNewStateDialog = true;
            memset(m_NewStateName, 0, sizeof(m_NewStateName));
        }

        if (m_ShowNewStateDialog)
        {
            ImGui::InputText("State Name", m_NewStateName, sizeof(m_NewStateName));
            ImGui::Checkbox("Is Blend Tree", &m_NewStateIsBlendTree);

            if (ImGui::Button("Create") && strlen(m_NewStateName) > 0)
            {
                AnimationState newState;
                newState.Name = m_NewStateName;
                newState.Type = m_NewStateIsBlendTree
                                    ? AnimationState::MotionType::BlendTree
                                    : AnimationState::MotionType::SingleClip;
                if (m_NewStateIsBlendTree)
                {
                    newState.Tree = Ref<BlendTree>::Create();
                }
                sm->AddState(newState);

                // Set as default if first state
                if (sm->GetStates().size() == 1)
                {
                    sm->SetDefaultState(newState.Name);
                }
                m_ShowNewStateDialog = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_ShowNewStateDialog = false;
            }
        }

        // State editor
        DrawStateEditor();
    }

    void AnimationGraphEditorPanel::DrawStateEditor()
    {
        if (m_SelectedStateName.empty())
        {
            return;
        }

        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;
        if (m_SelectedLayerIndex < 0 || m_SelectedLayerIndex >= static_cast<i32>(graph->Layers.size()))
        {
            return;
        }

        auto& sm = graph->Layers[m_SelectedLayerIndex].StateMachine;
        if (!sm)
        {
            return;
        }

        const auto* state = sm->GetState(m_SelectedStateName);
        if (!state)
        {
            return;
        }

        ImGui::Separator();
        ImGui::Text("Edit State: %s", m_SelectedStateName.c_str());

        // We need a mutable copy to edit - in production you'd modify via the state machine
        // For now, we display the state's properties
        ImGui::Text("Motion Type: %s",
                    state->Type == AnimationState::MotionType::SingleClip ? "Single Clip" : "Blend Tree");
        ImGui::Text("Speed: %.2f", state->Speed);
        ImGui::Text("Looping: %s", state->Looping ? "Yes" : "No");

        if (state->Type == AnimationState::MotionType::SingleClip)
        {
            ImGui::Text("Clip: %s", state->Clip ? state->Clip->Name.c_str() : "<none>");
        }

        if (ImGui::Button("Set as Default"))
        {
            sm->SetDefaultState(m_SelectedStateName);
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove State"))
        {
            sm->RemoveState(m_SelectedStateName);
            m_SelectedStateName.clear();
        }
    }

    void AnimationGraphEditorPanel::DrawTransitionEditor()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;

        if (graph->Layers.empty() || m_SelectedLayerIndex >= static_cast<i32>(graph->Layers.size()))
        {
            return;
        }

        auto& sm = graph->Layers[m_SelectedLayerIndex].StateMachine;
        if (!sm)
        {
            return;
        }

        ImGui::Text("Transitions");
        ImGui::Separator();

        auto const& transitions = sm->GetTransitions();
        for (i32 i = 0; i < static_cast<i32>(transitions.size()); ++i)
        {
            auto const& t = transitions[i];
            std::string label = (t.SourceState.empty() ? "*" : t.SourceState) + " -> " + t.DestinationState;

            bool isSelected = (m_SelectedTransitionIndex == i);
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_SelectedTransitionIndex = i;
            }
        }

        // Show selected transition details
        if (m_SelectedTransitionIndex >= 0 && m_SelectedTransitionIndex < static_cast<i32>(transitions.size()))
        {
            auto const& t = transitions[m_SelectedTransitionIndex];
            ImGui::Separator();
            ImGui::Text("From: %s", t.SourceState.empty() ? "Any State" : t.SourceState.c_str());
            ImGui::Text("To: %s", t.DestinationState.c_str());
            ImGui::Text("Blend Duration: %.2f s", t.BlendDuration);
            if (t.HasExitTime)
            {
                ImGui::Text("Exit Time: %.2f", t.ExitTime);
            }
            ImGui::Text("Conditions: %zu", t.Conditions.size());
            for (auto const& cond : t.Conditions)
            {
                const char* opStr = "?";
                switch (cond.Op)
                {
                    case TransitionCondition::Comparison::Greater:
                        opStr = ">";
                        break;
                    case TransitionCondition::Comparison::Less:
                        opStr = "<";
                        break;
                    case TransitionCondition::Comparison::Equal:
                        opStr = "==";
                        break;
                    case TransitionCondition::Comparison::NotEqual:
                        opStr = "!=";
                        break;
                    case TransitionCondition::Comparison::TriggerSet:
                        opStr = "Trigger";
                        break;
                }
                ImGui::BulletText("%s %s %.2f", cond.ParameterName.c_str(), opStr, cond.FloatThreshold);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Add Transition"))
        {
            AnimationTransition newTransition;
            newTransition.SourceState = "";
            newTransition.DestinationState = "";
            sm->AddTransition(newTransition);
        }
    }

    void AnimationGraphEditorPanel::DrawBlendTreeEditor()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;

        if (m_SelectedStateName.empty() || graph->Layers.empty())
        {
            ImGui::TextDisabled("Select a blend tree state to edit");
            return;
        }

        if (m_SelectedLayerIndex >= static_cast<i32>(graph->Layers.size()))
        {
            return;
        }

        auto& sm = graph->Layers[m_SelectedLayerIndex].StateMachine;
        if (!sm)
        {
            return;
        }

        const auto* state = sm->GetState(m_SelectedStateName);
        if (!state || state->Type != AnimationState::MotionType::BlendTree || !state->Tree)
        {
            ImGui::TextDisabled("Selected state is not a blend tree");
            return;
        }

        auto& tree = state->Tree;

        ImGui::Text("Blend Tree: %s", m_SelectedStateName.c_str());
        ImGui::Separator();

        // Blend type
        const char* blendTypes[] = { "Simple1D", "SimpleDirectional2D", "FreeformDirectional2D", "FreeformCartesian2D" };
        int typeIdx = static_cast<int>(tree->Type);
        ImGui::Text("Blend Type: %s", blendTypes[typeIdx]);
        ImGui::Text("Parameter X: %s", tree->BlendParameterX.c_str());
        if (tree->Type != BlendTree::BlendType::Simple1D)
        {
            ImGui::Text("Parameter Y: %s", tree->BlendParameterY.c_str());
        }

        ImGui::Separator();
        ImGui::Text("Children: %zu", tree->Children.size());

        // Visualize blend children
        for (sizet i = 0; i < tree->Children.size(); ++i)
        {
            auto const& child = tree->Children[i];
            if (tree->Type == BlendTree::BlendType::Simple1D)
            {
                ImGui::BulletText("Threshold: %.2f, Speed: %.1f, Clip: %s",
                                  child.Threshold, child.Speed,
                                  child.Clip ? child.Clip->Name.c_str() : "<none>");
            }
            else
            {
                ImGui::BulletText("Pos: (%.2f, %.2f), Speed: %.1f, Clip: %s",
                                  child.Position.x, child.Position.y, child.Speed,
                                  child.Clip ? child.Clip->Name.c_str() : "<none>");
            }
        }

        // 1D visualization bar
        if (tree->Type == BlendTree::BlendType::Simple1D && !tree->Children.empty())
        {
            ImGui::Separator();
            ImGui::Text("1D Blend Space:");
            f32 minT = tree->Children.front().Threshold;
            f32 maxT = tree->Children.back().Threshold;
            f32 range = maxT - minT;
            if (range > 0.0f)
            {
                f32 currentParam = graphComp.Parameters.GetFloat(tree->BlendParameterX);
                f32 normalizedPos = (currentParam - minT) / range;
                normalizedPos = glm::clamp(normalizedPos, 0.0f, 1.0f);

                ImVec2 barStart = ImGui::GetCursorScreenPos();
                f32 barWidth = ImGui::GetContentRegionAvail().x;
                f32 barHeight = 20.0f;
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // Background bar
                drawList->AddRectFilled(barStart,
                                        ImVec2(barStart.x + barWidth, barStart.y + barHeight),
                                        IM_COL32(50, 50, 50, 255));

                // Child markers
                for (auto const& child : tree->Children)
                {
                    f32 pos = (child.Threshold - minT) / range;
                    f32 x = barStart.x + pos * barWidth;
                    drawList->AddLine(
                        ImVec2(x, barStart.y),
                        ImVec2(x, barStart.y + barHeight),
                        IM_COL32(200, 200, 200, 255), 2.0f);
                }

                // Current parameter position
                f32 cursorX = barStart.x + normalizedPos * barWidth;
                drawList->AddTriangleFilled(
                    ImVec2(cursorX - 5, barStart.y),
                    ImVec2(cursorX + 5, barStart.y),
                    ImVec2(cursorX, barStart.y + 8),
                    IM_COL32(255, 200, 0, 255));

                ImGui::Dummy(ImVec2(barWidth, barHeight + 5));
            }
        }
    }

    void AnimationGraphEditorPanel::DrawLayerManager()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();
        auto& graph = graphComp.RuntimeGraph;

        ImGui::Text("Animation Layers");
        ImGui::Separator();

        for (sizet i = 0; i < graph->Layers.size(); ++i)
        {
            auto& layer = graph->Layers[i];
            ImGui::PushID(static_cast<int>(i));

            bool isSelected = (static_cast<i32>(i) == m_SelectedLayerIndex);
            if (ImGui::Selectable(layer.Name.c_str(), isSelected))
            {
                m_SelectedLayerIndex = static_cast<i32>(i);
            }

            if (isSelected)
            {
                ImGui::Indent();
                ImGui::Text("Blend Mode: %s",
                            layer.Mode == AnimationLayer::BlendMode::Override ? "Override" : "Additive");

                f32 weight = layer.Weight;
                if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f))
                {
                    layer.Weight = weight;
                }

                ImGui::Text("Affected Bones: %s",
                            layer.AffectedBones.empty() ? "All" : std::to_string(layer.AffectedBones.size()).c_str());

                if (layer.StateMachine)
                {
                    ImGui::Text("States: %zu", layer.StateMachine->GetStates().size());
                    ImGui::Text("Current: %s", layer.StateMachine->GetCurrentStateName().c_str());
                }

                ImGui::Unindent();
            }

            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Add Layer"))
        {
            AnimationLayer newLayer;
            newLayer.Name = "Layer " + std::to_string(graph->Layers.size());
            newLayer.StateMachine = Ref<AnimationStateMachine>::Create();
            graph->Layers.push_back(newLayer);
        }
    }

    void AnimationGraphEditorPanel::DrawLivePreview()
    {
        auto& graphComp = m_SelectedEntity.GetComponent<AnimationGraphComponent>();

        ImGui::Text("Live Preview - Parameter Sliders");
        ImGui::Separator();

        if (!graphComp.RuntimeGraph)
        {
            ImGui::TextDisabled("No runtime graph active");
            return;
        }

        // Show current state
        for (sizet i = 0; i < graphComp.RuntimeGraph->Layers.size(); ++i)
        {
            auto const& layer = graphComp.RuntimeGraph->Layers[i];
            if (layer.StateMachine)
            {
                ImGui::Text("Layer %zu [%s]: %s %s",
                            i, layer.Name.c_str(),
                            layer.StateMachine->GetCurrentStateName().c_str(),
                            layer.StateMachine->IsInTransition() ? "(transitioning)" : "");
            }
        }

        ImGui::Separator();

        // Parameter sliders for live control
        for (auto const& [name, param] : graphComp.Parameters.GetAll())
        {
            ImGui::PushID(name.c_str());
            switch (param.ParamType)
            {
                case AnimationParameterType::Float:
                {
                    f32 val = param.FloatValue;
                    if (ImGui::SliderFloat(name.c_str(), &val, -1.0f, 2.0f))
                    {
                        graphComp.Parameters.SetFloat(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Int:
                {
                    i32 val = param.IntValue;
                    if (ImGui::SliderInt(name.c_str(), &val, -10, 10))
                    {
                        graphComp.Parameters.SetInt(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Bool:
                {
                    bool val = param.BoolValue;
                    if (ImGui::Checkbox(name.c_str(), &val))
                    {
                        graphComp.Parameters.SetBool(name, val);
                    }
                    break;
                }
                case AnimationParameterType::Trigger:
                {
                    ImGui::Text("%s", name.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Fire"))
                    {
                        graphComp.Parameters.SetTrigger(name);
                    }
                    break;
                }
            }
            ImGui::PopID();
        }
    }
} // namespace OloEngine
