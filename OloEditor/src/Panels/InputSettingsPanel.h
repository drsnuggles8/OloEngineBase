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

        void OnImGuiRender();

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
        void ApplyNewBinding(InputBinding newBinding);

        bool OnKeyPressed(KeyPressedEvent const& e);
        bool OnMouseButtonPressed(MouseButtonPressedEvent const& e);

        // Rebinding state
        bool m_IsRebinding = false;
        std::string m_RebindActionName;
        sizet m_RebindBindingIndex = 0;
        bool m_RebindIsNewBinding = false; // true when adding a new binding slot

        // Add action state
        bool m_ShowAddActionPopup = false;
        char m_NewActionNameBuffer[128] = {};

        // Dirty tracking
        bool m_Dirty = false;

        // Undo/redo
        CommandHistory* m_CommandHistory = nullptr;
    };

} // namespace OloEngine
