#pragma once

#include "OloEngine/Core/IInputProvider.h"
#include "OloEngine/Core/InputAction.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace OloEngine
{
    // Transparent hash/equal for heterogeneous lookup with std::string_view
    struct StringHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view sv) const
        {
            return std::hash<std::string_view>{}(sv);
        }
    };

    struct StringEqual
    {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const
        {
            return lhs == rhs;
        }
    };

    class InputActionManager
    {
      public:
        static void Init();
        static void Shutdown();

        // Call once per frame before layer updates to refresh action states.
        static void Update();

        // Query methods — return false for unknown action names.
        [[nodiscard]] static bool IsActionPressed(std::string_view actionName);
        [[nodiscard]] static bool IsActionJustPressed(std::string_view actionName);
        [[nodiscard]] static bool IsActionJustReleased(std::string_view actionName);

        // Returns the analog value for the first GamepadAxis binding in the action.
        // Keyboard/mouse bindings return 0/1; gamepad axis bindings return the raw axis value.
        [[nodiscard]] static f32 GetActionAxisValue(std::string_view actionName);

        // Replace the active action map and reset all cached state.
        static void SetActionMap(const InputActionMap& map);

        // Inject a custom input provider (for testing). Passing nullptr restores the default.
        static void SetInputProvider(IInputProvider* provider);

        // Mutable access for editor panel mutation. Stale state is pruned in Update().
        [[nodiscard]] static InputActionMap& GetActionMap()
        {
            return s_ActiveMap;
        }
        [[nodiscard]] static const InputActionMap& GetActionMapConst()
        {
            return s_ActiveMap;
        }

      private:
        inline static InputActionMap s_ActiveMap;
        inline static std::unordered_map<std::string, bool, StringHash, StringEqual> s_CurrentState;
        inline static std::unordered_map<std::string, bool, StringHash, StringEqual> s_PreviousState;
        inline static std::unordered_map<std::string, f32, StringHash, StringEqual> s_AxisValues;
        static IInputProvider* s_InputProvider;
    };

} // namespace OloEngine
