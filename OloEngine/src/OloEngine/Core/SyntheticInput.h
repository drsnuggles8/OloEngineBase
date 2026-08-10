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

        // ---- cursor DISPLACEMENT (relative injection) ------------------------
        //
        // The absolute override above cannot express "the mouse moved 40 px right"
        // to a consumer that INTEGRATES the position across frames — a mouse-look
        // rig computing `mouse - lastMousePos` (issue #607). Such a consumer sees
        // the override arrive as a one-frame spike and, the moment the plan drains
        // and the override is dropped, that spike's exact mirror; the two cancel to
        // zero net rotation. Latching the override for longer does not help: the
        // mirror is produced by REVERTING it, not by its duration, so the sum over
        // any plan is still exactly zero. Measured on the #645 player rig: four
        // equal horizontal moves each produced dYaw = 0.000.
        //
        // The only faithful model is the one a real mouse implements — a
        // displacement leaves the cursor permanently moved. So this offset is
        // ACCUMULATED and deliberately OUTLIVES the plan; a delta consumer sees the
        // displacement once, on the frame it is applied, and never a mirror.
        // Reset() (editor teardown, and the explicit reset the MCP tool exposes)
        // is what puts it back.
        //
        // Applied on top of the HARDWARE position only. The absolute override wins
        // outright when set, so an injected click/drag still resolves to exactly the
        // pixel it names — relative injection is strictly additive and changes no
        // absolute-position behaviour.
        static void AddMouseDelta(glm::vec2 delta) noexcept
        {
            // fetch_add, not load-then-store: accumulation is a read-modify-write, and
            // splitting it into two operations would lose a concurrent delta outright.
            // Today's only writer is the game thread (the drain), so the split form
            // would happen to be correct — but it encodes that invariant nowhere, and
            // "+=" on an atomic is exactly the operation being expressed.
            // Relaxed on the components + release on the flag: the flag is the
            // fast-path gate, so publishing it last is what makes them visible.
            s_MouseOffsetX.fetch_add(delta.x, std::memory_order_relaxed);
            s_MouseOffsetY.fetch_add(delta.y, std::memory_order_relaxed);
            s_HasMouseOffset.store(true, std::memory_order_release);
        }

        [[nodiscard]] static bool TryGetMouseOffset(glm::vec2& out) noexcept
        {
            if (!s_HasMouseOffset.load(std::memory_order_acquire))
                return false;
            out = { s_MouseOffsetX.load(std::memory_order_relaxed), s_MouseOffsetY.load(std::memory_order_relaxed) };
            return true;
        }

        // Unconditional read for reporting (zero when nothing is accumulated).
        [[nodiscard]] static glm::vec2 GetMouseOffset() noexcept
        {
            glm::vec2 offset{ 0.0f };
            (void)TryGetMouseOffset(offset);
            return offset;
        }

        static void ClearMouseOffset() noexcept
        {
            s_MouseOffsetX.store(0.0f, std::memory_order_relaxed);
            s_MouseOffsetY.store(0.0f, std::memory_order_relaxed);
            s_HasMouseOffset.store(false, std::memory_order_release);
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
            ClearMouseOffset();
        }

      private:
        inline static std::array<std::atomic<bool>, s_KeyCount> s_Keys{};
        inline static std::array<std::atomic<bool>, s_MouseButtonCount> s_MouseButtons{};
        inline static std::atomic<i32> s_KeyDownCount{ 0 };
        inline static std::atomic<i32> s_ButtonDownCount{ 0 };
        inline static std::atomic<bool> s_HasMousePosition{ false };
        inline static std::atomic<f32> s_MouseX{ 0.0f };
        inline static std::atomic<f32> s_MouseY{ 0.0f };
        inline static std::atomic<bool> s_HasMouseOffset{ false };
        inline static std::atomic<f32> s_MouseOffsetX{ 0.0f };
        inline static std::atomic<f32> s_MouseOffsetY{ 0.0f };
    };
} // namespace OloEngine
