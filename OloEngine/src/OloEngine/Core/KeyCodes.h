#pragma once

namespace OloEngine
{
    using KeyCode = u16;

    namespace Key
    {
        // clang-format off

        // X-MACRO list: single source of truth for all key codes.
        // Used by LuaScriptGlue to auto-populate the KeyCode Lua table.
        #define OLO_KEY_LIST(X)       \
            X(Space, 32)              \
            X(Apostrophe, 39)         \
            X(Comma, 44)              \
            X(Minus, 45)              \
            X(Period, 46)             \
            X(Slash, 47)              \
            X(D0, 48)                 \
            X(D1, 49)                 \
            X(D2, 50)                 \
            X(D3, 51)                 \
            X(D4, 52)                 \
            X(D5, 53)                 \
            X(D6, 54)                 \
            X(D7, 55)                 \
            X(D8, 56)                 \
            X(D9, 57)                 \
            X(Semicolon, 59)          \
            X(Equal, 61)              \
            X(A, 65)                  \
            X(B, 66)                  \
            X(C, 67)                  \
            X(D, 68)                  \
            X(E, 69)                  \
            X(F, 70)                  \
            X(G, 71)                  \
            X(H, 72)                  \
            X(I, 73)                  \
            X(J, 74)                  \
            X(K, 75)                  \
            X(L, 76)                  \
            X(M, 77)                  \
            X(N, 78)                  \
            X(O, 79)                  \
            X(P, 80)                  \
            X(Q, 81)                  \
            X(R, 82)                  \
            X(S, 83)                  \
            X(T, 84)                  \
            X(U, 85)                  \
            X(V, 86)                  \
            X(W, 87)                  \
            X(X, 88)                  \
            X(Y, 89)                  \
            X(Z, 90)                  \
            X(LeftBracket, 91)        \
            X(Backslash, 92)          \
            X(RightBracket, 93)       \
            X(GraveAccent, 96)        \
            X(World1, 161)            \
            X(World2, 162)            \
            X(Escape, 256)            \
            X(Enter, 257)             \
            X(Tab, 258)               \
            X(Backspace, 259)         \
            X(Insert, 260)            \
            X(Delete, 261)            \
            X(Right, 262)             \
            X(Left, 263)              \
            X(Down, 264)              \
            X(Up, 265)                \
            X(PageUp, 266)            \
            X(PageDown, 267)          \
            X(Home, 268)              \
            X(End, 269)               \
            X(CapsLock, 280)          \
            X(ScrollLock, 281)        \
            X(NumLock, 282)           \
            X(PrintScreen, 283)       \
            X(Pause, 284)             \
            X(F1, 290)                \
            X(F2, 291)                \
            X(F3, 292)                \
            X(F4, 293)                \
            X(F5, 294)                \
            X(F6, 295)                \
            X(F7, 296)                \
            X(F8, 297)                \
            X(F9, 298)                \
            X(F10, 299)               \
            X(F11, 300)               \
            X(F12, 301)               \
            X(F13, 302)               \
            X(F14, 303)               \
            X(F15, 304)               \
            X(F16, 305)               \
            X(F17, 306)               \
            X(F18, 307)               \
            X(F19, 308)               \
            X(F20, 309)               \
            X(F21, 310)               \
            X(F22, 311)               \
            X(F23, 312)               \
            X(F24, 313)               \
            X(F25, 314)               \
            X(KP0, 320)               \
            X(KP1, 321)               \
            X(KP2, 322)               \
            X(KP3, 323)               \
            X(KP4, 324)               \
            X(KP5, 325)               \
            X(KP6, 326)               \
            X(KP7, 327)               \
            X(KP8, 328)               \
            X(KP9, 329)               \
            X(KPDecimal, 330)          \
            X(KPDivide, 331)           \
            X(KPMultiply, 332)         \
            X(KPSubtract, 333)         \
            X(KPAdd, 334)              \
            X(KPEnter, 335)            \
            X(KPEqual, 336)            \
            X(LeftShift, 340)          \
            X(LeftControl, 341)        \
            X(LeftAlt, 342)            \
            X(LeftSuper, 343)          \
            X(RightShift, 344)         \
            X(RightControl, 345)       \
            X(RightAlt, 346)           \
            X(RightSuper, 347)         \
            X(Menu, 348)

        // Generate the enum from the X-MACRO list
        enum : KeyCode
        {
            #define OLO_KEY_ENUM(name, val) name = val,
            OLO_KEY_LIST(OLO_KEY_ENUM)
            #undef OLO_KEY_ENUM
        };

        // clang-format on
    } // namespace Key
} // namespace OloEngine
