#pragma once

#include "OloEngine/ImGui/Colors.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::UI
{
    //=========================================================================================
    /// RAII Scoped Helpers

    class ScopedStyleColor
    {
      public:
        ScopedStyleColor() = default;

        ScopedStyleColor(ImGuiCol idx, ImVec4 color, bool predicate = true)
            : m_Set(predicate)
        {
            if (predicate)
            {
                ImGui::PushStyleColor(idx, color);
            }
        }

        ScopedStyleColor(ImGuiCol idx, ImU32 color, bool predicate = true)
            : m_Set(predicate)
        {
            if (predicate)
            {
                ImGui::PushStyleColor(idx, color);
            }
        }

        ~ScopedStyleColor()
        {
            if (m_Set)
            {
                ImGui::PopStyleColor();
            }
        }

        ScopedStyleColor(const ScopedStyleColor&) = delete;
        ScopedStyleColor& operator=(const ScopedStyleColor&) = delete;

      private:
        bool m_Set = false;
    };

    class ScopedStyle
    {
      public:
        ScopedStyle(const ScopedStyle&) = delete;
        ScopedStyle& operator=(const ScopedStyle&) = delete;

        template<typename T>
        ScopedStyle(ImGuiStyleVar styleVar, T value)
        {
            ImGui::PushStyleVar(styleVar, value);
        }

        ~ScopedStyle()
        {
            ImGui::PopStyleVar();
        }
    };

    class ScopedFont
    {
      public:
        ScopedFont(const ScopedFont&) = delete;
        ScopedFont& operator=(const ScopedFont&) = delete;

        explicit ScopedFont(ImFont* font)
        {
            ImGui::PushFont(font);
        }

        ~ScopedFont()
        {
            ImGui::PopFont();
        }
    };

    class ScopedID
    {
      public:
        ScopedID(const ScopedID&) = delete;
        ScopedID& operator=(const ScopedID&) = delete;

        template<typename T>
        explicit ScopedID(T id)
        {
            ImGui::PushID(id);
        }

        ~ScopedID()
        {
            ImGui::PopID();
        }
    };

    class ScopedColorStack
    {
      public:
        ScopedColorStack(const ScopedColorStack&) = delete;
        ScopedColorStack& operator=(const ScopedColorStack&) = delete;

        template<typename ColorType, typename... OtherColors>
        ScopedColorStack(ImGuiCol firstColorID, ColorType firstColor, OtherColors&&... otherColorPairs)
            : m_Count((sizeof...(otherColorPairs) / 2) + 1)
        {
            static_assert(
                (sizeof...(otherColorPairs) & 1u) == 0,
                "ScopedColorStack constructor expects a list of pairs of color IDs and colors as its arguments");

            PushColor(firstColorID, firstColor, std::forward<OtherColors>(otherColorPairs)...);
        }

        ~ScopedColorStack()
        {
            ImGui::PopStyleColor(m_Count);
        }

      private:
        int m_Count;

        template<typename ColorType, typename... OtherColors>
        void PushColor(ImGuiCol colorID, ColorType color, OtherColors&&... otherColorPairs)
        {
            ImGui::PushStyleColor(colorID, ImColor(color).Value);
            if constexpr (sizeof...(otherColorPairs) != 0)
            {
                PushColor(std::forward<OtherColors>(otherColorPairs)...);
            }
        }
    };

    class ScopedStyleStack
    {
      public:
        ScopedStyleStack(const ScopedStyleStack&) = delete;
        ScopedStyleStack& operator=(const ScopedStyleStack&) = delete;

        template<typename ValueType, typename... OtherStylePairs>
        ScopedStyleStack(ImGuiStyleVar firstStyleVar, ValueType firstValue, OtherStylePairs&&... otherStylePairs)
            : m_Count((sizeof...(otherStylePairs) / 2) + 1)
        {
            static_assert(
                (sizeof...(otherStylePairs) & 1u) == 0,
                "ScopedStyleStack constructor expects a list of pairs of style vars and values as its arguments");

            PushStyle(firstStyleVar, firstValue, std::forward<OtherStylePairs>(otherStylePairs)...);
        }

        ~ScopedStyleStack()
        {
            ImGui::PopStyleVar(m_Count);
        }

      private:
        int m_Count;

        template<typename ValueType, typename... OtherStylePairs>
        void PushStyle(ImGuiStyleVar styleVar, ValueType value, OtherStylePairs&&... otherStylePairs)
        {
            ImGui::PushStyleVar(styleVar, value);
            if constexpr (sizeof...(otherStylePairs) != 0)
            {
                PushStyle(std::forward<OtherStylePairs>(otherStylePairs)...);
            }
        }
    };

    class ScopedItemFlags
    {
      public:
        ScopedItemFlags(const ScopedItemFlags&) = delete;
        ScopedItemFlags& operator=(const ScopedItemFlags&) = delete;

        explicit ScopedItemFlags(ImGuiItemFlags flags, bool enable = true)
        {
            ImGui::PushItemFlag(flags, enable);
        }

        ~ScopedItemFlags()
        {
            ImGui::PopItemFlag();
        }
    };

    class ScopedDisable
    {
      public:
        ScopedDisable(const ScopedDisable&) = delete;
        ScopedDisable& operator=(const ScopedDisable&) = delete;

        explicit ScopedDisable(bool disabled = true);
        ~ScopedDisable();
    };

    //=========================================================================================
    /// Hover / Tooltip helpers

    inline bool IsItemHovered(float delayInSeconds = 0.1f, ImGuiHoveredFlags flags = 0)
    {
        return ImGui::IsItemHovered(flags) && GImGui->HoveredIdTimer > delayInSeconds;
    }

    inline void SetTooltip(
        std::string_view text,
        float delayInSeconds = 0.1f,
        bool allowWhenDisabled = true,
        ImVec2 padding = ImVec2(5, 5))
    {
        ImGuiHoveredFlags flags = allowWhenDisabled ? ImGuiHoveredFlags_AllowWhenDisabled : 0;
        if (IsItemHovered(delayInSeconds, flags))
        {
            ScopedStyle tooltipPadding(ImGuiStyleVar_WindowPadding, padding);
            ScopedStyleColor textCol(ImGuiCol_Text, Colors::Theme::textBrighter);
            std::string nullTerminated(text);
            ImGui::SetTooltip("%s", nullTerminated.c_str());
        }
    }

    //=========================================================================================
    /// Color manipulation (HSV-based)

    inline ImColor ColorWithValue(const ImColor& color, float value)
    {
        const ImVec4& colRaw = color.Value;
        float hue, sat, val;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, hue, sat, val);
        return ImColor::HSV(hue, sat, std::min(value, 1.0f));
    }

    inline ImColor ColorWithSaturation(const ImColor& color, float saturation)
    {
        const ImVec4& colRaw = color.Value;
        float hue, sat, val;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, hue, sat, val);
        return ImColor::HSV(hue, std::min(saturation, 1.0f), val);
    }

    inline ImColor ColorWithHue(const ImColor& color, float hue)
    {
        const ImVec4& colRaw = color.Value;
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, h, s, v);
        return ImColor::HSV(std::min(hue, 1.0f), s, v);
    }

    inline ImColor ColorWithAlpha(const ImColor& color, float alpha)
    {
        ImVec4 colRaw = color.Value;
        colRaw.w = alpha;
        return colRaw;
    }

    inline ImColor ColorWithMultipliedValue(const ImColor& color, float multiplier)
    {
        const ImVec4& colRaw = color.Value;
        float hue, sat, val;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, hue, sat, val);
        return ImColor::HSV(hue, sat, std::min(val * multiplier, 1.0f));
    }

    inline ImColor ColorWithMultipliedSaturation(const ImColor& color, float multiplier)
    {
        const ImVec4& colRaw = color.Value;
        float hue, sat, val;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, hue, sat, val);
        return ImColor::HSV(hue, std::min(sat * multiplier, 1.0f), val);
    }

    inline ImColor ColorWithMultipliedHue(const ImColor& color, float multiplier)
    {
        const ImVec4& colRaw = color.Value;
        float hue, sat, val;
        ImGui::ColorConvertRGBtoHSV(colRaw.x, colRaw.y, colRaw.z, hue, sat, val);
        return ImColor::HSV(std::min(hue * multiplier, 1.0f), sat, val);
    }

    inline ImColor ColorWithMultipliedAlpha(const ImColor& color, float multiplier)
    {
        ImVec4 colRaw = color.Value;
        colRaw.w *= multiplier;
        return colRaw;
    }

    //=========================================================================================
    /// Rectangle helpers

    inline ImRect GetItemRect()
    {
        return ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    inline ImRect RectExpanded(const ImRect& rect, float x, float y)
    {
        ImRect result = rect;
        result.Min.x -= x;
        result.Min.y -= y;
        result.Max.x += x;
        result.Max.y += y;
        return result;
    }

    inline ImRect RectOffset(const ImRect& rect, float x, float y)
    {
        ImRect result = rect;
        result.Min.x += x;
        result.Min.y += y;
        result.Max.x += x;
        result.Max.y += y;
        return result;
    }

    //=========================================================================================
    /// Drawing helpers

    namespace Draw
    {
        inline void Underline(bool fullWidth = false, float offsetX = 0.0f, float offsetY = -1.0f)
        {
            if (fullWidth)
            {
                if (ImGui::GetCurrentWindow()->DC.CurrentColumns != nullptr)
                {
                    ImGui::PushColumnsBackground();
                }
                else if (ImGui::GetCurrentTable() != nullptr)
                {
                    ImGui::TablePushBackgroundChannel();
                }
            }

            const float width = fullWidth ? ImGui::GetWindowWidth() : ImGui::GetContentRegionAvail().x;
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(cursor.x + offsetX, cursor.y + offsetY),
                ImVec2(cursor.x + width, cursor.y + offsetY),
                Colors::Theme::backgroundDark, 1.0f);

            if (fullWidth)
            {
                if (ImGui::GetCurrentWindow()->DC.CurrentColumns != nullptr)
                {
                    ImGui::PopColumnsBackground();
                }
                else if (ImGui::GetCurrentTable() != nullptr)
                {
                    ImGui::TablePopBackgroundChannel();
                }
            }
        }
    } // namespace Draw

    //=========================================================================================
    /// Popup helpers (gradient background)

    bool BeginPopup(const char* strId, ImGuiWindowFlags flags = 0);
    void EndPopup();

} // namespace OloEngine::UI
