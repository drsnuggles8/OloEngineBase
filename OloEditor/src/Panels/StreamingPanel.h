#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    class StreamingPanel
    {
      public:
        StreamingPanel() = default;

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }

        void OnImGuiRender();

      private:
        void DrawSettingsSection();
        void DrawRegionsSection();
        void DrawExportSection();
        void DrawDebugSection();

        void ExportRegion();

        Ref<Scene> m_Context;

        // Export region state
        char m_ExportRegionName[128] = "NewRegion";
        glm::vec3 m_ExportBoundsMin{ -50.0f, -10.0f, -50.0f };
        glm::vec3 m_ExportBoundsMax{ 50.0f, 50.0f, 50.0f };
        bool m_ExportUseSceneBounds = true;
    };
} // namespace OloEngine
