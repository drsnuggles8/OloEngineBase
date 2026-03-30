#pragma once

#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Scene/Scene.h"

#include <filesystem>

namespace OloEngine
{
    class AudioEventsPanel
    {
      public:
        AudioEventsPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

        /// Load the registry from the project's audio events YAML file.
        void LoadRegistry(const std::filesystem::path& filepath);

        /// Save the registry to the project's audio events YAML file.
        void SaveRegistry();

        /// Set the active scene so SaveRegistry can sync the runtime registry.
        void SetActiveScene(const Ref<Scene>& scene)
        {
            m_ActiveScene = scene;
        }

        /// Get the registry (for populating dropdowns in other panels).
        [[nodiscard]] Audio::AudioCommandRegistry& GetRegistry()
        {
            return m_Registry;
        }
        [[nodiscard]] const Audio::AudioCommandRegistry& GetRegistry() const
        {
            return m_Registry;
        }

      private:
        void RenderTriggerList();
        void RenderActionEditor();

        Audio::AudioCommandRegistry m_Registry;
        std::filesystem::path m_RegistryPath;
        Ref<Scene> m_ActiveScene;
        bool m_Dirty = false;
        bool m_ShowReloadConfirm = false;

        // UI state
        Audio::CommandID m_SelectedTrigger;
        char m_NewTriggerName[256] = {};
    };

} // namespace OloEngine
