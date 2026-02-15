#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

#include <string>

namespace OloEngine
{
    struct UIImageComponent;
    struct UIPanelComponent;
    struct UITextComponent;
    struct UIButtonComponent;
    struct UISliderComponent;
    struct UICheckboxComponent;
    struct UIProgressBarComponent;
    struct UIInputFieldComponent;
    struct UIScrollViewComponent;
    struct UIDropdownComponent;
    struct UIToggleComponent;

    class UIRenderer
    {
      public:
        // Begin a UI render pass with the given orthographic projection
        static void BeginScene(const glm::mat4& projection);
        static void EndScene();

        // Scissor clipping
        static void PushClipRect(const glm::vec2& position, const glm::vec2& size);
        static void PopClipRect();

        // Draw a solid-color rect at pixel coordinates
        static void DrawRect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, int entityID = -1);

        // Draw a textured rect at pixel coordinates
        static void DrawRect(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);

        // 9-slice: draws a texture with fixed-size borders and stretched center
        static void DrawNineSlice(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const glm::vec4& borderInsets, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);

        // Widget draw helpers
        static void DrawImage(const glm::vec2& position, const glm::vec2& size, const UIImageComponent& image, int entityID = -1);
        static void DrawPanel(const glm::vec2& position, const glm::vec2& size, const UIPanelComponent& panel, int entityID = -1);
        static void DrawUIText(const glm::vec2& position, const glm::vec2& size, const UITextComponent& text, int entityID = -1);
        static void DrawButton(const glm::vec2& position, const glm::vec2& size, const UIButtonComponent& button, int entityID = -1);
        static void DrawSlider(const glm::vec2& position, const glm::vec2& size, const UISliderComponent& slider, int entityID = -1);
        static void DrawCheckbox(const glm::vec2& position, const glm::vec2& size, const UICheckboxComponent& checkbox, int entityID = -1);
        static void DrawProgressBar(const glm::vec2& position, const glm::vec2& size, const UIProgressBarComponent& progressBar, int entityID = -1);
        static void DrawInputField(const glm::vec2& position, const glm::vec2& size, const UIInputFieldComponent& inputField, int entityID = -1);
        static void DrawScrollView(const glm::vec2& position, const glm::vec2& size, const UIScrollViewComponent& scrollView, int entityID = -1);
        static void DrawDropdown(const glm::vec2& position, const glm::vec2& size, const UIDropdownComponent& dropdown, int entityID = -1);
        static void DrawToggle(const glm::vec2& position, const glm::vec2& size, const UIToggleComponent& toggle, int entityID = -1);
    };
} // namespace OloEngine
