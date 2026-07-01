#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <functional>
#include <unordered_map>
#include <utility>

namespace OloEngine
{
    class Scene;

    // Per-frame navigation intent for the runtime UI. Filled from the active
    // input context's menu actions in Scene::OnUpdateRuntime (via
    // UINavigationSystem::PollActions), or by tests directly. All fields are
    // edge-triggered (this-frame press), matching InputActionManager's
    // IsActionJustPressed semantics — navigation is discrete, one step per press.
    struct UINavInput
    {
        bool NavUp = false;
        bool NavDown = false;
        bool NavLeft = false;
        bool NavRight = false;
        bool Activate = false; // confirm / submit  (gamepad South / Enter)
        bool Cancel = false;   // back / dismiss     (gamepad East  / Escape)
    };

    // Runtime-only navigation + widget-event state, owned by Scene
    // (Scene::GetUINavigation()). Never serialized, never copied with the scene,
    // and Clear()ed on Scene::OnRuntimeStop so a stop/play cycle does not leak a
    // stale focus target or stale event delegates. Mirrors GameplayEventBus.
    //
    // The event delegates are the "widget event callbacks / data binding" half of
    // issue #457: gameplay/scripts register a handler once, keyed by the widget
    // entity's UUID, and it fires whenever the widget is activated or its value
    // changes — from either gamepad/keyboard navigation OR mouse — with no
    // per-frame polling. UINavigationSystem::Update detects the changes and
    // dispatches.
    class UINavigation
    {
      public:
        using ClickCallback = std::function<void()>;
        using ValueChangedCallback = std::function<void(f32)>;

        // --- Focus ---
        void SetFocus(UUID widget)
        {
            m_Focused = widget;
        }
        [[nodiscard]] UUID GetFocus() const
        {
            return m_Focused;
        }
        void ClearFocus()
        {
            m_Focused = UUID(0);
        }
        [[nodiscard]] bool HasFocus() const
        {
            return static_cast<u64>(m_Focused) != 0;
        }

        // --- Event delegates (data binding) ---
        // Registering a handler for a widget replaces any previous one for the
        // same event. Pass an empty std::function to unbind.
        void OnClick(UUID widget, ClickCallback fn)
        {
            m_OnClick[static_cast<u64>(widget)] = std::move(fn);
        }
        void OnSubmit(UUID widget, ClickCallback fn)
        {
            m_OnSubmit[static_cast<u64>(widget)] = std::move(fn);
        }
        void OnValueChanged(UUID widget, ValueChangedCallback fn)
        {
            m_OnValueChanged[static_cast<u64>(widget)] = std::move(fn);
        }

        // --- Dispatch (invoked by UINavigationSystem for a change from any
        // source — gamepad activation OR a mouse-driven state transition). No-op
        // when nothing is registered for the widget. ---
        void FireClick(UUID widget)
        {
            if (auto it = m_OnClick.find(static_cast<u64>(widget)); it != m_OnClick.end() && it->second)
                it->second();
        }
        void FireSubmit(UUID widget)
        {
            if (auto it = m_OnSubmit.find(static_cast<u64>(widget)); it != m_OnSubmit.end() && it->second)
                it->second();
        }
        void FireValueChanged(UUID widget, f32 value)
        {
            if (auto it = m_OnValueChanged.find(static_cast<u64>(widget)); it != m_OnValueChanged.end() && it->second)
                it->second(value);
        }

        // Drop all focus, delegates and change-detection snapshots.
        void Clear()
        {
            m_Focused = UUID(0);
            m_OnClick.clear();
            m_OnSubmit.clear();
            m_OnValueChanged.clear();
            m_PrevSlider.clear();
            m_PrevCheckbox.clear();
            m_PrevToggle.clear();
            m_PrevButtonState.clear();
        }

      private:
        friend class UINavigationSystem;

        UUID m_Focused{ 0 }; // 0 == nothing focused

        // Delegates keyed by widget entity UUID (raw u64 to avoid UUID's implicit
        // u64 conversion tripping map-internal operator== ambiguity, C2666).
        std::unordered_map<u64, ClickCallback> m_OnClick;
        std::unordered_map<u64, ClickCallback> m_OnSubmit;
        std::unordered_map<u64, ValueChangedCallback> m_OnValueChanged;

        // Previous-frame snapshots for source-agnostic change detection (a value
        // that changed via mouse fires the same delegate as one changed via pad).
        std::unordered_map<u64, f32> m_PrevSlider;
        std::unordered_map<u64, bool> m_PrevCheckbox;
        std::unordered_map<u64, bool> m_PrevToggle;
        std::unordered_map<u64, u8> m_PrevButtonState;
    };

    // Drives runtime UI focus navigation and fires widget event delegates.
    // Called once per frame from Scene::OnUpdateRuntime, after
    // UILayoutSystem::ResolveLayout (so UIResolvedRectComponents are current) and
    // UIInputSystem::ProcessInput (so mouse-driven state changes are visible to
    // the change-detection pass).
    class UINavigationSystem
    {
      public:
        // Advance focus / activation for `input`, then dispatch any widget event
        // delegates whose trigger fired this frame (from gamepad OR mouse).
        static void Update(Scene& scene, const UINavInput& input);

        // Read the active input context's menu actions
        // (MenuUp/MenuDown/MenuLeft/MenuRight/MenuActivate/MenuCancel) into a
        // UINavInput. Unknown actions read as false, so this is a no-op source
        // until InstallDefaultMenuActions (or the app's own bindings) authors them
        // and the Menu context is active.
        [[nodiscard]] static UINavInput PollActions();

        // Author sensible default menu-navigation bindings (D-pad + arrow keys,
        // South/Enter to activate, East/Escape to cancel) into the Menu input
        // context. Uses only public InputActionManager API. Call once during
        // setup; push InputContextType::Menu to make them live.
        static void InstallDefaultMenuActions();

        // Fraction of a slider's [min,max] range that one Left/Right press moves.
        static constexpr f32 kSliderNavStep = 0.1f;

      private:
        // Change-detection + activation helpers. Members (not free functions) so
        // the class's friendship with UINavigation grants them access to its
        // private per-frame snapshot maps.
        static void SeedSnapshots(Scene& scene, UINavigation& nav);
        static void ActivateFocused(Scene& scene, UINavigation& nav);
        static bool AdjustFocusedSlider(Scene& scene, UINavigation& nav, const UINavInput& input);
        static void ReflectFocusOnButtons(Scene& scene, UINavigation& nav);
        static void DispatchChanges(Scene& scene, UINavigation& nav);
    };
} // namespace OloEngine
