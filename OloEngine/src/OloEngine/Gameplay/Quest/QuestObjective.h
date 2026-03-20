#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    struct QuestObjective
    {
        std::string ObjectiveID;
        std::string Description;

        enum class Type : u8
        {
            Kill,
            Collect,
            Interact,
            Reach,
            Escort,
            Custom
        };
        Type ObjectiveType = Type::Custom;

        std::string TargetID; // Entity tag, item ID, or location name
        i32 RequiredCount = 1;
        i32 CurrentCount = 0; // Runtime state

        bool IsOptional = false;
        bool IsHidden = false;
        bool IsCompleted = false;

        auto operator==(const QuestObjective&) const -> bool = default;
    };

    inline const char* ObjectiveTypeToString(QuestObjective::Type type)
    {
        switch (type)
        {
            case QuestObjective::Type::Kill:     return "Kill";
            case QuestObjective::Type::Collect:  return "Collect";
            case QuestObjective::Type::Interact: return "Interact";
            case QuestObjective::Type::Reach:    return "Reach";
            case QuestObjective::Type::Escort:   return "Escort";
            case QuestObjective::Type::Custom:   return "Custom";
            default:                             return "Unknown";
        }
    }

    inline QuestObjective::Type ObjectiveTypeFromString(const std::string& str)
    {
        if (str == "Kill")     return QuestObjective::Type::Kill;
        if (str == "Collect")  return QuestObjective::Type::Collect;
        if (str == "Interact") return QuestObjective::Type::Interact;
        if (str == "Reach")    return QuestObjective::Type::Reach;
        if (str == "Escort")   return QuestObjective::Type::Escort;
        return QuestObjective::Type::Custom;
    }

} // namespace OloEngine
