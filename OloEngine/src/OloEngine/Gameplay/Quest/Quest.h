#pragma once

#include "OloEngine/Gameplay/Quest/QuestObjective.h"

#include <string>
#include <vector>

namespace OloEngine
{
    enum class QuestStatus : u8
    {
        Unavailable, // Prerequisites not met
        Available,   // Can be accepted
        Active,      // In progress
        Completed,   // All objectives done, rewards given
        Failed,      // Failed condition triggered
        TurnedIn     // Completed and acknowledged
    };

    inline const char* QuestStatusToString(QuestStatus status)
    {
        switch (status)
        {
            case QuestStatus::Unavailable: return "Unavailable";
            case QuestStatus::Available:   return "Available";
            case QuestStatus::Active:      return "Active";
            case QuestStatus::Completed:   return "Completed";
            case QuestStatus::Failed:      return "Failed";
            case QuestStatus::TurnedIn:    return "TurnedIn";
            default:                       return "Unknown";
        }
    }

    inline QuestStatus QuestStatusFromString(const std::string& str)
    {
        if (str == "Unavailable") return QuestStatus::Unavailable;
        if (str == "Available")   return QuestStatus::Available;
        if (str == "Active")      return QuestStatus::Active;
        if (str == "Completed")   return QuestStatus::Completed;
        if (str == "Failed")      return QuestStatus::Failed;
        if (str == "TurnedIn")    return QuestStatus::TurnedIn;
        return QuestStatus::Unavailable;
    }

    struct QuestStage
    {
        std::string StageID;
        std::string Description;
        std::vector<QuestObjective> Objectives;
        bool RequireAllObjectives = true; // false = any objective completes stage

        auto operator==(const QuestStage&) const -> bool = default;
    };

    struct QuestBranchChoice
    {
        std::string ChoiceID;
        std::string Description;
        std::string NextQuestID;
        std::vector<std::string> GrantedTags;

        auto operator==(const QuestBranchChoice&) const -> bool = default;
    };

    struct QuestRewards
    {
        i32 ExperiencePoints = 0;
        i32 Currency = 0;
        std::vector<std::string> ItemRewards;
        std::vector<std::string> GrantedTags;

        auto operator==(const QuestRewards&) const -> bool = default;
    };

    struct QuestDefinition
    {
        std::string QuestID;
        std::string Title;
        std::string Description;
        std::string Category; // "Main", "Side", "Daily", "Bounty"

        std::vector<QuestStage> Stages;

        // Prerequisites
        std::vector<std::string> RequiredCompletedQuests;
        std::vector<std::string> RequiredTags;
        i32 RequiredLevel = 0;

        // Branching
        std::vector<QuestBranchChoice> CompletionChoices;

        // Rewards
        QuestRewards CompletionRewards;

        // Failure
        bool CanFail = false;
        f32 TimeLimit = -1.0f; // -1 = no time limit (seconds)
        std::vector<std::string> FailOnTags;

        // Repeatable
        bool IsRepeatable = false;
        f32 RepeatCooldownSeconds = 0.0f;

        auto operator==(const QuestDefinition&) const -> bool = default;
    };

} // namespace OloEngine
