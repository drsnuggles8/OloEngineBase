#include "OloEnginePCH.h"
#include "InputSettingsPanel.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/InputRebindController.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Core/GamepadCodes.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Project/Project.h"
#include "../UndoRedo/SpecializedCommands.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <utility>

namespace OloEngine
{
    void InputSettingsPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        std::string title = "Input Settings";
        if (m_Dirty)
        {
            title += " *";
        }

        ImGui::Begin(title.c_str(), p_open);

        DrawContextSelector();
        DrawActionMapHeader();

        ImGui::Separator();

        auto& map = EditMap();

        // Sort action names for stable display order
        std::vector<std::string> actionNames;
        {
            OLO_PROFILE_SCOPE("SortActionNames");
            actionNames.reserve(map.Actions.size());
            for (const auto& [name, _] : map.Actions)
            {
                actionNames.push_back(name);
            }
            std::ranges::sort(actionNames);
        }

        {
            OLO_PROFILE_SCOPE("RenderActions");
            for (const auto& name : actionNames)
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
        DrawAddGamepadBindingPopup();
        DrawRebindOverlay();

        // Poll gamepad for rebinding while the panel is open
        if (m_IsRebindingGamepad)
        {
            PollGamepadForRebind();
        }

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

    InputActionMap& InputSettingsPanel::EditMap()
    {
        // Author the selected context's map directly, without touching the runtime
        // active context. GetActionMapMutable creates the context on first edit.
        return InputActionManager::GetActionMapMutable(m_EditContext);
    }

    void InputSettingsPanel::DrawContextSelector()
    {
        // Selects which context's action map the panel edits. Each context owns its own
        // map (gameplay/menu/vehicle/custom). This only changes the editor's edit target
        // (m_EditContext) — it does NOT call SetInputContext, so it never collapses the
        // runtime context stack or resets live input state during Play.
        ImGui::TextUnformatted("Context:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##InputContext", InputContextTypeToString(m_EditContext)))
        {
            for (const auto ctx : AllInputContextTypes)
            {
                const bool selected = (ctx == m_EditContext);
                if (ImGui::Selectable(InputContextTypeToString(ctx), selected))
                {
                    m_EditContext = ctx;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void InputSettingsPanel::DrawActionMapHeader()
    {
        const auto& map = EditMap();

        ImGui::Text("Action Map: %s", map.Name.empty() ? "(unnamed)" : map.Name.c_str());
        ImGui::SameLine();

        bool hasProject = Project::GetActive() != nullptr;

        ImGui::BeginDisabled(!hasProject);
        if (ImGui::Button("Save"))
        {
            if (hasProject)
            {
                auto path = Project::GetInputActionMapPath();
                // Persist every authored context's map, not just the active one, so
                // per-context bindings survive a reload. Skip empty maps so a context
                // merely selected in the combo (which lazily creates an empty map) isn't
                // written out as a spurious empty context.
                InputActionSerializer::ContextMaps toSave;
                for (const auto& [ctx, ctxMap] : InputActionManager::GetAllContextMaps())
                {
                    if (!ctxMap.Actions.empty())
                    {
                        toSave[ctx] = ctxMap;
                    }
                }
                if (InputActionSerializer::SerializeContexts(toSave, path))
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
                // Revert to disk: replace ALL contexts wholesale so in-memory contexts
                // absent from the file are dropped (a true revert, not a merge). Legacy
                // single-map files load as the Gameplay context.
                if (auto loaded = InputActionSerializer::DeserializeContexts(path))
                {
                    InputActionManager::ReplaceAllContextMaps(*loaded);
                    m_EditContext = InputContextType::Gameplay;
                    m_Dirty = false;
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset to Empty"))
        {
            auto oldMap = EditMap();
            InputActionManager::SetActionMap(m_EditContext, {});
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(), "Reset Input Map",
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
            auto oldMap = EditMap();
            EditMap().RemoveAction(action.Name);
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(), "Remove Action",
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
                m_IsRebindingGamepad = false;
                m_RebindActionName = action.Name;
                m_RebindBindingIndex = action.Bindings.size();
                m_RebindIsNewBinding = true;
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Add Gamepad Button"))
            {
                m_IsRebindingGamepad = true;
                m_IsRebinding = false;
                m_RebindActionName = action.Name;
                m_RebindBindingIndex = action.Bindings.size();
                m_RebindIsNewBinding = true;
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Add Gamepad Axis"))
            {
                m_GamepadAxisActionName = action.Name;
                ImGui::OpenPopup("AddGamepadAxisPopup");
            }

            ImGui::Unindent();
        }

        ImGui::PopID();
    }

    void InputSettingsPanel::DrawBinding(InputAction& action, sizet bindingIndex)
    {
        ImGui::PushID(static_cast<int>(bindingIndex));

        const auto& binding = action.Bindings[bindingIndex];
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
            auto oldMap = EditMap();
            action.Bindings.erase(action.Bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
            m_Dirty = true;
            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(), "Remove Binding",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
        }

        ImGui::PopID();
    }

    void InputSettingsPanel::DrawRebindOverlay()
    {
        if (!m_IsRebinding && !m_IsRebindingGamepad)
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
            if (m_IsRebindingGamepad)
            {
                ImGui::Text("Press any gamepad button to bind to '%s'", m_RebindActionName.c_str());
                ImGui::Text("Press Escape or click Cancel to abort.");
            }
            else
            {
                ImGui::Text("Press any key or mouse button to bind to '%s'", m_RebindActionName.c_str());
                ImGui::Text("Press Escape to cancel.");
            }

            if (ImGui::Button("Cancel"))
            {
                m_IsRebinding = false;
                m_IsRebindingGamepad = false;
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
            bool duplicate = EditMap().HasAction(m_NewActionNameBuffer);

            if (duplicate)
            {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Action already exists!");
            }

            if (ImGui::Button("Create") && !nameEmpty && !duplicate)
            {
                auto oldMap = EditMap();
                InputAction newAction;
                newAction.Name = m_NewActionNameBuffer;
                EditMap().AddAction(std::move(newAction));
                m_Dirty = true;
                if (m_CommandHistory)
                {
                    m_CommandHistory->PushAlreadyExecuted(
                        std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(), "Add Action",
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
        auto oldMap = EditMap();

        auto& map = EditMap();
        if (map.GetAction(m_RebindActionName))
        {
            // Apply via the shared controller helper: it removes the binding from any other
            // action (editor policy: silent auto-replace) and assigns it to the target slot.
            InputRebindController::ApplyBinding(map, m_RebindActionName, m_RebindBindingIndex, m_RebindIsNewBinding, newBinding,
                                                /*removeFromConflicts=*/true);
            m_Dirty = true;

            if (m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(),
                                                                  m_RebindIsNewBinding ? "Add Binding" : "Rebind",
                                                                  [this]()
                                                                  { m_Dirty = true; }));
            }
        }

        m_IsRebinding = false;
    }

    bool InputSettingsPanel::OnKeyPressed(KeyPressedEvent const& e)
    {
        if (!m_IsRebinding && !m_IsRebindingGamepad)
        {
            return false;
        }

        // Escape cancels any rebinding mode
        if (e.GetKeyCode() == Key::Escape)
        {
            m_IsRebinding = false;
            m_IsRebindingGamepad = false;
            return true;
        }

        // Only apply keyboard binding in keyboard rebind mode
        if (m_IsRebinding)
        {
            ApplyNewBinding(InputBinding::Key(e.GetKeyCode()));
        }
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

    void InputSettingsPanel::PollGamepadForRebind()
    {
        if (!m_IsRebindingGamepad)
        {
            return;
        }

        for (i32 gpIdx = 0; gpIdx < GamepadManager::MaxGamepads; ++gpIdx)
        {
            const auto* gp = GamepadManager::GetGamepad(gpIdx);
            if (!gp || !gp->IsConnected())
            {
                continue;
            }

            for (u32 b = 0; b < static_cast<u32>(std::to_underlying(GamepadButton::Count)); ++b)
            {
                auto btn = static_cast<GamepadButton>(b);
                if (gp->IsButtonJustPressed(btn))
                {
                    m_IsRebindingGamepad = false;
                    ApplyNewBinding(InputBinding::GamepadBtn(btn));
                    return;
                }
            }
        }
    }

    void InputSettingsPanel::DrawAddGamepadBindingPopup()
    {
        if (ImGui::BeginPopup("AddGamepadAxisPopup"))
        {
            ImGui::Text("Select Gamepad Axis:");

            constexpr std::array<const char*, 6> axisNames = { "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "Left Trigger", "Right Trigger" };
            constexpr std::array<GamepadAxis, 6> axes = { GamepadAxis::LeftX, GamepadAxis::LeftY, GamepadAxis::RightX, GamepadAxis::RightY, GamepadAxis::LeftTrigger,
                                                          GamepadAxis::RightTrigger };

            for (i32 i = 0; i < static_cast<i32>(axisNames.size()); ++i)
            {
                if (ImGui::Selectable(axisNames[i]))
                {
                    auto& map = EditMap();
                    if (auto* action = map.GetAction(m_GamepadAxisActionName); action)
                    {
                        auto oldMap = EditMap();
                        action->Bindings.push_back(InputBinding::GamepadAx(axes[i], 0.5f, true));
                        m_Dirty = true;
                        if (m_CommandHistory)
                        {
                            m_CommandHistory->PushAlreadyExecuted(
                                std::make_unique<InputActionMapChangeCommand>(m_EditContext, std::move(oldMap), EditMap(), "Add Gamepad Axis",
                                                                              [this]()
                                                                              { m_Dirty = true; }));
                        }
                    }
                    ImGui::CloseCurrentPopup();
                }
            }

            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

} // namespace OloEngine
