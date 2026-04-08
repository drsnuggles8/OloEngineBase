#pragma once

namespace OloEngine
{
    using MouseCode = u16;

    namespace Mouse
    {
        // clang-format off

        // X-MACRO list: single source of truth for mouse button codes.
        // Used by LuaScriptGlue to auto-populate the MouseButton Lua table.
        #define OLO_MOUSE_LIST(X) \
            X(Button0, 0)         \
            X(Button1, 1)         \
            X(Button2, 2)         \
            X(Button3, 3)         \
            X(Button4, 4)         \
            X(Button5, 5)         \
            X(Button6, 6)         \
            X(Button7, 7)         \
            X(ButtonLast, 7)      \
            X(ButtonLeft, 0)      \
            X(ButtonRight, 1)     \
            X(ButtonMiddle, 2)

        // Generate the enum from the X-MACRO list
        enum : MouseCode
        {
            #define OLO_MOUSE_ENUM(name, val) name = val,
            OLO_MOUSE_LIST(OLO_MOUSE_ENUM)
            #undef OLO_MOUSE_ENUM
        };

        // clang-format on
    } // namespace Mouse
} // namespace OloEngine
