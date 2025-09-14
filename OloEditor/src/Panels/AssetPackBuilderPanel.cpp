#include "OloEnginePCH.h"
#include "AssetPackBuilderPanel.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"

#include <imgui.h>
#include <future>
#include <thread>
#include <cstring>

namespace OloEngine
{
    void AssetPackBuilderPanel::SyncUIFromSettings()
    {
        // Synchronize output path buffer from settings
        std::string pathStr = m_BuildSettings.OutputPath.string();
        
        // Safely copy to buffer with bounds checking
        sizet copyLength = std::min(pathStr.length(), sizeof(m_OutputPathBuffer) - 1);
        std::memcpy(m_OutputPathBuffer, pathStr.c_str(), copyLength);
        m_OutputPathBuffer[copyLength] = '\0';  // Ensure null termination
    }

    void AssetPackBuilderPanel::OnImGuiRender(bool& isOpen)
    {
        // One-time initialization of UI buffers from settings
        static bool s_UIInitialized = false;
        if (!s_UIInitialized)
        {
            SyncUIFromSettings();
            s_UIInitialized = true;
        }

        if (ImGui::Begin("Asset Pack Builder", &isOpen))
        {
            // Check if build is complete
            if (m_IsBuildInProgress.load() && m_BuildFuture.valid() && 
                m_BuildFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                m_LastBuildResult = m_BuildFuture.get();
                m_HasBuildResult.store(true);
                m_IsBuildInProgress.store(false);
                m_BuildProgressPermille.store(1000);  // 100% in permille
                
                if (m_LastBuildResult.Success)
                {
                    OLO_CORE_INFO("Asset pack build completed successfully: {}", m_LastBuildResult.OutputPath.string());
                }
                else
                {
                    OLO_CORE_ERROR("Asset pack build failed: {}", m_LastBuildResult.ErrorMessage);
                }
            }

            RenderBuildSettings();
            ImGui::Separator();
            
            RenderBuildActions();
            ImGui::Separator();
            
            if (m_IsBuildInProgress.load())
            {
                RenderBuildProgress();
                ImGui::Separator();
            }
            
            if (m_HasBuildResult.load())
            {
                RenderBuildResults();
            }
        }
        ImGui::End();
    }

    void AssetPackBuilderPanel::RenderBuildSettings()
    {
        if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            // Output path
            ImGui::Text("Output Path:");
            ImGui::SameLine();
            if (ImGui::InputText("##OutputPath", m_OutputPathBuffer, sizeof(m_OutputPathBuffer)))
            {
                m_BuildSettings.OutputPath = std::filesystem::path(m_OutputPathBuffer);
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse"))
            {
                // For now, just use default extension
                std::string defaultPath = m_OutputPathBuffer;
                if (defaultPath.empty())
                {
                    defaultPath = "Assets/AssetPack.olopack";
                }
                // Could implement file dialog here in the future
                OLO_CORE_INFO("File dialog not implemented yet, using: {}", defaultPath);
            }

            // Basic settings
            ImGui::Checkbox("Compress Assets", &m_BuildSettings.CompressAssets);
            ImGui::Checkbox("Include Script Module", &m_BuildSettings.IncludeScriptModule);
            ImGui::Checkbox("Validate Assets", &m_BuildSettings.ValidateAssets);

            // Advanced settings toggle
            if (ImGui::Button(m_ShowAdvancedSettings ? "Hide Advanced Settings" : "Show Advanced Settings"))
            {
                m_ShowAdvancedSettings = !m_ShowAdvancedSettings;
            }

            if (m_ShowAdvancedSettings)
            {
                ImGui::Separator();
                ImGui::Text("Advanced Settings");
                ImGui::Indent();

                // Additional settings could go here
                ImGui::Text("Future expansion area for advanced options");

                ImGui::Unindent();
            }

            ImGui::Unindent();
        }
    }

    void AssetPackBuilderPanel::RenderBuildActions()
    {
        if (ImGui::CollapsingHeader("Build Actions", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            // Check if we have an active project
            bool hasActiveProject = Project::GetActive() != nullptr;
            if (!hasActiveProject)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No active project loaded");
                ImGui::Unindent();
                return;
            }

            // Build button
            bool canBuild = !m_IsBuildInProgress.load() && hasActiveProject;
            if (!canBuild)
            {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("Build Asset Pack"))
            {
                StartBuild();
            }

            if (!canBuild)
            {
                ImGui::EndDisabled();
            }

            // Cancel button (only when building)
            if (m_IsBuildInProgress.load())
            {
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CancelBuild();
                }
            }

            // Status information
            ImGui::Spacing();
            ImGui::Text("Project: %s", Project::GetActive() ? Project::GetActive()->GetConfig().Name.c_str() : "None");
            
            std::filesystem::path assetDir = Project::GetActive() ? Project::GetAssetDirectory() : "";
            ImGui::Text("Assets Directory: %s", assetDir.string().c_str());

            ImGui::Unindent();
        }
    }

    void AssetPackBuilderPanel::RenderBuildProgress()
    {
        if (ImGui::CollapsingHeader("Build Progress", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();

            i32 permilleProgress = m_BuildProgressPermille.load();
            f32 progress = static_cast<f32>(permilleProgress) / 10.0f; // Convert permille to percentage
            ImGui::Text("Building asset pack...");
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), nullptr);
            ImGui::Text("Progress: %.1f%%", progress * 100.0f);

            ImGui::Unindent();
        }
    }

    void AssetPackBuilderPanel::RenderBuildResults()
    {
        if (ImGui::CollapsingHeader("Build Results"))
        {
            ImGui::Indent();

            if (m_LastBuildResult.Success)
            {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "✓ Build Successful");
                
                ImGui::Text("Output File: %s", m_LastBuildResult.OutputPath.string().c_str());
                ImGui::Text("Assets Packed: %zu", m_LastBuildResult.AssetCount);
                ImGui::Text("Scenes Packed: %zu", m_LastBuildResult.SceneCount);

                // Show file size if file exists
                std::error_code ec;
                if (std::filesystem::exists(m_LastBuildResult.OutputPath, ec) && !ec)
                {
                    auto fileSize = std::filesystem::file_size(m_LastBuildResult.OutputPath, ec);
                    if (!ec)
                    {
                        float fileSizeMB = static_cast<float>(fileSize) / (1024.0f * 1024.0f);
                        ImGui::Text("File Size: %.2f MB", fileSizeMB);
                    }
                }

                // Note: Explorer functionality not yet implemented in OloEngine
                // TODO: Add platform-specific directory opening functionality
                if (ImGui::Button("Copy Output Path"))
                {
                    ImGui::SetClipboardText(m_LastBuildResult.OutputPath.string().c_str());
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "✗ Build Failed");
                ImGui::TextWrapped("Error: %s", m_LastBuildResult.ErrorMessage.c_str());
            }

            ImGui::Spacing();
            if (ImGui::Button("Clear Results"))
            {
                m_HasBuildResult.store(false);
                m_LastBuildResult = {};
            }

            ImGui::Unindent();
        }
    }

    void AssetPackBuilderPanel::StartBuild()
    {
        if (m_IsBuildInProgress.load())
        {
            OLO_CORE_WARN("Asset pack build already in progress");
            return;
        }

        // Update settings from UI
        m_BuildSettings.OutputPath = std::filesystem::path(m_OutputPathBuffer);

        // Reset progress and results
        m_BuildProgressPermille.store(0);
        m_HasBuildResult.store(false);
        m_LastBuildResult = {};
        m_CancelRequested.store(false);

        // Start async build
        m_IsBuildInProgress.store(true);
        m_BuildFuture = std::async(std::launch::async, [this]() {
            // Create a float progress tracker for the AssetPackBuilder API
            std::atomic<f32> floatProgress{0.0f};
            
            // Launch a progress monitoring thread to convert float to permille
            std::thread progressMonitor([this, &floatProgress]() {
                while (m_IsBuildInProgress.load() && !m_CancelRequested.load()) {
                    f32 progress = floatProgress.load();
                    i32 permille = static_cast<i32>(progress * 1000.0f);
                    m_BuildProgressPermille.store(permille);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            
            auto result = AssetPackBuilder::BuildFromActiveProject(m_BuildSettings, floatProgress, &m_CancelRequested);
            
            // Final progress update
            if (result.Success && !m_CancelRequested.load()) {
                m_BuildProgressPermille.store(1000); // 100%
            }
            
            // Wait for progress monitor to finish
            if (progressMonitor.joinable()) {
                progressMonitor.join();
            }
            
            return result;
        });

        OLO_CORE_INFO("Started asset pack build to: {}", m_BuildSettings.OutputPath.string());
    }

    void AssetPackBuilderPanel::CancelBuild()
    {
        if (!m_IsBuildInProgress.load())
        {
            return;
        }

        // Signal cancellation cooperatively
        m_CancelRequested.store(true, std::memory_order_release);
        
        // Update UI state immediately for responsive feedback
        m_IsBuildInProgress.store(false, std::memory_order_release);
        m_BuildProgressPermille.store(0);
        
        OLO_CORE_INFO("Asset pack build cancellation requested");
        
        // Note: The actual build may continue in the background until completion
        // The destructor will wait() on the future to ensure proper cleanup
    }
}