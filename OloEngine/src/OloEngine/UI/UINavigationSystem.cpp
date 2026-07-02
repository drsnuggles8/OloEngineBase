#include "OloEnginePCH.h"
#include "UINavigationSystem.h"

#include "OloEngine/Core/GamepadCodes.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // A widget that participates in focus navigation, with its screen-space
        // centre resolved this frame.
        struct NavItem
        {
            UUID Id{ 0 };
            glm::vec2 Center{ 0.0f, 0.0f };
        };

        // Interactable == the widget currently accepts input. Mirrors the
        // per-type m_Interactable gate UIInputSystem uses.
        [[nodiscard]] bool IsNavigable(Entity entity)
        {
            if (entity.HasComponent<UIButtonComponent>())
                return entity.GetComponent<UIButtonComponent>().m_Interactable;
            if (entity.HasComponent<UISliderComponent>())
                return entity.GetComponent<UISliderComponent>().m_Interactable;
            if (entity.HasComponent<UICheckboxComponent>())
                return entity.GetComponent<UICheckboxComponent>().m_Interactable;
            if (entity.HasComponent<UIToggleComponent>())
                return entity.GetComponent<UIToggleComponent>().m_Interactable;
            if (entity.HasComponent<UIDropdownComponent>())
                return entity.GetComponent<UIDropdownComponent>().m_Interactable;
            if (entity.HasComponent<UIInputFieldComponent>())
                return entity.GetComponent<UIInputFieldComponent>().m_Interactable;
            return false;
        }

        // Gather every navigable widget with a resolved rect into `out`.
        void CollectNavItems(Scene& scene, std::vector<NavItem>& out)
        {
            for (const auto view = scene.GetAllEntitiesWith<UIResolvedRectComponent>(); const auto e : view)
            {
                Entity entity{ e, &scene };
                if (!IsNavigable(entity))
                    continue;

                const auto& resolved = view.get<UIResolvedRectComponent>(e);
                out.push_back(NavItem{ entity.GetUUID(), resolved.m_Position + resolved.m_Size * 0.5f });
            }
        }

        // Find the best navigation target from `fromCenter` in a screen-space
        // direction (dir is a unit axis; screen origin is top-left, +Y downwards).
        // Scores candidates by primary-axis travel plus a perpendicular-offset
        // penalty, the classic 2D focus-navigation heuristic. UUID(0) if none.
        [[nodiscard]] UUID BestInDirection(const std::vector<NavItem>& items, const glm::vec2& fromCenter, const glm::vec2& dir)
        {
            constexpr f32 threshold = 1.0f; // must move at least a pixel along dir
            UUID best{ 0 };
            f32 bestScore = std::numeric_limits<f32>::max();

            for (const auto& item : items)
            {
                const glm::vec2 delta = item.Center - fromCenter;
                const f32 along = glm::dot(delta, dir);
                if (along < threshold)
                    continue; // behind us or same row/column

                // Perpendicular distance = |delta| projected off the dir axis.
                const f32 perp = std::abs(delta.x * dir.y - delta.y * dir.x);
                if (const f32 score = along + 2.0f * perp; score < bestScore)
                {
                    bestScore = score;
                    best = item.Id;
                }
            }
            return best;
        }

        // Top-most, then left-most navigable widget — the seed focus when a nav
        // key is pressed while nothing is focused.
        [[nodiscard]] UUID FirstNavItem(const std::vector<NavItem>& items)
        {
            UUID first{ 0 };
            glm::vec2 bestPos{ std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max() };
            for (const auto& item : items)
            {
                if (item.Center.y < bestPos.y || (std::abs(item.Center.y - bestPos.y) < 1.0f && item.Center.x < bestPos.x))
                {
                    bestPos = item.Center;
                    first = item.Id;
                }
            }
            return first;
        }
    } // namespace

    // Seed change-detection snapshots for any widget seen for the first time,
    // using its value BEFORE this frame's navigation runs. Without this, a widget
    // adjusted on the very first frame it appears would record its post-change
    // value as the baseline and never fire.
    void UINavigationSystem::SeedSnapshots(Scene& scene, UINavigation& nav)
    {
        for (const auto view = scene.GetAllEntitiesWith<UISliderComponent>(); const auto e : view)
        {
            const auto id = static_cast<u64>(Entity{ e, &scene }.GetUUID());
            nav.m_PrevSlider.try_emplace(id, view.get<UISliderComponent>(e).m_Value);
        }
        for (const auto view = scene.GetAllEntitiesWith<UICheckboxComponent>(); const auto e : view)
        {
            const auto id = static_cast<u64>(Entity{ e, &scene }.GetUUID());
            nav.m_PrevCheckbox.try_emplace(id, view.get<UICheckboxComponent>(e).m_IsChecked);
        }
        for (const auto view = scene.GetAllEntitiesWith<UIToggleComponent>(); const auto e : view)
        {
            const auto id = static_cast<u64>(Entity{ e, &scene }.GetUUID());
            nav.m_PrevToggle.try_emplace(id, view.get<UIToggleComponent>(e).m_IsOn);
        }
        for (const auto view = scene.GetAllEntitiesWith<UIButtonComponent>(); const auto e : view)
        {
            const auto id = static_cast<u64>(Entity{ e, &scene }.GetUUID());
            nav.m_PrevButtonState.try_emplace(id, static_cast<u8>(view.get<UIButtonComponent>(e).m_State));
        }
    }

    // Activate the focused widget (gamepad South / Enter). Buttons fire their
    // click+submit delegates directly; checkboxes/toggles flip their value (the
    // change-detection pass then fires OnValueChanged, so mouse and pad share one
    // dispatch path). Sliders adjust via Left/Right, not Activate.
    void UINavigationSystem::ActivateFocused(Scene& scene, UINavigation& nav)
    {
        const UUID focused = nav.GetFocus();
        // TryGetEntityWithUUID, NOT GetEntityByUUID: focus can point at an entity
        // destroyed since last frame, and GetEntityByUUID asserts / does a checked
        // lookup (UB) on a missing UUID rather than returning a null Entity.
        auto entityOpt = scene.TryGetEntityWithUUID(focused);
        if (!entityOpt)
            return;
        Entity entity = *entityOpt;

        if (entity.HasComponent<UIButtonComponent>())
        {
            if (entity.GetComponent<UIButtonComponent>().m_Interactable)
            {
                nav.FireClick(focused);
                nav.FireSubmit(focused);
            }
        }
        else if (entity.HasComponent<UICheckboxComponent>())
        {
            if (auto& cb = entity.GetComponent<UICheckboxComponent>(); cb.m_Interactable)
                cb.m_IsChecked = !cb.m_IsChecked;
        }
        else if (entity.HasComponent<UIToggleComponent>())
        {
            if (auto& tg = entity.GetComponent<UIToggleComponent>(); tg.m_Interactable)
                tg.m_IsOn = !tg.m_IsOn;
        }
    }

    // Left/Right on a focused slider nudges its value by kSliderNavStep of the
    // range. Returns true if the focused widget was a slider (so Left/Right is
    // consumed as an adjustment rather than a focus move).
    bool UINavigationSystem::AdjustFocusedSlider(Scene& scene, UINavigation& nav, const UINavInput& input)
    {
        auto entityOpt = scene.TryGetEntityWithUUID(nav.GetFocus());
        if (!entityOpt || !entityOpt->HasComponent<UISliderComponent>())
            return false;

        auto& slider = entityOpt->GetComponent<UISliderComponent>();
        if (!slider.m_Interactable)
            return true; // still a slider — swallow Left/Right, just don't move

        const f32 range = slider.m_MaxValue - slider.m_MinValue;
        if (range > 0.0f && (input.NavLeft || input.NavRight))
        {
            const f32 step = range * kSliderNavStep;
            const f32 delta = (input.NavRight ? step : 0.0f) - (input.NavLeft ? step : 0.0f);
            slider.m_Value = glm::clamp(slider.m_Value + delta, slider.m_MinValue, slider.m_MaxValue);
        }
        return true;
    }

    // After navigation, mirror keyboard/pad focus onto the button hover state so
    // the existing renderer shows the focused button highlighted — but only when
    // the mouse hasn't already put it into Pressed/Disabled.
    void UINavigationSystem::ReflectFocusOnButtons(Scene& scene, UINavigation& nav)
    {
        const auto focused = static_cast<u64>(nav.GetFocus());
        for (const auto view = scene.GetAllEntitiesWith<UIButtonComponent>(); const auto e : view)
        {
            auto& button = view.get<UIButtonComponent>(e);
            if (!button.m_Interactable)
                continue;

            const auto id = static_cast<u64>(Entity{ e, &scene }.GetUUID());
            if (id == focused && button.m_State == UIButtonState::Normal)
                button.m_State = UIButtonState::Hovered;
        }
    }

    // Fire OnValueChanged / OnClick for changes since last frame, regardless of
    // whether the source was the mouse (UIInputSystem) or this navigation pass.
    //
    // Delegates are arbitrary user callbacks that may mutate the registry (add /
    // remove a UI component, destroy an entity). So the changes are collected
    // during view iteration and the callbacks are fired only AFTER every view has
    // been walked — firing mid-iteration would invalidate the entt view iterator
    // (the hazard documented in GameplayEventBus.h). Snapshot entries for widgets
    // that no longer exist are pruned in the same passes so the maps don't grow
    // unbounded across a runtime session.
    void UINavigationSystem::DispatchChanges(Scene& scene, UINavigation& nav)
    {
        constexpr f32 epsilon = 1e-6f;

        std::vector<std::pair<UUID, f32>> pendingValueChanged;
        std::vector<UUID> pendingClicks;
        std::unordered_set<u64> liveSliders, liveCheckboxes, liveToggles, liveButtons;

        for (const auto view = scene.GetAllEntitiesWith<UISliderComponent>(); const auto e : view)
        {
            const UUID uuid = Entity{ e, &scene }.GetUUID();
            liveSliders.insert(static_cast<u64>(uuid));
            const f32 cur = view.get<UISliderComponent>(e).m_Value;
            if (auto it = nav.m_PrevSlider.find(static_cast<u64>(uuid)); it != nav.m_PrevSlider.end())
            {
                if (std::abs(cur - it->second) > epsilon)
                {
                    pendingValueChanged.emplace_back(uuid, cur);
                    it->second = cur;
                }
            }
        }
        for (const auto view = scene.GetAllEntitiesWith<UICheckboxComponent>(); const auto e : view)
        {
            const UUID uuid = Entity{ e, &scene }.GetUUID();
            liveCheckboxes.insert(static_cast<u64>(uuid));
            const bool cur = view.get<UICheckboxComponent>(e).m_IsChecked;
            if (auto it = nav.m_PrevCheckbox.find(static_cast<u64>(uuid)); it != nav.m_PrevCheckbox.end() && it->second != cur)
            {
                pendingValueChanged.emplace_back(uuid, cur ? 1.0f : 0.0f);
                it->second = cur;
            }
        }
        for (const auto view = scene.GetAllEntitiesWith<UIToggleComponent>(); const auto e : view)
        {
            const UUID uuid = Entity{ e, &scene }.GetUUID();
            liveToggles.insert(static_cast<u64>(uuid));
            const bool cur = view.get<UIToggleComponent>(e).m_IsOn;
            if (auto it = nav.m_PrevToggle.find(static_cast<u64>(uuid)); it != nav.m_PrevToggle.end() && it->second != cur)
            {
                pendingValueChanged.emplace_back(uuid, cur ? 1.0f : 0.0f);
                it->second = cur;
            }
        }
        // Mouse-driven button click: a press-then-release-over transition shows up
        // as Pressed -> Hovered between two frames. Gamepad Activate fires click
        // directly (in ActivateFocused, outside iteration) without touching
        // m_State, so it never double-fires here.
        for (const auto view = scene.GetAllEntitiesWith<UIButtonComponent>(); const auto e : view)
        {
            const UUID uuid = Entity{ e, &scene }.GetUUID();
            liveButtons.insert(static_cast<u64>(uuid));
            const auto cur = static_cast<u8>(view.get<UIButtonComponent>(e).m_State);
            if (auto it = nav.m_PrevButtonState.find(static_cast<u64>(uuid)); it != nav.m_PrevButtonState.end())
            {
                if (it->second == static_cast<u8>(UIButtonState::Pressed) && cur == static_cast<u8>(UIButtonState::Hovered))
                    pendingClicks.push_back(uuid);
                it->second = cur;
            }
        }

        // Prune snapshots for widgets that vanished since they were seeded.
        std::erase_if(nav.m_PrevSlider, [&](const auto& kv)
                      { return !liveSliders.contains(kv.first); });
        std::erase_if(nav.m_PrevCheckbox, [&](const auto& kv)
                      { return !liveCheckboxes.contains(kv.first); });
        std::erase_if(nav.m_PrevToggle, [&](const auto& kv)
                      { return !liveToggles.contains(kv.first); });
        std::erase_if(nav.m_PrevButtonState, [&](const auto& kv)
                      { return !liveButtons.contains(kv.first); });

        // Fire outside all view iteration — a handler may now safely mutate the scene.
        for (const auto& [uuid, value] : pendingValueChanged)
            nav.FireValueChanged(uuid, value);
        for (const UUID uuid : pendingClicks)
        {
            nav.FireClick(uuid);
            nav.FireSubmit(uuid);
        }
    }

    void UINavigationSystem::Update(Scene& scene, const UINavInput& input)
    {
        OLO_PROFILE_FUNCTION();

        UINavigation& nav = scene.GetUINavigation();

        // A modal that captures raw input for itself (e.g. the rebind menu's
        // "press a button…" mode) suppresses navigation: ignore this frame's
        // directional/activate/cancel input so the press is not double-handled.
        // Mouse-driven delegate dispatch (step 2) still runs.
        const UINavInput navInput = nav.IsInputSuppressed() ? UINavInput{} : input;

        // 0. Baseline snapshots for change detection (pre-navigation values).
        SeedSnapshots(scene, nav);

        // 1. Navigation + activation.
        std::vector<NavItem> items;
        CollectNavItems(scene, items);

        // Drop focus that no longer points at a navigable widget (destroyed /
        // made non-interactable since last frame). TryGetEntityWithUUID returns
        // nullopt for a stale UUID; GetEntityByUUID would assert / UB instead.
        if (nav.HasFocus())
        {
            auto current = scene.TryGetEntityWithUUID(nav.GetFocus());
            if (!current || !IsNavigable(*current))
                nav.ClearFocus();
        }

        const bool anyDir = navInput.NavUp || navInput.NavDown || navInput.NavLeft || navInput.NavRight;

        if (!nav.HasFocus())
        {
            // First directional input seeds focus rather than moving.
            if (anyDir && !items.empty())
                nav.SetFocus(FirstNavItem(items));
        }
        else
        {
            // Left/Right adjusts a focused slider instead of moving focus.
            const bool sliderConsumed = AdjustFocusedSlider(scene, nav, navInput);

            glm::vec2 fromCenter{ 0.0f, 0.0f };
            bool focusInItems = false;
            for (const auto& item : items)
            {
                if (static_cast<u64>(item.Id) == static_cast<u64>(nav.GetFocus()))
                {
                    fromCenter = item.Center;
                    focusInItems = true;
                    break;
                }
            }

            // Only move focus directionally when the focused widget's on-screen
            // rect is known this frame. A focused widget with no resolved rect
            // (spawned pre-layout) is absent from `items`; navigating from a
            // {0,0} origin would jump to whatever sits nearest the screen corner.
            UUID target{ 0 };
            if (focusInItems)
            {
                if (navInput.NavUp)
                    target = BestInDirection(items, fromCenter, { 0.0f, -1.0f });
                else if (navInput.NavDown)
                    target = BestInDirection(items, fromCenter, { 0.0f, 1.0f });
                else if (navInput.NavLeft && !sliderConsumed)
                    target = BestInDirection(items, fromCenter, { -1.0f, 0.0f });
                else if (navInput.NavRight && !sliderConsumed)
                    target = BestInDirection(items, fromCenter, { 1.0f, 0.0f });
            }

            if (static_cast<u64>(target) != 0)
                nav.SetFocus(target);

            if (navInput.Activate)
                ActivateFocused(scene, nav);
        }

        if (navInput.Cancel)
            nav.ClearFocus();

        // 2. Dispatch value/click delegates for changes from any source. Runs
        //    BEFORE focus reflection so the mouse-click detector reads the honest
        //    UIInputSystem button state, not the focus-highlight overlay this
        //    system paints on in step 3.
        DispatchChanges(scene, nav);

        // 3. Reflect focus onto button visuals via the existing hover state.
        ReflectFocusOnButtons(scene, nav);
    }

    UINavInput UINavigationSystem::PollActions()
    {
        UINavInput in;
        in.NavUp = InputActionManager::IsActionJustPressed("MenuUp");
        in.NavDown = InputActionManager::IsActionJustPressed("MenuDown");
        in.NavLeft = InputActionManager::IsActionJustPressed("MenuLeft");
        in.NavRight = InputActionManager::IsActionJustPressed("MenuRight");
        in.Activate = InputActionManager::IsActionJustPressed("MenuActivate");
        in.Cancel = InputActionManager::IsActionJustPressed("MenuCancel");
        return in;
    }

    void UINavigationSystem::InstallDefaultMenuActions()
    {
        InputActionMap& map = InputActionManager::GetActionMapMutable(InputContextType::Menu);
        if (map.Name.empty())
            map.Name = "Menu";

        const auto add = [&map](const char* name, std::vector<InputBinding> bindings)
        {
            InputAction action;
            action.Name = name;
            action.Bindings = std::move(bindings);
            map.AddAction(std::move(action));
        };

        add("MenuUp", { InputBinding::Key(Key::Up), InputBinding::GamepadBtn(GamepadButton::DPadUp) });
        add("MenuDown", { InputBinding::Key(Key::Down), InputBinding::GamepadBtn(GamepadButton::DPadDown) });
        add("MenuLeft", { InputBinding::Key(Key::Left), InputBinding::GamepadBtn(GamepadButton::DPadLeft) });
        add("MenuRight", { InputBinding::Key(Key::Right), InputBinding::GamepadBtn(GamepadButton::DPadRight) });
        add("MenuActivate", { InputBinding::Key(Key::Enter), InputBinding::GamepadBtn(GamepadButton::South) });
        add("MenuCancel", { InputBinding::Key(Key::Escape), InputBinding::GamepadBtn(GamepadButton::East) });
    }
} // namespace OloEngine
