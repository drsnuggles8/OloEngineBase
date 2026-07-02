#include "OloEnginePCH.h"
#include "RuntimeInputRebindMenu.h"

#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Events/MouseEvent.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/UI/UINavigationSystem.h"

#include <algorithm>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Panel-relative pixel layout (top-left origin, Y-down — matches UILayoutSystem).
        constexpr f32 kPanelWidth = 780.0f;
        constexpr f32 kHeaderHeight = 76.0f;
        constexpr f32 kRowHeight = 44.0f;
        constexpr f32 kFooterHeight = 72.0f;
        constexpr f32 kButtonHeight = 32.0f;
        constexpr f32 kPad = 20.0f;

        // Column x/width within the panel.
        constexpr f32 kActionX = 20.0f;
        constexpr f32 kActionW = 165.0f;
        constexpr f32 kBindingsX = 190.0f;
        constexpr f32 kBindingsW = 300.0f;
        constexpr f32 kRebindX = 498.0f;
        constexpr f32 kRebindW = 96.0f;
        constexpr f32 kPadX = 600.0f;
        constexpr f32 kPadW = 68.0f;
        constexpr f32 kResetX = 675.0f;
        constexpr f32 kResetW = 85.0f;

        constexpr glm::vec4 kPanelColor = { 0.10f, 0.10f, 0.13f, 0.97f };
        constexpr glm::vec4 kOverlayColor = { 0.03f, 0.03f, 0.05f, 0.82f };
        constexpr glm::vec4 kTitleColor = { 1.0f, 0.95f, 0.6f, 1.0f };
        constexpr glm::vec4 kLabelColor = { 0.92f, 0.92f, 0.92f, 1.0f };
        constexpr glm::vec4 kBindingsColor = { 0.6f, 0.85f, 1.0f, 1.0f };
        constexpr glm::vec4 kStatusColor = { 0.6f, 0.9f, 0.6f, 1.0f };
        constexpr glm::vec4 kCaptureColor = { 1.0f, 0.95f, 0.7f, 1.0f };
        constexpr glm::vec4 kConflictColor = { 1.0f, 0.6f, 0.55f, 1.0f };

        constexpr glm::vec4 kBtnNormal = { 0.24f, 0.27f, 0.34f, 1.0f };
        constexpr glm::vec4 kBtnHovered = { 0.34f, 0.40f, 0.50f, 1.0f };
        constexpr glm::vec4 kBtnPressed = { 0.16f, 0.19f, 0.24f, 1.0f };
        constexpr glm::vec4 kBtnDisabled = { 0.15f, 0.15f, 0.18f, 0.5f };

        // Off-screen sentinel used to hide an overlay subtree without destroying it.
        constexpr glm::vec2 kOffscreen = { 100000.0f, 100000.0f };

        std::string SummariseBindings(const InputAction& action)
        {
            if (action.Bindings.empty())
            {
                return "(unbound)";
            }
            std::string out;
            for (sizet i = 0; i < action.Bindings.size(); ++i)
            {
                if (i > 0)
                {
                    out += ", ";
                }
                out += action.Bindings[i].GetDisplayName();
            }
            return out;
        }

        // Destroy an entity and all its descendants. Scene::DestroyEntity does NOT cascade to
        // children, so a plain DestroyEntity(canvas) would orphan every widget the menu built.
        // Children are collected before destroying so handles stay valid, then destroyed
        // depth-first (leaves before parents).
        void DestroyUITree(Scene& scene, Entity root)
        {
            if (!root)
            {
                return;
            }
            std::vector<Entity> children;
            if (root.HasComponent<RelationshipComponent>())
            {
                for (const UUID childUUID : root.GetComponent<RelationshipComponent>().m_Children)
                {
                    if (const auto childOpt = scene.TryGetEntityWithUUID(childUUID))
                    {
                        children.emplace_back(static_cast<entt::entity>(*childOpt), &scene);
                    }
                }
            }
            for (Entity child : children)
            {
                DestroyUITree(scene, child);
            }
            scene.DestroyEntity(root);
        }
    } // namespace

    Entity RuntimeInputRebindMenu::MakePanel(Entity parent, glm::vec2 anchorMin, glm::vec2 anchorMax, glm::vec2 anchoredPos, glm::vec2 size, glm::vec4 color)
    {
        Entity e = m_Scene->CreateEntity("RebindPanel");
        auto& rt = e.AddComponent<UIRectTransformComponent>();
        rt.m_AnchorMin = anchorMin;
        rt.m_AnchorMax = anchorMax;
        rt.m_AnchoredPosition = anchoredPos;
        rt.m_SizeDelta = size;
        rt.m_Pivot = anchorMin; // pivot at the anchor point (top-left children, centred panels)

        auto& panel = e.AddComponent<UIPanelComponent>();
        panel.m_BackgroundColor = color;

        e.SetParent(parent);
        return e;
    }

    Entity RuntimeInputRebindMenu::MakeText(Entity parent, const std::string& text, glm::vec2 anchoredPos, glm::vec2 size, f32 fontSize, glm::vec4 color)
    {
        Entity e = m_Scene->CreateEntity("RebindText");
        auto& rt = e.AddComponent<UIRectTransformComponent>();
        rt.m_AnchorMin = { 0.0f, 0.0f };
        rt.m_AnchorMax = { 0.0f, 0.0f };
        rt.m_AnchoredPosition = anchoredPos;
        rt.m_SizeDelta = size;
        rt.m_Pivot = { 0.0f, 0.0f };

        auto& txt = e.AddComponent<UITextComponent>();
        txt.m_Text = text;
        txt.m_FontSize = fontSize;
        txt.m_Color = color;
        txt.m_Alignment = UITextAlignment::MiddleLeft;

        e.SetParent(parent);
        return e;
    }

    Entity RuntimeInputRebindMenu::MakeButton(Entity parent, const std::string& label, glm::vec2 anchoredPos, glm::vec2 size)
    {
        Entity e = m_Scene->CreateEntity("RebindButton");
        auto& rt = e.AddComponent<UIRectTransformComponent>();
        rt.m_AnchorMin = { 0.0f, 0.0f };
        rt.m_AnchorMax = { 0.0f, 0.0f };
        rt.m_AnchoredPosition = anchoredPos;
        rt.m_SizeDelta = size;
        rt.m_Pivot = { 0.0f, 0.0f };

        auto& btn = e.AddComponent<UIButtonComponent>();
        btn.m_NormalColor = kBtnNormal;
        btn.m_HoveredColor = kBtnHovered;
        btn.m_PressedColor = kBtnPressed;
        btn.m_DisabledColor = kBtnDisabled;

        e.SetParent(parent);

        // Centred caption stretched over the button.
        Entity caption = m_Scene->CreateEntity("RebindButtonLabel");
        auto& crt = caption.AddComponent<UIRectTransformComponent>();
        crt.m_AnchorMin = { 0.0f, 0.0f };
        crt.m_AnchorMax = { 1.0f, 1.0f };
        crt.m_AnchoredPosition = { 0.0f, 0.0f };
        crt.m_SizeDelta = { 0.0f, 0.0f };
        crt.m_Pivot = { 0.0f, 0.0f };
        auto& ctxt = caption.AddComponent<UITextComponent>();
        ctxt.m_Text = label;
        ctxt.m_FontSize = 16.0f;
        ctxt.m_Color = kLabelColor;
        ctxt.m_Alignment = UITextAlignment::MiddleCenter;
        caption.SetParent(e);

        m_AllButtons.push_back(e);
        return e;
    }

    void RuntimeInputRebindMenu::Open(Scene& scene, InputContextType ctx, std::filesystem::path savePath)
    {
        if (m_Open)
        {
            Close();
        }

        m_Scene = &scene;
        m_Controller.SetTargetContext(ctx);
        m_SavePath = std::move(savePath);
        m_CloseRequested = false;
        m_AllButtons.clear();
        m_PrevButtonState.clear();
        m_Rows.clear();

        // Root canvas covering the whole screen, drawn above gameplay UI.
        m_Canvas = m_Scene->CreateEntity("RebindMenuCanvas");
        auto& canvas = m_Canvas.AddComponent<UICanvasComponent>();
        canvas.m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
        canvas.m_ScaleMode = UICanvasScaleMode::ScaleWithScreenSize;
        canvas.m_SortOrder = 1000;
        auto& canvasRT = m_Canvas.AddComponent<UIRectTransformComponent>();
        canvasRT.m_AnchorMin = { 0.0f, 0.0f };
        canvasRT.m_AnchorMax = { 1.0f, 1.0f };
        canvasRT.m_AnchoredPosition = { 0.0f, 0.0f };
        canvasRT.m_SizeDelta = { 0.0f, 0.0f };
        canvasRT.m_Pivot = { 0.0f, 0.0f };

        // Collect and sort the action names for a stable layout.
        std::vector<std::string> actionNames;
        {
            const InputActionMap& map = m_Controller.TargetMap();
            actionNames.reserve(map.Actions.size());
            for (const auto& [name, _] : map.Actions)
            {
                actionNames.push_back(name);
            }
        }
        std::ranges::sort(actionNames);

        const f32 panelHeight = kHeaderHeight + static_cast<f32>(actionNames.size()) * kRowHeight + kFooterHeight;

        // Dimmed full-screen backdrop, then the centred panel.
        MakePanel(m_Canvas, { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.55f });
        Entity panel = MakePanel(m_Canvas, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { kPanelWidth, panelHeight }, kPanelColor);

        MakeText(panel, "Rebind Controls", { kPad, 14.0f }, { kPanelWidth - 2.0f * kPad, 30.0f }, 26.0f, kTitleColor);
        m_StatusLabel = MakeText(panel, "", { kPad, 46.0f }, { kPanelWidth - 2.0f * kPad, 22.0f }, 15.0f, kStatusColor);

        // One row per action.
        for (sizet i = 0; i < actionNames.size(); ++i)
        {
            const f32 rowY = kHeaderHeight + static_cast<f32>(i) * kRowHeight;
            const f32 btnY = rowY + (kRowHeight - kButtonHeight) * 0.5f;

            Row row;
            row.Action = actionNames[i];
            MakeText(panel, actionNames[i], { kActionX, rowY }, { kActionW, kRowHeight }, 17.0f, kLabelColor);
            row.BindingsLabel = MakeText(panel, "", { kBindingsX, rowY }, { kBindingsW, kRowHeight }, 15.0f, kBindingsColor);
            row.RebindButton = MakeButton(panel, "Rebind", { kRebindX, btnY }, { kRebindW, kButtonHeight });
            row.PadButton = MakeButton(panel, "Pad", { kPadX, btnY }, { kPadW, kButtonHeight });
            row.ResetButton = MakeButton(panel, "Reset", { kResetX, btnY }, { kResetW, kButtonHeight });
            m_Rows.push_back(row);
        }

        // Footer buttons.
        const f32 footerY = kHeaderHeight + static_cast<f32>(actionNames.size()) * kRowHeight + 16.0f;
        m_ResetAllButton = MakeButton(panel, "Reset All to Default", { kActionX, footerY }, { 200.0f, 36.0f });
        m_SaveButton = MakeButton(panel, "Save", { 236.0f, footerY }, { 120.0f, 36.0f });
        m_CloseButton = MakeButton(panel, "Close", { kResetX - 20.0f, footerY }, { 105.0f, 36.0f });

        // --- Capture overlay (hidden until listening) ---
        m_CaptureOverlay = MakePanel(m_Canvas, { 0.0f, 0.0f }, { 1.0f, 1.0f }, kOffscreen, { 0.0f, 0.0f }, kOverlayColor);
        m_CaptureLabel = MakeText(m_CaptureOverlay, "", { 0.0f, 0.0f }, { kPanelWidth, 60.0f }, 24.0f, kCaptureColor);
        {
            // Centre the capture label on screen.
            auto& rt = m_CaptureLabel.GetComponent<UIRectTransformComponent>();
            rt.m_AnchorMin = { 0.5f, 0.5f };
            rt.m_AnchorMax = { 0.5f, 0.5f };
            rt.m_Pivot = { 0.5f, 0.5f };
            m_CaptureLabel.GetComponent<UITextComponent>().m_Alignment = UITextAlignment::MiddleCenter;
        }

        // --- Conflict overlay (hidden until a collision needs resolving) ---
        m_ConflictOverlay = MakePanel(m_Canvas, { 0.0f, 0.0f }, { 1.0f, 1.0f }, kOffscreen, { 0.0f, 0.0f }, kOverlayColor);
        Entity conflictPanel = MakePanel(m_ConflictOverlay, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 560.0f, 180.0f }, kPanelColor);
        m_ConflictLabel = MakeText(conflictPanel, "", { 20.0f, 20.0f }, { 520.0f, 70.0f }, 18.0f, kConflictColor);
        m_ConflictLabel.GetComponent<UITextComponent>().m_Alignment = UITextAlignment::MiddleCenter;
        m_ReplaceButton = MakeButton(conflictPanel, "Replace", { 30.0f, 120.0f }, { 150.0f, 40.0f });
        m_SwapButton = MakeButton(conflictPanel, "Swap", { 205.0f, 120.0f }, { 150.0f, 40.0f });
        m_CancelButton = MakeButton(conflictPanel, "Cancel", { 380.0f, 120.0f }, { 150.0f, 40.0f });

        m_Open = true;
        // This panel is a self-contained modal that handles its own input; suppress UI
        // navigation while it is open so a gamepad/keyboard press during capture is grabbed
        // only by the rebind controller and does not also drive menu navigation.
        m_Scene->GetUINavigation().SetInputSuppressed(true);
        RefreshLabels();
        RefreshOverlays();
    }

    void RuntimeInputRebindMenu::Close()
    {
        if (m_Scene && m_Canvas)
        {
            // Scene::DestroyEntity does not cascade, so tear down the whole canvas subtree.
            DestroyUITree(*m_Scene, m_Canvas);
            m_Scene->GetUINavigation().SetInputSuppressed(false);
        }
        m_Canvas = {};
        m_Rows.clear();
        m_AllButtons.clear();
        m_PrevButtonState.clear();
        m_Controller.CancelCapture();
        m_Open = false;
        m_CloseRequested = false;
        // Drop the scene reference last — after teardown that needs it — so a closed menu
        // never holds a pointer to a scene that may be destroyed next.
        m_Scene = nullptr;
    }

    void RuntimeInputRebindMenu::OnUpdate()
    {
        if (!m_Open || !m_Scene)
        {
            return;
        }

        // Drive gamepad capture (polled), then react to any resulting conflict/capture change.
        m_Controller.PollGamepad();

        if (m_Controller.HasPendingConflict())
        {
            // Only the conflict overlay is interactive while a collision is pending.
            if (Clicked(m_ReplaceButton))
            {
                m_Controller.ResolveConflict(RebindResolution::Replace);
            }
            else if (Clicked(m_SwapButton))
            {
                m_Controller.ResolveConflict(RebindResolution::Swap);
            }
            else if (Clicked(m_CancelButton))
            {
                m_Controller.ResolveConflict(RebindResolution::Cancel);
            }
        }
        else if (!m_Controller.IsCapturing())
        {
            // Normal interaction: rows + footer.
            for (const Row& row : m_Rows)
            {
                const InputAction* action = m_Controller.TargetMap().GetAction(row.Action);
                const bool hasBinding = action && !action->Bindings.empty();

                if (Clicked(row.RebindButton))
                {
                    // Rebind the primary binding, or start a fresh one if none.
                    if (hasBinding)
                    {
                        m_Controller.BeginRebind(row.Action, 0, /*gamepad=*/false);
                    }
                    else
                    {
                        m_Controller.BeginCaptureNew(row.Action, /*gamepad=*/false);
                    }
                }
                else if (Clicked(row.PadButton))
                {
                    m_Controller.BeginCaptureNew(row.Action, /*gamepad=*/true);
                }
                else if (Clicked(row.ResetButton))
                {
                    m_Controller.ResetActionToDefault(row.Action);
                    m_StatusLabel.GetComponent<UITextComponent>().m_Text = "Reset '" + row.Action + "' to default.";
                }

                // A rebind/pad click begins capture; stop here so a second click this frame
                // can't overwrite the capture we just started (and skip the footer below).
                if (m_Controller.IsCapturing())
                {
                    break;
                }
            }

            if (!m_Controller.IsCapturing())
            {
                if (Clicked(m_ResetAllButton))
                {
                    m_Controller.ResetTargetMapToDefault();
                    m_StatusLabel.GetComponent<UITextComponent>().m_Text = "Reset all actions to default.";
                }
                else if (Clicked(m_SaveButton))
                {
                    const bool ok = !m_SavePath.empty() && InputRebindController::Save(m_SavePath);
                    m_StatusLabel.GetComponent<UITextComponent>().m_Text = ok ? "Saved input bindings." : "Save failed (no project path).";
                }
                else if (Clicked(m_CloseButton))
                {
                    m_CloseRequested = true;
                }
            }
        }

        RefreshLabels();
        RefreshOverlays();
        UpdateButtonStates();

        if (m_CloseRequested)
        {
            Close();
        }
    }

    bool RuntimeInputRebindMenu::OnEvent(Event& e)
    {
        if (!m_Open || m_Controller.GetCaptureMode() != InputRebindController::CaptureMode::KeyboardMouse)
        {
            return false;
        }

        bool handled = false;
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>(
            [this, &handled](const KeyPressedEvent& ev)
            {
                handled = m_Controller.OnKeyPressed(ev.GetKeyCode());
                return handled;
            });
        dispatcher.Dispatch<MouseButtonPressedEvent>(
            [this, &handled](const MouseButtonPressedEvent& ev)
            {
                handled = m_Controller.OnMouseButtonPressed(ev.GetMouseButton());
                return handled;
            });
        return handled;
    }

    void RuntimeInputRebindMenu::RefreshLabels()
    {
        const InputActionMap& map = m_Controller.TargetMap();
        for (const Row& row : m_Rows)
        {
            Entity label = row.BindingsLabel; // non-const handle for the mutable GetComponent overload
            if (!label)
            {
                continue;
            }
            const InputAction* action = map.GetAction(row.Action);
            label.GetComponent<UITextComponent>().m_Text = action ? SummariseBindings(*action) : "(removed)";
        }
    }

    void RuntimeInputRebindMenu::RefreshOverlays()
    {
        const bool capturing = m_Controller.IsCapturing();
        const bool conflict = m_Controller.HasPendingConflict();

        SetActive(m_CaptureOverlay, capturing);
        SetActive(m_ConflictOverlay, conflict);

        // The overlays are visually modal, but the ECS UI input system has no z-order —
        // an overlay panel does not block the row/footer buttons behind it, and a click on
        // an overlay button that overlaps a row button can be consumed by the row button
        // instead. Gate interaction explicitly: while any overlay is up, disable every button,
        // then re-enable only the conflict-resolution buttons during a pending conflict.
        const bool overlayActive = capturing || conflict;
        for (Entity button : m_AllButtons)
        {
            if (button && button.HasComponent<UIButtonComponent>())
            {
                button.GetComponent<UIButtonComponent>().m_Interactable = !overlayActive;
            }
        }
        for (Entity button : { m_ReplaceButton, m_SwapButton, m_CancelButton })
        {
            if (button && button.HasComponent<UIButtonComponent>())
            {
                button.GetComponent<UIButtonComponent>().m_Interactable = conflict;
            }
        }

        if (capturing && m_CaptureLabel)
        {
            const std::string device = (m_Controller.GetCaptureMode() == InputRebindController::CaptureMode::Gamepad) ? "gamepad button" : "key or mouse button";
            m_CaptureLabel.GetComponent<UITextComponent>().m_Text =
                "Press a " + device + " for '" + std::string(m_Controller.GetCaptureActionName()) + "'\n(Esc to cancel)";
        }

        if (conflict && m_ConflictLabel)
        {
            const RebindConflict& c = m_Controller.GetPendingConflict();
            m_ConflictLabel.GetComponent<UITextComponent>().m_Text =
                "'" + c.Binding.GetDisplayName() + "' is already used by '" + c.ConflictingAction + "'.\nReplace, swap, or cancel?";
        }
    }

    bool RuntimeInputRebindMenu::Clicked(Entity button) const
    {
        if (!button || !button.HasComponent<UIButtonComponent>())
        {
            return false;
        }
        const UIButtonState cur = button.GetComponent<UIButtonComponent>().m_State;
        const auto it = m_PrevButtonState.find(button.GetUUID());
        const UIButtonState prev = (it != m_PrevButtonState.end()) ? it->second : UIButtonState::Normal;
        // A click is a press released back onto the button (press-then-release edge).
        return prev == UIButtonState::Pressed && cur == UIButtonState::Hovered;
    }

    void RuntimeInputRebindMenu::UpdateButtonStates()
    {
        for (const Entity button : m_AllButtons)
        {
            if (button && button.HasComponent<UIButtonComponent>())
            {
                m_PrevButtonState[button.GetUUID()] = button.GetComponent<UIButtonComponent>().m_State;
            }
        }
    }

    void RuntimeInputRebindMenu::SetActive(Entity root, bool visible)
    {
        if (!root || !root.HasComponent<UIRectTransformComponent>())
        {
            return;
        }
        // Hide by parking the subtree off-screen; the layout walk moves all children with it.
        root.GetComponent<UIRectTransformComponent>().m_AnchoredPosition = visible ? glm::vec2{ 0.0f, 0.0f } : kOffscreen;
    }

} // namespace OloEngine
