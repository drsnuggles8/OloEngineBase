#include "OloEnginePCH.h"
#include "InputRebindController.h"

#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/InputActionSerializer.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    RebindConflict InputRebindController::FindConflict(const InputActionMap& map, const InputBinding& binding, std::string_view ownerAction)
    {
        RebindConflict conflict;
        conflict.Binding = binding;

        for (const auto& [name, action] : map.Actions)
        {
            if (name == ownerAction)
            {
                continue;
            }
            const auto it = std::ranges::find(action.Bindings, binding);
            if (it != action.Bindings.end())
            {
                conflict.HasConflict = true;
                conflict.ConflictingAction = name;
                conflict.ConflictingBindingIndex = static_cast<sizet>(std::distance(action.Bindings.begin(), it));
                return conflict;
            }
        }

        return conflict;
    }

    void InputRebindController::ApplyBinding(InputActionMap& map, const std::string& actionName, sizet bindingIndex, bool isNewBinding,
                                             const InputBinding& binding, bool removeFromConflicts)
    {
        auto* action = map.GetAction(actionName);
        if (!action)
        {
            return;
        }

        if (removeFromConflicts)
        {
            // Erase the binding from every OTHER action so it maps to exactly one action.
            // Remove ALL occurrences (not just the first) so a pre-existing duplicate in a
            // malformed map can't survive and re-trigger the same conflict.
            for (auto& [name, act] : map.Actions)
            {
                if (name == actionName)
                {
                    continue;
                }
                std::erase(act.Bindings, binding);
            }
        }

        if (isNewBinding)
        {
            action->Bindings.push_back(binding);
        }
        else if (bindingIndex < action->Bindings.size())
        {
            action->Bindings[bindingIndex] = binding;
        }
        else
        {
            // Slot no longer valid (the map changed under us) — append rather than drop it.
            action->Bindings.push_back(binding);
        }
    }

    void InputRebindController::ResetActionToDefault(InputActionMap& map, const std::string& actionName)
    {
        const InputActionMap defaults = CreateDefaultGameActions();
        if (const auto* defAction = defaults.GetAction(actionName); defAction)
        {
            InputAction restored = *defAction;
            map.AddAction(std::move(restored));
        }
        else
        {
            // No default for this action — clearing it to default means removing it.
            map.RemoveAction(actionName);
        }
    }

    void InputRebindController::ResetMapToDefault(InputActionMap& map)
    {
        const std::string previousName = map.Name;
        map = CreateDefaultGameActions();
        // Preserve the map's own name so a context map keeps its identity after a reset.
        if (!previousName.empty())
        {
            map.Name = previousName;
        }
    }

    bool InputRebindController::Save(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return false;
        }

        InputActionSerializer::ContextMaps toSave;
        for (const auto& [ctx, ctxMap] : InputActionManager::GetAllContextMaps())
        {
            if (!ctxMap.Actions.empty())
            {
                toSave[ctx] = ctxMap;
            }
        }
        return InputActionSerializer::SerializeContexts(toSave, path);
    }

    void InputRebindController::BeginCapture(std::string actionName, sizet bindingIndex, bool isNewBinding, bool gamepad)
    {
        m_ActionName = std::move(actionName);
        m_BindingIndex = bindingIndex;
        m_IsNewBinding = isNewBinding;
        m_Mode = gamepad ? CaptureMode::Gamepad : CaptureMode::KeyboardMouse;
        m_Pending.reset();
    }

    void InputRebindController::CancelCapture()
    {
        m_Mode = CaptureMode::None;
        m_Pending.reset();
    }

    bool InputRebindController::OnKeyPressed(KeyCode key)
    {
        if (m_Mode != CaptureMode::KeyboardMouse)
        {
            return false;
        }

        // Escape aborts capture without binding.
        if (key == Key::Escape)
        {
            CancelCapture();
            return true;
        }

        FinishCapture(InputBinding::Key(key));
        return true;
    }

    bool InputRebindController::OnMouseButtonPressed(MouseCode button)
    {
        if (m_Mode != CaptureMode::KeyboardMouse)
        {
            return false;
        }

        FinishCapture(InputBinding::MouseButton(button));
        return true;
    }

    bool InputRebindController::PollGamepad()
    {
        if (m_Mode != CaptureMode::Gamepad)
        {
            return false;
        }

        for (i32 gpIdx = 0; gpIdx < GamepadManager::MaxGamepads; ++gpIdx)
        {
            const auto* gp = GamepadManager::GetGamepad(gpIdx);
            if (!gp || !gp->IsConnected())
            {
                continue;
            }

            for (u32 b = 0; b < static_cast<u32>(std::to_underlying(GamepadButton::Count)); ++b)
            {
                const auto btn = static_cast<GamepadButton>(b);
                if (gp->IsButtonJustPressed(btn))
                {
                    FinishCapture(InputBinding::GamepadBtn(btn));
                    return true;
                }
            }
        }

        return false;
    }

    void InputRebindController::FinishCapture(const InputBinding& binding)
    {
        // Capture is over regardless of whether a conflict needs resolving.
        m_Mode = CaptureMode::None;

        InputActionMap& map = TargetMap();
        RebindConflict conflict = FindConflict(map, binding, m_ActionName);

        if (!conflict.HasConflict)
        {
            // No collision — apply straight away.
            ApplyBinding(map, m_ActionName, m_BindingIndex, m_IsNewBinding, binding, /*removeFromConflicts=*/false);
            m_Pending.reset();
            return;
        }

        // Collision — stash the capture and surface the conflict for the UI to resolve.
        PendingCapture pending;
        pending.ActionName = m_ActionName;
        pending.BindingIndex = m_BindingIndex;
        pending.IsNewBinding = m_IsNewBinding;
        pending.Binding = binding;
        pending.Conflict = std::move(conflict);
        m_Pending = std::move(pending);
    }

    void InputRebindController::ResolveConflict(RebindResolution resolution)
    {
        if (!m_Pending.has_value())
        {
            return;
        }

        const PendingCapture pending = *m_Pending;
        m_Pending.reset();

        if (resolution == RebindResolution::Cancel)
        {
            return;
        }

        InputActionMap& map = TargetMap();

        switch (resolution)
        {
            case RebindResolution::Replace:
            {
                ApplyBinding(map, pending.ActionName, pending.BindingIndex, pending.IsNewBinding, pending.Binding, /*removeFromConflicts=*/true);
                break;
            }
            case RebindResolution::Keep:
            {
                // Assign to the target but leave the conflicting action alone (duplicate binding).
                ApplyBinding(map, pending.ActionName, pending.BindingIndex, pending.IsNewBinding, pending.Binding, /*removeFromConflicts=*/false);
                break;
            }
            case RebindResolution::Swap:
            {
                // Only swap if the target action still exists. If it was removed between capture
                // and resolution, mutating the conflicting action would drop its binding while the
                // target can't receive anything — so leave both untouched instead.
                auto* target = map.GetAction(pending.ActionName);
                if (!target)
                {
                    break;
                }

                // For a rebind (not a fresh binding), a stale / out-of-range slot index means the
                // map changed under us. Don't silently turn Swap into a destructive Replace that
                // drops the conflicting binding — leave both actions untouched instead.
                if (!pending.IsNewBinding && pending.BindingIndex >= target->Bindings.size())
                {
                    break;
                }

                // The conflicting slot must still hold the exact binding we detected at capture
                // time. If the map changed under us (slot now out of range, or holding a different
                // binding), don't overwrite/erase an unrelated binding — leave both actions
                // untouched and skip the swap.
                auto* conflictAction = map.GetAction(pending.Conflict.ConflictingAction);
                if (!conflictAction || pending.Conflict.ConflictingBindingIndex >= conflictAction->Bindings.size() || conflictAction->Bindings[pending.Conflict.ConflictingBindingIndex] != pending.Binding)
                {
                    break;
                }

                // Hand the conflicting action the target slot's OLD binding, then assign the new
                // one to the target. When the target has no old binding (a fresh binding), swap
                // degenerates to replace — the conflicting action simply loses the binding.
                if (!pending.IsNewBinding)
                {
                    conflictAction->Bindings[pending.Conflict.ConflictingBindingIndex] = target->Bindings[pending.BindingIndex];
                }
                else
                {
                    conflictAction->Bindings.erase(conflictAction->Bindings.begin() + static_cast<std::ptrdiff_t>(pending.Conflict.ConflictingBindingIndex));
                }

                ApplyBinding(map, pending.ActionName, pending.BindingIndex, pending.IsNewBinding, pending.Binding, /*removeFromConflicts=*/false);
                break;
            }
            case RebindResolution::Cancel:
                break; // handled above
        }
    }

} // namespace OloEngine
