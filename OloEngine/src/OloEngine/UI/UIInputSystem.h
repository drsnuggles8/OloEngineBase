#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Scene;

    // Per-frame keyboard input for focused UI text fields. The runtime gathers
    // this from Input (typed characters + just-pressed edit keys) at the
    // ProcessInput call site; tests construct it directly to drive synthetic
    // text input without a window. All edit-key flags are edge-triggered
    // (just-pressed this frame), so holding a key acts once unless re-pressed.
    struct UIKeyboardInput
    {
        std::vector<u32> m_TypedCharacters; // Unicode codepoints typed this frame (text entry)
        bool m_Backspace = false;           // delete the codepoint before the cursor
        bool m_Delete = false;              // delete the codepoint at the cursor
        bool m_CursorLeft = false;          // move cursor one codepoint left
        bool m_CursorRight = false;         // move cursor one codepoint right
        bool m_Home = false;                // move cursor to the start of the text
        bool m_End = false;                 // move cursor to the end of the text
    };

    class UIInputSystem
    {
      public:
        // Process UI input for the current frame.
        // mousePos: mouse position in viewport pixel coordinates (top-left origin).
        // mouseDown: whether the primary mouse button is currently held.
        // mousePressed: whether the primary mouse button was just pressed this frame.
        // scrollDeltaX: horizontal mouse scroll wheel delta.
        // scrollDeltaY: vertical mouse scroll wheel delta (positive = up/away from user).
        // keyboard: text-entry / cursor-edit input applied to the focused input field.
        static void ProcessInput(Scene& scene, const glm::vec2& mousePos, bool mouseDown, bool mousePressed, f32 scrollDeltaX = 0.0f, f32 scrollDeltaY = 0.0f, const UIKeyboardInput& keyboard = {});
    };
} // namespace OloEngine
