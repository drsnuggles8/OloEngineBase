#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/ImGui/ImGuiLayer.h"
#include "OloEngine/Asset/AssetPackBuilder.h"

#include <array>
#include <atomic>
#include <thread>
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
        AssetPackBuilderPanel();

        ~AssetPackBuilderPanel();

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

        /**
         * @brief Validate output path for asset pack
         * @param path Path to validate
         * @param errorMessage Output parameter for error message if validation fails
         * @return True if path is valid, false otherwise
         */
        bool ValidateOutputPath(const std::string& path, std::string& errorMessage) const;

      private:
        // Build settings
        AssetPackBuilder::BuildSettings m_BuildSettings;

        // Progress tracking
        std::atomic<i32> m_BuildProgressPermille{ 0 }; // Progress in permille (0-1000, where 1000 = 100%)
        std::atomic<bool> m_IsBuildInProgress{ false };
        std::jthread m_BuildThread;

        // Results
        mutable std::mutex m_ResultMutex; // Protects m_LastBuildResult and m_HasBuildResult
        AssetPackBuilder::BuildResult m_LastBuildResult;
        std::atomic<bool> m_HasBuildResult{ false };

        // UI state
        std::array<char, 512> m_OutputPathBuffer{};
        bool m_ShowAdvancedSettings = false;
        std::string m_OutputPathError{}; // Error message for invalid output path
    };
} // namespace OloEngine
