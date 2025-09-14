#pragma once

#include "OloEngine/Core/Base.h"
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
        
        ~AssetPackBuilderPanel()
        {
            // Request cancellation of any ongoing build
            m_CancelRequested.store(true);
            
            // Wait for the build future to complete if valid
            if (m_BuildFuture.valid())
            {
                // Wait for completion with timeout to avoid indefinite blocking
                using namespace std::chrono_literals;
                while (m_BuildFuture.wait_for(100ms) != std::future_status::ready)
                {
                    // Keep requesting cancellation during wait
                    m_CancelRequested.store(true);
                }
                
                // Get the result to properly clean up the future
                try
                {
                    m_BuildFuture.get();
                }
                catch (...)
                {
                    // Ignore exceptions during destruction
                }
            }
        }

        // Delete copy and move operations due to std::atomic and std::future members
        AssetPackBuilderPanel(const AssetPackBuilderPanel&) = delete;
        AssetPackBuilderPanel& operator=(const AssetPackBuilderPanel&) = delete;
        AssetPackBuilderPanel(AssetPackBuilderPanel&&) = delete;
        AssetPackBuilderPanel& operator=(AssetPackBuilderPanel&&) = delete;

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
        /**
         * @brief Synchronize UI buffer from build settings
         */
        void SyncUIFromSettings();

    private:
        // Build settings
        AssetPackBuilder::BuildSettings m_BuildSettings;
        
        // Progress tracking
        std::atomic<i32> m_BuildProgressPermille{0};  // Progress in permille (0-1000, where 1000 = 100%)
        std::atomic<bool> m_IsBuildInProgress{false};
        std::atomic<bool> m_CancelRequested{false};
        std::future<AssetPackBuilder::BuildResult> m_BuildFuture;
        
        // Results
        mutable std::mutex m_ResultMutex;  // Protects m_LastBuildResult and m_HasBuildResult
        AssetPackBuilder::BuildResult m_LastBuildResult;
        std::atomic<bool> m_HasBuildResult{false};
        
        // UI state
        char m_OutputPathBuffer[512] = "Assets/AssetPack.olopack";
        bool m_ShowAdvancedSettings = false;
    };
}