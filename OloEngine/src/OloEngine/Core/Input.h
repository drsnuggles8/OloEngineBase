#pragma once

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    // How the OS mouse cursor behaves. `Locked` is the FPS mode: the cursor is
    // hidden and pinned, and GetMousePosition() reports unbounded virtual motion
    // (no window-edge stall), which is what mouse-look wants.
    enum class CursorMode
    {
        Normal = 0, // visible, moves freely
        Hidden,     // hidden over the window, but not locked
        Locked      // hidden + locked (FPS look)
    };

    class Input
    {
      public:
        static bool IsKeyPressed(KeyCode key);
        static bool IsKeyJustPressed(KeyCode key);
        static bool IsKeyJustReleased(KeyCode key);

        static bool IsMouseButtonPressed(MouseCode button);
        static glm::vec2 GetMousePosition();
        static f32 GetMouseX();
        static f32 GetMouseY();

        // Cursor capture (no-op when there is no window, e.g. headless tests).
        static void SetCursorMode(CursorMode mode);
        static CursorMode GetCursorMode();

        // Unicode codepoints typed (text entry) during the frame that the most
        // recent Update() snapshotted. This is the text-input counterpart to the
        // key-state polling above: GLFW routes printable characters through a
        // separate char callback (which honours layout, modifiers, IME), so they
        // can't be reconstructed from IsKeyPressed. UI text fields consume this.
        static const std::vector<u32>& GetTypedCharacters();

        // Record a typed codepoint. Called by the platform window's char
        // callback; accumulated until the next Update() rotates the buffer.
        static void OnCharTyped(u32 codepoint);

        // Called once per frame from Application::Run() to snapshot keyboard
        // state and rotate the typed-character buffer for this frame.
        static void Update();
    };
} // namespace OloEngine
