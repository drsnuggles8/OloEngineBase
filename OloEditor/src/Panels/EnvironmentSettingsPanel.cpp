#include "OloEnginePCH.h"
#include "EnvironmentSettingsPanel.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Project/Project.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <filesystem>

namespace OloEngine
{
    EnvironmentSettingsPanel::EnvironmentSettingsPanel()
    {
        // Initialize with default settings
        m_Settings = EnvironmentSettings{};
    }

    void EnvironmentSettingsPanel::OnImGuiRender()
    {
        ImGui::Begin("Environment Settings");

        if (!m_Context)
        {
            ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.5f, 1.0f), "No scene context set");
            ImGui::End();
            return;
        }

        // Refresh available HDR files
        if (m_NeedsHDRRefresh)
        {
            m_AvailableHDRFiles.clear();
            auto assetDir = Project::GetAssetDirectory();
            if (std::filesystem::exists(assetDir))
            {
                for (auto& entry : std::filesystem::recursive_directory_iterator(assetDir))
                {
                    if (entry.is_regular_file())
                    {
                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".hdr" || ext == ".exr")
                        {
                            m_AvailableHDRFiles.push_back(entry.path().string());
                        }
                    }
                }
            }
            m_NeedsHDRRefresh = false;
        }

        DrawSkyboxSection();
        ImGui::Separator();
        DrawAmbientSection();
        ImGui::Separator();
        DrawIBLSection();
        ImGui::Separator();
        DrawFogSection();
        ImGui::Separator();
        DrawToneMappingSection();

        ImGui::End();
    }

    void EnvironmentSettingsPanel::DrawSkyboxSection()
    {
        if (ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();
            
            ImGui::Checkbox("Enable Skybox", &m_Settings.EnableSkybox);
            
            if (m_Settings.EnableSkybox)
            {
                // Current skybox display
                if (!m_Settings.SkyboxPath.empty())
                {
                    // Show just filename
                    auto lastSlash = m_Settings.SkyboxPath.find_last_of("/\\");
                    std::string filename = (lastSlash != std::string::npos) 
                        ? m_Settings.SkyboxPath.substr(lastSlash + 1) 
                        : m_Settings.SkyboxPath;
                    ImGui::Text("Current: %s", filename.c_str());
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No skybox loaded");
                }

                // HDR file dropdown
                if (!m_AvailableHDRFiles.empty())
                {
                    static int selectedHDR = -1;
                    if (ImGui::BeginCombo("Available HDR Files", 
                        selectedHDR >= 0 ? m_AvailableHDRFiles[selectedHDR].c_str() : "Select..."))
                    {
                        for (int i = 0; i < static_cast<int>(m_AvailableHDRFiles.size()); i++)
                        {
                            auto lastSlash = m_AvailableHDRFiles[i].find_last_of("/\\");
                            std::string filename = (lastSlash != std::string::npos) 
                                ? m_AvailableHDRFiles[i].substr(lastSlash + 1) 
                                : m_AvailableHDRFiles[i];
                                
                            if (ImGui::Selectable(filename.c_str(), selectedHDR == i))
                            {
                                selectedHDR = i;
                                LoadEnvironmentMap(m_AvailableHDRFiles[i]);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                
                if (ImGui::Button("Refresh HDR List"))
                {
                    m_NeedsHDRRefresh = true;
                }
                
                ImGui::SameLine();
                
                if (ImGui::Button("Browse..."))
                {
                    std::string filepath = FileDialogs::OpenFile(
                        "HDR Images (*.hdr;*.exr)\0*.hdr;*.exr\0"
                        "All Files (*.*)\0*.*\0");
                    if (!filepath.empty())
                    {
                        LoadEnvironmentMap(filepath);
                    }
                }
                
                ImGui::SliderFloat("Rotation##Skybox", &m_Settings.SkyboxRotation, 0.0f, 360.0f, "%.1f deg");
                ImGui::SliderFloat("Exposure##Skybox", &m_Settings.SkyboxExposure, 0.1f, 10.0f, "%.2f");
                
                if (m_Settings.EnvironmentMapAsset && ImGui::Button("Clear Skybox"))
                {
                    m_Settings.EnvironmentMapAsset = nullptr;
                    m_Settings.SkyboxPath.clear();
                }
            }
            
            ImGui::Unindent();
        }
    }

    void EnvironmentSettingsPanel::DrawAmbientSection()
    {
        if (ImGui::CollapsingHeader("Ambient Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();
            
            ImGui::Checkbox("Enable Ambient", &m_Settings.EnableAmbientLight);
            
            if (m_Settings.EnableAmbientLight)
            {
                ImGui::ColorEdit3("Ambient Color", glm::value_ptr(m_Settings.AmbientColor));
                ImGui::SliderFloat("Intensity##Ambient", &m_Settings.AmbientIntensity, 0.0f, 2.0f, "%.3f");
            }
            
            ImGui::Unindent();
        }
    }

    void EnvironmentSettingsPanel::DrawIBLSection()
    {
        if (ImGui::CollapsingHeader("Image-Based Lighting"))
        {
            ImGui::Indent();
            
            ImGui::Checkbox("Enable IBL", &m_Settings.EnableIBL);
            
            if (m_Settings.EnableIBL)
            {
                ImGui::SliderFloat("IBL Intensity", &m_Settings.IBLIntensity, 0.0f, 5.0f, "%.2f");
                
                if (m_Settings.EnvironmentMapAsset)
                {
                    bool hasIBL = m_Settings.EnvironmentMapAsset->HasIBL();
                    ImGui::TextColored(
                        hasIBL ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.5f, 0.2f, 1.0f),
                        hasIBL ? "IBL textures available" : "IBL not generated");
                    
                    if (!hasIBL && ImGui::Button("Generate IBL"))
                    {
                        IBLConfiguration config;
                        config.Quality = IBLQuality::Medium;
                        m_Settings.EnvironmentMapAsset->RegenerateIBL(config);
                    }
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load an environment map first");
                }
            }
            
            ImGui::Unindent();
        }
    }

    void EnvironmentSettingsPanel::DrawFogSection()
    {
        if (ImGui::CollapsingHeader("Fog"))
        {
            ImGui::Indent();
            
            ImGui::Checkbox("Enable Fog", &m_Settings.EnableFog);
            
            if (m_Settings.EnableFog)
            {
                ImGui::ColorEdit3("Fog Color", glm::value_ptr(m_Settings.FogColor));
                ImGui::SliderFloat("Density", &m_Settings.FogDensity, 0.0f, 0.1f, "%.4f");
                ImGui::DragFloat("Start Distance", &m_Settings.FogStart, 1.0f, 0.0f, 1000.0f);
                ImGui::DragFloat("End Distance", &m_Settings.FogEnd, 1.0f, 0.0f, 2000.0f);
                
                // Ensure end > start
                if (m_Settings.FogEnd < m_Settings.FogStart)
                    m_Settings.FogEnd = m_Settings.FogStart + 1.0f;
            }
            
            ImGui::Unindent();
        }
    }

    void EnvironmentSettingsPanel::DrawToneMappingSection()
    {
        if (ImGui::CollapsingHeader("Tone Mapping & Post-Processing"))
        {
            ImGui::Indent();
            
            const char* toneMappingModes[] = { "None", "Reinhard", "ACES", "Filmic", "Uncharted2" };
            int currentMode = static_cast<int>(m_Settings.ToneMapping);
            if (ImGui::Combo("Tone Mapping", &currentMode, toneMappingModes, IM_ARRAYSIZE(toneMappingModes)))
            {
                m_Settings.ToneMapping = static_cast<EnvironmentSettings::ToneMappingMode>(currentMode);
            }
            
            ImGui::SliderFloat("Exposure", &m_Settings.Exposure, 0.1f, 10.0f, "%.2f");
            ImGui::SliderFloat("Gamma", &m_Settings.Gamma, 1.0f, 3.0f, "%.2f");
            
            if (ImGui::Button("Reset to Defaults"))
            {
                m_Settings.ToneMapping = EnvironmentSettings::ToneMappingMode::ACES;
                m_Settings.Exposure = 1.0f;
                m_Settings.Gamma = 2.2f;
            }
            
            ImGui::Unindent();
        }
    }

    void EnvironmentSettingsPanel::LoadEnvironmentMap(const std::string& filepath)
    {
        EnvironmentMapSpecification spec;
        spec.FilePath = filepath;
        spec.Resolution = 512;
        spec.GenerateIBL = true;
        spec.GenerateMipmaps = true;
        
        try
        {
            m_Settings.EnvironmentMapAsset = EnvironmentMap::CreateFromEquirectangular(filepath);
            m_Settings.SkyboxPath = filepath;
            OLO_CORE_INFO("Loaded environment map: {}", filepath);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to load environment map: {}", e.what());
        }
    }

} // namespace OloEngine
