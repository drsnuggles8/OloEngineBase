#pragma once

#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/TransparentStringHash.h"

#include <optional>
#include <string>
#include <string_view>
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
        i32 ExperiencePoints = 0; // QuestCompleted — CompletionRewards XP, granted by QuestSystem (issue #635)
    };

    // Append-only sink the journal pushes QuestJournalChange records into when
    // a caller passes one. nullptr => no reporting (the default; existing
    // call sites are unaffected).
    using QuestEventSink = std::vector<QuestJournalChange>;

    class QuestJournal
    {
      public:
        // Transparent string-keyed containers enable heterogeneous lookup
        // (std::string_view / const char*) without materialising a temporary
        // std::string (cpp:S6045).
        using StringSet = std::unordered_set<std::string, StringHash, StringEqual>;
        template<typename V>
        using StringMap = std::unordered_map<std::string, V, StringHash, StringEqual>;

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
        [[nodiscard("quest status must be used")]] QuestStatus GetQuestStatus(const std::string& questId) const;
        [[nodiscard("objective pointer must be used")]] const QuestObjective* GetObjective(std::string_view questId, std::string_view objectiveId) const;
        [[nodiscard("stage index must be used")]] i32 GetCurrentStageIndex(const std::string& questId) const;
        [[nodiscard("active quest list must be used")]] std::vector<std::string> GetActiveQuests() const;
        [[nodiscard("completed quest list must be used")]] std::vector<std::string> GetCompletedQuests() const;
        [[nodiscard("completion check must be used")]] bool HasCompletedQuest(const std::string& questId) const;
        [[nodiscard("active check must be used")]] bool IsQuestActive(const std::string& questId) const;

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
        [[nodiscard("tag-presence check must be used")]] bool HasTag(const std::string& tag) const;
        [[nodiscard("tag set must be used")]] const StringSet& GetTags() const
        {
            return m_Tags;
        }

        // Player state for requirement evaluation (fed by external systems)
        void SetPlayerLevel(i32 level);
        [[nodiscard("player level must be used")]] i32 GetPlayerLevel() const
        {
            return m_PlayerLevel;
        }

        void SetReputation(const std::string& factionId, i32 value);
        [[nodiscard("reputation must be used")]] i32 GetReputation(const std::string& factionId) const;
        [[nodiscard("reputations map must be used")]] const StringMap<i32>& GetReputations() const
        {
            return m_Reputations;
        }

        void SetItemCount(const std::string& itemId, i32 count);
        [[nodiscard("item count must be used")]] i32 GetItemCount(const std::string& itemId) const;
        [[nodiscard("items map must be used")]] const StringMap<i32>& GetItems() const
        {
            return m_Items;
        }

        void SetStat(const std::string& statName, i32 value);
        [[nodiscard("stat value must be used")]] i32 GetStat(const std::string& statName) const;
        [[nodiscard("stats map must be used")]] const StringMap<i32>& GetStats() const
        {
            return m_Stats;
        }

        void SetPlayerClass(std::string_view className);
        [[nodiscard("player class must be used")]] const std::string& GetPlayerClass() const
        {
            return m_PlayerClass;
        }

        void SetPlayerFaction(std::string_view factionName);
        [[nodiscard("player faction must be used")]] const std::string& GetPlayerFaction() const
        {
            return m_PlayerFaction;
        }

        // Requirement evaluation
        [[nodiscard("requirement result must be used")]] bool CheckRequirement(const QuestRequirement& requirement) const;
        [[nodiscard("requirements result must be used")]] bool CheckRequirements(const std::vector<QuestRequirement>& requirements) const;
        [[nodiscard("unmet requirement list must be used")]] std::vector<const QuestRequirement*> GetUnmetRequirements(const std::vector<QuestRequirement>& requirements) const;

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

        [[nodiscard("active quest states must be used")]] const StringMap<ActiveQuestState>& GetActiveQuestStates() const
        {
            return m_ActiveQuests;
        }
        [[nodiscard("completed quest IDs must be used")]] const StringSet& GetCompletedQuestIDs() const
        {
            return m_CompletedQuestIDs;
        }
        [[nodiscard("failed quest IDs must be used")]] const StringSet& GetFailedQuestIDs() const
        {
            return m_FailedQuestIDs;
        }

        // For deserialization
        void SetActiveQuestState(const std::string& questId, ActiveQuestState state);
        void AddCompletedQuestID(const std::string& questId, const std::string& branchId = "");
        void AddFailedQuestID(const std::string& questId);

        // Branch tracking for completed quests
        [[nodiscard("completed quest branch must be used")]] const std::string& GetCompletedQuestBranch(const std::string& questId) const;
        [[nodiscard("completed quest branches must be used")]] const StringMap<std::string>& GetCompletedQuestBranches() const
        {
            return m_CompletedQuestBranches;
        }

        // Cooldown tracking for repeatable quests
        [[nodiscard("quest cooldowns must be used")]] const StringMap<f32>& GetQuestCooldowns() const
        {
            return m_QuestCooldowns;
        }
        void SetQuestCooldown(const std::string& questId, f32 remaining);

        auto operator==(const QuestJournal&) const -> bool = default;

      private:
        void NotifyObjectiveProgress(QuestObjective::Type type, std::string_view targetId, i32 amount, QuestEventSink* sink = nullptr);

        StringMap<ActiveQuestState> m_ActiveQuests;
        StringSet m_CompletedQuestIDs;
        StringSet m_FailedQuestIDs;
        StringSet m_Tags;
        StringMap<std::string> m_CompletedQuestBranches;
        StringMap<f32> m_QuestCooldowns;

        // External player state for requirement evaluation
        i32 m_PlayerLevel = 0;
        StringMap<i32> m_Reputations;
        StringMap<i32> m_Items;
        StringMap<i32> m_Stats;
        std::string m_PlayerClass;
        std::string m_PlayerFaction;
    };

} // namespace OloEngine
