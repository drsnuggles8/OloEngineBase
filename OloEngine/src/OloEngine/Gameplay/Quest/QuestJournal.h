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

    // Change-report record emitted by QuestJournal mutators into an optional
    // sink. The journal is a pure value type that doesn't know its owning
    // entity, so it can't build the entity-stamped QuestEvents.h payloads
    // itself. Instead it records *what changed* (including internal cascades:
    // objective complete -> stage advance -> auto-complete) and an
    // entity-aware caller (QuestSystem) translates these into the public
    // events. Kept out of QuestEvents.h because it's a transport detail, not a
    // public payload.
    struct QuestJournalChange
    {
        enum class Type : u8
        {
            QuestStarted,
            QuestAbandoned,
            QuestFailed,
            QuestCompleted,
            ObjectiveProgress,
            ObjectiveCompleted,
            StageAdvanced,
        };

        Type Kind = Type::QuestStarted;
        std::string QuestID;
        std::string ObjectiveID;  // ObjectiveProgress / ObjectiveCompleted
        std::string BranchChoice; // QuestCompleted
        i32 CurrentCount = 0;     // ObjectiveProgress
        i32 RequiredCount = 0;    // ObjectiveProgress
        i32 NewStageIndex = 0;    // StageAdvanced
    };

    // Append-only sink the journal pushes QuestJournalChange records into when
    // a caller passes one. nullptr => no reporting (the default; existing
    // call sites are unaffected).
    using QuestEventSink = std::vector<QuestJournalChange>;

    class QuestJournal
    {
      public:
        // Accept a quest (moves from Available/Unavailable to Active).
        // When `sink` is non-null, appends a QuestStarted record on success.
        bool AcceptQuest(const std::string& questId, const QuestDefinition& definition, QuestEventSink* sink = nullptr);

        // Abandon an active quest (records QuestAbandoned on success).
        bool AbandonQuest(const std::string& questId, QuestEventSink* sink = nullptr);

        // Complete a quest with optional branch choice; returns the rewards
        // payload on success (records QuestCompleted).
        std::optional<QuestRewards> CompleteQuest(const std::string& questId, const std::string& branchChoice = "", QuestEventSink* sink = nullptr);

        // Fail a quest explicitly (records QuestFailed on success).
        bool FailQuest(const std::string& questId, QuestEventSink* sink = nullptr);

        // Progress tracking. When `sink` is non-null, records ObjectiveProgress
        // / ObjectiveCompleted plus any StageAdvanced / QuestCompleted that the
        // increment cascades into.
        void IncrementObjective(const std::string& questId, const std::string& objectiveId, i32 amount = 1, QuestEventSink* sink = nullptr);
        void SetObjectiveComplete(const std::string& questId, const std::string& objectiveId, QuestEventSink* sink = nullptr);
        std::optional<QuestRewards> TryAdvanceStage(const std::string& questId, QuestEventSink* sink = nullptr);

        // Queries
        [[nodiscard]] QuestStatus GetQuestStatus(const std::string& questId) const;
        [[nodiscard]] const QuestObjective* GetObjective(const std::string& questId, const std::string& objectiveId) const;
        [[nodiscard]] i32 GetCurrentStageIndex(const std::string& questId) const;
        [[nodiscard]] std::vector<std::string> GetActiveQuests() const;
        [[nodiscard]] std::vector<std::string> GetCompletedQuests() const;
        [[nodiscard]] bool HasCompletedQuest(const std::string& questId) const;
        [[nodiscard]] bool IsQuestActive(const std::string& questId) const;

        // Notify-based progress by objective type. When `sink` is non-null,
        // records the same ObjectiveProgress / ObjectiveCompleted /
        // StageAdvanced / QuestCompleted cascade as IncrementObjective, across
        // every active quest with a matching objective.
        void NotifyKill(const std::string& targetTag, QuestEventSink* sink = nullptr);
        void NotifyCollect(const std::string& itemId, i32 count = 1, QuestEventSink* sink = nullptr);
        void NotifyInteract(const std::string& interactableId, QuestEventSink* sink = nullptr);
        void NotifyReachLocation(const std::string& locationId, QuestEventSink* sink = nullptr);

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

        // Time update (called each frame for timed quests). Records QuestFailed
        // into `sink` for any quest that hits its deadline / fail tag this tick.
        void UpdateTimers(f32 dt, QuestEventSink* sink = nullptr);

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
        void AddCompletedQuestID(const std::string& questId, const std::string& branchId = "");
        void AddFailedQuestID(const std::string& questId);

        // Branch tracking for completed quests
        [[nodiscard]] const std::string& GetCompletedQuestBranch(const std::string& questId) const;
        [[nodiscard]] const std::unordered_map<std::string, std::string>& GetCompletedQuestBranches() const
        {
            return m_CompletedQuestBranches;
        }

        // Cooldown tracking for repeatable quests
        [[nodiscard]] const std::unordered_map<std::string, f32>& GetQuestCooldowns() const
        {
            return m_QuestCooldowns;
        }
        void SetQuestCooldown(const std::string& questId, f32 remaining);

        auto operator==(const QuestJournal&) const -> bool = default;

      private:
        void NotifyObjectiveProgress(QuestObjective::Type type, const std::string& targetId, i32 amount, QuestEventSink* sink = nullptr);

        std::unordered_map<std::string, ActiveQuestState> m_ActiveQuests;
        std::unordered_set<std::string> m_CompletedQuestIDs;
        std::unordered_set<std::string> m_FailedQuestIDs;
        std::unordered_set<std::string> m_Tags;
        std::unordered_map<std::string, std::string> m_CompletedQuestBranches;
        std::unordered_map<std::string, f32> m_QuestCooldowns;

        // External player state for requirement evaluation
        i32 m_PlayerLevel = 0;
        std::unordered_map<std::string, i32> m_Reputations;
        std::unordered_map<std::string, i32> m_Items;
        std::unordered_map<std::string, i32> m_Stats;
        std::string m_PlayerClass;
        std::string m_PlayerFaction;
    };

} // namespace OloEngine
