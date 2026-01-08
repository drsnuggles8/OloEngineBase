#pragma once

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/TextureCubemap.h"

namespace OloEngine
{
    // Environment settings that can be edited in the panel
    struct EnvironmentSettings
    {
        // Skybox settings
        bool EnableSkybox = true;
        Ref<EnvironmentMap> EnvironmentMapAsset;
        std::string SkyboxPath;
        float SkyboxRotation = 0.0f;
        float SkyboxExposure = 1.0f;

        // Ambient lighting
        bool EnableAmbientLight = true;
        glm::vec3 AmbientColor = glm::vec3(0.1f);
        float AmbientIntensity = 0.3f;

        // IBL (Image-Based Lighting)
        bool EnableIBL = true;
        float IBLIntensity = 1.0f;

        // Fog settings
        bool EnableFog = false;
        glm::vec3 FogColor = glm::vec3(0.5f, 0.6f, 0.7f);
        float FogDensity = 0.01f;
        float FogStart = 10.0f;
        float FogEnd = 100.0f;

        // Tone mapping
        enum class ToneMappingMode : int
        {
            None = 0,
            Reinhard,
            ACES,
            Filmic,
            Uncharted2
        };
        ToneMappingMode ToneMapping = ToneMappingMode::ACES;
        float Gamma = 2.2f;
        float Exposure = 1.0f;
    };

    class EnvironmentSettingsPanel
    {
      public:
        EnvironmentSettingsPanel();
        ~EnvironmentSettingsPanel() = default;

        void OnImGuiRender();

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }

        EnvironmentSettings& GetSettings()
        {
            return m_Settings;
        }
        const EnvironmentSettings& GetSettings() const
        {
            return m_Settings;
        }

      private:
        void DrawSkyboxSection();
        void DrawAmbientSection();
        void DrawIBLSection();
        void DrawFogSection();
        void DrawToneMappingSection();

        void LoadEnvironmentMap(const std::string& filepath);

      private:
        Ref<Scene> m_Context;
        EnvironmentSettings m_Settings;

        // Cached HDR file paths for dropdown
        std::vector<std::string> m_AvailableHDRFiles;
        bool m_NeedsHDRRefresh = true;
    };

} // namespace OloEngine
