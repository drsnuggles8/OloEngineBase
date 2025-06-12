#pragma once

#include "OloEngine/Core/Base.h"
#include <imgui.h>
#include <string>

namespace OloEngine
{
    /**
     * @brief Common utilities and helper functions for debugging tools
     * 
     * Provides shared functionality to avoid code duplication across
     * all renderer debugging tools.
     */
    namespace DebugUtils
    {
        /**
         * @brief Format memory size in human-readable format
         * @param bytes Size in bytes
         * @return Formatted string (e.g., "1.5 MB", "512 KB")
         */
        std::string FormatMemorySize(size_t bytes);

        /**
         * @brief Format duration in human-readable format
         * @param milliseconds Duration in milliseconds
         * @return Formatted string (e.g., "15.2ms", "1.5s")
         */
        std::string FormatDuration(f64 milliseconds);

        /**
         * @brief Get color for performance indicator based on value
         * @param value Current value
         * @param warningThreshold Warning threshold
         * @param criticalThreshold Critical threshold
         * @return ImGui color (green/yellow/red)
         */
        ImVec4 GetPerformanceColor(f32 value, f32 warningThreshold, f32 criticalThreshold);

        /**
         * @brief Render a tooltip with information
         * @param text Tooltip text
         */
        void RenderTooltip(const char* text);

        /**
         * @brief Render a help marker with tooltip
         * @param helpText Text to show in tooltip
         */
        void RenderHelpMarker(const char* helpText);

        /**
         * @brief Render an export button with standard styling
         * @param label Button label
         * @param enabled Whether button is enabled
         * @return True if button was clicked
         */
        bool RenderExportButton(const char* label, bool enabled = true);

        /**
         * @brief Render a reset button with confirmation
         * @param label Button label
         * @param confirmationText Confirmation dialog text
         * @return True if reset was confirmed
         */
        bool RenderResetButton(const char* label, const char* confirmationText = "Are you sure you want to reset all data?");

        /**
         * @brief Get standard color for UI elements
         */
        namespace Colors
        {
            constexpr ImVec4 Good = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);       // Green
            constexpr ImVec4 Warning = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);    // Yellow
            constexpr ImVec4 Critical = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);   // Red
            constexpr ImVec4 Info = ImVec4(0.6f, 0.8f, 1.0f, 1.0f);       // Light blue
            constexpr ImVec4 Disabled = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);   // Gray
        }
    }
}
