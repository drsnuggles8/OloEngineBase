#include "OloEnginePCH.h"
#include "OloEngine/UI/UI.h"

namespace OloEngine::UI
{
    ScopedDisable::ScopedDisable(bool disabled)
    {
        ImGui::BeginDisabled(disabled);
    }

    ScopedDisable::~ScopedDisable()
    {
        ImGui::EndDisabled();
    }

    bool BeginPopup(const char* strId, ImGuiWindowFlags flags)
    {
        if (!ImGui::BeginPopup(strId, flags))
        {
            return false;
        }

        // Fill background with a nice gradient
        const float padding = ImGui::GetStyle().WindowBorderSize;
        const ImRect windowRect = RectExpanded(ImGui::GetCurrentWindow()->Rect(), -padding, -padding);
        ImGui::PushClipRect(windowRect.Min, windowRect.Max, false);

        const ImColor col1 = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        const ImColor col2 = ColorWithMultipliedValue(col1, 0.8f);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(windowRect.Min, windowRect.Max, col1, col1, col2, col2);
        ImGui::GetWindowDrawList()->AddRect(windowRect.Min, windowRect.Max, ColorWithMultipliedValue(col1, 1.1f));
        ImGui::PopClipRect();

        // Popped in EndPopup()
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 80));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));

        return true;
    }

    void EndPopup()
    {
        ImGui::PopStyleVar();   // WindowPadding
        ImGui::PopStyleColor(); // HeaderHovered
        ImGui::EndPopup();
    }

} // namespace OloEngine::UI
