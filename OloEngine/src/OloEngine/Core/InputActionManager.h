#pragma once

#include "OloEngine/Core/InputAction.h"

#include <string_view>
#include <unordered_map>

namespace OloEngine
{
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

        // Swap the active action map and reset state.
        static void SetActionMap(const InputActionMap& map);

        [[nodiscard]] static InputActionMap& GetActionMap() { return s_ActiveMap; }
        [[nodiscard]] static const InputActionMap& GetActionMapConst() { return s_ActiveMap; }

      private:
        inline static InputActionMap s_ActiveMap;
        inline static std::unordered_map<std::string, bool> s_CurrentState;
        inline static std::unordered_map<std::string, bool> s_PreviousState;
    };

} // namespace OloEngine
