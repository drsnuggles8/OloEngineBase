#include "OloEnginePCH.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "Platform/Steam/SteamManager.h"

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
        s_PendingRebindMenuContext.reset();
        s_SteamDigitalHandles.clear();
        s_SteamAnalogHandles.clear();
        s_SteamActionSetHandles.clear();
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
        s_PendingRebindMenuContext.reset();
        s_SteamDigitalHandles.clear();
        s_SteamAnalogHandles.clear();
        s_SteamActionSetHandles.clear();
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

        // Steam Input, when a controller is connected, governs every GAMEPAD-origin binding for
        // the frame — "Steam Input wins when available", see
        // docs/agent-rules/steamworks-platform-integration.md §11. Keyboard/Mouse bindings are
        // unaffected either way, so they still work as a fallback (and simultaneously) with a
        // Steam Input controller connected.
        //
        // Queried ONCE here rather than per-action: GetConnectedControllers() allocates and
        // makes a virtual call into the backend, and threading the result through avoids paying
        // that once per action in the active map.
        const std::vector<u64> steamControllers = SteamManager::IsInputAvailable() ? SteamManager::GetConnectedControllers() : std::vector<u64>{};
        const bool steamGovernsGamepad = !steamControllers.empty();
        if (steamGovernsGamepad)
        {
            ActivateSteamActionSet(s_ContextStack.back(), steamControllers);
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
                    if (steamGovernsGamepad)
                    {
                        continue; // Steam Input owns this controller now; see ApplySteamInputForAction below.
                    }
                    if (s_InputProvider->IsGamepadButtonPressed(binding.GPButton))
                    {
                        pressed = true;
                        break;
                    }
                }
                else if (binding.Type == InputBindingType::GamepadAxis)
                {
                    if (steamGovernsGamepad)
                    {
                        continue;
                    }
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

            // Track the best analog value for axis queries.
            // Prefer actual axis deflection over digital 0/1.
            f32 bestAxisValue = 0.0f;
            if (!steamGovernsGamepad)
            {
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
            }

            bool axisSuppliedBySteam = false;
            if (steamGovernsGamepad)
            {
                ApplySteamInputForAction(actionName, action, steamControllers, pressed, bestAxisValue, axisSuppliedBySteam);
            }

            // Fall back to digital state only when no axis produced a stronger value. Skipped
            // when Steam Input supplied a real analog magnitude this frame — that value is
            // trustworthy on its own (e.g. a 40% trigger pull alongside a digitally-bound
            // "pressed past threshold" origin on the same action) and must not be discarded in
            // favour of a fabricated full-scale snap.
            if (!axisSuppliedBySteam && pressed && std::abs(bestAxisValue) < 1.0f)
            {
                constexpr f32 axisZeroEpsilon = 1e-6f;
                bestAxisValue = (std::abs(bestAxisValue) > axisZeroEpsilon) ? std::copysign(1.0f, bestAxisValue) : 1.0f;
            }
            s_CurrentState[actionName] = pressed;
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

    void InputActionManager::ActivateSteamActionSet(InputContextType ctx, const std::vector<u64>& connectedControllers)
    {
        auto handleIt = s_SteamActionSetHandles.find(ctx);
        const u64 actionSet = handleIt != s_SteamActionSetHandles.end()
                                  ? handleIt->second
                                  : s_SteamActionSetHandles.emplace(ctx, SteamManager::GetActionSetHandle(InputContextTypeToString(ctx))).first->second;

        for (const auto controller : connectedControllers)
        {
            SteamManager::ActivateActionSet(controller, actionSet);
        }
    }

    void InputActionManager::ApplySteamInputForAction(const std::string& actionName, const InputAction& action,
                                                      const std::vector<u64>& connectedControllers, bool& pressed, f32& axisValue,
                                                      bool& axisSuppliedBySteam)
    {
        if (connectedControllers.empty())
        {
            return;
        }
        // Only the primary (first) connected controller drives action state — the same
        // simplification GetActionAxisValue already makes for engine gamepad bindings (index 0
        // only). Multi-controller local co-op through Steam Input is out of scope; see the
        // integration doc.
        const u64 controller = connectedControllers[0];

        auto digitalIt = s_SteamDigitalHandles.find(actionName);
        const u64 digitalHandle = digitalIt != s_SteamDigitalHandles.end()
                                      ? digitalIt->second
                                      : s_SteamDigitalHandles.emplace(actionName, SteamManager::GetDigitalActionHandle(actionName)).first->second;

        // Active=false means "no origin bound in the current action set" — the action-set
        // switch above may have just changed that, so this is re-checked every frame rather
        // than cached. Only override `pressed` when Steam actually has this action bound;
        // otherwise leave the keyboard/mouse-only result from above untouched.
        if (const SteamInputDigitalActionState digitalState = SteamManager::GetDigitalActionState(controller, digitalHandle);
            digitalState.Active)
        {
            pressed = pressed || digitalState.Pressed;
        }

        auto analogIt = s_SteamAnalogHandles.find(actionName);
        const u64 analogHandle = analogIt != s_SteamAnalogHandles.end()
                                     ? analogIt->second
                                     : s_SteamAnalogHandles.emplace(actionName, SteamManager::GetAnalogActionHandle(actionName)).first->second;

        if (const SteamInputAnalogActionState analogState = SteamManager::GetAnalogActionState(controller, analogHandle);
            analogState.Active)
        {
            f32 value = analogState.X;

            // If the action's own engine-side binding constrains this axis to one direction
            // (the split-into-two-actions convention, e.g. "MoveLeft"/"MoveRight" both reading
            // the same physical axis with opposite AxisPositive), honour that constraint for
            // the Steam-sourced value too — mirroring the clamp the raw-gamepad path above
            // applies — so an action doesn't silently start reporting the wrong sign purely
            // because a Steam Input controller took over. An action with no GamepadAxis
            // binding at all (Steam Input governs it entirely) is left unconstrained.
            for (const auto& binding : action.Bindings)
            {
                if (binding.Type != InputBindingType::GamepadAxis)
                {
                    continue;
                }
                value = binding.AxisPositive ? std::max(0.0f, value) : std::min(0.0f, value);
                break;
            }

            axisValue = value;
            axisSuppliedBySteam = true;
        }
    }

} // namespace OloEngine
