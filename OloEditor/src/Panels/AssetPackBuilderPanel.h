#pragma once

#include "OloEngine/ImGui/ImGuiLayer.h"
#include "OloEngine/Asset/AssetPackBuilder.h"

#include <atomic>
#include <future>
#include <mutex>

namespace OloEngine
{
    /**
     * @brief UI Panel for building asset packs from project assets
     * 
     * Provides a user interface for the AssetPackBuilder functionality,
     * allowing users to configure build settings and monitor progress.
     */
    class AssetPackBuilderPanel
    {
    public:
        AssetPackBuilderPanel() = default;
        ~AssetPackBuilderPanel();

        /**
         * @brief Render the ImGui interface
         * @param isOpen Reference to panel open state
         */
        void OnImGuiRender(bool& isOpen);

    private:
        /**
         * @brief Render build settings section
         */
        void RenderBuildSettings();

        /**
         * @brief Render build actions section
         */
        void RenderBuildActions();

        /**
         * @brief Render build progress section
         */
        void RenderBuildProgress();

        /**
         * @brief Render build results section
         */
        void RenderBuildResults();

        /**
         * @brief Start asset pack build process
         */
        void StartBuild();

        /**
         * @brief Cancel ongoing build process
         */
        void CancelBuild();

    private:
        // Build settings
        AssetPackBuilder::BuildSettings m_BuildSettings;
        
        // Progress tracking
        std::atomic<float> m_BuildProgress{0.0f};
        std::atomic<bool> m_IsBuildInProgress{false};
        std::atomic<bool> m_CancelRequested{false};
        std::future<AssetPackBuilder::BuildResult> m_BuildFuture;
        
        // Results
        AssetPackBuilder::BuildResult m_LastBuildResult;
        std::atomic<bool> m_HasBuildResult{false};
        
        // UI state
        char m_OutputPathBuffer[512] = "Assets/AssetPack.olopack";
        bool m_ShowAdvancedSettings = false;
    };
}