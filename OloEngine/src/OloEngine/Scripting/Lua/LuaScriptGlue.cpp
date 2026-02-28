#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine
{
    namespace Scripting
    {
        extern sol::state* GetState();
    }

    void LuaScriptGlue::RegisterAllTypes()
    {
        sol::state& lua = *Scripting::GetState();

        // --- GLM vector types (needed before components that use them) ---
        lua.new_usertype<glm::vec2>("vec2",
                                    sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(),
                                    "x", &glm::vec2::x,
                                    "y", &glm::vec2::y);

        lua.new_usertype<glm::vec4>("vec4",
                                    sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
                                    "x", &glm::vec4::x,
                                    "y", &glm::vec4::y,
                                    "z", &glm::vec4::z,
                                    "w", &glm::vec4::w);

        // --- UICanvasComponent ---
        lua.new_usertype<UICanvasComponent>("UICanvasComponent",
                                            "renderMode", &UICanvasComponent::m_RenderMode,
                                            "scaleMode", &UICanvasComponent::m_ScaleMode,
                                            "sortOrder", &UICanvasComponent::m_SortOrder,
                                            "referenceResolution", &UICanvasComponent::m_ReferenceResolution);

        // --- UIRectTransformComponent ---
        lua.new_usertype<UIRectTransformComponent>("UIRectTransformComponent",
                                                   "anchorMin", &UIRectTransformComponent::m_AnchorMin,
                                                   "anchorMax", &UIRectTransformComponent::m_AnchorMax,
                                                   "anchoredPosition", &UIRectTransformComponent::m_AnchoredPosition,
                                                   "sizeDelta", &UIRectTransformComponent::m_SizeDelta,
                                                   "pivot", &UIRectTransformComponent::m_Pivot,
                                                   "rotation", &UIRectTransformComponent::m_Rotation,
                                                   "scale", &UIRectTransformComponent::m_Scale);

        // --- UIImageComponent ---
        lua.new_usertype<UIImageComponent>("UIImageComponent",
                                           "color", &UIImageComponent::m_Color,
                                           "borderInsets", &UIImageComponent::m_BorderInsets);

        // --- UIPanelComponent ---
        lua.new_usertype<UIPanelComponent>("UIPanelComponent",
                                           "backgroundColor", &UIPanelComponent::m_BackgroundColor);

        // --- UITextComponent ---
        lua.new_usertype<UITextComponent>("UITextComponent",
                                          "text", &UITextComponent::m_Text,
                                          "fontSize", &UITextComponent::m_FontSize,
                                          "color", &UITextComponent::m_Color,
                                          "alignment", &UITextComponent::m_Alignment,
                                          "kerning", &UITextComponent::m_Kerning,
                                          "lineSpacing", &UITextComponent::m_LineSpacing);

        // --- UIButtonComponent ---
        lua.new_usertype<UIButtonComponent>("UIButtonComponent",
                                            "normalColor", &UIButtonComponent::m_NormalColor,
                                            "hoveredColor", &UIButtonComponent::m_HoveredColor,
                                            "pressedColor", &UIButtonComponent::m_PressedColor,
                                            "disabledColor", &UIButtonComponent::m_DisabledColor,
                                            "interactable", &UIButtonComponent::m_Interactable,
                                            "state", sol::readonly(&UIButtonComponent::m_State));

        // --- UISliderComponent ---
        lua.new_usertype<UISliderComponent>("UISliderComponent",
                                            "value", &UISliderComponent::m_Value,
                                            "minValue", &UISliderComponent::m_MinValue,
                                            "maxValue", &UISliderComponent::m_MaxValue,
                                            "direction", &UISliderComponent::m_Direction,
                                            "backgroundColor", &UISliderComponent::m_BackgroundColor,
                                            "fillColor", &UISliderComponent::m_FillColor,
                                            "handleColor", &UISliderComponent::m_HandleColor,
                                            "interactable", &UISliderComponent::m_Interactable);

        // --- UICheckboxComponent ---
        lua.new_usertype<UICheckboxComponent>("UICheckboxComponent",
                                              "isChecked", &UICheckboxComponent::m_IsChecked,
                                              "uncheckedColor", &UICheckboxComponent::m_UncheckedColor,
                                              "checkedColor", &UICheckboxComponent::m_CheckedColor,
                                              "checkmarkColor", &UICheckboxComponent::m_CheckmarkColor,
                                              "interactable", &UICheckboxComponent::m_Interactable);

        // --- UIProgressBarComponent ---
        lua.new_usertype<UIProgressBarComponent>("UIProgressBarComponent",
                                                 "value", &UIProgressBarComponent::m_Value,
                                                 "minValue", &UIProgressBarComponent::m_MinValue,
                                                 "maxValue", &UIProgressBarComponent::m_MaxValue,
                                                 "fillMethod", &UIProgressBarComponent::m_FillMethod,
                                                 "backgroundColor", &UIProgressBarComponent::m_BackgroundColor,
                                                 "fillColor", &UIProgressBarComponent::m_FillColor);

        // --- UIInputFieldComponent ---
        lua.new_usertype<UIInputFieldComponent>("UIInputFieldComponent",
                                                "text", &UIInputFieldComponent::m_Text,
                                                "placeholder", &UIInputFieldComponent::m_Placeholder,
                                                "fontSize", &UIInputFieldComponent::m_FontSize,
                                                "textColor", &UIInputFieldComponent::m_TextColor,
                                                "placeholderColor", &UIInputFieldComponent::m_PlaceholderColor,
                                                "backgroundColor", &UIInputFieldComponent::m_BackgroundColor,
                                                "characterLimit", &UIInputFieldComponent::m_CharacterLimit,
                                                "interactable", &UIInputFieldComponent::m_Interactable);

        // --- UIScrollViewComponent ---
        lua.new_usertype<UIScrollViewComponent>("UIScrollViewComponent",
                                                "scrollPosition", &UIScrollViewComponent::m_ScrollPosition,
                                                "contentSize", &UIScrollViewComponent::m_ContentSize,
                                                "scrollDirection", &UIScrollViewComponent::m_ScrollDirection,
                                                "scrollSpeed", &UIScrollViewComponent::m_ScrollSpeed,
                                                "showHorizontalScrollbar", &UIScrollViewComponent::m_ShowHorizontalScrollbar,
                                                "showVerticalScrollbar", &UIScrollViewComponent::m_ShowVerticalScrollbar,
                                                "scrollbarColor", &UIScrollViewComponent::m_ScrollbarColor,
                                                "scrollbarTrackColor", &UIScrollViewComponent::m_ScrollbarTrackColor);

        // --- UIDropdownComponent ---
        lua.new_usertype<UIDropdownComponent>("UIDropdownComponent",
                                              "selectedIndex", &UIDropdownComponent::m_SelectedIndex,
                                              "backgroundColor", &UIDropdownComponent::m_BackgroundColor,
                                              "highlightColor", &UIDropdownComponent::m_HighlightColor,
                                              "textColor", &UIDropdownComponent::m_TextColor,
                                              "fontSize", &UIDropdownComponent::m_FontSize,
                                              "itemHeight", &UIDropdownComponent::m_ItemHeight,
                                              "interactable", &UIDropdownComponent::m_Interactable);

        // --- UIGridLayoutComponent ---
        lua.new_usertype<UIGridLayoutComponent>("UIGridLayoutComponent",
                                                "cellSize", &UIGridLayoutComponent::m_CellSize,
                                                "spacing", &UIGridLayoutComponent::m_Spacing,
                                                "padding", &UIGridLayoutComponent::m_Padding,
                                                "startCorner", &UIGridLayoutComponent::m_StartCorner,
                                                "startAxis", &UIGridLayoutComponent::m_StartAxis,
                                                "constraintCount", &UIGridLayoutComponent::m_ConstraintCount);

        // --- UIToggleComponent ---
        lua.new_usertype<UIToggleComponent>("UIToggleComponent",
                                            "isOn", &UIToggleComponent::m_IsOn,
                                            "offColor", &UIToggleComponent::m_OffColor,
                                            "onColor", &UIToggleComponent::m_OnColor,
                                            "knobColor", &UIToggleComponent::m_KnobColor,
                                            "interactable", &UIToggleComponent::m_Interactable);

        // --- ParticleSystemComponent ---
        lua.new_usertype<ParticleSystem>("ParticleSystem",
                                         "playing", &ParticleSystem::Playing,
                                         "looping", &ParticleSystem::Looping,
                                         "duration", &ParticleSystem::Duration,
                                         "playbackSpeed", &ParticleSystem::PlaybackSpeed,
                                         "windInfluence", &ParticleSystem::WindInfluence,
                                         "getAliveCount", &ParticleSystem::GetAliveCount,
                                         "reset", &ParticleSystem::Reset);

        lua.new_usertype<ParticleEmitter>("ParticleEmitter",
                                          "rateOverTime", &ParticleEmitter::RateOverTime,
                                          "initialSpeed", &ParticleEmitter::InitialSpeed,
                                          "speedVariance", &ParticleEmitter::SpeedVariance,
                                          "lifetimeMin", &ParticleEmitter::LifetimeMin,
                                          "lifetimeMax", &ParticleEmitter::LifetimeMax,
                                          "initialSize", &ParticleEmitter::InitialSize,
                                          "sizeVariance", &ParticleEmitter::SizeVariance,
                                          "initialColor", &ParticleEmitter::InitialColor);

        lua.new_usertype<ParticleSystemComponent>("ParticleSystemComponent",
                                                  "system", &ParticleSystemComponent::System);

        // --- WindSettings (scene-level) ---
        lua.new_usertype<WindSettings>("WindSettings",
                                       "enabled", &WindSettings::Enabled,
                                       "direction", &WindSettings::Direction,
                                       "speed", &WindSettings::Speed,
                                       "gustStrength", &WindSettings::GustStrength,
                                       "gustFrequency", &WindSettings::GustFrequency,
                                       "turbulenceIntensity", &WindSettings::TurbulenceIntensity,
                                       "turbulenceScale", &WindSettings::TurbulenceScale);
    }
} // namespace OloEngine
