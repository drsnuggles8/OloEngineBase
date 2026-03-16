#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    class CommandHistory;

    class StreamingPanel
    {
      public:
        StreamingPanel() = default;

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }

        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

        void OnImGuiRender();

      private:
        void DrawSettingsSection();
        void DrawRegionsSection();
        void DrawExportSection();
        void DrawDebugSection();

        void ExportRegion();

        Ref<Scene> m_Context;
        CommandHistory* m_CommandHistory = nullptr;

        // Settings undo tracking
        bool m_IsEditingSettings = false;
        StreamingSettings m_SettingsSnapshot{};

        // Export region state
        char m_ExportRegionName[128] = "NewRegion";
        glm::vec3 m_ExportBoundsMin{ -50.0f, -10.0f, -50.0f };
        glm::vec3 m_ExportBoundsMax{ 50.0f, 50.0f, 50.0f };
        bool m_ExportUseSceneBounds = true;
    };
} // namespace OloEngine
