#pragma once

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    enum class InputBindingType : u8
    {
        Keyboard,
        Mouse
    };

    struct InputBinding
    {
        InputBindingType Type = InputBindingType::Keyboard;
        u16 Code = 0;

        [[nodiscard]] static InputBinding Key(KeyCode key)
        {
            return { InputBindingType::Keyboard, key };
        }

        [[nodiscard]] static InputBinding MouseButton(MouseCode button)
        {
            return { InputBindingType::Mouse, button };
        }

        [[nodiscard]] std::string GetDisplayName() const;

        bool operator==(const InputBinding& other) const = default;
    };

    struct InputAction
    {
        std::string Name;
        std::vector<InputBinding> Bindings;
    };

    struct InputActionMap
    {
        std::string Name;
        std::unordered_map<std::string, InputAction> Actions;

        void AddAction(InputAction action)
        {
            auto name = action.Name;
            Actions[std::move(name)] = std::move(action);
        }

        void RemoveAction(const std::string& name)
        {
            Actions.erase(name);
        }

        [[nodiscard]] InputAction* GetAction(const std::string& name)
        {
            auto it = Actions.find(name);
            return it != Actions.end() ? &it->second : nullptr;
        }

        [[nodiscard]] const InputAction* GetAction(const std::string& name) const
        {
            auto it = Actions.find(name);
            return it != Actions.end() ? &it->second : nullptr;
        }

        [[nodiscard]] bool HasAction(const std::string& name) const
        {
            return Actions.contains(name);
        }
    };

    // Creates a sample action map with common game actions for quick-start / demo use.
    // Not loaded by default — call explicitly if needed.
    [[nodiscard]] InputActionMap CreateDefaultGameActions();

} // namespace OloEngine
