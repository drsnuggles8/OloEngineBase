#include "OloEnginePCH.h"
#include "InputSettingsPanel.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Project/Project.h"
#include "../UndoRedo/SpecializedCommands.h"

#include <imgui.h>

#include <algorithm>

namespace OloEngine
{
    void InputSettingsPanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

        std::string title = "Input Settings";
        if (m_Dirty)
        {
            title += " *";
        }

        ImGui::Begin(title.c_str());

        DrawActionMapHeader();

        ImGui::Separator();

        auto& map = InputActionManager::GetActionMap();

        // Sort action names for stable display order
        std::vector<std::string> actionNames;
        {
            OLO_PROFILE_SCOPE("SortActionNames");
            actionNames.reserve(map.Actions.size());
            for (auto& [name, _] : map.Actions)
            {
                actionNames.push_back(name);
            }
            std::ranges::sort(actionNames);
        }

        {
            OLO_PROFILE_SCOPE("RenderActions");
            for (auto& name : actionNames)
            {
                auto* action = map.GetAction(name);
                if (action)
                {
                    DrawAction(*action);
                }
            }
        }

        ImGui::Separator();

        // Add Action button
        if (ImGui::Button("Add Action"))
        {
            m_ShowAddActionPopup = true;
            m_NewActionNameBuffer[0] = '\0';
            ImGui::OpenPopup("AddActionPopup");
        }

        DrawAddActionPopup();
        DrawRebindOverlay();

        ImGui::End();
    }

    void InputSettingsPanel::OnEvent(Event& e)
    {
        if (!m_IsRebinding)
        {
            return;
        }

        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent const& event)
                                             { return OnKeyPressed(event); });
        dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent const& event)
                                                     { return OnMouseButtonPressed(event); });
    }

    void InputSettingsPanel::DrawActionMapHeader()
    {
        auto& map = InputActionManager::GetActionMap();

        ImGui::Text("Action Map: %s", map.Name.empty() ? "(unnamed)" : map.Name.c_str());
        ImGui::SameLine();

        bool hasProject = Project::GetActive() != nullptr;

        ImGui::BeginDisabled(!hasProject);
        if (ImGui::Button("Save"))
        {
            if (hasProject)
            {
                auto path = Project::GetInputActionMapPath();
                if (InputActionSerializer::Serialize(map, path))
                {
                    m_Dirty = false;
                }
            }
        }
        ImGui::EndDisabled();
        if (!hasProject)
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Open a project to save input actions");
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Load from Disk"))
        {
            if (hasProject)
            {
                auto path = Project::GetInputActionMapPath();
                auto loaded = InputActionSerializer::Deserialize(path);
                if (loaded)
                {
                    InputActionManager::SetActionMap(*loaded);
                    m_Dirty = false;
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset to Empty"))
        {
            auto oldMap = InputActionManager::GetActionMap();
            InputActionManager::SetActionMap({});
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(std::move(oldMap), InputActionManager::GetActionMap(), "Reset Input Map",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
        }
    }

    void InputSettingsPanel::DrawAction(InputAction& action)
    {
        ImGui::PushID(action.Name.c_str());

        bool open = ImGui::CollapsingHeader(action.Name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

        // Remove action button (on same line as header)
        ImGui::SameLine(ImGui::GetWindowWidth() - 80.0f);
        if (ImGui::SmallButton("Remove"))
        {
            auto oldMap = InputActionManager::GetActionMap();
            InputActionManager::GetActionMap().RemoveAction(action.Name);
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(std::move(oldMap), InputActionManager::GetActionMap(), "Remove Action",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
            ImGui::PopID();
            return;
        }

        if (open)
        {
            ImGui::Indent();

            for (sizet i = 0; i < action.Bindings.size();)
            {
                sizet sizeBefore = action.Bindings.size();
                DrawBinding(action, i);
                // Only advance if no binding was erased
                if (action.Bindings.size() == sizeBefore)
                {
                    ++i;
                }
            }

            // Add Binding button
            if (ImGui::SmallButton("Add Binding"))
            {
                m_IsRebinding = true;
                m_RebindActionName = action.Name;
                m_RebindBindingIndex = action.Bindings.size();
                m_RebindIsNewBinding = true;
            }

            ImGui::Unindent();
        }

        ImGui::PopID();
    }

    void InputSettingsPanel::DrawBinding(InputAction& action, sizet bindingIndex)
    {
        ImGui::PushID(static_cast<int>(bindingIndex));

        auto& binding = action.Bindings[bindingIndex];
        std::string displayName = binding.GetDisplayName();

        ImGui::Text("%s", displayName.c_str());
        ImGui::SameLine();

        if (ImGui::SmallButton("Rebind"))
        {
            m_IsRebinding = true;
            m_RebindActionName = action.Name;
            m_RebindBindingIndex = bindingIndex;
            m_RebindIsNewBinding = false;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            auto oldMap = InputActionManager::GetActionMap();
            action.Bindings.erase(action.Bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(std::move(oldMap), InputActionManager::GetActionMap(), "Remove Binding",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
        }

        ImGui::PopID();
    }

    void InputSettingsPanel::DrawRebindOverlay()
    {
        if (!m_IsRebinding)
        {
            // Close the modal if it was left open (e.g. Escape-cancel in OnKeyPressed)
            if (ImGui::BeginPopupModal("RebindPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            return;
        }

        ImGui::OpenPopup("RebindPopup");
        if (ImGui::BeginPopupModal("RebindPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Press any key or mouse button to bind to '%s'", m_RebindActionName.c_str());
            ImGui::Text("Press Escape to cancel.");

            if (ImGui::Button("Cancel"))
            {
                m_IsRebinding = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void InputSettingsPanel::DrawAddActionPopup()
    {
        if (ImGui::BeginPopup("AddActionPopup"))
        {
            ImGui::Text("New Action Name:");
            ImGui::InputText("##NewActionName", m_NewActionNameBuffer, sizeof(m_NewActionNameBuffer));

            bool nameEmpty = (m_NewActionNameBuffer[0] == '\0');
            bool duplicate = InputActionManager::GetActionMap().HasAction(m_NewActionNameBuffer);

            if (duplicate)
            {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Action already exists!");
            }

            if (ImGui::Button("Create") && !nameEmpty && !duplicate)
            {
                auto oldMap = InputActionManager::GetActionMap();
                InputAction newAction;
                newAction.Name = m_NewActionNameBuffer;
                InputActionManager::GetActionMap().AddAction(std::move(newAction));
                m_Dirty = true;
                if (m_CommandHistory)
                {
                    m_CommandHistory->PushAlreadyExecuted(
                        std::make_unique<InputActionMapChangeCommand>(std::move(oldMap), InputActionManager::GetActionMap(), "Add Action",
                                                                      [this]()
                                                                      { m_Dirty = true; }));
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void InputSettingsPanel::ApplyNewBinding(InputBinding newBinding)
    {
        auto oldMap = InputActionManager::GetActionMap();

        auto& map = InputActionManager::GetActionMap();
        auto* action = map.GetAction(m_RebindActionName);
        if (action)
        {
            // Conflict detection — remove this binding from any other action
            for (auto& [name, act] : map.Actions)
            {
                if (name == m_RebindActionName)
                {
                    continue;
                }
                auto it = std::ranges::find(act.Bindings, newBinding);
                if (it != act.Bindings.end())
                {
                    act.Bindings.erase(it);
                }
            }

            // Apply the binding
            if (m_RebindIsNewBinding)
            {
                action->Bindings.push_back(newBinding);
            }
            else if (m_RebindBindingIndex < action->Bindings.size())
            {
                action->Bindings[m_RebindBindingIndex] = newBinding;
            }
            m_Dirty = true;

            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(std::move(oldMap), InputActionManager::GetActionMap(),
                                                                  m_RebindIsNewBinding ? "Add Binding" : "Rebind",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
        }

        m_IsRebinding = false;
    }

    bool InputSettingsPanel::OnKeyPressed(KeyPressedEvent const& e)
    {
        if (!m_IsRebinding)
        {
            return false;
        }

        // Escape cancels rebinding
        if (e.GetKeyCode() == Key::Escape)
        {
            m_IsRebinding = false;
            return true;
        }

        ApplyNewBinding(InputBinding::Key(e.GetKeyCode()));
        return true;
    }

    bool InputSettingsPanel::OnMouseButtonPressed(MouseButtonPressedEvent const& e)
    {
        if (!m_IsRebinding)
        {
            return false;
        }

        ApplyNewBinding(InputBinding::MouseButton(e.GetMouseButton()));
        return true;
    }

} // namespace OloEngine
