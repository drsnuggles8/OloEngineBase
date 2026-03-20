#pragma once

#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Core/Log.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class QuestDatabase;

    class QuestJournal
    {
      public:
        // Accept a quest (moves from Available/Unavailable to Active)
        bool AcceptQuest(const std::string& questId, const QuestDefinition& definition);

        // Abandon an active quest
        bool AbandonQuest(const std::string& questId);

        // Complete a quest with optional branch choice; returns the rewards payload on success
        std::optional<QuestRewards> CompleteQuest(const std::string& questId, const std::string& branchChoice = "");

        // Fail a quest explicitly
        bool FailQuest(const std::string& questId);

        // Progress tracking
        void IncrementObjective(const std::string& questId, const std::string& objectiveId, i32 amount = 1);
        void SetObjectiveComplete(const std::string& questId, const std::string& objectiveId);
        bool TryAdvanceStage(const std::string& questId);

        // Queries
        [[nodiscard]] QuestStatus GetQuestStatus(const std::string& questId) const;
        [[nodiscard]] const QuestObjective* GetObjective(const std::string& questId, const std::string& objectiveId) const;
        [[nodiscard]] i32 GetCurrentStageIndex(const std::string& questId) const;
        [[nodiscard]] std::vector<std::string> GetActiveQuests() const;
        [[nodiscard]] std::vector<std::string> GetCompletedQuests() const;
        [[nodiscard]] bool HasCompletedQuest(const std::string& questId) const;
        [[nodiscard]] bool IsQuestActive(const std::string& questId) const;

        // Notify-based progress by objective type
        void NotifyKill(const std::string& targetTag);
        void NotifyCollect(const std::string& itemId, i32 count = 1);
        void NotifyInteract(const std::string& interactableId);
        void NotifyReachLocation(const std::string& locationId);

        // Tag management (quest-granted tags on the player)
        void AddTag(const std::string& tag);
        [[nodiscard]] bool HasTag(const std::string& tag) const;
        [[nodiscard]] const std::unordered_set<std::string>& GetTags() const
        {
            return m_Tags;
        }

        // Player state for requirement evaluation (fed by external systems)
        void SetPlayerLevel(i32 level);
        [[nodiscard]] i32 GetPlayerLevel() const
        {
            return m_PlayerLevel;
        }

        void SetReputation(const std::string& factionId, i32 value);
        [[nodiscard]] i32 GetReputation(const std::string& factionId) const;
        [[nodiscard]] const std::unordered_map<std::string, i32>& GetReputations() const
        {
            return m_Reputations;
        }

        void SetItemCount(const std::string& itemId, i32 count);
        [[nodiscard]] i32 GetItemCount(const std::string& itemId) const;
        [[nodiscard]] const std::unordered_map<std::string, i32>& GetItems() const
        {
            return m_Items;
        }

        void SetStat(const std::string& statName, i32 value);
        [[nodiscard]] i32 GetStat(const std::string& statName) const;
        [[nodiscard]] const std::unordered_map<std::string, i32>& GetStats() const
        {
            return m_Stats;
        }

        void SetPlayerClass(const std::string& className);
        [[nodiscard]] const std::string& GetPlayerClass() const
        {
            return m_PlayerClass;
        }

        void SetPlayerFaction(const std::string& factionName);
        [[nodiscard]] const std::string& GetPlayerFaction() const
        {
            return m_PlayerFaction;
        }

        // Requirement evaluation
        [[nodiscard]] bool CheckRequirement(const QuestRequirement& requirement) const;
        [[nodiscard]] bool CheckRequirements(const std::vector<QuestRequirement>& requirements) const;
        [[nodiscard]] std::vector<const QuestRequirement*> GetUnmetRequirements(const std::vector<QuestRequirement>& requirements) const;

        // Time update (called each frame for timed quests)
        void UpdateTimers(f32 dt);

        // Direct access for serialization
        struct ActiveQuestState
        {
            std::string QuestID;
            QuestStatus Status = QuestStatus::Active;
            i32 CurrentStageIndex = 0;
            std::vector<QuestObjective> ObjectiveStates; // Runtime copy with progress
            f32 ElapsedTime = 0.0f;
            QuestDefinition Definition; // Stored definition for runtime use

            auto operator==(const ActiveQuestState&) const -> bool = default;
        };

        [[nodiscard]] const std::unordered_map<std::string, ActiveQuestState>& GetActiveQuestStates() const
        {
            return m_ActiveQuests;
        }
        [[nodiscard]] const std::unordered_set<std::string>& GetCompletedQuestIDs() const
        {
            return m_CompletedQuestIDs;
        }
        [[nodiscard]] const std::unordered_set<std::string>& GetFailedQuestIDs() const
        {
            return m_FailedQuestIDs;
        }

        // For deserialization
        void SetActiveQuestState(const std::string& questId, ActiveQuestState state);
        void AddCompletedQuestID(const std::string& questId);
        void AddFailedQuestID(const std::string& questId);

        auto operator==(const QuestJournal&) const -> bool = default;

      private:
        void NotifyObjectiveProgress(QuestObjective::Type type, const std::string& targetId, i32 amount);

        std::unordered_map<std::string, ActiveQuestState> m_ActiveQuests;
        std::unordered_set<std::string> m_CompletedQuestIDs;
        std::unordered_set<std::string> m_FailedQuestIDs;
        std::unordered_set<std::string> m_Tags;

        // External player state for requirement evaluation
        i32 m_PlayerLevel = 0;
        std::unordered_map<std::string, i32> m_Reputations;
        std::unordered_map<std::string, i32> m_Items;
        std::unordered_map<std::string, i32> m_Stats;
        std::string m_PlayerClass;
        std::string m_PlayerFaction;
    };

} // namespace OloEngine
