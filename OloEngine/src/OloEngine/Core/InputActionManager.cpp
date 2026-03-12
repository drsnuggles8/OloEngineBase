#include "OloEnginePCH.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Input.h"

namespace OloEngine
{
    // --- InputBinding helpers (defined here to keep the header lean) ---

    std::string InputBinding::GetDisplayName() const
    {
        // clang-format off
        if (Type == InputBindingType::Mouse)
        {
            switch (Code)
            {
                case Mouse::ButtonLeft:   return "Mouse: Left";
                case Mouse::ButtonRight:  return "Mouse: Right";
                case Mouse::ButtonMiddle: return "Mouse: Middle";
                default:                  return "Mouse: Button" + std::to_string(Code);
            }
        }

        // Keyboard
        switch (Code)
        {
            case Key::Space:        return "Keyboard: Space";
            case Key::Apostrophe:   return "Keyboard: '";
            case Key::Comma:        return "Keyboard: ,";
            case Key::Minus:        return "Keyboard: -";
            case Key::Period:       return "Keyboard: .";
            case Key::Slash:        return "Keyboard: /";
            case Key::Semicolon:    return "Keyboard: ;";
            case Key::Equal:        return "Keyboard: =";
            case Key::LeftBracket:  return "Keyboard: [";
            case Key::Backslash:    return "Keyboard: \\";
            case Key::RightBracket: return "Keyboard: ]";
            case Key::GraveAccent:  return "Keyboard: `";
            case Key::Escape:       return "Keyboard: Escape";
            case Key::Enter:        return "Keyboard: Enter";
            case Key::Tab:          return "Keyboard: Tab";
            case Key::Backspace:    return "Keyboard: Backspace";
            case Key::Insert:       return "Keyboard: Insert";
            case Key::Delete:       return "Keyboard: Delete";
            case Key::Right:        return "Keyboard: Right";
            case Key::Left:         return "Keyboard: Left";
            case Key::Down:         return "Keyboard: Down";
            case Key::Up:           return "Keyboard: Up";
            case Key::PageUp:       return "Keyboard: PageUp";
            case Key::PageDown:     return "Keyboard: PageDown";
            case Key::Home:         return "Keyboard: Home";
            case Key::End:          return "Keyboard: End";
            case Key::CapsLock:     return "Keyboard: CapsLock";
            case Key::ScrollLock:   return "Keyboard: ScrollLock";
            case Key::NumLock:      return "Keyboard: NumLock";
            case Key::PrintScreen:  return "Keyboard: PrintScreen";
            case Key::Pause:        return "Keyboard: Pause";
            case Key::LeftShift:    return "Keyboard: LeftShift";
            case Key::LeftControl:  return "Keyboard: LeftCtrl";
            case Key::LeftAlt:      return "Keyboard: LeftAlt";
            case Key::LeftSuper:    return "Keyboard: LeftSuper";
            case Key::RightShift:   return "Keyboard: RightShift";
            case Key::RightControl: return "Keyboard: RightCtrl";
            case Key::RightAlt:     return "Keyboard: RightAlt";
            case Key::RightSuper:   return "Keyboard: RightSuper";
            case Key::Menu:         return "Keyboard: Menu";
            default: break;
        }
        // clang-format on

        // A-Z
        if (Code >= Key::A && Code <= Key::Z)
        {
            return std::string("Keyboard: ") + static_cast<char>(Code);
        }
        // 0-9
        if (Code >= Key::D0 && Code <= Key::D9)
        {
            return std::string("Keyboard: ") + static_cast<char>(Code);
        }
        // F1-F25
        if (Code >= Key::F1 && Code <= Key::F25)
        {
            return "Keyboard: F" + std::to_string(Code - Key::F1 + 1);
        }
        // Keypad 0-9
        if (Code >= Key::KP0 && Code <= Key::KP9)
        {
            return "Keyboard: KP" + std::to_string(Code - Key::KP0);
        }

        return "Keyboard: Unknown(" + std::to_string(Code) + ")";
    }

    // --- CreateDefaultGameActions ---

    InputActionMap CreateDefaultGameActions()
    {
        InputActionMap map;
        map.Name = "DefaultGameActions";

        map.AddAction({ "MoveUp",    { InputBinding::Key(Key::W), InputBinding::Key(Key::Up) } });
        map.AddAction({ "MoveDown",  { InputBinding::Key(Key::S), InputBinding::Key(Key::Down) } });
        map.AddAction({ "MoveLeft",  { InputBinding::Key(Key::A), InputBinding::Key(Key::Left) } });
        map.AddAction({ "MoveRight", { InputBinding::Key(Key::D), InputBinding::Key(Key::Right) } });
        map.AddAction({ "Jump",      { InputBinding::Key(Key::Space) } });
        map.AddAction({ "Interact",  { InputBinding::Key(Key::E) } });

        return map;
    }

    // --- InputActionManager ---

    void InputActionManager::Init()
    {
        s_ActiveMap = {};
        s_CurrentState.clear();
        s_PreviousState.clear();
    }

    void InputActionManager::Shutdown()
    {
        s_ActiveMap = {};
        s_CurrentState.clear();
        s_PreviousState.clear();
    }

    void InputActionManager::Update()
    {
        s_PreviousState = s_CurrentState;

        for (auto& [actionName, action] : s_ActiveMap.Actions)
        {
            bool pressed = false;
            for (const auto& binding : action.Bindings)
            {
                if (binding.Type == InputBindingType::Keyboard)
                {
                    if (Input::IsKeyPressed(binding.Code))
                    {
                        pressed = true;
                        break;
                    }
                }
                else if (binding.Type == InputBindingType::Mouse)
                {
                    if (Input::IsMouseButtonPressed(binding.Code))
                    {
                        pressed = true;
                        break;
                    }
                }
            }
            s_CurrentState[actionName] = pressed;
        }
    }

    bool InputActionManager::IsActionPressed(std::string_view actionName)
    {
        auto it = s_CurrentState.find(std::string(actionName));
        if (it == s_CurrentState.end())
        {
            return false;
        }
        return it->second;
    }

    bool InputActionManager::IsActionJustPressed(std::string_view actionName)
    {
        std::string key(actionName);
        auto currentIt = s_CurrentState.find(key);
        if (currentIt == s_CurrentState.end() || !currentIt->second)
        {
            return false;
        }
        auto prevIt = s_PreviousState.find(key);
        return prevIt == s_PreviousState.end() || !prevIt->second;
    }

    bool InputActionManager::IsActionJustReleased(std::string_view actionName)
    {
        std::string key(actionName);
        auto currentIt = s_CurrentState.find(key);
        bool currentlyPressed = (currentIt != s_CurrentState.end()) && currentIt->second;
        if (currentlyPressed)
        {
            return false;
        }
        auto prevIt = s_PreviousState.find(key);
        return prevIt != s_PreviousState.end() && prevIt->second;
    }

    void InputActionManager::SetActionMap(const InputActionMap& map)
    {
        s_ActiveMap = map;
        s_CurrentState.clear();
        s_PreviousState.clear();
    }

} // namespace OloEngine
