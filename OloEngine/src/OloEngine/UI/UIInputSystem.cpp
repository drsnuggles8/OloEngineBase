#include "OloEnginePCH.h"
#include "UIInputSystem.h"

#include "OloEngine/Core/UTF8.h"
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

    // m_CursorPosition is a byte offset into the UTF-8 m_Text (matching the
    // renderer, which iterates m_Text by codepoint). These helpers keep the
    // cursor on codepoint boundaries so edits never split a multi-byte char.

    // Byte index of the codepoint immediately before `pos` (or 0 at the start).
    static i32 PrevCodepointStart(const std::string& text, i32 pos)
    {
        i32 i = pos - 1;
        // Walk back over UTF-8 continuation bytes (10xxxxxx) to the lead byte.
        while (i > 0 && (static_cast<unsigned char>(text[static_cast<sizet>(i)]) & 0xC0u) == 0x80u)
        {
            --i;
        }
        return glm::max(i, 0);
    }

    // Byte index of the codepoint immediately after the one starting at `pos`
    // (or the end of the string).
    static i32 NextCodepointStart(const std::string& text, i32 pos)
    {
        const i32 size = static_cast<i32>(text.size());
        if (pos >= size)
        {
            return size;
        }
        u32 cp = 0;
        sizet adv = 0;
        UTF8::DecodeCodepoint(text, static_cast<sizet>(pos), cp, adv);
        return glm::min(pos + static_cast<i32>(adv), size);
    }

    // Apply this frame's keyboard input to a single focused input field.
    static void ApplyKeyboardToInputField(UIInputFieldComponent& field, const UIKeyboardInput& keyboard)
    {
        // Clamp the cursor in case m_Text changed externally (serialization,
        // scripting) since it was last driven here.
        i32 cursor = glm::clamp(field.m_CursorPosition, 0, static_cast<i32>(field.m_Text.size()));

        // 1. Insert typed characters at the cursor.
        for (const u32 cp : keyboard.m_TypedCharacters)
        {
            // Skip control characters. GLFW routes Enter/Tab/Backspace through
            // key events (not char events), but guard defensively so a stray
            // control codepoint never lands in the text.
            if (cp < 0x20u || cp == 0x7Fu)
            {
                continue;
            }

            // Enforce the character limit, counted in codepoints (not bytes).
            if (field.m_CharacterLimit > 0 &&
                static_cast<i32>(UTF8::CountCodepoints(field.m_Text)) >= field.m_CharacterLimit)
            {
                break;
            }

            std::string encoded;
            UTF8::EncodeCodepoint(cp, encoded);
            field.m_Text.insert(static_cast<sizet>(cursor), encoded);
            cursor += static_cast<i32>(encoded.size());
        }

        // 2. Backspace — remove the codepoint before the cursor.
        if (keyboard.m_Backspace && cursor > 0)
        {
            const i32 prev = PrevCodepointStart(field.m_Text, cursor);
            field.m_Text.erase(static_cast<sizet>(prev), static_cast<sizet>(cursor - prev));
            cursor = prev;
        }

        // 3. Delete — remove the codepoint at the cursor.
        if (keyboard.m_Delete && cursor < static_cast<i32>(field.m_Text.size()))
        {
            const i32 next = NextCodepointStart(field.m_Text, cursor);
            field.m_Text.erase(static_cast<sizet>(cursor), static_cast<sizet>(next - cursor));
        }

        // 4. Cursor movement (codepoint-wise).
        if (keyboard.m_CursorLeft)
        {
            cursor = PrevCodepointStart(field.m_Text, cursor);
        }
        if (keyboard.m_CursorRight)
        {
            cursor = NextCodepointStart(field.m_Text, cursor);
        }
        if (keyboard.m_Home)
        {
            cursor = 0;
        }
        if (keyboard.m_End)
        {
            cursor = static_cast<i32>(field.m_Text.size());
        }

        field.m_CursorPosition = glm::clamp(cursor, 0, static_cast<i32>(field.m_Text.size()));
    }

    void UIInputSystem::ProcessInput(Scene& scene, const glm::vec2& mousePos, bool mouseDown, bool mousePressed, f32 scrollDeltaX, f32 scrollDeltaY, const UIKeyboardInput& keyboard)
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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!slider.m_Interactable)
                {
                    continue;
                }

                if (const bool hovered = PointInRect(mousePos, resolved.m_Position, resolved.m_Size); !consumed && mousePressed && hovered)
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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

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
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

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

                // Keyboard text editing applies only to the focused field. Focus
                // is resolved above, so a field focused by this frame's click can
                // also receive this frame's typed characters.
                if (inputField.m_IsFocused)
                {
                    ApplyKeyboardToInputField(inputField, keyboard);
                }
            }
        }

        // Scroll views (always process — scroll is independent of click consumption)
        {
            auto view = scene.GetAllEntitiesWith<UIScrollViewComponent, UIResolvedRectComponent>();
            for (const auto entity : view)
            {
                auto& scrollView = view.get<UIScrollViewComponent>(entity);
                const auto& resolved = view.get<UIResolvedRectComponent>(entity);

                if (!PointInRect(mousePos, resolved.m_Position, resolved.m_Size))
                {
                    continue;
                }

                constexpr f32 scrollEpsilon = 1e-6f;
                if (std::abs(scrollDeltaY) > scrollEpsilon &&
                    (scrollView.m_ScrollDirection == UIScrollDirection::Vertical ||
                     scrollView.m_ScrollDirection == UIScrollDirection::Both))
                {
                    scrollView.m_ScrollPosition.y -= scrollDeltaY * scrollView.m_ScrollSpeed;
                    const f32 maxScrollY = glm::max(scrollView.m_ContentSize.y - resolved.m_Size.y, 0.0f);
                    scrollView.m_ScrollPosition.y = glm::clamp(scrollView.m_ScrollPosition.y, 0.0f, maxScrollY);
                }
                if (std::abs(scrollDeltaX) > scrollEpsilon &&
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
