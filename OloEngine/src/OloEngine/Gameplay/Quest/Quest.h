#pragma once

#include "OloEngine/Gameplay/Quest/QuestObjective.h"
#include "OloEngine/Gameplay/Quest/QuestRequirement.h"

#include <string>
#include <string_view>
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

    [[nodiscard("status string must be used")]] inline const char* QuestStatusToString(QuestStatus status)
    {
        using enum QuestStatus;
        switch (status)
        {
            case Unavailable:
                return "Unavailable";
            case Available:
                return "Available";
            case Active:
                return "Active";
            case Completed:
                return "Completed";
            case Failed:
                return "Failed";
            case TurnedIn:
                return "TurnedIn";
            default:
                return "Unknown";
        }
    }

    inline QuestStatus QuestStatusFromString(std::string_view str)
    {
        using enum QuestStatus;
        if (str == "Unavailable")
            return Unavailable;
        if (str == "Available")
            return Available;
        if (str == "Active")
            return Active;
        if (str == "Completed")
            return Completed;
        if (str == "Failed")
            return Failed;
        if (str == "TurnedIn")
            return TurnedIn;
        return Unavailable;
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

        // Prerequisites (flexible requirement system)
        std::vector<QuestRequirement> Requirements;

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
