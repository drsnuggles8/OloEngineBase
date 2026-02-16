#include "OloEnginePCH.h"
#include "UIInputSystem.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine
{
    static bool PointInRect(const glm::vec2& point, const glm::vec2& rectPos, const glm::vec2& rectSize)
    {
        return point.x >= rectPos.x && point.x <= rectPos.x + rectSize.x &&
               point.y >= rectPos.y && point.y <= rectPos.y + rectSize.y;
    }

    void UIInputSystem::ProcessInput(Scene& scene, const glm::vec2& mousePos, bool mouseDown, bool mousePressed, f32 scrollDelta)
    {
        OLO_PROFILE_FUNCTION();
        // Buttons
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
                if (hovered && mouseDown)
                {
                    button.m_State = UIButtonState::Pressed;
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

        // Sliders
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

                if (mousePressed && hovered)
                {
                    slider.m_IsDragging = true;
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
                    }
                    normalizedValue = glm::clamp(normalizedValue, 0.0f, 1.0f);
                    slider.m_Value = slider.m_MinValue + normalizedValue * (slider.m_MaxValue - slider.m_MinValue);
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

                if (mousePressed && PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    checkbox.m_IsChecked = !checkbox.m_IsChecked;
                }
            }
        }

        // Input fields (focus management)
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
                    inputField.m_IsFocused = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);
                }
            }
        }

        // Scroll views
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

                if (scrollDelta != 0.0f)
                {
                    if (scrollView.m_ScrollDirection == UIScrollDirection::Vertical ||
                        scrollView.m_ScrollDirection == UIScrollDirection::Both)
                    {
                        scrollView.m_ScrollPosition.y -= scrollDelta * scrollView.m_ScrollSpeed;
                        const f32 maxScrollY = glm::max(scrollView.m_ContentSize.y - resolved.m_Size.y, 0.0f);
                        scrollView.m_ScrollPosition.y = glm::clamp(scrollView.m_ScrollPosition.y, 0.0f, maxScrollY);
                    }
                    if (scrollView.m_ScrollDirection == UIScrollDirection::Horizontal ||
                        scrollView.m_ScrollDirection == UIScrollDirection::Both)
                    {
                        scrollView.m_ScrollPosition.x -= scrollDelta * scrollView.m_ScrollSpeed;
                        const f32 maxScrollX = glm::max(scrollView.m_ContentSize.x - resolved.m_Size.x, 0.0f);
                        scrollView.m_ScrollPosition.x = glm::clamp(scrollView.m_ScrollPosition.x, 0.0f, maxScrollX);
                    }
                }
            }
        }

        // Dropdowns
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

                // Hit test main button area
                const bool hoveredMain = PointInRect(mousePos, resolved.m_Position, resolved.m_Size);

                if (dropdown.m_IsOpen && !dropdown.m_Options.empty())
                {
                    // Hit test popup list
                    const f32 itemHeight = glm::max(dropdown.m_ItemHeight, 1.0f);
                    const f32 listHeight = static_cast<f32>(dropdown.m_Options.size()) * itemHeight;
                    const glm::vec2 listPos = { resolved.m_Position.x, resolved.m_Position.y + resolved.m_Size.y };
                    const bool hoveredList = PointInRect(mousePos, listPos, { resolved.m_Size.x, listHeight });

                    // Update hovered index
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
                            dropdown.m_IsOpen = false;
                        }
                        else if (!hoveredMain)
                        {
                            dropdown.m_IsOpen = false;
                        }
                        else
                        {
                            dropdown.m_IsOpen = false; // Toggle close
                        }
                    }
                }
                else
                {
                    dropdown.m_HoveredIndex = -1;
                    if (mousePressed && hoveredMain)
                    {
                        dropdown.m_IsOpen = true;
                    }
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

                if (mousePressed && PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    toggle.m_IsOn = !toggle.m_IsOn;
                }
            }
        }
    }
} // namespace OloEngine
