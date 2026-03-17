#pragma once

#include "OloEngine/Core/Gamepad.h"

#include <array>
#include <functional>
#include <vector>

namespace OloEngine
{
    enum class InputDevice : u8
    {
        KeyboardMouse,
        GamepadDevice
    };

    class GamepadManager
    {
      public:
        static constexpr i32 MaxGamepads = 4;

        static void Initialize();
        static void Shutdown();

        // Call each frame before input processing
        static void Update();

        // Retrieve a specific gamepad (nullptr if out of range)
        [[nodiscard]] static OloEngine::Gamepad* GetGamepad(i32 index = 0);

        // Number of currently connected gamepads
        [[nodiscard]] static i32 GetConnectedCount();

        // All connected gamepads
        [[nodiscard]] static std::vector<OloEngine::Gamepad*> GetAllConnected();

        // Active input device detection
        [[nodiscard]] static InputDevice GetActiveDevice()
        {
            return s_ActiveDevice;
        }

        // Callbacks
        static inline std::function<void(i32 index)> OnGamepadConnected;
        static inline std::function<void(i32 index)> OnGamepadDisconnected;
        static inline std::function<void(InputDevice)> OnDeviceChanged;

      private:
        static inline std::array<OloEngine::Gamepad, MaxGamepads> s_Gamepads = {
            OloEngine::Gamepad(0), OloEngine::Gamepad(1), OloEngine::Gamepad(2), OloEngine::Gamepad(3)
        };
        static inline InputDevice s_ActiveDevice = InputDevice::KeyboardMouse;
        static inline bool s_Initialized = false;

        // Track previous connection states for hotplug detection
        static inline std::array<bool, MaxGamepads> s_WasConnected{};
    };

} // namespace OloEngine
