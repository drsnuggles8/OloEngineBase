#include "OloEnginePCH.h"
#include "InputSettingsPanel.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Project/Project.h"

#include <imgui.h>

#include <algorithm>

namespace OloEngine
{
    void InputSettingsPanel::OnImGuiRender()
    {
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
        actionNames.reserve(map.Actions.size());
        for (auto& [name, _] : map.Actions)
        {
            actionNames.push_back(name);
        }
        std::ranges::sort(actionNames);

        for (auto& name : actionNames)
        {
            auto* action = map.GetAction(name);
            if (action)
            {
                DrawAction(*action);
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
        dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent const& event) { return OnKeyPressed(event); });
        dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent const& event) { return OnMouseButtonPressed(event); });
    }

    void InputSettingsPanel::DrawActionMapHeader()
    {
        auto& map = InputActionManager::GetActionMap();

        ImGui::Text("Action Map: %s", map.Name.empty() ? "(unnamed)" : map.Name.c_str());
        ImGui::SameLine();

        bool hasProject = Project::GetActive() != nullptr;

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
            InputActionManager::SetActionMap({});
            m_Dirty = true;
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
            InputActionManager::GetActionMap().RemoveAction(action.Name);
            m_Dirty = true;
            ImGui::PopID();
            return;
        }

        if (open)
        {
            ImGui::Indent();

            // Draw existing bindings
            for (sizet i = 0; i < action.Bindings.size(); ++i)
            {
                DrawBinding(action, i);
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
            action.Bindings.erase(action.Bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
            m_Dirty = true;
        }

        ImGui::PopID();
    }

    void InputSettingsPanel::DrawRebindOverlay()
    {
        if (!m_IsRebinding)
        {
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
                InputAction newAction;
                newAction.Name = m_NewActionNameBuffer;
                InputActionManager::GetActionMap().AddAction(std::move(newAction));
                m_Dirty = true;
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

        InputBinding newBinding = InputBinding::Key(e.GetKeyCode());

        // Conflict detection
        auto& map = InputActionManager::GetActionMap();
        for (auto& [name, action] : map.Actions)
        {
            if (name == m_RebindActionName)
            {
                continue;
            }
            auto it = std::ranges::find(action.Bindings, newBinding);
            if (it != action.Bindings.end())
            {
                // Remove conflicting binding from the other action
                action.Bindings.erase(it);
            }
        }

        // Apply the binding
        auto* action = map.GetAction(m_RebindActionName);
        if (action)
        {
            if (m_RebindIsNewBinding)
            {
                action->Bindings.push_back(newBinding);
            }
            else if (m_RebindBindingIndex < action->Bindings.size())
            {
                action->Bindings[m_RebindBindingIndex] = newBinding;
            }
            m_Dirty = true;
        }

        m_IsRebinding = false;
        return true;
    }

    bool InputSettingsPanel::OnMouseButtonPressed(MouseButtonPressedEvent const& e)
    {
        if (!m_IsRebinding)
        {
            return false;
        }

        InputBinding newBinding = InputBinding::MouseButton(e.GetMouseButton());

        // Conflict detection
        auto& map = InputActionManager::GetActionMap();
        for (auto& [name, action] : map.Actions)
        {
            if (name == m_RebindActionName)
            {
                continue;
            }
            auto it = std::ranges::find(action.Bindings, newBinding);
            if (it != action.Bindings.end())
            {
                action.Bindings.erase(it);
            }
        }

        // Apply the binding
        auto* action = map.GetAction(m_RebindActionName);
        if (action)
        {
            if (m_RebindIsNewBinding)
            {
                action->Bindings.push_back(newBinding);
            }
            else if (m_RebindBindingIndex < action->Bindings.size())
            {
                action->Bindings[m_RebindBindingIndex] = newBinding;
            }
            m_Dirty = true;
        }

        m_IsRebinding = false;
        return true;
    }

} // namespace OloEngine
