#pragma once

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine
{
    // Describes a captured binding that is already used by a DIFFERENT action in the same map.
    struct RebindConflict
    {
        bool HasConflict = false;
        std::string ConflictingAction;     // the other action currently bound to Binding
        sizet ConflictingBindingIndex = 0; // its slot holding Binding
        InputBinding Binding{};
    };

    // How to resolve a captured binding that collides with an existing action's binding.
    enum class RebindResolution : u8
    {
        Replace, // remove the binding from the conflicting action, then assign it to the target
        Swap,    // hand the conflicting action the target slot's OLD binding, then assign the new one
        Keep,    // assign to the target but leave the conflicting action's binding intact (duplicate)
        Cancel   // abort — leave both actions untouched
    };

    // UI-agnostic driver for the "listen for input" rebind workflow shared by the editor
    // Input Settings panel and the runtime in-game rebind menu:
    //
    //   BeginCapture() -> feed input (OnKeyPressed / OnMouseButtonPressed / PollGamepad)
    //   -> a binding is captured -> if it conflicts, the collision is surfaced (not applied)
    //   -> ResolveConflict(); with no conflict the binding is applied immediately.
    //
    // It mutates the target context's InputActionMap in InputActionManager. The static helpers
    // (FindConflict / ApplyBinding / ResetActionToDefault / ResetMapToDefault) are the reusable
    // pure logic; the stateful capture machine layers the listen-for-next-input flow on top.
    class InputRebindController
    {
      public:
        enum class CaptureMode : u8
        {
            None,
            KeyboardMouse, // event-driven: OnKeyPressed / OnMouseButtonPressed
            Gamepad        // polled: PollGamepad once per frame
        };

        // --- Target context ---
        // Which context's map is edited. Defaults to Gameplay (what CreateDefaultGameActions
        // populates and what gameplay reads). Set before BeginCapture to retarget.
        void SetTargetContext(InputContextType ctx)
        {
            m_TargetContext = ctx;
        }
        [[nodiscard]] InputContextType GetTargetContext() const
        {
            return m_TargetContext;
        }
        [[nodiscard]] InputActionMap& TargetMap() const
        {
            return InputActionManager::GetActionMapMutable(m_TargetContext);
        }

        // --- Capture ---
        // Begin listening for the next input to assign to actionName's binding slot bindingIndex.
        // isNewBinding=true appends a new binding instead of overwriting the slot. gamepad=true
        // listens for a gamepad button (polled), otherwise keyboard/mouse (event-driven).
        void BeginCapture(std::string actionName, sizet bindingIndex, bool isNewBinding, bool gamepad);
        // Convenience: append a new binding to actionName.
        void BeginCaptureNew(std::string actionName, bool gamepad)
        {
            BeginCapture(std::move(actionName), 0, true, gamepad);
        }
        // Convenience: rebind an existing slot.
        void BeginRebind(std::string actionName, sizet bindingIndex, bool gamepad)
        {
            BeginCapture(std::move(actionName), bindingIndex, false, gamepad);
        }
        // Abort capture and discard any pending conflict.
        void CancelCapture();

        [[nodiscard]] bool IsCapturing() const
        {
            return m_Mode != CaptureMode::None;
        }
        [[nodiscard]] CaptureMode GetCaptureMode() const
        {
            return m_Mode;
        }
        [[nodiscard]] std::string_view GetCaptureActionName() const
        {
            return m_ActionName;
        }

        // Feed input while capturing. Returns true if the event was consumed (capture ended,
        // possibly leaving a pending conflict). Escape cancels keyboard/mouse capture.
        bool OnKeyPressed(KeyCode key);
        bool OnMouseButtonPressed(MouseCode button);
        // Poll all connected gamepads for a just-pressed button while in Gamepad capture mode.
        // Call once per frame. Returns true if a button was captured.
        bool PollGamepad();

        // --- Conflict resolution ---
        // After a binding is captured, if it collides with another action a pending conflict is
        // stored and NOT applied. With no conflict the binding is applied immediately and nothing
        // stays pending.
        [[nodiscard]] bool HasPendingConflict() const
        {
            return m_Pending.has_value() && m_Pending->Conflict.HasConflict;
        }
        [[nodiscard]] const RebindConflict& GetPendingConflict() const
        {
            OLO_CORE_ASSERT(HasPendingConflict(), "GetPendingConflict() called with no pending conflict — guard with HasPendingConflict()");
            return m_Pending->Conflict;
        }
        // Resolve the pending conflict per the chosen policy. No-op if nothing is pending.
        void ResolveConflict(RebindResolution resolution);

        // --- Reset to default ---
        // Restore one action (or the whole target map) to CreateDefaultGameActions().
        void ResetActionToDefault(const std::string& actionName)
        {
            ResetActionToDefault(TargetMap(), actionName);
        }
        void ResetTargetMapToDefault()
        {
            ResetMapToDefault(TargetMap());
        }

        // --- Persistence ---
        // Persist every non-empty context map to `path` via InputActionSerializer. Empty maps are
        // skipped (a merely-selected context isn't written). Returns false if serialization failed.
        [[nodiscard]] static bool Save(const std::filesystem::path& path);

        // --- Reusable pure logic (also used by the editor panel) ---
        // Find whether `binding` is already used by an action other than `ownerAction`.
        [[nodiscard]] static RebindConflict FindConflict(const InputActionMap& map, const InputBinding& binding, std::string_view ownerAction);
        // Assign `binding` to actionName's slot (append if isNewBinding). When removeFromConflicts,
        // the binding is first erased from every other action. No-op if the action is missing.
        static void ApplyBinding(InputActionMap& map, const std::string& actionName, sizet bindingIndex, bool isNewBinding, const InputBinding& binding,
                                 bool removeFromConflicts);
        // Restore actionName to its CreateDefaultGameActions() bindings; adds it if missing,
        // removes it if it has no default.
        static void ResetActionToDefault(InputActionMap& map, const std::string& actionName);
        // Replace every action in `map` with CreateDefaultGameActions().
        static void ResetMapToDefault(InputActionMap& map);

      private:
        // Apply the just-captured binding, or stash a pending conflict for the UI to resolve.
        void FinishCapture(const InputBinding& binding);

        struct PendingCapture
        {
            std::string ActionName;
            sizet BindingIndex = 0;
            bool IsNewBinding = false;
            InputBinding Binding{};
            RebindConflict Conflict{};
        };

        InputContextType m_TargetContext = InputContextType::Gameplay;
        CaptureMode m_Mode = CaptureMode::None;
        std::string m_ActionName;
        sizet m_BindingIndex = 0;
        bool m_IsNewBinding = false;
        std::optional<PendingCapture> m_Pending;
    };

} // namespace OloEngine
