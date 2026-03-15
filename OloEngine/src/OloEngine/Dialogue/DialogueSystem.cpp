#include "OloEnginePCH.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Asset/AssetManager.h"

namespace OloEngine
{
    DialogueSystem::DialogueSystem(Scene* scene)
        : m_Scene(scene)
    {
        m_UIController.Initialize(*scene);
    }

    DialogueSystem::~DialogueSystem()
    {
        if (m_Scene)
        {
            m_UIController.Shutdown(*m_Scene);
        }
    }

    void DialogueSystem::Update(Timestep ts)
    {
        OLO_PROFILE_FUNCTION();

        auto view = m_Scene->GetAllEntitiesWith<DialogueStateComponent>();
        for (auto entityHandle : view)
        {
            auto& state = view.get<DialogueStateComponent>(entityHandle);
            if (state.m_State == DialogueState::Displaying)
            {
                // Advance typewriter effect
                if (state.m_TextRevealProgress < 1.0f && !state.m_CurrentText.empty())
                {
                    f32 const totalChars = static_cast<f32>(state.m_CurrentText.size());
                    f32 const increment = (state.m_TextRevealSpeed * ts.GetSeconds()) / totalChars;
                    state.m_TextRevealProgress = std::min(state.m_TextRevealProgress + increment, 1.0f);
                }
            }
        }

        // Update UI controller
        m_UIController.Update(*m_Scene);
    }

    void DialogueSystem::StartDialogue(Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<DialogueComponent>())
        {
            OLO_CORE_WARN("DialogueSystem::StartDialogue - Entity has no DialogueComponent");
            return;
        }

        auto& dialogueComp = entity.GetComponent<DialogueComponent>();

        // Check TriggerOnce
        if (dialogueComp.m_TriggerOnce && dialogueComp.m_HasTriggered)
        {
            return;
        }

        // Load the dialogue tree asset
        auto dialogueTree = AssetManager::GetAsset<DialogueTreeAsset>(dialogueComp.m_DialogueTree);
        if (!dialogueTree)
        {
            OLO_CORE_ERROR("DialogueSystem::StartDialogue - Failed to load DialogueTreeAsset (handle: {})", static_cast<u64>(dialogueComp.m_DialogueTree));
            return;
        }

        if (dialogueTree->GetNodes().empty())
        {
            OLO_CORE_WARN("DialogueSystem::StartDialogue - DialogueTreeAsset has no nodes");
            return;
        }

        // Add or reset DialogueStateComponent
        if (!entity.HasComponent<DialogueStateComponent>())
        {
            entity.AddComponent<DialogueStateComponent>();
        }

        auto& state = entity.GetComponent<DialogueStateComponent>();
        state.m_CurrentNodeID = dialogueTree->GetRootNodeID();
        state.m_State = DialogueState::Processing;
        state.m_CurrentText.clear();
        state.m_CurrentSpeaker.clear();
        state.m_AvailableChoices.clear();
        state.m_SelectedChoiceIndex = -1;
        state.m_HoveredChoiceIndex = -1;
        state.m_TextRevealProgress = 0.0f;

        ProcessNode(entity, state.m_CurrentNodeID, 0);
    }

    void DialogueSystem::AdvanceDialogue(Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<DialogueStateComponent>())
        {
            return;
        }

        auto& state = entity.GetComponent<DialogueStateComponent>();
        if (state.m_State != DialogueState::Displaying)
        {
            return;
        }

        // If typewriter isn't done, snap to full text
        if (state.m_TextRevealProgress < 1.0f)
        {
            state.m_TextRevealProgress = 1.0f;
            return;
        }

        // Follow the default connection from current node
        auto& dialogueComp = entity.GetComponent<DialogueComponent>();
        auto dialogueTree = AssetManager::GetAsset<DialogueTreeAsset>(dialogueComp.m_DialogueTree);
        if (!dialogueTree)
        {
            EndDialogue(entity);
            return;
        }

        auto connections = dialogueTree->GetConnectionsFrom(state.m_CurrentNodeID);
        if (connections.empty())
        {
            // No more nodes — end the dialogue
            EndDialogue(entity);
            return;
        }

        // Take the first connection (default output)
        state.m_State = DialogueState::Processing;
        ProcessNode(entity, connections[0].TargetNodeID, 0);
    }

    void DialogueSystem::SelectChoice(Entity entity, i32 choiceIndex)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<DialogueStateComponent>())
        {
            return;
        }

        auto& state = entity.GetComponent<DialogueStateComponent>();
        if (state.m_State != DialogueState::WaitingForChoice)
        {
            return;
        }

        if (choiceIndex < 0 || choiceIndex >= static_cast<i32>(state.m_AvailableChoices.size()))
        {
            OLO_CORE_WARN("DialogueSystem::SelectChoice - Invalid choice index: {}", choiceIndex);
            return;
        }

        UUID targetNodeID = state.m_AvailableChoices[choiceIndex].TargetNodeID;
        state.m_SelectedChoiceIndex = choiceIndex;
        state.m_State = DialogueState::Processing;
        ProcessNode(entity, targetNodeID, 0);
    }

    void DialogueSystem::EndDialogue(Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        if (entity.HasComponent<DialogueComponent>())
        {
            auto& dialogueComp = entity.GetComponent<DialogueComponent>();
            dialogueComp.m_HasTriggered = true;
        }

        if (entity.HasComponent<DialogueStateComponent>())
        {
            entity.RemoveComponent<DialogueStateComponent>();
        }
    }

    void DialogueSystem::RegisterConditionHandler(const std::string& name, ConditionCallback handler)
    {
        m_ConditionHandlers[name] = std::move(handler);
    }

    void DialogueSystem::RegisterActionHandler(const std::string& name, ActionCallback handler)
    {
        m_ActionHandlers[name] = std::move(handler);
    }

    void DialogueSystem::ProcessNode(Entity entity, UUID nodeID, int hopCount)
    {
        OLO_PROFILE_FUNCTION();

        if (hopCount >= s_MaxHopCount)
        {
            OLO_CORE_ERROR("DialogueSystem::ProcessNode - Exceeded max hop count ({}), possible cycle detected at node {}", s_MaxHopCount, static_cast<u64>(nodeID));
            EndDialogue(entity);
            return;
        }

        auto& dialogueComp = entity.GetComponent<DialogueComponent>();
        auto dialogueTree = AssetManager::GetAsset<DialogueTreeAsset>(dialogueComp.m_DialogueTree);
        if (!dialogueTree)
        {
            EndDialogue(entity);
            return;
        }

        const auto* node = dialogueTree->FindNode(nodeID);
        if (!node)
        {
            OLO_CORE_ERROR("DialogueSystem::ProcessNode - Node not found: {}", static_cast<u64>(nodeID));
            EndDialogue(entity);
            return;
        }

        auto& state = entity.GetComponent<DialogueStateComponent>();
        state.m_CurrentNodeID = nodeID;

        if (node->Type == "dialogue")
        {
            // Extract text and speaker from properties
            state.m_CurrentText.clear();
            state.m_CurrentSpeaker.clear();

            if (auto it = node->Properties.find("text"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    state.m_CurrentText = *str;
            }
            if (auto it = node->Properties.find("speaker"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    state.m_CurrentSpeaker = *str;
            }

            state.m_AvailableChoices.clear();
            state.m_TextRevealProgress = 0.0f;
            state.m_State = DialogueState::Displaying;
        }
        else if (node->Type == "choice")
        {
            // Populate choices from outgoing connections
            auto connections = dialogueTree->GetConnectionsFrom(nodeID);
            state.m_AvailableChoices.clear();

            for (const auto& conn : connections)
            {
                const auto* targetNode = dialogueTree->FindNode(conn.TargetNodeID);
                DialogueChoice choice;
                choice.TargetNodeID = conn.TargetNodeID;

                // Use the source port as the choice label, or get from target node name
                if (!conn.SourcePort.empty() && conn.SourcePort != "output")
                {
                    choice.Text = conn.SourcePort;
                }
                else if (targetNode)
                {
                    choice.Text = targetNode->Name;
                }
                else
                {
                    choice.Text = "...";
                }

                // Check condition if specified
                if (!choice.Condition.empty())
                {
                    if (!EvaluateCondition(choice.Condition, ""))
                        continue; // Skip unavailable choices
                }

                state.m_AvailableChoices.push_back(std::move(choice));
            }

            if (state.m_AvailableChoices.empty())
            {
                // No valid choices available — end the dialogue instead of trapping
                OLO_CORE_WARN("DialogueSystem::ProcessNode - Choice node {} has no available choices", static_cast<u64>(nodeID));
                EndDialogue(entity);
                return;
            }

            state.m_SelectedChoiceIndex = -1;
            state.m_HoveredChoiceIndex = -1;
            state.m_State = DialogueState::WaitingForChoice;
        }
        else if (node->Type == "condition")
        {
            // Evaluate condition and follow true/false branch
            std::string conditionName;
            std::string conditionArgs;

            if (auto it = node->Properties.find("conditionExpression"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    conditionName = *str;
            }
            if (auto it = node->Properties.find("conditionArgs"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    conditionArgs = *str;
            }

            bool result = EvaluateCondition(conditionName, conditionArgs);

            // Find connections by port name ("true" or "false")
            auto connections = dialogueTree->GetConnectionsFrom(nodeID);
            UUID nextNodeID = 0;

            for (const auto& conn : connections)
            {
                if (result && conn.SourcePort == "true")
                {
                    nextNodeID = conn.TargetNodeID;
                    break;
                }
                if (!result && conn.SourcePort == "false")
                {
                    nextNodeID = conn.TargetNodeID;
                    break;
                }
            }

            if (static_cast<u64>(nextNodeID) == 0)
            {
                // No matching branch — try default connection
                if (!connections.empty())
                    nextNodeID = connections[0].TargetNodeID;
                else
                {
                    EndDialogue(entity);
                    return;
                }
            }

            ProcessNode(entity, nextNodeID, hopCount + 1);
        }
        else if (node->Type == "action")
        {
            // Execute action callback
            std::string actionName;
            std::string actionArgs;

            if (auto it = node->Properties.find("actionName"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    actionName = *str;
            }
            if (auto it = node->Properties.find("actionArgs"); it != node->Properties.end())
            {
                if (auto* str = std::get_if<std::string>(&it->second))
                    actionArgs = *str;
            }

            ExecuteAction(actionName, actionArgs);

            // Proceed to next connected node
            auto connections = dialogueTree->GetConnectionsFrom(nodeID);
            if (connections.empty())
            {
                EndDialogue(entity);
                return;
            }
            ProcessNode(entity, connections[0].TargetNodeID, hopCount + 1);
        }
        else
        {
            OLO_CORE_WARN("DialogueSystem::ProcessNode - Unknown node type: {}", node->Type);
            EndDialogue(entity);
        }
    }

    bool DialogueSystem::EvaluateCondition(const std::string& conditionName, const std::string& args)
    {
        OLO_PROFILE_FUNCTION();

        // Check registered handlers first
        if (auto it = m_ConditionHandlers.find(conditionName); it != m_ConditionHandlers.end())
        {
            return it->second(conditionName, args);
        }

        // Check wildcard handler
        if (auto it = m_ConditionHandlers.find("*"); it != m_ConditionHandlers.end())
        {
            return it->second(conditionName, args);
        }

        // Fallback: check DialogueVariables for a simple bool
        return m_Scene->GetDialogueVariables().GetBool(conditionName);
    }

    void DialogueSystem::ExecuteAction(const std::string& actionName, const std::string& args)
    {
        OLO_PROFILE_FUNCTION();

        // Check registered handlers first
        if (auto it = m_ActionHandlers.find(actionName); it != m_ActionHandlers.end())
        {
            it->second(actionName, args);
            return;
        }

        // Check wildcard handler
        if (auto it = m_ActionHandlers.find("*"); it != m_ActionHandlers.end())
        {
            it->second(actionName, args);
            return;
        }

        OLO_CORE_WARN("DialogueSystem::ExecuteAction - No handler registered for action: {}", actionName);
    }

} // namespace OloEngine
