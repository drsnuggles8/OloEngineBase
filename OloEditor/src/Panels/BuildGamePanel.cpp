#include "OloEnginePCH.h"
#include "BuildGamePanel.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

namespace OloEngine
{
    static constexpr const char* s_ConfigOptions[] = { "Debug", "Release", "Dist" };

    BuildGamePanel::BuildGamePanel()
    {
        // Initialize buffers with defaults
        const char* defaultName = "MyGame";
        sizet nameLen = std::min(std::strlen(defaultName), m_GameNameBuffer.size() - 1);
        std::memcpy(m_GameNameBuffer.data(), defaultName, nameLen);
        m_GameNameBuffer[nameLen] = '\0';

        // Default output path — GameBuilds folder next to the engine root
        auto defaultOutputPath = (Application::GetStartupWorkingDirectory().parent_path() / "GameBuilds").string();
        sizet pathLen = std::min(defaultOutputPath.size(), m_OutputPathBuffer.size() - 1);
        std::memcpy(m_OutputPathBuffer.data(), defaultOutputPath.c_str(), pathLen);
        m_OutputPathBuffer[pathLen] = '\0';
    }

    BuildGamePanel::~BuildGamePanel()
    {
        if (m_BuildThread.joinable())
        {
            m_CancelRequested.store(true, std::memory_order_release);
            m_BuildThread.request_stop();
        }
    }

    void BuildGamePanel::OnImGuiRender(bool& isOpen)
    {
        if (ImGui::Begin("Build Game", &isOpen))
        {
            // Check if build thread has completed
            if (!m_IsBuildInProgress.load() && m_BuildThread.joinable())
            {
                m_BuildThread.join();
                m_HasBuildResult.store(true);
                m_BuildProgressPermille.store(1000);

                if (m_LastBuildResult.Success)
                {
                    OLO_CORE_INFO("[BuildGame] Build completed: {}", m_LastBuildResult.OutputPath.string());
                }
                else
                {
                    OLO_CORE_ERROR("[BuildGame] Build failed: {}", m_LastBuildResult.ErrorMessage);
                }
            }

            RenderBuildSettings();
            ImGui::Separator();
            RenderBuildActions();

            if (m_IsBuildInProgress.load())
            {
                RenderBuildProgress();
            }

            if (m_HasBuildResult.load())
            {
                ImGui::Separator();
                RenderBuildResults();
            }
        }
        ImGui::End();
    }

    void BuildGamePanel::SetEditorScenePath(const std::filesystem::path& path)
    {
        m_EditorScenePath = path;
    }

    void BuildGamePanel::RenderBuildSettings()
    {
        ImGui::Text("Game Settings");
        ImGui::Spacing();

        ImGui::InputText("Game Name", m_GameNameBuffer.data(), m_GameNameBuffer.size());
        ImGui::InputText("Output Directory", m_OutputPathBuffer.data(), m_OutputPathBuffer.size());

        ImGui::Spacing();
        ImGui::Combo("Build Configuration", &m_ConfigIndex, s_ConfigOptions, IM_ARRAYSIZE(s_ConfigOptions));

        // Start scene selection
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Start Scene");
        ImGui::Spacing();

        // Show the currently selected start scene
        std::string sceneDisplay;
        if (!m_EditorScenePath.empty())
        {
            // Show the scene name relative to the asset directory when possible
            auto project = Project::GetActive();
            if (project)
            {
                std::error_code ec;
                auto relative = std::filesystem::relative(m_EditorScenePath, Project::GetAssetDirectory(), ec);
                sceneDisplay = ec ? m_EditorScenePath.filename().string() : relative.string();
            }
            else
            {
                sceneDisplay = m_EditorScenePath.filename().string();
            }
        }
        else
        {
            sceneDisplay = "(none — will use project default)";
        }

        ImGui::Text("Start Scene: %s", sceneDisplay.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            auto project = Project::GetActive();
            std::string initialDir;
            if (project)
            {
                initialDir = Project::GetAssetDirectory().string();
            }
            std::filesystem::path selected = FileDialogs::OpenFile(
                "OloEditor Scene (*.olo)\0*.olo\0",
                initialDir.empty() ? nullptr : initialDir.c_str());
            if (!selected.empty())
            {
                m_EditorScenePath = selected;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Game icon selection
        ImGui::Text("Game Icon");
        ImGui::Spacing();

        std::string iconDisplay = m_IconPath.empty()
                                      ? "(default OloEngine icon)"
                                      : m_IconPath.filename().string();
        ImGui::Text("Icon: %s", iconDisplay.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse##Icon"))
        {
            std::filesystem::path selected = FileDialogs::OpenFile(
                "Icon Files (*.ico)\0*.ico\0");
            if (!selected.empty())
            {
                m_IconPath = selected;
            }
        }
        if (!m_IconPath.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear##Icon"))
            {
                m_IconPath.clear();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("Compress Assets", &m_Settings.CompressAssets);
        ImGui::Checkbox("Include Script Module", &m_Settings.IncludeScriptModule);
        ImGui::Checkbox("Validate Assets", &m_Settings.ValidateAssets);
    }

    void BuildGamePanel::RenderBuildActions()
    {
        bool building = m_IsBuildInProgress.load();

        if (building)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Build Game", ImVec2(200, 40)))
        {
            std::string validationError;
            if (ValidateSettings(validationError))
            {
                StartBuild();
            }
            else
            {
                OLO_CORE_ERROR("[BuildGame] Validation failed: {}", validationError);
            }
        }

        if (building)
        {
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 40)))
            {
                CancelBuild();
            }
        }
    }

    void BuildGamePanel::RenderBuildProgress()
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Building...");

        i32 permille = m_BuildProgressPermille.load(std::memory_order_relaxed);
        f32 fraction = static_cast<f32>(permille) / 1000.0f;
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), nullptr);
    }

    void BuildGamePanel::RenderBuildResults()
    {
        if (m_LastBuildResult.Success)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::Text("Build Successful!");
            ImGui::PopStyleColor();

            ImGui::Text("Output: %s", m_LastBuildResult.OutputPath.string().c_str());
            ImGui::Text("Assets: %zu, Scenes: %zu", m_LastBuildResult.AssetCount, m_LastBuildResult.SceneCount);
            ImGui::Text("Total Size: %.1f MB",
                        static_cast<f64>(m_LastBuildResult.TotalSizeBytes) / (1024.0 * 1024.0));
            ImGui::Text("Build Time: %.1f seconds", m_LastBuildResult.BuildTimeSeconds);

            if (ImGui::Button("Open Output Folder"))
            {
#ifdef _WIN32
                auto absPath = std::filesystem::absolute(m_LastBuildResult.OutputPath);
                ShellExecuteW(nullptr, L"explore", absPath.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
            ImGui::Text("Build Failed!");
            ImGui::PopStyleColor();

            ImGui::TextWrapped("Error: %s", m_LastBuildResult.ErrorMessage.c_str());
        }
    }

    void BuildGamePanel::StartBuild()
    {
        // Auto-save the current editor scene to ensure the build packages the latest version
        if (m_SaveSceneCallback)
        {
            if (!m_SaveSceneCallback())
            {
                OLO_CORE_WARN("[GameBuild] Scene save failed — building with last-saved version");
            }
        }

        // Sync settings from UI
        m_Settings.GameName = m_GameNameBuffer.data();
        m_Settings.BuildConfiguration = s_ConfigOptions[m_ConfigIndex];

        // Resolve the output directory to an absolute path so the build pipeline
        // and "Open Output Folder" always work regardless of process cwd
        std::filesystem::path rawPath(m_OutputPathBuffer.data());
        if (rawPath.is_relative())
        {
            rawPath = std::filesystem::absolute(rawPath);
        }
        m_Settings.OutputDirectory = rawPath;

        // Set start scene — convert to asset-relative path for portability
        if (!m_EditorScenePath.empty())
        {
            auto project = Project::GetActive();
            if (project)
            {
                std::error_code ec;
                auto relative = std::filesystem::relative(m_EditorScenePath, Project::GetAssetDirectory(), ec);
                m_Settings.StartScene = ec ? m_EditorScenePath : relative;
            }
            else
            {
                m_Settings.StartScene = m_EditorScenePath;
            }
        }

        // Set custom icon path
        m_Settings.IconPath = m_IconPath;

        m_IsBuildInProgress.store(true, std::memory_order_release);
        m_HasBuildResult.store(false, std::memory_order_release);
        m_CancelRequested.store(false, std::memory_order_release);
        m_BuildProgressPermille.store(0, std::memory_order_relaxed);

        // Capture settings by value for the build thread
        GameBuildSettings settings = m_Settings;

        m_BuildThread = std::jthread([this, settings](std::stop_token)
                                     {
            std::atomic<f32> progress = 0.0f;

            // Launch a progress forwarding loop on this thread
            // (GameBuildPipeline::Build does its own work inline)
            auto result = GameBuildPipeline::Build(settings, progress, &m_CancelRequested);

            // Store result
            m_LastBuildResult = result;

            // Final progress update
            m_BuildProgressPermille.store(
                static_cast<i32>(progress.load(std::memory_order_relaxed) * 1000.0f),
                std::memory_order_relaxed);

            m_IsBuildInProgress.store(false, std::memory_order_release); });
    }

    void BuildGamePanel::CancelBuild()
    {
        m_CancelRequested.store(true, std::memory_order_release);
        OLO_CORE_INFO("[BuildGame] Build cancellation requested");
    }

    bool BuildGamePanel::ValidateSettings(std::string& errorMessage) const
    {
        if (std::strlen(m_GameNameBuffer.data()) == 0)
        {
            errorMessage = "Game name cannot be empty";
            return false;
        }

        if (std::strlen(m_OutputPathBuffer.data()) == 0)
        {
            errorMessage = "Output directory cannot be empty";
            return false;
        }

        if (!Project::GetActive())
        {
            errorMessage = "No active project. Open or create a project first.";
            return false;
        }

        return true;
    }

} // namespace OloEngine
