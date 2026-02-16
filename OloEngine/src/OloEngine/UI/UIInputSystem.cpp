#include "OloEnginePCH.h"
#include "UIInputSystem.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine
{
    static bool PointInRect(const glm::vec2& point, const glm::vec2& rectPos, const glm::vec2& rectSize)
    {
        OLO_PROFILE_FUNCTION();
        return point.x >= rectPos.x && point.x <= rectPos.x + rectSize.x &&
               point.y >= rectPos.y && point.y <= rectPos.y + rectSize.y;
    }

    void UIInputSystem::ProcessInput(Scene& scene, const glm::vec2& mousePos, bool mouseDown, bool mousePressed, f32 scrollDeltaX, f32 scrollDeltaY)
    {
        OLO_PROFILE_FUNCTION();

        // Track whether a click has been consumed by a higher-priority widget.
        // Processing order: Dropdowns (popups) > Sliders > Buttons > Checkboxes > Toggles > Input Fields.
        // Scroll views and button hover state are always updated regardless of consumption.
        bool consumed = false;

        // Dropdowns (highest priority — open popups overlay other widgets)
        {
            auto view = scene.GetAllEntitiesWith<UIDropdownComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& dropdown = view.get<UIDropdownComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!dropdown.m_Interactable)
                {
                    dropdown.m_IsOpen = false;
                    continue;
                }

                const bool hoveredMain = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);

                if (dropdown.m_IsOpen && !dropdown.m_Options.empty())
                {
                    const f32 itemHeight = glm::max(dropdown.m_ItemHeight, 1.0f);
                    const f32 listHeight = static_cast<f32>(dropdown.m_Options.size()) * itemHeight;
                    const glm::vec2 listPos = { resolved.m_Position.x, resolved.m_Position.y + resolved.m_Size.y };
                    const bool hoveredList = PointInRect(mousePos, listPos, { resolved.m_Size.x, listHeight });

                    if (hoveredList)
                    {
                        dropdown.m_HoveredIndex = static_cast<i32>((mousePos.y - listPos.y) / itemHeight);
                    }
                    else
                    {
                        dropdown.m_HoveredIndex = -1;
                    }

                    if (mousePressed)
                    {
                        if (hoveredList && dropdown.m_HoveredIndex >= 0 && dropdown.m_HoveredIndex < static_cast<i32>(dropdown.m_Options.size()))
                        {
                            dropdown.m_SelectedIndex = dropdown.m_HoveredIndex;
                        }
                        dropdown.m_IsOpen = false;
                        consumed = true;
                    }

                    // An open popup consumes hover/click even without mousePressed
                    if (hoveredList || hoveredMain)
                    {
                        consumed = true;
                    }
                }
                else
                {
                    dropdown.m_HoveredIndex = -1;
                    if (!consumed && mousePressed && hoveredMain)
                    {
                        dropdown.m_IsOpen = true;
                        consumed = true;
                    }
                }
            }
        }

        // Sliders (drag state must always track mouseDown release, but new drags require unconsumed press)
        {
            auto view = scene.GetAllEntitiesWith<UISliderComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& slider = view.get<UISliderComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!slider.m_Interactable)
                {
                    continue;
                }

                const bool hovered = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);

                if (!consumed && mousePressed && hovered)
                {
                    slider.m_IsDragging = true;
                    consumed = true;
                }

                if (!mouseDown)
                {
                    slider.m_IsDragging = false;
                }

                if (slider.m_IsDragging && resolved.m_Size.x > 0.0f && resolved.m_Size.y > 0.0f)
                {
                    f32 normalizedValue = 0.0f;
                    switch (slider.m_Direction)
                    {
                        case UISliderDirection::LeftToRight:
                            normalizedValue = (mousePos.x - resolved.m_Position.x) / resolved.m_Size.x;
                            break;
                        case UISliderDirection::RightToLeft:
                            normalizedValue = 1.0f - (mousePos.x - resolved.m_Position.x) / resolved.m_Size.x;
                            break;
                        case UISliderDirection::TopToBottom:
                            normalizedValue = (mousePos.y - resolved.m_Position.y) / resolved.m_Size.y;
                            break;
                        case UISliderDirection::BottomToTop:
                            normalizedValue = 1.0f - (mousePos.y - resolved.m_Position.y) / resolved.m_Size.y;
                            break;
                        default:
                            OLO_CORE_ASSERT(false, "Unknown UISliderDirection value");
                            break;
                    }
                    normalizedValue = glm::clamp(normalizedValue, 0.0f, 1.0f);
                    slider.m_Value = slider.m_MinValue + normalizedValue * (slider.m_MaxValue - slider.m_MinValue);
                }
            }
        }

        // Buttons (hover state always updates; press consumption is gated)
        {
            auto view = scene.GetAllEntitiesWith<UIButtonComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& button = view.get<UIButtonComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!button.m_Interactable)
                {
                    button.m_State = UIButtonState::Disabled;
                    continue;
                }

                const bool hovered = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);
                if (consumed)
                {
                    button.m_State = hovered ? UIButtonState::Normal : UIButtonState::Normal;
                    continue;
                }

                if (hovered && mouseDown)
                {
                    button.m_State = UIButtonState::Pressed;
                    if (mousePressed)
                    {
                        consumed = true;
                    }
                }
                else if (hovered)
                {
                    button.m_State = UIButtonState::Hovered;
                }
                else
                {
                    button.m_State = UIButtonState::Normal;
                }
            }
        }

        // Checkboxes
        {
            auto view = scene.GetAllEntitiesWith<UICheckboxComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& checkbox = view.get<UICheckboxComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!checkbox.m_Interactable)
                {
                    continue;
                }

                if (!consumed && mousePressed && PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    checkbox.m_IsChecked = !checkbox.m_IsChecked;
                    consumed = true;
                }
            }
        }

        // Toggles
        {
            auto view = scene.GetAllEntitiesWith<UIToggleComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& toggle = view.get<UIToggleComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!toggle.m_Interactable)
                {
                    continue;
                }

                if (!consumed && mousePressed && PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    toggle.m_IsOn = !toggle.m_IsOn;
                    consumed = true;
                }
            }
        }

        // Input fields (focus management — always updates focus state)
        {
            auto view = scene.GetAllEntitiesWith<UIInputFieldComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& inputField = view.get<UIInputFieldComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!inputField.m_Interactable)
                {
                    inputField.m_IsFocused = false;
                    continue;
                }

                if (mousePressed)
                {
                    if (consumed)
                    {
                        inputField.m_IsFocused = false;
                    }
                    else
                    {
                        inputField.m_IsFocused = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);
                        if (inputField.m_IsFocused)
                        {
                            consumed = true;
                        }
                    }
                }
            }
        }

        // Scroll views (always process — scroll is independent of click consumption)
        {
            auto view = scene.GetAllEntitiesWith<UIScrollViewComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& scrollView = view.get<UIScrollViewComponent>(entity);
                auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    continue;
                }

                if (scrollDeltaY != 0.0f &&
                    (scrollView.m_ScrollDirection == UIScrollDirection::Vertical ||
                     scrollView.m_ScrollDirection == UIScrollDirection::Both))
                {
                    scrollView.m_ScrollPosition.y -= scrollDeltaY * scrollView.m_ScrollSpeed;
                    const f32 maxScrollY = glm::max(scrollView.m_ContentSize.y - resolved.m_Size.y, 0.0f);
                    scrollView.m_ScrollPosition.y = glm::clamp(scrollView.m_ScrollPosition.y, 0.0f, maxScrollY);
                }
                if (scrollDeltaX != 0.0f &&
                    (scrollView.m_ScrollDirection == UIScrollDirection::Horizontal ||
                     scrollView.m_ScrollDirection == UIScrollDirection::Both))
                {
                    scrollView.m_ScrollPosition.x -= scrollDeltaX * scrollView.m_ScrollSpeed;
                    const f32 maxScrollX = glm::max(scrollView.m_ContentSize.x - resolved.m_Size.x, 0.0f);
                    scrollView.m_ScrollPosition.x = glm::clamp(scrollView.m_ScrollPosition.x, 0.0f, maxScrollX);
                }
            }
        }
    }
} // namespace OloEngine
