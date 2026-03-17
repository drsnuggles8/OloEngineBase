#include "OloEnginePCH.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

namespace OloEngine
{
    void GamepadManager::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        s_Initialized = true;
        s_ActiveDevice = InputDevice::KeyboardMouse;
        s_WasConnected.fill(false);

        // Do an initial poll to detect already-connected gamepads
        for (i32 i = 0; i < MaxGamepads; ++i)
        {
            s_Gamepads[i].Update();
            s_WasConnected[i] = s_Gamepads[i].IsConnected();
        }

        OLO_CORE_INFO("GamepadManager initialized ({} gamepads detected)", GetConnectedCount());
    }

    void GamepadManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Initialized = false;
        OnGamepadConnected = nullptr;
        OnGamepadDisconnected = nullptr;
        OnDeviceChanged = nullptr;
    }

    void GamepadManager::Update()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            return;
        }

        bool anyGamepadInput = false;

        for (i32 i = 0; i < MaxGamepads; ++i)
        {
            s_Gamepads[i].Update();

            bool nowConnected = s_Gamepads[i].IsConnected();

            // Hotplug detection
            if (nowConnected && !s_WasConnected[i])
            {
                if (OnGamepadConnected)
                {
                    OnGamepadConnected(i);
                }
            }
            else if (!nowConnected && s_WasConnected[i])
            {
                if (OnGamepadDisconnected)
                {
                    OnGamepadDisconnected(i);
                }
            }
            s_WasConnected[i] = nowConnected;

            if (s_Gamepads[i].HadInputThisFrame())
            {
                anyGamepadInput = true;
            }
        }

        // Input device switching
        if (anyGamepadInput && s_ActiveDevice != InputDevice::GamepadDevice)
        {
            s_ActiveDevice = InputDevice::GamepadDevice;
            if (OnDeviceChanged)
            {
                OnDeviceChanged(s_ActiveDevice);
            }
        }
        else if (!anyGamepadInput && s_ActiveDevice == InputDevice::GamepadDevice)
        {
            // Check for any keyboard/mouse activity to switch back
            if (s_HadKeyboardMouseInput)
            {
                s_ActiveDevice = InputDevice::KeyboardMouse;
                if (OnDeviceChanged)
                {
                    OnDeviceChanged(s_ActiveDevice);
                }
            }
        }
        s_HadKeyboardMouseInput = false;
    }

    OloEngine::Gamepad* GamepadManager::GetGamepad(i32 index)
    {
        if (index < 0 || index >= MaxGamepads)
        {
            return nullptr;
        }
        return &s_Gamepads[index];
    }

    i32 GamepadManager::GetConnectedCount()
    {
        i32 count = 0;
        for (i32 i = 0; i < MaxGamepads; ++i)
        {
            if (s_Gamepads[i].IsConnected())
            {
                ++count;
            }
        }
        return count;
    }

    std::vector<OloEngine::Gamepad*> GamepadManager::GetAllConnected()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<OloEngine::Gamepad*> result;
        result.reserve(MaxGamepads);
        for (i32 i = 0; i < MaxGamepads; ++i)
        {
            if (s_Gamepads[i].IsConnected())
            {
                result.push_back(&s_Gamepads[i]);
            }
        }
        return result;
    }

} // namespace OloEngine
