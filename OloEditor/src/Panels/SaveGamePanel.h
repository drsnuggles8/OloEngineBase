#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Scene/Scene.h"

#include <string>
#include <vector>

namespace OloEngine
{
    class SaveGamePanel
    {
      public:
        SaveGamePanel() = default;

        void SetContext(const Ref<Scene>& scene, const Ref<Framebuffer>& framebuffer)
        {
            m_Scene = scene;
            m_Framebuffer = framebuffer;
        }

        void OnImGuiRender();

        // Trigger save/load from outside (e.g., keyboard shortcuts)
        void TriggerQuickSave();
        void TriggerQuickLoad();

      private:
        void DrawSaveSection();
        void DrawLoadSection();
        void DrawSettingsSection();
        void RefreshSaveList();
        void FormatTimestamp(i64 timestamp, char* buffer, sizet bufferSize) const;

        Ref<Scene> m_Scene;
        Ref<Framebuffer> m_Framebuffer;

        // Save list cache
        std::vector<SaveFileInfo> m_SaveFiles;
        bool m_NeedsRefresh = true;

        // New save state
        char m_NewSaveName[128] = "MySave";
        bool m_IncludeThumbnail = true;

        // Auto-save settings
        f32 m_AutoSaveInterval = 0.0f;

        // Status message
        std::string m_StatusMessage;
        f32 m_StatusTimer = 0.0f;
    };

} // namespace OloEngine
