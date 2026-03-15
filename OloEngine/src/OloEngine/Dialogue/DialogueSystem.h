#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Dialogue/DialogueUIController.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    class Entity;
    class Scene;
    class DialogueTreeAsset;

    class DialogueSystem
    {
      public:
        using ConditionCallback = std::function<bool(const std::string& conditionName, const std::string& args)>;
        using ActionCallback = std::function<void(const std::string& actionName, const std::string& args)>;

        explicit DialogueSystem(Scene* scene);
        ~DialogueSystem();

        void Update(Timestep ts);

        void StartDialogue(Entity entity);
        void AdvanceDialogue(Entity entity);
        void SelectChoice(Entity entity, i32 choiceIndex);
        void EndDialogue(Entity entity);

        void RegisterConditionHandler(const std::string& name, ConditionCallback handler);
        void RegisterActionHandler(const std::string& name, ActionCallback handler);

      private:
        void ProcessNode(Entity entity, UUID nodeID, int hopCount = 0);
        bool EvaluateCondition(const std::string& conditionName, const std::string& args);
        void ExecuteAction(const std::string& actionName, const std::string& args);

        static constexpr int s_MaxHopCount = 256;

        Scene* m_Scene = nullptr;
        DialogueUIController m_UIController;
        std::unordered_map<std::string, ConditionCallback> m_ConditionHandlers;
        std::unordered_map<std::string, ActionCallback> m_ActionHandlers;
    };

} // namespace OloEngine
