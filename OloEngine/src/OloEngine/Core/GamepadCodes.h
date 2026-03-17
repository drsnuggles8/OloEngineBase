#pragma once

namespace OloEngine
{
    enum class GamepadButton : u8
    {
        South = 0, // A (Xbox) / Cross (PS)
        East,      // B (Xbox) / Circle (PS)
        West,      // X (Xbox) / Square (PS)
        North,     // Y (Xbox) / Triangle (PS)
        LeftBumper,
        RightBumper,
        Back,       // Select / Share
        Start,      // Menu / Options
        Guide,      // Xbox / PS button
        LeftThumb,  // L3
        RightThumb, // R3
        DPadUp,
        DPadRight,
        DPadDown,
        DPadLeft,
        Count
    };

    enum class GamepadAxis : u8
    {
        LeftX = 0,    // Left stick horizontal (-1 to +1)
        LeftY,        // Left stick vertical (-1 to +1)
        RightX,       // Right stick horizontal
        RightY,       // Right stick vertical
        LeftTrigger,  // LT (0 to +1)
        RightTrigger, // RT (0 to +1)
        Count
    };

    // Returns a platform-neutral display name for a gamepad button
    inline const char* GamepadButtonToString(GamepadButton button)
    {
        switch (button)
        {
            case GamepadButton::South:
                return "South";
            case GamepadButton::East:
                return "East";
            case GamepadButton::West:
                return "West";
            case GamepadButton::North:
                return "North";
            case GamepadButton::LeftBumper:
                return "LeftBumper";
            case GamepadButton::RightBumper:
                return "RightBumper";
            case GamepadButton::Back:
                return "Back";
            case GamepadButton::Start:
                return "Start";
            case GamepadButton::Guide:
                return "Guide";
            case GamepadButton::LeftThumb:
                return "LeftThumb";
            case GamepadButton::RightThumb:
                return "RightThumb";
            case GamepadButton::DPadUp:
                return "DPadUp";
            case GamepadButton::DPadRight:
                return "DPadRight";
            case GamepadButton::DPadDown:
                return "DPadDown";
            case GamepadButton::DPadLeft:
                return "DPadLeft";
            default:
                return "Unknown";
        }
    }

    inline const char* GamepadAxisToString(GamepadAxis axis)
    {
        switch (axis)
        {
            case GamepadAxis::LeftX:
                return "LeftX";
            case GamepadAxis::LeftY:
                return "LeftY";
            case GamepadAxis::RightX:
                return "RightX";
            case GamepadAxis::RightY:
                return "RightY";
            case GamepadAxis::LeftTrigger:
                return "LeftTrigger";
            case GamepadAxis::RightTrigger:
                return "RightTrigger";
            default:
                return "Unknown";
        }
    }

    inline std::optional<GamepadButton> StringToGamepadButton(const std::string& str)
    {
        if (str == "South")
            return GamepadButton::South;
        if (str == "East")
            return GamepadButton::East;
        if (str == "West")
            return GamepadButton::West;
        if (str == "North")
            return GamepadButton::North;
        if (str == "LeftBumper")
            return GamepadButton::LeftBumper;
        if (str == "RightBumper")
            return GamepadButton::RightBumper;
        if (str == "Back")
            return GamepadButton::Back;
        if (str == "Start")
            return GamepadButton::Start;
        if (str == "Guide")
            return GamepadButton::Guide;
        if (str == "LeftThumb")
            return GamepadButton::LeftThumb;
        if (str == "RightThumb")
            return GamepadButton::RightThumb;
        if (str == "DPadUp")
            return GamepadButton::DPadUp;
        if (str == "DPadRight")
            return GamepadButton::DPadRight;
        if (str == "DPadDown")
            return GamepadButton::DPadDown;
        if (str == "DPadLeft")
            return GamepadButton::DPadLeft;
        return std::nullopt;
    }

    inline std::optional<GamepadAxis> StringToGamepadAxis(const std::string& str)
    {
        if (str == "LeftX")
            return GamepadAxis::LeftX;
        if (str == "LeftY")
            return GamepadAxis::LeftY;
        if (str == "RightX")
            return GamepadAxis::RightX;
        if (str == "RightY")
            return GamepadAxis::RightY;
        if (str == "LeftTrigger")
            return GamepadAxis::LeftTrigger;
        if (str == "RightTrigger")
            return GamepadAxis::RightTrigger;
        return std::nullopt;
    }

} // namespace OloEngine
