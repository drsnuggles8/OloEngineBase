#include "OloEnginePCH.h"
#include "UIRenderer.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <stack>

namespace OloEngine
{
    static glm::mat4 MakeTransform(const glm::vec2& position, const glm::vec2& size)
    {
        const glm::vec3 center{ position.x + size.x * 0.5f, position.y + size.y * 0.5f, 0.0f };
        return glm::translate(glm::mat4(1.0f), center) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });
    }

    // Clip rect stack for scissor testing
    struct ClipRect
    {
        GLint x, y;
        GLsizei width, height;
    };
    static std::stack<ClipRect> s_ClipStack;
    static f32 s_ViewportHeight = 0.0f;
    static glm::mat4 s_CurrentProjection{ 1.0f };

    void UIRenderer::BeginScene(const glm::mat4& projection)
    {
        s_CurrentProjection = projection;
        // Extract viewport height from the ortho projection for scissor Y-flip
        // ortho(0, w, h, 0, -1, 1) => projection[1][1] = -2/h => h = -2/projection[1][1]
        if (projection[1][1] != 0.0f)
        {
            s_ViewportHeight = glm::abs(-2.0f / projection[1][1]);
        }
        Renderer2D::BeginScene(OloEngine::Camera(projection), glm::mat4(1.0f));
    }

    void UIRenderer::EndScene()
    {
        // Clear clip stack if any remain
        while (!s_ClipStack.empty())
        {
            s_ClipStack.pop();
        }
        Renderer2D::EndScene();
        RenderCommand::DisableScissorTest();
    }

    void UIRenderer::PushClipRect(const glm::vec2& position, const glm::vec2& size)
    {
        // Flush current batch before changing scissor state
        Renderer2D::EndScene();

        // Convert from UI space (Y-down) to OpenGL scissor space (Y-up)
        GLint x = static_cast<GLint>(position.x);
        GLint y = static_cast<GLint>(s_ViewportHeight - position.y - size.y);
        GLsizei w = static_cast<GLsizei>(size.x);
        GLsizei h = static_cast<GLsizei>(size.y);

        // Intersect with parent clip rect if any
        if (!s_ClipStack.empty())
        {
            const auto& parent = s_ClipStack.top();
            GLint x2 = glm::max(x, parent.x);
            GLint y2 = glm::max(y, parent.y);
            GLint right = glm::min(x + static_cast<GLint>(w), parent.x + static_cast<GLint>(parent.width));
            GLint top = glm::min(y + static_cast<GLint>(h), parent.y + static_cast<GLint>(parent.height));
            x = x2;
            y = y2;
            w = static_cast<GLsizei>(glm::max(right - x2, 0));
            h = static_cast<GLsizei>(glm::max(top - y2, 0));
        }

        s_ClipStack.push({ x, y, w, h });
        RenderCommand::EnableScissorTest();
        RenderCommand::SetScissorBox(x, y, w, h);

        // Restart batch with same projection
        Renderer2D::BeginScene(OloEngine::Camera(s_CurrentProjection), glm::mat4(1.0f));
    }

    void UIRenderer::PopClipRect()
    {
        // Flush current batch
        Renderer2D::EndScene();

        if (!s_ClipStack.empty())
        {
            s_ClipStack.pop();
        }

        if (s_ClipStack.empty())
        {
            RenderCommand::DisableScissorTest();
        }
        else
        {
            const auto& parent = s_ClipStack.top();
            RenderCommand::SetScissorBox(parent.x, parent.y, parent.width, parent.height);
        }

        // Restart batch with same projection
        Renderer2D::BeginScene(OloEngine::Camera(s_CurrentProjection), glm::mat4(1.0f));
    }

    void UIRenderer::DrawRect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, int entityID)
    {
        Renderer2D::DrawQuad(MakeTransform(position, size), color, entityID);
    }

    void UIRenderer::DrawRect(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const glm::vec4& tintColor, int entityID)
    {
        Renderer2D::DrawQuad(MakeTransform(position, size), texture, 1.0f, tintColor, entityID);
    }

    void UIRenderer::DrawNineSlice(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const glm::vec4& borderInsets, const glm::vec4& tintColor, int entityID)
    {
        // borderInsets: x=left, y=right, z=top, w=bottom (in pixels)
        const f32 left = borderInsets.x;
        const f32 right = borderInsets.y;
        const f32 top = borderInsets.z;
        const f32 bottom = borderInsets.w;

        const f32 texW = static_cast<f32>(texture->GetWidth());
        const f32 texH = static_cast<f32>(texture->GetHeight());

        // UV boundaries
        const f32 uLeft = left / texW;
        const f32 uRight = 1.0f - (right / texW);
        const f32 vTop = top / texH;
        const f32 vBottom = 1.0f - (bottom / texH);

        // Position boundaries
        const f32 x0 = position.x;
        const f32 x1 = position.x + left;
        const f32 x2 = position.x + size.x - right;
        const f32 y0 = position.y;
        const f32 y1 = position.y + top;
        const f32 y2 = position.y + size.y - bottom;

        // Helper to draw one slice
        auto drawSlice = [&](f32 px, f32 py, f32 pw, f32 ph, const glm::vec2& uvMin, const glm::vec2& uvMax)
        {
            if (pw > 0.0f && ph > 0.0f)
            {
                Renderer2D::DrawQuad(MakeTransform({ px, py }, { pw, ph }), texture, uvMin, uvMax, tintColor, entityID);
            }
        };

        // Top-left, Top-center, Top-right
        drawSlice(x0, y0, left, top, { 0.0f, 0.0f }, { uLeft, vTop });
        drawSlice(x1, y0, x2 - x1, top, { uLeft, 0.0f }, { uRight, vTop });
        drawSlice(x2, y0, right, top, { uRight, 0.0f }, { 1.0f, vTop });

        // Middle-left, Center, Middle-right
        drawSlice(x0, y1, left, y2 - y1, { 0.0f, vTop }, { uLeft, vBottom });
        drawSlice(x1, y1, x2 - x1, y2 - y1, { uLeft, vTop }, { uRight, vBottom });
        drawSlice(x2, y1, right, y2 - y1, { uRight, vTop }, { 1.0f, vBottom });

        // Bottom-left, Bottom-center, Bottom-right
        drawSlice(x0, y2, left, bottom, { 0.0f, vBottom }, { uLeft, 1.0f });
        drawSlice(x1, y2, x2 - x1, bottom, { uLeft, vBottom }, { uRight, 1.0f });
        drawSlice(x2, y2, right, bottom, { uRight, vBottom }, { 1.0f, 1.0f });
    }

    void UIRenderer::DrawImage(const glm::vec2& position, const glm::vec2& size, const UIImageComponent& image, int entityID)
    {
        if (image.m_Texture)
        {
            // Check if 9-slice borders are set
            if (image.m_BorderInsets.x > 0.0f || image.m_BorderInsets.y > 0.0f ||
                image.m_BorderInsets.z > 0.0f || image.m_BorderInsets.w > 0.0f)
            {
                DrawNineSlice(position, size, image.m_Texture, image.m_BorderInsets, image.m_Color, entityID);
            }
            else
            {
                DrawRect(position, size, image.m_Texture, image.m_Color, entityID);
            }
        }
        else
        {
            DrawRect(position, size, image.m_Color, entityID);
        }
    }

    void UIRenderer::DrawPanel(const glm::vec2& position, const glm::vec2& size, const UIPanelComponent& panel, int entityID)
    {
        if (panel.m_BackgroundTexture)
        {
            DrawRect(position, size, panel.m_BackgroundTexture, panel.m_BackgroundColor, entityID);
        }
        else
        {
            DrawRect(position, size, panel.m_BackgroundColor, entityID);
        }
    }

    void UIRenderer::DrawUIText(const glm::vec2& position, const glm::vec2& size, const UITextComponent& text, int entityID)
    {
        if (text.m_Text.empty() || !text.m_FontAsset)
        {
            return;
        }

        // Font size scaling: DrawString uses a 1:1 world-unit = pixel mapping,
        // so we scale the transform by fontSize to get the desired pixel size.
        const f32 scale = text.m_FontSize / 48.0f; // Default font metrics assume ~48 unit em

        glm::vec2 textOrigin = position;

        // Horizontal alignment
        switch (text.m_Alignment)
        {
            case UITextAlignment::TopCenter:
            case UITextAlignment::MiddleCenter:
            case UITextAlignment::BottomCenter:
                textOrigin.x += size.x * 0.5f;
                break;
            case UITextAlignment::TopRight:
            case UITextAlignment::MiddleRight:
            case UITextAlignment::BottomRight:
                textOrigin.x += size.x;
                break;
            default:
                break;
        }

        // Vertical alignment
        switch (text.m_Alignment)
        {
            case UITextAlignment::MiddleLeft:
            case UITextAlignment::MiddleCenter:
            case UITextAlignment::MiddleRight:
                textOrigin.y += size.y * 0.5f;
                break;
            case UITextAlignment::BottomLeft:
            case UITextAlignment::BottomCenter:
            case UITextAlignment::BottomRight:
                textOrigin.y += size.y;
                break;
            default:
                break;
        }

        glm::mat4 transform = glm::translate(glm::mat4(1.0f), { textOrigin.x, textOrigin.y, 0.0f }) * glm::scale(glm::mat4(1.0f), { scale, scale, 1.0f });

        Renderer2D::TextParams params;
        params.Color = text.m_Color;
        params.Kerning = text.m_Kerning;
        params.LineSpacing = text.m_LineSpacing;

        Renderer2D::DrawString(text.m_Text, text.m_FontAsset, transform, params, entityID);
    }

    void UIRenderer::DrawButton(const glm::vec2& position, const glm::vec2& size, const UIButtonComponent& button, int entityID)
    {
        glm::vec4 color;
        switch (button.m_State)
        {
            case UIButtonState::Hovered:
                color = button.m_HoveredColor;
                break;
            case UIButtonState::Pressed:
                color = button.m_PressedColor;
                break;
            case UIButtonState::Disabled:
                color = button.m_DisabledColor;
                break;
            default:
                color = button.m_NormalColor;
                break;
        }
        DrawRect(position, size, color, entityID);
    }

    void UIRenderer::DrawSlider(const glm::vec2& position, const glm::vec2& size, const UISliderComponent& slider, int entityID)
    {
        // Background track
        DrawRect(position, size, slider.m_BackgroundColor, entityID);

        // Calculate normalized value
        const f32 range = slider.m_MaxValue - slider.m_MinValue;
        const f32 normalizedValue = (range > 0.0f) ? (slider.m_Value - slider.m_MinValue) / range : 0.0f;
        const f32 clamped = glm::clamp(normalizedValue, 0.0f, 1.0f);

        // Fill area
        glm::vec2 fillPos = position;
        glm::vec2 fillSize = size;
        switch (slider.m_Direction)
        {
            case UISliderDirection::LeftToRight:
                fillSize.x *= clamped;
                break;
            case UISliderDirection::RightToLeft:
                fillPos.x += size.x * (1.0f - clamped);
                fillSize.x *= clamped;
                break;
            case UISliderDirection::TopToBottom:
                fillSize.y *= clamped;
                break;
            case UISliderDirection::BottomToTop:
                fillPos.y += size.y * (1.0f - clamped);
                fillSize.y *= clamped;
                break;
        }
        if (fillSize.x > 0.0f && fillSize.y > 0.0f)
        {
            DrawRect(fillPos, fillSize, slider.m_FillColor, entityID);
        }

        // Handle (small square at the fill edge)
        const f32 handleSize = glm::min(size.x, size.y) * 0.8f;
        glm::vec2 handlePos;
        switch (slider.m_Direction)
        {
            case UISliderDirection::LeftToRight:
                handlePos = { fillPos.x + fillSize.x - handleSize * 0.5f, position.y + (size.y - handleSize) * 0.5f };
                break;
            case UISliderDirection::RightToLeft:
                handlePos = { fillPos.x - handleSize * 0.5f, position.y + (size.y - handleSize) * 0.5f };
                break;
            case UISliderDirection::TopToBottom:
                handlePos = { position.x + (size.x - handleSize) * 0.5f, fillPos.y + fillSize.y - handleSize * 0.5f };
                break;
            case UISliderDirection::BottomToTop:
                handlePos = { position.x + (size.x - handleSize) * 0.5f, fillPos.y - handleSize * 0.5f };
                break;
        }
        DrawRect(handlePos, { handleSize, handleSize }, slider.m_HandleColor, entityID);
    }

    void UIRenderer::DrawCheckbox(const glm::vec2& position, const glm::vec2& size, const UICheckboxComponent& checkbox, int entityID)
    {
        // Box background
        const glm::vec4& bgColor = checkbox.m_IsChecked ? checkbox.m_CheckedColor : checkbox.m_UncheckedColor;
        DrawRect(position, size, bgColor, entityID);

        // Checkmark (inner rect when checked)
        if (checkbox.m_IsChecked)
        {
            const f32 inset = glm::min(size.x, size.y) * 0.2f;
            DrawRect(
                { position.x + inset, position.y + inset },
                { size.x - 2.0f * inset, size.y - 2.0f * inset },
                checkbox.m_CheckmarkColor, entityID);
        }
    }

    void UIRenderer::DrawProgressBar(const glm::vec2& position, const glm::vec2& size, const UIProgressBarComponent& progressBar, int entityID)
    {
        // Background
        DrawRect(position, size, progressBar.m_BackgroundColor, entityID);

        // Fill
        const f32 range = progressBar.m_MaxValue - progressBar.m_MinValue;
        const f32 normalizedValue = (range > 0.0f) ? (progressBar.m_Value - progressBar.m_MinValue) / range : 0.0f;
        const f32 clamped = glm::clamp(normalizedValue, 0.0f, 1.0f);

        glm::vec2 fillSize = size;
        if (progressBar.m_FillMethod == UIFillMethod::Horizontal)
        {
            fillSize.x *= clamped;
        }
        else
        {
            fillSize.y *= clamped;
        }

        if (fillSize.x > 0.0f && fillSize.y > 0.0f)
        {
            DrawRect(position, fillSize, progressBar.m_FillColor, entityID);
        }
    }

    void UIRenderer::DrawInputField(const glm::vec2& position, const glm::vec2& size, const UIInputFieldComponent& inputField, int entityID)
    {
        // Background
        glm::vec4 bgColor = inputField.m_BackgroundColor;
        if (inputField.m_IsFocused)
        {
            bgColor = bgColor * 1.2f; // Brighten when focused
            bgColor.a = inputField.m_BackgroundColor.a;
        }
        DrawRect(position, size, bgColor, entityID);

        // Text or placeholder
        const f32 scale = inputField.m_FontSize / 48.0f;
        const f32 padding = 4.0f;
        glm::vec2 textPos = { position.x + padding, position.y + size.y * 0.5f };
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), { textPos.x, textPos.y, 0.0f }) * glm::scale(glm::mat4(1.0f), { scale, scale, 1.0f });

        Renderer2D::TextParams params;
        if (inputField.m_Text.empty())
        {
            params.Color = inputField.m_PlaceholderColor;
            if (inputField.m_FontAsset)
            {
                Renderer2D::DrawString(inputField.m_Placeholder, inputField.m_FontAsset, transform, params, entityID);
            }
        }
        else
        {
            params.Color = inputField.m_TextColor;
            if (inputField.m_FontAsset)
            {
                Renderer2D::DrawString(inputField.m_Text, inputField.m_FontAsset, transform, params, entityID);
            }
        }
    }

    void UIRenderer::DrawScrollView(const glm::vec2& position, const glm::vec2& size, const UIScrollViewComponent& scrollView, int entityID)
    {
        // Background
        DrawRect(position, size, scrollView.m_ScrollbarTrackColor, entityID);

        // Vertical scrollbar
        if (scrollView.m_ShowVerticalScrollbar && scrollView.m_ContentSize.y > size.y)
        {
            const f32 scrollbarWidth = 8.0f;
            const f32 viewRatio = size.y / scrollView.m_ContentSize.y;
            const f32 scrollbarHeight = glm::max(size.y * viewRatio, 20.0f);
            const f32 maxScroll = scrollView.m_ContentSize.y - size.y;
            const f32 scrollRatio = (maxScroll > 0.0f) ? glm::clamp(scrollView.m_ScrollPosition.y / maxScroll, 0.0f, 1.0f) : 0.0f;
            const f32 scrollbarY = position.y + scrollRatio * (size.y - scrollbarHeight);

            // Track
            DrawRect({ position.x + size.x - scrollbarWidth, position.y }, { scrollbarWidth, size.y }, scrollView.m_ScrollbarTrackColor, entityID);
            // Thumb
            DrawRect({ position.x + size.x - scrollbarWidth, scrollbarY }, { scrollbarWidth, scrollbarHeight }, scrollView.m_ScrollbarColor, entityID);
        }

        // Horizontal scrollbar
        if (scrollView.m_ShowHorizontalScrollbar && scrollView.m_ContentSize.x > size.x)
        {
            const f32 scrollbarHeight = 8.0f;
            const f32 viewRatio = size.x / scrollView.m_ContentSize.x;
            const f32 scrollbarWidth = glm::max(size.x * viewRatio, 20.0f);
            const f32 maxScroll = scrollView.m_ContentSize.x - size.x;
            const f32 scrollRatio = (maxScroll > 0.0f) ? glm::clamp(scrollView.m_ScrollPosition.x / maxScroll, 0.0f, 1.0f) : 0.0f;
            const f32 scrollbarX = position.x + scrollRatio * (size.x - scrollbarWidth);

            // Track
            DrawRect({ position.x, position.y + size.y - scrollbarHeight }, { size.x, scrollbarHeight }, scrollView.m_ScrollbarTrackColor, entityID);
            // Thumb
            DrawRect({ scrollbarX, position.y + size.y - scrollbarHeight }, { scrollbarWidth, scrollbarHeight }, scrollView.m_ScrollbarColor, entityID);
        }
    }

    void UIRenderer::DrawDropdown(const glm::vec2& position, const glm::vec2& size, const UIDropdownComponent& dropdown, int entityID)
    {
        // Main dropdown background
        DrawRect(position, size, dropdown.m_BackgroundColor, entityID);

        // Selected text
        if (dropdown.m_SelectedIndex >= 0 && dropdown.m_SelectedIndex < static_cast<i32>(dropdown.m_Options.size()))
        {
            const f32 scale = dropdown.m_FontSize / 48.0f;
            const f32 padding = 4.0f;
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), { position.x + padding, position.y + size.y * 0.5f, 0.0f }) * glm::scale(glm::mat4(1.0f), { scale, scale, 1.0f });
            Renderer2D::TextParams params;
            params.Color = dropdown.m_TextColor;
            if (dropdown.m_FontAsset)
            {
                Renderer2D::DrawString(dropdown.m_Options[static_cast<sizet>(dropdown.m_SelectedIndex)].m_Label, dropdown.m_FontAsset, transform, params, entityID);
            }
        }

        // Arrow indicator
        const f32 arrowSize = size.y * 0.4f;
        const glm::vec2 arrowPos = { position.x + size.x - arrowSize - 4.0f, position.y + (size.y - arrowSize) * 0.5f };
        DrawRect(arrowPos, { arrowSize, arrowSize }, dropdown.m_TextColor, entityID);

        // Popup list when open
        if (dropdown.m_IsOpen && !dropdown.m_Options.empty())
        {
            const f32 listHeight = static_cast<f32>(dropdown.m_Options.size()) * dropdown.m_ItemHeight;
            const glm::vec2 listPos = { position.x, position.y + size.y };

            DrawRect(listPos, { size.x, listHeight }, dropdown.m_BackgroundColor, entityID);

            for (sizet i = 0; i < dropdown.m_Options.size(); ++i)
            {
                const f32 itemY = listPos.y + static_cast<f32>(i) * dropdown.m_ItemHeight;

                if (static_cast<i32>(i) == dropdown.m_HoveredIndex)
                {
                    DrawRect({ listPos.x, itemY }, { size.x, dropdown.m_ItemHeight }, dropdown.m_HighlightColor, entityID);
                }

                const f32 scale = dropdown.m_FontSize / 48.0f;
                const f32 padding = 4.0f;
                glm::mat4 transform = glm::translate(glm::mat4(1.0f), { listPos.x + padding, itemY + dropdown.m_ItemHeight * 0.5f, 0.0f }) * glm::scale(glm::mat4(1.0f), { scale, scale, 1.0f });
                Renderer2D::TextParams params;
                params.Color = dropdown.m_TextColor;
                if (dropdown.m_FontAsset)
                {
                    Renderer2D::DrawString(dropdown.m_Options[i].m_Label, dropdown.m_FontAsset, transform, params, entityID);
                }
            }
        }
    }

    void UIRenderer::DrawToggle(const glm::vec2& position, const glm::vec2& size, const UIToggleComponent& toggle, int entityID)
    {
        // Track background (pill shape approximated by a rect)
        const glm::vec4& trackColor = toggle.m_IsOn ? toggle.m_OnColor : toggle.m_OffColor;
        DrawRect(position, size, trackColor, entityID);

        // Knob
        const f32 knobSize = glm::min(size.x * 0.5f, size.y * 0.9f);
        const f32 knobY = position.y + (size.y - knobSize) * 0.5f;
        const f32 knobX = toggle.m_IsOn
                              ? position.x + size.x - knobSize - (size.y - knobSize) * 0.5f
                              : position.x + (size.y - knobSize) * 0.5f;

        DrawRect({ knobX, knobY }, { knobSize, knobSize }, toggle.m_KnobColor, entityID);
    }
} // namespace OloEngine
