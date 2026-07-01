#pragma once

#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Events/MouseEvent.h"

#include <string>

namespace OloEngine
{
    class CommandHistory;

    class InputSettingsPanel
    {
      public:
        InputSettingsPanel() = default;
        ~InputSettingsPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

        // Forward events for rebinding capture
        void OnEvent(Event& e);

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

      private:
        // The context the panel is currently authoring. Decoupled from the runtime
        // active context (InputActionManager::GetInputContext) so editing bindings in
        // the editor never switches / resets the live gameplay input context.
        InputActionMap& EditMap();

        void DrawContextSelector();
        void DrawActionMapHeader();
        void DrawAction(InputAction& action);
        void DrawBinding(InputAction& action, sizet bindingIndex);
        void DrawRebindOverlay();
        void DrawAddActionPopup();
        void DrawAddGamepadBindingPopup();
        void ApplyNewBinding(InputBinding newBinding);
        void PollGamepadForRebind();

        bool OnKeyPressed(KeyPressedEvent const& e);
        bool OnMouseButtonPressed(MouseButtonPressedEvent const& e);

        // Rebinding state
        bool m_IsRebinding = false;
        bool m_IsRebindingGamepad = false; // Scanning for gamepad button press
        std::string m_RebindActionName;
        sizet m_RebindBindingIndex = 0;
        bool m_RebindIsNewBinding = false; // true when adding a new binding slot

        // Add gamepad axis binding state
        std::string m_GamepadAxisActionName;

        // Add action state
        bool m_ShowAddActionPopup = false;
        char m_NewActionNameBuffer[128] = {};

        // Dirty tracking
        bool m_Dirty = false;

        // Which context the panel edits (see EditMap()). Defaults to Gameplay.
        InputContextType m_EditContext = InputContextType::Gameplay;

        // Undo/redo
        CommandHistory* m_CommandHistory = nullptr;
    };

} // namespace OloEngine
