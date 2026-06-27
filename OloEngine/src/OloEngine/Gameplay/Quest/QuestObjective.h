#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>

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

    [[nodiscard("objective type string must be used")]] inline const char* ObjectiveTypeToString(QuestObjective::Type type)
    {
        using enum QuestObjective::Type;
        switch (type)
        {
            case Kill:
                return "Kill";
            case Collect:
                return "Collect";
            case Interact:
                return "Interact";
            case Reach:
                return "Reach";
            case Escort:
                return "Escort";
            case Custom:
                return "Custom";
            default:
                return "Unknown";
        }
    }

    inline QuestObjective::Type ObjectiveTypeFromString(std::string_view str)
    {
        using enum QuestObjective::Type;
        if (str == "Kill")
            return Kill;
        if (str == "Collect")
            return Collect;
        if (str == "Interact")
            return Interact;
        if (str == "Reach")
            return Reach;
        if (str == "Escort")
            return Escort;
        return Custom;
    }

} // namespace OloEngine
