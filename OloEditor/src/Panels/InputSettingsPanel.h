#pragma once

#include "OloEngine/Core/InputAction.h"
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
        bool m_ShowAddGamepadAxis = false;
        std::string m_GamepadAxisActionName;

        // Add action state
        bool m_ShowAddActionPopup = false;
        char m_NewActionNameBuffer[128] = {};

        // Dirty tracking
        bool m_Dirty = false;

        // Undo/redo
        CommandHistory* m_CommandHistory = nullptr;
    };

} // namespace OloEngine
