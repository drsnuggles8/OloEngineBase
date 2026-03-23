#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Build/GameBuildSettings.h"
#include "OloEngine/Build/GameBuildPipeline.h"

#include <array>
#include <atomic>
#include <functional>
#include <thread>

namespace OloEngine
{
    /**
     * @brief Editor panel for building standalone game distributions
     *
     * Provides an ImGui interface for configuring and running the game build
     * pipeline. Users can set the game name, output directory, window settings,
     * and trigger the full build process with progress tracking.
     */
    class BuildGamePanel
    {
      public:
        BuildGamePanel();
        ~BuildGamePanel();

        // Non-copyable, non-movable due to atomics and thread
        BuildGamePanel(const BuildGamePanel&) = delete;
        BuildGamePanel& operator=(const BuildGamePanel&) = delete;
        BuildGamePanel(BuildGamePanel&&) = delete;
        BuildGamePanel& operator=(BuildGamePanel&&) = delete;

        /**
         * @brief Render the ImGui panel
         * @param isOpen Reference to panel open state
         */
        void OnImGuiRender(bool& isOpen);

        /// Set the path of the scene currently open in the editor.
        /// The build panel shows this as the default start scene.
        void SetEditorScenePath(const std::filesystem::path& path);

        /// Set whether the editor is currently in 3D mode.
        void SetIs3DMode(bool is3D)
        {
            m_Settings.Is3DMode = is3D;
        }

        /// Register a callback that saves the current editor scene to disk.
        /// Called automatically before each build to ensure the latest version is packaged.
        void SetSaveSceneCallback(std::function<bool()> callback)
        {
            m_SaveSceneCallback = std::move(callback);
        }

      private:
        void RenderBuildSettings();
        void RenderBuildActions();
        void RenderBuildProgress();
        void RenderBuildResults();

        void StartBuild();
        void CancelBuild();

        bool ValidateSettings(std::string& errorMessage) const;

      private:
        GameBuildSettings m_Settings;

        // UI state
        std::array<char, 256> m_GameNameBuffer{};
        std::array<char, 512> m_OutputPathBuffer{};
        int m_ConfigIndex = 2; // 0=Debug, 1=Release, 2=Dist

        // Build tracking
        std::atomic<i32> m_BuildProgressPermille{ 0 };
        std::atomic<bool> m_IsBuildInProgress{ false };
        std::atomic<bool> m_CancelRequested{ false };
        std::jthread m_BuildThread;

        // Scene selection
        std::filesystem::path m_EditorScenePath; // currently open scene (absolute)
        std::function<bool()> m_SaveSceneCallback;

        // Icon
        std::filesystem::path m_IconPath;

        // Results
        GameBuildResult m_LastBuildResult;
        std::atomic<bool> m_HasBuildResult{ false };
    };

} // namespace OloEngine
