#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestJournal.h"

namespace OloEngine
{
    bool QuestJournal::AcceptQuest(const std::string& questId, const QuestDefinition& definition)
    {
        if (m_ActiveQuests.contains(questId))
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' is already active", questId);
            return false;
        }

        if (m_CompletedQuestIDs.contains(questId) && !definition.IsRepeatable)
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' already completed and not repeatable", questId);
            return false;
        }

        if (definition.Stages.empty())
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' has no stages", questId);
            return false;
        }

        ActiveQuestState state;
        state.QuestID = questId;
        state.Status = QuestStatus::Active;
        state.CurrentStageIndex = 0;
        state.ElapsedTime = 0.0f;
        state.Definition = definition;

        // Copy objectives from the first stage as runtime state
        state.ObjectiveStates = definition.Stages[0].Objectives;
        for (auto& obj : state.ObjectiveStates)
        {
            obj.CurrentCount = 0;
            obj.IsCompleted = false;
        }

        m_ActiveQuests[questId] = std::move(state);
        OLO_CORE_INFO("[QuestJournal] Accepted quest '{}'", questId);
        return true;
    }

    bool QuestJournal::AbandonQuest(const std::string& questId)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return false;
        }

        std::string id = questId; // Copy before erase (questId may alias the map key)
        m_ActiveQuests.erase(it);
        OLO_CORE_INFO("[QuestJournal] Abandoned quest '{}'", id);
        return true;
    }

    bool QuestJournal::CompleteQuest(const std::string& questId, const std::string& branchChoice)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return false;
        }

        auto& state = it->second;
        state.Status = QuestStatus::Completed;

        // Grant rewards via tags
        for (auto const& tag : state.Definition.CompletionRewards.GrantedTags)
        {
            m_Tags.insert(tag);
        }

        // If there's a branch choice, grant its tags
        if (!branchChoice.empty())
        {
            for (auto const& choice : state.Definition.CompletionChoices)
            {
                if (choice.ChoiceID == branchChoice)
                {
                    for (auto const& tag : choice.GrantedTags)
                    {
                        m_Tags.insert(tag);
                    }
                    break;
                }
            }
        }

        std::string id = questId; // Copy before erase (questId may alias the map key)
        m_CompletedQuestIDs.insert(id);
        m_ActiveQuests.erase(it);
        OLO_CORE_INFO("[QuestJournal] Completed quest '{}'", id);
        return true;
    }

    bool QuestJournal::FailQuest(const std::string& questId)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return false;
        }

        it->second.Status = QuestStatus::Failed;
        std::string id = questId; // Copy before erase (questId may alias the map key)
        m_ActiveQuests.erase(it);
        OLO_CORE_INFO("[QuestJournal] Failed quest '{}'", id);
        return true;
    }

    void QuestJournal::IncrementObjective(const std::string& questId, const std::string& objectiveId, i32 amount)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return;
        }

        for (auto& obj : it->second.ObjectiveStates)
        {
            if (obj.ObjectiveID == objectiveId && !obj.IsCompleted)
            {
                obj.CurrentCount = std::min(obj.CurrentCount + amount, obj.RequiredCount);
                if (obj.CurrentCount >= obj.RequiredCount)
                {
                    obj.IsCompleted = true;
                }
                break;
            }
        }

        TryAdvanceStage(questId);
    }

    void QuestJournal::SetObjectiveComplete(const std::string& questId, const std::string& objectiveId)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return;
        }

        for (auto& obj : it->second.ObjectiveStates)
        {
            if (obj.ObjectiveID == objectiveId)
            {
                obj.CurrentCount = obj.RequiredCount;
                obj.IsCompleted = true;
                break;
            }
        }

        TryAdvanceStage(questId);
    }

    bool QuestJournal::TryAdvanceStage(const std::string& questId)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return false;
        }

        auto& state = it->second;
        auto const& definition = state.Definition;

        if (state.CurrentStageIndex >= static_cast<i32>(definition.Stages.size()))
        {
            return false;
        }

        auto const& currentStage = definition.Stages[static_cast<size_t>(state.CurrentStageIndex)];

        bool stageComplete;
        if (currentStage.RequireAllObjectives)
        {
            stageComplete = true;
            for (auto const& obj : state.ObjectiveStates)
            {
                if (!obj.IsOptional && !obj.IsCompleted)
                {
                    stageComplete = false;
                    break;
                }
            }
        }
        else
        {
            stageComplete = false;
            for (auto const& obj : state.ObjectiveStates)
            {
                if (obj.IsCompleted)
                {
                    stageComplete = true;
                    break;
                }
            }
        }

        if (!stageComplete)
        {
            return false;
        }

        state.CurrentStageIndex++;

        // Check if all stages are done
        if (state.CurrentStageIndex >= static_cast<i32>(definition.Stages.size()))
        {
            // Quest is ready for completion (auto-complete if no choices)
            if (definition.CompletionChoices.empty())
            {
                CompleteQuest(questId);
            }
            return true;
        }

        // Load objectives for next stage
        auto const& nextStage = definition.Stages[static_cast<size_t>(state.CurrentStageIndex)];
        state.ObjectiveStates = nextStage.Objectives;
        for (auto& obj : state.ObjectiveStates)
        {
            obj.CurrentCount = 0;
            obj.IsCompleted = false;
        }

        OLO_CORE_INFO("[QuestJournal] Quest '{}' advanced to stage {}", questId, state.CurrentStageIndex);
        return true;
    }

    QuestStatus QuestJournal::GetQuestStatus(const std::string& questId) const
    {
        auto it = m_ActiveQuests.find(questId);
        if (it != m_ActiveQuests.end())
        {
            return it->second.Status;
        }
        if (m_CompletedQuestIDs.contains(questId))
        {
            return QuestStatus::Completed;
        }
        return QuestStatus::Unavailable;
    }

    const QuestObjective* QuestJournal::GetObjective(const std::string& questId, const std::string& objectiveId) const
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return nullptr;
        }
        for (auto const& obj : it->second.ObjectiveStates)
        {
            if (obj.ObjectiveID == objectiveId)
            {
                return &obj;
            }
        }
        return nullptr;
    }

    i32 QuestJournal::GetCurrentStageIndex(const std::string& questId) const
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return -1;
        }
        return it->second.CurrentStageIndex;
    }

    std::vector<std::string> QuestJournal::GetActiveQuests() const
    {
        std::vector<std::string> result;
        result.reserve(m_ActiveQuests.size());
        for (auto const& [id, state] : m_ActiveQuests)
        {
            if (state.Status == QuestStatus::Active)
            {
                result.push_back(id);
            }
        }
        return result;
    }

    std::vector<std::string> QuestJournal::GetCompletedQuests() const
    {
        return { m_CompletedQuestIDs.begin(), m_CompletedQuestIDs.end() };
    }

    bool QuestJournal::HasCompletedQuest(const std::string& questId) const
    {
        return m_CompletedQuestIDs.contains(questId);
    }

    bool QuestJournal::IsQuestActive(const std::string& questId) const
    {
        auto it = m_ActiveQuests.find(questId);
        return it != m_ActiveQuests.end() && it->second.Status == QuestStatus::Active;
    }

    void QuestJournal::NotifyKill(const std::string& targetTag)
    {
        NotifyObjectiveProgress(QuestObjective::Type::Kill, targetTag, 1);
    }

    void QuestJournal::NotifyCollect(const std::string& itemId, i32 count)
    {
        NotifyObjectiveProgress(QuestObjective::Type::Collect, itemId, count);
    }

    void QuestJournal::NotifyInteract(const std::string& interactableId)
    {
        NotifyObjectiveProgress(QuestObjective::Type::Interact, interactableId, 1);
    }

    void QuestJournal::NotifyReachLocation(const std::string& locationId)
    {
        NotifyObjectiveProgress(QuestObjective::Type::Reach, locationId, 1);
    }

    void QuestJournal::AddTag(const std::string& tag)
    {
        m_Tags.insert(tag);
    }

    bool QuestJournal::HasTag(const std::string& tag) const
    {
        return m_Tags.contains(tag);
    }

    void QuestJournal::UpdateTimers(f32 dt)
    {
        std::vector<std::string> questsToFail;

        for (auto& [id, state] : m_ActiveQuests)
        {
            if (state.Status != QuestStatus::Active)
            {
                continue;
            }

            if (state.Definition.TimeLimit > 0.0f)
            {
                state.ElapsedTime += dt;
                if (state.ElapsedTime >= state.Definition.TimeLimit)
                {
                    questsToFail.push_back(id);
                }
            }

            // Check failure tags
            if (state.Definition.CanFail)
            {
                for (auto const& failTag : state.Definition.FailOnTags)
                {
                    if (m_Tags.contains(failTag))
                    {
                        questsToFail.push_back(id);
                        break;
                    }
                }
            }
        }

        for (auto const& questId : questsToFail)
        {
            FailQuest(questId);
        }
    }

    void QuestJournal::SetActiveQuestState(const std::string& questId, ActiveQuestState state)
    {
        m_ActiveQuests[questId] = std::move(state);
    }

    void QuestJournal::AddCompletedQuestID(const std::string& questId)
    {
        m_CompletedQuestIDs.insert(questId);
    }

    void QuestJournal::NotifyObjectiveProgress(QuestObjective::Type type, const std::string& targetId, i32 amount)
    {
        // Collect quests that need stage advancement; TryAdvanceStage may erase entries
        std::vector<std::string> questsToAdvance;

        for (auto& [questId, state] : m_ActiveQuests)
        {
            if (state.Status != QuestStatus::Active)
            {
                continue;
            }

            bool changed = false;
            for (auto& obj : state.ObjectiveStates)
            {
                if (obj.ObjectiveType == type && obj.TargetID == targetId && !obj.IsCompleted)
                {
                    obj.CurrentCount = std::min(obj.CurrentCount + amount, obj.RequiredCount);
                    if (obj.CurrentCount >= obj.RequiredCount)
                    {
                        obj.IsCompleted = true;
                    }
                    changed = true;
                }
            }

            if (changed)
            {
                questsToAdvance.push_back(questId);
            }
        }

        for (auto const& id : questsToAdvance)
        {
            TryAdvanceStage(id);
        }
    }

} // namespace OloEngine
