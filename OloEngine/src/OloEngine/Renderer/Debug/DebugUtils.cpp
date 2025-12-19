#include "DebugUtils.h"
#include "OloEngine/Core/Log.h"

#include <sstream>
#include <iomanip>

namespace OloEngine
{
    namespace DebugUtils
    {
        std::string FormatMemorySize(sizet bytes)
        {
            constexpr f64 KB = 1024.0;
            constexpr f64 MB = KB * 1024.0;
            constexpr f64 GB = MB * 1024.0;

            std::stringstream ss;
            ss << std::fixed << std::setprecision(1);

            if (bytes >= GB)
            {
                ss << (bytes / GB) << " GB";
            }
            else if (bytes >= MB)
            {
                ss << (bytes / MB) << " MB";
            }
            else if (bytes >= KB)
            {
                ss << (bytes / KB) << " KB";
            }
            else
            {
                ss << bytes << " B";
            }

            return ss.str();
        }

        std::string FormatDuration(f64 milliseconds)
        {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(2);

            if (milliseconds >= 1000.0)
            {
                ss << (milliseconds / 1000.0) << "s";
            }
            else if (milliseconds >= 1.0)
            {
                ss << milliseconds << "ms";
            }
            else
            {
                ss << (milliseconds * 1000.0) << "μs";
            }

            return ss.str();
        }

        ImVec4 GetPerformanceColor(f32 value, f32 warningThreshold, f32 criticalThreshold)
        {
            if (value >= criticalThreshold)
            {
                return Colors::Critical;
            }
            else if (value >= warningThreshold)
            {
                return Colors::Warning;
            }
            else
            {
                return Colors::Good;
            }
        }

        void RenderTooltip(const char* text)
        {
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        void RenderHelpMarker(const char* helpText)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            RenderTooltip(helpText);
        }

        bool RenderExportButton(const char* label, bool enabled)
        {
            if (!enabled)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            }

            bool clicked = false;
            if (enabled)
            {
                clicked = ImGui::Button(label);
            }
            else
            {
                ImGui::Button(label); // Disabled button
            }

            if (!enabled)
            {
                ImGui::PopStyleVar();
                RenderTooltip("No data available to export");
            }

            return clicked;
        }

        bool RenderResetButton(const char* label, const char* confirmationText)
        {
            static bool s_ShowConfirmation = false;
            static char s_CurrentLabel[256] = "";

            bool resetConfirmed = false;
            if (ImGui::Button(label))
            {
                s_ShowConfirmation = true;
#if defined(_MSC_VER)
                strcpy_s(s_CurrentLabel, sizeof(s_CurrentLabel), label);
#else
                std::strncpy(s_CurrentLabel, label, sizeof(s_CurrentLabel) - 1);
                s_CurrentLabel[sizeof(s_CurrentLabel) - 1] = '\0';
#endif
            }

            if (s_ShowConfirmation)
            {
                ImGui::OpenPopup("Reset Confirmation");
            }

            if (ImGui::BeginPopupModal("Reset Confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("%s", confirmationText);
                ImGui::Separator();

                if (ImGui::Button("Yes", ImVec2(120, 0)))
                {
                    resetConfirmed = true;
                    s_ShowConfirmation = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    s_ShowConfirmation = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            return resetConfirmed;
        }
    } // namespace DebugUtils
} // namespace OloEngine
