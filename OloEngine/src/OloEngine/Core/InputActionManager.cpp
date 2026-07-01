#include "OloEnginePCH.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Debug/Instrumentor.h"

namespace OloEngine
{
    // Default input provider that delegates to the static Input class (GLFW-backed).
    // Defined here to keep platform details out of the Core public API.
    class GlfwInputProvider final : public IInputProvider
    {
      public:
        [[nodiscard]] bool IsKeyPressed(KeyCode key) const override
        {
            return Input::IsKeyPressed(key);
        }

        [[nodiscard]] bool IsMouseButtonPressed(MouseCode button) const override
        {
            return Input::IsMouseButtonPressed(button);
        }

        [[nodiscard]] bool IsGamepadButtonPressed(GamepadButton button, i32 gamepadIndex) const override
        {
            const auto* gp = GamepadManager::GetGamepad(gamepadIndex);
            return gp && gp->IsButtonPressed(button);
        }

        [[nodiscard]] f32 GetGamepadAxis(GamepadAxis axis, i32 gamepadIndex) const override
        {
            const auto* gp = GamepadManager::GetGamepad(gamepadIndex);
            return gp ? gp->GetAxis(axis) : 0.0f;
        }
    };

    static GlfwInputProvider s_DefaultProvider;
    IInputProvider* InputActionManager::s_InputProvider = &s_DefaultProvider;

    // --- InputContextType <-> string (serialization / editor selector) ---

    const char* InputContextTypeToString(InputContextType ctx)
    {
        switch (ctx)
        {
            case InputContextType::Gameplay:
                return "Gameplay";
            case InputContextType::Menu:
                return "Menu";
            case InputContextType::Vehicle:
                return "Vehicle";
            case InputContextType::Custom:
                return "Custom";
        }
        return "Gameplay";
    }

    std::optional<InputContextType> StringToInputContextType(std::string_view str)
    {
        if (str == "Gameplay")
            return InputContextType::Gameplay;
        if (str == "Menu")
            return InputContextType::Menu;
        if (str == "Vehicle")
            return InputContextType::Vehicle;
        if (str == "Custom")
            return InputContextType::Custom;
        return std::nullopt;
    }

    // --- InputBinding helpers (defined here to keep the header lean) ---

    std::string InputBinding::GetDisplayName() const
    {
        // clang-format off
        if (Type == InputBindingType::GamepadButton)
        {
            return std::string("Gamepad: ") + GamepadButtonToString(GPButton);
        }
        if (Type == InputBindingType::GamepadAxis)
        {
            return std::string("Gamepad Axis: ") + GamepadAxisToString(GPAxis)
                + (AxisPositive ? " +" : " -");
        }
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

        map.AddAction({ "MoveUp", { InputBinding::Key(Key::W), InputBinding::Key(Key::Up), InputBinding::GamepadBtn(GamepadButton::DPadUp) } });
        map.AddAction({ "MoveDown", { InputBinding::Key(Key::S), InputBinding::Key(Key::Down), InputBinding::GamepadBtn(GamepadButton::DPadDown) } });
        map.AddAction({ "MoveLeft", { InputBinding::Key(Key::A), InputBinding::Key(Key::Left), InputBinding::GamepadBtn(GamepadButton::DPadLeft) } });
        map.AddAction({ "MoveRight", { InputBinding::Key(Key::D), InputBinding::Key(Key::Right), InputBinding::GamepadBtn(GamepadButton::DPadRight) } });
        map.AddAction({ "Jump", { InputBinding::Key(Key::Space), InputBinding::GamepadBtn(GamepadButton::South) } });
        map.AddAction({ "Interact", { InputBinding::Key(Key::E), InputBinding::GamepadBtn(GamepadButton::West) } });

        return map;
    }

    // --- InputActionManager ---

    void InputActionManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        s_ContextStack.assign(1, InputContextType::Gameplay);
        s_ContextMaps.clear();
        s_CurrentState.clear();
        s_PreviousState.clear();
        s_AxisValues.clear();
        s_SuppressTransientOnNextUpdate = false;
    }

    void InputActionManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_ContextStack.assign(1, InputContextType::Gameplay);
        s_ContextMaps.clear();
        s_CurrentState.clear();
        s_PreviousState.clear();
        s_AxisValues.clear();
        s_SuppressTransientOnNextUpdate = false;
    }

    void InputActionManager::Update()
    {
        OLO_PROFILE_FUNCTION();

        const InputActionMap& activeMap = ActiveMap();

        s_PreviousState = s_CurrentState;

        // Prune stale entries for actions that no longer exist in the map
        for (auto it = s_CurrentState.begin(); it != s_CurrentState.end();)
        {
            if (!activeMap.Actions.contains(it->first))
            {
                it = s_CurrentState.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = s_PreviousState.begin(); it != s_PreviousState.end();)
        {
            if (!activeMap.Actions.contains(it->first))
            {
                it = s_PreviousState.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (const auto& [actionName, action] : activeMap.Actions)
        {
            bool pressed = false;
            for (const auto& binding : action.Bindings)
            {
                if (binding.Type == InputBindingType::Keyboard)
                {
                    if (s_InputProvider->IsKeyPressed(binding.Code))
                    {
                        pressed = true;
                        break;
                    }
                }
                else if (binding.Type == InputBindingType::Mouse)
                {
                    if (s_InputProvider->IsMouseButtonPressed(binding.Code))
                    {
                        pressed = true;
                        break;
                    }
                }
                else if (binding.Type == InputBindingType::GamepadButton)
                {
                    if (s_InputProvider->IsGamepadButtonPressed(binding.GPButton))
                    {
                        pressed = true;
                        break;
                    }
                }
                else if (binding.Type == InputBindingType::GamepadAxis)
                {
                    f32 axisVal = s_InputProvider->GetGamepadAxis(binding.GPAxis);
                    if (binding.AxisPositive && axisVal >= binding.AxisThreshold)
                    {
                        pressed = true;
                        break;
                    }
                    if (!binding.AxisPositive && axisVal <= -binding.AxisThreshold)
                    {
                        pressed = true;
                        break;
                    }
                }
                else
                {
                    // No additional handling required.
                }
            }
            s_CurrentState[actionName] = pressed;

            // Track the best analog value for axis queries.
            // Prefer actual axis deflection over digital 0/1.
            f32 bestAxisValue = 0.0f;
            for (const auto& binding : action.Bindings)
            {
                if (binding.Type == InputBindingType::GamepadAxis)
                {
                    f32 axisVal = s_InputProvider->GetGamepadAxis(binding.GPAxis);
                    if (binding.AxisPositive)
                    {
                        axisVal = std::max(0.0f, axisVal);
                    }
                    else
                    {
                        axisVal = std::min(0.0f, axisVal);
                    }
                    if (std::abs(axisVal) > std::abs(bestAxisValue))
                    {
                        bestAxisValue = axisVal;
                    }
                }
            }
            // Fall back to digital state only when no axis produced a stronger value
            if (pressed && std::abs(bestAxisValue) < 1.0f)
            {
                constexpr f32 axisZeroEpsilon = 1e-6f;
                bestAxisValue = (std::abs(bestAxisValue) > axisZeroEpsilon) ? std::copysign(1.0f, bestAxisValue) : 1.0f;
            }
            s_AxisValues[actionName] = bestAxisValue;
        }

        // First frame after a context switch: align previous to current so no action
        // reports just-pressed/just-released from input that was already held. From the
        // next frame on, transitions are detected normally.
        if (s_SuppressTransientOnNextUpdate)
        {
            s_PreviousState = s_CurrentState;
            s_SuppressTransientOnNextUpdate = false;
        }
    }

    bool InputActionManager::IsActionPressed(std::string_view actionName)
    {
        auto it = s_CurrentState.find(actionName);
        if (it == s_CurrentState.end())
        {
            return false;
        }
        return it->second;
    }

    bool InputActionManager::IsActionJustPressed(std::string_view actionName)
    {
        if (auto currentIt = s_CurrentState.find(actionName); currentIt == s_CurrentState.end() || !currentIt->second)
        {
            return false;
        }
        auto prevIt = s_PreviousState.find(actionName);
        return prevIt == s_PreviousState.end() || !prevIt->second;
    }

    bool InputActionManager::IsActionJustReleased(std::string_view actionName)
    {
        auto currentIt = s_CurrentState.find(actionName);
        if (bool currentlyPressed = (currentIt != s_CurrentState.end()) && currentIt->second)
        {
            return false;
        }
        auto prevIt = s_PreviousState.find(actionName);
        return prevIt != s_PreviousState.end() && prevIt->second;
    }

    void InputActionManager::ResetStateForContextChange()
    {
        // Clear cached press/axis state and suppress just-pressed/just-released on the
        // next Update(), so a key still held from the previous context doesn't fire a
        // same-key action in the newly-activated context (e.g. Escape opening a menu
        // shouldn't instantly trigger the menu's Escape-bound "Back").
        s_CurrentState.clear();
        s_PreviousState.clear();
        s_AxisValues.clear();
        s_SuppressTransientOnNextUpdate = true;
    }

    void InputActionManager::SetActionMap(const InputActionMap& map)
    {
        ActiveMap() = map;
        s_CurrentState.clear();
        s_PreviousState.clear();
        s_AxisValues.clear();
    }

    void InputActionManager::SetActionMap(InputContextType ctx, const InputActionMap& map)
    {
        const bool isActive = (ctx == s_ContextStack.back());
        s_ContextMaps[ctx] = map;
        if (isActive)
        {
            s_CurrentState.clear();
            s_PreviousState.clear();
            s_AxisValues.clear();
        }
    }

    void InputActionManager::ReplaceAllContextMaps(const std::unordered_map<InputContextType, InputActionMap>& maps)
    {
        // Wholesale replace: assignment drops any context not present in `maps`, so maps
        // authored under a previously-loaded project can't linger into the new one.
        s_ContextMaps = maps;
        // The active context's map may have changed (or vanished) — reset cached state.
        s_CurrentState.clear();
        s_PreviousState.clear();
        s_AxisValues.clear();
    }

    void InputActionManager::SetInputContext(InputContextType ctx)
    {
        OLO_PROFILE_FUNCTION();

        // Hard switch: collapse the stack to a single entry. No-op when ctx is
        // already the sole active context, so cached state is preserved.
        if (s_ContextStack.size() == 1 && s_ContextStack.back() == ctx)
        {
            return;
        }

        const bool activeChanged = (s_ContextStack.back() != ctx);
        s_ContextStack.assign(1, ctx);
        if (activeChanged)
        {
            ResetStateForContextChange();
        }
    }

    void InputActionManager::PushContext(InputContextType ctx)
    {
        OLO_PROFILE_FUNCTION();

        const bool activeChanged = (s_ContextStack.back() != ctx);
        s_ContextStack.push_back(ctx);
        if (activeChanged)
        {
            ResetStateForContextChange();
        }
    }

    bool InputActionManager::PopContext()
    {
        OLO_PROFILE_FUNCTION();

        // Never pop the base context — there must always be an active context.
        if (s_ContextStack.size() <= 1)
        {
            return false;
        }

        const InputContextType popped = s_ContextStack.back();
        s_ContextStack.pop_back();
        if (s_ContextStack.back() != popped)
        {
            ResetStateForContextChange();
        }
        return true;
    }

    void InputActionManager::SetInputProvider(IInputProvider* provider)
    {
        s_InputProvider = provider ? provider : &s_DefaultProvider;
    }

    f32 InputActionManager::GetActionAxisValue(std::string_view actionName)
    {
        OLO_PROFILE_FUNCTION();

        auto it = s_AxisValues.find(actionName);
        if (it == s_AxisValues.end())
        {
            return 0.0f;
        }
        return it->second;
    }

} // namespace OloEngine
