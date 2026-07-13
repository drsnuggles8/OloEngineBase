#pragma once

// Synthetic-input overlay for the poll-based Input API (issue #607, olo_input_inject).
//
// The engine's Input:: family answers by asking GLFW for the CURRENT HARDWARE state
// (glfwGetKey / glfwGetMouseButton / glfwGetCursorPos). That is exactly right for a
// game, but it means a synthetic event fed into the window's event stream — an
// ImGui_ImplGlfw_* callback, or an engine Event — is INVISIBLE to Input::IsKeyPressed:
// no key was physically pressed, so GLFW keeps reporting "up". Editor code mixes both
// styles (EditorLayer::OnMouseButtonPressed reacts to the EVENT but reads the modifier
// via Input::IsKeyPressed(Key::LeftControl)), so an injected Ctrl+click would arrive
// with its Ctrl silently missing.
//
// This overlay closes that gap: the MCP input-injection hook records which synthetic
// keys / buttons are logically held and where the synthetic cursor is, and the platform
// Input implementations OR that state over the hardware state. Synthetic-down wins;
// synthetic-up never masks a real physical press, so a human at the keyboard is never
// locked out.
//
// Threading: written only from the game thread (the MCP hook runs inside a MarshalRead
// job, the drain runs in EditorLayer::OnUpdate). Reads can come from anywhere Input:: is
// called, so the state is atomic — the fast path is one relaxed load when nothing is
// injected.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>

namespace OloEngine
{
    class SyntheticInput
    {
      public:
        // GLFW_KEY_LAST is 348; size the table one past it so every KeyCode indexes.
        static constexpr sizet s_KeyCount = 350;
        // GLFW supports 8 mouse buttons (GLFW_MOUSE_BUTTON_LAST == 7).
        static constexpr sizet s_MouseButtonCount = 8;

        // ---- keys ----------------------------------------------------------

        static void SetKey(KeyCode key, bool down) noexcept
        {
            const auto index = static_cast<sizet>(key);
            if (index >= s_KeyCount)
                return;
            if (const bool previous = s_Keys[index].exchange(down, std::memory_order_release); previous != down)
                s_KeyDownCount.fetch_add(down ? 1 : -1, std::memory_order_release);
        }

        [[nodiscard]] static bool IsKeyDown(KeyCode key) noexcept
        {
            if (s_KeyDownCount.load(std::memory_order_acquire) == 0)
                return false;
            const auto index = static_cast<sizet>(key);
            return index < s_KeyCount && s_Keys[index].load(std::memory_order_acquire);
        }

        [[nodiscard]] static bool AnyKeyDown() noexcept
        {
            return s_KeyDownCount.load(std::memory_order_acquire) != 0;
        }

        // ---- mouse buttons -------------------------------------------------

        static void SetMouseButton(MouseCode button, bool down) noexcept
        {
            const auto index = static_cast<sizet>(button);
            if (index >= s_MouseButtonCount)
                return;
            if (const bool previous = s_MouseButtons[index].exchange(down, std::memory_order_release); previous != down)
                s_ButtonDownCount.fetch_add(down ? 1 : -1, std::memory_order_release);
        }

        [[nodiscard]] static bool IsMouseButtonDown(MouseCode button) noexcept
        {
            if (s_ButtonDownCount.load(std::memory_order_acquire) == 0)
                return false;
            const auto index = static_cast<sizet>(button);
            return index < s_MouseButtonCount && s_MouseButtons[index].load(std::memory_order_acquire);
        }

        // ---- cursor position -----------------------------------------------
        // In OS window CLIENT coordinates (logical pixels, origin top-left) — the
        // same space glfwGetCursorPos reports, so Input::GetMousePosition callers
        // need no conversion.

        static void SetMousePosition(glm::vec2 position) noexcept
        {
            s_MouseX.store(position.x, std::memory_order_relaxed);
            s_MouseY.store(position.y, std::memory_order_relaxed);
            s_HasMousePosition.store(true, std::memory_order_release);
        }

        [[nodiscard]] static bool TryGetMousePosition(glm::vec2& out) noexcept
        {
            if (!s_HasMousePosition.load(std::memory_order_acquire))
                return false;
            out = { s_MouseX.load(std::memory_order_relaxed), s_MouseY.load(std::memory_order_relaxed) };
            return true;
        }

        static void ClearMousePosition() noexcept
        {
            s_HasMousePosition.store(false, std::memory_order_release);
        }

        // Release every synthetic key / button and drop the cursor override. Called
        // on editor teardown (and available as a panic button) so a plan that was
        // interrupted mid-flight can never leave a key stuck down forever.
        static void Reset() noexcept
        {
            for (auto& key : s_Keys)
                key.store(false, std::memory_order_release);
            for (auto& button : s_MouseButtons)
                button.store(false, std::memory_order_release);
            s_KeyDownCount.store(0, std::memory_order_release);
            s_ButtonDownCount.store(0, std::memory_order_release);
            ClearMousePosition();
        }

      private:
        inline static std::array<std::atomic<bool>, s_KeyCount> s_Keys{};
        inline static std::array<std::atomic<bool>, s_MouseButtonCount> s_MouseButtons{};
        inline static std::atomic<i32> s_KeyDownCount{ 0 };
        inline static std::atomic<i32> s_ButtonDownCount{ 0 };
        inline static std::atomic<bool> s_HasMousePosition{ false };
        inline static std::atomic<f32> s_MouseX{ 0.0f };
        inline static std::atomic<f32> s_MouseY{ 0.0f };
    };
} // namespace OloEngine
