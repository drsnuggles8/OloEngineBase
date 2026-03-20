#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestJournal.h"

namespace OloEngine
{
    bool QuestJournal::AcceptQuest(const std::string& questId, const QuestDefinition& definition)
    {
        if (questId != definition.QuestID)
        {
            OLO_CORE_WARN("[QuestJournal] Quest ID mismatch: key='{}' vs definition='{}'", questId, definition.QuestID);
            return false;
        }

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

        // Check cooldown for repeatable quests
        if (auto cd = m_QuestCooldowns.find(questId); cd != m_QuestCooldowns.end() && cd->second > 0.0f)
        {
            OLO_CORE_WARN("[QuestJournal] Repeatable quest '{}' is on cooldown ({:.1f}s remaining)", questId, cd->second);
            return false;
        }

        if (definition.Stages.empty())
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' has no stages", questId);
            return false;
        }

        // Evaluate all requirements
        if (!CheckRequirements(definition.Requirements))
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' has unmet requirements", questId);
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

    std::optional<QuestRewards> QuestJournal::CompleteQuest(const std::string& questId, const std::string& branchChoice)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return std::nullopt;
        }

        auto& state = it->second;

        // Validate that all stages are done
        if (state.CurrentStageIndex < static_cast<i32>(state.Definition.Stages.size()))
        {
            OLO_CORE_WARN("[QuestJournal] Quest '{}' cannot complete: not all stages finished (stage {}/{})", questId, state.CurrentStageIndex, state.Definition.Stages.size());
            return std::nullopt;
        }

        // Validate branch choice when completion choices exist
        if (!state.Definition.CompletionChoices.empty())
        {
            if (branchChoice.empty())
            {
                OLO_CORE_WARN("[QuestJournal] Quest '{}' requires a branch choice but none provided", questId);
                return std::nullopt;
            }

            bool validChoice = false;
            for (auto const& choice : state.Definition.CompletionChoices)
            {
                if (choice.ChoiceID == branchChoice)
                {
                    validChoice = true;
                    break;
                }
            }
            if (!validChoice)
            {
                OLO_CORE_WARN("[QuestJournal] Quest '{}' has no branch choice '{}'", questId, branchChoice);
                return std::nullopt;
            }
        }

        state.Status = QuestStatus::Completed;

        // Collect rewards
        QuestRewards rewards = state.Definition.CompletionRewards;

        // Grant reward tags
        for (auto const& tag : rewards.GrantedTags)
        {
            m_Tags.insert(tag);
        }

        // Grant branch choice tags
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
        if (!branchChoice.empty())
        {
            m_CompletedQuestBranches[id] = branchChoice;
        }

        // Record cooldown for repeatable quests
        if (state.Definition.IsRepeatable && state.Definition.RepeatCooldownSeconds > 0.0f)
        {
            m_QuestCooldowns[id] = state.Definition.RepeatCooldownSeconds;
        }

        m_ActiveQuests.erase(it);
        OLO_CORE_INFO("[QuestJournal] Completed quest '{}'", id);
        return rewards;
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
        m_FailedQuestIDs.insert(id);
        m_ActiveQuests.erase(it);
        OLO_CORE_INFO("[QuestJournal] Failed quest '{}'", id);
        return true;
    }

    void QuestJournal::IncrementObjective(const std::string& questId, const std::string& objectiveId, i32 amount)
    {
        if (amount <= 0)
        {
            return;
        }

        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return;
        }

        for (auto& obj : it->second.ObjectiveStates)
        {
            if (obj.ObjectiveID == objectiveId && !obj.IsCompleted)
            {
                // Overflow-safe: clamp addition to [0, RequiredCount]
                i32 headroom = obj.RequiredCount - obj.CurrentCount;
                obj.CurrentCount += std::min(amount, headroom);
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

    std::optional<QuestRewards> QuestJournal::TryAdvanceStage(const std::string& questId)
    {
        auto it = m_ActiveQuests.find(questId);
        if (it == m_ActiveQuests.end())
        {
            return std::nullopt;
        }

        auto& state = it->second;
        auto const& definition = state.Definition;

        if (state.CurrentStageIndex >= static_cast<i32>(definition.Stages.size()))
        {
            return std::nullopt;
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
            return std::nullopt;
        }

        state.CurrentStageIndex++;

        // Check if all stages are done
        if (state.CurrentStageIndex >= static_cast<i32>(definition.Stages.size()))
        {
            // Quest is ready for completion (auto-complete if no choices)
            if (definition.CompletionChoices.empty())
            {
                return CompleteQuest(questId);
            }
            return std::nullopt;
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
        return std::nullopt;
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
        if (m_FailedQuestIDs.contains(questId))
        {
            return QuestStatus::Failed;
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

            if (!state.Definition.CanFail)
            {
                continue;
            }

            // Check time limit
            if (state.Definition.TimeLimit > 0.0f)
            {
                state.ElapsedTime += dt;
                if (state.ElapsedTime >= state.Definition.TimeLimit)
                {
                    questsToFail.push_back(id);
                    continue;
                }
            }

            // Check failure tags
            for (auto const& failTag : state.Definition.FailOnTags)
            {
                if (m_Tags.contains(failTag))
                {
                    questsToFail.push_back(id);
                    break;
                }
            }
        }

        for (auto const& questId : questsToFail)
        {
            FailQuest(questId);
        }

        // Decrement quest cooldowns
        for (auto it = m_QuestCooldowns.begin(); it != m_QuestCooldowns.end();)
        {
            it->second -= dt;
            if (it->second <= 0.0f)
            {
                it = m_QuestCooldowns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void QuestJournal::SetActiveQuestState(const std::string& questId, ActiveQuestState state)
    {
        m_ActiveQuests[questId] = std::move(state);
    }

    void QuestJournal::AddCompletedQuestID(const std::string& questId, const std::string& branchId)
    {
        m_CompletedQuestIDs.insert(questId);
        if (!branchId.empty())
        {
            m_CompletedQuestBranches[questId] = branchId;
        }
    }

    void QuestJournal::AddFailedQuestID(const std::string& questId)
    {
        m_FailedQuestIDs.insert(questId);
    }

    // Player state setters
    void QuestJournal::SetPlayerLevel(i32 level)
    {
        m_PlayerLevel = level;
    }

    void QuestJournal::SetReputation(const std::string& factionId, i32 value)
    {
        m_Reputations[factionId] = value;
    }

    i32 QuestJournal::GetReputation(const std::string& factionId) const
    {
        auto it = m_Reputations.find(factionId);
        return it != m_Reputations.end() ? it->second : 0;
    }

    void QuestJournal::SetItemCount(const std::string& itemId, i32 count)
    {
        m_Items[itemId] = count;
    }

    i32 QuestJournal::GetItemCount(const std::string& itemId) const
    {
        auto it = m_Items.find(itemId);
        return it != m_Items.end() ? it->second : 0;
    }

    void QuestJournal::SetStat(const std::string& statName, i32 value)
    {
        m_Stats[statName] = value;
    }

    i32 QuestJournal::GetStat(const std::string& statName) const
    {
        auto it = m_Stats.find(statName);
        return it != m_Stats.end() ? it->second : 0;
    }

    void QuestJournal::SetPlayerClass(const std::string& className)
    {
        m_PlayerClass = className;
    }

    void QuestJournal::SetPlayerFaction(const std::string& factionName)
    {
        m_PlayerFaction = factionName;
    }

    const std::string& QuestJournal::GetCompletedQuestBranch(const std::string& questId) const
    {
        static const std::string s_Empty;
        auto it = m_CompletedQuestBranches.find(questId);
        return it != m_CompletedQuestBranches.end() ? it->second : s_Empty;
    }

    void QuestJournal::SetQuestCooldown(const std::string& questId, f32 remaining)
    {
        if (remaining > 0.0f)
        {
            m_QuestCooldowns[questId] = remaining;
        }
    }

    // Requirement evaluation
    bool QuestJournal::CheckRequirement(const QuestRequirement& req) const
    {
        switch (req.Type)
        {
            case QuestRequirementType::QuestCompleted:
                return m_CompletedQuestIDs.contains(req.Target);

            case QuestRequirementType::QuestActive:
                return m_ActiveQuests.contains(req.Target);

            case QuestRequirementType::QuestFailed:
                return m_FailedQuestIDs.contains(req.Target);

            case QuestRequirementType::QuestNotStarted:
                return !m_ActiveQuests.contains(req.Target) && !m_CompletedQuestIDs.contains(req.Target) && !m_FailedQuestIDs.contains(req.Target);

            case QuestRequirementType::Level:
                return EvaluateComparison(m_PlayerLevel, req.Comparison, req.Value);

            case QuestRequirementType::Reputation:
                return EvaluateComparison(GetReputation(req.Target), req.Comparison, req.Value);

            case QuestRequirementType::HasTag:
                return m_Tags.contains(req.Target);

            case QuestRequirementType::DoesNotHaveTag:
                return !m_Tags.contains(req.Target);

            case QuestRequirementType::HasItem:
                return EvaluateComparison(GetItemCount(req.Target), req.Comparison, std::max(req.Value, 1));

            case QuestRequirementType::Stat:
                return EvaluateComparison(GetStat(req.Target), req.Comparison, req.Value);

            case QuestRequirementType::IsClass:
                return m_PlayerClass == req.Target;

            case QuestRequirementType::IsFaction:
                return m_PlayerFaction == req.Target;

            case QuestRequirementType::All:
            {
                for (auto const& child : req.Children)
                {
                    if (!CheckRequirement(child))
                    {
                        return false;
                    }
                }
                return true;
            }

            case QuestRequirementType::Any:
            {
                for (auto const& child : req.Children)
                {
                    if (CheckRequirement(child))
                    {
                        return true;
                    }
                }
                return false;
            }

            case QuestRequirementType::Not:
            {
                if (req.Children.size() != 1)
                {
                    return false;
                }
                return !CheckRequirement(req.Children[0]);
            }

            default:
                return false;
        }
    }

    bool QuestJournal::CheckRequirements(const std::vector<QuestRequirement>& requirements) const
    {
        for (auto const& req : requirements)
        {
            if (!CheckRequirement(req))
            {
                return false;
            }
        }
        return true;
    }

    std::vector<const QuestRequirement*> QuestJournal::GetUnmetRequirements(const std::vector<QuestRequirement>& requirements) const
    {
        std::vector<const QuestRequirement*> unmet;
        for (auto const& req : requirements)
        {
            if (!CheckRequirement(req))
            {
                unmet.push_back(&req);
            }
        }
        return unmet;
    }

    void QuestJournal::NotifyObjectiveProgress(QuestObjective::Type type, const std::string& targetId, i32 amount)
    {
        if (amount <= 0)
        {
            return;
        }

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
                    // Overflow-safe: clamp addition to [0, RequiredCount]
                    i32 headroom = obj.RequiredCount - obj.CurrentCount;
                    obj.CurrentCount += std::min(amount, headroom);
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
