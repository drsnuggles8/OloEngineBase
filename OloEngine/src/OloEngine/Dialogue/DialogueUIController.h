#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <entt.hpp>
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

        // Entity handles for the dialogue UI hierarchy
        entt::entity m_CanvasEntity = entt::null;
        entt::entity m_PanelEntity = entt::null;
        entt::entity m_SpeakerNameEntity = entt::null;
        entt::entity m_DialogueBodyEntity = entt::null;
        entt::entity m_PortraitEntity = entt::null;
        std::vector<entt::entity> m_ChoiceEntities;

        bool m_IsVisible = false;
        entt::entity m_ActiveNpcEntity = entt::null;
    };

} // namespace OloEngine
