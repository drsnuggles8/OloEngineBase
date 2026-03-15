#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <vector>

namespace OloEngine
{
    class Scene;
    class Entity;

    class DialogueUIController
    {
      public:
        DialogueUIController() = default;
        ~DialogueUIController() = default;

        void Initialize(Scene& scene);
        void Shutdown(Scene& scene);
        void Update(Scene& scene);

      private:
        void ShowDialogueBox(Scene& scene);
        void HideDialogueBox(Scene& scene);
        void UpdateChoiceEntities(Scene& scene);
        void ClearChoiceEntities(Scene& scene);

        // Entity UUIDs for the dialogue UI hierarchy (resolved via scene each frame)
        UUID m_CanvasEntity = 0;
        UUID m_PanelEntity = 0;
        UUID m_SpeakerNameEntity = 0;
        UUID m_DialogueBodyEntity = 0;
        UUID m_PortraitEntity = 0;
        std::vector<UUID> m_ChoiceEntities;

        bool m_IsVisible = false;
        UUID m_ActiveNpcEntity = 0;

        bool m_AdvanceKeyWasPressed = false;
    };

} // namespace OloEngine
